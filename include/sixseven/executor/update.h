#pragma once

#include "sixseven/executor/iterator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/index/bm25_index.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"
#include "sixseven/txn/lock_manager.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sixseven {

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

    const OutputSchema& output_schema() const override;
    std::string plan_node_name() const override { return "Update"; }
    std::string plan_node_detail() const override { return ""; }
    std::vector<const Iterator*> plan_children() const override;

    /// BM25 indexes on this table to maintain on update. Set by the planner.
    std::vector<Bm25MaintenanceTarget> bm25_targets_;

    /// DECIMAL scale per storage column (from CatalogColumnDef::scale), in
    /// storage-column order. Populated by the planner for tables with DECIMAL
    /// columns. When empty or shorter than the column list, scale defaults to 0.
    /// Used by fit_to_storage() to compute the scaled coefficient (GDB-1047).
    std::vector<int32_t> col_scales_;

    /// Table id of the target table. Set by the planner; used for locking (GDB-930).
    table_id_t target_table_id_ = 0;

    /// Set the transaction id used for version stamping (GDB-747): the old
    /// version's xmax and the new version's xmin. Defaults to frozen_txn_id
    /// (always-committed) when no transaction context is provided.
    void set_txn_id(txn_id_t txn_id) { txn_id_ = txn_id; }

    /// Set the lock manager and table id for locking during UPDATE (GDB-930).
    /// When set, acquires IX on the table + X on each row before mutation.
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
    const Schema& storage_schema_;
    std::unique_ptr<Iterator> child_;
    std::vector<UpdateAssignment> assignments_;
    const BoundStatement& bound_;
    OutputSchema schema_;
    bool executed_ = false;
    /// Transaction id stamped into old (xmax) / new (xmin) versions (GDB-747).
    txn_id_t txn_id_ = frozen_txn_id;
    /// Lock manager for acquiring IX table + X row locks on UPDATE (GDB-930).
    LockManager* lock_mgr_ = nullptr;
    /// Table id used for locking (GDB-930).
    table_id_t lock_table_id_ = 0;
};

} // namespace sixseven
