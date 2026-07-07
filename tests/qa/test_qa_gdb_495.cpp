/// @file test_qa_gdb_495.cpp
/// QA adversarial tests for GDB-495: Betweenness Centrality (Brandes' algorithm).
///
/// GDB-495 is the canonical ticket for Betweenness Centrality QA coverage.
/// GDB-489 originally tracked overlapping coverage for the same feature and
/// redirects to GDB-495; per GDB-958 (consolidation), this file absorbs every
/// scenario from GDB-489's test_qa_gdb_489.cpp that was not already a
/// behavioral duplicate of a GDB-495 test. GDB-489's true duplicates were
/// dropped; its genuinely unique cases were ported below (renamed to fit this
/// suite) so no coverage is lost. test_qa_gdb_489.cpp has been deleted.
///
/// Verifies:
///   AC1: Brandes' algorithm implementation.
///   AC2: Output schema: (node_id INT64, centrality FLOAT64).
///   AC3: Correct values on known graphs.
///   AC4: Unit tests passing / registration.
///
/// Adversarial categories: graph topology edge cases, parallel paths,
/// duplicate edges, self-loops, boundary node IDs, normalization boundaries,
/// analytical formula verification, multiple edge types, stress tests,
/// error paths, numerical stability.

#include "sixseven/common/types.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/betweenness_centrality.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "betweenness_qa_helpers.h"

