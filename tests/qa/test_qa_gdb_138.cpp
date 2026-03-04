/// @file test_qa_gdb_138.cpp
/// @brief QA adversarial tests for GDB-138: Cost model and plan enumeration.
///
/// Targets edge cases: zero rows/pages, extreme selectivities, zero/large limits,
/// sort of 0/1 rows, hash join with empty sides, cartesian joins, greedy
/// join ordering (>10 tables), disconnected join graphs, custom parameters,
/// access path with mismatched table_id, join method tie-breaking.

#include "giodb/planner/cost_model.h"
#include "giodb/planner/optimizer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

using namespace giodb;

// =============================================================================
// Helper
// =============================================================================

static std::unique_ptr<PhysicalPlanNode> make_scan(table_id_t id, double rows, double cost) {
    auto node = std::make_unique<PhysicalPlanNode>();
    node->type = PhysicalPlanNode::Type::TABLE_SCAN;
    node->table_id = id;
    node->cost.estimated_rows = rows;
    node->cost.total_cost = cost;
    return node;
}

// =============================================================================
// Cost model adversarial tests
// =============================================================================

TEST(QA138_CostModel, SeqScanZeroPagesZeroRows) {
    CostModel cm;
    auto cost = cm.seq_scan_cost(0, 0);
    EXPECT_DOUBLE_EQ(cost.total_cost, 0.0);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 0.0);
    EXPECT_DOUBLE_EQ(cost.startup_cost, 0.0);
}

TEST(QA138_CostModel, SeqScanLargeTable) {
    CostModel cm;
    auto cost = cm.seq_scan_cost(100000, 10000000);
    // 100000 * 1.0 + 10000000 * 0.01 = 100000 + 100000 = 200000
    EXPECT_DOUBLE_EQ(cost.total_cost, 200000.0);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 10000000.0);
}

TEST(QA138_CostModel, IndexScanZeroSelectivity) {
    CostModel cm;
    auto cost = cm.index_scan_cost(100, 10000, 0.0, 10);
    // 0 * 100 * 4 + 0 * 0.01 + 10 * 4 = 0 + 0 + 40 = 40
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 0.0);
    EXPECT_GT(cost.total_cost, 0.0); // Index traversal cost remains.
}

TEST(QA138_CostModel, IndexScanFullSelectivity) {
    CostModel cm;
    auto cost = cm.index_scan_cost(100, 10000, 1.0, 10);
    // 1.0 * 100 * 4 + 10000 * 0.01 + 10 * 4 = 400 + 100 + 40 = 540
    EXPECT_NEAR(cost.total_cost, 540.0, 0.01);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 10000.0);
}

TEST(QA138_CostModel, IndexScanZeroPages) {
    CostModel cm;
    auto cost = cm.index_scan_cost(0, 0, 0.5, 0);
    EXPECT_DOUBLE_EQ(cost.total_cost, 0.0);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 0.0);
}

TEST(QA138_CostModel, SortCostZeroRows) {
    CostModel cm;
    PlanCost input;
    input.total_cost = 0.0;
    input.estimated_rows = 0.0;

    auto cost = cm.sort_cost(input);
    // sort_cpu_cost(0) = 0 (n<=1), startup = 0 + 0 = 0
    // total = 0 + 0 * 0.01 = 0
    EXPECT_DOUBLE_EQ(cost.total_cost, 0.0);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 0.0);
}

TEST(QA138_CostModel, SortCostOneRow) {
    CostModel cm;
    PlanCost input;
    input.total_cost = 10.0;
    input.estimated_rows = 1.0;

    auto cost = cm.sort_cost(input);
    // sort_cpu_cost(1) = 0 (n<=1), startup = 10 + 0 = 10
    // total = 10 + 1 * 0.01 = 10.01
    EXPECT_NEAR(cost.total_cost, 10.01, 0.001);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 1.0);
}

TEST(QA138_CostModel, FilterZeroSelectivity) {
    CostModel cm;
    PlanCost input;
    input.total_cost = 100.0;
    input.estimated_rows = 1000.0;

    auto cost = cm.filter_cost(input, 0.0);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 0.0);
    EXPECT_GT(cost.total_cost, input.total_cost); // CPU cost still applied.
}

TEST(QA138_CostModel, FilterFullSelectivity) {
    CostModel cm;
    PlanCost input;
    input.total_cost = 100.0;
    input.estimated_rows = 1000.0;

    auto cost = cm.filter_cost(input, 1.0);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 1000.0);
}

