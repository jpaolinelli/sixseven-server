#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"

#include <gtest/gtest.h>

using namespace sixseven;

// -- Helpers ------------------------------------------------------------------

static std::vector<StmtPtr> parse_ok(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    EXPECT_TRUE(tokens.has_value()) << tokens.error().message;
    if (!tokens)
        return {};

    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_TRUE(stmts.has_value()) << stmts.error().message;
    return stmts ? std::move(*stmts) : std::vector<StmtPtr>{};
}

static StmtPtr parse_one(std::string_view sql) {
    auto stmts = parse_ok(sql);
    EXPECT_EQ(stmts.size(), 1u);
    if (stmts.size() != 1)
        return nullptr;
    return std::move(stmts[0]);
}

static void expect_parse_error(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return; // lexer error is also acceptable
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
    auto stmt = parse_one("CREATE TABLE IF NOT EXISTS users (id INT)");
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
    auto stmt = parse_one("CREATE TABLE t (id INT, name TEXT, PRIMARY KEY(id))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->constraints.size(), 1u);
    EXPECT_EQ(ct->constraints[0].kind, TableConstraint::Kind::PRIMARY_KEY);
    ASSERT_EQ(ct->constraints[0].columns.size(), 1u);
    EXPECT_EQ(ct->constraints[0].columns[0], "id");
}

TEST(Parser, CreateTableCompositePK) {
    auto stmt = parse_one("CREATE TABLE t (a INT, b INT, PRIMARY KEY(a, b))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->constraints.size(), 1u);
    ASSERT_EQ(ct->constraints[0].columns.size(), 2u);
    EXPECT_EQ(ct->constraints[0].columns[0], "a");
    EXPECT_EQ(ct->constraints[0].columns[1], "b");
}

TEST(Parser, CreateTableCheck) {
    auto stmt = parse_one("CREATE TABLE t (age INT CHECK(age > 0))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    auto* chk = dynamic_cast<BinaryExpr*>(ct->columns[0].check_expr.get());
    ASSERT_NE(chk, nullptr);
    EXPECT_EQ(chk->op, BinaryOp::GREATER);
}

TEST(Parser, CreateTableCheckConstraint) {
    auto stmt = parse_one("CREATE TABLE t (age INT, CHECK(age > 0))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->constraints.size(), 1u);
    EXPECT_EQ(ct->constraints[0].kind, TableConstraint::Kind::CHECK);
    EXPECT_NE(ct->constraints[0].check_expr, nullptr);
}

TEST(Parser, CreateTableForeignKey) {
    auto stmt = parse_one("CREATE TABLE orders ("
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
    auto stmt = parse_one("CREATE TABLE orders ("
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
    auto stmt = parse_one("CREATE TABLE t (a INT, b INT, UNIQUE(a, b))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->constraints.size(), 1u);
    EXPECT_EQ(ct->constraints[0].kind, TableConstraint::Kind::UNIQUE);
    ASSERT_EQ(ct->constraints[0].columns.size(), 2u);
}

TEST(Parser, CreateTableNamedConstraint) {
    auto stmt = parse_one("CREATE TABLE t (id INT, CONSTRAINT pk_t PRIMARY KEY(id))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->constraints.size(), 1u);
    EXPECT_EQ(ct->constraints[0].name, "pk_t");
    EXPECT_EQ(ct->constraints[0].kind, TableConstraint::Kind::PRIMARY_KEY);
}

TEST(Parser, CreateTableAllTypes) {
    auto stmt = parse_one("CREATE TABLE types ("
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
    auto stmt = parse_one("CREATE TABLE products ("
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
    auto stmt = parse_one("CREATE TABLE t ("
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
    auto stmt = parse_one("ALTER TABLE users RENAME COLUMN old_name TO new_name");
    auto* at = dynamic_cast<AlterTableStmt*>(stmt.get());
    ASSERT_NE(at, nullptr);
    EXPECT_EQ(at->action, AlterAction::RENAME_COLUMN);
    EXPECT_EQ(at->column_name, "old_name");
    EXPECT_EQ(at->new_column_name, "new_name");
}

// -- CREATE INDEX tests -------------------------------------------------------

TEST(Parser, CreateIndex) {
    auto stmt = parse_one("CREATE INDEX idx_users_name ON users(name)");
    auto* ci = dynamic_cast<CreateIndexStmt*>(stmt.get());
    ASSERT_NE(ci, nullptr);
    EXPECT_EQ(ci->name, "idx_users_name");
    EXPECT_EQ(ci->table_name, "users");
    EXPECT_FALSE(ci->is_unique);
    ASSERT_EQ(ci->columns.size(), 1u);
    EXPECT_EQ(ci->columns[0], "name");
}

TEST(Parser, CreateUniqueIndex) {
    auto stmt = parse_one("CREATE UNIQUE INDEX idx_email ON users(email)");
    auto* ci = dynamic_cast<CreateIndexStmt*>(stmt.get());
    ASSERT_NE(ci, nullptr);
    EXPECT_TRUE(ci->is_unique);
}

TEST(Parser, CreateIndexMultiColumn) {
    auto stmt = parse_one("CREATE INDEX idx_multi ON users(last_name, first_name)");
    auto* ci = dynamic_cast<CreateIndexStmt*>(stmt.get());
    ASSERT_NE(ci, nullptr);
    ASSERT_EQ(ci->columns.size(), 2u);
    EXPECT_EQ(ci->columns[0], "last_name");
    EXPECT_EQ(ci->columns[1], "first_name");
}

TEST(Parser, CreateIndexUsing) {
    auto stmt = parse_one("CREATE INDEX idx_hash ON users(id) USING hash");
    auto* ci = dynamic_cast<CreateIndexStmt*>(stmt.get());
    ASSERT_NE(ci, nullptr);
    EXPECT_EQ(ci->method, "hash");
}

TEST(Parser, CreateIndexIfNotExists) {
    auto stmt = parse_one("CREATE INDEX IF NOT EXISTS idx ON t(c)");
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
    auto stmt = parse_one("CREATE EDGE TYPE follows FROM users TO users");
    auto* ce = dynamic_cast<CreateEdgeTypeStmt*>(stmt.get());
    ASSERT_NE(ce, nullptr);
    EXPECT_EQ(ce->name, "follows");
    EXPECT_EQ(ce->from_table, "users");
    EXPECT_EQ(ce->to_table, "users");
    EXPECT_TRUE(ce->properties.empty());
}

TEST(Parser, CreateEdgeTypeWithProperties) {
    auto stmt = parse_one("CREATE EDGE TYPE knows (since TIMESTAMP, weight FLOAT) "
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
    auto stmt = parse_one("CREATE EDGE TYPE authored FROM users TO articles");
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

// -- CREATE DATABASE tests ----------------------------------------------------

TEST(Parser, CreateDatabaseBasic) {
    auto stmt = parse_one("CREATE DATABASE mydb");
    auto* cd = dynamic_cast<CreateDatabaseStmt*>(stmt.get());
    ASSERT_NE(cd, nullptr);
    EXPECT_EQ(cd->database_name, "mydb");
    EXPECT_FALSE(cd->if_not_exists);
}

TEST(Parser, CreateDatabaseIfNotExists) {
    auto stmt = parse_one("CREATE DATABASE IF NOT EXISTS mydb");
    auto* cd = dynamic_cast<CreateDatabaseStmt*>(stmt.get());
    ASSERT_NE(cd, nullptr);
    EXPECT_EQ(cd->database_name, "mydb");
    EXPECT_TRUE(cd->if_not_exists);
}

TEST(Parser, CreateDatabaseCaseInsensitive) {
    auto stmt = parse_one("create database MyDB");
    auto* cd = dynamic_cast<CreateDatabaseStmt*>(stmt.get());
    ASSERT_NE(cd, nullptr);
    EXPECT_EQ(cd->database_name, "MyDB");
}

TEST(Parser, CreateDatabaseMissingName) {
    expect_parse_error("CREATE DATABASE");
}

TEST(Parser, CreateDatabaseIfNotExistsMissingName) {
    expect_parse_error("CREATE DATABASE IF NOT EXISTS");
}

// -- DROP DATABASE tests ------------------------------------------------------

TEST(Parser, DropDatabaseBasic) {
    auto stmt = parse_one("DROP DATABASE mydb");
    auto* dd = dynamic_cast<DropDatabaseStmt*>(stmt.get());
    ASSERT_NE(dd, nullptr);
    EXPECT_EQ(dd->database_name, "mydb");
    EXPECT_FALSE(dd->if_exists);
    EXPECT_FALSE(dd->cascade);
}

TEST(Parser, DropDatabaseIfExists) {
    auto stmt = parse_one("DROP DATABASE IF EXISTS mydb");
    auto* dd = dynamic_cast<DropDatabaseStmt*>(stmt.get());
    ASSERT_NE(dd, nullptr);
    EXPECT_EQ(dd->database_name, "mydb");
    EXPECT_TRUE(dd->if_exists);
    EXPECT_FALSE(dd->cascade);
}

TEST(Parser, DropDatabaseCascade) {
    auto stmt = parse_one("DROP DATABASE mydb CASCADE");
    auto* dd = dynamic_cast<DropDatabaseStmt*>(stmt.get());
    ASSERT_NE(dd, nullptr);
    EXPECT_EQ(dd->database_name, "mydb");
    EXPECT_FALSE(dd->if_exists);
    EXPECT_TRUE(dd->cascade);
}

TEST(Parser, DropDatabaseIfExistsCascade) {
    auto stmt = parse_one("DROP DATABASE IF EXISTS mydb CASCADE");
    auto* dd = dynamic_cast<DropDatabaseStmt*>(stmt.get());
    ASSERT_NE(dd, nullptr);
    EXPECT_EQ(dd->database_name, "mydb");
    EXPECT_TRUE(dd->if_exists);
    EXPECT_TRUE(dd->cascade);
}

TEST(Parser, DropDatabaseRestrict) {
    auto stmt = parse_one("DROP DATABASE mydb RESTRICT");
    auto* dd = dynamic_cast<DropDatabaseStmt*>(stmt.get());
    ASSERT_NE(dd, nullptr);
    EXPECT_EQ(dd->database_name, "mydb");
    EXPECT_FALSE(dd->cascade);
}

TEST(Parser, DropDatabaseIfExistsAfterName) {
    auto stmt = parse_one("DROP DATABASE mydb IF EXISTS");
    auto* dd = dynamic_cast<DropDatabaseStmt*>(stmt.get());
    ASSERT_NE(dd, nullptr);
    EXPECT_EQ(dd->database_name, "mydb");
    EXPECT_TRUE(dd->if_exists);
    EXPECT_FALSE(dd->cascade);
}

TEST(Parser, DropDatabaseIfExistsAfterNameCascade) {
    auto stmt = parse_one("DROP DATABASE mydb IF EXISTS CASCADE");
    auto* dd = dynamic_cast<DropDatabaseStmt*>(stmt.get());
    ASSERT_NE(dd, nullptr);
    EXPECT_EQ(dd->database_name, "mydb");
    EXPECT_TRUE(dd->if_exists);
    EXPECT_TRUE(dd->cascade);
}

TEST(Parser, DropDatabaseMissingName) {
    expect_parse_error("DROP DATABASE");
}

// -- Multi-statement tests ----------------------------------------------------

TEST(Parser, MultipleStatements) {
    auto stmts = parse_ok("CREATE TABLE users (id INT); "
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

TEST(Parser, BoolTypeAlias) {
    auto stmt = parse_one("CREATE TABLE t (flag BOOL)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->columns.size(), 1u);
    EXPECT_EQ(ct->columns[0].type.name, "BOOL");
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
    auto stmt = parse_one("CREATE TABLE t (x INT, y INT, CHECK(x > 0 AND y > 0))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    auto* and_expr = dynamic_cast<BinaryExpr*>(ct->constraints[0].check_expr.get());
    ASSERT_NE(and_expr, nullptr);
    EXPECT_EQ(and_expr->op, BinaryOp::AND);
}

TEST(Parser, ExprIsNull) {
    auto stmt = parse_one("CREATE TABLE t (x INT, CHECK(x IS NOT NULL))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    auto* is_null = dynamic_cast<IsNullExpr*>(ct->constraints[0].check_expr.get());
    ASSERT_NE(is_null, nullptr);
    EXPECT_TRUE(is_null->negated);
}

TEST(Parser, ExprFunctionCall) {
    auto stmt = parse_one("CREATE TABLE t (ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP())");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    auto* fn = dynamic_cast<FunctionCallExpr*>(ct->columns[0].default_expr.get());
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
    ASSERT_FALSE(result.has_value());
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
    auto stmt = parse_one("create table Users (Id int not null, Name varchar(50))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->name, "Users");
    EXPECT_EQ(ct->columns[0].name, "Id");
}

// =============================================================================
// DML tests (GDB-104)
// =============================================================================

// -- INSERT tests -------------------------------------------------------------

TEST(Parser, InsertBasic) {
    auto stmt = parse_one("INSERT INTO users (name, age) VALUES ('Alice', 30)");
    auto* ins = dynamic_cast<InsertStmt*>(stmt.get());
    ASSERT_NE(ins, nullptr);
    EXPECT_EQ(ins->table_name, "users");
    ASSERT_EQ(ins->columns.size(), 2u);
    EXPECT_EQ(ins->columns[0], "name");
    EXPECT_EQ(ins->columns[1], "age");
    ASSERT_EQ(ins->values.size(), 1u);
    ASSERT_EQ(ins->values[0].size(), 2u);
    auto* name_lit = dynamic_cast<LiteralExpr*>(ins->values[0][0].get());
    ASSERT_NE(name_lit, nullptr);
    EXPECT_EQ(name_lit->kind, LiteralKind::STRING);
    EXPECT_EQ(name_lit->value, "Alice");
}

TEST(Parser, InsertNoColumns) {
    auto stmt = parse_one("INSERT INTO users VALUES (1, 'Bob', 25)");
    auto* ins = dynamic_cast<InsertStmt*>(stmt.get());
    ASSERT_NE(ins, nullptr);
    EXPECT_TRUE(ins->columns.empty());
    ASSERT_EQ(ins->values.size(), 1u);
    ASSERT_EQ(ins->values[0].size(), 3u);
}

TEST(Parser, InsertMultipleRows) {
    auto stmt = parse_one("INSERT INTO users (name) VALUES ('Alice'), ('Bob'), ('Charlie')");
    auto* ins = dynamic_cast<InsertStmt*>(stmt.get());
    ASSERT_NE(ins, nullptr);
    ASSERT_EQ(ins->values.size(), 3u);
    auto* v1 = dynamic_cast<LiteralExpr*>(ins->values[0][0].get());
    ASSERT_NE(v1, nullptr);
    EXPECT_EQ(v1->value, "Alice");
    auto* v3 = dynamic_cast<LiteralExpr*>(ins->values[2][0].get());
    ASSERT_NE(v3, nullptr);
    EXPECT_EQ(v3->value, "Charlie");
}

TEST(Parser, InsertSelect) {
    auto stmt = parse_one("INSERT INTO archive (name) SELECT name FROM users WHERE active = FALSE");
    auto* ins = dynamic_cast<InsertStmt*>(stmt.get());
    ASSERT_NE(ins, nullptr);
    EXPECT_EQ(ins->table_name, "archive");
    ASSERT_EQ(ins->columns.size(), 1u);
    EXPECT_TRUE(ins->values.empty());
    auto* sel = dynamic_cast<SelectStmt*>(ins->select.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].name, "users");
}

TEST(Parser, InsertReturning) {
    auto stmt = parse_one("INSERT INTO users (name) VALUES ('test') RETURNING id, name");
    auto* ins = dynamic_cast<InsertStmt*>(stmt.get());
    ASSERT_NE(ins, nullptr);
    ASSERT_EQ(ins->returning.size(), 2u);
    auto* r0 = dynamic_cast<ColumnRefExpr*>(ins->returning[0].expr.get());
    ASSERT_NE(r0, nullptr);
    EXPECT_EQ(r0->column, "id");
}

// -- UPDATE tests -------------------------------------------------------------

TEST(Parser, UpdateBasic) {
    auto stmt = parse_one("UPDATE users SET name = 'Bob' WHERE id = 1");
    auto* upd = dynamic_cast<UpdateStmt*>(stmt.get());
    ASSERT_NE(upd, nullptr);
    EXPECT_EQ(upd->table_name, "users");
    ASSERT_EQ(upd->assignments.size(), 1u);
    EXPECT_EQ(upd->assignments[0].column, "name");
    auto* val = dynamic_cast<LiteralExpr*>(upd->assignments[0].value.get());
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(val->value, "Bob");
    EXPECT_NE(upd->where_expr, nullptr);
}

TEST(Parser, UpdateMultipleSet) {
    auto stmt = parse_one("UPDATE products SET price = 9.99, stock = stock + 1");
    auto* upd = dynamic_cast<UpdateStmt*>(stmt.get());
    ASSERT_NE(upd, nullptr);
    ASSERT_EQ(upd->assignments.size(), 2u);
    EXPECT_EQ(upd->assignments[0].column, "price");
    EXPECT_EQ(upd->assignments[1].column, "stock");
}

TEST(Parser, UpdateReturning) {
    auto stmt = parse_one("UPDATE users SET active = FALSE WHERE id = 1 RETURNING *");
    auto* upd = dynamic_cast<UpdateStmt*>(stmt.get());
    ASSERT_NE(upd, nullptr);
    ASSERT_EQ(upd->returning.size(), 1u);
    EXPECT_TRUE(upd->returning[0].is_star);
}

// -- DELETE tests -------------------------------------------------------------

TEST(Parser, DeleteBasic) {
    auto stmt = parse_one("DELETE FROM users WHERE id = 42");
    auto* del = dynamic_cast<DeleteStmt*>(stmt.get());
    ASSERT_NE(del, nullptr);
    EXPECT_EQ(del->table_name, "users");
    EXPECT_NE(del->where_expr, nullptr);
}

TEST(Parser, DeleteNoWhere) {
    auto stmt = parse_one("DELETE FROM temp_table");
    auto* del = dynamic_cast<DeleteStmt*>(stmt.get());
    ASSERT_NE(del, nullptr);
    EXPECT_EQ(del->table_name, "temp_table");
    EXPECT_EQ(del->where_expr, nullptr);
}

TEST(Parser, DeleteReturning) {
    auto stmt = parse_one("DELETE FROM users WHERE id = 1 RETURNING id");
    auto* del = dynamic_cast<DeleteStmt*>(stmt.get());
    ASSERT_NE(del, nullptr);
    ASSERT_EQ(del->returning.size(), 1u);
}

// -- LINK tests ---------------------------------------------------------------

TEST(Parser, LinkBasic) {
    auto stmt = parse_one("LINK users(1) TO posts(42) VIA authored");
    auto* lnk = dynamic_cast<LinkStmt*>(stmt.get());
    ASSERT_NE(lnk, nullptr);
    EXPECT_EQ(lnk->source_table, "users");
    EXPECT_EQ(lnk->target_table, "posts");
    EXPECT_EQ(lnk->edge_type, "authored");
    auto* src = dynamic_cast<LiteralExpr*>(lnk->source_key.get());
    ASSERT_NE(src, nullptr);
    EXPECT_EQ(src->value, "1");
    auto* tgt = dynamic_cast<LiteralExpr*>(lnk->target_key.get());
    ASSERT_NE(tgt, nullptr);
    EXPECT_EQ(tgt->value, "42");
}

TEST(Parser, LinkWithProperties) {
    auto stmt =
        parse_one("LINK users(1) TO users(2) VIA follows (since = '2024-01-01', weight = 0.5)");
    auto* lnk = dynamic_cast<LinkStmt*>(stmt.get());
    ASSERT_NE(lnk, nullptr);
    ASSERT_EQ(lnk->properties.size(), 2u);
    EXPECT_EQ(lnk->properties[0].column, "since");
    EXPECT_EQ(lnk->properties[1].column, "weight");
}

// -- BULK LINK tests ----------------------------------------------------------

TEST(Parser, BulkLinkBasic) {
    auto stmt =
        parse_one("LINK users TO users VIA follows VALUES ('alice', 'bob'), ('carol', 'dave')");
    auto* blk = dynamic_cast<BulkLinkStmt*>(stmt.get());
    ASSERT_NE(blk, nullptr);
    EXPECT_EQ(blk->source_table, "users");
    EXPECT_EQ(blk->target_table, "users");
    EXPECT_EQ(blk->edge_type, "follows");
    ASSERT_EQ(blk->rows.size(), 2u);
    ASSERT_EQ(blk->rows[0].size(), 2u);
    ASSERT_EQ(blk->rows[1].size(), 2u);
    auto* r0c0 = dynamic_cast<LiteralExpr*>(blk->rows[0][0].get());
    ASSERT_NE(r0c0, nullptr);
    EXPECT_EQ(r0c0->value, "alice");
    auto* r0c1 = dynamic_cast<LiteralExpr*>(blk->rows[0][1].get());
    ASSERT_NE(r0c1, nullptr);
    EXPECT_EQ(r0c1->value, "bob");
}

TEST(Parser, BulkLinkWithProperties) {
    auto stmt = parse_one("LINK users TO products VIA rated VALUES "
                          "('alice', 42, 4.5, 'great'), "
                          "('bob', 17, 3.0, 'meh')");
    auto* blk = dynamic_cast<BulkLinkStmt*>(stmt.get());
    ASSERT_NE(blk, nullptr);
    ASSERT_EQ(blk->rows.size(), 2u);
    ASSERT_EQ(blk->rows[0].size(), 4u); // src_key, tgt_key, score, review
    ASSERT_EQ(blk->rows[1].size(), 4u);
}

TEST(Parser, BulkLinkSingleRow) {
    auto stmt = parse_one("LINK users TO posts VIA authored VALUES ('alice', 1)");
    auto* blk = dynamic_cast<BulkLinkStmt*>(stmt.get());
    ASSERT_NE(blk, nullptr);
    ASSERT_EQ(blk->rows.size(), 1u);
    ASSERT_EQ(blk->rows[0].size(), 2u);
}

TEST(Parser, SingleLinkStillWorks) {
    // Ensure the existing single-LINK syntax still produces LinkStmt.
    auto stmt = parse_one("LINK users('alice') TO users('bob') VIA follows");
    auto* lnk = dynamic_cast<LinkStmt*>(stmt.get());
    ASSERT_NE(lnk, nullptr) << "single LINK should parse as LinkStmt, not BulkLinkStmt";
    EXPECT_EQ(lnk->source_table, "users");
    EXPECT_EQ(lnk->target_table, "users");
    EXPECT_EQ(lnk->edge_type, "follows");
}

TEST(Parser, LinkBulkToDisambiguationVsParenForm) {
    // Regression test for GDB-1207: parse_link disambiguates the bulk form
    // (LINK table TO ...) from the single-edge form (LINK table(key) TO ...)
    // using a single-token lookahead on the token following the source table
    // name. Verify both forms still resolve to the correct statement type.
    auto bulk_stmt = parse_one("LINK accounts TO accounts VIA linked_to VALUES ('x', 'y')");
    auto* blk = dynamic_cast<BulkLinkStmt*>(bulk_stmt.get());
    ASSERT_NE(blk, nullptr) << "LINK table TO ... should parse as BulkLinkStmt";
    EXPECT_EQ(blk->source_table, "accounts");
    EXPECT_EQ(blk->target_table, "accounts");

    auto single_stmt = parse_one("LINK accounts(1) TO accounts(2) VIA linked_to");
    auto* lnk = dynamic_cast<LinkStmt*>(single_stmt.get());
    ASSERT_NE(lnk, nullptr) << "LINK table(key) TO ... should parse as LinkStmt";
    EXPECT_EQ(lnk->source_table, "accounts");
    EXPECT_EQ(lnk->target_table, "accounts");
}

// -- UNLINK tests -------------------------------------------------------------

TEST(Parser, UnlinkBasic) {
    auto stmt = parse_one("UNLINK users(1) FROM posts(42) VIA authored");
    auto* ulnk = dynamic_cast<UnlinkStmt*>(stmt.get());
    ASSERT_NE(ulnk, nullptr);
    EXPECT_EQ(ulnk->source_table, "users");
    EXPECT_EQ(ulnk->target_table, "posts");
    EXPECT_EQ(ulnk->edge_type, "authored");
    EXPECT_EQ(ulnk->where_expr, nullptr);
}

TEST(Parser, UnlinkWithWhere) {
    auto stmt = parse_one("UNLINK users(1) FROM users(2) VIA follows WHERE weight < 0.5");
    auto* ulnk = dynamic_cast<UnlinkStmt*>(stmt.get());
    ASSERT_NE(ulnk, nullptr);
    EXPECT_NE(ulnk->where_expr, nullptr);
}

// -- SELECT tests (basic, for subqueries) -------------------------------------

TEST(Parser, SelectBasic) {
    auto stmt = parse_one("SELECT id, name FROM users");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_FALSE(sel->distinct);
    ASSERT_EQ(sel->items.size(), 2u);
    auto* c0 = dynamic_cast<ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(c0, nullptr);
    EXPECT_EQ(c0->column, "id");
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].name, "users");
}

TEST(Parser, SelectQuotedIdentifiers) {
    // The audit's motivating PG-wire-compat example.
    auto stmt = parse_one("SELECT \"id\" FROM \"users\"");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* c0 = dynamic_cast<ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(c0, nullptr);
    EXPECT_EQ(c0->column, "id");
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].name, "users");
}

TEST(Parser, SelectQuotedKeywordAsColumnName) {
    // A quoted keyword ("select") is a plain column reference, not a
    // re-parse of the SELECT keyword.
    auto stmt = parse_one("SELECT \"select\" FROM t");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* c0 = dynamic_cast<ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(c0, nullptr);
    EXPECT_EQ(c0->column, "select");
}

TEST(Parser, SelectQuotedIdentifierPreservesCase) {
    // Quoted identifiers are case-sensitive/case-preserving, matching the
    // engine's existing case-preserving unquoted-identifier behavior.
    auto stmt = parse_one("SELECT \"MixedCase\" FROM t");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* c0 = dynamic_cast<ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(c0, nullptr);
    EXPECT_EQ(c0->column, "MixedCase");
}

TEST(Parser, SelectQuotedIdentifierEscapedQuoteUnescaped) {
    // "a""b" -> column name a"b (the "" is unescaped to a literal ").
    auto stmt = parse_one("SELECT \"a\"\"b\" FROM t");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* c0 = dynamic_cast<ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(c0, nullptr);
    EXPECT_EQ(c0->column, "a\"b");
}

TEST(Parser, SelectQuotedIdentifierQualifiedColumn) {
    auto stmt = parse_one("SELECT \"u\".\"id\" FROM \"users\" AS \"u\"");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* c0 = dynamic_cast<ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(c0, nullptr);
    EXPECT_EQ(c0->table, "u");
    EXPECT_EQ(c0->column, "id");
}

TEST(Parser, UnterminatedQuotedIdentifierIsParseError) {
    expect_parse_error("SELECT \"unterminated FROM t");
}

TEST(Parser, SelectStar) {
    auto stmt = parse_one("SELECT * FROM users");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    EXPECT_TRUE(sel->items[0].is_star);
}

TEST(Parser, SelectTableStar) {
    auto stmt = parse_one("SELECT u.* FROM users AS u");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    EXPECT_TRUE(sel->items[0].is_star);
    EXPECT_EQ(sel->items[0].table_star, "u");
    EXPECT_EQ(sel->from[0].alias, "u");
}

TEST(Parser, SelectDistinct) {
    auto stmt = parse_one("SELECT DISTINCT name FROM users");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_TRUE(sel->distinct);
}

TEST(Parser, SelectWhere) {
    auto stmt = parse_one("SELECT id FROM users WHERE age > 18");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_NE(sel->where_expr, nullptr);
}

TEST(Parser, SelectOrderBy) {
    auto stmt = parse_one("SELECT name FROM users ORDER BY name ASC, age DESC");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->order_by.size(), 2u);
    EXPECT_EQ(sel->order_by[0].direction, SortDirection::ASC);
    EXPECT_EQ(sel->order_by[1].direction, SortDirection::DESC);
}

TEST(Parser, SelectLimitOffset) {
    auto stmt = parse_one("SELECT * FROM users LIMIT 10 OFFSET 20");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* lim = dynamic_cast<LiteralExpr*>(sel->limit.get());
    ASSERT_NE(lim, nullptr);
    EXPECT_EQ(lim->value, "10");
    auto* off = dynamic_cast<LiteralExpr*>(sel->offset.get());
    ASSERT_NE(off, nullptr);
    EXPECT_EQ(off->value, "20");
}

TEST(Parser, SelectAlias) {
    auto stmt = parse_one("SELECT name AS n FROM users");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->items[0].alias, "n");
}

// =============================================================================
// Enhanced expression tests (GDB-104)
// =============================================================================

// -- IN expression ------------------------------------------------------------

TEST(Parser, ExprInValues) {
    auto stmt = parse_one("SELECT id FROM users WHERE status IN ('active', 'pending')");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* in_expr = dynamic_cast<InExpr*>(sel->where_expr.get());
    ASSERT_NE(in_expr, nullptr);
    EXPECT_FALSE(in_expr->negated);
    ASSERT_EQ(in_expr->values.size(), 2u);
    auto* v0 = dynamic_cast<LiteralExpr*>(in_expr->values[0].get());
    ASSERT_NE(v0, nullptr);
    EXPECT_EQ(v0->value, "active");
}

TEST(Parser, ExprNotIn) {
    auto stmt = parse_one("SELECT id FROM users WHERE id NOT IN (1, 2, 3)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* in_expr = dynamic_cast<InExpr*>(sel->where_expr.get());
    ASSERT_NE(in_expr, nullptr);
    EXPECT_TRUE(in_expr->negated);
    ASSERT_EQ(in_expr->values.size(), 3u);
}

TEST(Parser, ExprInSubquery) {
    auto stmt = parse_one("SELECT * FROM orders WHERE user_id IN (SELECT id FROM users)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* in_expr = dynamic_cast<InExpr*>(sel->where_expr.get());
    ASSERT_NE(in_expr, nullptr);
    EXPECT_TRUE(in_expr->values.empty());
    auto* sub = dynamic_cast<SelectStmt*>(in_expr->subquery.get());
    ASSERT_NE(sub, nullptr);
}

// -- BETWEEN expression -------------------------------------------------------

TEST(Parser, ExprBetween) {
    auto stmt = parse_one("SELECT * FROM products WHERE price BETWEEN 10 AND 100");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* bet = dynamic_cast<BetweenExpr*>(sel->where_expr.get());
    ASSERT_NE(bet, nullptr);
    EXPECT_FALSE(bet->negated);
    auto* low = dynamic_cast<LiteralExpr*>(bet->low.get());
    ASSERT_NE(low, nullptr);
    EXPECT_EQ(low->value, "10");
    auto* high = dynamic_cast<LiteralExpr*>(bet->high.get());
    ASSERT_NE(high, nullptr);
    EXPECT_EQ(high->value, "100");
}

TEST(Parser, ExprNotBetween) {
    auto stmt = parse_one("SELECT * FROM t WHERE x NOT BETWEEN 0 AND 9");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* bet = dynamic_cast<BetweenExpr*>(sel->where_expr.get());
    ASSERT_NE(bet, nullptr);
    EXPECT_TRUE(bet->negated);
}

TEST(Parser, ExprBetweenAndOther) {
    // x BETWEEN 1 AND 10 AND y = 5 — first AND is BETWEEN, second is boolean.
    auto stmt = parse_one("SELECT * FROM t WHERE x BETWEEN 1 AND 10 AND y = 5");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* and_expr = dynamic_cast<BinaryExpr*>(sel->where_expr.get());
    ASSERT_NE(and_expr, nullptr);
    EXPECT_EQ(and_expr->op, BinaryOp::AND);
    auto* bet = dynamic_cast<BetweenExpr*>(and_expr->lhs.get());
    ASSERT_NE(bet, nullptr);
}

// -- LIKE expression ----------------------------------------------------------

TEST(Parser, ExprLike) {
    auto stmt = parse_one("SELECT * FROM users WHERE name LIKE 'A%'");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* like = dynamic_cast<LikeExpr*>(sel->where_expr.get());
    ASSERT_NE(like, nullptr);
    EXPECT_FALSE(like->negated);
    auto* pat = dynamic_cast<LiteralExpr*>(like->pattern.get());
    ASSERT_NE(pat, nullptr);
    EXPECT_EQ(pat->value, "A%");
}

TEST(Parser, ExprNotLike) {
    auto stmt = parse_one("SELECT * FROM users WHERE name NOT LIKE '%test%'");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* like = dynamic_cast<LikeExpr*>(sel->where_expr.get());
    ASSERT_NE(like, nullptr);
    EXPECT_TRUE(like->negated);
}

// -- CASE expression ----------------------------------------------------------

TEST(Parser, ExprCaseSearched) {
    auto stmt = parse_one("SELECT CASE WHEN age < 18 THEN 'minor' "
                          "WHEN age < 65 THEN 'adult' ELSE 'senior' END FROM users");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* case_expr = dynamic_cast<CaseExpr*>(sel->items[0].expr.get());
    ASSERT_NE(case_expr, nullptr);
    EXPECT_EQ(case_expr->operand, nullptr);
    ASSERT_EQ(case_expr->whens.size(), 2u);
    EXPECT_NE(case_expr->else_expr, nullptr);
}

TEST(Parser, ExprCaseSimple) {
    auto stmt = parse_one("SELECT CASE status WHEN 'A' THEN 'Active' "
                          "WHEN 'I' THEN 'Inactive' END FROM users");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* case_expr = dynamic_cast<CaseExpr*>(sel->items[0].expr.get());
    ASSERT_NE(case_expr, nullptr);
    EXPECT_NE(case_expr->operand, nullptr);
    ASSERT_EQ(case_expr->whens.size(), 2u);
    EXPECT_EQ(case_expr->else_expr, nullptr);
}

// -- CAST expression ----------------------------------------------------------

TEST(Parser, ExprCast) {
    auto stmt = parse_one("SELECT CAST(age AS TEXT) FROM users");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* cast = dynamic_cast<CastExpr*>(sel->items[0].expr.get());
    ASSERT_NE(cast, nullptr);
    EXPECT_EQ(cast->target_type.name, "TEXT");
    auto* col = dynamic_cast<ColumnRefExpr*>(cast->expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "age");
}

TEST(Parser, ExprColonColonCast) {
    auto stmt = parse_one("SELECT age::TEXT FROM users");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* cast = dynamic_cast<CastExpr*>(sel->items[0].expr.get());
    ASSERT_NE(cast, nullptr);
    EXPECT_EQ(cast->target_type.name, "TEXT");
}

// -- EXISTS expression --------------------------------------------------------

TEST(Parser, ExprExists) {
    auto stmt = parse_one(
        "SELECT * FROM users WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* exists = dynamic_cast<ExistsExpr*>(sel->where_expr.get());
    ASSERT_NE(exists, nullptr);
    auto* sub = dynamic_cast<SelectStmt*>(exists->subquery.get());
    ASSERT_NE(sub, nullptr);
}

// -- Subquery expression ------------------------------------------------------

TEST(Parser, ExprSubquery) {
    auto stmt = parse_one("SELECT (SELECT COUNT(*) FROM orders) FROM users");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* sub = dynamic_cast<SubqueryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(sub, nullptr);
}

// -- Array literal ------------------------------------------------------------

TEST(Parser, ExprArray) {
    auto stmt = parse_one("SELECT * FROM products WHERE vec = [1.0, 2.0, 3.0]");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* eq = dynamic_cast<BinaryExpr*>(sel->where_expr.get());
    ASSERT_NE(eq, nullptr);
    auto* arr = dynamic_cast<ArrayExpr*>(eq->rhs.get());
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->elements.size(), 3u);
}

// -- Aggregate functions with DISTINCT and STAR -------------------------------

TEST(Parser, ExprCountStar) {
    auto stmt = parse_one("SELECT COUNT(*) FROM users");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* fn = dynamic_cast<FunctionCallExpr*>(sel->items[0].expr.get());
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->name, "COUNT");
    ASSERT_EQ(fn->args.size(), 1u);
    auto* star = dynamic_cast<ColumnRefExpr*>(fn->args[0].get());
    ASSERT_NE(star, nullptr);
    EXPECT_EQ(star->column, "*");
}

TEST(Parser, ExprCountDistinct) {
    auto stmt = parse_one("SELECT COUNT(DISTINCT name) FROM users");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* fn = dynamic_cast<FunctionCallExpr*>(sel->items[0].expr.get());
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->name, "COUNT");
    EXPECT_TRUE(fn->distinct);
    ASSERT_EQ(fn->args.size(), 1u);
}

// -- Qualified column ref -----------------------------------------------------

TEST(Parser, ExprQualifiedColumn) {
    auto stmt = parse_one("SELECT users.name FROM users WHERE users.id = 1");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* col = dynamic_cast<ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->table, "users");
    EXPECT_EQ(col->column, "name");
}

// -- Complex expressions combining operators ----------------------------------

TEST(Parser, ExprComplex) {
    auto stmt = parse_one("SELECT * FROM t WHERE a > 1 AND b IN (1, 2) OR c LIKE '%x%'");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    // OR has lowest precedence: (a > 1 AND b IN (1, 2)) OR (c LIKE '%x%')
    auto* or_expr = dynamic_cast<BinaryExpr*>(sel->where_expr.get());
    ASSERT_NE(or_expr, nullptr);
    EXPECT_EQ(or_expr->op, BinaryOp::OR);
}

// -- DML error handling -------------------------------------------------------

// -- JOIN tests ---------------------------------------------------------------

TEST(Parser, SelectInnerJoin) {
    auto stmt = parse_one("SELECT u.name, o.total FROM users u "
                          "INNER JOIN orders o ON u.id = o.user_id");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].name, "users");
    EXPECT_EQ(sel->from[0].alias, "u");
    ASSERT_EQ(sel->joins.size(), 1u);
    EXPECT_EQ(sel->joins[0].type, JoinType::INNER);
    EXPECT_EQ(sel->joins[0].table.name, "orders");
    EXPECT_EQ(sel->joins[0].table.alias, "o");
    EXPECT_NE(sel->joins[0].on_expr, nullptr);
}

TEST(Parser, SelectLeftJoin) {
    auto stmt = parse_one("SELECT * FROM users LEFT JOIN orders ON users.id = orders.user_id");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->joins.size(), 1u);
    EXPECT_EQ(sel->joins[0].type, JoinType::LEFT);
}

TEST(Parser, SelectLeftOuterJoin) {
    auto stmt =
        parse_one("SELECT * FROM users LEFT OUTER JOIN orders ON users.id = orders.user_id");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->joins.size(), 1u);
    EXPECT_EQ(sel->joins[0].type, JoinType::LEFT);
}

TEST(Parser, SelectRightJoin) {
    auto stmt = parse_one("SELECT * FROM users RIGHT JOIN orders ON users.id = orders.user_id");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->joins.size(), 1u);
    EXPECT_EQ(sel->joins[0].type, JoinType::RIGHT);
}

TEST(Parser, SelectFullOuterJoin) {
    auto stmt = parse_one("SELECT * FROM a FULL OUTER JOIN b ON a.id = b.id");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->joins.size(), 1u);
    EXPECT_EQ(sel->joins[0].type, JoinType::FULL);
}

TEST(Parser, SelectCrossJoin) {
    auto stmt = parse_one("SELECT * FROM a CROSS JOIN b");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->joins.size(), 1u);
    EXPECT_EQ(sel->joins[0].type, JoinType::CROSS);
    EXPECT_EQ(sel->joins[0].on_expr, nullptr);
}

TEST(Parser, SelectBareJoin) {
    auto stmt = parse_one("SELECT * FROM a JOIN b ON a.id = b.a_id");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->joins.size(), 1u);
    EXPECT_EQ(sel->joins[0].type, JoinType::INNER);
}

TEST(Parser, SelectMultiJoin) {
    auto stmt = parse_one("SELECT * FROM a "
                          "JOIN b ON a.id = b.a_id "
                          "LEFT JOIN c ON b.id = c.b_id");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->joins.size(), 2u);
    EXPECT_EQ(sel->joins[0].type, JoinType::INNER);
    EXPECT_EQ(sel->joins[1].type, JoinType::LEFT);
}

// -- GROUP BY / HAVING tests --------------------------------------------------

TEST(Parser, SelectGroupBy) {
    auto stmt = parse_one("SELECT department, COUNT(*) FROM employees GROUP BY department");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->group_by.size(), 1u);
    auto* col = dynamic_cast<ColumnRefExpr*>(sel->group_by[0].get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "department");
}

TEST(Parser, SelectGroupByMultiple) {
    auto stmt = parse_one("SELECT dept, role, COUNT(*) FROM emp GROUP BY dept, role");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->group_by.size(), 2u);
}

