/// @file test_qa_gdb_489.cpp
/// QA adversarial tests for GDB-489: Betweenness Centrality (Brandes' algorithm).
///
/// Verifies:
///   AC1: Brandes' algorithm implementation.
///   AC2: Output schema: (node_id INT64, centrality FLOAT64).
///   AC3: Correct values on known graphs.
///   AC4: Unit tests passing.
///
/// Adversarial categories: boundary values, error paths, edge cases, stress,
/// graph topology corner cases, numerical stability.

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

class QA_GDB489_Betweenness : public ::testing::Test {
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

    /// Run betweenness centrality with raw named_args (for type edge cases).
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

TEST_F(QA_GDB489_Betweenness, AC1_LinearChainUnnormalized) {
    // 1 -> 2 -> 3: node 2 is on the only shortest path from 1 to 3.
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
    EXPECT_DOUBLE_EQ(scores[2], 1.0);
    EXPECT_DOUBLE_EQ(scores[3], 0.0);
}

TEST_F(QA_GDB489_Betweenness, AC1_DiamondSplitsBetweenness) {
    // Diamond: 1->2, 1->3, 2->4, 3->4.
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

TEST_F(QA_GDB489_Betweenness, AC1_DirectedCycleSymmetric) {
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

TEST_F(QA_GDB489_Betweenness, AC1_LongerChainScores) {
    // 1->2->3->4: inner nodes 2 and 3 each lie on 2 shortest paths.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
    EXPECT_DOUBLE_EQ(scores[2], 2.0);
    EXPECT_DOUBLE_EQ(scores[3], 2.0);
    EXPECT_DOUBLE_EQ(scores[4], 0.0);
}

TEST_F(QA_GDB489_Betweenness, AC1_BridgeNodeHighest) {
    // Two clusters connected by bridge node 3.
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

TEST_F(QA_GDB489_Betweenness, AC1_DeterministicOutput) {
    // Same input should produce exact same output on repeated runs.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {1, 3}});

    auto result1 = run_betweenness("knows");
    ASSERT_TRUE(result1.has_value());
    auto result2 = run_betweenness("knows");
    ASSERT_TRUE(result2.has_value());

    ASSERT_EQ(result1->size(), result2->size());
    for (size_t i = 0; i < result1->size(); ++i) {
        auto score1 = std::get<double>((*result1)[i].values[1].data());
        auto score2 = std::get<double>((*result2)[i].values[1].data());
        EXPECT_DOUBLE_EQ(score1, score2) << "row " << i << " should be deterministic";
    }
}

TEST_F(QA_GDB489_Betweenness, AC1_NormalizationCorrect) {
    // 1->2->3: unnormalized score[2] = 1.0.
    // Normalized by 1/((n-1)(n-2)) = 1/(2*1) = 0.5 for n=3 directed.
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_DOUBLE_EQ(scores[2], 0.5);
}

TEST_F(QA_GDB489_Betweenness, AC1_NormalizationSkippedForTwoNodes) {
    // n=2: normalization guard (n > 2) should skip normalization.
    build_graph("knows", {{1, 2}});

    auto norm_result = run_betweenness("knows");
    auto unnorm_result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(norm_result.has_value());
    ASSERT_TRUE(unnorm_result.has_value());

    auto norm_scores = to_centrality_map(*norm_result);
    auto unnorm_scores = to_centrality_map(*unnorm_result);

    // With n=2, normalized and unnormalized should be the same (both 0).
    EXPECT_DOUBLE_EQ(norm_scores[1], unnorm_scores[1]);
    EXPECT_DOUBLE_EQ(norm_scores[2], unnorm_scores[2]);
}

// ============================================================================
// AC2: Output schema: (node_id INT64, centrality FLOAT64)
// ============================================================================

TEST_F(QA_GDB489_Betweenness, AC2_OutputSchemaDefinition) {
    auto def = make_betweenness_centrality_def();
    ASSERT_EQ(def.output_columns.size(), 2u);

    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_FALSE(def.output_columns[0].nullable);

    EXPECT_EQ(def.output_columns[1].name, "centrality");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::FLOAT64);
    EXPECT_FALSE(def.output_columns[1].nullable);
}

TEST_F(QA_GDB489_Betweenness, AC2_OutputRowStructure) {
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value());

    for (const auto& row : *result) {
        ASSERT_EQ(row.values.size(), 2u) << "each row must have exactly 2 columns";
        EXPECT_TRUE(std::holds_alternative<int64_t>(row.values[0].data()));
        EXPECT_TRUE(std::holds_alternative<double>(row.values[1].data()));
    }
}

TEST_F(QA_GDB489_Betweenness, AC2_OutputSortedByNodeId) {
    build_graph("knows", {{5, 1}, {3, 7}, {7, 5}, {1, 3}});

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value());
    verify_sorted_by_node_id(*result);
}

TEST_F(QA_GDB489_Betweenness, AC2_AlgorithmName) {
    auto def = make_betweenness_centrality_def();
    EXPECT_EQ(def.name, "betweenness");
}

TEST_F(QA_GDB489_Betweenness, AC2_ParameterDefinition) {
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

TEST_F(QA_GDB489_Betweenness, AC3_EmptyGraph) {
    build_graph("knows", {});

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(QA_GDB489_Betweenness, AC3_SingleEdgeZeroCentrality) {
    // With only 2 nodes, no node can be between any pair.
    build_graph("knows", {{1, 2}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
    EXPECT_DOUBLE_EQ(scores[2], 0.0);
}

TEST_F(QA_GDB489_Betweenness, AC3_StarGraphAllZero) {
    // Hub 1 -> {2,3,4,5}: no intermediate nodes on any shortest path.
    build_graph("knows", {{1, 2}, {1, 3}, {1, 4}, {1, 5}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    for (const auto& [node, score] : scores) {
        EXPECT_DOUBLE_EQ(score, 0.0) << "node " << node << " in star should have zero";
    }
}

TEST_F(QA_GDB489_Betweenness, AC3_DisconnectedComponentsAllZero) {
    // Two disconnected pairs: no node is between any other pair.
    build_graph("knows", {{1, 2}, {3, 4}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 4u);
    for (const auto& [node, score] : scores) {
        EXPECT_DOUBLE_EQ(score, 0.0) << "node " << node << " in disconnected graph";
    }
}

TEST_F(QA_GDB489_Betweenness, AC3_FiveNodeChainScores) {
    // 1->2->3->4->5: verify exact unnormalized scores.
    // Node 2: on paths 1->3, 1->4, 1->5 = 3 paths
    // Node 3: on paths 1->4, 1->5, 2->4, 2->5 = 4 paths
    // Node 4: on paths 1->5, 2->5, 3->5 = 3 paths
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

TEST_F(QA_GDB489_Betweenness, AC3_BidirectionalChain) {
    // 1<->2<->3 (bidirectional).
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

TEST_F(QA_GDB489_Betweenness, AC3_CompleteFourNodeGraph) {
    // K4 (complete directed graph on 4 nodes).
    // Every node pair has a direct edge, so no intermediary.
    // All betweenness should be zero.
    build_graph("knows",
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

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 4u);
    for (const auto& [node, score] : scores) {
        EXPECT_DOUBLE_EQ(score, 0.0)
            << "node " << node << " in complete graph should have zero betweenness";
    }
}

// ============================================================================
// AC4: Registration
// ============================================================================

TEST(QA_GDB489_Def, Registration) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_betweenness_centrality_def(),
                                              betweenness_centrality_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto* entry = registry.find("betweenness");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "betweenness");
}

TEST(QA_GDB489_Def, CaseInsensitiveLookup) {
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

TEST_F(QA_GDB489_Betweenness, SelfLoop) {
    // Self-loop should not inflate betweenness of the self-looping node.
    // Graph: 1->1, 1->2, 2->3.
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

TEST_F(QA_GDB489_Betweenness, PureSelfLoop) {
    // Single node with a self-loop only.
    build_graph("knows", {{1, 1}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 1u);
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
}

TEST_F(QA_GDB489_Betweenness, MultipleSelfLoops) {
    // All nodes have self-loops.
    build_graph("knows", {{1, 1}, {2, 2}, {3, 3}, {1, 2}, {2, 3}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    verify_scores_non_negative(scores);
    verify_scores_finite(scores);

    // Self-loops should not affect betweenness. Node 2 is on path 1->3.
    EXPECT_DOUBLE_EQ(scores[2], 1.0);
}

TEST_F(QA_GDB489_Betweenness, DuplicateEdges) {
    // Multiple identical edges 1->2 should not inflate centrality.
    // Graph: 1->2 (x3), 2->3.
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

TEST_F(QA_GDB489_Betweenness, DuplicateEdgesOnDiamond) {
    // Diamond with duplicate edges on one path.
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

TEST_F(QA_GDB489_Betweenness, LargeNodeIds) {
    // Use large (but valid) int64_t node IDs.
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

TEST_F(QA_GDB489_Betweenness, NegativeNodeIds) {
    // Negative node IDs should work correctly.
    build_graph("knows", {{-10, -5}, {-5, 0}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 3u);
    EXPECT_DOUBLE_EQ(scores[-10], 0.0);
    EXPECT_DOUBLE_EQ(scores[-5], 1.0);
    EXPECT_DOUBLE_EQ(scores[0], 0.0);
}

TEST_F(QA_GDB489_Betweenness, MixedPositiveNegativeNodeIds) {
    build_graph("knows", {{-1, 0}, {0, 1}});

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value());
    verify_sorted_by_node_id(*result);

    auto scores = to_centrality_map(*result);
    EXPECT_DOUBLE_EQ(scores[0], 0.5); // normalized: 1 / (2*1) = 0.5
}

// ============================================================================
// Normalization boundary values
// ============================================================================

TEST_F(QA_GDB489_Betweenness, NormalizationSkippedForOneNode) {
    // Edge: 1->1 (self-loop creates 1 node). Normalization should be skipped.
    build_graph("knows", {{1, 1}});

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    // n=1, normalization skipped, centrality=0 for single node.
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
}

TEST_F(QA_GDB489_Betweenness, NormalizationForLargeN) {
    // 1->2->3->4->5->6: chain of 6.
    // Unnormalized: node 3 is on paths from {1,2} to {4,5,6} = 6 paths
    //               node 4 is on paths from {1,2,3} to {5,6} = 6 paths
    // (Actually: node 2 on 4 paths, node 3 on 6, node 4 on 6, node 5 on 4)
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

TEST_F(QA_GDB489_Betweenness, NonexistentEdgeType) {
    build_graph("knows", {{1, 2}});

    auto result = run_betweenness("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB489_Betweenness, StringNormalizedValueFails) {
    build_graph("knows", {{1, 2}});

    auto result = run_betweenness_raw("knows", {{"normalized", Value(std::string("true"))}});
    ASSERT_FALSE(result.has_value()) << "string value for 'normalized' should fail with type error";
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST_F(QA_GDB489_Betweenness, NullNormalizedValueFails) {
    build_graph("knows", {{1, 2}});

    auto result = run_betweenness_raw("knows", {{"normalized", Value()}});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB489_Betweenness, DefaultParameterWhenOmitted) {
    // Pass no named args — should use default (normalized=true).
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run_betweenness_raw("knows", {});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 3u);

    // Default is normalized=true, n=3: factor = 1/((3-1)(3-2)) = 0.5
    EXPECT_DOUBLE_EQ(scores[2], 0.5);
}

TEST_F(QA_GDB489_Betweenness, IntegerNormalizedCoercion) {
    // Pass integer 1 for normalized (should coerce to true).
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run_betweenness_raw("knows", {{"normalized", Value(static_cast<int64_t>(1))}});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    // Should behave like normalized=true.
    EXPECT_DOUBLE_EQ(scores[2], 0.5);
}

TEST_F(QA_GDB489_Betweenness, IntegerZeroNormalizedCoercion) {
    // Pass integer 0 for normalized (should coerce to false).
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run_betweenness_raw("knows", {{"normalized", Value(static_cast<int64_t>(0))}});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    // Should behave like normalized=false.
    EXPECT_DOUBLE_EQ(scores[2], 1.0);
}

// ============================================================================
// Numerical stability
// ============================================================================

TEST_F(QA_GDB489_Betweenness, DenseGraphNoNaNOrInf) {
    // Complete graph K_20: every node links to every other.
    // Sigma values grow exponentially; verify no overflow to NaN/Inf.
    constexpr int64_t N = 20;
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 0; i < N; ++i) {
        for (int64_t j = 0; j < N; ++j) {
            if (i != j) {
                edges.push_back({i, j});
            }
        }
    }
    build_graph("knows", edges);

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), static_cast<size_t>(N));
    verify_scores_finite(scores);
    verify_scores_non_negative(scores);

    // Complete graph: all betweenness should be zero (direct edges everywhere).
    for (const auto& [node, score] : scores) {
        EXPECT_DOUBLE_EQ(score, 0.0) << "node " << node << " in K_20 should have zero";
    }
}

TEST_F(QA_GDB489_Betweenness, LargeSparseGraphFiniteScores) {
    // Grid-like structure: chain of 100 nodes with shortcuts every 10.
    // This creates many alternative shortest paths but shouldn't overflow.
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

TEST_F(QA_GDB489_Betweenness, HighDegreeNodeFiniteScores) {
    // Hub node 0 connects to 200 leaf nodes. Each leaf also connects to the next.
    // This creates many paths through the hub.
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

TEST_F(QA_GDB489_Betweenness, StressRingGraph) {
    // Ring of 300 nodes. All scores should be symmetric (equal).
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

TEST_F(QA_GDB489_Betweenness, StressLinearChain100) {
    // Linear chain of 100 nodes.
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

TEST_F(QA_GDB489_Betweenness, StressBinaryTree) {
    // Full binary tree with depth 6 (63 nodes), directed downward only.
    // Root->children: 1->2, 1->3, 2->4, 2->5, 3->6, 3->7, etc.
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

TEST_F(QA_GDB489_Betweenness, StressManyDisconnectedPairs) {
    // 100 disconnected pairs: {(1,2), (3,4), (5,6), ...}.
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
