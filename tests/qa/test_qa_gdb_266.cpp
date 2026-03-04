/// @file test_qa_gdb_266.cpp
/// QA adversarial tests for GDB-266: Edge properties in enriched TRAVERSE output.
///
/// Tests cover:
///   - AC1: SELECT edge_property returns correct values
///   - AC2: Edge properties correspond to the BFS edge (shortest path edge)
///   - AC3: Edge properties appear after meta-columns in SELECT *
///   - AC4: Edge properties are nullable
///   - AC5: Edge types with zero properties (no extra columns)
///   - AC6: Edge types with multiple properties
///   - Boundary: LINK without properties when edge has defined properties (should fail)
///   - Boundary: DIRECTION IN with edge properties
///   - Boundary: DIRECTION BOTH with edge properties
///   - Boundary: Dangling edges with edge properties
///   - Boundary: WHERE filter on edge property
///   - Boundary: ORDER BY edge property
///   - Boundary: Qualified column reference (follows.weight)
///   - Boundary: Aliased TRAVERSE with edge property references
///   - Stress: Many nodes with edge properties

#include "giodb/catalog/catalog.h"
#include "giodb/common/result.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/query_engine.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/graph/graph_engine.h"
#include "giodb/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace giodb {
namespace {

// ============================================================================
// Fixture: builds users table + follows edges WITH properties
// ============================================================================

class QA_GDB266 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "giodb_qa_gdb266";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        // Create table and insert rows.
        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");
        exec_ok("INSERT INTO users VALUES (3, 'Jane')");
        exec_ok("INSERT INTO users VALUES (4, 'Jake')");
        exec_ok("INSERT INTO users VALUES (5, 'Zara')");
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
            return QueryResult{}; // Return empty result on failure.
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

    void exec_error(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
    }

    /// Find column index by name (asserts it exists).
    size_t col_idx(const QueryResult& qr, const std::string& name) {
        for (size_t i = 0; i < qr.column_names.size(); ++i) {
            if (qr.column_names[i] == name)
                return i;
        }
        ADD_FAILURE() << "column not found: " << name;
        return 0;
    }

    /// Helper to extract integer value from edge property regardless of
    /// underlying type (INT32 or INT64). Edge properties may store values
    /// as INT64 (literal type) even when schema declares INT32.
    int64_t get_int(const Value& v) {
        if (v.is_null()) {
            ADD_FAILURE() << "expected integer value, got NULL";
            return 0;
        }
        // Try INT64 first (literal type), then INT32.
        try {
            return v.as_int64();
        } catch (...) {}
        try {
            return static_cast<int64_t>(v.as_int32());
        } catch (...) {}
        ADD_FAILURE() << "value is not an integer";
        return 0;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ============================================================================
// AC1: SELECT follows.weight, __depth returns edge property values
// ============================================================================

TEST_F(QA_GDB266, AC1_SelectEdgePropertyReturnsValues) {
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 10)");
    exec_ok("LINK users(1) TO users(3) VIA follows (weight = 20)");

    auto qr = exec_ok("SELECT name, weight, __depth "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "ORDER BY name");

    ASSERT_EQ(qr.rows.size(), 2u);
    ASSERT_EQ(qr.rows[0].size(), 3u);

    // Bob (weight=10), Jane (weight=20).
    EXPECT_EQ(qr.rows[0][0].as_string(), "Bob");
    EXPECT_FALSE(qr.rows[0][1].is_null()) << "weight should not be NULL";
    EXPECT_EQ(get_int(qr.rows[0][1]), 10);
    EXPECT_EQ(qr.rows[0][2].as_int64(), 1);

    EXPECT_EQ(qr.rows[1][0].as_string(), "Jane");
    EXPECT_FALSE(qr.rows[1][1].is_null()) << "weight should not be NULL";
    EXPECT_EQ(get_int(qr.rows[1][1]), 20);
    EXPECT_EQ(qr.rows[1][2].as_int64(), 1);
}

// ============================================================================
// AC2: Edge properties correspond to BFS shortest path edge
// ============================================================================

TEST_F(QA_GDB266, AC2_EdgePropertiesFromShortestPathEdge) {
    // Diamond graph: 1→2 (w=10), 1→3 (w=20), 2→4 (w=30), 3→4 (w=40).
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 10)");
    exec_ok("LINK users(1) TO users(3) VIA follows (weight = 20)");
    exec_ok("LINK users(2) TO users(4) VIA follows (weight = 30)");
    exec_ok("LINK users(3) TO users(4) VIA follows (weight = 40)");

