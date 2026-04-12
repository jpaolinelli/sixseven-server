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

/// Check that nodes in the same expected group share the same community_id,
/// and nodes in different groups have different community_ids.
void verify_communities(const std::unordered_map<int64_t, int64_t>& communities,
                        const std::vector<std::vector<int64_t>>& expected_groups) {
    // Collect all expected nodes.
    std::unordered_set<int64_t> expected_nodes;
    for (const auto& group : expected_groups) {
        for (int64_t n : group) {
            expected_nodes.insert(n);
        }
    }
    EXPECT_EQ(communities.size(), expected_nodes.size());

    // Verify nodes within the same group share a community.
    for (const auto& group : expected_groups) {
        if (group.empty())
            continue;
        auto it = communities.find(group[0]);
        ASSERT_NE(it, communities.end()) << "node " << group[0] << " not found in results";
        int64_t expected_comm = it->second;
        for (int64_t n : group) {
            auto jt = communities.find(n);
            ASSERT_NE(jt, communities.end()) << "node " << n << " not found in results";
            EXPECT_EQ(jt->second, expected_comm)
                << "nodes " << group[0] << " and " << n << " should be in the same community";
        }
    }

    // Verify nodes in different groups have different communities.
    for (size_t i = 0; i < expected_groups.size(); ++i) {
        for (size_t j = i + 1; j < expected_groups.size(); ++j) {
            if (expected_groups[i].empty() || expected_groups[j].empty())
                continue;
            auto ci = communities.at(expected_groups[i][0]);
            auto cj = communities.at(expected_groups[j][0]);
            EXPECT_NE(ci, cj) << "groups " << i << " and " << j
                              << " should have different community IDs";
        }
    }
}

/// Verify that community IDs are contiguous starting from 0.
void verify_contiguous_ids(const std::unordered_map<int64_t, int64_t>& communities) {
    std::unordered_set<int64_t> unique_ids;
    for (const auto& [_, comm] : communities) {
        unique_ids.insert(comm);
    }
    int64_t max_id = -1;
    for (int64_t id : unique_ids) {
        EXPECT_GE(id, 0) << "community IDs should be non-negative";
        max_id = std::max(max_id, id);
    }
    if (!unique_ids.empty()) {
        EXPECT_EQ(max_id, static_cast<int64_t>(unique_ids.size()) - 1)
            << "community IDs should be contiguous from 0";
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Algorithm definition tests
// ---------------------------------------------------------------------------

TEST(CommunityDetectDef, OutputSchema) {
    auto def = make_community_detect_def();
    EXPECT_EQ(def.name, "community_detect");
    ASSERT_EQ(def.output_columns.size(), 2u);
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[1].name, "community_id");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::INT64);
}

TEST(CommunityDetectDef, Parameters) {
    auto def = make_community_detect_def();
    ASSERT_EQ(def.params.size(), 2u);

    EXPECT_EQ(def.params[0].name, "resolution");
    EXPECT_EQ(def.params[0].type_id, TypeId::FLOAT64);
    EXPECT_FALSE(def.params[0].required);
    ASSERT_TRUE(def.params[0].default_value.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(def.params[0].default_value->data()), 1.0);

    EXPECT_EQ(def.params[1].name, "max_iterations");
    EXPECT_EQ(def.params[1].type_id, TypeId::INT64);
    EXPECT_FALSE(def.params[1].required);
    ASSERT_TRUE(def.params[1].default_value.has_value());
    EXPECT_EQ(std::get<int64_t>(def.params[1].default_value->data()), 10);
}

TEST(CommunityDetectDef, Registration) {
    AlgorithmRegistry registry;
    auto result =
        registry.register_algorithm(make_community_detect_def(), community_detect_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto* entry = registry.find("community_detect");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "community_detect");
}

// ---------------------------------------------------------------------------
// Test fixture with a GraphEngine
// ---------------------------------------------------------------------------

class CommunityDetectTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
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

    /// Run community_detect with default parameters.
    Result<std::vector<AlgorithmRow>> run_cd(const std::string& edge_type) {
        AlgorithmContext ctx{
            engine_,
            default_database_id,
            edge_type,
            {{"resolution", Value(1.0)}, {"max_iterations", Value(static_cast<int64_t>(10))}}};
        return community_detect_execute(ctx);
    }

    /// Run community_detect with custom parameters.
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
// Execution tests on known graphs
// ---------------------------------------------------------------------------

