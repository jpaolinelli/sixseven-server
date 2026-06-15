// QA regression tests for GDB-809
//
// Ticket: Replace vacuous IndexScanFasterThanSeqScanForSelectiveQuery test with
// real plan-choice assertions on choose_access_path().
//
// This file:
//   1. Verifies the two new developer tests are non-vacuous (they would fail on
//      a planner regression) by reasoning about and probing flipped selectivity.
//   2. Adversarially probes choose_access_path() across the selectivity spectrum
//      to find the actual decision boundary and confirm the new tests' values
//      (0.0001, 0.9) are safely on the correct side.
//   3. Tests edge cases: empty table, no indexes, non-matching table_id,
//      multiple candidate indexes, selectivity exactly at 0.0 and 1.0.

#include "sixseven/catalog/schema.h"
#include "sixseven/planner/cost_model.h"
#include "sixseven/planner/optimizer.h"
#include "sixseven/planner/statistics.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helpers: build the same table/index setup as the developer tests
// ---------------------------------------------------------------------------

static TableStats make_large_table() {
    TableStats ts;
    ts.page_count = 200;
    ts.row_count = 10000;
    return ts;
}

static IndexDef make_btree_index(table_id_t table_id = 1, index_id_t id = 1) {
    IndexDef idx;
    idx.index_id = id;
    idx.table_id = table_id;
    idx.name = "idx_id";
    idx.index_type = "btree";
    idx.columns = "id";
    idx.is_unique = true;
    return idx;
}

// ---------------------------------------------------------------------------
// GDB809: Non-vacuity regression — flipping selectivity flips the decision
// ---------------------------------------------------------------------------

// If the dev test SelectivePredicateChoosesIndexScan (selectivity=0.0001) were
// replaced with a constant-return stub that always returns SEQ_SCAN, these
// tests would catch it.  The inverse predicate (selectivity=0.9) must flip to
// SEQ_SCAN, proving choose_access_path() is actually comparing costs.
TEST(QA_GDB809_PlanChoice, FlippedSelectivityFlipsDecision) {
    CostModel cm;
    TableStats ts = make_large_table();
    std::vector<IndexDef> idxs = {make_btree_index()};

    auto selective = choose_access_path(1, ts, idxs, 0.0001, cm);
    auto broad = choose_access_path(1, ts, idxs, 0.9, cm);

    // These two must differ — if both return the same method the cost model is broken.
    EXPECT_NE(selective.method, broad.method)
        << "choose_access_path returned the same method for selectivity=0.0001 and 0.9 "
           "— the cost model comparison is likely broken";
    EXPECT_EQ(selective.method, AccessMethod::INDEX_SCAN);
    EXPECT_EQ(broad.method, AccessMethod::SEQ_SCAN);
}

// ---------------------------------------------------------------------------
// GDB809: Selectivity spectrum — locate decision boundary
//
// With the default CostModel and table (200 pages, 10 000 rows):
//   seq_scan_cost  = 200*1.0 + 10000*0.01 = 300
//   index_pages    = max(1, 200/100) = 2
//   index_scan_cost = s*200*4.0 + s*10000*0.01 + 2*4.0
//                   = 800s + 100s + 8 = 900s + 8
// Break-even: 900s + 8 = 300  =>  s = 292/900 ≈ 0.3244
// ---------------------------------------------------------------------------

TEST(QA_GDB809_PlanChoice, PointLookupSelectivity_IndexScan) {
    // 1 matching row / 10 000 = selectivity 0.0001
    CostModel cm;
    TableStats ts = make_large_table();
    auto path = choose_access_path(1, ts, {make_btree_index()}, 0.0001, cm);
    EXPECT_EQ(path.method, AccessMethod::INDEX_SCAN)
        << "Point-lookup selectivity (0.0001) must choose INDEX_SCAN";
    EXPECT_EQ(path.index_id, 1u);
}

TEST(QA_GDB809_PlanChoice, ModeratelySelective_IndexScan) {
    // selectivity=0.10 is well below the ~0.32 break-even → INDEX_SCAN
    CostModel cm;
    TableStats ts = make_large_table();
    auto path = choose_access_path(1, ts, {make_btree_index()}, 0.10, cm);
    EXPECT_EQ(path.method, AccessMethod::INDEX_SCAN)
        << "Selectivity 0.10 is below break-even (~0.32) — expected INDEX_SCAN";
}

