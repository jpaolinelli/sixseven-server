#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

namespace sixseven {
namespace {

// ============================================================================
// Adversarial QA tests for multi-ticket bug fix branch
// Covers: GDB-257, GDB-261, GDB-300/301/302, GDB-303, GDB-308
// ============================================================================

// ---------------------------------------------------------------------------
// Helper: parse-only tests (no execution needed)
// ---------------------------------------------------------------------------

static StmtPtr parse_one(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    EXPECT_TRUE(tokens.has_value()) << tokens.error().message;
    if (!tokens)
        return nullptr;
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_TRUE(stmts.has_value()) << sql << ": " << stmts.error().message;
    if (!stmts || stmts->empty())
        return nullptr;
    return std::move((*stmts)[0]);
}

static void parse_error(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return; // lexer error is acceptable
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_FALSE(stmts.has_value()) << "Expected parse error for: " << sql;
}

// ============================================================================
// GDB-257 Adversarial: EMBEDDING named-parameter syntax
// ============================================================================

// Case-insensitive parameter names (SOURCE, PROVIDER).
TEST(QA_GDB257_Adv, CaseInsensitiveParamNames) {
    auto stmt = parse_one("CREATE TABLE t (id INT PRIMARY KEY, "
                          "vec EMBEDDING(384, SOURCE='title', PROVIDER='openai'))");
    ASSERT_NE(stmt, nullptr);
    auto* create = dynamic_cast<const CreateTableStmt*>(stmt.get());
    ASSERT_NE(create, nullptr);
    ASSERT_GE(create->columns.size(), 2u);
    EXPECT_EQ(create->columns[1].type.source, "title");
    EXPECT_EQ(create->columns[1].type.provider, "openai");
}

// Mixed case parameter names.
TEST(QA_GDB257_Adv, MixedCaseParamNames) {
    auto stmt = parse_one("CREATE TABLE t (id INT PRIMARY KEY, "
                          "vec EMBEDDING(384, Source='body', Provider='builtin/384'))");
    ASSERT_NE(stmt, nullptr);
    auto* create = dynamic_cast<const CreateTableStmt*>(stmt.get());
    ASSERT_NE(create, nullptr);
    ASSERT_GE(create->columns.size(), 2u);
    EXPECT_EQ(create->columns[1].type.source, "body");
    EXPECT_EQ(create->columns[1].type.provider, "builtin/384");
}

// Empty string value for source should parse (validation is downstream).
TEST(QA_GDB257_Adv, EmptySourceValue) {
    auto stmt = parse_one("CREATE TABLE t (id INT PRIMARY KEY, "
                          "vec EMBEDDING(384, source='', provider='openai'))");
    ASSERT_NE(stmt, nullptr);
    auto* create = dynamic_cast<const CreateTableStmt*>(stmt.get());
    ASSERT_NE(create, nullptr);
    ASSERT_GE(create->columns.size(), 2u);
    EXPECT_EQ(create->columns[1].type.source, "");
}

// Source value with special characters (concatenation expression).
TEST(QA_GDB257_Adv, SourceWithConcatExpression) {
    auto stmt = parse_one("CREATE TABLE t (id INT PRIMARY KEY, "
                          "vec EMBEDDING(384, source='title || body', provider='openai'))");
    ASSERT_NE(stmt, nullptr);
    auto* create = dynamic_cast<const CreateTableStmt*>(stmt.get());
    ASSERT_NE(create, nullptr);
    ASSERT_GE(create->columns.size(), 2u);
    EXPECT_EQ(create->columns[1].type.source, "title || body");
}

// Duplicate provider parameter should error.
TEST(QA_GDB257_Adv, DuplicateProviderParameter) {
    parse_error("CREATE TABLE t (id INT PRIMARY KEY, "
                "vec EMBEDDING(384, provider='a', provider='b'))");
}

// Mixing a named EMBEDDING parameter with a subsequent positional one should
// error. (The plain "only source, missing provider" case -- EMBEDDING(384,
// source='title') -- is already covered by QA_GDB257.MissingProviderParameter
// in test_qa_gdb_257.cpp, so this adversarial slot exercises the distinct
// named-then-positional mix instead of duplicating it.)
TEST(QA_GDB257_Adv, MixedNamedThenPositionalParam) {
    parse_error("CREATE TABLE t (id INT PRIMARY KEY, "
                "vec EMBEDDING(384, source='title', 'openai'))");
}

// Dimension 0 with named params should parse (dimension validation is downstream).
TEST(QA_GDB257_Adv, ZeroDimensionWithNamedParams) {
    auto stmt = parse_one("CREATE TABLE t (id INT PRIMARY KEY, "
                          "vec EMBEDDING(0, source='title', provider='openai'))");
    ASSERT_NE(stmt, nullptr);
    auto* create = dynamic_cast<const CreateTableStmt*>(stmt.get());
    ASSERT_NE(create, nullptr);
    ASSERT_GE(create->columns.size(), 2u);
    EXPECT_EQ(create->columns[1].type.param1, 0);
}

// Large dimension with named params.
TEST(QA_GDB257_Adv, LargeDimensionWithNamedParams) {
    auto stmt = parse_one("CREATE TABLE t (id INT PRIMARY KEY, "
                          "vec EMBEDDING(4096, source='title', provider='openai'))");
    ASSERT_NE(stmt, nullptr);
    auto* create = dynamic_cast<const CreateTableStmt*>(stmt.get());
    ASSERT_NE(create, nullptr);
    ASSERT_GE(create->columns.size(), 2u);
    EXPECT_EQ(create->columns[1].type.param1, 4096);
}

// ============================================================================
// GDB-261 Adversarial: CREATE DATABASE IF NOT EXISTS
// ============================================================================

class QA_GDB261_Adv : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb261_adv";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        if (!result.has_value()) {
            ADD_FAILURE() << sql << ": " << result.error().message;
            return {};
        }
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected);
        }
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// Double IF NOT EXISTS: both before and after name (should succeed, not error).
TEST_F(QA_GDB261_Adv, DoubleIfNotExists) {
    // CREATE DATABASE IF NOT EXISTS mydb — first time.
    exec_ok("CREATE DATABASE IF NOT EXISTS mydb");
    // Second call with after-name syntax should also succeed silently.
    exec_ok("CREATE DATABASE mydb IF NOT EXISTS");
}

