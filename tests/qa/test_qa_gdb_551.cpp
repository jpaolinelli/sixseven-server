/// @file test_qa_gdb_551.cpp
/// QA adversarial tests for GDB-551: Betweenness centrality duplicate edges
/// inflate sigma and skew centrality scores.
///
/// Verifies the fix deduplicates edges when building the adjacency list.

#include "sixseven/common/result.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/betweenness_centrality.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "graph_qa_fixture.h"

namespace sixseven {
namespace {

using graph_qa::GraphQaFixtureBase;

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

class QA_GDB551_Betweenness : public GraphQaFixtureBase {
protected:
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
    // Diamond: 1->2, 1->3, 2->4, 3->4, with the 1->2 edge duplicated 10x
    // (11 parallel edges 1->2 total, plus one 1->3).
    //
    // On the DEDUPED graph, node 4 is reached from 1 via two distinct
    // shortest paths of length 2: 1->2->4 and 1->3->4. Nodes 2 and 3 are
    // each the sole intermediary on exactly one of those paths, so Brandes'
    // dependency accumulation splits evenly: sigma(2)=1, sigma(3)=1,
    // sigma(4)=2, giving scores[2] == scores[3] == 0.5.
    //
    // This is discriminating for the GDB-551 dedup fix: WITHOUT dedup, the
    // 10 duplicate 1->2 edges would inflate sigma(2) to 10 (edge multiplicity
    // counted as distinct shortest paths) while sigma(3) stays 1, making
    // sigma(4) = 11. Brandes' delta accumulation splits the dependency on 4
    // proportionally to each predecessor's sigma share, so node 2 would get
    // 10/11 of the credit (~0.909) and node 3 only 1/11 (~0.091) instead of
    // the correct 0.5/0.5 split -- the exact assertions below would fail
    // under the un-deduped (buggy) implementation.
    build_graph("diamond_dup",
                {
                    {1, 2},
                    {1, 2},
                    {1, 2},
                    {1, 2},
                    {1, 2},
                    {1, 2},
                    {1, 2},
                    {1, 2},
                    {1, 2},
                    {1, 2},
                    {1, 2},
                    {1, 3},
                    {2, 4},
                    {3, 4},
                });

    auto result = run_unnormalized("diamond_dup");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_centrality_map(*result);
    EXPECT_DOUBLE_EQ(scores[1], 0.0) << "Source node has no intermediary role";
    EXPECT_DOUBLE_EQ(scores[2], 0.5)
        << "Node 2 is sole intermediary on one of two equal shortest paths";
    EXPECT_DOUBLE_EQ(scores[3], 0.5)
        << "Node 3 is sole intermediary on the other equal shortest path";
    EXPECT_DOUBLE_EQ(scores[4], 0.0) << "Sink node has no intermediary role";
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
