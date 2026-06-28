#include "sixseven/planner/cost_model.h"
#include "sixseven/planner/optimizer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

using namespace sixseven;

// =============================================================================
// Helpers
// =============================================================================

static std::unique_ptr<PhysicalPlanNode> make_test_scan(table_id_t id, double rows, double cost) {
    auto node = std::make_unique<PhysicalPlanNode>();
    node->type = PhysicalPlanNode::Type::TABLE_SCAN;
    node->table_id = id;
    node->cost.estimated_rows = rows;
    node->cost.total_cost = cost;
    return node;
}

// Recursively walk a join plan, collecting the table_id of every TABLE_SCAN
// leaf and counting the JOIN nodes, while validating that every JOIN node has
// two children and every leaf is a TABLE_SCAN. Lets the join-order tests assert
// the actual tree shape (no dropped, duplicated, or malformed nodes) instead of
// only checking the root node type.
static void collect_join_tree(const PhysicalPlanNode* node,
                              std::vector<table_id_t>& leaf_ids,
                              int& join_count) {
    ASSERT_NE(node, nullptr);
    if (node->type == PhysicalPlanNode::Type::JOIN) {
        ++join_count;
        ASSERT_NE(node->left, nullptr);
        ASSERT_NE(node->right, nullptr);
        collect_join_tree(node->left.get(), leaf_ids, join_count);
        collect_join_tree(node->right.get(), leaf_ids, join_count);
    } else {
        EXPECT_EQ(node->type, PhysicalPlanNode::Type::TABLE_SCAN);
        leaf_ids.push_back(node->table_id);
    }
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

TEST(AccessPathTest, IndexForDifferentTableIdIgnored) {
    CostModel cm;
    TableStats ts;
    ts.page_count = 100;
    ts.row_count = 10000;

    IndexDef idx;
    idx.index_id = 99;
    idx.table_id = 42; // different table
    idx.name = "idx_other";
    idx.index_type = "btree";
    idx.columns = "col";
    idx.is_unique = false;

    // Even with low selectivity the index is for a different table, so seq scan wins.
    auto path = choose_access_path(1, ts, {idx}, 0.001, cm);
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
    // NL  = 10 + 100*200 + 100*10000*0.01 = 30010
    // HJ  = (10 + 100*0.01) + 200 + 10000*0.01 + (100+10000)*0.0025 = 336.25
    // SMJ = 10 + 200 + sort(100) + sort(10000) + (100+10000)*0.01 = 644.85
    // Hash join wins by a wide margin for a large inner relation.
    EXPECT_EQ(method, JoinMethod::HASH_JOIN);
    EXPECT_GT(cost.total_cost, 0.0);
}

// When the outer side has exactly 1 row, nested loop pays inner.total only once
// and skips the per-row hash-table or sort overhead that HJ and SMJ must pay.
// Proof (cpu_tuple_cost=0.01, cpu_operator_cost=0.0025):
//   NL  = o_t + 1*i_t + 1*i_r*0.01
//   HJ  = (o_t + 1*0.01) + i_t + i_r*0.01 + (1+i_r)*0.0025
//         NL < HJ iff 0 < 0.01 + (1+i_r)*0.0025 -- always true.
//   SMJ = o_t + i_t + sort(1) + sort(i_r) + (1+i_r)*0.01; sort(1)=0 by formula.
//         NL < SMJ iff 0 < sort(i_r) + 1*0.01 -- always true for i_r >= 1.
TEST(JoinMethodTest, NestedLoopForSingleOuterRow) {
    CostModel cm;
    PlanCost outer;
    outer.total_cost = 5.0;
    outer.estimated_rows = 1.0;

    PlanCost inner;
    inner.total_cost = 3.0;
    inner.estimated_rows = 3.0;

    // NL  = 5 + 1*3 + 1*3*0.01  = 8.03
    // HJ  = (5+0.01) + 3 + 3*0.01 + (1+3)*0.0025 = 8.05   (build=outer, 1 row)
    // SMJ = 5 + 3 + 0 + sort(3) + (1+3)*0.01 = 8.05 + sort(3)
    // Nested loop wins: the inner is scanned exactly once; hash-build and sort
    // overhead are not amortised over multiple outer rows.
    auto [method, cost] = choose_join_method(outer, inner, 0.5, false, false, cm);
    EXPECT_EQ(method, JoinMethod::NESTED_LOOP);
    EXPECT_GT(cost.total_cost, 0.0);
}

TEST(JoinMethodTest, OutputRowsReflectsSelectivity) {
    CostModel cm;
    PlanCost left;
    left.total_cost = 10.0;
    left.estimated_rows = 1000.0;

    PlanCost right;
    right.total_cost = 10.0;
    right.estimated_rows = 1000.0;

    double sel = 0.001;
    auto [method, cost] = choose_join_method(left, right, sel, false, false, cm);
    // NL  = 10 + 1000*10 + 1000*1000*0.01 = 20010
    // HJ  = (10 + 1000*0.01) + 10 + 1000*0.01 + (1000+1000)*0.0025 = 45
    // SMJ = 10 + 10 + sort(1000) + sort(1000) + (1000+1000)*0.01 = 89.83
    // Hash join wins; output cardinality must reflect the join selectivity.
    EXPECT_EQ(method, JoinMethod::HASH_JOIN);
    EXPECT_NEAR(cost.estimated_rows, 1000.0 * 1000.0 * sel, 1.0);
}

// =============================================================================
// Join ordering tests
// =============================================================================

TEST(JoinOrderTest, TwoTableJoin) {
    CostModel cm;

    const PlanCost cost0{0.0, 10.0, 100.0};
    const PlanCost cost1{0.0, 20.0, 1000.0};

    std::vector<JoinRelation> rels;
    rels.push_back({1ULL << 0, cost0, make_test_scan(0, 100.0, 10.0)});
    rels.push_back({1ULL << 1, cost1, make_test_scan(1, 1000.0, 20.0)});

    std::vector<JoinEdge> edges = {{0, 1, 0.01}};

    auto plan = optimize_join_order(rels, edges, cm);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PhysicalPlanNode::Type::JOIN);

    // With exactly two relations the DP has a single legal split, so the
    // children are fully determined: the lower dense bit (table 0) is the left
    // child and the higher (table 1) the right. The old test asserted only the
    // root type, so a reversed or dropped child would have passed.
    ASSERT_NE(plan->left, nullptr);
    ASSERT_NE(plan->right, nullptr);
    EXPECT_EQ(plan->left->type, PhysicalPlanNode::Type::TABLE_SCAN);
    EXPECT_EQ(plan->right->type, PhysicalPlanNode::Type::TABLE_SCAN);
    EXPECT_EQ(plan->left->table_id, static_cast<table_id_t>(0));
    EXPECT_EQ(plan->right->table_id, static_cast<table_id_t>(1));

    // The single enumerated alternative is the source of truth for the chosen
    // join method and root cost, so a wrong method or a mis-costed root is now
    // caught instead of silently passing.
    auto [expected_method, expected_cost] =
        choose_join_method(cost0, cost1, 0.01, false, false, cm);
    EXPECT_EQ(plan->join_method, expected_method);
    EXPECT_DOUBLE_EQ(plan->cost.total_cost, expected_cost.total_cost);
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

    // A correct 3-way plan is a binary tree with two JOIN nodes and three
    // TABLE_SCAN leaves covering exactly tables {0,1,2}. The old test checked
    // only the root type, so a plan that dropped or duplicated a table would
    // have passed.
    std::vector<table_id_t> leaf_ids;
    int join_count = 0;
    collect_join_tree(plan.get(), leaf_ids, join_count);
    EXPECT_EQ(join_count, 2);
    std::sort(leaf_ids.begin(), leaf_ids.end());
    EXPECT_EQ(leaf_ids,
              (std::vector<table_id_t>{static_cast<table_id_t>(0),
                                       static_cast<table_id_t>(1),
                                       static_cast<table_id_t>(2)}));
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

    // A correct 4-way DP plan is a binary tree with three JOIN nodes and four
    // TABLE_SCAN leaves covering exactly tables {0,1,2,3}. Without this the DP
    // could drop, duplicate, or malform a node and the test would still pass.
    std::vector<table_id_t> leaf_ids;
    int join_count = 0;
    collect_join_tree(plan.get(), leaf_ids, join_count);
    EXPECT_EQ(join_count, 3);
    std::sort(leaf_ids.begin(), leaf_ids.end());
    EXPECT_EQ(leaf_ids,
              (std::vector<table_id_t>{static_cast<table_id_t>(0),
                                       static_cast<table_id_t>(1),
                                       static_cast<table_id_t>(2),
                                       static_cast<table_id_t>(3)}));
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
    // Verify that a hash join prefers the smaller relation as its build side.
    // The physical plan node does not record which child is the build side
    // (build-side selection lives entirely in the cost model), so the
    // preference is asserted directly against the cost model: hashing the
    // smaller relation has a strictly lower startup cost, and
    // choose_join_method normalizes to that cheaper orientation regardless of
    // the order its arguments are passed in.
    CostModel cm;

    const PlanCost small{0.0, 10.0, 100.0};     // 100-row relation
    const PlanCost large{0.0, 200.0, 100000.0}; // 100000-row relation
    const double sel = 0.0001;

    const PlanCost build_small = cm.hash_join_cost(small, large, sel);
    const PlanCost build_large = cm.hash_join_cost(large, small, sel);
    // Building the smaller side is cheaper to start (lower startup cost) and
    // never more expensive overall.
    EXPECT_LT(build_small.startup_cost, build_large.startup_cost);
    EXPECT_LE(build_small.total_cost, build_large.total_cost);

    // choose_join_method must pick the smaller-build orientation no matter which
    // way the two relations are handed to it: the reported cost is identical and
    // matches the smaller-build hash cost (its startup cost is the cheaper one).
    auto [method_a, cost_a] = choose_join_method(small, large, sel, false, false, cm);
    auto [method_b, cost_b] = choose_join_method(large, small, sel, false, false, cm);
    EXPECT_EQ(method_a, method_b);
    EXPECT_DOUBLE_EQ(cost_a.total_cost, cost_b.total_cost);
    if (method_a == JoinMethod::HASH_JOIN) {
        EXPECT_DOUBLE_EQ(cost_a.startup_cost, build_small.startup_cost);
    }

    // The optimizer still produces a join over exactly the two input tables.
    std::vector<JoinRelation> rels;
    rels.push_back({1ULL << 0, small, make_test_scan(0, 100.0, 10.0)});
    rels.push_back({1ULL << 1, large, make_test_scan(1, 100000.0, 200.0)});

    std::vector<JoinEdge> edges = {{0, 1, sel}};

    auto plan = optimize_join_order(rels, edges, cm);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PhysicalPlanNode::Type::JOIN);
    EXPECT_GT(plan->cost.total_cost, 0.0);

    std::vector<table_id_t> leaf_ids;
    int join_count = 0;
    collect_join_tree(plan.get(), leaf_ids, join_count);
    EXPECT_EQ(join_count, 1);
    std::sort(leaf_ids.begin(), leaf_ids.end());
    EXPECT_EQ(leaf_ids,
              (std::vector<table_id_t>{static_cast<table_id_t>(0), static_cast<table_id_t>(1)}));
}

