#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/edge_traversal.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/traversal.h"
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

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

// ============================================================================
// Fixture: Homogeneous edges (users follows users)
//
//   users: (1, "Alice"), (2, "Bob"), (3, "Jane"), (4, "Jake"), (5, "Zara")
//   follows edges: 1→2, 1→3, 2→4, 3→4, 4→5
//
//   BFS from 1 OUT: discovers 2, 3 (depth 1), 4 (depth 2), 5 (depth 3)
//   All edges between discovered nodes: 1→2, 1→3, 2→4, 3→4, 4→5  (5 edges)
// ============================================================================

class EdgeTraversalTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_edge_trav";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");
        exec_ok("INSERT INTO users VALUES (3, 'Jane')");
        exec_ok("INSERT INTO users VALUES (4, 'Jake')");
        exec_ok("INSERT INTO users VALUES (5, 'Zara')");

        exec_ok("CREATE EDGE TYPE follows FROM users TO users");
        exec_ok("LINK users(1) TO users(2) VIA follows");
        exec_ok("LINK users(1) TO users(3) VIA follows");
        exec_ok("LINK users(2) TO users(4) VIA follows");
        exec_ok("LINK users(3) TO users(4) VIA follows");
        exec_ok("LINK users(4) TO users(5) VIA follows");
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

    /// Extract an integer from a Value that may be INT32 or INT64.
    static int64_t val_to_int64(const Value& v) {
        if (v.type_id() == TypeId::INT32)
            return v.as_int32();
        return v.as_int64();
    }

    /// Collect (from, to) pairs from edge query results.
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
            edges.emplace_back(val_to_int64(row[from_idx]), val_to_int64(row[to_idx]));
        }
        std::sort(edges.begin(), edges.end());
        return edges;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ============================================================================
// AC1: MODE EDGES returns one row per edge between discovered nodes
// ============================================================================

TEST_F(EdgeTraversalTest, BasicEdgeOutput) {
    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");

    // Schema: __from, __to
    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "__from");
    EXPECT_EQ(qr.column_names[1], "__to");

    // 5 edges: 1→2, 1→3, 2→4, 3→4, 4→5
    auto edges = collect_edges(qr);
    ASSERT_EQ(edges.size(), 5u);
    EXPECT_EQ(edges[0], std::make_pair(1L, 2L));
    EXPECT_EQ(edges[1], std::make_pair(1L, 3L));
    EXPECT_EQ(edges[2], std::make_pair(2L, 4L));
    EXPECT_EQ(edges[3], std::make_pair(3L, 4L));
    EXPECT_EQ(edges[4], std::make_pair(4L, 5L));
}

// ============================================================================
// AC2: Cross-edges are included (not just BFS tree edges)
// ============================================================================

TEST_F(EdgeTraversalTest, CrossEdgesIncluded) {
    // Add a cross-edge: 2→3 (not a BFS tree edge since 3 is discovered via 1→3).
    exec_ok("LINK users(2) TO users(3) VIA follows");

    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");

    auto edges = collect_edges(qr);
    ASSERT_EQ(edges.size(), 6u);

    // 2→3 cross-edge must be present.
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(2L, 3L)), edges.end());
}

// ============================================================================
// AC3: Bidirectional edges both appear
// ============================================================================

TEST_F(EdgeTraversalTest, BidirectionalEdges) {
    // Add reverse edge: 2→1 (both nodes discovered, so both 1→2 and 2→1 appear).
    exec_ok("LINK users(2) TO users(1) VIA follows");

    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");

    auto edges = collect_edges(qr);

    // Both directions must appear.
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(1L, 2L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(2L, 1L)), edges.end());
}

// ============================================================================
// AC4: Edge properties are returned per edge row
// ============================================================================

TEST_F(EdgeTraversalTest, EdgeProperties) {
    // Create edge type with properties.
    exec_ok("CREATE TABLE people (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO people VALUES (1, 'A')");
    exec_ok("INSERT INTO people VALUES (2, 'B')");
    exec_ok("CREATE EDGE TYPE knows(weight FLOAT) FROM people TO people");
    exec_ok("LINK people(1) TO people(2) VIA knows (weight = 0.75)");

    auto qr = exec_ok("SELECT knows.weight, __from, __to "
                      "FROM TRAVERSE knows FROM people(1) DIRECTION OUT MODE EDGES");

    ASSERT_EQ(qr.rows.size(), 1u);

    // Find weight column.
    size_t weight_idx = 0;
    for (size_t i = 0; i < qr.column_names.size(); ++i) {
        if (qr.column_names[i] == "weight")
            weight_idx = i;
    }
    EXPECT_NEAR(qr.rows[0][weight_idx].as_float64(), 0.75, 0.001);
}

// ============================================================================
// AC5: WHERE filters on edge properties
// ============================================================================

