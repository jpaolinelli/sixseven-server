/// @file test_qa_gdb_267.cpp
/// QA adversarial tests for GDB-267: Heterogeneous edge traversal support.
///
/// Verifies:
///   AC1: OUT from source returns target table columns (posts, not users).
///   AC2: IN from target returns source table columns (users, not posts).
///   AC3: DIRECTION BOTH on heterogeneous edges → rejected with clear error.
///   AC4: DIRECTION BOTH on homogeneous edges → continues to work.
///
/// Adversarial categories: boundary values, error paths, edge cases, schema
/// verification, multi-hop, empty results, WHERE filter on target columns.

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

#include "test_qa_helpers.h"

namespace sixseven {
namespace {

// ============================================================================
// Fixture: Two tables (users, posts) with heterogeneous "authored" edges,
// and a homogeneous "follows" edge on users. Also an edge type with
// properties for property-related adversarial tests.
// ============================================================================

class QA_GDB267_HeteroTraversal : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb267";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        // Users table.
        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");
        exec_ok("INSERT INTO users VALUES (3, 'Charlie')");

        // Posts table (different columns than users).
        exec_ok("CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR, body VARCHAR)");
        exec_ok("INSERT INTO posts VALUES (10, 'Hello', 'body1')");
        exec_ok("INSERT INTO posts VALUES (20, 'World', 'body2')");
        exec_ok("INSERT INTO posts VALUES (30, 'Draft', 'body3')");
        exec_ok("INSERT INTO posts VALUES (40, 'Archived', 'body4')");

        // Heterogeneous edge: users → posts.
        exec_ok("CREATE EDGE TYPE authored FROM users TO posts");
        exec_ok("LINK users(1) TO posts(10) VIA authored");
        exec_ok("LINK users(1) TO posts(20) VIA authored");
        exec_ok("LINK users(2) TO posts(30) VIA authored");
        // User 3 has no authored edges (boundary: no outgoing edges).
        // Post 40 has no authored edges (boundary: dangling post).

        // Homogeneous edge: users → users.
        exec_ok("CREATE EDGE TYPE follows FROM users TO users");
        exec_ok("LINK users(1) TO users(2) VIA follows");
        exec_ok("LINK users(2) TO users(3) VIA follows");
        // Creates a chain: 1→2→3 for multi-hop BOTH tests.

        // Heterogeneous edge with properties.
        exec_ok("CREATE EDGE TYPE rated (score INT, comment VARCHAR) FROM users TO posts");
        exec_ok("LINK users(1) TO posts(10) VIA rated (score = 5, comment = 'great')");
        exec_ok("LINK users(2) TO posts(20) VIA rated (score = 3, comment = 'okay')");
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
        return result ? std::move(*result) : QueryResult{};
    }

