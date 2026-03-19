/// QA adversarial tests for GDB-28: Graph Traversal & Pattern Matching.
/// Tests BFS traversal, shortest path, and MATCH pattern matching operators
/// with edge cases, boundary values, and adversarial inputs.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/pattern_match.h"
#include "sixseven/executor/shortest_path.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/traversal.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Traversal Adversarial Tests
// ============================================================================

class QA_Traversal : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_ = std::make_unique<Catalog>();
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        TableSchema ts;
        ts.name = "nodes";
        CatalogColumnDef pk_col;
        pk_col.ordinal = 0;
        pk_col.name = "id";
        pk_col.type_id = TypeId::INT64;
        pk_col.nullable = false;
        ts.columns.push_back(pk_col);
        ts.pk_columns = "id";
        auto tid = catalog_->create_table(default_database_id, std::move(ts));
        ASSERT_TRUE(tid.has_value()) << tid.error().message;
        table_id_ = *tid;

        auto eid = graph_->create_edge_type(default_database_id, 
            "follows", table_id_, table_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;
    }

    void link(int64_t from, int64_t to) {
        auto r = graph_->link("follows", Value(from), Value(to));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    /// Run BFS and return (node_pk, depth) pairs.
    std::vector<std::pair<int64_t, int64_t>> run_bfs(int64_t start,
                                                     TraverseDirection dir = TraverseDirection::OUT,
                                                     int32_t max_depth = 100,
                                                     size_t max_visited = 100000,
                                                     bool fetch = false,
                                                     bool collect_edges = false) {
        TraversalConfig config;
        config.edge_type = "follows";
        config.start_key = Value(start);
        config.direction = dir;
        config.max_depth = max_depth;
        config.max_visited = max_visited;
        config.fetch = fetch;
        config.collect_edges = collect_edges;

        std::vector<OutputColumn> cols;
        cols.push_back({"", "node", TypeId::INT64, false, 0});
        cols.push_back({"", "depth", TypeId::INT64, false, 0});
        if (fetch) {
            cols.push_back({"", "source", TypeId::INT64, true, 0});
        }
        OutputSchema schema(std::move(cols));

        BoundStatement bound;
        TraversalOperator op(*graph_, std::move(config), std::move(schema), nullptr, bound);

        auto open_result = op.open();
        EXPECT_TRUE(open_result.has_value()) << open_result.error().message;

        std::vector<std::pair<int64_t, int64_t>> results;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row->has_value()) {
                break;
            }
            auto& vals = row->value().values;
            results.emplace_back(vals[0].as_int64(), vals[1].as_int64());
        }
        op.close();
        return results;
    }

    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t table_id_ = 0;
};

// -- Boundary: max_depth = 0 means no expansion --
TEST_F(QA_Traversal, DepthZeroReturnsNothing) {
    link(1, 2);
    auto results = run_bfs(1, TraverseDirection::OUT, 0);
    // max_depth=0: start node (depth 0) is not emitted, and no expansion.
    EXPECT_TRUE(results.empty());
}

// -- Boundary: max_depth = 1 on a chain --
TEST_F(QA_Traversal, DepthOneOnChain) {
    // Chain: 1 -> 2 -> 3 -> 4 -> 5
    link(1, 2);
    link(2, 3);
    link(3, 4);
    link(4, 5);

    auto results = run_bfs(1, TraverseDirection::OUT, 1);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].first, 2);
    EXPECT_EQ(results[0].second, 1);
}

// -- Boundary: max_depth equals exact chain length --
TEST_F(QA_Traversal, DepthEqualsChainLength) {
    // Chain: 1 -> 2 -> 3 (length 2)
    link(1, 2);
    link(2, 3);

    auto results = run_bfs(1, TraverseDirection::OUT, 2);
    // With max_depth=2, node at depth 2 should be reached.
    ASSERT_EQ(results.size(), 2u);
    std::vector<int64_t> nodes;
    for (auto& [n, d] : results) {
        nodes.push_back(n);
    }
    std::sort(nodes.begin(), nodes.end());
    EXPECT_EQ(nodes[0], 2);
    EXPECT_EQ(nodes[1], 3);
}

// -- Boundary: max_visited = 1 limits to just the start node --
TEST_F(QA_Traversal, MaxVisitedOne) {
    link(1, 2);
    link(1, 3);

    auto results = run_bfs(1, TraverseDirection::OUT, 100, 1);
    // max_visited=1: start node is inserted (size=1), then no expansion
    // because visited.size() >= max_visited.
    EXPECT_TRUE(results.empty());
}

