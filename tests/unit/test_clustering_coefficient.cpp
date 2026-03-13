#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/clustering_coefficient.h"
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

/// Result row: (node_id, local_coefficient, triangles, degree).
struct ClusteringResult {
    double local_coefficient;
    int64_t triangles;
    int64_t degree;
};

/// Extract per-node results from algorithm output rows.
std::unordered_map<int64_t, ClusteringResult>
to_clustering_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, ClusteringResult> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 4u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto local_coefficient = std::get<double>(row.values[1].data());
        auto triangles = std::get<int64_t>(row.values[2].data());
        auto degree = std::get<int64_t>(row.values[3].data());
        result[node_id] = {local_coefficient, triangles, degree};
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Algorithm definition tests
// ---------------------------------------------------------------------------

TEST(ClusteringCoefficientDef, OutputSchema) {
    auto def = make_clustering_coefficient_def();
    EXPECT_EQ(def.name, "clustering_coefficient");
    ASSERT_EQ(def.output_columns.size(), 4u);
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[1].name, "local_coefficient");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::FLOAT64);
    EXPECT_EQ(def.output_columns[2].name, "triangles");
    EXPECT_EQ(def.output_columns[2].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[3].name, "degree");
    EXPECT_EQ(def.output_columns[3].type_id, TypeId::INT64);
}

TEST(ClusteringCoefficientDef, NoParameters) {
    auto def = make_clustering_coefficient_def();
    EXPECT_TRUE(def.params.empty());
}

TEST(ClusteringCoefficientDef, Registration) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_clustering_coefficient_def(),
                                              clustering_coefficient_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto* entry = registry.find("clustering_coefficient");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "clustering_coefficient");
}

// ---------------------------------------------------------------------------
// Test fixture with a GraphEngine
// ---------------------------------------------------------------------------

class ClusteringCoefficientTest : public ::testing::Test {
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

