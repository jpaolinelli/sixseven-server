// QA adversarial tests for GDB-887: iterative Tarjan kEmpty reference fix.
//
// Focus areas:
//   (1) kEmpty branch correctness — nodes that appear only as edge targets
//       (never source) have no adj entry and must take the kEmpty path.
//   (2) Dangling-reference safety — the reference to kEmpty or adj->second
//       must remain valid across the entire while-loop body.
//   (3) Multi-edge / duplicate edges between the same pair.
//   (4) Pure self-loop-only nodes (no other edges) — kEmpty path.
//   (5) Large negative and zero node IDs.
//   (6) Long chain (no back edges) — many nodes that are edge targets but
//       not edge sources, exercising kEmpty on the internal frames.
//   (7) Component count and correctness on adversarial topologies.
//   (8) Single-node graph with no self-loop (impossible via the API, but
//       covered by the pure-target-node path).

#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/strongly_connected_components.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

// ---------------------------------------------------------------------------
// Shared helpers (mirror of the unit-test helpers)
// ---------------------------------------------------------------------------

namespace {

static TableSchema make_node_schema(const std::string& name) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {
        {0, "id", TypeId::INT64, false, ""},
    };
    schema.pk_columns = "id";
    return schema;
}

static Value pk(int64_t v) { return Value(v); }

struct SCCResult {
    int64_t component_id;
    int64_t component_size;
};

std::unordered_map<int64_t, SCCResult> to_scc_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, SCCResult> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 3u);
        auto node_id  = std::get<int64_t>(row.values[0].data());
        auto comp_id  = std::get<int64_t>(row.values[1].data());
        auto comp_sz  = std::get<int64_t>(row.values[2].data());
        result[node_id] = {comp_id, comp_sz};
    }
    return result;
}

int64_t count_components(const std::unordered_map<int64_t, SCCResult>& m) {
    std::unordered_set<int64_t> comps;
    for (const auto& [_, r] : m) comps.insert(r.component_id);
    return static_cast<int64_t>(comps.size());
}

} // namespace

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class QA_GDB887 : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        auto t = catalog_.create_table(default_database_id, make_node_schema("nodes"));
        ASSERT_TRUE(t.has_value()) << t.error().message;
        table_id_ = *t;
    }

    void build_graph(const std::string& edge_type,
                     const std::vector<std::pair<int64_t, int64_t>>& edges) {
        auto et = engine_.create_edge_type(
            default_database_id, edge_type, table_id_, table_id_,
            TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(et.has_value()) << et.error().message;

        for (auto [src, tgt] : edges) {
            auto link = engine_.link(default_database_id, edge_type, pk(src), pk(tgt));
            ASSERT_TRUE(link.has_value()) << link.error().message;
        }
    }

    Result<std::vector<AlgorithmRow>> run(const std::string& edge_type = "follows") {
        AlgorithmContext ctx{engine_, default_database_id, edge_type, {}};
        return strongly_connected_components_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ---------------------------------------------------------------------------
// AC1 / kEmpty branch: pure-sink node — a node that ONLY appears as an edge
// target (never as source).  It has no adj entry; the ternary must bind kEmpty
// and loop zero times without dangling.
//
// Graph: 1->2  (node 2 is a pure sink; its adj entry does not exist)
// Expected: 2 singletons.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB887, PureSinkNodeHitsKEmptyBranch) {
    build_graph("follows", {{1, 2}});

    auto result = run();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    ASSERT_EQ(m.size(), 2u);

    // Node 2 is a pure sink — singleton SCC.
    EXPECT_EQ(m[2].component_id, 2);
    EXPECT_EQ(m[2].component_size, 1);
    // Node 1 is also a singleton.
    EXPECT_EQ(m[1].component_id, 1);
    EXPECT_EQ(m[1].component_size, 1);
    EXPECT_EQ(count_components(m), 2);
}

// ---------------------------------------------------------------------------
// kEmpty branch: chain of sinks — 1->2->3->4->5, where nodes 2,3,4,5 are
// pure sinks in the sense of having no FURTHER out-edges (well, 2 has 3, etc).
// But the LAST node (5) has no adj entry at all. Verify it gets a frame
// pushed onto call_stack and is correctly processed via kEmpty.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB887, ChainWithTerminalSinkHitsKEmpty) {
    build_graph("follows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}});

    auto result = run();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    ASSERT_EQ(m.size(), 5u);

    // No back edges — every node is its own SCC.
    for (int64_t n : {1, 2, 3, 4, 5}) {
        EXPECT_EQ(m[n].component_id, n) << "node " << n;
        EXPECT_EQ(m[n].component_size, 1) << "node " << n;
    }
    EXPECT_EQ(count_components(m), 5);
}

// ---------------------------------------------------------------------------
// kEmpty branch: multiple disconnected pure-sink nodes, each a separate
// DFS start from sorted_nodes.  The DFS initializes each via call_stack,
// immediately hits frame.adj_idx >= neighbors.size() (0 >= 0 via kEmpty),
// pops the SCC root, and exits.  This exercises the "start node has no adj
// entry" path — a distinct code path from a mid-DFS sink.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB887, MultipleDisconnectedSinkNodes) {
    // 10->20  10->30  10->40  (20,30,40 are sinks with no adj entries)
    build_graph("follows", {{10, 20}, {10, 30}, {10, 40}});

    auto result = run();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    ASSERT_EQ(m.size(), 4u);

    EXPECT_EQ(count_components(m), 4); // all singletons
    for (int64_t n : {10, 20, 30, 40}) {
        EXPECT_EQ(m[n].component_size, 1) << "node " << n;
    }
}