TEST(Parser, SelectGroupByHaving) {
    auto stmt = parse_one("SELECT dept, COUNT(*) FROM emp GROUP BY dept HAVING COUNT(*) > 5");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->group_by.size(), 1u);
    EXPECT_NE(sel->having_expr, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(sel->having_expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::GREATER);
}

// -- Set operations -----------------------------------------------------------

TEST(Parser, SelectUnion) {
    auto stmt = parse_one("SELECT id FROM a UNION SELECT id FROM b");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->set_op, SelectStmt::SetOp::UNION);
    auto* rhs = dynamic_cast<SelectStmt*>(sel->set_rhs.get());
    ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(rhs->from[0].name, "b");
}

TEST(Parser, SelectUnionAll) {
    auto stmt = parse_one("SELECT id FROM a UNION ALL SELECT id FROM b");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->set_op, SelectStmt::SetOp::UNION_ALL);
}

TEST(Parser, SelectIntersect) {
    auto stmt = parse_one("SELECT id FROM a INTERSECT SELECT id FROM b");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->set_op, SelectStmt::SetOp::INTERSECT);
}

TEST(Parser, SelectExcept) {
    auto stmt = parse_one("SELECT id FROM a EXCEPT SELECT id FROM b");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->set_op, SelectStmt::SetOp::EXCEPT);
}

