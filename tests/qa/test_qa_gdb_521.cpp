/// @file test_qa_gdb_521.cpp
/// QA adversarial tests for GDB-521: Harmonic Centrality algorithm.
///
/// Verifies:
///   AC1: harmonic_centrality() function registered and callable.
///   AC2: Returns correct harmonic centrality scores.
///   AC3: Handles disconnected graphs without special parameters.
///   AC4: Normalized values in [0, 1] range.
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
#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/harmonic_centrality.h"

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

/// Per-node harmonic centrality result.
struct HarmonicInfo {
    double harmonic;
    double normalized_harmonic;
};

/// Extract per-node results from algorithm output rows.
std::unordered_map<int64_t, HarmonicInfo>
to_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, HarmonicInfo> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 3u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto harmonic = std::get<double>(row.values[1].data());
        auto normalized = std::get<double>(row.values[2].data());
        result[node_id] = {harmonic, normalized};
    }
    return result;
}

// ============================================================================
// Fixture
// ============================================================================

class GDB521_HarmonicCentralityQA : public ::testing::Test {
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
        return harmonic_centrality_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ============================================================================
// AC1: Function registered and callable
// ============================================================================

TEST_F(GDB521_HarmonicCentralityQA, AC1_RegistrationAndLookup) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_harmonic_centrality_def(),
                                              harmonic_centrality_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_TRUE(registry.has("harmonic"));
    auto* entry = registry.find("harmonic");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "harmonic");
}

TEST_F(GDB521_HarmonicCentralityQA, AC1_CaseInsensitiveLookup) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_harmonic_centrality_def(),
                                              harmonic_centrality_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_TRUE(registry.has("HARMONIC"));
    EXPECT_TRUE(registry.has("Harmonic"));
    EXPECT_NE(registry.find("HARMONIC"), nullptr);
}

TEST_F(GDB521_HarmonicCentralityQA, AC1_DuplicateRegistrationFails) {
    AlgorithmRegistry registry;
    auto r1 = registry.register_algorithm(make_harmonic_centrality_def(),
                                          harmonic_centrality_execute);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    auto r2 = registry.register_algorithm(make_harmonic_centrality_def(),
                                          harmonic_centrality_execute);
    EXPECT_FALSE(r2.has_value());
}

TEST_F(GDB521_HarmonicCentralityQA, AC1_OutputSchemaCorrect) {
    auto def = make_harmonic_centrality_def();
    ASSERT_EQ(def.output_columns.size(), 3u);
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[1].name, "harmonic");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::FLOAT64);
    EXPECT_EQ(def.output_columns[2].name, "normalized_harmonic");
    EXPECT_EQ(def.output_columns[2].type_id, TypeId::FLOAT64);
}

TEST_F(GDB521_HarmonicCentralityQA, AC1_NoParameters) {
    auto def = make_harmonic_centrality_def();
    EXPECT_TRUE(def.params.empty())
        << "harmonic centrality should have no parameters (naturally handles disconnected graphs)";
}

TEST_F(GDB521_HarmonicCentralityQA, AC1_CallableOnSimpleGraph) {
    build_graph("knows", {{1, 2}, {2, 3}});
    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 3u);
}

// ============================================================================
// AC2: Returns correct harmonic centrality scores
// ============================================================================

TEST_F(GDB521_HarmonicCentralityQA, AC2_DirectedTriangleKnownValues) {
    // 1->2->3->1 (directed cycle)
    // Each node reaches one at d=1 and one at d=2.
    // H = 1/1 + 1/2 = 1.5
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);

    for (int64_t node : {1, 2, 3}) {
        EXPECT_DOUBLE_EQ(m[node].harmonic, 1.5)
            << "node " << node << " in directed triangle should have H=1.5";
        EXPECT_DOUBLE_EQ(m[node].normalized_harmonic, 0.75)
            << "node " << node << " normalized = 1.5/(3-1) = 0.75";
    }
}