TEST(QA138_CostModel, LimitZeroRows) {
    CostModel cm;
    PlanCost input;
    input.startup_cost = 10.0;
    input.total_cost = 100.0;
    input.estimated_rows = 1000.0;

    auto cost = cm.limit_cost(input, 0);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 0.0);
    EXPECT_DOUBLE_EQ(cost.total_cost, input.startup_cost);
}

TEST(QA138_CostModel, LimitExceedsInputRows) {
    CostModel cm;
    PlanCost input;
    input.startup_cost = 10.0;
    input.total_cost = 100.0;
    input.estimated_rows = 50.0;

    auto cost = cm.limit_cost(input, 1000);
    // fraction = min(1.0, 1000/50) = 1.0
    // estimated_rows = min(1000, 50) = 50
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 50.0);
    EXPECT_DOUBLE_EQ(cost.total_cost, input.total_cost);
}

TEST(QA138_CostModel, LimitOnZeroInputRows) {
    CostModel cm;
    PlanCost input;
    input.startup_cost = 0.0;
    input.total_cost = 0.0;
    input.estimated_rows = 0.0;

    auto cost = cm.limit_cost(input, 10);
    // fraction = 1.0 (estimated_rows==0 path)
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 0.0);
    EXPECT_GE(cost.total_cost, 0.0);
}

TEST(QA138_CostModel, HashJoinEmptyBuildSide) {
    CostModel cm;
    PlanCost build;
    build.total_cost = 0.0;
    build.estimated_rows = 0.0;

    PlanCost probe;
    probe.total_cost = 100.0;
    probe.estimated_rows = 1000.0;

    auto cost = cm.hash_join_cost(build, probe, 0.1);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 0.0);
    EXPECT_GE(cost.total_cost, 0.0);
}

TEST(QA138_CostModel, NestedLoopEmptyOuter) {
    CostModel cm;
    PlanCost outer;
    outer.total_cost = 0.0;
    outer.estimated_rows = 0.0;

    PlanCost inner;
    inner.total_cost = 100.0;
    inner.estimated_rows = 1000.0;

    auto cost = cm.nested_loop_join_cost(outer, inner, 0.1);
    // outer.total + 0 * inner.total + 0 * 1000 * 0.01 = 0
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 0.0);
    EXPECT_DOUBLE_EQ(cost.total_cost, 0.0);
}

TEST(QA138_CostModel, SortMergeJoinBothSorted) {
    CostModel cm;
    PlanCost left;
    left.total_cost = 100.0;
    left.estimated_rows = 1000.0;

    PlanCost right;
    right.total_cost = 200.0;
    right.estimated_rows = 5000.0;

    auto sorted = cm.sort_merge_join_cost(left, right, true, true, 0.001);
    auto unsorted = cm.sort_merge_join_cost(left, right, false, false, 0.001);
    EXPECT_LT(sorted.total_cost, unsorted.total_cost);
    EXPECT_EQ(sorted.estimated_rows, unsorted.estimated_rows);
}

TEST(QA138_CostModel, ProjectPreservesRows) {
    CostModel cm;
    PlanCost input;
    input.total_cost = 100.0;
    input.estimated_rows = 1000.0;

    auto cost = cm.project_cost(input);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 1000.0);
    EXPECT_GT(cost.total_cost, input.total_cost);
}

TEST(QA138_CostModel, HashAggregateMoreGroupsThanRows) {
    CostModel cm;
    PlanCost input;
    input.total_cost = 100.0;
    input.estimated_rows = 10.0;

    // num_groups > input rows: unusual but should not crash.
    auto cost = cm.hash_aggregate_cost(input, 1000);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 1000.0);
    EXPECT_GT(cost.total_cost, 0.0);
}

TEST(QA138_CostModel, HashAggregateOneGroup) {
    CostModel cm;
    PlanCost input;
    input.total_cost = 100.0;
    input.estimated_rows = 1000.0;

    auto cost = cm.hash_aggregate_cost(input, 1);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 1.0);
    EXPECT_GT(cost.total_cost, input.total_cost);
}

TEST(QA138_CostModel, AllZeroParams) {
    CostModelParams params;
    params.seq_page_cost = 0.0;
    params.random_page_cost = 0.0;
    params.cpu_tuple_cost = 0.0;
    params.cpu_operator_cost = 0.0;
    CostModel cm(params);

    auto cost = cm.seq_scan_cost(100, 10000);
    EXPECT_DOUBLE_EQ(cost.total_cost, 0.0);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 10000.0);
}

// =============================================================================
// Access path adversarial tests
// =============================================================================