    void exec_error(const std::string& sql, StatusCode expected_code) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected_code);
        }
    }

    /// Helper to extract integer value regardless of underlying type (INT32
    /// or INT64). Meta-columns like __node and __source use the table's actual
    /// PK type.
    int64_t get_int(const Value& v) {
        if (v.is_null()) {
            ADD_FAILURE() << "expected integer value, got NULL";
            return 0;
        }
        try {
            return v.as_int64();
        } catch (...) {}
        try {
            return static_cast<int64_t>(v.as_int32());
        } catch (...) {}
        ADD_FAILURE() << "value is not an integer";
        return 0;
    }

    void exec_error_msg(const std::string& sql,
                        StatusCode expected_code,
                        const std::string& msg_substring) {
        auto result = engine_->execute(sql);
        ASSERT_FALSE(result.has_value()) << sql << " should have failed";
        EXPECT_EQ(result.error().code, expected_code);
        EXPECT_NE(result.error().message.find(msg_substring), std::string::npos)
            << "Expected error message to contain '" << msg_substring
            << "', got: " << result.error().message;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ============================================================================
// AC1: OUT from source returns target table columns
// ============================================================================

/// Verify that SELECT on specific columns from target table works.
TEST_F(QA_GDB267_HeteroTraversal, AC1_OutFromSource_SelectSpecificTargetColumns) {
    auto qr = exec_ok("SELECT title, __depth FROM TRAVERSE authored FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "title");
    EXPECT_EQ(qr.column_names[1], "__depth");

    ASSERT_EQ(qr.rows.size(), 2u);
    std::vector<std::string> titles;
    for (const auto& row : qr.rows) {
        titles.push_back(row[0].as_string());
        EXPECT_EQ(row[1].as_int64(), 1);
    }
    std::sort(titles.begin(), titles.end());
    EXPECT_EQ(titles[0], "Hello");
    EXPECT_EQ(titles[1], "World");
}

/// Verify that target table columns NOT in source table are accessible.
/// posts has "body" column that users does NOT have.
TEST_F(QA_GDB267_HeteroTraversal, AC1_OutFromSource_ColumnUniqueToTargetTable) {
    auto qr = exec_ok("SELECT body FROM TRAVERSE authored FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "body");

    ASSERT_EQ(qr.rows.size(), 2u);
    std::vector<std::string> bodies;
    for (const auto& row : qr.rows) {
        bodies.push_back(row[0].as_string());
    }
    std::sort(bodies.begin(), bodies.end());
    EXPECT_EQ(bodies[0], "body1");
    EXPECT_EQ(bodies[1], "body2");
}

/// Verify id column in SELECT * refers to target table's id (posts), not source (users).
TEST_F(QA_GDB267_HeteroTraversal, AC1_OutFromSource_SelectStarReturnsTargetId) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE authored FROM users(1) DIRECTION OUT");

    // posts columns: id, title, body + meta: __node, __depth, __source = 6 columns.
    ASSERT_GE(qr.column_names.size(), 6u);
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "title");
    EXPECT_EQ(qr.column_names[2], "body");

    ASSERT_EQ(qr.rows.size(), 2u);
    // The id values should be post IDs (10, 20), NOT user IDs (1).
    std::vector<int32_t> ids;
    for (const auto& row : qr.rows) {
        ids.push_back(row[0].as_int32());
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids[0], 10);
    EXPECT_EQ(ids[1], 20);
}

/// Verify a different user (user 2) who authored different posts.
TEST_F(QA_GDB267_HeteroTraversal, AC1_OutFromDifferentSourceUser) {
    auto qr = exec_ok("SELECT title FROM TRAVERSE authored FROM users(2) DIRECTION OUT");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Draft");
}

// ============================================================================
// AC2: IN from target side returns source table columns
// ============================================================================

/// Verify that IN direction from a post returns user columns.
TEST_F(QA_GDB267_HeteroTraversal, AC2_InFromTarget_ReturnsSourceColumns) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE authored FROM posts(10) DIRECTION IN");

    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "name");
    EXPECT_EQ(qr.column_names[1], "__depth");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Alice");
    EXPECT_EQ(qr.rows[0][1].as_int64(), 1);
}

/// Verify IN from a different post returns the correct author.
TEST_F(QA_GDB267_HeteroTraversal, AC2_InFromDifferentPost) {
    auto qr = exec_ok("SELECT name FROM TRAVERSE authored FROM posts(30) DIRECTION IN");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Bob");
}

/// Verify SELECT * with IN returns users schema (id, name) + meta.
TEST_F(QA_GDB267_HeteroTraversal, AC2_InFromTarget_SelectStarReturnsSourceSchema) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE authored FROM posts(10) DIRECTION IN");

    // users columns: id, name + meta: __node, __depth, __source = 5 columns.
    ASSERT_GE(qr.column_names.size(), 5u);
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "name");
    EXPECT_EQ(qr.column_names[2], "__node");
    EXPECT_EQ(qr.column_names[3], "__depth");
    EXPECT_EQ(qr.column_names[4], "__source");

    ASSERT_EQ(qr.rows.size(), 1u);
    // id should be user id (1), NOT post id (10).
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_string(), "Alice");
}

// ============================================================================
// AC3: DIRECTION BOTH on heterogeneous edges → rejected
// ============================================================================

/// Verify BOTH from source side is rejected.
TEST_F(QA_GDB267_HeteroTraversal, AC3_BothOnHetero_FromSourceRejected) {
    exec_error("SELECT * FROM TRAVERSE authored FROM users(1) DIRECTION BOTH",
               StatusCode::TYPE_ERROR);
}

/// Verify BOTH from target side is also rejected.
TEST_F(QA_GDB267_HeteroTraversal, AC3_BothOnHetero_FromTargetRejected) {
    exec_error("SELECT * FROM TRAVERSE authored FROM posts(10) DIRECTION BOTH",
               StatusCode::TYPE_ERROR);
}