    auto qr = exec_ok("SELECT __node, __depth, weight "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "ORDER BY __node");

    ASSERT_EQ(qr.rows.size(), 3u);

    // Node 2 reached via 1→2 (weight=10).
    EXPECT_EQ(get_int(qr.rows[0][0]), 2);
    EXPECT_EQ(get_int(qr.rows[0][2]), 10);

    // Node 3 reached via 1→3 (weight=20).
    EXPECT_EQ(get_int(qr.rows[1][0]), 3);
    EXPECT_EQ(get_int(qr.rows[1][2]), 20);

    // Node 4: BFS order may pick 2→4 or 3→4.
    EXPECT_EQ(get_int(qr.rows[2][0]), 4);
    auto weight_for_4 = get_int(qr.rows[2][2]);
    EXPECT_TRUE(weight_for_4 == 30 || weight_for_4 == 40)
        << "node 4 weight should be 30 or 40, got " << weight_for_4;
}

// ============================================================================
// AC3: Edge properties appear after meta-columns in SELECT *
// ============================================================================

TEST_F(QA_GDB266, AC3_SelectStarShowsEdgePropertiesAfterMeta) {
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 42)");

    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    // Expected columns: id, name, __node, __depth, __source, weight.
    ASSERT_EQ(qr.column_names.size(), 6u);
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "name");
    EXPECT_EQ(qr.column_names[2], "__node");
    EXPECT_EQ(qr.column_names[3], "__depth");
    EXPECT_EQ(qr.column_names[4], "__source");
    EXPECT_EQ(qr.column_names[5], "weight");

    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.rows[0].size(), 6u);
    EXPECT_EQ(get_int(qr.rows[0][5]), 42);
}

// ============================================================================
// AC4: Edge properties are nullable
// ============================================================================

TEST_F(QA_GDB266, AC4_EdgePropertiesNullableForDifferentPaths) {
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 10)");
    exec_ok("LINK users(2) TO users(3) VIA follows (weight = 20)");

    auto qr = exec_ok("SELECT __node, weight "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "ORDER BY __node");

    ASSERT_EQ(qr.rows.size(), 2u);

    // Node 2: weight from 1→2 = 10.
    EXPECT_EQ(get_int(qr.rows[0][0]), 2);
    EXPECT_EQ(get_int(qr.rows[0][1]), 10);

    // Node 3: weight from 2→3 = 20.
    EXPECT_EQ(get_int(qr.rows[1][0]), 3);
    EXPECT_EQ(get_int(qr.rows[1][1]), 20);
}

// ============================================================================
// AC5: Edge types with zero properties — no extra columns added
// ============================================================================

TEST_F(QA_GDB266, AC5_ZeroPropertiesNoExtraColumns) {
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows");

    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    // Should have exactly: id, name, __node, __depth, __source (5 columns).
    ASSERT_EQ(qr.column_names.size(), 5u);
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "name");
    EXPECT_EQ(qr.column_names[2], "__node");
    EXPECT_EQ(qr.column_names[3], "__depth");
    EXPECT_EQ(qr.column_names[4], "__source");

    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.rows[0].size(), 5u);
}

// ============================================================================
// AC6: Edge types with multiple properties
// ============================================================================

TEST_F(QA_GDB266, AC6_MultipleEdgeProperties) {
    exec_ok("CREATE EDGE TYPE follows (weight INT, label VARCHAR) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 10, label = 'friend')");
    exec_ok("LINK users(1) TO users(3) VIA follows (weight = 5, label = 'colleague')");

    auto qr = exec_ok("SELECT name, weight, label "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "ORDER BY name");

    ASSERT_EQ(qr.rows.size(), 2u);
    ASSERT_EQ(qr.column_names.size(), 3u);

    // Bob: weight=10, label='friend'.
    EXPECT_EQ(qr.rows[0][0].as_string(), "Bob");
    EXPECT_EQ(get_int(qr.rows[0][1]), 10);
    EXPECT_EQ(qr.rows[0][2].as_string(), "friend");

    // Jane: weight=5, label='colleague'.
    EXPECT_EQ(qr.rows[1][0].as_string(), "Jane");
    EXPECT_EQ(get_int(qr.rows[1][1]), 5);
    EXPECT_EQ(qr.rows[1][2].as_string(), "colleague");
}

// ============================================================================
// SELECT * with multiple properties shows correct order
// ============================================================================

