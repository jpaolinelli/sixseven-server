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
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// QA Fixture for GDB-298: Edge-centric TRAVERSE output (MODE EDGES)
// ============================================================================

class QA_GDB298_EdgeTraversal : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb298";
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

    /// Helper to extract integer value regardless of underlying type (INT32
    /// or INT64). Meta-columns like __from/__to use the table's actual PK type.
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

    /// Collect (from, to) pairs sorted.
    std::vector<std::pair<int64_t, int64_t>> collect_edges(const QueryResult& qr) {
        std::vector<std::pair<int64_t, int64_t>> edges;
        size_t from_idx = 0;
        size_t to_idx = 1;
        for (size_t i = 0; i < qr.column_names.size(); ++i) {
            if (qr.column_names[i] == "__from")
                from_idx = i;
            if (qr.column_names[i] == "__to")
                to_idx = i;
        }
        for (const auto& row : qr.rows) {
            edges.emplace_back(get_int(row[from_idx]), get_int(row[to_idx]));
        }
        std::sort(edges.begin(), edges.end());
        return edges;
    }

    /// Set up a basic homogeneous graph:
    ///   users: 1..N, follows edges as specified.
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

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ============================================================================
// AC1: One row per edge between discovered nodes
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, AC1_CompleteGraphEdgeCount) {
    // Complete graph K5: every node connects to every other.
    // 5 nodes, 5*4 = 20 directed edges.
    setup_users(5);
    for (int i = 1; i <= 5; ++i) {
        for (int j = 1; j <= 5; ++j) {
            if (i != j) {
                link(i, j);
            }
        }
    }

    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");

    // BFS from 1 OUT discovers all 5 nodes. All 20 edges should appear.
    ASSERT_EQ(qr.rows.size(), 20u);

    auto edges = collect_edges(qr);
    // Verify every (i, j) pair with i!=j is present.
    for (int i = 1; i <= 5; ++i) {
        for (int j = 1; j <= 5; ++j) {
            if (i != j) {
                EXPECT_NE(
                    std::find(edges.begin(), edges.end(), std::make_pair((int64_t)i, (int64_t)j)),
                    edges.end())
                    << "Missing edge " << i << " -> " << j;
            }
        }
    }
}

TEST_F(QA_GDB298_EdgeTraversal, AC1_LinearChainEdges) {
    // Linear chain: 1→2→3→4→5
    setup_users(5);
    link(1, 2);
    link(2, 3);
    link(3, 4);
    link(4, 5);

    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");

    auto edges = collect_edges(qr);
    ASSERT_EQ(edges.size(), 4u);
    EXPECT_EQ(edges[0], std::make_pair(1L, 2L));
    EXPECT_EQ(edges[1], std::make_pair(2L, 3L));
    EXPECT_EQ(edges[2], std::make_pair(3L, 4L));
    EXPECT_EQ(edges[3], std::make_pair(4L, 5L));
}