// Multiple sequential IF NOT EXISTS on same database.
TEST_F(QA_GDB261_Adv, TripleIfNotExists) {
    exec_ok("CREATE DATABASE IF NOT EXISTS testdb");
    exec_ok("CREATE DATABASE IF NOT EXISTS testdb");
    exec_ok("CREATE DATABASE testdb IF NOT EXISTS");
}

// IF NOT EXISTS parse: partial "IF" without "NOT EXISTS" after name should error.
TEST(QA_GDB261_Parse, PartialIfAfterName) {
    parse_error("CREATE DATABASE mydb IF");
}

// IF NOT EXISTS parse: "IF NOT" without "EXISTS" after name should error.
TEST(QA_GDB261_Parse, PartialIfNotAfterName) {
    parse_error("CREATE DATABASE mydb IF NOT");
}

// ============================================================================
// GDB-303 Adversarial: Edge properties via TRAVERSE alias
// ============================================================================

class QA_GDB303_Adv : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb303_adv";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        exec_ok("CREATE TABLE people (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO people VALUES (1, 'Alice')");
        exec_ok("INSERT INTO people VALUES (2, 'Bob')");
        exec_ok("INSERT INTO people VALUES (3, 'Charlie')");
        exec_ok("CREATE EDGE TYPE knows (weight INT, label VARCHAR) FROM people TO people");
        exec_ok("LINK people(1) TO people(2) VIA knows (weight = 10, label = 'friend')");
        exec_ok("LINK people(1) TO people(3) VIA knows (weight = 5, label = 'colleague')");
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        if (!result.has_value()) {
            ADD_FAILURE() << sql << ": " << result.error().message;
            return {};
        }
        return std::move(*result);
    }

    void exec_error(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
    }

    static int64_t val_to_int64(const Value& v) {
        if (v.type_id() == TypeId::INT32)
            return v.as_int32();
        return v.as_int64();
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// Multiple edge properties via alias.
TEST_F(QA_GDB303_Adv, MultipleEdgePropertiesViaAlias) {
    auto qr = exec_ok("SELECT t.weight, t.label "
                      "FROM TRAVERSE knows FROM people(1) DIRECTION OUT AS t");
    ASSERT_EQ(qr.rows.size(), 2u);
    // Verify column names are correct.
    ASSERT_GE(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "weight");
    EXPECT_EQ(qr.column_names[1], "label");
}

// WHERE clause referencing edge property via alias.
TEST_F(QA_GDB303_Adv, WhereClauseOnAliasedEdgeProperty) {
    auto qr = exec_ok("SELECT t.name, t.weight "
                      "FROM TRAVERSE knows FROM people(1) DIRECTION OUT AS t "
                      "WHERE t.weight > 7");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Bob");
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 10);
}

// Edge mode: multiple properties via alias.
TEST_F(QA_GDB303_Adv, EdgeModeMultiplePropertiesViaAlias) {
    auto qr = exec_ok("SELECT t.weight, t.label "
                      "FROM TRAVERSE knows FROM people(1) DIRECTION OUT MODE EDGES AS t");
    ASSERT_EQ(qr.rows.size(), 2u);
}

