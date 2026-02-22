#pragma once

#include "giodb/executor/iterator.h"
#include "giodb/executor/tuple.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"

#include <memory>
#include <string>
#include <vector>

namespace giodb {

/// A single projection expression: an Expr* to evaluate plus the output column alias.
struct ProjectionExpr {
    const Expr* expr = nullptr;
    std::string alias; ///< Output column name (SELECT ... AS alias).
};

/// Projection operator: evaluates a list of expressions for each input tuple,
/// producing a new tuple with the projected columns.
///
/// Handles: column references, arithmetic expressions, literals, aliases.
class ProjectOperator : public Iterator {
public:
    /// @param child        The child operator to pull tuples from.
    /// @param projections  List of expressions to evaluate per row.
    /// @param output_schema The output schema (computed from projections).
    /// @param bound        BoundStatement with expr_types map for evaluation.
    ProjectOperator(std::unique_ptr<Iterator> child,
                    std::vector<ProjectionExpr> projections, OutputSchema output_schema,
                    const BoundStatement& bound);

    Result<void> open() override;
    Result<std::optional<Tuple>> next() override;
    void close() override;
    const OutputSchema& output_schema() const override;

private:
    std::unique_ptr<Iterator> child_;
    std::vector<ProjectionExpr> projections_;
    OutputSchema schema_;
    const BoundStatement& bound_;
};

} // namespace giodb
