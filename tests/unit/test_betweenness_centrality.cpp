#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/betweenness_centrality.h"
#include "sixseven/graph/graph_engine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

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

} // namespace

// ---------------------------------------------------------------------------
// Algorithm definition tests
// ---------------------------------------------------------------------------

TEST(BetweennessCentralityDef, OutputSchema) {
    auto def = make_betweenness_centrality_def();
    EXPECT_EQ(def.name, "betweenness");
    ASSERT_EQ(def.output_columns.size(), 2u);
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[1].name, "centrality");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::FLOAT64);
}

TEST(BetweennessCentralityDef, Parameters) {
    auto def = make_betweenness_centrality_def();
    ASSERT_EQ(def.params.size(), 1u);

    EXPECT_EQ(def.params[0].name, "normalized");
    EXPECT_EQ(def.params[0].type_id, TypeId::BOOL);
    EXPECT_FALSE(def.params[0].required);
    ASSERT_TRUE(def.params[0].default_value.has_value());
    EXPECT_EQ(std::get<bool>(def.params[0].default_value->data()), true);
}

TEST(BetweennessCentralityDef, Registration) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_betweenness_centrality_def(),
                                              betweenness_centrality_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto* entry = registry.find("betweenness");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "betweenness");
}

// ---------------------------------------------------------------------------
// Test fixture with a GraphEngine
// ---------------------------------------------------------------------------

class BetweennessCentralityTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto t = catalog_.create_table(default_database_id, make_table_schema("nodes"));
        ASSERT_TRUE(t.has_value()) << t.error().message;
        table_id_ = *t;
    }

    /// Create an edge type and link a list of (src, tgt) pairs.
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

    /// Run betweenness centrality with default parameters (normalized=true).
    Result<std::vector<AlgorithmRow>> run_betweenness(const std::string& edge_type) {
        AlgorithmContext ctx{
            engine_, default_database_id, edge_type, {{"normalized", Value(true)}}};
        return betweenness_centrality_execute(ctx);
    }

    /// Run betweenness centrality with unnormalized scores.
    Result<std::vector<AlgorithmRow>> run_betweenness_unnormalized(const std::string& edge_type) {
        AlgorithmContext ctx{
            engine_, default_database_id, edge_type, {{"normalized", Value(false)}}};
        return betweenness_centrality_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ---------------------------------------------------------------------------
// Execution tests on known graphs
// ---------------------------------------------------------------------------

TEST_F(BetweennessCentralityTest, EmptyGraph) {
    build_graph("knows", {});

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(BetweennessCentralityTest, SingleEdge) {
    build_graph("knows", {{1, 2}});

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 2u);
    verify_scores_non_negative(scores);

    // With only 2 nodes, no node can be "between" others.
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
    EXPECT_DOUBLE_EQ(scores[2], 0.0);
}

TEST_F(BetweennessCentralityTest, LinearChain) {
    // 1 -> 2 -> 3
    // Node 2 lies on the shortest path from 1 to 3.
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 3u);
    verify_scores_non_negative(scores);

    // Node 2 is on the shortest path 1->3, so it has centrality 1.0 (unnormalized).
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
    EXPECT_DOUBLE_EQ(scores[2], 1.0);
    EXPECT_DOUBLE_EQ(scores[3], 0.0);
}

TEST_F(BetweennessCentralityTest, LinearChainNormalized) {
    // 1 -> 2 -> 3
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 3u);

    // Normalized by 1/((n-1)(n-2)) = 1/(2*1) = 0.5 for directed graphs with n=3.
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
    EXPECT_DOUBLE_EQ(scores[2], 0.5);
    EXPECT_DOUBLE_EQ(scores[3], 0.0);
}