    /// Run clustering coefficient.
    Result<std::vector<AlgorithmRow>> run(const std::string& edge_type) {
        AlgorithmContext ctx{engine_, edge_type, {}};
        return clustering_coefficient_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ---------------------------------------------------------------------------
// Empty graph
// ---------------------------------------------------------------------------

TEST_F(ClusteringCoefficientTest, EmptyGraph) {
    build_graph("knows", {});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

// ---------------------------------------------------------------------------
// Single edge — both nodes have degree 1, coefficient = 0
// ---------------------------------------------------------------------------

TEST_F(ClusteringCoefficientTest, SingleEdge) {
    // 1 -> 2 (undirected view: both have degree 1)
    build_graph("knows", {{1, 2}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_clustering_map(*result);
    EXPECT_EQ(scores.size(), 2u);

    // Degree 1 < 2, so coefficient = 0 for both.
    EXPECT_DOUBLE_EQ(scores[1].local_coefficient, 0.0);
    EXPECT_EQ(scores[1].degree, 1);
    EXPECT_EQ(scores[1].triangles, 0);

    EXPECT_DOUBLE_EQ(scores[2].local_coefficient, 0.0);
    EXPECT_EQ(scores[2].degree, 1);
    EXPECT_EQ(scores[2].triangles, 0);
}

// ---------------------------------------------------------------------------
// Triangle graph — all coefficients = 1.0
// ---------------------------------------------------------------------------

TEST_F(ClusteringCoefficientTest, Triangle) {
    // 1->2, 2->3, 3->1 (forms a triangle in undirected view)
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_clustering_map(*result);
    EXPECT_EQ(scores.size(), 3u);

    // Each node has degree 2, one triangle. C = 2*1 / (2*1) = 1.0
    for (int64_t node : {1, 2, 3}) {
        EXPECT_DOUBLE_EQ(scores[node].local_coefficient, 1.0) << "node " << node;
        EXPECT_EQ(scores[node].triangles, 1) << "node " << node;
        EXPECT_EQ(scores[node].degree, 2) << "node " << node;
    }
}

// ---------------------------------------------------------------------------
// Complete graph K4 — all coefficients = 1.0
// ---------------------------------------------------------------------------

TEST_F(ClusteringCoefficientTest, CompleteGraph) {
    // K4 bidirectional: all pairs connected.
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

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_clustering_map(*result);
    EXPECT_EQ(scores.size(), 4u);

    // Each node has degree 3, 3 triangles. C = 2*3 / (3*2) = 1.0
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_DOUBLE_EQ(scores[node].local_coefficient, 1.0)
            << "node " << node << " in K4 should have coefficient 1.0";
        EXPECT_EQ(scores[node].triangles, 3) << "node " << node;
        EXPECT_EQ(scores[node].degree, 3) << "node " << node;
    }
}

// ---------------------------------------------------------------------------
// Star graph — central node = 0 (neighbors not connected), leaves = 0 (degree 1)
// ---------------------------------------------------------------------------

TEST_F(ClusteringCoefficientTest, StarGraph) {
    // Hub = 1, spokes = 2,3,4,5 (bidirectional)
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

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_clustering_map(*result);
    EXPECT_EQ(scores.size(), 5u);

    // Hub (1): degree 4, 0 triangles. C = 0/12 = 0
    EXPECT_DOUBLE_EQ(scores[1].local_coefficient, 0.0);
    EXPECT_EQ(scores[1].triangles, 0);
    EXPECT_EQ(scores[1].degree, 4);

    // Spokes: degree 1, coefficient = 0
    for (int64_t node : {2, 3, 4, 5}) {
        EXPECT_DOUBLE_EQ(scores[node].local_coefficient, 0.0) << "leaf node " << node;
        EXPECT_EQ(scores[node].degree, 1) << "leaf node " << node;
        EXPECT_EQ(scores[node].triangles, 0) << "leaf node " << node;
    }
}

// ---------------------------------------------------------------------------
// Path graph — no triangles, all coefficients = 0
// ---------------------------------------------------------------------------

TEST_F(ClusteringCoefficientTest, PathGraph) {
    // 1 <-> 2 <-> 3 <-> 4
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {3, 4},
                    {4, 3},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_clustering_map(*result);
    EXPECT_EQ(scores.size(), 4u);

    // No triangles in a path graph.
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_DOUBLE_EQ(scores[node].local_coefficient, 0.0) << "node " << node;
        EXPECT_EQ(scores[node].triangles, 0) << "node " << node;
    }

    // End nodes have degree 1, middle nodes have degree 2.
    EXPECT_EQ(scores[1].degree, 1);
    EXPECT_EQ(scores[2].degree, 2);
    EXPECT_EQ(scores[3].degree, 2);
    EXPECT_EQ(scores[4].degree, 1);
}

// ---------------------------------------------------------------------------
// Node with degree < 2 returns coefficient 0
// ---------------------------------------------------------------------------

TEST_F(ClusteringCoefficientTest, DegreeLessThanTwo) {
    // Node 3 only connected to 1 (degree 1). Nodes 1,2 form a triangle with 3.
    // Actually: 1->2, 2->1, 1->3 => undirected: 1-2, 1-3. Node 3 has degree 1.
    build_graph("knows", {{1, 2}, {2, 1}, {1, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_clustering_map(*result);

    // Node 3 has degree 1 => coefficient 0.
    EXPECT_DOUBLE_EQ(scores[3].local_coefficient, 0.0);
    EXPECT_EQ(scores[3].degree, 1);
}

// ---------------------------------------------------------------------------
// Known analytical coefficients — "bowtie" graph
// ---------------------------------------------------------------------------

TEST_F(ClusteringCoefficientTest, KnownAnalyticalCoefficients) {
    // Two triangles sharing node 3: {1,2,3} and {3,4,5}
    // All bidirectional.
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {3, 1},
                    {1, 3},
                    {3, 4},
                    {4, 3},
                    {4, 5},
                    {5, 4},
                    {5, 3},
                    {3, 5},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_clustering_map(*result);
    EXPECT_EQ(scores.size(), 5u);

    // Nodes 1, 2: degree 2, 1 triangle. C = 2*1 / (2*1) = 1.0
    EXPECT_DOUBLE_EQ(scores[1].local_coefficient, 1.0);
    EXPECT_EQ(scores[1].triangles, 1);
    EXPECT_EQ(scores[1].degree, 2);

    EXPECT_DOUBLE_EQ(scores[2].local_coefficient, 1.0);
    EXPECT_EQ(scores[2].triangles, 1);

    // Node 3: degree 4, 2 triangles. C = 2*2 / (4*3) = 4/12 = 1/3
    EXPECT_NEAR(scores[3].local_coefficient, 1.0 / 3.0, 1e-10);
    EXPECT_EQ(scores[3].triangles, 2);
    EXPECT_EQ(scores[3].degree, 4);

    // Nodes 4, 5: degree 2, 1 triangle. C = 1.0
    EXPECT_DOUBLE_EQ(scores[4].local_coefficient, 1.0);
    EXPECT_DOUBLE_EQ(scores[5].local_coefficient, 1.0);
}

// ---------------------------------------------------------------------------
// Triangle count accuracy — verify exact counts
// ---------------------------------------------------------------------------

TEST_F(ClusteringCoefficientTest, TriangleCountAccuracy) {
    // K5 complete graph: each node participates in C(4,2) = 6 triangles.
    build_graph("knows",
                {
                    {1, 2}, {1, 3}, {1, 4}, {1, 5}, {2, 1}, {2, 3}, {2, 4}, {2, 5}, {3, 1}, {3, 2},
                    {3, 4}, {3, 5}, {4, 1}, {4, 2}, {4, 3}, {4, 5}, {5, 1}, {5, 2}, {5, 3}, {5, 4},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_clustering_map(*result);
    EXPECT_EQ(scores.size(), 5u);

    // In K5, each node has degree 4 and 6 triangles. C = 2*6/(4*3) = 1.0
    for (int64_t node : {1, 2, 3, 4, 5}) {
        EXPECT_EQ(scores[node].triangles, 6) << "node " << node << " in K5";
        EXPECT_EQ(scores[node].degree, 4) << "node " << node;
        EXPECT_DOUBLE_EQ(scores[node].local_coefficient, 1.0) << "node " << node;
    }
}

// ---------------------------------------------------------------------------
// Directed graph behavior — uses total (undirected) degree
// ---------------------------------------------------------------------------

TEST_F(ClusteringCoefficientTest, DirectedGraphBehavior) {
    // Directed triangle: 1->2, 2->3, 3->1
    // Undirected view: 1-2, 2-3, 3-1 — forms triangle. All coefficients = 1.0
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_clustering_map(*result);
    EXPECT_EQ(scores.size(), 3u);

    for (int64_t node : {1, 2, 3}) {
        EXPECT_DOUBLE_EQ(scores[node].local_coefficient, 1.0) << "node " << node;
        EXPECT_EQ(scores[node].degree, 2) << "node " << node;
        EXPECT_EQ(scores[node].triangles, 1) << "node " << node;
    }
}

// ---------------------------------------------------------------------------
// Self-loop — should not affect coefficient
// ---------------------------------------------------------------------------

TEST_F(ClusteringCoefficientTest, SelfLoop) {
    // 1->1 self-loop, plus triangle 1->2, 2->3, 3->1
    build_graph("knows", {{1, 1}, {1, 2}, {2, 3}, {3, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_clustering_map(*result);

    // Self-loop should not inflate degree or triangles.
    // Triangle still present: 1-2, 2-3, 3-1.
    for (int64_t node : {1, 2, 3}) {
        EXPECT_DOUBLE_EQ(scores[node].local_coefficient, 1.0) << "node " << node;
        EXPECT_EQ(scores[node].degree, 2) << "node " << node;
        EXPECT_EQ(scores[node].triangles, 1) << "node " << node;
    }
}

// ---------------------------------------------------------------------------
// Coefficients in [0, 1] range
// ---------------------------------------------------------------------------

TEST_F(ClusteringCoefficientTest, CoefficientsInUnitRange) {
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {3, 1},
                    {1, 3},
                    {1, 4},
                    {4, 5},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_clustering_map(*result);
    for (const auto& [node, r] : scores) {
        EXPECT_GE(r.local_coefficient, 0.0) << "node " << node << " coefficient >= 0";
        EXPECT_LE(r.local_coefficient, 1.0 + 1e-10) << "node " << node << " coefficient <= 1";
    }
}

// ---------------------------------------------------------------------------
// Results ordered by node_id
// ---------------------------------------------------------------------------

TEST_F(ClusteringCoefficientTest, ResultsOrderedByNodeId) {
    build_graph("knows", {{5, 1}, {3, 2}, {4, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (size_t i = 1; i < result->size(); ++i) {
        auto prev = std::get<int64_t>((*result)[i - 1].values[0].data());
        auto curr = std::get<int64_t>((*result)[i].values[0].data());
        EXPECT_LT(prev, curr) << "results should be sorted by node_id";
    }
}

// ---------------------------------------------------------------------------
// Nonexistent edge type fails
// ---------------------------------------------------------------------------

TEST_F(ClusteringCoefficientTest, NonexistentEdgeTypeFails) {
    build_graph("knows", {{1, 2}});

    auto result = run("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// ---------------------------------------------------------------------------
// Diamond graph — known mixed coefficients
// ---------------------------------------------------------------------------

TEST_F(ClusteringCoefficientTest, DiamondGraph) {
    // Diamond: 1-2, 1-3, 2-4, 3-4, 2-3 (bidirectional)
    // This makes triangles: {1,2,3} and {2,3,4}
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {1, 3},
                    {3, 1},
                    {2, 3},
                    {3, 2},
                    {2, 4},
                    {4, 2},
                    {3, 4},
                    {4, 3},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_clustering_map(*result);
    EXPECT_EQ(scores.size(), 4u);

    // Node 1: degree 2, neighbors {2,3}. 2-3 connected => 1 triangle. C = 2*1/(2*1) = 1.0
    EXPECT_DOUBLE_EQ(scores[1].local_coefficient, 1.0);
    EXPECT_EQ(scores[1].triangles, 1);

    // Node 2: degree 3, neighbors {1,3,4}. Pairs: 1-3(yes), 1-4(no), 3-4(yes) => 2 triangles.
    // C = 2*2/(3*2) = 4/6 = 2/3
    EXPECT_NEAR(scores[2].local_coefficient, 2.0 / 3.0, 1e-10);
    EXPECT_EQ(scores[2].triangles, 2);

    // Node 3: degree 3, same as node 2. C = 2/3
    EXPECT_NEAR(scores[3].local_coefficient, 2.0 / 3.0, 1e-10);
    EXPECT_EQ(scores[3].triangles, 2);

    // Node 4: degree 2, neighbors {2,3}. 2-3 connected => 1 triangle. C = 1.0
    EXPECT_DOUBLE_EQ(scores[4].local_coefficient, 1.0);
    EXPECT_EQ(scores[4].triangles, 1);
}

// ---------------------------------------------------------------------------
// Isolated node (appears as self-loop only)
// ---------------------------------------------------------------------------

TEST_F(ClusteringCoefficientTest, IsolatedSelfLoopNode) {
    build_graph("knows", {{1, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_clustering_map(*result);
    EXPECT_EQ(scores.size(), 1u);

    EXPECT_DOUBLE_EQ(scores[1].local_coefficient, 0.0);
    EXPECT_EQ(scores[1].degree, 0);
    EXPECT_EQ(scores[1].triangles, 0);
}
