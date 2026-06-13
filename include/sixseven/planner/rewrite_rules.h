#pragma once

#include "sixseven/parser/ast.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace sixseven {

// =============================================================================
// Constant folding
// =============================================================================

/// Deep-clone an expression tree into freshly owned nodes. Returns nullptr for
/// expressions that cannot be trivially copied (those holding a StmtPtr, e.g.
/// EXISTS / scalar subqueries; a NEAREST node's WITHIN TRAVERSE scope is dropped
/// from the clone since a cloned NEAREST is only used as a residual that
/// evaluates to TRUE). Used to synthesize owned predicate trees during planning.
[[nodiscard]] ExprPtr clone_expr_public(const Expr& expr);

/// Fold constant expressions at plan time.
/// Examples: 1 + 2 -> 3, true AND x -> x, false OR x -> x
/// Returns a new expression if folded, or nullptr if no change.
[[nodiscard]] ExprPtr fold_constants(const Expr& expr);

// =============================================================================
// Predicate pushdown
// =============================================================================

/// Extract conjuncts from a WHERE clause (split on AND).
/// Given `a AND b AND c`, returns {a, b, c} as non-owning pointers.
[[nodiscard]] std::vector<const Expr*> extract_conjuncts(const Expr& expr);

/// Classify a predicate by which tables it references.
/// Returns the set of table names referenced in the expression.
[[nodiscard]] std::unordered_set<std::string> referenced_tables(const Expr& expr);

/// Check if a predicate references only columns from a single table.
[[nodiscard]] bool is_single_table_predicate(const Expr& expr, const std::string& table_name);

/// Check if a predicate is a join condition (references exactly two tables).
[[nodiscard]] bool is_join_predicate(const Expr& expr);

/// Extract join predicates from a WHERE clause that match a given pair of tables.
/// Returns the predicates that reference both left_table and right_table.
[[nodiscard]] std::vector<const Expr*>
extract_join_predicates(const std::vector<const Expr*>& conjuncts,
                        const std::string& left_table,
                        const std::string& right_table);

/// Extract single-table predicates that only reference the given table.
[[nodiscard]] std::vector<const Expr*>
extract_table_predicates(const std::vector<const Expr*>& conjuncts, const std::string& table_name);

// =============================================================================
// Projection pushdown
// =============================================================================

/// Collect all column references from an expression.
struct ColumnRef {
    std::string table_name;
    std::string column_name;
};

[[nodiscard]] std::vector<ColumnRef> collect_column_refs(const Expr& expr);

/// Collect all column references from a list of expressions.
[[nodiscard]] std::vector<ColumnRef> collect_column_refs(const std::vector<ExprPtr>& exprs);

/// Determine the minimal set of columns needed from a specific table,
/// given the column references used in the query.
[[nodiscard]] std::unordered_set<std::string> needed_columns(const std::vector<ColumnRef>& refs,
                                                             const std::string& table_name);

// =============================================================================
// Boolean simplification
// =============================================================================

/// Simplify boolean expressions.
/// Implemented rules:
/// - NOT NOT x -> x  (double-negation elimination, applied recursively so
///   NOT NOT NOT x -> NOT x, NOT NOT NOT NOT x -> x, etc.)
/// - NOT true -> false, NOT false -> true  (via fold_constants fallback)
/// - Constant arithmetic folding (e.g. 1+2 -> 3) via fold_constants fallback
/// Returns a new expression if simplified, or nullptr if no change.
/// Note: identity rules such as true AND x -> x are NOT implemented.
[[nodiscard]] ExprPtr simplify_boolean(const Expr& expr);

// =============================================================================
// Subquery decorrelation helpers
// =============================================================================

/// Check if a subquery (EXISTS/IN) is correlated (references outer scope).
[[nodiscard]] bool is_correlated_subquery(const Expr& expr,
                                          const std::unordered_set<std::string>& outer_tables);

/// Determine the join type for a subquery rewrite.
/// EXISTS -> SEMI join, NOT EXISTS -> ANTI join, IN -> SEMI join.
[[nodiscard]] JoinType subquery_to_join_type(const Expr& expr);

} // namespace sixseven