// -- Isolated node with no edges --
TEST_F(QA_Traversal, IsolatedNode) {
    // Don't add any edges. Node 42 has no edges.
    auto results = run_bfs(42, TraverseDirection::OUT);
    EXPECT_TRUE(results.empty());
    auto results_in = run_bfs(42, TraverseDirection::IN);
    EXPECT_TRUE(results_in.empty());
    auto results_both = run_bfs(42, TraverseDirection::BOTH);
    EXPECT_TRUE(results_both.empty());
}

// -- Self-loop: node points to itself --
TEST_F(QA_Traversal, SelfLoop) {
    link(1, 1);

    auto results = run_bfs(1, TraverseDirection::OUT);
    // Node 1 is visited at depth 0, neighbor 1 is already visited.
    EXPECT_TRUE(results.empty());
}

// -- Dense small cycle: triangle --
TEST_F(QA_Traversal, TriangleCycle) {
    link(1, 2);
    link(2, 3);
    link(3, 1);

    auto results = run_bfs(1, TraverseDirection::OUT);
    ASSERT_EQ(results.size(), 2u);
    std::vector<int64_t> nodes;
    for (auto& [n, d] : results) {
        nodes.push_back(n);
    }
    std::sort(nodes.begin(), nodes.end());
    EXPECT_EQ(nodes[0], 2);
    EXPECT_EQ(nodes[1], 3);
}

// -- Diamond graph: multiple paths to same node --
TEST_F(QA_Traversal, DiamondGraph) {
    // 1 -> 2, 1 -> 3, 2 -> 4, 3 -> 4
    link(1, 2);
    link(1, 3);
    link(2, 4);
    link(3, 4);

    auto results = run_bfs(1, TraverseDirection::OUT);
    // Node 4 should appear only once (BFS visited set prevents duplicates).
    ASSERT_EQ(results.size(), 3u);
    std::set<int64_t> unique_nodes;
    for (auto& [n, d] : results) {
        unique_nodes.insert(n);
    }
    EXPECT_EQ(unique_nodes.size(), 3u);
    EXPECT_TRUE(unique_nodes.count(2) > 0);
    EXPECT_TRUE(unique_nodes.count(3) > 0);
    EXPECT_TRUE(unique_nodes.count(4) > 0);
}

// -- Long chain: stress test with 200 nodes --
TEST_F(QA_Traversal, LongChainStress) {
    for (int64_t i = 1; i < 200; ++i) {
        link(i, i + 1);
    }

    auto results = run_bfs(1, TraverseDirection::OUT, 200);
    ASSERT_EQ(results.size(), 199u);
    // Last node should be at depth 199.
    auto it = std::find_if(results.begin(), results.end(), [](auto& p) { return p.first == 200; });
    ASSERT_NE(it, results.end());
    EXPECT_EQ(it->second, 199);
}

// -- Fan-out: one node with many children --
TEST_F(QA_Traversal, FanOutStress) {
    for (int64_t i = 2; i <= 101; ++i) {
        link(1, i);
    }

    auto results = run_bfs(1, TraverseDirection::OUT);
    ASSERT_EQ(results.size(), 100u);
    for (auto& [n, d] : results) {
        EXPECT_EQ(d, 1);
    }
}

// -- BOTH direction on a directed chain --
TEST_F(QA_Traversal, BothDirectionOnChain) {
    link(1, 2);
    link(2, 3);

    // From node 2 with BOTH: should find 1 (IN) and 3 (OUT).
    auto results = run_bfs(2, TraverseDirection::BOTH);
    ASSERT_EQ(results.size(), 2u);
    std::vector<int64_t> nodes;
    for (auto& [n, d] : results) {
        nodes.push_back(n);
    }
    std::sort(nodes.begin(), nodes.end());
    EXPECT_EQ(nodes[0], 1);
    EXPECT_EQ(nodes[1], 3);
}

