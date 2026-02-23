#pragma once

#include "giodb/common/result.h"
#include "giodb/common/value.h"
#include "giodb/executor/tuple.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"

namespace giodb {

// Forward declarations.
class Catalog;
class StorageManager;

/// Context for evaluating subquery expressions at runtime.
///
/// When provided, the expression evaluator can plan and execute
/// SubqueryExpr, ExistsExpr, and InExpr-with-subquery nodes inline.
/// Without this context, those nodes return NOT_IMPLEMENTED errors.
struct SubqueryContext {
    const Catalog& catalog;
    StorageManager& storage;
};

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

} // namespace giodb
