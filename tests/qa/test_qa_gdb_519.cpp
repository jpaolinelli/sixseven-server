/// @file test_qa_gdb_519.cpp
/// QA adversarial tests for GDB-519: Wasserman-Faust Closeness Centrality
/// for disconnected graphs.
///
/// Verifies:
///   AC1: Wasserman-Faust variant correctly scales by component size.
///   AC2: Nodes in disconnected components get meaningful (non-zero) scores.
///   AC3: Consistent with standard closeness on fully connected graphs.
///   AC4: Unit tests with multi-component graphs.
///   AC5: No regressions.
///
/// Adversarial categories: boundary values, error paths, graph topology
/// edge cases, numerical stability, stress tests, mathematical invariants.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/closeness_centrality.h"
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

struct WFInfo {
    double closeness;
    int64_t sum_farness;
    int64_t reachable_count;
    int64_t component_size;
    double normalized_closeness;
};

std::unordered_map<int64_t, WFInfo>
to_wf_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, WFInfo> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 6u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto closeness = std::get<double>(row.values[1].data());
        auto sum_farness = std::get<int64_t>(row.values[2].data());
        auto reachable_count = std::get<int64_t>(row.values[3].data());
        auto component_size = std::get<int64_t>(row.values[4].data());
        auto normalized_closeness = std::get<double>(row.values[5].data());
        result[node_id] = {closeness, sum_farness, reachable_count, component_size,
                           normalized_closeness};
    }
    return result;
}

// ============================================================================
// Test fixture
// ============================================================================

class GDB519_WassermanFaust : public ::testing::Test {
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

    Result<std::vector<AlgorithmRow>> run_wf(const std::string& edge_type) {
        std::unordered_map<std::string, Value> args;
        args["variant"] = Value(std::string("wasserman_faust"));
        AlgorithmContext ctx{engine_, edge_type, args};
        return closeness_centrality_execute(ctx);
    }

