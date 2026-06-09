#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

/// Test fixture for heterogeneous edge traversal (edges connecting different tables).
///
/// Schema:
///   users: (id INT PK, name VARCHAR) — rows: (1, "Alice"), (2, "Bob")
///   posts: (id INT PK, title VARCHAR) — rows: (10, "Hello"), (20, "World"), (30, "Draft")
///   edge type "authored" FROM users TO posts — heterogeneous
///   edge type "follows" FROM users TO users — homogeneous (for BOTH direction tests)
///   edges: authored 1→10, authored 1→20, authored 2→30
///   edges: follows 1→2
class HeterogeneousTraversalTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_hetero_trav";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        // Create tables and insert rows.
        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");

        exec_ok("CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR)");
        exec_ok("INSERT INTO posts VALUES (10, 'Hello')");
        exec_ok("INSERT INTO posts VALUES (20, 'World')");
        exec_ok("INSERT INTO posts VALUES (30, 'Draft')");

        // Heterogeneous edge type: users → posts.
        exec_ok("CREATE EDGE TYPE authored FROM users TO posts");
        exec_ok("LINK users(1) TO posts(10) VIA authored");
        exec_ok("LINK users(1) TO posts(20) VIA authored");
        exec_ok("LINK users(2) TO posts(30) VIA authored");

        // Homogeneous edge type: users → users.
        exec_ok("CREATE EDGE TYPE follows FROM users TO users");
        exec_ok("LINK users(1) TO users(2) VIA follows");
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << ": " << result.error().message;
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected);
        }
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

// ============================================================================
// 1. OUT from source table returns target table (posts) columns
// ============================================================================

TEST_F(HeterogeneousTraversalTest, OutFromSourceReturnsTargetColumns) {
    auto qr = exec_ok("SELECT title, __depth FROM TRAVERSE authored FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "title");
    EXPECT_EQ(qr.column_names[1], "__depth");

    // User 1 authored posts 10 and 20.
    ASSERT_EQ(qr.rows.size(), 2u);

    std::vector<std::string> titles;
    for (const auto& row : qr.rows) {
        titles.push_back(row[0].as_string());
        EXPECT_EQ(val_to_int64(row[1]), 1); // All at depth 1.
    }
    std::sort(titles.begin(), titles.end());
    EXPECT_EQ(titles[0], "Hello");
    EXPECT_EQ(titles[1], "World");
}

// ============================================================================
// 2. IN from target table returns source table (users) columns
// ============================================================================

TEST_F(HeterogeneousTraversalTest, InFromTargetReturnsSourceColumns) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE authored FROM posts(10) DIRECTION IN");

    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "name");
    EXPECT_EQ(qr.column_names[1], "__depth");

    // Post 10 was authored by user 1 (Alice).
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Alice");
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 1);
}

// ============================================================================
// 3. DIRECTION BOTH on heterogeneous edge → binding error
// ============================================================================

TEST_F(HeterogeneousTraversalTest, BothOnHeterogeneousEdgeRejected) {
    exec_error("SELECT * FROM TRAVERSE authored FROM users(1) DIRECTION BOTH",
               StatusCode::TYPE_ERROR);
}

// ============================================================================
// 4. DIRECTION BOTH on homogeneous edge (follows) → works correctly
// ============================================================================

TEST_F(HeterogeneousTraversalTest, BothOnHomogeneousEdgeWorks) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION BOTH");

    // User 1 follows user 2 (OUT), no one follows user 1 (IN) in our data.
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Bob");
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 1);
}

// ============================================================================
// 5. FROM table not an endpoint of edge type → clear error
// ============================================================================

TEST_F(HeterogeneousTraversalTest, FromTableNotEndpointRejected) {
    // posts is not an endpoint of "follows" (follows is users→users).
    exec_error("SELECT * FROM TRAVERSE follows FROM posts(10) DIRECTION OUT",
               StatusCode::TYPE_ERROR);
}

// ============================================================================
// 6. SELECT * on heterogeneous OUT returns full target table schema + meta
// ============================================================================

