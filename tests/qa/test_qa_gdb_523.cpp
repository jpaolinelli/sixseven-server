/// @file test_qa_gdb_523.cpp
/// QA adversarial tests for GDB-523: Triangle Count algorithm.
///
/// Verifies:
///   AC1: triangle_count() function registered and callable.
///   AC2: Returns correct triangle count per node.
///   AC3: Degree-ordering optimization implemented (correctness-verified).
///   AC4: Handles directed edges (count directed triangles).
///   AC5: Unit tests written and passing.
///   AC6: No regressions.
///
/// Adversarial categories: boundary values, error paths, graph topology
/// edge cases, self-loops, duplicate edges, directed vs undirected semantics,
/// mathematical invariants, stress tests, determinism, large/negative node IDs.

#include "sixseven/common/result.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/triangle_count.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <vector>

#include "graph_qa_fixture.h"

namespace sixseven {
namespace {

using graph_qa::GraphQaFixtureBase;

/// Extract per-node triangle counts from algorithm output rows.
std::unordered_map<int64_t, int64_t> to_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, int64_t> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 2u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto count = std::get<int64_t>(row.values[1].data());
        result[node_id] = count;
    }
    return result;
}

/// Compute total graph triangles from per-node counts.
int64_t total_triangles(const std::unordered_map<int64_t, int64_t>& counts) {
    int64_t sum = 0;
    for (const auto& [_, c] : counts) {
        sum += c;
    }
    return sum / 3;
}

// ============================================================================
// Fixture
// ============================================================================

class GDB523_TriangleCountQA : public GraphQaFixtureBase {
protected:
    Result<std::vector<AlgorithmRow>> run(const std::string& edge_type) {
        AlgorithmContext ctx{engine_, default_database_id, edge_type, {}};
        return triangle_count_execute(ctx);
    }
};

// ============================================================================
// AC1: Function registered and callable
// ============================================================================

TEST_F(GDB523_TriangleCountQA, AC1_RegistrationAndLookup) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_triangle_count_def(), triangle_count_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_TRUE(registry.has("triangle_count"));
    auto* entry = registry.find("triangle_count");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "triangle_count");
}

TEST_F(GDB523_TriangleCountQA, AC1_CaseInsensitiveLookup) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_triangle_count_def(), triangle_count_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_TRUE(registry.has("TRIANGLE_COUNT"));
    EXPECT_TRUE(registry.has("Triangle_Count"));
    EXPECT_NE(registry.find("TRIANGLE_COUNT"), nullptr);
}

TEST_F(GDB523_TriangleCountQA, AC1_DuplicateRegistrationFails) {
    AlgorithmRegistry registry;
    auto r1 = registry.register_algorithm(make_triangle_count_def(), triangle_count_execute);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    auto r2 = registry.register_algorithm(make_triangle_count_def(), triangle_count_execute);
    EXPECT_FALSE(r2.has_value());
}

TEST_F(GDB523_TriangleCountQA, AC1_OutputSchemaCorrect) {
    auto def = make_triangle_count_def();
    ASSERT_EQ(def.output_columns.size(), 2u);
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[1].name, "triangle_count");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::INT64);
}

TEST_F(GDB523_TriangleCountQA, AC1_NoParameters) {
    auto def = make_triangle_count_def();
    EXPECT_TRUE(def.params.empty());
}

TEST_F(GDB523_TriangleCountQA, AC1_CallableOnSimpleGraph) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});
    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 3u);
}

// ============================================================================
// AC2: Returns correct triangle count per node
// ============================================================================

TEST_F(GDB523_TriangleCountQA, AC2_SingleTriangleAllNodesCountOne) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node], 1) << "node " << node << " should participate in 1 triangle";
    }
    EXPECT_EQ(total_triangles(m), 1);
}

TEST_F(GDB523_TriangleCountQA, AC2_CompleteK4) {
    build_graph("knows",
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

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_EQ(m[node], 3) << "node " << node << " in K4 should have 3 triangles";
    }
    EXPECT_EQ(total_triangles(m), 4);
}

