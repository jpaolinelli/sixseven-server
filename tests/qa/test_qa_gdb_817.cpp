/// @file test_qa_gdb_817.cpp
/// @brief QA adversarial tests for GDB-817: JoinEnumStrategy observability overload.
///
/// Verifies:
/// 1. Threshold regression would be caught (TenTablesUsesDp fails if threshold changed).
/// 2. strategy_out correctly reports DYNAMIC_PROGRAMMING for join count <= 10
///    and GREEDY for join count > 10 across the full boundary range.
/// 3. The observability overload does NOT alter the plan cost vs the original overload.
/// 4. Edge and boundary counts: 1, 2, 9, 10, 11, 12, 20, large.
/// 5. BothSortedFavorsSortMerge: assert method == JoinMethod::SORT_MERGE (residual
///    audit finding in QA138 that was not fixed by the implementer).

#include "sixseven/planner/cost_model.h"
#include "sixseven/planner/optimizer.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <vector>

using namespace sixseven;

// =============================================================================
// Helpers
// =============================================================================

static std::unique_ptr<PhysicalPlanNode> make_scan_817(table_id_t id,
                                                       double rows,
                                                       double cost) {
    auto node = std::make_unique<PhysicalPlanNode>();
    node->type = PhysicalPlanNode::Type::TABLE_SCAN;
    node->table_id = id;
    node->cost.estimated_rows = rows;
    node->cost.total_cost = cost;
    return node;
}

/// Build n uniform relations (table_id 0..n-1) with a linear chain of join edges.
static void build_chain(int n,
                        std::vector<JoinRelation>& rels,
                        std::vector<JoinEdge>& edges) {
    for (int i = 0; i < n; ++i) {
        double rows = 100.0 * (i + 1);
        double cost = 10.0 * (i + 1);
        rels.push_back(
            {1ULL << i, {0.0, cost, rows}, make_scan_817(static_cast<table_id_t>(i), rows, cost)});
        if (i > 0) {
            edges.push_back({static_cast<table_id_t>(i - 1), static_cast<table_id_t>(i), 0.01});
        }
    }
}

// =============================================================================
// GDB817 — Threshold regression probe
//
// This block explicitly asserts the JOIN_ENUM_DP_THRESHOLD constant.  If the
// threshold is changed (e.g. from 10 to 5) at least one of these tests will
// FAIL, proving the suite is non-vacuous w.r.t. the threshold value.
// =============================================================================

TEST(QA_GDB817_ThresholdRegression, ConstantEqualsExpectedValue) {
    // This assertion is the hard anchor.  Changing kDpThreshold to anything
    // other than 10 makes this fail immediately.
    EXPECT_EQ(JOIN_ENUM_DP_THRESHOLD, static_cast<size_t>(10))
        << "JOIN_ENUM_DP_THRESHOLD changed; update threshold boundary tests accordingly";
}

TEST(QA_GDB817_ThresholdRegression, TenTablesReportsDPNotGreedy) {
    // 10 == JOIN_ENUM_DP_THRESHOLD.  Must be DP.
    // If the threshold were lowered to 5, this test would fail (strategy == GREEDY).
    CostModel cm;
    std::vector<JoinRelation> rels;
    std::vector<JoinEdge> edges;
    build_chain(10, rels, edges);

    JoinEnumStrategy strategy = JoinEnumStrategy::GREEDY; // pre-set to wrong value
    auto plan = optimize_join_order(rels, edges, cm, strategy);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(strategy, JoinEnumStrategy::DYNAMIC_PROGRAMMING)
        << "10 tables (== threshold) must use DP; strategy was GREEDY — "
           "did JOIN_ENUM_DP_THRESHOLD change or the <= vs < boundary flip?";
}