TEST_F(HeterogeneousTraversalTest, SelectStarOutReturnsTargetSchema) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE authored FROM users(1) DIRECTION OUT");

    // posts columns (id, title) + meta-columns (__node, __depth, __source).
    ASSERT_GE(qr.column_names.size(), 5u);
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "title");
    EXPECT_EQ(qr.column_names[2], "__node");
    EXPECT_EQ(qr.column_names[3], "__depth");
    EXPECT_EQ(qr.column_names[4], "__source");

    ASSERT_EQ(qr.rows.size(), 2u);

    // Verify enriched data: id matches __node.
    for (const auto& row : qr.rows) {
        EXPECT_EQ(row[0].as_int32(), static_cast<int32_t>(val_to_int64(row[2])));
    }
}

// ============================================================================
// 7. SELECT * on heterogeneous IN returns full source table schema + meta
// ============================================================================

TEST_F(HeterogeneousTraversalTest, SelectStarInReturnsSourceSchema) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE authored FROM posts(10) DIRECTION IN");

    // users columns (id, name) + meta-columns (__node, __depth, __source).
    ASSERT_GE(qr.column_names.size(), 5u);
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "name");
    EXPECT_EQ(qr.column_names[2], "__node");
    EXPECT_EQ(qr.column_names[3], "__depth");
    EXPECT_EQ(qr.column_names[4], "__source");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "Alice");
}

// ============================================================================
// 8. GDB-304 regression: overlapping PKs across tables must not collide
//    in the BFS visited set.
// ============================================================================

TEST_F(HeterogeneousTraversalTest, PKCollisionOutDirection) {
    // Insert a post whose PK matches a user PK (both id=1).
    exec_ok("INSERT INTO posts VALUES (1, 'Collider')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    auto qr = exec_ok("SELECT title FROM TRAVERSE authored FROM users(1) DIRECTION OUT");

    // User 1 authored posts 10, 20, AND 1 — all three must appear.
    std::vector<std::string> titles;
    for (const auto& row : qr.rows) {
        titles.push_back(row[0].as_string());
    }
    std::sort(titles.begin(), titles.end());

    ASSERT_EQ(titles.size(), 3u);
    EXPECT_EQ(titles[0], "Collider");
    EXPECT_EQ(titles[1], "Hello");
    EXPECT_EQ(titles[2], "World");
}

TEST_F(HeterogeneousTraversalTest, PKCollisionInDirection) {
    // Post id=1 exists in posts but also user id=1 exists.
    exec_ok("INSERT INTO posts VALUES (1, 'Collider')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    auto qr = exec_ok("SELECT name FROM TRAVERSE authored FROM posts(1) DIRECTION IN");

    // Post 1 was authored by user 1 (Alice) — must not be skipped.
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Alice");
}

// ============================================================================
// GDB-694: heterogeneous TRACE must not hang when a target PK equals the
// start PK. The parent map is keyed by PK only, so posts(1) reached from
// users(1) used to record itself as its own parent and reconstruct_path()
// looped forever (unbounded memory growth from plain SQL).
// ============================================================================

TEST_F(HeterogeneousTraversalTest, TraceSamePkEnrichedOutTwoStepPath) {
    exec_ok("INSERT INTO posts VALUES (1, 'Collider')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    auto qr = exec_ok("SELECT title, __node, __depth, __path "
                      "FROM TRAVERSE authored FROM users(1) DIRECTION OUT WITH TRACE");

    // users(1) authored posts 10, 20, and 1.
    ASSERT_EQ(qr.rows.size(), 3u);
    bool saw_collider = false;
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row[3].type_id(), TypeId::PATH);
        const Path& p = row[3].as_path();
        ASSERT_EQ(p.steps.size(), 2u) << "path must be start -> target, not a self-chain";
        EXPECT_EQ(p.steps[0].node_pk, 1);
        EXPECT_EQ(p.steps[1].node_pk, val_to_int64(row[1]));
        EXPECT_GE(p.steps[0].edge_id, 0) << "start step must carry the outgoing edge";
        EXPECT_EQ(p.steps[1].edge_id, -1) << "terminal step has no outgoing edge";
        if (val_to_int64(row[1]) == 1) {
            saw_collider = true;
            EXPECT_EQ(row[0].as_string(), "Collider");
        }
    }
    EXPECT_TRUE(saw_collider) << "posts(1) must be reached despite the PK collision";
}

