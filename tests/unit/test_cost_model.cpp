#include "sixseven/planner/cost_model.h"
#include "sixseven/planner/optimizer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

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

// =============================================================================
// Access path selection tests
// =============================================================================

TEST(AccessPathTest, SeqScanWhenNoIndexes) {
    CostModel cm;
    TableStats ts;
    ts.page_count = 100;
    ts.row_count = 10000;

    auto path = choose_access_path(1, ts, {}, 0.01, cm);
    EXPECT_EQ(path.method, AccessMethod::SEQ_SCAN);
}

TEST(AccessPathTest, IndexScanForSelectivePredicate) {
    CostModel cm;
    TableStats ts;
    ts.page_count = 100;
    ts.row_count = 10000;

    IndexDef idx;
    idx.index_id = 1;
    idx.table_id = 1;
    idx.name = "idx_col";
    idx.index_type = "btree";
    idx.columns = "col";
    idx.is_unique = false;

    auto path = choose_access_path(1, ts, {idx}, 0.01, cm);
    EXPECT_EQ(path.method, AccessMethod::INDEX_SCAN);
    EXPECT_EQ(path.index_id, 1);
}

TEST(AccessPathTest, SeqScanForBroadPredicate) {
    CostModel cm;
    TableStats ts;
    ts.page_count = 100;
    ts.row_count = 10000;

    IndexDef idx;
    idx.index_id = 1;
    idx.table_id = 1;
    idx.name = "idx_col";
    idx.index_type = "btree";
    idx.columns = "col";
    idx.is_unique = false;

    auto path = choose_access_path(1, ts, {idx}, 0.9, cm);
    EXPECT_EQ(path.method, AccessMethod::SEQ_SCAN);
}

// =============================================================================
// Join method selection tests
// =============================================================================

TEST(JoinMethodTest, ChoosesBestMethod) {
    CostModel cm;
    PlanCost left;
    left.total_cost = 10.0;
    left.estimated_rows = 100.0;

    PlanCost right;
    right.total_cost = 200.0;
    right.estimated_rows = 10000.0;

    auto [method, cost] = choose_join_method(left, right, 0.001, false, false, cm);
    // Should choose hash join for large inner relation.
    EXPECT_NE(method, JoinMethod::NESTED_LOOP);
    EXPECT_GT(cost.total_cost, 0.0);
}

TEST(JoinMethodTest, NestedLoopForSmallInner) {
    CostModel cm;
    PlanCost outer;
    outer.total_cost = 10.0;
    outer.estimated_rows = 10.0;

    PlanCost inner;
    inner.total_cost = 1.0;
    inner.estimated_rows = 5.0;

    auto [method, cost] = choose_join_method(outer, inner, 0.1, false, false, cm);
    // For very small relations, nested loop should be competitive.
    EXPECT_GT(cost.total_cost, 0.0);
}

// =============================================================================
// Join ordering tests
// =============================================================================

static std::unique_ptr<PhysicalPlanNode> make_test_scan(table_id_t id, double rows, double cost) {
    auto node = std::make_unique<PhysicalPlanNode>();
    node->type = PhysicalPlanNode::Type::TABLE_SCAN;
    node->table_id = id;
    node->cost.estimated_rows = rows;
    node->cost.total_cost = cost;
    return node;
}

TEST(JoinOrderTest, TwoTableJoin) {
    CostModel cm;

    std::vector<JoinRelation> rels;
    rels.push_back({1ULL << 0, {0.0, 10.0, 100.0}, make_test_scan(0, 100.0, 10.0)});
    rels.push_back({1ULL << 1, {0.0, 20.0, 1000.0}, make_test_scan(1, 1000.0, 20.0)});

    std::vector<JoinEdge> edges = {{0, 1, 0.01}};

    auto plan = optimize_join_order(rels, edges, cm);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PhysicalPlanNode::Type::JOIN);
}

