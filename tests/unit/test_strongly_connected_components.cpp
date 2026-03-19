#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/strongly_connected_components.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

static TableSchema make_table_schema(const std::string& name) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {
        {0, "id", TypeId::INT64, false, ""},
    };
    schema.pk_columns = "id";
    return schema;
}

static Value pk(int64_t v) {
    return Value(v);
}

/// Extract per-node SCC results: node_id -> {component_id, component_size}.
struct SCCResult {
    int64_t component_id;
    int64_t component_size;
};

std::unordered_map<int64_t, SCCResult> to_scc_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, SCCResult> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 3u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto comp_id = std::get<int64_t>(row.values[1].data());
        auto comp_size = std::get<int64_t>(row.values[2].data());
        result[node_id] = {comp_id, comp_size};
    }
    return result;
}

/// Count distinct components in the result.
int64_t count_components(const std::unordered_map<int64_t, SCCResult>& m) {
    std::unordered_set<int64_t> comps;
    for (const auto& [_, r] : m) {
        comps.insert(r.component_id);
    }
    return static_cast<int64_t>(comps.size());
}

} // namespace

// ---------------------------------------------------------------------------
// Algorithm definition tests
// ---------------------------------------------------------------------------

TEST(SCCDef, OutputSchema) {
    auto def = make_strongly_connected_components_def();
    EXPECT_EQ(def.name, "strongly_connected_components");
    ASSERT_EQ(def.output_columns.size(), 3u);
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[1].name, "component_id");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[2].name, "component_size");
    EXPECT_EQ(def.output_columns[2].type_id, TypeId::INT64);
}

TEST(SCCDef, NoParameters) {
    auto def = make_strongly_connected_components_def();
    EXPECT_TRUE(def.params.empty());
}

TEST(SCCDef, Registration) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_strongly_connected_components_def(),
                                              strongly_connected_components_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto* entry = registry.find("strongly_connected_components");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "strongly_connected_components");
}

// ---------------------------------------------------------------------------
// Test fixture with a GraphEngine
// ---------------------------------------------------------------------------

class SCCTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto t = catalog_.create_table(default_database_id, make_table_schema("nodes"));
        ASSERT_TRUE(t.has_value()) << t.error().message;
        table_id_ = *t;
    }

    /// Create an edge type and link a list of (src, tgt) pairs.
    void build_graph(const std::string& edge_type,
                     const std::vector<std::pair<int64_t, int64_t>>& edges) {
        auto et = engine_.create_edge_type(default_database_id, 
            edge_type, table_id_, table_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(et.has_value()) << et.error().message;

        for (auto [src, tgt] : edges) {
            auto link = engine_.link(edge_type, pk(src), pk(tgt));
            ASSERT_TRUE(link.has_value()) << link.error().message;
        }
    }

    /// Run strongly_connected_components.
    Result<std::vector<AlgorithmRow>> run(const std::string& edge_type) {
        AlgorithmContext ctx{engine_, edge_type, {}};
        return strongly_connected_components_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ---------------------------------------------------------------------------
// Empty graph
// ---------------------------------------------------------------------------

TEST_F(SCCTest, EmptyGraph) {
    build_graph("follows", {});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

// ---------------------------------------------------------------------------
// Single node (via self-loop) — one SCC of size 1
// ---------------------------------------------------------------------------

TEST_F(SCCTest, SingleNodeSelfLoop) {
    build_graph("follows", {{1, 1}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 1u);
    EXPECT_EQ(m[1].component_id, 1);
    EXPECT_EQ(m[1].component_size, 1);
}

// ---------------------------------------------------------------------------
// Simple cycle (single SCC) — 1->2->3->1
// ---------------------------------------------------------------------------

TEST_F(SCCTest, SimpleCycle) {
    build_graph("follows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 3u);

    // All in same component, component_id = smallest = 1
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 3) << "node " << node;
    }
    EXPECT_EQ(count_components(m), 1);
}

// ---------------------------------------------------------------------------
// DAG — each node is its own SCC
// ---------------------------------------------------------------------------

TEST_F(SCCTest, DAG) {
    // 1->2->3->4 (no back edges)
    build_graph("follows", {{1, 2}, {2, 3}, {3, 4}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 4u);

    // Each node is its own SCC.
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_EQ(m[node].component_id, node) << "node " << node;
        EXPECT_EQ(m[node].component_size, 1) << "node " << node;
    }
    EXPECT_EQ(count_components(m), 4);
}

// ---------------------------------------------------------------------------
// Two SCCs connected by bridge edge
// ---------------------------------------------------------------------------

TEST_F(SCCTest, TwoSCCsWithBridge) {
    // SCC1: {1, 2, 3} cycle
    // SCC2: {4, 5} cycle
    // Bridge: 3->4 (one-way, does not merge SCCs)
    build_graph("follows", {{1, 2}, {2, 3}, {3, 1}, {3, 4}, {4, 5}, {5, 4}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 5u);

    // SCC1: {1, 2, 3}, component_id = 1
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 3) << "node " << node;
    }

    // SCC2: {4, 5}, component_id = 4
    for (int64_t node : {4, 5}) {
        EXPECT_EQ(m[node].component_id, 4) << "node " << node;
        EXPECT_EQ(m[node].component_size, 2) << "node " << node;
    }

    EXPECT_EQ(count_components(m), 2);
}

// ---------------------------------------------------------------------------
// Fully connected directed graph — single SCC
// ---------------------------------------------------------------------------

TEST_F(SCCTest, FullyConnectedGraph) {
    // All pairs connected bidirectionally.
    build_graph("follows",
                {
                    {1, 2},
                    {1, 3},
                    {1, 4},
                    {2, 1},
                    {2, 3},
                    {2, 4},
                    {3, 1},
                    {3, 2},
                    {3, 4},
                    {4, 1},
                    {4, 2},
                    {4, 3},
                });

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 4u);

    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 4) << "node " << node;
    }
    EXPECT_EQ(count_components(m), 1);
}

