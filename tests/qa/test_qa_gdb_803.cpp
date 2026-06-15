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
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb803";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
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
            << "[SQL] " << sql << "\n[ERR] " << (result ? "" : result.error().message);
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
        auto r = index_manager_->rebuild_all_indexes();
        ASSERT_TRUE(r.has_value()) << r.error().message;
        engine_->set_index_manager(index_manager_.get());
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<IndexManager> index_manager_;
};

// =============================================================================
// SMJ Reachability
// =============================================================================

// SMJ is chosen when inputs are LEGITIMATELY sorted on the join key.
// This test uses subqueries with ORDER BY so each child is an ExternalSort
// node sorted on the join key column. The cost model then detects pre-sorted
// inputs, eliminates the sort overhead from SMJ's cost, and should choose
// SortMergeJoin over HashJoin when ANALYZE statistics are present.
TEST_F(QA_GDB803, SMJ_ReachableViaSQL_AfterANALYZE) {
    exec_ok("CREATE TABLE smj_left (id INT, val INT)");
    exec_ok("CREATE TABLE smj_right (id INT, score INT)");

    // Insert enough rows for ANALYZE to produce meaningful cost estimates.
    for (int i = 1; i <= 500; ++i) {
        exec_ok("INSERT INTO smj_left VALUES (" + std::to_string(i) + ", " + std::to_string(i * 2) +
                ")");
        exec_ok("INSERT INTO smj_right VALUES (" + std::to_string(i) + ", " +
                std::to_string(i * 3) + ")");
    }

    // Run ANALYZE to populate table statistics (required for cost-based path).
    exec_ok("ANALYZE smj_left");
    exec_ok("ANALYZE smj_right");

    // Join via subqueries with ORDER BY on the join key. This produces
    // ExternalSort children sorted on `id`, so is_sorted_on_join_key returns
    // true for both sides. The cost model should then prefer SortMergeJoin.
    const std::string join_sql = "SELECT l.id FROM "
                                 "(SELECT id, val FROM smj_left ORDER BY id) AS l "
                                 "JOIN (SELECT id, score FROM smj_right ORDER BY id) AS r "
                                 "ON l.id = r.id";

    auto plan = explain(join_sql);

    bool smj_selected = plan.find("Sort Merge Join") != std::string::npos ||
                        plan.find("SortMerge") != std::string::npos;

    EXPECT_TRUE(smj_selected) << "SortMergeJoin was NOT selected even with pre-sorted inputs.\n"
                              << "Both subqueries ORDER BY the join key so is_sorted_on_join_key\n"
                              << "should return true, making SMJ cost-competitive.\n"
                              << "EXPLAIN output:\n"
                              << plan;
}

// Adversarial: verify the planner produces correct join results regardless of
// which join method is chosen. SMJ must not silently drop rows.
TEST_F(QA_GDB803, SMJ_JoinCorrectnessWithANALYZE_AllRowsPresent) {
    exec_ok("CREATE TABLE left_t (id INT, tag VARCHAR)");
    exec_ok("CREATE TABLE right_t (id INT, dept VARCHAR)");

    for (int i = 1; i <= 100; ++i) {
        exec_ok("INSERT INTO left_t VALUES (" + std::to_string(i) + ", 'L" + std::to_string(i) +
                "')");
        exec_ok("INSERT INTO right_t VALUES (" + std::to_string(i) + ", 'D" +
                std::to_string(i % 5) + "')");
    }

    exec_ok("ANALYZE left_t");
    exec_ok("ANALYZE right_t");

    auto qr = exec_ok("SELECT left_t.id FROM left_t "
                      "JOIN right_t ON left_t.id = right_t.id "
                      "ORDER BY left_t.id");

    // All 100 rows must be present — no silent row loss from SMJ merge.
    ASSERT_EQ(qr.rows.size(), 100u)
        << "Join result has wrong number of rows — possible SMJ merge bug";
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(qr.rows[i][0].as_int32(), i + 1) << "Row " << i << " has wrong id value";
    }
}

