/// @file test_qa_gdb_480.cpp
/// QA adversarial tests for GDB-480: Connected Components algorithm.
///
/// Verifies:
///   AC1: Union-Find implementation correct.
///   AC2: Output schema: (node_id, component_id).
///   AC3: Composable with JOIN.
///   AC4: Unit tests on known graphs.
///
/// Adversarial categories: boundary values, error paths, edge cases, stress,
/// duplicate edges, self-loops, E2E composability.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/connected_components.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Helpers
// ============================================================================

static TableSchema make_table_schema(const std::string& name, TypeId pk_type = TypeId::INT64) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {
        {0, "id", pk_type, false, ""},
    };
    schema.pk_columns = "id";
    return schema;
}

static Value pk(int64_t v) {
    return Value(v);
}

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
    std::unordered_set<int64_t> expected_nodes;
    for (const auto& group : expected_groups) {
        for (int64_t n : group) {
            expected_nodes.insert(n);
        }
    }
    EXPECT_EQ(components.size(), expected_nodes.size());

    for (const auto& group : expected_groups) {
        if (group.empty())
            continue;
        auto it = components.find(group[0]);
        ASSERT_NE(it, components.end()) << "node " << group[0] << " not found";
        int64_t expected_comp = it->second;
        for (int64_t n : group) {
            auto jt = components.find(n);
            ASSERT_NE(jt, components.end()) << "node " << n << " not found";
            EXPECT_EQ(jt->second, expected_comp)
                << "nodes " << group[0] << " and " << n << " should share component";
        }
    }

    for (size_t i = 0; i < expected_groups.size(); ++i) {
        for (size_t j = i + 1; j < expected_groups.size(); ++j) {
            if (expected_groups[i].empty() || expected_groups[j].empty())
                continue;
            auto ci = components.at(expected_groups[i][0]);
            auto cj = components.at(expected_groups[j][0]);
            EXPECT_NE(ci, cj) << "groups " << i << " and " << j << " should differ";
        }
    }
}

// ============================================================================
// Part 1: Direct algorithm adversarial tests (Union-Find correctness)
// ============================================================================

