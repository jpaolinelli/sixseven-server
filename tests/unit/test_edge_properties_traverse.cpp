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

namespace sixseven {
namespace {

/// Test fixture for edge properties in enriched TRAVERSE output.
///
/// Builds graph with edge properties:
///   users: (1, "Alice"), (2, "Bob"), (3, "Jane")
///   edge type "follows" with a weight FLOAT64 property
///   edges: 1->2 (weight=1.5), 1->3 (weight=2.5), 2->3 (weight=3.5)
class EdgePropsTraverseTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_edge_props_trav";
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

        // Create edge type with a weight property.
        exec_ok("CREATE EDGE TYPE follows (weight DOUBLE) FROM users TO users");

        // Link with property values.
        exec_ok("LINK users(1) TO users(2) VIA follows (weight = 1.5)");
        exec_ok("LINK users(1) TO users(3) VIA follows (weight = 2.5)");
        exec_ok("LINK users(2) TO users(3) VIA follows (weight = 3.5)");
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

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ============================================================================
// 1. Select edge property by qualified name
// ============================================================================

TEST_F(EdgePropsTraverseTest, SelectEdgeProperty) {
    auto qr =
        exec_ok("SELECT follows.weight, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "weight");
    EXPECT_EQ(qr.column_names[1], "__depth");

    // BFS from 1: 2 (depth 1), 3 (depth 1 or 2 depending on visit order)
    ASSERT_GE(qr.rows.size(), 2u);

    // Collect weight values by depth.
    std::vector<double> depth1_weights;
    for (const auto& row : qr.rows) {
        if (row[1].as_int64() == 1) {
            depth1_weights.push_back(row[0].as_float64());
        }
    }
    std::sort(depth1_weights.begin(), depth1_weights.end());

    // Depth 1: nodes 2 and 3 reached via edges with weight 1.5 and 2.5.
    ASSERT_EQ(depth1_weights.size(), 2u);
    EXPECT_DOUBLE_EQ(depth1_weights[0], 1.5);
    EXPECT_DOUBLE_EQ(depth1_weights[1], 2.5);
}

// ============================================================================
// 2. Edge properties correspond to the edge traversed to reach each node
// ============================================================================

TEST_F(EdgePropsTraverseTest, EdgePropertyCorrespondsToTraversedEdge) {
    auto qr = exec_ok(
        "SELECT name, follows.weight, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    ASSERT_GE(qr.rows.size(), 2u);

    // Depth-1 nodes: Bob reached via edge weight=1.5, Jane via weight=2.5.
    for (const auto& row : qr.rows) {
        if (row[2].as_int64() == 1) {
            if (row[0].as_string() == "Bob") {
                EXPECT_DOUBLE_EQ(row[1].as_float64(), 1.5);
            } else if (row[0].as_string() == "Jane") {
                EXPECT_DOUBLE_EQ(row[1].as_float64(), 2.5);
            }
        }
    }
}

// ============================================================================
// 3. SELECT * includes edge property columns after meta-columns
// ============================================================================

TEST_F(EdgePropsTraverseTest, SelectStarIncludesEdgeProperties) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    // Expected columns: id, name, __node, __depth, __source, weight
    ASSERT_GE(qr.column_names.size(), 6u);
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "name");
    EXPECT_EQ(qr.column_names[2], "__node");
    EXPECT_EQ(qr.column_names[3], "__depth");
    EXPECT_EQ(qr.column_names[4], "__source");
    EXPECT_EQ(qr.column_names[5], "weight");

    // Verify weight values are populated.
    for (const auto& row : qr.rows) {
        if (row[3].as_int64() == 1) {
            EXPECT_FALSE(row[5].is_null()) << "weight should be populated for depth-1 nodes";
        }
    }
}

// ============================================================================
// 4. Edge type with zero properties — no extra columns
// ============================================================================

TEST_F(EdgePropsTraverseTest, ZeroPropertiesNoExtraColumns) {
    // Create a separate edge type with no properties.
    exec_ok("CREATE EDGE TYPE knows FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA knows");

    auto qr = exec_ok("SELECT * FROM TRAVERSE knows FROM users(1) DIRECTION OUT");

    // Expected columns: id, name, __node, __depth, __source (no extra).
    ASSERT_EQ(qr.column_names.size(), 5u);
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "name");
    EXPECT_EQ(qr.column_names[2], "__node");
    EXPECT_EQ(qr.column_names[3], "__depth");
    EXPECT_EQ(qr.column_names[4], "__source");
}

// ============================================================================
// 5. Edge type with multiple properties
// ============================================================================

TEST_F(EdgePropsTraverseTest, MultipleProperties) {
    exec_ok("CREATE EDGE TYPE rates (score INT, comment VARCHAR) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA rates (score = 5, comment = 'great')");
    exec_ok("LINK users(1) TO users(3) VIA rates (score = 3, comment = 'ok')");

    auto qr = exec_ok(
        "SELECT rates.score, rates.comment FROM TRAVERSE rates FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "score");
    EXPECT_EQ(qr.column_names[1], "comment");

    ASSERT_EQ(qr.rows.size(), 2u);

    // Collect scores.
    std::vector<int64_t> scores;
    std::vector<std::string> comments;
    for (const auto& row : qr.rows) {
        scores.push_back(row[0].as_int64());
        comments.push_back(row[1].as_string());
    }
    std::sort(scores.begin(), scores.end());

    EXPECT_EQ(scores[0], 3);
    EXPECT_EQ(scores[1], 5);
}

// ============================================================================
// 6. SELECT * with multiple properties — correct column order
// ============================================================================

TEST_F(EdgePropsTraverseTest, MultiplePropertiesSelectStar) {
    exec_ok("CREATE EDGE TYPE rates (score INT, comment VARCHAR) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA rates (score = 5, comment = 'great')");

    auto qr = exec_ok("SELECT * FROM TRAVERSE rates FROM users(1) DIRECTION OUT");

    // id, name, __node, __depth, __source, score, comment
    ASSERT_GE(qr.column_names.size(), 7u);
    EXPECT_EQ(qr.column_names[5], "score");
    EXPECT_EQ(qr.column_names[6], "comment");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][5].as_int64(), 5);
    EXPECT_EQ(qr.rows[0][6].as_string(), "great");
}

