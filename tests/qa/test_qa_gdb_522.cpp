/// @file test_qa_gdb_522.cpp
/// QA adversarial tests for GDB-522: Clustering Coefficient algorithm.
///
/// Verifies:
///   AC1: clustering_coefficient() function registered and callable.
///   AC2: Returns correct local clustering coefficient per node.
///   AC3: Handles nodes with degree < 2 (coefficient = 0).
///   AC4: Triangle counting is correct.
///   AC5: Unit tests written and passing.
///   AC6: No regressions.
///
/// Adversarial categories: boundary values, error paths, graph topology
/// edge cases, numerical stability, stress tests, mathematical invariants,
/// duplicate edges, self-loops, directed vs undirected semantics.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/clustering_coefficient.h"
#include "sixseven/graph/graph_engine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>
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

/// Per-node clustering coefficient result.
struct ClusteringInfo {
    double local_coefficient;
    int64_t triangles;
    int64_t degree;
};

/// Extract per-node results from algorithm output rows.
std::unordered_map<int64_t, ClusteringInfo>
to_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, ClusteringInfo> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 4u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto local_coefficient = std::get<double>(row.values[1].data());
        auto triangles = std::get<int64_t>(row.values[2].data());
        auto degree = std::get<int64_t>(row.values[3].data());
        result[node_id] = {local_coefficient, triangles, degree};
    }
    return result;
}

// ============================================================================
// Fixture
// ============================================================================

class GDB522_ClusteringCoefficientQA : public ::testing::Test {
protected:
    void SetUp() override {
        auto t = catalog_.create_table(default_database_id, make_table_schema("nodes"));
        ASSERT_TRUE(t.has_value()) << t.error().message;
        table_id_ = *t;
    }

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

    Result<std::vector<AlgorithmRow>> run(const std::string& edge_type) {
        AlgorithmContext ctx{engine_, edge_type, {}};
        return clustering_coefficient_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ============================================================================
// AC1: Function registered and callable
// ============================================================================

TEST_F(GDB522_ClusteringCoefficientQA, AC1_RegistrationAndLookup) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_clustering_coefficient_def(),
                                              clustering_coefficient_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_TRUE(registry.has("clustering_coefficient"));
    auto* entry = registry.find("clustering_coefficient");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "clustering_coefficient");
}

TEST_F(GDB522_ClusteringCoefficientQA, AC1_CaseInsensitiveLookup) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_clustering_coefficient_def(),
                                              clustering_coefficient_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_TRUE(registry.has("CLUSTERING_COEFFICIENT"));
    EXPECT_TRUE(registry.has("Clustering_Coefficient"));
    EXPECT_NE(registry.find("CLUSTERING_COEFFICIENT"), nullptr);
}

TEST_F(GDB522_ClusteringCoefficientQA, AC1_DuplicateRegistrationFails) {
    AlgorithmRegistry registry;
    auto r1 = registry.register_algorithm(make_clustering_coefficient_def(),
                                          clustering_coefficient_execute);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    auto r2 = registry.register_algorithm(make_clustering_coefficient_def(),
                                          clustering_coefficient_execute);
    EXPECT_FALSE(r2.has_value());
}

TEST_F(GDB522_ClusteringCoefficientQA, AC1_OutputSchemaCorrect) {
    auto def = make_clustering_coefficient_def();
    ASSERT_EQ(def.output_columns.size(), 4u);
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[1].name, "local_coefficient");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::FLOAT64);
    EXPECT_EQ(def.output_columns[2].name, "triangles");
    EXPECT_EQ(def.output_columns[2].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[3].name, "degree");
    EXPECT_EQ(def.output_columns[3].type_id, TypeId::INT64);
}

TEST_F(GDB522_ClusteringCoefficientQA, AC1_NoParameters) {
    auto def = make_clustering_coefficient_def();
    EXPECT_TRUE(def.params.empty());
}

TEST_F(GDB522_ClusteringCoefficientQA, AC1_CallableOnSimpleGraph) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});
    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 3u);
}