// =============================================================================
// GDB-817: DP/greedy threshold strategy assertions
// =============================================================================

/// Build a chain of N relations with uniform edge selectivity and return the
/// chosen JoinEnumStrategy alongside the plan.
static std::pair<std::unique_ptr<PhysicalPlanNode>, JoinEnumStrategy> build_chain_plan(int n) {
    CostModel cm;
    std::vector<JoinRelation> rels;
    std::vector<JoinEdge> edges;
    for (int i = 0; i < n; ++i) {
        double rows = 100.0 * (i + 1);
        double cost = 10.0 * (i + 1);
        rels.push_back({1ULL << i, {0.0, cost, rows}, make_test_scan(i, rows, cost)});
        if (i > 0) {
            edges.push_back({static_cast<table_id_t>(i - 1), static_cast<table_id_t>(i), 0.01});
        }
    }
    JoinEnumStrategy strategy = JoinEnumStrategy::DYNAMIC_PROGRAMMING;
    auto plan = optimize_join_order(rels, edges, cm, strategy);
    return {std::move(plan), strategy};
}

// Threshold - 1 (9 tables) must use DP.
TEST(JoinOrderStrategyTest, NineTablesUsesDP) {
    auto [plan, strategy] = build_chain_plan(9);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PhysicalPlanNode::Type::JOIN);
    EXPECT_EQ(strategy, JoinEnumStrategy::DYNAMIC_PROGRAMMING)
        << "9 relations (< threshold=" << JOIN_ENUM_DP_THRESHOLD << ") must use DP";
}

