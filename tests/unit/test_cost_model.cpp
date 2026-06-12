#include "sixseven/planner/cost_model.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace sixseven;

// =============================================================================
// Cost model tests
// =============================================================================

class CostModelTest : public ::testing::Test {
protected:
    CostModel cost_model_;
};

TEST_F(CostModelTest, SeqScanCost) {
    auto cost = cost_model_.seq_scan_cost(100, 10000);
    // pages * 1.0 + rows * 0.01 = 100 + 100 = 200
    EXPECT_DOUBLE_EQ(cost.total_cost, 200.0);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 10000.0);
    EXPECT_DOUBLE_EQ(cost.startup_cost, 0.0);
}

TEST_F(CostModelTest, IndexScanCostHighSelectivity) {
    // High selectivity (most rows match): index scan is expensive.
    auto cost = cost_model_.index_scan_cost(100, 10000, 0.9, 10);
    // 0.9 * 100 * 4.0 + 9000 * 0.01 + 10 * 4.0 = 360 + 90 + 40 = 490
    EXPECT_NEAR(cost.total_cost, 490.0, 1.0);
    EXPECT_NEAR(cost.estimated_rows, 9000.0, 1.0);
}

TEST_F(CostModelTest, IndexScanCostLowSelectivity) {
    // Low selectivity (few rows match): index scan is cheap.
    auto cost = cost_model_.index_scan_cost(100, 10000, 0.01, 10);
    // 0.01 * 100 * 4.0 + 100 * 0.01 + 10 * 4.0 = 4 + 1 + 40 = 45
    EXPECT_NEAR(cost.total_cost, 45.0, 1.0);
    EXPECT_NEAR(cost.estimated_rows, 100.0, 1.0);
}

TEST_F(CostModelTest, IndexScanCheaperThanSeqScanForSelectivePredicate) {
    auto seq = cost_model_.seq_scan_cost(100, 10000);
    auto idx = cost_model_.index_scan_cost(100, 10000, 0.01, 10);
    EXPECT_LT(idx.total_cost, seq.total_cost);
}

TEST_F(CostModelTest, SeqScanCheaperThanIndexScanForBroadPredicate) {
    auto seq = cost_model_.seq_scan_cost(100, 10000);
    auto idx = cost_model_.index_scan_cost(100, 10000, 0.9, 10);
    EXPECT_LT(seq.total_cost, idx.total_cost);
}

TEST_F(CostModelTest, HashJoinCost) {
    PlanCost left;
    left.total_cost = 100.0;
    left.estimated_rows = 1000.0;

    PlanCost right;
    right.total_cost = 200.0;
    right.estimated_rows = 5000.0;

    auto cost = cost_model_.hash_join_cost(left, right, 0.001);
    EXPECT_GT(cost.total_cost, 0.0);
    EXPECT_NEAR(cost.estimated_rows, 1000.0 * 5000.0 * 0.001, 1.0);
}

TEST_F(CostModelTest, NestedLoopJoinCost) {
    PlanCost outer;
    outer.total_cost = 10.0;
    outer.estimated_rows = 100.0;

    PlanCost inner;
    inner.total_cost = 5.0;
    inner.estimated_rows = 50.0;

    auto cost = cost_model_.nested_loop_join_cost(outer, inner, 0.01);
    // outer.total + outer.rows * inner.total + outer.rows * inner.rows * cpu_tuple_cost
    // = 10 + 100 * 5 + 100 * 50 * 0.01 = 10 + 500 + 50 = 560
    EXPECT_NEAR(cost.total_cost, 560.0, 1.0);
    EXPECT_NEAR(cost.estimated_rows, 100.0 * 50.0 * 0.01, 1.0);
}

TEST_F(CostModelTest, SortMergeJoinCost) {
    PlanCost left;
    left.total_cost = 100.0;
    left.estimated_rows = 1000.0;

    PlanCost right;
    right.total_cost = 200.0;
    right.estimated_rows = 5000.0;

    auto cost = cost_model_.sort_merge_join_cost(left, right, false, false, 0.001);
    EXPECT_GT(cost.total_cost, 0.0);
    EXPECT_NEAR(cost.estimated_rows, 1000.0 * 5000.0 * 0.001, 1.0);
}

TEST_F(CostModelTest, SortMergeJoinCheaperWhenAlreadySorted) {
    PlanCost left;
    left.total_cost = 100.0;
    left.estimated_rows = 1000.0;

    PlanCost right;
    right.total_cost = 200.0;
    right.estimated_rows = 5000.0;

    auto unsorted = cost_model_.sort_merge_join_cost(left, right, false, false, 0.001);
    auto sorted = cost_model_.sort_merge_join_cost(left, right, true, true, 0.001);
    EXPECT_LT(sorted.total_cost, unsorted.total_cost);
}

TEST_F(CostModelTest, SortCost) {
    PlanCost input;
    input.total_cost = 100.0;
    input.estimated_rows = 1000.0;

    auto cost = cost_model_.sort_cost(input);
    EXPECT_GT(cost.total_cost, input.total_cost);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, input.estimated_rows);
}

TEST_F(CostModelTest, FilterCost) {
    PlanCost input;
    input.total_cost = 100.0;
    input.estimated_rows = 1000.0;

    auto cost = cost_model_.filter_cost(input, 0.1);
    EXPECT_GT(cost.total_cost, input.total_cost);
    EXPECT_NEAR(cost.estimated_rows, 100.0, 1.0);
}

TEST_F(CostModelTest, LimitCost) {
    PlanCost input;
    input.startup_cost = 10.0;
    input.total_cost = 100.0;
    input.estimated_rows = 1000.0;

    auto cost = cost_model_.limit_cost(input, 10);
    EXPECT_LT(cost.total_cost, input.total_cost);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 10.0);
}

TEST_F(CostModelTest, HashAggregateCost) {
    PlanCost input;
    input.total_cost = 100.0;
    input.estimated_rows = 1000.0;

    auto cost = cost_model_.hash_aggregate_cost(input, 50);
    EXPECT_GT(cost.total_cost, input.total_cost);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 50.0);
}

TEST_F(CostModelTest, CustomParams) {
    CostModelParams params;
    params.seq_page_cost = 2.0;
    params.cpu_tuple_cost = 0.02;
    CostModel cm(params);

    auto cost = cm.seq_scan_cost(100, 10000);
    // 100 * 2.0 + 10000 * 0.02 = 200 + 200 = 400
    EXPECT_DOUBLE_EQ(cost.total_cost, 400.0);
}