// -- CTE (WITH) tests ---------------------------------------------------------

TEST(Parser, SelectWithCTE) {
    auto stmt = parse_one("WITH active AS (SELECT * FROM users WHERE active = TRUE) "
                          "SELECT * FROM active");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->ctes.size(), 1u);
    EXPECT_EQ(sel->ctes[0].name, "active");
    auto* cte_query = dynamic_cast<SelectStmt*>(sel->ctes[0].query.get());
    ASSERT_NE(cte_query, nullptr);
    EXPECT_EQ(sel->from[0].name, "active");
}

TEST(Parser, SelectWithMultipleCTEs) {
    auto stmt = parse_one("WITH a AS (SELECT 1), b AS (SELECT 2) "
                          "SELECT * FROM a, b");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->ctes.size(), 2u);
    EXPECT_EQ(sel->ctes[0].name, "a");
    EXPECT_EQ(sel->ctes[1].name, "b");
}

TEST(Parser, SelectWithCTENamedRecursiveIsNotConfusedWithKeyword) {
    // A CTE legitimately named something other than "recursive" following a
    // plain WITH must still parse normally (regression guard for GDB-1205).
    auto stmt = parse_one("WITH recursively_named AS (SELECT 1) "
                          "SELECT * FROM recursively_named");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->ctes.size(), 1u);
    EXPECT_EQ(sel->ctes[0].name, "recursively_named");
}