/// Verify the error message contains actionable guidance.
TEST_F(QA_GDB267_HeteroTraversal, AC3_BothOnHetero_ErrorMessageIsInformative) {
    exec_error_msg("SELECT * FROM TRAVERSE authored FROM users(1) DIRECTION BOTH",
                   StatusCode::TYPE_ERROR,
                   "DIRECTION BOTH not supported");
}

/// Verify error message mentions the edge type name.
TEST_F(QA_GDB267_HeteroTraversal, AC3_BothOnHetero_ErrorMessageMentionsEdgeType) {
    exec_error_msg("SELECT * FROM TRAVERSE authored FROM users(1) DIRECTION BOTH",
                   StatusCode::TYPE_ERROR,
                   "authored");
}

/// Verify error message suggests alternatives (OUT or IN).
TEST_F(QA_GDB267_HeteroTraversal, AC3_BothOnHetero_ErrorMessageSuggestsAlternatives) {
    exec_error_msg("SELECT * FROM TRAVERSE authored FROM users(1) DIRECTION BOTH",
                   StatusCode::TYPE_ERROR,
                   "DIRECTION OUT or DIRECTION IN");
}

/// Verify BOTH rejection also applies to the "rated" edge type (with properties).
TEST_F(QA_GDB267_HeteroTraversal, AC3_BothOnHetero_AlsoRejectsEdgeWithProperties) {
    exec_error("SELECT * FROM TRAVERSE rated FROM users(1) DIRECTION BOTH", StatusCode::TYPE_ERROR);
}

// ============================================================================
// AC4: DIRECTION BOTH on homogeneous edges → works
// ============================================================================

/// Verify BOTH on follows (users→users) works and returns users columns.
TEST_F(QA_GDB267_HeteroTraversal, AC4_BothOnHomogeneous_Works) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION BOTH");

    // User 1 → user 2 (OUT), nothing follows user 1 (IN).
    ASSERT_EQ(qr.rows.size(), 2u); // user 2 at depth 1, user 3 at depth 2.

    std::vector<std::string> names;
    for (const auto& row : qr.rows) {
        names.push_back(row[0].as_string());
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names[0], "Bob");
    EXPECT_EQ(names[1], "Charlie");
}

/// Verify BOTH on homogeneous edge returns correct schema.
TEST_F(QA_GDB267_HeteroTraversal, AC4_BothOnHomogeneous_ReturnsCorrectSchema) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION BOTH");

    // users columns: id, name + meta: __node, __depth, __source = 5 columns.
    ASSERT_GE(qr.column_names.size(), 5u);
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "name");
}

/// Verify BOTH from middle of a chain discovers both directions.
TEST_F(QA_GDB267_HeteroTraversal, AC4_BothOnHomogeneous_FromMiddleNode) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(2) DIRECTION BOTH");

    // User 2: OUT→user 3, IN←user 1.
    ASSERT_EQ(qr.rows.size(), 2u);
    std::vector<std::string> names;
    for (const auto& row : qr.rows) {
        names.push_back(row[0].as_string());
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names[0], "Alice");
    EXPECT_EQ(names[1], "Charlie");
}

// ============================================================================
// Adversarial: Boundary & edge cases
// ============================================================================

/// Source node with no outgoing edges returns empty result.
TEST_F(QA_GDB267_HeteroTraversal, Boundary_SourceWithNoEdgesReturnsEmpty) {
    auto qr = exec_ok("SELECT title FROM TRAVERSE authored FROM users(3) DIRECTION OUT");
    EXPECT_EQ(qr.rows.size(), 0u);
}

/// Target node with no incoming edges returns empty result.
TEST_F(QA_GDB267_HeteroTraversal, Boundary_TargetWithNoIncomingEdgesReturnsEmpty) {
    auto qr = exec_ok("SELECT name FROM TRAVERSE authored FROM posts(40) DIRECTION IN");
    EXPECT_EQ(qr.rows.size(), 0u);
}

/// Non-existent start key returns empty result (no crash).
TEST_F(QA_GDB267_HeteroTraversal, Boundary_NonExistentStartKeyReturnsEmpty) {
    auto qr = exec_ok("SELECT title FROM TRAVERSE authored FROM users(999) DIRECTION OUT");
    EXPECT_EQ(qr.rows.size(), 0u);
}

