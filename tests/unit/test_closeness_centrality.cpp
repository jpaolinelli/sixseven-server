#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/closeness_centrality.h"
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

/// Result row: (node_id, closeness, sum_farness, reachable_count,
///              component_size, normalized_closeness).
struct ClosenessResult {
    double closeness;
    int64_t sum_farness;
    int64_t reachable_count;
    int64_t component_size;
    double normalized_closeness;
};

/// Extract per-node results from algorithm output rows.
std::unordered_map<int64_t, ClosenessResult>
to_closeness_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, ClosenessResult> result;
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

} // namespace

// ---------------------------------------------------------------------------
// Algorithm definition tests
// ---------------------------------------------------------------------------

TEST(ClosenessCentralityDef, OutputSchema) {
    auto def = make_closeness_centrality_def();
    EXPECT_EQ(def.name, "closeness");
    ASSERT_EQ(def.output_columns.size(), 6u);
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[1].name, "closeness");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::FLOAT64);
    EXPECT_EQ(def.output_columns[2].name, "sum_farness");
    EXPECT_EQ(def.output_columns[2].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[3].name, "reachable_count");
    EXPECT_EQ(def.output_columns[3].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[4].name, "component_size");
    EXPECT_EQ(def.output_columns[4].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[5].name, "normalized_closeness");
    EXPECT_EQ(def.output_columns[5].type_id, TypeId::FLOAT64);
}

TEST(ClosenessCentralityDef, VariantParameter) {
    auto def = make_closeness_centrality_def();
    ASSERT_EQ(def.params.size(), 1u);
    EXPECT_EQ(def.params[0].name, "variant");
    EXPECT_EQ(def.params[0].type_id, TypeId::STRING);
    EXPECT_FALSE(def.params[0].required);
    ASSERT_TRUE(def.params[0].default_value.has_value());
    EXPECT_EQ(std::get<std::string>(def.params[0].default_value->data()), "standard");
}

TEST(ClosenessCentralityDef, Registration) {
    AlgorithmRegistry registry;
    auto result =
        registry.register_algorithm(make_closeness_centrality_def(), closeness_centrality_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto* entry = registry.find("closeness");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "closeness");
}

// ---------------------------------------------------------------------------
// Test fixture with a GraphEngine
// ---------------------------------------------------------------------------

class ClosenessCentralityTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto t = catalog_.create_table(default_database_id, make_table_schema("nodes"));
        ASSERT_TRUE(t.has_value()) << t.error().message;
        table_id_ = *t;
    }

    /// Create an edge type and link a list of (src, tgt) pairs.
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

    /// Run closeness centrality with default (standard) variant.
    Result<std::vector<AlgorithmRow>> run_closeness(const std::string& edge_type) {
        AlgorithmContext ctx{engine_, edge_type, {}};
        return closeness_centrality_execute(ctx);
    }

    /// Run closeness centrality with a specific variant.
    Result<std::vector<AlgorithmRow>>
    run_closeness(const std::string& edge_type, const std::string& variant) {
        std::unordered_map<std::string, Value> args;
        args["variant"] = Value(variant);
        AlgorithmContext ctx{engine_, edge_type, args};
        return closeness_centrality_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ---------------------------------------------------------------------------
// Empty and single-edge edge cases
// ---------------------------------------------------------------------------

TEST_F(ClosenessCentralityTest, EmptyGraph) {
    build_graph("knows", {});

    auto result = run_closeness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(ClosenessCentralityTest, SingleEdge) {
    // 1 -> 2
    build_graph("knows", {{1, 2}});

    auto result = run_closeness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 2u);

    // Node 1 reaches node 2 at distance 1.
    EXPECT_DOUBLE_EQ(scores[1].closeness, 1.0); // (2-1)/1 = 1.0
    EXPECT_EQ(scores[1].sum_farness, 1);
    EXPECT_EQ(scores[1].reachable_count, 2);
    EXPECT_EQ(scores[1].component_size, 2);

    // Node 2 cannot reach anyone.
    EXPECT_DOUBLE_EQ(scores[2].closeness, 0.0);
    EXPECT_EQ(scores[2].sum_farness, 0);
    EXPECT_EQ(scores[2].reachable_count, 1);
    EXPECT_EQ(scores[2].component_size, 2);
}

// ---------------------------------------------------------------------------
// Path graph — center node highest closeness
// ---------------------------------------------------------------------------

TEST_F(ClosenessCentralityTest, BidirectionalPathCenterHighest) {
    // Bidirectional path: 1 <-> 2 <-> 3 <-> 4 <-> 5
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {3, 4},
                    {4, 3},
                    {4, 5},
                    {5, 4},
                });

    auto result = run_closeness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 5u);

    // Center node (3) should have the highest closeness.
    for (int64_t node : {1, 2, 4, 5}) {
        EXPECT_GT(scores[3].closeness, scores[node].closeness)
            << "center node 3 should have higher closeness than node " << node;
    }

    // Symmetry: node 2 == node 4, node 1 == node 5.
    EXPECT_NEAR(scores[2].closeness, scores[4].closeness, 1e-10);
    EXPECT_NEAR(scores[1].closeness, scores[5].closeness, 1e-10);

    // Known values for center node 3: distances to 2(1),4(1),1(2),5(2).
    // sum_farness = 6, reachable = 5, closeness = 4/6.
    EXPECT_EQ(scores[3].sum_farness, 6);
    EXPECT_EQ(scores[3].reachable_count, 5);
    EXPECT_NEAR(scores[3].closeness, 4.0 / 6.0, 1e-10);
    EXPECT_EQ(scores[3].component_size, 5);
}

