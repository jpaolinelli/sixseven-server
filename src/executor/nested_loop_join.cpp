#include "sixseven/executor/nested_loop_join.h"

#include "sixseven/executor/expr_evaluator.h"

namespace sixseven {

NestedLoopJoinOperator::NestedLoopJoinOperator(std::unique_ptr<Iterator> left,
                                               std::unique_ptr<Iterator> right,
                                               JoinType type,
                                               const Expr* on_expr,
                                               const BoundStatement& bound,
                                               OutputSchema schema)
    : left_(std::move(left)), right_(std::move(right)), type_(type), on_expr_(on_expr),
      bound_(bound), schema_(std::move(schema)), output_schema_(schema_) {
    // SEMI/ANTI joins only output left-side columns, so build a trimmed schema.
    if (type_ == JoinType::SEMI || type_ == JoinType::ANTI) {
        size_t left_count = left_->output_schema().column_count();
        std::vector<OutputColumn> left_cols;
        left_cols.reserve(left_count);
        for (size_t i = 0; i < left_count && i < schema_.column_count(); ++i) {
            left_cols.push_back(schema_.column(i));
        }
        output_schema_ = OutputSchema(std::move(left_cols));
    }
}

// ---------------------------------------------------------------------------
// open
// ---------------------------------------------------------------------------

Result<void> NestedLoopJoinOperator::do_open() {
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

    // Materialise all right-side tuples.
    right_tuples_.clear();
    while (true) {
        auto row = right_->next();
        if (!row) {
            return make_error(row.error().code, row.error().message);
        }
        if (!row->has_value()) {
            break;
        }
        right_tuples_.push_back(std::move(row->value()));
    }
    right_->close();

    // Initialise tracking for RIGHT/FULL outer joins.
    if (type_ == JoinType::RIGHT || type_ == JoinType::FULL) {
        right_matched_.assign(right_tuples_.size(), false);
    }

    // State initialisation.
    current_left_ = std::nullopt;
    right_cursor_ = 0;
    left_exhausted_ = false;
    left_matched_ = false;
    emitting_unmatched_right_ = false;
    unmatched_right_cursor_ = 0;

    return ok();
}

// ---------------------------------------------------------------------------
// next
// ---------------------------------------------------------------------------

Result<std::optional<Tuple>> NestedLoopJoinOperator::do_next() {
    // Phase 2: emit unmatched right tuples (RIGHT/FULL only).
    if (emitting_unmatched_right_) {
        while (unmatched_right_cursor_ < right_tuples_.size()) {
            size_t idx = unmatched_right_cursor_++;
            if (!right_matched_[idx]) {
                return ok(std::optional<Tuple>(null_left_with_right(right_tuples_[idx])));
            }
        }
        return ok(std::optional<Tuple>(std::nullopt));
    }

    // Phase 1: nested loop over left x right.
    while (true) {
        // Fetch next left tuple if needed.
        if (!current_left_.has_value()) {
            if (left_exhausted_) {
                // Transition to phase 2 for RIGHT/FULL.
                if (type_ == JoinType::RIGHT || type_ == JoinType::FULL) {
                    emitting_unmatched_right_ = true;
                    return next();
                }
                return ok(std::optional<Tuple>(std::nullopt));
            }

            auto row = left_->next();
            if (!row) {
                return make_error(row.error().code, row.error().message);
            }
            if (!row->has_value()) {
                left_exhausted_ = true;
                if (type_ == JoinType::RIGHT || type_ == JoinType::FULL) {
                    emitting_unmatched_right_ = true;
                    return next();
                }
                return ok(std::optional<Tuple>(std::nullopt));
            }

            current_left_ = std::move(row->value());
            right_cursor_ = 0;
            left_matched_ = false;
            saw_null_ = false;
        }

        // Scan right tuples for current left.
        while (right_cursor_ < right_tuples_.size()) {
            size_t idx = right_cursor_++;
            auto combined = combine(*current_left_, right_tuples_[idx]);

            // CROSS JOIN: no predicate, always matches.
            bool match = (type_ == JoinType::CROSS);
            if (!match && on_expr_) {
                // For null-aware ANTI join, use evaluate_expr to detect NULLs.
                if (null_aware_anti_ && type_ == JoinType::ANTI) {
                    auto val = evaluate_expr(*on_expr_, combined, schema_, bound_);
                    if (!val) {
                        return make_error(val.error().code, val.error().message);
                    }
                    if (val->is_null()) {
                        saw_null_ = true;
                    } else if (val->type_id() == TypeId::BOOL && val->as_bool()) {
                        match = true;
                    }
                } else {
                    auto pred = matches(combined);
                    if (!pred) {
                        return make_error(pred.error().code, pred.error().message);
                    }
                    match = *pred;
                }
            } else if (!on_expr_ && type_ != JoinType::CROSS) {
                // No predicate and not CROSS → treat as cross join (all match).
                match = true;
            }

            if (match) {
                left_matched_ = true;
                if (type_ == JoinType::RIGHT || type_ == JoinType::FULL) {
                    right_matched_[idx] = true;
                }

                switch (type_) {
                case JoinType::SEMI:
                    // Emit left tuple once, skip remaining right tuples.
                    current_left_ = std::nullopt;
                    return ok(std::optional<Tuple>(
                        Tuple{std::vector<Value>(combined.values.begin(),
                                                 combined.values.begin() +
                                                     static_cast<ptrdiff_t>(left_col_count_)),
                              {}}));

                case JoinType::ANTI:
                    // Match found → skip this left tuple entirely.
                    current_left_ = std::nullopt;
                    break;

                default:
                    // INNER, LEFT, RIGHT, FULL, CROSS → emit combined tuple.
                    return ok(std::optional<Tuple>(std::move(combined)));
                }

                if (type_ == JoinType::ANTI) {
                    break; // break inner loop, fetch next left
                }
            }
        }

        // Right side exhausted for current left tuple.
        if (type_ == JoinType::ANTI && !left_matched_) {
            // Null-aware: if any comparison yielded NULL, suppress emission.
            if (saw_null_) {
                current_left_ = std::nullopt;
                continue;
            }
            // No match found → emit left tuple.
            Tuple result;
            result.values = std::move(current_left_->values);
            current_left_ = std::nullopt;
            return ok(std::optional<Tuple>(std::move(result)));
        }

        if ((type_ == JoinType::LEFT || type_ == JoinType::FULL) && !left_matched_) {
            // Left outer: emit left with NULL right.
            auto result = left_with_null_right(*current_left_);
            current_left_ = std::nullopt;
            return ok(std::optional<Tuple>(std::move(result)));
        }

        // Move to next left tuple.
        current_left_ = std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// close
// ---------------------------------------------------------------------------

void NestedLoopJoinOperator::do_close() {
    right_tuples_.clear();
    right_matched_.clear();
    current_left_ = std::nullopt;
    left_->close();
}

// ---------------------------------------------------------------------------
// output_schema
// ---------------------------------------------------------------------------

const OutputSchema& NestedLoopJoinOperator::output_schema() const {
    return output_schema_;
}

// ---------------------------------------------------------------------------
// Plan inspection
// ---------------------------------------------------------------------------

std::string NestedLoopJoinOperator::plan_node_name() const {
    return "Nested Loop";
}

std::string NestedLoopJoinOperator::plan_node_detail() const {
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

std::vector<const Iterator*> NestedLoopJoinOperator::plan_children() const {
    return {left_.get(), right_.get()};
}

std::vector<Iterator*> NestedLoopJoinOperator::plan_children_mutable() {
    return {left_.get(), right_.get()};
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Tuple NestedLoopJoinOperator::combine(const Tuple& left, const Tuple& right) const {
    Tuple result;
    result.values.reserve(left_col_count_ + right_col_count_);
    result.values.insert(result.values.end(), left.values.begin(), left.values.end());
    result.values.insert(result.values.end(), right.values.begin(), right.values.end());
    return result;
}

Tuple NestedLoopJoinOperator::left_with_null_right(const Tuple& left) const {
    Tuple result;
    result.values.reserve(left_col_count_ + right_col_count_);
    result.values.insert(result.values.end(), left.values.begin(), left.values.end());
    for (size_t i = 0; i < right_col_count_; ++i) {
        result.values.push_back(Value::make_null());
    }
    return result;
}

Tuple NestedLoopJoinOperator::null_left_with_right(const Tuple& right) const {
    Tuple result;
    result.values.reserve(left_col_count_ + right_col_count_);
    for (size_t i = 0; i < left_col_count_; ++i) {
        result.values.push_back(Value::make_null());
    }
    result.values.insert(result.values.end(), right.values.begin(), right.values.end());
    return result;
}

Result<bool> NestedLoopJoinOperator::matches(const Tuple& combined) const {
    return evaluate_predicate(*on_expr_, combined, schema_, bound_);
}

} // namespace sixseven
