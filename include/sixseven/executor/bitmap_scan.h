#pragma once

#include "sixseven/executor/iterator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/index/btree_index.h"
#include "sixseven/index/btree_key.h"
#include "sixseven/index/rid.h"
#include "sixseven/planner/binder.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace sixseven {

/// Describes a single index scan to include in a bitmap.
struct BitmapIndexScan {
    const BTreeIndex* index = nullptr;
    std::optional<KeyType> begin_key;
    std::optional<KeyType> end_key;
};

/// Combine mode for multiple bitmap index scans.
enum class BitmapCombineMode : uint8_t {
    OR,  ///< Union of all matching RIDs (BitmapOr).
    AND, ///< Intersection of all matching RIDs (BitmapAnd).
};

/// Bitmap index scan operator: collects matching RIDs from one or more B+ tree
/// index scans, sorts them by page_id for sequential I/O, then fetches tuples
/// from the heap in page order.
///
/// This reduces random I/O compared to a plain index scan for medium-selectivity
/// queries. Supports combining multiple index scans via BitmapAnd / BitmapOr.
///
/// Phase 1 (open): scan indexes, collect RIDs, combine, sort by page_id.
/// Phase 2 (next): fetch tuples from heap in page order, apply residual predicate.
class BitmapScanOperator : public Iterator {
public:
    /// @param scans           One or more index scan descriptors.
    /// @param combine_mode    How to combine RIDs from multiple scans (OR/AND).
    /// @param heap            The heap file backing the table.
    /// @param storage_schema  Byte-level schema for TupleSerializer::deserialize.
    /// @param output_schema   Logical output schema (column names/types).
    /// @param predicate       Optional residual WHERE-clause predicate.
    /// @param bound           BoundStatement with expr_types map for predicate eval.
    BitmapScanOperator(std::vector<BitmapIndexScan> scans,
                       BitmapCombineMode combine_mode,
                       TableHeap& heap,
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
    /// Collect all matching RIDs from a single index scan.
    [[nodiscard]] Result<std::vector<RID>> collect_rids(const BitmapIndexScan& scan) const;

    /// Combine RID sets using OR (union).
    [[nodiscard]] static std::vector<RID> bitmap_or(const std::vector<std::vector<RID>>& rid_sets);

    /// Combine RID sets using AND (intersection).
    [[nodiscard]] static std::vector<RID> bitmap_and(const std::vector<std::vector<RID>>& rid_sets);

    std::vector<BitmapIndexScan> scans_;
    BitmapCombineMode combine_mode_;
    TableHeap& heap_;
    const Schema& storage_schema_;
    OutputSchema schema_;
    const Expr* predicate_;
    const BoundStatement* bound_;

    /// Sorted RIDs collected during open().
    std::vector<RID> rids_;
    size_t rid_pos_ = 0;
};

} // namespace sixseven
