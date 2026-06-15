// GDB-803 QA adversarial tests — Make SortMergeJoin, BitmapScan, and
// ExternalSort reachable.
//
// Priority investigation (reviewer flag):
//   SMJ reachability: planner calls choose_join_method with left_sorted=false,
//   right_sorted=false unconditionally. Even after ANALYZE the sort cost
//   penalty may prevent SMJ from ever winning. Verify end-to-end via EXPLAIN
//   and file a bug if SMJ cannot be reached through the planner via SQL.
//
// Also tests:
//   BitmapScan over-filtering with LESS/GREATER OR arms (begin_key set, no
//   end_key — may scan entire index from a point).
//   BitmapScan typed columns: UUID and TIMESTAMP literals need coercion.
//   ExternalSort correctness on large inputs (1000+ rows).
//   ExternalSort does not regress small sort correctness.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/index_manager.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/planner/cost_model.h"
#include "sixseven/planner/optimizer.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../unit/test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// Fixture
// =============================================================================

class QA_GDB803 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ =
            std::filesystem::temp_directory_path() / "sixseven_qa_gdb803";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_  = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        index_manager_.reset();
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value())
            << "[SQL] " << sql << "\n[ERR] "
            << (result ? "" : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    bool exec_succeeds(const std::string& sql) {
        auto result = engine_->execute(sql);
        return result.has_value();
    }

    std::string explain(const std::string& sql) {
        auto qr = exec_ok("EXPLAIN " + sql);
        std::string text;
        for (const auto& row : qr.rows) {
            text += row[0].as_string();
            text += "\n";
        }
        return text;
    }

    void rebuild_indexes() {
        index_manager_ = std::make_unique<IndexManager>(catalog_, *storage_);
        auto r         = index_manager_->rebuild_all_indexes();
        ASSERT_TRUE(r.has_value()) << r.error().message;
        engine_->set_index_manager(index_manager_.get());
    }

    DiskManager                  dm_;
    Catalog                      catalog_;
    std::filesystem::path        data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine>    engine_;
    std::unique_ptr<IndexManager>   index_manager_;
};

// =============================================================================
// SMJ Reachability — Priority Investigation
// =============================================================================

// The planner wires SMJ when choose_join_method returns SORT_MERGE.
// However, choose_join_method is called with left_sorted=false and
// right_sorted=false unconditionally in the planner (line ~1672 of planner.cpp).
// This test verifies whether, after ANALYZE (which populates cost estimates),
// the cost model CAN in fact return SORT_MERGE and the planner CAN in fact
// select SortMergeJoinOperator.
//
// Approach: use a very large row count so the sort overhead in SMJ is
// amortized. After ANALYZE, cost estimates are present, activating the
// cost-based path. EXPLAIN output must contain "Sort Merge Join".
//
// If this test fails: the planner never reaches SMJ via SQL — Critical defect.
TEST_F(QA_GDB803, SMJ_ReachableViaSQL_AfterANALYZE) {
    exec_ok("CREATE TABLE smj_left (id INT, val INT)");
    exec_ok("CREATE TABLE smj_right (id INT, score INT)");

    // Insert enough rows that SMJ may be cost-competitive even with sort cost.
    // 500 rows each — small but enough to give cost estimates non-trivial weight.
    for (int i = 1; i <= 500; ++i) {
        exec_ok("INSERT INTO smj_left VALUES (" + std::to_string(i) + ", " +
                std::to_string(i * 2) + ")");
        exec_ok("INSERT INTO smj_right VALUES (" + std::to_string(i) + ", " +
                std::to_string(i * 3) + ")");
    }

    // Run ANALYZE to populate table statistics.
    exec_ok("ANALYZE smj_left");
    exec_ok("ANALYZE smj_right");

    // Issue join and check EXPLAIN output.
    const std::string join_sql =
        "SELECT smj_left.id FROM smj_left "
        "JOIN smj_right ON smj_left.id = smj_right.id";

    auto plan = explain(join_sql);

    // This test documents whether SMJ is actually selected.
    // If "Sort Merge Join" does NOT appear, SMJ is unreachable end-to-end.
    bool smj_selected = plan.find("Sort Merge Join") != std::string::npos ||
                        plan.find("SortMerge") != std::string::npos;

    // EXPECT (not ASSERT) so we still run correctness checks below.
    EXPECT_TRUE(smj_selected)
        << "CRITICAL: SortMergeJoin was NOT selected even after ANALYZE.\n"
        << "Planner passes left_sorted=false, right_sorted=false to "
           "choose_join_method unconditionally, preventing SMJ from winning "
           "the cost competition.\nEXPLAIN output:\n"
        << plan;
}

