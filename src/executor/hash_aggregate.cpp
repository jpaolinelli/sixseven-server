#include "sixseven/executor/hash_aggregate.h"

#include "sixseven/common/coercion.h"
#include "sixseven/executor/expr_evaluator.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace sixseven {

namespace {

/// Convert a numeric Value to double for SUM/AVG accumulation.
Result<double> value_to_double(const Value& v) {
    if (v.is_null()) {
        return make_error(StatusCode::TYPE_ERROR, "NULL in aggregate arithmetic");
    }
    switch (v.type_id()) {
    case TypeId::INT8:
        return ok(static_cast<double>(v.as_int8()));
    case TypeId::INT16:
        return ok(static_cast<double>(v.as_int16()));
    case TypeId::INT32:
        return ok(static_cast<double>(v.as_int32()));
    case TypeId::INT64:
        return ok(static_cast<double>(v.as_int64()));
    case TypeId::UINT8:
        return ok(static_cast<double>(v.as_uint8()));
    case TypeId::UINT16:
        return ok(static_cast<double>(v.as_uint16()));
    case TypeId::UINT32:
        return ok(static_cast<double>(v.as_uint32()));
    case TypeId::UINT64:
        return ok(static_cast<double>(v.as_uint64()));
    case TypeId::FLOAT32:
        return ok(static_cast<double>(v.as_float32()));
    case TypeId::FLOAT64:
        return ok(v.as_float64());
    default:
        return make_error(StatusCode::TYPE_ERROR,
                          "cannot convert " + std::string(type_name(v.type_id())) + " to numeric");
    }
}

/// Convert a numeric Value to int64 for integer SUM accumulation.
Result<int64_t> value_to_int64(const Value& v) {
    if (v.is_null()) {
        return make_error(StatusCode::TYPE_ERROR, "NULL in aggregate arithmetic");
    }
    switch (v.type_id()) {
    case TypeId::INT8:
        return ok(static_cast<int64_t>(v.as_int8()));
    case TypeId::INT16:
        return ok(static_cast<int64_t>(v.as_int16()));
    case TypeId::INT32:
        return ok(static_cast<int64_t>(v.as_int32()));
    case TypeId::INT64:
        return ok(v.as_int64());
    case TypeId::UINT8:
        return ok(static_cast<int64_t>(v.as_uint8()));
    case TypeId::UINT16:
        return ok(static_cast<int64_t>(v.as_uint16()));
    case TypeId::UINT32:
        return ok(static_cast<int64_t>(v.as_uint32()));
    case TypeId::UINT64:
        return ok(static_cast<int64_t>(v.as_uint64()));
    default:
        return make_error(StatusCode::TYPE_ERROR,
                          "cannot convert " + std::string(type_name(v.type_id())) + " to integer");
    }
}

/// Hash a single Value for building the group-key hash.
size_t hash_single_value(const Value& v) {
    if (v.is_null()) {
        return 0;
    }
    switch (v.type_id()) {
    case TypeId::INT8:
        return std::hash<int8_t>{}(v.as_int8());
    case TypeId::INT16:
        return std::hash<int16_t>{}(v.as_int16());
    case TypeId::INT32:
        return std::hash<int32_t>{}(v.as_int32());
    case TypeId::INT64:
        return std::hash<int64_t>{}(v.as_int64());
    case TypeId::UINT8:
        return std::hash<uint8_t>{}(v.as_uint8());
    case TypeId::UINT16:
        return std::hash<uint16_t>{}(v.as_uint16());
    case TypeId::UINT32:
        return std::hash<uint32_t>{}(v.as_uint32());
    case TypeId::UINT64:
        return std::hash<uint64_t>{}(v.as_uint64());
    case TypeId::FLOAT32:
        return std::hash<float>{}(v.as_float32());
    case TypeId::FLOAT64:
        return std::hash<double>{}(v.as_float64());
    case TypeId::BOOL:
        return std::hash<bool>{}(v.as_bool());
    case TypeId::STRING:
        return std::hash<std::string>{}(v.as_string());
    default:
        return 0;
    }
}

/// Per-aggregate accumulator state.
struct AccumulatorState {
    AggFunc func;

    // COUNT / COUNT_STAR
    int64_t count = 0;

    // SUM — integer path
    int64_t int_sum = 0;
    bool sum_is_integer = false;

    // SUM — float path
    double float_sum = 0.0;

    // AVG
    double avg_sum = 0.0;
    int64_t avg_count = 0;

    // MIN / MAX
    Value extremum;
    bool has_extremum = false;

    // COUNT(DISTINCT)
    std::vector<Value> distinct_values;

