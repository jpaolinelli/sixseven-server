#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/degree_centrality.h"
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

/// Degree info for a single node.
struct DegreeInfo {
    int64_t in_degree;
    int64_t out_degree;
    int64_t degree;
    double normalized_degree;
};

/// Extract (node_id, DegreeInfo) from algorithm result rows.
std::unordered_map<int64_t, DegreeInfo> to_degree_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, DegreeInfo> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 5u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto in_deg = std::get<int64_t>(row.values[1].data());
        auto out_deg = std::get<int64_t>(row.values[2].data());
        auto deg = std::get<int64_t>(row.values[3].data());
        auto norm = std::get<double>(row.values[4].data());
        result[node_id] = {in_deg, out_deg, deg, norm};
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Algorithm definition tests
// ---------------------------------------------------------------------------

TEST(DegreeCentralityDef, OutputSchema) {
    auto def = make_degree_centrality_def();
    EXPECT_EQ(def.name, "degree_centrality");
    ASSERT_EQ(def.output_columns.size(), 5u);
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[1].name, "in_degree");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[2].name, "out_degree");
    EXPECT_EQ(def.output_columns[2].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[3].name, "degree");
    EXPECT_EQ(def.output_columns[3].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[4].name, "normalized_degree");
    EXPECT_EQ(def.output_columns[4].type_id, TypeId::FLOAT64);
}

TEST(DegreeCentralityDef, Parameters) {
    auto def = make_degree_centrality_def();
    ASSERT_EQ(def.params.size(), 1u);

    EXPECT_EQ(def.params[0].name, "direction");
    EXPECT_EQ(def.params[0].type_id, TypeId::STRING);
    EXPECT_FALSE(def.params[0].required);
    ASSERT_TRUE(def.params[0].default_value.has_value());
    EXPECT_EQ(std::get<std::string>(def.params[0].default_value->data()), "all");
}

TEST(DegreeCentralityDef, Registration) {
    AlgorithmRegistry registry;
    auto result =
        registry.register_algorithm(make_degree_centrality_def(), degree_centrality_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto* entry = registry.find("degree_centrality");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "degree_centrality");
}

// ---------------------------------------------------------------------------
// Test fixture with a GraphEngine
// ---------------------------------------------------------------------------

class DegreeCentralityTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto t = catalog_.create_table(default_database_id, make_table_schema("nodes"));
        ASSERT_TRUE(t.has_value()) << t.error().message;
        table_id_ = *t;
    }

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

    Result<std::vector<AlgorithmRow>> run(const std::string& edge_type,
                                          const std::string& direction = "all") {
        AlgorithmContext ctx{engine_, edge_type, {{"direction", Value(std::string(direction))}}};
        return degree_centrality_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ---------------------------------------------------------------------------
// Execution tests
// ---------------------------------------------------------------------------

TEST_F(DegreeCentralityTest, EmptyGraph) {
    build_graph("knows", {});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(DegreeCentralityTest, SingleEdge) {
    // 1 -> 2
    build_graph("knows", {{1, 2}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), 2u);

    // Node 1: out=1, in=0, total=1
    EXPECT_EQ(degrees[1].in_degree, 0);
    EXPECT_EQ(degrees[1].out_degree, 1);
    EXPECT_EQ(degrees[1].degree, 1);

    // Node 2: out=0, in=1, total=1
    EXPECT_EQ(degrees[2].in_degree, 1);
    EXPECT_EQ(degrees[2].out_degree, 0);
    EXPECT_EQ(degrees[2].degree, 1);

    // Normalized: 1 / (2-1) = 1.0
    EXPECT_DOUBLE_EQ(degrees[1].normalized_degree, 1.0);
    EXPECT_DOUBLE_EQ(degrees[2].normalized_degree, 1.0);
}

TEST_F(DegreeCentralityTest, StarGraph) {
    // 2, 3, 4, 5 all point to 1 (hub)
    build_graph("knows", {{2, 1}, {3, 1}, {4, 1}, {5, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), 5u);

    // Hub node 1: in=4, out=0, total=4
    EXPECT_EQ(degrees[1].in_degree, 4);
    EXPECT_EQ(degrees[1].out_degree, 0);
    EXPECT_EQ(degrees[1].degree, 4);

    // Leaf nodes: in=0, out=1, total=1
    for (int64_t leaf : {2, 3, 4, 5}) {
        EXPECT_EQ(degrees[leaf].in_degree, 0);
        EXPECT_EQ(degrees[leaf].out_degree, 1);
        EXPECT_EQ(degrees[leaf].degree, 1);
    }

    // Hub has max degree
    for (int64_t leaf : {2, 3, 4, 5}) {
        EXPECT_GT(degrees[1].degree, degrees[leaf].degree);
    }

    // Normalized: hub = 4/(5-1) = 1.0, leaf = 1/(5-1) = 0.25
    EXPECT_DOUBLE_EQ(degrees[1].normalized_degree, 1.0);
    EXPECT_DOUBLE_EQ(degrees[2].normalized_degree, 0.25);
}

TEST_F(DegreeCentralityTest, CompleteGraph) {
    // K4: all nodes connected to each other (directed both ways)
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

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), 4u);

    // All nodes should have equal degree: in=3, out=3, total=6
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_EQ(degrees[node].in_degree, 3);
        EXPECT_EQ(degrees[node].out_degree, 3);
        EXPECT_EQ(degrees[node].degree, 6);
        // Normalized: 6 / (4-1) = 2.0
        EXPECT_DOUBLE_EQ(degrees[node].normalized_degree, 2.0);
    }
}