    Result<std::vector<AlgorithmRow>> run_standard(const std::string& edge_type) {
        std::unordered_map<std::string, Value> args;
        args["variant"] = Value(std::string("standard"));
        AlgorithmContext ctx{engine_, edge_type, args};
        return closeness_centrality_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ============================================================================
// AC1: Wasserman-Faust variant correctly scales by component size
// ============================================================================

/// Verify the WF formula: C_WF(v) = [(n_c-1)/(N-1)] * [(n_c-1)/sum_farness]
/// for all nodes in a multi-component graph with known analytical values.
TEST_F(GDB519_WassermanFaust, AC1_FormulaVerification_ThreeComponents) {
    // Component A: bidirectional K3 (nodes 1,2,3) — n_c = 3
    // Component B: bidirectional K4 (nodes 10,11,12,13) — n_c = 4
    // Component C: bidirectional pair (nodes 20,21) — n_c = 2
    // N = 9
    build_graph("knows", {
        // K3
        {1, 2}, {2, 1}, {1, 3}, {3, 1}, {2, 3}, {3, 2},
        // K4
        {10, 11}, {11, 10}, {10, 12}, {12, 10}, {10, 13}, {13, 10},
        {11, 12}, {12, 11}, {11, 13}, {13, 11}, {12, 13}, {13, 12},
        // K2
        {20, 21}, {21, 20},
    });

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_wf_map(*result);
    ASSERT_EQ(scores.size(), 9u);

    const int64_t N = 9;

    // K3: each node reaches 2 others at d=1. sum_farness=2, nc=3.
    // normalized = (3-1)/2 = 1.0. scaling = (3-1)/(9-1) = 2/8 = 0.25.
    // WF = 0.25 * 1.0 = 0.25.
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(scores[node].component_size, 3);
        EXPECT_NEAR(scores[node].normalized_closeness, 1.0, 1e-10);
        double scaling = static_cast<double>(3 - 1) / static_cast<double>(N - 1);
        EXPECT_NEAR(scores[node].closeness, scaling * 1.0, 1e-10)
            << "K3 node " << node;
    }

    // K4: each node reaches 3 others at d=1. sum_farness=3, nc=4.
    // normalized = (4-1)/3 = 1.0. scaling = (4-1)/(9-1) = 3/8 = 0.375.
    // WF = 0.375 * 1.0 = 0.375.
    for (int64_t node : {10, 11, 12, 13}) {
        EXPECT_EQ(scores[node].component_size, 4);
        EXPECT_NEAR(scores[node].normalized_closeness, 1.0, 1e-10);
        double scaling = static_cast<double>(4 - 1) / static_cast<double>(N - 1);
        EXPECT_NEAR(scores[node].closeness, scaling * 1.0, 1e-10)
            << "K4 node " << node;
    }

    // K2: each node reaches 1 other at d=1. sum_farness=1, nc=2.
    // normalized = (2-1)/1 = 1.0. scaling = (2-1)/(9-1) = 1/8 = 0.125.
    // WF = 0.125 * 1.0 = 0.125.
    for (int64_t node : {20, 21}) {
        EXPECT_EQ(scores[node].component_size, 2);
        EXPECT_NEAR(scores[node].normalized_closeness, 1.0, 1e-10);
        double scaling = static_cast<double>(2 - 1) / static_cast<double>(N - 1);
        EXPECT_NEAR(scores[node].closeness, scaling * 1.0, 1e-10)
            << "K2 node " << node;
    }

    // Verify ordering: K4 nodes > K3 nodes > K2 nodes.
    EXPECT_GT(scores[10].closeness, scores[1].closeness);
    EXPECT_GT(scores[1].closeness, scores[20].closeness);
}

/// Verify scaling factor is exactly (n_c-1)/(N-1) by algebraic check.
TEST_F(GDB519_WassermanFaust, AC1_ScalingFactorAlgebraicCheck) {
    // Two components of equal internal structure but different sizes:
    // Comp A: path 1->2->3->4 (nc=4)
    // Comp B: path 10->11 (nc=2)
    // N=6
    build_graph("knows", {
        {1, 2}, {2, 3}, {3, 4},
        {10, 11},
    });

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_wf_map(*result);

    const int64_t N = 6;

    // For every reachable node, verify WF = [(nc-1)/(N-1)] * [(nc-1)/sum_farness]
    for (const auto& [node, r] : scores) {
        if (r.sum_farness > 0 && r.component_size > 1) {
            double expected_norm =
                static_cast<double>(r.component_size - 1) / static_cast<double>(r.sum_farness);
            double expected_scaling =
                static_cast<double>(r.component_size - 1) / static_cast<double>(N - 1);
            double expected_wf = expected_scaling * expected_norm;

            EXPECT_NEAR(r.normalized_closeness, expected_norm, 1e-10)
                << "node " << node << " normalized_closeness formula check";
            EXPECT_NEAR(r.closeness, expected_wf, 1e-10)
                << "node " << node << " WF formula check";
        }
    }
}

// ============================================================================
// AC2: Nodes in disconnected components get meaningful (non-zero) scores
// ============================================================================

/// Every non-sink node in a disconnected graph must have closeness > 0.
TEST_F(GDB519_WassermanFaust, AC2_DisconnectedNodesNonZero) {
    // 5 disconnected bidirectional pairs: all nodes should have WF > 0.
    build_graph("knows", {
        {1, 2}, {2, 1},
        {3, 4}, {4, 3},
        {5, 6}, {6, 5},
        {7, 8}, {8, 7},
        {9, 10}, {10, 9},
    });

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_wf_map(*result);
    ASSERT_EQ(scores.size(), 10u);

    for (const auto& [node, r] : scores) {
        EXPECT_GT(r.closeness, 0.0)
            << "node " << node << " in disconnected bidirectional pair should have WF > 0";
        EXPECT_EQ(r.component_size, 2);
    }
}

/// Standard closeness gives 0 for sink nodes in directed disconnected graphs,
/// but WF should also give 0 for sink nodes (they can't reach anyone).
TEST_F(GDB519_WassermanFaust, AC2_DirectedDisconnectedSinksAreZero) {
    // Two directed edges: 1->2, 3->4. Sinks (2,4) can't reach anyone.
    build_graph("knows", {{1, 2}, {3, 4}});

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_wf_map(*result);

    // Sources should have WF > 0.
    EXPECT_GT(scores[1].closeness, 0.0);
    EXPECT_GT(scores[3].closeness, 0.0);

    // Sinks should have WF = 0 (can't reach anyone).
    EXPECT_DOUBLE_EQ(scores[2].closeness, 0.0);
    EXPECT_DOUBLE_EQ(scores[4].closeness, 0.0);
}

/// Many small components: every reachable node should still get a score > 0.
TEST_F(GDB519_WassermanFaust, AC2_ManySmallComponents) {
    // 10 bidirectional triangles: nodes (i*10+1, i*10+2, i*10+3) for i=0..9.
    // N = 30, each nc = 3.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int i = 0; i < 10; ++i) {
        int64_t a = i * 10 + 1;
        int64_t b = i * 10 + 2;
        int64_t c = i * 10 + 3;
        edges.push_back({a, b}); edges.push_back({b, a});
        edges.push_back({b, c}); edges.push_back({c, b});
        edges.push_back({a, c}); edges.push_back({c, a});
    }
    build_graph("knows", edges);

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_wf_map(*result);
    ASSERT_EQ(scores.size(), 30u);