TEST(Parser, WithRecursiveReturnsClearParseError) {
    // GDB-1205: WITH RECURSIVE must produce an explicit, actionable parse
    // error instead of silently treating RECURSIVE as the CTE name.
    Lexer lexer("WITH RECURSIVE t AS (SELECT 1) SELECT * FROM t");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value()) << tokens.error().message;

    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    ASSERT_FALSE(stmts.has_value());
    EXPECT_EQ(stmts.error().code, StatusCode::PARSE_ERROR);
    EXPECT_NE(stmts.error().message.find("recursive"), std::string::npos)
        << "actual message: " << stmts.error().message;
    EXPECT_NE(stmts.error().message.find("not supported"), std::string::npos)
        << "actual message: " << stmts.error().message;
}

// -- Subquery in FROM ---------------------------------------------------------

TEST(Parser, SelectSubqueryInFrom) {
    auto stmt = parse_one("SELECT s.id FROM (SELECT id FROM users WHERE active = TRUE) AS s");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].alias, "s");
    auto* sub = dynamic_cast<SelectStmt*>(sel->from[0].subquery.get());
    ASSERT_NE(sub, nullptr);
    EXPECT_EQ(sub->from[0].name, "users");
}

TEST(Parser, SelectSubqueryImplicitAlias) {
    auto stmt = parse_one("SELECT s.id FROM (SELECT id FROM users) s");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->from[0].alias, "s");
    EXPECT_NE(sel->from[0].subquery, nullptr);
}

