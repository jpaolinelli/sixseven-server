#pragma once

#include "giodb/executor/iterator.h"
#include "giodb/executor/tuple.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"
#include "giodb/table/table_heap.h"
#include "giodb/table/tuple.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace giodb {

/// Insert operator: evaluates value rows (or pulls from a child iterator
/// for INSERT...SELECT), serialises them, and inserts into a TableHeap.
///
/// The output is a single-row tuple containing the affected row count.
/// Note: WAL logging and index updates are deferred to a later integration ticket.
class InsertOperator : public Iterator {
public:
    /// Construct for INSERT INTO ... VALUES (...), (...).
    /// @param heap           Target table heap.
    /// @param storage_schema Byte-level schema for TupleSerializer.
    /// @param value_rows     Each inner vector is one row of expression pointers.
    /// @param bound          BoundStatement for expression evaluation.
    InsertOperator(TableHeap& heap, const Schema& storage_schema,
                   std::vector<std::vector<const Expr*>> value_rows,
                   const BoundStatement& bound);

    /// Construct for INSERT INTO ... SELECT ...
    /// @param heap           Target table heap.
    /// @param storage_schema Byte-level schema for TupleSerializer.
    /// @param child          Child iterator producing rows to insert.
    InsertOperator(TableHeap& heap, const Schema& storage_schema,
                   std::unique_ptr<Iterator> child);

    Result<void> open() override;
    Result<std::optional<Tuple>> next() override;
    void close() override;
    const OutputSchema& output_schema() const override;

private:
    TableHeap& heap_;
    const Schema& storage_schema_;
    std::vector<std::vector<const Expr*>> value_rows_;
    BoundStatement bound_;
    std::unique_ptr<Iterator> child_;
    OutputSchema schema_;
    bool executed_ = false;
};

} // namespace giodb