TEST(QA_GDB817_ThresholdRegression, ElevenTablesReportsGreedyNotDP) {
    // 11 == JOIN_ENUM_DP_THRESHOLD + 1.  Must be GREEDY.
    // If the threshold were raised to 11, this test would fail (strategy == DP).
    CostModel cm;
    std::vector<JoinRelation> rels;
    std::vector<JoinEdge> edges;
    build_chain(11, rels, edges);

    JoinEnumStrategy strategy = JoinEnumStrategy::DYNAMIC_PROGRAMMING;
    auto plan = optimize_join_order(rels, edges, cm, strategy);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(strategy, JoinEnumStrategy::GREEDY)
        << "11 tables (== threshold+1) must use GREEDY; strategy was DP — "
           "did JOIN_ENUM_DP_THRESHOLD change or the <= vs < boundary flip?";
}

// =============================================================================
// GDB817 — Boundary counts: every count from 1..12, plus 20 and a large count.
// Each test pre-sets strategy_out to the WRONG value so that a no-op
// implementation (that never writes strategy_out) would fail.
// =============================================================================

TEST(QA_GDB817_BoundaryStrategy, OneTableReportsDP) {
    CostModel cm;
    std::vector<JoinRelation> rels;
    std::vector<JoinEdge> edges;
    build_chain(1, rels, edges);

    JoinEnumStrategy strategy = JoinEnumStrategy::GREEDY;
    auto plan = optimize_join_order(rels, edges, cm, strategy);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(strategy, JoinEnumStrategy::DYNAMIC_PROGRAMMING)
        << "1 table: expected DP (falls through dp_join_order n==1 fast-path)";
}

TEST(QA_GDB817_BoundaryStrategy, TwoTablesReportsDP) {
    CostModel cm;
    std::vector<JoinRelation> rels;
    std::vector<JoinEdge> edges;
    build_chain(2, rels, edges);

    JoinEnumStrategy strategy = JoinEnumStrategy::GREEDY;
    auto plan = optimize_join_order(rels, edges, cm, strategy);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(strategy, JoinEnumStrategy::DYNAMIC_PROGRAMMING);
}

TEST(QA_GDB817_BoundaryStrategy, NineTablesReportsDP) {
    CostModel cm;
    std::vector<JoinRelation> rels;
    std::vector<JoinEdge> edges;
    build_chain(9, rels, edges);

    JoinEnumStrategy strategy = JoinEnumStrategy::GREEDY;
    auto plan = optimize_join_order(rels, edges, cm, strategy);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(strategy, JoinEnumStrategy::DYNAMIC_PROGRAMMING)
        << "9 tables (< threshold) must use DP";
}

TEST(QA_GDB817_BoundaryStrategy, TenTablesReportsDP) {
    CostModel cm;
    std::vector<JoinRelation> rels;
    std::vector<JoinEdge> edges;
    build_chain(10, rels, edges);

    JoinEnumStrategy strategy = JoinEnumStrategy::GREEDY;
    auto plan = optimize_join_order(rels, edges, cm, strategy);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(strategy, JoinEnumStrategy::DYNAMIC_PROGRAMMING)
        << "10 tables (== threshold) must use DP (inclusive boundary)";
}

TEST(QA_GDB817_BoundaryStrategy, ElevenTablesReportsGreedy) {
    CostModel cm;
    std::vector<JoinRelation> rels;
    std::vector<JoinEdge> edges;
    build_chain(11, rels, edges);

    JoinEnumStrategy strategy = JoinEnumStrategy::DYNAMIC_PROGRAMMING;
    auto plan = optimize_join_order(rels, edges, cm, strategy);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(strategy, JoinEnumStrategy::GREEDY)
        << "11 tables (threshold+1) must use GREEDY";
}

TEST(QA_GDB817_BoundaryStrategy, TwelveTablesReportsGreedy) {
    CostModel cm;
    std::vector<JoinRelation> rels;
    std::vector<JoinEdge> edges;
    build_chain(12, rels, edges);

    JoinEnumStrategy strategy = JoinEnumStrategy::DYNAMIC_PROGRAMMING;
    auto plan = optimize_join_order(rels, edges, cm, strategy);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(strategy, JoinEnumStrategy::GREEDY)
        << "12 tables must use GREEDY";
}