// Table column + edge property + meta column via same alias.
TEST_F(QA_GDB303_Adv, MixedColumnsViaAlias) {
    auto qr = exec_ok("SELECT t.name, t.weight, t.__node, t.__depth "
                      "FROM TRAVERSE knows FROM people(1) DIRECTION OUT AS t");
    ASSERT_EQ(qr.rows.size(), 2u);
    ASSERT_EQ(qr.column_names.size(), 4u);
    // Verify all columns present in each row.
    for (const auto& row : qr.rows) {
        EXPECT_EQ(row.size(), 4u);
    }
}

// Unqualified edge property access still works with alias defined.
TEST_F(QA_GDB303_Adv, UnqualifiedPropertyWithAliasPresent) {
    auto qr = exec_ok("SELECT weight, label "
                      "FROM TRAVERSE knows FROM people(1) DIRECTION OUT AS t");
    ASSERT_EQ(qr.rows.size(), 2u);
}

// ============================================================================
// GDB-308 Adversarial: LINK PK existence validation
// ============================================================================

class QA_GDB308_Adv : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb308_adv";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        if (!result.has_value()) {
            ADD_FAILURE() << sql << ": " << result.error().message;
            return {};
        }
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected)
                << "Expected " << static_cast<int>(expected) << " but got "
                << static_cast<int>(result.error().code) << ": " << result.error().message;
        }
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// LINK after DELETE: row existed then was deleted.
TEST_F(QA_GDB308_Adv, LinkAfterDeleteFails) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'Alice')");
    exec_ok("INSERT INTO users VALUES (2, 'Bob')");
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");

    // Delete row 2.
    exec_ok("DELETE FROM users WHERE id = 2");

    // LINK to deleted row should fail.
    exec_error("LINK users(1) TO users(2) VIA follows", StatusCode::NOT_FOUND);
}

// LINK with edge properties when source PK doesn't exist.
// Verify property evaluation doesn't happen before PK check.
TEST_F(QA_GDB308_Adv, LinkWithPropertiesNonExistentSource) {
    exec_ok("CREATE TABLE items (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO items VALUES (1, 'Item A')");
    exec_ok("CREATE EDGE TYPE links (weight INT) FROM items TO items");

    exec_error("LINK items(999) TO items(1) VIA links (weight = 42)", StatusCode::NOT_FOUND);
}

// LINK with heterogeneous edge type (different source and target tables).
TEST_F(QA_GDB308_Adv, HeterogeneousEdgeValidPKs) {
    exec_ok("CREATE TABLE authors (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO authors VALUES (1, 'Author A')");
    exec_ok("INSERT INTO books VALUES (10, 'Book B')");
    exec_ok("CREATE EDGE TYPE wrote FROM authors TO books");

    exec_ok("LINK authors(1) TO books(10) VIA wrote");
}

// LINK with heterogeneous edge type, non-existent source.
TEST_F(QA_GDB308_Adv, HeterogeneousEdgeNonExistentSource) {
    exec_ok("CREATE TABLE authors (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO authors VALUES (1, 'Author A')");
    exec_ok("INSERT INTO books VALUES (10, 'Book B')");
    exec_ok("CREATE EDGE TYPE wrote FROM authors TO books");

    exec_error("LINK authors(999) TO books(10) VIA wrote", StatusCode::NOT_FOUND);
}

// LINK with heterogeneous edge type, non-existent target.
TEST_F(QA_GDB308_Adv, HeterogeneousEdgeNonExistentTarget) {
    exec_ok("CREATE TABLE authors (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO authors VALUES (1, 'Author A')");
    exec_ok("INSERT INTO books VALUES (10, 'Book B')");
    exec_ok("CREATE EDGE TYPE wrote FROM authors TO books");

    exec_error("LINK authors(1) TO books(999) VIA wrote", StatusCode::NOT_FOUND);
}

// LINK with boundary PK value: 0.
TEST_F(QA_GDB308_Adv, BoundaryPKZero) {
    exec_ok("CREATE TABLE items (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO items VALUES (0, 'Zero')");
    exec_ok("INSERT INTO items VALUES (1, 'One')");
    exec_ok("CREATE EDGE TYPE links FROM items TO items");

    // PK 0 exists, should succeed.
    exec_ok("LINK items(0) TO items(1) VIA links");
}

// LINK with negative PK value.
TEST_F(QA_GDB308_Adv, NegativePK) {
    exec_ok("CREATE TABLE items (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO items VALUES (-1, 'Negative')");
    exec_ok("INSERT INTO items VALUES (1, 'Positive')");
    exec_ok("CREATE EDGE TYPE links FROM items TO items");

    exec_ok("LINK items(-1) TO items(1) VIA links");
}

