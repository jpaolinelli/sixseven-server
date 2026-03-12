/// @file test_qa_gdb_518.cpp
/// QA adversarial tests for GDB-518: Closeness Centrality algorithm.
///
/// Verifies:
///   AC1: closeness_centrality() function registered and callable.
///   AC2: Returns correct closeness score per node.
///   AC3: Handles disconnected nodes (closeness = 0 for isolated nodes).
///   AC4: Unit tests written and passing.
///   AC5: No regressions.
///
/// Adversarial categories: boundary values, error paths, graph topology
/// edge cases, numerical stability, stress tests, mathematical invariants.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/closeness_centrality.h"
#include "sixseven/graph/graph_engine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Helpers
// ============================================================================

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

/// Closeness info for a single node.
struct ClosenessInfo {
    double closeness;
    int64_t sum_farness;
    int64_t reachable_count;
};

/// Extract (node_id, ClosenessInfo) from algorithm result rows.
std::unordered_map<int64_t, ClosenessInfo>
to_closeness_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, ClosenessInfo> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 4u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto closeness = std::get<double>(row.values[1].data());
        auto sum_farness = std::get<int64_t>(row.values[2].data());
        auto reachable_count = std::get<int64_t>(row.values[3].data());
        result[node_id] = {closeness, sum_farness, reachable_count};
    }
    return result;
}

/// Verify output rows are sorted by node_id.
void verify_sorted_by_node_id(const std::vector<AlgorithmRow>& rows) {
    for (size_t i = 1; i < rows.size(); ++i) {
        auto prev = std::get<int64_t>(rows[i - 1].values[0].data());
        auto curr = std::get<int64_t>(rows[i].values[0].data());
        EXPECT_LT(prev, curr) << "results should be sorted by node_id";
    }
}

/// Verify all closeness values are finite (not NaN or Inf).
void verify_closeness_finite(const std::unordered_map<int64_t, ClosenessInfo>& scores) {
    for (const auto& [node, info] : scores) {
        EXPECT_FALSE(std::isnan(info.closeness))
            << "node " << node << " has NaN closeness";
        EXPECT_FALSE(std::isinf(info.closeness))
            << "node " << node << " has Inf closeness";
    }
}

/// Verify the closeness formula invariant:
/// closeness = (reachable_count - 1) / sum_farness when sum_farness > 0,
/// closeness = 0 when sum_farness == 0 or reachable_count <= 1.
void verify_closeness_formula(const std::unordered_map<int64_t, ClosenessInfo>& scores) {
    for (const auto& [node, info] : scores) {
        if (info.reachable_count <= 1 || info.sum_farness == 0) {
            EXPECT_DOUBLE_EQ(info.closeness, 0.0)
                << "node " << node << " with reachable=" << info.reachable_count
                << " sum_farness=" << info.sum_farness << " should have closeness 0";
        } else {
            double expected = static_cast<double>(info.reachable_count - 1) /
                              static_cast<double>(info.sum_farness);
            EXPECT_NEAR(info.closeness, expected, 1e-10)
                << "node " << node << " closeness formula mismatch";
        }
    }
}

/// Verify closeness is in [0.0, 1.0] range.
void verify_closeness_bounds(const std::unordered_map<int64_t, ClosenessInfo>& scores) {
    for (const auto& [node, info] : scores) {
        EXPECT_GE(info.closeness, 0.0) << "node " << node << " closeness < 0";
        EXPECT_LE(info.closeness, 1.0 + 1e-10)
            << "node " << node << " closeness exceeds 1.0";
    }
}

/// Verify basic constraints on sum_farness and reachable_count.
void verify_basic_constraints(const std::unordered_map<int64_t, ClosenessInfo>& scores) {
    for (const auto& [node, info] : scores) {
        EXPECT_GE(info.sum_farness, 0) << "node " << node << " negative sum_farness";
        EXPECT_GE(info.reachable_count, 1)
            << "node " << node << " reachable_count < 1 (should include self)";
    }
}

// ============================================================================
// Test fixture
// ============================================================================

class QA_GDB518_ClosenessCentrality : public ::testing::Test {
protected:
    void SetUp() override {
        auto t = catalog_.create_table(default_database_id, make_table_schema("nodes"));
        ASSERT_TRUE(t.has_value()) << t.error().message;
        table_id_ = *t;
    }

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

