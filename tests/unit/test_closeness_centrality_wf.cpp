#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/closeness_centrality.h"
#include "sixseven/graph/graph_engine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

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

struct WFResult {
    double closeness;
    int64_t sum_farness;
    int64_t reachable_count;
    int64_t component_size;
    double normalized_closeness;
};

std::unordered_map<int64_t, WFResult> to_wf_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, WFResult> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 6u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto closeness = std::get<double>(row.values[1].data());
        auto sum_farness = std::get<int64_t>(row.values[2].data());
        auto reachable_count = std::get<int64_t>(row.values[3].data());
        auto component_size = std::get<int64_t>(row.values[4].data());
        auto normalized_closeness = std::get<double>(row.values[5].data());
        result[node_id] = {
            closeness, sum_farness, reachable_count, component_size, normalized_closeness};
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class WassermanFaustTest : public ::testing::Test {
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

    Result<std::vector<AlgorithmRow>> run_wf(const std::string& edge_type) {
        std::unordered_map<std::string, Value> args;
        args["variant"] = Value(std::string("wasserman_faust"));
        AlgorithmContext ctx{engine_, default_database_id, edge_type, args};
        return closeness_centrality_execute(ctx);
    }

    Result<std::vector<AlgorithmRow>> run_standard(const std::string& edge_type) {
        std::unordered_map<std::string, Value> args;
        args["variant"] = Value(std::string("standard"));
        AlgorithmContext ctx{engine_, default_database_id, edge_type, args};
        return closeness_centrality_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ---------------------------------------------------------------------------
// Multi-component graph (2+ disconnected components)
// ---------------------------------------------------------------------------

TEST_F(WassermanFaustTest, TwoDisconnectedComponents) {
    // Component A: 1 <-> 2 <-> 3 (triangle, bidirectional)
    // Component B: 10 <-> 11
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {1, 3},
                    {3, 1},
                    {10, 11},
                    {11, 10},
                });

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map(*result);
    EXPECT_EQ(scores.size(), 5u);

    // Component A has 3 nodes, Component B has 2 nodes, N=5.
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(scores[node].component_size, 3);
    }
    for (int64_t node : {10, 11}) {
        EXPECT_EQ(scores[node].component_size, 2);
    }

    // All nodes in disconnected components should get non-zero WF scores.
    for (int64_t node : {1, 2, 3, 10, 11}) {
        EXPECT_GT(scores[node].closeness, 0.0)
            << "node " << node << " should have non-zero WF closeness";
    }
}

// ---------------------------------------------------------------------------
// Larger component nodes score higher than smaller component nodes
// ---------------------------------------------------------------------------

TEST_F(WassermanFaustTest, LargerComponentScoresHigher) {
    // Component A: complete K4 (nodes 1-4). Each has within-closeness = 1.0.
    // Component B: complete K2 (nodes 10-11). Each has within-closeness = 1.0.
    // N = 6.
    // WF for A: [(4-1)/(6-1)] * 1.0 = 3/5 = 0.6
    // WF for B: [(2-1)/(6-1)] * 1.0 = 1/5 = 0.2
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {1, 3},
                    {3, 1},
                    {1, 4},
                    {4, 1},
                    {2, 3},
                    {3, 2},
                    {2, 4},
                    {4, 2},
                    {3, 4},
                    {4, 3},
                    {10, 11},
                    {11, 10},
                });

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map(*result);

    // Nodes in the larger component should have higher WF closeness.
    for (int64_t big : {1, 2, 3, 4}) {
        for (int64_t small : {10, 11}) {
            EXPECT_GT(scores[big].closeness, scores[small].closeness)
                << "node " << big << " (large component) should score higher than node " << small
                << " (small component)";
        }
    }

    // Verify exact WF values.
    // Component A: n_c=4, N=6. Each node reaches 3 others at d=1.
    // normalized_closeness = (4-1)/3 = 1.0
    // WF = (4-1)/(6-1) * 1.0 = 3/5 = 0.6
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_NEAR(scores[node].closeness, 3.0 / 5.0, 1e-10) << "node " << node << " WF closeness";
        EXPECT_NEAR(scores[node].normalized_closeness, 1.0, 1e-10)
            << "node " << node << " normalized closeness";
    }

    // Component B: n_c=2, N=6. Each node reaches 1 other at d=1.
    // normalized_closeness = (2-1)/1 = 1.0
    // WF = (2-1)/(6-1) * 1.0 = 1/5 = 0.2
    for (int64_t node : {10, 11}) {
        EXPECT_NEAR(scores[node].closeness, 1.0 / 5.0, 1e-10) << "node " << node << " WF closeness";
        EXPECT_NEAR(scores[node].normalized_closeness, 1.0, 1e-10)
            << "node " << node << " normalized closeness";
    }
}