// Adversarial: verify the planner produces correct join results regardless of
// which join method is chosen. SMJ must not silently drop rows.
TEST_F(QA_GDB803, SMJ_JoinCorrectnessWithANALYZE_AllRowsPresent) {
    exec_ok("CREATE TABLE left_t (id INT, tag VARCHAR)");
    exec_ok("CREATE TABLE right_t (id INT, dept VARCHAR)");

    for (int i = 1; i <= 100; ++i) {
        exec_ok("INSERT INTO left_t VALUES (" + std::to_string(i) + ", 'L" +
                std::to_string(i) + "')");
        exec_ok("INSERT INTO right_t VALUES (" + std::to_string(i) + ", 'D" +
                std::to_string(i % 5) + "')");
    }

    exec_ok("ANALYZE left_t");
    exec_ok("ANALYZE right_t");

    auto qr = exec_ok(
        "SELECT left_t.id FROM left_t "
        "JOIN right_t ON left_t.id = right_t.id "
        "ORDER BY left_t.id");

    // All 100 rows must be present — no silent row loss from SMJ merge.
    ASSERT_EQ(qr.rows.size(), 100u)
        << "Join result has wrong number of rows — possible SMJ merge bug";
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(qr.rows[i][0].as_int32(), i + 1)
            << "Row " << i << " has wrong id value";
    }
}

// Adversarial: ANALYZE on empty tables. Cost estimates from empty tables
// should default gracefully and not trigger SMJ or crash.
TEST_F(QA_GDB803, SMJ_EmptyTableAfterANALYZE_NoCrash) {
    exec_ok("CREATE TABLE empty_l (id INT)");
    exec_ok("CREATE TABLE empty_r (id INT)");
    exec_ok("ANALYZE empty_l");
    exec_ok("ANALYZE empty_r");

    auto qr = exec_ok(
        "SELECT empty_l.id FROM empty_l "
        "JOIN empty_r ON empty_l.id = empty_r.id");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// BitmapScan over-filtering with range (LESS/GREATER) OR arms
// =============================================================================

// The BitmapScan code sets only begin_key for LESS predicates but begin_key
// is the LOWER bound of range_scan. For `price < 30`, the begin_key should
// be nullopt (scan from start) with end_key = 30. Setting begin_key = 30
// and leaving end_key = nullopt scans from 30 to infinity — the OPPOSITE
// of what is intended.
//
// This test catches that over-filtering (or under-filtering) defect.
TEST_F(QA_GDB803, BitmapScan_LESS_OR_Arm_ReturnsCorrectRows) {
    exec_ok("CREATE TABLE products2 (id INT, price INT, name VARCHAR)");
    exec_ok("INSERT INTO products2 VALUES "
            "(1, 10, 'cheap'), (2, 50, 'mid'), (3, 100, 'exp'), "
            "(4, 5, 'cheap2'), (5, 200, 'luxury')");
    exec_ok("CREATE INDEX idx_p2_id ON products2(id)");
    exec_ok("CREATE INDEX idx_p2_price ON products2(price)");
    rebuild_indexes();

    // Query: id = 3 OR price < 20
    // Expected: id=3 (price=100) PLUS rows with price < 20: id=1(price=10), id=4(price=5)
    // Total = 3 rows: ids 1, 3, 4.
    auto qr = exec_ok(
        "SELECT id FROM products2 WHERE id = 3 OR price < 20 ORDER BY id");

    ASSERT_EQ(qr.rows.size(), 3u)
        << "LESS OR arm: expected 3 rows (id=1,3,4) but got " << qr.rows.size()
        << ". BitmapScan may set begin_key=20 instead of end_key=20 for LESS.";
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 3);
    EXPECT_EQ(qr.rows[2][0].as_int32(), 4);
}