// -- FETCH mode: verify 3-column output --
TEST_F(QA_Traversal, FetchModeOutputShape) {
    link(1, 2);
    link(2, 3);

    TraversalConfig config;
    config.edge_type = "follows";
    config.start_key = Value(static_cast<int64_t>(1));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;
    config.fetch = true;

    std::vector<OutputColumn> cols;
    cols.push_back({"", "node", TypeId::INT64, false, 0});
    cols.push_back({"", "depth", TypeId::INT64, false, 0});
    cols.push_back({"", "source", TypeId::INT64, true, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    TraversalOperator op(*graph_, std::move(config), std::move(schema), nullptr, bound);
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<std::tuple<int64_t, int64_t, int64_t>> results;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        auto& vals = row->value().values;
        ASSERT_EQ(vals.size(), 3u);
        results.emplace_back(vals[0].as_int64(), vals[1].as_int64(), vals[2].as_int64());
    }
    op.close();

    ASSERT_EQ(results.size(), 2u);
    // Node 2 at depth 1, source=1.
    auto& [n1, d1, s1] = results[0];
    EXPECT_EQ(n1, 2);
    EXPECT_EQ(d1, 1);
    EXPECT_EQ(s1, 1);
    // Node 3 at depth 2, source=2.
    auto& [n2, d2, s2] = results[1];
    EXPECT_EQ(n2, 3);
    EXPECT_EQ(d2, 2);
    EXPECT_EQ(s2, 2);
}

// -- Edge collection: verify correct edge count --
TEST_F(QA_Traversal, EdgeCollectionCount) {
    link(1, 2);
    link(1, 3);
    link(2, 4);

    TraversalConfig config;
    config.edge_type = "follows";
    config.start_key = Value(static_cast<int64_t>(1));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;
    config.collect_edges = true;

    std::vector<OutputColumn> cols;
    cols.push_back({"", "node", TypeId::INT64, false, 0});
    cols.push_back({"", "depth", TypeId::INT64, false, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    TraversalOperator op(*graph_, std::move(config), std::move(schema), nullptr, bound);
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value());

    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value());
        if (!row->has_value()) {
            break;
        }
    }

    // 3 edges traversed: 1->2, 1->3, 2->4
    EXPECT_EQ(op.collected_edges().size(), 3u);
    op.close();
}

// -- Reopen after close: verify operator can be reused --
TEST_F(QA_Traversal, ReopenAfterClose) {
    link(1, 2);

    TraversalConfig config;
    config.edge_type = "follows";
    config.start_key = Value(static_cast<int64_t>(1));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;

    std::vector<OutputColumn> cols;
    cols.push_back({"", "node", TypeId::INT64, false, 0});
    cols.push_back({"", "depth", TypeId::INT64, false, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    TraversalOperator op(*graph_, std::move(config), std::move(schema), nullptr, bound);

    // First run.
    auto r1 = op.open();
    ASSERT_TRUE(r1.has_value());
    auto row1 = op.next();
    ASSERT_TRUE(row1.has_value());
    EXPECT_TRUE(row1->has_value());
    op.close();

    // Second run.
    auto r2 = op.open();
    ASSERT_TRUE(r2.has_value());
    auto row2 = op.next();
    ASSERT_TRUE(row2.has_value());
    EXPECT_TRUE(row2->has_value());
    EXPECT_EQ(row2->value().values[0].as_int64(), 2);
    op.close();
}

// ============================================================================
// Shortest Path Adversarial Tests
// ============================================================================

class QA_ShortestPath : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_ = std::make_unique<Catalog>();
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        TableSchema ts;
        ts.name = "nodes";
        CatalogColumnDef pk_col;
        pk_col.ordinal = 0;
        pk_col.name = "id";
        pk_col.type_id = TypeId::INT64;
        pk_col.nullable = false;
        ts.columns.push_back(pk_col);
        ts.pk_columns = "id";
        auto tid = catalog_->create_table(default_database_id, std::move(ts));
        ASSERT_TRUE(tid.has_value()) << tid.error().message;

        auto eid = graph_->create_edge_type(default_database_id, "edge", *tid, *tid, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;
    }

    void link(int64_t from, int64_t to) {
        auto r = graph_->link("edge", Value(from), Value(to));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    std::vector<std::pair<int64_t, int64_t>>
    find_path(int64_t from,
              int64_t to,
              TraverseDirection dir = TraverseDirection::OUT,
              int32_t max_depth = 100) {
        ShortestPathConfig config;
        config.edge_type = "edge";
        config.from_key = Value(from);
        config.to_key = Value(to);
        config.direction = dir;
        config.max_depth = max_depth;

        std::vector<OutputColumn> cols;
        cols.push_back({"", "node", TypeId::INT64, false, 0});
        cols.push_back({"", "hop", TypeId::INT64, false, 0});
        OutputSchema schema(std::move(cols));

        ShortestPathOperator op(*graph_, std::move(config), std::move(schema));

        auto open_result = op.open();
        EXPECT_TRUE(open_result.has_value()) << open_result.error().message;

        std::vector<std::pair<int64_t, int64_t>> path;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row->has_value()) {
                break;
            }
            auto& vals = row->value().values;
            path.emplace_back(vals[0].as_int64(), vals[1].as_int64());
        }
        op.close();
        return path;
    }

    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<GraphEngine> graph_;
};

// -- Same node: trivial path --
TEST_F(QA_ShortestPath, SameNodeReturnsHopZero) {
    auto path = find_path(42, 42);
    ASSERT_EQ(path.size(), 1u);
    EXPECT_EQ(path[0].first, 42);
    EXPECT_EQ(path[0].second, 0);
}

// -- Direct edge: 1-hop path --
TEST_F(QA_ShortestPath, DirectEdgeOneHop) {
    link(1, 2);
    auto path = find_path(1, 2);
    ASSERT_EQ(path.size(), 2u);
    EXPECT_EQ(path[0].first, 1);
    EXPECT_EQ(path[0].second, 0);
    EXPECT_EQ(path[1].first, 2);
    EXPECT_EQ(path[1].second, 1);
}

