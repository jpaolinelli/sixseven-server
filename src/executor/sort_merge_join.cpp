#include "giodb/executor/sort_merge_join.h"

#include "giodb/common/coercion.h"
#include "giodb/executor/expr_evaluator.h"

#include <algorithm>

namespace giodb {

SortMergeJoinOperator::SortMergeJoinOperator(std::unique_ptr<Iterator> left,
                                             std::unique_ptr<Iterator> right,
                                             JoinType type,
                                             const Expr* left_key,
                                             const Expr* right_key,
                                             const BoundStatement& bound,
                                             OutputSchema schema)
    : left_(std::move(left)), right_(std::move(right)), type_(type), left_key_(left_key),
      right_key_(right_key), bound_(bound), schema_(std::move(schema)) {}

// ---------------------------------------------------------------------------
// open — materialise, sort, and merge
// ---------------------------------------------------------------------------

Result<void> SortMergeJoinOperator::do_open() {
    auto lr = left_->open();
    if (!lr) {
        return lr;
    }
    auto rr = right_->open();
    if (!rr) {
        return rr;
    }

    left_col_count_ = left_->output_schema().column_count();
    right_col_count_ = right_->output_schema().column_count();

    // Materialise left.
    std::vector<Tuple> left_tuples;
    while (true) {
        auto row = left_->next();
        if (!row) {
            return make_error(row.error().code, row.error().message);
        }
        if (!row->has_value()) {
            break;
        }
        left_tuples.push_back(std::move(row->value()));
    }
    left_->close();

    // Materialise right.
    std::vector<Tuple> right_tuples;
    while (true) {
        auto row = right_->next();
        if (!row) {
            return make_error(row.error().code, row.error().message);
        }
        if (!row->has_value()) {
            break;
        }
        right_tuples.push_back(std::move(row->value()));
    }
    right_->close();

    const auto& left_schema = left_->output_schema();
    const auto& right_schema = right_->output_schema();

    // Sort function: returns a comparator for tuples by key expression.
    auto make_comparator = [this](const Expr* key_expr, const OutputSchema& key_schema) {
        return [key_expr, &key_schema, this](const Tuple& a, const Tuple& b) -> bool {
            auto va = evaluate_expr(*key_expr, a, key_schema, bound_);
            auto vb = evaluate_expr(*key_expr, b, key_schema, bound_);
            if (!va || !vb) {
                return false;
            }
            // NULLs sort last.
            if (va->is_null() && vb->is_null()) {
                return false;
            }
            if (va->is_null()) {
                return false;
            }
            if (vb->is_null()) {
                return true;
            }
            auto cmp = compare(*va, *vb);
            if (!cmp) {
                return false;
            }
            return *cmp == std::strong_ordering::less;
        };
    };

    // Sort both sides.
    std::stable_sort(
        left_tuples.begin(), left_tuples.end(), make_comparator(left_key_, left_schema));
    std::stable_sort(
        right_tuples.begin(), right_tuples.end(), make_comparator(right_key_, right_schema));

    // Merge phase.
    output_.clear();
    output_cursor_ = 0;

    size_t li = 0;
    size_t ri = 0;

    // Helper to evaluate key values.
    auto left_val = [&](size_t idx) -> Result<Value> {
        return evaluate_expr(*left_key_, left_tuples[idx], left_schema, bound_);
    };
    auto right_val = [&](size_t idx) -> Result<Value> {
        return evaluate_expr(*right_key_, right_tuples[idx], right_schema, bound_);
    };

    // Helper to combine tuples.
    auto combine = [&](const Tuple& l, const Tuple& r) -> Tuple {
        Tuple result;
        result.values.reserve(left_col_count_ + right_col_count_);
        result.values.insert(result.values.end(), l.values.begin(), l.values.end());
        result.values.insert(result.values.end(), r.values.begin(), r.values.end());
        return result;
    };

    auto left_with_null_right = [&](const Tuple& l) -> Tuple {
        Tuple result;
        result.values.reserve(left_col_count_ + right_col_count_);
        result.values.insert(result.values.end(), l.values.begin(), l.values.end());
        for (size_t i = 0; i < right_col_count_; ++i) {
            result.values.push_back(Value::make_null());
        }
        return result;
    };

    auto null_left_with_right = [&](const Tuple& r) -> Tuple {
        Tuple result;
        result.values.reserve(left_col_count_ + right_col_count_);
        for (size_t i = 0; i < left_col_count_; ++i) {
            result.values.push_back(Value::make_null());
        }
        result.values.insert(result.values.end(), r.values.begin(), r.values.end());
        return result;
    };

    while (li < left_tuples.size() && ri < right_tuples.size()) {
        auto lv = left_val(li);
        auto rv = right_val(ri);

        if (!lv || !rv) {
            return make_error(StatusCode::INTERNAL_ERROR, "failed to evaluate join key");
        }

        // Handle NULLs: NULLs don't match anything.
        if (lv->is_null()) {
            if (type_ == JoinType::LEFT || type_ == JoinType::FULL) {
                output_.push_back(left_with_null_right(left_tuples[li]));
            }
            ++li;
            continue;
        }
        if (rv->is_null()) {
            if (type_ == JoinType::RIGHT || type_ == JoinType::FULL) {
                output_.push_back(null_left_with_right(right_tuples[ri]));
            }
            ++ri;
            continue;
        }

        auto cmp = compare(*lv, *rv);
        if (!cmp) {
            return make_error(StatusCode::INTERNAL_ERROR, "failed to compare join keys");
        }

        if (*cmp == std::strong_ordering::less) {
            // Left key < right key: left has no match.
            if (type_ == JoinType::LEFT || type_ == JoinType::FULL) {
                output_.push_back(left_with_null_right(left_tuples[li]));
            }
            ++li;
        } else if (*cmp == std::strong_ordering::greater) {
            // Left key > right key: right has no match.
            if (type_ == JoinType::RIGHT || type_ == JoinType::FULL) {
                output_.push_back(null_left_with_right(right_tuples[ri]));
            }
            ++ri;
        } else {
            // Keys are equal — handle duplicates on both sides.
            // Find the range of equal keys on left and right.
            size_t li_start = li;
            while (li < left_tuples.size()) {
                auto next_lv = left_val(li);
                if (!next_lv || next_lv->is_null()) {
                    break;
                }
                auto eq = compare(*lv, *next_lv);
                if (!eq || *eq != std::strong_ordering::equal) {
                    break;
                }
                ++li;
            }

            size_t ri_start = ri;
            while (ri < right_tuples.size()) {
                auto next_rv = right_val(ri);
                if (!next_rv || next_rv->is_null()) {
                    break;
                }
                auto eq = compare(*rv, *next_rv);
                if (!eq || *eq != std::strong_ordering::equal) {
                    break;
                }
                ++ri;
            }

            // Cross-product of the matching groups.
            for (size_t l = li_start; l < li; ++l) {
                for (size_t r = ri_start; r < ri; ++r) {
                    output_.push_back(combine(left_tuples[l], right_tuples[r]));
                }
            }
        }
    }

    // Remaining left tuples (no right match).
    while (li < left_tuples.size()) {
        if (type_ == JoinType::LEFT || type_ == JoinType::FULL) {
            output_.push_back(left_with_null_right(left_tuples[li]));
        }
        ++li;
    }

    // Remaining right tuples (no left match).
    while (ri < right_tuples.size()) {
        if (type_ == JoinType::RIGHT || type_ == JoinType::FULL) {
            output_.push_back(null_left_with_right(right_tuples[ri]));
        }
        ++ri;
    }

    return ok();
}

// ---------------------------------------------------------------------------
// next / close / output_schema
// ---------------------------------------------------------------------------

Result<std::optional<Tuple>> SortMergeJoinOperator::do_next() {
    if (output_cursor_ >= output_.size()) {
        return ok(std::optional<Tuple>(std::nullopt));
    }
    return ok(std::optional<Tuple>(std::move(output_[output_cursor_++])));
}

void SortMergeJoinOperator::do_close() {
    output_.clear();
    output_cursor_ = 0;
}

const OutputSchema& SortMergeJoinOperator::output_schema() const {
    return schema_;
}

// ---------------------------------------------------------------------------
// Plan inspection
// ---------------------------------------------------------------------------

std::string SortMergeJoinOperator::plan_node_name() const {
    return "Merge Join";
}

std::string SortMergeJoinOperator::plan_node_detail() const {
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

std::vector<const Iterator*> SortMergeJoinOperator::plan_children() const {
    return {left_.get(), right_.get()};
}

std::vector<Iterator*> SortMergeJoinOperator::plan_children_mutable() {
    return {left_.get(), right_.get()};
}

} // namespace giodb