TEST_F(GDB523_TriangleCountQA, AC2_CompleteK5) {
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 5; ++i) {
        for (int64_t j = 1; j <= 5; ++j) {
            if (i != j)
                edges.push_back({i, j});
        }
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (int64_t node : {1, 2, 3, 4, 5}) {
        EXPECT_EQ(m[node], 6) << "node " << node << " in K5";
    }
    EXPECT_EQ(total_triangles(m), 10);
}

TEST_F(GDB523_TriangleCountQA, AC2_StarGraphZeroTriangles) {
    build_graph("knows",
                {
                    {1, 2},
                    {1, 3},
                    {1, 4},
                    {1, 5},
                    {2, 1},
                    {3, 1},
                    {4, 1},
                    {5, 1},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (int64_t node : {1, 2, 3, 4, 5}) {
        EXPECT_EQ(m[node], 0) << "star graph node " << node;
    }
    EXPECT_EQ(total_triangles(m), 0);
}

TEST_F(GDB523_TriangleCountQA, AC2_PathGraphZeroTriangles) {
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {3, 4},
                    {4, 3},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_EQ(m[node], 0) << "path graph node " << node;
    }
}

TEST_F(GDB523_TriangleCountQA, AC2_TwoTrianglesSharingEdge) {
    // Triangles: {1,2,3} and {2,3,4}. Shared edge: 2-3.
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {3, 1},
                    {1, 3},
                    {2, 4},
                    {4, 2},
                    {3, 4},
                    {4, 3},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m[1], 1);
    EXPECT_EQ(m[2], 2);
    EXPECT_EQ(m[3], 2);
    EXPECT_EQ(m[4], 1);
    EXPECT_EQ(total_triangles(m), 2);
}

TEST_F(GDB523_TriangleCountQA, AC2_BowtieGraphSharedNode) {
    // Two triangles sharing node 3: {1,2,3} and {3,4,5}.
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {3, 1},
                    {1, 3},
                    {3, 4},
                    {4, 3},
                    {4, 5},
                    {5, 4},
                    {5, 3},
                    {3, 5},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m[1], 1);
    EXPECT_EQ(m[2], 1);
    EXPECT_EQ(m[3], 2);
    EXPECT_EQ(m[4], 1);
    EXPECT_EQ(m[5], 1);
    EXPECT_EQ(total_triangles(m), 2);
}

// ============================================================================
// AC3: Degree-ordering optimization correctness
// ============================================================================

TEST_F(GDB523_TriangleCountQA, AC3_DegreeOrderingSameResultsRegardlessOfEdgeOrder) {
    // Same graph as TwoTrianglesSharingEdge but reversed edge insertion order.
    build_graph("knows",
                {
                    {4, 3},
                    {3, 4},
                    {4, 2},
                    {2, 4},
                    {3, 2},
                    {2, 3},
                    {3, 1},
                    {1, 3},
                    {1, 2},
                    {2, 1},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m[1], 1);
    EXPECT_EQ(m[2], 2);
    EXPECT_EQ(m[3], 2);
    EXPECT_EQ(m[4], 1);
    EXPECT_EQ(total_triangles(m), 2);
}

TEST_F(GDB523_TriangleCountQA, AC3_DegreeOrderingWithVaryingDegrees) {
    // Node 1 has degree 4 (hub), nodes 2-5 have degree 2 or 1.
    // Triangles: {1,2,3}, {1,3,4}.
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {1, 3},
                    {3, 1},
                    {1, 4},
                    {4, 1},
                    {1, 5},
                    {5, 1},
                    {2, 3},
                    {3, 2},
                    {3, 4},
                    {4, 3},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m[1], 2); // Part of {1,2,3} and {1,3,4}
    EXPECT_EQ(m[2], 1); // Part of {1,2,3}
    EXPECT_EQ(m[3], 2); // Part of {1,2,3} and {1,3,4}
    EXPECT_EQ(m[4], 1); // Part of {1,3,4}
    EXPECT_EQ(m[5], 0); // Pendant, no triangles
    EXPECT_EQ(total_triangles(m), 2);
}

