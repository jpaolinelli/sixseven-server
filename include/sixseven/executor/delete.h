#pragma once

#include "sixseven/executor/iterator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/index/bm25_index.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/txn/lock_manager.h"
#include "sixseven/vector/hnsw_index.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace sixseven {

/// Delete operator: scans tuples from a child iterator and deletes
/// each one from the heap.
///
/// The output is a single-row tuple containing the affected row count.
/// Note: WAL logging and index updates are deferred to a later integration ticket.
class DeleteOperator : public Iterator {
public:
    /// @param heap  Target table heap.
    /// @param child Child iterator yielding tuples to delete (with WHERE applied).
    DeleteOperator(TableHeap& heap, std::unique_ptr<Iterator> child);

    const OutputSchema& output_schema() const override;
    std::string plan_node_name() const override { return "Delete"; }
    std::string plan_node_detail() const override { return ""; }
    std::vector<const Iterator*> plan_children() const override;

    /// BM25 indexes on this table to maintain on delete. Set by the planner.
    /// Only the index pointer is used (removal is keyed by RID).
    std::vector<Bm25MaintenanceTarget> bm25_targets_;

    /// Table id of the target table. Set by the planner; used for locking (GDB-930).
    table_id_t target_table_id_ = 0;

    /// HNSW indexes on this table to maintain on delete. Set by the planner.
    /// On delete, the node corresponding to the deleted RID is tombstoned and
    /// its rid_map slot is invalidated so nearest-scan skips it consistently.
    std::vector<HnswMaintenanceTarget> hnsw_targets_;

    /// Set the transaction id stamped as xmax on deleted versions (GDB-747).
    /// Defaults to frozen_txn_id (always-committed) when no transaction
    /// context is provided.
    void set_txn_id(txn_id_t txn_id) { txn_id_ = txn_id; }

    /// Set the lock manager and table id for locking during DELETE (GDB-930).
    /// When set, acquires IX on the table + X on each row before deletion.
    void set_lock_manager(LockManager* lock_mgr, table_id_t table_id) {
        lock_mgr_ = lock_mgr;
        lock_table_id_ = table_id;
    }

    /// The heap this operator writes to (for executor-side bookkeeping).
    [[nodiscard]] TableHeap& target_heap() { return heap_; }

protected:
    Result<void> do_open() override;
    Result<std::optional<Tuple>> do_next() override;
    void do_close() override;
    std::vector<Iterator*> plan_children_mutable() override;

private:
    TableHeap& heap_;
    std::unique_ptr<Iterator> child_;
    OutputSchema schema_;
    bool executed_ = false;
    /// Transaction id stamped as xmax on deleted tuple versions (GDB-747).
    txn_id_t txn_id_ = frozen_txn_id;
    /// Lock manager for acquiring IX table + X row locks on DELETE (GDB-930).
    LockManager* lock_mgr_ = nullptr;
    /// Table id used for locking (GDB-930).
    table_id_t lock_table_id_ = 0;
};

} // namespace sixseven