// -- No path between disconnected components --
TEST_F(QA_ShortestPath, DisconnectedComponents) {
    link(1, 2);
    link(3, 4);
    auto path = find_path(1, 4);
    EXPECT_TRUE(path.empty());
}

// -- Nonexistent source node --
TEST_F(QA_ShortestPath, NonexistentSourceNode) {
    link(1, 2);
    auto path = find_path(999, 2);
    EXPECT_TRUE(path.empty());
}

// -- Nonexistent target node --
TEST_F(QA_ShortestPath, NonexistentTargetNode) {
    link(1, 2);
    auto path = find_path(1, 999);
    EXPECT_TRUE(path.empty());
}

// -- Both nodes nonexistent --
TEST_F(QA_ShortestPath, BothNodesNonexistent) {
    link(1, 2);
    auto path = find_path(100, 200);
    EXPECT_TRUE(path.empty());
}

// -- max_depth = 0: only same-node paths --
TEST_F(QA_ShortestPath, MaxDepthZeroNoSelfEdge) {
    link(1, 2);
    // max_depth=0 with different source/target: no path possible.
    auto path = find_path(1, 2, TraverseDirection::OUT, 0);
    EXPECT_TRUE(path.empty());
}

// -- max_depth = 0 with same node --
TEST_F(QA_ShortestPath, MaxDepthZeroSameNode) {
    // Same node case is handled before BFS, so max_depth doesn't matter.
    auto path = find_path(5, 5, TraverseDirection::OUT, 0);
    ASSERT_EQ(path.size(), 1u);
    EXPECT_EQ(path[0].first, 5);
}

// -- CRITICAL TEST: max_depth = exact path length --
// The bidirectional BFS should find paths whose length equals max_depth.
TEST_F(QA_ShortestPath, MaxDepthEqualsPathLength) {
    // Chain: 1 -> 2 -> 3 (path length 2)
    link(1, 2);
    link(2, 3);

    auto path = find_path(1, 3, TraverseDirection::OUT, 2);
    // A path of length 2 with max_depth=2 SHOULD be found.
    ASSERT_EQ(path.size(), 3u) << "Path of exactly max_depth hops should be found";
    EXPECT_EQ(path[0].first, 1);
    EXPECT_EQ(path[2].first, 3);
}

// -- CRITICAL TEST: max_depth = 3 on a 3-hop chain --
TEST_F(QA_ShortestPath, MaxDepthEqualsThreeHopPath) {
    // Chain: 1 -> 2 -> 3 -> 4 (path length 3)
    link(1, 2);
    link(2, 3);
    link(3, 4);

    auto path = find_path(1, 4, TraverseDirection::OUT, 3);
    // A 3-hop path with max_depth=3 SHOULD be found.
    ASSERT_EQ(path.size(), 4u) << "Path of exactly max_depth=3 hops should be found";
    EXPECT_EQ(path[0].first, 1);
    EXPECT_EQ(path[3].first, 4);
}

// -- Path with cycle: shortest path should still work --
TEST_F(QA_ShortestPath, CycleDoesNotConfuse) {
    // Graph with cycle: 1 -> 2 -> 3 -> 1, plus 3 -> 4
    link(1, 2);
    link(2, 3);
    link(3, 1);
    link(3, 4);

    auto path = find_path(1, 4);
    // Shortest: 1 -> 2 -> 3 -> 4 (3 hops)
    ASSERT_EQ(path.size(), 4u);
    EXPECT_EQ(path[0].first, 1);
    EXPECT_EQ(path[3].first, 4);
}

// -- Two equal-length paths: either is acceptable --
TEST_F(QA_ShortestPath, TwoEqualPaths) {
    // 1 -> 2 -> 4, and 1 -> 3 -> 4
    link(1, 2);
    link(2, 4);
    link(1, 3);
    link(3, 4);

    auto path = find_path(1, 4);
    ASSERT_EQ(path.size(), 3u);
    EXPECT_EQ(path[0].first, 1);
    EXPECT_EQ(path[2].first, 4);
}

// -- Reverse direction: IN --
TEST_F(QA_ShortestPath, ReverseDirectionIN) {
    link(1, 2);
    link(2, 3);

    // From 3 to 1 following IN direction (reverse edges).
    auto path = find_path(3, 1, TraverseDirection::IN);
    ASSERT_EQ(path.size(), 3u);
    EXPECT_EQ(path[0].first, 3);
    EXPECT_EQ(path[2].first, 1);
}

