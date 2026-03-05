/// @file test_qa_gdb_256.cpp
/// QA adversarial tests for GDB-256: GraphEngine never instantiated in server startup.
///
/// Verifies that:
///   1. GraphEngine is properly wired to QueryEngine (all graph DDL/DML succeed).
///   2. Nullptr guard still produces clean errors (no crashes, correct StatusCode).
///   3. Edge cases around graph operations behave correctly.
///   4. Graph operations work with various PK types, empty tables, etc.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

using namespace sixseven;

// =============================================================================
// Fixture: QueryEngine WITH GraphEngine (the normal, fixed configuration)
// =============================================================================

class QA_GDB256_WithGraph : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_qa_gdb256_graph";
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
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "Expected error but got success for: " << sql;
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected)
                << "Expected " << static_cast<int>(expected) << " but got "
                << static_cast<int>(result.error().code) << " (" << result.error().message << ")";
        }
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// Fixture: QueryEngine WITHOUT GraphEngine (nullptr, pre-fix behavior)
// =============================================================================

class QA_GDB256_NoGraph : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_qa_gdb256_nograph";
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
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "Expected error but got success for: " << sql;
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected)
                << "Expected " << static_cast<int>(expected) << " but got "
                << static_cast<int>(result.error().code) << " (" << result.error().message << ")";
        }
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// Nullptr guard tests: graph operations without GraphEngine should return
// clean errors, not crash.
// =============================================================================

TEST_F(QA_GDB256_NoGraph, CreateEdgeTypeReturnsInternalError) {
    exec_ok("CREATE TABLE a (id INT PRIMARY KEY, name VARCHAR)");
    exec_error("CREATE EDGE TYPE rel FROM a TO a", StatusCode::INTERNAL_ERROR);
}

TEST_F(QA_GDB256_NoGraph, DropEdgeTypeReturnsInternalError) {
    exec_error("DROP EDGE TYPE nonexistent", StatusCode::INTERNAL_ERROR);
}

TEST_F(QA_GDB256_NoGraph, DropEdgeTypeIfExistsAlsoReturnsInternalError) {
    // Even IF EXISTS should fail if graph engine is not available.
    exec_error("DROP EDGE TYPE IF EXISTS nonexistent", StatusCode::INTERNAL_ERROR);
}

TEST_F(QA_GDB256_NoGraph, NonGraphOperationsStillWorkWithoutGraphEngine) {
    // Relational operations should be completely unaffected.
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR, age INT)");
    exec_ok("INSERT INTO users VALUES (1, 'alice', 30)");
    exec_ok("INSERT INTO users VALUES (2, 'bob', 25)");
    auto qr = exec_ok("SELECT * FROM users ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "alice");
    EXPECT_EQ(qr.rows[1][1].as_string(), "bob");
}

// =============================================================================
// Core fix verification: graph operations WITH GraphEngine succeed
// =============================================================================

TEST_F(QA_GDB256_WithGraph, CreateEdgeTypeBasic) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    auto qr = exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    EXPECT_EQ(qr.message, "CREATE EDGE TYPE");
}

TEST_F(QA_GDB256_WithGraph, DropEdgeTypeBasic) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    auto qr = exec_ok("DROP EDGE TYPE follows");
    EXPECT_EQ(qr.message, "DROP EDGE TYPE");
}

TEST_F(QA_GDB256_WithGraph, LinkAndUnlinkBasic) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO users VALUES (2, 'bob')");
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");

    auto link = exec_ok("LINK users(1) TO users(2) VIA follows");
    EXPECT_EQ(link.affected_rows, 1);

    auto unlink = exec_ok("UNLINK users(1) FROM users(2) VIA follows");
    EXPECT_EQ(unlink.affected_rows, 1);
}

// =============================================================================
// Edge cases: CREATE EDGE TYPE
// =============================================================================

TEST_F(QA_GDB256_WithGraph, CreateEdgeTypeBetweenDifferentTables) {
    exec_ok("CREATE TABLE authors (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR)");
    auto qr = exec_ok("CREATE EDGE TYPE wrote FROM authors TO books");
    EXPECT_EQ(qr.message, "CREATE EDGE TYPE");
}

TEST_F(QA_GDB256_WithGraph, CreateEdgeTypeDuplicateNameFails) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    exec_error("CREATE EDGE TYPE follows FROM users TO users", StatusCode::ALREADY_EXISTS);
}

TEST_F(QA_GDB256_WithGraph, CreateEdgeTypeNonExistentSourceTableFails) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_error("CREATE EDGE TYPE rel FROM nonexistent TO users", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB256_WithGraph, CreateEdgeTypeNonExistentTargetTableFails) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_error("CREATE EDGE TYPE rel FROM users TO nonexistent", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB256_WithGraph, CreateEdgeTypeBothTablesNonExistent) {
    exec_error("CREATE EDGE TYPE rel FROM ghost1 TO ghost2", StatusCode::NOT_FOUND);
}

// =============================================================================
// Edge cases: DROP EDGE TYPE
// =============================================================================