// ---------------------------------------------------------------------------
// Fully connected graph: WF matches standard closeness
// ---------------------------------------------------------------------------

TEST_F(WassermanFaustTest, FullyConnectedMatchesStandard) {
    // Complete K4: all nodes in one component, n_c = N = 4.
    // Scaling factor = (4-1)/(4-1) = 1.0, so WF = standard.
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {1, 3},
                    {3, 1},
                    {1, 4},
                    {4, 1},
                    {2, 3},
                    {3, 2},
                    {2, 4},
                    {4, 2},
                    {3, 4},
                    {4, 3},
                });

    auto wf_result = run_wf("knows");
    ASSERT_TRUE(wf_result.has_value()) << wf_result.error().message;
    auto wf_scores = to_wf_map(*wf_result);

    // On a fully connected graph, WF closeness should equal standard.
    // Each node: closeness = 3/3 = 1.0. WF = (3/3)*1.0 = 1.0.
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_DOUBLE_EQ(wf_scores[node].closeness, 1.0)
            << "WF on complete graph should be 1.0 for node " << node;
        EXPECT_DOUBLE_EQ(wf_scores[node].normalized_closeness, 1.0);
        EXPECT_EQ(wf_scores[node].component_size, 4);
    }
}

TEST_F(WassermanFaustTest, FullyConnectedPathMatchesStandard) {
    // Bidirectional path 1 <-> 2 <-> 3: single component, n_c = N = 3.
    build_graph("knows", {{1, 2}, {2, 1}, {2, 3}, {3, 2}});

    auto wf_result = run_wf("knows");
    ASSERT_TRUE(wf_result.has_value()) << wf_result.error().message;
    auto wf_scores = to_wf_map(*wf_result);

    // Re-run as standard for comparison.
    // Need a new graph engine for the second run — just verify WF formula manually.
    // Node 2 (center): reaches 1(d=1), 3(d=1). sum_farness=2, reachable=3.
    // normalized = (3-1)/2 = 1.0. WF = (3-1)/(3-1) * 1.0 = 1.0.
    EXPECT_NEAR(wf_scores[2].closeness, 1.0, 1e-10);

    // Node 1: reaches 2(d=1), 3(d=2). sum_farness=3, reachable=3.
    // normalized = (3-1)/3 = 2/3. WF = (3-1)/(3-1) * 2/3 = 2/3.
    EXPECT_NEAR(wf_scores[1].closeness, 2.0 / 3.0, 1e-10);
    EXPECT_NEAR(wf_scores[3].closeness, 2.0 / 3.0, 1e-10);
}

// ---------------------------------------------------------------------------
// Single isolated node (pair with only one direction)
// ---------------------------------------------------------------------------

TEST_F(WassermanFaustTest, SingleDirectedEdgeIsolatedSink) {
    // 1 -> 2: both in same weakly connected component of size 2.
    // Node 2 has no outgoing edges: reachable_count=1, closeness=0.
    build_graph("knows", {{1, 2}});

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map(*result);
    EXPECT_EQ(scores.size(), 2u);

    // Node 1: reaches node 2 at d=1. sum_farness=1, reachable=2, n_c=2, N=2.
    // normalized = (2-1)/1 = 1.0. WF = (2-1)/(2-1) * 1.0 = 1.0.
    EXPECT_NEAR(scores[1].closeness, 1.0, 1e-10);
    EXPECT_EQ(scores[1].component_size, 2);

    // Node 2: can't reach anyone. WF = 0.
    EXPECT_DOUBLE_EQ(scores[2].closeness, 0.0);
    EXPECT_DOUBLE_EQ(scores[2].normalized_closeness, 0.0);
    EXPECT_EQ(scores[2].component_size, 2);
}