TEST(QA_GDB809_PlanChoice, JustBelowBreakEven_IndexScan) {
    // selectivity=0.30 is just below the ~0.3244 break-even → should still be INDEX_SCAN
    CostModel cm;
    TableStats ts = make_large_table();
    auto path = choose_access_path(1, ts, {make_btree_index()}, 0.30, cm);
    // index_cost = 900*0.30 + 8 = 270+8 = 278 < 300  → INDEX_SCAN
    EXPECT_EQ(path.method, AccessMethod::INDEX_SCAN)
        << "Selectivity 0.30 (index_cost≈278 vs seq_cost≈300) should choose INDEX_SCAN";
}

TEST(QA_GDB809_PlanChoice, JustAboveBreakEven_SeqScan) {
    // selectivity=0.35 is just above the ~0.3244 break-even → SEQ_SCAN
    CostModel cm;
    TableStats ts = make_large_table();
    auto path = choose_access_path(1, ts, {make_btree_index()}, 0.35, cm);
    // index_cost = 900*0.35 + 8 = 315+8 = 323 > 300  → SEQ_SCAN
    EXPECT_EQ(path.method, AccessMethod::SEQ_SCAN)
        << "Selectivity 0.35 (index_cost≈323 vs seq_cost≈300) should choose SEQ_SCAN";
}

TEST(QA_GDB809_PlanChoice, BroadSelectivity_SeqScan) {
    // selectivity=0.9 — developer test value
    CostModel cm;
    TableStats ts = make_large_table();
    auto path = choose_access_path(1, ts, {make_btree_index()}, 0.9, cm);
    EXPECT_EQ(path.method, AccessMethod::SEQ_SCAN)
        << "Selectivity 0.9 must choose SEQ_SCAN (index overhead too high)";
}

TEST(QA_GDB809_PlanChoice, FullTableSelectivity1_SeqScan) {
    // selectivity=1.0 — full table scan
    CostModel cm;
    TableStats ts = make_large_table();
    auto path = choose_access_path(1, ts, {make_btree_index()}, 1.0, cm);
    EXPECT_EQ(path.method, AccessMethod::SEQ_SCAN)
        << "Selectivity 1.0 (full table) must choose SEQ_SCAN";
}

TEST(QA_GDB809_PlanChoice, ZeroSelectivity_IndexScan) {
    // selectivity=0.0 — no rows match; index_cost = 0+0+8 = 8 < 300 → INDEX_SCAN
    CostModel cm;
    TableStats ts = make_large_table();
    auto path = choose_access_path(1, ts, {make_btree_index()}, 0.0, cm);
    EXPECT_EQ(path.method, AccessMethod::INDEX_SCAN)
        << "Selectivity 0.0 (index_cost=8 vs seq=300) should choose INDEX_SCAN";
}

// ---------------------------------------------------------------------------
// GDB809: Edge cases
// ---------------------------------------------------------------------------

TEST(QA_GDB809_EdgeCases, NoIndexes_AlwaysSeqScan) {
    // Without any indexes, choose_access_path must fall back to SEQ_SCAN
    // regardless of how selective the predicate is.
    CostModel cm;
    TableStats ts = make_large_table();
    std::vector<IndexDef> empty_indexes;

    auto path = choose_access_path(1, ts, empty_indexes, 0.0001, cm);
    EXPECT_EQ(path.method, AccessMethod::SEQ_SCAN)
        << "No indexes available — must choose SEQ_SCAN";
}

TEST(QA_GDB809_EdgeCases, IndexOnDifferentTable_SeqScan) {
    // An index that belongs to table_id=99 must NOT be used for table_id=1.
    // This is the non-indexed column case: the index exists but is on the wrong table.
    CostModel cm;
    TableStats ts = make_large_table();
    IndexDef wrong_table_idx = make_btree_index(/*table_id=*/99, /*id=*/5);
    std::vector<IndexDef> idxs = {wrong_table_idx};

    auto path = choose_access_path(1, ts, idxs, 0.0001, cm);
    EXPECT_EQ(path.method, AccessMethod::SEQ_SCAN)
        << "Index on table_id=99 must not be used for table_id=1 (wrong-table guard)";
}