// ---------------------------------------------------------------------------
// Classic Tarjan example — multiple SCCs
// ---------------------------------------------------------------------------

TEST_F(SCCTest, ClassicTarjanExample) {
    // Graph from Tarjan's original paper example:
    //   1->2, 2->3, 3->1  (SCC: {1,2,3})
    //   3->4, 4->5, 5->6, 6->4  (SCC: {4,5,6})
    //   6->7  (SCC: {7} — singleton)
    build_graph("follows", {{1, 2}, {2, 3}, {3, 1}, {3, 4}, {4, 5}, {5, 6}, {6, 4}, {6, 7}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 7u);

    // SCC {1,2,3}
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 3) << "node " << node;
    }

    // SCC {4,5,6}
    for (int64_t node : {4, 5, 6}) {
        EXPECT_EQ(m[node].component_id, 4) << "node " << node;
        EXPECT_EQ(m[node].component_size, 3) << "node " << node;
    }

    // Singleton {7}
    EXPECT_EQ(m[7].component_id, 7);
    EXPECT_EQ(m[7].component_size, 1);

    EXPECT_EQ(count_components(m), 3);
}

// ---------------------------------------------------------------------------
// Component IDs are stable/deterministic across runs
// ---------------------------------------------------------------------------

TEST_F(SCCTest, DeterministicComponentIds) {
    build_graph("follows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}, {5, 4}});

    // Run twice and verify identical results.
    auto r1 = run("follows");
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    auto r2 = run("follows");
    ASSERT_TRUE(r2.has_value()) << r2.error().message;

    ASSERT_EQ(r1->size(), r2->size());
    for (size_t i = 0; i < r1->size(); ++i) {
        for (size_t j = 0; j < r1->at(i).values.size(); ++j) {
            EXPECT_EQ(std::get<int64_t>(r1->at(i).values[j].data()),
                      std::get<int64_t>(r2->at(i).values[j].data()));
        }
    }
}

// ---------------------------------------------------------------------------
// Component size accuracy
// ---------------------------------------------------------------------------

TEST_F(SCCTest, ComponentSizeAccuracy) {
    // 3 SCCs of sizes 1, 2, 3
    build_graph("follows",
                {
                    {1, 2},
                    {2, 3},
                    {3, 1}, // SCC size 3
                    {4, 5},
                    {5, 4}, // SCC size 2
                    {6, 7}, // 6 and 7 are singletons (no back edge)
                });

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);

    // Verify component sizes.
    EXPECT_EQ(m[1].component_size, 3);
    EXPECT_EQ(m[4].component_size, 2);
    EXPECT_EQ(m[6].component_size, 1);
    EXPECT_EQ(m[7].component_size, 1);

    // Verify component_id is smallest node in component.
    EXPECT_EQ(m[1].component_id, 1);
    EXPECT_EQ(m[2].component_id, 1);
    EXPECT_EQ(m[3].component_id, 1);
    EXPECT_EQ(m[4].component_id, 4);
    EXPECT_EQ(m[5].component_id, 4);
    EXPECT_EQ(m[6].component_id, 6);
    EXPECT_EQ(m[7].component_id, 7);
}