// GREATER OR arm: id > 3 OR price = 10
// Expected: price=10 is id=1; id>3 includes id=4,5. Union = ids 1,4,5 = 3 rows.
TEST_F(QA_GDB803, BitmapScan_GREATER_OR_Arm_ReturnsCorrectRows) {
    exec_ok("CREATE TABLE products3 (id INT, price INT)");
    exec_ok("INSERT INTO products3 VALUES "
            "(1, 10), (2, 50), (3, 100), (4, 5), (5, 200)");
    exec_ok("CREATE INDEX idx_p3_id ON products3(id)");
    exec_ok("CREATE INDEX idx_p3_price ON products3(price)");
    rebuild_indexes();

    auto qr = exec_ok(
        "SELECT id FROM products3 WHERE id > 3 OR price = 10 ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u)
        << "GREATER OR arm: expected 3 rows (id=1,4,5) but got "
        << qr.rows.size()
        << ". BitmapScan may incorrectly set begin_key for GREATER predicate.";
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 4);
    EXPECT_EQ(qr.rows[2][0].as_int32(), 5);
}

// LESS_EQUAL OR arm: id <= 2 OR price > 100
// Expected: id<=2 gives rows 1,2; price>100 gives row 5 (price=200).
// Union = 3 rows.
TEST_F(QA_GDB803, BitmapScan_LESS_EQUAL_OR_Arm_ReturnsCorrectRows) {
    exec_ok("CREATE TABLE products4 (id INT, price INT)");
    exec_ok("INSERT INTO products4 VALUES "
            "(1, 10), (2, 50), (3, 100), (4, 5), (5, 200)");
    exec_ok("CREATE INDEX idx_p4_id ON products4(id)");
    exec_ok("CREATE INDEX idx_p4_price ON products4(price)");
    rebuild_indexes();

    auto qr = exec_ok(
        "SELECT id FROM products4 WHERE id <= 2 OR price > 100 ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u)
        << "LESS_EQUAL OR arm: expected ids 1,2,5 but got " << qr.rows.size();
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 2);
    EXPECT_EQ(qr.rows[2][0].as_int32(), 5);
}

// Both OR arms are range (non-equality): id < 2 OR price > 150
// id<2 → id=1; price>150 → id=5. Union = 2 rows.
TEST_F(QA_GDB803, BitmapScan_BothArmsRange_ReturnsCorrectRows) {
    exec_ok("CREATE TABLE products5 (id INT, price INT)");
    exec_ok("INSERT INTO products5 VALUES "
            "(1, 10), (2, 50), (3, 100), (4, 5), (5, 200)");
    exec_ok("CREATE INDEX idx_p5_id ON products5(id)");
    exec_ok("CREATE INDEX idx_p5_price ON products5(price)");
    rebuild_indexes();

    auto qr = exec_ok(
        "SELECT id FROM products5 WHERE id < 2 OR price > 150 ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u)
        << "Both range arms: expected ids 1,5 but got " << qr.rows.size();
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 5);
}

// No false positives: OR of two equality predicates that match only one row
// each, with no overlap. Ensure BitmapScan does not return extra rows.
TEST_F(QA_GDB803, BitmapScan_NoFalsePositives_ExactUnion) {
    exec_ok("CREATE TABLE products6 (id INT, price INT)");
    exec_ok("INSERT INTO products6 VALUES "
            "(1, 10), (2, 20), (3, 30), (4, 40), (5, 50)");
    exec_ok("CREATE INDEX idx_p6_id ON products6(id)");
    exec_ok("CREATE INDEX idx_p6_price ON products6(price)");
    rebuild_indexes();

    // id=2 OR price=40 → exactly rows 2 and 4
    auto qr = exec_ok(
        "SELECT id FROM products6 WHERE id = 2 OR price = 40 ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u)
        << "Expected exactly 2 rows but got " << qr.rows.size()
        << " — BitmapScan may return false positives";
    EXPECT_EQ(qr.rows[0][0].as_int32(), 2);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 4);
}

