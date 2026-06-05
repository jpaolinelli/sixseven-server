#pragma once

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/executor/iterator.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/index/bm25_index.h"
#include "sixseven/index/btree_index.h"
#include "sixseven/index/hash_index.h"
#include "sixseven/index/rid.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/vector/hnsw_index.h"
#include "sixseven/vector/provider_registry.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sixseven {

class EmbeddingWorkerPool;
struct MatchConfig;

/// Translates a BoundStatement into a physical operator (Iterator) tree.
///
/// The planner directly constructs Volcano iterators without an
/// intermediate logical plan layer.  When an optimizer is added in the
/// future, it would be inserted between the Binder and the Planner.
///
/// Supports: SELECT (with subqueries, CTEs), INSERT, UPDATE, DELETE.
/// DDL statements are handled by QueryEngine directly since they don't
/// produce iterator trees.
class Planner {
public:
    /// @param catalog          System catalog for schema lookups.
    /// @param storage          StorageManager for TableHeap access.
    /// @param database_id      Current database context for table resolution.
    /// @param graph_engine     Optional GraphEngine for graph query planning.
    /// @param provider_registry Optional ProviderRegistry for text auto-embedding.
    /// @param hnsw_indexes     Optional map of index_id → loaded HnswIndex.
    /// @param btree_indexes    Optional map of index_id → loaded BTreeIndex.
    /// @param hash_indexes     Optional map of index_id → loaded HashIndex.
    /// @param embedding_pool   Optional EmbeddingWorkerPool for async INSERT embedding.
    /// @param algorithm_registry Optional AlgorithmRegistry for algorithm TVFs.
    /// @param hnsw_rid_maps    Optional HNSW node_id → RID maps for direct lookups.
    Planner(Catalog& catalog,
            StorageManager& storage,
            database_id_t database_id = default_database_id,
            GraphEngine* graph_engine = nullptr,
            ProviderRegistry* provider_registry = nullptr,
            std::unordered_map<index_id_t, HnswIndex*>* hnsw_indexes = nullptr,
            std::unordered_map<index_id_t, BTreeIndex*>* btree_indexes = nullptr,
            std::unordered_map<index_id_t, HashIndex*>* hash_indexes = nullptr,
            EmbeddingWorkerPool* embedding_pool = nullptr,
            AlgorithmRegistry* algorithm_registry = nullptr,
            std::unordered_map<index_id_t, std::vector<RID>>* hnsw_rid_maps = nullptr,
            std::unordered_map<index_id_t, Bm25Index*>* bm25_indexes = nullptr);

    /// Build an iterator tree for a DML/query statement.
    ///
    /// @param bound        The bound statement from the Binder.
    /// @param owned_exprs  Output: expressions created by the planner
    ///                     (e.g. ColumnRefExprs for SELECT *). The caller
    ///                     must keep this vector alive as long as the
    ///                     returned iterator is in use.
    [[nodiscard]] Result<std::unique_ptr<Iterator>> plan(const BoundStatement& bound,
                                                         std::vector<ExprPtr>& owned_exprs);

    /// Provide the current outer row when planning a correlated subquery so that
    /// configuration expressions that reference outer columns — a NEAREST target
    /// vector or a TRAVERSE start key — evaluate against it. Pass nullptrs to
    /// clear. The pointed-to objects must outlive planning.
    void
    set_outer_context(const Tuple* tuple, const OutputSchema* schema, const BoundStatement* bound) {
        outer_tuple_ = tuple;
        outer_schema_ = schema;
        outer_bound_ = bound;
    }

private:
    [[nodiscard]] Result<std::unique_ptr<Iterator>> plan_select(const SelectStmt& stmt,
                                                                const BoundStatement& bound,
                                                                std::vector<ExprPtr>& owned_exprs);

    /// Overload that merges an outer CTE map into the local CTE scope.
    [[nodiscard]] Result<std::unique_ptr<Iterator>>
    plan_select(const SelectStmt& stmt,
                const BoundStatement& bound,
                std::vector<ExprPtr>& owned_exprs,
                const std::unordered_map<std::string, const SelectStmt*>& outer_cte_map);

    [[nodiscard]] Result<std::unique_ptr<Iterator>> plan_insert(const InsertStmt& stmt,
                                                                const BoundStatement& bound);

    [[nodiscard]] Result<std::unique_ptr<Iterator>> plan_update(const UpdateStmt& stmt,
                                                                const BoundStatement& bound);

    [[nodiscard]] Result<std::unique_ptr<Iterator>> plan_delete(const DeleteStmt& stmt,
                                                                const BoundStatement& bound);

    /// Build an OutputSchema from BoundStatement::output_columns.
    [[nodiscard]] static OutputSchema
    build_output_schema(const std::vector<ResolvedColumn>& columns);

    /// Build an OutputSchema from a TableSchema (all columns of the table).
    /// Uses @p table_alias as the table name if non-empty.
    [[nodiscard]] static OutputSchema
    build_table_output_schema(const TableSchema& ts, const std::string& table_alias = "");

    /// Plan a FROM source: physical table, CTE reference, or derived table.
    ///
    /// @param table_ref    The FROM clause table reference.
    /// @param alias        The resolved alias for this source.
    /// @param cte_map      Map of CTE name → CTE query for the current SELECT.
    /// @param bound        Parent BoundStatement (for expression types).
    /// @param owned_exprs  Owned expression storage.
    /// @return An iterator + output schema for the source.
    struct PlannedSource {
        std::unique_ptr<Iterator> iter;
        OutputSchema schema;
    };

