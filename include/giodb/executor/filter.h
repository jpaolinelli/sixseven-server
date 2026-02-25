#pragma once

#include "giodb/executor/iterator.h"
#include "giodb/executor/tuple.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"

#include <memory>

namespace giodb {

// Forward declaration.
struct SubqueryContext;

/// Filter operator: wraps a child iterator and passes through only
/// those tuples for which the predicate evaluates to true.
class FilterOperator : public Iterator {
public:
    /// @param child     The child operator to pull tuples from.
    /// @param predicate The filter expression (must evaluate to BOOL).
    /// @param bound     BoundStatement with expr_types map for evaluation.
    FilterOperator(std::unique_ptr<Iterator> child,
                   const Expr& predicate,
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
    const Expr& predicate_;
    const BoundStatement& bound_;
    const SubqueryContext* subquery_ctx_ = nullptr;
};

} // namespace giodb
