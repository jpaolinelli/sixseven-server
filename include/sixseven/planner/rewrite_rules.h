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

/// Simplify boolean expressions:
/// - true AND x -> x
/// - false AND x -> false
/// - true OR x -> true
/// - false OR x -> x
/// - NOT NOT x -> x
/// - NOT true -> false, NOT false -> true
/// Returns a new expression if simplified, or nullptr if no change.
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