    // STRING_AGG
    std::vector<std::string> string_parts;
    std::string separator;
};

/// Initialise accumulator state for a given aggregate function.
AccumulatorState make_accumulator(const AggregateDescriptor& desc) {
    AccumulatorState state;
    state.func = desc.func;
    state.separator = desc.separator;
    return state;
}

/// Feed a value into the accumulator. NULL handling follows SQL standard.
Result<void> accumulate(AccumulatorState& state, const Value& val) {
    switch (state.func) {
    case AggFunc::COUNT_STAR:
        ++state.count;
        return ok();

    case AggFunc::COUNT:
        if (!val.is_null()) {
            ++state.count;
        }
        return ok();

    case AggFunc::COUNT_DISTINCT:
        if (!val.is_null()) {
            state.distinct_values.push_back(val);
        }
        return ok();

    case AggFunc::SUM:
        if (val.is_null()) {
            return ok(); // Skip NULLs
        }
        if (is_integer(val.type_id())) {
            auto i = value_to_int64(val);
            if (!i) {
                return tl::unexpected(i.error());
            }
            state.int_sum += *i;
            state.sum_is_integer = true;
        } else {
            auto d = value_to_double(val);
            if (!d) {
                return tl::unexpected(d.error());
            }
            state.float_sum += *d;
        }
        ++state.count; // Track non-NULL count for NULL result detection
        return ok();

    case AggFunc::AVG:
        if (val.is_null()) {
            return ok(); // Skip NULLs
        }
        {
            auto d = value_to_double(val);
            if (!d) {
                return tl::unexpected(d.error());
            }
            state.avg_sum += *d;
            ++state.avg_count;
        }
        return ok();

    case AggFunc::MIN:
        if (val.is_null()) {
            return ok(); // Skip NULLs
        }
        if (!state.has_extremum) {
            state.extremum = val;
            state.has_extremum = true;
        } else {
            auto cmp = compare(val, state.extremum);
            if (!cmp) {
                return tl::unexpected(cmp.error());
            }
            if (*cmp == std::strong_ordering::less) {
                state.extremum = val;
            }
        }
        return ok();

    case AggFunc::MAX:
        if (val.is_null()) {
            return ok(); // Skip NULLs
        }
        if (!state.has_extremum) {
            state.extremum = val;
            state.has_extremum = true;
        } else {
            auto cmp = compare(val, state.extremum);
            if (!cmp) {
                return tl::unexpected(cmp.error());
            }
            if (*cmp == std::strong_ordering::greater) {
                state.extremum = val;
            }
        }
        return ok();

    case AggFunc::STRING_AGG:
        if (val.is_null()) {
            return ok(); // Skip NULLs
        }
        if (val.type_id() == TypeId::STRING) {
            state.string_parts.push_back(val.as_string());
        } else {
            auto coerced = coerce(val, TypeId::STRING);
            if (!coerced) {
                return tl::unexpected(coerced.error());
            }
            state.string_parts.push_back(coerced->as_string());
        }
        return ok();
    }

    return make_error(StatusCode::INTERNAL_ERROR, "unknown aggregate function");
}

/// Finalise the accumulator and produce a result Value.
Value finalise(AccumulatorState& state) {
    switch (state.func) {
    case AggFunc::COUNT_STAR:
    case AggFunc::COUNT:
        return Value(state.count);

    case AggFunc::COUNT_DISTINCT: {
        // Deduplicate by sorting and removing duplicates.
        int64_t distinct_count = 0;
        if (!state.distinct_values.empty()) {
            // Use a simple O(n^2) comparison for correctness.
            std::vector<Value> unique;
            for (auto& v : state.distinct_values) {
                bool is_dup = false;
                for (auto& u : unique) {
                    auto cmp = compare(v, u);
                    if (cmp && *cmp == std::strong_ordering::equal) {
                        is_dup = true;
                        break;
                    }
                }
                if (!is_dup) {
                    unique.push_back(v);
                }
            }
            distinct_count = static_cast<int64_t>(unique.size());
        }
        return Value(distinct_count);
    }

    case AggFunc::SUM:
        if (state.count == 0) {
            return Value::make_null(); // SUM over no non-NULL values = NULL
        }
        if (state.sum_is_integer && state.float_sum == 0.0) {
            return Value(state.int_sum);
        }
        return Value(static_cast<double>(state.int_sum) + state.float_sum);

    case AggFunc::AVG:
        if (state.avg_count == 0) {
            return Value::make_null(); // AVG over no non-NULL values = NULL
        }
        return Value(state.avg_sum / static_cast<double>(state.avg_count));

    case AggFunc::MIN:
    case AggFunc::MAX:
        if (!state.has_extremum) {
            return Value::make_null(); // MIN/MAX over no non-NULL values = NULL
        }
        return state.extremum;

    case AggFunc::STRING_AGG:
        if (state.string_parts.empty()) {
            return Value::make_null();
        }
        {
            std::string result;
            for (size_t i = 0; i < state.string_parts.size(); ++i) {
                if (i > 0) {
                    result += state.separator;
                }
                result += state.string_parts[i];
            }
            return Value(std::move(result));
        }
    }
    return Value::make_null();
}

/// A group in the hash table: group key values + per-aggregate accumulators.
struct GroupState {
    std::vector<Value> key;
    std::vector<AccumulatorState> accumulators;
};

} // anonymous namespace

// ===========================================================================
// HashAggregateOperator
// ===========================================================================