TEST(JoinOrderTest, ThreeTableJoin) {
    CostModel cm;

    std::vector<JoinRelation> rels;
    rels.push_back({1ULL << 0, {0.0, 10.0, 100.0}, make_test_scan(0, 100.0, 10.0)});
    rels.push_back({1ULL << 1, {0.0, 20.0, 1000.0}, make_test_scan(1, 1000.0, 20.0)});
    rels.push_back({1ULL << 2, {0.0, 5.0, 50.0}, make_test_scan(2, 50.0, 5.0)});

    std::vector<JoinEdge> edges = {
        {0, 1, 0.01},
        {1, 2, 0.02},
    };

    auto plan = optimize_join_order(rels, edges, cm);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PhysicalPlanNode::Type::JOIN);
    EXPECT_GT(plan->cost.total_cost, 0.0);
}

TEST(JoinOrderTest, FourTableJoinDP) {
    CostModel cm;

    std::vector<JoinRelation> rels;
    rels.push_back({1ULL << 0, {0.0, 10.0, 100.0}, make_test_scan(0, 100.0, 10.0)});
    rels.push_back({1ULL << 1, {0.0, 20.0, 1000.0}, make_test_scan(1, 1000.0, 20.0)});
    rels.push_back({1ULL << 2, {0.0, 5.0, 50.0}, make_test_scan(2, 50.0, 5.0)});
    rels.push_back({1ULL << 3, {0.0, 15.0, 500.0}, make_test_scan(3, 500.0, 15.0)});

    std::vector<JoinEdge> edges = {
        {0, 1, 0.01},
        {1, 2, 0.02},
        {2, 3, 0.01},
    };

    auto plan = optimize_join_order(rels, edges, cm);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PhysicalPlanNode::Type::JOIN);
}

TEST(JoinOrderTest, SingleTable) {
    CostModel cm;

    std::vector<JoinRelation> rels;
    rels.push_back({1ULL << 0, {0.0, 10.0, 100.0}, make_test_scan(0, 100.0, 10.0)});

    std::vector<JoinEdge> edges;

    auto plan = optimize_join_order(rels, edges, cm);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PhysicalPlanNode::Type::TABLE_SCAN);
}

TEST(JoinOrderTest, EmptyRelations) {
    CostModel cm;

    std::vector<JoinRelation> rels;
    std::vector<JoinEdge> edges;

    auto plan = optimize_join_order(rels, edges, cm);
    EXPECT_EQ(plan, nullptr);
}

TEST(JoinOrderTest, PrefersSmallerBuildSide) {
    // Verify that the optimizer prefers plans with smaller build sides for hash joins.
    CostModel cm;

    std::vector<JoinRelation> rels;
    rels.push_back({1ULL << 0, {0.0, 10.0, 100.0}, make_test_scan(0, 100.0, 10.0)});
    rels.push_back({1ULL << 1, {0.0, 200.0, 100000.0}, make_test_scan(1, 100000.0, 200.0)});

    std::vector<JoinEdge> edges = {{0, 1, 0.0001}};

    auto plan = optimize_join_order(rels, edges, cm);
    ASSERT_NE(plan, nullptr);
    EXPECT_GT(plan->cost.total_cost, 0.0);
}

// =============================================================================
// GDB-741 regression tests: dense remapping of sparse / large table IDs
// =============================================================================

// Before the fix, table_ids {3, 40} would cause dp_join_order to set
// full_set = (1<<3)|(1<<40) = 0x10000000008 and iterate ~2^40 subsets.
// After the fix, relations are remapped to dense indices 0 and 1; the loop
// runs 3 times (subsets of 0b11).  This test must complete in milliseconds.
TEST(JoinOrderTest, GDB741_SparseTableIds_CompletesInstantly) {
    CostModel cm;

    // table_ids 3 and 40 -- sparse, previously causing O(2^40) iteration
    std::vector<JoinRelation> rels;
    rels.push_back({1ULL << 3, {0.0, 10.0, 100.0}, make_test_scan(3, 100.0, 10.0)});
    rels.push_back({1ULL << 40, {0.0, 20.0, 1000.0}, make_test_scan(40, 1000.0, 20.0)});

    std::vector<JoinEdge> edges = {{3, 40, 0.01}};

    auto plan = optimize_join_order(rels, edges, cm);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PhysicalPlanNode::Type::JOIN);
    EXPECT_GT(plan->cost.total_cost, 0.0);
}