/// IN from source side: users(1) is source, IN follows edges TO user(1).
/// Since authored edges have users as source, get_edges_to returns edges
/// where target_pk matches — but authored targets are posts, not users.
/// So no edge would have target_pk = 1 (a user ID) → empty result.
TEST_F(QA_GDB267_HeteroTraversal, Boundary_InFromSourceSideReturnsEmpty) {
    auto qr = exec_ok("SELECT name FROM TRAVERSE authored FROM users(1) DIRECTION IN");
    // Users(1) is the source. IN traverses backward — looks for edges TO user(1).
    // But authored edges go users→posts, so target PKs are post IDs.
    // No edge has target_pk = 1, so empty.
    EXPECT_EQ(qr.rows.size(), 0u);
}

/// OUT from target side: posts(10) is target, OUT follows edges FROM post(10).
/// Since authored edges have users as source, no edge has source_pk = 10 → empty.
TEST_F(QA_GDB267_HeteroTraversal, Boundary_OutFromTargetSideReturnsEmpty) {
    auto qr = exec_ok("SELECT title FROM TRAVERSE authored FROM posts(10) DIRECTION OUT");
    EXPECT_EQ(qr.rows.size(), 0u);
}

/// FROM table that is not an endpoint of the edge type → error.
TEST_F(QA_GDB267_HeteroTraversal, Error_FromTableNotEndpoint) {
    // "follows" is users→users, posts is not an endpoint.
    exec_error("SELECT * FROM TRAVERSE follows FROM posts(10) DIRECTION OUT",
               StatusCode::TYPE_ERROR);
}

/// Non-existent edge type → error.
TEST_F(QA_GDB267_HeteroTraversal, Error_NonExistentEdgeType) {
    auto result =
        engine_->execute("SELECT * FROM TRAVERSE nonexistent FROM users(1) DIRECTION OUT");
    EXPECT_FALSE(result.has_value());
}

/// Non-existent FROM table → error.
TEST_F(QA_GDB267_HeteroTraversal, Error_NonExistentFromTable) {
    auto result = engine_->execute("SELECT * FROM TRAVERSE authored FROM ghosts(1) DIRECTION OUT");
    EXPECT_FALSE(result.has_value());
}

// ============================================================================
// Adversarial: WHERE filter on target table columns
// ============================================================================

/// WHERE filter on target-table column (posts.title) in heterogeneous traversal.
TEST_F(QA_GDB267_HeteroTraversal, Where_FilterOnTargetColumn) {
    auto qr = exec_ok(
        "SELECT title FROM TRAVERSE authored FROM users(1) DIRECTION OUT WHERE title = 'Hello'");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Hello");
}

/// WHERE filter on meta-column __depth.
TEST_F(QA_GDB267_HeteroTraversal, Where_FilterOnMetaColumn) {
    auto qr = exec_ok("SELECT title, __depth FROM TRAVERSE authored FROM users(1) DIRECTION OUT "
                      "WHERE __depth = 1");

    // All results are at depth 1 for single-hop edges.
    ASSERT_EQ(qr.rows.size(), 2u);
}

/// WHERE filter that eliminates all rows → empty result.
TEST_F(QA_GDB267_HeteroTraversal, Where_FilterEliminatesAllRows) {
    auto qr = exec_ok("SELECT title FROM TRAVERSE authored FROM users(1) DIRECTION OUT "
                      "WHERE title = 'nonexistent'");

    EXPECT_EQ(qr.rows.size(), 0u);
}

/// Referencing a source-table column name that doesn't exist in target → error.
/// "name" exists in users but NOT in posts.
TEST_F(QA_GDB267_HeteroTraversal, Where_ReferenceSourceColumnInTargetContextFails) {
    auto result =
        engine_->execute("SELECT name FROM TRAVERSE authored FROM users(1) DIRECTION OUT");
    // "name" is a users column, but OUT traversal returns posts columns.
    EXPECT_FALSE(result.has_value());
}

// ============================================================================
// Adversarial: Edge properties with heterogeneous traversal
// ============================================================================

/// Edge properties are included in heterogeneous OUT traversal.
TEST_F(QA_GDB267_HeteroTraversal, EdgeProps_IncludedInHeteroOut) {
    auto qr = exec_ok(
        "SELECT title, rated.score, rated.comment FROM TRAVERSE rated FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Hello");
    EXPECT_EQ(qr.rows[0][1].as_int64(), 5);
    EXPECT_EQ(qr.rows[0][2].as_string(), "great");
}