// Adversarial: ANALYZE on empty tables. Cost estimates from empty tables
// should default gracefully and not trigger SMJ or crash.
TEST_F(QA_GDB803, SMJ_EmptyTableAfterANALYZE_NoCrash) {
    exec_ok("CREATE TABLE empty_l (id INT)");
    exec_ok("CREATE TABLE empty_r (id INT)");
    exec_ok("ANALYZE empty_l");
    exec_ok("ANALYZE empty_r");

    auto qr = exec_ok("SELECT empty_l.id FROM empty_l "
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
    auto qr = exec_ok("SELECT id FROM products2 WHERE id = 3 OR price < 20 ORDER BY id");

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

    auto qr = exec_ok("SELECT id FROM products3 WHERE id > 3 OR price = 10 ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u)
        << "GREATER OR arm: expected 3 rows (id=1,4,5) but got " << qr.rows.size()
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

    auto qr = exec_ok("SELECT id FROM products4 WHERE id <= 2 OR price > 100 ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u) << "LESS_EQUAL OR arm: expected ids 1,2,5 but got "
                                  << qr.rows.size();
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

    auto qr = exec_ok("SELECT id FROM products5 WHERE id < 2 OR price > 150 ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u) << "Both range arms: expected ids 1,5 but got " << qr.rows.size();
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
    auto qr = exec_ok("SELECT id FROM products6 WHERE id = 2 OR price = 40 ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u) << "Expected exactly 2 rows but got " << qr.rows.size()
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
    auto qr = exec_ok("SELECT id FROM products7 WHERE id = 1 OR price = 10 ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 1u) << "Expected exactly 1 row (duplicate from overlap), got "
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
    auto qr = exec_ok("SELECT score FROM uuid_items "
                      "WHERE uid = '00000000-0000-0000-0000-000000000001' OR score = 30 "
                      "ORDER BY score");

    ASSERT_EQ(qr.rows.size(), 2u) << "UUID OR scan: expected 2 rows but got " << qr.rows.size();
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

    auto qr = exec_ok("SELECT val FROM ts_events "
                      "WHERE ts = '2024-01-01 00:00:00' OR val = 3 "
                      "ORDER BY val");

    ASSERT_EQ(qr.rows.size(), 2u) << "TIMESTAMP OR scan: expected 2 rows but got "
                                  << qr.rows.size();
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

    ASSERT_EQ(qr.rows.size(), 1000u) << "ExternalSort: expected 1000 rows";
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(qr.rows[i][0].as_int32(), i + 1) << "ExternalSort: row " << i << " out of order";
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
        << "EXPLAIN must show 'External Sort' not 'Sort'.\nPlan:\n"
        << plan;
    // Must NOT show the old SortOperator node name.
    EXPECT_EQ(plan.find("-> Sort"), std::string::npos)
        << "EXPLAIN shows old Sort node. ExternalSort should replace it.\nPlan:\n"
        << plan;
}

// =============================================================================
// Cost model: HashJoin wins for unsorted inputs (no over-selection of SMJ)
// =============================================================================

// Verify that the cost model does NOT pick SortMergeJoin when both inputs are
// unsorted. This is the regression guard: after GDB-1261 fixed the sortedness
// detection, the planner should pass left_sorted=false, right_sorted=false for
// plain SeqScan inputs, and the cost model should choose HashJoin (cheaper
// because SMJ needs to sort both inputs first).
TEST(QA_GDB803_CostModel, SMJ_NeverWins_WithUnsortedInputs_DocumentBehavior) {
    // Simulate ANALYZE stats for two medium-sized tables (typical SeqScan cost).
    PlanCost left;
    left.total_cost = 500.0; // seq_scan of 500-row table
    left.estimated_rows = 500.0;

    PlanCost right;
    right.total_cost = 500.0;
    right.estimated_rows = 500.0;

    const CostModel cost_model;
    // Unsorted inputs: the planner now correctly passes false, false.
    auto [method, cost] = choose_join_method(left, right, 0.1, false, false, cost_model);

    // HashJoin must win for unsorted inputs — SMJ sort overhead makes it
    // more expensive than building a hash table for this row count.
    EXPECT_NE(method, JoinMethod::SORT_MERGE)
        << "Cost model chose SORT_MERGE for UNSORTED inputs (over-selection).\n"
        << "HashJoin should be preferred when sort cost must be paid.\n"
        << "Total cost: " << cost.total_cost;
    (void)cost;
}

// Verify SMJ IS chosen by cost model when both inputs are pre-sorted
// (the unit test scenario from test_planner_operator_reachability.cpp).
// If this fails, the cost model itself is broken.
TEST(QA_GDB803_CostModel, SMJ_ChosenWhen_BothInputsPreSorted) {
    PlanCost left;
    left.total_cost = 100.0;
    left.estimated_rows = 10000.0;

    PlanCost right;
    right.total_cost = 100.0;
    right.estimated_rows = 10000.0;

    const CostModel cost_model;
    auto [method, cost] = choose_join_method(left, right, 0.1, true, true, cost_model);

    EXPECT_EQ(method, JoinMethod::SORT_MERGE)
        << "Cost model must pick SORT_MERGE when both inputs are pre-sorted "
        << "(zero sort cost). Actual method: "
        << (method == JoinMethod::HASH_JOIN ? "HASH_JOIN" : "NESTED_LOOP");
    (void)cost;
}

// =============================================================================
// GDB-1261 fix: is_sorted_on_join_key adversarial probes
// =============================================================================

// Adversarial: sort on a DIFFERENT column than the join key.
// If is_sorted_on_join_key ignores the sort-key column and only checks that the
// child is an ExternalSort, SMJ would be incorrectly selected.
// Query: join on `id` but ORDER BY `score` in left subquery.
// Expected: HashJoin (not SMJ) — the left input is NOT sorted on the join key.
TEST_F(QA_GDB803, SMJ_NotSelected_WhenSortedOnDifferentColumn) {
    exec_ok("CREATE TABLE diff_left (id INT, score INT)");
    exec_ok("CREATE TABLE diff_right (id INT, val INT)");

    for (int i = 1; i <= 500; ++i) {
        exec_ok("INSERT INTO diff_left VALUES (" + std::to_string(i) + ", " +
                std::to_string(501 - i) + ")");
        exec_ok("INSERT INTO diff_right VALUES (" + std::to_string(i) + ", " +
                std::to_string(i) + ")");
    }
    exec_ok("ANALYZE diff_left");
    exec_ok("ANALYZE diff_right");

    // Left subquery sorts by `score`, not `id`. Right subquery sorts by `val`, not `id`.
    // is_sorted_on_join_key should return false for both sides.
    const std::string sql =
        "SELECT l.id FROM "
        "(SELECT id, score FROM diff_left ORDER BY score) AS l "
        "JOIN (SELECT id, val FROM diff_right ORDER BY val) AS r "
        "ON l.id = r.id";

    auto plan = explain(sql);
    bool smj_selected = plan.find("Sort Merge Join") != std::string::npos;
    EXPECT_FALSE(smj_selected)
        << "SMJ was incorrectly selected when inputs are sorted on DIFFERENT columns "
           "than the join key.\nis_sorted_on_join_key must check the sort key column "
           "name matches the join key column name.\nPlan:\n"
        << plan;

    // Correctness: regardless of join method, result must be correct (500 rows).
    auto qr = exec_ok(sql);
    EXPECT_EQ(qr.rows.size(), 500u)
        << "Join correctness failed: expected 500 rows but got " << qr.rows.size();
}

// Adversarial: join on column `id`, both tables have a column named `id`,
// left subquery ORDER BY id -> SMJ SHOULD be selected (both sorted on join key).
// This is the positive case confirming the fix works end-to-end through SQL.
TEST_F(QA_GDB803, SMJ_Selected_WhenBothSubqueriesSortedOnJoinKey_Large) {
    exec_ok("CREATE TABLE smj_l2 (id INT, data INT)");
    exec_ok("CREATE TABLE smj_r2 (id INT, data INT)");

    for (int i = 1; i <= 1000; ++i) {
        exec_ok("INSERT INTO smj_l2 VALUES (" + std::to_string(i) + ", " +
                std::to_string(i * 2) + ")");
        exec_ok("INSERT INTO smj_r2 VALUES (" + std::to_string(i) + ", " +
                std::to_string(i * 3) + ")");
    }
    exec_ok("ANALYZE smj_l2");
    exec_ok("ANALYZE smj_r2");

    const std::string sql =
        "SELECT l.id FROM "
        "(SELECT id, data FROM smj_l2 ORDER BY id) AS l "
        "JOIN (SELECT id, data FROM smj_r2 ORDER BY id) AS r "
        "ON l.id = r.id";

    auto plan = explain(sql);
    bool smj_selected = plan.find("Sort Merge Join") != std::string::npos;
    EXPECT_TRUE(smj_selected)
        << "SMJ must be selected when both subqueries ORDER BY the join key.\nPlan:\n"
        << plan;

    // Correctness: all 1000 matching rows must appear.
    auto qr = exec_ok(sql);
    EXPECT_EQ(qr.rows.size(), 1000u)
        << "SMJ correctness: expected 1000 rows but got " << qr.rows.size();
}

// Adversarial: join on asymmetric column names (l.id = r.score).
// Left sorted on `id`, right sorted on `score`. The join key for the left is
// `id` and for the right is `score`. is_sorted_on_join_key must check each
// side independently with its own key hint.
// This tests that the lkey_hint / rkey_hint are correctly attributed.
TEST_F(QA_GDB803, SMJ_AsymmetricJoinKey_CorrectSortednessCheck) {
    exec_ok("CREATE TABLE asym_left (id INT, payload INT)");
    exec_ok("CREATE TABLE asym_right (score INT, payload INT)");

    for (int i = 1; i <= 500; ++i) {
        exec_ok("INSERT INTO asym_left VALUES (" + std::to_string(i) + ", 0)");
        exec_ok("INSERT INTO asym_right VALUES (" + std::to_string(i) + ", 0)");
    }
    exec_ok("ANALYZE asym_left");
    exec_ok("ANALYZE asym_right");

    // Left subquery sorts by `id`, right subquery sorts by `score`.
    // ON l.id = r.score — left key is `id`, right key is `score`.
    // Both inputs ARE sorted on THEIR respective join keys.
    // SMJ SHOULD be selected.
    const std::string sql =
        "SELECT l.id FROM "
        "(SELECT id, payload FROM asym_left ORDER BY id) AS l "
        "JOIN (SELECT score, payload FROM asym_right ORDER BY score) AS r "
        "ON l.id = r.score";

    auto plan = explain(sql);
    bool smj_selected = plan.find("Sort Merge Join") != std::string::npos;
    EXPECT_TRUE(smj_selected)
        << "SMJ must be selected for asymmetric key names when each subquery "
           "is sorted on its own join key column.\nPlan:\n"
        << plan;

    // Correctness: 500 matching rows.
    auto qr = exec_ok(sql);
    EXPECT_EQ(qr.rows.size(), 500u)
        << "Asymmetric join correctness: expected 500 rows, got " << qr.rows.size();
}

// Adversarial: is_sorted_on_join_key through a Filter layer.
// The transparent-wrapper traversal in is_sorted_on_join_key must descend
// through a Filter to reach the ExternalSort below.
// Query: left is (SELECT ... ORDER BY id) but filtered by a WHERE inside the subquery.
// The outer SubqueryScan wraps ExternalSort -> Filter structure.
// Correct behavior: planner should still detect sortedness.
TEST_F(QA_GDB803, SMJ_SortednessDetected_ThroughFilterLayer) {
    exec_ok("CREATE TABLE filter_left (id INT, active INT)");
    exec_ok("CREATE TABLE filter_right (id INT, val INT)");

    for (int i = 1; i <= 600; ++i) {
        exec_ok("INSERT INTO filter_left VALUES (" + std::to_string(i) + ", " +
                std::to_string(i % 2) + ")");
        exec_ok("INSERT INTO filter_right VALUES (" + std::to_string(i) + ", " +
                std::to_string(i) + ")");
    }
    exec_ok("ANALYZE filter_left");
    exec_ok("ANALYZE filter_right");

    // Correctness: join with filtered left — result must be correct regardless
    // of join method chosen.
    auto qr = exec_ok(
        "SELECT l.id FROM "
        "(SELECT id FROM filter_left WHERE active = 1 ORDER BY id) AS l "
        "JOIN (SELECT id FROM filter_right ORDER BY id) AS r "
        "ON l.id = r.id "
        "ORDER BY l.id");

    // active=1 rows are odd ids: 1,3,5,...,599 → 300 rows
    ASSERT_EQ(qr.rows.size(), 300u)
        << "Filter+sort join: expected 300 rows (odd ids 1-599) but got " << qr.rows.size();
    for (size_t i = 0; i < qr.rows.size(); ++i) {
        int expected_id = static_cast<int>(i) * 2 + 1;
        EXPECT_EQ(qr.rows[i][0].as_int32(), expected_id)
            << "Row " << i << " has wrong id";
    }
}

// Adversarial: self-join where both sides scan the same table.
// is_sorted_on_join_key must not confuse the two sides of the self-join.
// If the left subquery ORDER BY id and right subquery ORDER BY id, both are
// sorted — SMJ should be selected and produce correct results (no row mixing).
TEST_F(QA_GDB803, SMJ_SelfJoin_CorrectResults) {
    exec_ok("CREATE TABLE self_tbl (id INT, grp INT)");
    for (int i = 1; i <= 20; ++i) {
        exec_ok("INSERT INTO self_tbl VALUES (" + std::to_string(i) + ", " +
                std::to_string(i % 4) + ")");
    }
    exec_ok("ANALYZE self_tbl");

    // Self-join: find pairs (a.id, b.id) where a.grp = b.grp, a.id < b.id.
    // Join on grp (same-name column on both sides).
    auto qr = exec_ok(
        "SELECT a.id, b.id FROM self_tbl AS a "
        "JOIN self_tbl AS b ON a.grp = b.grp "
        "WHERE a.id < b.id "
        "ORDER BY a.id, b.id");

    // grp 0: ids 4,8,12,16,20 → C(5,2)=10 pairs
    // grp 1: ids 1,5,9,13,17 → C(5,2)=10 pairs
    // grp 2: ids 2,6,10,14,18 → C(5,2)=10 pairs
    // grp 3: ids 3,7,11,15,19 → C(5,2)=10 pairs
    // Total: 40 pairs
    EXPECT_EQ(qr.rows.size(), 40u)
        << "Self-join: expected 40 pairs, got " << qr.rows.size()
        << ". SMJ may have produced wrong results for self-join.";
}

// Adversarial: multi-join chain (three tables). After the first join, the
// combined operator is a Hash/SMJ join (2+ children). is_sorted_on_join_key
// on this multi-child operator should return false (correctly unsorted),
// preventing the second join from incorrectly selecting SMJ.
TEST_F(QA_GDB803, SMJ_ThreeTableJoin_MiddleInputNotSorted) {
    exec_ok("CREATE TABLE t3a (id INT, val INT)");
    exec_ok("CREATE TABLE t3b (id INT, val INT)");
    exec_ok("CREATE TABLE t3c (id INT, val INT)");

    for (int i = 1; i <= 50; ++i) {
        exec_ok("INSERT INTO t3a VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")");
        exec_ok("INSERT INTO t3b VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")");
        exec_ok("INSERT INTO t3c VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")");
    }
    exec_ok("ANALYZE t3a");
    exec_ok("ANALYZE t3b");
    exec_ok("ANALYZE t3c");

    // Three-table join: t3a JOIN t3b JOIN t3c all on id.
    // Result must contain 50 rows with matching ids.
    auto qr = exec_ok(
        "SELECT t3a.id FROM t3a "
        "JOIN t3b ON t3a.id = t3b.id "
        "JOIN t3c ON t3a.id = t3c.id "
        "ORDER BY t3a.id");

    ASSERT_EQ(qr.rows.size(), 50u)
        << "Three-table join: expected 50 rows but got " << qr.rows.size();
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(qr.rows[i][0].as_int32(), i + 1)
            << "Three-table join: row " << i << " has wrong id";
    }
}

// Adversarial: HashJoin (unsorted inputs) correctness regression guard.
// With ANALYZE stats for small tables, the planner should choose HashJoin
// for plain SeqScan inputs. Verify the result is correct (no data corruption).
TEST_F(QA_GDB803, HashJoin_Unsorted_CorrectResults) {
    exec_ok("CREATE TABLE hj_l (id INT, name VARCHAR)");
    exec_ok("CREATE TABLE hj_r (id INT, dept VARCHAR)");

    for (int i = 1; i <= 200; ++i) {
        exec_ok("INSERT INTO hj_l VALUES (" + std::to_string(i) + ", 'emp" +
                std::to_string(i) + "')");
        exec_ok("INSERT INTO hj_r VALUES (" + std::to_string(i) + ", 'dept" +
                std::to_string(i % 10) + "')");
    }
    exec_ok("ANALYZE hj_l");
    exec_ok("ANALYZE hj_r");

    // Plain SeqScan on both sides — should choose HashJoin.
    auto plan = explain("SELECT hj_l.id FROM hj_l JOIN hj_r ON hj_l.id = hj_r.id");
    bool smj = plan.find("Sort Merge Join") != std::string::npos;
    EXPECT_FALSE(smj)
        << "SMJ over-selected for plain SeqScan inputs (unsorted).\nPlan:\n"
        << plan;

    auto qr = exec_ok(
        "SELECT hj_l.id FROM hj_l JOIN hj_r ON hj_l.id = hj_r.id ORDER BY hj_l.id");
    ASSERT_EQ(qr.rows.size(), 200u)
        << "HashJoin correctness: expected 200 rows but got " << qr.rows.size();
    for (int i = 0; i < 200; ++i) {
        EXPECT_EQ(qr.rows[i][0].as_int32(), i + 1)
            << "HashJoin correctness: row " << i << " wrong id";
    }
}

// Adversarial: non-equi join (ON a.id < b.id). Non-equi joins cannot be
// expressed as equi-joins, so the planner falls through to NestedLoopJoin.
// is_sorted_on_join_key is only invoked when on_expr is a BINARY EQUAL.
// This tests that the code path for non-equi joins still works correctly.
TEST_F(QA_GDB803, NonEquiJoin_NoSMJ_NestedLoop) {
    exec_ok("CREATE TABLE neq_l (id INT)");
    exec_ok("CREATE TABLE neq_r (id INT)");
    exec_ok("INSERT INTO neq_l VALUES (1), (2), (3)");
    exec_ok("INSERT INTO neq_r VALUES (2), (3), (4)");
    exec_ok("ANALYZE neq_l");
    exec_ok("ANALYZE neq_r");

    // ON neq_l.id < neq_r.id — non-equi, must use NestedLoop.
    auto plan = explain("SELECT neq_l.id FROM neq_l JOIN neq_r ON neq_l.id < neq_r.id");
    bool smj = plan.find("Sort Merge Join") != std::string::npos;
    EXPECT_FALSE(smj)
        << "SMJ must NOT be selected for non-equi join predicates.\nPlan:\n"
        << plan;

    // Correctness: pairs (1,2),(1,3),(1,4),(2,3),(2,4),(3,4) = 6 rows.
    auto qr = exec_ok(
        "SELECT neq_l.id FROM neq_l JOIN neq_r ON neq_l.id < neq_r.id ORDER BY neq_l.id");
    EXPECT_EQ(qr.rows.size(), 6u) << "Non-equi join: expected 6 rows but got " << qr.rows.size();
}

// Adversarial: SMJ join produces CORRECT results for partial match inputs.
// Left has ids 1-100, right has only even ids 2,4,...,100.
// SMJ must advance both pointers correctly and match only 50 rows.
TEST_F(QA_GDB803, SMJ_PartialMatch_CorrectResults) {
    exec_ok("CREATE TABLE pm_l (id INT)");
    exec_ok("CREATE TABLE pm_r (id INT)");

    for (int i = 1; i <= 100; ++i) {
        exec_ok("INSERT INTO pm_l VALUES (" + std::to_string(i) + ")");
    }
    for (int i = 2; i <= 100; i += 2) {
        exec_ok("INSERT INTO pm_r VALUES (" + std::to_string(i) + ")");
    }
    exec_ok("ANALYZE pm_l");
    exec_ok("ANALYZE pm_r");

    // Use ORDER BY on join key to trigger SMJ.
    auto qr = exec_ok(
        "SELECT l.id FROM "
        "(SELECT id FROM pm_l ORDER BY id) AS l "
        "JOIN (SELECT id FROM pm_r ORDER BY id) AS r "
        "ON l.id = r.id "
        "ORDER BY l.id");

    ASSERT_EQ(qr.rows.size(), 50u)
        << "SMJ partial match: expected 50 rows (even ids 2-100) but got " << qr.rows.size();
    for (size_t i = 0; i < qr.rows.size(); ++i) {
        EXPECT_EQ(qr.rows[i][0].as_int32(), static_cast<int>(i + 1) * 2)
            << "SMJ partial match: row " << i << " wrong id";
    }
}