TEST_F(DegreeCentralityTest, DirectedGraphAsymmetric) {
    // 1 -> 2, 1 -> 3, 2 -> 3
    build_graph("knows", {{1, 2}, {1, 3}, {2, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), 3u);

    // Node 1: in=0, out=2
    EXPECT_EQ(degrees[1].in_degree, 0);
    EXPECT_EQ(degrees[1].out_degree, 2);
    EXPECT_EQ(degrees[1].degree, 2);

    // Node 2: in=1, out=1
    EXPECT_EQ(degrees[2].in_degree, 1);
    EXPECT_EQ(degrees[2].out_degree, 1);
    EXPECT_EQ(degrees[2].degree, 2);

    // Node 3: in=2, out=0
    EXPECT_EQ(degrees[3].in_degree, 2);
    EXPECT_EQ(degrees[3].out_degree, 0);
    EXPECT_EQ(degrees[3].degree, 2);
}

TEST_F(DegreeCentralityTest, IsolatedNode) {
    // 1 -> 2, node 3 is isolated (appears only as target of edge from 2)
    // Actually, to get an isolated node we need it to appear in an edge.
    // A node with degree 0 = a node that's a source with no incoming edges
    // and no outgoing to other connected nodes -- but in this framework, nodes
    // only appear if they're in edges. So let's test a "sink" node (in only).
    build_graph("knows", {{1, 2}, {1, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);

    // Node 1: source only, in=0
    EXPECT_EQ(degrees[1].in_degree, 0);
    EXPECT_EQ(degrees[1].out_degree, 2);

    // Node 2 and 3: sink only, out=0
    EXPECT_EQ(degrees[2].out_degree, 0);
    EXPECT_EQ(degrees[3].out_degree, 0);
}

TEST_F(DegreeCentralityTest, DirectionIn) {
    // 1 -> 2, 1 -> 3, 2 -> 3
    build_graph("knows", {{1, 2}, {1, 3}, {2, 3}});

    auto result = run("knows", "in");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);

    // degree column should equal in_degree when direction = "in"
    EXPECT_EQ(degrees[1].degree, degrees[1].in_degree);
    EXPECT_EQ(degrees[2].degree, degrees[2].in_degree);
    EXPECT_EQ(degrees[3].degree, degrees[3].in_degree);

    EXPECT_EQ(degrees[1].degree, 0);
    EXPECT_EQ(degrees[2].degree, 1);
    EXPECT_EQ(degrees[3].degree, 2);

    // Normalized: degree / (N-1) = degree / 2
    EXPECT_DOUBLE_EQ(degrees[1].normalized_degree, 0.0);
    EXPECT_DOUBLE_EQ(degrees[2].normalized_degree, 0.5);
    EXPECT_DOUBLE_EQ(degrees[3].normalized_degree, 1.0);
}

TEST_F(DegreeCentralityTest, DirectionOut) {
    // 1 -> 2, 1 -> 3, 2 -> 3
    build_graph("knows", {{1, 2}, {1, 3}, {2, 3}});

    auto result = run("knows", "out");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);

    // degree column should equal out_degree when direction = "out"
    EXPECT_EQ(degrees[1].degree, degrees[1].out_degree);
    EXPECT_EQ(degrees[2].degree, degrees[2].out_degree);
    EXPECT_EQ(degrees[3].degree, degrees[3].out_degree);

    EXPECT_EQ(degrees[1].degree, 2);
    EXPECT_EQ(degrees[2].degree, 1);
    EXPECT_EQ(degrees[3].degree, 0);

    // Normalized: degree / (N-1) = degree / 2
    EXPECT_DOUBLE_EQ(degrees[1].normalized_degree, 1.0);
    EXPECT_DOUBLE_EQ(degrees[2].normalized_degree, 0.5);
    EXPECT_DOUBLE_EQ(degrees[3].normalized_degree, 0.0);
}

TEST_F(DegreeCentralityTest, DirectionAll) {
    // Explicit "all" should give in + out
    build_graph("knows", {{1, 2}, {1, 3}, {2, 3}});

    auto result = run("knows", "all");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);

    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(degrees[node].degree, degrees[node].in_degree + degrees[node].out_degree);
    }
}