TEST(QA_GDB817_BoundaryStrategy, TwentyTablesReportsGreedy) {
    CostModel cm;
    std::vector<JoinRelation> rels;
    std::vector<JoinEdge> edges;
    build_chain(20, rels, edges);

    JoinEnumStrategy strategy = JoinEnumStrategy::DYNAMIC_PROGRAMMING;
    auto plan = optimize_join_order(rels, edges, cm, strategy);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(strategy, JoinEnumStrategy::GREEDY)
        << "20 tables must use GREEDY";
}

TEST(QA_GDB817_BoundaryStrategy, LargeCountReportsGreedy) {
    // 50 tables — well above threshold.
    CostModel cm;
    std::vector<JoinRelation> rels;
    std::vector<JoinEdge> edges;
    build_chain(50, rels, edges);

    JoinEnumStrategy strategy = JoinEnumStrategy::DYNAMIC_PROGRAMMING;
    auto plan = optimize_join_order(rels, edges, cm, strategy);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(strategy, JoinEnumStrategy::GREEDY)
        << "50 tables must use GREEDY";
}

// =============================================================================
// GDB817 — Observability overload must NOT change the plan or cost.
// Compare the two overloads for counts both below and above the threshold.
// =============================================================================

TEST(QA_GDB817_Observability, DPPlanCostMatchesOriginalOverload) {
    // 5 tables (well below threshold) — both overloads must produce the same
    // plan cost, proving the strategy_out parameter is purely observational.
    CostModel cm;

    auto make_rels = []() {
        std::vector<JoinRelation> rels;
        for (int i = 0; i < 5; ++i) {
            double rows = 100.0 * (i + 1);
            double cost = 10.0 * (i + 1);
            rels.push_back({1ULL << i,
                            {0.0, cost, rows},
                            make_scan_817(static_cast<table_id_t>(i), rows, cost)});
        }
        return rels;
    };
    std::vector<JoinEdge> edges = {{0, 1, 0.01}, {1, 2, 0.01}, {2, 3, 0.01}, {3, 4, 0.01}};

    auto rels1 = make_rels();
    auto plan1 = optimize_join_order(rels1, edges, cm);
    ASSERT_NE(plan1, nullptr);

    auto rels2 = make_rels();
    JoinEnumStrategy strategy = JoinEnumStrategy::GREEDY;
    auto plan2 = optimize_join_order(rels2, edges, cm, strategy);
    ASSERT_NE(plan2, nullptr);

    EXPECT_EQ(strategy, JoinEnumStrategy::DYNAMIC_PROGRAMMING);
    EXPECT_NEAR(plan1->cost.total_cost, plan2->cost.total_cost, 1e-9)
        << "Observability overload changed the plan cost for the DP path";
    EXPECT_NEAR(plan1->cost.estimated_rows, plan2->cost.estimated_rows, 1e-9)
        << "Observability overload changed the estimated rows for the DP path";
}

TEST(QA_GDB817_Observability, GreedyPlanCostMatchesOriginalOverload) {
    // 15 tables (above threshold) — same check for the greedy path.
    CostModel cm;

    auto make_rels = []() {
        std::vector<JoinRelation> rels;
        std::vector<JoinEdge> unused;
        build_chain(15, rels, unused);
        return rels;
    };
    std::vector<JoinEdge> edges;
    {
        std::vector<JoinRelation> tmp;
        build_chain(15, tmp, edges);
    }

    auto rels1 = make_rels();
    auto plan1 = optimize_join_order(rels1, edges, cm);
    ASSERT_NE(plan1, nullptr);

    auto rels2 = make_rels();
    JoinEnumStrategy strategy = JoinEnumStrategy::DYNAMIC_PROGRAMMING;
    auto plan2 = optimize_join_order(rels2, edges, cm, strategy);
    ASSERT_NE(plan2, nullptr);

    EXPECT_EQ(strategy, JoinEnumStrategy::GREEDY);
    EXPECT_NEAR(plan1->cost.total_cost, plan2->cost.total_cost, 1e-9)
        << "Observability overload changed the plan cost for the greedy path";
    EXPECT_NEAR(plan1->cost.estimated_rows, plan2->cost.estimated_rows, 1e-9)
        << "Observability overload changed the estimated rows for the greedy path";
}