HashAggregateOperator::HashAggregateOperator(std::unique_ptr<Iterator> child,
                                             std::vector<const Expr*> group_by_exprs,
                                             std::vector<AggregateDescriptor> aggregates,
                                             const BoundStatement& bound,
                                             OutputSchema schema)
    : child_(std::move(child)), group_by_exprs_(std::move(group_by_exprs)),
      aggregates_(std::move(aggregates)), bound_(bound), schema_(std::move(schema)) {}

size_t HashAggregateOperator::hash_key(const std::vector<Value>& key) {
    size_t h = 0;
    for (auto& v : key) {
        h ^= hash_single_value(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }
    return h;
}

bool HashAggregateOperator::keys_equal(const std::vector<Value>& a, const std::vector<Value>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        // NULLs are considered equal for GROUP BY purposes (SQL standard).
        if (a[i].is_null() && b[i].is_null()) {
            continue;
        }
        if (a[i].is_null() || b[i].is_null()) {
            return false;
        }
        auto cmp = compare(a[i], b[i]);
        if (!cmp || *cmp != std::strong_ordering::equal) {
            return false;
        }
    }
    return true;
}

Result<void> HashAggregateOperator::do_open() {
    auto child_open = child_->open();
    if (!child_open) {
        return child_open;
    }

    const auto& child_schema = child_->output_schema();

    // Hash table: hash → list of group indices.
    std::unordered_multimap<size_t, size_t> hash_table;
    std::vector<GroupState> groups;

    // Materialise all child tuples and group them.
    while (true) {
        auto row = child_->next();
        if (!row) {
            return tl::unexpected(row.error());
        }
        if (!row->has_value()) {
            break;
        }
        auto& tuple = row->value();

        // Evaluate GROUP BY expressions to get the group key.
        std::vector<Value> key;
        key.reserve(group_by_exprs_.size());
        for (auto* expr : group_by_exprs_) {
            auto val = evaluate_expr(*expr, tuple, child_schema, bound_);
            if (!val) {
                return tl::unexpected(val.error());
            }
            key.push_back(std::move(*val));
        }

        // Find or create group.
        size_t h = hash_key(key);
        size_t group_idx = groups.size(); // default: new group
        bool found = false;

        auto range = hash_table.equal_range(h);
        for (auto it = range.first; it != range.second; ++it) {
            if (keys_equal(groups[it->second].key, key)) {
                group_idx = it->second;
                found = true;
                break;
            }
        }

        if (!found) {
            GroupState gs;
            gs.key = std::move(key);
            gs.accumulators.reserve(aggregates_.size());
            for (auto& desc : aggregates_) {
                gs.accumulators.push_back(make_accumulator(desc));
            }
            groups.push_back(std::move(gs));
            hash_table.emplace(h, group_idx);
        }

        // Accumulate each aggregate for this tuple.
        auto& group = groups[group_idx];
        for (size_t i = 0; i < aggregates_.size(); ++i) {
            Value arg_val;
            if (aggregates_[i].func == AggFunc::COUNT_STAR) {
                // COUNT(*) doesn't evaluate an argument.
            } else if (aggregates_[i].arg != nullptr) {
                auto val = evaluate_expr(*aggregates_[i].arg, tuple, child_schema, bound_);
                if (!val) {
                    return tl::unexpected(val.error());
                }
                arg_val = std::move(*val);
            }

            auto acc_result = accumulate(group.accumulators[i], arg_val);
            if (!acc_result) {
                return acc_result;
            }
        }
    }

    child_->close();

    // If no groups and no GROUP BY (global aggregation), create a single empty group.
    if (groups.empty() && group_by_exprs_.empty()) {
        GroupState gs;
        gs.accumulators.reserve(aggregates_.size());
        for (auto& desc : aggregates_) {
            gs.accumulators.push_back(make_accumulator(desc));
        }
        groups.push_back(std::move(gs));
    }

    // Finalise: produce output tuples.
    output_.reserve(groups.size());
    for (auto& group : groups) {
        std::vector<Value> values;
        values.reserve(group.key.size() + group.accumulators.size());

        // GROUP BY values first.
        for (auto& v : group.key) {
            values.push_back(std::move(v));
        }

        // Aggregate results.
        for (auto& acc : group.accumulators) {
            values.push_back(finalise(acc));
        }

        output_.push_back(Tuple{std::move(values), {}});
    }

    cursor_ = 0;
    return ok();
}

Result<std::optional<Tuple>> HashAggregateOperator::do_next() {
    if (cursor_ >= output_.size()) {
        return ok(std::optional<Tuple>(std::nullopt));
    }
    auto& t = output_[cursor_++];
    return ok(std::optional<Tuple>(Tuple{t.values, t.rid}));
}

void HashAggregateOperator::do_close() {
    output_.clear();
    cursor_ = 0;
}

const OutputSchema& HashAggregateOperator::output_schema() const {
    return schema_;
}

std::string HashAggregateOperator::plan_node_name() const {
    return "HashAggregate";
}

std::string HashAggregateOperator::plan_node_detail() const {
    return "";
}

std::vector<const Iterator*> HashAggregateOperator::plan_children() const {
    return {child_.get()};
}

std::vector<Iterator*> HashAggregateOperator::plan_children_mutable() {
    return {child_.get()};
}

} // namespace sixseven