TEST(QA_GDB809_EdgeCases, EmptyTable_IndexScan) {
    // Edge: empty table (0 pages, 0 rows).
    // seq_scan_cost = 0; index_scan_cost = 0 + 0 + max(1,0)*4 = 4.
    // SEQ_SCAN is cheaper (0 < 4) — result must be SEQ_SCAN.
    CostModel cm;
    TableStats ts;
    ts.page_count = 0;
    ts.row_count = 0;

    auto path = choose_access_path(1, ts, {make_btree_index()}, 0.0001, cm);
    // With 0 pages: seq_cost = 0*1.0 + 0*0.01 = 0; index_cost = 0 + 0 + max(1,0)*4 = 4.
    // SEQ_SCAN is cheaper.
    EXPECT_EQ(path.method, AccessMethod::SEQ_SCAN)
        << "Empty table: seq_scan_cost=0 must be cheaper than index_scan overhead";
}

TEST(QA_GDB809_EdgeCases, MultipleCandidateIndexes_BestChosen) {
    // Two indexes on the same table — the optimizer must pick the better one.
    // Both are identical here so the first match wins; we just verify no crash
    // and that INDEX_SCAN is chosen for a selective predicate.
    CostModel cm;
    TableStats ts = make_large_table();
    std::vector<IndexDef> idxs = {
        make_btree_index(1, 10),
        make_btree_index(1, 20),
    };

    auto path = choose_access_path(1, ts, idxs, 0.0001, cm);
    EXPECT_EQ(path.method, AccessMethod::INDEX_SCAN)
        << "With multiple indexes, should still choose INDEX_SCAN for selective predicate";
    // index_id should be one of the two candidates
    EXPECT_TRUE(path.index_id == 10 || path.index_id == 20)
        << "Chosen index_id should come from a candidate index";
}

TEST(QA_GDB809_EdgeCases, SinglePageTable_IndexOverhead) {
    // Very small table (1 page). SEQ_SCAN is almost always cheaper.
    // seq_cost = 1*1.0 + 100*0.01 = 1 + 1 = 2
    // index_cost = s*1*4.0 + s*100*0.01 + max(1,0)*4 = 4.01s + 4
    // Break-even: 4.01s + 4 = 2 → negative s: index is never cheaper.
    CostModel cm;
    TableStats ts;
    ts.page_count = 1;
    ts.row_count = 100;
    std::vector<IndexDef> idxs = {make_btree_index()};

    auto path = choose_access_path(1, ts, idxs, 0.0001, cm);
    EXPECT_EQ(path.method, AccessMethod::SEQ_SCAN)
        << "Single-page table: SEQ_SCAN (cost=2) should beat INDEX_SCAN (cost=4) even for "
           "selectivity=0.0001";
}

TEST(QA_GDB809_EdgeCases, DeveloperTestValues_SafelyOnCorrectSide) {
    // Verify that the values chosen in the dev tests (0.0001 and 0.9) are NOT
    // razor-close to the decision boundary (~0.3244 with default params).
    // We compute the costs explicitly and check the margin is substantial.
    CostModel cm;
    const auto& p = cm.params();
    TableStats ts = make_large_table();

    const double seq_cost = ts.page_count * p.seq_page_cost + ts.row_count * p.cpu_tuple_cost;
    const uint32_t index_pages = std::max(1u, ts.page_count / 100u);

    auto index_cost_at = [&](double s) {
        return s * ts.page_count * p.random_page_cost + s * ts.row_count * p.cpu_tuple_cost +
               index_pages * p.random_page_cost;
    };

    // selectivity=0.0001: index must be significantly cheaper than seq
    const double idx_cost_selective = index_cost_at(0.0001);
    EXPECT_LT(idx_cost_selective, seq_cost * 0.10)
        << "selectivity=0.0001: index_cost should be <10% of seq_cost (well within INDEX_SCAN "
           "territory)";

    // selectivity=0.9: seq must be significantly cheaper than index
    const double idx_cost_broad = index_cost_at(0.9);
    EXPECT_GT(idx_cost_broad, seq_cost * 2.0)
        << "selectivity=0.9: index_cost should be >2x seq_cost (well within SEQ_SCAN territory)";
}