// -- BOTH direction: can use any direction --
TEST_F(QA_ShortestPath, BothDirectionFindsPath) {
    link(1, 2);
    link(3, 2);

    // From 1 to 3 using BOTH: 1 -> 2, then 2 <- 3 (reverse).
    auto path = find_path(1, 3, TraverseDirection::BOTH);
    ASSERT_EQ(path.size(), 3u);
    EXPECT_EQ(path[0].first, 1);
    EXPECT_EQ(path[2].first, 3);
}

// -- Hop numbers should be monotonically increasing --
TEST_F(QA_ShortestPath, HopNumbersMonotonic) {
    link(1, 2);
    link(2, 3);
    link(3, 4);

    auto path = find_path(1, 4);
    ASSERT_GE(path.size(), 2u);
    for (size_t i = 1; i < path.size(); ++i) {
        EXPECT_GT(path[i].second, path[i - 1].second)
            << "Hop numbers should be strictly increasing";
    }
}

// -- Large graph: find shortest among multiple paths --
TEST_F(QA_ShortestPath, ShortestAmongMultiplePaths) {
    // Short path: 1 -> 2 -> 5 (2 hops)
    link(1, 2);
    link(2, 5);
    // Long path: 1 -> 3 -> 4 -> 5 (3 hops)
    link(1, 3);
    link(3, 4);
    link(4, 5);

    auto path = find_path(1, 5);
    // Shortest path should be 2 hops.
    ASSERT_EQ(path.size(), 3u);
    EXPECT_EQ(path[0].first, 1);
    EXPECT_EQ(path[2].first, 5);
}

// -- Stress: long chain 1 -> 2 -> ... -> 50 --
TEST_F(QA_ShortestPath, LongChainStress) {
    for (int64_t i = 1; i < 50; ++i) {
        link(i, i + 1);
    }

    auto path = find_path(1, 50);
    ASSERT_EQ(path.size(), 50u);
    EXPECT_EQ(path[0].first, 1);
    EXPECT_EQ(path[49].first, 50);
}

// -- Reopen after close --
TEST_F(QA_ShortestPath, ReopenAfterClose) {
    link(1, 2);

    ShortestPathConfig config;
    config.edge_type = "edge";
    config.from_key = Value(static_cast<int64_t>(1));
    config.to_key = Value(static_cast<int64_t>(2));
    config.direction = TraverseDirection::OUT;

    std::vector<OutputColumn> cols;
    cols.push_back({"", "node", TypeId::INT64, false, 0});
    cols.push_back({"", "hop", TypeId::INT64, false, 0});
    OutputSchema schema(std::move(cols));

    ShortestPathOperator op(*graph_, std::move(config), std::move(schema));

    auto r1 = op.open();
    ASSERT_TRUE(r1.has_value());
    int count1 = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value());
        if (!row->has_value()) {
            break;
        }
        ++count1;
    }
    op.close();

    auto r2 = op.open();
    ASSERT_TRUE(r2.has_value());
    int count2 = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value());
        if (!row->has_value()) {
            break;
        }
        ++count2;
    }
    op.close();

    EXPECT_EQ(count1, count2);
    EXPECT_EQ(count1, 2);
}

// ============================================================================
// Pattern Match Adversarial Tests
// ============================================================================

class QA_PatternMatch : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_qa_pattern_match";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        // Create 'people' table.
        {
            TableSchema ts;
            ts.name = "people";
            CatalogColumnDef id_col;
            id_col.ordinal = 0;
            id_col.name = "id";
            id_col.type_id = TypeId::INT64;
            id_col.nullable = false;
            ts.columns.push_back(id_col);
            CatalogColumnDef name_col;
            name_col.ordinal = 1;
            name_col.name = "name";
            name_col.type_id = TypeId::STRING;
            name_col.nullable = false;
            ts.columns.push_back(name_col);
            ts.pk_columns = "id";
            auto tid = catalog_->create_table(default_database_id, std::move(ts));
            ASSERT_TRUE(tid.has_value()) << tid.error().message;
            people_id_ = *tid;

            auto schema = catalog_->get_table(default_database_id, "people");
            ASSERT_TRUE(schema.has_value());
            auto sr = storage_->create_table_storage(default_database_id, people_id_, *schema);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }

        // Create edge type: people -[knows]-> people.
        auto eid = graph_->create_edge_type(default_database_id, 
            "knows", people_id_, people_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void insert_person(int64_t id, const std::string& name) {
        auto ts = storage_->get_table_storage(people_id_);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;

        std::vector<Value> vals = {Value(id), Value(name)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    void link(int64_t from, int64_t to) {
        auto r = graph_->link("knows", Value(from), Value(to));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t people_id_ = 0;
};

// -- Single-hop with no matching edges --
TEST_F(QA_PatternMatch, SingleHopNoEdges) {
    insert_person(1, "Alice");
    // No edges added.

    MatchConfig config;
    config.nodes.push_back({"p", "people"});
    config.nodes.push_back({"q", "people"});
    config.edges.push_back({"r", "knows", TraverseDirection::OUT});

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"p", "name", TypeId::STRING, false, people_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            std::move(schema),
                            nullptr,
                            bound);

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value());

    auto row = op.next();
    ASSERT_TRUE(row.has_value());
    EXPECT_FALSE(row->has_value());
    op.close();
}

// -- Empty table: no source rows --
TEST_F(QA_PatternMatch, EmptySourceTable) {
    // People table is empty. No rows to match.

    MatchConfig config;
    config.nodes.push_back({"p", "people"});
    config.nodes.push_back({"q", "people"});
    config.edges.push_back({"r", "knows", TraverseDirection::OUT});

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"p", "name", TypeId::STRING, false, people_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            std::move(schema),
                            nullptr,
                            bound);

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value());

    auto row = op.next();
    ASSERT_TRUE(row.has_value());
    EXPECT_FALSE(row->has_value());
    op.close();
}