// ---------------------------------------------------------------------------
// Component size correctly reported
// ---------------------------------------------------------------------------

TEST_F(WassermanFaustTest, ComponentSizesThreeComponents) {
    // Component A: 1->2->3 (chain, 3 nodes)
    // Component B: 10<->11<->12<->13 (path, 4 nodes)
    // Component C: 20->21 (2 nodes)
    // Total N = 9.
    build_graph("knows",
                {
                    {1, 2},
                    {2, 3},
                    {10, 11},
                    {11, 10},
                    {11, 12},
                    {12, 11},
                    {12, 13},
                    {13, 12},
                    {20, 21},
                });

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map(*result);
    EXPECT_EQ(scores.size(), 9u);

    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(scores[node].component_size, 3)
            << "node " << node << " should be in component of size 3";
    }
    for (int64_t node : {10, 11, 12, 13}) {
        EXPECT_EQ(scores[node].component_size, 4)
            << "node " << node << " should be in component of size 4";
    }
    for (int64_t node : {20, 21}) {
        EXPECT_EQ(scores[node].component_size, 2)
            << "node " << node << " should be in component of size 2";
    }
}

// ---------------------------------------------------------------------------
// Known analytical values from Wasserman-Faust formula
// ---------------------------------------------------------------------------

TEST_F(WassermanFaustTest, KnownAnalyticalValues) {
    // Two components:
    // Component A: bidirectional triangle 1<->2<->3<->1 (n_c=3)
    // Component B: bidirectional pair 10<->11 (n_c=2)
    // N = 5.
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {3, 1},
                    {1, 3},
                    {10, 11},
                    {11, 10},
                });

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map(*result);

    // Component A (triangle): each node reaches 2 others at d=1.
    // sum_farness = 2, reachable = 3.
    // normalized = (3-1)/2 = 1.0.
    // WF = [(3-1)/(5-1)] * 1.0 = 2/4 = 0.5.
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(scores[node].sum_farness, 2);
        EXPECT_EQ(scores[node].reachable_count, 3);
        EXPECT_NEAR(scores[node].normalized_closeness, 1.0, 1e-10);
        EXPECT_NEAR(scores[node].closeness, 0.5, 1e-10)
            << "node " << node << " WF closeness should be 0.5";
    }

    // Component B (pair): each reaches 1 other at d=1.
    // sum_farness = 1, reachable = 2.
    // normalized = (2-1)/1 = 1.0.
    // WF = [(2-1)/(5-1)] * 1.0 = 1/4 = 0.25.
    for (int64_t node : {10, 11}) {
        EXPECT_EQ(scores[node].sum_farness, 1);
        EXPECT_EQ(scores[node].reachable_count, 2);
        EXPECT_NEAR(scores[node].normalized_closeness, 1.0, 1e-10);
        EXPECT_NEAR(scores[node].closeness, 0.25, 1e-10)
            << "node " << node << " WF closeness should be 0.25";
    }
}

