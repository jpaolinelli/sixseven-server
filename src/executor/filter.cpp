#include "sixseven/executor/filter.h"

#include "sixseven/executor/expr_evaluator.h"

namespace sixseven {

FilterOperator::FilterOperator(std::unique_ptr<Iterator> child,
                               const Expr& predicate,
                               const BoundStatement& bound,
                               const SubqueryContext* subquery_ctx)
    : child_(std::move(child)), predicate_(predicate), bound_(bound), subquery_ctx_(subquery_ctx) {}

Result<void> FilterOperator::do_open() {
    return child_->open();
}

Result<std::optional<Tuple>> FilterOperator::do_next() {
    while (true) {
        auto row = child_->next();
        if (!row) {
            return row;
        }
        if (!row->has_value()) {
            return row; // Child exhausted.
        }

        auto pass = evaluate_predicate(
            predicate_, row->value(), child_->output_schema(), bound_, subquery_ctx_);
        if (!pass) {
            return make_error(pass.error().code, pass.error().message);
        }
        if (*pass) {
            return row;
        }
        // Predicate rejected — try next row.
    }
}

void FilterOperator::do_close() {
    child_->close();
}

const OutputSchema& FilterOperator::output_schema() const {
    return child_->output_schema();
}

std::string FilterOperator::plan_node_name() const {
    return "Filter";
}

std::vector<const Iterator*> FilterOperator::plan_children() const {
    return {child_.get()};
}

std::vector<Iterator*> FilterOperator::plan_children_mutable() {
    return {child_.get()};
}

} // namespace sixseven