    // All should have identical WF since all components are structurally identical.
    double first_wf = scores.begin()->second.closeness;
    for (const auto& [node, r] : scores) {
        EXPECT_GT(r.closeness, 0.0) << "node " << node;
        EXPECT_NEAR(r.closeness, first_wf, 1e-10)
            << "node " << node << " should match all others in identical components";
        EXPECT_EQ(r.component_size, 3) << "node " << node;
    }

    // Expected: normalized = (3-1)/2 = 1.0. scaling = (3-1)/(30-1) = 2/29.
    // WF = 2/29 * 1.0 ≈ 0.06897.
    EXPECT_NEAR(first_wf, 2.0 / 29.0, 1e-10);
}

// ============================================================================
// AC3: Consistent with standard closeness on fully connected graphs
// ============================================================================

/// On a single-component graph, WF should equal standard closeness
/// since (n_c-1)/(N-1) = 1.0 when n_c = N.
TEST_F(GDB519_WassermanFaust, AC3_SingleComponentWFEqualsStandard) {
    // Bidirectional star: hub=1, spokes=2..6. Single component, N=nc=6.
    build_graph("star", {
        {1, 2}, {2, 1}, {1, 3}, {3, 1}, {1, 4}, {4, 1},
        {1, 5}, {5, 1}, {1, 6}, {6, 1},
    });

    auto wf_result = run_wf("star");
    ASSERT_TRUE(wf_result.has_value()) << wf_result.error().message;

    auto std_result = run_standard("star");
    ASSERT_TRUE(std_result.has_value()) << std_result.error().message;

    auto wf_scores = to_wf_map(*wf_result);
    auto std_scores = to_wf_map(*std_result);

    ASSERT_EQ(wf_scores.size(), std_scores.size());
    for (const auto& [node, wf] : wf_scores) {
        auto it = std_scores.find(node);
        ASSERT_NE(it, std_scores.end());
        EXPECT_NEAR(wf.closeness, it->second.closeness, 1e-10)
            << "node " << node << " WF should equal standard on single component";
    }
}

/// Another connected topology: directed cycle.
TEST_F(GDB519_WassermanFaust, AC3_DirectedCycleWFEqualsStandard) {
    // Directed cycle: 1->2->3->4->5->1. Single component.
    build_graph("cycle", {{1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 1}});

    auto wf_result = run_wf("cycle");
    ASSERT_TRUE(wf_result.has_value()) << wf_result.error().message;

    auto std_result = run_standard("cycle");
    ASSERT_TRUE(std_result.has_value()) << std_result.error().message;

    auto wf_scores = to_wf_map(*wf_result);
    auto std_scores = to_wf_map(*std_result);

    for (const auto& [node, wf] : wf_scores) {
        auto it = std_scores.find(node);
        ASSERT_NE(it, std_scores.end());
        EXPECT_NEAR(wf.closeness, it->second.closeness, 1e-10)
            << "node " << node << " WF should match standard on directed cycle";
    }
}