// ============================================================================
// AC2: Cross-edges are included
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, AC2_MultipleCrossEdges) {
    // Diamond graph with extra cross-edges:
    //   1→2, 1→3, 2→4, 3→4, 2→3, 3→2, 4→2
    setup_users(4);
    link(1, 2);
    link(1, 3);
    link(2, 4);
    link(3, 4);
    link(2, 3); // cross-edge
    link(3, 2); // cross-edge
    link(4, 2); // cross-edge (back-edge)

    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");

    auto edges = collect_edges(qr);
    ASSERT_EQ(edges.size(), 7u);

    // All 7 edges must be present.
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(1L, 2L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(1L, 3L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(2L, 3L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(2L, 4L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(3L, 2L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(3L, 4L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(4L, 2L)), edges.end());
}

// ============================================================================
// AC3: Bidirectional edges both appear
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, AC3_AllBidirectionalPairs) {
    // Mutual follows: 1↔2, 1↔3, 2↔3
    setup_users(3);
    link(1, 2);
    link(2, 1);
    link(1, 3);
    link(3, 1);
    link(2, 3);
    link(3, 2);

    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");

    auto edges = collect_edges(qr);
    // 6 bidirectional edges between 3 nodes.
    ASSERT_EQ(edges.size(), 6u);
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(1L, 2L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(2L, 1L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(1L, 3L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(3L, 1L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(2L, 3L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(3L, 2L)), edges.end());
}

// ============================================================================
// AC4: Edge properties returned per row
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, AC4_MultipleEdgeProperties) {
    // Edge type with multiple properties.
    exec_ok("CREATE TABLE people (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO people VALUES (1, 'A')");
    exec_ok("INSERT INTO people VALUES (2, 'B')");
    exec_ok("INSERT INTO people VALUES (3, 'C')");
    exec_ok("CREATE EDGE TYPE friendship(weight FLOAT, since INT) FROM people TO people");
    exec_ok("LINK people(1) TO people(2) VIA friendship (weight = 0.5, since = 2020)");
    exec_ok("LINK people(1) TO people(3) VIA friendship (weight = 0.9, since = 2021)");

    auto qr = exec_ok("SELECT __from, __to, friendship.weight, friendship.since "
                      "FROM TRAVERSE friendship FROM people(1) DIRECTION OUT MODE EDGES "
                      "ORDER BY __to");

    ASSERT_EQ(qr.rows.size(), 2u);

    // Find column indices.
    size_t w_idx = 0, s_idx = 0;
    for (size_t i = 0; i < qr.column_names.size(); ++i) {
        if (qr.column_names[i] == "weight")
            w_idx = i;
        if (qr.column_names[i] == "since")
            s_idx = i;
    }

    // Edge 1→2: weight=0.5, since=2020
    EXPECT_NEAR(qr.rows[0][w_idx].as_float64(), 0.5, 0.001);
    EXPECT_EQ(qr.rows[0][s_idx].as_int64(), 2020);

    // Edge 1→3: weight=0.9, since=2021
    EXPECT_NEAR(qr.rows[1][w_idx].as_float64(), 0.9, 0.001);
    EXPECT_EQ(qr.rows[1][s_idx].as_int64(), 2021);
}

// ============================================================================
// AC5: WHERE filters on edge properties
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, AC5_WhereOnEdgePropertyLessThan) {
    exec_ok("CREATE TABLE people (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO people VALUES (1, 'A')");
    exec_ok("INSERT INTO people VALUES (2, 'B')");
    exec_ok("INSERT INTO people VALUES (3, 'C')");
    exec_ok("CREATE EDGE TYPE knows(weight FLOAT) FROM people TO people");
    exec_ok("LINK people(1) TO people(2) VIA knows (weight = 0.2)");
    exec_ok("LINK people(1) TO people(3) VIA knows (weight = 0.8)");
    exec_ok("LINK people(2) TO people(3) VIA knows (weight = 0.4)");

    auto qr = exec_ok("SELECT __from, __to "
                      "FROM TRAVERSE knows FROM people(1) DIRECTION OUT MODE EDGES "
                      "WHERE knows.weight < 0.5");

    auto edges = collect_edges(qr);
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_EQ(edges[0], std::make_pair(1L, 2L)); // weight=0.2
    EXPECT_EQ(edges[1], std::make_pair(2L, 3L)); // weight=0.4
}

TEST_F(QA_GDB298_EdgeTraversal, AC5_WhereFiltersAllEdges) {
    exec_ok("CREATE TABLE people (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO people VALUES (1, 'A')");
    exec_ok("INSERT INTO people VALUES (2, 'B')");
    exec_ok("CREATE EDGE TYPE knows(weight FLOAT) FROM people TO people");
    exec_ok("LINK people(1) TO people(2) VIA knows (weight = 0.3)");

    // WHERE filters out all edges.
    auto qr = exec_ok("SELECT __from, __to "
                      "FROM TRAVERSE knows FROM people(1) DIRECTION OUT MODE EDGES "
                      "WHERE knows.weight > 0.99");

    EXPECT_TRUE(qr.rows.empty());
}

// ============================================================================
// AC6: ORDER BY and LIMIT compose correctly
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, AC6_OrderByDepthDescWithLimit) {
    // Chain: 1→2→3→4→5
    setup_users(5);
    link(1, 2);
    link(2, 3);
    link(3, 4);
    link(4, 5);

    auto qr = exec_ok("SELECT __from, __to, __depth "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES "
                      "ORDER BY __depth DESC LIMIT 2");

    ASSERT_EQ(qr.rows.size(), 2u);

    // Deepest edges first: 4→5 (depth 4), 3→4 (depth 3)
    EXPECT_EQ(qr.rows[0][2].as_int64(), 4); // depth of edge 4→5
    EXPECT_EQ(qr.rows[1][2].as_int64(), 3); // depth of edge 3→4
}

TEST_F(QA_GDB298_EdgeTraversal, AC6_LimitZeroReturnsEmpty) {
    setup_users(3);
    link(1, 2);
    link(1, 3);

    auto qr = exec_ok("SELECT __from, __to "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES "
                      "LIMIT 0");

    EXPECT_TRUE(qr.rows.empty());
}

TEST_F(QA_GDB298_EdgeTraversal, AC6_LimitLargerThanResultSet) {
    setup_users(3);
    link(1, 2);

    auto qr = exec_ok("SELECT __from, __to "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES "
                      "LIMIT 1000");

    // Only 1 edge exists.
    ASSERT_EQ(qr.rows.size(), 1u);
}

// ============================================================================
// AC7: Heterogeneous edge types work with MODE EDGES
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, AC7_HeterogeneousMultipleEdgesOut) {
    exec_ok("CREATE TABLE authors (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO authors VALUES (1, 'Alice')");

    exec_ok("CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO books VALUES (10, 'Book A')");
    exec_ok("INSERT INTO books VALUES (20, 'Book B')");
    exec_ok("INSERT INTO books VALUES (30, 'Book C')");

    exec_ok("CREATE EDGE TYPE wrote FROM authors TO books");
    exec_ok("LINK authors(1) TO books(10) VIA wrote");
    exec_ok("LINK authors(1) TO books(20) VIA wrote");
    exec_ok("LINK authors(1) TO books(30) VIA wrote");

    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE wrote FROM authors(1) DIRECTION OUT MODE EDGES");

    auto edges = collect_edges(qr);
    ASSERT_EQ(edges.size(), 3u);
    EXPECT_EQ(edges[0], std::make_pair(1L, 10L));
    EXPECT_EQ(edges[1], std::make_pair(1L, 20L));
    EXPECT_EQ(edges[2], std::make_pair(1L, 30L));
}

TEST_F(QA_GDB298_EdgeTraversal, AC7_HeterogeneousInDirection) {
    exec_ok("CREATE TABLE authors (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO authors VALUES (1, 'Alice')");
    exec_ok("INSERT INTO authors VALUES (2, 'Bob')");
    exec_ok("INSERT INTO authors VALUES (3, 'Carol')");

    exec_ok("CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO books VALUES (10, 'Book A')");

    exec_ok("CREATE EDGE TYPE wrote FROM authors TO books");
    exec_ok("LINK authors(1) TO books(10) VIA wrote");
    exec_ok("LINK authors(2) TO books(10) VIA wrote");
    exec_ok("LINK authors(3) TO books(10) VIA wrote");

    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE wrote FROM books(10) DIRECTION IN MODE EDGES");

    auto edges = collect_edges(qr);
    ASSERT_EQ(edges.size(), 3u);
    EXPECT_EQ(edges[0], std::make_pair(1L, 10L));
    EXPECT_EQ(edges[1], std::make_pair(2L, 10L));
    EXPECT_EQ(edges[2], std::make_pair(3L, 10L));
}

TEST_F(QA_GDB298_EdgeTraversal, AC7_HeterogeneousBothRejected) {
    exec_ok("CREATE TABLE authors (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO authors VALUES (1, 'Alice')");

    exec_ok("CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO books VALUES (10, 'Book A')");

    exec_ok("CREATE EDGE TYPE wrote FROM authors TO books");

    // DIRECTION BOTH on heterogeneous edge should be rejected.
    exec_error("SELECT __from, __to FROM TRAVERSE wrote FROM authors(1) DIRECTION BOTH MODE EDGES",
               StatusCode::TYPE_ERROR);
}

// ============================================================================
// AC8: Default mode remains node-centric (backward compatibility)
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, AC8_DefaultModeHasNoFromTo) {
    setup_users(3);
    link(1, 2);
    link(1, 3);

    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    // Should NOT have __from or __to columns.
    for (const auto& col : qr.column_names) {
        EXPECT_NE(col, "__from") << "Default mode should not have __from";
        EXPECT_NE(col, "__to") << "Default mode should not have __to";
    }

    // Should have node-centric columns.
    bool has_id = false, has_node = false;
    for (const auto& col : qr.column_names) {
        if (col == "id")
            has_id = true;
        if (col == "__node")
            has_node = true;
    }
    EXPECT_TRUE(has_id);
    EXPECT_TRUE(has_node);
    EXPECT_EQ(qr.rows.size(), 2u); // 2 discovered nodes, not 2 edges
}

TEST_F(QA_GDB298_EdgeTraversal, AC8_ExplicitModeNodesMatchesDefault) {
    setup_users(4);
    link(1, 2);
    link(1, 3);
    link(2, 4);

    auto qr_default =
        exec_ok("SELECT id, name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                "ORDER BY id");
    auto qr_explicit =
        exec_ok("SELECT id, name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                "MODE NODES ORDER BY id");

    ASSERT_EQ(qr_default.rows.size(), qr_explicit.rows.size());
    ASSERT_EQ(qr_default.column_names, qr_explicit.column_names);
    // Verify same row count and column structure — both should be node-centric.
    for (size_t i = 0; i < qr_default.rows.size(); ++i) {
        EXPECT_EQ(qr_default.rows[i].size(), qr_explicit.rows[i].size());
    }
}

// ============================================================================
// AC9: Empty traversal returns empty edge result
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, AC9_IsolatedNodeNoEdges) {
    setup_users(3);
    link(1, 2);
    // Node 3 is isolated — no edges at all.

    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(3) DIRECTION OUT MODE EDGES");
    EXPECT_TRUE(qr.rows.empty());
}

TEST_F(QA_GDB298_EdgeTraversal, AC9_NoIncomingEdgesInDirection) {
    setup_users(3);
    link(1, 2);
    link(1, 3);

    // Node 1 has no incoming edges.
    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION IN MODE EDGES");
    EXPECT_TRUE(qr.rows.empty());
}

TEST_F(QA_GDB298_EdgeTraversal, AC9_MaxDepthZeroNoSelfLoop) {
    setup_users(3);
    link(1, 2);
    link(1, 3);

    // MAX_DEPTH 0: BFS doesn't expand, only start node discovered.
    // No self-loop on node 1 → no edges.
    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 0 "
                "MODE EDGES");
    EXPECT_TRUE(qr.rows.empty());
}

// ============================================================================
// AC10: Self-loops included
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, AC10_SelfLoopOnly) {
    setup_users(1);
    link(1, 1); // Self-loop only.

    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");

    auto edges = collect_edges(qr);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0], std::make_pair(1L, 1L));
}