TEST_F(EdgeTraversalTest, WhereOnEdgeProperties) {
    exec_ok("CREATE TABLE people (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO people VALUES (1, 'A')");
    exec_ok("INSERT INTO people VALUES (2, 'B')");
    exec_ok("INSERT INTO people VALUES (3, 'C')");
    exec_ok("CREATE EDGE TYPE knows(weight FLOAT) FROM people TO people");
    exec_ok("LINK people(1) TO people(2) VIA knows (weight = 0.3)");
    exec_ok("LINK people(1) TO people(3) VIA knows (weight = 0.8)");

    auto qr = exec_ok("SELECT __from, __to, knows.weight "
                      "FROM TRAVERSE knows FROM people(1) DIRECTION OUT MODE EDGES "
                      "WHERE knows.weight > 0.5");

    // Only edge 1→3 (weight 0.8) passes the filter.
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 1);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 3);
}

// ============================================================================
// AC6: ORDER BY and LIMIT compose correctly
// ============================================================================

TEST_F(EdgeTraversalTest, OrderByAndLimit) {
    auto qr = exec_ok("SELECT __from, __to, __depth "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES "
                      "ORDER BY __depth, __from, __to LIMIT 3");

    ASSERT_EQ(qr.rows.size(), 3u);

    // Depth 1 edges: 1→2, 1→3  (from=1, depth=max(0,1)=1)
    EXPECT_EQ(val_to_int64(qr.rows[0][2]), 1); // depth
    EXPECT_EQ(val_to_int64(qr.rows[1][2]), 1);
}

// ============================================================================
// AC7: Heterogeneous edge types work with MODE EDGES
// ============================================================================

TEST_F(EdgeTraversalTest, HeterogeneousEdges) {
    exec_ok("CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO posts VALUES (10, 'Hello')");
    exec_ok("INSERT INTO posts VALUES (20, 'World')");
    exec_ok("CREATE EDGE TYPE authored FROM users TO posts");
    exec_ok("LINK users(1) TO posts(10) VIA authored");
    exec_ok("LINK users(1) TO posts(20) VIA authored");

    auto qr = exec_ok("SELECT __from, __to "
                      "FROM TRAVERSE authored FROM users(1) DIRECTION OUT MODE EDGES");

    auto edges = collect_edges(qr);
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_EQ(edges[0], std::make_pair(1L, 10L));
    EXPECT_EQ(edges[1], std::make_pair(1L, 20L));
}

// ============================================================================
// AC8: Default mode (no MODE clause) remains node-centric
// ============================================================================

TEST_F(EdgeTraversalTest, DefaultModeIsNodeCentric) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    // Node-centric: should have target table columns + meta-columns.
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "name");

    // Find __node meta-column.
    bool has_node = false;
    for (const auto& col : qr.column_names) {
        if (col == "__node")
            has_node = true;
    }
    EXPECT_TRUE(has_node);

    // Should NOT have __from or __to.
    bool has_from = false;
    bool has_to = false;
    for (const auto& col : qr.column_names) {
        if (col == "__from")
            has_from = true;
        if (col == "__to")
            has_to = true;
    }
    EXPECT_FALSE(has_from);
    EXPECT_FALSE(has_to);

    // 4 nodes discovered (not 5 edges).
    EXPECT_EQ(qr.rows.size(), 4u);
}

// ============================================================================
// AC9: Empty traversal returns empty edge result set
// ============================================================================

TEST_F(EdgeTraversalTest, EmptyTraversal) {
    // Node 5 has no outgoing edges.
    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(5) DIRECTION OUT MODE EDGES");
    EXPECT_TRUE(qr.rows.empty());
}

// ============================================================================
// AC10: Self-loops are included
// ============================================================================

TEST_F(EdgeTraversalTest, SelfLoopIncluded) {
    // Add self-loop: 1→1.
    exec_ok("LINK users(1) TO users(1) VIA follows");

    auto qr =
        exec_ok("SELECT __from, __to FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");

    auto edges = collect_edges(qr);

    // Self-loop 1→1 must be present.
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(1L, 1L)), edges.end());
}

// ============================================================================
// __depth is max(depth_from, depth_to) for each edge
// ============================================================================