    Result<std::vector<AlgorithmRow>> run(const std::string& edge_type) {
        AlgorithmContext ctx{engine_, edge_type, {}};
        return closeness_centrality_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ============================================================================
// AC1: closeness_centrality() function registered and callable
// ============================================================================

TEST(QA_GDB518_Def, AC1_Registration) {
    AlgorithmRegistry registry;
    auto result =
        registry.register_algorithm(make_closeness_centrality_def(), closeness_centrality_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto* entry = registry.find("closeness");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "closeness");
}

TEST(QA_GDB518_Def, AC1_CaseInsensitiveLookup) {
    AlgorithmRegistry registry;
    (void)registry.register_algorithm(make_closeness_centrality_def(), closeness_centrality_execute);

    EXPECT_NE(registry.find("CLOSENESS"), nullptr);
    EXPECT_NE(registry.find("Closeness"), nullptr);
    EXPECT_NE(registry.find("closeness"), nullptr);
}

TEST(QA_GDB518_Def, AC1_OutputSchemaColumns) {
    auto def = make_closeness_centrality_def();
    ASSERT_EQ(def.output_columns.size(), 4u);

    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_FALSE(def.output_columns[0].nullable);

    EXPECT_EQ(def.output_columns[1].name, "closeness");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::FLOAT64);
    EXPECT_FALSE(def.output_columns[1].nullable);

    EXPECT_EQ(def.output_columns[2].name, "sum_farness");
    EXPECT_EQ(def.output_columns[2].type_id, TypeId::INT64);
    EXPECT_FALSE(def.output_columns[2].nullable);

    EXPECT_EQ(def.output_columns[3].name, "reachable_count");
    EXPECT_EQ(def.output_columns[3].type_id, TypeId::INT64);
    EXPECT_FALSE(def.output_columns[3].nullable);
}

TEST(QA_GDB518_Def, AC1_NoParameters) {
    auto def = make_closeness_centrality_def();
    EXPECT_TRUE(def.params.empty());
}

TEST_F(QA_GDB518_ClosenessCentrality, AC1_OutputRowStructure) {
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value());

    for (const auto& row : *result) {
        ASSERT_EQ(row.values.size(), 4u) << "each row must have exactly 4 columns";
        EXPECT_TRUE(std::holds_alternative<int64_t>(row.values[0].data()));
        EXPECT_TRUE(std::holds_alternative<double>(row.values[1].data()));
        EXPECT_TRUE(std::holds_alternative<int64_t>(row.values[2].data()));
        EXPECT_TRUE(std::holds_alternative<int64_t>(row.values[3].data()));
    }
}

// ============================================================================
// AC2: Returns correct closeness score per node
// ============================================================================

