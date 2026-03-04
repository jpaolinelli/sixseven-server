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
#include <set>
#include <string>
#include <vector>

namespace giodb {
namespace {

// ============================================================================
// QA Fixture for GDB-309: Graph-aware view toggle not appearing for TRAVERSE
//
// GDB-309 is a frontend bug where the Nodes/Edges/Graph toggle didn't appear
// for TRAVERSE queries unless __node was explicitly in the SELECT list.
// These tests verify the C++ server-side behavior is correct and that the
// meta-column output is reliable for various TRAVERSE query patterns.
// ============================================================================

class QA_GDB309_GraphViewToggle : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "giodb_qa_gdb309";
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
            EXPECT_EQ(result.error().code, expected);
        }
    }

    /// Set up users table with follows edges (same as GDB-298 tests).
    void setup_users(int count) {
        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        for (int i = 1; i <= count; ++i) {
            exec_ok("INSERT INTO users VALUES (" + std::to_string(i) + ", 'User" +
                    std::to_string(i) + "')");
        }
        exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    }

    void link(int from, int to) {
        exec_ok("LINK users(" + std::to_string(from) + ") TO users(" + std::to_string(to) +
                ") VIA follows");
    }

    /// Check if a column name is present in the result.
    bool has_column(const QueryResult& qr, const std::string& name) const {
        return std::find(qr.column_names.begin(), qr.column_names.end(), name) !=
               qr.column_names.end();
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
        } catch (...) {
        }
        try {
            return static_cast<int64_t>(v.as_int32());
        } catch (...) {
        }
        ADD_FAILURE() << "value is not an integer";
        return 0;
    }

    /// Get column index by name, or -1 if not found.
    int col_idx(const QueryResult& qr, const std::string& name) const {
        for (size_t i = 0; i < qr.column_names.size(); ++i) {
            if (qr.column_names[i] == name) return static_cast<int>(i);
        }
        return -1;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ============================================================================
// User's exact scenario: SELECT name, __depth FROM TRAVERSE ... ORDER BY LIMIT
// The server should return correct data even when user only selects __depth.
// ============================================================================

TEST_F(QA_GDB309_GraphViewToggle, UserScenario_SelectNameDepthWithOrderByLimit) {
    setup_users(3);
    link(1, 2);
    link(1, 3);

    auto qr = exec_ok(
        "SELECT name, __depth "
        "FROM TRAVERSE follows FROM users(1) DIRECTION OUT FETCH AS t "
        "ORDER BY name LIMIT 10");

    // Server should return rows for discovered nodes (2 and 3).
    ASSERT_EQ(qr.rows.size(), 2u);

    // Columns should be exactly what the user SELECTed.
    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "name");
    EXPECT_EQ(qr.column_names[1], "__depth");

    // __depth should be present (this is the meta-column the frontend uses for detection).
    int depth_idx = col_idx(qr, "__depth");
    ASSERT_GE(depth_idx, 0);
    for (const auto& row : qr.rows) {
        EXPECT_EQ(row[depth_idx].as_int64(), 1); // Both nodes at depth 1
    }

    // Verify ORDER BY name works: User2, User3 alphabetically.
    EXPECT_EQ(qr.rows[0][0].as_string(), "User2");
    EXPECT_EQ(qr.rows[1][0].as_string(), "User3");
}

// ============================================================================
// Meta-column presence: verify __node, __depth, __source always available
// when selected via SELECT *
// ============================================================================

TEST_F(QA_GDB309_GraphViewToggle, SelectStar_ContainsAllMetaColumns) {
    setup_users(2);
    link(1, 2);

    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT FETCH");

    EXPECT_TRUE(has_column(qr, "__node")) << "SELECT * should include __node";
    EXPECT_TRUE(has_column(qr, "__depth")) << "SELECT * should include __depth";
    EXPECT_TRUE(has_column(qr, "__source")) << "SELECT * should include __source";
    ASSERT_EQ(qr.rows.size(), 1u);
}

// ============================================================================
// Selective meta-column: only __depth in SELECT
// ============================================================================

