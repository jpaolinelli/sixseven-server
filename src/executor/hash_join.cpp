#include "sixseven/executor/hash_join.h"

#include "sixseven/common/coercion.h"
#include "sixseven/executor/expr_evaluator.h"

#include <functional>

namespace sixseven {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

HashJoinOperator::HashJoinOperator(std::unique_ptr<Iterator> probe,
                                   std::unique_ptr<Iterator> build,
                                   JoinType type,
                                   const Expr* probe_key,
                                   const Expr* build_key,
                                   const BoundStatement& bound,
                                   OutputSchema schema,
                                   size_t work_mem)
    : probe_child_(std::move(probe)), build_child_(std::move(build)), type_(type),
      probe_key_(probe_key), build_key_(build_key), bound_(bound), schema_(std::move(schema)),
      work_mem_(work_mem) {}

// ---------------------------------------------------------------------------
// Value hashing / equality
// ---------------------------------------------------------------------------

size_t HashJoinOperator::hash_value(const Value& v) {
    if (v.is_null()) {
        return 0;
    }
    // Hash based on the variant index + value content.
    const auto& data = v.data();
    return std::visit(
        [](const auto& val) -> size_t {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return 0;
            } else if constexpr (std::is_same_v<T, std::string>) {
                return std::hash<std::string>{}(val);
            } else if constexpr (std::is_same_v<T, bool>) {
                return std::hash<bool>{}(val);
            } else if constexpr (std::is_arithmetic_v<T>) {
                return std::hash<T>{}(val);
            } else {
                // For complex types (Blob, Date, etc.), use a simple approach.
                return 0;
            }
        },
        data);
}

bool HashJoinOperator::values_equal(const Value& a, const Value& b) {
    // NULLs never match in join predicates.
    if (a.is_null() || b.is_null()) {
        return false;
    }

    // Use the compare function from the Value system.
    auto cmp = compare(a, b);
    if (!cmp) {
        return false;
    }
    return *cmp == std::strong_ordering::equal;
}

// ---------------------------------------------------------------------------
// open
// ---------------------------------------------------------------------------

Result<void> HashJoinOperator::do_open() {
    auto pr = probe_child_->open();
    if (!pr) {
        return pr;
    }
    auto br = build_child_->open();
    if (!br) {
        return br;
    }

    probe_col_count_ = probe_child_->output_schema().column_count();
    build_col_count_ = build_child_->output_schema().column_count();

    // Materialise build side.
    std::vector<Tuple> build_tuples;
    while (true) {
        auto row = build_child_->next();
        if (!row) {
            return make_error(row.error().code, row.error().message);
        }
        if (!row->has_value()) {
            break;
        }
        build_tuples.push_back(std::move(row->value()));
    }
    build_child_->close();

    // Materialise probe side.
    std::vector<Tuple> probe_tuples;
    while (true) {
        auto row = probe_child_->next();
        if (!row) {
            return make_error(row.error().code, row.error().message);
        }
        if (!row->has_value()) {
            break;
        }
        probe_tuples.push_back(std::move(row->value()));
    }
    probe_child_->close();

    // Choose strategy: in-memory or grace hash.
    output_.clear();
    output_cursor_ = 0;

    if (work_mem_ > 0 && build_tuples.size() > work_mem_) {
        auto result = run_grace(build_tuples, probe_tuples);
        if (!result) {
            return result;
        }
    } else {
        auto result = run_in_memory(build_tuples, probe_tuples);
        if (!result) {
            return result;
        }
    }

    return ok();
}

// ---------------------------------------------------------------------------
// In-memory hash join
// ---------------------------------------------------------------------------

Result<void> HashJoinOperator::run_in_memory(std::vector<Tuple>& build_tuples,
                                             std::vector<Tuple>& probe_tuples) {
    const auto& build_schema = build_child_->output_schema();
    const auto& probe_schema = probe_child_->output_schema();

    // Build phase: hash build-side tuples by key.
    std::unordered_multimap<size_t, size_t> hash_table; // hash -> index into build_tuples
    for (size_t i = 0; i < build_tuples.size(); ++i) {
        auto key_val = evaluate_expr(*build_key_, build_tuples[i], build_schema, bound_);
        if (!key_val) {
            return make_error(key_val.error().code, key_val.error().message);
        }
        size_t h = hash_value(*key_val);
        hash_table.emplace(h, i);
    }

    // Probe phase: for each probe tuple, look up matching build tuples.
    for (auto& probe_tuple : probe_tuples) {
        auto key_val = evaluate_expr(*probe_key_, probe_tuple, probe_schema, bound_);
        if (!key_val) {
            return make_error(key_val.error().code, key_val.error().message);
        }

        // NULL keys never match.
        if (key_val->is_null()) {
            if (type_ == JoinType::LEFT) {
                output_.push_back(probe_with_null_build(probe_tuple));
            }
            continue;
        }

        size_t h = hash_value(*key_val);
        auto range = hash_table.equal_range(h);
        bool matched = false;

        for (auto it = range.first; it != range.second; ++it) {
            // Re-evaluate build key to confirm match (handles hash collisions).
            auto build_val =
                evaluate_expr(*build_key_, build_tuples[it->second], build_schema, bound_);
            if (!build_val) {
                continue;
            }
            if (values_equal(*key_val, *build_val)) {
                output_.push_back(combine(probe_tuple, build_tuples[it->second]));
                matched = true;
            }
        }

        if (!matched && type_ == JoinType::LEFT) {
            output_.push_back(probe_with_null_build(probe_tuple));
        }
    }

    return ok();
}

