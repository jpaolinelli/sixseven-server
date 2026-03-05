#pragma once

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/parser/ast.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sixseven {

// ---------------------------------------------------------------------------
// Resolved metadata types
// ---------------------------------------------------------------------------

/// A column reference that has been resolved against the catalog.
struct ResolvedColumn {
    table_id_t table_id = 0; ///< 0 for computed expressions.
    int32_t ordinal = -1;    ///< Column ordinal within the table, -1 for computed.
    std::string table_name;  ///< Alias or table name as written in the query.
    std::string column_name;
    TypeId type_id = TypeId::INT32;
    bool nullable = true;
};

/// Type annotation for an expression, stored in a side map keyed by `const Expr*`.
struct ExprType {
    TypeId type_id = TypeId::INT32;
    bool nullable = true;
    bool is_aggregate = false; ///< True if this expression contains an aggregate call.
};

// ---------------------------------------------------------------------------
// Name resolution scope
// ---------------------------------------------------------------------------

/// One table (or subquery/CTE) visible in the current scope.
struct ScopeTable {
    table_id_t table_id = 0;
    std::string alias;
    std::vector<ResolvedColumn> columns;
};

/// Name resolution scope, with optional parent for correlated subqueries.
class Scope {
public:
    explicit Scope(Scope* parent = nullptr) : parent_(parent) {}

    /// Add a table to this scope.
    void add_table(ScopeTable table);

    /// Resolve an unqualified column name. Returns NOT_FOUND or INVALID_ARGUMENT
    /// (ambiguous) on failure.
    Result<ResolvedColumn> resolve_column(const std::string& column) const;

    /// Resolve a qualified column reference (table.column).
    Result<ResolvedColumn> resolve_column(const std::string& table,
                                          const std::string& column) const;

    /// Return all columns in scope (for SELECT *).
    std::vector<ResolvedColumn> all_columns() const;

    /// Return columns for a specific table alias (for table.*).
    Result<std::vector<ResolvedColumn>> columns_for(const std::string& alias) const;

private:
    Scope* parent_ = nullptr;
    std::vector<ScopeTable> tables_;
};

// ---------------------------------------------------------------------------
// BoundStatement — output of binding
// ---------------------------------------------------------------------------

/// The result of binding a parsed statement against the catalog.
struct BoundStatement {
    const Stmt* stmt = nullptr; ///< Non-owning pointer to the original AST.
    std::vector<ResolvedColumn> output_columns;
    std::vector<table_id_t> referenced_tables;
    std::unordered_map<const Expr*, ExprType> expr_types;
    std::vector<edge_id_t> referenced_edge_types;
};

// ---------------------------------------------------------------------------
// Binder
// ---------------------------------------------------------------------------

/// Semantic analyser that resolves names, checks types, and validates
/// statements against the catalog.
///
/// Usage:
/// ```
///   Catalog catalog;
///   // ... populate catalog ...
///   Binder binder(catalog);
///   auto result = binder.bind(*parsed_stmt);
///   if (result) { /* use result->output_columns, etc. */ }
/// ```
class Binder {
public:
    /// @param catalog      System catalog for schema lookups.
    /// @param database_id  The current database context for name resolution.
    explicit Binder(const Catalog& catalog, database_id_t database_id = default_database_id)
        : catalog_(catalog), database_id_(database_id) {}

    /// Bind a parsed statement. Validates names, types, and semantics.
    [[nodiscard]] Result<BoundStatement> bind(const Stmt& stmt);

    /// Inject outer CTE bindings so subquery binders can resolve CTE references.
    /// Injected CTEs survive the top-level bind() call's CTE clearing.
    void add_cte(const std::string& name, const BoundStatement& bound) {
        cte_results_.emplace(name, bound);
        has_outer_ctes_ = true;
    }

private:
    const Catalog& catalog_;
    database_id_t database_id_;

    /// CTE results accumulated during binding. Inner subqueries can reference
    /// CTEs defined by their parent queries.
    std::unordered_map<std::string, BoundStatement> cte_results_;

    /// Set when outer CTE bindings have been injected via add_cte().
    /// Prevents bind() from clearing them at the top-level entry point.
    bool has_outer_ctes_ = false;

    /// Recursion depth for bind() calls. Used to clear CTE state only at the
    /// top-level entry point, not during recursive calls for sub-queries/CTEs.
    int bind_depth_ = 0;

    // -- Statement handlers ---------------------------------------------------

    Result<BoundStatement> bind_select(const SelectStmt& stmt);
    Result<BoundStatement> bind_select(const SelectStmt& stmt, Scope* parent_scope);
    Result<BoundStatement> bind_insert(const InsertStmt& stmt);
    Result<BoundStatement> bind_update(const UpdateStmt& stmt);
    Result<BoundStatement> bind_delete(const DeleteStmt& stmt);
    Result<BoundStatement> bind_create_table(const CreateTableStmt& stmt);
    Result<BoundStatement> bind_drop_table(const DropTableStmt& stmt);
    Result<BoundStatement> bind_alter_table(const AlterTableStmt& stmt);
    Result<BoundStatement> bind_create_index(const CreateIndexStmt& stmt);
    Result<BoundStatement> bind_drop_index(const DropIndexStmt& stmt);
    Result<BoundStatement> bind_create_edge_type(const CreateEdgeTypeStmt& stmt);
    Result<BoundStatement> bind_drop_edge_type(const DropEdgeTypeStmt& stmt);
    Result<BoundStatement> bind_link(const LinkStmt& stmt);
    Result<BoundStatement> bind_unlink(const UnlinkStmt& stmt);
    Result<BoundStatement> bind_traverse(const TraverseStmt& stmt);
    Result<BoundStatement> bind_nearest(const NearestStmt& stmt);
    Result<BoundStatement> bind_match(const MatchStmt& stmt);
    Result<BoundStatement> bind_shortest_path(const ShortestPathStmt& stmt);
    Result<BoundStatement> bind_explain(const ExplainStmt& stmt);
    Result<BoundStatement> bind_describe(const DescribeStmt& stmt);
    Result<BoundStatement> bind_show(const ShowStmt& stmt);
    Result<BoundStatement> bind_passthrough(const Stmt& stmt);

    // -- Expression binding ---------------------------------------------------

    Result<ExprType> bind_expr(const Expr& expr, Scope& scope, BoundStatement& bound);
    Result<ExprType> bind_literal(const LiteralExpr& expr);
    Result<ExprType> bind_column_ref(const ColumnRefExpr& expr, Scope& scope);
    Result<ExprType> bind_binary(const BinaryExpr& expr, Scope& scope, BoundStatement& bound);
    Result<ExprType> bind_unary(const UnaryExpr& expr, Scope& scope, BoundStatement& bound);
    Result<ExprType>
    bind_function_call(const FunctionCallExpr& expr, Scope& scope, BoundStatement& bound);
    Result<ExprType> bind_cast(const CastExpr& expr, Scope& scope, BoundStatement& bound);
    Result<ExprType> bind_case(const CaseExpr& expr, Scope& scope, BoundStatement& bound);
    Result<ExprType> bind_in(const InExpr& expr, Scope& scope, BoundStatement& bound);
    Result<ExprType> bind_between(const BetweenExpr& expr, Scope& scope, BoundStatement& bound);
    Result<ExprType> bind_is_null(const IsNullExpr& expr, Scope& scope, BoundStatement& bound);
    Result<ExprType> bind_like(const LikeExpr& expr, Scope& scope, BoundStatement& bound);
    Result<ExprType> bind_exists(const ExistsExpr& expr, Scope& scope, BoundStatement& bound);
    Result<ExprType> bind_subquery(const SubqueryExpr& expr, Scope& scope, BoundStatement& bound);
    Result<ExprType> bind_array(const ArrayExpr& expr, Scope& scope, BoundStatement& bound);

    // -- Helpers --------------------------------------------------------------

    /// Build a Scope from the FROM clause tables and JOINs.
    Result<Scope> build_from_scope(const SelectStmt& stmt, Scope* parent, BoundStatement& bound);

    /// Expand SELECT items (handling * and table.*) and bind each expression.
    Result<std::vector<ResolvedColumn>>
    expand_select_items(const std::vector<SelectItem>& items, Scope& scope, BoundStatement& bound);

    /// Validate aggregate usage: if GROUP BY present, non-aggregate select expressions
    /// must appear in GROUP BY.
    Result<void>
    validate_aggregates(const SelectStmt& stmt, const Scope& scope, const BoundStatement& bound);

    /// Bind RETURNING clause columns.
    Result<std::vector<ResolvedColumn>>
    bind_returning(const std::vector<SelectItem>& items, Scope& scope, BoundStatement& bound);

    /// Build a ScopeTable from a catalog TableSchema.
    ScopeTable make_scope_table(const TableSchema& schema, const std::string& alias) const;

    /// Build a ScopeTable for a TRAVERSE source in FROM/JOIN.
    Result<ScopeTable> build_traverse_scope(const TableRef& tref, BoundStatement& bound);

    /// Resolve a table in the current database, wrapping NOT_FOUND errors with
    /// the database name for clearer diagnostics.
    Result<TableSchema> resolve_table(const std::string& table_name) const;

    /// Check whether an expression references an aggregate (by consulting expr_types map).
    bool contains_aggregate(const Expr& expr, const BoundStatement& bound) const;

    /// Check whether an expression matches one of the GROUP BY expressions
    /// (simple column name comparison).
    bool in_group_by(const Expr& expr, const std::vector<ExprPtr>& group_by) const;
};

} // namespace sixseven
