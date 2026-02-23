#pragma once

#include "giodb/catalog/catalog.h"
#include "giodb/common/result.h"
#include "giodb/executor/expr_evaluator.h"
#include "giodb/executor/iterator.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/executor/tuple.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace giodb {

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
    /// @param catalog  System catalog for schema lookups.
    /// @param storage  StorageManager for TableHeap access.
    Planner(const Catalog& catalog, StorageManager& storage);

    /// Build an iterator tree for a DML/query statement.
    ///
    /// @param bound        The bound statement from the Binder.
    /// @param owned_exprs  Output: expressions created by the planner
    ///                     (e.g. ColumnRefExprs for SELECT *). The caller
    ///                     must keep this vector alive as long as the
    ///                     returned iterator is in use.
    [[nodiscard]] Result<std::unique_ptr<Iterator>> plan(const BoundStatement& bound,
                                                         std::vector<ExprPtr>& owned_exprs);

private:
    [[nodiscard]] Result<std::unique_ptr<Iterator>> plan_select(const SelectStmt& stmt,
                                                                const BoundStatement& bound,
                                                                std::vector<ExprPtr>& owned_exprs);

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

    /// Extract EXISTS/NOT EXISTS/IN-subquery conditions from a WHERE clause
    /// and rewrite them as SEMI/ANTI joins. Returns the remaining WHERE
    /// predicate (may be nullptr if all conditions were rewritten).
    [[nodiscard]] Result<const Expr*>
    rewrite_subquery_predicates(const Expr& where_expr,
                                std::unique_ptr<Iterator>& child,
                                const BoundStatement& bound,
                                const std::unordered_map<std::string, const SelectStmt*>& cte_map,
                                std::vector<ExprPtr>& owned_exprs);

    const Catalog& catalog_;
    StorageManager& storage_;
    SubqueryContext subquery_ctx_;
};

} // namespace giodb