    [[nodiscard]] Result<PlannedSource>
    plan_from_source(const TableRef& table_ref,
                     const std::string& alias,
                     const std::unordered_map<std::string, const SelectStmt*>& cte_map,
                     const BoundStatement& bound,
                     std::vector<ExprPtr>& owned_exprs);

    /// Bind all CTEs from a CTE map and inject them into a Binder using a
    /// fixpoint loop so later CTEs can reference earlier ones regardless of
    /// iteration order.
    void inject_cte_bindings(Binder& binder,
                             const std::unordered_map<std::string, const SelectStmt*>& cte_map);

    /// Extract EXISTS/NOT EXISTS/IN-subquery conditions from a WHERE clause
    /// and rewrite them as SEMI/ANTI joins. Returns the remaining WHERE
    /// predicate (may be nullptr if all conditions were rewritten).
    [[nodiscard]] Result<const Expr*>
    rewrite_subquery_predicates(const Expr& where_expr,
                                std::unique_ptr<Iterator>& child,
                                const BoundStatement& bound,
                                const std::unordered_map<std::string, const SelectStmt*>& cte_map,
                                std::vector<ExprPtr>& owned_exprs);

    /// Try to create an IndexScanOperator (B+ tree or hash) for a table if a
    /// suitable index exists for the given WHERE predicate. Returns nullptr if
    /// no suitable index is found (caller should fall back to SeqScan).
    [[nodiscard]] Result<std::unique_ptr<Iterator>>
    try_plan_index_scan(const TableSchema& table_schema,
                        TableStorage* storage,
                        const OutputSchema& table_output,
                        const Expr* where_expr,
                        const BoundStatement& bound);

    /// Try to create a Bm25ScanOperator for a `MATCH(col) TO 'q'` WHERE
    /// predicate. @p score_output must already include the trailing _score
    /// column. Returns an error if MATCH appears in an unsupported position or
    /// no BM25 index exists for the column; returns nullptr if there is no
    /// MATCH predicate at all.
    [[nodiscard]] Result<std::unique_ptr<Iterator>>
    try_plan_bm25_scan(const TableSchema& table_schema,
                       TableStorage* storage,
                       const OutputSchema& score_output,
                       const Expr* where_expr,
                       const BoundStatement& bound);

    /// Collect the BM25 indexes (and their text-column ordinals) on a table so
    /// INSERT/UPDATE/DELETE operators can maintain them. Empty if none or the
    /// index map is unavailable.
    [[nodiscard]] std::vector<Bm25MaintenanceTarget>
    collect_bm25_targets(const TableSchema& schema) const;

    /// Build a full-scope OutputSchema from all node tables in a MATCH pattern.
    /// Includes all columns from every node variable's label table so that
    /// WHERE clauses can reference columns not in the SELECT list.
    [[nodiscard]] OutputSchema build_match_scope_schema(const MatchConfig& config) const;

    // -- Graph query planning -------------------------------------------------

    [[nodiscard]] Result<std::unique_ptr<Iterator>> plan_traverse(const TraverseStmt& stmt,
                                                                  const BoundStatement& bound);

    [[nodiscard]] Result<std::unique_ptr<Iterator>> plan_shortest_path(const ShortestPathStmt& stmt,
                                                                       const BoundStatement& bound);

    [[nodiscard]] Result<std::unique_ptr<Iterator>> plan_match(const MatchStmt& stmt,
                                                               const BoundStatement& bound,
                                                               std::vector<ExprPtr>& owned_exprs);

    // -- Vector query planning ------------------------------------------------

    /// Build a NearestScanOperator from the individual NEAREST parameters
    /// (borrowed from a NearestExpr). Output schema is table columns + _distance.
    [[nodiscard]] Result<std::unique_ptr<Iterator>>
    plan_nearest_impl(const std::string& table_name,
                      const std::string& column_name,
                      const Expr* k_expr,
                      const Expr* target_expr,
                      NearestMetric metric,
                      const Stmt* within_traverse,
                      const Expr* where_expr,
                      const BoundStatement& bound);

    /// Try to build a vector scan for a `NEAREST(col, k) TO ...` WHERE predicate.
    /// Returns nullptr if there is no NEAREST predicate; errors if it appears in
    /// an unsupported position.
    [[nodiscard]] Result<std::unique_ptr<Iterator>> try_plan_vector_scan(
        const TableSchema& table_schema, const Expr* where_expr, const BoundStatement& bound);

    Catalog& catalog_;
    StorageManager& storage_;
    database_id_t database_id_;
    GraphEngine* graph_engine_;
    ProviderRegistry* provider_registry_;
    std::unordered_map<index_id_t, HnswIndex*>* hnsw_indexes_;
    std::unordered_map<index_id_t, BTreeIndex*>* btree_indexes_;
    std::unordered_map<index_id_t, HashIndex*>* hash_indexes_;
    std::unordered_map<index_id_t, std::vector<RID>>* hnsw_rid_maps_;
    std::unordered_map<index_id_t, Bm25Index*>* bm25_indexes_;
    EmbeddingWorkerPool* embedding_pool_;
    AlgorithmRegistry* algorithm_registry_;
    SubqueryContext subquery_ctx_;

    // Outer row for correlated-subquery planning (see set_outer_context).
    const Tuple* outer_tuple_ = nullptr;
    const OutputSchema* outer_schema_ = nullptr;
    const BoundStatement* outer_bound_ = nullptr;
};

} // namespace sixseven