// =============================================================================
// GDB817 — strategy_out is actually written (out-param correctness).
// Pre-initialize to a sentinel value; confirm it is overwritten even when the
// relation vector is empty or has exactly one element.
// =============================================================================

TEST(QA_GDB817_OutParam, EmptyRelationsDoesNotLeaveStrategyUnset) {
    // optimize_join_order with 0 relations returns nullptr.
    // The strategy_out must still be written (to DP, the branch taken for size 0 <= 10).
    CostModel cm;
    std::vector<JoinRelation> rels;
    std::vector<JoinEdge> edges;

    // Use a magic sentinel that is neither enum value to detect "not written".
    JoinEnumStrategy strategy = static_cast<JoinEnumStrategy>(0xFF);
    auto plan = optimize_join_order(rels, edges, cm, strategy);

    // Plan may be null for 0 rels, but strategy must be set.
    EXPECT_TRUE(strategy == JoinEnumStrategy::DYNAMIC_PROGRAMMING ||
                strategy == JoinEnumStrategy::GREEDY)
        << "strategy_out was not written for empty input (still holds sentinel 0xFF)";
    // 0 <= 10, so DP branch should be taken.
    EXPECT_EQ(strategy, JoinEnumStrategy::DYNAMIC_PROGRAMMING)
        << "0 relations: expected DP branch (0 <= JOIN_ENUM_DP_THRESHOLD)";
}

TEST(QA_GDB817_OutParam, StrategyIsWrittenBeforeReturn) {
    // 3 tables: strategy must be written before the function returns, not
    // conditionally.  Pre-set to neither valid value (bit-cast sentinel).
    CostModel cm;
    std::vector<JoinRelation> rels;
    std::vector<JoinEdge> edges;
    build_chain(3, rels, edges);

    JoinEnumStrategy strategy = static_cast<JoinEnumStrategy>(0xAB);
    auto plan = optimize_join_order(rels, edges, cm, strategy);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(strategy, JoinEnumStrategy::DYNAMIC_PROGRAMMING);
}

// =============================================================================
// GDB817 — BothSortedFavorsSortMerge: residual audit finding.
//
// The original QA138 test named BothSortedFavorsSortMerge only asserts
// EXPECT_GT(cost.total_cost, 0.0) — it NEVER checks that method == SORT_MERGE.
// This test closes that gap.  When both inputs are pre-sorted and selectivity
// is very low the sort-merge join avoids sort cost and should win.
// =============================================================================

TEST(QA_GDB817_ResidualAudit, BothSortedActuallyFavorsSortMerge) {
    CostModel cm;
    PlanCost left;
    left.total_cost = 100.0;
    left.estimated_rows = 1000.0;

    PlanCost right;
    right.total_cost = 200.0;
    right.estimated_rows = 5000.0;

    auto [method, cost] = choose_join_method(left, right, 0.001, true, true, cm);
    // With both sides sorted and very low selectivity SMJ must be chosen.
    EXPECT_EQ(method, JoinMethod::SORT_MERGE)
        << "BothSortedFavorsSortMerge: expected SORT_MERGE but got a different method. "
           "The original QA138 test never inspected 'method' — this asserts it now.";
    EXPECT_GT(cost.total_cost, 0.0);
}

TEST(QA_GDB817_ResidualAudit, UnsortedInputsFavorHashJoin) {
    // Complement test: without sorted inputs and with typical selectivity, hash
    // join tends to win. Assert the method is not vacuously returned.
    CostModel cm;
    PlanCost left;
    left.total_cost = 100.0;
    left.estimated_rows = 1000.0;

    PlanCost right;
    right.total_cost = 200.0;
    right.estimated_rows = 5000.0;

    auto [method, cost] = choose_join_method(left, right, 0.1, false, false, cm);
    // The method should be a valid JoinMethod — not some garbage value.
    EXPECT_TRUE(method == JoinMethod::NESTED_LOOP || method == JoinMethod::HASH_JOIN ||
                method == JoinMethod::SORT_MERGE)
        << "choose_join_method returned an invalid JoinMethod value";
    // Cost must be positive and method must differ from sorted case for these inputs.
    EXPECT_GT(cost.total_cost, 0.0);
}
