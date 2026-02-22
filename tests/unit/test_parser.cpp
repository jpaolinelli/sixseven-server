#include "giodb/parser/lexer.h"
#include "giodb/parser/parser.h"

#include <gtest/gtest.h>

using namespace giodb;

// -- Helpers ------------------------------------------------------------------

static std::vector<StmtPtr> parse_ok(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    EXPECT_TRUE(tokens.has_value()) << tokens.error().message;
    if (!tokens) return {};

    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_TRUE(stmts.has_value()) << stmts.error().message;
    return stmts ? std::move(*stmts) : std::vector<StmtPtr>{};
}

static StmtPtr parse_one(std::string_view sql) {
    auto stmts = parse_ok(sql);
    EXPECT_EQ(stmts.size(), 1u);
    if (stmts.size() != 1) return nullptr;
    return std::move(stmts[0]);
}

static void expect_parse_error(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens) return; // lexer error is also acceptable
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_FALSE(stmts.has_value()) << "expected parse error for: " << sql;
}

// -- CREATE TABLE tests -------------------------------------------------------

TEST(Parser, CreateTableBasic) {
    auto stmt = parse_one("CREATE TABLE users (id INT, name VARCHAR(100))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->name, "users");
    EXPECT_FALSE(ct->if_not_exists);
    ASSERT_EQ(ct->columns.size(), 2u);
    EXPECT_EQ(ct->columns[0].name, "id");
    EXPECT_EQ(ct->columns[0].type.name, "INT");
    EXPECT_EQ(ct->columns[1].name, "name");
    EXPECT_EQ(ct->columns[1].type.name, "VARCHAR");
    EXPECT_EQ(ct->columns[1].type.param1.value(), 100);
}

TEST(Parser, CreateTableIfNotExists) {
    auto stmt = parse_one(
        "CREATE TABLE IF NOT EXISTS users (id INT)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_TRUE(ct->if_not_exists);
    EXPECT_EQ(ct->name, "users");
}

TEST(Parser, CreateTableNotNull) {
    auto stmt = parse_one("CREATE TABLE t (id INT NOT NULL)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_FALSE(ct->columns[0].nullable);
}

TEST(Parser, CreateTableDefault) {
    auto stmt = parse_one("CREATE TABLE t (age INT DEFAULT 0)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    auto* def = dynamic_cast<LiteralExpr*>(ct->columns[0].default_expr.get());
    ASSERT_NE(def, nullptr);
    EXPECT_EQ(def->kind, LiteralKind::INTEGER);
    EXPECT_EQ(def->value, "0");
}

TEST(Parser, CreateTableDefaultString) {
    auto stmt = parse_one("CREATE TABLE t (status VARCHAR(20) DEFAULT 'active')");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    auto* def = dynamic_cast<LiteralExpr*>(ct->columns[0].default_expr.get());
    ASSERT_NE(def, nullptr);
    EXPECT_EQ(def->kind, LiteralKind::STRING);
    EXPECT_EQ(def->value, "active");
}

TEST(Parser, CreateTableUnique) {
    auto stmt = parse_one("CREATE TABLE t (email VARCHAR(255) UNIQUE)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_TRUE(ct->columns[0].is_unique);
}

TEST(Parser, CreateTablePrimaryKeyColumn) {
    auto stmt = parse_one("CREATE TABLE t (id INT PRIMARY KEY)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_FALSE(ct->columns[0].nullable);
    EXPECT_TRUE(ct->columns[0].is_unique);
}

TEST(Parser, CreateTablePrimaryKeyConstraint) {
    auto stmt = parse_one(
        "CREATE TABLE t (id INT, name TEXT, PRIMARY KEY(id))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->constraints.size(), 1u);
    EXPECT_EQ(ct->constraints[0].kind, TableConstraint::Kind::PRIMARY_KEY);
    ASSERT_EQ(ct->constraints[0].columns.size(), 1u);
    EXPECT_EQ(ct->constraints[0].columns[0], "id");
}

TEST(Parser, CreateTableCompositePK) {
    auto stmt = parse_one(
        "CREATE TABLE t (a INT, b INT, PRIMARY KEY(a, b))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->constraints.size(), 1u);
    ASSERT_EQ(ct->constraints[0].columns.size(), 2u);
    EXPECT_EQ(ct->constraints[0].columns[0], "a");
    EXPECT_EQ(ct->constraints[0].columns[1], "b");
}

TEST(Parser, CreateTableCheck) {
    auto stmt = parse_one(
        "CREATE TABLE t (age INT CHECK(age > 0))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    auto* chk = dynamic_cast<BinaryExpr*>(ct->columns[0].check_expr.get());
    ASSERT_NE(chk, nullptr);
    EXPECT_EQ(chk->op, BinaryOp::GREATER);
}

TEST(Parser, CreateTableCheckConstraint) {
    auto stmt = parse_one(
        "CREATE TABLE t (age INT, CHECK(age > 0))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->constraints.size(), 1u);
    EXPECT_EQ(ct->constraints[0].kind, TableConstraint::Kind::CHECK);
    EXPECT_NE(ct->constraints[0].check_expr, nullptr);
}

TEST(Parser, CreateTableForeignKey) {
    auto stmt = parse_one(
        "CREATE TABLE orders ("
        "  id INT PRIMARY KEY,"
        "  user_id INT REFERENCES users(id) ON DELETE CASCADE"
        ")");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->columns[1].fk_table, "users");
    EXPECT_EQ(ct->columns[1].fk_column, "id");
    EXPECT_EQ(ct->columns[1].fk_on_delete, ReferentialAction::CASCADE);
}

TEST(Parser, CreateTableForeignKeyConstraint) {
    auto stmt = parse_one(
        "CREATE TABLE orders ("
        "  id INT, user_id INT,"
        "  FOREIGN KEY(user_id) REFERENCES users(id)"
        ")");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->constraints.size(), 1u);
    EXPECT_EQ(ct->constraints[0].kind, TableConstraint::Kind::FOREIGN_KEY);
    EXPECT_EQ(ct->constraints[0].fk_table, "users");
}

TEST(Parser, CreateTableUniqueConstraint) {
    auto stmt = parse_one(
        "CREATE TABLE t (a INT, b INT, UNIQUE(a, b))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->constraints.size(), 1u);
    EXPECT_EQ(ct->constraints[0].kind, TableConstraint::Kind::UNIQUE);
    ASSERT_EQ(ct->constraints[0].columns.size(), 2u);
}

TEST(Parser, CreateTableNamedConstraint) {
    auto stmt = parse_one(
        "CREATE TABLE t (id INT, CONSTRAINT pk_t PRIMARY KEY(id))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->constraints.size(), 1u);
    EXPECT_EQ(ct->constraints[0].name, "pk_t");
    EXPECT_EQ(ct->constraints[0].kind, TableConstraint::Kind::PRIMARY_KEY);
}

TEST(Parser, CreateTableAllTypes) {
    auto stmt = parse_one(
        "CREATE TABLE types ("
        "  a TINYINT, b SMALLINT, c INT, d BIGINT,"
        "  e FLOAT, f DOUBLE, g DECIMAL(10, 2),"
        "  h BOOLEAN, i CHAR(10), j VARCHAR(255), k TEXT,"
        "  l BLOB, m DATE, n TIME, o TIMESTAMP, p INTERVAL,"
        "  q POINT, r JSON, s UUID"
        ")");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->columns.size(), 19u);
    EXPECT_EQ(ct->columns[0].type.name, "TINYINT");
    EXPECT_EQ(ct->columns[6].type.name, "DECIMAL");
    EXPECT_EQ(ct->columns[6].type.param1.value(), 10);
    EXPECT_EQ(ct->columns[6].type.param2.value(), 2);
    EXPECT_EQ(ct->columns[17].type.name, "JSON");
    EXPECT_EQ(ct->columns[18].type.name, "UUID");
}

TEST(Parser, CreateTableEmbedding) {
    auto stmt = parse_one(
        "CREATE TABLE products ("
        "  id INT, description TEXT,"
        "  vec EMBEDDING(384, description, 'openai')"
        ")");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->columns.size(), 3u);
    EXPECT_EQ(ct->columns[2].type.name, "EMBEDDING");
    EXPECT_EQ(ct->columns[2].type.param1.value(), 384);
    EXPECT_EQ(ct->columns[2].type.source, "description");
    EXPECT_EQ(ct->columns[2].type.provider, "openai");
}

TEST(Parser, CreateTableMultipleConstraints) {
    auto stmt = parse_one(
        "CREATE TABLE t ("
        "  id INT NOT NULL UNIQUE DEFAULT 0,"
        "  name VARCHAR(100) NOT NULL"
        ")");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_FALSE(ct->columns[0].nullable);
    EXPECT_TRUE(ct->columns[0].is_unique);
    EXPECT_NE(ct->columns[0].default_expr, nullptr);
    EXPECT_FALSE(ct->columns[1].nullable);
}

// -- DROP TABLE tests ---------------------------------------------------------

TEST(Parser, DropTable) {
    auto stmt = parse_one("DROP TABLE users");
    auto* dt = dynamic_cast<DropTableStmt*>(stmt.get());
    ASSERT_NE(dt, nullptr);
    EXPECT_EQ(dt->name, "users");
    EXPECT_FALSE(dt->if_exists);
    EXPECT_FALSE(dt->cascade);
}

TEST(Parser, DropTableIfExists) {
    auto stmt = parse_one("DROP TABLE IF EXISTS old_table");
    auto* dt = dynamic_cast<DropTableStmt*>(stmt.get());
    ASSERT_NE(dt, nullptr);
    EXPECT_TRUE(dt->if_exists);
    EXPECT_EQ(dt->name, "old_table");
}

TEST(Parser, DropTableCascade) {
    auto stmt = parse_one("DROP TABLE users CASCADE");
    auto* dt = dynamic_cast<DropTableStmt*>(stmt.get());
    ASSERT_NE(dt, nullptr);
    EXPECT_TRUE(dt->cascade);
}

TEST(Parser, DropTableRestrict) {
    auto stmt = parse_one("DROP TABLE users RESTRICT");
    auto* dt = dynamic_cast<DropTableStmt*>(stmt.get());
    ASSERT_NE(dt, nullptr);
    EXPECT_FALSE(dt->cascade);
}

// -- ALTER TABLE tests --------------------------------------------------------

TEST(Parser, AlterTableAddColumn) {
    auto stmt = parse_one("ALTER TABLE users ADD name VARCHAR(100)");
    auto* at = dynamic_cast<AlterTableStmt*>(stmt.get());
    ASSERT_NE(at, nullptr);
    EXPECT_EQ(at->table_name, "users");
    EXPECT_EQ(at->action, AlterAction::ADD_COLUMN);
    EXPECT_EQ(at->column.name, "name");
    EXPECT_EQ(at->column.type.name, "VARCHAR");
}

TEST(Parser, AlterTableAddColumnKeyword) {
    auto stmt = parse_one("ALTER TABLE users ADD COLUMN email TEXT");
    auto* at = dynamic_cast<AlterTableStmt*>(stmt.get());
    ASSERT_NE(at, nullptr);
    EXPECT_EQ(at->action, AlterAction::ADD_COLUMN);
    EXPECT_EQ(at->column.name, "email");
}

TEST(Parser, AlterTableDropColumn) {
    auto stmt = parse_one("ALTER TABLE users DROP COLUMN old_col");
    auto* at = dynamic_cast<AlterTableStmt*>(stmt.get());
    ASSERT_NE(at, nullptr);
    EXPECT_EQ(at->action, AlterAction::DROP_COLUMN);
    EXPECT_EQ(at->column_name, "old_col");
}

TEST(Parser, AlterTableRenameColumn) {
    auto stmt = parse_one(
        "ALTER TABLE users RENAME COLUMN old_name TO new_name");
    auto* at = dynamic_cast<AlterTableStmt*>(stmt.get());
    ASSERT_NE(at, nullptr);
    EXPECT_EQ(at->action, AlterAction::RENAME_COLUMN);
    EXPECT_EQ(at->column_name, "old_name");
    EXPECT_EQ(at->new_column_name, "new_name");
}

// -- CREATE INDEX tests -------------------------------------------------------

TEST(Parser, CreateIndex) {
    auto stmt = parse_one(
        "CREATE INDEX idx_users_name ON users(name)");
    auto* ci = dynamic_cast<CreateIndexStmt*>(stmt.get());
    ASSERT_NE(ci, nullptr);
    EXPECT_EQ(ci->name, "idx_users_name");
    EXPECT_EQ(ci->table_name, "users");
    EXPECT_FALSE(ci->is_unique);
    ASSERT_EQ(ci->columns.size(), 1u);
    EXPECT_EQ(ci->columns[0], "name");
}

TEST(Parser, CreateUniqueIndex) {
    auto stmt = parse_one(
        "CREATE UNIQUE INDEX idx_email ON users(email)");
    auto* ci = dynamic_cast<CreateIndexStmt*>(stmt.get());
    ASSERT_NE(ci, nullptr);
    EXPECT_TRUE(ci->is_unique);
}

TEST(Parser, CreateIndexMultiColumn) {
    auto stmt = parse_one(
        "CREATE INDEX idx_multi ON users(last_name, first_name)");
    auto* ci = dynamic_cast<CreateIndexStmt*>(stmt.get());
    ASSERT_NE(ci, nullptr);
    ASSERT_EQ(ci->columns.size(), 2u);
    EXPECT_EQ(ci->columns[0], "last_name");
    EXPECT_EQ(ci->columns[1], "first_name");
}

TEST(Parser, CreateIndexUsing) {
    auto stmt = parse_one(
        "CREATE INDEX idx_hash ON users(id) USING hash");
    auto* ci = dynamic_cast<CreateIndexStmt*>(stmt.get());
    ASSERT_NE(ci, nullptr);
    EXPECT_EQ(ci->method, "hash");
}

TEST(Parser, CreateIndexIfNotExists) {
    auto stmt = parse_one(
        "CREATE INDEX IF NOT EXISTS idx ON t(c)");
    auto* ci = dynamic_cast<CreateIndexStmt*>(stmt.get());
    ASSERT_NE(ci, nullptr);
    EXPECT_TRUE(ci->if_not_exists);
}

// -- DROP INDEX tests ---------------------------------------------------------

TEST(Parser, DropIndex) {
    auto stmt = parse_one("DROP INDEX idx_old");
    auto* di = dynamic_cast<DropIndexStmt*>(stmt.get());
    ASSERT_NE(di, nullptr);
    EXPECT_EQ(di->name, "idx_old");
    EXPECT_FALSE(di->if_exists);
}

TEST(Parser, DropIndexIfExists) {
    auto stmt = parse_one("DROP INDEX IF EXISTS idx_maybe");
    auto* di = dynamic_cast<DropIndexStmt*>(stmt.get());
    ASSERT_NE(di, nullptr);
    EXPECT_TRUE(di->if_exists);
}

// -- CREATE EDGE TYPE tests ---------------------------------------------------

TEST(Parser, CreateEdgeType) {
    auto stmt = parse_one(
        "CREATE EDGE TYPE follows FROM users TO users");
    auto* ce = dynamic_cast<CreateEdgeTypeStmt*>(stmt.get());
    ASSERT_NE(ce, nullptr);
    EXPECT_EQ(ce->name, "follows");
    EXPECT_EQ(ce->from_table, "users");
    EXPECT_EQ(ce->to_table, "users");
    EXPECT_TRUE(ce->properties.empty());
}

TEST(Parser, CreateEdgeTypeWithProperties) {
    auto stmt = parse_one(
        "CREATE EDGE TYPE knows (since TIMESTAMP, weight FLOAT) "
        "FROM users TO users");
    auto* ce = dynamic_cast<CreateEdgeTypeStmt*>(stmt.get());
    ASSERT_NE(ce, nullptr);
    ASSERT_EQ(ce->properties.size(), 2u);
    EXPECT_EQ(ce->properties[0].name, "since");
    EXPECT_EQ(ce->properties[0].type.name, "TIMESTAMP");
    EXPECT_EQ(ce->properties[1].name, "weight");
    EXPECT_EQ(ce->properties[1].type.name, "FLOAT");
}

TEST(Parser, CreateEdgeTypeDifferentTables) {
    auto stmt = parse_one(
        "CREATE EDGE TYPE authored FROM users TO articles");
    auto* ce = dynamic_cast<CreateEdgeTypeStmt*>(stmt.get());
    ASSERT_NE(ce, nullptr);
    EXPECT_EQ(ce->from_table, "users");
    EXPECT_EQ(ce->to_table, "articles");
}

// -- DROP EDGE TYPE tests -----------------------------------------------------

TEST(Parser, DropEdgeType) {
    auto stmt = parse_one("DROP EDGE TYPE follows");
    auto* de = dynamic_cast<DropEdgeTypeStmt*>(stmt.get());
    ASSERT_NE(de, nullptr);
    EXPECT_EQ(de->name, "follows");
    EXPECT_FALSE(de->if_exists);
}

TEST(Parser, DropEdgeTypeIfExists) {
    auto stmt = parse_one("DROP EDGE TYPE IF EXISTS old_edge");
    auto* de = dynamic_cast<DropEdgeTypeStmt*>(stmt.get());
    ASSERT_NE(de, nullptr);
    EXPECT_TRUE(de->if_exists);
}

// -- Multi-statement tests ----------------------------------------------------

TEST(Parser, MultipleStatements) {
    auto stmts = parse_ok(
        "CREATE TABLE users (id INT); "
        "CREATE TABLE orders (id INT); "
        "DROP TABLE old_table;");
    ASSERT_EQ(stmts.size(), 3u);
    EXPECT_NE(dynamic_cast<CreateTableStmt*>(stmts[0].get()), nullptr);
    EXPECT_NE(dynamic_cast<CreateTableStmt*>(stmts[1].get()), nullptr);
    EXPECT_NE(dynamic_cast<DropTableStmt*>(stmts[2].get()), nullptr);
}

TEST(Parser, TrailingSemicolon) {
    auto stmts = parse_ok("CREATE TABLE t (id INT);");
    ASSERT_EQ(stmts.size(), 1u);
}

TEST(Parser, NoTrailingSemicolon) {
    auto stmts = parse_ok("CREATE TABLE t (id INT)");
    ASSERT_EQ(stmts.size(), 1u);
}

// -- Expression parsing tests (basic, used by CHECK/DEFAULT) ------------------

TEST(Parser, ExprIntegerLiteral) {
    auto stmt = parse_one("CREATE TABLE t (x INT DEFAULT 42)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(ct->columns[0].default_expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::INTEGER);
    EXPECT_EQ(lit->value, "42");
}

TEST(Parser, ExprNegativeNumber) {
    auto stmt = parse_one("CREATE TABLE t (x INT DEFAULT -1)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    auto* neg = dynamic_cast<UnaryExpr*>(ct->columns[0].default_expr.get());
    ASSERT_NE(neg, nullptr);
    EXPECT_EQ(neg->op, UnaryOp::NEGATE);
}

TEST(Parser, ExprNull) {
    auto stmt = parse_one("CREATE TABLE t (x INT DEFAULT NULL)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(ct->columns[0].default_expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::NULL_LITERAL);
}

TEST(Parser, ExprBoolean) {
    auto stmt = parse_one("CREATE TABLE t (x BOOLEAN DEFAULT TRUE)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(ct->columns[0].default_expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::BOOLEAN);
    EXPECT_EQ(lit->value, "true");
}

TEST(Parser, ExprCheckComparison) {
    auto stmt = parse_one("CREATE TABLE t (x INT, CHECK(x >= 0))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->constraints.size(), 1u);
    auto* bin = dynamic_cast<BinaryExpr*>(ct->constraints[0].check_expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::GREATER_EQUAL);
}

TEST(Parser, ExprCheckAndOr) {
    auto stmt = parse_one(
        "CREATE TABLE t (x INT, y INT, CHECK(x > 0 AND y > 0))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    auto* and_expr = dynamic_cast<BinaryExpr*>(
        ct->constraints[0].check_expr.get());
    ASSERT_NE(and_expr, nullptr);
    EXPECT_EQ(and_expr->op, BinaryOp::AND);
}

TEST(Parser, ExprIsNull) {
    auto stmt = parse_one(
        "CREATE TABLE t (x INT, CHECK(x IS NOT NULL))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    auto* is_null = dynamic_cast<IsNullExpr*>(
        ct->constraints[0].check_expr.get());
    ASSERT_NE(is_null, nullptr);
    EXPECT_TRUE(is_null->negated);
}

TEST(Parser, ExprFunctionCall) {
    auto stmt = parse_one(
        "CREATE TABLE t (ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP())");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    auto* fn = dynamic_cast<FunctionCallExpr*>(
        ct->columns[0].default_expr.get());
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->name, "CURRENT_TIMESTAMP");
    EXPECT_TRUE(fn->args.empty());
}

TEST(Parser, ExprPrecedence) {
    // 1 + 2 * 3 should parse as 1 + (2 * 3)
    auto stmt = parse_one("CREATE TABLE t (x INT CHECK(x = 1 + 2 * 3))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    auto* eq = dynamic_cast<BinaryExpr*>(ct->columns[0].check_expr.get());
    ASSERT_NE(eq, nullptr);
    EXPECT_EQ(eq->op, BinaryOp::EQUAL);
    auto* add = dynamic_cast<BinaryExpr*>(eq->rhs.get());
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->op, BinaryOp::ADD);
    auto* mul = dynamic_cast<BinaryExpr*>(add->rhs.get());
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op, BinaryOp::MULTIPLY);
}

// -- Error handling tests -----------------------------------------------------

TEST(Parser, ErrorMissingTableName) {
    expect_parse_error("CREATE TABLE");
}

TEST(Parser, ErrorMissingParen) {
    expect_parse_error("CREATE TABLE t id INT");
}

TEST(Parser, ErrorMissingColumnType) {
    expect_parse_error("CREATE TABLE t (id)");
}

TEST(Parser, ErrorUnexpectedToken) {
    expect_parse_error("FROBNICATE");
}

TEST(Parser, ErrorRecovery) {
    // Two statements, first malformed. Both should produce errors.
    Lexer lexer("CREATE TABLE; DROP TABLE");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(std::move(*tokens));
    auto result = parser.parse_all();
    EXPECT_FALSE(result.has_value());
    // Error message should mention count.
    EXPECT_NE(result.error().message.find("parse error"), std::string::npos);
}

TEST(Parser, EmptyInput) {
    auto stmts = parse_ok("");
    EXPECT_TRUE(stmts.empty());
}

// -- Keyword as identifier tests ----------------------------------------------

TEST(Parser, KeywordAsColumnName) {
    // "type" is a keyword but should be usable as a column name.
    auto stmt = parse_one("CREATE TABLE t (type INT, status TEXT)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->columns[0].name, "type");
    EXPECT_EQ(ct->columns[1].name, "status");
}

TEST(Parser, KeywordAsTableName) {
    auto stmt = parse_one("CREATE TABLE index (id INT)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->name, "index");
}

// -- Case insensitivity -------------------------------------------------------

TEST(Parser, CaseInsensitiveKeywords) {
    auto stmt = parse_one(
        "create table Users (Id int not null, Name varchar(50))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->name, "Users");
    EXPECT_EQ(ct->columns[0].name, "Id");
}
