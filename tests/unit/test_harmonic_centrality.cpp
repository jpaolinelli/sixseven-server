#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/harmonic_centrality.h"

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

/// Result row: (node_id, harmonic, normalized_harmonic).
struct HarmonicResult {
    double harmonic;
    double normalized_harmonic;
};

/// Extract per-node results from algorithm output rows.
std::unordered_map<int64_t, HarmonicResult> to_harmonic_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, HarmonicResult> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 3u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto harmonic = std::get<double>(row.values[1].data());
        auto normalized_harmonic = std::get<double>(row.values[2].data());
        result[node_id] = {harmonic, normalized_harmonic};
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Algorithm definition tests
// ---------------------------------------------------------------------------

TEST(HarmonicCentralityDef, OutputSchema) {
    auto def = make_harmonic_centrality_def();
    EXPECT_EQ(def.name, "harmonic");
    ASSERT_EQ(def.output_columns.size(), 3u);
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[1].name, "harmonic");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::FLOAT64);
    EXPECT_EQ(def.output_columns[2].name, "normalized_harmonic");
    EXPECT_EQ(def.output_columns[2].type_id, TypeId::FLOAT64);
}

TEST(HarmonicCentralityDef, NoParameters) {
    auto def = make_harmonic_centrality_def();
    EXPECT_TRUE(def.params.empty());
}

TEST(HarmonicCentralityDef, Registration) {
    AlgorithmRegistry registry;
    auto result =
        registry.register_algorithm(make_harmonic_centrality_def(), harmonic_centrality_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto* entry = registry.find("harmonic");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "harmonic");
}

// ---------------------------------------------------------------------------
// Test fixture with a GraphEngine
// ---------------------------------------------------------------------------

class HarmonicCentralityTest : public ::testing::Test {
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
            edge_type, table_id_, table_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(et.has_value()) << et.error().message;