// ---------------------------------------------------------------------------
// Complete graph — all equal closeness
// ---------------------------------------------------------------------------

TEST_F(ClosenessCentralityTest, CompleteGraph) {
    // K4 directed: every pair has a direct edge.
    build_graph("knows",
                {
                    {1, 2},
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
                    {4, 3},
                });

    auto result = run_closeness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 4u);

    // All nodes should have identical closeness = 1.0.
    // Each reaches 3 others at distance 1. closeness = 3/3 = 1.0.
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_DOUBLE_EQ(scores[node].closeness, 1.0)
            << "node " << node << " in complete graph should have closeness 1.0";
        EXPECT_EQ(scores[node].sum_farness, 3);
        EXPECT_EQ(scores[node].reachable_count, 4);
        EXPECT_EQ(scores[node].component_size, 4);
    }
}

// ---------------------------------------------------------------------------
// Star graph — central node = 1.0 closeness
// ---------------------------------------------------------------------------

TEST_F(ClosenessCentralityTest, StarGraphCentralNode) {
    // Hub = 1, spokes = 2..5. Only outgoing edges from hub.
    build_graph("knows", {{1, 2}, {1, 3}, {1, 4}, {1, 5}});

    auto result = run_closeness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 5u);

    // Hub reaches all 4 spokes at distance 1. closeness = 4/4 = 1.0.
    EXPECT_DOUBLE_EQ(scores[1].closeness, 1.0);
    EXPECT_EQ(scores[1].sum_farness, 4);
    EXPECT_EQ(scores[1].reachable_count, 5);
    EXPECT_EQ(scores[1].component_size, 5);

    // Spokes cannot reach anyone (no outgoing edges).
    for (int64_t node : {2, 3, 4, 5}) {
        EXPECT_DOUBLE_EQ(scores[node].closeness, 0.0)
            << "spoke node " << node << " should have closeness 0";
        EXPECT_EQ(scores[node].reachable_count, 1);
        EXPECT_EQ(scores[node].component_size, 5);
    }
}

// ---------------------------------------------------------------------------
// Disconnected graph — isolated components
// ---------------------------------------------------------------------------

TEST_F(ClosenessCentralityTest, DisconnectedGraph) {
    // Two disconnected pairs: 1->2, 3->4
    build_graph("knows", {{1, 2}, {3, 4}});

    auto result = run_closeness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 4u);

    // Source nodes reach 1 other node.
    EXPECT_DOUBLE_EQ(scores[1].closeness, 1.0);
    EXPECT_DOUBLE_EQ(scores[3].closeness, 1.0);

    // Sink nodes cannot reach anyone.
    EXPECT_DOUBLE_EQ(scores[2].closeness, 0.0);
    EXPECT_DOUBLE_EQ(scores[4].closeness, 0.0);

    // Component sizes.
    EXPECT_EQ(scores[1].component_size, 2);
    EXPECT_EQ(scores[2].component_size, 2);
    EXPECT_EQ(scores[3].component_size, 2);
    EXPECT_EQ(scores[4].component_size, 2);
}

// ---------------------------------------------------------------------------
// Known analytical values — directed triangle
// ---------------------------------------------------------------------------

TEST_F(ClosenessCentralityTest, DirectedTriangle) {
    // 1->2, 2->3, 3->1 (directed cycle)
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run_closeness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 3u);

    // Each node reaches 2 others: one at d=1, one at d=2.
    // sum_farness = 3, reachable = 3, closeness = 2/3.
    for (int64_t node : {1, 2, 3}) {
        EXPECT_NEAR(scores[node].closeness, 2.0 / 3.0, 1e-10)
            << "node " << node << " in directed triangle";
        EXPECT_EQ(scores[node].sum_farness, 3);
        EXPECT_EQ(scores[node].reachable_count, 3);
        EXPECT_EQ(scores[node].component_size, 3);
    }
}

// ---------------------------------------------------------------------------
// sum_farness and reachable_count correctness
// ---------------------------------------------------------------------------