// Overlap: both OR arms match the same row. BitmapScan OR mode must deduplicate.
TEST_F(QA_GDB803, BitmapScan_OverlapDeduplication) {
    exec_ok("CREATE TABLE products7 (id INT, price INT)");
    exec_ok("INSERT INTO products7 VALUES (1, 10), (2, 20), (3, 30)");
    exec_ok("CREATE INDEX idx_p7_id ON products7(id)");
    exec_ok("CREATE INDEX idx_p7_price ON products7(price)");
    rebuild_indexes();

    // id=1 OR price=10 — both conditions match row (id=1, price=10)
    auto qr = exec_ok(
        "SELECT id FROM products7 WHERE id = 1 OR price = 10 ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 1u)
        << "Expected exactly 1 row (duplicate from overlap), got "
        << qr.rows.size();
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
}

// =============================================================================
// BitmapScan with typed columns — UUID and TIMESTAMP literal coercion
// =============================================================================

// UUID column with UUID literal in OR predicate.
// If the coercion path in find_btree fails silently, we fall back to SeqScan
// which may return correct results. The test verifies no crash and correct rows.
TEST_F(QA_GDB803, BitmapScan_UUIDColumn_LiteralCoercion_NoCrash) {
    exec_ok("CREATE TABLE uuid_items (uid UUID, score INT)");
    exec_ok("INSERT INTO uuid_items VALUES "
            "('00000000-0000-0000-0000-000000000001', 10), "
            "('00000000-0000-0000-0000-000000000002', 20), "
            "('00000000-0000-0000-0000-000000000003', 30)");
    exec_ok("CREATE INDEX idx_uuid_uid ON uuid_items(uid)");
    exec_ok("CREATE INDEX idx_uuid_score ON uuid_items(score)");
    rebuild_indexes();

    // This may or may not trigger BitmapScan — but it must not crash and must
    // return correct results.
    auto qr = exec_ok(
        "SELECT score FROM uuid_items "
        "WHERE uid = '00000000-0000-0000-0000-000000000001' OR score = 30 "
        "ORDER BY score");

    ASSERT_EQ(qr.rows.size(), 2u)
        << "UUID OR scan: expected 2 rows but got " << qr.rows.size();
    EXPECT_EQ(qr.rows[0][0].as_int32(), 10);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 30);
}

// TIMESTAMP column with timestamp literal.
TEST_F(QA_GDB803, BitmapScan_TimestampColumn_LiteralCoercion_NoCrash) {
    exec_ok("CREATE TABLE ts_events (ts TIMESTAMP, val INT)");
    exec_ok("INSERT INTO ts_events VALUES "
            "('2024-01-01 00:00:00', 1), "
            "('2024-06-15 12:00:00', 2), "
            "('2024-12-31 23:59:59', 3)");
    exec_ok("CREATE INDEX idx_ts_ts ON ts_events(ts)");
    exec_ok("CREATE INDEX idx_ts_val ON ts_events(val)");
    rebuild_indexes();

    auto qr = exec_ok(
        "SELECT val FROM ts_events "
        "WHERE ts = '2024-01-01 00:00:00' OR val = 3 "
        "ORDER BY val");

    ASSERT_EQ(qr.rows.size(), 2u)
        << "TIMESTAMP OR scan: expected 2 rows but got " << qr.rows.size();
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 3);
}

// =============================================================================
// ExternalSort correctness and stress
// =============================================================================

// Large sort: 1000 rows inserted in reverse order, ORDER BY must produce
// them in ascending order. Verifies ExternalSort correctness at scale.
TEST_F(QA_GDB803, ExternalSort_LargeTable_CorrectOrder) {
    exec_ok("CREATE TABLE big_nums (n INT)");
    for (int i = 1000; i >= 1; --i) {
        exec_ok("INSERT INTO big_nums VALUES (" + std::to_string(i) + ")");
    }

    auto qr = exec_ok("SELECT n FROM big_nums ORDER BY n");

    ASSERT_EQ(qr.rows.size(), 1000u)
        << "ExternalSort: expected 1000 rows";
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(qr.rows[i][0].as_int32(), i + 1)
            << "ExternalSort: row " << i << " out of order";
    }
}

