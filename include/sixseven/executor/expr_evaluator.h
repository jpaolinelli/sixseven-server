#pragma once

#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"

#include <unordered_map>
#include <vector>

namespace sixseven {

// Forward declarations.
class Catalog;
class StorageManager;
class WalReceiver;
class WalWriter;
class GraphEngine;
class ProviderRegistry;
class HnswIndex;
class BTreeIndex;
class HashIndex;
class Bm25Index;
struct RID;

/// Context for evaluating subquery expressions at runtime.
///
/// When provided, the expression evaluator can plan and execute
/// SubqueryExpr, ExistsExpr, and InExpr-with-subquery nodes inline.
/// Without this context, those nodes return NOT_IMPLEMENTED errors.
///
/// `graph_engine` and `provider_registry` are optional and only required when a
/// subquery is a graph (TRAVERSE/MATCH) or vector (NEAREST) statement.
///
/// `hnsw_indexes` / `btree_indexes` / `hash_indexes` / `hnsw_rid_maps` /
/// `bm25_indexes` (GDB-1297) mirror the top-level Planner's own per-plan index
/// handles. They let a subquery that is planned or re-planned through this
/// context -- e.g. the runtime fallback used to (re-)plan a correlated
/// subquery that could not be decorrelated into a join at plan time --
/// locate a BM25/HNSW/B-tree/hash index exactly as a top-level query does.
/// nullptr means "no such index map available", matching Planner's defaults.
///
/// `outer_tuple` / `outer_schema` (GDB-1309) mirror the top-level Planner's
/// own outer-row context (see Planner::set_outer_context). A correlated
/// IN/scalar subquery's WHERE clause is planned and evaluated by operators
/// (FilterOperator, SeqScanOperator's residual, etc.) that only know about
/// their own row's schema -- they have no notion of the enclosing query's
/// row. When a qualified column reference does not resolve against the
/// local schema, eval_column_ref falls back to these fields (when set) so a
/// plain WHERE-clause outer-column comparison (e.g. `b.id = r.book_id`)
/// resolves the same way NEAREST/TRAVERSE/MATCH's outer-row config
/// expressions already do. nullptr means "no outer row available", matching
/// Planner's defaults for a non-correlated (or top-level) plan.
struct SubqueryContext {
    Catalog& catalog;
    StorageManager& storage;
    GraphEngine* graph_engine = nullptr;
    ProviderRegistry* provider_registry = nullptr;
    std::unordered_map<index_id_t, HnswIndex*>* hnsw_indexes = nullptr;
    std::unordered_map<index_id_t, BTreeIndex*>* btree_indexes = nullptr;
    std::unordered_map<index_id_t, HashIndex*>* hash_indexes = nullptr;
    std::unordered_map<index_id_t, std::vector<RID>>* hnsw_rid_maps = nullptr;
    std::unordered_map<index_id_t, Bm25Index*>* bm25_indexes = nullptr;
    const Tuple* outer_tuple = nullptr;
    const OutputSchema* outer_schema = nullptr;
};

/// Context for evaluating system functions (pg_current_wal_lsn, etc.).
///
/// Set by QueryEngine before executing a query so that the expression
/// evaluator can resolve replication-related system functions.
struct SystemFunctionContext {
    bool standby_mode = false;
    WalWriter* wal_writer = nullptr;
    WalReceiver* wal_receiver = nullptr;
};

/// Set the thread-local system function context for expression evaluation.
void set_system_function_context(const SystemFunctionContext* ctx);

/// Get the current thread-local system function context.
const SystemFunctionContext* get_system_function_context();

/// Evaluate an expression against a tuple, returning a Value.
///
/// The `bound` statement provides type information via the `expr_types` side map
/// (populated by the Binder). Column references are resolved against `schema`.
///
/// @param subquery_ctx  Optional context for inline subquery execution.
[[nodiscard]] Result<Value> evaluate_expr(const Expr& expr,
                                          const Tuple& tuple,
                                          const OutputSchema& schema,
                                          const BoundStatement& bound,
                                          const SubqueryContext* subquery_ctx = nullptr);

/// Evaluate an expression and coerce the result to bool.
///
/// Used for WHERE, HAVING, JOIN ON, and CASE WHEN conditions.
/// NULL is treated as false (SQL three-valued logic: WHERE filters out NULLs).
///
/// @param subquery_ctx  Optional context for inline subquery execution.
[[nodiscard]] Result<bool> evaluate_predicate(const Expr& expr,
                                              const Tuple& tuple,
                                              const OutputSchema& schema,
                                              const BoundStatement& bound,
                                              const SubqueryContext* subquery_ctx = nullptr);

} // namespace sixseven