// ---------------------------------------------------------------------------
// Grace hash join
// ---------------------------------------------------------------------------

Result<void> HashJoinOperator::run_grace(std::vector<Tuple>& build_tuples,
                                         std::vector<Tuple>& probe_tuples) {
    const auto& build_schema = build_child_->output_schema();
    const auto& probe_schema = probe_child_->output_schema();

    // Number of partitions — use a simple fixed count.
    constexpr size_t NUM_PARTITIONS = 8;

    // Partition build side.
    std::vector<std::vector<Tuple>> build_partitions(NUM_PARTITIONS);
    for (auto& tuple : build_tuples) {
        auto key_val = evaluate_expr(*build_key_, tuple, build_schema, bound_);
        if (!key_val) {
            return make_error(key_val.error().code, key_val.error().message);
        }
        // Use a different hash seed for partitioning vs. the in-memory hash table.
        size_t h = hash_value(*key_val);
        size_t partition = (h >> 16) % NUM_PARTITIONS; // Use upper bits for partition.
        build_partitions[partition].push_back(std::move(tuple));
    }

    // Partition probe side.
    std::vector<std::vector<Tuple>> probe_partitions(NUM_PARTITIONS);
    for (auto& tuple : probe_tuples) {
        auto key_val = evaluate_expr(*probe_key_, tuple, probe_schema, bound_);
        if (!key_val) {
            return make_error(key_val.error().code, key_val.error().message);
        }
        size_t h = hash_value(*key_val);
        size_t partition = (h >> 16) % NUM_PARTITIONS;
        probe_partitions[partition].push_back(std::move(tuple));
    }

    // Join each partition pair in-memory.
    for (size_t p = 0; p < NUM_PARTITIONS; ++p) {
        if (build_partitions[p].empty() && probe_partitions[p].empty()) {
            continue;
        }
        if (build_partitions[p].empty()) {
            // No build tuples in this partition.
            if (type_ == JoinType::LEFT) {
                for (auto& probe_tuple : probe_partitions[p]) {
                    output_.push_back(probe_with_null_build(probe_tuple));
                }
            }
            continue;
        }
        auto result = run_in_memory(build_partitions[p], probe_partitions[p]);
        if (!result) {
            return result;
        }
    }

    return ok();
}

// ---------------------------------------------------------------------------
// next / close / output_schema
// ---------------------------------------------------------------------------

Result<std::optional<Tuple>> HashJoinOperator::do_next() {
    if (output_cursor_ >= output_.size()) {
        return ok(std::optional<Tuple>(std::nullopt));
    }
    return ok(std::optional<Tuple>(std::move(output_[output_cursor_++])));
}

void HashJoinOperator::do_close() {
    output_.clear();
    output_cursor_ = 0;
}

const OutputSchema& HashJoinOperator::output_schema() const {
    return schema_;
}

// ---------------------------------------------------------------------------
// Plan inspection
// ---------------------------------------------------------------------------

std::string HashJoinOperator::plan_node_name() const {
    return "Hash Join";
}

std::string HashJoinOperator::plan_node_detail() const {
    switch (type_) {
    case JoinType::INNER:
        return "Inner Join";
    case JoinType::LEFT:
        return "Left Join";
    case JoinType::RIGHT:
        return "Right Join";
    case JoinType::FULL:
        return "Full Join";
    case JoinType::CROSS:
        return "Cross Join";
    case JoinType::SEMI:
        return "Semi Join";
    case JoinType::ANTI:
        return "Anti Join";
    }
    return "";
}

std::vector<const Iterator*> HashJoinOperator::plan_children() const {
    return {probe_child_.get(), build_child_.get()};
}

std::vector<Iterator*> HashJoinOperator::plan_children_mutable() {
    return {probe_child_.get(), build_child_.get()};
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Tuple HashJoinOperator::combine(const Tuple& probe, const Tuple& build) const {
    Tuple result;
    result.values.reserve(probe_col_count_ + build_col_count_);
    result.values.insert(result.values.end(), probe.values.begin(), probe.values.end());
    result.values.insert(result.values.end(), build.values.begin(), build.values.end());
    return result;
}

Tuple HashJoinOperator::probe_with_null_build(const Tuple& probe) const {
    Tuple result;
    result.values.reserve(probe_col_count_ + build_col_count_);
    result.values.insert(result.values.end(), probe.values.begin(), probe.values.end());
    for (size_t i = 0; i < build_col_count_; ++i) {
        result.values.push_back(Value::make_null());
    }
    return result;
}

} // namespace sixseven
