#pragma once

#include "giodb/executor/iterator.h"
#include "giodb/executor/tuple.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"
#include "giodb/table/table_heap.h"
#include "giodb/table/tuple.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace giodb {

/// One SET assignment: column_index = expr.
struct UpdateAssignment {
    size_t column_index = 0; ///< Index in the storage schema.
    const Expr* expr = nullptr;
};

/// Update operator: scans tuples from a child iterator, applies SET
/// assignments, and updates each tuple in the heap.
///
/// The output is a single-row tuple containing the affected row count.
/// Note: WAL logging and index updates are deferred to a later integration ticket.
class UpdateOperator : public Iterator {
public:
    /// @param heap           Target table heap.
    /// @param storage_schema Byte-level schema for TupleSerializer.
    /// @param child          Child iterator yielding tuples to update (with WHERE applied).
    /// @param assignments    SET column = expr assignments.
    /// @param bound          BoundStatement for expression evaluation.
    UpdateOperator(TableHeap& heap,
                   const Schema& storage_schema,
                   std::unique_ptr<Iterator> child,
                   std::vector<UpdateAssignment> assignments,
                   const BoundStatement& bound);

    Result<void> open() override;
    Result<std::optional<Tuple>> next() override;
    void close() override;
    const OutputSchema& output_schema() const override;

private:
    TableHeap& heap_;
    const Schema& storage_schema_;
    std::unique_ptr<Iterator> child_;
    std::vector<UpdateAssignment> assignments_;
    const BoundStatement& bound_;
    OutputSchema schema_;
    bool executed_ = false;
};

} // namespace giodb
