/// @file test_qa_gdb_486.cpp
/// QA adversarial tests for GDB-486: Community Detection (Louvain) algorithm.
///
/// Verifies the implementation and adversarially tests:
///   AC1: Louvain modularity optimization
///   AC2: Output schema: (node_id, community_id)
///   AC3: Communities correctly identified on known graphs
///   AC4: Unit tests passing
///
/// Adversarial categories: edge cases, boundary values, parameter validation,
/// error paths, stress tests, graph topology extremes.

#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/louvain.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

/// Extract (node_id, community_id) pairs from algorithm result rows.
std::unordered_map<int64_t, int64_t> to_community_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, int64_t> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 2u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto community_id = std::get<int64_t>(row.values[1].data());
        result[node_id] = community_id;
    }
    return result;
}

/// Verify that community IDs are contiguous starting from 0.
void verify_contiguous_ids(const std::unordered_map<int64_t, int64_t>& communities) {
    if (communities.empty())
        return;
    std::unordered_set<int64_t> unique_ids;
    for (const auto& [_, comm] : communities) {
        unique_ids.insert(comm);
    }
    int64_t max_id = -1;
    for (int64_t id : unique_ids) {
        EXPECT_GE(id, 0) << "community IDs should be non-negative";
        max_id = std::max(max_id, id);
    }
    EXPECT_EQ(max_id, static_cast<int64_t>(unique_ids.size()) - 1)
        << "community IDs should be contiguous from 0";
}

