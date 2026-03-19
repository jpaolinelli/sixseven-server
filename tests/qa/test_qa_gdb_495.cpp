/// @file test_qa_gdb_495.cpp
/// QA adversarial tests for GDB-495: Betweenness Centrality.
///
/// Verifies:
///   AC1: Brandes' algorithm implementation.
///   AC2: Output schema: (node_id INT64, centrality FLOAT64).
///   AC3: Correct values on known graphs.
///   AC4: Unit tests passing.
///
/// Adversarial categories: graph topology edge cases, parallel paths,
/// duplicate edges, boundary node IDs, normalization boundaries,
/// analytical formula verification, multiple edge types, stress tests.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/betweenness_centrality.h"
#include "sixseven/graph/graph_engine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
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

/// Extract (node_id, centrality) pairs from algorithm result rows.
std::unordered_map<int64_t, double> to_centrality_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, double> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 2u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto centrality = std::get<double>(row.values[1].data());
        result[node_id] = centrality;
    }
    return result;
}

/// Verify that all centrality scores are non-negative.
void verify_scores_non_negative(const std::unordered_map<int64_t, double>& scores) {
    for (const auto& [node, score] : scores) {
        EXPECT_GE(score, 0.0) << "node " << node << " should have non-negative centrality";
    }
}

/// Verify that no score is NaN or Inf.
void verify_scores_finite(const std::unordered_map<int64_t, double>& scores) {
    for (const auto& [node, score] : scores) {
        EXPECT_FALSE(std::isnan(score)) << "node " << node << " has NaN centrality";
        EXPECT_FALSE(std::isinf(score)) << "node " << node << " has Inf centrality";
    }
}

/// Verify output rows are sorted by node_id.
void verify_sorted_by_node_id(const std::vector<AlgorithmRow>& rows) {
    for (size_t i = 1; i < rows.size(); ++i) {
        auto prev = std::get<int64_t>(rows[i - 1].values[0].data());
        auto curr = std::get<int64_t>(rows[i].values[0].data());
        EXPECT_LT(prev, curr) << "results should be sorted by node_id";
    }
}

// ============================================================================
// Test fixture
// ============================================================================

class QA_GDB495_Betweenness : public ::testing::Test {
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

    /// Run betweenness centrality with normalized=true (default).
    Result<std::vector<AlgorithmRow>> run_betweenness(const std::string& edge_type) {
        AlgorithmContext ctx{
            engine_, default_database_id, edge_type, {{"normalized", Value(true)}}};
        return betweenness_centrality_execute(ctx);
    }

    /// Run betweenness centrality with normalized=false.
    Result<std::vector<AlgorithmRow>> run_betweenness_unnormalized(const std::string& edge_type) {
        AlgorithmContext ctx{
            engine_, default_database_id, edge_type, {{"normalized", Value(false)}}};
        return betweenness_centrality_execute(ctx);
    }