// DESC sort correctness on large input.
TEST_F(QA_GDB803, ExternalSort_LargeTable_DescCorrectOrder) {
    exec_ok("CREATE TABLE big_desc (n INT)");
    for (int i = 1; i <= 500; ++i) {
        exec_ok("INSERT INTO big_desc VALUES (" + std::to_string(i) + ")");
    }

    auto qr = exec_ok("SELECT n FROM big_desc ORDER BY n DESC");

    ASSERT_EQ(qr.rows.size(), 500u);
    for (int i = 0; i < 500; ++i) {
        EXPECT_EQ(qr.rows[i][0].as_int32(), 500 - i)
            << "DESC sort: row " << i << " has wrong value";
    }
}

// Multi-column sort stability: sort by (a ASC, b DESC).
TEST_F(QA_GDB803, ExternalSort_MultiColumnSort_Correctness) {
    exec_ok("CREATE TABLE mc_sort (a INT, b INT)");
    exec_ok("INSERT INTO mc_sort VALUES "
            "(2, 10), (1, 30), (1, 20), (2, 5), (1, 10)");

    auto qr = exec_ok("SELECT a, b FROM mc_sort ORDER BY a ASC, b DESC");
    ASSERT_EQ(qr.rows.size(), 5u);

    // a=1 rows first, sorted by b DESC: (1,30), (1,20), (1,10)
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_int32(), 30);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[1][1].as_int32(), 20);
    EXPECT_EQ(qr.rows[2][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[2][1].as_int32(), 10);

    // a=2 rows next, sorted by b DESC: (2,10), (2,5)
    EXPECT_EQ(qr.rows[3][0].as_int32(), 2);
    EXPECT_EQ(qr.rows[3][1].as_int32(), 10);
    EXPECT_EQ(qr.rows[4][0].as_int32(), 2);
    EXPECT_EQ(qr.rows[4][1].as_int32(), 5);
}

// No regression: small sort (3 rows) still works correctly with ExternalSort.
TEST_F(QA_GDB803, ExternalSort_SmallTable_NoRegression) {
    exec_ok("CREATE TABLE tiny_sort (x INT)");
    exec_ok("INSERT INTO tiny_sort VALUES (3), (1), (2)");

    auto qr = exec_ok("SELECT x FROM tiny_sort ORDER BY x");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 2);
    EXPECT_EQ(qr.rows[2][0].as_int32(), 3);
}

// Empty table ORDER BY: ExternalSort on zero rows must not crash.
TEST_F(QA_GDB803, ExternalSort_EmptyTable_NoCrash) {
    exec_ok("CREATE TABLE empty_sort (x INT)");
    auto qr = exec_ok("SELECT x FROM empty_sort ORDER BY x");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// Single row ORDER BY: ExternalSort on one row.
TEST_F(QA_GDB803, ExternalSort_SingleRow_NoCrash) {
    exec_ok("CREATE TABLE one_sort (x INT)");
    exec_ok("INSERT INTO one_sort VALUES (42)");
    auto qr = exec_ok("SELECT x FROM one_sort ORDER BY x");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 42);
}

// ExternalSort with string column: VARCHAR ORDER BY.
TEST_F(QA_GDB803, ExternalSort_StringColumn_CorrectOrder) {
    exec_ok("CREATE TABLE str_sort (s VARCHAR)");
    exec_ok("INSERT INTO str_sort VALUES ('banana'), ('apple'), ('cherry'), ('date')");

    auto qr = exec_ok("SELECT s FROM str_sort ORDER BY s");
    ASSERT_EQ(qr.rows.size(), 4u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "apple");
    EXPECT_EQ(qr.rows[1][0].as_string(), "banana");
    EXPECT_EQ(qr.rows[2][0].as_string(), "cherry");
    EXPECT_EQ(qr.rows[3][0].as_string(), "date");
}

