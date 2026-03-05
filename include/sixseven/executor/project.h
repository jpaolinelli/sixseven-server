#pragma once

#include "sixseven/executor/iterator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"

#include <memory>
#include <string>
#include <vector>

namespace sixseven {

// Forward declaration.
struct SubqueryContext;

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
                    std::vector<ProjectionExpr> projections,
                    OutputSchema output_schema,
                    const BoundStatement& bound,
                    const SubqueryContext* subquery_ctx = nullptr);

    const OutputSchema& output_schema() const override;

    // Plan inspection
    std::string plan_node_name() const override;
    std::vector<const Iterator*> plan_children() const override;

protected:
    Result<void> do_open() override;
    Result<std::optional<Tuple>> do_next() override;
    void do_close() override;
    std::vector<Iterator*> plan_children_mutable() override;

private:
    std::unique_ptr<Iterator> child_;
    std::vector<ProjectionExpr> projections_;
    OutputSchema schema_;
    const BoundStatement& bound_;
    const SubqueryContext* subquery_ctx_ = nullptr;
};

} // namespace sixseven