/// Edge properties are included in heterogeneous IN traversal.
TEST_F(QA_GDB267_HeteroTraversal, EdgeProps_IncludedInHeteroIn) {
    auto qr = exec_ok(
        "SELECT name, rated.score, rated.comment FROM TRAVERSE rated FROM posts(20) DIRECTION IN");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Bob");
    EXPECT_EQ(qr.rows[0][1].as_int64(), 3);
    EXPECT_EQ(qr.rows[0][2].as_string(), "okay");
}

/// SELECT * with edge properties on heterogeneous edge includes all columns.
TEST_F(QA_GDB267_HeteroTraversal, EdgeProps_SelectStarIncludesEdgeProps) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE rated FROM users(1) DIRECTION OUT");

    // posts: id, title, body + meta: __node, __depth, __source + edge props: score, comment = 8.
    ASSERT_EQ(qr.column_names.size(), 8u);
    EXPECT_EQ(qr.column_names[6], "score");
    EXPECT_EQ(qr.column_names[7], "comment");
}

// ============================================================================
// Adversarial: Multi-hop on homogeneous with BOTH
// ============================================================================

/// Multi-hop BOTH traversal discovers transitive neighbors.
/// Chain: 1→2→3. From user 1, BOTH should reach user 2 (depth 1) and user 3 (depth 2).
TEST_F(QA_GDB267_HeteroTraversal, MultiHop_BothOnHomogeneousChain) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION BOTH");

    ASSERT_EQ(qr.rows.size(), 2u);

    // Collect (name, depth) pairs.
    std::vector<std::pair<std::string, int64_t>> results;
    for (const auto& row : qr.rows) {
        results.emplace_back(row[0].as_string(), row[1].as_int64());
    }
    std::sort(results.begin(), results.end());
    EXPECT_EQ(results[0].first, "Bob");
    EXPECT_EQ(results[0].second, 1);
    EXPECT_EQ(results[1].first, "Charlie");
    EXPECT_EQ(results[1].second, 2);
}

/// Multi-hop BOTH on homogeneous with default depth returns all reachable.
TEST_F(QA_GDB267_HeteroTraversal, MultiHop_BothDefaultDepthReturnsAll) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION BOTH");

    // Chain 1→2→3. From user 1, BOTH should find user 2 (depth 1), user 3 (depth 2).
    ASSERT_EQ(qr.rows.size(), 2u);
}

// ============================================================================
// Adversarial: __source meta-column correctness
// ============================================================================

