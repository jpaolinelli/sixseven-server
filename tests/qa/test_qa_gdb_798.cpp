// QA tests for GDB-798: Louvain parameter/self-loop tests pass even if the feature
// under test is ignored.
//
// These tests verify that the resolution, max_iterations, and self-loop handling
// actually affect the algorithm output - not just that the algorithm runs.

#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/louvain.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "test_catalog_helpers.h"

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

/// Count distinct communities.
size_t count_communities(const std::unordered_map<int64_t, int64_t>& communities) {
    std::unordered_set<int64_t> unique;
    for (const auto& [_, c] : communities) {
        unique.insert(c);
    }
    return unique.size();
}

} // namespace

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class LouvainParamQATest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
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

    Result<std::vector<AlgorithmRow>>
    run_cd(const std::string& edge_type, double resolution, int64_t max_iterations) {
        AlgorithmContext ctx{
            engine_,
            default_database_id,
            edge_type,
            {{"resolution", Value(resolution)}, {"max_iterations", Value(max_iterations)}}};
        return community_detect_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ---------------------------------------------------------------------------
// Test: Resolution parameter actually changes the output
// ---------------------------------------------------------------------------

// Higher resolution should produce more (smaller) communities.
// Lower resolution should produce fewer (larger) communities.
// This test verifies that the resolution parameter actually affects the output.
TEST_F(LouvainParamQATest, ResolutionAffectsCommunityCount) {
    // Use a graph with clear modular structure - two dense clusters with sparse connection.
    // With low resolution, both clusters merge into one community.
    // With high resolution, they may stay separate.
    build_graph("knows",
                {// Cluster A (dense)
                 {1, 2},
                 {2, 3},
                 {3, 1},
                 {1, 3},
                 {2, 1},
                 {3, 2},
                 // Cluster B (dense)
                 {4, 5},
                 {5, 6},
                 {6, 4},
                 {4, 6},
                 {5, 4},
                 {6, 5},
                 // Single bridge between clusters
                 {3, 4}});

    // Run with very low resolution (0.1) - strongly favors merging.
    auto result_low = run_cd("knows", 0.1, 100);
    ASSERT_TRUE(result_low.has_value()) << result_low.error().message;
    auto comms_low = to_community_map(*result_low);
    size_t num_comms_low = count_communities(comms_low);

    // Run with very high resolution (5.0) - strongly favors splitting.
    auto result_high = run_cd("knows", 5.0, 100);
    ASSERT_TRUE(result_high.has_value()) << result_high.error().message;
    auto comms_high = to_community_map(*result_high);
    size_t num_comms_high = count_communities(comms_high);

    // The number of communities should differ between low and high resolution.
    // If resolution is ignored, both would produce the same result.
    // Note: We don't assert the exact values, just that they're different.
    // (Some graphs may be stable regardless of resolution, but with extreme
    // values like 0.1 vs 5.0 we expect to see a difference on this graph.)
    EXPECT_NE(num_comms_low, num_comms_high)
        << "Resolution parameter should affect community count: low=" << num_comms_low
        << " vs high=" << num_comms_high << ". If equal, resolution may be ignored. "
        << "Note: This test may be graph-sensitive; if resolution truly affects assignments "
        << "(as shown by ResolutionAffectsAssignment), this is a test design issue, not a bug.";
}

// Another resolution test with a different graph structure.
TEST_F(LouvainParamQATest, ResolutionAffectsAssignment) {
    // Star-like graph with center connected to multiple leaves.
    // Different resolutions may assign leaves to different communities.
    build_graph("knows",
                {{1, 2},
                 {1, 3},
                 {1, 4},
                 {1, 5},
                 {1, 6},
                 {2, 3},
                 {4, 5}}); // Some leaf connectivity

    auto result_1 = run_cd("knows", 1.0, 100);
    ASSERT_TRUE(result_1.has_value()) << result_1.error().message;
    auto comms_1 = to_community_map(*result_1);

    auto result_3 = run_cd("knows", 3.0, 100);
    ASSERT_TRUE(result_3.has_value()) << result_3.error().message;
    auto comms_3 = to_community_map(*result_3);

    // At resolution=1.0 vs resolution=3.0, we should see different community assignments
    // for at least some nodes. If resolution is ignored, assignments would be identical.
    int differing_assignments = 0;
    for (const auto& [node, comm1] : comms_1) {
        auto it = comms_3.find(node);
        if (it != comms_3.end() && it->second != comm1) {
            ++differing_assignments;
        }
    }

    EXPECT_GT(differing_assignments, 0)
        << "At least one node should have different community assignment between "
        << "resolution=1.0 and resolution=3.0. Found 0 differences - resolution may be ignored.";
}

// ---------------------------------------------------------------------------
// Test: max_iterations parameter actually affects convergence
// ---------------------------------------------------------------------------

TEST_F(LouvainParamQATest, MaxIterationsAffectsConvergence) {
    // Use a graph complex enough that iteration limit matters.
    // A larger graph with multiple possible partitions.
    build_graph("knows",
                {{1, 2},
                 {2, 3},
                 {3, 4},
                 {4, 5},
                 {5, 6},
                 {6, 7},
                 {7, 8},
                 {8, 1}, // Ring
                 {1, 5}, // Cross link to create tension
                 {3, 7}}); // Another cross link

    // Run with just 1 iteration - algorithm hasn't converged.
    auto result_1 = run_cd("knows", 1.0, 1);
    ASSERT_TRUE(result_1.has_value()) << result_1.error().message;
    auto comms_1 = to_community_map(*result_1);

    // Run with 100 iterations - should converge to stable state.
    auto result_100 = run_cd("knows", 1.0, 100);
    ASSERT_TRUE(result_100.has_value()) << result_100.error().message;
    auto comms_100 = to_community_map(*result_100);

    // With 1 iteration vs 100 iterations, the community assignments should differ
    // if max_iterations is actually being used. If ignored, results would be identical.
    int differing_assignments = 0;
    for (const auto& [node, comm1] : comms_1) {
        auto it = comms_100.find(node);
        if (it != comms_100.end() && it->second != comm1) {
            ++differing_assignments;
        }
    }

    EXPECT_GT(differing_assignments, 0)
        << "At least one node should have different community assignment between "
        << "1 iteration and 100 iterations. Found 0 differences - max_iterations may be ignored.";
}

// Verify that with enough iterations, the algorithm converges (same result on repeated runs).
TEST_F(LouvainParamQATest, IterationsConvergeToStableState) {
    build_graph("knows",
                {{1, 2},
                 {2, 3},
                 {3, 1},
                 {4, 5},
                 {5, 6},
                 {6, 4},
                 {3, 4}});

    // Run twice with max_iterations.
    auto result_a = run_cd("knows", 1.0, 50);
    ASSERT_TRUE(result_a.has_value()) << result_a.error().message;
    auto comms_a = to_community_map(*result_a);

    auto result_b = run_cd("knows", 1.0, 50);
    ASSERT_TRUE(result_b.has_value()) << result_b.error().message;
    auto comms_b = to_community_map(*result_b);

    // Results should be identical (converged to same stable state).
    EXPECT_EQ(comms_a, comms_b) << "Algorithm should converge to same result on repeated runs";
}

// ---------------------------------------------------------------------------
// Test: Self-loops are actually ignored in the algorithm
// ---------------------------------------------------------------------------

// Self-loops should NOT contribute to node degree or edge weight.
// This test creates a graph where counting vs ignoring self-loop would give
// different community assignments.
TEST_F(LouvainParamQATest, SelfLoopDoesNotAffectCommunityAssignment) {
    // Build a graph where node 1 has a self-loop plus connections to nodes 2 and 3.
    // If self-loop is counted in degree, node 1 has higher internal weight and might
    // stay separate. If ignored, node 1 should merge with its neighbors.
    //
    // Graph: 1 --(self-loop)--> 1, 1 --> 2, 2 --> 3
    // This is a path 1-2-3 with an extra self-loop on 1.
    build_graph("knows", {{1, 1}, {1, 2}, {2, 3}});

    auto result = run_cd("knows", 1.0, 100);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto comms = to_community_map(*result);

    // All three nodes should be in the same community because they're connected.
    // The self-loop should NOT cause node 1 to remain isolated.
    size_t num_comms = count_communities(comms);
    EXPECT_EQ(num_comms, 1u) << "All connected nodes should be in one community";
}

// Verify self-loop is not counted in total edge weight.
// Use a graph where the total weight matters for modularity calculation.
TEST_F(LouvainParamQATest, SelfLoopNotCountedInEdgeWeight) {
    // Create two disconnected edges: (1,2) and a self-loop (3,3).
    // The self-loop should NOT increase total graph weight that affects modularity.
    build_graph("knows", {{1, 2}, {3, 3}});

    auto result = run_cd("knows", 1.0, 100);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto comms = to_community_map(*result);

    // We should have 3 nodes with at least 2 communities (1-2 in one, 3 in another).
    // If self-loop was counted, it might affect the modularity calculation.
    EXPECT_EQ(comms.size(), 3u);
    EXPECT_GE(count_communities(comms), 2u);
}

// Self-loop with high weight should not dominate the algorithm.
TEST_F(LouvainParamQATest, SelfLoopWithNormalEdges) {
    // Node 1 has a strong self-loop but also connects to node 2.
    // The self-loop should not prevent node 1 from being assigned to the same
    // community as node 2 (since they're connected).
    build_graph("knows", {{1, 1}, {1, 1}, {1, 1}, {1, 2}, {2, 3}});

    auto result = run_cd("knows", 1.0, 100);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto comms = to_community_map(*result);

    // Nodes 1, 2, 3 are connected via edges (1,2) and (2,3), so should be in same community.
    // Multiple self-loops should not change this.
    size_t num_comms = count_communities(comms);
    EXPECT_EQ(num_comms, 1u) << "Multiple self-loops should not affect community assignment";
}