namespace sixseven {
namespace {

using betweenness_qa::BetweennessQaFixtureBase;
using betweenness_qa::to_centrality_map;
using betweenness_qa::verify_scores_finite;
using betweenness_qa::verify_scores_non_negative;
using betweenness_qa::verify_sorted_by_node_id;

// ============================================================================
// Test fixture
// ============================================================================

class QA_GDB495_Betweenness : public BetweennessQaFixtureBase {};

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

TEST_F(QA_GDB495_Betweenness, AC1_LinearChainUnnormalized) {
    // Ported from GDB-489. 1 -> 2 -> 3: node 2 is on the only shortest path
    // from 1 to 3.
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
    EXPECT_DOUBLE_EQ(scores[2], 1.0);
    EXPECT_DOUBLE_EQ(scores[3], 0.0);
}

TEST_F(QA_GDB495_Betweenness, AC1_DiamondSplitsBetweenness) {
    // Ported from GDB-489. Diamond: 1->2, 1->3, 2->4, 3->4.
    // Two shortest paths from 1 to 4. Nodes 2 and 3 each get 0.5.
    build_graph("knows", {{1, 2}, {1, 3}, {2, 4}, {3, 4}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
    EXPECT_NEAR(scores[2], 0.5, 1e-10);
    EXPECT_NEAR(scores[3], 0.5, 1e-10);
    EXPECT_DOUBLE_EQ(scores[4], 0.0);
}

TEST_F(QA_GDB495_Betweenness, AC1_DirectedThreeCycleSymmetric) {
    // Ported from GDB-489 (AC1_DirectedCycleSymmetric).
    // 1->2->3->1: directed 3-cycle. All nodes symmetric.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 3u);
    // All nodes should have the same betweenness due to symmetry.
    EXPECT_NEAR(scores[1], scores[2], 1e-10);
    EXPECT_NEAR(scores[2], scores[3], 1e-10);
}

TEST_F(QA_GDB495_Betweenness, AC1_LongerChainScores) {
    // Ported from GDB-489. 1->2->3->4: inner nodes 2 and 3 each lie on 2
    // shortest paths.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
    EXPECT_DOUBLE_EQ(scores[2], 2.0);
    EXPECT_DOUBLE_EQ(scores[3], 2.0);
    EXPECT_DOUBLE_EQ(scores[4], 0.0);
}

TEST_F(QA_GDB495_Betweenness, AC1_BridgeNodeHighest) {
    // Ported from GDB-489. Two clusters connected by bridge node 3.
    // 1->3, 2->3, 3->4, 3->5
    build_graph("knows", {{1, 3}, {2, 3}, {3, 4}, {3, 5}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    // Node 3 should have the highest betweenness.
    for (int64_t node : {1, 2, 4, 5}) {
        EXPECT_GT(scores[3], scores[node]) << "bridge node 3 should outrank node " << node;
    }
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

TEST_F(QA_GDB495_Betweenness, AC2_OutputRowStructure) {
    // Ported from GDB-489. Explicit per-row variant-type check, distinct from
    // AC2_EveryRowHasTwoColumns's loop-with-index style.
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value());

    for (const auto& row : *result) {
        ASSERT_EQ(row.values.size(), 2u) << "each row must have exactly 2 columns";
        EXPECT_TRUE(std::holds_alternative<int64_t>(row.values[0].data()));
        EXPECT_TRUE(std::holds_alternative<double>(row.values[1].data()));
    }
}

TEST_F(QA_GDB495_Betweenness, AC2_AlgorithmName) {
    // Ported from GDB-489.
    auto def = make_betweenness_centrality_def();
    EXPECT_EQ(def.name, "betweenness");
}

TEST_F(QA_GDB495_Betweenness, AC2_ParameterDefinition) {
    // Ported from GDB-489.
    auto def = make_betweenness_centrality_def();
    ASSERT_EQ(def.params.size(), 1u);
    EXPECT_EQ(def.params[0].name, "normalized");
    EXPECT_EQ(def.params[0].type_id, TypeId::BOOL);
    EXPECT_FALSE(def.params[0].required);
    ASSERT_TRUE(def.params[0].default_value.has_value());
    EXPECT_EQ(std::get<bool>(def.params[0].default_value->data()), true);
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

TEST_F(QA_GDB495_Betweenness, AC3_SingleEdgeZeroCentrality) {
    // Ported from GDB-489. With only 2 nodes, no node can be between any
    // pair.
    build_graph("knows", {{1, 2}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
    EXPECT_DOUBLE_EQ(scores[2], 0.0);
}

TEST_F(QA_GDB495_Betweenness, AC3_StarGraphAllZero) {
    // Ported from GDB-489. Hub 1 -> {2,3,4,5}: no intermediate nodes on any
    // shortest path.
    build_graph("knows", {{1, 2}, {1, 3}, {1, 4}, {1, 5}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    for (const auto& [node, score] : scores) {
        EXPECT_DOUBLE_EQ(score, 0.0) << "node " << node << " in star should have zero";
    }
}

TEST_F(QA_GDB495_Betweenness, AC3_DisconnectedComponentsAllZero) {
    // Ported from GDB-489. Two disconnected pairs: no node is between any
    // other pair.
    build_graph("knows", {{1, 2}, {3, 4}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 4u);
    for (const auto& [node, score] : scores) {
        EXPECT_DOUBLE_EQ(score, 0.0) << "node " << node << " in disconnected graph";
    }
}

TEST_F(QA_GDB495_Betweenness, AC3_BidirectionalChain) {
    // Ported from GDB-489. 1<->2<->3 (bidirectional).
    build_graph("knows", {{1, 2}, {2, 1}, {2, 3}, {3, 2}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 3u);
    verify_scores_non_negative(scores);

    // Node 2 is between (1,3) and (3,1). Endpoints should be symmetric.
    EXPECT_GT(scores[2], scores[1]);
    EXPECT_GT(scores[2], scores[3]);
    EXPECT_NEAR(scores[1], scores[3], 1e-10);
}

// ============================================================================
// AC4: Registration
// ============================================================================

TEST(QA_GDB495_Def, Registration) {
    // Ported from GDB-489.
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_betweenness_centrality_def(),
                                              betweenness_centrality_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto* entry = registry.find("betweenness");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "betweenness");
}

TEST(QA_GDB495_Def, CaseInsensitiveLookup) {
    // Ported from GDB-489.
    AlgorithmRegistry registry;
    (void)registry.register_algorithm(make_betweenness_centrality_def(),
                                      betweenness_centrality_execute);

    EXPECT_NE(registry.find("BETWEENNESS"), nullptr);
    EXPECT_NE(registry.find("Betweenness"), nullptr);
    EXPECT_NE(registry.find("betweenness"), nullptr);
    EXPECT_NE(registry.find("bEtWeEnNeSs"), nullptr);
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

TEST_F(QA_GDB495_Betweenness, SelfLoop) {
    // Ported from GDB-489. Self-loop should not inflate betweenness of the
    // self-looping node. Graph: 1->1, 1->2, 2->3.
    build_graph("knows", {{1, 1}, {1, 2}, {2, 3}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 3u);
    verify_scores_non_negative(scores);
    verify_scores_finite(scores);

    // Compare with the same graph without the self-loop.
    // Node 2 should still have centrality = 1.0 (on path 1->3).
    EXPECT_DOUBLE_EQ(scores[2], 1.0);
    // Self-loop on node 1 should NOT give it betweenness.
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
}

TEST_F(QA_GDB495_Betweenness, PureSelfLoop) {
    // Ported from GDB-489. Single node with a self-loop only.
    build_graph("knows", {{1, 1}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 1u);
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
}

TEST_F(QA_GDB495_Betweenness, MultipleSelfLoops) {
    // Ported from GDB-489. All nodes have self-loops.
    build_graph("knows", {{1, 1}, {2, 2}, {3, 3}, {1, 2}, {2, 3}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    verify_scores_non_negative(scores);
    verify_scores_finite(scores);

    // Self-loops should not affect betweenness. Node 2 is on path 1->3.
    EXPECT_DOUBLE_EQ(scores[2], 1.0);
}

TEST_F(QA_GDB495_Betweenness, DuplicateEdges) {
    // Ported from GDB-489. Multiple identical edges 1->2 should not inflate
    // centrality. Graph: 1->2 (x3), 2->3.
    build_graph("knows", {{1, 2}, {1, 2}, {1, 2}, {2, 3}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    verify_scores_non_negative(scores);
    verify_scores_finite(scores);

    // Node 2 should have centrality = 1.0 regardless of duplicate edges.
    // If duplicates inflate sigma, this will fail.
    EXPECT_NEAR(scores[2], 1.0, 1e-10)
        << "duplicate edges should not inflate betweenness centrality";
}

TEST_F(QA_GDB495_Betweenness, DuplicateEdgesOnDiamond) {
    // Ported from GDB-489. Diamond with duplicate edges on one path.
    // 1->2 (x3), 1->3, 2->4, 3->4
    build_graph("knows", {{1, 2}, {1, 2}, {1, 2}, {1, 3}, {2, 4}, {3, 4}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    verify_scores_non_negative(scores);
    verify_scores_finite(scores);

    // In a correct implementation, duplicate edges should not create
    // additional shortest paths. Nodes 2 and 3 should still split evenly.
    EXPECT_NEAR(scores[2], scores[3], 0.1)
        << "duplicate edges should not skew betweenness between parallel paths";
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

TEST_F(QA_GDB495_Betweenness, NormalizationSkippedForOneNode) {
    // Ported from GDB-489. Edge: 1->1 (self-loop creates 1 node).
    // Normalization should be skipped.
    build_graph("knows", {{1, 1}});

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    // n=1, normalization skipped, centrality=0 for single node.
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
}

TEST_F(QA_GDB495_Betweenness, NormalizationForLargeN) {
    // Ported from GDB-489. 1->2->3->4->5->6: chain of 6.
    // Normalization factor: 1/((6-1)(6-2)) = 1/20.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 6}});

    auto unnorm = run_betweenness_unnormalized("knows");
    auto norm = run_betweenness("knows");
    ASSERT_TRUE(unnorm.has_value());
    ASSERT_TRUE(norm.has_value());

    auto unnorm_scores = to_centrality_map(*unnorm);
    auto norm_scores = to_centrality_map(*norm);

    double factor = 1.0 / (5.0 * 4.0); // 1/((n-1)(n-2)) = 1/20
    for (const auto& [node, unnorm_score] : unnorm_scores) {
        EXPECT_NEAR(norm_scores[node], unnorm_score * factor, 1e-10)
            << "normalized score for node " << node << " should be unnormalized * 1/20";
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

TEST_F(QA_GDB495_Betweenness, StringNormalizedValueFails) {
    // Ported from GDB-489.
    build_graph("knows", {{1, 2}});

    auto result = run_betweenness_raw("knows", {{"normalized", Value(std::string("true"))}});
    ASSERT_FALSE(result.has_value()) << "string value for 'normalized' should fail with type error";
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST_F(QA_GDB495_Betweenness, NullNormalizedValueFails) {
    // Ported from GDB-489.
    build_graph("knows", {{1, 2}});

    auto result = run_betweenness_raw("knows", {{"normalized", Value()}});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB495_Betweenness, IntegerNormalizedCoercion) {
    // Ported from GDB-489. Pass integer 1 for normalized (should coerce to
    // true).
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run_betweenness_raw("knows", {{"normalized", Value(static_cast<int64_t>(1))}});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    // Should behave like normalized=true.
    EXPECT_DOUBLE_EQ(scores[2], 0.5);
}

TEST_F(QA_GDB495_Betweenness, IntegerZeroNormalizedCoercion) {
    // Ported from GDB-489. Pass integer 0 for normalized (should coerce to
    // false).
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run_betweenness_raw("knows", {{"normalized", Value(static_cast<int64_t>(0))}});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    // Should behave like normalized=false.
    EXPECT_DOUBLE_EQ(scores[2], 1.0);
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
    auto link =
        engine_.link(default_database_id, "follows", betweenness_qa::pk(1), betweenness_qa::pk(3));
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

TEST_F(QA_GDB495_Betweenness, LargeNodeIds) {
    // Ported from GDB-489. Use large (but valid) int64_t node IDs.
    constexpr int64_t A = 1000000000LL;
    constexpr int64_t B = 2000000000LL;
    constexpr int64_t C = 3000000000LL;
    build_graph("knows", {{A, B}, {B, C}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 3u);
    verify_scores_non_negative(scores);

    // Node B is on the shortest path A->C.
    EXPECT_DOUBLE_EQ(scores[A], 0.0);
    EXPECT_DOUBLE_EQ(scores[B], 1.0);
    EXPECT_DOUBLE_EQ(scores[C], 0.0);
}

TEST_F(QA_GDB495_Betweenness, NegativeNodeIds) {
    // Ported from GDB-489. Negative node IDs should work correctly.
    build_graph("knows", {{-10, -5}, {-5, 0}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 3u);
    EXPECT_DOUBLE_EQ(scores[-10], 0.0);
    EXPECT_DOUBLE_EQ(scores[-5], 1.0);
    EXPECT_DOUBLE_EQ(scores[0], 0.0);
}

TEST_F(QA_GDB495_Betweenness, MixedPositiveNegativeNodeIds) {
    // Ported from GDB-489.
    build_graph("knows", {{-1, 0}, {0, 1}});

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value());
    verify_sorted_by_node_id(*result);

    auto scores = to_centrality_map(*result);
    EXPECT_DOUBLE_EQ(scores[0], 0.5); // normalized: 1 / (2*1) = 0.5
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
// Numerical stability
// ============================================================================

TEST_F(QA_GDB495_Betweenness, LargeSparseGraphFiniteScores) {
    // Ported from GDB-489. Grid-like structure: chain of 100 nodes with
    // shortcuts every 10. This creates many alternative shortest paths but
    // shouldn't overflow.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 0; i < 99; ++i) {
        edges.push_back({i, i + 1});
    }
    // Add shortcuts.
    for (int64_t i = 0; i < 90; i += 10) {
        edges.push_back({i, i + 10});
    }
    build_graph("knows", edges);

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 100u);
    verify_scores_finite(scores);
    verify_scores_non_negative(scores);
}

TEST_F(QA_GDB495_Betweenness, HighDegreeNodeFiniteScores) {
    // Ported from GDB-489. Hub node 0 connects to 200 leaf nodes. Each leaf
    // also connects to the next. This creates many paths through the hub.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 200; ++i) {
        edges.push_back({0, i});
    }
    for (int64_t i = 1; i < 200; ++i) {
        edges.push_back({i, i + 1});
    }
    build_graph("knows", edges);

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    verify_scores_finite(scores);
    verify_scores_non_negative(scores);
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

TEST_F(QA_GDB495_Betweenness, StressRingGraph300) {
    // Ported from GDB-489 (StressRingGraph). Directed ring of 300 nodes.
    // All scores should be symmetric (equal). Distinct from
    // StressBidirectionalRing200 above (directed vs bidirectional, different
    // size).
    constexpr int64_t N = 300;
    std::vector<std::pair<int64_t, int64_t>> edges;
    edges.reserve(N);
    for (int64_t i = 0; i < N; ++i) {
        edges.push_back({i, (i + 1) % N});
    }
    build_graph("knows", edges);

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), static_cast<size_t>(N));
    verify_scores_finite(scores);
    verify_scores_non_negative(scores);

    // All nodes in a directed ring should have the same betweenness.
    double first_score = scores[0];
    for (const auto& [node, score] : scores) {
        EXPECT_NEAR(score, first_score, 1e-10) << "node " << node << " in ring should match node 0";
    }
}

TEST_F(QA_GDB495_Betweenness, StressLinearChain100) {
    // Ported from GDB-489. Linear chain of 100 nodes.
    constexpr int64_t N = 100;
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 0; i < N - 1; ++i) {
        edges.push_back({i, i + 1});
    }
    build_graph("knows", edges);

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), static_cast<size_t>(N));
    verify_scores_finite(scores);
    verify_scores_non_negative(scores);

    // Endpoints should have zero betweenness.
    EXPECT_DOUBLE_EQ(scores[0], 0.0);
    EXPECT_DOUBLE_EQ(scores[N - 1], 0.0);

    // Middle node should have the highest betweenness in a chain.
    // (symmetric distribution: scores increase toward center)
    for (int64_t i = 1; i < N / 2; ++i) {
        EXPECT_LE(scores[i - 1], scores[i] + 1e-10)
            << "scores should increase toward center: node " << i;
    }
}

TEST_F(QA_GDB495_Betweenness, StressBinaryTree) {
    // Ported from GDB-489. Full binary tree with depth 6 (63 nodes), directed
    // downward only. Root->children: 1->2, 1->3, 2->4, 2->5, 3->6, 3->7, etc.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 31; ++i) {
        edges.push_back({i, 2 * i});
        edges.push_back({i, 2 * i + 1});
    }
    build_graph("knows", edges);

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    verify_scores_finite(scores);
    verify_scores_non_negative(scores);

    // In a directed tree with only downward edges:
    // - Root (1) is only a source, never an intermediate => betweenness = 0.
    // - Leaf nodes (32-63) are only destinations => betweenness = 0.
    // - Inner nodes (2-31) are intermediates on paths from ancestors to descendants.
    EXPECT_DOUBLE_EQ(scores[1], 0.0) << "root has zero betweenness in directed tree";

    for (int64_t leaf = 32; leaf <= 63; ++leaf) {
        EXPECT_DOUBLE_EQ(scores[leaf], 0.0)
            << "leaf " << leaf << " has zero betweenness in directed tree";
    }

    // Inner nodes at shallower depths should have higher betweenness
    // (they're on more paths). Node 2 and 3 (depth 1) should outrank
    // nodes at depth 2+ because they mediate more ancestor-to-descendant paths.
    for (int64_t inner = 4; inner <= 31; ++inner) {
        EXPECT_GE(scores[2], scores[inner] - 1e-10)
            << "depth-1 node 2 should have >= betweenness than node " << inner;
    }
}

TEST_F(QA_GDB495_Betweenness, StressManyDisconnectedPairs) {
    // Ported from GDB-489. 100 disconnected pairs: {(1,2), (3,4), (5,6), ...}.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 0; i < 200; i += 2) {
        edges.push_back({i, i + 1});
    }
    build_graph("knows", edges);

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 200u);
    verify_scores_finite(scores);

    // All scores should be zero (no intermediate nodes).
    for (const auto& [node, score] : scores) {
        EXPECT_DOUBLE_EQ(score, 0.0) << "node " << node;
    }
}

} // namespace
} // namespace sixseven
