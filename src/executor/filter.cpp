#include "giodb/executor/filter.h"

#include "giodb/executor/expr_evaluator.h"

namespace giodb {

FilterOperator::FilterOperator(std::unique_ptr<Iterator> child,
                               const Expr& predicate,
                               const BoundStatement& bound)
    : child_(std::move(child)), predicate_(predicate), bound_(bound) {}

Result<void> FilterOperator::open() {
    return child_->open();
}

Result<std::optional<Tuple>> FilterOperator::next() {
    while (true) {
        auto row = child_->next();
        if (!row) {
            return row;
        }
        if (!row->has_value()) {
            return row; // Child exhausted.
        }

        auto pass = evaluate_predicate(predicate_, row->value(), child_->output_schema(), bound_);
        if (!pass) {
            return make_error(pass.error().code, pass.error().message);
        }
        if (*pass) {
            return row;
        }
        // Predicate rejected — try next row.
    }
}

void FilterOperator::close() {
    child_->close();
}

const OutputSchema& FilterOperator::output_schema() const {
    return child_->output_schema();
}

} // namespace giodb
