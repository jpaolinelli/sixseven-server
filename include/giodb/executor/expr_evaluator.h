#pragma once

#include "giodb/common/result.h"
#include "giodb/common/value.h"
#include "giodb/executor/tuple.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"

namespace giodb {

/// Evaluate an expression against a tuple, returning a Value.
///
/// The `bound` statement provides type information via the `expr_types` side map
/// (populated by the Binder). Column references are resolved against `schema`.
[[nodiscard]] Result<Value> evaluate_expr(const Expr& expr,
                                          const Tuple& tuple,
                                          const OutputSchema& schema,
                                          const BoundStatement& bound);

/// Evaluate an expression and coerce the result to bool.
///
/// Used for WHERE, HAVING, JOIN ON, and CASE WHEN conditions.
/// NULL is treated as false (SQL three-valued logic: WHERE filters out NULLs).
[[nodiscard]] Result<bool> evaluate_predicate(const Expr& expr,
                                              const Tuple& tuple,
                                              const OutputSchema& schema,
                                              const BoundStatement& bound);

} // namespace giodb