TEST_F(BetweennessCentralityTest, StarGraph) {
    // Hub node 1 connects to all others: 1->2, 1->3, 1->4, 1->5
    // No node is between any pair of source-target.
    build_graph("knows", {{1, 2}, {1, 3}, {1, 4}, {1, 5}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 5u);
    verify_scores_non_negative(scores);

    // In a star with edges only from hub, there are no intermediate nodes.
    for (const auto& [node, score] : scores) {
        EXPECT_DOUBLE_EQ(score, 0.0)
            << "node " << node << " should have zero betweenness in a star";
    }
}

TEST_F(BetweennessCentralityTest, LongerLinearChain) {
    // 1 -> 2 -> 3 -> 4
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 4u);
    verify_scores_non_negative(scores);

    // Node 2 is on paths: 1->3 (via 2), 1->4 (via 2). Score = 2.
    // Node 3 is on paths: 1->4 (via 3), 2->4 (via 3). Score = 2.
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
    EXPECT_DOUBLE_EQ(scores[2], 2.0);
    EXPECT_DOUBLE_EQ(scores[3], 2.0);
    EXPECT_DOUBLE_EQ(scores[4], 0.0);
}

TEST_F(BetweennessCentralityTest, DiamondGraph) {
    // Diamond: 1->2, 1->3, 2->4, 3->4
    // Two paths from 1 to 4: 1->2->4 and 1->3->4.
    // Nodes 2 and 3 each carry half the betweenness for the pair (1,4).
    build_graph("knows", {{1, 2}, {1, 3}, {2, 4}, {3, 4}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 4u);
    verify_scores_non_negative(scores);

    // Nodes 2 and 3 split the betweenness for pair (1,4): each gets 0.5.
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
    EXPECT_NEAR(scores[2], 0.5, 1e-10);
    EXPECT_NEAR(scores[3], 0.5, 1e-10);
    EXPECT_DOUBLE_EQ(scores[4], 0.0);
}

TEST_F(BetweennessCentralityTest, CycleGraph) {
    // 1 -> 2 -> 3 -> 1 (directed cycle)
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 3u);
    verify_scores_non_negative(scores);

    // In a directed 3-cycle, each node lies on exactly one shortest path
    // from its predecessor to its successor. All scores equal.
    EXPECT_NEAR(scores[1], scores[2], 1e-10);
    EXPECT_NEAR(scores[2], scores[3], 1e-10);
}

TEST_F(BetweennessCentralityTest, ResultsOrderedByNodeId) {
    build_graph("knows", {{3, 1}, {5, 2}, {4, 3}});

    auto result = run_betweenness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (size_t i = 1; i < result->size(); ++i) {
        auto prev = std::get<int64_t>((*result)[i - 1].values[0].data());
        auto curr = std::get<int64_t>((*result)[i].values[0].data());
        EXPECT_LT(prev, curr) << "results should be sorted by node_id";
    }
}

TEST_F(BetweennessCentralityTest, NonexistentEdgeTypeFails) {
    build_graph("knows", {{1, 2}});

    auto result = run_betweenness("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(BetweennessCentralityTest, BridgeNode) {
    // Two clusters connected by a single bridge node (3).
    // 1->3, 2->3, 3->4, 3->5
    build_graph("knows", {{1, 3}, {2, 3}, {3, 4}, {3, 5}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 5u);
    verify_scores_non_negative(scores);

    // Node 3 should have the highest betweenness as the bridge.
    for (int64_t node : {1, 2, 4, 5}) {
        EXPECT_GT(scores[3], scores[node]) << "bridge node 3 should outrank node " << node;
    }
}

TEST_F(BetweennessCentralityTest, DisconnectedComponents) {
    // 1->2, 3->4 (two disconnected pairs)
    build_graph("knows", {{1, 2}, {3, 4}});

    auto result = run_betweenness_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_EQ(scores.size(), 4u);

    // No node is between any other pair (each pair is disconnected).
    for (const auto& [node, score] : scores) {
        EXPECT_DOUBLE_EQ(score, 0.0)
            << "node " << node << " should have zero betweenness in disconnected graph";
    }
}