// -- Implicit alias tests -----------------------------------------------------

TEST(Parser, SelectImplicitTableAlias) {
    auto stmt = parse_one("SELECT u.name FROM users u WHERE u.id = 1");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->from[0].name, "users");
    EXPECT_EQ(sel->from[0].alias, "u");
}

TEST(Parser, SelectImplicitColumnAlias) {
    auto stmt = parse_one("SELECT name n FROM users");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->items[0].alias, "n");
}

// -- Complex SELECT combining multiple clauses --------------------------------

TEST(Parser, SelectAllClauses) {
    auto stmt = parse_one("SELECT DISTINCT dept, COUNT(*) AS cnt "
                          "FROM employees e "
                          "JOIN departments d ON e.dept_id = d.id "
                          "WHERE e.active = TRUE "
                          "GROUP BY dept "
                          "HAVING COUNT(*) > 3 "
                          "ORDER BY cnt DESC "
                          "LIMIT 10 OFFSET 5");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_TRUE(sel->distinct);
    ASSERT_EQ(sel->items.size(), 2u);
    EXPECT_EQ(sel->items[1].alias, "cnt");
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].alias, "e");
    ASSERT_EQ(sel->joins.size(), 1u);
    EXPECT_NE(sel->where_expr, nullptr);
    ASSERT_EQ(sel->group_by.size(), 1u);
    EXPECT_NE(sel->having_expr, nullptr);
    ASSERT_EQ(sel->order_by.size(), 1u);
    EXPECT_EQ(sel->order_by[0].direction, SortDirection::DESC);
    EXPECT_NE(sel->limit, nullptr);
    EXPECT_NE(sel->offset, nullptr);
}

// -- DML error handling -------------------------------------------------------

TEST(Parser, ErrorInsertMissingInto) {
    expect_parse_error("INSERT users (name) VALUES ('test')");
}

TEST(Parser, ErrorUpdateMissingSet) {
    expect_parse_error("UPDATE users name = 'test'");
}

TEST(Parser, ErrorDeleteMissingFrom) {
    expect_parse_error("DELETE users WHERE id = 1");
}

// =============================================================================
// Graph statement tests (GDB-106)
// =============================================================================

// -- TRAVERSE tests -----------------------------------------------------------

TEST(Parser, TraverseBasic) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1)");
    auto* tr = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "follows");
    EXPECT_EQ(tr->from_table, "users");
    EXPECT_EQ(tr->direction, TraverseDirection::OUT);
    EXPECT_FALSE(tr->max_depth.has_value());
    EXPECT_EQ(tr->where_expr, nullptr);
    EXPECT_FALSE(tr->fetch);
}

TEST(Parser, TraverseDirection) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1) DIRECTION IN");
    auto* tr = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->direction, TraverseDirection::IN);
}

TEST(Parser, TraverseMaxDepth) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1) MAX_DEPTH 3");
    auto* tr = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->max_depth.value(), 3);
}

TEST(Parser, TraverseAllOptions) {
    auto stmt = parse_one("TRAVERSE knows FROM users(42) DIRECTION BOTH MAX_DEPTH 5 "
                          "WHERE weight > 0.5 FETCH");
    auto* tr = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "knows");
    EXPECT_EQ(tr->direction, TraverseDirection::BOTH);
    EXPECT_EQ(tr->max_depth.value(), 5);
    EXPECT_NE(tr->where_expr, nullptr);
    EXPECT_TRUE(tr->fetch);
}

// -- TRAVERSE-in-FROM tests (GDB-263) -----------------------------------------

TEST(Parser, TraverseInFromBasic) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    ASSERT_NE(sel->from[0].traverse_source, nullptr);
    EXPECT_TRUE(sel->from[0].name.empty());
    EXPECT_TRUE(sel->from[0].alias.empty());
    auto* tr = dynamic_cast<TraverseStmt*>(sel->from[0].traverse_source.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "follows");
    EXPECT_EQ(tr->from_table, "users");
    EXPECT_EQ(tr->direction, TraverseDirection::OUT);
}

TEST(Parser, TraverseInFromWithAlias) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT AS t");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    ASSERT_NE(sel->from[0].traverse_source, nullptr);
    EXPECT_EQ(sel->from[0].alias, "t");
    auto* tr = dynamic_cast<TraverseStmt*>(sel->from[0].traverse_source.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "follows");
}

TEST(Parser, TraverseInFromImplicitAlias) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT trav");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    ASSERT_NE(sel->from[0].traverse_source, nullptr);
    EXPECT_EQ(sel->from[0].alias, "trav");
}

TEST(Parser, TraverseInFromAllClauses) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE knows FROM users(42) DIRECTION BOTH MAX_DEPTH 5 "
                          "WHERE weight > 0.5 FETCH AS t");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    ASSERT_NE(sel->from[0].traverse_source, nullptr);
    EXPECT_EQ(sel->from[0].alias, "t");
    auto* tr = dynamic_cast<TraverseStmt*>(sel->from[0].traverse_source.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "knows");
    EXPECT_EQ(tr->direction, TraverseDirection::BOTH);
    ASSERT_TRUE(tr->max_depth.has_value());
    EXPECT_EQ(*tr->max_depth, 5);
    EXPECT_NE(tr->where_expr, nullptr);
    EXPECT_TRUE(tr->fetch);
}

TEST(Parser, TraverseInFromDirectionIn) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION IN");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    auto* tr = dynamic_cast<TraverseStmt*>(sel->from[0].traverse_source.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->direction, TraverseDirection::IN);
}

TEST(Parser, TraverseInFromMaxDepthOnly) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) MAX_DEPTH 3");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    auto* tr = dynamic_cast<TraverseStmt*>(sel->from[0].traverse_source.get());
    ASSERT_NE(tr, nullptr);
    ASSERT_TRUE(tr->max_depth.has_value());
    EXPECT_EQ(*tr->max_depth, 3);
}

TEST(Parser, TraverseStandaloneStillWorks) {
    // Backward compatibility: standalone TRAVERSE still parses as TraverseStmt.
    auto stmt = parse_one("TRAVERSE follows FROM users(1) DIRECTION OUT");
    auto* tr = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "follows");
    EXPECT_EQ(tr->from_table, "users");
    EXPECT_EQ(tr->direction, TraverseDirection::OUT);
}

TEST(Parser, TraverseInFromMalformedMissingEdge) {
    expect_parse_error("SELECT * FROM TRAVERSE FROM users(1)");
}

TEST(Parser, TraverseInFromMalformedMissingFrom) {
    expect_parse_error("SELECT * FROM TRAVERSE follows users(1)");
}

TEST(Parser, TraverseInFromMalformedMissingKey) {
    expect_parse_error("SELECT * FROM TRAVERSE follows FROM users");
}

// -- TRAVERSE WITH TRACE tests (GDB-677) --------------------------------------

TEST(Parser, TraverseWithTrace) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1) WITH TRACE");
    auto* tr = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "follows");
    EXPECT_EQ(tr->from_table, "users");
    EXPECT_TRUE(tr->trace);
    EXPECT_FALSE(tr->fetch);
}

TEST(Parser, TraverseFetchWithTrace) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1) FETCH WITH TRACE");
    auto* tr = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_TRUE(tr->fetch);
    EXPECT_TRUE(tr->trace);
}

TEST(Parser, TraverseNoTraceDefault) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1)");
    auto* tr = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_FALSE(tr->trace);
}

TEST(Parser, TraverseWithTraceInSelect) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) WITH TRACE");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    ASSERT_NE(sel->from[0].traverse_source, nullptr);
    auto* tr = dynamic_cast<TraverseStmt*>(sel->from[0].traverse_source.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_TRUE(tr->trace);
}

// -- TRACE-as-identifier regression tests (GDB-693) ---------------------------
//
// GDB-677 added TRACE to the lexer keyword map but omitted it from
// is_name_token(), making the bare word `trace` a fully reserved keyword and
// breaking previously-valid SQL. These tests pin the fix: `trace` must remain
// usable as a table name, column name, SELECT column, and edge-type identifier,
// consistent with the analogous TRAVERSE-modifier keyword FETCH.

TEST(Parser, TraceUsableAsTableName) {
    auto stmt = parse_one("CREATE TABLE trace (id INT)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->name, "trace");
    ASSERT_EQ(ct->columns.size(), 1u);
    EXPECT_EQ(ct->columns[0].name, "id");
}

TEST(Parser, TraceUsableAsColumnName) {
    auto stmt = parse_one("CREATE TABLE t (trace INT)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->name, "t");
    ASSERT_EQ(ct->columns.size(), 1u);
    EXPECT_EQ(ct->columns[0].name, "trace");
    EXPECT_EQ(ct->columns[0].type.name, "INT");
}

TEST(Parser, TraceUsableAsSelectColumn) {
    auto stmt = parse_one("SELECT trace FROM events");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* col = dynamic_cast<ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "trace");
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].name, "events");
}

TEST(Parser, TraceUsableAsEdgeType) {
    auto stmt = parse_one("TRAVERSE trace FROM users(1)");
    auto* tr = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "trace");
    EXPECT_EQ(tr->from_table, "users");
    EXPECT_FALSE(tr->trace);
}

// Control: the sibling TRAVERSE-modifier keyword FETCH was already accepted as
// an identifier. This guards against a regression in the opposite direction.
TEST(Parser, FetchUsableAsTableName) {
    auto stmt = parse_one("CREATE TABLE fetch (id INT)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->name, "fetch");
    ASSERT_EQ(ct->columns.size(), 1u);
    EXPECT_EQ(ct->columns[0].name, "id");
}

// -- WITH TRACE clause-combination tests (GDB-680) -----------------------------
//
// WITH TRACE is the final optional clause of a TRAVERSE statement. These tests
// pin its interaction with every other optional clause (DIRECTION, MAX_DEPTH,
// MODE, WHERE, FETCH) and its error handling.

TEST(Parser, TraverseTraceAfterDirectionAndMaxDepth) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1) DIRECTION IN MAX_DEPTH 2 WITH TRACE");
    auto* tr = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->direction, TraverseDirection::IN);
    ASSERT_TRUE(tr->max_depth.has_value());
    EXPECT_EQ(*tr->max_depth, 2);
    EXPECT_TRUE(tr->trace);
}

