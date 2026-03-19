/// @file test_qa_gdb_551.cpp
/// QA adversarial tests for GDB-551: Betweenness centrality duplicate edges
/// inflate sigma and skew centrality scores.
///
/// Verifies the fix deduplicates edges when building the adjacency list.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/betweenness_centrality.h"
#include "sixseven/graph/graph_engine.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
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

std::unordered_map<int64_t, double> to_centrality_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, double> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 2u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto centrality = std::get<double>(row.values[1].data());
        result[node_id] = centrality;
    }
    return result;
}

// ============================================================================
// Fixture
// ============================================================================

class QA_GDB551_Betweenness : public ::testing::Test {
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

    Result<std::vector<AlgorithmRow>> run_unnormalized(const std::string& edge_type) {
        AlgorithmContext ctx{
            engine_, default_database_id, edge_type, {{"normalized", Value(false)}}};
        return betweenness_centrality_execute(ctx);
    }

    Result<std::vector<AlgorithmRow>> run_normalized(const std::string& edge_type) {
        AlgorithmContext ctx{
            engine_, default_database_id, edge_type, {{"normalized", Value(true)}}};
        return betweenness_centrality_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ============================================================================
// Core fix: duplicate edges should not inflate sigma
// ============================================================================

TEST_F(QA_GDB551_Betweenness, DiamondWithDuplicateEdges_EqualScores) {
    // Diamond: 1->2, 1->2, 1->2, 1->3, 2->4, 3->4
    // Even with 3 duplicate edges 1->2, nodes 2 and 3 should have equal
    // betweenness since there's still only one unique shortest path through each.
    build_graph("knows", {{1, 2}, {1, 2}, {1, 2}, {1, 3}, {2, 4}, {3, 4}});

    auto result = run_unnormalized("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_NEAR(scores[2], scores[3], 1e-10)
        << "Duplicate edges should not skew betweenness: node 2 = " << scores[2]
        << ", node 3 = " << scores[3];
    EXPECT_NEAR(scores[2], 0.5, 1e-10);
    EXPECT_NEAR(scores[3], 0.5, 1e-10);
}

TEST_F(QA_GDB551_Betweenness, AllDuplicateEdges_SameAsNoDuplicates) {
    // Build two identical edge types: one with duplicates, one without.
    // They should produce identical betweenness scores.
    // Chain: 1->2->3
    build_graph("clean", {{1, 2}, {2, 3}});
    build_graph("dupes", {{1, 2}, {1, 2}, {1, 2}, {2, 3}, {2, 3}});

    auto clean = run_unnormalized("clean");
    auto dupes = run_unnormalized("dupes");
    ASSERT_TRUE(clean.has_value()) << clean.error().message;
    ASSERT_TRUE(dupes.has_value()) << dupes.error().message;

    auto clean_scores = to_centrality_map(*clean);
    auto dupes_scores = to_centrality_map(*dupes);

    EXPECT_EQ(clean_scores.size(), dupes_scores.size());
    for (auto& [node, score] : clean_scores) {
        EXPECT_NEAR(score, dupes_scores[node], 1e-10)
            << "Node " << node << " should have same score with/without duplicates";
    }
}

TEST_F(QA_GDB551_Betweenness, MassiveDuplication_StillCorrect) {
    // Star graph: center node 1, spokes to 2,3,4,5
    // Add 10 duplicates of each spoke edge.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t tgt = 2; tgt <= 5; ++tgt) {
        for (int i = 0; i < 10; ++i) {
            edges.push_back({1, tgt});
        }
    }
    build_graph("star", edges);

    auto result = run_unnormalized("star");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    // In a star, center has all betweenness. Leaf nodes have 0.
    for (int64_t leaf = 2; leaf <= 5; ++leaf) {
        EXPECT_DOUBLE_EQ(scores[leaf], 0.0) << "Leaf node " << leaf << " should have 0 betweenness";
    }
}

TEST_F(QA_GDB551_Betweenness, SelfLoopDuplicates_Ignored) {
    // Self-loops (1->1) with duplicates should not crash or skew results.
    build_graph("loop", {{1, 1}, {1, 1}, {1, 2}, {2, 3}});

    auto result = run_unnormalized("loop");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_NEAR(scores[2], 1.0, 1e-10) << "Node 2 on path 1->2->3 should have betweenness 1.0";
}

TEST_F(QA_GDB551_Betweenness, NoDuplicates_Unaffected) {
    // Verify the fix doesn't break the no-duplicates case.
    build_graph("basic", {{1, 2}, {2, 3}, {3, 4}});

    auto result = run_unnormalized("basic");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_DOUBLE_EQ(scores[1], 0.0);
    EXPECT_DOUBLE_EQ(scores[2], 2.0);
    EXPECT_DOUBLE_EQ(scores[3], 2.0);
    EXPECT_DOUBLE_EQ(scores[4], 0.0);
}

TEST_F(QA_GDB551_Betweenness, EmptyGraph_NoCrash) {
    auto et = engine_.create_edge_type(
        default_database_id, "empty", table_id_, table_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et.has_value());

    AlgorithmContext ctx{engine_, default_database_id, "empty", {{"normalized", Value(false)}}};
    auto result = betweenness_centrality_execute(ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 0u);
}

TEST_F(QA_GDB551_Betweenness, NormalizedWithDuplicates) {
    // Verify normalized mode also handles duplicates correctly.
    build_graph("norm", {{1, 2}, {1, 2}, {1, 3}, {2, 4}, {3, 4}});

    auto result = run_normalized("norm");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    // Nodes 2 and 3 should have equal normalized betweenness.
    EXPECT_NEAR(scores[2], scores[3], 1e-10);
}

} // namespace
} // namespace sixseven