// ---------------------------------------------------------------------------
// Self-loop-only node: node 1 has only 1->1 (self-loop), no other edges.
// The self-loop is skipped by the adjacency-building code (src==tgt), so
// node 1 appears in all_nodes but has NO adj entry.  It must hit kEmpty and
// be a singleton.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB887, SelfLoopOnlyNodeIsKEmptySingleton) {
    build_graph("follows", {{1, 1}});

    auto result = run();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[1].component_id, 1);
    EXPECT_EQ(m[1].component_size, 1);
    EXPECT_EQ(count_components(m), 1);
}

// ---------------------------------------------------------------------------
// Self-loop-only node mixed with a cycle: node 99 has only a self-loop
// (kEmpty path), while nodes 1-2-3 form a cycle.  Verify both are correct
// and node 99 is an independent singleton.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB887, SelfLoopMixedWithCycle) {
    build_graph("follows", {{99, 99}, {1, 2}, {2, 3}, {3, 1}});

    auto result = run();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    ASSERT_EQ(m.size(), 4u);

    // Cycle SCC
    for (int64_t n : {1, 2, 3}) {
        EXPECT_EQ(m[n].component_id, 1) << "node " << n;
        EXPECT_EQ(m[n].component_size, 3) << "node " << n;
    }
    // Self-loop singleton
    EXPECT_EQ(m[99].component_id, 99);
    EXPECT_EQ(m[99].component_size, 1);
    EXPECT_EQ(count_components(m), 2);
}

// ---------------------------------------------------------------------------
// Duplicate edges (multi-edge): two edges 1->2 and 1->2 inserted.
// adj[1] = {2, 2}.  When the algorithm processes the second neighbor entry
// for 2, disc[2] already exists, so it takes the back/cross-edge branch.
// Result must still be 2 singletons (not a cycle, no back-edge from 2->1).
// ---------------------------------------------------------------------------

TEST_F(QA_GDB887, DuplicateEdgesDoNotCreateFalseCycle) {
    // Insert the same edge twice.
    build_graph("follows", {{1, 2}, {1, 2}});

    auto result = run();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    ASSERT_EQ(m.size(), 2u);

    EXPECT_EQ(m[1].component_id, 1);
    EXPECT_EQ(m[1].component_size, 1);
    EXPECT_EQ(m[2].component_id, 2);
    EXPECT_EQ(m[2].component_size, 1);
    EXPECT_EQ(count_components(m), 2);
}

// ---------------------------------------------------------------------------
// Duplicate edges that form a cycle: 1->2, 2->1, 2->1 (extra back edge).
// Result: still one SCC of size 2.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB887, DuplicateBackEdgesStillOneSCC) {
    build_graph("follows", {{1, 2}, {2, 1}, {2, 1}});

    auto result = run();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    ASSERT_EQ(m.size(), 2u);

    EXPECT_EQ(m[1].component_id, 1);
    EXPECT_EQ(m[1].component_size, 2);
    EXPECT_EQ(m[2].component_id, 1);
    EXPECT_EQ(m[2].component_size, 2);
    EXPECT_EQ(count_components(m), 1);
}