TEST_F(GDB521_HarmonicCentralityQA, AC2_SingleEdgeKnownValues) {
    // 1->2: node 1 reaches 2 at d=1, node 2 reaches nobody.
    build_graph("knows", {{1, 2}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_DOUBLE_EQ(m[1].harmonic, 1.0);
    EXPECT_DOUBLE_EQ(m[1].normalized_harmonic, 1.0);
    EXPECT_DOUBLE_EQ(m[2].harmonic, 0.0);
    EXPECT_DOUBLE_EQ(m[2].normalized_harmonic, 0.0);
}

TEST_F(GDB521_HarmonicCentralityQA, AC2_DiamondGraphKnownValues) {
    // 1->2, 1->3, 2->4, 3->4
    build_graph("knows", {{1, 2}, {1, 3}, {2, 4}, {3, 4}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    // Node 1: reaches 2(d=1), 3(d=1), 4(d=2). H = 1 + 1 + 0.5 = 2.5
    EXPECT_DOUBLE_EQ(m[1].harmonic, 2.5);
    EXPECT_DOUBLE_EQ(m[1].normalized_harmonic, 2.5 / 3.0);

    // Node 2: reaches 4(d=1). H = 1.0
    EXPECT_DOUBLE_EQ(m[2].harmonic, 1.0);

    // Node 3: reaches 4(d=1). H = 1.0
    EXPECT_DOUBLE_EQ(m[3].harmonic, 1.0);

    // Node 2 and 3 are symmetric.
    EXPECT_DOUBLE_EQ(m[2].harmonic, m[3].harmonic);

    // Node 4: can't reach anyone. H = 0.0
    EXPECT_DOUBLE_EQ(m[4].harmonic, 0.0);
}

TEST_F(GDB521_HarmonicCentralityQA, AC2_BidirectionalPathKnownValues) {
    // Bidirectional path: 1<->2<->3<->4<->5
    build_graph("knows", {
        {1, 2}, {2, 1}, {2, 3}, {3, 2}, {3, 4}, {4, 3}, {4, 5}, {5, 4},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);

    // Node 1: d(2)=1, d(3)=2, d(4)=3, d(5)=4. H = 1 + 1/2 + 1/3 + 1/4 = 25/12
    EXPECT_NEAR(m[1].harmonic, 25.0 / 12.0, 1e-10);

    // Node 3 (center): d(2)=1, d(4)=1, d(1)=2, d(5)=2. H = 1+1+0.5+0.5 = 3.0
    EXPECT_DOUBLE_EQ(m[3].harmonic, 3.0);

    // Symmetry
    EXPECT_NEAR(m[1].harmonic, m[5].harmonic, 1e-10);
    EXPECT_NEAR(m[2].harmonic, m[4].harmonic, 1e-10);

    // Ordering: center > inner > endpoint
    EXPECT_GT(m[3].harmonic, m[2].harmonic);
    EXPECT_GT(m[2].harmonic, m[1].harmonic);
}

TEST_F(GDB521_HarmonicCentralityQA, AC2_CompleteGraphK4) {
    // K4 directed: all pairs connected.
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
        EXPECT_DOUBLE_EQ(m[node].harmonic, 3.0);
        EXPECT_DOUBLE_EQ(m[node].normalized_harmonic, 1.0);
    }
}

TEST_F(GDB521_HarmonicCentralityQA, AC2_StarGraphBidirectional) {
    // Hub = 1, spokes = 2..5, bidirectional.
    build_graph("knows", {
        {1, 2}, {1, 3}, {1, 4}, {1, 5},
        {2, 1}, {3, 1}, {4, 1}, {5, 1},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    // Hub reaches 4 spokes at d=1. H(hub) = 4.0
    EXPECT_DOUBLE_EQ(m[1].harmonic, 4.0);
    EXPECT_DOUBLE_EQ(m[1].normalized_harmonic, 1.0);

    // Spoke reaches hub at d=1, 3 other spokes at d=2. H = 1 + 3*(1/2) = 2.5
    for (int64_t node : {2, 3, 4, 5}) {
        EXPECT_DOUBLE_EQ(m[node].harmonic, 2.5);
        EXPECT_DOUBLE_EQ(m[node].normalized_harmonic, 2.5 / 4.0);
    }
}

// ============================================================================
// AC3: Handles disconnected graphs without special parameters
// ============================================================================

TEST_F(GDB521_HarmonicCentralityQA, AC3_TwoDisconnectedPairs) {
    // {1<->2}, {3<->4} — two isolated components.
    build_graph("knows", {{1, 2}, {2, 1}, {3, 4}, {4, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 4u);

    // Node 1: reaches 2 at d=1. H = 1.0. Normalized = 1.0/(4-1) = 1/3.
    EXPECT_DOUBLE_EQ(m[1].harmonic, 1.0);
    EXPECT_NEAR(m[1].normalized_harmonic, 1.0 / 3.0, 1e-10);

    // Symmetry within components.
    EXPECT_DOUBLE_EQ(m[1].harmonic, m[2].harmonic);
    EXPECT_DOUBLE_EQ(m[3].harmonic, m[4].harmonic);
    EXPECT_DOUBLE_EQ(m[1].harmonic, m[3].harmonic);
}

TEST_F(GDB521_HarmonicCentralityQA, AC3_ThreeDisconnectedComponents) {
    // {1<->2}, {3<->4<->5}, {6} (isolated via self-loop)
    build_graph("knows", {
        {1, 2}, {2, 1},
        {3, 4}, {4, 3}, {4, 5}, {5, 4},
        {6, 6},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 6u);

    // All scores should be non-negative — disconnected handled gracefully.
    for (const auto& [node, info] : m) {
        EXPECT_GE(info.harmonic, 0.0) << "node " << node;
        EXPECT_GE(info.normalized_harmonic, 0.0) << "node " << node;
    }

    // Node 4 (center of component 2) should have highest harmonic in that component.
    EXPECT_GT(m[4].harmonic, m[3].harmonic);
    EXPECT_GT(m[4].harmonic, m[5].harmonic);

    // Isolated node 6 should have H=0.
    EXPECT_DOUBLE_EQ(m[6].harmonic, 0.0);
}

TEST_F(GDB521_HarmonicCentralityQA, AC3_DisconnectedScoresPartialNotZero) {
    // Key difference from closeness: harmonic gives partial scores, not 0/undefined.
    build_graph("knows", {{1, 2}, {2, 1}, {3, 4}, {4, 3}, {4, 5}, {5, 4}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    // All nodes with at least one neighbor should have positive harmonic.
    for (int64_t node : {1, 2, 3, 4, 5}) {
        EXPECT_GT(m[node].harmonic, 0.0)
            << "node " << node << " should have positive harmonic even in disconnected graph";
    }
}

TEST_F(GDB521_HarmonicCentralityQA, AC3_NoSpecialParametersNeeded) {
    // Verify no parameters are needed — empty params map works.
    build_graph("knows", {{1, 2}, {3, 4}});

    AlgorithmContext ctx{engine_, "knows", {}};
    auto result = harmonic_centrality_execute(ctx);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 4u);
}

// ============================================================================
// AC4: Normalized values in [0, 1] range
// ============================================================================

TEST_F(GDB521_HarmonicCentralityQA, AC4_NormalizedInUnitRangeForVariousGraphs) {
    // Test across several topologies.
    build_graph("knows", {
        {1, 2}, {2, 3}, {3, 1}, {1, 4}, {4, 5}, {5, 6}, {6, 4},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (const auto& [node, info] : m) {
        EXPECT_GE(info.normalized_harmonic, 0.0)
            << "node " << node << " normalized should be >= 0";
        EXPECT_LE(info.normalized_harmonic, 1.0 + 1e-10)
            << "node " << node << " normalized should be <= 1";
    }
}

TEST_F(GDB521_HarmonicCentralityQA, AC4_NormalizedFormulaCorrect) {
    // Verify normalized = harmonic / (N-1) for all nodes.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 1}, {1, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    int64_t n = static_cast<int64_t>(m.size());

    for (const auto& [node, info] : m) {
        double expected_norm = info.harmonic / static_cast<double>(n - 1);
        EXPECT_NEAR(info.normalized_harmonic, expected_norm, 1e-10)
            << "node " << node << ": normalized should equal harmonic / (N-1)";
    }
}

TEST_F(GDB521_HarmonicCentralityQA, AC4_NormalizedMaxIsOneForCompleteGraph) {
    // Complete bidirectional K5: max normalized should be exactly 1.0.
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
    for (const auto& [node, info] : m) {
        EXPECT_DOUBLE_EQ(info.normalized_harmonic, 1.0)
            << "node " << node << " in K5 should have normalized = 1.0";
    }
}

TEST_F(GDB521_HarmonicCentralityQA, AC4_NormalizedForSingleNodeIsZero) {
    // Single node (via self-loop): N=1, normalized = 0.0.
    build_graph("knows", {{1, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 1u);
    EXPECT_DOUBLE_EQ(m[1].normalized_harmonic, 0.0);
}

// ============================================================================
// Adversarial: Empty and boundary graphs
// ============================================================================

TEST_F(GDB521_HarmonicCentralityQA, Boundary_EmptyGraphReturnsEmpty) {
    build_graph("knows", {});
    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(GDB521_HarmonicCentralityQA, Boundary_TwoNodesBidirectional) {
    build_graph("knows", {{1, 2}, {2, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 2u);

    // Each reaches the other at d=1. H = 1.0. Normalized = 1.0/(2-1) = 1.0.
    EXPECT_DOUBLE_EQ(m[1].harmonic, 1.0);
    EXPECT_DOUBLE_EQ(m[1].normalized_harmonic, 1.0);
    EXPECT_DOUBLE_EQ(m[2].harmonic, 1.0);
    EXPECT_DOUBLE_EQ(m[2].normalized_harmonic, 1.0);
}

TEST_F(GDB521_HarmonicCentralityQA, Boundary_SelfLoopOnly) {
    build_graph("knows", {{1, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 1u);
    EXPECT_DOUBLE_EQ(m[1].harmonic, 0.0);
    EXPECT_DOUBLE_EQ(m[1].normalized_harmonic, 0.0);
}

TEST_F(GDB521_HarmonicCentralityQA, Boundary_MultipleSelfLoopsOnly) {
    build_graph("knows", {{1, 1}, {2, 2}, {3, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);

    // Each node is isolated — no outgoing edges to distinct nodes.
    // BFS from each starts at source, self-loop is skipped (already visited).
    for (int64_t node : {1, 2, 3}) {
        EXPECT_DOUBLE_EQ(m[node].harmonic, 0.0)
            << "node " << node << " with only self-loop should have H=0";
    }
}

// ============================================================================
// Adversarial: Directed semantics
// ============================================================================

TEST_F(GDB521_HarmonicCentralityQA, Directed_OneWayChain) {
    // 1->2->3->4->5. Only the source can reach all, sink can reach none.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);

    // Node 1: reaches 2(1), 3(2), 4(3), 5(4). H = 1 + 1/2 + 1/3 + 1/4 = 25/12
    EXPECT_NEAR(m[1].harmonic, 25.0 / 12.0, 1e-10);

    // Node 5: reaches nobody. H = 0.
    EXPECT_DOUBLE_EQ(m[5].harmonic, 0.0);

    // Monotonic decrease: H(1) > H(2) > H(3) > H(4) > H(5)
    EXPECT_GT(m[1].harmonic, m[2].harmonic);
    EXPECT_GT(m[2].harmonic, m[3].harmonic);
    EXPECT_GT(m[3].harmonic, m[4].harmonic);
    EXPECT_GT(m[4].harmonic, m[5].harmonic);
}

TEST_F(GDB521_HarmonicCentralityQA, Directed_AsymmetricScores) {
    // 1->2, 2->3. Node 1 reaches 2,3 but node 3 reaches nobody.
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_GT(m[1].harmonic, m[2].harmonic);
    EXPECT_GT(m[2].harmonic, m[3].harmonic);
    EXPECT_DOUBLE_EQ(m[3].harmonic, 0.0);
}

// ============================================================================
// Adversarial: Duplicate edges
// ============================================================================

TEST_F(GDB521_HarmonicCentralityQA, DuplicateEdges_SameResult) {
    // Multiple edges between same nodes should not inflate scores.
    // BFS visits each node once regardless of duplicate adjacency entries.
    build_graph("knows", {{1, 2}, {1, 2}, {1, 2}, {2, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);

    // Node 1: reaches 2(d=1), 3(d=2). H = 1 + 0.5 = 1.5
    EXPECT_NEAR(m[1].harmonic, 1.5, 1e-10);
}

// ============================================================================
// Adversarial: Large node IDs
// ============================================================================

TEST_F(GDB521_HarmonicCentralityQA, LargeNodeIds) {
    build_graph("knows", {
        {1000000, 2000000},
        {2000000, 3000000},
        {3000000, 1000000},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);

    // Directed cycle: all equal H = 1.5
    for (const auto& [node, info] : m) {
        EXPECT_DOUBLE_EQ(info.harmonic, 1.5) << "node " << node;
    }
}

TEST_F(GDB521_HarmonicCentralityQA, NegativeNodeIds) {
    build_graph("knows", {{-1, -2}, {-2, -3}, {-3, -1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);

    for (const auto& [node, info] : m) {
        EXPECT_DOUBLE_EQ(info.harmonic, 1.5) << "node " << node;
    }
}

// ============================================================================
// Adversarial: Numerical stability
// ============================================================================

TEST_F(GDB521_HarmonicCentralityQA, Numerical_NoNaNOrInf) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}, {6, 6}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (const auto& row : *result) {
        auto h = std::get<double>(row.values[1].data());
        auto nh = std::get<double>(row.values[2].data());
        EXPECT_FALSE(std::isnan(h)) << "harmonic must not be NaN";
        EXPECT_FALSE(std::isinf(h)) << "harmonic must not be Inf";
        EXPECT_FALSE(std::isnan(nh)) << "normalized_harmonic must not be NaN";
        EXPECT_FALSE(std::isinf(nh)) << "normalized_harmonic must not be Inf";
    }
}

TEST_F(GDB521_HarmonicCentralityQA, Numerical_Deterministic) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 1}, {1, 3}});

    auto r1 = run("knows");
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    auto r2 = run("knows");
    ASSERT_TRUE(r2.has_value()) << r2.error().message;

    ASSERT_EQ(r1->size(), r2->size());
    for (size_t i = 0; i < r1->size(); ++i) {
        auto h1 = std::get<double>((*r1)[i].values[1].data());
        auto h2 = std::get<double>((*r2)[i].values[1].data());
        EXPECT_DOUBLE_EQ(h1, h2) << "harmonic scores should be deterministic";

        auto n1 = std::get<double>((*r1)[i].values[2].data());
        auto n2 = std::get<double>((*r2)[i].values[2].data());
        EXPECT_DOUBLE_EQ(n1, n2) << "normalized scores should be deterministic";
    }
}

TEST_F(GDB521_HarmonicCentralityQA, Numerical_HarmonicAlwaysNonNegative) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {1, 4}, {4, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (const auto& [node, info] : m) {
        EXPECT_GE(info.harmonic, 0.0) << "node " << node;
        EXPECT_GE(info.normalized_harmonic, 0.0) << "node " << node;
    }
}

// ============================================================================
// Adversarial: Output structure
// ============================================================================

TEST_F(GDB521_HarmonicCentralityQA, OutputAlways3Columns) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (const auto& row : *result) {
        EXPECT_EQ(row.values.size(), 3u) << "each output row must have exactly 3 values";
    }
}

TEST_F(GDB521_HarmonicCentralityQA, OutputSortedByNodeId) {
    build_graph("knows", {{50, 10}, {10, 30}, {30, 20}, {20, 40}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (size_t i = 1; i < result->size(); ++i) {
        auto prev = std::get<int64_t>((*result)[i - 1].values[0].data());
        auto curr = std::get<int64_t>((*result)[i].values[0].data());
        EXPECT_LT(prev, curr) << "results must be sorted by node_id";
    }
}

// ============================================================================
// Adversarial: Error paths
// ============================================================================

TEST_F(GDB521_HarmonicCentralityQA, Error_NonexistentEdgeType) {
    build_graph("knows", {{1, 2}});

    auto result = run("no_such_edge_type");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// ============================================================================
// Adversarial: Graph topology edge cases
// ============================================================================

TEST_F(GDB521_HarmonicCentralityQA, Topology_WheelGraph) {
    // Wheel W5: hub=1, rim 2-3-4-5-6-2, all bidirectional + hub to all.
    build_graph("knows", {
        {1, 2}, {2, 1}, {1, 3}, {3, 1}, {1, 4}, {4, 1}, {1, 5}, {5, 1}, {1, 6}, {6, 1},
        {2, 3}, {3, 2}, {3, 4}, {4, 3}, {4, 5}, {5, 4}, {5, 6}, {6, 5}, {6, 2}, {2, 6},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 6u);

    // Hub should have highest harmonic (reaches all 5 at d=1).
    EXPECT_DOUBLE_EQ(m[1].harmonic, 5.0);

    // All rim nodes should be equal by symmetry.
    for (int64_t i = 3; i <= 6; ++i) {
        EXPECT_NEAR(m[2].harmonic, m[i].harmonic, 1e-10)
            << "rim nodes should have equal harmonic centrality";
    }

    EXPECT_GT(m[1].harmonic, m[2].harmonic);
}

TEST_F(GDB521_HarmonicCentralityQA, Topology_SelfLoopPlusRealEdges) {
    // Self-loops mixed with real edges should not affect BFS.
    build_graph("knows", {{1, 1}, {1, 2}, {2, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);

    // Node 1: reaches 2(d=1), 3(d=2). H = 1 + 0.5 = 1.5
    // Self-loop doesn't contribute because node 1 is already at distance 0.
    EXPECT_NEAR(m[1].harmonic, 1.5, 1e-10);
}

TEST_F(GDB521_HarmonicCentralityQA, Topology_TreeGraph) {
    // Binary tree: 1->{2,3}, 2->{4,5}, 3->{6,7} (directed).
    build_graph("knows", {
        {1, 2}, {1, 3}, {2, 4}, {2, 5}, {3, 6}, {3, 7},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 7u);

    // Root (1) reaches all 6 nodes: 2(1), 3(1), 4(2), 5(2), 6(2), 7(2).
    // H(1) = 2*(1/1) + 4*(1/2) = 2 + 2 = 4.0
    EXPECT_DOUBLE_EQ(m[1].harmonic, 4.0);

    // Leaves cannot reach anyone. H = 0.
    for (int64_t leaf : {4, 5, 6, 7}) {
        EXPECT_DOUBLE_EQ(m[leaf].harmonic, 0.0)
            << "leaf node " << leaf << " should have H=0 in directed tree";
    }

    // Node 2: reaches 4(1), 5(1). H(2) = 2.0
    EXPECT_DOUBLE_EQ(m[2].harmonic, 2.0);

    // Symmetry: nodes 2 and 3 should be equal.
    EXPECT_DOUBLE_EQ(m[2].harmonic, m[3].harmonic);
}

TEST_F(GDB521_HarmonicCentralityQA, Topology_BipartiteGraph) {
    // Complete bipartite K3,3: {1,2,3} -> {4,5,6}, bidirectional.
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

    // K3,3 bidirectional: each node reaches 3 at d=1 and 2 at d=2.
    // H = 3*(1/1) + 2*(1/2) = 3 + 1 = 4.0
    for (const auto& [node, info] : m) {
        EXPECT_DOUBLE_EQ(info.harmonic, 4.0)
            << "node " << node << " in K3,3 bidirectional should have H=4.0";
    }
}

// ============================================================================
// Adversarial: Mathematical invariant — sum of reciprocals
// ============================================================================

TEST_F(GDB521_HarmonicCentralityQA, Invariant_HarmonicIsSumOfReciprocals) {
    // For a known graph, manually compute and verify the sum.
    // Graph: 1->2, 1->3, 2->3, 3->4
    build_graph("knows", {{1, 2}, {1, 3}, {2, 3}, {3, 4}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);

    // Node 1: BFS: 2(d=1), 3(d=1), 4(d=2). H = 1/1 + 1/1 + 1/2 = 2.5
    EXPECT_DOUBLE_EQ(m[1].harmonic, 2.5);

    // Node 2: BFS: 3(d=1), 4(d=2). H = 1/1 + 1/2 = 1.5
    EXPECT_DOUBLE_EQ(m[2].harmonic, 1.5);

    // Node 3: BFS: 4(d=1). H = 1.0
    EXPECT_DOUBLE_EQ(m[3].harmonic, 1.0);

    // Node 4: BFS: no outgoing. H = 0.0
    EXPECT_DOUBLE_EQ(m[4].harmonic, 0.0);
}

// ============================================================================
// Stress tests
// ============================================================================

TEST_F(GDB521_HarmonicCentralityQA, Stress_ChainOf100Nodes) {
    // Directed chain: 1->2->3->...->100.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i < 100; ++i) {
        edges.push_back({i, i + 1});
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 100u);

    // Node 1 should have highest harmonic (reaches all 99 others).
    // Node 100 should have H=0 (reaches nobody).
    double max_h = 0.0;
    int64_t max_node = -1;
    for (const auto& [node, info] : m) {
        if (info.harmonic > max_h) {
            max_h = info.harmonic;
            max_node = node;
        }
        EXPECT_GE(info.normalized_harmonic, 0.0);
        EXPECT_LE(info.normalized_harmonic, 1.0 + 1e-10);
    }
    EXPECT_EQ(max_node, 1) << "node 1 should have highest H in directed chain";
    EXPECT_DOUBLE_EQ(m[100].harmonic, 0.0);
}

TEST_F(GDB521_HarmonicCentralityQA, Stress_BidirectionalChainOf100) {
    // Bidirectional chain: 1<->2<->3<->...->100.
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

    // Center node (50) should have highest harmonic.
    double max_h = 0.0;
    int64_t max_node = -1;
    for (const auto& [node, info] : m) {
        if (info.harmonic > max_h) {
            max_h = info.harmonic;
            max_node = node;
        }
    }
    EXPECT_GE(max_node, 45);
    EXPECT_LE(max_node, 55);

    // All normalized in [0,1].
    for (const auto& [node, info] : m) {
        EXPECT_GE(info.normalized_harmonic, 0.0);
        EXPECT_LE(info.normalized_harmonic, 1.0 + 1e-10);
    }
}

TEST_F(GDB521_HarmonicCentralityQA, Stress_DenseGraph50Nodes) {
    // K50 bidirectional.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 50; ++i) {
        for (int64_t j = 1; j <= 50; ++j) {
            if (i != j) edges.push_back({i, j});
        }
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 50u);

    // Complete graph: all nodes at d=1 to all others. H = 49.0. Normalized = 1.0.
    for (const auto& [node, info] : m) {
        EXPECT_DOUBLE_EQ(info.harmonic, 49.0)
            << "node " << node << " in K50 should have H=49.0";
        EXPECT_DOUBLE_EQ(info.normalized_harmonic, 1.0)
            << "node " << node << " in K50 should have normalized=1.0";
    }
}

TEST_F(GDB521_HarmonicCentralityQA, Stress_ManyDisconnectedComponents) {
    // 50 disconnected pairs: 100 nodes total.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 99; i += 2) {
        edges.push_back({i, i + 1});
        edges.push_back({i + 1, i});
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 100u);

    // Each node reaches 1 other at d=1. H = 1.0. Normalized = 1.0/99.
    for (const auto& [node, info] : m) {
        EXPECT_DOUBLE_EQ(info.harmonic, 1.0);
        EXPECT_NEAR(info.normalized_harmonic, 1.0 / 99.0, 1e-10);
    }
}

} // namespace
} // namespace sixseven
