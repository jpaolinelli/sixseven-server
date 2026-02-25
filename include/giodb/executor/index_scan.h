#pragma once

#include "giodb/executor/iterator.h"
#include "giodb/executor/tuple.h"
#include "giodb/index/btree_index.h"
#include "giodb/index/btree_iterator.h"
#include "giodb/planner/binder.h"
#include "giodb/table/table_heap.h"
#include "giodb/table/tuple.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace giodb {

/// Index scan operator: reads tuples via a B+ tree index instead of a full
/// table scan. Supports point lookups, range scans, predicate pushdown,
/// composite key prefixes, and index-only scans.
///
/// The scan range is defined by [begin_key, end_key) bounds pushed into the
/// B+ tree's range_scan. After fetching each RID from the index, the operator
/// retrieves the full tuple from the heap and applies an optional residual
/// predicate (for conditions that couldn't be pushed into the index range).
///
/// When `index_only` is true and all output columns are present in the index
/// key, heap fetches are skipped entirely and tuples are constructed directly
/// from the index key values.
class IndexScanOperator : public Iterator {
public:
    /// @param index           The B+ tree index to scan.
    /// @param heap            The heap file backing the table.
    /// @param storage_schema  Byte-level schema for TupleSerializer::deserialize.
    /// @param output_schema   Logical output schema (column names/types).
    /// @param begin_key       Inclusive lower bound (nullopt = scan from start).
    /// @param end_key         Exclusive upper bound (nullopt = scan to end).
    /// @param index_col_indexes Positions of each index key column in the table
    ///                        schema (used for index-only scans to build tuples
    ///                        from key values). Empty disables index-only scan.
    /// @param index_only      If true and index covers all output columns, skip
    ///                        heap fetches and construct tuples from key values.
    /// @param predicate       Optional residual WHERE-clause predicate.
    /// @param bound           BoundStatement with expr_types map for predicate eval.
    IndexScanOperator(const BTreeIndex& index,
                      TableHeap& heap,
                      const Schema& storage_schema,
                      OutputSchema output_schema,
                      std::optional<KeyType> begin_key,
                      std::optional<KeyType> end_key,
                      std::vector<size_t> index_col_indexes,
                      bool index_only = false,
                      const Expr* predicate = nullptr,
                      const BoundStatement* bound = nullptr);

    Result<void> open() override;
    Result<std::optional<Tuple>> next() override;
    void close() override;
    const OutputSchema& output_schema() const override;

private:
    /// Build a Tuple directly from the index key values (index-only path).
    Tuple build_tuple_from_key(const KeyType& key, const RID& rid) const;

    const BTreeIndex& index_;
    TableHeap& heap_;
    const Schema& storage_schema_;
    OutputSchema schema_;
    std::optional<KeyType> begin_key_;
    std::optional<KeyType> end_key_;
    std::vector<size_t> index_col_indexes_;
    bool index_only_;
    const Expr* predicate_;
    const BoundStatement* bound_;
    std::optional<BTreeIterator> iter_;
};

} // namespace giodb