// ---------------------------------------------------------------------------
// Zero and negative node IDs: the algorithm uses int64_t throughout.
// A node with id 0 must be handled (and was used as hub in the unit test).
// Negative IDs are legitimate int64_t values.
// Graph: -5 -> -3 -> -1 -> -5 (cycle), -1 -> 0 (bridge), 0 is a sink.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB887, NegativeAndZeroNodeIds) {
    build_graph("follows", {{-5, -3}, {-3, -1}, {-1, -5}, {-1, 0}});

    auto result = run();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    ASSERT_EQ(m.size(), 4u);

    // SCC: {-5, -3, -1}, component_id = -5 (smallest).
    for (int64_t n : {-5, -3, -1}) {
        EXPECT_EQ(m[n].component_id, -5) << "node " << n;
        EXPECT_EQ(m[n].component_size, 3) << "node " << n;
    }
    // Node 0 is a pure sink — singleton, kEmpty branch.
    EXPECT_EQ(m[0].component_id, 0);
    EXPECT_EQ(m[0].component_size, 1);
    EXPECT_EQ(count_components(m), 2);
}

// ---------------------------------------------------------------------------
// Large disconnected graph: 200 independent singleton edges (A->B, no back).
// All 400 nodes are singletons; 200 of them (the targets) hit kEmpty.
// This stresses both the kEmpty path and the sorted_nodes ordering.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB887, ManyDisconnectedSinkEdgesAllSingletons) {
    const int kPairs = 200;
    std::vector<std::pair<int64_t, int64_t>> edges;
    edges.reserve(static_cast<size_t>(kPairs));
    for (int i = 0; i < kPairs; ++i) {
        // src = i*2, tgt = i*2+1 (all disjoint pairs)
        edges.push_back({static_cast<int64_t>(i * 2), static_cast<int64_t>(i * 2 + 1)});
    }
    build_graph("follows", edges);

    auto result = run();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    ASSERT_EQ(m.size(), static_cast<size_t>(kPairs * 2));

    // Every node must be a singleton.
    for (const auto& [node, r] : m) {
        EXPECT_EQ(r.component_size, 1) << "node " << node << " should be singleton";
        EXPECT_EQ(r.component_id, node) << "node " << node << " should self-label";
    }
    EXPECT_EQ(count_components(m), kPairs * 2);
}

// ---------------------------------------------------------------------------
// High-degree hub with kEmpty: hub 0 has 300 out-edges to leaves 1..300.
// Only the hub->leaf direction (no back edges). Every leaf is a pure-sink
// kEmpty node.  Result: 1 singleton for hub, 300 singletons for leaves.
// Verifies kEmpty does not corrupt hub's adj iteration when the DFS visits
// a leaf (which immediately hits kEmpty and pops back to hub's frame).
// ---------------------------------------------------------------------------

TEST_F(QA_GDB887, HighDegreeHubWithPureSinkLeaves) {
    const int64_t kDeg = 300;
    std::vector<std::pair<int64_t, int64_t>> edges;
    edges.reserve(static_cast<size_t>(kDeg));
    for (int64_t leaf = 1; leaf <= kDeg; ++leaf) {
        edges.push_back({0, leaf}); // one-way: hub -> leaf only
    }
    build_graph("follows", edges);

    auto result = run();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    ASSERT_EQ(m.size(), static_cast<size_t>(kDeg + 1));

    // Hub is a singleton (no back-edges from leaves).
    EXPECT_EQ(m[0].component_id, 0);
    EXPECT_EQ(m[0].component_size, 1);

    // Every leaf is a singleton (kEmpty branch).
    for (int64_t leaf = 1; leaf <= kDeg; ++leaf) {
        EXPECT_EQ(m[leaf].component_id, leaf) << "leaf " << leaf;
        EXPECT_EQ(m[leaf].component_size, 1) << "leaf " << leaf;
    }
    EXPECT_EQ(count_components(m), kDeg + 1);
}