TEST_F(QA_GDB298_EdgeTraversal, AC10_SelfLoopAtNonStartNode) {
    setup_users(3);
    link(1, 2);
    link(2, 2); // Self-loop at non-start node.
    link(2, 3);

    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");

    auto edges = collect_edges(qr);
    // Expected edges: 1→2, 2→2, 2→3
    ASSERT_EQ(edges.size(), 3u);
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(1L, 2L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(2L, 2L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(2L, 3L)), edges.end());
}

TEST_F(QA_GDB298_EdgeTraversal, AC10_SelfLoopWithMaxDepthZero) {
    setup_users(2);
    link(1, 1); // Self-loop on start node.
    link(1, 2);

    // MAX_DEPTH 0: only start node discovered.
    // Self-loop 1→1 should still appear (both endpoints are discovered).
    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 0 "
                "MODE EDGES");

    auto edges = collect_edges(qr);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0], std::make_pair(1L, 1L));
}

// ============================================================================
// Boundary: __depth computation
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, DepthIsMaxOfEndpointsForCrossEdge) {
    // 1→2 (depth 1), 1→3 (depth 1), 3→2 cross-edge
    // Cross-edge 3→2: max(depth[3]=1, depth[2]=1) = 1
    setup_users(3);
    link(1, 2);
    link(1, 3);
    link(3, 2);

    auto qr = exec_ok("SELECT __from, __to, __depth "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES "
                      "ORDER BY __from, __to");

    ASSERT_EQ(qr.rows.size(), 3u);

    std::map<std::pair<int64_t, int64_t>, int64_t> edge_depths;
    for (const auto& row : qr.rows) {
        auto key = std::make_pair(get_int(row[0]), get_int(row[1]));
        edge_depths[key] = row[2].as_int64();
    }

    EXPECT_EQ(edge_depths[std::make_pair(1L, 2L)], 1); // max(0, 1) = 1
    EXPECT_EQ(edge_depths[std::make_pair(1L, 3L)], 1); // max(0, 1) = 1
    EXPECT_EQ(edge_depths[std::make_pair(3L, 2L)], 1); // max(1, 1) = 1
}