TEST_F(GDB523_TriangleCountQA, AC3_AllNodesSameDegree) {
    // Cycle of length 3 (triangle) — all nodes have degree 2.
    // Degree ordering falls through to node ID tiebreaker.
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {3, 1},
                    {1, 3},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node], 1) << "node " << node;
    }
    EXPECT_EQ(total_triangles(m), 1);
}

// ============================================================================
// AC4: Handles directed edges (undirected symmetrization)
// ============================================================================

TEST_F(GDB523_TriangleCountQA, AC4_DirectedCycleFormsTriangle) {
    // Directed cycle: 1->2->3->1 (no reverse edges).
    // Undirected view still forms a triangle.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node], 1) << "node " << node;
    }
    EXPECT_EQ(total_triangles(m), 1);
}

TEST_F(GDB523_TriangleCountQA, AC4_DirectedEdgesMissingThirdLeg) {
    // 1->2, 1->3 but NO edge between 2 and 3.
    // Undirected: 1-2, 1-3. No triangle.
    build_graph("knows", {{1, 2}, {1, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (const auto& [node, count] : m) {
        EXPECT_EQ(count, 0) << "node " << node << " with incomplete triangle";
    }
}

TEST_F(GDB523_TriangleCountQA, AC4_MixedDirectedAndBidirectional) {
    // 1<->2 (bidirectional), 2->3, 3->1 (directed).
    // Undirected view: 1-2, 2-3, 3-1 — triangle exists.
    build_graph("knows", {{1, 2}, {2, 1}, {2, 3}, {3, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node], 1) << "node " << node;
    }
}

TEST_F(GDB523_TriangleCountQA, AC4_DirectedK4OnlyOneDirection) {
    // All edges in one direction: 1->2, 1->3, 1->4, 2->3, 2->4, 3->4.
    // Undirected view: K4. 4 triangles total.
    build_graph("knows",
                {
                    {1, 2},
                    {1, 3},
                    {1, 4},
                    {2, 3},
                    {2, 4},
                    {3, 4},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_EQ(m[node], 3) << "node " << node << " in directed K4";
    }
    EXPECT_EQ(total_triangles(m), 4);
}

// ============================================================================
// Adversarial: Self-loops
// ============================================================================

TEST_F(GDB523_TriangleCountQA, SelfLoop_SingleNodeSelfLoopZeroTriangles) {
    build_graph("knows", {{1, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 1u);
    EXPECT_EQ(m[1], 0);
}

TEST_F(GDB523_TriangleCountQA, SelfLoop_DoesNotInflateCount) {
    // Triangle 1-2-3 plus self-loop on node 1.
    build_graph("knows", {{1, 1}, {1, 2}, {2, 3}, {3, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node], 1) << "node " << node;
    }
    EXPECT_EQ(total_triangles(m), 1);
}

TEST_F(GDB523_TriangleCountQA, SelfLoop_MultipleSelfLoopsIgnored) {
    build_graph("knows",
                {
                    {1, 1},
                    {2, 2},
                    {3, 3},
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {3, 1},
                    {1, 3},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node], 1) << "node " << node;
    }
}

TEST_F(GDB523_TriangleCountQA, SelfLoop_OnlyNodeWithSelfLoopAmidTriangle) {
    // Triangle {1,2,3}, node 4 only has a self-loop.
    build_graph("knows",
                {
                    {1, 2},
                    {2, 3},
                    {3, 1},
                    {4, 4},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 4u);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node], 1) << "node " << node;
    }
    EXPECT_EQ(m[4], 0);
}

// ============================================================================
// Adversarial: Duplicate/parallel edges
// ============================================================================

TEST_F(GDB523_TriangleCountQA, DuplicateEdges_NoInflation) {
    // Multiple edges between same pair should not inflate triangle counts.
    build_graph("knows",
                {
                    {1, 2},
                    {1, 2},
                    {1, 2},
                    {2, 3},
                    {2, 3},
                    {3, 1},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node], 1) << "node " << node;
    }
    EXPECT_EQ(total_triangles(m), 1);
}

// ============================================================================
// Adversarial: Empty and boundary graphs
// ============================================================================

TEST_F(GDB523_TriangleCountQA, Boundary_EmptyGraphReturnsEmpty) {
    build_graph("knows", {});
    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(GDB523_TriangleCountQA, Boundary_TwoNodesSingleEdge) {
    build_graph("knows", {{1, 2}});
    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 2u);
    EXPECT_EQ(m[1], 0);
    EXPECT_EQ(m[2], 0);
}

TEST_F(GDB523_TriangleCountQA, Boundary_AllSelfLoopsOnly) {
    build_graph("knows", {{1, 1}, {2, 2}, {3, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node], 0) << "node " << node;
    }
}

TEST_F(GDB523_TriangleCountQA, Boundary_ThreeNodesAlmostTriangleMissingOneEdge) {
    // 1-2, 2-3, but NO 1-3. Not a triangle.
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node], 0) << "node " << node;
    }
}