TEST_F(EdgeTraversalTest, DepthIsMaxOfEndpoints) {
    auto qr = exec_ok("SELECT __from, __to, __depth "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES "
                      "ORDER BY __from, __to");

    // Edge 1→2: max(depth[1]=0, depth[2]=1) = 1
    // Edge 1→3: max(0, 1) = 1
    // Edge 2→4: max(1, 2) = 2
    // Edge 3→4: max(1, 2) = 2
    // Edge 4→5: max(2, 3) = 3
    ASSERT_EQ(qr.rows.size(), 5u);

    // Build a map from (from, to) -> depth for verification.
    std::map<std::pair<int64_t, int64_t>, int64_t> edge_depths;
    for (const auto& row : qr.rows) {
        auto key = std::make_pair(val_to_int64(row[0]), val_to_int64(row[1]));
        edge_depths[key] = val_to_int64(row[2]);
    }

    EXPECT_EQ(edge_depths[std::make_pair(1L, 2L)], 1);
    EXPECT_EQ(edge_depths[std::make_pair(1L, 3L)], 1);
    EXPECT_EQ(edge_depths[std::make_pair(2L, 4L)], 2);
    EXPECT_EQ(edge_depths[std::make_pair(3L, 4L)], 2);
    EXPECT_EQ(edge_depths[std::make_pair(4L, 5L)], 3);
}

// ============================================================================
// SELECT * returns __from, __to, __depth (no table columns)
// ============================================================================

TEST_F(EdgeTraversalTest, SelectStarEdgeMode) {
    auto qr = exec_ok(
        "SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 1 MODE EDGES");

    // Should have: __from, __to, __depth (no edge properties on follows).
    ASSERT_EQ(qr.column_names.size(), 3u);
    EXPECT_EQ(qr.column_names[0], "__from");
    EXPECT_EQ(qr.column_names[1], "__to");
    EXPECT_EQ(qr.column_names[2], "__depth");

    // MAX_DEPTH 1: discovers nodes 2, 3 → edges 1→2, 1→3.
    ASSERT_EQ(qr.rows.size(), 2u);
}

// ============================================================================
// MODE NODES explicitly produces same result as no MODE clause
// ============================================================================

TEST_F(EdgeTraversalTest, ExplicitModeNodes) {
    auto qr_default = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(1) "
                              "DIRECTION OUT");
    auto qr_explicit = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(1) "
                               "DIRECTION OUT MODE NODES");

    ASSERT_EQ(qr_default.rows.size(), qr_explicit.rows.size());
    ASSERT_EQ(qr_default.column_names, qr_explicit.column_names);
}

// ============================================================================
// Heterogeneous IN direction — edge mode
// ============================================================================

TEST_F(EdgeTraversalTest, HeterogeneousInDirection) {
    exec_ok("CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO posts VALUES (10, 'Hello')");
    exec_ok("INSERT INTO posts VALUES (20, 'World')");
    exec_ok("CREATE EDGE TYPE authored FROM users TO posts");
    exec_ok("LINK users(1) TO posts(10) VIA authored");
    exec_ok("LINK users(2) TO posts(10) VIA authored");
    exec_ok("LINK users(1) TO posts(20) VIA authored");

    auto qr = exec_ok("SELECT __from, __to "
                      "FROM TRAVERSE authored FROM posts(10) DIRECTION IN MODE EDGES");

    // Post 10 was authored by users 1 and 2.
    // Edges: 1→10, 2→10
    auto edges = collect_edges(qr);
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_EQ(edges[0], std::make_pair(1L, 10L));
    EXPECT_EQ(edges[1], std::make_pair(2L, 10L));
}

// ============================================================================
// WHERE on __depth filters edges correctly
// ============================================================================

TEST_F(EdgeTraversalTest, WhereOnDepth) {
    auto qr = exec_ok("SELECT __from, __to "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES "
                      "WHERE __depth = 1");

    // Depth 1 edges: 1→2, 1→3 (max(0,1) = 1)
    auto edges = collect_edges(qr);
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_EQ(edges[0], std::make_pair(1L, 2L));
    EXPECT_EQ(edges[1], std::make_pair(1L, 3L));
}

// ============================================================================
// MAX_DEPTH limits BFS discovery, which limits edge results
// ============================================================================