TEST_F(QA_GDB266, SelectStarMultiplePropertiesOrder) {
    exec_ok("CREATE EDGE TYPE follows (weight INT, label VARCHAR) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 10, label = 'test')");

    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    // id, name, __node, __depth, __source, weight, label.
    ASSERT_EQ(qr.column_names.size(), 7u);
    EXPECT_EQ(qr.column_names[5], "weight");
    EXPECT_EQ(qr.column_names[6], "label");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(get_int(qr.rows[0][5]), 10);
    EXPECT_EQ(qr.rows[0][6].as_string(), "test");
}

// ============================================================================
// Edge properties with FLOAT type
// ============================================================================

TEST_F(QA_GDB266, FloatEdgeProperty) {
    exec_ok("CREATE EDGE TYPE follows (score DOUBLE) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (score = 3.14)");

    auto qr = exec_ok("SELECT name, score "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Bob");
    EXPECT_FALSE(qr.rows[0][1].is_null());
    EXPECT_NEAR(qr.rows[0][1].as_float64(), 3.14, 0.01);
}

// ============================================================================
// WHERE filter on edge property
// ============================================================================

TEST_F(QA_GDB266, WhereOnEdgeProperty) {
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 10)");
    exec_ok("LINK users(1) TO users(3) VIA follows (weight = 5)");

    auto qr = exec_ok("SELECT name, weight "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "WHERE weight > 7");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Bob");
    EXPECT_EQ(get_int(qr.rows[0][1]), 10);
}

// ============================================================================
// ORDER BY edge property
// ============================================================================

TEST_F(QA_GDB266, OrderByEdgeProperty) {
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 50)");
    exec_ok("LINK users(1) TO users(3) VIA follows (weight = 10)");
    exec_ok("LINK users(2) TO users(4) VIA follows (weight = 30)");

    auto qr = exec_ok("SELECT name, weight "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "ORDER BY weight");

    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(get_int(qr.rows[0][1]), 10);
    EXPECT_EQ(get_int(qr.rows[1][1]), 30);
    EXPECT_EQ(get_int(qr.rows[2][1]), 50);
}

// ============================================================================
// Qualified column reference: follows.weight
// ============================================================================

TEST_F(QA_GDB266, QualifiedEdgePropertyReference) {
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 42)");

    auto qr = exec_ok("SELECT follows.weight "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(get_int(qr.rows[0][0]), 42);
}

// ============================================================================
// Aliased TRAVERSE with edge property — t.weight should work
// ============================================================================

TEST_F(QA_GDB266, AliasedTraverseEdgeProperty) {
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 99)");

    // When TRAVERSE is aliased as 't', edge properties should also be
    // accessible via 't.weight' (same as t.name and t.__depth).
    // The binder sets table_name = trav->edge_type for edge properties,
    // while other columns use alias. This test verifies if the alias works.
    auto result = engine_->execute("SELECT t.name, t.weight "
                                   "FROM TRAVERSE follows FROM users(1) DIRECTION OUT AS t");

    if (result.has_value()) {
        // If it works, verify the values.
        ASSERT_EQ(result->rows.size(), 1u);
        EXPECT_EQ(result->rows[0][0].as_string(), "Bob");
        EXPECT_EQ(get_int(result->rows[0][1]), 99);
    } else {
        // If it fails, this is a bug: edge properties aren't accessible via alias.
        ADD_FAILURE() << "BUG: Edge properties not accessible via TRAVERSE alias. "
                      << "t.weight should work but got: " << result.error().message;
    }
}

// ============================================================================
// DIRECTION IN: edge properties from incoming edges
// ============================================================================

TEST_F(QA_GDB266, DirectionInEdgeProperties) {
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 10)");
    exec_ok("LINK users(3) TO users(2) VIA follows (weight = 20)");

    // From user 2 going IN: reaches 1 (via 1→2, w=10) and 3 (via 3→2, w=20).
    auto qr = exec_ok("SELECT name, weight "
                      "FROM TRAVERSE follows FROM users(2) DIRECTION IN "
                      "ORDER BY name");

    ASSERT_EQ(qr.rows.size(), 2u);

    // Alice reached via edge 1→2 (weight=10).
    EXPECT_EQ(qr.rows[0][0].as_string(), "Alice");
    EXPECT_EQ(get_int(qr.rows[0][1]), 10);

    // Jane reached via edge 3→2 (weight=20).
    EXPECT_EQ(qr.rows[1][0].as_string(), "Jane");
    EXPECT_EQ(get_int(qr.rows[1][1]), 20);
}