TEST_F(CommunityDetectTest, EmptyGraph) {
    build_graph("knows", {});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(CommunityDetectTest, SingleEdge) {
    build_graph("knows", {{1, 2}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 2u);
    // Two connected nodes should be in the same community.
    EXPECT_EQ(communities[1], communities[2]);
    verify_contiguous_ids(communities);
}

TEST_F(CommunityDetectTest, Triangle) {
    // A fully connected triangle: all nodes should be in the same community.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto communities = to_community_map(*result);
    verify_communities(communities, {{1, 2, 3}});
    verify_contiguous_ids(communities);
}

TEST_F(CommunityDetectTest, TwoCliquesConnectedByBridge) {
    // Two triangles connected by a single bridge edge.
    // Clique A: 1-2-3, Clique B: 4-5-6, Bridge: 3-4
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}, {5, 6}, {6, 4}, {3, 4}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 6u);

    // Nodes within the same clique should be in the same community.
    EXPECT_EQ(communities[1], communities[2]);
    EXPECT_EQ(communities[1], communities[3]);
    EXPECT_EQ(communities[4], communities[5]);
    EXPECT_EQ(communities[4], communities[6]);

    // The two cliques should be in different communities.
    EXPECT_NE(communities[1], communities[4]);
    verify_contiguous_ids(communities);
}

TEST_F(CommunityDetectTest, DisconnectedComponents) {
    // Two completely disconnected components.
    // Component A: 1-2-3, Component B: 4-5-6
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}, {5, 6}, {6, 4}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto communities = to_community_map(*result);
    verify_communities(communities, {{1, 2, 3}, {4, 5, 6}});
    verify_contiguous_ids(communities);
}

TEST_F(CommunityDetectTest, StarGraph) {
    // Hub (1) connected to leaves (2,3,4,5). All in one community.
    build_graph("knows", {{1, 2}, {1, 3}, {1, 4}, {1, 5}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto communities = to_community_map(*result);
    verify_communities(communities, {{1, 2, 3, 4, 5}});
    verify_contiguous_ids(communities);
}

TEST_F(CommunityDetectTest, LinearChain) {
    // A chain: 1-2-3-4
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 4u);
    verify_contiguous_ids(communities);
}

TEST_F(CommunityDetectTest, ResultsOrderedByNodeId) {
    build_graph("knows", {{3, 1}, {5, 2}, {4, 3}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (size_t i = 1; i < result->size(); ++i) {
        auto prev = std::get<int64_t>((*result)[i - 1].values[0].data());
        auto curr = std::get<int64_t>((*result)[i].values[0].data());
        EXPECT_LT(prev, curr) << "results should be sorted by node_id";
    }
}

TEST_F(CommunityDetectTest, NonexistentEdgeTypeFails) {
    build_graph("knows", {{1, 2}});

    auto result = run_cd("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(CommunityDetectTest, KarateClubLikeGraph) {
    // A larger graph with two clear clusters connected by sparse edges.
    // Cluster A: 1-2-3-4-5 (dense connections)
    // Cluster B: 6-7-8-9-10 (dense connections)
    // Bridge: single edge 5-6
    build_graph("knows",
                {// Cluster A internal edges
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
                 // Cluster B internal edges
                 {6, 7},
                 {6, 8},
                 {6, 9},
                 {6, 10},
                 {7, 8},
                 {7, 9},
                 {7, 10},
                 {8, 9},
                 {8, 10},
                 {9, 10},
                 // Bridge
                 {5, 6}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 10u);

    // All nodes in Cluster A should be in the same community.
    EXPECT_EQ(communities[1], communities[2]);
    EXPECT_EQ(communities[1], communities[3]);
    EXPECT_EQ(communities[1], communities[4]);
    EXPECT_EQ(communities[1], communities[5]);

    // All nodes in Cluster B should be in the same community.
    EXPECT_EQ(communities[6], communities[7]);
    EXPECT_EQ(communities[6], communities[8]);
    EXPECT_EQ(communities[6], communities[9]);
    EXPECT_EQ(communities[6], communities[10]);

    // The two clusters should be in different communities.
    EXPECT_NE(communities[1], communities[6]);
    verify_contiguous_ids(communities);
}

TEST_F(CommunityDetectTest, CustomResolution) {
    // Higher resolution tends to find more (smaller) communities.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}, {5, 6}, {6, 4}, {3, 4}});

    auto result = run_cd("knows", 1.5, 10);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 6u);
    verify_contiguous_ids(communities);
}

TEST_F(CommunityDetectTest, CustomMaxIterations) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run_cd("knows", 1.0, 1);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 3u);
    verify_contiguous_ids(communities);
}

TEST_F(CommunityDetectTest, SelfLoopIgnored) {
    // Self-loop on node 1 should be ignored.
    build_graph("knows", {{1, 1}, {1, 2}, {2, 3}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto communities = to_community_map(*result);
    EXPECT_EQ(communities.size(), 3u);
    verify_contiguous_ids(communities);
}