TEST_F(HeterogeneousTraversalTest, TraceSamePkEdgeModeOutTrivialPath) {
    exec_ok("INSERT INTO posts VALUES (1, 'Collider')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    auto qr = exec_ok("SELECT __from, __to, __path "
                      "FROM TRAVERSE authored FROM users(1) DIRECTION OUT MODE EDGES WITH TRACE");

    // Three edges out of users(1): to posts 10, 20, and 1.
    ASSERT_EQ(qr.rows.size(), 3u);
    for (const auto& row : qr.rows) {
        EXPECT_EQ(val_to_int64(row[0]), 1);
        ASSERT_EQ(row[2].type_id(), TypeId::PATH);
        const Path& p = row[2].as_path();
        ASSERT_EQ(p.steps.size(), 1u)
            << "path to the start (__from) node must be the trivial single step";
        EXPECT_EQ(p.steps[0].node_pk, 1);
        EXPECT_EQ(p.steps[0].edge_id, -1);
    }
}

TEST_F(HeterogeneousTraversalTest, TraceSamePkEnrichedInTwoStepPath) {
    exec_ok("INSERT INTO posts VALUES (1, 'Collider')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    auto qr = exec_ok("SELECT name, __node, __depth, __path "
                      "FROM TRAVERSE authored FROM posts(1) DIRECTION IN WITH TRACE");

    // posts(1) was authored by users(1) (Alice).
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Alice");
    ASSERT_EQ(qr.rows[0][3].type_id(), TypeId::PATH);
    const Path& p = qr.rows[0][3].as_path();
    ASSERT_EQ(p.steps.size(), 2u) << "path must be start -> source node, not a self-chain";
    EXPECT_EQ(p.steps[0].node_pk, 1); // start: posts(1)
    EXPECT_EQ(p.steps[1].node_pk, 1); // reached: users(1)
    EXPECT_GE(p.steps[0].edge_id, 0);
    EXPECT_EQ(p.steps[1].edge_id, -1);
}

TEST_F(HeterogeneousTraversalTest, TraceSamePkEdgeModeInTwoStepPath) {
    exec_ok("INSERT INTO posts VALUES (1, 'Collider')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    auto qr = exec_ok("SELECT __from, __to, __path "
                      "FROM TRAVERSE authored FROM posts(1) DIRECTION IN MODE EDGES WITH TRACE");

    // One edge users(1) -> posts(1); __from is users(1), discovered at depth 1.
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 1);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 1);
    ASSERT_EQ(qr.rows[0][2].type_id(), TypeId::PATH);
    const Path& p = qr.rows[0][2].as_path();
    ASSERT_EQ(p.steps.size(), 2u) << "path must be start -> __from node, not a self-chain";
    EXPECT_EQ(p.steps[0].node_pk, 1);
    EXPECT_EQ(p.steps[1].node_pk, 1);
    EXPECT_GE(p.steps[0].edge_id, 0);
    EXPECT_EQ(p.steps[1].edge_id, -1);
}

TEST_F(HeterogeneousTraversalTest, TraceDistinctPksStillCorrect) {
    auto qr = exec_ok("SELECT title, __node, __path "
                      "FROM TRAVERSE authored FROM users(2) DIRECTION OUT WITH TRACE");

    // users(2) authored only posts(30); the depth-bounded reconstruction must
    // not change the non-colliding heterogeneous path shape.
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Draft");
    ASSERT_EQ(qr.rows[0][2].type_id(), TypeId::PATH);
    const Path& p = qr.rows[0][2].as_path();
    ASSERT_EQ(p.steps.size(), 2u);
    EXPECT_EQ(p.steps[0].node_pk, 2);
    EXPECT_EQ(p.steps[1].node_pk, 30);
    EXPECT_EQ(p.length(), 1);
}

