/// @file test_qa_gdb_524.cpp
/// QA adversarial tests for GDB-524: Strongly Connected Components algorithm.
///
/// Verifies:
///   AC1: strongly_connected_components() function registered and callable.
///   AC2: Tarjan's algorithm correctly identifies all SCCs.
///   AC3: Component IDs are stable and deterministic.
///   AC4: Handles DAGs (each node is its own SCC).
///   AC5: Handles fully connected graphs (single SCC).
///   AC6: Unit tests written and passing.
///   AC7: No regressions.
///
/// Adversarial categories: boundary values, error paths, graph topology
/// edge cases, self-loops, DAGs, cycles, complex SCC structures,
/// determinism, large/negative node IDs, stress tests.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/strongly_connected_components.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Helpers
// ============================================================================

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

/// Verify that all nodes with the same component_id agree on component_size.
void verify_consistent_sizes(const std::unordered_map<int64_t, SCCResult>& m) {
    std::unordered_map<int64_t, int64_t> expected_size;
    for (const auto& [node, r] : m) {
        if (expected_size.count(r.component_id) > 0) {
            EXPECT_EQ(expected_size[r.component_id], r.component_size)
                << "node " << node << " has inconsistent component_size for component "
                << r.component_id;
        } else {
            expected_size[r.component_id] = r.component_size;
        }
    }
}

/// Verify that component_size matches the actual count of nodes in each component.
void verify_size_matches_count(const std::unordered_map<int64_t, SCCResult>& m) {
    std::unordered_map<int64_t, int64_t> actual_count;
    for (const auto& [_, r] : m) {
        ++actual_count[r.component_id];
    }
    for (const auto& [node, r] : m) {
        EXPECT_EQ(r.component_size, actual_count[r.component_id])
            << "node " << node << " component_size doesn't match actual count";
    }
}

/// Verify component_id is the smallest node_id in its component.
void verify_component_id_is_min(const std::unordered_map<int64_t, SCCResult>& m) {
    std::unordered_map<int64_t, int64_t> min_node;
    for (const auto& [node, r] : m) {
        if (min_node.count(r.component_id) == 0 || node < min_node[r.component_id]) {
            min_node[r.component_id] = node;
        }
    }
    for (const auto& [comp_id, min_id] : min_node) {
        EXPECT_EQ(comp_id, min_id)
            << "component_id " << comp_id << " should equal smallest node " << min_id;
    }
}

// ============================================================================
// Fixture
// ============================================================================

class GDB524_SCCQA : public ::testing::Test {
protected:
    void SetUp() override {
        auto t = catalog_.create_table(default_database_id, make_table_schema("nodes"));
        ASSERT_TRUE(t.has_value()) << t.error().message;
        table_id_ = *t;
    }

    void build_graph(const std::string& edge_type,
                     const std::vector<std::pair<int64_t, int64_t>>& edges) {
        auto et = engine_.create_edge_type(
            default_database_id, edge_type, table_id_, table_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(et.has_value()) << et.error().message;
        for (auto [src, tgt] : edges) {
            auto link = engine_.link(default_database_id, edge_type, pk(src), pk(tgt));
            ASSERT_TRUE(link.has_value()) << link.error().message;
        }
    }

    Result<std::vector<AlgorithmRow>> run(const std::string& edge_type) {
        AlgorithmContext ctx{engine_, default_database_id, edge_type, {}};
        return strongly_connected_components_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ============================================================================
// AC1: Function registered and callable
// ============================================================================

TEST_F(GDB524_SCCQA, AC1_RegistrationAndLookup) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_strongly_connected_components_def(),
                                              strongly_connected_components_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_TRUE(registry.has("strongly_connected_components"));
    auto* entry = registry.find("strongly_connected_components");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "strongly_connected_components");
}

TEST_F(GDB524_SCCQA, AC1_CaseInsensitiveLookup) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_strongly_connected_components_def(),
                                              strongly_connected_components_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_TRUE(registry.has("STRONGLY_CONNECTED_COMPONENTS"));
    EXPECT_TRUE(registry.has("Strongly_Connected_Components"));
    EXPECT_NE(registry.find("STRONGLY_CONNECTED_COMPONENTS"), nullptr);
}