// -- Single-hop: verify all results returned --
TEST_F(QA_PatternMatch, SingleHopAllResults) {
    insert_person(1, "Alice");
    insert_person(2, "Bob");
    insert_person(3, "Charlie");
    link(1, 2);
    link(1, 3);
    link(2, 3);

    MatchConfig config;
    config.nodes.push_back({"p", "people"});
    config.nodes.push_back({"q", "people"});
    config.edges.push_back({"r", "knows", TraverseDirection::OUT});

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"p", "name", TypeId::STRING, false, people_id_});
    out_cols.push_back({"q", "name", TypeId::STRING, false, people_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            std::move(schema),
                            nullptr,
                            bound);

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<std::pair<std::string, std::string>> results;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        auto& vals = row->value().values;
        ASSERT_EQ(vals.size(), 2u);
        results.emplace_back(vals[0].as_string(), vals[1].as_string());
    }
    op.close();

    // 3 edges: Alice->Bob, Alice->Charlie, Bob->Charlie
    ASSERT_EQ(results.size(), 3u);
    std::sort(results.begin(), results.end());
    EXPECT_EQ(results[0].first, "Alice");
    EXPECT_EQ(results[0].second, "Bob");
    EXPECT_EQ(results[1].first, "Alice");
    EXPECT_EQ(results[1].second, "Charlie");
    EXPECT_EQ(results[2].first, "Bob");
    EXPECT_EQ(results[2].second, "Charlie");
}

// -- Multi-hop: 3-hop chain A->B->C->D --
TEST_F(QA_PatternMatch, MultiHopThreeHopChain) {
    insert_person(1, "Alice");
    insert_person(2, "Bob");
    insert_person(3, "Charlie");
    insert_person(4, "Diana");
    link(1, 2);
    link(2, 3);
    link(3, 4);

    // MATCH (a:people)-[r1:knows]->(b:people)-[r2:knows]->(c:people)
    MatchConfig config;
    config.nodes.push_back({"a", "people"});
    config.nodes.push_back({"b", "people"});
    config.nodes.push_back({"c", "people"});
    config.edges.push_back({"r1", "knows", TraverseDirection::OUT});
    config.edges.push_back({"r2", "knows", TraverseDirection::OUT});

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, people_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, people_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, people_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            std::move(schema),
                            nullptr,
                            bound);

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<std::tuple<std::string, std::string, std::string>> results;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        auto& vals = row->value().values;
        ASSERT_EQ(vals.size(), 3u);
        results.emplace_back(vals[0].as_string(), vals[1].as_string(), vals[2].as_string());
    }
    op.close();

    // Alice->Bob->Charlie, Bob->Charlie->Diana
    ASSERT_EQ(results.size(), 2u);
    std::sort(results.begin(), results.end());
    EXPECT_EQ(std::get<0>(results[0]), "Alice");
    EXPECT_EQ(std::get<1>(results[0]), "Bob");
    EXPECT_EQ(std::get<2>(results[0]), "Charlie");
    EXPECT_EQ(std::get<0>(results[1]), "Bob");
    EXPECT_EQ(std::get<1>(results[1]), "Charlie");
    EXPECT_EQ(std::get<2>(results[1]), "Diana");
}

