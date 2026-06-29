#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/connected_components.h"
#include "sixseven/graph/graph_engine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "test_catalog_helpers.h"
#include "test_graph_helpers.h"

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Extract (node_id, component_id) pairs from algorithm result rows.
std::unordered_map<int64_t, int64_t> to_component_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, int64_t> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 2u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto component_id = std::get<int64_t>(row.values[1].data());
        result[node_id] = component_id;
    }
    return result;
}

/// Check that nodes in the same expected group share the same component_id,
/// and nodes in different groups have different component_ids.
void verify_components(const std::unordered_map<int64_t, int64_t>& components,
                       const std::vector<std::vector<int64_t>>& expected_groups) {
    // Collect all expected nodes.
    std::unordered_set<int64_t> expected_nodes;
    for (const auto& group : expected_groups) {
        for (int64_t n : group) {
            expected_nodes.insert(n);
        }
    }
    EXPECT_EQ(components.size(), expected_nodes.size());

    // Verify nodes within the same group share a component.
    for (const auto& group : expected_groups) {
        if (group.empty())
            continue;
        auto it = components.find(group[0]);
        ASSERT_NE(it, components.end()) << "node " << group[0] << " not found in results";
        int64_t expected_comp = it->second;
        for (int64_t n : group) {
            auto jt = components.find(n);
            ASSERT_NE(jt, components.end()) << "node " << n << " not found in results";
            EXPECT_EQ(jt->second, expected_comp)
                << "nodes " << group[0] << " and " << n << " should be in the same component";
        }
    }

    // Verify nodes in different groups have different components.
    for (size_t i = 0; i < expected_groups.size(); ++i) {
        for (size_t j = i + 1; j < expected_groups.size(); ++j) {
            if (expected_groups[i].empty() || expected_groups[j].empty())
                continue;
            auto ci = components.at(expected_groups[i][0]);
            auto cj = components.at(expected_groups[j][0]);
            EXPECT_NE(ci, cj) << "groups " << i << " and " << j
                              << " should have different component IDs";
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Algorithm definition tests
// ---------------------------------------------------------------------------

TEST(ConnectedComponentsDef, OutputSchema) {
    auto def = make_connected_components_def();
    EXPECT_EQ(def.name, "connected_components");
    ASSERT_EQ(def.output_columns.size(), 2u);
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[1].name, "component_id");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::INT64);
}

TEST(ConnectedComponentsDef, NoParams) {
    auto def = make_connected_components_def();
    EXPECT_TRUE(def.params.empty());
}

TEST(ConnectedComponentsDef, Registration) {
    AlgorithmRegistry registry;
    auto result =
        registry.register_algorithm(make_connected_components_def(), connected_components_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto* entry = registry.find("connected_components");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "connected_components");
}

// ---------------------------------------------------------------------------
// Test fixture with a GraphEngine
// ---------------------------------------------------------------------------

class ConnectedComponentsTest : public ::testing::Test {
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

    /// Run connected_components and return the result.
    Result<std::vector<AlgorithmRow>> run_cc(const std::string& edge_type) {
        AlgorithmContext ctx{engine_, default_database_id, edge_type, {}};
        return connected_components_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ---------------------------------------------------------------------------
// Execution tests on known graphs
// ---------------------------------------------------------------------------

TEST_F(ConnectedComponentsTest, EmptyGraph) {
    build_graph("knows", {});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(ConnectedComponentsTest, SingleEdge) {
    build_graph("knows", {{1, 2}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);
    verify_components(components, {{1, 2}});
}

TEST_F(ConnectedComponentsTest, SingleComponent) {
    //  1 - 2 - 3 - 4
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);
    verify_components(components, {{1, 2, 3, 4}});
}

TEST_F(ConnectedComponentsTest, TwoComponents) {
    //  Component A: 1 - 2 - 3
    //  Component B: 4 - 5
    build_graph("knows", {{1, 2}, {2, 3}, {4, 5}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);
    verify_components(components, {{1, 2, 3}, {4, 5}});
}

TEST_F(ConnectedComponentsTest, ThreeComponents) {
    //  Component A: 1 - 2
    //  Component B: 3 - 4
    //  Component C: 5 - 6
    build_graph("knows", {{1, 2}, {3, 4}, {5, 6}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);
    verify_components(components, {{1, 2}, {3, 4}, {5, 6}});
}

TEST_F(ConnectedComponentsTest, CycleGraph) {
    //  1 - 2 - 3 - 1 (triangle)
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);
    verify_components(components, {{1, 2, 3}});
}

TEST_F(ConnectedComponentsTest, StarGraph) {
    //  1 is connected to 2, 3, 4, 5
    build_graph("knows", {{1, 2}, {1, 3}, {1, 4}, {1, 5}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);
    verify_components(components, {{1, 2, 3, 4, 5}});
}

TEST_F(ConnectedComponentsTest, ResultsOrderedByNodeId) {
    build_graph("knows", {{3, 1}, {5, 2}, {4, 3}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Verify rows are sorted by node_id.
    for (size_t i = 1; i < result->size(); ++i) {
        auto prev = std::get<int64_t>((*result)[i - 1].values[0].data());
        auto curr = std::get<int64_t>((*result)[i].values[0].data());
        EXPECT_LT(prev, curr) << "results should be sorted by node_id";
    }
}

TEST_F(ConnectedComponentsTest, MergingComponentsViaLateEdge) {
    //  Initially two components: {1,2} and {3,4}
    //  Then edge 2-3 merges them into one.
    build_graph("knows", {{1, 2}, {3, 4}, {2, 3}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);
    verify_components(components, {{1, 2, 3, 4}});
}

TEST_F(ConnectedComponentsTest, NonexistentEdgeTypeFails) {
    build_graph("knows", {{1, 2}});

    auto result = run_cc("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(ConnectedComponentsTest, LargerGraph) {
    //  Component A: 1-2-3-4-5 (chain)
    //  Component B: 10-11-12 (chain)
    //  Component C: 20-21 (single edge)
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}, {10, 11}, {11, 12}, {20, 21}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);
    EXPECT_EQ(components.size(), 10u);
    verify_components(components, {{1, 2, 3, 4, 5}, {10, 11, 12}, {20, 21}});
}
