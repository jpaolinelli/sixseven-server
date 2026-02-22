#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace giodb {

// ---------------------------------------------------------------------------
// Forward declarations and smart-pointer aliases
// ---------------------------------------------------------------------------

struct Expr;
struct Stmt;

/// Owning pointer to an expression AST node.
using ExprPtr = std::unique_ptr<Expr>;

/// Owning pointer to a statement AST node.
using StmtPtr = std::unique_ptr<Stmt>;

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

/// Binary operators.
enum class BinaryOp : uint8_t {
    ADD,
    SUBTRACT,
    MULTIPLY,
    DIVIDE,
    MODULO,
    EQUAL,
    NOT_EQUAL,
    LESS,
    GREATER,
    LESS_EQUAL,
    GREATER_EQUAL,
    AND,
    OR,
    CONCAT,
};

/// Unary operators.
enum class UnaryOp : uint8_t {
    NEGATE,
    NOT,
};

/// JOIN types.
enum class JoinType : uint8_t {
    INNER,
    LEFT,
    RIGHT,
    FULL,
    CROSS,
};

/// Sort direction for ORDER BY.
enum class SortDirection : uint8_t {
    ASC,
    DESC,
};

/// Graph traversal direction.
enum class TraverseDirection : uint8_t {
    IN,
    OUT,
    BOTH,
};

/// Vector similarity metric for NEAREST.
enum class NearestMetric : uint8_t {
    COSINE,
    L2,
    DOT,
};

/// ALTER TABLE action types.
enum class AlterAction : uint8_t {
    ADD_COLUMN,
    DROP_COLUMN,
    RENAME_COLUMN,
};

/// SHOW target for SHOW statements.
enum class ShowTarget : uint8_t {
    TABLES,
    COLUMNS,
    EDGE_TYPES,
    INDEXES,
    PARAMETER,
};

/// Literal value kinds.
enum class LiteralKind : uint8_t {
    INTEGER,
    FLOAT,
    STRING,
    BOOLEAN,
    NULL_LITERAL,
};

/// Referential action for FOREIGN KEY constraints.
enum class ReferentialAction : uint8_t {
    CASCADE,
    RESTRICT,
};

// ---------------------------------------------------------------------------
// Supporting types
// ---------------------------------------------------------------------------

/// Parsed SQL type annotation (e.g., INT, VARCHAR(255), DECIMAL(10,2),
/// EMBEDDING(384, source_col, 'openai')).
struct TypeSpec {
    std::string name;
    std::optional<int32_t> param1;  ///< Length, precision, or dimension.
    std::optional<int32_t> param2;  ///< Scale (for DECIMAL).
    std::string source;             ///< Source column (for EMBEDDING).
    std::string provider;           ///< Provider name (for EMBEDDING).
};

/// Column definition in CREATE TABLE (distinct from tuple-level ColumnDef).
struct AstColumnDef {
    std::string name;
    TypeSpec type;
    bool nullable = true;
    bool is_unique = false;
    ExprPtr default_expr;
    ExprPtr check_expr;
    std::string fk_table;
    std::string fk_column;
    ReferentialAction fk_on_delete = ReferentialAction::RESTRICT;
};

/// Table-level constraint in CREATE TABLE.
struct TableConstraint {
    enum class Kind : uint8_t {
        PRIMARY_KEY,
        UNIQUE,
        CHECK,
        FOREIGN_KEY,
    };

    Kind kind;
    std::string name;
    std::vector<std::string> columns;
    ExprPtr check_expr;
    std::string fk_table;
    std::vector<std::string> fk_columns;
    ReferentialAction on_delete = ReferentialAction::RESTRICT;
};

/// Table reference in FROM clause (table name or subquery).
struct TableRef {
    std::string name;
    std::string alias;
    StmtPtr subquery;
};

/// JOIN clause.
struct JoinClause {
    JoinType type = JoinType::INNER;
    TableRef table;
    ExprPtr on_expr;
};

/// ORDER BY item.
struct OrderByItem {
    ExprPtr expr;
    SortDirection direction = SortDirection::ASC;
};

/// SET clause assignment (col = expr).
struct Assignment {
    std::string column;
    ExprPtr value;
};

/// A single item in a SELECT list.
struct SelectItem {
    ExprPtr expr;
    std::string alias;
    bool is_star = false;
    std::string table_star;  ///< Non-empty for table.* (e.g., "users").
};

/// Edge property definition in CREATE EDGE TYPE.
struct EdgeProperty {
    std::string name;
    TypeSpec type;
};

/// MATCH pattern node (vertex in a graph pattern).
struct NodePattern {
    std::string variable;
    std::string label;
};

/// MATCH pattern edge (relationship in a graph pattern).
struct EdgePatternDef {
    std::string variable;
    std::string edge_type;
    TraverseDirection direction = TraverseDirection::OUT;
};

/// Element in a MATCH path pattern: node [--edge--> node ...].
struct PathElement {
    NodePattern node;
    std::optional<EdgePatternDef> outgoing_edge;
};

/// CASE WHEN clause.
struct CaseWhen {
    ExprPtr condition;
    ExprPtr result;
};