    /// Run betweenness centrality with raw named_args.
    Result<std::vector<AlgorithmRow>>
    run_betweenness_raw(const std::string& edge_type, std::unordered_map<std::string, Value> args) {
        AlgorithmContext ctx{engine_, default_database_id, edge_type, std::move(args)};
        return betweenness_centrality_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ============================================================================
// AC1: Brandes' algorithm implementation
// ============================================================================

TEST_F(QA_GDB495_Betweenness, AC1_ThreeWayParallelPaths) {
    // W-graph: 1->2, 1->3, 1->4, 2->5, 3->5, 4->5
    // Three equal-length shortest paths from 1 to 5.
    // Each intermediary (2, 3, 4) gets 1/3 of the betweenness for pair (1,5).
    build_graph("knows", {{1, 2}, {1, 3}, {1, 4}, {2, 5}, {3, 5}, {4, 5}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 5u);
    verify_scores_non_negative(scores);

    // Nodes 2, 3, 4 each carry 1/3 of betweenness for pair (1,5).
    EXPECT_NEAR(scores[2], 1.0 / 3.0, 1e-10);
    EXPECT_NEAR(scores[3], 1.0 / 3.0, 1e-10);
    EXPECT_NEAR(scores[4], 1.0 / 3.0, 1e-10);
    // Source and sink have zero betweenness.
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
    EXPECT_DOUBLE_EQ(scores[5], 0.0);
}

TEST_F(QA_GDB495_Betweenness, AC1_HourglassGraph) {
    // Hourglass: {1,2} -> 3 -> {4,5}
    // Node 3 is the sole bridge. It lies on all 4 paths: 1->4, 1->5, 2->4, 2->5.
    build_graph("knows", {{1, 3}, {2, 3}, {3, 4}, {3, 5}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 5u);
    verify_scores_non_negative(scores);

    // Node 3 is on all cross-cluster shortest paths.
    // BFS from 1: node 3 on paths 1->3->4 and 1->3->5 => delta[3]=2
    // BFS from 2: node 3 on paths 2->3->4 and 2->3->5 => delta[3]=2
    // Total centrality[3] = 4.0
    EXPECT_DOUBLE_EQ(scores[3], 4.0);
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
    EXPECT_DOUBLE_EQ(scores[2], 0.0);
    EXPECT_DOUBLE_EQ(scores[4], 0.0);
    EXPECT_DOUBLE_EQ(scores[5], 0.0);
}

TEST_F(QA_GDB495_Betweenness, AC1_TwoHopChainWithBypass) {
    // 1->2->3 plus bypass 1->3.
    // Two paths from 1 to 3: direct (1->3) and through 2 (1->2->3).
    // Direct path is shorter (length 1 vs 2), so node 2 is NOT on any
    // shortest path from 1 to 3. Betweenness of node 2 should be 0.
    build_graph("knows", {{1, 2}, {2, 3}, {1, 3}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 3u);

    // With the bypass, the shortest path 1->3 has length 1.
    // Node 2 is NOT on any shortest path from 1 to 3.
    EXPECT_DOUBLE_EQ(scores[2], 0.0)
        << "node 2 should not be on any shortest path when bypass exists";
}

TEST_F(QA_GDB495_Betweenness, AC1_DirectedPathNotReversed) {
    // 1->2->3: directed. There is no path from 3 to 1.
    // Verify the algorithm respects edge direction.
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    // Node 2 is only on the path 1->3. Score = 1.0.
    // If the algorithm incorrectly treats edges as undirected,
    // node 2 would also be on path 3->1, giving score 2.0.
    EXPECT_DOUBLE_EQ(scores[2], 1.0);
}

TEST_F(QA_GDB495_Betweenness, AC1_FourNodeChainExactValues) {
    // 1->2->3->4->5: verify exact unnormalized scores.
    // In a directed path 0..N-1, betweenness(i) = i * (N-1-i).
    // Nodes: 1,2,3,4,5 (mapped to positions 0,1,2,3,4).
    // Node 1 (pos 0): 0*4 = 0
    // Node 2 (pos 1): 1*3 = 3
    // Node 3 (pos 2): 2*2 = 4
    // Node 4 (pos 3): 3*1 = 3
    // Node 5 (pos 4): 4*0 = 0
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
    EXPECT_DOUBLE_EQ(scores[2], 3.0);
    EXPECT_DOUBLE_EQ(scores[3], 4.0);
    EXPECT_DOUBLE_EQ(scores[4], 3.0);
    EXPECT_DOUBLE_EQ(scores[5], 0.0);
}

TEST_F(QA_GDB495_Betweenness, AC1_DirectedFourCycleSymmetry) {
    // 1->2->3->4->1: directed 4-cycle.
    // All nodes are symmetric. Each node lies on exactly the same number
    // of shortest paths as every other node.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 1}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 4u);

    // All nodes should have the same betweenness.
    double expected = scores[1];
    EXPECT_NEAR(scores[2], expected, 1e-10);
    EXPECT_NEAR(scores[3], expected, 1e-10);
    EXPECT_NEAR(scores[4], expected, 1e-10);
}

// ============================================================================
// AC2: Output schema: (node_id INT64, centrality FLOAT64)
// ============================================================================

TEST_F(QA_GDB495_Betweenness, AC2_EveryRowHasTwoColumns) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}});

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value());