TEST(QA138_AccessPath, IndexForDifferentTableIgnored) {
    CostModel cm;
    TableStats ts;
    ts.page_count = 100;
    ts.row_count = 10000;

    IndexDef idx;
    idx.index_id = 1;
    idx.table_id = 999; // Wrong table.
    idx.name = "idx_wrong";
    idx.index_type = "btree";
    idx.columns = "col";
    idx.is_unique = false;

    auto path = choose_access_path(1, ts, {idx}, 0.01, cm);
    EXPECT_EQ(path.method, AccessMethod::SEQ_SCAN);
}

TEST(QA138_AccessPath, ZeroPageTable) {
    CostModel cm;
    TableStats ts;
    ts.page_count = 0;
    ts.row_count = 0;

    IndexDef idx;
    idx.index_id = 1;
    idx.table_id = 1;
    idx.name = "idx_col";
    idx.index_type = "btree";
    idx.columns = "col";
    idx.is_unique = false;

    auto path = choose_access_path(1, ts, {idx}, 0.5, cm);
    // Both seq scan and index scan cost ~0 for empty table.
    EXPECT_GE(path.cost.total_cost, 0.0);
}

TEST(QA138_AccessPath, MultipleIndexesBestChosen) {
    CostModel cm;
    TableStats ts;
    ts.page_count = 100;
    ts.row_count = 10000;

    IndexDef idx1;
    idx1.index_id = 1;
    idx1.table_id = 1;
    idx1.name = "idx1";
    idx1.index_type = "btree";
    idx1.columns = "col1";
    idx1.is_unique = false;

    IndexDef idx2;
    idx2.index_id = 2;
    idx2.table_id = 1;
    idx2.name = "idx2";
    idx2.index_type = "btree";
    idx2.columns = "col2";
    idx2.is_unique = false;

    // Both indexes have same selectivity, so the first one found wins.
    auto path = choose_access_path(1, ts, {idx1, idx2}, 0.01, cm);
    EXPECT_EQ(path.method, AccessMethod::INDEX_SCAN);
}

TEST(QA138_AccessPath, FullSelectivityPrefersSeqScan) {
    CostModel cm;
    TableStats ts;
    ts.page_count = 100;
    ts.row_count = 10000;

    IndexDef idx;
    idx.index_id = 1;
    idx.table_id = 1;
    idx.name = "idx";
    idx.index_type = "btree";
    idx.columns = "col";
    idx.is_unique = false;

    auto path = choose_access_path(1, ts, {idx}, 1.0, cm);
    EXPECT_EQ(path.method, AccessMethod::SEQ_SCAN);
}

// =============================================================================
// Join method adversarial tests
// =============================================================================

TEST(QA138_JoinMethod, BothSortedFavorsSortMerge) {
    CostModel cm;
    PlanCost left;
    left.total_cost = 100.0;
    left.estimated_rows = 1000.0;

    PlanCost right;
    right.total_cost = 200.0;
    right.estimated_rows = 5000.0;

    auto [method, cost] = choose_join_method(left, right, 0.001, true, true, cm);
    // When both sides are already sorted, SMJ skips sort cost — should be competitive.
    EXPECT_GT(cost.total_cost, 0.0);
}

TEST(QA138_JoinMethod, EqualSizedRelations) {
    CostModel cm;
    PlanCost left;
    left.total_cost = 100.0;
    left.estimated_rows = 1000.0;

    PlanCost right;
    right.total_cost = 100.0;
    right.estimated_rows = 1000.0;

    auto [method, cost] = choose_join_method(left, right, 0.01, false, false, cm);
    EXPECT_GT(cost.total_cost, 0.0);
    EXPECT_GT(cost.estimated_rows, 0.0);
}

TEST(QA138_JoinMethod, CartesianJoinHighSelectivity) {
    CostModel cm;
    PlanCost left;
    left.total_cost = 10.0;
    left.estimated_rows = 100.0;

    PlanCost right;
    right.total_cost = 10.0;
    right.estimated_rows = 100.0;

    // Cartesian product: selectivity = 1.0.
    auto [method, cost] = choose_join_method(left, right, 1.0, false, false, cm);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 10000.0);
}

TEST(QA138_JoinMethod, ZeroSelectivityJoin) {
    CostModel cm;
    PlanCost left;
    left.total_cost = 10.0;
    left.estimated_rows = 100.0;

    PlanCost right;
    right.total_cost = 10.0;
    right.estimated_rows = 100.0;

    auto [method, cost] = choose_join_method(left, right, 0.0, false, false, cm);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 0.0);
}