// ORDER BY column not in SELECT (expression sort key).
TEST_F(QA_GDB803, ExternalSort_OrderByColumnNotInSelect_Correctness) {
    exec_ok("CREATE TABLE order_test (id INT, name VARCHAR, score INT)");
    exec_ok("INSERT INTO order_test VALUES (3, 'c', 10), (1, 'a', 30), (2, 'b', 20)");

    auto qr = exec_ok("SELECT id, name FROM order_test ORDER BY score");
    ASSERT_EQ(qr.rows.size(), 3u);
    // Ordered by score asc: score=10 (id=3), score=20 (id=2), score=30 (id=1)
    EXPECT_EQ(qr.rows[0][0].as_int32(), 3);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 2);
    EXPECT_EQ(qr.rows[2][0].as_int32(), 1);
}

// ExternalSort EXPLAIN shows "External Sort" not "Sort".
TEST_F(QA_GDB803, ExternalSort_EXPLAIN_ShowsExternalSortNode) {
    exec_ok("CREATE TABLE explain_sort (n INT)");
    exec_ok("INSERT INTO explain_sort VALUES (1), (2), (3)");

    auto plan = explain("SELECT n FROM explain_sort ORDER BY n");
    EXPECT_NE(plan.find("External Sort"), std::string::npos)
        << "EXPLAIN must show 'External Sort' not 'Sort'.\nPlan:\n" << plan;
    // Must NOT show the old SortOperator node name.
    EXPECT_EQ(plan.find("-> Sort"), std::string::npos)
        << "EXPLAIN shows old Sort node. ExternalSort should replace it.\nPlan:\n"
        << plan;
}

// =============================================================================
// Cost model unit test: SMJ with both inputs unsorted (planner scenario)
// =============================================================================

// Verify what the cost model returns when called the same way the planner
// calls it: left_sorted=false, right_sorted=false. This documents whether
// SMJ can EVER win via the planner's actual call path.
TEST(QA_GDB803_CostModel, SMJ_NeverWins_WithUnsortedInputs_DocumentBehavior) {
    // Simulate ANALYZE stats for two medium-sized tables.
    PlanCost left;
    left.total_cost    = 500.0; // seq_scan of 500-row table
    left.estimated_rows = 500.0;

    PlanCost right;
    right.total_cost    = 500.0;
    right.estimated_rows = 500.0;

    const CostModel cost_model;
    // Planner always passes false, false.
    auto [method, cost] =
        choose_join_method(left, right, 0.1, false, false, cost_model);

    // Document the actual result. If method == SORT_MERGE here,
    // SMJ is potentially reachable. If not, it is unreachable.
    if (method != JoinMethod::SORT_MERGE) {
        // This is the expected failure: SMJ cannot be reached via SQL because
        // the planner never passes pre-sorted hints to choose_join_method.
        // A follow-up ticket should wire pre-sorted detection.
        GTEST_LOG_(WARNING)
            << "CostModel with left_sorted=false, right_sorted=false chose "
            << (method == JoinMethod::HASH_JOIN ? "HASH_JOIN" : "NESTED_LOOP")
            << " (NOT SORT_MERGE). "
            << "This confirms SMJ is unreachable via the planner's SQL path. "
            << "Expected cost: " << cost.total_cost;
    } else {
        GTEST_LOG_(INFO)
            << "CostModel chose SORT_MERGE even with unsorted=false inputs. "
            << "SMJ may be reachable via SQL for this row count.";
    }
    // Not asserting — this is a documentation test.
    // The Critical defect is filed separately as a Bug ticket.
    (void)method;
    (void)cost;
}

// Verify SMJ IS chosen by cost model when both inputs are pre-sorted
// (the unit test scenario from test_planner_operator_reachability.cpp).
// If this fails, the cost model itself is broken.
TEST(QA_GDB803_CostModel, SMJ_ChosenWhen_BothInputsPreSorted) {
    PlanCost left;
    left.total_cost     = 100.0;
    left.estimated_rows = 10000.0;

    PlanCost right;
    right.total_cost     = 100.0;
    right.estimated_rows = 10000.0;

    const CostModel cost_model;
    auto [method, cost] =
        choose_join_method(left, right, 0.1, true, true, cost_model);

    EXPECT_EQ(method, JoinMethod::SORT_MERGE)
        << "Cost model must pick SORT_MERGE when both inputs are pre-sorted "
        << "(zero sort cost). Actual method: "
        << (method == JoinMethod::HASH_JOIN ? "HASH_JOIN" : "NESTED_LOOP");
    (void)cost;
}