// ---------------------------------------------------------------------------
// Expression AST base class
// ---------------------------------------------------------------------------

/// Base class for all expression AST nodes.
struct Expr {
    uint32_t line = 0;
    uint32_t column = 0;

    virtual ~Expr() = default;

protected:
    Expr() = default;
};

// ---------------------------------------------------------------------------
// Concrete expression node types
// ---------------------------------------------------------------------------

/// Literal value: integer, float, string, boolean, or NULL.
struct LiteralExpr : Expr {
    LiteralKind kind;
    std::string value;
};

/// Column reference: [table.]column.
struct ColumnRefExpr : Expr {
    std::string table;
    std::string column;
};

/// Binary operation: lhs op rhs.
struct BinaryExpr : Expr {
    BinaryOp op;
    ExprPtr lhs;
    ExprPtr rhs;
};

/// Unary operation: op operand.
struct UnaryExpr : Expr {
    UnaryOp op;
    ExprPtr operand;
};

/// Function call: name(args...) or name(DISTINCT args...).
struct FunctionCallExpr : Expr {
    std::string name;
    std::vector<ExprPtr> args;
    bool distinct = false;
};

/// Type cast: expr::type or CAST(expr AS type).
struct CastExpr : Expr {
    ExprPtr expr;
    TypeSpec target_type;
};

/// CASE expression:
///   CASE [operand] WHEN cond THEN result ... [ELSE else_expr] END.
struct CaseExpr : Expr {
    ExprPtr operand;
    std::vector<CaseWhen> whens;
    ExprPtr else_expr;
};

/// IN expression: expr [NOT] IN (values...) or expr [NOT] IN (SELECT ...).
struct InExpr : Expr {
    ExprPtr expr;
    std::vector<ExprPtr> values;
    StmtPtr subquery;
    bool negated = false;
};

/// BETWEEN expression: expr [NOT] BETWEEN low AND high.
struct BetweenExpr : Expr {
    ExprPtr expr;
    ExprPtr low;
    ExprPtr high;
    bool negated = false;
};

/// IS [NOT] NULL expression.
struct IsNullExpr : Expr {
    ExprPtr expr;
    bool negated = false;
};

/// LIKE expression: expr [NOT] LIKE pattern.
struct LikeExpr : Expr {
    ExprPtr expr;
    ExprPtr pattern;
    bool negated = false;
};

/// EXISTS (SELECT ...).
struct ExistsExpr : Expr {
    StmtPtr subquery;
};

/// Scalar subquery: (SELECT ...) used as a value.
struct SubqueryExpr : Expr {
    StmtPtr subquery;
};

/// Array literal: [1.0, 2.0, 3.0] for vector constants.
struct ArrayExpr : Expr {
    std::vector<ExprPtr> elements;
};

// ---------------------------------------------------------------------------
// Statement AST base class
// ---------------------------------------------------------------------------

/// Base class for all statement AST nodes.
struct Stmt {
    virtual ~Stmt() = default;

protected:
    Stmt() = default;
};

// ---------------------------------------------------------------------------
// DDL statements
// ---------------------------------------------------------------------------

/// CREATE TABLE name (...) [IF NOT EXISTS].
struct CreateTableStmt : Stmt {
    std::string name;
    std::vector<AstColumnDef> columns;
    std::vector<TableConstraint> constraints;
    bool if_not_exists = false;
};

/// DROP TABLE name [IF EXISTS] [CASCADE|RESTRICT].
struct DropTableStmt : Stmt {
    std::string name;
    bool if_exists = false;
    bool cascade = false;
};

/// ALTER TABLE name ADD/DROP/RENAME COLUMN ...
struct AlterTableStmt : Stmt {
    std::string table_name;
    AlterAction action;
    AstColumnDef column;
    std::string column_name;
    std::string new_column_name;
};

/// CREATE [UNIQUE] INDEX name ON table(cols...) [IF NOT EXISTS] [USING method].
struct CreateIndexStmt : Stmt {
    std::string name;
    std::string table_name;
    std::vector<std::string> columns;
    bool is_unique = false;
    std::string method;
    bool if_not_exists = false;
};

/// DROP INDEX name [IF EXISTS].
struct DropIndexStmt : Stmt {
    std::string name;
    bool if_exists = false;
};

/// CREATE EDGE TYPE name (props...) FROM table TO table.
struct CreateEdgeTypeStmt : Stmt {
    std::string name;
    std::vector<EdgeProperty> properties;
    std::string from_table;
    std::string to_table;
};

/// DROP EDGE TYPE name [IF EXISTS].
struct DropEdgeTypeStmt : Stmt {
    std::string name;
    bool if_exists = false;
};

// ---------------------------------------------------------------------------
// DML statements
// ---------------------------------------------------------------------------

/// INSERT INTO table (cols) VALUES (...), (...) | SELECT ...
///   [RETURNING cols].
struct InsertStmt : Stmt {
    std::string table_name;
    std::vector<std::string> columns;
    std::vector<std::vector<ExprPtr>> values;
    StmtPtr select;
    std::vector<SelectItem> returning;
};