/// __source should be the node from which we reached the current node.
TEST_F(QA_GDB267_HeteroTraversal, MetaSource_CorrectForHeteroOut) {
    auto qr = exec_ok(
        "SELECT title, __node, __source FROM TRAVERSE authored FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr.rows.size(), 2u);
    for (const auto& row : qr.rows) {
        // __source should be 1 (user 1 is the start node that reached these posts).
        EXPECT_EQ(get_int(row[2]), 1);
    }
}

/// __source for IN direction should be the post node that led to the user.
TEST_F(QA_GDB267_HeteroTraversal, MetaSource_CorrectForHeteroIn) {
    auto qr =
        exec_ok("SELECT name, __node, __source FROM TRAVERSE authored FROM posts(10) DIRECTION IN");

    ASSERT_EQ(qr.rows.size(), 1u);
    // __source should be 10 (post 10 is the start node).
    EXPECT_EQ(get_int(qr.rows[0][2]), 10);
}

// ============================================================================
// Adversarial: Stress test — many edges
// ============================================================================

/// Create a larger dataset with PKs starting above the source PK range.
/// Uses PKs 100+ to avoid the PK collision bug (see PKCollision test below).
TEST_F(QA_GDB267_HeteroTraversal, Stress_ManyEdgesNonOverlappingPKs) {
    // Create a "comments" table with PKs starting at 100 to avoid collision.
    exec_ok("CREATE TABLE comments (id INT PRIMARY KEY, text VARCHAR)");
    exec_ok("CREATE EDGE TYPE commented FROM users TO comments");

    for (int i = 100; i <= 149; ++i) {
        exec_ok("INSERT INTO comments VALUES (" + std::to_string(i) + ", 'comment" +
                std::to_string(i) + "')");
        exec_ok("LINK users(1) TO comments(" + std::to_string(i) + ") VIA commented");
    }

    auto qr = exec_ok("SELECT text FROM TRAVERSE commented FROM users(1) DIRECTION OUT");
    EXPECT_EQ(qr.rows.size(), 50u);

    // Verify IN direction from one comment.
    auto qr_in = exec_ok("SELECT name FROM TRAVERSE commented FROM comments(125) DIRECTION IN");
    ASSERT_EQ(qr_in.rows.size(), 1u);
    EXPECT_EQ(qr_in.rows[0][0].as_string(), "Alice");

    // BOTH should be rejected for heterogeneous.
    exec_error("SELECT * FROM TRAVERSE commented FROM users(1) DIRECTION BOTH",
               StatusCode::TYPE_ERROR);
}

// ============================================================================
// BUG: PK collision in BFS visited set for heterogeneous edges
// ============================================================================

/// BUG: When the start node's PK value collides with a target table row's PK,
/// the target row is silently skipped. The BFS visited set uses raw Value
/// without distinguishing which table a node belongs to.
///
/// Example: users(1) → posts(1) via "authored".
/// The BFS starts with visited = {Value(1)} (user 1's PK).
/// When it finds neighbor posts(1), it sees Value(1) is already visited → skip.
/// This causes missing rows in heterogeneous traversal results.
TEST_F(QA_GDB267_HeteroTraversal, Bug_PKCollisionInVisitedSet) {
    // Insert a post with PK = 1 (same as user 1's PK).
    exec_ok("INSERT INTO posts VALUES (1, 'Collision Post', 'body_collision')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    // User 1 authored posts: 10, 20, AND 1 (the new one).
    auto qr = exec_ok("SELECT id, title FROM TRAVERSE authored FROM users(1) DIRECTION OUT");

    // Regression test for heterogeneous BFS visited-set PK collision (fixed in
    // enriched_traversal.cpp via GDB-694/696): for heterogeneous edges the start
    // node lives in a different table than the target nodes, so its PK must NOT
    // be seeded into the visited set — doing so would suppress any target node
    // whose PK happens to equal the start node's PK.
    // Fix: `if (!heterogeneous_) { visited.insert(config_.start_key); }`.
    // Expected: all 3 posts (10, 20, 1) are returned.
    std::vector<int32_t> ids;
    for (const auto& row : qr.rows) {
        ids.push_back(row[0].as_int32());
    }
    std::sort(ids.begin(), ids.end());

    EXPECT_EQ(qr.rows.size(), 3u) << "BFS visited set PK collision: post(1) skipped because "
                                     "user(1) has the same PK value in visited set";
}

/// Same PK collision bug but with IN direction.
/// Start from posts(1) IN → should find user 1. But if posts(1) PK=1
/// collides with target user(1) PK=1, the result could be affected.
TEST_F(QA_GDB267_HeteroTraversal, Bug_PKCollisionInDirection) {
    exec_ok("INSERT INTO posts VALUES (1, 'Collision Post', 'body_collision')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    // Regression test for heterogeneous BFS PK collision in IN direction (fixed
    // by the same heterogeneous-guard in enriched_traversal.cpp). Start key is
    // Value(1) for posts(1); without the fix the visited set would contain
    // Value(1) and then skip user(1) whose PK is also Value(1).
    // Expected: user 1 ("Alice") is returned.
    auto qr = exec_ok("SELECT name FROM TRAVERSE authored FROM posts(1) DIRECTION IN");

    EXPECT_EQ(qr.rows.size(), 1u) << "BFS visited set PK collision: user(1) skipped because "
                                     "start node posts(1) has the same PK value";
}

// ============================================================================
// Adversarial: Multiple source nodes authoring the same post
// ============================================================================

/// Two users link to the same post. IN from that post should return both.
TEST_F(QA_GDB267_HeteroTraversal, MultipleAuthors_InReturnsAll) {
    // Both user 1 and user 2 authored post 10 (user 1 already linked in SetUp).
    exec_ok("LINK users(2) TO posts(10) VIA authored");

    auto qr = exec_ok("SELECT name FROM TRAVERSE authored FROM posts(10) DIRECTION IN");
    ASSERT_EQ(qr.rows.size(), 2u);

    std::vector<std::string> names;
    for (const auto& row : qr.rows) {
        names.push_back(row[0].as_string());
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names[0], "Alice");
    EXPECT_EQ(names[1], "Bob");
}

} // namespace
} // namespace sixseven