TEST_F(QA_GDB298_EdgeTraversal, DepthForBackEdgeToStart) {
    // 1→2→3, plus back-edge 3→1.
    // Edge 3→1: max(depth[3]=2, depth[1]=0) = 2
    setup_users(3);
    link(1, 2);
    link(2, 3);
    link(3, 1); // back-edge

    auto qr = exec_ok("SELECT __from, __to, __depth "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");

    std::map<std::pair<int64_t, int64_t>, int64_t> edge_depths;
    for (const auto& row : qr.rows) {
        auto key = std::make_pair(get_int(row[0]), get_int(row[1]));
        edge_depths[key] = row[2].as_int64();
    }

    EXPECT_EQ(edge_depths[std::make_pair(3L, 1L)], 2); // max(2, 0) = 2
}

// ============================================================================
// Boundary: MAX_DEPTH limits discovered nodes and therefore edges
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, MaxDepth1ExcludesDeeperEdges) {
    // Chain: 1→2→3→4, plus cross-edge 2→4
    setup_users(4);
    link(1, 2);
    link(2, 3);
    link(3, 4);
    link(2, 4); // cross-edge between discovered nodes (if depth allows)

    // MAX_DEPTH 1: discovers {1, 2}.
    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 1 "
                "MODE EDGES");

    auto edges = collect_edges(qr);
    // Only edges between {1, 2}: 1→2.
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0], std::make_pair(1L, 2L));
}