TEST(Parser, TraverseModeEdgesWithTrace) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) MODE EDGES WITH TRACE");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    auto* tr = dynamic_cast<TraverseStmt*>(sel->from[0].traverse_source.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->mode, TraverseMode::EDGES);
    EXPECT_TRUE(tr->trace);
}

TEST(Parser, TraverseWhereThenTrace) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1) WHERE depth > 1 WITH TRACE");
    auto* tr = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(tr, nullptr);
    ASSERT_NE(tr->where_expr, nullptr);
    EXPECT_TRUE(tr->trace);
}

TEST(Parser, TraverseAllClausesWithTrace) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1) DIRECTION BOTH MAX_DEPTH 3 "
                          "WHERE depth > 0 FETCH WITH TRACE");
    auto* tr = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->direction, TraverseDirection::BOTH);
    ASSERT_TRUE(tr->max_depth.has_value());
    EXPECT_EQ(*tr->max_depth, 3);
    ASSERT_NE(tr->where_expr, nullptr);
    EXPECT_TRUE(tr->fetch);
    EXPECT_TRUE(tr->trace);
}

TEST(Parser, TraverseWithMissingTraceKeywordErrors) {
    expect_parse_error("TRAVERSE follows FROM users(1) WITH");
}

TEST(Parser, TraverseWithWrongKeywordAfterWithErrors) {
    expect_parse_error("TRAVERSE follows FROM users(1) WITH FETCH");
}

// -- SHORTEST PATH tests ------------------------------------------------------

TEST(Parser, ShortestPathBasic) {
    auto stmt = parse_one("SHORTEST PATH FROM users(1) TO users(42) VIA follows");
    auto* sp = dynamic_cast<ShortestPathStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->from_table, "users");
    EXPECT_EQ(sp->to_table, "users");
    EXPECT_EQ(sp->edge_type, "follows");
    EXPECT_EQ(sp->direction, TraverseDirection::OUT);
    EXPECT_FALSE(sp->max_depth.has_value());
}

TEST(Parser, ShortestPathWithOptions) {
    auto stmt = parse_one("SHORTEST PATH FROM users(1) TO users(99) VIA knows "
                          "DIRECTION BOTH MAX_DEPTH 10");
    auto* sp = dynamic_cast<ShortestPathStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->direction, TraverseDirection::BOTH);
    EXPECT_EQ(sp->max_depth.value(), 10);
}

// -- MATCH tests --------------------------------------------------------------

TEST(Parser, MatchBasic) {
    auto stmt = parse_one("MATCH (a:users)-[e:follows]->(b:users) RETURN a.name, b.name");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 2u);
    EXPECT_EQ(m->pattern[0].node.variable, "a");
    EXPECT_EQ(m->pattern[0].node.label, "users");
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->variable, "e");
    EXPECT_EQ(m->pattern[0].outgoing_edge->edge_type, "follows");
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::OUT);
    EXPECT_EQ(m->pattern[1].node.variable, "b");
    EXPECT_EQ(m->pattern[1].node.label, "users");
    EXPECT_FALSE(m->pattern[1].outgoing_edge.has_value());
    ASSERT_EQ(m->return_items.size(), 2u);
}

TEST(Parser, MatchWithWhere) {
    auto stmt = parse_one("MATCH (a:users)-[e:follows]->(b:users) "
                          "WHERE a.age > 18 RETURN b.name");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_NE(m->where_expr, nullptr);
    ASSERT_EQ(m->return_items.size(), 1u);
}

TEST(Parser, MatchIncoming) {
    auto stmt = parse_one("MATCH (a:users)<-[e:follows]-(b:users) RETURN a.name");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::IN);
}

TEST(Parser, MatchUndirected) {
    auto stmt = parse_one("MATCH (a:users)-[e:knows]-(b:users) RETURN a.name");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::BOTH);
}

TEST(Parser, MatchMultiHop) {
    auto stmt = parse_one("MATCH (a:users)-[e1:follows]->(b:users)-[e2:follows]->(c:users) "
                          "RETURN a.name, c.name");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 3u);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    ASSERT_TRUE(m->pattern[1].outgoing_edge.has_value());
    EXPECT_FALSE(m->pattern[2].outgoing_edge.has_value());
}

// -- NEAREST predicate tests --------------------------------------------------

// Find the NEAREST(...) predicate in a WHERE expression tree.
static const NearestExpr* find_nearest(const Expr* e) {
    if (e == nullptr) {
        return nullptr;
    }
    if (auto* n = dynamic_cast<const NearestExpr*>(e)) {
        return n;
    }
    if (auto* b = dynamic_cast<const BinaryExpr*>(e)) {
        if (auto* l = find_nearest(b->lhs.get())) {
            return l;
        }
        return find_nearest(b->rhs.get());
    }
    if (auto* u = dynamic_cast<const UnaryExpr*>(e)) {
        return find_nearest(u->operand.get());
    }
    return nullptr;
}

TEST(Parser, NearestBasic) {
    auto stmt = parse_one("SELECT * FROM products WHERE NEAREST(embedding, 5) TO [1.0, 2.0, 3.0]");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* n = find_nearest(sel->where_expr.get());
    ASSERT_NE(n, nullptr);
    auto* k = dynamic_cast<LiteralExpr*>(n->k.get());
    ASSERT_NE(k, nullptr);
    EXPECT_EQ(k->value, "5");
    auto* col = dynamic_cast<ColumnRefExpr*>(n->column.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "embedding");
    EXPECT_EQ(n->metric, NearestMetric::COSINE);
}

TEST(Parser, NearestWithResidualFilter) {
    auto stmt =
        parse_one("SELECT * FROM products WHERE NEAREST(vec, 10) TO [1.0, 0.0] AND active = TRUE");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_NE(find_nearest(sel->where_expr.get()), nullptr);
}

TEST(Parser, NearestWithMetric) {
    auto stmt = parse_one("SELECT * FROM items WHERE NEAREST(embedding, 5) TO [1.0, 2.0] USING L2");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* n = find_nearest(sel->where_expr.get());
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->metric, NearestMetric::L2);
}

TEST(Parser, NearestWithinTraverse) {
    auto stmt =
        parse_one("SELECT * FROM articles WHERE NEAREST(content_vec, 5) TO 'machine learning' "
                  "WITHIN TRAVERSE cites FROM articles('abc-123') DIRECTION OUT MAX_DEPTH 3");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* n = find_nearest(sel->where_expr.get());
    ASSERT_NE(n, nullptr);
    auto* col = dynamic_cast<ColumnRefExpr*>(n->column.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "content_vec");
    ASSERT_NE(n->within_traverse, nullptr);
    auto* t = dynamic_cast<TraverseStmt*>(n->within_traverse.get());
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->edge_type, "cites");
    EXPECT_EQ(t->from_table, "articles");
    EXPECT_EQ(t->direction, TraverseDirection::OUT);
    ASSERT_TRUE(t->max_depth.has_value());
    EXPECT_EQ(*t->max_depth, 3);
}

TEST(Parser, NearestWithinTraverseAndResidualUsing) {
    auto stmt = parse_one("SELECT * FROM posts WHERE NEAREST(body_vec, 10) TO 'data analysis' "
                          "WITHIN TRAVERSE authored FROM users('u1') DIRECTION OUT MAX_DEPTH 1 "
                          "USING L2 AND score > 0.5");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* n = find_nearest(sel->where_expr.get());
    ASSERT_NE(n, nullptr);
    EXPECT_NE(n->within_traverse, nullptr);
    EXPECT_EQ(n->metric, NearestMetric::L2);
}

// =============================================================================
// TCL statement tests (GDB-106)
// =============================================================================

TEST(Parser, Begin) {
    auto stmt = parse_one("BEGIN");
    EXPECT_NE(dynamic_cast<BeginStmt*>(stmt.get()), nullptr);
}

TEST(Parser, BeginTransaction) {
    auto stmt = parse_one("BEGIN TRANSACTION");
    EXPECT_NE(dynamic_cast<BeginStmt*>(stmt.get()), nullptr);
}

TEST(Parser, Commit) {
    auto stmt = parse_one("COMMIT");
    EXPECT_NE(dynamic_cast<CommitStmt*>(stmt.get()), nullptr);
}

TEST(Parser, Rollback) {
    auto stmt = parse_one("ROLLBACK");
    auto* rb = dynamic_cast<RollbackStmt*>(stmt.get());
    ASSERT_NE(rb, nullptr);
    EXPECT_TRUE(rb->savepoint.empty());
}

TEST(Parser, RollbackToSavepoint) {
    auto stmt = parse_one("ROLLBACK TO sp1");
    auto* rb = dynamic_cast<RollbackStmt*>(stmt.get());
    ASSERT_NE(rb, nullptr);
    EXPECT_EQ(rb->savepoint, "sp1");
}

TEST(Parser, Savepoint) {
    auto stmt = parse_one("SAVEPOINT sp1");
    auto* sp = dynamic_cast<SavepointStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->name, "sp1");
}

// =============================================================================
// Admin statement tests (GDB-106)
// =============================================================================

TEST(Parser, SetParameter) {
    auto stmt = parse_one("SET work_mem = 1024");
    auto* s = dynamic_cast<SetStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->parameter, "work_mem");
    auto* val = dynamic_cast<LiteralExpr*>(s->value.get());
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(val->value, "1024");
}

TEST(Parser, ShowTables) {
    auto stmt = parse_one("SHOW TABLES");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::TABLES);
}

TEST(Parser, ShowColumns) {
    auto stmt = parse_one("SHOW COLUMNS FROM users");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::COLUMNS);
    EXPECT_EQ(s->name, "users");
}

TEST(Parser, ShowEdgeTypes) {
    auto stmt = parse_one("SHOW EDGE TYPES");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::EDGE_TYPES);
}

TEST(Parser, ShowIndexes) {
    auto stmt = parse_one("SHOW INDEXES");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::INDEXES);
}

TEST(Parser, Explain) {
    auto stmt = parse_one("EXPLAIN SELECT * FROM users");
    auto* e = dynamic_cast<ExplainStmt*>(stmt.get());
    ASSERT_NE(e, nullptr);
    EXPECT_FALSE(e->analyze);
    auto* inner = dynamic_cast<SelectStmt*>(e->statement.get());
    ASSERT_NE(inner, nullptr);
}

TEST(Parser, ExplainAnalyze) {
    auto stmt = parse_one("EXPLAIN ANALYZE SELECT * FROM users");
    auto* e = dynamic_cast<ExplainStmt*>(stmt.get());
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->analyze);
    auto* inner = dynamic_cast<SelectStmt*>(e->statement.get());
    ASSERT_NE(inner, nullptr);
}