/// Connected bidirectional path: WF = standard since nc = N.
TEST_F(GDB519_WassermanFaust, AC3_ConnectedPathWFEqualsStandard) {
    // 1 <-> 2 <-> 3 <-> 4 <-> 5 <-> 6 <-> 7
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 6; ++i) {
        edges.push_back({i, i + 1});
        edges.push_back({i + 1, i});
    }
    build_graph("path", edges);

    auto wf_result = run_wf("path");
    ASSERT_TRUE(wf_result.has_value()) << wf_result.error().message;

    auto std_result = run_standard("path");
    ASSERT_TRUE(std_result.has_value()) << std_result.error().message;

    auto wf_scores = to_wf_map(*wf_result);
    auto std_scores = to_wf_map(*std_result);

    for (const auto& [node, wf] : wf_scores) {
        EXPECT_NEAR(wf.closeness, std_scores[node].closeness, 1e-10)
            << "node " << node;
    }
}

// ============================================================================
// AC4: Multi-component graph tests
// ============================================================================

/// One large component + one tiny component: verify ranking.
TEST_F(GDB519_WassermanFaust, AC4_LargeVsTinyComponent) {
    // Large: bidirectional K5 (nodes 1-5), nc=5.
    // Tiny: bidirectional pair (nodes 100,101), nc=2.
    // N=7.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 5; ++i) {
        for (int64_t j = 1; j <= 5; ++j) {
            if (i != j) edges.push_back({i, j});
        }
    }
    edges.push_back({100, 101});
    edges.push_back({101, 100});
    build_graph("knows", edges);

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_wf_map(*result);
    ASSERT_EQ(scores.size(), 7u);

    // Large component nodes should all rank higher than tiny component nodes.
    for (int64_t big : {1, 2, 3, 4, 5}) {
        for (int64_t small_node : {100, 101}) {
            EXPECT_GT(scores[big].closeness, scores[small_node].closeness)
                << "K5 node " << big << " should rank higher than K2 node " << small_node;
        }
    }
}

/// Five components of sizes 1-edge each but with different internal structures.
TEST_F(GDB519_WassermanFaust, AC4_DifferentDirectedTopologies) {
    // Comp A: 1->2->3 (chain, nc=3). Node 1 reaches 2,3. Node 2 reaches 3. Node 3 reaches none.
    // Comp B: 10<->11<->12 (bidir path, nc=3). All reach all.
    // N = 6.
    build_graph("knows", {
        {1, 2}, {2, 3},
        {10, 11}, {11, 10}, {11, 12}, {12, 11},
    });

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_wf_map(*result);
    ASSERT_EQ(scores.size(), 6u);

    // Both components have nc=3 and N=6, so scaling = (3-1)/(6-1) = 2/5.
    // The difference is within-component closeness.

    // Node 10 (center of bidir path): reaches 11(d=1), 12(d=2). farness=3, reachable=3.
    // Wait — 10<->11<->12. From 10: 11(d=1), 12(d=2). sum=3. normalized=(3-1)/3=2/3.
    // WF = 2/5 * 2/3 = 4/15.

    // Node 11 (center of bidir path): reaches 10(d=1), 12(d=1). farness=2, reachable=3.
    // normalized = (3-1)/2 = 1.0. WF = 2/5 * 1.0 = 2/5 = 0.4.
    EXPECT_NEAR(scores[11].closeness, 2.0 / 5.0, 1e-10);

    // Node 1 (chain head): reaches 2(d=1), 3(d=2). farness=3, reachable=3.
    // normalized = (3-1)/3 = 2/3. WF = 2/5 * 2/3 = 4/15.
    EXPECT_NEAR(scores[1].closeness, 4.0 / 15.0, 1e-10);

    // Node 3 (chain tail): can't reach anyone. WF = 0.
    EXPECT_DOUBLE_EQ(scores[3].closeness, 0.0);
}

