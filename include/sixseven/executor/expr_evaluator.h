#pragma once

#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"

namespace sixseven {

// Forward declarations.
class Catalog;
class StorageManager;
class WalReceiver;
class WalWriter;
class GraphEngine;
class ProviderRegistry;

/// Context for evaluating subquery expressions at runtime.
///
/// When provided, the expression evaluator can plan and execute
/// SubqueryExpr, ExistsExpr, and InExpr-with-subquery nodes inline.
/// Without this context, those nodes return NOT_IMPLEMENTED errors.
///
/// `graph_engine` and `provider_registry` are optional and only required when a
/// subquery is a graph (TRAVERSE/MATCH) or vector (NEAREST) statement.
struct SubqueryContext {
    Catalog& catalog;
    StorageManager& storage;
    GraphEngine* graph_engine = nullptr;
    ProviderRegistry* provider_registry = nullptr;
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