/// Verify that nodes in the same group share a community ID, and nodes in
/// different groups have different community IDs.
void verify_communities(const std::unordered_map<int64_t, int64_t>& communities,
                        const std::vector<std::vector<int64_t>>& expected_groups) {
    for (const auto& group : expected_groups) {
        if (group.empty())
            continue;
        auto it = communities.find(group[0]);
        ASSERT_NE(it, communities.end()) << "node " << group[0] << " not found";
        int64_t expected_comm = it->second;
        for (int64_t n : group) {
            auto jt = communities.find(n);
            ASSERT_NE(jt, communities.end()) << "node " << n << " not found";
            EXPECT_EQ(jt->second, expected_comm)
                << "nodes " << group[0] << " and " << n << " should share community";
        }
    }
    for (size_t i = 0; i < expected_groups.size(); ++i) {
        for (size_t j = i + 1; j < expected_groups.size(); ++j) {
            if (expected_groups[i].empty() || expected_groups[j].empty())
                continue;
            auto ci = communities.at(expected_groups[i][0]);
            auto cj = communities.at(expected_groups[j][0]);
            EXPECT_NE(ci, cj) << "groups " << i << " and " << j << " should differ";
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class QA_GDB486 : public ::testing::Test {
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

    Result<std::vector<AlgorithmRow>> run_cd(const std::string& edge_type) {
        AlgorithmContext ctx{
            engine_,
            default_database_id,
            edge_type,
            {{"resolution", Value(1.0)}, {"max_iterations", Value(static_cast<int64_t>(10))}}};
        return community_detect_execute(ctx);
    }

    Result<std::vector<AlgorithmRow>>
    run_cd(const std::string& edge_type, double resolution, int64_t max_iterations) {
        AlgorithmContext ctx{
            engine_,
            default_database_id,
            edge_type,
            {{"resolution", Value(resolution)}, {"max_iterations", Value(max_iterations)}}};
        return community_detect_execute(ctx);
    }

    Result<std::vector<AlgorithmRow>> run_cd_no_params(const std::string& edge_type) {
        AlgorithmContext ctx{engine_, default_database_id, edge_type, {}};
        return community_detect_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ===========================================================================
// AC1: Louvain modularity optimization — correctness on known topologies
// ===========================================================================

TEST_F(QA_GDB486, AC1_BarbelGraph) {
    // Two complete K4 cliques connected by a single bridge edge.
    // Barbell graph: clear community structure.
    build_graph("knows",
                {
                    // Clique A: 1-2-3-4
                    {1, 2},
                    {1, 3},
                    {1, 4},
                    {2, 3},
                    {2, 4},
                    {3, 4},
                    // Clique B: 5-6-7-8
                    {5, 6},
                    {5, 7},
                    {5, 8},
                    {6, 7},
                    {6, 8},
                    {7, 8},
                    // Bridge
                    {4, 5},
                });

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    verify_communities(communities, {{1, 2, 3, 4}, {5, 6, 7, 8}});
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, AC1_ThreeCliquesWithBridges) {
    // Three K3 cliques connected by bridges: 3-4 and 6-7.
    build_graph("knows",
                {
                    {1, 2},
                    {2, 3},
                    {3, 1},
                    {4, 5},
                    {5, 6},
                    {6, 4},
                    {7, 8},
                    {8, 9},
                    {9, 7},
                    {3, 4},
                    {6, 7},
                });

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 9u);
    verify_contiguous_ids(communities);

    // Nodes within each clique should be co-located.
    EXPECT_EQ(communities[1], communities[2]);
    EXPECT_EQ(communities[1], communities[3]);
    EXPECT_EQ(communities[4], communities[5]);
    EXPECT_EQ(communities[4], communities[6]);
    EXPECT_EQ(communities[7], communities[8]);
    EXPECT_EQ(communities[7], communities[9]);
}

TEST_F(QA_GDB486, AC1_CompleteGraph_K5) {
    // A complete graph K5 — should be a single community.
    build_graph("knows",
                {
                    {1, 2},
                    {1, 3},
                    {1, 4},
                    {1, 5},
                    {2, 3},
                    {2, 4},
                    {2, 5},
                    {3, 4},
                    {3, 5},
                    {4, 5},
                });

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    verify_communities(communities, {{1, 2, 3, 4, 5}});
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, AC1_CycleGraph) {
    // A ring of 6 nodes — no clear community structure.
    build_graph("knows",
                {
                    {1, 2},
                    {2, 3},
                    {3, 4},
                    {4, 5},
                    {5, 6},
                    {6, 1},
                });

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 6u);
    verify_contiguous_ids(communities);
}

// ===========================================================================
// AC2: Output schema — (node_id INT64, community_id INT64)
// ===========================================================================

TEST_F(QA_GDB486, AC2_OutputSchemaDefinition) {
    auto def = make_community_detect_def();
    ASSERT_EQ(def.output_columns.size(), 2u);
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_FALSE(def.output_columns[0].nullable);
    EXPECT_EQ(def.output_columns[1].name, "community_id");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::INT64);
    EXPECT_FALSE(def.output_columns[1].nullable);
}

TEST_F(QA_GDB486, AC2_OutputRowsContainTwoValues) {
    build_graph("knows", {{1, 2}, {2, 3}});
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    for (const auto& row : *result) {
        EXPECT_EQ(row.values.size(), 2u);
        // Both values must be int64_t.
        EXPECT_NO_THROW(std::get<int64_t>(row.values[0].data()));
        EXPECT_NO_THROW(std::get<int64_t>(row.values[1].data()));
    }
}

TEST_F(QA_GDB486, AC2_AllNodesAppearInOutput) {
    build_graph("knows", {{10, 20}, {20, 30}, {40, 50}});
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    // All 5 nodes should appear.
    EXPECT_NE(communities.find(10), communities.end());
    EXPECT_NE(communities.find(20), communities.end());
    EXPECT_NE(communities.find(30), communities.end());
    EXPECT_NE(communities.find(40), communities.end());
    EXPECT_NE(communities.find(50), communities.end());
}

TEST_F(QA_GDB486, AC2_OutputSortedByNodeId) {
    build_graph("knows", {{100, 1}, {50, 25}, {75, 3}});
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_GE(result->size(), 2u);
    for (size_t i = 1; i < result->size(); ++i) {
        auto prev = std::get<int64_t>((*result)[i - 1].values[0].data());
        auto curr = std::get<int64_t>((*result)[i].values[0].data());
        EXPECT_LT(prev, curr) << "results should be sorted by node_id";
    }
}

// ===========================================================================
// AC3: Communities correctly identified on known graphs
// ===========================================================================

TEST_F(QA_GDB486, AC3_DisconnectedPairsAreSeparateCommunities) {
    // Three disconnected pairs: (1,2), (3,4), (5,6).
    build_graph("knows", {{1, 2}, {3, 4}, {5, 6}});
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    verify_communities(communities, {{1, 2}, {3, 4}, {5, 6}});
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, AC3_FiveDisconnectedCliques) {
    // Five separate K3 triangles — should find 5 communities.
    build_graph("knows",
                {
                    {1, 2},
                    {2, 3},
                    {3, 1},
                    {4, 5},
                    {5, 6},
                    {6, 4},
                    {7, 8},
                    {8, 9},
                    {9, 7},
                    {10, 11},
                    {11, 12},
                    {12, 10},
                    {13, 14},
                    {14, 15},
                    {15, 13},
                });
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    verify_communities(communities, {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {10, 11, 12}, {13, 14, 15}});
    verify_contiguous_ids(communities);
}

// ===========================================================================
// Edge cases: isolated nodes, single node, empty graphs
// ===========================================================================

TEST_F(QA_GDB486, EdgeCase_SingleIsolatedNode) {
    // A self-loop creates the node but the loop is ignored.
    build_graph("knows", {{1, 1}});
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Node 1 should be in its own community.
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 1u);
    EXPECT_EQ(communities[1], 0);
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, EdgeCase_MultipleSelfLoops) {
    // Multiple self-loops, no real edges.
    build_graph("knows", {{1, 1}, {2, 2}, {3, 3}});
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 3u);
    // Each node in its own community since there are no real edges.
    EXPECT_NE(communities[1], communities[2]);
    EXPECT_NE(communities[1], communities[3]);
    EXPECT_NE(communities[2], communities[3]);
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, EdgeCase_MixedSelfLoopsAndRealEdges) {
    // Self-loops should not affect community detection of real edges.
    build_graph("knows", {{1, 1}, {1, 2}, {2, 2}, {2, 3}, {3, 3}, {3, 1}});
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    // Triangle 1-2-3 should all be in the same community.
    verify_communities(communities, {{1, 2, 3}});
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, EdgeCase_EmptyGraph) {
    build_graph("knows", {});
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

// ===========================================================================
// Bidirectional edges — ensure no double-counting
// ===========================================================================

TEST_F(QA_GDB486, Bidirectional_BothDirectionsPresent) {
    // Both (1,2) and (2,1) in input — should be treated as a single undirected edge.
    build_graph("knows", {{1, 2}, {2, 1}, {2, 3}, {3, 2}});
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 3u);
    // All three should be in the same community (chain with deduplication).
    verify_communities(communities, {{1, 2, 3}});
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, Bidirectional_CliqueWithReverseEdges) {
    // K3 with all 6 directed edges — should still be one community.
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {1, 3},
                    {3, 1},
                });
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    verify_communities(communities, {{1, 2, 3}});
    verify_contiguous_ids(communities);
}