class QA_GDB480_CC : public ::testing::Test {
protected:
    void SetUp() override {
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

    Result<std::vector<AlgorithmRow>> run_cc(const std::string& edge_type) {
        AlgorithmContext ctx{engine_, default_database_id, edge_type, {}};
        return connected_components_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// --- Self-loops ---

TEST_F(QA_GDB480_CC, SelfLoop) {
    // A self-loop should produce a single node in its own component.
    build_graph("knows", {{1, 1}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 1u);

    auto components = to_component_map(*result);
    EXPECT_EQ(components.size(), 1u);
    EXPECT_TRUE(components.count(1) > 0);
}

TEST_F(QA_GDB480_CC, SelfLoopWithOtherEdges) {
    // Self-loop on node 1, plus edges 1-2, 3-3.
    // Expected: {1,2} in one component, {3} in another.
    build_graph("knows", {{1, 1}, {1, 2}, {3, 3}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);
    verify_components(components, {{1, 2}, {3}});
}

// --- Duplicate edges ---

TEST_F(QA_GDB480_CC, DuplicateEdges) {
    // Same edge repeated should not create duplicate nodes or break components.
    build_graph("knows", {{1, 2}, {1, 2}, {1, 2}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);
    EXPECT_EQ(components.size(), 2u);
    verify_components(components, {{1, 2}});
}

TEST_F(QA_GDB480_CC, DuplicateEdgesReversed) {
    // Edge in both directions: 1->2 and 2->1.
    build_graph("knows", {{1, 2}, {2, 1}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);
    EXPECT_EQ(components.size(), 2u);
    verify_components(components, {{1, 2}});
}

// --- Boundary node IDs ---

TEST_F(QA_GDB480_CC, ZeroNodeId) {
    build_graph("knows", {{0, 1}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);
    EXPECT_EQ(components.size(), 2u);
    verify_components(components, {{0, 1}});
}

TEST_F(QA_GDB480_CC, NegativeNodeIds) {
    build_graph("knows", {{-1, -2}, {-3, -4}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);
    verify_components(components, {{-1, -2}, {-3, -4}});
}

TEST_F(QA_GDB480_CC, MixedSignNodeIds) {
    // Negative and positive node IDs in the same graph.
    build_graph("knows", {{-1, 1}, {-2, 2}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);
    verify_components(components, {{-1, 1}, {-2, 2}});
}

TEST_F(QA_GDB480_CC, LargeNodeIds) {
    // Use INT64_MAX and nearby values.
    int64_t big = std::numeric_limits<int64_t>::max();
    int64_t big2 = big - 1;
    build_graph("knows", {{big, big2}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);
    EXPECT_EQ(components.size(), 2u);
    verify_components(components, {{big, big2}});
}

TEST_F(QA_GDB480_CC, MinNodeId) {
    // INT64_MIN as a node ID.
    int64_t min_id = std::numeric_limits<int64_t>::min();
    build_graph("knows", {{min_id, 0}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);
    verify_components(components, {{min_id, 0}});
}

// --- Result ordering ---

TEST_F(QA_GDB480_CC, ResultsSortedByNodeIdNegative) {
    // Verify sort order with negative node IDs.
    build_graph("knows", {{5, -5}, {3, -3}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (size_t i = 1; i < result->size(); ++i) {
        auto prev = std::get<int64_t>((*result)[i - 1].values[0].data());
        auto curr = std::get<int64_t>((*result)[i].values[0].data());
        EXPECT_LT(prev, curr) << "results should be sorted by node_id";
    }
}

// --- Output schema (AC2) ---

TEST_F(QA_GDB480_CC, OutputSchemaTwoColumns) {
    build_graph("knows", {{1, 2}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (const auto& row : *result) {
        EXPECT_EQ(row.values.size(), 2u) << "each row should have exactly 2 columns";
        // Both values should be int64_t.
        EXPECT_TRUE(std::holds_alternative<int64_t>(row.values[0].data()));
        EXPECT_TRUE(std::holds_alternative<int64_t>(row.values[1].data()));
    }
}

TEST_F(QA_GDB480_CC, ComponentIdIsValidNodeId) {
    // The component_id should be one of the node_ids in the same component.
    build_graph("knows", {{1, 2}, {2, 3}, {4, 5}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);

    // Gather all node IDs.
    std::unordered_set<int64_t> all_nodes;
    for (const auto& [node, _] : components) {
        all_nodes.insert(node);
    }

    // Every component_id should be a node that exists in the result.
    for (const auto& [node, comp] : components) {
        EXPECT_TRUE(all_nodes.count(comp) > 0)
            << "component_id " << comp << " for node " << node << " is not a valid node";
    }
}

// --- Error paths ---

TEST_F(QA_GDB480_CC, NonexistentEdgeType) {
    build_graph("knows", {{1, 2}});

    auto result = run_cc("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB480_CC, EmptyGraphReturnsEmpty) {
    build_graph("knows", {});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

// --- Algorithm definition ---

TEST_F(QA_GDB480_CC, DefinitionSchema) {
    auto def = make_connected_components_def();
    EXPECT_EQ(def.name, "connected_components");
    ASSERT_EQ(def.output_columns.size(), 2u);
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_FALSE(def.output_columns[0].nullable);
    EXPECT_EQ(def.output_columns[1].name, "component_id");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::INT64);
    EXPECT_FALSE(def.output_columns[1].nullable);
    EXPECT_TRUE(def.params.empty());
}

// --- Stress tests ---

TEST_F(QA_GDB480_CC, LongChain) {
    // Chain of 200 nodes: 0-1-2-...-199 → single component.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 0; i < 199; ++i) {
        edges.push_back({i, i + 1});
    }
    build_graph("knows", edges);

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 200u);

    // All nodes should share the same component.
    auto components = to_component_map(*result);
    int64_t first_comp = components.at(0);
    for (int64_t i = 1; i < 200; ++i) {
        EXPECT_EQ(components.at(i), first_comp) << "node " << i << " has wrong component";
    }
}

TEST_F(QA_GDB480_CC, ManyDisconnectedPairs) {
    // 100 disconnected pairs → 100 components.
    std::vector<std::pair<int64_t, int64_t>> edges;
    std::vector<std::vector<int64_t>> expected_groups;
    for (int64_t i = 0; i < 100; ++i) {
        int64_t a = i * 2;
        int64_t b = i * 2 + 1;
        edges.push_back({a, b});
        expected_groups.push_back({a, b});
    }
    build_graph("knows", edges);

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 200u);

    auto components = to_component_map(*result);
    verify_components(components, expected_groups);
}

TEST_F(QA_GDB480_CC, StarGraphLarge) {
    // Hub node 0 connected to 100 leaf nodes.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 100; ++i) {
        edges.push_back({0, i});
    }
    build_graph("knows", edges);

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 101u);

    auto components = to_component_map(*result);
    // All 101 nodes in one component.
    int64_t comp = components.at(0);
    for (int64_t i = 1; i <= 100; ++i) {
        EXPECT_EQ(components.at(i), comp);
    }
}

TEST_F(QA_GDB480_CC, CompleteGraphSmall) {
    // K5 complete graph (all pairs connected).
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 5; ++i) {
        for (int64_t j = i + 1; j <= 5; ++j) {
            edges.push_back({i, j});
        }
    }
    build_graph("knows", edges);

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);
    verify_components(components, {{1, 2, 3, 4, 5}});
}

TEST_F(QA_GDB480_CC, DisconnectedSingletonsViaMultipleSelfLoops) {
    // Multiple self-loops: each node is its own component.
    build_graph("knows", {{1, 1}, {2, 2}, {3, 3}, {4, 4}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 4u);

    auto components = to_component_map(*result);
    verify_components(components, {{1}, {2}, {3}, {4}});
}

// --- Merging correctness ---

TEST_F(QA_GDB480_CC, ThreeComponentsMergedByBridgeEdges) {
    // Start: {1,2}, {3,4}, {5,6}
    // Bridge 2-3 merges first two, bridge 4-5 merges all.
    build_graph("knows", {{1, 2}, {3, 4}, {5, 6}, {2, 3}, {4, 5}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);
    verify_components(components, {{1, 2, 3, 4, 5, 6}});
}

TEST_F(QA_GDB480_CC, MergeLateFromBothDirections) {
    // Edges processed left-to-right: 1-2, 3-4, 5-6, then 2-5, 4-6
    // After: all in one component.
    build_graph("knows", {{1, 2}, {3, 4}, {5, 6}, {2, 5}, {4, 6}});

    auto result = run_cc("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto components = to_component_map(*result);
    verify_components(components, {{1, 2, 3, 4, 5, 6}});
}

// ============================================================================
// Part 2: Registration (AC1)
// ============================================================================

TEST(QA_GDB480_Registration, RegistrationAndLookup) {
    AlgorithmRegistry registry;
    auto result =
        registry.register_algorithm(make_connected_components_def(), connected_components_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto* entry = registry.find("connected_components");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "connected_components");
    EXPECT_EQ(entry->def.output_columns.size(), 2u);
}

TEST(QA_GDB480_Registration, CaseInsensitiveLookup) {
    AlgorithmRegistry registry;
    (void)registry.register_algorithm(make_connected_components_def(),
                                      connected_components_execute);

    EXPECT_NE(registry.find("CONNECTED_COMPONENTS"), nullptr);
    EXPECT_NE(registry.find("Connected_Components"), nullptr);
    EXPECT_NE(registry.find("connected_components"), nullptr);
}

TEST(QA_GDB480_Registration, DuplicateRegistrationFails) {
    AlgorithmRegistry registry;
    (void)registry.register_algorithm(make_connected_components_def(),
                                      connected_components_execute);

    auto dup =
        registry.register_algorithm(make_connected_components_def(), connected_components_execute);
    EXPECT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, StatusCode::ALREADY_EXISTS);
}

// ============================================================================
// Part 3: E2E composability tests (AC3)
// ============================================================================

class QA_GDB480_E2E : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb480";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        // Register the REAL connected_components algorithm.
        (void)algo_registry_.register_algorithm(make_connected_components_def(),
                                                connected_components_execute);
        engine_->set_algorithm_registry(&algo_registry_);

        // Create tables and edges.
        //   users: 1=Alice, 2=Bob, 3=Charlie, 4=Diana, 5=Eve
        //   Edge type "knows": 1-2, 2-3, 4-5
        //   Expected components: {1,2,3} and {4,5}
        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");
        exec_ok("INSERT INTO users VALUES (3, 'Charlie')");
        exec_ok("INSERT INTO users VALUES (4, 'Diana')");
        exec_ok("INSERT INTO users VALUES (5, 'Eve')");
        exec_ok("CREATE EDGE TYPE knows FROM users TO users");
        exec_ok("LINK users(1) TO users(2) VIA knows");
        exec_ok("LINK users(2) TO users(3) VIA knows");
        exec_ok("LINK users(4) TO users(5) VIA knows");
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << ": " << result.error().message;
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected_code) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected_code);
        }
    }

    void exec_error(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
    AlgorithmRegistry algo_registry_;
};

// --- AC2: Output schema via SELECT * ---

TEST_F(QA_GDB480_E2E, SelectStarReturnsCorrectColumns) {
    auto result = exec_ok("SELECT * FROM connected_components('knows')");

    // After GDB-548 fix, SELECT * should return node_id and component_id columns.
    ASSERT_EQ(result.column_names.size(), 2u);
    EXPECT_EQ(result.column_names[0], "node_id");
    EXPECT_EQ(result.column_names[1], "component_id");
}

TEST_F(QA_GDB480_E2E, SelectStarReturnsCorrectRowCount) {
    auto result = exec_ok("SELECT * FROM connected_components('knows')");

    // 5 nodes total: 1,2,3,4,5
    EXPECT_EQ(result.rows.size(), 5u);
}

// --- AC2: Named column references ---

TEST_F(QA_GDB480_E2E, SelectNamedColumns) {
    auto result = exec_ok("SELECT node_id, component_id FROM connected_components('knows')");
    ASSERT_EQ(result.column_names.size(), 2u);
    EXPECT_EQ(result.rows.size(), 5u);
}

TEST_F(QA_GDB480_E2E, SelectSingleColumn) {
    auto result = exec_ok("SELECT node_id FROM connected_components('knows')");
    ASSERT_EQ(result.column_names.size(), 1u);
    EXPECT_EQ(result.column_names[0], "node_id");
    EXPECT_EQ(result.rows.size(), 5u);
}

// --- AC3: Composable with WHERE ---

TEST_F(QA_GDB480_E2E, WhereOnNodeId) {
    auto result = exec_ok(
        "SELECT node_id, component_id FROM connected_components('knows') WHERE node_id = 1");
    EXPECT_EQ(result.rows.size(), 1u);
}

TEST_F(QA_GDB480_E2E, WhereNoMatch) {
    auto result = exec_ok("SELECT * FROM connected_components('knows') WHERE node_id = 999");
    EXPECT_EQ(result.rows.size(), 0u);
}

// --- AC3: Composable with ORDER BY ---

TEST_F(QA_GDB480_E2E, OrderByNodeIdDesc) {
    auto result =
        exec_ok("SELECT node_id FROM connected_components('knows') ORDER BY node_id DESC");
    ASSERT_GE(result.rows.size(), 2u);

    // Verify descending order.
    for (size_t i = 1; i < result.rows.size(); ++i) {
        auto prev = std::get<int64_t>(result.rows[i - 1][0].data());
        auto curr = std::get<int64_t>(result.rows[i][0].data());
        EXPECT_GE(prev, curr) << "results should be sorted descending by node_id";
    }
}

// --- AC3: Composable with JOIN ---

TEST_F(QA_GDB480_E2E, JoinWithTable) {
    auto result = exec_ok("SELECT u.name, cc.component_id "
                          "FROM connected_components('knows') AS cc "
                          "JOIN users AS u ON cc.node_id = u.id");

    // Should return one row per user that appears in the graph.
    EXPECT_EQ(result.rows.size(), 5u);
    ASSERT_EQ(result.column_names.size(), 2u);
    EXPECT_EQ(result.column_names[0], "name");
    EXPECT_EQ(result.column_names[1], "component_id");
}

TEST_F(QA_GDB480_E2E, JoinWithTableAndWhere) {
    auto result = exec_ok("SELECT u.name, cc.component_id "
                          "FROM connected_components('knows') AS cc "
                          "JOIN users AS u ON cc.node_id = u.id "
                          "WHERE u.name = 'Alice'");

    EXPECT_EQ(result.rows.size(), 1u);
}

// --- Error paths ---

TEST_F(QA_GDB480_E2E, UnknownAlgorithmFails) {
    exec_error("SELECT * FROM nonexistent_algo('knows')", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB480_E2E, MissingEdgeTypeArgFails) {
    exec_error("SELECT * FROM connected_components()", StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB480_E2E, NonStringEdgeTypeFails) {
    exec_error("SELECT * FROM connected_components(42)", StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB480_E2E, UnknownNamedParamFails) {
    // connected_components has no params, so any named param should fail.
    exec_error("SELECT * FROM connected_components('knows', foo := 42)",
               StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB480_E2E, CaseInsensitiveAlgorithmName) {
    auto r1 = exec_ok("SELECT * FROM connected_components('knows')");
    auto r2 = exec_ok("SELECT * FROM CONNECTED_COMPONENTS('knows')");
    auto r3 = exec_ok("SELECT * FROM Connected_Components('knows')");
    EXPECT_EQ(r1.rows.size(), r2.rows.size());
    EXPECT_EQ(r2.rows.size(), r3.rows.size());
}

// --- Empty graph E2E ---

TEST_F(QA_GDB480_E2E, EmptyEdgeType) {
    exec_ok("CREATE TABLE nodes2 (id INT PRIMARY KEY)");
    exec_ok("CREATE EDGE TYPE empty_edge FROM nodes2 TO nodes2");

    auto result = exec_ok("SELECT * FROM connected_components('empty_edge')");
    EXPECT_EQ(result.rows.size(), 0u);
}

} // namespace
} // namespace sixseven
