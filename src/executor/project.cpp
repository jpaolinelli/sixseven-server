#include "giodb/executor/project.h"

#include "giodb/executor/expr_evaluator.h"

namespace giodb {

ProjectOperator::ProjectOperator(std::unique_ptr<Iterator> child,
                                 std::vector<ProjectionExpr> projections,
                                 OutputSchema output_schema, const BoundStatement& bound)
    : child_(std::move(child)),
      projections_(std::move(projections)),
      schema_(std::move(output_schema)),
      bound_(bound) {}

Result<void> ProjectOperator::open() {
    return child_->open();
}

Result<std::optional<Tuple>> ProjectOperator::next() {
    auto row = child_->next();
    if (!row) {
        return row;
    }
    if (!row->has_value()) {
        return row; // Child exhausted.
    }

    // Evaluate each projection expression against the input tuple.
    auto& input = row->value();
    std::vector<Value> projected;
    projected.reserve(projections_.size());

    for (const auto& proj : projections_) {
        auto val = evaluate_expr(*proj.expr, input, child_->output_schema(), bound_);
        if (!val) {
            return make_error(val.error().code, val.error().message);
        }
        projected.push_back(std::move(*val));
    }

    return ok(std::optional<Tuple>(Tuple{std::move(projected), input.rid}));
}

void ProjectOperator::close() {
    child_->close();
}

const OutputSchema& ProjectOperator::output_schema() const {
    return schema_;
}

} // namespace giodb