// ============================================================================
// Adversarial: Large and negative node IDs
// ============================================================================

TEST_F(GDB523_TriangleCountQA, LargeNodeIds) {
    build_graph("knows",
                {
                    {1000000, 2000000},
                    {2000000, 3000000},
                    {3000000, 1000000},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);
    for (const auto& [node, count] : m) {
        EXPECT_EQ(count, 1) << "node " << node;
    }
    EXPECT_EQ(total_triangles(m), 1);
}

TEST_F(GDB523_TriangleCountQA, NegativeNodeIds) {
    build_graph("knows", {{-1, -2}, {-2, -3}, {-3, -1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);
    for (const auto& [node, count] : m) {
        EXPECT_EQ(count, 1) << "node " << node;
    }
}

TEST_F(GDB523_TriangleCountQA, MixedPositiveNegativeAndZeroNodeIds) {
    build_graph("knows", {{-1, 0}, {0, 1}, {1, -1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);
    for (const auto& [node, count] : m) {
        EXPECT_EQ(count, 1) << "node " << node;
    }
}

// ============================================================================
// Adversarial: Error paths
// ============================================================================

TEST_F(GDB523_TriangleCountQA, Error_NonexistentEdgeType) {
    build_graph("knows", {{1, 2}});

    auto result = run("no_such_edge_type");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// ============================================================================
// Adversarial: Graph topology edge cases
// ============================================================================

TEST_F(GDB523_TriangleCountQA, Topology_BipartiteK33NoTriangles) {
    // Complete bipartite K3,3 has no triangles.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 3; ++i) {
        for (int64_t j = 4; j <= 6; ++j) {
            edges.push_back({i, j});
            edges.push_back({j, i});
        }
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 6u);
    for (const auto& [node, count] : m) {
        EXPECT_EQ(count, 0) << "node " << node << " in bipartite graph";
    }
    EXPECT_EQ(total_triangles(m), 0);
}

TEST_F(GDB523_TriangleCountQA, Topology_WheelGraph) {
    // Wheel W5: hub=1, rim 2-3-4-5-6-2, all bidirectional + hub to all.
    build_graph("knows",
                {
                    {1, 2}, {2, 1}, {1, 3}, {3, 1}, {1, 4}, {4, 1}, {1, 5}, {5, 1}, {1, 6}, {6, 1},
                    {2, 3}, {3, 2}, {3, 4}, {4, 3}, {4, 5}, {5, 4}, {5, 6}, {6, 5}, {6, 2}, {2, 6},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 6u);

    // Hub (1): degree 5, participates in 5 triangles (one with each rim pair).
    EXPECT_EQ(m[1], 5);

    // Rim nodes: degree 3, each participates in 2 triangles.
    for (int64_t node : {2, 3, 4, 5, 6}) {
        EXPECT_EQ(m[node], 2) << "rim node " << node;
    }
    // Total: (5 + 5*2) / 3 = 15/3 = 5
    EXPECT_EQ(total_triangles(m), 5);
}

TEST_F(GDB523_TriangleCountQA, Topology_CycleLength5NoTriangles) {
    // Simple cycle: 1-2-3-4-5-1. Odd cycle, no triangles.
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {3, 4},
                    {4, 3},
                    {4, 5},
                    {5, 4},
                    {5, 1},
                    {1, 5},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (int64_t node : {1, 2, 3, 4, 5}) {
        EXPECT_EQ(m[node], 0) << "node " << node << " in C5";
    }
}

TEST_F(GDB523_TriangleCountQA, Topology_PetersenGraphTriangleFree) {
    // Petersen graph: 10 nodes, 3-regular, no triangles.
    build_graph("knows",
                {
                    // Outer cycle
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {3, 4},
                    {4, 3},
                    {4, 5},
                    {5, 4},
                    {5, 1},
                    {1, 5},
                    // Spokes
                    {1, 6},
                    {6, 1},
                    {2, 7},
                    {7, 2},
                    {3, 8},
                    {8, 3},
                    {4, 9},
                    {9, 4},
                    {5, 10},
                    {10, 5},
                    // Inner pentagram
                    {6, 8},
                    {8, 6},
                    {8, 10},
                    {10, 8},
                    {10, 7},
                    {7, 10},
                    {7, 9},
                    {9, 7},
                    {9, 6},
                    {6, 9},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 10u);
    for (const auto& [node, count] : m) {
        EXPECT_EQ(count, 0) << "node " << node << " in Petersen graph";
    }
}

TEST_F(GDB523_TriangleCountQA, Topology_DisconnectedComponents) {
    // Two separate triangles: {1,2,3} and {4,5,6}.
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {3, 1},
                    {1, 3},
                    {4, 5},
                    {5, 4},
                    {5, 6},
                    {6, 5},
                    {6, 4},
                    {4, 6},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 6u);
    for (const auto& [node, count] : m) {
        EXPECT_EQ(count, 1) << "node " << node;
    }
    EXPECT_EQ(total_triangles(m), 2);
}

TEST_F(GDB523_TriangleCountQA, Topology_TreeGraphAlwaysZero) {
    // Binary tree: 1->{2,3}, 2->{4,5}, 3->{6,7} (bidirectional).
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {1, 3},
                    {3, 1},
                    {2, 4},
                    {4, 2},
                    {2, 5},
                    {5, 2},
                    {3, 6},
                    {6, 3},
                    {3, 7},
                    {7, 3},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (const auto& [node, count] : m) {
        EXPECT_EQ(count, 0) << "node " << node << " in tree";
    }
}

TEST_F(GDB523_TriangleCountQA, Topology_CompleteK3PlusPendantNode) {
    // Triangle {1,2,3} + pendant node 4 off node 1.
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {3, 1},
                    {1, 3},
                    {1, 4},
                    {4, 1},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m[1], 1);
    EXPECT_EQ(m[2], 1);
    EXPECT_EQ(m[3], 1);
    EXPECT_EQ(m[4], 0);
}

// ============================================================================
// Mathematical invariants
// ============================================================================

TEST_F(GDB523_TriangleCountQA, Invariant_SumDivisibleByThree) {
    // The sum of all per-node triangle counts must always be divisible by 3,
    // since each triangle is counted once per participating node.
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {3, 1},
                    {1, 3},
                    {3, 4},
                    {4, 3},
                    {4, 5},
                    {5, 4},
                    {5, 3},
                    {3, 5},
                    {1, 6},
                    {6, 1},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    int64_t sum = 0;
    for (const auto& [_, c] : m) {
        sum += c;
    }
    EXPECT_EQ(sum % 3, 0) << "sum of per-node counts must be divisible by 3";
}

TEST_F(GDB523_TriangleCountQA, Invariant_CompleteGraphFormula) {
    // In complete graph Kn: each node participates in C(n-1, 2) triangles.
    // Total triangles = C(n, 3).
    // Test with K6.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 6; ++i) {
        for (int64_t j = 1; j <= 6; ++j) {
            if (i != j)
                edges.push_back({i, j});
        }
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    // Each node in K6: C(5,2) = 10 triangles.
    for (const auto& [node, count] : m) {
        EXPECT_EQ(count, 10) << "node " << node << " in K6";
    }
    // Total: C(6,3) = 20
    EXPECT_EQ(total_triangles(m), 20);
}

TEST_F(GDB523_TriangleCountQA, Invariant_CountsNonNegative) {
    build_graph("knows",
                {
                    {1, 2},
                    {2, 3},
                    {3, 1},
                    {1, 1},
                    {4, 5},
                    {6, 6},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (const auto& [node, count] : m) {
        EXPECT_GE(count, 0) << "node " << node << " count must be non-negative";
    }
}

// ============================================================================
// Output structure
// ============================================================================

TEST_F(GDB523_TriangleCountQA, Output_Always2Columns) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (const auto& row : *result) {
        EXPECT_EQ(row.values.size(), 2u) << "each output row must have exactly 2 values";
    }
}

TEST_F(GDB523_TriangleCountQA, Output_SortedByNodeId) {
    build_graph("knows", {{50, 10}, {10, 30}, {30, 20}, {20, 50}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (size_t i = 1; i < result->size(); ++i) {
        auto prev = std::get<int64_t>((*result)[i - 1].values[0].data());
        auto curr = std::get<int64_t>((*result)[i].values[0].data());
        EXPECT_LT(prev, curr) << "results must be sorted by node_id";
    }
}

TEST_F(GDB523_TriangleCountQA, Output_AllNodesPresent) {
    // Every node in the graph appears in output, including isolated ones.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {4, 4}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 4u);
    EXPECT_TRUE(m.count(4) > 0) << "self-loop-only node 4 should appear in output";
}

// ============================================================================
// Determinism
// ============================================================================

TEST_F(GDB523_TriangleCountQA, Deterministic_SameResultsOnRepeatedRuns) {
    build_graph("knows",
                {
                    {1, 2},
                    {2, 3},
                    {3, 4},
                    {4, 1},
                    {1, 3},
                    {2, 4},
                });

    auto r1 = run("knows");
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    auto r2 = run("knows");
    ASSERT_TRUE(r2.has_value()) << r2.error().message;

    ASSERT_EQ(r1->size(), r2->size());
    for (size_t i = 0; i < r1->size(); ++i) {
        auto id1 = std::get<int64_t>((*r1)[i].values[0].data());
        auto id2 = std::get<int64_t>((*r2)[i].values[0].data());
        EXPECT_EQ(id1, id2);

        auto c1 = std::get<int64_t>((*r1)[i].values[1].data());
        auto c2 = std::get<int64_t>((*r2)[i].values[1].data());
        EXPECT_EQ(c1, c2) << "triangle counts should be deterministic";
    }
}

// ============================================================================
// Stress tests
// ============================================================================

TEST_F(GDB523_TriangleCountQA, Stress_ChainOf100Nodes) {
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i < 100; ++i) {
        edges.push_back({i, i + 1});
        edges.push_back({i + 1, i});
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 100u);
    for (const auto& [node, count] : m) {
        EXPECT_EQ(count, 0) << "node " << node << " in chain has no triangles";
    }
}

TEST_F(GDB523_TriangleCountQA, Stress_CompleteK20) {
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 20; ++i) {
        for (int64_t j = 1; j <= 20; ++j) {
            if (i != j)
                edges.push_back({i, j});
        }
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 20u);
    for (const auto& [node, count] : m) {
        // C(19,2) = 171
        EXPECT_EQ(count, 171) << "node " << node << " in K20";
    }
    // C(20,3) = 1140
    EXPECT_EQ(total_triangles(m), 1140);
}

TEST_F(GDB523_TriangleCountQA, Stress_ManyDisconnectedTriangles) {
    // 30 disconnected triangles: 90 nodes total.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t t = 0; t < 30; ++t) {
        int64_t base = t * 3 + 1;
        edges.push_back({base, base + 1});
        edges.push_back({base + 1, base});
        edges.push_back({base + 1, base + 2});
        edges.push_back({base + 2, base + 1});
        edges.push_back({base + 2, base});
        edges.push_back({base, base + 2});
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 90u);
    for (const auto& [node, count] : m) {
        EXPECT_EQ(count, 1) << "node " << node;
    }
    EXPECT_EQ(total_triangles(m), 30);
}

TEST_F(GDB523_TriangleCountQA, Stress_StarWith50Spokes) {
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 2; i <= 51; ++i) {
        edges.push_back({1, i});
        edges.push_back({i, 1});
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 51u);
    for (const auto& [node, count] : m) {
        EXPECT_EQ(count, 0) << "node " << node << " in star graph";
    }
}

} // namespace
} // namespace sixseven