// ============================================================================
// GDB-605: Heterogeneous edges with DIFFERENT PK types (UUID vs INT).
//
// The bug: TRAVERSE start key was validated against the wrong table's PK type
// when source and target tables have different key types.
// ============================================================================

/// Fixture with UUID source table and INT32 target table.
///
/// Schema:
///   authors: (id UUID PK, name VARCHAR) — different PK type than articles
///   articles: (id INT PK, title VARCHAR)
///   edge type "wrote" FROM authors TO articles
class HeterogeneousPKTypeTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_hetero_pk";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        exec_ok("CREATE TABLE authors (id UUID PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO authors VALUES ('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee', 'Alice')");
        exec_ok("INSERT INTO authors VALUES ('11111111-2222-3333-4444-555555555555', 'Bob')");

        exec_ok("CREATE TABLE articles (id INT PRIMARY KEY, title VARCHAR)");
        exec_ok("INSERT INTO articles VALUES (10, 'Hello')");
        exec_ok("INSERT INTO articles VALUES (20, 'World')");
        exec_ok("INSERT INTO articles VALUES (30, 'Draft')");

        exec_ok("CREATE EDGE TYPE wrote FROM authors TO articles");
        exec_ok("LINK authors('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') TO articles(10) VIA wrote");
        exec_ok("LINK authors('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') TO articles(20) VIA wrote");
        exec_ok("LINK authors('11111111-2222-3333-4444-555555555555') TO articles(30) VIA wrote");
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << ": " << result.error().message;
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
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// GDB-605: OUT from UUID source — start key must validate against UUID, not INT.
TEST_F(HeterogeneousPKTypeTest, OutFromUUIDSource) {
    auto qr = exec_ok("SELECT title, __depth FROM TRAVERSE wrote "
                      "FROM authors('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') DIRECTION OUT");

    ASSERT_EQ(qr.rows.size(), 2u);
    std::vector<std::string> titles;
    for (const auto& row : qr.rows) {
        titles.push_back(row[0].as_string());
    }
    std::sort(titles.begin(), titles.end());
    EXPECT_EQ(titles[0], "Hello");
    EXPECT_EQ(titles[1], "World");
}

// GDB-605: IN from INT target — start key must validate against INT, not UUID.
TEST_F(HeterogeneousPKTypeTest, InFromINTTarget) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE wrote "
                      "FROM articles(10) DIRECTION IN");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Alice");
}

// GDB-605: Standalone TRAVERSE (no SELECT FROM) with UUID source.
TEST_F(HeterogeneousPKTypeTest, StandaloneTraverseOutUUID) {
    auto qr = exec_ok("TRAVERSE wrote FROM authors('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') "
                      "DIRECTION OUT FETCH");

    // Should return nodes without error.
    EXPECT_GE(qr.rows.size(), 2u);
}

// GDB-605: Standalone TRAVERSE (no SELECT FROM) with INT target going IN.
TEST_F(HeterogeneousPKTypeTest, StandaloneTraverseInINT) {
    auto qr = exec_ok("TRAVERSE wrote FROM articles(10) DIRECTION IN FETCH");

    EXPECT_GE(qr.rows.size(), 1u);
}

// GDB-605: SELECT * enriched traversal with UUID → INT.
TEST_F(HeterogeneousPKTypeTest, SelectStarOutUUIDSource) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE wrote "
                      "FROM authors('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') DIRECTION OUT");

    // articles columns (id, title) + meta-columns.
    ASSERT_GE(qr.column_names.size(), 5u);
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "title");
    ASSERT_EQ(qr.rows.size(), 2u);
}

// GDB-605: SELECT * enriched traversal with INT → UUID (IN direction).
TEST_F(HeterogeneousPKTypeTest, SelectStarInINTTarget) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE wrote FROM articles(10) DIRECTION IN");

    // authors columns (id, name) + meta-columns.
    ASSERT_GE(qr.column_names.size(), 5u);
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "name");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "Alice");
}

} // namespace
} // namespace sixseven