// Exactly at threshold (10 tables) must use DP.
TEST(JoinOrderStrategyTest, TenTablesUsesDP) {
    auto [plan, strategy] = build_chain_plan(10);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PhysicalPlanNode::Type::JOIN);
    EXPECT_EQ(strategy, JoinEnumStrategy::DYNAMIC_PROGRAMMING)
        << "10 relations (== threshold=" << JOIN_ENUM_DP_THRESHOLD << ") must use DP";
}

// Threshold + 1 (11 tables) must use greedy.
TEST(JoinOrderStrategyTest, ElevenTablesUsesGreedy) {
    auto [plan, strategy] = build_chain_plan(11);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PhysicalPlanNode::Type::JOIN);
    EXPECT_EQ(strategy, JoinEnumStrategy::GREEDY)
        << "11 relations (> threshold=" << JOIN_ENUM_DP_THRESHOLD << ") must use greedy";
}

// Well above threshold (20 tables) must use greedy.
TEST(JoinOrderStrategyTest, TwentyTablesUsesGreedy) {
    auto [plan, strategy] = build_chain_plan(20);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PhysicalPlanNode::Type::JOIN);
    EXPECT_EQ(strategy, JoinEnumStrategy::GREEDY)
        << "20 relations (> threshold=" << JOIN_ENUM_DP_THRESHOLD << ") must use greedy";
}

// Single table always returns without running any enumeration; strategy should
// still be reported as DP (the branch taken in optimize_join_order is the DP
// branch, which then delegates to dp_join_order whose n==1 fast-path fires).
TEST(JoinOrderStrategyTest, SingleTableReportsDPStrategy) {
    CostModel cm;
    std::vector<JoinRelation> rels;
    rels.push_back({1ULL << 0, {0.0, 10.0, 100.0}, make_test_scan(0, 100.0, 10.0)});
    std::vector<JoinEdge> edges;
    JoinEnumStrategy strategy = JoinEnumStrategy::GREEDY; // pre-set to wrong value
    auto plan = optimize_join_order(rels, edges, cm, strategy);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(strategy, JoinEnumStrategy::DYNAMIC_PROGRAMMING);
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
