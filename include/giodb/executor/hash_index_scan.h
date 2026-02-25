#pragma once

#include "giodb/executor/iterator.h"
#include "giodb/executor/tuple.h"
#include "giodb/index/hash_index.h"
#include "giodb/planner/binder.h"
#include "giodb/table/table_heap.h"
#include "giodb/table/tuple.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace giodb {

/// Hash index scan operator: performs point lookups via a hash index for
/// equality predicates only. Unlike the B+ tree IndexScanOperator, this
/// operator does not support range scans.
///
/// On open(), it performs the hash lookup and collects all matching RIDs.
/// On next(), it iterates through the matches, fetching tuples from the heap
/// and applying an optional residual predicate.
class HashIndexScanOperator : public Iterator {
public:
    /// @param index           The hash index to scan.
    /// @param heap            The heap file backing the table.
    /// @param storage_schema  Byte-level schema for TupleSerializer::deserialize.
    /// @param output_schema   Logical output schema (column names/types).
    /// @param lookup_key      The key to look up in the hash index.
    /// @param index_col_indexes Positions of each index key column in the table
    ///                        schema (used for index-only scans).
    /// @param index_only      If true and index covers all output columns, skip
    ///                        heap fetches.
    /// @param predicate       Optional residual WHERE-clause predicate.
    /// @param bound           BoundStatement with expr_types map for predicate eval.
    HashIndexScanOperator(const HashIndex& index,
                          TableHeap& heap,
                          const Schema& storage_schema,
                          OutputSchema output_schema,
                          KeyType lookup_key,
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

    const HashIndex& index_;
    TableHeap& heap_;
    const Schema& storage_schema_;
    OutputSchema schema_;
    KeyType lookup_key_;
    std::vector<size_t> index_col_indexes_;
    bool index_only_;
    const Expr* predicate_;
    const BoundStatement* bound_;

    /// Matching RIDs from the hash lookup, collected on open().
    std::vector<RID> matching_rids_;
    size_t current_rid_idx_ = 0;
};

} // namespace giodb
