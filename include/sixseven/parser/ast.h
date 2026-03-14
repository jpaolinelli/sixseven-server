#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sixseven {

// ---------------------------------------------------------------------------
// Forward declarations and smart-pointer aliases
// ---------------------------------------------------------------------------

struct Expr;
struct Stmt;
class AstVisitor;

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
    SEMI,
    ANTI,
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

/// Graph traversal output mode.
enum class TraverseMode : uint8_t {
    NODES, ///< Default: one row per visited node (GDB-265).
    EDGES, ///< One row per edge between discovered nodes (GDB-298).
};

/// Vector similarity metric for NEAREST.
enum class NearestMetric : uint8_t {
    COSINE,
    L2,
    DOT,
};

/// Path selector for shortest-path queries within MATCH.
enum class PathSelector : uint8_t {
    NONE,         ///< No path selector (regular MATCH).
    ANY_SHORTEST, ///< Return one shortest path.
    ALL_SHORTEST, ///< Return all paths tied for shortest hop count.
    SHORTEST_K,   ///< Return K shortest distinct paths.
};

/// ALTER TABLE action types.
enum class AlterAction : uint8_t {
    ADD_COLUMN,
    DROP_COLUMN,
    RENAME_COLUMN,
};

/// SHOW target for SHOW statements.
enum class ShowTarget : uint8_t {
    DATABASES,
    TABLES,
    COLUMNS,
    EDGE_TYPES,
    INDEXES,
    EMBEDDINGS,
    PROVIDERS,
    REPLICATION_SLOTS,
    REPLICATION_STATUS,
    STANDBY_STATUS,
    PARAMETER,
    ALL,
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
    std::optional<int32_t> param1; ///< Length, precision, or dimension.
    std::optional<int32_t> param2; ///< Scale (for DECIMAL).
    std::string source;            ///< Source column (for EMBEDDING).
    std::string provider;          ///< Provider name (for EMBEDDING).
};

/// Column definition in CREATE TABLE (distinct from tuple-level ColumnDef).
struct AstColumnDef {
    std::string name;
    TypeSpec type;
    bool nullable = true;
    bool is_unique = false;
    bool is_primary_key = false;
    bool is_autoincrement = false;
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

/// Table reference in FROM clause (table name, subquery, TRAVERSE source,
/// or algorithm function call).
struct TableRef {
    std::string name;
    std::string alias;
    StmtPtr subquery;
    StmtPtr traverse_source;
    StmtPtr match_source;   ///< MatchStmt for MATCH in FROM clause.
    ExprPtr algorithm_call; ///< FunctionCallExpr for algorithm TVFs in FROM.
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
    std::string table_star; ///< Non-empty for table.* (e.g., "users").
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
    ExprPtr filter_expr; ///< Inline WHERE predicate (e.g., WHERE b.active = TRUE).
};

/// MATCH pattern edge (relationship in a graph pattern).
///
/// Supports variable-length path quantifiers:
///   {min,max}  — hop range (e.g., {1,6})
///   {n}        — exact hops (min_hops = max_hops = n)
///   +          — one or more ({1, nullopt})
///   *          — zero or more ({0, nullopt})
/// When both are nullopt, the edge is a fixed single hop.
struct EdgePatternDef {
    std::string variable;
    std::string edge_type;
    TraverseDirection direction = TraverseDirection::OUT;
    std::optional<int32_t> min_hops; ///< Minimum hops (nullopt = fixed single hop).
    std::optional<int32_t> max_hops; ///< Maximum hops (nullopt = unbounded).
    ExprPtr filter_expr;             ///< Inline WHERE predicate (e.g., WHERE r.since > '2020').

    /// Default constructor.
    EdgePatternDef() = default;

    /// Backward-compatible constructor (fixed single-hop edge).
    EdgePatternDef(std::string var, std::string etype, TraverseDirection dir)
        : variable(std::move(var)), edge_type(std::move(etype)), direction(dir) {}

    /// Full constructor with hop quantifiers.
    EdgePatternDef(std::string var,
                   std::string etype,
                   TraverseDirection dir,
                   std::optional<int32_t> min_h,
                   std::optional<int32_t> max_h)
        : variable(std::move(var)), edge_type(std::move(etype)), direction(dir), min_hops(min_h),
          max_hops(max_h) {}

