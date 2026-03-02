/// @file test_qa_gdb_103.cpp
/// QA adversarial tests for GDB-103: Recursive descent parser for DDL statements.
///
/// Tests cover: edge cases, boundary values, error paths, constraint
/// combinations, type spec parsing, and error recovery for all DDL statements.

#include "giodb/parser/lexer.h"
#include "giodb/parser/parser.h"

#include <gtest/gtest.h>

using namespace giodb;

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

static Result<std::vector<StmtPtr>> parse_result(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return tl::unexpected(tokens.error());
    Parser parser(std::move(*tokens));
    return parser.parse_all();
}

// =============================================================================
// CREATE TABLE edge cases
// =============================================================================

TEST(QA_GDB103, CreateTableEmptyColumnList) {
    // CREATE TABLE t () — empty table definition
    // Most SQL engines reject this; verify parser behavior.
    auto result = parse_result("CREATE TABLE t ()");
    if (result.has_value()) {
        // If parser allows it, verify the table has no columns.
        ASSERT_EQ(result->size(), 1u);
        auto* ct = dynamic_cast<CreateTableStmt*>((*result)[0].get());
        ASSERT_NE(ct, nullptr);
        EXPECT_TRUE(ct->columns.empty());
        EXPECT_TRUE(ct->constraints.empty());
    }
    // Either error or empty table is acceptable parser behavior;
    // semantic validation should catch empty tables.
}

TEST(QA_GDB103, CreateTableTrailingComma) {
    // Trailing comma before closing paren should be a parse error.
    expect_parse_error("CREATE TABLE t (a INT,)");
}

TEST(QA_GDB103, CreateTableOnlyConstraints) {
    // Table with only a constraint and no column definitions.
    auto result = parse_result("CREATE TABLE t (PRIMARY KEY(a))");
    if (result.has_value()) {
        auto* ct = dynamic_cast<CreateTableStmt*>((*result)[0].get());
        ASSERT_NE(ct, nullptr);
        EXPECT_TRUE(ct->columns.empty());
        EXPECT_EQ(ct->constraints.size(), 1u);
        EXPECT_EQ(ct->constraints[0].kind, TableConstraint::Kind::PRIMARY_KEY);
    }
}

TEST(QA_GDB103, CreateTableMissingEverything) {
    expect_parse_error("CREATE TABLE");
}

TEST(QA_GDB103, CreateTableUnclosedParen) {
    expect_parse_error("CREATE TABLE t (a INT");
}

TEST(QA_GDB103, CreateTableMissingColumnType) {
    expect_parse_error("CREATE TABLE t (a)");
}