TEST_F(QA_GDB309_GraphViewToggle, SelectOnlyDepth_ReturnsDepthValues) {
    setup_users(4);
    link(1, 2);
    link(2, 3);
    link(3, 4);

    auto qr = exec_ok(
        "SELECT __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT FETCH "
        "ORDER BY __depth");

    ASSERT_EQ(qr.rows.size(), 3u);
    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "__depth");

    EXPECT_EQ(qr.rows[0][0].as_int64(), 1); // User2
    EXPECT_EQ(qr.rows[1][0].as_int64(), 2); // User3
    EXPECT_EQ(qr.rows[2][0].as_int64(), 3); // User4
}

// ============================================================================
// Selective meta-column: only __source in SELECT
// ============================================================================

TEST_F(QA_GDB309_GraphViewToggle, SelectOnlySource_ReturnsSourceValues) {
    setup_users(3);
    link(1, 2);
    link(1, 3);

    auto qr = exec_ok(
        "SELECT name, __source FROM TRAVERSE follows FROM users(1) DIRECTION OUT FETCH "
        "ORDER BY name");

    ASSERT_EQ(qr.rows.size(), 2u);
    ASSERT_EQ(qr.column_names.size(), 2u);

    int src_idx = col_idx(qr, "__source");
    ASSERT_GE(src_idx, 0);
    // Source of both nodes should be 1 (the start node).
    EXPECT_EQ(get_int(qr.rows[0][src_idx]), 1);
    EXPECT_EQ(get_int(qr.rows[1][src_idx]), 1);
}

// ============================================================================
// Selective meta-column: only __node in SELECT
// ============================================================================

TEST_F(QA_GDB309_GraphViewToggle, SelectOnlyNode_ReturnsNodePKs) {
    setup_users(3);
    link(1, 2);
    link(1, 3);

    auto qr = exec_ok(
        "SELECT __node FROM TRAVERSE follows FROM users(1) DIRECTION OUT FETCH "
        "ORDER BY __node");

    ASSERT_EQ(qr.rows.size(), 2u);
    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "__node");
    EXPECT_EQ(get_int(qr.rows[0][0]), 2);
    EXPECT_EQ(get_int(qr.rows[1][0]), 3);
}

// ============================================================================
// MODE EDGES with FETCH and ORDER BY/LIMIT — verify correct parse order
// buildEdgeQuery in the frontend naively appends MODE EDGES at the end,
// which would place it after ORDER BY / LIMIT. The correct position is
// before WHERE/FETCH. These tests verify the server accepts correct syntax.
// ============================================================================

TEST_F(QA_GDB309_GraphViewToggle, ModeEdges_CorrectPosition_BeforeFetch) {
    setup_users(3);
    link(1, 2);
    link(1, 3);

    // MODE EDGES before WHERE and FETCH (correct position per grammar).
    auto qr = exec_ok(
        "SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
        "MODE EDGES");

    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_TRUE(has_column(qr, "__from"));
    EXPECT_TRUE(has_column(qr, "__to"));
}

TEST_F(QA_GDB309_GraphViewToggle, ModeEdges_WithOrderByAndLimit) {
    setup_users(4);
    link(1, 2);
    link(1, 3);
    link(2, 4);

    auto qr = exec_ok(
        "SELECT __from, __to, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
        "MODE EDGES ORDER BY __depth DESC LIMIT 2");

    ASSERT_EQ(qr.rows.size(), 2u);
    // Top 2 by depth DESC.
    EXPECT_EQ(qr.rows[0][2].as_int64(), 2); // edge at depth 2
    EXPECT_EQ(qr.rows[1][2].as_int64(), 1); // edge at depth 1
}

TEST_F(QA_GDB309_GraphViewToggle, ModeEdges_AfterOrderByLimit_Fails) {
    setup_users(2);
    link(1, 2);

    // This is what buildEdgeQuery produces: MODE EDGES after ORDER BY LIMIT.
    // The parser doesn't see MODE as a traverse keyword at this position —
    // it gets interpreted as an alias or column reference, causing a binding error.
    auto result = engine_->execute(
        "SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
        "ORDER BY __from LIMIT 10 MODE EDGES");

    // The query should fail because MODE EDGES in this position is not recognized
    // as the traverse mode keyword.
    EXPECT_FALSE(result.has_value())
        << "MODE EDGES after ORDER BY LIMIT should not produce valid edge-centric results";
}