// ---------------------------------------------------------------------------
// Cross-edge (forward edge) correctness: node u's DFS visits w, which is
// already visited but NOT on on_stack (cross edge). The low-link of u must
// NOT be updated (only back-edges on on_stack update low).
// Graph: 1->2->3, 3->2 (back), 1->3 (forward/cross after 2 finishes).
// SCCs: {2,3} and {1}.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB887, CrossEdgeDoesNotMergeComponents) {
    // 1->2, 2->3, 3->2 (back, merges 2 and 3), 1->3 (cross after {2,3} done)
    build_graph("follows", {{1, 2}, {2, 3}, {3, 2}, {1, 3}});

    auto result = run();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    ASSERT_EQ(m.size(), 3u);

    // {2,3} form a cycle — one SCC.
    EXPECT_EQ(m[2].component_id, 2);
    EXPECT_EQ(m[2].component_size, 2);
    EXPECT_EQ(m[3].component_id, 2);
    EXPECT_EQ(m[3].component_size, 2);
    // {1} is a singleton (1 can reach {2,3} but {2,3} cannot reach 1).
    EXPECT_EQ(m[1].component_id, 1);
    EXPECT_EQ(m[1].component_size, 1);
    EXPECT_EQ(count_components(m), 2);
}

// ---------------------------------------------------------------------------
// Output completeness: every node from all_nodes must appear in the result.
// A mixed graph with sinks, sources, and cycle members — verify row count.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB887, OutputContainsEveryNode) {
    // Mix: cycle {1,2,3}, DAG chain 4->5->6 (6 is kEmpty sink),
    // isolated self-loop 7->7 (kEmpty after self-loop skip),
    // extra sink 8 (target of 3).
    build_graph("follows", {
        {1, 2}, {2, 3}, {3, 1}, // cycle
        {4, 5}, {5, 6},          // chain
        {7, 7},                  // self-loop only
        {3, 8},                  // extra sink from cycle
    });

    auto result = run();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Nodes: 1,2,3,4,5,6,7,8 = 8 total
    auto m = to_scc_map(*result);
    ASSERT_EQ(m.size(), 8u);

    // Cycle
    for (int64_t n : {1, 2, 3}) {
        EXPECT_EQ(m[n].component_id, 1) << "node " << n;
        EXPECT_EQ(m[n].component_size, 3) << "node " << n;
    }
    // DAG singletons
    for (int64_t n : {4, 5, 6}) {
        EXPECT_EQ(m[n].component_size, 1) << "node " << n;
    }
    // Self-loop singleton (kEmpty)
    EXPECT_EQ(m[7].component_id, 7);
    EXPECT_EQ(m[7].component_size, 1);
    // Extra sink (kEmpty)
    EXPECT_EQ(m[8].component_id, 8);
    EXPECT_EQ(m[8].component_size, 1);
}

// ---------------------------------------------------------------------------
// Component size sum must equal total node count.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB887, ComponentSizesSumToNodeCount) {
    build_graph("follows", {
        {1, 2}, {2, 3}, {3, 1},  // SCC size 3
        {4, 5}, {5, 4},           // SCC size 2
        {6, 7},                   // two singletons
        {8, 8},                   // self-loop -> singleton (kEmpty)
    });

    auto result = run();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);

    // Sum of all component_size values must equal total node count
    // (each node counted once).
    std::unordered_map<int64_t, int64_t> seen_comp;
    int64_t total = 0;
    for (const auto& [node, r] : m) {
        if (seen_comp.find(r.component_id) == seen_comp.end()) {
            seen_comp[r.component_id] = r.component_size;
            total += r.component_size;
        }
    }
    EXPECT_EQ(total, static_cast<int64_t>(m.size()));
}

// ---------------------------------------------------------------------------
// Single big cycle — N nodes in one ring. Every node has exactly one
// out-neighbor (adj entry of size 1), so no kEmpty is exercised, but this
// verifies correctness is preserved end-to-end.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB887, SingleBigCycle) {
    const int64_t kN = 100;
    std::vector<std::pair<int64_t, int64_t>> edges;
    edges.reserve(static_cast<size_t>(kN));
    for (int64_t i = 1; i <= kN; ++i) {
        edges.push_back({i, (i % kN) + 1}); // 1->2->...->100->1
    }
    build_graph("follows", edges);

    auto result = run();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    ASSERT_EQ(m.size(), static_cast<size_t>(kN));

    // All must be in one SCC; component_id = 1 (smallest).
    for (int64_t n = 1; n <= kN; ++n) {
        EXPECT_EQ(m[n].component_id, 1) << "node " << n;
        EXPECT_EQ(m[n].component_size, kN) << "node " << n;
    }
    EXPECT_EQ(count_components(m), 1);
}