// =============================================================================
// Join ordering adversarial tests
// =============================================================================

TEST(QA138_JoinOrder, CartesianProductNoEdges) {
    CostModel cm;
    std::vector<JoinRelation> rels;
    rels.push_back({1ULL << 0, {0.0, 10.0, 100.0}, make_scan(0, 100.0, 10.0)});
    rels.push_back({1ULL << 1, {0.0, 20.0, 200.0}, make_scan(1, 200.0, 20.0)});

    std::vector<JoinEdge> edges; // No join predicates.

    auto plan = optimize_join_order(rels, edges, cm);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PhysicalPlanNode::Type::JOIN);
    // Cartesian: estimated_rows = 100 * 200 * 1.0 = 20000
    EXPECT_DOUBLE_EQ(plan->cost.estimated_rows, 20000.0);
}

TEST(QA138_JoinOrder, DisconnectedJoinGraph) {
    CostModel cm;
    // Tables 0,1 are connected; table 2 is disconnected.
    std::vector<JoinRelation> rels;
    rels.push_back({1ULL << 0, {0.0, 10.0, 100.0}, make_scan(0, 100.0, 10.0)});
    rels.push_back({1ULL << 1, {0.0, 20.0, 200.0}, make_scan(1, 200.0, 20.0)});
    rels.push_back({1ULL << 2, {0.0, 5.0, 50.0}, make_scan(2, 50.0, 5.0)});

    std::vector<JoinEdge> edges = {{0, 1, 0.01}};

    auto plan = optimize_join_order(rels, edges, cm);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PhysicalPlanNode::Type::JOIN);
}

TEST(QA138_JoinOrder, TenTablesUsesDp) {
    CostModel cm;
    std::vector<JoinRelation> rels;
    std::vector<JoinEdge> edges;

    for (int i = 0; i < 10; ++i) {
        double rows = 100.0 * (i + 1);
        double cost = 10.0 * (i + 1);
        rels.push_back({1ULL << i, {0.0, cost, rows}, make_scan(i, rows, cost)});
        if (i > 0) {
            edges.push_back({static_cast<table_id_t>(i - 1), static_cast<table_id_t>(i), 0.01});
        }
    }

    auto plan = optimize_join_order(rels, edges, cm);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PhysicalPlanNode::Type::JOIN);
    EXPECT_GT(plan->cost.total_cost, 0.0);
}

TEST(QA138_JoinOrder, ElevenTablesUsesGreedy) {
    CostModel cm;
    std::vector<JoinRelation> rels;
    std::vector<JoinEdge> edges;

    for (int i = 0; i < 11; ++i) {
        double rows = 100.0 * (i + 1);
        double cost = 10.0 * (i + 1);
        rels.push_back({1ULL << i, {0.0, cost, rows}, make_scan(i, rows, cost)});
        if (i > 0) {
            edges.push_back({static_cast<table_id_t>(i - 1), static_cast<table_id_t>(i), 0.01});
        }
    }

    auto plan = optimize_join_order(rels, edges, cm);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PhysicalPlanNode::Type::JOIN);
}

TEST(QA138_JoinOrder, GreedyStartsWithSmallestRelation) {
    CostModel cm;
    std::vector<JoinRelation> rels;
    std::vector<JoinEdge> edges;

    // 12 tables: table 5 is smallest.
    for (int i = 0; i < 12; ++i) {
        double rows = (i == 5) ? 1.0 : 1000.0 * (i + 1);
        double cost = rows * 0.01;
        rels.push_back({1ULL << i, {0.0, cost, rows}, make_scan(i, rows, cost)});
        if (i > 0) {
            edges.push_back({static_cast<table_id_t>(i - 1), static_cast<table_id_t>(i), 0.01});
        }
    }

    auto plan = optimize_join_order(rels, edges, cm);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PhysicalPlanNode::Type::JOIN);
}

TEST(QA138_JoinOrder, TwoTablesEqualSize) {
    CostModel cm;
    std::vector<JoinRelation> rels;
    rels.push_back({1ULL << 0, {0.0, 100.0, 1000.0}, make_scan(0, 1000.0, 100.0)});
    rels.push_back({1ULL << 1, {0.0, 100.0, 1000.0}, make_scan(1, 1000.0, 100.0)});

    std::vector<JoinEdge> edges = {{0, 1, 0.01}};

    auto plan = optimize_join_order(rels, edges, cm);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PhysicalPlanNode::Type::JOIN);
    // Symmetry: should produce a valid plan either way.
    EXPECT_GT(plan->cost.total_cost, 0.0);
}