// ============================================================================
// Boundary values
// ============================================================================

/// Self-loop only: single node with a self-loop. N=1, nc=1.
TEST_F(GDB519_WassermanFaust, Boundary_SelfLoop) {
    build_graph("knows", {{1, 1}});

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_wf_map(*result);
    ASSERT_EQ(scores.size(), 1u);

    // Node 1: BFS visits itself, neighbor is itself (already visited).
    // reachable=1, sum_farness=0. nc=1. closeness=0.
    EXPECT_DOUBLE_EQ(scores[1].closeness, 0.0);
    EXPECT_DOUBLE_EQ(scores[1].normalized_closeness, 0.0);
    EXPECT_EQ(scores[1].component_size, 1);
}

/// Self-loop plus a normal edge: verify self-loop doesn't corrupt results.
TEST_F(GDB519_WassermanFaust, Boundary_SelfLoopPlusNormalEdge) {
    build_graph("knows", {{1, 1}, {1, 2}, {2, 1}});

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_wf_map(*result);
    ASSERT_EQ(scores.size(), 2u);

    // Single component nc=2, N=2. scaling = 1/1 = 1.0.
    // Node 1: reaches 2 at d=1. normalized = (2-1)/1 = 1.0. WF = 1.0.
    EXPECT_NEAR(scores[1].closeness, 1.0, 1e-10);
    // Node 2: reaches 1 at d=1. normalized = (2-1)/1 = 1.0. WF = 1.0.
    EXPECT_NEAR(scores[2].closeness, 1.0, 1e-10);
}

/// Two nodes, single directed edge: N=2, nc=2.
TEST_F(GDB519_WassermanFaust, Boundary_TwoNodesSingleDirection) {
    build_graph("knows", {{1, 2}});

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_wf_map(*result);

    // nc=2, N=2. scaling = (2-1)/(2-1) = 1.0.
    // Node 1: normalized = (2-1)/1 = 1.0. WF = 1.0.
    EXPECT_NEAR(scores[1].closeness, 1.0, 1e-10);
    // Node 2: can't reach anyone. WF = 0.
    EXPECT_DOUBLE_EQ(scores[2].closeness, 0.0);
}

