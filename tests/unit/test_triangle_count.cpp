#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/triangle_count.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
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

/// Extract per-node triangle counts from algorithm output rows.
std::unordered_map<int64_t, int64_t> to_triangle_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, int64_t> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 2u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto count = std::get<int64_t>(row.values[1].data());
        result[node_id] = count;
    }
    return result;
}

/// Compute total graph triangles from per-node counts.
int64_t total_triangles(const std::unordered_map<int64_t, int64_t>& counts) {
    int64_t sum = 0;
    for (const auto& [_, c] : counts) {
        sum += c;
    }
    // Each triangle is counted once per participating node (3 times total).
    return sum / 3;
}

} // namespace

// ---------------------------------------------------------------------------
// Algorithm definition tests
// ---------------------------------------------------------------------------

TEST(TriangleCountDef, OutputSchema) {
    auto def = make_triangle_count_def();
    EXPECT_EQ(def.name, "triangle_count");
    ASSERT_EQ(def.output_columns.size(), 2u);
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[1].name, "triangle_count");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::INT64);
}

TEST(TriangleCountDef, NoParameters) {
    auto def = make_triangle_count_def();
    EXPECT_TRUE(def.params.empty());
}

TEST(TriangleCountDef, Registration) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_triangle_count_def(), triangle_count_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto* entry = registry.find("triangle_count");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "triangle_count");
}

// ---------------------------------------------------------------------------
// Test fixture with a GraphEngine
// ---------------------------------------------------------------------------

class TriangleCountTest : public ::testing::Test {
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

