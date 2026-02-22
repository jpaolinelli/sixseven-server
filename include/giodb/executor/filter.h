#pragma once

#include "giodb/executor/iterator.h"
#include "giodb/executor/tuple.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"

#include <memory>

namespace giodb {

/// Filter operator: wraps a child iterator and passes through only
/// those tuples for which the predicate evaluates to true.
class FilterOperator : public Iterator {
public:
    /// @param child     The child operator to pull tuples from.
    /// @param predicate The filter expression (must evaluate to BOOL).
    /// @param bound     BoundStatement with expr_types map for evaluation.
    FilterOperator(std::unique_ptr<Iterator> child, const Expr& predicate,
                   const BoundStatement& bound);

    Result<void> open() override;
    Result<std::optional<Tuple>> next() override;
    void close() override;
    const OutputSchema& output_schema() const override;

private:
    std::unique_ptr<Iterator> child_;
    const Expr& predicate_;
    const BoundStatement& bound_;
};

} // namespace giodb