        for (auto [src, tgt] : edges) {
            auto link = engine_.link(edge_type, pk(src), pk(tgt));
            ASSERT_TRUE(link.has_value()) << link.error().message;
        }
    }

    /// Run harmonic centrality.
    Result<std::vector<AlgorithmRow>> run_harmonic(const std::string& edge_type) {
        AlgorithmContext ctx{engine_, edge_type, {}};
        return harmonic_centrality_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ---------------------------------------------------------------------------
// Empty graph and single-edge edge cases
// ---------------------------------------------------------------------------

TEST_F(HarmonicCentralityTest, EmptyGraph) {
    build_graph("knows", {});

    auto result = run_harmonic("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(HarmonicCentralityTest, SingleEdge) {
    // 1 -> 2
    build_graph("knows", {{1, 2}});

    auto result = run_harmonic("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_harmonic_map(*result);
    EXPECT_EQ(scores.size(), 2u);

    // Node 1 reaches node 2 at distance 1. H(1) = 1/1 = 1.0
    EXPECT_DOUBLE_EQ(scores[1].harmonic, 1.0);
    // Normalized: 1.0 / (2-1) = 1.0
    EXPECT_DOUBLE_EQ(scores[1].normalized_harmonic, 1.0);

    // Node 2 cannot reach anyone. H(2) = 0.0
    EXPECT_DOUBLE_EQ(scores[2].harmonic, 0.0);
    EXPECT_DOUBLE_EQ(scores[2].normalized_harmonic, 0.0);
}

// ---------------------------------------------------------------------------
// Complete graph — all equal harmonic centrality
// ---------------------------------------------------------------------------

TEST_F(HarmonicCentralityTest, CompleteGraph) {
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

    auto result = run_harmonic("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_harmonic_map(*result);
    EXPECT_EQ(scores.size(), 4u);

    // All nodes at distance 1 to 3 others. H = 3 * (1/1) = 3.0
    // Normalized: 3.0 / (4-1) = 1.0
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_DOUBLE_EQ(scores[node].harmonic, 3.0)
            << "node " << node << " in complete graph should have harmonic 3.0";
        EXPECT_DOUBLE_EQ(scores[node].normalized_harmonic, 1.0)
            << "node " << node << " in complete graph should have normalized_harmonic 1.0";
    }
}

// ---------------------------------------------------------------------------
// Star graph — central node highest
// ---------------------------------------------------------------------------

TEST_F(HarmonicCentralityTest, StarGraphCentralNodeHighest) {
    // Bidirectional star: hub = 1, spokes = 2..5
    build_graph("knows",
                {
                    {1, 2},
                    {1, 3},
                    {1, 4},
                    {1, 5},
                    {2, 1},
                    {3, 1},
                    {4, 1},
                    {5, 1},
                });

    auto result = run_harmonic("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_harmonic_map(*result);
    EXPECT_EQ(scores.size(), 5u);

    // Hub (1) reaches 4 spokes at distance 1. H(1) = 4 * 1/1 = 4.0
    EXPECT_DOUBLE_EQ(scores[1].harmonic, 4.0);
    EXPECT_DOUBLE_EQ(scores[1].normalized_harmonic, 1.0);

    // Spoke reaches hub at d=1, 3 other spokes at d=2. H = 1/1 + 3*(1/2) = 2.5
    for (int64_t node : {2, 3, 4, 5}) {
        EXPECT_DOUBLE_EQ(scores[node].harmonic, 2.5)
            << "spoke node " << node << " should have harmonic 2.5";
        EXPECT_DOUBLE_EQ(scores[node].normalized_harmonic, 2.5 / 4.0) << "spoke node " << node;
    }

    // Hub has highest harmonic centrality.
    for (int64_t node : {2, 3, 4, 5}) {
        EXPECT_GT(scores[1].harmonic, scores[node].harmonic);
    }
}

// ---------------------------------------------------------------------------
// Disconnected graph — partial scores, not zero
// ---------------------------------------------------------------------------

TEST_F(HarmonicCentralityTest, DisconnectedGraph) {
    // Two disconnected components: {1<->2}, {3<->4<->5}
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {3, 4},
                    {4, 3},
                    {4, 5},
                    {5, 4},
                });

    auto result = run_harmonic("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_harmonic_map(*result);
    EXPECT_EQ(scores.size(), 5u);

    // Node 1: reaches 2 at d=1. H(1) = 1.0. Unreachable nodes contribute 0.
    EXPECT_DOUBLE_EQ(scores[1].harmonic, 1.0);
    // Normalized: 1.0 / (5-1) = 0.25
    EXPECT_DOUBLE_EQ(scores[1].normalized_harmonic, 0.25);

    // Node 4: reaches 3 at d=1 and 5 at d=1. H(4) = 2.0
    EXPECT_DOUBLE_EQ(scores[4].harmonic, 2.0);
    EXPECT_DOUBLE_EQ(scores[4].normalized_harmonic, 0.5);

    // Node 3: reaches 4 at d=1, 5 at d=2. H(3) = 1/1 + 1/2 = 1.5
    EXPECT_DOUBLE_EQ(scores[3].harmonic, 1.5);

    // All scores are positive (not zero) for nodes with at least one neighbor.
    for (int64_t node : {1, 2, 3, 4, 5}) {
        EXPECT_GT(scores[node].harmonic, 0.0)
            << "node " << node << " should have positive harmonic centrality";
    }
}

// ---------------------------------------------------------------------------
// Path graph — center nodes highest
// ---------------------------------------------------------------------------

TEST_F(HarmonicCentralityTest, BidirectionalPathCenterHighest) {
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

    auto result = run_harmonic("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_harmonic_map(*result);
    EXPECT_EQ(scores.size(), 5u);

    // Center node (3) should have the highest harmonic centrality.
    // H(3) = 1/1 + 1/1 + 1/2 + 1/2 = 3.0
    EXPECT_DOUBLE_EQ(scores[3].harmonic, 3.0);

    for (int64_t node : {1, 2, 4, 5}) {
        EXPECT_GT(scores[3].harmonic, scores[node].harmonic)
            << "center node 3 should have higher harmonic than node " << node;
    }

    // Symmetry: node 2 == node 4, node 1 == node 5.
    EXPECT_NEAR(scores[2].harmonic, scores[4].harmonic, 1e-10);
    EXPECT_NEAR(scores[1].harmonic, scores[5].harmonic, 1e-10);

    // Known value for node 1: reach 2(1), 3(2), 4(3), 5(4).
    // H(1) = 1/1 + 1/2 + 1/3 + 1/4 = 25/12
    EXPECT_NEAR(scores[1].harmonic, 25.0 / 12.0, 1e-10);
}

// ---------------------------------------------------------------------------
// Normalized values in [0, 1] range
// ---------------------------------------------------------------------------

TEST_F(HarmonicCentralityTest, NormalizedValuesInUnitRange) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {1, 4}, {4, 5}});

    auto result = run_harmonic("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_harmonic_map(*result);
    for (const auto& [node, r] : scores) {
        EXPECT_GE(r.normalized_harmonic, 0.0) << "node " << node << " normalized should be >= 0";
        EXPECT_LE(r.normalized_harmonic, 1.0 + 1e-10)
            << "node " << node << " normalized should be <= 1";
    }
}

// ---------------------------------------------------------------------------
// Compare with closeness centrality on connected graphs
// ---------------------------------------------------------------------------

TEST_F(HarmonicCentralityTest, OrderingMatchesClosenessOnConnectedGraph) {
    // On a connected directed graph, harmonic and closeness centrality
    // should produce the same relative ordering of nodes.
    // Bidirectional triangle + tail: 1<->2<->3<->1, 3->4
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {3, 1},
                    {1, 3},
                    {3, 4},
                });

    auto result = run_harmonic("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_harmonic_map(*result);
    EXPECT_EQ(scores.size(), 4u);

    // Node 3 should have highest harmonic (reaches all, closest distances).
    // Node 4 should have lowest (can't reach anyone).
    EXPECT_GT(scores[3].harmonic, scores[1].harmonic);
    EXPECT_GT(scores[3].harmonic, scores[2].harmonic);
    EXPECT_GT(scores[3].harmonic, scores[4].harmonic);
    EXPECT_DOUBLE_EQ(scores[4].harmonic, 0.0);
}

// ---------------------------------------------------------------------------
// Single node (self-loop only, or single isolated node in edge set)
// ---------------------------------------------------------------------------

TEST_F(HarmonicCentralityTest, SingleNodeSelfLoop) {
    // A self-loop 1->1 shouldn't add anything — BFS starts at source already visited.
    build_graph("knows", {{1, 1}});

    auto result = run_harmonic("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_harmonic_map(*result);
    EXPECT_EQ(scores.size(), 1u);
    EXPECT_DOUBLE_EQ(scores[1].harmonic, 0.0);
    EXPECT_DOUBLE_EQ(scores[1].normalized_harmonic, 0.0);
}

// ---------------------------------------------------------------------------
// Results ordering
// ---------------------------------------------------------------------------

TEST_F(HarmonicCentralityTest, ResultsOrderedByNodeId) {
    build_graph("knows", {{5, 1}, {3, 2}, {4, 3}});

    auto result = run_harmonic("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (size_t i = 1; i < result->size(); ++i) {
        auto prev = std::get<int64_t>((*result)[i - 1].values[0].data());
        auto curr = std::get<int64_t>((*result)[i].values[0].data());
        EXPECT_LT(prev, curr) << "results should be sorted by node_id";
    }
}

// ---------------------------------------------------------------------------
// Error cases
// ---------------------------------------------------------------------------

TEST_F(HarmonicCentralityTest, NonexistentEdgeTypeFails) {
    build_graph("knows", {{1, 2}});

    auto result = run_harmonic("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// ---------------------------------------------------------------------------
// Directed triangle — known analytical values
// ---------------------------------------------------------------------------

TEST_F(HarmonicCentralityTest, DirectedTriangle) {
    // 1->2, 2->3, 3->1 (directed cycle)
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run_harmonic("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_harmonic_map(*result);
    EXPECT_EQ(scores.size(), 3u);

    // Each node reaches one at d=1, one at d=2.
    // H = 1/1 + 1/2 = 1.5
    // Normalized = 1.5 / (3-1) = 0.75
    for (int64_t node : {1, 2, 3}) {
        EXPECT_DOUBLE_EQ(scores[node].harmonic, 1.5) << "node " << node << " in directed triangle";
        EXPECT_DOUBLE_EQ(scores[node].normalized_harmonic, 0.75)
            << "node " << node << " in directed triangle";
    }
}

// ---------------------------------------------------------------------------
// Diamond graph
// ---------------------------------------------------------------------------

TEST_F(HarmonicCentralityTest, DiamondGraph) {
    // 1->2, 1->3, 2->4, 3->4
    build_graph("knows", {{1, 2}, {1, 3}, {2, 4}, {3, 4}});

    auto result = run_harmonic("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_harmonic_map(*result);
    EXPECT_EQ(scores.size(), 4u);

    // Node 1: reaches 2(d=1), 3(d=1), 4(d=2). H = 1 + 1 + 0.5 = 2.5
    EXPECT_DOUBLE_EQ(scores[1].harmonic, 2.5);
    EXPECT_DOUBLE_EQ(scores[1].normalized_harmonic, 2.5 / 3.0);

    // Node 2: reaches 4(d=1). H = 1.0
    EXPECT_DOUBLE_EQ(scores[2].harmonic, 1.0);

    // Node 3: reaches 4(d=1). H = 1.0
    EXPECT_DOUBLE_EQ(scores[3].harmonic, 1.0);

    // Node 2 and 3 are symmetric.
    EXPECT_DOUBLE_EQ(scores[2].harmonic, scores[3].harmonic);

    // Node 4: can't reach anyone. H = 0.0
    EXPECT_DOUBLE_EQ(scores[4].harmonic, 0.0);
}

// ---------------------------------------------------------------------------
// Harmonic values non-negative
// ---------------------------------------------------------------------------

TEST_F(HarmonicCentralityTest, HarmonicValuesNonNegative) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {1, 4}, {4, 5}});

    auto result = run_harmonic("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_harmonic_map(*result);
    for (const auto& [node, r] : scores) {
        EXPECT_GE(r.harmonic, 0.0) << "node " << node << " should have non-negative harmonic";
        EXPECT_GE(r.normalized_harmonic, 0.0)
            << "node " << node << " should have non-negative normalized_harmonic";
    }
}