TEST_F(QA_GDB298_EdgeTraversal, MaxDepth2IncludesCrossEdge) {
    // Chain: 1→2→3→4, plus cross-edge 2→4
    setup_users(4);
    link(1, 2);
    link(2, 3);
    link(3, 4);
    link(2, 4); // cross-edge

    // MAX_DEPTH 2: discovers {1, 2, 3, 4} because 4 is reachable via 2→4 at depth 2.
    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 2 "
                "MODE EDGES");

    auto edges = collect_edges(qr);
    // Edges between {1, 2, 3, 4}: 1→2, 2→3, 2→4, 3→4
    ASSERT_EQ(edges.size(), 4u);
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(1L, 2L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(2L, 3L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(2L, 4L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(3L, 4L)), edges.end());
}

// ============================================================================
// DIRECTION BOTH with edge mode
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, DirectionBothCapturesIncomingCrossEdges) {
    // 1→2, 3→1 (incoming to start node).
    // DIRECTION BOTH from 1 discovers {1, 2, 3}.
    setup_users(3);
    link(1, 2);
    link(3, 1);

    auto qr = exec_ok(
        "SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION BOTH MODE EDGES");

    auto edges = collect_edges(qr);
    // Edges between {1, 2, 3}: 1→2, 3→1.
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(1L, 2L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(3L, 1L)), edges.end());
}