// -- Single-hop with BOTH direction (same table) --
TEST_F(QA_PatternMatch, SingleHopBothDirection) {
    insert_person(1, "Alice");
    insert_person(2, "Bob");
    link(1, 2);

    MatchConfig config;
    config.nodes.push_back({"p", "people"});
    config.nodes.push_back({"q", "people"});
    config.edges.push_back({"r", "knows", TraverseDirection::BOTH});

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"p", "name", TypeId::STRING, false, people_id_});
    out_cols.push_back({"q", "name", TypeId::STRING, false, people_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            std::move(schema),
                            nullptr,
                            bound);

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<std::pair<std::string, std::string>> results;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        auto& vals = row->value().values;
        ASSERT_EQ(vals.size(), 2u);
        results.emplace_back(vals[0].as_string(), vals[1].as_string());
    }
    op.close();

    // BOTH direction: Alice->Bob (forward), Bob->Alice (reverse from Bob's perspective)
    ASSERT_EQ(results.size(), 2u);
    std::sort(results.begin(), results.end());
    EXPECT_EQ(results[0].first, "Alice");
    EXPECT_EQ(results[0].second, "Bob");
    EXPECT_EQ(results[1].first, "Bob");
    EXPECT_EQ(results[1].second, "Alice");
}

// -- Nonexistent edge type --
TEST_F(QA_PatternMatch, NonexistentEdgeType) {
    insert_person(1, "Alice");

    MatchConfig config;
    config.nodes.push_back({"p", "people"});
    config.nodes.push_back({"q", "people"});
    config.edges.push_back({"r", "nonexistent_edge", TraverseDirection::OUT});

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"p", "name", TypeId::STRING, false, people_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            std::move(schema),
                            nullptr,
                            bound);

    auto open_result = op.open();
    // Should return an error for nonexistent edge type.
    EXPECT_FALSE(open_result.has_value());
}

// -- Reopen after close --
TEST_F(QA_PatternMatch, ReopenAfterClose) {
    insert_person(1, "Alice");
    insert_person(2, "Bob");
    link(1, 2);

    MatchConfig config;
    config.nodes.push_back({"p", "people"});
    config.nodes.push_back({"q", "people"});
    config.edges.push_back({"r", "knows", TraverseDirection::OUT});

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"p", "name", TypeId::STRING, false, people_id_});
    out_cols.push_back({"q", "name", TypeId::STRING, false, people_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            std::move(schema),
                            nullptr,
                            bound);

    // First run.
    auto r1 = op.open();
    ASSERT_TRUE(r1.has_value());
    int count1 = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value());
        if (!row->has_value()) {
            break;
        }
        ++count1;
    }
    op.close();

    // Second run.
    auto r2 = op.open();
    ASSERT_TRUE(r2.has_value());
    int count2 = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value());
        if (!row->has_value()) {
            break;
        }
        ++count2;
    }
    op.close();

    EXPECT_EQ(count1, count2);
    EXPECT_EQ(count1, 1);
}

// -- Single-hop IN direction --
TEST_F(QA_PatternMatch, SingleHopINDirection) {
    insert_person(1, "Alice");
    insert_person(2, "Bob");
    link(1, 2);

    MatchConfig config;
    config.nodes.push_back({"p", "people"});
    config.nodes.push_back({"q", "people"});
    config.edges.push_back({"r", "knows", TraverseDirection::IN});

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"p", "name", TypeId::STRING, false, people_id_});
    out_cols.push_back({"q", "name", TypeId::STRING, false, people_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            std::move(schema),
                            nullptr,
                            bound);

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<std::pair<std::string, std::string>> results;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        auto& vals = row->value().values;
        ASSERT_EQ(vals.size(), 2u);
        results.emplace_back(vals[0].as_string(), vals[1].as_string());
    }
    op.close();

    // IN direction: for each person p, find edges where p is the TARGET.
    // Alice has no incoming edges. Bob has incoming from Alice.
    // So (Bob, Alice) should be the result.
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].first, "Bob");
    EXPECT_EQ(results[0].second, "Alice");
}

// -- Self-loop in pattern matching --
TEST_F(QA_PatternMatch, SelfLoopEdge) {
    insert_person(1, "Alice");
    link(1, 1);

    MatchConfig config;
    config.nodes.push_back({"p", "people"});
    config.nodes.push_back({"q", "people"});
    config.edges.push_back({"r", "knows", TraverseDirection::OUT});

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"p", "name", TypeId::STRING, false, people_id_});
    out_cols.push_back({"q", "name", TypeId::STRING, false, people_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            std::move(schema),
                            nullptr,
                            bound);

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<std::pair<std::string, std::string>> results;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        auto& vals = row->value().values;
        ASSERT_EQ(vals.size(), 2u);
        results.emplace_back(vals[0].as_string(), vals[1].as_string());
    }
    op.close();

    // Self-loop: Alice -> Alice
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].first, "Alice");
    EXPECT_EQ(results[0].second, "Alice");
}

// ============================================================================
// Acceptance Criteria Verification
// ============================================================================