TEST_F(QA_GDB256_WithGraph, DropEdgeTypeNotFoundFails) {
    exec_error("DROP EDGE TYPE phantom", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB256_WithGraph, DropEdgeTypeIfExistsNotFoundSucceeds) {
    auto qr = exec_ok("DROP EDGE TYPE IF EXISTS phantom");
    EXPECT_EQ(qr.message, "DROP EDGE TYPE");
}

TEST_F(QA_GDB256_WithGraph, DropEdgeTypeTwiceFails) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    exec_ok("DROP EDGE TYPE follows");
    exec_error("DROP EDGE TYPE follows", StatusCode::NOT_FOUND);
}

// =============================================================================
// Edge cases: LINK
// =============================================================================

TEST_F(QA_GDB256_WithGraph, LinkNonExistentEdgeTypeFails) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO users VALUES (2, 'bob')");
    // Edge type not created — binder should reject.
    exec_error("LINK users(1) TO users(2) VIA follows", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB256_WithGraph, LinkSelfLoop) {
    // A node linking to itself should work.
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");

    auto qr = exec_ok("LINK users(1) TO users(1) VIA follows");
    EXPECT_EQ(qr.affected_rows, 1);
}

TEST_F(QA_GDB256_WithGraph, LinkMultipleEdgesSameDirection) {
    // Multiple edges between the same pair (no prevent_duplicates by default).
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO users VALUES (2, 'bob')");
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");

    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(1) TO users(2) VIA follows");
    // Both succeed — duplicates allowed by default.
}

TEST_F(QA_GDB256_WithGraph, UnlinkNonExistentEdgeFails) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO users VALUES (2, 'bob')");
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");

    // No edge exists yet — unlink should fail.
    exec_error("UNLINK users(1) FROM users(2) VIA follows", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB256_WithGraph, UnlinkAfterAlreadyUnlinked) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO users VALUES (2, 'bob')");
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");

    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("UNLINK users(1) FROM users(2) VIA follows");
    // Second unlink: edge no longer exists.
    exec_error("UNLINK users(1) FROM users(2) VIA follows", StatusCode::NOT_FOUND);
}

// =============================================================================
// Full lifecycle and stress tests
// =============================================================================

TEST_F(QA_GDB256_WithGraph, CreateDropRecreateEdgeType) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    exec_ok("DROP EDGE TYPE follows");
    // Recreate after drop — should succeed.
    auto qr = exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    EXPECT_EQ(qr.message, "CREATE EDGE TYPE");
}

TEST_F(QA_GDB256_WithGraph, LinkUnlinkAfterRecreate) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO users VALUES (2, 'bob')");

    exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("DROP EDGE TYPE follows");

    // Recreate and link again.
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    auto qr = exec_ok("LINK users(1) TO users(2) VIA follows");
    EXPECT_EQ(qr.affected_rows, 1);
}

TEST_F(QA_GDB256_WithGraph, MultipleEdgeTypesCoexist) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE TABLE posts (id INT PRIMARY KEY, body VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO users VALUES (2, 'bob')");
    exec_ok("INSERT INTO posts VALUES (10, 'hello')");

    exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    exec_ok("CREATE EDGE TYPE authored FROM users TO posts");

    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(1) TO posts(10) VIA authored");

    exec_ok("UNLINK users(1) FROM users(2) VIA follows");
    exec_ok("UNLINK users(1) FROM posts(10) VIA authored");

    exec_ok("DROP EDGE TYPE follows");
    exec_ok("DROP EDGE TYPE authored");
}

TEST_F(QA_GDB256_WithGraph, StressManyLinksAndUnlinks) {
    exec_ok("CREATE TABLE nodes (id INT PRIMARY KEY, label VARCHAR)");
    for (int i = 1; i <= 50; ++i) {
        exec_ok("INSERT INTO nodes VALUES (" + std::to_string(i) + ", 'n" + std::to_string(i) +
                "')");
    }
    exec_ok("CREATE EDGE TYPE connects FROM nodes TO nodes");

    // Create 49 edges: 1->2, 2->3, ..., 49->50
    for (int i = 1; i < 50; ++i) {
        exec_ok("LINK nodes(" + std::to_string(i) + ") TO nodes(" + std::to_string(i + 1) +
                ") VIA connects");
    }

    // Unlink all of them.
    for (int i = 1; i < 50; ++i) {
        exec_ok("UNLINK nodes(" + std::to_string(i) + ") FROM nodes(" + std::to_string(i + 1) +
                ") VIA connects");
    }

    // All edges gone — any further unlink should fail.
    exec_error("UNLINK nodes(1) FROM nodes(2) VIA connects", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB256_WithGraph, LinkAfterDropEdgeTypeFails) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO users VALUES (2, 'bob')");
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    exec_ok("DROP EDGE TYPE follows");

    // Trying to link via a dropped edge type — binder resolves first.
    exec_error("LINK users(1) TO users(2) VIA follows", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB256_WithGraph, RelationalOpsUnaffectedByGraphEngine) {
    // Ensure presence of GraphEngine doesn't interfere with normal SQL.
    exec_ok("CREATE TABLE t (id INT PRIMARY KEY, val VARCHAR)");
    exec_ok("INSERT INTO t VALUES (1, 'a')");
    exec_ok("INSERT INTO t VALUES (2, 'b')");
    auto qr = exec_ok("SELECT * FROM t ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 2);
}