TEST_F(QA_GDB309_GraphViewToggle, ModeEdges_AfterFetchAs_SilentlyIgnored) {
    setup_users(2);
    link(1, 2);

    // This is what buildEdgeQuery produces for the user's exact query.
    // MODE EDGES is placed after FETCH AS alias and ORDER BY — the server
    // accepts this but silently runs in NODE mode (not edge mode).
    auto result = engine_->execute(
        "SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
        "FETCH AS t ORDER BY name LIMIT 10 MODE EDGES");

    if (result.has_value()) {
        // If the query succeeds, it's running in node mode (not edge mode).
        // Verify it does NOT have edge-centric columns.
        EXPECT_FALSE(has_column(*result, "__from"))
            << "MODE EDGES in wrong position should NOT activate edge mode";
        EXPECT_FALSE(has_column(*result, "__to"))
            << "MODE EDGES in wrong position should NOT activate edge mode";

        // It should have node-centric columns from the user's SELECT.
        EXPECT_TRUE(has_column(*result, "name"));
        EXPECT_TRUE(has_column(*result, "__depth"));
    }
    // Whether it fails or silently ignores MODE EDGES, the frontend's
    // buildEdgeQuery approach produces incorrect edge queries.
}

// ============================================================================
// Verify edge-centric query with SELECT * returns expected schema
// ============================================================================

TEST_F(QA_GDB309_GraphViewToggle, ModeEdges_SelectStar_HasFromToDepth) {
    setup_users(2);
    link(1, 2);

    auto qr = exec_ok(
        "SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");

    EXPECT_TRUE(has_column(qr, "__from"));
    EXPECT_TRUE(has_column(qr, "__to"));
    EXPECT_TRUE(has_column(qr, "__depth"));
    // Should NOT have node-centric columns like __node or __source.
    EXPECT_FALSE(has_column(qr, "__node"));
    EXPECT_FALSE(has_column(qr, "__source"));
    ASSERT_EQ(qr.rows.size(), 1u);
}

// ============================================================================
// Verify that user columns like 'name' do NOT exist in edge-centric mode.
// This proves buildEdgeQuery's approach of preserving the user's SELECT list
// is fundamentally broken for edge queries.
// ============================================================================

TEST_F(QA_GDB309_GraphViewToggle, ModeEdges_SelectUserColumn_Error) {
    setup_users(3);
    link(1, 2);
    link(1, 3);

    // 'name' is a table column, not available in edge-centric mode.
    // Edge mode schema is: __from, __to, __depth, [edge_properties...]
    exec_error(
        "SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES",
        StatusCode::NOT_FOUND);
}

// ============================================================================
// Dual-query pattern: verify both node and edge queries return correct data
// for the same graph. This mimics what the frontend should do.
// ============================================================================

TEST_F(QA_GDB309_GraphViewToggle, DualQuery_NodeAndEdge_Consistent) {
    setup_users(4);
    link(1, 2);
    link(1, 3);
    link(2, 4);

    // Node-centric query (what user runs).
    auto nodes = exec_ok(
        "SELECT name, __node, __depth, __source "
        "FROM TRAVERSE follows FROM users(1) DIRECTION OUT FETCH ORDER BY __node");

    // Edge-centric query (what frontend should auto-generate).
    auto edges = exec_ok(
        "SELECT __from, __to, __depth "
        "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");

    // Node result: 3 nodes discovered (2, 3, 4).
    ASSERT_EQ(nodes.rows.size(), 3u);

    // Edge result: 3 edges (1→2, 1→3, 2→4).
    ASSERT_EQ(edges.rows.size(), 3u);

    // Verify every __from and __to in edge results corresponds to
    // discovered nodes in the node result.
    std::set<int64_t> discovered_pks;
    discovered_pks.insert(1); // start node
    int node_pk_idx = col_idx(nodes, "__node");
    ASSERT_GE(node_pk_idx, 0);
    for (const auto& row : nodes.rows) {
        discovered_pks.insert(get_int(row[node_pk_idx]));
    }

    int from_idx = col_idx(edges, "__from");
    int to_idx = col_idx(edges, "__to");
    ASSERT_GE(from_idx, 0);
    ASSERT_GE(to_idx, 0);
    for (const auto& row : edges.rows) {
        EXPECT_TRUE(discovered_pks.count(get_int(row[from_idx])) > 0)
            << "Edge __from " << get_int(row[from_idx]) << " not in discovered nodes";
        EXPECT_TRUE(discovered_pks.count(get_int(row[to_idx])) > 0)
            << "Edge __to " << get_int(row[to_idx]) << " not in discovered nodes";
    }
}