// LINK with negative PK that doesn't exist.
TEST_F(QA_GDB308_Adv, NegativePKNotFound) {
    exec_ok("CREATE TABLE items (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO items VALUES (1, 'One')");
    exec_ok("CREATE EDGE TYPE links FROM items TO items");

    exec_error("LINK items(-999) TO items(1) VIA links", StatusCode::NOT_FOUND);
}

// LINK then LINK duplicate — should succeed (edge creation is additive).
TEST_F(QA_GDB308_Adv, DuplicateLinkSucceeds) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'Alice')");
    exec_ok("INSERT INTO users VALUES (2, 'Bob')");
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");

    exec_ok("LINK users(1) TO users(2) VIA follows");
    // Duplicate LINK — PKs exist, so validation passes.
    exec_ok("LINK users(1) TO users(2) VIA follows");
}

// LINK self-edge: source == target PK.
TEST_F(QA_GDB308_Adv, SelfEdge) {
    exec_ok("CREATE TABLE nodes (id INT PRIMARY KEY, val VARCHAR)");
    exec_ok("INSERT INTO nodes VALUES (1, 'self')");
    exec_ok("CREATE EDGE TYPE loops FROM nodes TO nodes");

    exec_ok("LINK nodes(1) TO nodes(1) VIA loops");
}

// Empty table — LINK should fail since no PKs exist.
TEST_F(QA_GDB308_Adv, EmptyTableFails) {
    exec_ok("CREATE TABLE empty_tbl (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE EDGE TYPE refs FROM empty_tbl TO empty_tbl");

    exec_error("LINK empty_tbl(1) TO empty_tbl(2) VIA refs", StatusCode::NOT_FOUND);
}

// ============================================================================
// GDB-300/301/302 Adversarial: Defensive improvements
// ============================================================================

class QA_GDB300_Adv : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb300_adv";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        if (!result.has_value()) {
            ADD_FAILURE() << sql << ": " << result.error().message;
            return {};
        }
        return std::move(*result);
    }

    static int64_t val_to_int64(const Value& v) {
        if (v.type_id() == TypeId::INT32)
            return v.as_int32();
        return v.as_int64();
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// Enriched traversal with many hops — schema column count consistency.
TEST_F(QA_GDB300_Adv, DeepTraversalColumnCountConsistency) {
    exec_ok("CREATE TABLE nodes (id INT PRIMARY KEY, label VARCHAR)");
    for (int i = 1; i <= 10; ++i) {
        exec_ok("INSERT INTO nodes VALUES (" + std::to_string(i) + ", 'N" + std::to_string(i) +
                "')");
    }
    exec_ok("CREATE EDGE TYPE chain FROM nodes TO nodes");
    for (int i = 1; i < 10; ++i) {
        exec_ok("LINK nodes(" + std::to_string(i) + ") TO nodes(" + std::to_string(i + 1) +
                ") VIA chain");
    }

    auto qr = exec_ok("SELECT id, label, __node, __depth, __source "
                      "FROM TRAVERSE chain FROM nodes(1) DIRECTION OUT MAX_DEPTH 10");
    ASSERT_EQ(qr.column_names.size(), 5u);
    // All rows have consistent column count.
    for (const auto& row : qr.rows) {
        EXPECT_EQ(row.size(), 5u);
    }
    // Should reach nodes 2-10 (9 nodes).
    EXPECT_EQ(qr.rows.size(), 9u);
}

// Enriched traversal with edge properties — column count consistency.
TEST_F(QA_GDB300_Adv, TraversalWithEdgePropertiesColumnCount) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'Alice')");
    exec_ok("INSERT INTO users VALUES (2, 'Bob')");
    exec_ok("INSERT INTO users VALUES (3, 'Charlie')");
    exec_ok("CREATE EDGE TYPE follows (weight INT, tag VARCHAR) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 5, tag = 'a')");
    exec_ok("LINK users(2) TO users(3) VIA follows (weight = 10, tag = 'b')");

    auto qr = exec_ok("SELECT id, name, __node, __depth, __source, weight, tag "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 3");
    ASSERT_EQ(qr.column_names.size(), 7u);
    for (const auto& row : qr.rows) {
        EXPECT_EQ(row.size(), 7u);
    }
}

// SELECT * traversal — all columns should be present.
TEST_F(QA_GDB300_Adv, SelectStarWithEdgeProperties) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'Alice')");
    exec_ok("INSERT INTO users VALUES (2, 'Bob')");
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 42)");

    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    // id, name, __node, __depth, __source, weight = 6 columns
    ASSERT_EQ(qr.column_names.size(), 6u);
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0].size(), 6u);
}

} // namespace
} // namespace sixseven