TEST_F(EdgeTraversalTest, MaxDepthLimitsEdges) {
    auto qr = exec_ok("SELECT __from, __to "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 1 MODE EDGES");

    // MAX_DEPTH 1: discovers {1, 2, 3}. Edges: 1→2, 1→3.
    auto edges = collect_edges(qr);
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_EQ(edges[0], std::make_pair(1L, 2L));
    EXPECT_EQ(edges[1], std::make_pair(1L, 3L));
}

// ============================================================================
// DIRECTION BOTH with homogeneous edges — captures edges in both directions
// ============================================================================

TEST_F(EdgeTraversalTest, DirectionBothHomogeneous) {
    // Add reverse edge 2→1.
    exec_ok("LINK users(2) TO users(1) VIA follows");

    auto qr = exec_ok("SELECT __from, __to "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION BOTH MAX_DEPTH 1 MODE EDGES");

    auto edges = collect_edges(qr);

    // With BOTH MAX_DEPTH 1 from node 1:
    // OUT edges from 1: 1→2, 1→3
    // IN edges to 1: 2→1
    // Discovered: {1, 2, 3}
    // All edges between {1, 2, 3}: 1→2, 1→3, 2→1
    ASSERT_EQ(edges.size(), 3u);
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(1L, 2L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(1L, 3L)), edges.end());
    EXPECT_NE(std::find(edges.begin(), edges.end(), std::make_pair(2L, 1L)), edges.end());
}

// ============================================================================
// WITH TRACE adds a __path column showing the path to each edge's __from node
// (GDB-678)
// ============================================================================

TEST_F(EdgeTraversalTest, TraceAddsPathToFromNode) {
    auto qr = exec_ok("SELECT __from, __to, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES WITH TRACE");

    ASSERT_EQ(qr.column_names.size(), 3u);
    EXPECT_EQ(qr.column_names[2], "__path");

    // 5 edges: 1→2, 1→3, 2→4, 3→4, 4→5.
    ASSERT_EQ(qr.rows.size(), 5u);

    // Index path values by (from, to).
    std::map<std::pair<int64_t, int64_t>, Path> paths;
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row[2].type_id(), TypeId::PATH);
        paths.emplace(std::make_pair(val_to_int64(row[0]), val_to_int64(row[1])), row[2].as_path());
    }

    // Edge 1→2: __from = 1 (the start node), so the path is just [1].
    const Path& p_1_2 = paths.at(std::make_pair(1L, 2L));
    ASSERT_EQ(p_1_2.steps.size(), 1u);
    EXPECT_EQ(p_1_2.steps[0].node_pk, 1);

    // Edge 4→5: __from = 4, reached via 1 → (2 or 3) → 4, so the path ends at 4.
    const Path& p_4_5 = paths.at(std::make_pair(4L, 5L));
    EXPECT_EQ(p_4_5.steps.front().node_pk, 1);
    EXPECT_EQ(p_4_5.steps.back().node_pk, 4);
    EXPECT_EQ(p_4_5.length(), 2); // two hops from 1 to 4
}

TEST_F(EdgeTraversalTest, TracePathsAreCycleFree) {
    // Add a cycle 5 → 1; traced paths must still be acyclic.
    exec_ok("LINK users(5) TO users(1) VIA follows");

    auto qr = exec_ok("SELECT __from, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES WITH TRACE");

    for (const auto& row : qr.rows) {
        ASSERT_EQ(row[1].type_id(), TypeId::PATH);
        const Path& p = row[1].as_path();
        std::set<int64_t> seen;
        for (const auto& step : p.steps) {
            EXPECT_TRUE(seen.insert(step.node_pk).second)
                << "path contains a repeated node: " << step.node_pk;
        }
    }
}

TEST_F(EdgeTraversalTest, NoTraceNoPathColumn) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");
    for (const auto& name : qr.column_names) {
        EXPECT_NE(name, "__path") << "__path must not appear without WITH TRACE";
    }
}

// ============================================================================
// GDB-1214: max_visited exceeded -> explicit error (unified across graph ops)
// ============================================================================

TEST_F(EdgeTraversalTest, ExceedingMaxVisitedReturnsError) {
    // The users graph reaches 5 nodes from user 1 (1,2,3,4,5). A max_visited
    // of 2 is exceeded during BFS expansion, so open() must fail with an
    // explicit error rather than silently truncating the edge scan.
    TraversalConfig config;
    config.edge_type = "follows";
    config.start_key = Value(int64_t{1});
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;
    config.max_visited = 2;

    std::vector<OutputColumn> cols;
    cols.push_back({"", "__from", TypeId::INT64, false, 0});
    cols.push_back({"", "__to", TypeId::INT64, false, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    EdgeTraversalOperator op(*graph_engine_, std::move(config), std::move(schema), nullptr, bound);

    auto open_result = op.open();
    ASSERT_FALSE(open_result.has_value());
    EXPECT_EQ(open_result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(open_result.error().message.find("exceeded max_visited limit (2)"), std::string::npos)
        << open_result.error().message;
}

TEST_F(EdgeTraversalTest, UnderMaxVisitedLimitReturnsCompleteEdges) {
    // Regression guard: a generous max_visited budget must not affect
    // correctness -- same expectation as BasicEdgeOutput.
    TraversalConfig config;
    config.edge_type = "follows";
    config.start_key = Value(int64_t{1});
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;
    config.max_visited = 100000;

    std::vector<OutputColumn> cols;
    cols.push_back({"", "__from", TypeId::INT64, false, 0});
    cols.push_back({"", "__to", TypeId::INT64, false, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    EdgeTraversalOperator op(*graph_engine_, std::move(config), std::move(schema), nullptr, bound);

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    size_t count = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value())
            break;
        ++count;
    }
    op.close();
    EXPECT_EQ(count, 5u);
}

} // namespace
} // namespace sixseven