// ============================================================================
// TRAVERSE with no results — empty graph (user's scenario if node has no edges)
// ============================================================================

TEST_F(QA_GDB309_GraphViewToggle, EmptyTraversal_IsolatedNode) {
    setup_users(1);
    // Node 1 has no edges.

    auto qr = exec_ok(
        "SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT FETCH "
        "ORDER BY name LIMIT 10");

    EXPECT_TRUE(qr.rows.empty());
    // Columns should still be present even with 0 rows.
    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "name");
    EXPECT_EQ(qr.column_names[1], "__depth");
}

// ============================================================================
// Edge properties visible in edge mode but not in node mode
// ============================================================================

TEST_F(QA_GDB309_GraphViewToggle, EdgeProperties_InBothModes) {
    exec_ok("CREATE TABLE people (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO people VALUES (1, 'Alice')");
    exec_ok("INSERT INTO people VALUES (2, 'Bob')");
    exec_ok("CREATE EDGE TYPE knows(weight FLOAT) FROM people TO people");
    exec_ok("LINK people(1) TO people(2) VIA knows (weight = 0.75)");

    // Node mode SELECT * includes edge properties after meta-columns.
    auto nodes = exec_ok(
        "SELECT * FROM TRAVERSE knows FROM people(1) DIRECTION OUT FETCH");
    EXPECT_TRUE(has_column(nodes, "__node"));
    EXPECT_TRUE(has_column(nodes, "__depth"));
    ASSERT_EQ(nodes.rows.size(), 1u);

    // Edge mode: weight column explicitly accessible.
    auto edges = exec_ok(
        "SELECT __from, __to, knows.weight "
        "FROM TRAVERSE knows FROM people(1) DIRECTION OUT MODE EDGES");
    EXPECT_TRUE(has_column(edges, "weight"));
    ASSERT_EQ(edges.rows.size(), 1u);

    int w_idx = col_idx(edges, "weight");
    ASSERT_GE(w_idx, 0);
    EXPECT_NEAR(edges.rows[0][w_idx].as_float64(), 0.75, 0.001);
}

// ============================================================================
// Heterogeneous edge with selective columns — the user's scenario variant
// ============================================================================

TEST_F(QA_GDB309_GraphViewToggle, Heterogeneous_SelectDepthOnly) {
    exec_ok("CREATE TABLE authors (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO authors VALUES (1, 'Alice')");

    exec_ok("CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO books VALUES (10, 'Book A')");
    exec_ok("INSERT INTO books VALUES (20, 'Book B')");

    exec_ok("CREATE EDGE TYPE wrote FROM authors TO books");
    exec_ok("LINK authors(1) TO books(10) VIA wrote");
    exec_ok("LINK authors(1) TO books(20) VIA wrote");

    // User selects only title and __depth — no __node.
    auto qr = exec_ok(
        "SELECT title, __depth FROM TRAVERSE wrote FROM authors(1) DIRECTION OUT FETCH "
        "ORDER BY title");

    ASSERT_EQ(qr.rows.size(), 2u);
    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "title");
    EXPECT_EQ(qr.column_names[1], "__depth");

    EXPECT_EQ(qr.rows[0][0].as_string(), "Book A");
    EXPECT_EQ(qr.rows[1][0].as_string(), "Book B");
    EXPECT_EQ(qr.rows[0][1].as_int64(), 1);
    EXPECT_EQ(qr.rows[1][1].as_int64(), 1);
}

// ============================================================================
// Stress: verify meta-columns are correct for a deep chain
// ============================================================================