TEST_F(QA_GDB518_ClosenessCentrality, AC2_SingleEdgeDirected) {
    // 1->2: node 1 reaches 2 at d=1, node 2 reaches nobody.
    build_graph("knows", {{1, 2}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 2u);

    // Node 1: reachable=2, sum_farness=1, closeness=1/1=1.0
    EXPECT_DOUBLE_EQ(scores[1].closeness, 1.0);
    EXPECT_EQ(scores[1].sum_farness, 1);
    EXPECT_EQ(scores[1].reachable_count, 2);

    // Node 2: reachable=1, closeness=0
    EXPECT_DOUBLE_EQ(scores[2].closeness, 0.0);
    EXPECT_EQ(scores[2].sum_farness, 0);
    EXPECT_EQ(scores[2].reachable_count, 1);
}

TEST_F(QA_GDB518_ClosenessCentrality, AC2_DirectedChainExactValues) {
    // 1->2->3->4->5: verify exact closeness values for all nodes.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 5u);

    // Node 1: reaches 2(1), 3(2), 4(3), 5(4). sum=10, reachable=5, closeness=4/10=0.4
    EXPECT_EQ(scores[1].sum_farness, 10);
    EXPECT_EQ(scores[1].reachable_count, 5);
    EXPECT_NEAR(scores[1].closeness, 4.0 / 10.0, 1e-10);

    // Node 2: reaches 3(1), 4(2), 5(3). sum=6, reachable=4, closeness=3/6=0.5
    EXPECT_EQ(scores[2].sum_farness, 6);
    EXPECT_EQ(scores[2].reachable_count, 4);
    EXPECT_NEAR(scores[2].closeness, 3.0 / 6.0, 1e-10);

    // Node 3: reaches 4(1), 5(2). sum=3, reachable=3, closeness=2/3
    EXPECT_EQ(scores[3].sum_farness, 3);
    EXPECT_EQ(scores[3].reachable_count, 3);
    EXPECT_NEAR(scores[3].closeness, 2.0 / 3.0, 1e-10);

    // Node 4: reaches 5(1). sum=1, reachable=2, closeness=1/1=1.0
    EXPECT_EQ(scores[4].sum_farness, 1);
    EXPECT_EQ(scores[4].reachable_count, 2);
    EXPECT_DOUBLE_EQ(scores[4].closeness, 1.0);

    // Node 5: reaches nobody. closeness=0
    EXPECT_EQ(scores[5].sum_farness, 0);
    EXPECT_EQ(scores[5].reachable_count, 1);
    EXPECT_DOUBLE_EQ(scores[5].closeness, 0.0);
}

TEST_F(QA_GDB518_ClosenessCentrality, AC2_BidirectionalPathCenterHighest) {
    // Bidirectional path: 1<->2<->3<->4<->5
    build_graph("knows",
                {{1, 2}, {2, 1}, {2, 3}, {3, 2}, {3, 4}, {4, 3}, {4, 5}, {5, 4}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 5u);

    // Center node 3 should have highest closeness.
    for (int64_t node : {1, 2, 4, 5}) {
        EXPECT_GT(scores[3].closeness, scores[node].closeness)
            << "center node 3 should beat node " << node;
    }

    // Symmetry: scores[2] == scores[4], scores[1] == scores[5]
    EXPECT_NEAR(scores[2].closeness, scores[4].closeness, 1e-10);
    EXPECT_NEAR(scores[1].closeness, scores[5].closeness, 1e-10);

    // Node 3: d(2)=1, d(4)=1, d(1)=2, d(5)=2. sum=6, reachable=5, closeness=4/6
    EXPECT_EQ(scores[3].sum_farness, 6);
    EXPECT_EQ(scores[3].reachable_count, 5);
    EXPECT_NEAR(scores[3].closeness, 4.0 / 6.0, 1e-10);

    verify_closeness_formula(scores);
}

TEST_F(QA_GDB518_ClosenessCentrality, AC2_CompleteGraphAllEqual) {
    // Complete K4 (bidirectional): all closeness = 1.0
    build_graph("knows",
                {{1, 2}, {1, 3}, {1, 4}, {2, 1}, {2, 3}, {2, 4},
                 {3, 1}, {3, 2}, {3, 4}, {4, 1}, {4, 2}, {4, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 4u);

    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_DOUBLE_EQ(scores[node].closeness, 1.0)
            << "node " << node << " in K4 should have closeness 1.0";
        EXPECT_EQ(scores[node].sum_farness, 3);
        EXPECT_EQ(scores[node].reachable_count, 4);
    }
}

TEST_F(QA_GDB518_ClosenessCentrality, AC2_StarGraphOutgoing) {
    // Hub=1 with only outgoing edges to 2,3,4,5.
    build_graph("knows", {{1, 2}, {1, 3}, {1, 4}, {1, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 5u);

    // Hub: reaches all 4 at d=1. closeness = 4/4 = 1.0
    EXPECT_DOUBLE_EQ(scores[1].closeness, 1.0);
    EXPECT_EQ(scores[1].sum_farness, 4);
    EXPECT_EQ(scores[1].reachable_count, 5);

    // Spokes: can't reach anyone
    for (int64_t node : {2, 3, 4, 5}) {
        EXPECT_DOUBLE_EQ(scores[node].closeness, 0.0);
        EXPECT_EQ(scores[node].reachable_count, 1);
    }
}

TEST_F(QA_GDB518_ClosenessCentrality, AC2_BidirectionalStarGraph) {
    // Hub=1 with bidirectional edges to 2,3,4,5.
    // Spokes can reach hub at d=1, then other spokes at d=2.
    build_graph("knows", {{1, 2}, {2, 1}, {1, 3}, {3, 1}, {1, 4}, {4, 1}, {1, 5}, {5, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 5u);

    // Hub: reaches all 4 at d=1. sum=4, reachable=5, closeness=4/4=1.0
    EXPECT_DOUBLE_EQ(scores[1].closeness, 1.0);
    EXPECT_EQ(scores[1].sum_farness, 4);
    EXPECT_EQ(scores[1].reachable_count, 5);

    // Spoke 2: reaches hub(1), then 3(2), 4(2), 5(2). sum=1+2+2+2=7, reachable=5
    // closeness = 4/7
    for (int64_t node : {2, 3, 4, 5}) {
        EXPECT_EQ(scores[node].sum_farness, 7);
        EXPECT_EQ(scores[node].reachable_count, 5);
        EXPECT_NEAR(scores[node].closeness, 4.0 / 7.0, 1e-10)
            << "spoke node " << node;
    }

    // Hub has strictly higher closeness than any spoke
    for (int64_t node : {2, 3, 4, 5}) {
        EXPECT_GT(scores[1].closeness, scores[node].closeness);
    }
}

TEST_F(QA_GDB518_ClosenessCentrality, AC2_DirectedTriangleCycle) {
    // 1->2->3->1 (directed cycle): all equal closeness.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 3u);

    // Each reaches 2 others: one at d=1, one at d=2. sum=3, reachable=3
    for (int64_t node : {1, 2, 3}) {
        EXPECT_NEAR(scores[node].closeness, 2.0 / 3.0, 1e-10);
        EXPECT_EQ(scores[node].sum_farness, 3);
        EXPECT_EQ(scores[node].reachable_count, 3);
    }
}

TEST_F(QA_GDB518_ClosenessCentrality, AC2_DiamondGraphExactValues) {
    // 1->2, 1->3, 2->4, 3->4 (diamond/DAG)
    build_graph("knows", {{1, 2}, {1, 3}, {2, 4}, {3, 4}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 4u);

    // Node 1: 2(1),3(1),4(2). sum=4, reachable=4, closeness=3/4
    EXPECT_EQ(scores[1].sum_farness, 4);
    EXPECT_EQ(scores[1].reachable_count, 4);
    EXPECT_NEAR(scores[1].closeness, 3.0 / 4.0, 1e-10);

    // Node 2: 4(1). sum=1, reachable=2, closeness=1.0
    EXPECT_EQ(scores[2].sum_farness, 1);
    EXPECT_EQ(scores[2].reachable_count, 2);
    EXPECT_DOUBLE_EQ(scores[2].closeness, 1.0);

    // Node 3: 4(1). Same as node 2 by symmetry.
    EXPECT_NEAR(scores[3].closeness, scores[2].closeness, 1e-10);

    // Node 4: reaches nobody. closeness=0
    EXPECT_DOUBLE_EQ(scores[4].closeness, 0.0);

    verify_closeness_formula(scores);
}

TEST_F(QA_GDB518_ClosenessCentrality, AC2_FormulaInvariantComplexGraph) {
    // Complex graph: verify formula holds for every node.
    build_graph("knows",
                {{1, 2}, {1, 3}, {2, 3}, {3, 4}, {4, 1}, {4, 5}, {5, 2}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    verify_closeness_formula(scores);
    verify_closeness_bounds(scores);
    verify_basic_constraints(scores);
}

// ============================================================================
// AC3: Handles disconnected nodes (closeness = 0 for isolated nodes)
// ============================================================================

TEST_F(QA_GDB518_ClosenessCentrality, AC3_DisconnectedPairs) {
    // Two disconnected directed pairs: 1->2, 3->4
    build_graph("knows", {{1, 2}, {3, 4}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 4u);

    // Source nodes reach 1 other node: closeness = 1.0
    EXPECT_DOUBLE_EQ(scores[1].closeness, 1.0);
    EXPECT_DOUBLE_EQ(scores[3].closeness, 1.0);

    // Sink nodes reach nobody: closeness = 0
    EXPECT_DOUBLE_EQ(scores[2].closeness, 0.0);
    EXPECT_DOUBLE_EQ(scores[4].closeness, 0.0);

    // Cross-component: node 1 can't reach node 3 or 4
    EXPECT_EQ(scores[1].reachable_count, 2); // only itself + node 2
    EXPECT_EQ(scores[3].reachable_count, 2); // only itself + node 4
}

TEST_F(QA_GDB518_ClosenessCentrality, AC3_ThreeDisconnectedComponents) {
    // Three components: {1->2}, {3->4->5}, {6->7}
    build_graph("knows", {{1, 2}, {3, 4}, {4, 5}, {6, 7}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 7u);

    // Node 3 reaches 4(1), 5(2). sum=3, reachable=3, closeness=2/3
    EXPECT_EQ(scores[3].sum_farness, 3);
    EXPECT_EQ(scores[3].reachable_count, 3);
    EXPECT_NEAR(scores[3].closeness, 2.0 / 3.0, 1e-10);

    // Verify no node claims reachable_count > its component size
    EXPECT_LE(scores[1].reachable_count, 2);
    EXPECT_LE(scores[6].reachable_count, 2);

    verify_closeness_formula(scores);
}

TEST_F(QA_GDB518_ClosenessCentrality, AC3_SinkOnlyNodesGetZero) {
    // Star: 1->2, 1->3, 1->4. Nodes 2,3,4 are sinks.
    build_graph("knows", {{1, 2}, {1, 3}, {1, 4}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    for (int64_t sink : {2, 3, 4}) {
        EXPECT_DOUBLE_EQ(scores[sink].closeness, 0.0)
            << "sink node " << sink << " should have closeness 0";
        EXPECT_EQ(scores[sink].reachable_count, 1);
        EXPECT_EQ(scores[sink].sum_farness, 0);
    }
}

TEST_F(QA_GDB518_ClosenessCentrality, AC3_MixedConnectedAndIsolated) {
    // Connected component {1->2->3->1} and isolated pair {4->5}
    // In the connected cycle, nodes reach everyone in cycle.
    // In the pair, 4 reaches 5 only, 5 reaches nobody.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 5u);

    // Cycle nodes: all reach 2 others, all have same closeness
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(scores[node].reachable_count, 3);
        EXPECT_NEAR(scores[node].closeness, 2.0 / 3.0, 1e-10);
    }

    // Node 5 isolated from cycle and unreachable from 4's perspective
    EXPECT_DOUBLE_EQ(scores[5].closeness, 0.0);
}

// ============================================================================
// Edge cases: empty, self-loops, duplicates
// ============================================================================

TEST_F(QA_GDB518_ClosenessCentrality, EdgeCase_EmptyGraph) {
    build_graph("knows", {});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(QA_GDB518_ClosenessCentrality, EdgeCase_SelfLoop) {
    // Single self-loop: 1->1. Node 1 only reaches itself.
    build_graph("knows", {{1, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 1u);

    // Self-loop: BFS from 1 starts with dist[1]=0. Neighbor 1 already visited.
    // reachable_count=1, sum_farness=0, closeness=0.
    EXPECT_DOUBLE_EQ(scores[1].closeness, 0.0);
    EXPECT_EQ(scores[1].sum_farness, 0);
    EXPECT_EQ(scores[1].reachable_count, 1);
}

TEST_F(QA_GDB518_ClosenessCentrality, EdgeCase_SelfLoopPlusOutgoing) {
    // 1->1 and 1->2. Self-loop shouldn't affect BFS to node 2.
    build_graph("knows", {{1, 1}, {1, 2}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 2u);

    // Node 1: reaches 2 at d=1 (self-loop doesn't help). reachable=2, sum=1
    EXPECT_DOUBLE_EQ(scores[1].closeness, 1.0);
    EXPECT_EQ(scores[1].reachable_count, 2);
    EXPECT_EQ(scores[1].sum_farness, 1);

    // Node 2: can't reach anyone
    EXPECT_DOUBLE_EQ(scores[2].closeness, 0.0);
}

TEST_F(QA_GDB518_ClosenessCentrality, EdgeCase_MultipleSelfLoops) {
    // All nodes have self-loops: 1->1, 2->2, 3->3, plus chain 1->2->3.
    build_graph("knows", {{1, 1}, {2, 2}, {3, 3}, {1, 2}, {2, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 3u);

    // Self-loops shouldn't change BFS distances.
    // Node 1: reaches 2(1), 3(2). sum=3, reachable=3, closeness=2/3
    EXPECT_EQ(scores[1].sum_farness, 3);
    EXPECT_EQ(scores[1].reachable_count, 3);
    EXPECT_NEAR(scores[1].closeness, 2.0 / 3.0, 1e-10);

    // Node 2: reaches 3(1). sum=1, reachable=2, closeness=1.0
    EXPECT_DOUBLE_EQ(scores[2].closeness, 1.0);

    // Node 3: reaches nobody. closeness=0
    EXPECT_DOUBLE_EQ(scores[3].closeness, 0.0);

    verify_closeness_formula(scores);
}

TEST_F(QA_GDB518_ClosenessCentrality, EdgeCase_DuplicateEdges) {
    // Duplicate edge: 1->2 three times. BFS should still find d(1,2)=1.
    build_graph("knows", {{1, 2}, {1, 2}, {1, 2}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 2u);

    // Duplicates don't change shortest paths.
    EXPECT_DOUBLE_EQ(scores[1].closeness, 1.0);
    EXPECT_EQ(scores[1].sum_farness, 1);
    EXPECT_EQ(scores[1].reachable_count, 2);

    EXPECT_DOUBLE_EQ(scores[2].closeness, 0.0);
}

TEST_F(QA_GDB518_ClosenessCentrality, EdgeCase_DuplicateEdgesInChain) {
    // 1->2 (x2), 2->3 (x3). Duplicates shouldn't affect distances.
    build_graph("knows", {{1, 2}, {1, 2}, {2, 3}, {2, 3}, {2, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 3u);

    // Node 1: reaches 2(1), 3(2). sum=3, reachable=3, closeness=2/3
    EXPECT_EQ(scores[1].sum_farness, 3);
    EXPECT_NEAR(scores[1].closeness, 2.0 / 3.0, 1e-10);

    verify_closeness_formula(scores);
}

// ============================================================================
// Error paths
// ============================================================================

TEST_F(QA_GDB518_ClosenessCentrality, Error_NonexistentEdgeType) {
    build_graph("knows", {{1, 2}});

    auto result = run("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB518_ClosenessCentrality, Error_EmptyEdgeTypeName) {
    // Edge type "" doesn't exist — should fail.
    build_graph("knows", {{1, 2}});

    auto result = run("");
    ASSERT_FALSE(result.has_value());
}

// ============================================================================
// Graph topology edge cases
// ============================================================================

TEST_F(QA_GDB518_ClosenessCentrality, Topology_DirectedCycle5) {
    // 1->2->3->4->5->1: all equal closeness.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 5u);

    // Each node: reaches 4 others at d=1,2,3,4. sum=10, reachable=5
    // closeness = 4/10 = 0.4
    for (int64_t node : {1, 2, 3, 4, 5}) {
        EXPECT_EQ(scores[node].sum_farness, 10);
        EXPECT_EQ(scores[node].reachable_count, 5);
        EXPECT_NEAR(scores[node].closeness, 4.0 / 10.0, 1e-10)
            << "node " << node << " in 5-cycle";
    }
}

TEST_F(QA_GDB518_ClosenessCentrality, Topology_DirectedCycle3) {
    // 1->2->3->1: all equal closeness = 2/3.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    for (int64_t node : {1, 2, 3}) {
        EXPECT_EQ(scores[node].sum_farness, 3);
        EXPECT_EQ(scores[node].reachable_count, 3);
        EXPECT_NEAR(scores[node].closeness, 2.0 / 3.0, 1e-10);
    }
}

TEST_F(QA_GDB518_ClosenessCentrality, Topology_LargeNodeIds) {
    constexpr int64_t A = 1000000000LL;
    constexpr int64_t B = 2000000000LL;
    constexpr int64_t C = 3000000000LL;
    build_graph("knows", {{A, B}, {B, C}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 3u);
    verify_sorted_by_node_id(*result);

    // Node A: reaches B(1), C(2). sum=3, reachable=3, closeness=2/3
    EXPECT_NEAR(scores[A].closeness, 2.0 / 3.0, 1e-10);

    // Node B: reaches C(1). closeness=1.0
    EXPECT_DOUBLE_EQ(scores[B].closeness, 1.0);

    // Node C: reaches nobody
    EXPECT_DOUBLE_EQ(scores[C].closeness, 0.0);
}

TEST_F(QA_GDB518_ClosenessCentrality, Topology_NegativeNodeIds) {
    build_graph("knows", {{-10, -5}, {-5, 0}, {0, -10}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 3u);
    verify_sorted_by_node_id(*result);

    // Directed cycle: all equal closeness = 2/3
    for (int64_t node : {-10, -5, 0}) {
        EXPECT_NEAR(scores[node].closeness, 2.0 / 3.0, 1e-10)
            << "node " << node;
    }
}

TEST_F(QA_GDB518_ClosenessCentrality, Topology_MixedPositiveNegativeIds) {
    build_graph("knows", {{-1, 0}, {0, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value());
    verify_sorted_by_node_id(*result);

    auto scores = to_closeness_map(*result);
    // -1 reaches 0(1), 1(2). closeness=2/3
    EXPECT_NEAR(scores[-1].closeness, 2.0 / 3.0, 1e-10);
    // 0 reaches 1(1). closeness=1.0
    EXPECT_DOUBLE_EQ(scores[0].closeness, 1.0);
    // 1 reaches nobody. closeness=0
    EXPECT_DOUBLE_EQ(scores[1].closeness, 0.0);
}

TEST_F(QA_GDB518_ClosenessCentrality, Topology_TwoNodeBidirectional) {
    // 1<->2: both reach the other at d=1.
    build_graph("knows", {{1, 2}, {2, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 2u);

    // Both: reachable=2, sum=1, closeness=1.0
    EXPECT_DOUBLE_EQ(scores[1].closeness, 1.0);
    EXPECT_DOUBLE_EQ(scores[2].closeness, 1.0);
}

TEST_F(QA_GDB518_ClosenessCentrality, Topology_ComplexDAGMultiplePaths) {
    // DAG with multiple paths: 1->2, 1->3, 2->4, 3->4, 1->4
    // BFS should find shortest path 1->4 directly at d=1, not via 2 or 3.
    build_graph("knows", {{1, 2}, {1, 3}, {2, 4}, {3, 4}, {1, 4}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 4u);

    // Node 1: reaches 2(1), 3(1), 4(1). sum=3, reachable=4, closeness=3/3=1.0
    EXPECT_EQ(scores[1].sum_farness, 3);
    EXPECT_EQ(scores[1].reachable_count, 4);
    EXPECT_DOUBLE_EQ(scores[1].closeness, 1.0);

    verify_closeness_formula(scores);
}

TEST_F(QA_GDB518_ClosenessCentrality, Topology_BowTieGraph) {
    // Bow-tie: 1->3, 2->3, 3->4, 3->5. Node 3 is the center.
    build_graph("knows", {{1, 3}, {2, 3}, {3, 4}, {3, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 5u);

    // Node 1: reaches 3(1), 4(2), 5(2). sum=5, reachable=4, closeness=3/5
    EXPECT_EQ(scores[1].sum_farness, 5);
    EXPECT_EQ(scores[1].reachable_count, 4);
    EXPECT_NEAR(scores[1].closeness, 3.0 / 5.0, 1e-10);

    // Node 3: reaches 4(1), 5(1). sum=2, reachable=3, closeness=2/2=1.0
    EXPECT_EQ(scores[3].sum_farness, 2);
    EXPECT_EQ(scores[3].reachable_count, 3);
    EXPECT_DOUBLE_EQ(scores[3].closeness, 1.0);

    // Node 3 has highest closeness
    for (int64_t node : {1, 2, 4, 5}) {
        EXPECT_GE(scores[3].closeness, scores[node].closeness)
            << "center node 3 should have highest closeness";
    }

    verify_closeness_formula(scores);
}

// ============================================================================
// Output ordering & determinism
// ============================================================================

TEST_F(QA_GDB518_ClosenessCentrality, OutputSortedByNodeId) {
    build_graph("knows", {{5, 3}, {3, 1}, {4, 2}, {7, 6}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value());
    verify_sorted_by_node_id(*result);
}

TEST_F(QA_GDB518_ClosenessCentrality, DeterministicOutput) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {1, 3}});

    auto result1 = run("knows");
    auto result2 = run("knows");
    ASSERT_TRUE(result1.has_value());
    ASSERT_TRUE(result2.has_value());

    ASSERT_EQ(result1->size(), result2->size());
    for (size_t i = 0; i < result1->size(); ++i) {
        ASSERT_EQ((*result1)[i].values.size(), 4u);
        // node_id
        EXPECT_EQ(std::get<int64_t>((*result1)[i].values[0].data()),
                  std::get<int64_t>((*result2)[i].values[0].data()));
        // closeness
        EXPECT_DOUBLE_EQ(std::get<double>((*result1)[i].values[1].data()),
                         std::get<double>((*result2)[i].values[1].data()));
        // sum_farness
        EXPECT_EQ(std::get<int64_t>((*result1)[i].values[2].data()),
                  std::get<int64_t>((*result2)[i].values[2].data()));
        // reachable_count
        EXPECT_EQ(std::get<int64_t>((*result1)[i].values[3].data()),
                  std::get<int64_t>((*result2)[i].values[3].data()));
    }
}

// ============================================================================
// Numerical stability
// ============================================================================

TEST_F(QA_GDB518_ClosenessCentrality, NumericalStability_AllClosenessFinite) {
    // Dense graph K10: verify no NaN or Inf.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 0; i < 10; ++i) {
        for (int64_t j = 0; j < 10; ++j) {
            if (i != j) {
                edges.push_back({i, j});
            }
        }
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    verify_closeness_finite(scores);
    verify_closeness_bounds(scores);

    // K10: each node reaches 9 others at d=1. closeness = 9/9 = 1.0
    for (const auto& [node, info] : scores) {
        EXPECT_DOUBLE_EQ(info.closeness, 1.0) << "node " << node;
    }
}

TEST_F(QA_GDB518_ClosenessCentrality, NumericalStability_ClosenessNonNegative) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value());

    auto scores = to_closeness_map(*result);
    verify_closeness_bounds(scores);
    verify_basic_constraints(scores);
}

TEST_F(QA_GDB518_ClosenessCentrality, NumericalStability_ClosenessAtMostOne) {
    // In any graph, closeness = (reachable-1)/sum_farness.
    // Since sum_farness >= reachable-1 (minimum distance is 1 for each reachable node),
    // closeness <= 1.0 always.
    build_graph("knows",
                {{1, 2}, {2, 3}, {3, 1}, {1, 4}, {4, 5}, {5, 1}, {2, 4}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value());

    auto scores = to_closeness_map(*result);
    verify_closeness_bounds(scores);
}

// ============================================================================
// Stress tests
// ============================================================================

TEST_F(QA_GDB518_ClosenessCentrality, Stress_DirectedRing100) {
    // 100-node directed ring. All nodes equal closeness.
    constexpr int64_t N = 100;
    std::vector<std::pair<int64_t, int64_t>> edges;
    edges.reserve(N);
    for (int64_t i = 0; i < N; ++i) {
        edges.push_back({i, (i + 1) % N});
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), static_cast<size_t>(N));

    // Each node in directed ring reaches N-1 others at distances 1,2,...,N-1.
    // sum = N*(N-1)/2. closeness = (N-1) / (N*(N-1)/2) = 2/N
    int64_t expected_sum = N * (N - 1) / 2;
    double expected_closeness = 2.0 / static_cast<double>(N);

    for (const auto& [node, info] : scores) {
        EXPECT_EQ(info.sum_farness, expected_sum) << "node " << node;
        EXPECT_EQ(info.reachable_count, N) << "node " << node;
        EXPECT_NEAR(info.closeness, expected_closeness, 1e-10) << "node " << node;
    }

    verify_sorted_by_node_id(*result);
}

TEST_F(QA_GDB518_ClosenessCentrality, Stress_LinearChain50) {
    // 50-node directed chain.
    constexpr int64_t N = 50;
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 0; i < N - 1; ++i) {
        edges.push_back({i, i + 1});
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), static_cast<size_t>(N));

    // First node reaches all N-1 others; last node reaches nobody.
    EXPECT_EQ(scores[0].reachable_count, N);
    EXPECT_DOUBLE_EQ(scores[N - 1].closeness, 0.0);
    EXPECT_EQ(scores[N - 1].reachable_count, 1);

    // In a directed chain, closeness = 2/(N-i) for node i (0-indexed).
    // Closeness is monotonically INCREASING from node 0 to node N-2,
    // then 0 for the last node (which can't reach anyone).
    for (int64_t i = 0; i < N - 2; ++i) {
        EXPECT_LT(scores[i].closeness, scores[i + 1].closeness)
            << "node " << i << " should have lower closeness than node " << (i + 1);
    }

    // Verify exact formula: closeness[i] = 2/(N-i) for i in [0, N-2]
    for (int64_t i = 0; i < N - 1; ++i) {
        double expected = 2.0 / static_cast<double>(N - i);
        EXPECT_NEAR(scores[i].closeness, expected, 1e-10) << "node " << i;
    }

    verify_closeness_formula(scores);
    verify_closeness_finite(scores);
}

TEST_F(QA_GDB518_ClosenessCentrality, Stress_HubAndSpoke100) {
    // Hub node 0 with 100 outgoing edges.
    constexpr int64_t SPOKES = 100;
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= SPOKES; ++i) {
        edges.push_back({0, i});
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), static_cast<size_t>(SPOKES + 1));

    // Hub: reaches all SPOKES at d=1. sum=SPOKES, reachable=SPOKES+1, closeness=SPOKES/SPOKES=1.0
    EXPECT_DOUBLE_EQ(scores[0].closeness, 1.0);
    EXPECT_EQ(scores[0].sum_farness, SPOKES);
    EXPECT_EQ(scores[0].reachable_count, SPOKES + 1);

    // All spokes: closeness=0
    for (int64_t i = 1; i <= SPOKES; ++i) {
        EXPECT_DOUBLE_EQ(scores[i].closeness, 0.0) << "spoke " << i;
    }
}

TEST_F(QA_GDB518_ClosenessCentrality, Stress_BidirectionalHubAndSpoke50) {
    // Hub=0, bidirectional edges to 50 spokes.
    constexpr int64_t SPOKES = 50;
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= SPOKES; ++i) {
        edges.push_back({0, i});
        edges.push_back({i, 0});
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    int64_t N = SPOKES + 1;
    EXPECT_EQ(scores.size(), static_cast<size_t>(N));

    // Hub: reaches all SPOKES at d=1. closeness=SPOKES/SPOKES=1.0
    EXPECT_DOUBLE_EQ(scores[0].closeness, 1.0);

    // Each spoke: reaches hub(1), then all other spokes at d=2.
    // sum = 1 + (SPOKES-1)*2 = 2*SPOKES - 1
    // reachable = N = SPOKES+1
    // closeness = SPOKES / (2*SPOKES - 1)
    int64_t expected_farness = 2 * SPOKES - 1;
    double expected_closeness = static_cast<double>(SPOKES) / static_cast<double>(expected_farness);

    for (int64_t i = 1; i <= SPOKES; ++i) {
        EXPECT_EQ(scores[i].sum_farness, expected_farness) << "spoke " << i;
        EXPECT_NEAR(scores[i].closeness, expected_closeness, 1e-10) << "spoke " << i;
    }

    verify_closeness_formula(scores);
}

TEST_F(QA_GDB518_ClosenessCentrality, Stress_ManyDisconnectedPairs) {
    // 100 disconnected directed pairs: (0->1), (2->3), ..., (198->199)
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 0; i < 200; i += 2) {
        edges.push_back({i, i + 1});
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), 200u);
    verify_closeness_finite(scores);

    // Source nodes: closeness=1.0, sink nodes: closeness=0.0
    for (int64_t i = 0; i < 200; i += 2) {
        EXPECT_DOUBLE_EQ(scores[i].closeness, 1.0)
            << "source node " << i;
        EXPECT_DOUBLE_EQ(scores[i + 1].closeness, 0.0)
            << "sink node " << (i + 1);
    }
}

TEST_F(QA_GDB518_ClosenessCentrality, Stress_CompleteK20) {
    // Complete directed graph K20: all closeness = 1.0
    constexpr int64_t N = 20;
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 0; i < N; ++i) {
        for (int64_t j = 0; j < N; ++j) {
            if (i != j) {
                edges.push_back({i, j});
            }
        }
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), static_cast<size_t>(N));

    for (const auto& [node, info] : scores) {
        EXPECT_DOUBLE_EQ(info.closeness, 1.0) << "node " << node;
        EXPECT_EQ(info.sum_farness, N - 1) << "node " << node;
        EXPECT_EQ(info.reachable_count, N) << "node " << node;
    }

    verify_sorted_by_node_id(*result);
}

TEST_F(QA_GDB518_ClosenessCentrality, Stress_BidirectionalChain30) {
    // Bidirectional chain of 30 nodes. Center should have highest closeness.
    constexpr int64_t N = 30;
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 0; i < N - 1; ++i) {
        edges.push_back({i, i + 1});
        edges.push_back({i + 1, i});
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_closeness_map(*result);
    EXPECT_EQ(scores.size(), static_cast<size_t>(N));

    // All nodes in a bidirectional chain reach all other nodes.
    for (const auto& [node, info] : scores) {
        EXPECT_EQ(info.reachable_count, N) << "node " << node;
    }

    // Center node (N/2 - 1 = 14) should have highest closeness.
    int64_t center = N / 2 - 1;
    for (int64_t i = 0; i < N; ++i) {
        if (i != center) {
            EXPECT_GE(scores[center].closeness, scores[i].closeness)
                << "center node " << center << " should have >= closeness than node " << i;
        }
    }

    // Symmetry: node i and node (N-1-i) should have equal closeness.
    for (int64_t i = 0; i < N / 2; ++i) {
        EXPECT_NEAR(scores[i].closeness, scores[N - 1 - i].closeness, 1e-10)
            << "symmetric nodes " << i << " and " << (N - 1 - i);
    }

    verify_closeness_formula(scores);
    verify_closeness_finite(scores);
}

// ============================================================================
// Mathematical invariant tests
// ============================================================================

TEST_F(QA_GDB518_ClosenessCentrality, Invariant_ReachableCountBound) {
    // reachable_count should never exceed total node count.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value());

    auto scores = to_closeness_map(*result);
    for (const auto& [node, info] : scores) {
        EXPECT_LE(info.reachable_count, static_cast<int64_t>(scores.size()))
            << "node " << node << " reachable_count exceeds total nodes";
    }
}

TEST_F(QA_GDB518_ClosenessCentrality, Invariant_SumFarnessLowerBound) {
    // sum_farness >= reachable_count - 1 (minimum distance is 1 per reachable node).
    build_graph("knows",
                {{1, 2}, {1, 3}, {2, 3}, {3, 4}, {4, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value());

    auto scores = to_closeness_map(*result);
    for (const auto& [node, info] : scores) {
        if (info.reachable_count > 1) {
            EXPECT_GE(info.sum_farness, info.reachable_count - 1)
                << "node " << node
                << " sum_farness should be >= reachable_count-1";
        }
    }
}

TEST_F(QA_GDB518_ClosenessCentrality, Invariant_ClosenessFormulaHoldsEverywhere) {
    // Random-ish complex graph: verify formula for every node.
    build_graph("knows",
                {{1, 2}, {2, 3}, {3, 1}, {1, 4}, {4, 5}, {5, 6},
                 {6, 4}, {7, 8}, {8, 7}, {9, 1}, {3, 7}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value());

    auto scores = to_closeness_map(*result);
    verify_closeness_formula(scores);
    verify_closeness_bounds(scores);
    verify_basic_constraints(scores);
    verify_closeness_finite(scores);
}

TEST_F(QA_GDB518_ClosenessCentrality, Invariant_HigherReachabilityNotAlwaysHigherCloseness) {
    // Demonstrate that more reachable nodes doesn't always mean higher closeness.
    // 1->2->3->4->5 and 6->7
    // Node 1: reachable=5, sum=10, closeness=0.4
    // Node 6: reachable=2, sum=1, closeness=1.0
    // Node 6 has higher closeness despite lower reachable count.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}, {6, 7}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value());

    auto scores = to_closeness_map(*result);

    EXPECT_GT(scores[1].reachable_count, scores[6].reachable_count);
    EXPECT_GT(scores[6].closeness, scores[1].closeness)
        << "higher reachability doesn't guarantee higher closeness";
}

} // namespace
} // namespace sixseven