/// UPDATE table SET col=val, ... WHERE expr [RETURNING cols].
struct UpdateStmt : Stmt {
    std::string table_name;
    std::vector<Assignment> assignments;
    ExprPtr where_expr;
    std::vector<SelectItem> returning;
};

/// DELETE FROM table WHERE expr [RETURNING cols].
struct DeleteStmt : Stmt {
    std::string table_name;
    ExprPtr where_expr;
    std::vector<SelectItem> returning;
};

/// LINK table(pk) TO table(pk) VIA edge_type (prop=val, ...).
struct LinkStmt : Stmt {
    std::string source_table;
    ExprPtr source_key;
    std::string target_table;
    ExprPtr target_key;
    std::string edge_type;
    std::vector<Assignment> properties;
};

/// UNLINK table(pk) FROM table(pk) VIA edge_type [WHERE expr].
struct UnlinkStmt : Stmt {
    std::string source_table;
    ExprPtr source_key;
    std::string target_table;
    ExprPtr target_key;
    std::string edge_type;
    ExprPtr where_expr;
};

// ---------------------------------------------------------------------------
// Query statements
// ---------------------------------------------------------------------------

/// SELECT [DISTINCT] items FROM table [JOINs] [WHERE] [GROUP BY]
///   [HAVING] [ORDER BY] [LIMIT n OFFSET m]
///   [UNION/INTERSECT/EXCEPT ...].
struct SelectStmt : Stmt {
    /// Set operations.
    enum class SetOp : uint8_t {
        NONE,
        UNION,
        UNION_ALL,
        INTERSECT,
        EXCEPT,
    };

    /// Common Table Expression (WITH clause).
    struct CTE {
        std::string name;
        StmtPtr query;
    };

    bool distinct = false;
    std::vector<SelectItem> items;
    std::vector<TableRef> from;
    std::vector<JoinClause> joins;
    ExprPtr where_expr;
    std::vector<ExprPtr> group_by;
    ExprPtr having_expr;
    std::vector<OrderByItem> order_by;
    ExprPtr limit;
    ExprPtr offset;
    SetOp set_op = SetOp::NONE;
    StmtPtr set_rhs;
    std::vector<CTE> ctes;
};

/// TRAVERSE edge_type FROM table(pk) [DIRECTION ...] [MAX_DEPTH n]
///   [WHERE expr] [FETCH].
struct TraverseStmt : Stmt {
    std::string edge_type;
    std::string from_table;
    ExprPtr from_key;
    TraverseDirection direction = TraverseDirection::OUT;
    std::optional<int32_t> max_depth;
    ExprPtr where_expr;
    bool fetch = false;
};

/// NEAREST k FROM table.col TO target [WHERE expr] [USING metric].
struct NearestStmt : Stmt {
    ExprPtr k;
    std::string table_name;
    std::string column_name;
    ExprPtr target;
    ExprPtr where_expr;
    NearestMetric metric = NearestMetric::COSINE;
    StmtPtr within_traverse;
};

/// MATCH pattern WHERE expr RETURN items.
struct MatchStmt : Stmt {
    std::vector<PathElement> pattern;
    ExprPtr where_expr;
    std::vector<SelectItem> return_items;
};

/// SHORTEST PATH FROM table(pk) TO table(pk) VIA edge_type
///   [DIRECTION ...] [MAX_DEPTH n].
struct ShortestPathStmt : Stmt {
    std::string from_table;
    ExprPtr from_key;
    std::string to_table;
    ExprPtr to_key;
    std::string edge_type;
    TraverseDirection direction = TraverseDirection::OUT;
    std::optional<int32_t> max_depth;
};

// ---------------------------------------------------------------------------
// TCL statements
// ---------------------------------------------------------------------------

/// BEGIN [TRANSACTION].
struct BeginStmt : Stmt {};

/// COMMIT.
struct CommitStmt : Stmt {};

/// ROLLBACK [TO savepoint].
struct RollbackStmt : Stmt {
    std::string savepoint;
};

/// SAVEPOINT name.
struct SavepointStmt : Stmt {
    std::string name;
};

// ---------------------------------------------------------------------------
// Admin statements
// ---------------------------------------------------------------------------

/// SET parameter = value.
struct SetStmt : Stmt {
    std::string parameter;
    ExprPtr value;
};

/// SHOW (TABLES | COLUMNS FROM table | EDGE TYPES | INDEXES | parameter).
struct ShowStmt : Stmt {
    ShowTarget target;
    std::string name;
};

/// EXPLAIN [ANALYZE] statement.
struct ExplainStmt : Stmt {
    StmtPtr statement;
    bool analyze = false;
};

/// DESCRIBE table.
struct DescribeStmt : Stmt {
    std::string table_name;
};

/// REEMBED TABLE table.
struct ReembedStmt : Stmt {
    std::string table_name;
};

/// VACUUM [table].
struct VacuumStmt : Stmt {
    std::string table_name;
};

/// ANALYZE [table].
struct AnalyzeStmt : Stmt {
    std::string table_name;
};

} // namespace giodb
