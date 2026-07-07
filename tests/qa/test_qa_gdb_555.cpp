/// @file test_qa_gdb_555.cpp
/// QA adversarial tests for GDB-555: ALL_SHORTEST with weighted paths misses
/// equal-cost paths through shared intermediate nodes.
///
/// Verifies the fix: ALL_SHORTEST with WEIGHT uses <= for equal-cost paths.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "shortest_path_qa_fixture.h"

namespace sixseven {
namespace {

using shortest_path_qa::ShortestPathQaFixtureBase;

// ============================================================================
// Fixture
// ============================================================================

class QA_GDB555 : public ShortestPathQaFixtureBase {
public:
    QA_GDB555() : ShortestPathQaFixtureBase("sixseven_qa_gdb555") {}
};

// ============================================================================
// Core fix: ALL_SHORTEST finds equal-cost paths through shared intermediate
// ============================================================================

TEST_F(QA_GDB555, AllShortest_SharedIntermediate_BothPathsFound) {
    // Graph:
    //   1 --(1)--> 2 --(1)--> 3 --(1)--> 4
    //   1 --(1)--> 5 --(1)--> 3 --(1)--> 4
    // Both paths 1->2->3->4 and 1->5->3->4 have cost 3.
    for (int64_t id : {1, 2, 3, 4, 5})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 3, 1.0);
    link(3, 4, 1.0);
    link(1, 5, 1.0);
    link(5, 3, 1.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_4 = filter_pair(results, 1, 4);

    ASSERT_EQ(from_1_to_4.size(), 2u)
        << "ALL_SHORTEST should find both equal-cost paths through shared node 3";

    // Both paths should have cost 3.
    for (const auto* t : from_1_to_4) {
        EXPECT_DOUBLE_EQ(t->values[2].as_path().total_weight, 3.0);
    }
}

TEST_F(QA_GDB555, AllShortest_SharedIntermediate_CorrectPaths) {
    // Same graph as above, verify actual paths.
    for (int64_t id : {1, 2, 3, 4, 5})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 3, 1.0);
    link(3, 4, 1.0);
    link(1, 5, 1.0);
    link(5, 3, 1.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_4 = filter_pair(results, 1, 4);
    ASSERT_EQ(from_1_to_4.size(), 2u);

    // Collect second node of each path (should be 2 and 5).
    std::set<int64_t> second_nodes;
    for (const auto* t : from_1_to_4) {
        const auto& path = t->values[2].as_path();
        ASSERT_GE(path.steps.size(), 2u);
        second_nodes.insert(path.steps[1].node_pk_as_int64());
    }
    EXPECT_TRUE(second_nodes.count(2)) << "Path through node 2 should be found";
    EXPECT_TRUE(second_nodes.count(5)) << "Path through node 5 should be found";
}

// ============================================================================
// Regression: ANY_SHORTEST still returns single path
// ============================================================================

TEST_F(QA_GDB555, AnyShortest_StillReturnsSinglePath) {
    for (int64_t id : {1, 2, 3, 4, 5})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 3, 1.0);
    link(3, 4, 1.0);
    link(1, 5, 1.0);
    link(5, 3, 1.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ANY_SHORTEST, w.get());
    auto from_1_to_4 = filter_pair(results, 1, 4);
    EXPECT_EQ(from_1_to_4.size(), 1u) << "ANY_SHORTEST should still return exactly one path";
}

// ============================================================================
// Adversarial: multiple shared intermediate nodes
// ============================================================================

TEST_F(QA_GDB555, AllShortest_TwoSharedIntermediates) {
    // Graph:
    //   1 --(1)--> 2 --(1)--> 3 --(1)--> 4 --(1)--> 5
    //   1 --(1)--> 6 --(1)--> 3 --(1)--> 4 --(1)--> 5
    //   1 --(1)--> 2 --(1)--> 7 --(2)--> 5  (cost 4, not shortest)
    // Two shortest paths 1->2->3->4->5 and 1->6->3->4->5 (cost 4 each).
    for (int64_t id : {1, 2, 3, 4, 5, 6, 7})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 3, 1.0);
    link(3, 4, 1.0);
    link(4, 5, 1.0);
    link(1, 6, 1.0);
    link(6, 3, 1.0);
    link(2, 7, 1.0);
    link(7, 5, 2.0); // cost 4 via 1->2->7->5 (same cost but different path)

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_5 = filter_pair(results, 1, 5);

    // Should find at least 2 equal-cost paths (through 2->3->4 and 6->3->4),
    // and possibly the 1->2->7->5 path too (also cost 4).
    EXPECT_GE(from_1_to_5.size(), 2u) << "ALL_SHORTEST should find multiple equal-cost paths";

    // All returned paths should have the same cost.
    double expected_cost = from_1_to_5[0]->values[2].as_path().total_weight;
    for (const auto* t : from_1_to_5) {
        EXPECT_DOUBLE_EQ(t->values[2].as_path().total_weight, expected_cost);
    }
}

// ============================================================================
// Edge case: all edges same weight, diamond graph
// ============================================================================

TEST_F(QA_GDB555, AllShortest_Diamond_EqualWeights) {
    // Diamond: 1->(2 and 3)->4, all weights 1.0
    for (int64_t id : {1, 2, 3, 4})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(1, 3, 1.0);
    link(2, 4, 1.0);
    link(3, 4, 1.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_4 = filter_pair(results, 1, 4);

    EXPECT_EQ(from_1_to_4.size(), 2u)
        << "Diamond with equal weights: 2 shortest paths 1->2->4 and 1->3->4";
}

// Edge case: unequal weights, only one shortest path.
// The "1->2->4 (cost 2) vs 1->3->4 (cost 10), expect only the shortest" repro
// lives in test_qa_gdb_559.cpp as QA_GDB559.AllShortest_TicketRepro_OnlyShortestReturned
// (GDB-559 owns the ALL_SHORTEST cost-filtering fix). It was a verbatim duplicate
// here and was removed to avoid lockstep maintenance (GDB-1010).

// ============================================================================
// Edge case: zero-weight edges with shared intermediate
// ============================================================================

TEST_F(QA_GDB555, AllShortest_ZeroWeightSharedNode) {
    // 1--(0)-->2--(0)-->3
    // 1--(0)-->4--(0)-->3
    for (int64_t id : {1, 2, 3, 4})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 0.0);
    link(2, 3, 0.0);
    link(1, 4, 0.0);
    link(4, 3, 0.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_3 = filter_pair(results, 1, 3);

    EXPECT_EQ(from_1_to_3.size(), 2u)
        << "ALL_SHORTEST with zero-weight edges should find both paths";
    for (const auto* t : from_1_to_3) {
        EXPECT_DOUBLE_EQ(t->values[2].as_path().total_weight, 0.0);
    }
}

} // namespace
} // namespace sixseven