// ============================================================================
// DIRECTION BOTH: edge properties from bidirectional traversal
// ============================================================================

TEST_F(QA_GDB266, DirectionBothEdgeProperties) {
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 10)");
    exec_ok("LINK users(3) TO users(2) VIA follows (weight = 20)");

    // From user 2 BOTH: reaches 1 and 3.
    auto qr = exec_ok("SELECT __node, weight "
                      "FROM TRAVERSE follows FROM users(2) DIRECTION BOTH "
                      "ORDER BY __node");

    ASSERT_EQ(qr.rows.size(), 2u);

    EXPECT_EQ(get_int(qr.rows[0][0]), 1);
    EXPECT_EQ(get_int(qr.rows[1][0]), 3);

    // Both should have non-null weights.
    EXPECT_FALSE(qr.rows[0][1].is_null());
    EXPECT_FALSE(qr.rows[1][1].is_null());
}

// ============================================================================
// Dangling edge: edge properties still present even if target row is missing
// ============================================================================

TEST_F(QA_GDB266, DanglingEdgeWithProperties) {
    // GDB-315 added PK validation — dangling edges (pointing to non-existent
    // rows) can no longer be created.  Verify LINK to non-existent PK fails.
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 10)");

    auto r = engine_->execute("LINK users(1) TO users(999) VIA follows (weight = 77)");
    EXPECT_FALSE(r.has_value()) << "LINK to non-existent PK should fail";

    // Only the valid edge should exist.
    auto qr = exec_ok("SELECT id, name, weight "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 2);
    EXPECT_EQ(qr.rows[0][1].as_string(), "Bob");
    EXPECT_EQ(get_int(qr.rows[0][2]), 10);
}

// ============================================================================
// LINK without properties when edge type has defined properties should fail
// ============================================================================

TEST_F(QA_GDB266, LinkWithoutPropertiesFails) {
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_error("LINK users(1) TO users(2) VIA follows");
}

// ============================================================================
// LINK with wrong number of properties should fail
// ============================================================================

TEST_F(QA_GDB266, LinkWrongPropertyCountFails) {
    exec_ok("CREATE EDGE TYPE follows (weight INT, label VARCHAR) FROM users TO users");
    exec_error("LINK users(1) TO users(2) VIA follows (weight = 10)");
}

// ============================================================================
// Multi-hop traversal: edge properties at each depth
// ============================================================================

TEST_F(QA_GDB266, MultiHopEdgeProperties) {
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 10)");
    exec_ok("LINK users(2) TO users(3) VIA follows (weight = 20)");
    exec_ok("LINK users(3) TO users(4) VIA follows (weight = 30)");

    auto qr = exec_ok("SELECT name, __depth, weight "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "ORDER BY __depth");

    ASSERT_EQ(qr.rows.size(), 3u);

    // Depth 1: Bob (weight=10).
    EXPECT_EQ(qr.rows[0][0].as_string(), "Bob");
    EXPECT_EQ(qr.rows[0][1].as_int64(), 1);
    EXPECT_EQ(get_int(qr.rows[0][2]), 10);

    // Depth 2: Jane (weight=20).
    EXPECT_EQ(qr.rows[1][0].as_string(), "Jane");
    EXPECT_EQ(qr.rows[1][1].as_int64(), 2);
    EXPECT_EQ(get_int(qr.rows[1][2]), 20);

    // Depth 3: Jake (weight=30).
    EXPECT_EQ(qr.rows[2][0].as_string(), "Jake");
    EXPECT_EQ(qr.rows[2][1].as_int64(), 3);
    EXPECT_EQ(get_int(qr.rows[2][2]), 30);
}

// ============================================================================
// Edge properties in aggregation: SUM(weight)
// ============================================================================

TEST_F(QA_GDB266, AggregateOnEdgeProperty) {
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 10)");
    exec_ok("LINK users(1) TO users(3) VIA follows (weight = 20)");
    exec_ok("LINK users(2) TO users(4) VIA follows (weight = 30)");

    auto qr = exec_ok("SELECT SUM(weight) "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr.rows.size(), 1u);
    // 10 + 20 + 30 = 60.
    EXPECT_EQ(qr.rows[0][0].as_int64(), 60);
}

// ============================================================================
// GROUP BY with edge property
// ============================================================================