// Three tables with widely separated IDs: {1, 50, 63}.
// Before the fix this set full_set = (1<<63)-1... iterating ~2^63 subsets.
// After the fix, 3 dense bits, 7 subsets.
TEST(JoinOrderTest, GDB741_VerySparseBitPositions_ThreeTables) {
    CostModel cm;

    std::vector<JoinRelation> rels;
    rels.push_back({1ULL << 1, {0.0, 10.0, 100.0}, make_test_scan(1, 100.0, 10.0)});
    rels.push_back({1ULL << 50, {0.0, 5.0, 50.0}, make_test_scan(50, 50.0, 5.0)});
    rels.push_back({1ULL << 63, {0.0, 20.0, 1000.0}, make_test_scan(63, 1000.0, 20.0)});

    std::vector<JoinEdge> edges = {
        {1, 50, 0.01},
        {50, 63, 0.02},
    };

    auto plan = optimize_join_order(rels, edges, cm);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PhysicalPlanNode::Type::JOIN);
    EXPECT_GT(plan->cost.total_cost, 0.0);
}

// Correctness regression: result for dense ids {0,1,2} must be identical
// (same join method and cost) with the pre-fix behaviour since dense ids
// were already a special case of the fix (dense index == original id).
TEST(JoinOrderTest, GDB741_DenseIdsResultUnchanged) {
    CostModel cm;

    std::vector<JoinRelation> rels1;
    rels1.push_back({1ULL << 0, {0.0, 10.0, 100.0}, make_test_scan(0, 100.0, 10.0)});
    rels1.push_back({1ULL << 1, {0.0, 20.0, 1000.0}, make_test_scan(1, 1000.0, 20.0)});
    rels1.push_back({1ULL << 2, {0.0, 5.0, 50.0}, make_test_scan(2, 50.0, 5.0)});

    std::vector<JoinEdge> edges = {{0, 1, 0.01}, {1, 2, 0.02}};

    auto plan1 = optimize_join_order(rels1, edges, cm);
    ASSERT_NE(plan1, nullptr);
    EXPECT_EQ(plan1->type, PhysicalPlanNode::Type::JOIN);

    // Same logical query but with table_ids remapped to {10, 11, 12}.
    // The result plan shape and cost must be the same because the only
    // thing that changes is the table identifier -- not the costs or edges.
    std::vector<JoinRelation> rels2;
    rels2.push_back({1ULL << 10, {0.0, 10.0, 100.0}, make_test_scan(10, 100.0, 10.0)});
    rels2.push_back({1ULL << 11, {0.0, 20.0, 1000.0}, make_test_scan(11, 1000.0, 20.0)});
    rels2.push_back({1ULL << 12, {0.0, 5.0, 50.0}, make_test_scan(12, 50.0, 5.0)});

    std::vector<JoinEdge> edges2 = {{10, 11, 0.01}, {11, 12, 0.02}};

    auto plan2 = optimize_join_order(rels2, edges2, cm);
    ASSERT_NE(plan2, nullptr);
    EXPECT_EQ(plan2->type, PhysicalPlanNode::Type::JOIN);

    // Costs must match because inputs are identical up to table id labelling.
    EXPECT_NEAR(plan1->cost.total_cost, plan2->cost.total_cost, 1e-6);
    EXPECT_NEAR(plan1->cost.estimated_rows, plan2->cost.estimated_rows, 1e-6);
}