    /// Return true if this edge has a variable-length quantifier.
    [[nodiscard]] bool is_variable_length() const { return min_hops.has_value(); }
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
    uint32_t col = 0; ///< Source column (named 'col' to avoid clash with ColumnRefExpr::column).

    virtual ~Expr() = default;

    /// Accept an AST visitor (double-dispatch).
    virtual void accept(AstVisitor& visitor) const = 0;

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
    void accept(AstVisitor& visitor) const override;
};

/// Column reference: [table.]column.
struct ColumnRefExpr : Expr {
    std::string table;
    std::string column;
    void accept(AstVisitor& visitor) const override;
};

/// Binary operation: lhs op rhs.
struct BinaryExpr : Expr {
    BinaryOp op;
    ExprPtr lhs;
    ExprPtr rhs;
    void accept(AstVisitor& visitor) const override;
};

/// Unary operation: op operand.
struct UnaryExpr : Expr {
    UnaryOp op;
    ExprPtr operand;
    void accept(AstVisitor& visitor) const override;
};

/// A single named argument: name := value.
struct NamedArg {
    std::string name;
    ExprPtr value;
};

/// Function call: name(args..., param := value, ...) or name(DISTINCT args...).
struct FunctionCallExpr : Expr {
    std::string name;
    std::vector<ExprPtr> args;
    std::vector<NamedArg> named_args;
    bool distinct = false;
    void accept(AstVisitor& visitor) const override;
};

/// Type cast: expr::type or CAST(expr AS type).
struct CastExpr : Expr {
    ExprPtr expr;
    TypeSpec target_type;
    void accept(AstVisitor& visitor) const override;
};

/// CASE expression:
///   CASE [operand] WHEN cond THEN result ... [ELSE else_expr] END.
struct CaseExpr : Expr {
    ExprPtr operand;
    std::vector<CaseWhen> whens;
    ExprPtr else_expr;
    void accept(AstVisitor& visitor) const override;
};

/// IN expression: expr [NOT] IN (values...) or expr [NOT] IN (SELECT ...).
struct InExpr : Expr {
    ExprPtr expr;
    std::vector<ExprPtr> values;
    StmtPtr subquery;
    bool negated = false;
    void accept(AstVisitor& visitor) const override;
};

/// BETWEEN expression: expr [NOT] BETWEEN low AND high.
struct BetweenExpr : Expr {
    ExprPtr expr;
    ExprPtr low;
    ExprPtr high;
    bool negated = false;
    void accept(AstVisitor& visitor) const override;
};

/// IS [NOT] NULL expression.
struct IsNullExpr : Expr {
    ExprPtr expr;
    bool negated = false;
    void accept(AstVisitor& visitor) const override;
};

/// LIKE expression: expr [NOT] LIKE pattern.
struct LikeExpr : Expr {
    ExprPtr expr;
    ExprPtr pattern;
    bool negated = false;
    void accept(AstVisitor& visitor) const override;
};

/// EXISTS (SELECT ...).
struct ExistsExpr : Expr {
    StmtPtr subquery;
    void accept(AstVisitor& visitor) const override;
};

/// Scalar subquery: (SELECT ...) used as a value.
struct SubqueryExpr : Expr {
    StmtPtr subquery;
    void accept(AstVisitor& visitor) const override;
};

/// Array literal: [1.0, 2.0, 3.0] for vector constants.
struct ArrayExpr : Expr {
    std::vector<ExprPtr> elements;
    void accept(AstVisitor& visitor) const override;
};

// ---------------------------------------------------------------------------
// Statement AST base class
// ---------------------------------------------------------------------------

/// Base class for all statement AST nodes.
struct Stmt {
    virtual ~Stmt() = default;