TEST_F(QA_GDB266, GroupByEdgeProperty) {
    exec_ok("CREATE EDGE TYPE follows (label VARCHAR) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (label = 'friend')");
    exec_ok("LINK users(1) TO users(3) VIA follows (label = 'colleague')");
    exec_ok("LINK users(2) TO users(4) VIA follows (label = 'friend')");

    auto qr = exec_ok("SELECT label, COUNT(*) "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "GROUP BY label ORDER BY label");

    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "colleague");
    EXPECT_EQ(qr.rows[0][1].as_int64(), 1);
    EXPECT_EQ(qr.rows[1][0].as_string(), "friend");
    EXPECT_EQ(qr.rows[1][1].as_int64(), 2);
}

// ============================================================================
// MAX_DEPTH with edge properties
// ============================================================================

TEST_F(QA_GDB266, MaxDepthWithEdgeProperties) {
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 10)");
    exec_ok("LINK users(2) TO users(3) VIA follows (weight = 20)");
    exec_ok("LINK users(3) TO users(4) VIA follows (weight = 30)");

    auto qr = exec_ok("SELECT name, weight "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 1");

    // Only depth 1: Bob (weight=10).
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Bob");
    EXPECT_EQ(get_int(qr.rows[0][1]), 10);
}

// ============================================================================
// Heterogeneous edge type (different source/target tables) with properties
// ============================================================================

TEST_F(QA_GDB266, HeterogeneousEdgeWithProperties) {
    exec_ok("CREATE TABLE posts (pid INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO posts VALUES (100, 'Hello World')");
    exec_ok("INSERT INTO posts VALUES (101, 'Second Post')");

    exec_ok("CREATE EDGE TYPE authored (rating INT) FROM users TO posts");
    exec_ok("LINK users(1) TO posts(100) VIA authored (rating = 5)");
    exec_ok("LINK users(1) TO posts(101) VIA authored (rating = 3)");

    auto qr = exec_ok("SELECT title, rating "
                      "FROM TRAVERSE authored FROM users(1) DIRECTION OUT "
                      "ORDER BY title");

    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Hello World");
    EXPECT_EQ(get_int(qr.rows[0][1]), 5);
    EXPECT_EQ(qr.rows[1][0].as_string(), "Second Post");
    EXPECT_EQ(get_int(qr.rows[1][1]), 3);
}

// ============================================================================
// Edge properties with heterogeneous edge — IN direction
// ============================================================================

TEST_F(QA_GDB266, HeterogeneousEdgeInDirectionWithProperties) {
    exec_ok("CREATE TABLE posts (pid INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO posts VALUES (100, 'Hello World')");

    exec_ok("CREATE EDGE TYPE authored (rating INT) FROM users TO posts");
    exec_ok("LINK users(1) TO posts(100) VIA authored (rating = 5)");
    exec_ok("LINK users(2) TO posts(100) VIA authored (rating = 8)");

    // IN from post 100: reaches users who authored it.
    auto qr = exec_ok("SELECT name, rating "
                      "FROM TRAVERSE authored FROM posts(100) DIRECTION IN "
                      "ORDER BY name");

    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Alice");
    EXPECT_EQ(get_int(qr.rows[0][1]), 5);
    EXPECT_EQ(qr.rows[1][0].as_string(), "Bob");
    EXPECT_EQ(get_int(qr.rows[1][1]), 8);
}

// ============================================================================
// Empty traversal with properties — schema still correct
// ============================================================================

TEST_F(QA_GDB266, EmptyTraversalSchemaWithProperties) {
    exec_ok("CREATE EDGE TYPE follows (weight INT, label VARCHAR) FROM users TO users");

    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(5) DIRECTION OUT");

    EXPECT_TRUE(qr.rows.empty());
    // Schema should have: id, name, __node, __depth, __source, weight, label.
    ASSERT_EQ(qr.column_names.size(), 7u);
    EXPECT_EQ(qr.column_names[5], "weight");
    EXPECT_EQ(qr.column_names[6], "label");
}

// ============================================================================
// Cycle detection preserves edge properties
// ============================================================================