// ============================================================================
// 7. Edge property access with explicit alias — edge_type.property syntax
//    GDB-606: rated.score must work even when the TRAVERSE has AS alias.
// ============================================================================

TEST_F(EdgePropsTraverseTest, EdgePropertyWithExplicitAlias) {
    auto qr = exec_ok("SELECT follows.weight, __depth "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT AS t");

    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "weight");
    EXPECT_EQ(qr.column_names[1], "__depth");

    // BFS from 1: 2 and 3 at depth 1.
    std::vector<double> depth1_weights;
    for (const auto& row : qr.rows) {
        if (row[1].as_int64() == 1) {
            depth1_weights.push_back(row[0].as_float64());
        }
    }
    std::sort(depth1_weights.begin(), depth1_weights.end());

    ASSERT_EQ(depth1_weights.size(), 2u);
    EXPECT_DOUBLE_EQ(depth1_weights[0], 1.5);
    EXPECT_DOUBLE_EQ(depth1_weights[1], 2.5);
}

// ============================================================================
// 8. Edge property + table column with explicit alias
// ============================================================================

TEST_F(EdgePropsTraverseTest, EdgePropertyAndTableColumnWithAlias) {
    auto qr = exec_ok(
        "SELECT name, follows.weight "
        "FROM TRAVERSE follows FROM users(1) DIRECTION OUT AS t");

    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "name");
    EXPECT_EQ(qr.column_names[1], "weight");

    ASSERT_GE(qr.rows.size(), 2u);

    // Verify weight values are populated for depth-1 nodes.
    for (const auto& row : qr.rows) {
        EXPECT_FALSE(row[1].is_null()) << "weight should be populated";
    }
}

// ============================================================================
// 9. Edge property access with explicit alias — multiple properties
// ============================================================================

TEST_F(EdgePropsTraverseTest, MultiplePropertiesWithExplicitAlias) {
    exec_ok("CREATE EDGE TYPE rates (score INT, comment VARCHAR) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA rates (score = 5, comment = 'great')");
    exec_ok("LINK users(1) TO users(3) VIA rates (score = 3, comment = 'ok')");

    auto qr = exec_ok("SELECT rates.score, rates.comment "
                      "FROM TRAVERSE rates FROM users(1) DIRECTION OUT AS r");

    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "score");
    EXPECT_EQ(qr.column_names[1], "comment");

    ASSERT_EQ(qr.rows.size(), 2u);

    std::vector<int64_t> scores;
    for (const auto& row : qr.rows) {
        scores.push_back(row[0].as_int64());
    }
    std::sort(scores.begin(), scores.end());

    EXPECT_EQ(scores[0], 3);
    EXPECT_EQ(scores[1], 5);
}

} // namespace
} // namespace sixseven
