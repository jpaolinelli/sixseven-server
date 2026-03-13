#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/eigenvector_centrality.h"
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

/// Result row: (node_id, score, iterations_used).
struct EigenvectorResult {
    double score;
    int64_t iterations_used;
};

/// Extract per-node results from algorithm output rows.
std::unordered_map<int64_t, EigenvectorResult>
to_eigenvector_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, EigenvectorResult> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 3u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto score = std::get<double>(row.values[1].data());
        auto iterations_used = std::get<int64_t>(row.values[2].data());
        result[node_id] = {score, iterations_used};
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Algorithm definition tests
// ---------------------------------------------------------------------------

TEST(EigenvectorCentralityDef, OutputSchema) {
    auto def = make_eigenvector_centrality_def();
    EXPECT_EQ(def.name, "eigenvector");
    ASSERT_EQ(def.output_columns.size(), 3u);
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[1].name, "score");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::FLOAT64);
    EXPECT_EQ(def.output_columns[2].name, "iterations_used");
    EXPECT_EQ(def.output_columns[2].type_id, TypeId::INT64);
}

TEST(EigenvectorCentralityDef, Parameters) {
    auto def = make_eigenvector_centrality_def();
    ASSERT_EQ(def.params.size(), 2u);

    EXPECT_EQ(def.params[0].name, "iterations");
    EXPECT_EQ(def.params[0].type_id, TypeId::INT64);
    EXPECT_FALSE(def.params[0].required);
    ASSERT_TRUE(def.params[0].default_value.has_value());
    EXPECT_EQ(std::get<int64_t>(def.params[0].default_value->data()), 100);

    EXPECT_EQ(def.params[1].name, "tolerance");
    EXPECT_EQ(def.params[1].type_id, TypeId::FLOAT64);
    EXPECT_FALSE(def.params[1].required);
    ASSERT_TRUE(def.params[1].default_value.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(def.params[1].default_value->data()), 1e-6);
}

TEST(EigenvectorCentralityDef, Registration) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_eigenvector_centrality_def(),
                                              eigenvector_centrality_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto* entry = registry.find("eigenvector");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "eigenvector");
}

// ---------------------------------------------------------------------------
// Test fixture with a GraphEngine
// ---------------------------------------------------------------------------

class EigenvectorCentralityTest : public ::testing::Test {
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

    /// Run eigenvector centrality with default parameters.
    Result<std::vector<AlgorithmRow>> run(const std::string& edge_type) {
        AlgorithmContext ctx{engine_, edge_type, {}};
        return eigenvector_centrality_execute(ctx);
    }