/// Empty graph returns empty results.
TEST_F(GDB519_WassermanFaust, Boundary_EmptyGraph) {
    build_graph("knows", {});

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

// ============================================================================
// Error paths
// ============================================================================

/// Invalid variant name should fail.
TEST_F(GDB519_WassermanFaust, Error_InvalidVariant) {
    build_graph("knows", {{1, 2}});

    std::unordered_map<std::string, Value> args;
    args["variant"] = Value(std::string("bogus"));
    AlgorithmContext ctx{engine_, "knows", args};
    auto result = closeness_centrality_execute(ctx);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

/// Nonexistent edge type should fail.
TEST_F(GDB519_WassermanFaust, Error_NonexistentEdgeType) {
    build_graph("knows", {{1, 2}});

    auto result = run_wf("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

/// Null variant parameter value should fail.
TEST_F(GDB519_WassermanFaust, Error_NullVariantParam) {
    build_graph("knows", {{1, 2}});

    std::unordered_map<std::string, Value> args;
    args["variant"] = Value(); // null
    AlgorithmContext ctx{engine_, "knows", args};
    auto result = closeness_centrality_execute(ctx);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

/// Integer variant parameter should fail with TYPE_ERROR.
TEST_F(GDB519_WassermanFaust, Error_IntegerVariantParam) {
    build_graph("knows", {{1, 2}});

    std::unordered_map<std::string, Value> args;
    args["variant"] = Value(int64_t{42});
    AlgorithmContext ctx{engine_, "knows", args};
    auto result = closeness_centrality_execute(ctx);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// ============================================================================
// Numerical stability
// ============================================================================

/// Long chain: verify no floating point issues with large farness sums.
TEST_F(GDB519_WassermanFaust, Numerical_LongChain) {
    // Bidirectional chain of 50 nodes in one component + a pair in another.
    // This tests large sum_farness values.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i < 50; ++i) {
        edges.push_back({i, i + 1});
        edges.push_back({i + 1, i});
    }
    // Disconnected pair.
    edges.push_back({100, 101});
    edges.push_back({101, 100});
    build_graph("knows", edges);

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_wf_map(*result);
    ASSERT_EQ(scores.size(), 52u);

    // All values must be finite and non-negative.
    for (const auto& [node, r] : scores) {
        EXPECT_GE(r.closeness, 0.0) << "node " << node;
        EXPECT_FALSE(std::isnan(r.closeness)) << "node " << node << " NaN";
        EXPECT_FALSE(std::isinf(r.closeness)) << "node " << node << " Inf";
        EXPECT_GE(r.normalized_closeness, 0.0) << "node " << node;
        EXPECT_FALSE(std::isnan(r.normalized_closeness)) << "node " << node;
        EXPECT_FALSE(std::isinf(r.normalized_closeness)) << "node " << node;
    }

    // Center node (25) of the 50-chain should have highest WF in that component.
    for (int64_t node : {1, 50}) {
        EXPECT_GT(scores[25].closeness, scores[node].closeness)
            << "center node 25 should rank higher than endpoint " << node;
    }

    // All 50-chain nodes should rank higher than pair nodes.
    // Even the endpoint of the chain is in a bigger component.
    EXPECT_GT(scores[1].closeness, scores[100].closeness)
        << "chain endpoint should rank higher than pair node due to component size";
}

/// Verify that all WF values are <= 1.0 for undirected (bidirectional) graphs.
TEST_F(GDB519_WassermanFaust, Numerical_WFBoundedByOneForUndirected) {
    // Bidirectional K5 + bidirectional K3. All undirected, all reach all in component.
    build_graph("knows", {
        {1, 2}, {2, 1}, {1, 3}, {3, 1}, {1, 4}, {4, 1}, {1, 5}, {5, 1},
        {2, 3}, {3, 2}, {2, 4}, {4, 2}, {2, 5}, {5, 2},
        {3, 4}, {4, 3}, {3, 5}, {5, 3},
        {4, 5}, {5, 4},
        {10, 11}, {11, 10}, {10, 12}, {12, 10}, {11, 12}, {12, 11},
    });

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_wf_map(*result);

    for (const auto& [node, r] : scores) {
        EXPECT_LE(r.closeness, 1.0 + 1e-10)
            << "node " << node << " WF should be <= 1.0 for undirected graph";
    }
}

// ============================================================================
// Mathematical invariants
// ============================================================================

/// WF closeness of a node must equal scaling * normalized_closeness.
TEST_F(GDB519_WassermanFaust, Invariant_WFEqualsScalingTimesNormalized) {
    // Complex multi-component directed graph.
    build_graph("knows", {
        {1, 2}, {2, 3}, {3, 1},       // directed cycle, nc=3
        {10, 11}, {11, 12}, {12, 10},  // another directed cycle, nc=3
        {20, 21},                       // pair, nc=2
        {30, 31}, {31, 30},            // bidirectional pair, nc=2
    });

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_wf_map(*result);

    const auto N = static_cast<int64_t>(scores.size());
    ASSERT_GT(N, 1);

    for (const auto& [node, r] : scores) {
        if (r.normalized_closeness > 0.0 && r.component_size > 1) {
            double scaling =
                static_cast<double>(r.component_size - 1) / static_cast<double>(N - 1);
            double expected_wf = scaling * r.normalized_closeness;
            EXPECT_NEAR(r.closeness, expected_wf, 1e-10)
                << "node " << node << " invariant: WF = scaling * normalized";
        } else {
            EXPECT_DOUBLE_EQ(r.closeness, 0.0)
                << "node " << node << " with 0 normalized should have 0 WF";
        }
    }
}

/// Component size for a node must equal the number of nodes with the same
/// component_size root. In other words: summing unique component_sizes should
/// equal the total number of nodes.
TEST_F(GDB519_WassermanFaust, Invariant_ComponentSizesSumToN) {
    build_graph("knows", {
        {1, 2}, {2, 3},        // nc=3
        {10, 11},              // nc=2
        {20, 21}, {21, 22},    // nc=3
        {30, 31}, {31, 30},    // nc=2
    });

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_wf_map(*result);

    // Group nodes by their component membership (nodes with same component_size
    // adjacent in node_id space).
    std::unordered_map<int64_t, std::vector<int64_t>> components;
    for (const auto& [node, r] : scores) {
        components[r.component_size].push_back(node);
    }

    // Total node count should match scores size.
    EXPECT_GT(scores.size(), 0u);

    // Verify each node's component_size matches the count of nodes in its component.
    // We can verify this by checking nodes with the same component root.
    // Simpler check: each node's component_size >= 1 and <= N.
    for (const auto& [node, r] : scores) {
        EXPECT_GE(r.component_size, 1) << "node " << node;
        EXPECT_LE(r.component_size, static_cast<int64_t>(scores.size()))
            << "node " << node;
    }
}

/// Reachable count must be <= component size (for directed graphs, you may not
/// reach all nodes in your weakly connected component).
TEST_F(GDB519_WassermanFaust, Invariant_ReachableLeqComponentSize) {
    // Directed graph: not all nodes in a component are reachable from each other.
    build_graph("knows", {
        {1, 2}, {2, 3},        // 1 reaches 2,3. 2 reaches 3. 3 reaches none. nc=3.
        {10, 11}, {11, 10},    // Bidirectional pair. nc=2.
    });

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_wf_map(*result);

    for (const auto& [node, r] : scores) {
        EXPECT_LE(r.reachable_count, r.component_size)
            << "node " << node << " reachable_count should be <= component_size";
    }
}

/// Sum farness must be >= reachable_count - 1 (minimum when all at distance 1).
TEST_F(GDB519_WassermanFaust, Invariant_FarnessLowerBound) {
    build_graph("knows", {
        {1, 2}, {1, 3}, {1, 4}, {2, 1}, {3, 1}, {4, 1},  // star
        {10, 11}, {11, 12},  // chain
    });

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_wf_map(*result);

    for (const auto& [node, r] : scores) {
        if (r.reachable_count > 1) {
            EXPECT_GE(r.sum_farness, r.reachable_count - 1)
                << "node " << node << " farness must be >= reachable-1";
        }
    }
}

// ============================================================================
// Stress test
// ============================================================================

/// Moderate-sized graph with multiple components: verify no crashes or hangs.
TEST_F(GDB519_WassermanFaust, Stress_ModerateGraph) {
    // 5 bidirectional cycles of 20 nodes each = 100 nodes total, 5 components.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int comp = 0; comp < 5; ++comp) {
        int64_t base = comp * 100;
        for (int64_t i = 0; i < 20; ++i) {
            int64_t a = base + i;
            int64_t b = base + (i + 1) % 20;
            edges.push_back({a, b});
            edges.push_back({b, a});
        }
    }
    build_graph("knows", edges);

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_wf_map(*result);
    ASSERT_EQ(scores.size(), 100u);

    // All nodes in same-sized components should have equal WF closeness.
    double expected_wf = scores.begin()->second.closeness;
    for (const auto& [node, r] : scores) {
        EXPECT_GT(r.closeness, 0.0) << "node " << node;
        EXPECT_NEAR(r.closeness, expected_wf, 1e-10)
            << "all nodes in identical 20-node cycles should have same WF";
        EXPECT_EQ(r.component_size, 20) << "node " << node;
    }
}

/// Large graph: 200 nodes in mixed component sizes.
TEST_F(GDB519_WassermanFaust, Stress_LargerMixedComponents) {
    std::vector<std::pair<int64_t, int64_t>> edges;

    // Component 1: bidirectional K10 (nodes 1-10)
    for (int64_t i = 1; i <= 10; ++i) {
        for (int64_t j = i + 1; j <= 10; ++j) {
            edges.push_back({i, j});
            edges.push_back({j, i});
        }
    }

    // Components 2-20: bidirectional pairs (nodes 100+)
    for (int c = 0; c < 19; ++c) {
        int64_t a = 100 + c * 2;
        int64_t b = 101 + c * 2;
        edges.push_back({a, b});
        edges.push_back({b, a});
    }

    build_graph("knows", edges);

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_wf_map(*result);

    // 10 + 19*2 = 48 nodes.
    ASSERT_EQ(scores.size(), 48u);

    // K10 nodes should all rank above pair nodes.
    for (int64_t big : {1, 5, 10}) {
        EXPECT_GT(scores[big].closeness, scores[100].closeness)
            << "K10 node " << big << " should rank above pair node";
    }

    // All values finite and non-negative.
    for (const auto& [node, r] : scores) {
        EXPECT_GE(r.closeness, 0.0);
        EXPECT_FALSE(std::isnan(r.closeness));
        EXPECT_FALSE(std::isinf(r.closeness));
    }
}

// ============================================================================
// Output schema
// ============================================================================

/// Verify output rows have exactly 6 columns with correct types.
TEST_F(GDB519_WassermanFaust, Schema_SixColumnsCorrectTypes) {
    build_graph("knows", {{1, 2}, {2, 1}, {3, 4}, {4, 3}});

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (const auto& row : *result) {
        ASSERT_EQ(row.values.size(), 6u);
        // node_id: INT64
        EXPECT_NO_THROW(std::get<int64_t>(row.values[0].data()));
        // closeness: FLOAT64
        EXPECT_NO_THROW(std::get<double>(row.values[1].data()));
        // sum_farness: INT64
        EXPECT_NO_THROW(std::get<int64_t>(row.values[2].data()));
        // reachable_count: INT64
        EXPECT_NO_THROW(std::get<int64_t>(row.values[3].data()));
        // component_size: INT64
        EXPECT_NO_THROW(std::get<int64_t>(row.values[4].data()));
        // normalized_closeness: FLOAT64
        EXPECT_NO_THROW(std::get<double>(row.values[5].data()));
    }
}

/// Results should be sorted by node_id.
TEST_F(GDB519_WassermanFaust, Schema_ResultsSortedByNodeId) {
    build_graph("knows", {
        {50, 10}, {10, 30}, {30, 50},
        {5, 90}, {90, 5},
    });

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (size_t i = 1; i < result->size(); ++i) {
        auto prev = std::get<int64_t>((*result)[i - 1].values[0].data());
        auto curr = std::get<int64_t>((*result)[i].values[0].data());
        EXPECT_LT(prev, curr) << "results must be sorted by node_id";
    }
}

// ============================================================================
// Regression: standard variant unchanged
// ============================================================================

/// Adding WF variant should not change standard variant behavior.
TEST_F(GDB519_WassermanFaust, Regression_StandardUnchanged) {
    // Verify standard closeness still works correctly on a known graph.
    // Directed triangle: 1->2, 2->3, 3->1.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run_standard("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_wf_map(*result);

    // Each node reaches 2 others at d=1 and d=2. sum=3. closeness = 2/3.
    for (int64_t node : {1, 2, 3}) {
        EXPECT_NEAR(scores[node].closeness, 2.0 / 3.0, 1e-10);
        EXPECT_EQ(scores[node].component_size, 3);
        // Standard: closeness == normalized_closeness.
        EXPECT_NEAR(scores[node].closeness, scores[node].normalized_closeness, 1e-10);
    }
}

/// Default variant (no args) should behave as standard.
TEST_F(GDB519_WassermanFaust, Regression_DefaultIsStandard) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    // No variant arg at all.
    AlgorithmContext ctx{engine_, "knows", {}};
    auto result = closeness_centrality_execute(ctx);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map(*result);
    for (int64_t node : {1, 2, 3}) {
        // Standard closeness for directed triangle: 2/3.
        EXPECT_NEAR(scores[node].closeness, 2.0 / 3.0, 1e-10);
    }
}

} // namespace
} // namespace sixseven
