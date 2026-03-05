#pragma once

#include "sixseven/executor/iterator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/planner/binder.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"

#include <optional>

namespace sixseven {

/// Sequential scan operator: reads all tuples from a TableHeap,
/// deserialises them into Values, and optionally applies a predicate.
class SeqScanOperator : public Iterator {
public:
    /// @param heap          The heap file to scan.
    /// @param storage_schema Byte-level schema for TupleSerializer::deserialize.
    /// @param output_schema  Logical output schema (column names/types).
    /// @param predicate      Optional WHERE-clause expression (nullptr = no filter).
    /// @param bound          BoundStatement with expr_types map for predicate eval.
    SeqScanOperator(TableHeap& heap,
                    const Schema& storage_schema,
                    OutputSchema output_schema,
                    const Expr* predicate = nullptr,
                    const BoundStatement* bound = nullptr);

    const OutputSchema& output_schema() const override;

    // Plan inspection
    std::string plan_node_name() const override;
    std::string plan_node_detail() const override;

protected:
    Result<void> do_open() override;
    Result<std::optional<Tuple>> do_next() override;
    void do_close() override;

private:
    TableHeap& heap_;
    const Schema& storage_schema_;
    OutputSchema schema_;
    const Expr* predicate_;
    const BoundStatement* bound_;
    std::optional<TableIterator> iter_;
};

} // namespace sixseven
