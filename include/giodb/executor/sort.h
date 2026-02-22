#pragma once

#include "giodb/executor/iterator.h"
#include "giodb/executor/tuple.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace giodb {

/// One element of an ORDER BY specification.
struct SortKey {
    const Expr* expr = nullptr;
    SortDirection direction = SortDirection::ASC;
};

/// Sort operator: materialises ALL tuples from the child, sorts them
/// in memory according to the ORDER BY keys, then returns them one by one.
///
/// This is a blocking operator (open must consume all child tuples before
/// next can return the first sorted row).
class SortOperator : public Iterator {
public:
    /// @param child  The child operator to pull tuples from.
    /// @param keys   ORDER BY specification (expression + ASC/DESC).
    /// @param bound  BoundStatement with expr_types map for evaluation.
    SortOperator(std::unique_ptr<Iterator> child, std::vector<SortKey> keys,
                 const BoundStatement& bound);

    Result<void> open() override;
    Result<std::optional<Tuple>> next() override;
    void close() override;
    const OutputSchema& output_schema() const override;

private:
    std::unique_ptr<Iterator> child_;
    std::vector<SortKey> keys_;
    const BoundStatement& bound_;
    std::vector<Tuple> sorted_;
    size_t cursor_ = 0;
};

} // namespace giodb