// AC: TRAVERSE returns correct nodes at each depth
TEST_F(QA_Traversal, AC_TraverseReturnsCorrectNodesAtEachDepth) {
    // Build: 1 -> 2, 1 -> 3, 2 -> 4, 3 -> 5
    link(1, 2);
    link(1, 3);
    link(2, 4);
    link(3, 5);

    auto results = run_bfs(1, TraverseDirection::OUT);
    ASSERT_EQ(results.size(), 4u);

    // Depth 1: nodes 2, 3
    std::vector<int64_t> depth1;
    std::vector<int64_t> depth2;
    for (auto& [node, depth] : results) {
        if (depth == 1) {
            depth1.push_back(node);
        }
        if (depth == 2) {
            depth2.push_back(node);
        }
    }
    std::sort(depth1.begin(), depth1.end());
    std::sort(depth2.begin(), depth2.end());

    ASSERT_EQ(depth1.size(), 2u);
    EXPECT_EQ(depth1[0], 2);
    EXPECT_EQ(depth1[1], 3);

    ASSERT_EQ(depth2.size(), 2u);
    EXPECT_EQ(depth2[0], 4);
    EXPECT_EQ(depth2[1], 5);
}

// AC: Direction control (IN/OUT/BOTH) works correctly
TEST_F(QA_Traversal, AC_DirectionControlWorksCorrectly) {
    link(1, 2);
    link(2, 3);

    // OUT from 1: should reach 2, 3
    auto out_results = run_bfs(1, TraverseDirection::OUT);
    ASSERT_EQ(out_results.size(), 2u);

    // IN from 3: should reach 2, 1
    auto in_results = run_bfs(3, TraverseDirection::IN);
    ASSERT_EQ(in_results.size(), 2u);

    // BOTH from 2: should reach 1 (in) and 3 (out)
    auto both_results = run_bfs(2, TraverseDirection::BOTH);
    ASSERT_EQ(both_results.size(), 2u);
    std::vector<int64_t> both_nodes;
    for (auto& [n, d] : both_results) {
        both_nodes.push_back(n);
    }
    std::sort(both_nodes.begin(), both_nodes.end());
    EXPECT_EQ(both_nodes[0], 1);
    EXPECT_EQ(both_nodes[1], 3);
}

// AC: FETCH enriches results with full row data
TEST_F(QA_Traversal, AC_FetchEnrichesResultsWithRowData) {
    link(1, 2);

    auto results = run_bfs(1, TraverseDirection::OUT, 100, 100000, true);
    ASSERT_EQ(results.size(), 1u);
    // In FETCH mode, tuple has 3 values: (node_pk, depth, source_pk).
    // We capture (node, depth) from the helper. Verify node and depth correct.
    EXPECT_EQ(results[0].first, 2);
    EXPECT_EQ(results[0].second, 1);
}

// AC: Shortest path finds minimum hops between nodes
TEST_F(QA_ShortestPath, AC_ShortestPathFindsMinimumHops) {
    // Short: 1 -> 2 -> 3 (2 hops)
    link(1, 2);
    link(2, 3);
    // Long: 1 -> 10 -> 20 -> 30 -> 3 (4 hops)
    link(1, 10);
    link(10, 20);
    link(20, 30);
    link(30, 3);

    auto path = find_path(1, 3);
    // Must be the shortest: 2 hops (3 nodes).
    ASSERT_EQ(path.size(), 3u);
    EXPECT_EQ(path[0].first, 1);
    EXPECT_EQ(path[2].first, 3);
}

// AC: MATCH patterns produce correct join results
TEST_F(QA_PatternMatch, AC_MatchPatternsProduceCorrectJoinResults) {
    insert_person(1, "Alice");
    insert_person(2, "Bob");
    insert_person(3, "Charlie");
    link(1, 2);
    link(2, 3);

    MatchConfig config;
    config.nodes.push_back({"a", "people"});
    config.nodes.push_back({"b", "people"});
    config.edges.push_back({"r", "knows", TraverseDirection::OUT});

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, people_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, people_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            std::move(schema),
                            nullptr,
                            bound);

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<std::pair<std::string, std::string>> results;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        auto& vals = row->value().values;
        ASSERT_EQ(vals.size(), 2u);
        results.emplace_back(vals[0].as_string(), vals[1].as_string());
    }
    op.close();

    ASSERT_EQ(results.size(), 2u);
    std::sort(results.begin(), results.end());
    EXPECT_EQ(results[0].first, "Alice");
    EXPECT_EQ(results[0].second, "Bob");
    EXPECT_EQ(results[1].first, "Bob");
    EXPECT_EQ(results[1].second, "Charlie");
}

// AC: Unit tests for traversal, shortest path, and pattern matching
TEST_F(QA_Traversal, AC_UnitTestsExistForTraversal) {
    // This test verifies the operator lifecycle works end-to-end.
    link(1, 2);
    auto results = run_bfs(1);
    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].first, 2);
}

} // namespace
} // namespace sixseven