TEST(QA_GDB103, CreateTableDuplicateColumnConstraints) {
    // Multiple NOT NULL on same column — parser should accept (semantics checks).
    auto stmt = parse_one("CREATE TABLE t (a INT NOT NULL NOT NULL)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_FALSE(ct->columns[0].nullable);
}

TEST(QA_GDB103, CreateTableNullThenNotNull) {
    // NULL followed by NOT NULL — last one wins.
    auto stmt = parse_one("CREATE TABLE t (a INT NULL NOT NULL)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_FALSE(ct->columns[0].nullable);
}

TEST(QA_GDB103, CreateTableNotNullThenNull) {
    // NOT NULL followed by NULL — last one wins.
    auto stmt = parse_one("CREATE TABLE t (a INT NOT NULL NULL)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_TRUE(ct->columns[0].nullable);
}

TEST(QA_GDB103, CreateTableMultiplePrimaryKeysInline) {
    // Two columns each with PRIMARY KEY — parser should accept
    // (semantic validation catches this).
    auto stmt = parse_one("CREATE TABLE t (a INT PRIMARY KEY, b INT PRIMARY KEY)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->columns.size(), 2u);
    EXPECT_FALSE(ct->columns[0].nullable); // PRIMARY KEY -> NOT NULL
    EXPECT_FALSE(ct->columns[1].nullable);
}

TEST(QA_GDB103, CreateTableAllConstraintTypes) {
    // Column with every constraint type in one definition.
    auto stmt = parse_one(
        "CREATE TABLE t (a INT NOT NULL UNIQUE DEFAULT 42 CHECK(a > 0) "
        "REFERENCES other(id) ON DELETE CASCADE)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->columns.size(), 1u);
    auto& col = ct->columns[0];
    EXPECT_FALSE(col.nullable);
    EXPECT_TRUE(col.is_unique);
    EXPECT_NE(col.default_expr, nullptr);
    EXPECT_NE(col.check_expr, nullptr);
    EXPECT_EQ(col.fk_table, "other");
    EXPECT_EQ(col.fk_column, "id");
    EXPECT_EQ(col.fk_on_delete, ReferentialAction::CASCADE);
}

TEST(QA_GDB103, CreateTableForeignKeyOnDeleteRestrict) {
    auto stmt = parse_one(
        "CREATE TABLE t (a INT REFERENCES other(id) ON DELETE RESTRICT)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->columns[0].fk_on_delete, ReferentialAction::RESTRICT);
}

TEST(QA_GDB103, CreateTableForeignKeyNoOnDelete) {
    // Without ON DELETE, default should be RESTRICT.
    auto stmt = parse_one("CREATE TABLE t (a INT REFERENCES other(id))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->columns[0].fk_on_delete, ReferentialAction::RESTRICT);
}

TEST(QA_GDB103, CreateTableForeignKeyOnDeleteInvalid) {
    // ON DELETE with invalid action.
    expect_parse_error(
        "CREATE TABLE t (a INT REFERENCES other(id) ON DELETE FROBNICATE)");
}

TEST(QA_GDB103, CreateTableForeignKeyOnMissingDelete) {
    // ON without DELETE.
    expect_parse_error("CREATE TABLE t (a INT REFERENCES other(id) ON UPDATE CASCADE)");
}

TEST(QA_GDB103, CreateTableConstraintForeignKey) {
    // Table-level FOREIGN KEY constraint.
    auto stmt = parse_one(
        "CREATE TABLE t (a INT, FOREIGN KEY(a) REFERENCES other(id) ON DELETE CASCADE)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->constraints.size(), 1u);
    EXPECT_EQ(ct->constraints[0].kind, TableConstraint::Kind::FOREIGN_KEY);
    EXPECT_EQ(ct->constraints[0].fk_table, "other");
    EXPECT_EQ(ct->constraints[0].on_delete, ReferentialAction::CASCADE);
}

TEST(QA_GDB103, CreateTableNamedConstraint) {
    // CONSTRAINT name PRIMARY KEY(...)
    auto stmt = parse_one(
        "CREATE TABLE t (a INT, CONSTRAINT pk_t PRIMARY KEY(a))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->constraints.size(), 1u);
    EXPECT_EQ(ct->constraints[0].name, "pk_t");
    EXPECT_EQ(ct->constraints[0].kind, TableConstraint::Kind::PRIMARY_KEY);
}

TEST(QA_GDB103, CreateTableNamedConstraintFollowedByInvalid) {
    // CONSTRAINT name followed by neither PRIMARY/UNIQUE/CHECK/FOREIGN.
    expect_parse_error("CREATE TABLE t (a INT, CONSTRAINT foo FROBNICATE)");
}

TEST(QA_GDB103, CreateTableCheckConstraintTable) {
    // Table-level CHECK constraint.
    auto stmt = parse_one(
        "CREATE TABLE t (a INT, b INT, CHECK(a > b))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->constraints.size(), 1u);
    EXPECT_EQ(ct->constraints[0].kind, TableConstraint::Kind::CHECK);
    EXPECT_NE(ct->constraints[0].check_expr, nullptr);
}

TEST(QA_GDB103, CreateTableUniqueConstraint) {
    // Table-level UNIQUE constraint with multiple columns.
    auto stmt = parse_one(
        "CREATE TABLE t (a INT, b INT, UNIQUE(a, b))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->constraints.size(), 1u);
    EXPECT_EQ(ct->constraints[0].kind, TableConstraint::Kind::UNIQUE);
    EXPECT_EQ(ct->constraints[0].columns.size(), 2u);
}

TEST(QA_GDB103, CreateTableMultipleMixedConstraints) {
    // Mix of column-level and table-level constraints.
    auto stmt = parse_one(
        "CREATE TABLE t ("
        "  a INT NOT NULL UNIQUE,"
        "  b INT CHECK(b > 0),"
        "  c INT REFERENCES other(id),"
        "  PRIMARY KEY(a),"
        "  FOREIGN KEY(c) REFERENCES other(id) ON DELETE CASCADE,"
        "  UNIQUE(a, b)"
        ")");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->columns.size(), 3u);
    EXPECT_EQ(ct->constraints.size(), 3u);
}

// =============================================================================
// Type spec edge cases
// =============================================================================

TEST(QA_GDB103, TypeSpecAllBaseTypes) {
    // Verify all 22 GioDB base types parse correctly.
    const char* types[] = {
        "INT",       "INTEGER",   "TINYINT",   "SMALLINT", "BIGINT",
        "FLOAT",     "DOUBLE",    "DECIMAL",   "NUMERIC",  "BOOLEAN",
        "CHAR",      "VARCHAR",   "TEXT",      "BLOB",     "DATE",
        "TIME",      "TIMESTAMP", "INTERVAL",  "POINT",    "JSON",
        "UUID",
    };
    for (const char* type_name : types) {
        std::string sql = std::string("CREATE TABLE t (a ") + type_name + ")";
        auto stmt = parse_one(sql);
        auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
        ASSERT_NE(ct, nullptr) << "Failed for type: " << type_name;
        EXPECT_EQ(ct->columns.size(), 1u) << "Failed for type: " << type_name;
    }
}

TEST(QA_GDB103, TypeSpecDecimalWithPrecisionAndScale) {
    auto stmt = parse_one("CREATE TABLE t (a DECIMAL(10, 2))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->columns[0].type.name, "DECIMAL");
    EXPECT_EQ(ct->columns[0].type.param1, 10);
    EXPECT_EQ(ct->columns[0].type.param2, 2);
}

TEST(QA_GDB103, TypeSpecDecimalPrecisionOnly) {
    auto stmt = parse_one("CREATE TABLE t (a DECIMAL(18))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->columns[0].type.param1, 18);
    EXPECT_FALSE(ct->columns[0].type.param2.has_value());
}

TEST(QA_GDB103, TypeSpecVarcharWithLength) {
    auto stmt = parse_one("CREATE TABLE t (a VARCHAR(255))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->columns[0].type.name, "VARCHAR");
    EXPECT_EQ(ct->columns[0].type.param1, 255);
}

TEST(QA_GDB103, TypeSpecEmbeddingFull) {
    auto stmt = parse_one(
        "CREATE TABLE t (a EMBEDDING(384, content, 'openai'))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->columns[0].type.name, "EMBEDDING");
    EXPECT_EQ(ct->columns[0].type.param1, 384);
    EXPECT_EQ(ct->columns[0].type.source, "content");
    EXPECT_EQ(ct->columns[0].type.provider, "openai");
}

TEST(QA_GDB103, TypeSpecEmbeddingMissingProvider) {
    // EMBEDDING(384, col) without provider should error.
    expect_parse_error("CREATE TABLE t (a EMBEDDING(384, content))");
}

TEST(QA_GDB103, TypeSpecEmbeddingMissingSource) {
    // EMBEDDING(384) without source or provider should error.
    expect_parse_error("CREATE TABLE t (a EMBEDDING(384))");
}

TEST(QA_GDB103, TypeSpecEmbeddingMissingDimension) {
    // EMBEDDING() with no arguments.
    expect_parse_error("CREATE TABLE t (a EMBEDDING())");
}

TEST(QA_GDB103, TypeSpecEmbeddingWithoutParens) {
    // EMBEDDING without parentheses — should parse as plain type name
    // or error depending on implementation.
    auto result = parse_result("CREATE TABLE t (a EMBEDDING)");
    // EMBEDDING without parens might be valid (like VARCHAR without length).
    if (result.has_value()) {
        auto* ct = dynamic_cast<CreateTableStmt*>((*result)[0].get());
        ASSERT_NE(ct, nullptr);
        EXPECT_EQ(ct->columns[0].type.name, "EMBEDDING");
        EXPECT_FALSE(ct->columns[0].type.param1.has_value());
    }
}

TEST(QA_GDB103, TypeSpecOverflowInteger) {
    // Integer overflow in type parameter.
    expect_parse_error("CREATE TABLE t (a VARCHAR(99999999999999))");
}

TEST(QA_GDB103, TypeSpecZeroPrecision) {
    // DECIMAL(0) — parser should accept, semantics validates.
    auto stmt = parse_one("CREATE TABLE t (a DECIMAL(0))");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->columns[0].type.param1, 0);
}

TEST(QA_GDB103, TypeSpecCaseInsensitive) {
    // Types should be case-insensitive.
    auto stmt = parse_one("CREATE TABLE t (a int, b Varchar(10), c Boolean)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->columns.size(), 3u);
    // Normalized to uppercase.
    EXPECT_EQ(ct->columns[0].type.name, "INT");
    EXPECT_EQ(ct->columns[1].type.name, "VARCHAR");
    EXPECT_EQ(ct->columns[2].type.name, "BOOLEAN");
}

TEST(QA_GDB103, TypeSpecInvalidTypeName) {
    expect_parse_error("CREATE TABLE t (a FROBNICATE)");
}

// =============================================================================
// DROP TABLE edge cases
// =============================================================================

TEST(QA_GDB103, DropTableBasic) {
    auto stmt = parse_one("DROP TABLE users");
    auto* dt = dynamic_cast<DropTableStmt*>(stmt.get());
    ASSERT_NE(dt, nullptr);
    EXPECT_EQ(dt->name, "users");
    EXPECT_FALSE(dt->if_exists);
    EXPECT_FALSE(dt->cascade);
}

TEST(QA_GDB103, DropTableIfExistsCascade) {
    auto stmt = parse_one("DROP TABLE IF EXISTS users CASCADE");
    auto* dt = dynamic_cast<DropTableStmt*>(stmt.get());
    ASSERT_NE(dt, nullptr);
    EXPECT_TRUE(dt->if_exists);
    EXPECT_TRUE(dt->cascade);
}

TEST(QA_GDB103, DropTableRestrict) {
    auto stmt = parse_one("DROP TABLE users RESTRICT");
    auto* dt = dynamic_cast<DropTableStmt*>(stmt.get());
    ASSERT_NE(dt, nullptr);
    EXPECT_FALSE(dt->cascade);
}

TEST(QA_GDB103, DropTableMissingName) {
    expect_parse_error("DROP TABLE");
}

TEST(QA_GDB103, DropTableIfMissingExists) {
    expect_parse_error("DROP TABLE IF users");
}

// =============================================================================
// ALTER TABLE edge cases
// =============================================================================

TEST(QA_GDB103, AlterTableAddColumnFull) {
    auto stmt = parse_one(
        "ALTER TABLE users ADD COLUMN email VARCHAR(255) NOT NULL");
    auto* at = dynamic_cast<AlterTableStmt*>(stmt.get());
    ASSERT_NE(at, nullptr);
    EXPECT_EQ(at->table_name, "users");
    EXPECT_EQ(at->action, AlterAction::ADD_COLUMN);
    EXPECT_EQ(at->column.name, "email");
    EXPECT_EQ(at->column.type.name, "VARCHAR");
    EXPECT_FALSE(at->column.nullable);
}

TEST(QA_GDB103, AlterTableAddWithoutColumn) {
    // ADD without COLUMN keyword should still work (COLUMN is optional).
    auto stmt = parse_one("ALTER TABLE users ADD age INT");
    auto* at = dynamic_cast<AlterTableStmt*>(stmt.get());
    ASSERT_NE(at, nullptr);
    EXPECT_EQ(at->action, AlterAction::ADD_COLUMN);
    EXPECT_EQ(at->column.name, "age");
}

TEST(QA_GDB103, AlterTableDropColumn) {
    auto stmt = parse_one("ALTER TABLE users DROP COLUMN email");
    auto* at = dynamic_cast<AlterTableStmt*>(stmt.get());
    ASSERT_NE(at, nullptr);
    EXPECT_EQ(at->action, AlterAction::DROP_COLUMN);
    EXPECT_EQ(at->column_name, "email");
}

TEST(QA_GDB103, AlterTableDropWithoutColumn) {
    auto stmt = parse_one("ALTER TABLE users DROP email");
    auto* at = dynamic_cast<AlterTableStmt*>(stmt.get());
    ASSERT_NE(at, nullptr);
    EXPECT_EQ(at->action, AlterAction::DROP_COLUMN);
    EXPECT_EQ(at->column_name, "email");
}

TEST(QA_GDB103, AlterTableRenameColumn) {
    auto stmt = parse_one("ALTER TABLE users RENAME COLUMN name TO full_name");
    auto* at = dynamic_cast<AlterTableStmt*>(stmt.get());
    ASSERT_NE(at, nullptr);
    EXPECT_EQ(at->action, AlterAction::RENAME_COLUMN);
    EXPECT_EQ(at->column_name, "name");
    EXPECT_EQ(at->new_column_name, "full_name");
}

TEST(QA_GDB103, AlterTableRenameWithoutColumn) {
    auto stmt = parse_one("ALTER TABLE users RENAME name TO full_name");
    auto* at = dynamic_cast<AlterTableStmt*>(stmt.get());
    ASSERT_NE(at, nullptr);
    EXPECT_EQ(at->action, AlterAction::RENAME_COLUMN);
    EXPECT_EQ(at->column_name, "name");
    EXPECT_EQ(at->new_column_name, "full_name");
}

TEST(QA_GDB103, AlterTableInvalidAction) {
    expect_parse_error("ALTER TABLE users FROBNICATE");
}

TEST(QA_GDB103, AlterTableMissingTableName) {
    expect_parse_error("ALTER TABLE");
}

TEST(QA_GDB103, AlterTableRenameMissingTo) {
    expect_parse_error("ALTER TABLE users RENAME old_col new_col");
}

TEST(QA_GDB103, AlterTableRenameMissingNewName) {
    expect_parse_error("ALTER TABLE users RENAME old_col TO");
}

TEST(QA_GDB103, AlterTableAddMissingType) {
    expect_parse_error("ALTER TABLE users ADD col");
}

// =============================================================================
// CREATE INDEX edge cases
// =============================================================================

TEST(QA_GDB103, CreateIndexBasic) {
    auto stmt = parse_one("CREATE INDEX idx_name ON users(email)");
    auto* ci = dynamic_cast<CreateIndexStmt*>(stmt.get());
    ASSERT_NE(ci, nullptr);
    EXPECT_EQ(ci->name, "idx_name");
    EXPECT_EQ(ci->table_name, "users");
    EXPECT_EQ(ci->columns.size(), 1u);
    EXPECT_EQ(ci->columns[0], "email");
    EXPECT_FALSE(ci->is_unique);
    EXPECT_TRUE(ci->method.empty());
}

TEST(QA_GDB103, CreateUniqueIndex) {
    auto stmt = parse_one("CREATE UNIQUE INDEX idx ON users(email)");
    auto* ci = dynamic_cast<CreateIndexStmt*>(stmt.get());
    ASSERT_NE(ci, nullptr);
    EXPECT_TRUE(ci->is_unique);
}

TEST(QA_GDB103, CreateIndexMultiColumn) {
    auto stmt = parse_one("CREATE INDEX idx ON users(first, last, email)");
    auto* ci = dynamic_cast<CreateIndexStmt*>(stmt.get());
    ASSERT_NE(ci, nullptr);
    EXPECT_EQ(ci->columns.size(), 3u);
}

TEST(QA_GDB103, CreateIndexUsing) {
    auto stmt = parse_one("CREATE INDEX idx ON users(email) USING btree");
    auto* ci = dynamic_cast<CreateIndexStmt*>(stmt.get());
    ASSERT_NE(ci, nullptr);
    EXPECT_EQ(ci->method, "btree");
}

TEST(QA_GDB103, CreateIndexIfNotExists) {
    auto stmt = parse_one("CREATE INDEX IF NOT EXISTS idx ON users(email)");
    auto* ci = dynamic_cast<CreateIndexStmt*>(stmt.get());
    ASSERT_NE(ci, nullptr);
    EXPECT_TRUE(ci->if_not_exists);
}

TEST(QA_GDB103, CreateIndexMissingOn) {
    expect_parse_error("CREATE INDEX idx users(email)");
}

TEST(QA_GDB103, CreateIndexMissingColumns) {
    expect_parse_error("CREATE INDEX idx ON users");
}

TEST(QA_GDB103, CreateIndexEmptyColumns) {
    expect_parse_error("CREATE INDEX idx ON users()");
}

TEST(QA_GDB103, DropIndexBasic) {
    auto stmt = parse_one("DROP INDEX idx_name");
    auto* di = dynamic_cast<DropIndexStmt*>(stmt.get());
    ASSERT_NE(di, nullptr);
    EXPECT_EQ(di->name, "idx_name");
    EXPECT_FALSE(di->if_exists);
}

TEST(QA_GDB103, DropIndexIfExists) {
    auto stmt = parse_one("DROP INDEX IF EXISTS idx_name");
    auto* di = dynamic_cast<DropIndexStmt*>(stmt.get());
    ASSERT_NE(di, nullptr);
    EXPECT_TRUE(di->if_exists);
}

TEST(QA_GDB103, DropIndexMissingName) {
    expect_parse_error("DROP INDEX");
}

// =============================================================================
// CREATE/DROP EDGE TYPE edge cases
// =============================================================================

TEST(QA_GDB103, CreateEdgeTypeBasic) {
    auto stmt = parse_one("CREATE EDGE TYPE follows FROM users TO users");
    auto* ce = dynamic_cast<CreateEdgeTypeStmt*>(stmt.get());
    ASSERT_NE(ce, nullptr);
    EXPECT_EQ(ce->name, "follows");
    EXPECT_TRUE(ce->properties.empty());
    EXPECT_EQ(ce->from_table, "users");
    EXPECT_EQ(ce->to_table, "users");
}

TEST(QA_GDB103, CreateEdgeTypeWithProperties) {
    auto stmt = parse_one(
        "CREATE EDGE TYPE rated (score FLOAT, review TEXT) FROM users TO products");
    auto* ce = dynamic_cast<CreateEdgeTypeStmt*>(stmt.get());
    ASSERT_NE(ce, nullptr);
    EXPECT_EQ(ce->properties.size(), 2u);
    EXPECT_EQ(ce->properties[0].name, "score");
    EXPECT_EQ(ce->properties[0].type.name, "FLOAT");
    EXPECT_EQ(ce->properties[1].name, "review");
    EXPECT_EQ(ce->from_table, "users");
    EXPECT_EQ(ce->to_table, "products");
}

TEST(QA_GDB103, CreateEdgeTypeEmptyProperties) {
    // Empty property list: CREATE EDGE TYPE e () FROM a TO b.
    auto stmt = parse_one("CREATE EDGE TYPE follows () FROM users TO users");
    auto* ce = dynamic_cast<CreateEdgeTypeStmt*>(stmt.get());
    ASSERT_NE(ce, nullptr);
    EXPECT_TRUE(ce->properties.empty());
}

TEST(QA_GDB103, CreateEdgeTypeMissingFrom) {
    expect_parse_error("CREATE EDGE TYPE follows TO users");
}

TEST(QA_GDB103, CreateEdgeTypeMissingTo) {
    expect_parse_error("CREATE EDGE TYPE follows FROM users");
}

TEST(QA_GDB103, CreateEdgeTypeMissingType) {
    expect_parse_error("CREATE EDGE follows FROM users TO users");
}

TEST(QA_GDB103, DropEdgeTypeBasic) {
    auto stmt = parse_one("DROP EDGE TYPE follows");
    auto* de = dynamic_cast<DropEdgeTypeStmt*>(stmt.get());
    ASSERT_NE(de, nullptr);
    EXPECT_EQ(de->name, "follows");
    EXPECT_FALSE(de->if_exists);
}

TEST(QA_GDB103, DropEdgeTypeIfExists) {
    auto stmt = parse_one("DROP EDGE TYPE IF EXISTS follows");
    auto* de = dynamic_cast<DropEdgeTypeStmt*>(stmt.get());
    ASSERT_NE(de, nullptr);
    EXPECT_TRUE(de->if_exists);
}

TEST(QA_GDB103, DropEdgeTypeMissingName) {
    expect_parse_error("DROP EDGE TYPE");
}

// =============================================================================
// CREATE/DROP DATABASE edge cases
// =============================================================================

TEST(QA_GDB103, CreateDatabaseBasic) {
    auto stmt = parse_one("CREATE DATABASE mydb");
    auto* cd = dynamic_cast<CreateDatabaseStmt*>(stmt.get());
    ASSERT_NE(cd, nullptr);
    EXPECT_EQ(cd->database_name, "mydb");
    EXPECT_FALSE(cd->if_not_exists);
}

TEST(QA_GDB103, CreateDatabaseIfNotExists) {
    auto stmt = parse_one("CREATE DATABASE IF NOT EXISTS mydb");
    auto* cd = dynamic_cast<CreateDatabaseStmt*>(stmt.get());
    ASSERT_NE(cd, nullptr);
    EXPECT_TRUE(cd->if_not_exists);
}

TEST(QA_GDB103, CreateDatabaseMissingName) {
    expect_parse_error("CREATE DATABASE");
}

TEST(QA_GDB103, DropDatabaseBasic) {
    auto stmt = parse_one("DROP DATABASE mydb");
    auto* dd = dynamic_cast<DropDatabaseStmt*>(stmt.get());
    ASSERT_NE(dd, nullptr);
    EXPECT_EQ(dd->database_name, "mydb");
}

TEST(QA_GDB103, DropDatabaseCascade) {
    auto stmt = parse_one("DROP DATABASE mydb CASCADE");
    auto* dd = dynamic_cast<DropDatabaseStmt*>(stmt.get());
    ASSERT_NE(dd, nullptr);
    EXPECT_TRUE(dd->cascade);
}

TEST(QA_GDB103, DropDatabaseMissingName) {
    expect_parse_error("DROP DATABASE");
}

// =============================================================================
// Error recovery
// =============================================================================

TEST(QA_GDB103, ErrorRecoveryMultipleErrors) {
    // Multiple invalid statements — error recovery should collect all errors.
    auto result = parse_result(
        "CREATE TABLE; DROP TABLE; ALTER TABLE");
    ASSERT_FALSE(result.has_value());
    // Error message should mention count.
    auto msg = result.error().message;
    EXPECT_NE(msg.find("parse error"), std::string::npos);
}

TEST(QA_GDB103, ErrorRecoveryValidAmongInvalid) {
    // Valid statement mixed with invalid ones — parse_all returns error.
    auto result = parse_result(
        "CREATE TABLE t (a INT); DROP TABLE; CREATE TABLE u (b INT)");
    // parse_all returns error if ANY statement fails.
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB103, ErrorMessageContainsLineColumn) {
    auto result = parse_result("CREATE TABLE t (a)");
    ASSERT_FALSE(result.has_value());
    auto msg = result.error().message;
    EXPECT_NE(msg.find("line"), std::string::npos);
    EXPECT_NE(msg.find("column"), std::string::npos);
}

TEST(QA_GDB103, ParseMultipleValidDDL) {
    // Multiple valid DDL statements separated by semicolons.
    auto stmts = parse_ok(
        "CREATE TABLE t (a INT);"
        "DROP TABLE t;"
        "CREATE INDEX idx ON t(a);"
        "DROP INDEX idx;"
        "CREATE EDGE TYPE e FROM t TO t;"
        "DROP EDGE TYPE e");
    EXPECT_EQ(stmts.size(), 6u);
}

TEST(QA_GDB103, ParseStraySemicolons) {
    // Stray semicolons between statements should be skipped.
    auto stmts = parse_ok(";;; CREATE TABLE t (a INT) ;;; DROP TABLE t ;;;");
    EXPECT_EQ(stmts.size(), 2u);
}

// =============================================================================
// Keyword as identifier tests
// =============================================================================

TEST(QA_GDB103, KeywordAsTableNameInCreateTable) {
    // Keywords that are in is_name_token should work as table names.
    auto stmt = parse_one("CREATE TABLE index (id INT)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->name, "index");
}

TEST(QA_GDB103, TypeKeywordAsColumnName) {
    // Type keywords should be usable as column names.
    auto stmt = parse_one("CREATE TABLE t (int INT, varchar TEXT)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->columns[0].name, "int");
    EXPECT_EQ(ct->columns[1].name, "varchar");
}

TEST(QA_GDB103, KeywordAsEdgeTypeName) {
    auto stmt = parse_one("CREATE EDGE TYPE index FROM t TO t");
    auto* ce = dynamic_cast<CreateEdgeTypeStmt*>(stmt.get());
    ASSERT_NE(ce, nullptr);
    EXPECT_EQ(ce->name, "index");
}

// =============================================================================
// IF NOT EXISTS / IF EXISTS edge cases
// =============================================================================

TEST(QA_GDB103, CreateTableIfNotExistsPartiallyTyped) {
    // IF without NOT should error.
    expect_parse_error("CREATE TABLE IF t (a INT)");
}

TEST(QA_GDB103, CreateTableIfNotWithoutExists) {
    // IF NOT without EXISTS should error.
    expect_parse_error("CREATE TABLE IF NOT t (a INT)");
}

TEST(QA_GDB103, DropTableIfWithoutExists) {
    expect_parse_error("DROP TABLE IF t");
}

TEST(QA_GDB103, CreateIndexIfNotExistsPartial) {
    expect_parse_error("CREATE INDEX IF idx ON t(a)");
}

// =============================================================================
// Stress / adversarial
// =============================================================================

TEST(QA_GDB103, CreateTableManyColumns) {
    // Table with many columns.
    std::string sql = "CREATE TABLE big (";
    for (int i = 0; i < 100; ++i) {
        if (i > 0)
            sql += ", ";
        sql += "col" + std::to_string(i) + " INT";
    }
    sql += ")";
    auto stmt = parse_one(sql);
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->columns.size(), 100u);
}

TEST(QA_GDB103, CreateIndexManyColumns) {
    // Index with many columns.
    std::string sql = "CREATE INDEX idx ON t(";
    for (int i = 0; i < 50; ++i) {
        if (i > 0)
            sql += ", ";
        sql += "col" + std::to_string(i);
    }
    sql += ")";
    auto stmt = parse_one(sql);
    auto* ci = dynamic_cast<CreateIndexStmt*>(stmt.get());
    ASSERT_NE(ci, nullptr);
    EXPECT_EQ(ci->columns.size(), 50u);
}

TEST(QA_GDB103, CreateTableLongIdentifier) {
    // Very long identifier.
    std::string long_name(1000, 'a');
    std::string sql = "CREATE TABLE " + long_name + " (id INT)";
    auto stmt = parse_one(sql);
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->name, long_name);
}

TEST(QA_GDB103, ManyStatements) {
    // Many DDL statements in sequence.
    std::string sql;
    for (int i = 0; i < 100; ++i) {
        sql += "CREATE TABLE t" + std::to_string(i) + " (id INT); ";
    }
    auto stmts = parse_ok(sql);
    EXPECT_EQ(stmts.size(), 100u);
}

// =============================================================================
// parse() single-statement API
// =============================================================================

TEST(QA_GDB103, ParseSingleStatement) {
    Lexer lexer("CREATE TABLE t (a INT)");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(std::move(*tokens));
    auto stmt = parser.parse();
    ASSERT_TRUE(stmt.has_value());
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt->get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->name, "t");
}

TEST(QA_GDB103, ParseSingleStatementWithSemicolon) {
    Lexer lexer("CREATE TABLE t (a INT);");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(std::move(*tokens));
    auto stmt = parser.parse();
    ASSERT_TRUE(stmt.has_value());
}

TEST(QA_GDB103, ParseEmptyInput) {
    Lexer lexer("");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(std::move(*tokens));
    auto stmt = parser.parse();
    EXPECT_FALSE(stmt.has_value());
}

// =============================================================================
// CREATE/DROP/ALTER USER
// =============================================================================

TEST(QA_GDB103, CreateUserBasic) {
    auto stmt = parse_one("CREATE USER admin WITH PASSWORD 'secret123'");
    auto* cu = dynamic_cast<CreateUserStmt*>(stmt.get());
    ASSERT_NE(cu, nullptr);
    EXPECT_EQ(cu->username, "admin");
    EXPECT_EQ(cu->password, "secret123");
}

TEST(QA_GDB103, DropUserBasic) {
    auto stmt = parse_one("DROP USER admin");
    auto* du = dynamic_cast<DropUserStmt*>(stmt.get());
    ASSERT_NE(du, nullptr);
    EXPECT_EQ(du->username, "admin");
}

TEST(QA_GDB103, DropUserIfExists) {
    auto stmt = parse_one("DROP USER IF EXISTS admin");
    auto* du = dynamic_cast<DropUserStmt*>(stmt.get());
    ASSERT_NE(du, nullptr);
    EXPECT_TRUE(du->if_exists);
}

TEST(QA_GDB103, AlterUserPassword) {
    auto stmt = parse_one("ALTER USER admin WITH PASSWORD 'newpass'");
    auto* au = dynamic_cast<AlterUserStmt*>(stmt.get());
    ASSERT_NE(au, nullptr);
    EXPECT_EQ(au->username, "admin");
    EXPECT_EQ(au->password, "newpass");
}

// =============================================================================
// CREATE dispatch edge cases
// =============================================================================

TEST(QA_GDB103, CreateUnknownObject) {
    expect_parse_error("CREATE FROBNICATE foo");
}

TEST(QA_GDB103, DropUnknownObject) {
    expect_parse_error("DROP FROBNICATE foo");
}

TEST(QA_GDB103, AlterUnknownObject) {
    expect_parse_error("ALTER FROBNICATE foo");
}

TEST(QA_GDB103, CreateEdgeMissingType) {
    // CREATE EDGE without TYPE.
    expect_parse_error("CREATE EDGE follows FROM t TO t");
}

TEST(QA_GDB103, DropEdgeMissingType) {
    expect_parse_error("DROP EDGE follows");
}

// =============================================================================
// Default expression in column definition
// =============================================================================

TEST(QA_GDB103, DefaultExpressionInteger) {
    auto stmt = parse_one("CREATE TABLE t (a INT DEFAULT 42)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(ct->columns[0].default_expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::INTEGER);
    EXPECT_EQ(lit->value, "42");
}

TEST(QA_GDB103, DefaultExpressionString) {
    auto stmt = parse_one("CREATE TABLE t (a TEXT DEFAULT 'hello')");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(ct->columns[0].default_expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::STRING);
}

TEST(QA_GDB103, DefaultExpressionNull) {
    auto stmt = parse_one("CREATE TABLE t (a INT DEFAULT NULL)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(ct->columns[0].default_expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::NULL_LITERAL);
}

TEST(QA_GDB103, DefaultExpressionBoolean) {
    auto stmt = parse_one("CREATE TABLE t (a BOOLEAN DEFAULT TRUE)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(ct->columns[0].default_expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::BOOLEAN);
}