TEST_F(QA_GDB309_GraphViewToggle, DeepChain_DepthIncrements) {
    const int N = 20;
    setup_users(N + 1);
    for (int i = 1; i <= N; ++i) {
        link(i, i + 1);
    }

    auto qr = exec_ok(
        "SELECT __depth, __node FROM TRAVERSE follows FROM users(1) DIRECTION OUT FETCH "
        "ORDER BY __depth");

    ASSERT_EQ(qr.rows.size(), static_cast<size_t>(N));

    int depth_idx = col_idx(qr, "__depth");
    int node_idx = col_idx(qr, "__node");
    ASSERT_GE(depth_idx, 0);
    ASSERT_GE(node_idx, 0);

    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(qr.rows[i][depth_idx].as_int64(), i + 1)
            << "Node at index " << i << " should have depth " << (i + 1);
        EXPECT_EQ(get_int(qr.rows[i][node_idx]), i + 2)
            << "Node at index " << i << " should be user " << (i + 2);
    }
}

// ============================================================================
// FETCH AS alias — verify alias doesn't interfere with meta-columns
// ============================================================================

TEST_F(QA_GDB309_GraphViewToggle, FetchAs_MetaColumnsStillAccessible) {
    setup_users(2);
    link(1, 2);

    // FETCH AS t — the alias allows using table columns by name.
    auto qr = exec_ok(
        "SELECT name, __node, __depth, __source "
        "FROM TRAVERSE follows FROM users(1) DIRECTION OUT FETCH AS t");

    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.column_names.size(), 4u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "User2");
    EXPECT_EQ(get_int(qr.rows[0][1]), 2);  // __node
    EXPECT_EQ(qr.rows[0][2].as_int64(), 1);  // __depth
    EXPECT_EQ(get_int(qr.rows[0][3]), 1);  // __source (parent is 1)
}

// ============================================================================
// WHERE on meta-columns in node mode
// ============================================================================

TEST_F(QA_GDB309_GraphViewToggle, WhereOnDepth_FiltersCorrectly) {
    setup_users(5);
    link(1, 2);
    link(2, 3);
    link(3, 4);
    link(4, 5);

    auto qr = exec_ok(
        "SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT FETCH "
        "WHERE __depth <= 2 ORDER BY __depth");

    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "User2"); // depth 1
    EXPECT_EQ(qr.rows[1][0].as_string(), "User3"); // depth 2
}

TEST_F(QA_GDB309_GraphViewToggle, WhereOnSource_FiltersCorrectly) {
    setup_users(4);
    link(1, 2);
    link(1, 3);
    link(2, 4);

    auto qr = exec_ok(
        "SELECT name, __source FROM TRAVERSE follows FROM users(1) DIRECTION OUT FETCH "
        "WHERE __source = 1 ORDER BY name");

    // Only nodes directly reachable from node 1: User2 and User3.
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "User2");
    EXPECT_EQ(qr.rows[1][0].as_string(), "User3");
}

// ============================================================================
// Combination: __depth + __source without __node (graph view needs all three
// for proper visualization, but frontend only gets what user selects)
// ============================================================================

TEST_F(QA_GDB309_GraphViewToggle, SelectDepthAndSource_NoNode) {
    setup_users(3);
    link(1, 2);
    link(2, 3);

    auto qr = exec_ok(
        "SELECT __depth, __source FROM TRAVERSE follows FROM users(1) DIRECTION OUT FETCH "
        "ORDER BY __depth");

    ASSERT_EQ(qr.rows.size(), 2u);
    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "__depth");
    EXPECT_EQ(qr.column_names[1], "__source");

    // User2: depth 1, source 1
    EXPECT_EQ(qr.rows[0][0].as_int64(), 1);
    EXPECT_EQ(get_int(qr.rows[0][1]), 1);

    // User3: depth 2, source 2
    EXPECT_EQ(qr.rows[1][0].as_int64(), 2);
    EXPECT_EQ(get_int(qr.rows[1][1]), 2);
}

// ============================================================================
// DIRECTION BOTH with partial meta-column selection
// ============================================================================

TEST_F(QA_GDB309_GraphViewToggle, DirectionBoth_SelectNameAndDepth) {
    setup_users(3);
    link(1, 2);
    link(3, 1);

    auto qr = exec_ok(
        "SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION BOTH FETCH "
        "ORDER BY name");

    // Discovers User2 (out) and User3 (in).
    ASSERT_EQ(qr.rows.size(), 2u);
    // Both at depth 1.
    EXPECT_EQ(qr.rows[0][1].as_int64(), 1);
    EXPECT_EQ(qr.rows[1][1].as_int64(), 1);
}

} // namespace
} // namespace giodb