// ============================================================================
// AC2: Returns correct local clustering coefficient per node
// ============================================================================

TEST_F(GDB522_ClusteringCoefficientQA, AC2_TriangleAllCoefficientsOne) {
    // Triangle: 1-2-3-1. Each node has degree 2, 1 triangle.
    // C = 2*1 / (2*1) = 1.0
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_DOUBLE_EQ(m[node].local_coefficient, 1.0) << "node " << node;
        EXPECT_EQ(m[node].triangles, 1) << "node " << node;
        EXPECT_EQ(m[node].degree, 2) << "node " << node;
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, AC2_CompleteK4AllCoefficientsOne) {
    // K4 bidirectional: all nodes fully connected, coefficient = 1.0.
    build_graph("knows", {
        {1, 2}, {1, 3}, {1, 4},
        {2, 1}, {2, 3}, {2, 4},
        {3, 1}, {3, 2}, {3, 4},
        {4, 1}, {4, 2}, {4, 3},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_DOUBLE_EQ(m[node].local_coefficient, 1.0) << "node " << node;
        EXPECT_EQ(m[node].triangles, 3) << "node " << node;
        EXPECT_EQ(m[node].degree, 3) << "node " << node;
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, AC2_StarGraphCentralNodeZero) {
    // Hub = 1, spokes = 2..5, bidirectional.
    // Hub has degree 4, but no neighbors are connected. C = 0.
    build_graph("knows", {
        {1, 2}, {1, 3}, {1, 4}, {1, 5},
        {2, 1}, {3, 1}, {4, 1}, {5, 1},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_DOUBLE_EQ(m[1].local_coefficient, 0.0);
    EXPECT_EQ(m[1].triangles, 0);
    EXPECT_EQ(m[1].degree, 4);

    for (int64_t node : {2, 3, 4, 5}) {
        EXPECT_DOUBLE_EQ(m[node].local_coefficient, 0.0) << "leaf " << node;
        EXPECT_EQ(m[node].degree, 1) << "leaf " << node;
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, AC2_BowtieGraphMixedCoefficients) {
    // Two triangles sharing node 3: {1,2,3} and {3,4,5}.
    build_graph("knows", {
        {1, 2}, {2, 1}, {2, 3}, {3, 2}, {3, 1}, {1, 3},
        {3, 4}, {4, 3}, {4, 5}, {5, 4}, {5, 3}, {3, 5},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 5u);

    // Nodes 1, 2: degree 2, 1 triangle. C = 2*1/(2*1) = 1.0
    EXPECT_DOUBLE_EQ(m[1].local_coefficient, 1.0);
    EXPECT_DOUBLE_EQ(m[2].local_coefficient, 1.0);

    // Node 3: degree 4, 2 triangles. C = 2*2/(4*3) = 4/12 = 1/3
    EXPECT_NEAR(m[3].local_coefficient, 1.0 / 3.0, 1e-10);
    EXPECT_EQ(m[3].triangles, 2);
    EXPECT_EQ(m[3].degree, 4);

    // Nodes 4, 5: degree 2, 1 triangle. C = 1.0
    EXPECT_DOUBLE_EQ(m[4].local_coefficient, 1.0);
    EXPECT_DOUBLE_EQ(m[5].local_coefficient, 1.0);
}

TEST_F(GDB522_ClusteringCoefficientQA, AC2_DiamondGraphKnownValues) {
    // Diamond: 1-2, 1-3, 2-3, 2-4, 3-4 (bidirectional).
    // Triangles: {1,2,3} and {2,3,4}.
    build_graph("knows", {
        {1, 2}, {2, 1}, {1, 3}, {3, 1}, {2, 3}, {3, 2},
        {2, 4}, {4, 2}, {3, 4}, {4, 3},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);

    // Node 1: degree 2, 1 triangle (2-3 connected). C = 2*1/(2*1) = 1.0
    EXPECT_DOUBLE_EQ(m[1].local_coefficient, 1.0);
    EXPECT_EQ(m[1].triangles, 1);

    // Node 2: degree 3, neighbors {1,3,4}. Pairs: 1-3(yes), 1-4(no), 3-4(yes). 2 triangles.
    // C = 2*2/(3*2) = 2/3
    EXPECT_NEAR(m[2].local_coefficient, 2.0 / 3.0, 1e-10);
    EXPECT_EQ(m[2].triangles, 2);

    // Node 3: same as node 2 by symmetry.
    EXPECT_NEAR(m[3].local_coefficient, 2.0 / 3.0, 1e-10);
    EXPECT_EQ(m[3].triangles, 2);

    // Node 4: degree 2, 1 triangle (2-3 connected). C = 1.0
    EXPECT_DOUBLE_EQ(m[4].local_coefficient, 1.0);
    EXPECT_EQ(m[4].triangles, 1);
}

TEST_F(GDB522_ClusteringCoefficientQA, AC2_PathGraphAllZero) {
    // 1<->2<->3<->4: no triangles exist. All coefficients = 0.
    build_graph("knows", {
        {1, 2}, {2, 1}, {2, 3}, {3, 2}, {3, 4}, {4, 3},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_DOUBLE_EQ(m[node].local_coefficient, 0.0) << "node " << node;
        EXPECT_EQ(m[node].triangles, 0) << "node " << node;
    }
}

// ============================================================================
// AC3: Handles nodes with degree < 2 (coefficient = 0)
// ============================================================================

TEST_F(GDB522_ClusteringCoefficientQA, AC3_SingleEdgeBothDegreeOne) {
    build_graph("knows", {{1, 2}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_DOUBLE_EQ(m[1].local_coefficient, 0.0);
    EXPECT_EQ(m[1].degree, 1);
    EXPECT_DOUBLE_EQ(m[2].local_coefficient, 0.0);
    EXPECT_EQ(m[2].degree, 1);
}

TEST_F(GDB522_ClusteringCoefficientQA, AC3_IsolatedNodeFromSelfLoop) {
    // Self-loop only: node exists but has degree 0 in undirected neighbor set.
    build_graph("knows", {{1, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 1u);
    EXPECT_DOUBLE_EQ(m[1].local_coefficient, 0.0);
    EXPECT_EQ(m[1].degree, 0);
    EXPECT_EQ(m[1].triangles, 0);
}

TEST_F(GDB522_ClusteringCoefficientQA, AC3_MixedDegreesLessThanTwo) {
    // Node 4 has degree 1 (only connected to 1). Nodes 1,2,3 form a triangle.
    build_graph("knows", {
        {1, 2}, {2, 1}, {2, 3}, {3, 2}, {3, 1}, {1, 3}, {1, 4}, {4, 1},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);

    // Node 4: degree 1, coefficient = 0.
    EXPECT_DOUBLE_EQ(m[4].local_coefficient, 0.0);
    EXPECT_EQ(m[4].degree, 1);
    EXPECT_EQ(m[4].triangles, 0);

    // Node 1: degree 3 (neighbors: 2,3,4). Pairs: 2-3(yes), 2-4(no), 3-4(no). 1 triangle.
    // C = 2*1/(3*2) = 1/3
    EXPECT_NEAR(m[1].local_coefficient, 1.0 / 3.0, 1e-10);
    EXPECT_EQ(m[1].triangles, 1);
}

TEST_F(GDB522_ClusteringCoefficientQA, AC3_DegreeZeroNodeAmidOthers) {
    // Node 5 only has a self-loop. Nodes 1-2-3 form a triangle.
    build_graph("knows", {
        {1, 2}, {2, 3}, {3, 1}, {5, 5},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_DOUBLE_EQ(m[5].local_coefficient, 0.0);
    EXPECT_EQ(m[5].degree, 0);
}

// ============================================================================
// AC4: Triangle counting is correct
// ============================================================================

TEST_F(GDB522_ClusteringCoefficientQA, AC4_K5ExactTriangleCounts) {
    // K5: each node has degree 4 and participates in C(4,2) = 6 triangles.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 5; ++i) {
        for (int64_t j = 1; j <= 5; ++j) {
            if (i != j) edges.push_back({i, j});
        }
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (int64_t node : {1, 2, 3, 4, 5}) {
        EXPECT_EQ(m[node].triangles, 6) << "node " << node << " in K5";
        EXPECT_EQ(m[node].degree, 4) << "node " << node;
        EXPECT_DOUBLE_EQ(m[node].local_coefficient, 1.0) << "node " << node;
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, AC4_TwoOverlappingTriangles) {
    // Two triangles sharing edge 2-3: {1,2,3} and {2,3,4}.
    build_graph("knows", {
        {1, 2}, {2, 1}, {2, 3}, {3, 2}, {3, 1}, {1, 3},
        {2, 4}, {4, 2}, {3, 4}, {4, 3},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);

    // Node 2: degree 3, neighbors {1,3,4}. Connected pairs: 1-3(yes), 3-4(yes), 1-4(no). 2 triangles.
    EXPECT_EQ(m[2].triangles, 2);
    // Node 3: same by symmetry.
    EXPECT_EQ(m[3].triangles, 2);
    // Nodes 1, 4: degree 2, 1 triangle each.
    EXPECT_EQ(m[1].triangles, 1);
    EXPECT_EQ(m[4].triangles, 1);
}

TEST_F(GDB522_ClusteringCoefficientQA, AC4_NoTrianglesInBipartiteGraph) {
    // Complete bipartite K3,3 has no triangles (no odd cycles).
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

    for (const auto& [node, info] : m) {
        EXPECT_EQ(info.triangles, 0)
            << "node " << node << " in bipartite graph should have 0 triangles";
        EXPECT_DOUBLE_EQ(info.local_coefficient, 0.0) << "node " << node;
        EXPECT_EQ(info.degree, 3) << "node " << node;
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, AC4_TriangleCountWithPendantNodes) {
    // Triangle {1,2,3} plus pendant node 4 off node 1.
    // Node 1 degree 3 (2,3,4). Only 2-3 connected = 1 triangle.
    build_graph("knows", {
        {1, 2}, {2, 1}, {2, 3}, {3, 2}, {3, 1}, {1, 3},
        {1, 4}, {4, 1},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m[1].triangles, 1);
    EXPECT_EQ(m[1].degree, 3);
    // C(1) = 2*1/(3*2) = 1/3
    EXPECT_NEAR(m[1].local_coefficient, 1.0 / 3.0, 1e-10);

    // Nodes 2, 3: degree 2, 1 triangle. C = 1.0
    EXPECT_EQ(m[2].triangles, 1);
    EXPECT_DOUBLE_EQ(m[2].local_coefficient, 1.0);
}

// ============================================================================
// Adversarial: Self-loops
// ============================================================================

TEST_F(GDB522_ClusteringCoefficientQA, SelfLoop_DoesNotInflateDegreeOrTriangles) {
    // Self-loop on node 1, plus triangle 1-2-3.
    build_graph("knows", {{1, 1}, {1, 2}, {2, 3}, {3, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_DOUBLE_EQ(m[node].local_coefficient, 1.0) << "node " << node;
        EXPECT_EQ(m[node].degree, 2) << "node " << node;
        EXPECT_EQ(m[node].triangles, 1) << "node " << node;
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, SelfLoop_MultipleSelfLoopsIgnored) {
    // Multiple self-loops on different nodes mixed with real edges.
    build_graph("knows", {
        {1, 1}, {2, 2}, {3, 3},
        {1, 2}, {2, 1}, {2, 3}, {3, 2}, {3, 1}, {1, 3},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    // Triangle should still be detected; self-loops ignored.
    for (int64_t node : {1, 2, 3}) {
        EXPECT_DOUBLE_EQ(m[node].local_coefficient, 1.0) << "node " << node;
        EXPECT_EQ(m[node].degree, 2) << "node " << node;
    }
}

// ============================================================================
// Adversarial: Duplicate/parallel edges
// ============================================================================

TEST_F(GDB522_ClusteringCoefficientQA, DuplicateEdges_NoInflation) {
    // Multiple edges between same pair should not inflate degree or triangles.
    build_graph("knows", {
        {1, 2}, {1, 2}, {1, 2},
        {2, 3}, {2, 3},
        {3, 1},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    // Undirected view: 1-2, 2-3, 3-1 forms a triangle.
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(m[node].degree, 2) << "node " << node;
        EXPECT_EQ(m[node].triangles, 1) << "node " << node;
        EXPECT_DOUBLE_EQ(m[node].local_coefficient, 1.0) << "node " << node;
    }
}

// ============================================================================
// Adversarial: Directed graph behavior (undirected symmetrization)
// ============================================================================

TEST_F(GDB522_ClusteringCoefficientQA, Directed_UnidirectionalTriangle) {
    // Directed triangle 1->2->3->1. Undirected view still forms triangle.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_DOUBLE_EQ(m[node].local_coefficient, 1.0) << "node " << node;
        EXPECT_EQ(m[node].degree, 2) << "node " << node;
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, Directed_NoTriangleIfMissingEdge) {
    // 1->2, 1->3 but NO edge between 2 and 3.
    // Undirected view: 1-2, 1-3. No triangle.
    build_graph("knows", {{1, 2}, {1, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_DOUBLE_EQ(m[1].local_coefficient, 0.0);
    EXPECT_EQ(m[1].degree, 2);
    EXPECT_EQ(m[1].triangles, 0);
}

// ============================================================================
// Adversarial: Empty and boundary graphs
// ============================================================================

TEST_F(GDB522_ClusteringCoefficientQA, Boundary_EmptyGraphReturnsEmpty) {
    build_graph("knows", {});
    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(GDB522_ClusteringCoefficientQA, Boundary_SingleNodeSelfLoop) {
    build_graph("knows", {{1, 1}});
    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 1u);
    EXPECT_DOUBLE_EQ(m[1].local_coefficient, 0.0);
    EXPECT_EQ(m[1].degree, 0);
}

TEST_F(GDB522_ClusteringCoefficientQA, Boundary_TwoNodesSingleEdge) {
    build_graph("knows", {{1, 2}});
    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 2u);
    EXPECT_DOUBLE_EQ(m[1].local_coefficient, 0.0);
    EXPECT_DOUBLE_EQ(m[2].local_coefficient, 0.0);
}

TEST_F(GDB522_ClusteringCoefficientQA, Boundary_AllSelfLoopsOnly) {
    build_graph("knows", {{1, 1}, {2, 2}, {3, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_DOUBLE_EQ(m[node].local_coefficient, 0.0) << "node " << node;
        EXPECT_EQ(m[node].degree, 0) << "node " << node;
    }
}

// ============================================================================
// Adversarial: Large and negative node IDs
// ============================================================================

TEST_F(GDB522_ClusteringCoefficientQA, LargeNodeIds) {
    build_graph("knows", {
        {1000000, 2000000}, {2000000, 3000000}, {3000000, 1000000},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);
    for (const auto& [node, info] : m) {
        EXPECT_DOUBLE_EQ(info.local_coefficient, 1.0) << "node " << node;
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, NegativeNodeIds) {
    build_graph("knows", {{-1, -2}, {-2, -3}, {-3, -1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);
    for (const auto& [node, info] : m) {
        EXPECT_DOUBLE_EQ(info.local_coefficient, 1.0) << "node " << node;
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, MixedPositiveNegativeNodeIds) {
    build_graph("knows", {{-1, 0}, {0, 1}, {1, -1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);
    for (const auto& [node, info] : m) {
        EXPECT_DOUBLE_EQ(info.local_coefficient, 1.0) << "node " << node;
    }
}

// ============================================================================
// Adversarial: Numerical stability
// ============================================================================

TEST_F(GDB522_ClusteringCoefficientQA, Numerical_NoNaNOrInf) {
    build_graph("knows", {
        {1, 2}, {2, 3}, {3, 1}, {1, 1}, {4, 5}, {6, 6},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (const auto& row : *result) {
        auto coeff = std::get<double>(row.values[1].data());
        EXPECT_FALSE(std::isnan(coeff)) << "coefficient must not be NaN";
        EXPECT_FALSE(std::isinf(coeff)) << "coefficient must not be Inf";
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, Numerical_CoefficientsInUnitRange) {
    // Test across several topologies.
    build_graph("knows", {
        {1, 2}, {2, 1}, {2, 3}, {3, 2}, {3, 1}, {1, 3},
        {1, 4}, {4, 5},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (const auto& [node, info] : m) {
        EXPECT_GE(info.local_coefficient, 0.0) << "node " << node << " coefficient >= 0";
        EXPECT_LE(info.local_coefficient, 1.0 + 1e-10) << "node " << node << " coefficient <= 1";
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, Numerical_Deterministic) {
    build_graph("knows", {
        {1, 2}, {2, 3}, {3, 4}, {4, 1}, {1, 3}, {2, 4},
    });

    auto r1 = run("knows");
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    auto r2 = run("knows");
    ASSERT_TRUE(r2.has_value()) << r2.error().message;

    ASSERT_EQ(r1->size(), r2->size());
    for (size_t i = 0; i < r1->size(); ++i) {
        auto c1 = std::get<double>((*r1)[i].values[1].data());
        auto c2 = std::get<double>((*r2)[i].values[1].data());
        EXPECT_DOUBLE_EQ(c1, c2) << "coefficients should be deterministic";

        auto t1 = std::get<int64_t>((*r1)[i].values[2].data());
        auto t2 = std::get<int64_t>((*r2)[i].values[2].data());
        EXPECT_EQ(t1, t2) << "triangle counts should be deterministic";
    }
}

// ============================================================================
// Adversarial: Mathematical invariant — coefficient formula
// ============================================================================

TEST_F(GDB522_ClusteringCoefficientQA, Invariant_FormulaVerification) {
    // For every node, verify C = 2T / (k*(k-1)) when degree >= 2.
    build_graph("knows", {
        {1, 2}, {2, 1}, {2, 3}, {3, 2}, {3, 1}, {1, 3},
        {3, 4}, {4, 3}, {4, 5}, {5, 4}, {5, 3}, {3, 5},
        {1, 6}, {6, 1},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (const auto& [node, info] : m) {
        if (info.degree >= 2) {
            double expected = (2.0 * static_cast<double>(info.triangles)) /
                              (static_cast<double>(info.degree) *
                               static_cast<double>(info.degree - 1));
            EXPECT_NEAR(info.local_coefficient, expected, 1e-10)
                << "node " << node << ": C should match 2T/(k*(k-1))";
        } else {
            EXPECT_DOUBLE_EQ(info.local_coefficient, 0.0)
                << "node " << node << " with degree < 2 should have C=0";
        }
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, Invariant_CompleteGraphAlwaysOne) {
    // In any complete graph, all coefficients must be 1.0.
    // K6 bidirectional.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 6; ++i) {
        for (int64_t j = 1; j <= 6; ++j) {
            if (i != j) edges.push_back({i, j});
        }
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (const auto& [node, info] : m) {
        EXPECT_DOUBLE_EQ(info.local_coefficient, 1.0) << "node " << node << " in K6";
        EXPECT_EQ(info.degree, 5) << "node " << node;
        // C(5,2) = 10 triangles per node in K6.
        EXPECT_EQ(info.triangles, 10) << "node " << node;
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, Invariant_TreeGraphAlwaysZero) {
    // Trees have no cycles, so no triangles. All coefficients = 0.
    // Binary tree: 1->{2,3}, 2->{4,5}, 3->{6,7} (bidirectional).
    build_graph("knows", {
        {1, 2}, {2, 1}, {1, 3}, {3, 1},
        {2, 4}, {4, 2}, {2, 5}, {5, 2},
        {3, 6}, {6, 3}, {3, 7}, {7, 3},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (const auto& [node, info] : m) {
        EXPECT_EQ(info.triangles, 0) << "node " << node << " in tree has no triangles";
        EXPECT_DOUBLE_EQ(info.local_coefficient, 0.0) << "node " << node;
    }
}

// ============================================================================
// Adversarial: Output structure
// ============================================================================

TEST_F(GDB522_ClusteringCoefficientQA, OutputAlways4Columns) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (const auto& row : *result) {
        EXPECT_EQ(row.values.size(), 4u) << "each output row must have exactly 4 values";
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, OutputSortedByNodeId) {
    build_graph("knows", {{50, 10}, {10, 30}, {30, 20}, {20, 50}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (size_t i = 1; i < result->size(); ++i) {
        auto prev = std::get<int64_t>((*result)[i - 1].values[0].data());
        auto curr = std::get<int64_t>((*result)[i].values[0].data());
        EXPECT_LT(prev, curr) << "results must be sorted by node_id";
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, OutputNodeCountMatchesGraphNodes) {
    // Ensure every node in the graph appears in output, including isolated ones.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {4, 4}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    // Nodes 1, 2, 3 (from triangle) + 4 (self-loop) = 4 nodes.
    EXPECT_EQ(m.size(), 4u);
    EXPECT_TRUE(m.count(4) > 0) << "self-loop-only node 4 should appear in output";
}

// ============================================================================
// Adversarial: Error paths
// ============================================================================

TEST_F(GDB522_ClusteringCoefficientQA, Error_NonexistentEdgeType) {
    build_graph("knows", {{1, 2}});

    auto result = run("no_such_edge_type");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// ============================================================================
// Adversarial: Graph topology edge cases
// ============================================================================

TEST_F(GDB522_ClusteringCoefficientQA, Topology_WheelGraph) {
    // Wheel W5: hub=1, rim 2-3-4-5-6-2, all bidirectional + hub to all.
    build_graph("knows", {
        {1, 2}, {2, 1}, {1, 3}, {3, 1}, {1, 4}, {4, 1}, {1, 5}, {5, 1}, {1, 6}, {6, 1},
        {2, 3}, {3, 2}, {3, 4}, {4, 3}, {4, 5}, {5, 4}, {5, 6}, {6, 5}, {6, 2}, {2, 6},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 6u);

    // Hub (1): degree 5, neighbors {2,3,4,5,6}.
    // Connected pairs among neighbors: 2-3, 3-4, 4-5, 5-6, 6-2 = 5 triangles.
    // C(1) = 2*5/(5*4) = 10/20 = 0.5
    EXPECT_EQ(m[1].degree, 5);
    EXPECT_EQ(m[1].triangles, 5);
    EXPECT_DOUBLE_EQ(m[1].local_coefficient, 0.5);

    // Rim nodes (e.g., 2): degree 3, neighbors {1,3,6}.
    // Connected pairs: 1-3(yes), 1-6(yes), 3-6(no) = 2 triangles.
    // C(2) = 2*2/(3*2) = 2/3
    for (int64_t node : {2, 3, 4, 5, 6}) {
        EXPECT_EQ(m[node].degree, 3) << "rim node " << node;
        EXPECT_EQ(m[node].triangles, 2) << "rim node " << node;
        EXPECT_NEAR(m[node].local_coefficient, 2.0 / 3.0, 1e-10)
            << "rim node " << node;
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, Topology_CycleGraphNoTriangles) {
    // Simple cycle: 1-2-3-4-5-1. No triangles (cycle length 5).
    build_graph("knows", {
        {1, 2}, {2, 1}, {2, 3}, {3, 2}, {3, 4}, {4, 3},
        {4, 5}, {5, 4}, {5, 1}, {1, 5},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (int64_t node : {1, 2, 3, 4, 5}) {
        EXPECT_EQ(m[node].degree, 2) << "node " << node;
        EXPECT_EQ(m[node].triangles, 0) << "node " << node;
        EXPECT_DOUBLE_EQ(m[node].local_coefficient, 0.0) << "node " << node;
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, Topology_PetersenGraph) {
    // Petersen graph: 10 nodes, 3-regular, no triangles.
    // Outer cycle: 1-2-3-4-5-1, inner pentagram: 6-8-10-7-9-6.
    // Spokes: 1-6, 2-7, 3-8, 4-9, 5-10.
    build_graph("knows", {
        // Outer cycle
        {1, 2}, {2, 1}, {2, 3}, {3, 2}, {3, 4}, {4, 3}, {4, 5}, {5, 4}, {5, 1}, {1, 5},
        // Spokes
        {1, 6}, {6, 1}, {2, 7}, {7, 2}, {3, 8}, {8, 3}, {4, 9}, {9, 4}, {5, 10}, {10, 5},
        // Inner pentagram
        {6, 8}, {8, 6}, {8, 10}, {10, 8}, {10, 7}, {7, 10}, {7, 9}, {9, 7}, {9, 6}, {6, 9},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 10u);

    // Petersen graph is triangle-free and 3-regular.
    for (const auto& [node, info] : m) {
        EXPECT_EQ(info.degree, 3) << "node " << node << " should be 3-regular";
        EXPECT_EQ(info.triangles, 0)
            << "node " << node << " in Petersen graph should have 0 triangles";
        EXPECT_DOUBLE_EQ(info.local_coefficient, 0.0) << "node " << node;
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, Topology_DisconnectedComponents) {
    // Two separate triangles: {1,2,3} and {4,5,6}. All coefficients = 1.0.
    build_graph("knows", {
        {1, 2}, {2, 1}, {2, 3}, {3, 2}, {3, 1}, {1, 3},
        {4, 5}, {5, 4}, {5, 6}, {6, 5}, {6, 4}, {4, 6},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 6u);
    for (const auto& [node, info] : m) {
        EXPECT_DOUBLE_EQ(info.local_coefficient, 1.0) << "node " << node;
    }
}

// ============================================================================
// Stress tests
// ============================================================================

TEST_F(GDB522_ClusteringCoefficientQA, Stress_ChainOf100Nodes) {
    // Bidirectional chain: no triangles.
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

    for (const auto& [node, info] : m) {
        EXPECT_EQ(info.triangles, 0) << "node " << node << " in chain has no triangles";
        EXPECT_DOUBLE_EQ(info.local_coefficient, 0.0) << "node " << node;
        EXPECT_GE(info.local_coefficient, 0.0);
        EXPECT_LE(info.local_coefficient, 1.0 + 1e-10);
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, Stress_CompleteK20) {
    // K20 bidirectional: all coefficients = 1.0.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 20; ++i) {
        for (int64_t j = 1; j <= 20; ++j) {
            if (i != j) edges.push_back({i, j});
        }
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 20u);

    for (const auto& [node, info] : m) {
        EXPECT_DOUBLE_EQ(info.local_coefficient, 1.0) << "node " << node << " in K20";
        EXPECT_EQ(info.degree, 19) << "node " << node;
        // C(19,2) = 171 triangles per node.
        EXPECT_EQ(info.triangles, 171) << "node " << node;
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, Stress_ManyDisconnectedTriangles) {
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

    for (const auto& [node, info] : m) {
        EXPECT_DOUBLE_EQ(info.local_coefficient, 1.0)
            << "node " << node << " in disconnected triangle should have C=1.0";
        EXPECT_EQ(info.degree, 2) << "node " << node;
        EXPECT_EQ(info.triangles, 1) << "node " << node;
    }
}

TEST_F(GDB522_ClusteringCoefficientQA, Stress_StarWith50Spokes) {
    // Star with 50 spokes: hub has degree 50, no triangles (C=0).
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
    EXPECT_EQ(m[1].degree, 50);
    EXPECT_EQ(m[1].triangles, 0);
    EXPECT_DOUBLE_EQ(m[1].local_coefficient, 0.0);

    for (int64_t i = 2; i <= 51; ++i) {
        EXPECT_EQ(m[i].degree, 1) << "spoke " << i;
        EXPECT_DOUBLE_EQ(m[i].local_coefficient, 0.0) << "spoke " << i;
    }
}

} // namespace
} // namespace sixseven