TEST_F(QA_GDB298_EdgeTraversal, DirectionInOnlyIncomingEdges) {
    // 2→1, 3→1, 1→4. Traverse IN from 1 discovers {2, 3}.
    setup_users(4);
    link(2, 1);
    link(3, 1);
    link(1, 4);

    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION IN MODE EDGES");

    auto edges = collect_edges(qr);
    // Discovered: {1, 2, 3}. Edges between them: 2→1, 3→1.
    // 1→4 NOT included (node 4 not discovered via IN).
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(2L, 1L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(3L, 1L)), edges.end());
}

// ============================================================================
// SELECT * in edge mode
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, SelectStarWithEdgeProperties) {
    exec_ok("CREATE TABLE people (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO people VALUES (1, 'A')");
    exec_ok("INSERT INTO people VALUES (2, 'B')");
    exec_ok("CREATE EDGE TYPE knows(weight FLOAT, tag VARCHAR) FROM people TO people");
    exec_ok("LINK people(1) TO people(2) VIA knows (weight = 0.5, tag = 'friend')");

    auto qr = exec_ok("SELECT * FROM TRAVERSE knows FROM people(1) DIRECTION OUT MODE EDGES");

    // Expect: __from, __to, __depth, weight, tag
    ASSERT_EQ(qr.column_names.size(), 5u);
    EXPECT_EQ(qr.column_names[0], "__from");
    EXPECT_EQ(qr.column_names[1], "__to");
    EXPECT_EQ(qr.column_names[2], "__depth");
    EXPECT_EQ(qr.column_names[3], "weight");
    EXPECT_EQ(qr.column_names[4], "tag");

    ASSERT_EQ(qr.rows.size(), 1u);
}

// ============================================================================
// WHERE filtering on meta-columns (__from, __to)
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, WhereOnFromColumn) {
    setup_users(4);
    link(1, 2);
    link(1, 3);
    link(2, 4);
    link(3, 4);

    // Filter to edges originating from node 1.
    auto qr = exec_ok("SELECT __from, __to "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES "
                      "WHERE __from = 1");

    auto edges = collect_edges(qr);
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_EQ(edges[0], std::make_pair(1L, 2L));
    EXPECT_EQ(edges[1], std::make_pair(1L, 3L));
}

TEST_F(QA_GDB298_EdgeTraversal, WhereOnToColumn) {
    setup_users(4);
    link(1, 2);
    link(1, 3);
    link(2, 4);
    link(3, 4);

    // Filter to edges targeting node 4.
    auto qr = exec_ok("SELECT __from, __to "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES "
                      "WHERE __to = 4");

    auto edges = collect_edges(qr);
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_EQ(edges[0], std::make_pair(2L, 4L));
    EXPECT_EQ(edges[1], std::make_pair(3L, 4L));
}

// ============================================================================
// Stress: larger graph with many edges
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, StressStarTopology) {
    // Star topology: node 1 connects to 50 other nodes.
    const int N = 50;
    exec_ok("CREATE TABLE nodes (id INT PRIMARY KEY, label VARCHAR)");
    for (int i = 1; i <= N + 1; ++i) {
        exec_ok("INSERT INTO nodes VALUES (" + std::to_string(i) + ", 'N" + std::to_string(i) +
                "')");
    }
    exec_ok("CREATE EDGE TYPE link FROM nodes TO nodes");
    for (int i = 2; i <= N + 1; ++i) {
        exec_ok("LINK nodes(1) TO nodes(" + std::to_string(i) + ") VIA link");
    }

    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE link FROM nodes(1) DIRECTION OUT MODE EDGES");

    // 50 edges from node 1 to each other node.
    ASSERT_EQ(qr.rows.size(), static_cast<size_t>(N));
}