TEST_F(QA_GDB266, CycleDetectionWithEdgeProperties) {
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 10)");
    exec_ok("LINK users(2) TO users(3) VIA follows (weight = 20)");
    exec_ok("LINK users(3) TO users(1) VIA follows (weight = 30)");

    auto qr = exec_ok("SELECT __node, weight "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "ORDER BY __node");

    // BFS: 1→2 (w=10), 2→3 (w=20). 3→1 is a cycle, skipped.
    ASSERT_EQ(qr.rows.size(), 2u);

    EXPECT_EQ(get_int(qr.rows[0][0]), 2);
    EXPECT_EQ(get_int(qr.rows[0][1]), 10);

    EXPECT_EQ(get_int(qr.rows[1][0]), 3);
    EXPECT_EQ(get_int(qr.rows[1][1]), 20);
}

// ============================================================================
// Expression on edge property: weight * 2
// ============================================================================

TEST_F(QA_GDB266, ExpressionOnEdgeProperty) {
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 15)");

    auto qr = exec_ok("SELECT name, weight * 2 AS doubled "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Bob");
    EXPECT_EQ(qr.rows[0][1].as_int64(), 30);
}

// ============================================================================
// Combined WHERE on table column AND edge property
// ============================================================================

TEST_F(QA_GDB266, WhereCombinedTableAndEdgeProperty) {
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 10)");
    exec_ok("LINK users(1) TO users(3) VIA follows (weight = 5)");

    auto qr = exec_ok("SELECT name, weight "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "WHERE name LIKE 'B%' AND weight > 5");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Bob");
    EXPECT_EQ(get_int(qr.rows[0][1]), 10);
}

// ============================================================================
// Stress: many nodes with edge properties
// ============================================================================

TEST_F(QA_GDB266, StressManyNodesWithProperties) {
    exec_ok("CREATE TABLE chain (id INT PRIMARY KEY, label VARCHAR)");
    exec_ok("CREATE EDGE TYPE next (weight INT) FROM chain TO chain");

    for (int i = 1; i <= 50; ++i) {
        exec_ok("INSERT INTO chain VALUES (" + std::to_string(i) + ", 'node_" + std::to_string(i) +
                "')");
    }
    for (int i = 1; i < 50; ++i) {
        exec_ok("LINK chain(" + std::to_string(i) + ") TO chain(" + std::to_string(i + 1) +
                ") VIA next (weight = " + std::to_string(i * 10) + ")");
    }

    auto qr = exec_ok("SELECT label, weight, __depth "
                      "FROM TRAVERSE next FROM chain(1) DIRECTION OUT");

    // Should get 49 nodes (2 through 50).
    ASSERT_EQ(qr.rows.size(), 49u);

    // Every row should have enriched label and weight.
    for (const auto& row : qr.rows) {
        EXPECT_FALSE(row[0].is_null()) << "label should be enriched";
        EXPECT_FALSE(row[1].is_null()) << "weight should be present";
    }

    // Verify first edge weight = 10, last = 490.
    int64_t min_weight = INT64_MAX;
    int64_t max_weight = INT64_MIN;
    for (const auto& row : qr.rows) {
        auto w = get_int(row[1]);
        min_weight = std::min(min_weight, w);
        max_weight = std::max(max_weight, w);
    }
    EXPECT_EQ(min_weight, 10);
    EXPECT_EQ(max_weight, 490);
}

// ============================================================================
// Repeated execution: same query twice gives same results
// ============================================================================

TEST_F(QA_GDB266, RepeatedExecutionWithProperties) {
    exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 10)");

    auto qr1 = exec_ok("SELECT weight FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    auto qr2 = exec_ok("SELECT weight FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr1.rows.size(), qr2.rows.size());
    for (size_t i = 0; i < qr1.rows.size(); ++i) {
        EXPECT_EQ(get_int(qr1.rows[i][0]), get_int(qr2.rows[i][0]));
    }
}

// ============================================================================
// SELECT only edge properties (no table columns, no meta-columns)
// ============================================================================

TEST_F(QA_GDB266, SelectOnlyEdgeProperties) {
    exec_ok("CREATE EDGE TYPE follows (weight INT, label VARCHAR) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 10, label = 'test')");

    auto qr = exec_ok("SELECT weight, label "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "weight");
    EXPECT_EQ(qr.column_names[1], "label");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(get_int(qr.rows[0][0]), 10);
    EXPECT_EQ(qr.rows[0][1].as_string(), "test");
}

// ============================================================================
// Edge properties with STRING type containing special characters
// ============================================================================

TEST_F(QA_GDB266, StringEdgePropertySpecialChars) {
    exec_ok("CREATE EDGE TYPE follows (label VARCHAR) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (label = 'hello, world')");

    auto qr = exec_ok("SELECT name, label "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "hello, world");
}

} // namespace
} // namespace giodb