TEST_F(WassermanFaustTest, AnalyticalValuesWithVaryingCentrality) {
    // Component A: directed path 1->2->3 (n_c=3)
    // Component B: bidirectional pair 10<->11 (n_c=2)
    // N = 5.
    build_graph("knows",
                {
                    {1, 2},
                    {2, 3},
                    {10, 11},
                    {11, 10},
                });

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map(*result);

    // Node 1: reaches 2(d=1), 3(d=2). sum_farness=3, reachable=3.
    // normalized = (3-1)/3 = 2/3. WF = (3-1)/(5-1) * 2/3 = 2/4 * 2/3 = 1/3.
    EXPECT_NEAR(scores[1].normalized_closeness, 2.0 / 3.0, 1e-10);
    EXPECT_NEAR(scores[1].closeness, 1.0 / 3.0, 1e-10);

    // Node 2: reaches 3(d=1). sum_farness=1, reachable=2.
    // normalized = (3-1)/1 = 2.0. WF = (3-1)/(5-1) * 2.0 = 2/4 * 2 = 1.0.
    EXPECT_NEAR(scores[2].normalized_closeness, 2.0, 1e-10);
    EXPECT_NEAR(scores[2].closeness, 1.0, 1e-10);

    // Node 3: can't reach anyone. WF = 0.
    EXPECT_DOUBLE_EQ(scores[3].closeness, 0.0);
    EXPECT_DOUBLE_EQ(scores[3].normalized_closeness, 0.0);

    // Nodes 10, 11: n_c=2, each reaches 1 at d=1.
    // normalized = (2-1)/1 = 1.0. WF = (2-1)/(5-1) * 1.0 = 1/4 = 0.25.
    for (int64_t node : {10, 11}) {
        EXPECT_NEAR(scores[node].closeness, 0.25, 1e-10);
    }
}

// ---------------------------------------------------------------------------
// Normalized closeness values
// ---------------------------------------------------------------------------

TEST_F(WassermanFaustTest, NormalizedClosenessIsWithinComponent) {
    // The normalized_closeness should be (n_c - 1) / sum_farness,
    // which is the within-component closeness before the WF scaling.
    // Component A: bidirectional path 1<->2<->3
    // Component B: single directed edge 10->11
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {10, 11},
                });

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map(*result);

    // Verify normalized_closeness = (n_c - 1) / sum_farness for reachable nodes.
    for (const auto& [node, r] : scores) {
        if (r.sum_farness > 0 && r.reachable_count > 1) {
            double expected_norm =
                static_cast<double>(r.component_size - 1) / static_cast<double>(r.sum_farness);
            EXPECT_NEAR(r.normalized_closeness, expected_norm, 1e-10)
                << "node " << node << " normalized_closeness should be (n_c-1)/sum_farness";
        } else {
            EXPECT_DOUBLE_EQ(r.normalized_closeness, 0.0)
                << "node " << node << " with no reachable nodes should have 0 normalized_closeness";
        }
    }

    // Verify WF = [(n_c-1)/(N-1)] * normalized_closeness.
    auto total_nodes = static_cast<int64_t>(scores.size());
    for (const auto& [node, r] : scores) {
        if (r.normalized_closeness > 0.0) {
            double scaling =
                static_cast<double>(r.component_size - 1) / static_cast<double>(total_nodes - 1);
            double expected_wf = scaling * r.normalized_closeness;
            EXPECT_NEAR(r.closeness, expected_wf, 1e-10)
                << "node " << node << " WF closeness should be scaling * normalized_closeness";
        } else {
            EXPECT_DOUBLE_EQ(r.closeness, 0.0);
        }
    }
}

// ---------------------------------------------------------------------------
// WF closeness values are non-negative and finite
// ---------------------------------------------------------------------------

TEST_F(WassermanFaustTest, ValuesNonNegativeAndFinite) {
    build_graph("knows",
                {
                    {1, 2},
                    {2, 3},
                    {3, 1},
                    {10, 11},
                    {11, 12},
                    {20, 21},
                });

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map(*result);
    for (const auto& [node, r] : scores) {
        EXPECT_GE(r.closeness, 0.0) << "node " << node;
        EXPECT_FALSE(std::isnan(r.closeness)) << "node " << node << " closeness is NaN";
        EXPECT_FALSE(std::isinf(r.closeness)) << "node " << node << " closeness is Inf";
        EXPECT_GE(r.normalized_closeness, 0.0) << "node " << node;
        EXPECT_FALSE(std::isnan(r.normalized_closeness)) << "node " << node;
        EXPECT_FALSE(std::isinf(r.normalized_closeness)) << "node " << node;
        EXPECT_GE(r.component_size, 1) << "node " << node;
    }
}

// ---------------------------------------------------------------------------
// Empty graph returns empty result
// ---------------------------------------------------------------------------

TEST_F(WassermanFaustTest, EmptyGraph) {
    build_graph("knows", {});

    auto result = run_wf("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}