// ---------------------------------------------------------------------------
// Undirected graph — all connected nodes form single component
// ---------------------------------------------------------------------------

TEST_F(SCCTest, UndirectedGraphSingleComponent) {
    // Bidirectional edges make all reachable nodes a single SCC.
    build_graph("follows",
                {
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {3, 4},
                    {4, 3},
                });

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 4u);

    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 4) << "node " << node;
    }
    EXPECT_EQ(count_components(m), 1);
}

// ---------------------------------------------------------------------------
// Single edge (no cycle) — two singletons
// ---------------------------------------------------------------------------

TEST_F(SCCTest, SingleEdgeNoBackEdge) {
    build_graph("follows", {{1, 2}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 2u);

    EXPECT_EQ(m[1].component_id, 1);
    EXPECT_EQ(m[1].component_size, 1);
    EXPECT_EQ(m[2].component_id, 2);
    EXPECT_EQ(m[2].component_size, 1);
}

// ---------------------------------------------------------------------------
// Results ordered by node_id
// ---------------------------------------------------------------------------

TEST_F(SCCTest, ResultsOrderedByNodeId) {
    build_graph("follows", {{5, 3}, {3, 1}, {1, 5}, {2, 4}, {4, 2}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (size_t i = 1; i < result->size(); ++i) {
        auto prev = std::get<int64_t>((*result)[i - 1].values[0].data());
        auto curr = std::get<int64_t>((*result)[i].values[0].data());
        EXPECT_LT(prev, curr) << "results should be sorted by node_id";
    }
}

// ---------------------------------------------------------------------------
// Nonexistent edge type fails
// ---------------------------------------------------------------------------

TEST_F(SCCTest, NonexistentEdgeTypeFails) {
    build_graph("follows", {{1, 2}});

    auto result = run("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// ---------------------------------------------------------------------------
// Self-loop does not create a multi-node SCC
// ---------------------------------------------------------------------------

TEST_F(SCCTest, SelfLoopIgnored) {
    // Self-loop on node 1, plus a DAG edge 1->2.
    build_graph("follows", {{1, 1}, {1, 2}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 2u);

    // Both nodes are singletons (self-loop does not form a cycle with others).
    EXPECT_EQ(m[1].component_id, 1);
    EXPECT_EQ(m[1].component_size, 1);
    EXPECT_EQ(m[2].component_id, 2);
    EXPECT_EQ(m[2].component_size, 1);
}

// ---------------------------------------------------------------------------
// Disconnected graph — multiple independent SCCs
// ---------------------------------------------------------------------------

TEST_F(SCCTest, DisconnectedCycles) {
    // Two independent cycles with no edges between them.
    build_graph("follows",
                {
                    {1, 2},
                    {2, 3},
                    {3, 1}, // Cycle 1
                    {10, 11},
                    {11, 10}, // Cycle 2
                });

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 5u);

    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 3) << "node " << node;
    }
    for (int64_t node : {10, 11}) {
        EXPECT_EQ(m[node].component_id, 10) << "node " << node;
        EXPECT_EQ(m[node].component_size, 2) << "node " << node;
    }

    EXPECT_EQ(count_components(m), 2);
}

// ---------------------------------------------------------------------------
// Component ID is smallest node in component
// ---------------------------------------------------------------------------

TEST_F(SCCTest, ComponentIdIsSmallestNode) {
    // Cycle with non-sequential IDs.
    build_graph("follows", {{100, 50}, {50, 200}, {200, 100}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 3u);

    // component_id should be 50 (smallest in the SCC).
    for (int64_t node : {50, 100, 200}) {
        EXPECT_EQ(m[node].component_id, 50) << "node " << node;
        EXPECT_EQ(m[node].component_size, 3) << "node " << node;
    }
}