TEST_F(ClosenessCentralityTest, FarnessAndReachableCount) {
    // 1->2->3->4 (directed chain)
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}});

    auto result = run_closeness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 4u);

    // Node 1: reach 2(d=1), 3(d=2), 4(d=3). sum_farness=6, reachable=4
    EXPECT_EQ(scores[1].sum_farness, 6);
    EXPECT_EQ(scores[1].reachable_count, 4);
    EXPECT_NEAR(scores[1].closeness, 3.0 / 6.0, 1e-10);

    // Node 2: reach 3(d=1), 4(d=2). sum_farness=3, reachable=3
    EXPECT_EQ(scores[2].sum_farness, 3);
    EXPECT_EQ(scores[2].reachable_count, 3);
    EXPECT_NEAR(scores[2].closeness, 2.0 / 3.0, 1e-10);

    // Node 3: reach 4(d=1). sum_farness=1, reachable=2
    EXPECT_EQ(scores[3].sum_farness, 1);
    EXPECT_EQ(scores[3].reachable_count, 2);
    EXPECT_DOUBLE_EQ(scores[3].closeness, 1.0);

    // Node 4: can't reach anyone. sum_farness=0, reachable=1
    EXPECT_EQ(scores[4].sum_farness, 0);
    EXPECT_EQ(scores[4].reachable_count, 1);
    EXPECT_DOUBLE_EQ(scores[4].closeness, 0.0);

    // All in one weakly connected component.
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_EQ(scores[node].component_size, 4);
    }
}

// ---------------------------------------------------------------------------
// Results ordering and error cases
// ---------------------------------------------------------------------------

TEST_F(ClosenessCentralityTest, ResultsOrderedByNodeId) {
    build_graph("knows", {{5, 1}, {3, 2}, {4, 3}});

    auto result = run_closeness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (size_t i = 1; i < result->size(); ++i) {
        auto prev = std::get<int64_t>((*result)[i - 1].values[0].data());
        auto curr = std::get<int64_t>((*result)[i].values[0].data());
        EXPECT_LT(prev, curr) << "results should be sorted by node_id";
    }
}

TEST_F(ClosenessCentralityTest, NonexistentEdgeTypeFails) {
    build_graph("knows", {{1, 2}});

    auto result = run_closeness("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(ClosenessCentralityTest, InvalidVariantFails) {
    build_graph("knows", {{1, 2}});

    auto result = run_closeness("knows", "invalid_variant");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Closeness values in valid range
// ---------------------------------------------------------------------------

TEST_F(ClosenessCentralityTest, ClosenessValuesNonNegative) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {1, 4}, {4, 5}});

    auto result = run_closeness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    for (const auto& [node, r] : scores) {
        EXPECT_GE(r.closeness, 0.0) << "node " << node << " should have non-negative closeness";
        EXPECT_GE(r.sum_farness, 0) << "node " << node << " should have non-negative sum_farness";
        EXPECT_GE(r.reachable_count, 1) << "node " << node << " should have reachable_count >= 1";
        EXPECT_GE(r.component_size, 1) << "node " << node << " should have component_size >= 1";
    }
}

TEST_F(ClosenessCentralityTest, ClosenessAtMostOne) {
    // In complete K3, all closeness should be exactly 1.0 (maximum possible).
    build_graph("knows", {{1, 2}, {2, 1}, {1, 3}, {3, 1}, {2, 3}, {3, 2}});

    auto result = run_closeness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    for (const auto& [node, r] : scores) {
        EXPECT_LE(r.closeness, 1.0 + 1e-10)
            << "node " << node << " closeness should not exceed 1.0";
    }
}

// ---------------------------------------------------------------------------
// Diamond graph
// ---------------------------------------------------------------------------

TEST_F(ClosenessCentralityTest, DiamondGraph) {
    // 1->2, 1->3, 2->4, 3->4
    build_graph("knows", {{1, 2}, {1, 3}, {2, 4}, {3, 4}});

    auto result = run_closeness("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 4u);

    // Node 1: reach 2(1), 3(1), 4(2). sum_farness=4, reachable=4, closeness=3/4
    EXPECT_EQ(scores[1].sum_farness, 4);
    EXPECT_EQ(scores[1].reachable_count, 4);
    EXPECT_NEAR(scores[1].closeness, 3.0 / 4.0, 1e-10);

    // Node 2: reach 4(1). sum_farness=1, reachable=2, closeness=1/1=1.0
    EXPECT_EQ(scores[2].sum_farness, 1);
    EXPECT_EQ(scores[2].reachable_count, 2);
    EXPECT_DOUBLE_EQ(scores[2].closeness, 1.0);

    // Node 3: reach 4(1). Same as node 2.
    EXPECT_NEAR(scores[2].closeness, scores[3].closeness, 1e-10);

    // Node 4: can't reach anyone.
    EXPECT_DOUBLE_EQ(scores[4].closeness, 0.0);

    // All in one component.
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_EQ(scores[node].component_size, 4);
    }
}

// ---------------------------------------------------------------------------
// Standard closeness values equal normalized_closeness
// ---------------------------------------------------------------------------

TEST_F(ClosenessCentralityTest, StandardVariantNormalizedMatchesCloseness) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run_closeness("knows", "standard");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    for (const auto& [node, r] : scores) {
        EXPECT_DOUBLE_EQ(r.closeness, r.normalized_closeness)
            << "standard variant: closeness should equal normalized_closeness for node " << node;
    }
}