    /// Accept an AST visitor (double-dispatch).
    virtual void accept(AstVisitor& visitor) const = 0;

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
    void accept(AstVisitor& visitor) const override;
};

/// DROP TABLE name [IF EXISTS] [CASCADE|RESTRICT].
struct DropTableStmt : Stmt {
    std::string name;
    bool if_exists = false;
    bool cascade = false;
    void accept(AstVisitor& visitor) const override;
};

/// ALTER TABLE name ADD/DROP/RENAME COLUMN ...
struct AlterTableStmt : Stmt {
    std::string table_name;
    AlterAction action;
    AstColumnDef column;
    std::string column_name;
    std::string new_column_name;
    void accept(AstVisitor& visitor) const override;
};

/// CREATE [UNIQUE] INDEX name ON table(cols...) [IF NOT EXISTS] [USING method].
struct CreateIndexStmt : Stmt {
    std::string name;
    std::string table_name;
    std::vector<std::string> columns;
    bool is_unique = false;
    std::string method;
    bool if_not_exists = false;
    void accept(AstVisitor& visitor) const override;
};

/// DROP INDEX name [IF EXISTS].
struct DropIndexStmt : Stmt {
    std::string name;
    bool if_exists = false;
    void accept(AstVisitor& visitor) const override;
};

/// CREATE EDGE TYPE name (props...) FROM table TO table.
struct CreateEdgeTypeStmt : Stmt {
    std::string name;
    std::vector<EdgeProperty> properties;
    std::string from_table;
    std::string to_table;
    void accept(AstVisitor& visitor) const override;
};

/// DROP EDGE TYPE name [IF EXISTS].
struct DropEdgeTypeStmt : Stmt {
    std::string name;
    bool if_exists = false;
    void accept(AstVisitor& visitor) const override;
};

/// CREATE DATABASE [IF NOT EXISTS] name.
struct CreateDatabaseStmt : Stmt {
    std::string database_name;
    bool if_not_exists = false;
    void accept(AstVisitor& visitor) const override;
};

/// DROP DATABASE [IF EXISTS] name [CASCADE|RESTRICT].
struct DropDatabaseStmt : Stmt {
    std::string database_name;
    bool if_exists = false;
    bool cascade = false;
    void accept(AstVisitor& visitor) const override;
};

/// CREATE USER name WITH PASSWORD 'password'.
struct CreateUserStmt : Stmt {
    std::string username;
    std::string password;
    void accept(AstVisitor& visitor) const override;
};

/// DROP USER [IF EXISTS] name.
struct DropUserStmt : Stmt {
    std::string username;
    bool if_exists = false;
    void accept(AstVisitor& visitor) const override;
};

/// ALTER USER name WITH PASSWORD 'new_password'.
struct AlterUserStmt : Stmt {
    std::string username;
    std::string password;
    void accept(AstVisitor& visitor) const override;
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
    void accept(AstVisitor& visitor) const override;
};

/// UPDATE table SET col=val, ... WHERE expr [RETURNING cols].
struct UpdateStmt : Stmt {
    std::string table_name;
    std::vector<Assignment> assignments;
    ExprPtr where_expr;
    std::vector<SelectItem> returning;
    void accept(AstVisitor& visitor) const override;
};

/// DELETE FROM table WHERE expr [RETURNING cols].
struct DeleteStmt : Stmt {
    std::string table_name;
    ExprPtr where_expr;
    std::vector<SelectItem> returning;
    void accept(AstVisitor& visitor) const override;
};

/// LINK table(pk) TO table(pk) VIA edge_type (prop=val, ...).
struct LinkStmt : Stmt {
    std::string source_table;
    ExprPtr source_key;
    std::string target_table;
    ExprPtr target_key;
    std::string edge_type;
    std::vector<Assignment> properties;
    void accept(AstVisitor& visitor) const override;
};

/// UNLINK table(pk) FROM table(pk) VIA edge_type [WHERE expr].
struct UnlinkStmt : Stmt {
    std::string source_table;
    ExprPtr source_key;
    std::string target_table;
    ExprPtr target_key;
    std::string edge_type;
    ExprPtr where_expr;
    void accept(AstVisitor& visitor) const override;
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
    void accept(AstVisitor& visitor) const override;
};

/// TRAVERSE edge_type FROM table(pk) [DIRECTION ...] [MAX_DEPTH n]
///   [MODE NODES|EDGES] [WHERE expr] [FETCH].
struct TraverseStmt : Stmt {
    std::string edge_type;
    std::string from_table;
    ExprPtr from_key;
    TraverseDirection direction = TraverseDirection::OUT;
    std::optional<int32_t> max_depth;
    TraverseMode mode = TraverseMode::NODES;
    ExprPtr where_expr;
    bool fetch = false;
    void accept(AstVisitor& visitor) const override;
};

/// NEAREST k FROM table.col TO target [WITHIN TRAVERSE ...]
///   [WHERE expr] [USING metric].
struct NearestStmt : Stmt {
    ExprPtr k;
    std::string table_name;
    std::string column_name;
    ExprPtr target;
    ExprPtr where_expr;
    NearestMetric metric = NearestMetric::COSINE;
    StmtPtr within_traverse;
    void accept(AstVisitor& visitor) const override;
};

/// MATCH pattern WHERE expr RETURN items.
///
/// Supports path selectors for shortest-path queries:
///   p = ANY SHORTEST (a)-[:knows]->{1,10}(b)
///   p = ALL SHORTEST (a)-[:knows]->{1,10}(b)
///   p = SHORTEST 3 (a)-[:knows]->{1,10}(b)
struct MatchStmt : Stmt {
    std::vector<PathElement> pattern;
    ExprPtr where_expr;
    std::vector<SelectItem> return_items;
    PathSelector path_selector = PathSelector::NONE;
    std::string path_variable; ///< Path variable name (e.g., "p").
    int32_t shortest_k = 0;    ///< K for SHORTEST K selector.
    ExprPtr weight_expr;       ///< Optional WEIGHT expression for weighted shortest path.
    void accept(AstVisitor& visitor) const override;
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
    void accept(AstVisitor& visitor) const override;
};

// ---------------------------------------------------------------------------
// TCL statements
// ---------------------------------------------------------------------------

/// BEGIN [TRANSACTION].
struct BeginStmt : Stmt {
    void accept(AstVisitor& visitor) const override;
};

/// COMMIT.
struct CommitStmt : Stmt {
    void accept(AstVisitor& visitor) const override;
};

/// ROLLBACK [TO savepoint].
struct RollbackStmt : Stmt {
    std::string savepoint;
    void accept(AstVisitor& visitor) const override;
};

/// SAVEPOINT name.
struct SavepointStmt : Stmt {
    std::string name;
    void accept(AstVisitor& visitor) const override;
};

// ---------------------------------------------------------------------------
// Admin statements
// ---------------------------------------------------------------------------

/// SET parameter = value.
struct SetStmt : Stmt {
    std::string parameter;
    ExprPtr value;
    void accept(AstVisitor& visitor) const override;
};

/// SHOW (TABLES | COLUMNS FROM table | EDGE TYPES | INDEXES | parameter).
struct ShowStmt : Stmt {
    ShowTarget target;
    std::string name;
    void accept(AstVisitor& visitor) const override;
};

/// Output format for EXPLAIN.
enum class ExplainFormat : uint8_t {
    TEXT,
    JSON,
};

/// EXPLAIN [ANALYZE] [FORMAT TEXT|JSON] statement.
struct ExplainStmt : Stmt {
    StmtPtr statement;
    bool analyze = false;
    ExplainFormat format = ExplainFormat::TEXT;
    void accept(AstVisitor& visitor) const override;
};

/// DESCRIBE table.
struct DescribeStmt : Stmt {
    std::string table_name;
    void accept(AstVisitor& visitor) const override;
};

/// REEMBED TABLE table.
struct ReembedStmt : Stmt {
    std::string table_name;
    void accept(AstVisitor& visitor) const override;
};

/// VACUUM [table].
struct VacuumStmt : Stmt {
    std::string table_name;
    void accept(AstVisitor& visitor) const override;
};

/// ANALYZE [table].
struct AnalyzeStmt : Stmt {
    std::string table_name;
    void accept(AstVisitor& visitor) const override;
};

// ---------------------------------------------------------------------------
// AST visitor interface
// ---------------------------------------------------------------------------

/// Visitor interface for double-dispatch over AST nodes.
///
/// Implement this interface to traverse the AST without dynamic_cast.
/// Each concrete node type calls the corresponding visit() method.
///
/// Usage:
/// ```
///   class MyVisitor : public AstVisitor {
///       void visit(const CreateTableStmt& s) override { ... }
///       // ... override all visit methods ...
///   };
///   MyVisitor v;
///   stmt->accept(v);
/// ```
class AstVisitor {
public:
    virtual ~AstVisitor() = default;