TEST_F(GDB524_SCCQA, AC1_DuplicateRegistrationFails) {
    AlgorithmRegistry registry;
    auto r1 = registry.register_algorithm(make_strongly_connected_components_def(),
                                          strongly_connected_components_execute);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    auto r2 = registry.register_algorithm(make_strongly_connected_components_def(),
                                          strongly_connected_components_execute);
    EXPECT_FALSE(r2.has_value());
}

TEST_F(GDB524_SCCQA, AC1_OutputSchemaCorrect) {
    auto def = make_strongly_connected_components_def();
    ASSERT_EQ(def.output_columns.size(), 3u);
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[1].name, "component_id");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[2].name, "component_size");
    EXPECT_EQ(def.output_columns[2].type_id, TypeId::INT64);
}

TEST_F(GDB524_SCCQA, AC1_NoParameters) {
    auto def = make_strongly_connected_components_def();
    EXPECT_TRUE(def.params.empty());
}

TEST_F(GDB524_SCCQA, AC1_CallableOnSimpleGraph) {
    build_graph("follows", {{1, 2}, {2, 3}, {3, 1}});
    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 3u);
}

// ============================================================================
// AC2: Tarjan's algorithm correctly identifies all SCCs
// ============================================================================

TEST_F(GDB524_SCCQA, AC2_SimpleCycle) {
    build_graph("follows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 3u);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 3) << "node " << node;
    }
    EXPECT_EQ(count_components(m), 1);
}

TEST_F(GDB524_SCCQA, AC2_ClassicTarjan) {
    // SCC1: {1,2,3}, SCC2: {4,5,6}, singleton {7}
    build_graph("follows", {{1, 2}, {2, 3}, {3, 1}, {3, 4}, {4, 5}, {5, 6}, {6, 4}, {6, 7}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 7u);

    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 3) << "node " << node;
    }
    for (int64_t node : {4, 5, 6}) {
        EXPECT_EQ(m[node].component_id, 4) << "node " << node;
        EXPECT_EQ(m[node].component_size, 3) << "node " << node;
    }
    EXPECT_EQ(m[7].component_id, 7);
    EXPECT_EQ(m[7].component_size, 1);
    EXPECT_EQ(count_components(m), 3);
    verify_consistent_sizes(m);
    verify_size_matches_count(m);
    verify_component_id_is_min(m);
}

TEST_F(GDB524_SCCQA, AC2_TwoSCCsWithBridge) {
    build_graph("follows", {{1, 2}, {2, 3}, {3, 1}, {3, 4}, {4, 5}, {5, 4}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 5u);

    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 3) << "node " << node;
    }
    for (int64_t node : {4, 5}) {
        EXPECT_EQ(m[node].component_id, 4) << "node " << node;
        EXPECT_EQ(m[node].component_size, 2) << "node " << node;
    }
    EXPECT_EQ(count_components(m), 2);
}

TEST_F(GDB524_SCCQA, AC2_NestedSCCStructure) {
    // Graph with cross edges that could confuse Tarjan's:
    // SCC1: {1,2,3}, SCC2: {4,5}, SCC3: {6}
    // Cross edges: 1->4, 4->6, 3->6
    build_graph("follows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}, {5, 4}, {1, 4}, {4, 6}, {3, 6}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 6u);

    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
    }
    for (int64_t node : {4, 5}) {
        EXPECT_EQ(m[node].component_id, 4) << "node " << node;
    }
    EXPECT_EQ(m[6].component_id, 6);
    EXPECT_EQ(m[6].component_size, 1);
    verify_size_matches_count(m);
    verify_component_id_is_min(m);
}