    for (size_t i = 0; i < result->size(); ++i) {
        ASSERT_EQ((*result)[i].values.size(), 2u) << "row " << i << " must have 2 columns";
        EXPECT_TRUE(std::holds_alternative<int64_t>((*result)[i].values[0].data()))
            << "row " << i << " column 0 must be int64_t";
        EXPECT_TRUE(std::holds_alternative<double>((*result)[i].values[1].data()))
            << "row " << i << " column 1 must be double";
    }
}

TEST_F(QA_GDB495_Betweenness, AC2_OutputSortedByNodeId) {
    // Use non-sequential node IDs to stress sorting.
    build_graph("knows", {{100, 1}, {1, 50}, {50, 200}, {200, 10}});

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value());
    verify_sorted_by_node_id(*result);
}

TEST_F(QA_GDB495_Betweenness, AC2_EmptyGraphReturnsEmptyRows) {
    build_graph("knows", {});

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

TEST_F(QA_GDB495_Betweenness, AC2_AlgorithmDefSchema) {
    auto def = make_betweenness_centrality_def();
    EXPECT_EQ(def.name, "betweenness");
    ASSERT_EQ(def.output_columns.size(), 2u);
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_FALSE(def.output_columns[0].nullable);
    EXPECT_EQ(def.output_columns[1].name, "centrality");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::FLOAT64);
    EXPECT_FALSE(def.output_columns[1].nullable);
}

// ============================================================================
// AC3: Correct values on known graphs
// ============================================================================

TEST_F(QA_GDB495_Betweenness, AC3_CompleteGraphK5AllZero) {
    // K5: every pair has a direct edge. No intermediate nodes on any
    // shortest path. All betweenness should be zero.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 5; ++i) {
        for (int64_t j = 1; j <= 5; ++j) {
            if (i != j)
                edges.push_back({i, j});
        }
    }
    build_graph("knows", edges);

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 5u);
    for (const auto& [node, score] : scores) {
        EXPECT_DOUBLE_EQ(score, 0.0) << "node " << node << " in K5 should have zero";
    }
}