TEST_F(DegreeCentralityTest, NormalizedDegree) {
    // 5 nodes: 1->2, 1->3, 1->4, 1->5
    build_graph("knows", {{1, 2}, {1, 3}, {1, 4}, {1, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), 5u);

    // N=5, so norm_divisor = 4
    // Node 1: degree=4, normalized=4/4=1.0
    EXPECT_DOUBLE_EQ(degrees[1].normalized_degree, 1.0);

    // Leaf nodes: degree=1, normalized=1/4=0.25
    for (int64_t leaf : {2, 3, 4, 5}) {
        EXPECT_DOUBLE_EQ(degrees[leaf].normalized_degree, 0.25);
    }
}

TEST_F(DegreeCentralityTest, SingleNodeGraph) {
    // Self-loop: node 1 -> node 1
    build_graph("knows", {{1, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), 1u);

    // Self-loop: in=1, out=1, total=2
    EXPECT_EQ(degrees[1].in_degree, 1);
    EXPECT_EQ(degrees[1].out_degree, 1);
    EXPECT_EQ(degrees[1].degree, 2);

    // N=1, norm_divisor=1 (avoid division by zero), normalized=2/1=2.0
    EXPECT_DOUBLE_EQ(degrees[1].normalized_degree, 2.0);
}

TEST_F(DegreeCentralityTest, ResultsOrderedByNodeId) {
    build_graph("knows", {{5, 3}, {3, 1}, {4, 2}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (size_t i = 1; i < result->size(); ++i) {
        auto prev = std::get<int64_t>((*result)[i - 1].values[0].data());
        auto curr = std::get<int64_t>((*result)[i].values[0].data());
        EXPECT_LT(prev, curr) << "results should be sorted by node_id";
    }
}

TEST_F(DegreeCentralityTest, NonexistentEdgeTypeFails) {
    build_graph("knows", {{1, 2}});

    auto result = run("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(DegreeCentralityTest, InvalidDirectionFails) {
    build_graph("knows", {{1, 2}});

    auto result = run("knows", "invalid");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(DegreeCentralityTest, InDegreeAndOutDegreeAlwaysPresent) {
    // Even with direction="in", in_degree and out_degree columns still have correct values.
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run("knows", "in");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);

    // in_degree and out_degree should still be fully computed
    EXPECT_EQ(degrees[1].out_degree, 1);
    EXPECT_EQ(degrees[2].in_degree, 1);
    EXPECT_EQ(degrees[2].out_degree, 1);
    EXPECT_EQ(degrees[3].in_degree, 1);
    EXPECT_EQ(degrees[3].out_degree, 0);
}

TEST_F(DegreeCentralityTest, MultipleEdgesSameNodes) {
    // Multiple edges between same pair: 1->2 twice
    build_graph("knows", {{1, 2}, {1, 2}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), 2u);

    // Counts should reflect all edges (multigraph)
    EXPECT_EQ(degrees[1].out_degree, 2);
    EXPECT_EQ(degrees[2].in_degree, 2);
}