TEST(Parser, Describe) {
    auto stmt = parse_one("DESCRIBE users");
    auto* d = dynamic_cast<DescribeStmt*>(stmt.get());
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->table_name, "users");
}

TEST(Parser, Reembed) {
    auto stmt = parse_one("REEMBED TABLE products");
    auto* r = dynamic_cast<ReembedStmt*>(stmt.get());
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->table_name, "products");
}

TEST(Parser, Vacuum) {
    auto stmt = parse_one("VACUUM");
    auto* v = dynamic_cast<VacuumStmt*>(stmt.get());
    ASSERT_NE(v, nullptr);
    EXPECT_TRUE(v->table_name.empty());
}

TEST(Parser, VacuumTable) {
    auto stmt = parse_one("VACUUM users");
    auto* v = dynamic_cast<VacuumStmt*>(stmt.get());
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->table_name, "users");
}

TEST(Parser, Analyze) {
    auto stmt = parse_one("ANALYZE");
    auto* a = dynamic_cast<AnalyzeStmt*>(stmt.get());
    ASSERT_NE(a, nullptr);
    EXPECT_TRUE(a->table_name.empty());
}

TEST(Parser, AnalyzeTable) {
    auto stmt = parse_one("ANALYZE users");
    auto* a = dynamic_cast<AnalyzeStmt*>(stmt.get());
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->table_name, "users");
}

// =============================================================================
// AstVisitor tests (GDB-102)
// =============================================================================

namespace {

/// Test visitor that records which visit() method was called.
class TypeRecordingVisitor : public AstVisitor {
public:
    std::string visited_type;

    // -- Expressions --
    void visit(const LiteralExpr&) override { visited_type = "LiteralExpr"; }
    void visit(const ParamRefExpr&) override { visited_type = "ParamRefExpr"; }
    void visit(const ColumnRefExpr&) override { visited_type = "ColumnRefExpr"; }
    void visit(const BinaryExpr&) override { visited_type = "BinaryExpr"; }
    void visit(const UnaryExpr&) override { visited_type = "UnaryExpr"; }
    void visit(const FunctionCallExpr&) override { visited_type = "FunctionCallExpr"; }
    void visit(const CastExpr&) override { visited_type = "CastExpr"; }
    void visit(const CaseExpr&) override { visited_type = "CaseExpr"; }
    void visit(const InExpr&) override { visited_type = "InExpr"; }
    void visit(const BetweenExpr&) override { visited_type = "BetweenExpr"; }
    void visit(const IsNullExpr&) override { visited_type = "IsNullExpr"; }
    void visit(const LikeExpr&) override { visited_type = "LikeExpr"; }
    void visit(const MatchExpr&) override { visited_type = "MatchExpr"; }
    void visit(const NearestExpr&) override { visited_type = "NearestExpr"; }
    void visit(const ExistsExpr&) override { visited_type = "ExistsExpr"; }
    void visit(const SubqueryExpr&) override { visited_type = "SubqueryExpr"; }
    void visit(const ArrayExpr&) override { visited_type = "ArrayExpr"; }
    void visit(const WindowFunctionExpr&) override { visited_type = "WindowFunctionExpr"; }

    // -- DDL --
    void visit(const CreateTableStmt&) override { visited_type = "CreateTableStmt"; }
    void visit(const DropTableStmt&) override { visited_type = "DropTableStmt"; }
    void visit(const AlterTableStmt&) override { visited_type = "AlterTableStmt"; }
    void visit(const CreateIndexStmt&) override { visited_type = "CreateIndexStmt"; }
    void visit(const DropIndexStmt&) override { visited_type = "DropIndexStmt"; }
    void visit(const CreateEdgeTypeStmt&) override { visited_type = "CreateEdgeTypeStmt"; }
    void visit(const DropEdgeTypeStmt&) override { visited_type = "DropEdgeTypeStmt"; }
    void visit(const CreateDatabaseStmt&) override { visited_type = "CreateDatabaseStmt"; }
    void visit(const DropDatabaseStmt&) override { visited_type = "DropDatabaseStmt"; }
    void visit(const CreateUserStmt&) override { visited_type = "CreateUserStmt"; }
    void visit(const DropUserStmt&) override { visited_type = "DropUserStmt"; }
    void visit(const AlterUserStmt&) override { visited_type = "AlterUserStmt"; }

    // -- DML --
    void visit(const InsertStmt&) override { visited_type = "InsertStmt"; }
    void visit(const UpdateStmt&) override { visited_type = "UpdateStmt"; }
    void visit(const DeleteStmt&) override { visited_type = "DeleteStmt"; }
    void visit(const LinkStmt&) override { visited_type = "LinkStmt"; }
    void visit(const BulkLinkStmt&) override { visited_type = "BulkLinkStmt"; }
    void visit(const UnlinkStmt&) override { visited_type = "UnlinkStmt"; }

    // -- Query --
    void visit(const SelectStmt&) override { visited_type = "SelectStmt"; }
    void visit(const TraverseStmt&) override { visited_type = "TraverseStmt"; }
    void visit(const MatchStmt&) override { visited_type = "MatchStmt"; }
    void visit(const ShortestPathStmt&) override { visited_type = "ShortestPathStmt"; }

    // -- TCL --
    void visit(const BeginStmt&) override { visited_type = "BeginStmt"; }
    void visit(const CommitStmt&) override { visited_type = "CommitStmt"; }
    void visit(const RollbackStmt&) override { visited_type = "RollbackStmt"; }
    void visit(const SavepointStmt&) override { visited_type = "SavepointStmt"; }
    void visit(const ReleaseSavepointStmt&) override { visited_type = "ReleaseSavepointStmt"; }

    // -- Admin --
    void visit(const SetStmt&) override { visited_type = "SetStmt"; }
    void visit(const ShowStmt&) override { visited_type = "ShowStmt"; }
    void visit(const ExplainStmt&) override { visited_type = "ExplainStmt"; }
    void visit(const DescribeStmt&) override { visited_type = "DescribeStmt"; }
    void visit(const BackfillStmt&) override { visited_type = "BackfillStmt"; }
    void visit(const ReembedStmt&) override { visited_type = "ReembedStmt"; }
    void visit(const ReindexStmt&) override { visited_type = "ReindexStmt"; }
    void visit(const VacuumStmt&) override { visited_type = "VacuumStmt"; }
    void visit(const AnalyzeStmt&) override { visited_type = "AnalyzeStmt"; }
};

/// Helper: parse SQL, accept visitor on the resulting statement.
std::string visit_stmt(std::string_view sql) {
    auto stmt = parse_one(sql);
    if (!stmt)
        return "";
    TypeRecordingVisitor v;
    stmt->accept(v);
    return v.visited_type;
}

} // namespace

TEST(AstVisitor, DispatchesDDL) {
    EXPECT_EQ(visit_stmt("CREATE TABLE t (id INT)"), "CreateTableStmt");
    EXPECT_EQ(visit_stmt("DROP TABLE t"), "DropTableStmt");
    EXPECT_EQ(visit_stmt("ALTER TABLE t ADD COLUMN c INT"), "AlterTableStmt");
    EXPECT_EQ(visit_stmt("CREATE INDEX idx ON t(c)"), "CreateIndexStmt");
    EXPECT_EQ(visit_stmt("DROP INDEX idx"), "DropIndexStmt");
    EXPECT_EQ(visit_stmt("CREATE EDGE TYPE follows FROM users TO users"), "CreateEdgeTypeStmt");
    EXPECT_EQ(visit_stmt("DROP EDGE TYPE follows"), "DropEdgeTypeStmt");
    EXPECT_EQ(visit_stmt("CREATE DATABASE mydb"), "CreateDatabaseStmt");
    EXPECT_EQ(visit_stmt("DROP DATABASE mydb"), "DropDatabaseStmt");
}

TEST(AstVisitor, DispatchesDML) {
    EXPECT_EQ(visit_stmt("INSERT INTO t (c) VALUES (1)"), "InsertStmt");
    EXPECT_EQ(visit_stmt("UPDATE t SET c = 1"), "UpdateStmt");
    EXPECT_EQ(visit_stmt("DELETE FROM t WHERE id = 1"), "DeleteStmt");
    EXPECT_EQ(visit_stmt("LINK users(1) TO posts(2) VIA authored"), "LinkStmt");
    EXPECT_EQ(visit_stmt("UNLINK users(1) FROM posts(2) VIA authored"), "UnlinkStmt");
}

TEST(AstVisitor, DispatchesQuery) {
    EXPECT_EQ(visit_stmt("SELECT 1"), "SelectStmt");
    EXPECT_EQ(visit_stmt("TRAVERSE follows FROM users(1)"), "TraverseStmt");
    EXPECT_EQ(visit_stmt("MATCH (a:users)-[e:follows]->(b:users) RETURN a.name"), "MatchStmt");
    EXPECT_EQ(visit_stmt("SHORTEST PATH FROM users(1) TO users(2) VIA follows"),
              "ShortestPathStmt");
}

TEST(AstVisitor, DispatchesTCL) {
    EXPECT_EQ(visit_stmt("BEGIN"), "BeginStmt");
    EXPECT_EQ(visit_stmt("COMMIT"), "CommitStmt");
    EXPECT_EQ(visit_stmt("ROLLBACK"), "RollbackStmt");
    EXPECT_EQ(visit_stmt("SAVEPOINT sp1"), "SavepointStmt");
}

TEST(AstVisitor, DispatchesAdmin) {
    EXPECT_EQ(visit_stmt("SET param = 42"), "SetStmt");
    EXPECT_EQ(visit_stmt("SHOW TABLES"), "ShowStmt");
    EXPECT_EQ(visit_stmt("EXPLAIN SELECT 1"), "ExplainStmt");
    EXPECT_EQ(visit_stmt("DESCRIBE users"), "DescribeStmt");
    EXPECT_EQ(visit_stmt("BACKFILL EMBEDDINGS ON users"), "BackfillStmt");
    EXPECT_EQ(visit_stmt("REEMBED TABLE users"), "ReembedStmt");
    EXPECT_EQ(visit_stmt("VACUUM"), "VacuumStmt");
    EXPECT_EQ(visit_stmt("ANALYZE"), "AnalyzeStmt");
}