// ===========================================================================
// Parameter boundary values
// ===========================================================================

TEST_F(QA_GDB486, Param_DefaultsWhenNotProvided) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});
    auto result = run_cd_no_params("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 3u);
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, Param_ZeroMaxIterations) {
    // max_iterations=0 means no optimization — each node stays in its own community.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});
    auto result = run_cd("knows", 1.0, 0);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 3u);
    // Each node should be in its own community.
    EXPECT_NE(communities[1], communities[2]);
    EXPECT_NE(communities[1], communities[3]);
    EXPECT_NE(communities[2], communities[3]);
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, Param_NegativeMaxIterations) {
    // Negative max_iterations — loop doesn't execute; each node in its own community.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});
    auto result = run_cd("knows", 1.0, -1);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 3u);
    EXPECT_NE(communities[1], communities[2]);
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, Param_SingleIteration) {
    // max_iterations=1 — limited optimization, but on a clear graph should still work.
    build_graph("knows",
                {
                    {1, 2},
                    {1, 3},
                    {2, 3},
                    {4, 5},
                    {4, 6},
                    {5, 6},
                });
    auto result = run_cd("knows", 1.0, 1);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 6u);
    // With well-separated components, even 1 iteration should detect them.
    EXPECT_EQ(communities[1], communities[2]);
    EXPECT_EQ(communities[1], communities[3]);
    EXPECT_EQ(communities[4], communities[5]);
    EXPECT_EQ(communities[4], communities[6]);
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, Param_LargeMaxIterations) {
    // Very large max_iterations — should converge and produce valid results.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});
    auto result = run_cd("knows", 1.0, 10000);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    verify_communities(communities, {{1, 2, 3}});
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, Param_ZeroResolution) {
    // Resolution=0 — modularity gain = ki_in (all positive if there are edges).
    // This means every node prefers to join its neighbor's community.
    build_graph("knows",
                {
                    {1, 2},
                    {1, 3},
                    {2, 3},
                    {4, 5},
                    {4, 6},
                    {5, 6},
                    {3, 4},
                });
    auto result = run_cd("knows", 0.0, 10);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 6u);
    // With zero resolution, all connected nodes tend to merge into one community.
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, Param_VeryHighResolution) {
    // Very high resolution penalizes large communities, so each node may stay alone.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});
    auto result = run_cd("knows", 100.0, 10);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 3u);
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, Param_NegativeResolution) {
    // Negative resolution — algorithm should still run without crash.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});
    auto result = run_cd("knows", -1.0, 10);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 3u);
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, Param_VerySmallResolution) {
    // Epsilon-like resolution.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});
    auto result = run_cd("knows", 1e-15, 10);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    verify_contiguous_ids(communities);
}