TEST_F(QA_GDB298_EdgeTraversal, StressDenseSubgraph) {
    // 8 nodes, complete graph = 56 directed edges.
    const int N = 8;
    exec_ok("CREATE TABLE nodes (id INT PRIMARY KEY, label VARCHAR)");
    for (int i = 1; i <= N; ++i) {
        exec_ok("INSERT INTO nodes VALUES (" + std::to_string(i) + ", 'N" + std::to_string(i) +
                "')");
    }
    exec_ok("CREATE EDGE TYPE conn FROM nodes TO nodes");
    for (int i = 1; i <= N; ++i) {
        for (int j = 1; j <= N; ++j) {
            if (i != j) {
                exec_ok("LINK nodes(" + std::to_string(i) + ") TO nodes(" + std::to_string(j) +
                        ") VIA conn");
            }
        }
    }

    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE conn FROM nodes(1) DIRECTION OUT MODE EDGES");

    // N*(N-1) = 56 edges.
    ASSERT_EQ(qr.rows.size(), static_cast<size_t>(N * (N - 1)));
}

// ============================================================================
// Parser edge cases
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, ParserModeEdgesCaseInsensitive) {
    setup_users(2);
    link(1, 2);

    // Lowercase "mode edges" should work via case-insensitive matching.
    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT mode edges");

    ASSERT_EQ(qr.rows.size(), 1u);
}

TEST_F(QA_GDB298_EdgeTraversal, ParserInvalidModeKeyword) {
    setup_users(2);
    link(1, 2);

    // "MODE VERTEX" is invalid — should error.
    exec_error("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE VERTEX",
               StatusCode::PARSE_ERROR);
}

// ============================================================================
// Multiple edge types on same tables — only specified type returned
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, OnlySpecifiedEdgeTypeReturned) {
    setup_users(3);
    exec_ok("CREATE EDGE TYPE blocks FROM users TO users");
    link(1, 2);                                      // follows
    exec_ok("LINK users(1) TO users(3) VIA blocks"); // blocks
    exec_ok("LINK users(1) TO users(2) VIA blocks"); // blocks

    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");

    // Only follows edges: 1→2.
    auto edges = collect_edges(qr);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0], std::make_pair(1L, 2L));
}

// ============================================================================
// Iterator protocol: re-open after close
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, RepeatedExecutionConsistentResults) {
    setup_users(3);
    link(1, 2);
    link(1, 3);

    // Execute the same query twice — results should be identical.
    auto qr1 =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");
    auto qr2 =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");

    auto edges1 = collect_edges(qr1);
    auto edges2 = collect_edges(qr2);
    EXPECT_EQ(edges1, edges2);
}

// ============================================================================
// Edge mode with edges added AFTER initial setup
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, EdgesAddedIncrementally) {
    setup_users(4);
    link(1, 2);
    link(1, 3);

    auto qr1 =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");
    ASSERT_EQ(qr1.rows.size(), 2u);

    // Add more edges.
    link(2, 4);
    link(3, 4);

    auto qr2 =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");
    auto edges = collect_edges(qr2);
    ASSERT_EQ(edges.size(), 4u);
}

// ============================================================================
// Edge-centric traversal with heterogeneous edge properties
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, HeterogeneousEdgeProperties) {
    exec_ok("CREATE TABLE authors (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO authors VALUES (1, 'Alice')");

    exec_ok("CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO books VALUES (10, 'Book A')");
    exec_ok("INSERT INTO books VALUES (20, 'Book B')");

    exec_ok("CREATE EDGE TYPE wrote(year INT) FROM authors TO books");
    exec_ok("LINK authors(1) TO books(10) VIA wrote (year = 2020)");
    exec_ok("LINK authors(1) TO books(20) VIA wrote (year = 2023)");

    auto qr = exec_ok("SELECT __from, __to, wrote.year "
                      "FROM TRAVERSE wrote FROM authors(1) DIRECTION OUT MODE EDGES "
                      "ORDER BY wrote.year");

    ASSERT_EQ(qr.rows.size(), 2u);

    size_t y_idx = 0;
    for (size_t i = 0; i < qr.column_names.size(); ++i) {
        if (qr.column_names[i] == "year")
            y_idx = i;
    }

    EXPECT_EQ(qr.rows[0][y_idx].as_int64(), 2020);
    EXPECT_EQ(qr.rows[1][y_idx].as_int64(), 2023);
}