TEST(AstVisitor, DispatchesExpressions) {
    // Parse expressions via SELECT, then visit the select item's expression.
    TypeRecordingVisitor v;

    // LiteralExpr
    auto s1 = parse_one("SELECT 42");
    auto* sel1 = dynamic_cast<SelectStmt*>(s1.get());
    ASSERT_NE(sel1, nullptr);
    ASSERT_FALSE(sel1->items.empty());
    sel1->items[0].expr->accept(v);
    EXPECT_EQ(v.visited_type, "LiteralExpr");

    // ColumnRefExpr
    auto s2 = parse_one("SELECT name FROM t");
    auto* sel2 = dynamic_cast<SelectStmt*>(s2.get());
    ASSERT_NE(sel2, nullptr);
    sel2->items[0].expr->accept(v);
    EXPECT_EQ(v.visited_type, "ColumnRefExpr");

    // BinaryExpr
    auto s3 = parse_one("SELECT 1 + 2");
    auto* sel3 = dynamic_cast<SelectStmt*>(s3.get());
    ASSERT_NE(sel3, nullptr);
    sel3->items[0].expr->accept(v);
    EXPECT_EQ(v.visited_type, "BinaryExpr");

    // UnaryExpr
    auto s4 = parse_one("SELECT -1");
    auto* sel4 = dynamic_cast<SelectStmt*>(s4.get());
    ASSERT_NE(sel4, nullptr);
    sel4->items[0].expr->accept(v);
    EXPECT_EQ(v.visited_type, "UnaryExpr");

    // FunctionCallExpr
    auto s5 = parse_one("SELECT COUNT(*)");
    auto* sel5 = dynamic_cast<SelectStmt*>(s5.get());
    ASSERT_NE(sel5, nullptr);
    sel5->items[0].expr->accept(v);
    EXPECT_EQ(v.visited_type, "FunctionCallExpr");

    // ArrayExpr
    auto s6 = parse_one("SELECT [1, 2, 3]");
    auto* sel6 = dynamic_cast<SelectStmt*>(s6.get());
    ASSERT_NE(sel6, nullptr);
    sel6->items[0].expr->accept(v);
    EXPECT_EQ(v.visited_type, "ArrayExpr");
}

TEST(AstVisitor, DispatchesCastExpr) {
    TypeRecordingVisitor v;
    auto s = parse_one("SELECT x::INT FROM t");
    auto* sel = dynamic_cast<SelectStmt*>(s.get());
    ASSERT_NE(sel, nullptr);
    sel->items[0].expr->accept(v);
    EXPECT_EQ(v.visited_type, "CastExpr");
}

TEST(AstVisitor, DispatchesPredicateExprs) {
    TypeRecordingVisitor v;

    // IsNullExpr
    auto s1 = parse_one("SELECT x FROM t WHERE x IS NULL");
    auto* sel1 = dynamic_cast<SelectStmt*>(s1.get());
    ASSERT_NE(sel1, nullptr);
    sel1->where_expr->accept(v);
    EXPECT_EQ(v.visited_type, "IsNullExpr");

    // LikeExpr
    auto s2 = parse_one("SELECT x FROM t WHERE x LIKE 'a%'");
    auto* sel2 = dynamic_cast<SelectStmt*>(s2.get());
    ASSERT_NE(sel2, nullptr);
    sel2->where_expr->accept(v);
    EXPECT_EQ(v.visited_type, "LikeExpr");

    // BetweenExpr
    auto s3 = parse_one("SELECT x FROM t WHERE x BETWEEN 1 AND 10");
    auto* sel3 = dynamic_cast<SelectStmt*>(s3.get());
    ASSERT_NE(sel3, nullptr);
    sel3->where_expr->accept(v);
    EXPECT_EQ(v.visited_type, "BetweenExpr");

    // InExpr
    auto s4 = parse_one("SELECT x FROM t WHERE x IN (1, 2, 3)");
    auto* sel4 = dynamic_cast<SelectStmt*>(s4.get());
    ASSERT_NE(sel4, nullptr);
    sel4->where_expr->accept(v);
    EXPECT_EQ(v.visited_type, "InExpr");
}

// =============================================================================
// BACKFILL parser tests
// =============================================================================

TEST(ParseBackfill, BasicBackfill) {
    auto stmt = parse_one("BACKFILL EMBEDDINGS ON my_table");
    auto* backfill = dynamic_cast<BackfillStmt*>(stmt.get());
    ASSERT_NE(backfill, nullptr);
    EXPECT_EQ(backfill->table_name, "my_table");
    EXPECT_EQ(backfill->batch_size, 100u); // default
    EXPECT_EQ(backfill->rate_limit, 0u);   // default (unlimited)
}

TEST(ParseBackfill, WithBatchSize) {
    auto stmt = parse_one("BACKFILL EMBEDDINGS ON posts BATCH 500");
    auto* backfill = dynamic_cast<BackfillStmt*>(stmt.get());
    ASSERT_NE(backfill, nullptr);
    EXPECT_EQ(backfill->table_name, "posts");
    EXPECT_EQ(backfill->batch_size, 500u);
    EXPECT_EQ(backfill->rate_limit, 0u);
}

TEST(ParseBackfill, WithRateLimit) {
    auto stmt = parse_one("BACKFILL EMBEDDINGS ON posts RATE_LIMIT 1000");
    auto* backfill = dynamic_cast<BackfillStmt*>(stmt.get());
    ASSERT_NE(backfill, nullptr);
    EXPECT_EQ(backfill->table_name, "posts");
    EXPECT_EQ(backfill->batch_size, 100u);
    EXPECT_EQ(backfill->rate_limit, 1000u);
}

TEST(ParseBackfill, WithBothOptions) {
    auto stmt = parse_one("BACKFILL EMBEDDINGS ON posts BATCH 200 RATE_LIMIT 5000");
    auto* backfill = dynamic_cast<BackfillStmt*>(stmt.get());
    ASSERT_NE(backfill, nullptr);
    EXPECT_EQ(backfill->table_name, "posts");
    EXPECT_EQ(backfill->batch_size, 200u);
    EXPECT_EQ(backfill->rate_limit, 5000u);
}

TEST(ParseBackfill, MissingEmbeddingsKeyword) {
    Lexer lexer("BACKFILL ON posts");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(std::move(*tokens));
    auto result = parser.parse();
    EXPECT_FALSE(result.has_value());
}

TEST(ParseBackfill, MissingOnKeyword) {
    Lexer lexer("BACKFILL EMBEDDINGS posts");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(std::move(*tokens));
    auto result = parser.parse();
    EXPECT_FALSE(result.has_value());
}

TEST(ParseBackfill, ShowBackfillStatus) {
    auto stmt = parse_one("SHOW BACKFILL STATUS");
    auto* show = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(show, nullptr);
    EXPECT_EQ(show->target, ShowTarget::BACKFILL);
}

TEST(ParseBackfill, ShowBackfillWithoutStatus) {
    auto stmt = parse_one("SHOW BACKFILL");
    auto* show = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(show, nullptr);
    EXPECT_EQ(show->target, ShowTarget::BACKFILL);
}

// -- ParamRef ($N) tests -------------------------------------------------------

TEST(ParamRef, ParseSingleParam) {
    auto stmt = parse_one("SELECT name FROM users WHERE id = $1");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_NE(sel->where_expr, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(sel->where_expr.get());
    ASSERT_NE(bin, nullptr);
    auto* param = dynamic_cast<ParamRefExpr*>(bin->rhs.get());
    ASSERT_NE(param, nullptr);
    EXPECT_EQ(param->index, 1);
}

TEST(ParamRef, ParseMultipleParams) {
    auto stmt = parse_one("SELECT * FROM t WHERE a = $1 AND b = $2");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* and_expr = dynamic_cast<BinaryExpr*>(sel->where_expr.get());
    ASSERT_NE(and_expr, nullptr);
    auto* lhs_bin = dynamic_cast<BinaryExpr*>(and_expr->lhs.get());
    auto* rhs_bin = dynamic_cast<BinaryExpr*>(and_expr->rhs.get());
    ASSERT_NE(lhs_bin, nullptr);
    ASSERT_NE(rhs_bin, nullptr);
    auto* p1 = dynamic_cast<ParamRefExpr*>(lhs_bin->rhs.get());
    auto* p2 = dynamic_cast<ParamRefExpr*>(rhs_bin->rhs.get());
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(p1->index, 1);
    EXPECT_EQ(p2->index, 2);
}

TEST(ParamRef, LexerTokenizesParamRef) {
    Lexer lexer("$1 $23");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_GE(tokens->size(), 2u);
    EXPECT_EQ((*tokens)[0].type, TokenType::PARAM_REF);
    EXPECT_EQ((*tokens)[0].lexeme, "$1");
    EXPECT_EQ((*tokens)[1].type, TokenType::PARAM_REF);
    EXPECT_EQ((*tokens)[1].lexeme, "$23");
}

// -- SELECT without FROM -------------------------------------------------------

TEST(SelectWithoutFrom, ParseSelectLiteral) {
    auto stmt = parse_one("SELECT 1 AS val");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_TRUE(sel->from.empty());
    ASSERT_EQ(sel->items.size(), 1u);
    auto* lit = dynamic_cast<LiteralExpr*>(sel->items[0].expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::INTEGER);
    EXPECT_EQ(sel->items[0].alias, "val");
}

TEST(SelectWithoutFrom, ParseSelectStringLiteral) {
    auto stmt = parse_one("SELECT 'hello' AS greeting");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_TRUE(sel->from.empty());
    auto* lit = dynamic_cast<LiteralExpr*>(sel->items[0].expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::STRING);
}

// -- Graph / vector statements as subqueries ----------------------------------

TEST(SubqueryStmt, DerivedTableNearestRejected) {
    // Vector search is a WHERE predicate now; NEAREST is no longer a statement,
    // so it cannot be used as a derived table / subquery.
    expect_parse_error("SELECT * FROM (NEAREST 5 FROM docs.vec TO [1.0, 2.0]) AS n");
}

TEST(SubqueryStmt, InTraverse) {
    auto stmt = parse_one("SELECT * FROM users WHERE id IN (TRAVERSE follows FROM users(1))");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* in = dynamic_cast<InExpr*>(sel->where_expr.get());
    ASSERT_NE(in, nullptr);
    ASSERT_NE(in->subquery, nullptr);
    EXPECT_NE(dynamic_cast<TraverseStmt*>(in->subquery.get()), nullptr);
}

TEST(SubqueryStmt, ExistsNearestRejected) {
    // NEAREST is no longer a statement, so it cannot appear inside EXISTS(...).
    expect_parse_error("SELECT * FROM docs WHERE EXISTS (NEAREST 1 FROM docs.vec TO [1.0])");
}

TEST(SubqueryStmt, ScalarTraverse) {
    auto stmt = parse_one("SELECT (TRAVERSE follows FROM users(1)) FROM users");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_FALSE(sel->items.empty());
    auto* sub = dynamic_cast<SubqueryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(sub, nullptr);
    EXPECT_NE(dynamic_cast<TraverseStmt*>(sub->subquery.get()), nullptr);
}

TEST(SubqueryStmt, MatchDerivedTable) {
    auto stmt =
        parse_one("SELECT * FROM (MATCH (a:users)-[r:follows]->(b:users) RETURN a.id) AS m");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    ASSERT_NE(sel->from[0].subquery, nullptr);
    EXPECT_NE(dynamic_cast<MatchStmt*>(sel->from[0].subquery.get()), nullptr);
}

TEST(SubqueryStmt, NonQueryStatementInExistsIsError) {
    // EXISTS ( must be followed by a query statement, not an arbitrary expression.
    expect_parse_error("SELECT * FROM docs WHERE EXISTS (1 + 1)");
}