    /// Run triangle count.
    Result<std::vector<AlgorithmRow>> run(const std::string& edge_type) {
        AlgorithmContext ctx{engine_, default_database_id, edge_type, {}};
        return triangle_count_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ---------------------------------------------------------------------------
// Empty graph
// ---------------------------------------------------------------------------

TEST_F(TriangleCountTest, EmptyGraph) {
    build_graph("knows", {});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

// ---------------------------------------------------------------------------
// Single triangle — each node count = 1, total = 1
// ---------------------------------------------------------------------------

TEST_F(TriangleCountTest, SingleTriangle) {
    // 1->2, 2->3, 3->1 (forms a triangle in undirected view)
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto counts = to_triangle_map(*result);
    EXPECT_EQ(counts.size(), 3u);

    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(counts[node], 1) << "node " << node << " should participate in 1 triangle";
    }
    EXPECT_EQ(total_triangles(counts), 1);
}

// ---------------------------------------------------------------------------
// Complete graph K4 — each node = 3, total = 4
// ---------------------------------------------------------------------------

TEST_F(TriangleCountTest, CompleteGraphK4) {
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

    auto counts = to_triangle_map(*result);
    EXPECT_EQ(counts.size(), 4u);

    // Each node in K4 participates in C(3,2) = 3 triangles.
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_EQ(counts[node], 3) << "node " << node << " in K4";
    }
    // Total triangles in K4 = C(4,3) = 4.
    EXPECT_EQ(total_triangles(counts), 4);
}

// ---------------------------------------------------------------------------
// Complete graph K5 — each node = 6, total = 10
// ---------------------------------------------------------------------------

TEST_F(TriangleCountTest, CompleteGraphK5) {
    build_graph("knows",
                {
                    {1, 2}, {1, 3}, {1, 4}, {1, 5}, {2, 1}, {2, 3}, {2, 4}, {2, 5}, {3, 1}, {3, 2},
                    {3, 4}, {3, 5}, {4, 1}, {4, 2}, {4, 3}, {4, 5}, {5, 1}, {5, 2}, {5, 3}, {5, 4},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto counts = to_triangle_map(*result);
    EXPECT_EQ(counts.size(), 5u);

    // Each node in K5 participates in C(4,2) = 6 triangles.
    for (int64_t node : {1, 2, 3, 4, 5}) {
        EXPECT_EQ(counts[node], 6) << "node " << node << " in K5";
    }
    // Total triangles in K5 = C(5,3) = 10.
    EXPECT_EQ(total_triangles(counts), 10);
}

// ---------------------------------------------------------------------------
// Star graph — 0 triangles
// ---------------------------------------------------------------------------

TEST_F(TriangleCountTest, StarGraph) {
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

    auto counts = to_triangle_map(*result);
    EXPECT_EQ(counts.size(), 5u);

    for (int64_t node : {1, 2, 3, 4, 5}) {
        EXPECT_EQ(counts[node], 0) << "star graph node " << node;
    }
    EXPECT_EQ(total_triangles(counts), 0);
}

// ---------------------------------------------------------------------------
// Path graph — 0 triangles
// ---------------------------------------------------------------------------

TEST_F(TriangleCountTest, PathGraph) {
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

    auto counts = to_triangle_map(*result);
    EXPECT_EQ(counts.size(), 4u);

    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_EQ(counts[node], 0) << "path graph node " << node;
    }
    EXPECT_EQ(total_triangles(counts), 0);
}

// ---------------------------------------------------------------------------
// Two connected triangles sharing an edge (bowtie without center)
// ---------------------------------------------------------------------------

TEST_F(TriangleCountTest, TwoTrianglesSharingEdge) {
    // Triangle 1: {1, 2, 3}, Triangle 2: {2, 3, 4}
    // Shared edge: 2-3
    build_graph("knows",
                {
                    {1, 2},
                    {2, 1},
                    {2, 3},
                    {3, 2},
                    {3, 1},
                    {1, 3},
                    {2, 4},
                    {4, 2},
                    {3, 4},
                    {4, 3},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto counts = to_triangle_map(*result);
    EXPECT_EQ(counts.size(), 4u);

    // Node 1: part of triangle {1,2,3} only => 1
    EXPECT_EQ(counts[1], 1);
    // Node 2: part of {1,2,3} and {2,3,4} => 2
    EXPECT_EQ(counts[2], 2);
    // Node 3: part of {1,2,3} and {2,3,4} => 2
    EXPECT_EQ(counts[3], 2);
    // Node 4: part of {2,3,4} only => 1
    EXPECT_EQ(counts[4], 1);

    EXPECT_EQ(total_triangles(counts), 2);
}

// ---------------------------------------------------------------------------
// Directed triangle counting — directed edges still form undirected triangle
// ---------------------------------------------------------------------------

TEST_F(TriangleCountTest, DirectedTriangle) {
    // Directed cycle: 1->2, 2->3, 3->1 (no reverse edges)
    // Undirected view: 1-2, 2-3, 3-1 forms a triangle.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto counts = to_triangle_map(*result);
    EXPECT_EQ(counts.size(), 3u);

    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(counts[node], 1) << "node " << node;
    }
    EXPECT_EQ(total_triangles(counts), 1);
}

// ---------------------------------------------------------------------------
// Degree-ordering optimization correctness
// Verify same results regardless of edge insertion order
// ---------------------------------------------------------------------------

TEST_F(TriangleCountTest, DegreeOrderingCorrectness) {
    // Diamond graph: same as TwoTrianglesSharingEdge but different edge order
    build_graph("knows",
                {
                    {4, 3},
                    {3, 4},
                    {3, 2},
                    {2, 3},
                    {4, 2},
                    {2, 4},
                    {1, 3},
                    {3, 1},
                    {1, 2},
                    {2, 1},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto counts = to_triangle_map(*result);
    EXPECT_EQ(counts.size(), 4u);

    // Same graph as TwoTrianglesSharingEdge — same results expected.
    EXPECT_EQ(counts[1], 1);
    EXPECT_EQ(counts[2], 2);
    EXPECT_EQ(counts[3], 2);
    EXPECT_EQ(counts[4], 1);
    EXPECT_EQ(total_triangles(counts), 2);
}

// ---------------------------------------------------------------------------
// Single node with self-loop — 0 triangles
// ---------------------------------------------------------------------------

TEST_F(TriangleCountTest, SingleNodeSelfLoop) {
    build_graph("knows", {{1, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto counts = to_triangle_map(*result);
    EXPECT_EQ(counts.size(), 1u);
    EXPECT_EQ(counts[1], 0);
}

// ---------------------------------------------------------------------------
// Self-loop does not affect triangle counting
// ---------------------------------------------------------------------------

TEST_F(TriangleCountTest, SelfLoopIgnored) {
    // Triangle 1-2-3 plus self-loop on node 1
    build_graph("knows", {{1, 1}, {1, 2}, {2, 3}, {3, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto counts = to_triangle_map(*result);

    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(counts[node], 1) << "node " << node;
    }
    EXPECT_EQ(total_triangles(counts), 1);
}

// ---------------------------------------------------------------------------
// Results ordered by node_id
// ---------------------------------------------------------------------------

TEST_F(TriangleCountTest, ResultsOrderedByNodeId) {
    build_graph("knows", {{5, 1}, {3, 2}, {4, 3}, {1, 3}, {3, 5}});

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

TEST_F(TriangleCountTest, NonexistentEdgeTypeFails) {
    build_graph("knows", {{1, 2}});

    auto result = run("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// ---------------------------------------------------------------------------
// Bowtie graph — two triangles sharing a single node
// ---------------------------------------------------------------------------

TEST_F(TriangleCountTest, BowTieGraph) {
    // Triangle {1,2,3} and {3,4,5}, sharing node 3
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

    auto counts = to_triangle_map(*result);
    EXPECT_EQ(counts.size(), 5u);

    // Nodes 1, 2: part of {1,2,3} only => 1
    EXPECT_EQ(counts[1], 1);
    EXPECT_EQ(counts[2], 1);
    // Node 3: part of {1,2,3} and {3,4,5} => 2
    EXPECT_EQ(counts[3], 2);
    // Nodes 4, 5: part of {3,4,5} only => 1
    EXPECT_EQ(counts[4], 1);
    EXPECT_EQ(counts[5], 1);

    EXPECT_EQ(total_triangles(counts), 2);
}

// ---------------------------------------------------------------------------
// Non-negative triangle counts
// ---------------------------------------------------------------------------

TEST_F(TriangleCountTest, CountsAreNonNegative) {
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

    auto counts = to_triangle_map(*result);
    for (const auto& [node, c] : counts) {
        EXPECT_GE(c, 0) << "node " << node << " count must be non-negative";
    }
}