// ============================================================================
// Disconnected subgraphs — edges only between reachable nodes
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, DisconnectedSubgraphExcludesUnreachable) {
    // Two disconnected components: {1,2,3} and {4,5}.
    setup_users(5);
    link(1, 2);
    link(2, 3);
    link(4, 5); // Unreachable from node 1.
    link(5, 4);

    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");

    auto edges = collect_edges(qr);
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_EQ(edges[0], std::make_pair(1L, 2L));
    EXPECT_EQ(edges[1], std::make_pair(2L, 3L));

    // Edges 4→5 and 5→4 should NOT appear.
    EXPECT_EQ(std::find(edges.begin(), edges.end(), std::make_pair(4L, 5L)), edges.end());
    EXPECT_EQ(std::find(edges.begin(), edges.end(), std::make_pair(5L, 4L)), edges.end());
}

// ============================================================================
// WHERE + ORDER BY + LIMIT combined
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, WhereOrderByLimitCombined) {
    exec_ok("CREATE TABLE people (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO people VALUES (1, 'A')");
    exec_ok("INSERT INTO people VALUES (2, 'B')");
    exec_ok("INSERT INTO people VALUES (3, 'C')");
    exec_ok("INSERT INTO people VALUES (4, 'D')");
    exec_ok("CREATE EDGE TYPE knows(weight FLOAT) FROM people TO people");
    exec_ok("LINK people(1) TO people(2) VIA knows (weight = 0.1)");
    exec_ok("LINK people(1) TO people(3) VIA knows (weight = 0.9)");
    exec_ok("LINK people(1) TO people(4) VIA knows (weight = 0.5)");
    exec_ok("LINK people(2) TO people(3) VIA knows (weight = 0.7)");

    auto qr = exec_ok("SELECT __from, __to, knows.weight "
                      "FROM TRAVERSE knows FROM people(1) DIRECTION OUT MODE EDGES "
                      "WHERE knows.weight > 0.3 "
                      "ORDER BY knows.weight DESC "
                      "LIMIT 2");

    ASSERT_EQ(qr.rows.size(), 2u);

    size_t w_idx = 0;
    for (size_t i = 0; i < qr.column_names.size(); ++i) {
        if (qr.column_names[i] == "weight")
            w_idx = i;
    }

    // Top 2 by weight DESC after filtering > 0.3: 0.9, 0.7
    EXPECT_NEAR(qr.rows[0][w_idx].as_float64(), 0.9, 0.001);
    EXPECT_NEAR(qr.rows[1][w_idx].as_float64(), 0.7, 0.001);
}

// ============================================================================
// Single node, single self-loop with properties
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, SelfLoopWithProperties) {
    exec_ok("CREATE TABLE people (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO people VALUES (1, 'A')");
    exec_ok("CREATE EDGE TYPE refers(note VARCHAR) FROM people TO people");
    exec_ok("LINK people(1) TO people(1) VIA refers (note = 'self-ref')");

    auto qr = exec_ok("SELECT __from, __to, refers.note "
                      "FROM TRAVERSE refers FROM people(1) DIRECTION OUT MODE EDGES");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(get_int(qr.rows[0][0]), 1);
    EXPECT_EQ(get_int(qr.rows[0][1]), 1);

    size_t n_idx = 0;
    for (size_t i = 0; i < qr.column_names.size(); ++i) {
        if (qr.column_names[i] == "note")
            n_idx = i;
    }
    EXPECT_EQ(qr.rows[0][n_idx].as_string(), "self-ref");
}

// ============================================================================
// Cycle detection: triangle cycle
// ============================================================================

TEST_F(QA_GDB298_EdgeTraversal, TriangleCycleAllEdgesReturned) {
    // 1→2, 2→3, 3→1 (cycle). All 3 nodes discovered, all 3 edges returned.
    setup_users(3);
    link(1, 2);
    link(2, 3);
    link(3, 1);

    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");

    auto edges = collect_edges(qr);
    ASSERT_EQ(edges.size(), 3u);
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(1L, 2L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(2L, 3L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(3L, 1L)), edges.end());
}

} // namespace
} // namespace sixseven
