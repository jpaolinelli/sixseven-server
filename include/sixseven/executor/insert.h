#pragma once

#include "sixseven/catalog/schema.h"
#include "sixseven/common/types.h"
#include "sixseven/executor/iterator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/index/bm25_index.h"
#include "sixseven/index/rid.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sixseven {

class Catalog;
class EmbeddingWorkerPool;

/// Describes an auto-increment column for the InsertOperator.
struct AutoIncrementCol {
    size_t col_idx;      ///< Column index in the storage schema.
    TypeId type_id;      ///< Column type for overflow checking and value creation.
    table_id_t table_id; ///< Table ID for catalog counter lookup.
    bool is_placeholder; ///< True if the column was omitted and filled with a NULL placeholder.
};

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
    InsertOperator(TableHeap& heap,
                   const Schema& storage_schema,
                   std::vector<std::vector<const Expr*>> value_rows,
                   const BoundStatement& bound);

    /// Construct for INSERT INTO ... SELECT ...
    /// @param heap           Target table heap.
    /// @param storage_schema Byte-level schema for TupleSerializer.
    /// @param child          Child iterator producing rows to insert.
    InsertOperator(TableHeap& heap, const Schema& storage_schema, std::unique_ptr<Iterator> child);

    const OutputSchema& output_schema() const override;
    std::string plan_node_name() const override { return "Insert"; }
    std::string plan_node_detail() const override { return ""; }
    std::vector<const Iterator*> plan_children() const override;

    /// Owns default expressions parsed by the planner for missing INSERT columns.
    /// Set by the planner after construction; must outlive the operator.
    std::vector<ExprPtr> owned_default_exprs_;

    /// Auto-increment column info. Set by the planner after construction.
    std::vector<AutoIncrementCol> autoincrement_cols_;

    /// Catalog reference for auto-increment counter management.
    /// Set by the planner if the table has auto-increment columns.
    Catalog* catalog_ = nullptr;

    /// Table ID for embedding job creation. Set by the planner.
    table_id_t embedding_table_id_ = 0;

    /// Column names in schema order, for resolving source_expr to value index.
    std::vector<std::string> column_names_;

    /// Embedding column definitions for this table. Set by the planner.
    std::vector<EmbeddingColumnDef> embedding_cols_;

    /// Embedding worker pool for async embedding generation. Set by the planner.
    EmbeddingWorkerPool* embedding_pool_ = nullptr;

    /// BM25 indexes on this table to maintain on insert. Set by the planner.
    std::vector<Bm25MaintenanceTarget> bm25_targets_;

    /// Set the transaction id stamped as xmin on inserted versions (GDB-747).
    /// Defaults to frozen_txn_id (always-committed) when no transaction
    /// context is provided.
    void set_txn_id(txn_id_t txn_id) { txn_id_ = txn_id; }

    /// The heap this operator writes to (for executor-side bookkeeping).
    [[nodiscard]] TableHeap& target_heap() { return heap_; }

protected:
    Result<void> do_open() override;
    Result<std::optional<Tuple>> do_next() override;
    void do_close() override;
    std::vector<Iterator*> plan_children_mutable() override;

private:
    /// Create a Value from an int64_t counter for the given type.
    static Result<Value> make_autoincrement_value(int64_t counter, TypeId type_id);

    /// Enqueue async embedding jobs for EMBEDDING columns on the table.
    /// Best-effort: logs warnings on failure but does not propagate errors.
    void enqueue_embedding_jobs(const RID& rid, const std::vector<Value>& values);

    /// Synchronously index the row's text into any BM25 indexes on the table.
    /// Best-effort: logs warnings on failure but does not propagate errors.
    void maintain_bm25(const RID& rid, const std::vector<Value>& values);

    TableHeap& heap_;
    const Schema& storage_schema_;
    std::vector<std::vector<const Expr*>> value_rows_;
    BoundStatement bound_;
    std::unique_ptr<Iterator> child_;
    OutputSchema schema_;
    bool executed_ = false;
    /// Transaction id stamped as xmin on inserted tuple versions (GDB-747).
    txn_id_t txn_id_ = frozen_txn_id;
};

} // namespace sixseven