// ===========================================================================
// Error paths
// ===========================================================================

TEST_F(QA_GDB486, Error_NonexistentEdgeType) {
    build_graph("knows", {{1, 2}});
    auto result = run_cd("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// ===========================================================================
// Edge key collision — potential deduplication bug with large node IDs
// ===========================================================================

TEST_F(QA_GDB486, EdgeKeyCollision_DistinctEdgesWithCollidingKeys) {
    // The edge deduplication uses: key = (lo << 32) | (hi & 0xFFFFFFFF)
    // Edge (0, 1) and edge (0, 4294967297) would have the same key since
    // 4294967297 & 0xFFFFFFFF == 1. This tests whether distinct edges are
    // incorrectly deduplicated.
    //
    // Node IDs: 0, 1, 4294967297 (= 2^32 + 1)
    // Expected: edges (0,1) and (0, 4294967297) are both counted.
    int64_t large_id = (1LL << 32) + 1; // 4294967297
    build_graph("knows", {{0, 1}, {0, large_id}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    // All three nodes should appear.
    EXPECT_EQ(communities.size(), 3u);
    EXPECT_NE(communities.find(0), communities.end());
    EXPECT_NE(communities.find(1), communities.end());
    EXPECT_NE(communities.find(large_id), communities.end());

    // Since edges (0,1) and (0, large_id) are distinct, node 0 connects
    // to both. If the edge key collision causes one edge to be dropped,
    // one of the nodes would become disconnected (its own community).
    // With both edges present, all three are connected through node 0.
    verify_communities(communities, {{0, 1, large_id}});
}

TEST_F(QA_GDB486, EdgeKeyCollision_TwoEdgesSameLowerBits) {
    // Another collision scenario: edges where hi values share lower 32 bits.
    // Edge (10, 100) and edge (10, 4294967396) where 4294967396 & 0xFFFFFFFF == 100
    int64_t id_a = 100;
    int64_t id_b = (1LL << 32) + 100; // upper bits differ, lower 32 match
    build_graph("knows", {{10, id_a}, {10, id_b}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 3u);
}

// ===========================================================================
// Large node IDs
// ===========================================================================

TEST_F(QA_GDB486, LargeNodeIds_PositiveBoundary) {
    int64_t large = 1000000000LL;
    build_graph("knows", {{large, large + 1}, {large + 1, large + 2}, {large + 2, large}});
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    verify_communities(communities, {{large, large + 1, large + 2}});
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, LargeNodeIds_NegativeIds) {
    // Negative node IDs should work.
    build_graph("knows", {{-1, -2}, {-2, -3}, {-3, -1}});
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    verify_communities(communities, {{-1, -2, -3}});
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, LargeNodeIds_MixedSignIds) {
    // Mix of positive and negative node IDs.
    build_graph("knows", {{-1, 1}, {1, -2}, {-2, -1}});
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    verify_communities(communities, {{-1, 1, -2}});
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, LargeNodeIds_ZeroNodeId) {
    build_graph("knows", {{0, 1}, {1, 2}});
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_NE(communities.find(0), communities.end());
    verify_contiguous_ids(communities);
}

// ===========================================================================
// Algorithm definition and registration
// ===========================================================================

TEST_F(QA_GDB486, Def_RegistrationAndLookup) {
    AlgorithmRegistry registry;
    auto reg = registry.register_algorithm(make_community_detect_def(), community_detect_execute);
    ASSERT_TRUE(reg.has_value()) << reg.error().message;

    auto* entry = registry.find("community_detect");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "community_detect");
    EXPECT_EQ(entry->def.output_columns.size(), 2u);
    EXPECT_EQ(entry->def.params.size(), 2u);
}

// ===========================================================================
// Stress test — moderately large graph
// ===========================================================================

TEST_F(QA_GDB486, Stress_TwoLargeCliques) {
    // Two cliques of 25 nodes each connected by a bridge.
    const int clique_size = 25;
    std::vector<std::pair<int64_t, int64_t>> edges;

    // Clique A: nodes 1..25
    for (int i = 1; i <= clique_size; ++i) {
        for (int j = i + 1; j <= clique_size; ++j) {
            edges.push_back({i, j});
        }
    }
    // Clique B: nodes 26..50
    for (int i = 26; i <= 50; ++i) {
        for (int j = i + 1; j <= 50; ++j) {
            edges.push_back({i, j});
        }
    }
    // Bridge
    edges.push_back({25, 26});

    build_graph("knows", edges);

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 50u);

    // All nodes in clique A share a community.
    for (int i = 2; i <= clique_size; ++i) {
        EXPECT_EQ(communities[1], communities[i]) << "node " << i << " should be in clique A";
    }
    // All nodes in clique B share a community.
    for (int i = 27; i <= 50; ++i) {
        EXPECT_EQ(communities[26], communities[i]) << "node " << i << " should be in clique B";
    }
    // Two cliques are separate.
    EXPECT_NE(communities[1], communities[26]);
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, Stress_ManyDisconnectedTriangles) {
    // 50 disconnected triangles = 150 nodes.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int i = 0; i < 50; ++i) {
        int64_t base = i * 3 + 1;
        edges.push_back({base, base + 1});
        edges.push_back({base + 1, base + 2});
        edges.push_back({base + 2, base});
    }
    build_graph("knows", edges);

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 150u);

    // Verify each triangle is its own community.
    std::unordered_set<int64_t> seen_communities;
    for (int i = 0; i < 50; ++i) {
        int64_t base = i * 3 + 1;
        EXPECT_EQ(communities[base], communities[base + 1]);
        EXPECT_EQ(communities[base], communities[base + 2]);
        seen_communities.insert(communities[base]);
    }
    EXPECT_EQ(seen_communities.size(), 50u);
    verify_contiguous_ids(communities);
}

// ===========================================================================
// Graph topology edge cases
// ===========================================================================

TEST_F(QA_GDB486, Topology_DuplicateEdgesInInput) {
    // Same edge appears multiple times — deduplication should handle it.
    build_graph("knows", {{1, 2}, {1, 2}, {1, 2}, {2, 3}, {2, 3}});
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 3u);
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, Topology_PathGraph) {
    // Long path: 1-2-3-4-5-6-7-8
    build_graph("knows",
                {
                    {1, 2},
                    {2, 3},
                    {3, 4},
                    {4, 5},
                    {5, 6},
                    {6, 7},
                    {7, 8},
                });
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 8u);
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, Topology_WheelGraph) {
    // Hub node 1 connected to all outer nodes 2-6, outer ring connected.
    build_graph("knows",
                {
                    {1, 2},
                    {1, 3},
                    {1, 4},
                    {1, 5},
                    {1, 6},
                    {2, 3},
                    {3, 4},
                    {4, 5},
                    {5, 6},
                    {6, 2},
                });
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 6u);
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, Topology_BipartiteGraph) {
    // Complete bipartite K3,3: nodes {1,2,3} each connected to {4,5,6}.
    build_graph("knows",
                {
                    {1, 4},
                    {1, 5},
                    {1, 6},
                    {2, 4},
                    {2, 5},
                    {2, 6},
                    {3, 4},
                    {3, 5},
                    {3, 6},
                });
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 6u);
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB486, Topology_TreeGraph) {
    // Binary tree of depth 3: root=1, children=2,3, grandchildren=4,5,6,7.
    build_graph("knows",
                {
                    {1, 2},
                    {1, 3},
                    {2, 4},
                    {2, 5},
                    {3, 6},
                    {3, 7},
                });
    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 7u);
    verify_contiguous_ids(communities);
}

// ===========================================================================
// Determinism — same graph, same result
// ===========================================================================

TEST_F(QA_GDB486, Determinism_SameResultOnRerun) {
    build_graph("knows",
                {
                    {1, 2},
                    {1, 3},
                    {2, 3},
                    {4, 5},
                    {4, 6},
                    {5, 6},
                    {3, 4},
                });

    auto result1 = run_cd("knows");
    ASSERT_TRUE(result1.has_value()) << result1.error().message;
    auto result2 = run_cd("knows");
    ASSERT_TRUE(result2.has_value()) << result2.error().message;

    ASSERT_EQ(result1->size(), result2->size());
    for (size_t i = 0; i < result1->size(); ++i) {
        ASSERT_EQ((*result1)[i].values.size(), (*result2)[i].values.size());
        for (size_t j = 0; j < (*result1)[i].values.size(); ++j) {
            EXPECT_EQ(std::get<int64_t>((*result1)[i].values[j].data()),
                      std::get<int64_t>((*result2)[i].values[j].data()))
                << "row " << i << " col " << j << " differs between runs";
        }
    }
}