    // -- Expressions ----------------------------------------------------------

    virtual void visit(const LiteralExpr& node) = 0;
    virtual void visit(const ColumnRefExpr& node) = 0;
    virtual void visit(const BinaryExpr& node) = 0;
    virtual void visit(const UnaryExpr& node) = 0;
    virtual void visit(const FunctionCallExpr& node) = 0;
    virtual void visit(const CastExpr& node) = 0;
    virtual void visit(const CaseExpr& node) = 0;
    virtual void visit(const InExpr& node) = 0;
    virtual void visit(const BetweenExpr& node) = 0;
    virtual void visit(const IsNullExpr& node) = 0;
    virtual void visit(const LikeExpr& node) = 0;
    virtual void visit(const ExistsExpr& node) = 0;
    virtual void visit(const SubqueryExpr& node) = 0;
    virtual void visit(const ArrayExpr& node) = 0;

    // -- DDL statements -------------------------------------------------------

    virtual void visit(const CreateTableStmt& node) = 0;
    virtual void visit(const DropTableStmt& node) = 0;
    virtual void visit(const AlterTableStmt& node) = 0;
    virtual void visit(const CreateIndexStmt& node) = 0;
    virtual void visit(const DropIndexStmt& node) = 0;
    virtual void visit(const CreateEdgeTypeStmt& node) = 0;
    virtual void visit(const DropEdgeTypeStmt& node) = 0;
    virtual void visit(const CreateDatabaseStmt& node) = 0;
    virtual void visit(const DropDatabaseStmt& node) = 0;
    virtual void visit(const CreateUserStmt& node) = 0;
    virtual void visit(const DropUserStmt& node) = 0;
    virtual void visit(const AlterUserStmt& node) = 0;

