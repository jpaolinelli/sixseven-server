#include "giodb/executor/window_function.h"

#include "giodb/common/coercion.h"
#include "giodb/executor/expr_evaluator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace giodb {

namespace {

/// Convert a numeric Value to double for SUM/AVG accumulation.
Result<double> value_to_double(const Value& v) {
    if (v.is_null()) {
        return make_error(StatusCode::TYPE_ERROR, "NULL in window aggregate arithmetic");
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
        return make_error(StatusCode::TYPE_ERROR, "NULL in window aggregate arithmetic");
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

/// Hash a single Value for building partition-key hashes.
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

/// Hash a vector of Values (partition key).
size_t hash_key(const std::vector<Value>& key) {
    size_t h = 0;
    for (auto& v : key) {
        h ^= hash_single_value(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }
    return h;
}

/// Check if two partition keys are equal. NULLs are considered equal
/// for PARTITION BY purposes (SQL standard).
bool keys_equal(const std::vector<Value>& a, const std::vector<Value>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
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

/// Check if two rows are peers (equal on all ORDER BY keys).
bool are_peers(const Tuple& a,
               const Tuple& b,
               const std::vector<SortKey>& order_by,
               const OutputSchema& child_schema,
               const BoundStatement& bound) {
    for (auto& key : order_by) {
        auto va = evaluate_expr(*key.expr, a, child_schema, bound);
        auto vb = evaluate_expr(*key.expr, b, child_schema, bound);
        if (!va || !vb) {
            return false;
        }
        if (va->is_null() && vb->is_null()) {
            continue;
        }
        if (va->is_null() || vb->is_null()) {
            return false;
        }
        auto cmp = compare(*va, *vb);
        if (!cmp || *cmp != std::strong_ordering::equal) {
            return false;
        }
    }
    return true;
}

/// Evaluate a window aggregate function over a frame within a partition.
Result<Value> evaluate_window_aggregate(WindowFunc func,
                                        const Expr* arg,
                                        const std::vector<Tuple>& partition,
                                        size_t frame_start,
                                        size_t frame_end,
                                        const OutputSchema& child_schema,
                                        const BoundStatement& bound) {

    switch (func) {
    case WindowFunc::WIN_COUNT: {
        int64_t count = 0;
        for (size_t i = frame_start; i <= frame_end; ++i) {
            if (arg != nullptr) {
                auto val = evaluate_expr(*arg, partition[i], child_schema, bound);
                if (!val) {
                    return tl::unexpected(val.error());
                }
                if (!val->is_null()) {
                    ++count;
                }
            } else {
                ++count; // COUNT(*)
            }
        }
        return ok(Value(count));
    }

    case WindowFunc::WIN_SUM: {
        int64_t int_sum = 0;
        double float_sum = 0.0;
        bool has_integer = false;
        int64_t non_null_count = 0;

        for (size_t i = frame_start; i <= frame_end; ++i) {
            auto val = evaluate_expr(*arg, partition[i], child_schema, bound);
            if (!val) {
                return tl::unexpected(val.error());
            }
            if (val->is_null()) {
                continue;
            }
            ++non_null_count;
            if (is_integer(val->type_id())) {
                auto iv = value_to_int64(*val);
                if (!iv) {
                    return tl::unexpected(iv.error());
                }
                int_sum += *iv;
                has_integer = true;
            } else {
                auto dv = value_to_double(*val);
                if (!dv) {
                    return tl::unexpected(dv.error());
                }
                float_sum += *dv;
            }
        }
        if (non_null_count == 0) {
            return ok(Value::make_null());
        }
        if (has_integer && float_sum == 0.0) {
            return ok(Value(int_sum));
        }
        return ok(Value(static_cast<double>(int_sum) + float_sum));
    }

    case WindowFunc::WIN_AVG: {
        double sum = 0.0;
        int64_t count = 0;

        for (size_t i = frame_start; i <= frame_end; ++i) {
            auto val = evaluate_expr(*arg, partition[i], child_schema, bound);
            if (!val) {
                return tl::unexpected(val.error());
            }
            if (val->is_null()) {
                continue;
            }
            auto dv = value_to_double(*val);
            if (!dv) {
                return tl::unexpected(dv.error());
            }
            sum += *dv;
            ++count;
        }
        if (count == 0) {
            return ok(Value::make_null());
        }
        return ok(Value(sum / static_cast<double>(count)));
    }

    case WindowFunc::WIN_MIN: {
        Value extremum;
        bool has_extremum = false;

        for (size_t i = frame_start; i <= frame_end; ++i) {
            auto val = evaluate_expr(*arg, partition[i], child_schema, bound);
            if (!val) {
                return tl::unexpected(val.error());
            }
            if (val->is_null()) {
                continue;
            }
            if (!has_extremum) {
                extremum = std::move(*val);
                has_extremum = true;
            } else {
                auto cmp = compare(*val, extremum);
                if (!cmp) {
                    return tl::unexpected(cmp.error());
                }
                if (*cmp == std::strong_ordering::less) {
                    extremum = std::move(*val);
                }
            }
        }
        if (!has_extremum) {
            return ok(Value::make_null());
        }
        return ok(std::move(extremum));
    }

    case WindowFunc::WIN_MAX: {
        Value extremum;
        bool has_extremum = false;

        for (size_t i = frame_start; i <= frame_end; ++i) {
            auto val = evaluate_expr(*arg, partition[i], child_schema, bound);
            if (!val) {
                return tl::unexpected(val.error());
            }
            if (val->is_null()) {
                continue;
            }
            if (!has_extremum) {
                extremum = std::move(*val);
                has_extremum = true;
            } else {
                auto cmp = compare(*val, extremum);
                if (!cmp) {
                    return tl::unexpected(cmp.error());
                }
                if (*cmp == std::strong_ordering::greater) {
                    extremum = std::move(*val);
                }
            }
        }
        if (!has_extremum) {
            return ok(Value::make_null());
        }
        return ok(std::move(extremum));
    }

    default:
        return make_error(StatusCode::INTERNAL_ERROR, "not an aggregate window function");
    }
}

} // anonymous namespace

// ===========================================================================
// WindowOperator
// ===========================================================================

WindowOperator::WindowOperator(std::unique_ptr<Iterator> child,
                               std::vector<const Expr*> partition_by,
                               std::vector<SortKey> order_by,
                               std::vector<WindowFunctionDescriptor> functions,
                               const BoundStatement& bound,
                               OutputSchema schema)
    : child_(std::move(child)), partition_by_(std::move(partition_by)),
      order_by_(std::move(order_by)), functions_(std::move(functions)), bound_(bound),
      schema_(std::move(schema)) {}

size_t WindowOperator::resolve_bound(FrameBound bound,
                                     int64_t offset,
                                     size_t current_row,
                                     size_t partition_size) {
    switch (bound) {
    case FrameBound::UNBOUNDED_PRECEDING:
        return 0;
    case FrameBound::N_PRECEDING: {
        if (offset < 0) {
            return 0;
        }
        auto n = static_cast<size_t>(offset);
        return current_row >= n ? current_row - n : 0;
    }
    case FrameBound::CURRENT_ROW:
        return current_row;
    case FrameBound::N_FOLLOWING: {
        if (offset < 0) {
            return current_row;
        }
        auto n = static_cast<size_t>(offset);
        size_t result = current_row + n;
        return result < partition_size ? result : partition_size - 1;
    }
    case FrameBound::UNBOUNDED_FOLLOWING:
        return partition_size - 1;
    }
    return current_row; // fallback
}

bool WindowOperator::is_frame_empty(const WindowFrameSpec& frame,
                                    size_t current_row,
                                    size_t partition_size) {
    // Compute unclamped (signed) frame positions to detect logically inverted
    // frames that resolve_bound's clamping would mask.
    auto raw_pos = [](FrameBound bound, int64_t offset, int64_t pos, int64_t psize) -> int64_t {
        switch (bound) {
        case FrameBound::UNBOUNDED_PRECEDING:
            return 0;
        case FrameBound::N_PRECEDING:
            return pos - std::max(offset, int64_t(0));
        case FrameBound::CURRENT_ROW:
            return pos;
        case FrameBound::N_FOLLOWING:
            return pos + std::max(offset, int64_t(0));
        case FrameBound::UNBOUNDED_FOLLOWING:
            return psize - 1;
        }
        return pos;
    };

    auto pos = static_cast<int64_t>(current_row);
    auto psize = static_cast<int64_t>(partition_size);
    int64_t raw_start = raw_pos(frame.start_bound, frame.start_offset, pos, psize);
    int64_t raw_end = raw_pos(frame.end_bound, frame.end_offset, pos, psize);
    return raw_start > raw_end || raw_start >= psize || raw_end < 0;
}

Result<void> WindowOperator::do_open() {
    auto child_open = child_->open();
    if (!child_open) {
        return child_open;
    }

    const auto& child_schema = child_->output_schema();

    // Step 1: Materialise all child tuples.
    std::vector<Tuple> all_tuples;
    while (true) {
        auto row = child_->next();
        if (!row) {
            return tl::unexpected(row.error());
        }
        if (!row->has_value()) {
            break;
        }
        all_tuples.push_back(std::move(row->value()));
    }
    child_->close();

    if (all_tuples.empty()) {
        output_.clear();
        cursor_ = 0;
        return ok();
    }

    // Step 2: Partition tuples by PARTITION BY columns.
    // Use a hash map: hash -> list of partition indices.
    // Each partition is a vector of indices into all_tuples.
    std::vector<std::vector<Value>> partition_keys;
    std::vector<std::vector<size_t>> partitions;
    std::unordered_multimap<size_t, size_t> partition_map;

    for (size_t row_idx = 0; row_idx < all_tuples.size(); ++row_idx) {
        auto& tuple = all_tuples[row_idx];

        // Evaluate PARTITION BY expressions.
        std::vector<Value> key;
        key.reserve(partition_by_.size());
        for (auto* expr : partition_by_) {
            auto val = evaluate_expr(*expr, tuple, child_schema, bound_);
            if (!val) {
                return tl::unexpected(val.error());
            }
            key.push_back(std::move(*val));
        }

        // Find or create partition.
        size_t h = hash_key(key);
        size_t part_idx = partitions.size();
        bool found = false;

        auto range = partition_map.equal_range(h);
        for (auto it = range.first; it != range.second; ++it) {
            if (keys_equal(partition_keys[it->second], key)) {
                part_idx = it->second;
                found = true;
                break;
            }
        }

        if (!found) {
            partition_keys.push_back(std::move(key));
            partitions.emplace_back();
            partition_map.emplace(h, part_idx);
        }

        partitions[part_idx].push_back(row_idx);
    }

    // Step 3: Sort each partition by ORDER BY keys and evaluate window functions.
    // We store results indexed by original row position.
    // For each row, we compute a vector of window function result Values.
    std::vector<std::vector<Value>> win_results(all_tuples.size());

    for (auto& part_indices : partitions) {
        // Sort partition indices by ORDER BY keys using stable_sort.
        const auto& keys = order_by_;
        const auto& bound = bound_;

        std::stable_sort(part_indices.begin(),
                         part_indices.end(),
                         [&all_tuples, &child_schema, &keys, &bound](size_t ai, size_t bi) -> bool {
                             auto& a = all_tuples[ai];
                             auto& b = all_tuples[bi];
                             for (const auto& key : keys) {
                                 auto va = evaluate_expr(*key.expr, a, child_schema, bound);
                                 auto vb = evaluate_expr(*key.expr, b, child_schema, bound);
                                 if (!va || !vb) {
                                     return false;
                                 }
                                 if (va->is_null() && vb->is_null()) {
                                     continue;
                                 }
                                 if (va->is_null()) {
                                     return key.direction == SortDirection::DESC;
                                 }
                                 if (vb->is_null()) {
                                     return key.direction == SortDirection::ASC;
                                 }
                                 auto cmp = compare(*va, *vb);
                                 if (!cmp) {
                                     return false;
                                 }
                                 if (*cmp == std::strong_ordering::less) {
                                     return key.direction == SortDirection::ASC;
                                 }
                                 if (*cmp == std::strong_ordering::greater) {
                                     return key.direction == SortDirection::DESC;
                                 }
                             }
                             return false;
                         });

        // Build a local partition vector (sorted tuples) for evaluation.
        std::vector<Tuple> partition;
        partition.reserve(part_indices.size());
        for (auto idx : part_indices) {
            partition.push_back(Tuple{all_tuples[idx].values, all_tuples[idx].rid});
        }

        size_t psize = partition.size();

        // Evaluate each window function for every row in this partition.
        for (size_t pos = 0; pos < psize; ++pos) {
            size_t original_idx = part_indices[pos];
            win_results[original_idx].reserve(functions_.size());

            for (auto& wf : functions_) {
                Value result;

                switch (wf.func) {
                    // ---- Ranking functions ----

                case WindowFunc::ROW_NUMBER:
                    result = Value(static_cast<int64_t>(pos + 1));
                    break;

                case WindowFunc::RANK: {
                    // RANK = 1 + count of rows strictly before this peer group.
                    // Find the first row in the current peer group.
                    int64_t peer_start = static_cast<int64_t>(pos);
                    for (int64_t j = static_cast<int64_t>(pos) - 1; j >= 0; --j) {
                        if (are_peers(partition[static_cast<size_t>(j)],
                                      partition[pos],
                                      order_by_,
                                      child_schema,
                                      bound_)) {
                            peer_start = j;
                        } else {
                            break;
                        }
                    }
                    result = Value(peer_start + 1);
                    break;
                }

                case WindowFunc::DENSE_RANK: {
                    // DENSE_RANK: count distinct peer groups up to and including this row.
                    int64_t dense_rank = 1;
                    for (size_t j = 1; j <= pos; ++j) {
                        if (!are_peers(
                                partition[j - 1], partition[j], order_by_, child_schema, bound_)) {
                            ++dense_rank;
                        }
                    }
                    result = Value(dense_rank);
                    break;
                }

                case WindowFunc::NTILE: {
                    int64_t buckets = wf.ntile_buckets;
                    if (buckets <= 0) {
                        buckets = 1;
                    }
                    auto total = static_cast<int64_t>(psize);
                    // NTILE distributes rows as evenly as possible.
                    // First (total % buckets) groups get (total/buckets + 1) rows,
                    // remaining groups get (total/buckets) rows.
                    int64_t base_size = total / buckets;
                    int64_t remainder = total % buckets;
                    int64_t row_pos = static_cast<int64_t>(pos);

                    // Rows in larger buckets: remainder * (base_size + 1)
                    int64_t large_bucket_rows = remainder * (base_size + 1);

                    int64_t bucket;
                    if (row_pos < large_bucket_rows) {
                        bucket = row_pos / (base_size + 1) + 1;
                    } else {
                        bucket = remainder + (row_pos - large_bucket_rows) / base_size + 1;
                    }
                    result = Value(bucket);
                    break;
                }

                    // ---- Offset functions ----

                case WindowFunc::LAG: {
                    auto target = static_cast<int64_t>(pos) - wf.offset;
                    if (target < 0 || target >= static_cast<int64_t>(psize)) {
                        result = wf.default_value;
                    } else {
                        if (wf.arg != nullptr) {
                            auto val = evaluate_expr(*wf.arg,
                                                     partition[static_cast<size_t>(target)],
                                                     child_schema,
                                                     bound_);
                            if (!val) {
                                return tl::unexpected(val.error());
                            }
                            result = std::move(*val);
                        } else {
                            result = Value::make_null();
                        }
                    }
                    break;
                }

                case WindowFunc::LEAD: {
                    auto target = static_cast<int64_t>(pos) + wf.offset;
                    if (target < 0 || target >= static_cast<int64_t>(psize)) {
                        result = wf.default_value;
                    } else {
                        if (wf.arg != nullptr) {
                            auto val = evaluate_expr(*wf.arg,
                                                     partition[static_cast<size_t>(target)],
                                                     child_schema,
                                                     bound_);
                            if (!val) {
                                return tl::unexpected(val.error());
                            }
                            result = std::move(*val);
                        } else {
                            result = Value::make_null();
                        }
                    }
                    break;
                }

                case WindowFunc::FIRST_VALUE: {
                    if (is_frame_empty(wf.frame, pos, psize)) {
                        result = Value::make_null();
                    } else if (wf.arg != nullptr) {
                        size_t frame_start =
                            resolve_bound(wf.frame.start_bound, wf.frame.start_offset, pos, psize);
                        auto val =
                            evaluate_expr(*wf.arg, partition[frame_start], child_schema, bound_);
                        if (!val) {
                            return tl::unexpected(val.error());
                        }
                        result = std::move(*val);
                    } else {
                        result = Value::make_null();
                    }
                    break;
                }

                case WindowFunc::LAST_VALUE: {
                    if (is_frame_empty(wf.frame, pos, psize)) {
                        result = Value::make_null();
                    } else if (wf.arg != nullptr) {
                        size_t frame_end =
                            resolve_bound(wf.frame.end_bound, wf.frame.end_offset, pos, psize);
                        auto val =
                            evaluate_expr(*wf.arg, partition[frame_end], child_schema, bound_);
                        if (!val) {
                            return tl::unexpected(val.error());
                        }
                        result = std::move(*val);
                    } else {
                        result = Value::make_null();
                    }
                    break;
                }

                    // ---- Aggregate window functions ----

                case WindowFunc::WIN_SUM:
                case WindowFunc::WIN_AVG:
                case WindowFunc::WIN_COUNT:
                case WindowFunc::WIN_MIN:
                case WindowFunc::WIN_MAX: {
                    if (is_frame_empty(wf.frame, pos, psize)) {
                        // Empty frame.
                        if (wf.func == WindowFunc::WIN_COUNT) {
                            result = Value(static_cast<int64_t>(0));
                        } else {
                            result = Value::make_null();
                        }
                    } else {
                        size_t frame_start =
                            resolve_bound(wf.frame.start_bound, wf.frame.start_offset, pos, psize);
                        size_t frame_end =
                            resolve_bound(wf.frame.end_bound, wf.frame.end_offset, pos, psize);
                        auto agg_result = evaluate_window_aggregate(wf.func,
                                                                    wf.arg,
                                                                    partition,
                                                                    frame_start,
                                                                    frame_end,
                                                                    child_schema,
                                                                    bound_);
                        if (!agg_result) {
                            return tl::unexpected(agg_result.error());
                        }
                        result = std::move(*agg_result);
                    }
                    break;
                }
                } // switch

                win_results[original_idx].push_back(std::move(result));
            }
        }
    }

    // Step 4: Build output tuples preserving original input order.
    // Each output tuple = child columns + window function results.
    output_.reserve(all_tuples.size());
    for (size_t i = 0; i < all_tuples.size(); ++i) {
        std::vector<Value> values;
        size_t child_cols = child_schema.column_count();
        values.reserve(child_cols + functions_.size());

        for (auto& v : all_tuples[i].values) {
            values.push_back(std::move(v));
        }
        for (auto& v : win_results[i]) {
            values.push_back(std::move(v));
        }

        output_.push_back(Tuple{std::move(values), all_tuples[i].rid});
    }

    cursor_ = 0;
    return ok();
}

Result<std::optional<Tuple>> WindowOperator::do_next() {
    if (cursor_ >= output_.size()) {
        return ok(std::optional<Tuple>(std::nullopt));
    }
    auto& t = output_[cursor_++];
    return ok(std::optional<Tuple>(Tuple{t.values, t.rid}));
}

void WindowOperator::do_close() {
    output_.clear();
    cursor_ = 0;
}

const OutputSchema& WindowOperator::output_schema() const {
    return schema_;
}

std::string WindowOperator::plan_node_name() const {
    return "WindowAgg";
}

std::string WindowOperator::plan_node_detail() const {
    return "";
}

std::vector<const Iterator*> WindowOperator::plan_children() const {
    return {child_.get()};
}

std::vector<Iterator*> WindowOperator::plan_children_mutable() {
    return {child_.get()};
}

} // namespace giodb
