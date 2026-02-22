#pragma once

#include "giodb/executor/iterator.h"
#include "giodb/executor/tuple.h"
#include "giodb/planner/binder.h"
#include "giodb/table/table_heap.h"
#include "giodb/table/tuple.h"

#include <optional>

namespace giodb {

/// Sequential scan operator: reads all tuples from a TableHeap,
/// deserialises them into Values, and optionally applies a predicate.
class SeqScanOperator : public Iterator {
public:
    /// @param heap          The heap file to scan.
    /// @param storage_schema Byte-level schema for TupleSerializer::deserialize.
    /// @param output_schema  Logical output schema (column names/types).
    /// @param predicate      Optional WHERE-clause expression (nullptr = no filter).
    /// @param bound          BoundStatement with expr_types map for predicate eval.
    SeqScanOperator(TableHeap& heap, const Schema& storage_schema,
                    OutputSchema output_schema, const Expr* predicate = nullptr,
                    const BoundStatement* bound = nullptr);

    Result<void> open() override;
    Result<std::optional<Tuple>> next() override;
    void close() override;
    const OutputSchema& output_schema() const override;

private:
    TableHeap& heap_;
    const Schema& storage_schema_;
    OutputSchema schema_;
    const Expr* predicate_;
    const BoundStatement* bound_;
    std::optional<TableIterator> iter_;
};

} // namespace giodb