TEST_F(GDB524_SCCQA, AC2_DiamondDAGWithBackEdge) {
    // Diamond: 1->2, 1->3, 2->4, 3->4, plus back edge 4->1 forming one big SCC.
    build_graph("follows", {{1, 2}, {1, 3}, {2, 4}, {3, 4}, {4, 1}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 4u);

    // All 4 nodes should be in the same SCC since there's a cycle through all.
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 4) << "node " << node;
    }
    EXPECT_EQ(count_components(m), 1);
}

TEST_F(GDB524_SCCQA, AC2_LongCycleWithShortcut) {
    // Long cycle 1->2->3->4->5->1, plus shortcut 1->3.
    // All nodes reachable from each other => single SCC.
    build_graph("follows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 1}, {1, 3}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    for (int64_t node : {1, 2, 3, 4, 5}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 5) << "node " << node;
    }
}

TEST_F(GDB524_SCCQA, AC2_FigureEightGraph) {
    // Two cycles sharing node 3: {1,2,3} and {3,4,5}.
    // But since they share node 3 and both are reachable, this could merge
    // into one SCC depending on edge directions.
    // 1->2->3->1 and 3->4->5->3 => one big SCC? No, two SCCs!
    // Actually: from 1, you can reach 3, then 4,5 via 3->4->5->3.
    // From 4, can you reach 1? 4->5->3->1. Yes!
    // So all 5 nodes are in one SCC.
    build_graph("follows", {{1, 2}, {2, 3}, {3, 1}, {3, 4}, {4, 5}, {5, 3}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 5u);

    for (int64_t node : {1, 2, 3, 4, 5}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 5) << "node " << node;
    }
    EXPECT_EQ(count_components(m), 1);
}

TEST_F(GDB524_SCCQA, AC2_FigureEightNotMerged) {
    // Two cycles sharing node 3 but NOT merging:
    // 1->2->3->1 (SCC: {1,2,3}) and 3->4->5->4 (SCC: {4,5})
    // Node 3 can reach 4 and 5, but 4 and 5 cannot reach 1,2.
    build_graph("follows", {{1, 2}, {2, 3}, {3, 1}, {3, 4}, {4, 5}, {5, 4}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 5u);

    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 3) << "node " << node;
    }
    for (int64_t node : {4, 5}) {
        EXPECT_EQ(m[node].component_id, 4) << "node " << node;
        EXPECT_EQ(m[node].component_size, 2) << "node " << node;
    }
}

// ============================================================================
// AC3: Component IDs are stable and deterministic
// ============================================================================