    /// Run eigenvector centrality with named args.
    Result<std::vector<AlgorithmRow>>
    run(const std::string& edge_type, std::unordered_map<std::string, Value> args) {
        AlgorithmContext ctx{engine_, edge_type, std::move(args)};
        return eigenvector_centrality_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ---------------------------------------------------------------------------
// Empty and single-edge edge cases
// ---------------------------------------------------------------------------

TEST_F(EigenvectorCentralityTest, EmptyGraph) {
    build_graph("knows", {});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(EigenvectorCentralityTest, SingleEdge) {
    // 1 -> 2 (treated as undirected: 1 -- 2)
    build_graph("knows", {{1, 2}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_eigenvector_map(*result);
    EXPECT_EQ(scores.size(), 2u);

    // Both nodes are symmetric in undirected graph, should have equal scores.
    EXPECT_NEAR(scores[1].score, scores[2].score, 1e-6);
    // Normalised to max = 1.0.
    EXPECT_NEAR(scores[1].score, 1.0, 1e-6);
}

TEST_F(EigenvectorCentralityTest, SingleNodeComponent) {
    // Two edges forming two separate components: {1,2} and {3} via 3->3 self-loop.
    // Use a graph where node 3 only connects to itself via another mechanism.
    // Actually, let's just test a graph with an isolated pair and an isolated node
    // by using two separate edge patterns.
    // Simpler: test a single directed edge. Nodes 1 and 2 form one undirected component.
    build_graph("knows", {{1, 2}, {3, 4}, {5, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_eigenvector_map(*result);
    // Node 5 is a self-loop, forms a single-node undirected component with itself.
    // Actually the self-loop makes node 5 connect to itself, but it's still a single node.
    // The component has size 1, so score = 0.
    EXPECT_EQ(scores.size(), 5u);
    EXPECT_NEAR(scores[5].score, 0.0, 1e-6);
    EXPECT_EQ(scores[5].iterations_used, 0);
}

// ---------------------------------------------------------------------------
// Star graph — central node highest score
// ---------------------------------------------------------------------------

TEST_F(EigenvectorCentralityTest, StarGraphCentralNodeHighest) {
    // Hub = 1, spokes = 2..5. Bidirectional for undirected semantics.
    build_graph("knows", {{1, 2}, {2, 1}, {1, 3}, {3, 1}, {1, 4}, {4, 1}, {1, 5}, {5, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_eigenvector_map(*result);
    EXPECT_EQ(scores.size(), 5u);

    // Hub should have the highest score (normalised to 1.0).
    EXPECT_NEAR(scores[1].score, 1.0, 1e-6);

    // All spokes should have equal scores.
    for (int64_t node : {3, 4, 5}) {
        EXPECT_NEAR(scores[2].score, scores[node].score, 1e-6)
            << "spoke nodes should have equal scores";
    }

    // Hub score > spoke score.
    EXPECT_GT(scores[1].score, scores[2].score);
}

// ---------------------------------------------------------------------------
// Complete graph — all nodes equal score
// ---------------------------------------------------------------------------

TEST_F(EigenvectorCentralityTest, CompleteGraph) {
    // K4 bidirectional.
    build_graph("knows",
                {
                    {1, 2}, {2, 1}, {1, 3}, {3, 1}, {1, 4}, {4, 1},
                    {2, 3}, {3, 2}, {2, 4}, {4, 2}, {3, 4}, {4, 3},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_eigenvector_map(*result);
    EXPECT_EQ(scores.size(), 4u);

    // All nodes should have equal scores, normalised to 1.0.
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_NEAR(scores[node].score, 1.0, 1e-4)
            << "node " << node << " in complete graph should have score ~1.0";
    }
}

// ---------------------------------------------------------------------------
// Path graph — center nodes higher than endpoints
// ---------------------------------------------------------------------------

TEST_F(EigenvectorCentralityTest, PathGraphCenterHigher) {
    // Bidirectional path: 1 <-> 2 <-> 3 <-> 4 <-> 5
    build_graph("knows",
                {
                    {1, 2}, {2, 1},
                    {2, 3}, {3, 2},
                    {3, 4}, {4, 3},
                    {4, 5}, {5, 4},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_eigenvector_map(*result);
    EXPECT_EQ(scores.size(), 5u);

    // Center node (3) should have the highest score.
    EXPECT_NEAR(scores[3].score, 1.0, 1e-6);

    // Symmetry: node 2 == node 4, node 1 == node 5.
    EXPECT_NEAR(scores[2].score, scores[4].score, 1e-6);
    EXPECT_NEAR(scores[1].score, scores[5].score, 1e-6);

    // Center > inner > outer.
    EXPECT_GT(scores[3].score, scores[2].score);
    EXPECT_GT(scores[2].score, scores[1].score);
}

// ---------------------------------------------------------------------------
// Known eigenvector values for K3
// ---------------------------------------------------------------------------

TEST_F(EigenvectorCentralityTest, TriangleKnownValues) {
    // K3 undirected: adjacency matrix eigenvalues are 2, -1, -1.
    // Principal eigenvector: [1/sqrt(3), 1/sqrt(3), 1/sqrt(3)].
    // After normalisation to max=1: all equal to 1.0.
    build_graph("knows", {{1, 2}, {2, 1}, {2, 3}, {3, 2}, {1, 3}, {3, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_eigenvector_map(*result);
    EXPECT_EQ(scores.size(), 3u);

    for (int64_t node : {1, 2, 3}) {
        EXPECT_NEAR(scores[node].score, 1.0, 1e-6)
            << "node " << node << " in K3 should have score 1.0";
    }
}

// ---------------------------------------------------------------------------
// Convergence within specified tolerance
// ---------------------------------------------------------------------------

TEST_F(EigenvectorCentralityTest, ConvergenceWithinTolerance) {
    // K4 should converge quickly.
    build_graph("knows",
                {
                    {1, 2}, {2, 1}, {1, 3}, {3, 1}, {1, 4}, {4, 1},
                    {2, 3}, {3, 2}, {2, 4}, {4, 2}, {3, 4}, {4, 3},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_eigenvector_map(*result);

    // Should converge well before 100 iterations for K4.
    for (const auto& [node, r] : scores) {
        EXPECT_LT(r.iterations_used, 100)
            << "node " << node << " should converge before max iterations";
    }
}

// ---------------------------------------------------------------------------
// Max iterations cutoff behavior
// ---------------------------------------------------------------------------

TEST_F(EigenvectorCentralityTest, MaxIterationsCutoff) {
    // Use a large path graph with only 1 iteration allowed.
    build_graph("knows",
                {
                    {1, 2}, {2, 1},
                    {2, 3}, {3, 2},
                    {3, 4}, {4, 3},
                    {4, 5}, {5, 4},
                });

    std::unordered_map<std::string, Value> args;
    args["iterations"] = Value(static_cast<int64_t>(1));

    auto result = run("knows", args);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_eigenvector_map(*result);

    // With only 1 iteration, it should not have converged (iterations_used == 1).
    for (const auto& [node, r] : scores) {
        EXPECT_EQ(r.iterations_used, 1)
            << "node " << node << " should use exactly 1 iteration";
    }

    // Scores should still be valid (non-negative, normalised).
    for (const auto& [node, r] : scores) {
        EXPECT_GE(r.score, 0.0);
        EXPECT_LE(r.score, 1.0 + 1e-10);
    }
}

// ---------------------------------------------------------------------------
// Disconnected components handled gracefully
// ---------------------------------------------------------------------------

TEST_F(EigenvectorCentralityTest, DisconnectedComponents) {
    // Component A: 1 <-> 2 <-> 3 (path)
    // Component B: 4 <-> 5 (pair)
    build_graph("knows",
                {
                    {1, 2}, {2, 1},
                    {2, 3}, {3, 2},
                    {4, 5}, {5, 4},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_eigenvector_map(*result);
    EXPECT_EQ(scores.size(), 5u);

    // All scores should be valid.
    for (const auto& [node, r] : scores) {
        EXPECT_GE(r.score, 0.0) << "node " << node;
        EXPECT_LE(r.score, 1.0 + 1e-10) << "node " << node;
    }

    // Within component A: center (2) should have highest score.
    EXPECT_GT(scores[2].score, scores[1].score);
    EXPECT_NEAR(scores[1].score, scores[3].score, 1e-6);

    // Within component B: both equal.
    EXPECT_NEAR(scores[4].score, scores[5].score, 1e-6);

    // Global max should be 1.0.
    double max_score = 0.0;
    for (const auto& [node, r] : scores) {
        max_score = std::max(max_score, r.score);
    }
    EXPECT_NEAR(max_score, 1.0, 1e-6);
}

// ---------------------------------------------------------------------------
// Results ordering and error cases
// ---------------------------------------------------------------------------

TEST_F(EigenvectorCentralityTest, ResultsOrderedByNodeId) {
    build_graph("knows", {{5, 1}, {3, 2}, {4, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (size_t i = 1; i < result->size(); ++i) {
        auto prev = std::get<int64_t>((*result)[i - 1].values[0].data());
        auto curr = std::get<int64_t>((*result)[i].values[0].data());
        EXPECT_LT(prev, curr) << "results should be sorted by node_id";
    }
}

TEST_F(EigenvectorCentralityTest, NonexistentEdgeTypeFails) {
    build_graph("knows", {{1, 2}});

    auto result = run("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(EigenvectorCentralityTest, InvalidIterationsFails) {
    build_graph("knows", {{1, 2}});

    std::unordered_map<std::string, Value> args;
    args["iterations"] = Value(static_cast<int64_t>(0));

    auto result = run("knows", args);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(EigenvectorCentralityTest, NegativeToleranceFails) {
    build_graph("knows", {{1, 2}});

    std::unordered_map<std::string, Value> args;
    args["tolerance"] = Value(-1.0);

    auto result = run("knows", args);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Score values in valid range
// ---------------------------------------------------------------------------

TEST_F(EigenvectorCentralityTest, ScoresNonNegativeAndNormalized) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {1, 4}, {4, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_eigenvector_map(*result);
    double max_score = 0.0;
    for (const auto& [node, r] : scores) {
        EXPECT_GE(r.score, 0.0) << "node " << node << " should have non-negative score";
        EXPECT_LE(r.score, 1.0 + 1e-10) << "node " << node << " score should be <= 1.0";
        max_score = std::max(max_score, r.score);
    }
    // At least one node should have score 1.0.
    EXPECT_NEAR(max_score, 1.0, 1e-6);
}

// ---------------------------------------------------------------------------
// Custom tolerance converges
// ---------------------------------------------------------------------------

TEST_F(EigenvectorCentralityTest, CustomToleranceConverges) {
    // Tight tolerance should still converge on a simple graph.
    build_graph("knows", {{1, 2}, {2, 1}, {2, 3}, {3, 2}});

    std::unordered_map<std::string, Value> args;
    args["tolerance"] = Value(1e-10);
    args["iterations"] = Value(static_cast<int64_t>(1000));

    auto result = run("knows", args);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_eigenvector_map(*result);

    // Should converge before 1000 iterations.
    for (const auto& [node, r] : scores) {
        EXPECT_LT(r.iterations_used, 1000)
            << "node " << node << " should converge with tight tolerance";
    }
}

// ---------------------------------------------------------------------------
// Directed edges treated as undirected
// ---------------------------------------------------------------------------

TEST_F(EigenvectorCentralityTest, DirectedEdgesTreatedAsUndirected) {
    // Only one-directional edges: 1->2->3.
    // Eigenvector centrality treats graph as undirected: 1--2--3.
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_eigenvector_map(*result);
    EXPECT_EQ(scores.size(), 3u);

    // Center node (2) should have highest score in undirected path.
    EXPECT_NEAR(scores[2].score, 1.0, 1e-6);

    // Endpoints should be symmetric.
    EXPECT_NEAR(scores[1].score, scores[3].score, 1e-6);
    EXPECT_GT(scores[2].score, scores[1].score);
}