TEST_F(QA_GDB495_Betweenness, AC3_BidirectionalPairZero) {
    // 1<->2: bidirectional pair. No node is between any other.
    build_graph("knows", {{1, 2}, {2, 1}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
    EXPECT_DOUBLE_EQ(scores[2], 0.0);
}

TEST_F(QA_GDB495_Betweenness, AC3_FiveChainNormalized) {
    // 1->2->3->4->5, n=5.
    // Normalization factor: 1/((5-1)(5-2)) = 1/12.
    // Unnormalized: node 2=3, node 3=4, node 4=3.
    // Normalized: node 2=3/12=0.25, node 3=4/12=1/3, node 4=3/12=0.25.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}});

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    double factor = 1.0 / 12.0;
    EXPECT_NEAR(scores[1], 0.0, 1e-10);
    EXPECT_NEAR(scores[2], 3.0 * factor, 1e-10);
    EXPECT_NEAR(scores[3], 4.0 * factor, 1e-10);
    EXPECT_NEAR(scores[4], 3.0 * factor, 1e-10);
    EXPECT_NEAR(scores[5], 0.0, 1e-10);
}

TEST_F(QA_GDB495_Betweenness, AC3_ThreeDisconnectedTriangles) {
    // Three disconnected directed triangles.
    // No node is between nodes in different components.
    // Within each triangle (directed cycle), all nodes are symmetric.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}, {5, 6}, {6, 4}, {7, 8}, {8, 9}, {9, 7}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 9u);
    verify_scores_non_negative(scores);

    // Within each triangle, all nodes should have the same betweenness.
    EXPECT_NEAR(scores[1], scores[2], 1e-10);
    EXPECT_NEAR(scores[2], scores[3], 1e-10);
    EXPECT_NEAR(scores[4], scores[5], 1e-10);
    EXPECT_NEAR(scores[5], scores[6], 1e-10);
    EXPECT_NEAR(scores[7], scores[8], 1e-10);
    EXPECT_NEAR(scores[8], scores[9], 1e-10);

    // Cross-component symmetry: all triangles are isomorphic.
    EXPECT_NEAR(scores[1], scores[4], 1e-10);
    EXPECT_NEAR(scores[4], scores[7], 1e-10);
}

// ============================================================================
// Graph topology edge cases
// ============================================================================

TEST_F(QA_GDB495_Betweenness, SinkOnlyNodes) {
    // Nodes 3 and 4 are pure sinks (no outgoing edges).
    // 1->2, 1->3, 2->4
    build_graph("knows", {{1, 2}, {1, 3}, {2, 4}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 4u);
    verify_scores_non_negative(scores);

    // Node 2 is on the shortest path 1->4.
    EXPECT_DOUBLE_EQ(scores[2], 1.0);
    // Sink nodes and source have zero betweenness.
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
    EXPECT_DOUBLE_EQ(scores[3], 0.0);
    EXPECT_DOUBLE_EQ(scores[4], 0.0);
}

TEST_F(QA_GDB495_Betweenness, SourceOnlyNodes) {
    // Nodes 1 and 2 are pure sources (no incoming edges).
    // 1->3, 2->3, 3->4
    build_graph("knows", {{1, 3}, {2, 3}, {3, 4}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    verify_scores_non_negative(scores);

    // Node 3 is on paths 1->4 and 2->4.
    EXPECT_DOUBLE_EQ(scores[3], 2.0);
}

TEST_F(QA_GDB495_Betweenness, LadderGraph) {
    // Ladder: 1->2->3 and 4->5->6, with rungs 1->4, 2->5, 3->6.
    // Multiple shortest paths exist between some pairs.
    build_graph("knows", {{1, 2}, {2, 3}, {4, 5}, {5, 6}, {1, 4}, {2, 5}, {3, 6}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 6u);
    verify_scores_non_negative(scores);
    verify_scores_finite(scores);
}

TEST_F(QA_GDB495_Betweenness, LongDiamondChain) {
    // Chain of diamonds: 1->{2,3}->4->{5,6}->7.
    // At each diamond, the two parallel paths split betweenness.
    build_graph("knows", {{1, 2}, {1, 3}, {2, 4}, {3, 4}, {4, 5}, {4, 6}, {5, 7}, {6, 7}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 7u);
    verify_scores_non_negative(scores);
    verify_scores_finite(scores);

    // Nodes 2 and 3 should have equal betweenness (symmetric in first diamond).
    EXPECT_NEAR(scores[2], scores[3], 1e-10);
    // Nodes 5 and 6 should have equal betweenness (symmetric in second diamond).
    EXPECT_NEAR(scores[5], scores[6], 1e-10);
    // Node 4 is the bridge between the two diamonds — should have highest score.
    EXPECT_GT(scores[4], scores[2]);
    EXPECT_GT(scores[4], scores[5]);
}

// ============================================================================
// Normalization boundary values
// ============================================================================

TEST_F(QA_GDB495_Betweenness, NormalizationExactlyTwoNodes) {
    // n=2: normalization guard (n > 2) skips normalization.
    // With normalized=true, scores should be same as unnormalized (both 0).
    build_graph("knows", {{1, 2}});

    auto norm = run_betweenness("knows");
    auto unnorm = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(norm.has_value());
    ASSERT_TRUE(unnorm.has_value());

    auto norm_scores = to_centrality_map(*norm);
    auto unnorm_scores = to_centrality_map(*unnorm);

    EXPECT_DOUBLE_EQ(norm_scores[1], unnorm_scores[1]);
    EXPECT_DOUBLE_EQ(norm_scores[2], unnorm_scores[2]);
}

TEST_F(QA_GDB495_Betweenness, NormalizationExactlyThreeNodes) {
    // n=3: first case where normalization applies.
    // Factor = 1/((3-1)(3-2)) = 1/2 = 0.5.
    build_graph("knows", {{1, 2}, {2, 3}});

    auto norm = run_betweenness("knows");
    auto unnorm = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(norm.has_value());
    ASSERT_TRUE(unnorm.has_value());

    auto norm_scores = to_centrality_map(*norm);
    auto unnorm_scores = to_centrality_map(*unnorm);

    EXPECT_DOUBLE_EQ(unnorm_scores[2], 1.0);
    EXPECT_DOUBLE_EQ(norm_scores[2], 0.5);
}

TEST_F(QA_GDB495_Betweenness, NormalizedScoresInZeroOneRange) {
    // For any directed graph with n >= 3, normalized betweenness
    // should be in [0, 1] range.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}, {1, 3}, {2, 4}, {3, 5}});

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    for (const auto& [node, score] : scores) {
        EXPECT_GE(score, 0.0) << "node " << node << " normalized score should be >= 0";
        EXPECT_LE(score, 1.0) << "node " << node << " normalized score should be <= 1";
    }
}

// ============================================================================
// Error paths
// ============================================================================

TEST_F(QA_GDB495_Betweenness, NonexistentEdgeTypeReturnsNotFound) {
    build_graph("knows", {{1, 2}});

    auto result = run_betweenness("ghost_edge");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB495_Betweenness, FloatNormalizedValueFails) {
    build_graph("knows", {{1, 2}});

    auto result = run_betweenness_raw("knows", {{"normalized", Value(1.5)}});
    ASSERT_FALSE(result.has_value()) << "float value for 'normalized' should fail with type error";
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST_F(QA_GDB495_Betweenness, DefaultNormalizedWhenOmitted) {
    // No named_args at all — should use default (normalized=true).
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run_betweenness_raw("knows", {});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    // Default is normalized=true, n=3: factor = 0.5.
    EXPECT_DOUBLE_EQ(scores[2], 0.5);
}

// ============================================================================
// Multiple edge types
// ============================================================================

TEST_F(QA_GDB495_Betweenness, OnlyUsesSpecifiedEdgeType) {
    // Create two edge types with different graphs.
    // "knows": 1->2->3 (node 2 has betweenness 1.0)
    // "follows": 1->3 (direct edge, no intermediate)
    build_graph("knows", {{1, 2}, {2, 3}});

    auto et2 = engine_.create_edge_type(
        default_database_id, "follows", table_id_, table_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et2.has_value()) << et2.error().message;
    auto link = engine_.link(default_database_id, "follows", pk(1), pk(3));
    ASSERT_TRUE(link.has_value()) << link.error().message;

    // Run betweenness on "knows" — should only see the knows graph.
    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 3u);
    EXPECT_DOUBLE_EQ(scores[2], 1.0);
}

// ============================================================================
// Boundary node IDs
// ============================================================================

TEST_F(QA_GDB495_Betweenness, ZeroNodeId) {
    // Node ID 0 is a valid int64_t value.
    build_graph("knows", {{0, 1}, {1, 2}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 3u);
    EXPECT_DOUBLE_EQ(scores[0], 0.0);
    EXPECT_DOUBLE_EQ(scores[1], 1.0);
    EXPECT_DOUBLE_EQ(scores[2], 0.0);
}

TEST_F(QA_GDB495_Betweenness, ConsecutiveNegativeIds) {
    // Negative IDs in sequence.
    build_graph("knows", {{-3, -2}, {-2, -1}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_DOUBLE_EQ(scores[-3], 0.0);
    EXPECT_DOUBLE_EQ(scores[-2], 1.0);
    EXPECT_DOUBLE_EQ(scores[-1], 0.0);
}

// ============================================================================
// Determinism
// ============================================================================

TEST_F(QA_GDB495_Betweenness, DeterministicAcrossRuns) {
    // Same graph, same parameters — results must be identical.
    build_graph("knows", {{1, 2}, {2, 3}, {1, 3}, {3, 4}, {4, 5}, {2, 4}});

    auto result1 = run_betweenness("knows");
    auto result2 = run_betweenness("knows");
    ASSERT_TRUE(result1.has_value());
    ASSERT_TRUE(result2.has_value());

    ASSERT_EQ(result1->size(), result2->size());
    for (size_t i = 0; i < result1->size(); ++i) {
        auto id1 = std::get<int64_t>((*result1)[i].values[0].data());
        auto id2 = std::get<int64_t>((*result2)[i].values[0].data());
        auto s1 = std::get<double>((*result1)[i].values[1].data());
        auto s2 = std::get<double>((*result2)[i].values[1].data());
        EXPECT_EQ(id1, id2) << "row " << i << " node_id mismatch";
        EXPECT_DOUBLE_EQ(s1, s2) << "row " << i << " score mismatch";
    }
}

// ============================================================================
// Analytical formula verification
// ============================================================================

TEST_F(QA_GDB495_Betweenness, PathGraphFormulaVerification) {
    // For a directed path graph 0->1->2->...->N-1,
    // the unnormalized betweenness of node i is: i * (N-1-i).
    // This is because node i lies on all shortest paths from nodes
    // {0..i-1} to nodes {i+1..N-1}.
    constexpr int64_t N = 10;
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 0; i < N - 1; ++i) {
        edges.push_back({i, i + 1});
    }
    build_graph("knows", edges);

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), static_cast<size_t>(N));

    for (int64_t i = 0; i < N; ++i) {
        double expected = static_cast<double>(i) * static_cast<double>(N - 1 - i);
        EXPECT_DOUBLE_EQ(scores[i], expected) << "node " << i << " in path graph P_" << N;
    }
}

// ============================================================================
// Stress tests
// ============================================================================

TEST_F(QA_GDB495_Betweenness, StressCompleteK30) {
    // K30: complete directed graph. All betweenness should be zero.
    constexpr int64_t N = 30;
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 0; i < N; ++i) {
        for (int64_t j = 0; j < N; ++j) {
            if (i != j)
                edges.push_back({i, j});
        }
    }
    build_graph("knows", edges);

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), static_cast<size_t>(N));
    verify_scores_finite(scores);
    for (const auto& [node, score] : scores) {
        EXPECT_DOUBLE_EQ(score, 0.0) << "node " << node << " in K30";
    }
}

TEST_F(QA_GDB495_Betweenness, StressBidirectionalRing200) {
    // Bidirectional ring of 200 nodes. All scores should be equal by symmetry.
    constexpr int64_t N = 200;
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 0; i < N; ++i) {
        edges.push_back({i, (i + 1) % N});
        edges.push_back({(i + 1) % N, i});
    }
    build_graph("knows", edges);

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), static_cast<size_t>(N));
    verify_scores_finite(scores);
    verify_scores_non_negative(scores);

    // All nodes should have the same betweenness due to symmetry.
    double first = scores[0];
    for (const auto& [node, score] : scores) {
        EXPECT_NEAR(score, first, 1e-8)
            << "node " << node << " in bidirectional ring should match node 0";
    }
}

TEST_F(QA_GDB495_Betweenness, StressStarWithBackEdges) {
    // Hub node 0 -> {1..50}, plus back edges {1..50} -> 0.
    // This creates a bidirectional star.
    constexpr int64_t N = 50;
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= N; ++i) {
        edges.push_back({0, i});
        edges.push_back({i, 0});
    }
    build_graph("knows", edges);

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    verify_scores_finite(scores);
    verify_scores_non_negative(scores);

    // Hub node 0 should have the highest betweenness.
    // It's on the shortest path between any pair of leaf nodes (i -> 0 -> j).
    for (int64_t i = 1; i <= N; ++i) {
        EXPECT_GT(scores[0], scores[i]) << "hub node 0 should outrank leaf " << i;
    }

    // All leaf nodes should have equal betweenness (symmetric).
    for (int64_t i = 2; i <= N; ++i) {
        EXPECT_NEAR(scores[1], scores[i], 1e-10) << "leaf " << i << " should match leaf 1";
    }
}

TEST_F(QA_GDB495_Betweenness, StressPathGraph50WithNormalization) {
    // Path graph with 50 nodes, normalized.
    // Verify all normalized scores are in [0, 1].
    constexpr int64_t N = 50;
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 0; i < N - 1; ++i) {
        edges.push_back({i, i + 1});
    }
    build_graph("knows", edges);

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), static_cast<size_t>(N));
    verify_scores_finite(scores);

    for (const auto& [node, score] : scores) {
        EXPECT_GE(score, -1e-10) << "node " << node;
        EXPECT_LE(score, 1.0 + 1e-10) << "node " << node;
    }

    // Endpoints should have zero.
    EXPECT_DOUBLE_EQ(scores[0], 0.0);
    EXPECT_DOUBLE_EQ(scores[N - 1], 0.0);
}

} // namespace
} // namespace sixseven