TEST_F(GDB524_SCCQA, AC3_DeterministicAcrossRuns) {
    build_graph("follows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}, {5, 4}});

    auto r1 = run("follows");
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    auto r2 = run("follows");
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    auto r3 = run("follows");
    ASSERT_TRUE(r3.has_value()) << r3.error().message;

    ASSERT_EQ(r1->size(), r2->size());
    ASSERT_EQ(r2->size(), r3->size());
    for (size_t i = 0; i < r1->size(); ++i) {
        for (size_t j = 0; j < 3; ++j) {
            auto v1 = std::get<int64_t>(r1->at(i).values[j].data());
            auto v2 = std::get<int64_t>(r2->at(i).values[j].data());
            auto v3 = std::get<int64_t>(r3->at(i).values[j].data());
            EXPECT_EQ(v1, v2);
            EXPECT_EQ(v2, v3);
        }
    }
}

TEST_F(GDB524_SCCQA, AC3_ComponentIdIsSmallestNodeInComponent) {
    // Non-sequential IDs with gaps.
    build_graph("follows", {{100, 50}, {50, 200}, {200, 100}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    for (int64_t node : {50, 100, 200}) {
        EXPECT_EQ(m[node].component_id, 50) << "node " << node;
    }
    verify_component_id_is_min(m);
}

TEST_F(GDB524_SCCQA, AC3_ComponentIdStableWithLargeGaps) {
    // Large ID gaps: 1, 1000, 999999.
    build_graph("follows", {{1, 1000}, {1000, 999999}, {999999, 1}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    for (int64_t node : {1, 1000, 999999}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
    }
}

// ============================================================================
// AC4: Handles DAGs (each node is its own SCC)
// ============================================================================

TEST_F(GDB524_SCCQA, AC4_LinearDAG) {
    build_graph("follows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 5u);
    for (int64_t node : {1, 2, 3, 4, 5}) {
        EXPECT_EQ(m[node].component_id, node) << "node " << node;
        EXPECT_EQ(m[node].component_size, 1) << "node " << node;
    }
    EXPECT_EQ(count_components(m), 5);
}

TEST_F(GDB524_SCCQA, AC4_DiamondDAGNoBackEdge) {
    // Diamond: 1->2, 1->3, 2->4, 3->4. No back edges.
    build_graph("follows", {{1, 2}, {1, 3}, {2, 4}, {3, 4}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 4u);
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_EQ(m[node].component_id, node) << "node " << node;
        EXPECT_EQ(m[node].component_size, 1) << "node " << node;
    }
    EXPECT_EQ(count_components(m), 4);
}

TEST_F(GDB524_SCCQA, AC4_TreeDAG) {
    // Binary tree: 1->{2,3}, 2->{4,5}, 3->{6,7}
    build_graph("follows", {{1, 2}, {1, 3}, {2, 4}, {2, 5}, {3, 6}, {3, 7}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 7u);
    for (int64_t node : {1, 2, 3, 4, 5, 6, 7}) {
        EXPECT_EQ(m[node].component_id, node) << "node " << node;
        EXPECT_EQ(m[node].component_size, 1) << "node " << node;
    }
    EXPECT_EQ(count_components(m), 7);
}

TEST_F(GDB524_SCCQA, AC4_WideDAG) {
    // Node 1 points to 10 sinks.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 2; i <= 11; ++i) {
        edges.push_back({1, i});
    }
    build_graph("follows", edges);

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 11u);
    for (int64_t node : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}) {
        EXPECT_EQ(m[node].component_size, 1) << "node " << node;
    }
}

// ============================================================================
// AC5: Handles fully connected graphs (single SCC)
// ============================================================================

TEST_F(GDB524_SCCQA, AC5_FullyConnected4) {
    build_graph("follows",
                {{1, 2},
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
                 {4, 3}});

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

TEST_F(GDB524_SCCQA, AC5_FullyConnectedK6) {
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 6; ++i) {
        for (int64_t j = 1; j <= 6; ++j) {
            if (i != j)
                edges.push_back({i, j});
        }
    }
    build_graph("follows", edges);

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 6u);
    for (int64_t node : {1, 2, 3, 4, 5, 6}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 6) << "node " << node;
    }
}

TEST_F(GDB524_SCCQA, AC5_HamiltonianCycleSingleSCC) {
    // Single Hamiltonian cycle: 1->2->3->4->5->6->1
    // All nodes mutually reachable => single SCC.
    build_graph("follows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 6}, {6, 1}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    for (int64_t node : {1, 2, 3, 4, 5, 6}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 6) << "node " << node;
    }
}

// ============================================================================
// Adversarial: Self-loops
// ============================================================================

TEST_F(GDB524_SCCQA, SelfLoop_SingleNodeOnly) {
    build_graph("follows", {{1, 1}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 1u);
    EXPECT_EQ(m[1].component_id, 1);
    EXPECT_EQ(m[1].component_size, 1);
}

TEST_F(GDB524_SCCQA, SelfLoop_DoesNotMergeWithNeighbor) {
    build_graph("follows", {{1, 1}, {1, 2}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 2u);
    EXPECT_EQ(m[1].component_id, 1);
    EXPECT_EQ(m[1].component_size, 1);
    EXPECT_EQ(m[2].component_id, 2);
    EXPECT_EQ(m[2].component_size, 1);
}

TEST_F(GDB524_SCCQA, SelfLoop_AllNodesSelfLoops) {
    build_graph("follows", {{1, 1}, {2, 2}, {3, 3}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 3u);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node].component_id, node) << "node " << node;
        EXPECT_EQ(m[node].component_size, 1) << "node " << node;
    }
}

TEST_F(GDB524_SCCQA, SelfLoop_WithinSCC) {
    // Self-loop on node in a cycle. SCC should still be detected.
    build_graph("follows", {{1, 1}, {1, 2}, {2, 3}, {3, 1}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 3) << "node " << node;
    }
}

// ============================================================================
// Adversarial: Empty and boundary graphs
// ============================================================================

TEST_F(GDB524_SCCQA, Boundary_EmptyGraph) {
    build_graph("follows", {});
    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(GDB524_SCCQA, Boundary_SingleEdgeNoBackEdge) {
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

TEST_F(GDB524_SCCQA, Boundary_TwoNodesCycle) {
    build_graph("follows", {{1, 2}, {2, 1}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 2u);
    EXPECT_EQ(m[1].component_id, 1);
    EXPECT_EQ(m[1].component_size, 2);
    EXPECT_EQ(m[2].component_id, 1);
    EXPECT_EQ(m[2].component_size, 2);
}

// ============================================================================
// Adversarial: Undirected graphs (bidirectional edges)
// ============================================================================

TEST_F(GDB524_SCCQA, Undirected_ConnectedChain) {
    // Bidirectional chain: 1<->2<->3<->4
    // All nodes mutually reachable => single SCC.
    build_graph("follows", {{1, 2}, {2, 1}, {2, 3}, {3, 2}, {3, 4}, {4, 3}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 4u);
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 4) << "node " << node;
    }
}

TEST_F(GDB524_SCCQA, Undirected_TwoDisconnectedComponents) {
    // {1,2,3} bidirectional and {4,5} bidirectional, no connection.
    build_graph("follows", {{1, 2}, {2, 1}, {2, 3}, {3, 2}, {3, 1}, {1, 3}, {4, 5}, {5, 4}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 5u);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
    }
    for (int64_t node : {4, 5}) {
        EXPECT_EQ(m[node].component_id, 4) << "node " << node;
    }
}

// ============================================================================
// Adversarial: Large and negative node IDs
// ============================================================================

TEST_F(GDB524_SCCQA, LargeNodeIds) {
    build_graph("follows", {{1000000, 2000000}, {2000000, 3000000}, {3000000, 1000000}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    for (int64_t node : {1000000, 2000000, 3000000}) {
        EXPECT_EQ(m[node].component_id, 1000000) << "node " << node;
        EXPECT_EQ(m[node].component_size, 3) << "node " << node;
    }
}

TEST_F(GDB524_SCCQA, NegativeNodeIds) {
    build_graph("follows", {{-3, -2}, {-2, -1}, {-1, -3}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 3u);
    for (int64_t node : {-3, -2, -1}) {
        EXPECT_EQ(m[node].component_id, -3) << "node " << node;
        EXPECT_EQ(m[node].component_size, 3) << "node " << node;
    }
    verify_component_id_is_min(m);
}

TEST_F(GDB524_SCCQA, MixedPositiveNegativeAndZeroNodeIds) {
    build_graph("follows", {{-1, 0}, {0, 1}, {1, -1}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 3u);
    for (int64_t node : {-1, 0, 1}) {
        EXPECT_EQ(m[node].component_id, -1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 3) << "node " << node;
    }
}

TEST_F(GDB524_SCCQA, NegativeNodeIdsDAG) {
    // DAG with negative IDs: -10 -> -5 -> -1
    build_graph("follows", {{-10, -5}, {-5, -1}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 3u);
    for (int64_t node : {-10, -5, -1}) {
        EXPECT_EQ(m[node].component_id, node) << "node " << node;
        EXPECT_EQ(m[node].component_size, 1) << "node " << node;
    }
}

// ============================================================================
// Adversarial: Error paths
// ============================================================================

TEST_F(GDB524_SCCQA, Error_NonexistentEdgeType) {
    build_graph("follows", {{1, 2}});

    auto result = run("no_such_edge_type");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// ============================================================================
// Adversarial: Complex topology edge cases
// ============================================================================

TEST_F(GDB524_SCCQA, Topology_ManySmallSCCsConnectedByBridges) {
    // Chain of 2-node SCCs connected by one-way bridges:
    // {1,2} -> {3,4} -> {5,6}
    build_graph("follows", {{1, 2}, {2, 1}, {2, 3}, {3, 4}, {4, 3}, {4, 5}, {5, 6}, {6, 5}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 6u);

    for (int64_t node : {1, 2}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 2) << "node " << node;
    }
    for (int64_t node : {3, 4}) {
        EXPECT_EQ(m[node].component_id, 3) << "node " << node;
        EXPECT_EQ(m[node].component_size, 2) << "node " << node;
    }
    for (int64_t node : {5, 6}) {
        EXPECT_EQ(m[node].component_id, 5) << "node " << node;
        EXPECT_EQ(m[node].component_size, 2) << "node " << node;
    }
    EXPECT_EQ(count_components(m), 3);
}

TEST_F(GDB524_SCCQA, Topology_CrossEdgeBetweenSCCsDoesNotMerge) {
    // Two SCCs {1,2,3} and {4,5,6} with cross edges in both directions
    // between different pairs: 3->4 and 6->1. This creates a cycle
    // between the two SCCs, merging them into one!
    build_graph("follows",
                {{1, 2},
                 {2, 3},
                 {3, 1}, // SCC candidate 1
                 {4, 5},
                 {5, 6},
                 {6, 4}, // SCC candidate 2
                 {3, 4},
                 {6, 1}}); // Cross edges forming super-cycle

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 6u);

    // All nodes should be in one SCC because 1->2->3->4->5->6->1.
    for (int64_t node : {1, 2, 3, 4, 5, 6}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 6) << "node " << node;
    }
}

TEST_F(GDB524_SCCQA, Topology_SourceAndSinkNodes) {
    // Source node 1 (only outgoing), sink node 5 (only incoming),
    // cycle in the middle {2,3,4}.
    build_graph("follows", {{1, 2}, {2, 3}, {3, 4}, {4, 2}, {4, 5}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 5u);

    EXPECT_EQ(m[1].component_id, 1);
    EXPECT_EQ(m[1].component_size, 1);
    for (int64_t node : {2, 3, 4}) {
        EXPECT_EQ(m[node].component_id, 2) << "node " << node;
        EXPECT_EQ(m[node].component_size, 3) << "node " << node;
    }
    EXPECT_EQ(m[5].component_id, 5);
    EXPECT_EQ(m[5].component_size, 1);
}

TEST_F(GDB524_SCCQA, Topology_DisconnectedGraphMultipleSCCs) {
    // Two completely separate cycles with no edges between them.
    build_graph("follows",
                {{1, 2},
                 {2, 3},
                 {3, 1}, // SCC: {1,2,3}
                 {10, 11},
                 {11, 10}}); // SCC: {10,11}

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 5u);

    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
    }
    for (int64_t node : {10, 11}) {
        EXPECT_EQ(m[node].component_id, 10) << "node " << node;
    }
    EXPECT_EQ(count_components(m), 2);
}

TEST_F(GDB524_SCCQA, Topology_AllSingletonsNoEdgesBetween) {
    // All self-loops, no inter-node edges.
    build_graph("follows", {{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 5u);
    EXPECT_EQ(count_components(m), 5);
    for (const auto& [node, r] : m) {
        EXPECT_EQ(r.component_size, 1) << "node " << node;
    }
}

TEST_F(GDB524_SCCQA, Topology_SingletonWithIncomingAndOutgoing) {
    // Node 2 has incoming from 1 and outgoing to 3, but no cycle through it.
    // 1->2->3, no back edges.
    build_graph("follows", {{1, 2}, {2, 3}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 3u);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node].component_size, 1) << "node " << node;
    }
}

// ============================================================================
// Adversarial: Mathematical invariants
// ============================================================================

TEST_F(GDB524_SCCQA, Invariant_SumOfComponentSizesEqualsNodeCount) {
    build_graph("follows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}, {5, 4}, {6, 7}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);

    // Sum of component_size for each unique component should equal total nodes.
    std::unordered_map<int64_t, int64_t> comp_sizes;
    for (const auto& [_, r] : m) {
        comp_sizes[r.component_id] = r.component_size;
    }
    int64_t total = 0;
    for (const auto& [_, size] : comp_sizes) {
        total += size;
    }
    EXPECT_EQ(total, static_cast<int64_t>(m.size()));
}

TEST_F(GDB524_SCCQA, Invariant_ComponentSizeMatchesActualCount) {
    build_graph("follows", {{1, 2}, {2, 3}, {3, 1}, {3, 4}, {4, 5}, {5, 6}, {6, 4}, {6, 7}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    verify_size_matches_count(m);
    verify_consistent_sizes(m);
    verify_component_id_is_min(m);
}

TEST_F(GDB524_SCCQA, Invariant_EveryNodeAppears) {
    build_graph("follows", {{1, 2}, {2, 3}, {3, 1}, {4, 4}, {5, 6}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    // All nodes from edges should appear.
    for (int64_t node : {1, 2, 3, 4, 5, 6}) {
        EXPECT_TRUE(m.count(node) > 0) << "node " << node << " missing from output";
    }
}

// ============================================================================
// Output structure
// ============================================================================

TEST_F(GDB524_SCCQA, Output_Always3Columns) {
    build_graph("follows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (const auto& row : *result) {
        EXPECT_EQ(row.values.size(), 3u) << "each output row must have 3 values";
    }
}

TEST_F(GDB524_SCCQA, Output_SortedByNodeId) {
    build_graph("follows", {{50, 10}, {10, 30}, {30, 50}, {20, 40}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (size_t i = 1; i < result->size(); ++i) {
        auto prev = std::get<int64_t>((*result)[i - 1].values[0].data());
        auto curr = std::get<int64_t>((*result)[i].values[0].data());
        EXPECT_LT(prev, curr) << "results must be sorted by node_id";
    }
}

TEST_F(GDB524_SCCQA, Output_RowCountMatchesNodeCount) {
    build_graph("follows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}, {5, 4}});

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // 5 distinct nodes.
    EXPECT_EQ(result->size(), 5u);
}

// ============================================================================
// Stress tests
// ============================================================================

TEST_F(GDB524_SCCQA, Stress_LongChainDAG100) {
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i < 100; ++i) {
        edges.push_back({i, i + 1});
    }
    build_graph("follows", edges);

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 100u);
    for (const auto& [node, r] : m) {
        EXPECT_EQ(r.component_size, 1) << "node " << node << " in chain";
    }
    EXPECT_EQ(count_components(m), 100);
}

TEST_F(GDB524_SCCQA, Stress_LongCycle100) {
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i < 100; ++i) {
        edges.push_back({i, i + 1});
    }
    edges.push_back({100, 1}); // Close the cycle.
    build_graph("follows", edges);

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 100u);
    for (const auto& [node, r] : m) {
        EXPECT_EQ(r.component_id, 1) << "node " << node;
        EXPECT_EQ(r.component_size, 100) << "node " << node;
    }
    EXPECT_EQ(count_components(m), 1);
}

TEST_F(GDB524_SCCQA, Stress_ManyDisconnectedPairSCCs) {
    // 50 disconnected 2-node cycles: {1,2}, {3,4}, ..., {99,100}.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i < 100; i += 2) {
        edges.push_back({i, i + 1});
        edges.push_back({i + 1, i});
    }
    build_graph("follows", edges);

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 100u);
    EXPECT_EQ(count_components(m), 50);

    for (int64_t i = 1; i < 100; i += 2) {
        EXPECT_EQ(m[i].component_id, i) << "node " << i;
        EXPECT_EQ(m[i].component_size, 2) << "node " << i;
        EXPECT_EQ(m[i + 1].component_id, i) << "node " << (i + 1);
        EXPECT_EQ(m[i + 1].component_size, 2) << "node " << (i + 1);
    }
    verify_size_matches_count(m);
}

TEST_F(GDB524_SCCQA, Stress_CompleteK20SingleSCC) {
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 20; ++i) {
        for (int64_t j = 1; j <= 20; ++j) {
            if (i != j)
                edges.push_back({i, j});
        }
    }
    build_graph("follows", edges);

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 20u);
    for (const auto& [node, r] : m) {
        EXPECT_EQ(r.component_id, 1) << "node " << node;
        EXPECT_EQ(r.component_size, 20) << "node " << node;
    }
    EXPECT_EQ(count_components(m), 1);
}

TEST_F(GDB524_SCCQA, Stress_MixedSCCsAndSingletons) {
    // 3-cycle, 2-cycle, 5 singletons via DAG edges.
    std::vector<std::pair<int64_t, int64_t>> edges = {
        {1, 2},
        {2, 3},
        {3, 1}, // SCC: {1,2,3}
        {4, 5},
        {5, 4}, // SCC: {4,5}
        {3, 6},
        {6, 7},
        {7, 8}, // DAG singletons: 6, 7, 8
        {5, 9},
        {9, 10}, // DAG singletons: 9, 10
    };
    build_graph("follows", edges);

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 10u);

    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node].component_id, 1) << "node " << node;
        EXPECT_EQ(m[node].component_size, 3) << "node " << node;
    }
    for (int64_t node : {4, 5}) {
        EXPECT_EQ(m[node].component_id, 4) << "node " << node;
        EXPECT_EQ(m[node].component_size, 2) << "node " << node;
    }
    for (int64_t node : {6, 7, 8, 9, 10}) {
        EXPECT_EQ(m[node].component_size, 1) << "node " << node;
    }
    EXPECT_EQ(count_components(m), 7);
    verify_size_matches_count(m);
    verify_component_id_is_min(m);
}

TEST_F(GDB524_SCCQA, Stress_DeepNestedSCCChain) {
    // Chain of 10 SCCs each of size 3, connected by one-way bridges.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t g = 0; g < 10; ++g) {
        int64_t base = g * 3 + 1;
        edges.push_back({base, base + 1});
        edges.push_back({base + 1, base + 2});
        edges.push_back({base + 2, base});
        if (g > 0) {
            // One-way bridge from previous group to this one.
            edges.push_back({(g - 1) * 3 + 3, base});
        }
    }
    build_graph("follows", edges);

    auto result = run("follows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_scc_map(*result);
    EXPECT_EQ(m.size(), 30u);
    EXPECT_EQ(count_components(m), 10);

    for (int64_t g = 0; g < 10; ++g) {
        int64_t base = g * 3 + 1;
        int64_t expected_comp_id = base;
        for (int64_t offset = 0; offset < 3; ++offset) {
            EXPECT_EQ(m[base + offset].component_id, expected_comp_id)
                << "node " << (base + offset);
            EXPECT_EQ(m[base + offset].component_size, 3) << "node " << (base + offset);
        }
    }
    verify_size_matches_count(m);
}

} // namespace
} // namespace sixseven