    // -- DML statements -------------------------------------------------------

    virtual void visit(const InsertStmt& node) = 0;
    virtual void visit(const UpdateStmt& node) = 0;
    virtual void visit(const DeleteStmt& node) = 0;
    virtual void visit(const LinkStmt& node) = 0;
    virtual void visit(const UnlinkStmt& node) = 0;

    // -- Query statements -----------------------------------------------------

    virtual void visit(const SelectStmt& node) = 0;
    virtual void visit(const TraverseStmt& node) = 0;
    virtual void visit(const NearestStmt& node) = 0;
    virtual void visit(const MatchStmt& node) = 0;
    virtual void visit(const ShortestPathStmt& node) = 0;

    // -- TCL statements -------------------------------------------------------

    virtual void visit(const BeginStmt& node) = 0;
    virtual void visit(const CommitStmt& node) = 0;
    virtual void visit(const RollbackStmt& node) = 0;
    virtual void visit(const SavepointStmt& node) = 0;

    // -- Admin statements -----------------------------------------------------

    virtual void visit(const SetStmt& node) = 0;
    virtual void visit(const ShowStmt& node) = 0;
    virtual void visit(const ExplainStmt& node) = 0;
    virtual void visit(const DescribeStmt& node) = 0;
    virtual void visit(const ReembedStmt& node) = 0;
    virtual void visit(const VacuumStmt& node) = 0;
    virtual void visit(const AnalyzeStmt& node) = 0;
};

// ---------------------------------------------------------------------------
// accept() implementations (defined after AstVisitor is complete)
// ---------------------------------------------------------------------------

// -- Expressions --------------------------------------------------------------

inline void LiteralExpr::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void ColumnRefExpr::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void BinaryExpr::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void UnaryExpr::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void FunctionCallExpr::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void CastExpr::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void CaseExpr::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void InExpr::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void BetweenExpr::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void IsNullExpr::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void LikeExpr::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void ExistsExpr::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void SubqueryExpr::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void ArrayExpr::accept(AstVisitor& v) const {
    v.visit(*this);
}

// -- Statements ---------------------------------------------------------------

inline void CreateTableStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void DropTableStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void AlterTableStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void CreateIndexStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void DropIndexStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void CreateEdgeTypeStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void DropEdgeTypeStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void CreateDatabaseStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void DropDatabaseStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void CreateUserStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void DropUserStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void AlterUserStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void InsertStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void UpdateStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void DeleteStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void LinkStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void UnlinkStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void SelectStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void TraverseStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void NearestStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void MatchStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void ShortestPathStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void BeginStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void CommitStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void RollbackStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void SavepointStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void SetStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void ShowStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void ExplainStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void DescribeStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void ReembedStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void VacuumStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}
inline void AnalyzeStmt::accept(AstVisitor& v) const {
    v.visit(*this);
}

} // namespace sixseven