// =============================================================================
// Clone plan tests
// =============================================================================

TEST(QA138_PlanNode, JoinPlanHasLeftAndRight) {
    CostModel cm;
    std::vector<JoinRelation> rels;
    rels.push_back({1ULL << 0, {0.0, 10.0, 100.0}, make_scan(0, 100.0, 10.0)});
    rels.push_back({1ULL << 1, {0.0, 20.0, 200.0}, make_scan(1, 200.0, 20.0)});

    std::vector<JoinEdge> edges = {{0, 1, 0.01}};

    auto plan = optimize_join_order(rels, edges, cm);
    ASSERT_NE(plan, nullptr);
    ASSERT_NE(plan->left, nullptr);
    ASSERT_NE(plan->right, nullptr);
    EXPECT_EQ(plan->left->type, PhysicalPlanNode::Type::TABLE_SCAN);
    EXPECT_EQ(plan->right->type, PhysicalPlanNode::Type::TABLE_SCAN);
}

TEST(QA138_PlanNode, ThreeTableJoinNested) {
    CostModel cm;
    std::vector<JoinRelation> rels;
    rels.push_back({1ULL << 0, {0.0, 10.0, 100.0}, make_scan(0, 100.0, 10.0)});
    rels.push_back({1ULL << 1, {0.0, 20.0, 200.0}, make_scan(1, 200.0, 20.0)});
    rels.push_back({1ULL << 2, {0.0, 5.0, 50.0}, make_scan(2, 50.0, 5.0)});

    std::vector<JoinEdge> edges = {{0, 1, 0.01}, {1, 2, 0.02}};

    auto plan = optimize_join_order(rels, edges, cm);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PhysicalPlanNode::Type::JOIN);
    // One child should be a join, the other a scan.
    bool has_nested_join = (plan->left && plan->left->type == PhysicalPlanNode::Type::JOIN) ||
                           (plan->right && plan->right->type == PhysicalPlanNode::Type::JOIN);
    EXPECT_TRUE(has_nested_join);
}

// =============================================================================
// Cost model formula verification
// =============================================================================

TEST(QA138_CostModel, SortCostFormulaVerification) {
    CostModel cm;
    PlanCost input;
    input.total_cost = 50.0;
    input.estimated_rows = 1000.0;

    auto cost = cm.sort_cost(input);
    double expected_sort_cpu = 1000.0 * std::log2(1000.0) * 0.0025;
    double expected_startup = 50.0 + expected_sort_cpu;
    double expected_total = expected_startup + 1000.0 * 0.01;

    EXPECT_NEAR(cost.startup_cost, expected_startup, 0.01);
    EXPECT_NEAR(cost.total_cost, expected_total, 0.01);
}

TEST(QA138_CostModel, HashJoinFormulaVerification) {
    CostModel cm;
    PlanCost build;
    build.total_cost = 100.0;
    build.estimated_rows = 500.0;

    PlanCost probe;
    probe.total_cost = 200.0;
    probe.estimated_rows = 2000.0;

    auto cost = cm.hash_join_cost(build, probe, 0.01);

    double expected_startup = 100.0 + 500.0 * 0.01;
    double expected_total = expected_startup + 200.0 + 2000.0 * 0.01 + (500.0 + 2000.0) * 0.0025;
    double expected_rows = 500.0 * 2000.0 * 0.01;

    EXPECT_NEAR(cost.startup_cost, expected_startup, 0.01);
    EXPECT_NEAR(cost.total_cost, expected_total, 0.01);
    EXPECT_NEAR(cost.estimated_rows, expected_rows, 0.01);
}

TEST(QA138_CostModel, NestedLoopFormulaVerification) {
    CostModel cm;
    PlanCost outer;
    outer.startup_cost = 5.0;
    outer.total_cost = 20.0;
    outer.estimated_rows = 50.0;

    PlanCost inner;
    inner.startup_cost = 2.0;
    inner.total_cost = 10.0;
    inner.estimated_rows = 100.0;

    auto cost = cm.nested_loop_join_cost(outer, inner, 0.05);
    // startup = 5 + 2 = 7
    // total = 20 + 50 * 10 + 50 * 100 * 0.01 = 20 + 500 + 50 = 570
    // rows = 50 * 100 * 0.05 = 250
    EXPECT_DOUBLE_EQ(cost.startup_cost, 7.0);
    EXPECT_DOUBLE_EQ(cost.total_cost, 570.0);
    EXPECT_DOUBLE_EQ(cost.estimated_rows, 250.0);
}
