/// GDB-837: Adversarial QA tests for IN (subquery) evaluation.
///
/// Covers:
///   1. NULL combinatorics in 3VL truth table (outer NULL, inner NULL, both)
///   2. Duplicates in subquery set — no double-counting in outer result
///   3. Large subquery set (stress)
///   4. Subquery returning ALL NULLs
///   5. Single-row subquery
///   6. Subquery with WHERE / LIMIT / ORDER BY
///   7. Subquery over an empty table (already in dev tests; re-verified here)
///   8. Subquery referencing a non-existent column — clean error
///   9. Nested IN (subquery within subquery) if supported
///  10. Multi-column subquery error path — no crash / partial state
///  11. Correlated probe — document behavior (error or silent wrong results)
///  12. Outer NULL row IN non-null-set → filtered (UNKNOWN)
///  13. Outer NULL row NOT IN non-null-set → filtered (UNKNOWN)
///  14. Outer NULL row NOT IN empty-set → passes (TRUE)

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// Fixture
// =============================================================================

class QA_GDB837 : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb837";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_  = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto r = engine_->execute(sql);
        EXPECT_TRUE(r.has_value()) << "[exec_ok] SQL: " << sql
                                   << "\n  error: " << (r ? "" : r.error().message);
        return r ? std::move(*r) : QueryResult{};
    }

    void exec_error(const std::string& sql, StatusCode expected_code) {
        auto r = engine_->execute(sql);
        ASSERT_FALSE(r.has_value()) << "[exec_error] Expected failure for: " << sql;
        EXPECT_EQ(r.error().code, expected_code) << r.error().message;
    }

    /// Extract int32 values from column 0, sorted ascending.
    std::vector<int32_t> col0_sorted(const QueryResult& qr) {
        std::vector<int32_t> out;
        out.reserve(qr.rows.size());
        for (const auto& row : qr.rows) {
            out.push_back(row[0].as_int32());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// 1. NULL combinatorics — outer NULL vs. IN
// =============================================================================

/// Outer row has NULL id. NULL IN (non-null, non-empty set) → UNKNOWN → filtered.
TEST_F(QA_GDB837, OuterNullInNonNullSet_IsFiltered) {
    exec_ok("CREATE TABLE outer_null_in (id INT)");
    exec_ok("INSERT INTO outer_null_in (id) VALUES (NULL)");
    exec_ok("INSERT INTO outer_null_in VALUES (5)");

    exec_ok("CREATE TABLE values_set (v INT)");
    exec_ok("INSERT INTO values_set VALUES (5)");
    exec_ok("INSERT INTO values_set VALUES (10)");

    // NULL IN (5, 10) → UNKNOWN → row filtered.
    // 5 IN (5, 10) → TRUE → row passes.
    auto qr = exec_ok("SELECT id FROM outer_null_in WHERE id IN (SELECT v FROM values_set)");
    // Only id=5 should be returned; the NULL row is UNKNOWN → filtered.
    ASSERT_EQ(qr.rows.size(), 1u) << "NULL outer IN non-null subquery must yield UNKNOWN (filtered)";
    EXPECT_EQ(qr.rows[0][0].as_int32(), 5);
}

/// Outer row has NULL id. NULL NOT IN (non-null, non-empty set) → UNKNOWN → filtered.
TEST_F(QA_GDB837, OuterNullNotInNonNullSet_IsFiltered) {
    exec_ok("CREATE TABLE outer_null_notin (id INT)");
    exec_ok("INSERT INTO outer_null_notin (id) VALUES (NULL)");
    exec_ok("INSERT INTO outer_null_notin VALUES (3)");
    exec_ok("INSERT INTO outer_null_notin VALUES (7)");

    exec_ok("CREATE TABLE excl_set (v INT)");
    exec_ok("INSERT INTO excl_set VALUES (99)");

    // NULL NOT IN (99) → UNKNOWN → filtered.
    // 3 NOT IN (99) → TRUE → passes.
    // 7 NOT IN (99) → TRUE → passes.
    auto qr = exec_ok("SELECT id FROM outer_null_notin WHERE id NOT IN (SELECT v FROM excl_set)");
    // Only ids 3 and 7 should appear.
    ASSERT_EQ(qr.rows.size(), 2u) << "NULL outer NOT IN non-null subquery must yield UNKNOWN (filtered)";
    auto ids = col0_sorted(qr);
    EXPECT_EQ(ids[0], 3);
    EXPECT_EQ(ids[1], 7);
}

/// Outer row has NULL id. NULL NOT IN (empty set) → TRUE → row passes.
TEST_F(QA_GDB837, OuterNullNotInEmptySet_Passes) {
    exec_ok("CREATE TABLE outer_null_notin_empty (id INT)");
    exec_ok("INSERT INTO outer_null_notin_empty (id) VALUES (NULL)");
    exec_ok("INSERT INTO outer_null_notin_empty VALUES (42)");

    exec_ok("CREATE TABLE empty_excl (v INT)");
    // no rows

    // NULL NOT IN () → TRUE (empty set, vacuously not in).
    // 42 NOT IN () → TRUE.
    // Both rows should pass.
    auto qr = exec_ok("SELECT id FROM outer_null_notin_empty WHERE id NOT IN (SELECT v FROM empty_excl)");
    EXPECT_EQ(qr.rows.size(), 2u) << "NOT IN empty set must return all rows including NULLs";
}

/// Outer row has NULL id. NULL IN (empty set) → FALSE → filtered.
TEST_F(QA_GDB837, OuterNullInEmptySet_IsFiltered) {
    exec_ok("CREATE TABLE outer_null_in_empty (id INT)");
    exec_ok("INSERT INTO outer_null_in_empty (id) VALUES (NULL)");
    exec_ok("INSERT INTO outer_null_in_empty VALUES (1)");

    exec_ok("CREATE TABLE empty_vals (v INT)");
    // no rows

    // NULL IN () → FALSE (empty set), 1 IN () → FALSE.
    // Both filtered → 0 rows.
    auto qr = exec_ok("SELECT id FROM outer_null_in_empty WHERE id IN (SELECT v FROM empty_vals)");
    EXPECT_EQ(qr.rows.size(), 0u) << "IN empty set must always return FALSE (0 rows)";
}

// =============================================================================
// 2. Subquery returns ALL NULLs
// =============================================================================

/// IN (subquery that returns only NULLs): every outer row → UNKNOWN → 0 rows.
TEST_F(QA_GDB837, InSubqueryAllNulls_ReturnsZeroRows) {
    exec_ok("CREATE TABLE main_t (id INT)");
    exec_ok("INSERT INTO main_t VALUES (1)");
    exec_ok("INSERT INTO main_t VALUES (2)");
    exec_ok("INSERT INTO main_t VALUES (3)");

    exec_ok("CREATE TABLE all_null_t (v INT)");
    exec_ok("INSERT INTO all_null_t (v) VALUES (NULL)");
    exec_ok("INSERT INTO all_null_t (v) VALUES (NULL)");

    // x IN (NULL, NULL): 1 IN (NULL, NULL) → UNKNOWN, etc.
    auto qr = exec_ok("SELECT id FROM main_t WHERE id IN (SELECT v FROM all_null_t)");
    EXPECT_EQ(qr.rows.size(), 0u) << "IN subquery with only NULLs must return 0 rows (all UNKNOWN)";
}

/// NOT IN (subquery that returns only NULLs): every outer row → UNKNOWN → 0 rows.
TEST_F(QA_GDB837, NotInSubqueryAllNulls_ReturnsZeroRows) {
    exec_ok("CREATE TABLE main_t2 (id INT)");
    exec_ok("INSERT INTO main_t2 VALUES (1)");
    exec_ok("INSERT INTO main_t2 VALUES (2)");

    exec_ok("CREATE TABLE all_null_t2 (v INT)");
    exec_ok("INSERT INTO all_null_t2 (v) VALUES (NULL)");

    // x NOT IN (NULL): every outer row → UNKNOWN → filtered.
    auto qr = exec_ok("SELECT id FROM main_t2 WHERE id NOT IN (SELECT v FROM all_null_t2)");
    EXPECT_EQ(qr.rows.size(), 0u) << "NOT IN (only NULLs) must return 0 rows";
}

// =============================================================================
// 3. Duplicates in subquery — no double-counting in outer result
// =============================================================================

/// Subquery returns duplicate values. Each matching outer row must appear exactly once.
TEST_F(QA_GDB837, InSubqueryWithDuplicatesNoDoubleCounting) {
    exec_ok("CREATE TABLE outer_dup (id INT)");
    exec_ok("INSERT INTO outer_dup VALUES (1)");
    exec_ok("INSERT INTO outer_dup VALUES (2)");
    exec_ok("INSERT INTO outer_dup VALUES (3)");

    exec_ok("CREATE TABLE dup_sub (v INT)");
    exec_ok("INSERT INTO dup_sub VALUES (2)");
    exec_ok("INSERT INTO dup_sub VALUES (2)");  // duplicate
    exec_ok("INSERT INTO dup_sub VALUES (2)");  // triple
    exec_ok("INSERT INTO dup_sub VALUES (3)");

    // id=2 appears once in outer, subquery has 2 three times. Outer should yield 2 rows: {2, 3}.
    auto qr = exec_ok("SELECT id FROM outer_dup WHERE id IN (SELECT v FROM dup_sub)");
    ASSERT_EQ(qr.rows.size(), 2u) << "Duplicates in subquery must not multiply outer result rows";
    auto ids = col0_sorted(qr);
    EXPECT_EQ(ids[0], 2);
    EXPECT_EQ(ids[1], 3);
}

/// NOT IN with duplicates: each non-matching outer row must appear exactly once.
TEST_F(QA_GDB837, NotInSubqueryWithDuplicatesNoDoubleCounting) {
    exec_ok("CREATE TABLE outer_dup2 (id INT)");
    exec_ok("INSERT INTO outer_dup2 VALUES (10)");
    exec_ok("INSERT INTO outer_dup2 VALUES (20)");
    exec_ok("INSERT INTO outer_dup2 VALUES (30)");

    exec_ok("CREATE TABLE dup_excl (v INT)");
    exec_ok("INSERT INTO dup_excl VALUES (20)");
    exec_ok("INSERT INTO dup_excl VALUES (20)");  // duplicate

    // 10 NOT IN (20, 20) → TRUE, 20 → FALSE, 30 → TRUE.
    auto qr = exec_ok("SELECT id FROM outer_dup2 WHERE id NOT IN (SELECT v FROM dup_excl)");
    ASSERT_EQ(qr.rows.size(), 2u) << "Duplicates in NOT IN subquery must not multiply outer result rows";
    auto ids = col0_sorted(qr);
    EXPECT_EQ(ids[0], 10);
    EXPECT_EQ(ids[1], 30);
}

// =============================================================================
// 4. Single-row subquery
// =============================================================================

TEST_F(QA_GDB837, InSingleRowSubquery) {
    exec_ok("CREATE TABLE sr_outer (id INT)");
    exec_ok("INSERT INTO sr_outer VALUES (5)");
    exec_ok("INSERT INTO sr_outer VALUES (6)");

    exec_ok("CREATE TABLE sr_inner (v INT)");
    exec_ok("INSERT INTO sr_inner VALUES (5)");

    auto qr = exec_ok("SELECT id FROM sr_outer WHERE id IN (SELECT v FROM sr_inner)");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 5);
}

TEST_F(QA_GDB837, NotInSingleRowSubquery) {
    exec_ok("CREATE TABLE sr_outer2 (id INT)");
    exec_ok("INSERT INTO sr_outer2 VALUES (5)");
    exec_ok("INSERT INTO sr_outer2 VALUES (6)");

    exec_ok("CREATE TABLE sr_inner2 (v INT)");
    exec_ok("INSERT INTO sr_inner2 VALUES (5)");

    auto qr = exec_ok("SELECT id FROM sr_outer2 WHERE id NOT IN (SELECT v FROM sr_inner2)");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 6);
}

// =============================================================================
// 5. Large subquery set (stress — membership must still be correct)
// =============================================================================

TEST_F(QA_GDB837, InLargeSubquerySet) {
    exec_ok("CREATE TABLE large_outer (id INT)");
    // outer: 1..20
    for (int i = 1; i <= 20; ++i) {
        exec_ok("INSERT INTO large_outer VALUES (" + std::to_string(i) + ")");
    }

    exec_ok("CREATE TABLE large_inner (v INT)");
    // inner: even numbers 2..40 (200 rows)
    for (int i = 1; i <= 100; ++i) {
        exec_ok("INSERT INTO large_inner VALUES (" + std::to_string(i * 2) + ")");
    }

    // Outer rows 2,4,6,8,10,12,14,16,18,20 should match.
    auto qr = exec_ok("SELECT id FROM large_outer WHERE id IN (SELECT v FROM large_inner)");
    ASSERT_EQ(qr.rows.size(), 10u) << "Large subquery: exactly 10 even outer rows expected";
    auto ids = col0_sorted(qr);
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(ids[i], (i + 1) * 2);
    }
}

// =============================================================================
// 6. Subquery with WHERE / ORDER BY / LIMIT
// =============================================================================

/// Subquery with its own WHERE clause — only filtered rows form the set.
TEST_F(QA_GDB837, InSubqueryWithWhereClause) {
    exec_ok("CREATE TABLE filtered_outer (id INT)");
    exec_ok("INSERT INTO filtered_outer VALUES (1)");
    exec_ok("INSERT INTO filtered_outer VALUES (2)");
    exec_ok("INSERT INTO filtered_outer VALUES (3)");

    exec_ok("CREATE TABLE filtered_inner (v INT, active INT)");
    exec_ok("INSERT INTO filtered_inner VALUES (1, 0)");  // inactive
    exec_ok("INSERT INTO filtered_inner VALUES (2, 1)");  // active
    exec_ok("INSERT INTO filtered_inner VALUES (3, 0)");  // inactive

    // Subquery: SELECT v FROM filtered_inner WHERE active = 1 → {2}
    auto qr = exec_ok(
        "SELECT id FROM filtered_outer "
        "WHERE id IN (SELECT v FROM filtered_inner WHERE active = 1)");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 2);
}

/// Subquery with ORDER BY — ordering must not affect membership semantics.
TEST_F(QA_GDB837, InSubqueryWithOrderBy) {
    exec_ok("CREATE TABLE ord_outer (id INT)");
    exec_ok("INSERT INTO ord_outer VALUES (1)");
    exec_ok("INSERT INTO ord_outer VALUES (2)");
    exec_ok("INSERT INTO ord_outer VALUES (3)");

    exec_ok("CREATE TABLE ord_inner (v INT)");
    exec_ok("INSERT INTO ord_inner VALUES (3)");
    exec_ok("INSERT INTO ord_inner VALUES (1)");
    exec_ok("INSERT INTO ord_inner VALUES (2)");

    auto qr = exec_ok(
        "SELECT id FROM ord_outer "
        "WHERE id IN (SELECT v FROM ord_inner ORDER BY v DESC)");
    ASSERT_EQ(qr.rows.size(), 3u) << "ORDER BY in subquery must not affect IN membership";
}

/// Subquery with LIMIT — only the limited rows form the set.
TEST_F(QA_GDB837, InSubqueryWithLimit) {
    exec_ok("CREATE TABLE lim_outer (id INT)");
    exec_ok("INSERT INTO lim_outer VALUES (1)");
    exec_ok("INSERT INTO lim_outer VALUES (2)");
    exec_ok("INSERT INTO lim_outer VALUES (3)");

    exec_ok("CREATE TABLE lim_inner (v INT)");
    exec_ok("INSERT INTO lim_inner VALUES (1)");
    exec_ok("INSERT INTO lim_inner VALUES (2)");
    exec_ok("INSERT INTO lim_inner VALUES (3)");

    // LIMIT 2 on inner — only 2 values in the set; at most 2 outer rows match.
    auto qr = exec_ok(
        "SELECT id FROM lim_outer "
        "WHERE id IN (SELECT v FROM lim_inner ORDER BY v ASC LIMIT 2)");
    // Only ids 1 and 2 are in the limited set.
    ASSERT_EQ(qr.rows.size(), 2u) << "LIMIT in subquery must restrict the membership set";
    auto ids = col0_sorted(qr);
    EXPECT_EQ(ids[0], 1);
    EXPECT_EQ(ids[1], 2);
}

// =============================================================================
// 7. Subquery referencing a non-existent column — clean error, no crash
// =============================================================================

TEST_F(QA_GDB837, InSubqueryBadColumnReturnsError) {
    exec_ok("CREATE TABLE err_outer (id INT)");
    exec_ok("INSERT INTO err_outer VALUES (1)");

    exec_ok("CREATE TABLE err_inner (v INT)");
    exec_ok("INSERT INTO err_inner VALUES (1)");

    // "nonexistent_col" does not exist in err_inner — should fail cleanly.
    exec_error("SELECT id FROM err_outer WHERE id IN (SELECT nonexistent_col FROM err_inner)",
               StatusCode::NOT_FOUND);
}

// =============================================================================
// 8. Multi-column subquery — INVALID_ARGUMENT, no crash or partial state
//    (Re-verified here so QA owns a copy separate from the dev test.)
// =============================================================================

TEST_F(QA_GDB837, MultiColumnSubqueryErrorNoPartialState) {
    exec_ok("CREATE TABLE mc_outer (x INT)");
    exec_ok("INSERT INTO mc_outer VALUES (1)");
    exec_ok("INSERT INTO mc_outer VALUES (2)");

    exec_ok("CREATE TABLE mc_inner (a INT, b INT)");
    exec_ok("INSERT INTO mc_inner VALUES (1, 10)");

    exec_error("SELECT x FROM mc_outer WHERE x IN (SELECT a, b FROM mc_inner)",
               StatusCode::INVALID_ARGUMENT);

    // After the error, the engine must still be usable (no corrupted state).
    auto qr = exec_ok("SELECT x FROM mc_outer");
    EXPECT_EQ(qr.rows.size(), 2u) << "Engine must remain functional after INVALID_ARGUMENT error";
}

// =============================================================================
// 9. Zero-column subquery — should fail cleanly (not crash)
// =============================================================================

TEST_F(QA_GDB837, ZeroColumnSubqueryError) {
    exec_ok("CREATE TABLE zc_outer (id INT)");
    exec_ok("INSERT INTO zc_outer VALUES (1)");

    // "SELECT" with no columns is a parse error; this tests graceful handling.
    // We expect either PARSE_ERROR or INVALID_ARGUMENT — not a crash.
    auto r = engine_->execute("SELECT id FROM zc_outer WHERE id IN (SELECT FROM zc_outer)");
    if (r.has_value()) {
        // If the parser accepts this somehow, the row count should still be sane.
        // No assertion on count, just checking no crash.
    } else {
        // Any error code is acceptable — just not a crash or INTERNAL_ERROR.
        EXPECT_NE(r.error().code, StatusCode::INTERNAL_ERROR)
            << "Zero-column subquery must not cause INTERNAL_ERROR";
    }
}

// =============================================================================
// 10. Nested IN — subquery contains its own IN subquery
// =============================================================================

TEST_F(QA_GDB837, NestedInSubquery) {
    exec_ok("CREATE TABLE level0 (id INT)");
    exec_ok("INSERT INTO level0 VALUES (1)");
    exec_ok("INSERT INTO level0 VALUES (2)");
    exec_ok("INSERT INTO level0 VALUES (3)");

    exec_ok("CREATE TABLE level1 (v INT)");
    exec_ok("INSERT INTO level1 VALUES (2)");
    exec_ok("INSERT INTO level1 VALUES (3)");
    exec_ok("INSERT INTO level1 VALUES (4)");

    exec_ok("CREATE TABLE level2 (w INT)");
    exec_ok("INSERT INTO level2 VALUES (3)");
    exec_ok("INSERT INTO level2 VALUES (5)");

    // outer IN (inner IN (innermost)):
    //   level1 WHERE v IN (SELECT w FROM level2) → {3}
    //   level0 WHERE id IN ({3}) → {3}
    auto qr = exec_ok(
        "SELECT id FROM level0 "
        "WHERE id IN ("
        "  SELECT v FROM level1 WHERE v IN (SELECT w FROM level2)"
        ")");
    // Should return exactly id=3.
    ASSERT_EQ(qr.rows.size(), 1u) << "Nested IN subquery must evaluate correctly";
    EXPECT_EQ(qr.rows[0][0].as_int32(), 3);
}

// =============================================================================
// 11. Correlated subquery probe
//     WHERE x IN (SELECT y FROM t2 WHERE t2.z = outer.col)
//     Must either error cleanly OR return correct results.
//     Returning WRONG results (not an error) is a bug.
// =============================================================================

TEST_F(QA_GDB837, CorrelatedInSubquery_DocumentedBehavior) {
    exec_ok("CREATE TABLE corr_outer (id INT, cat INT)");
    exec_ok("INSERT INTO corr_outer VALUES (1, 10)");
    exec_ok("INSERT INTO corr_outer VALUES (2, 20)");
    exec_ok("INSERT INTO corr_outer VALUES (3, 10)");

    exec_ok("CREATE TABLE corr_inner (val INT, cat INT)");
    exec_ok("INSERT INTO corr_inner VALUES (1, 10)");
    exec_ok("INSERT INTO corr_inner VALUES (2, 20)");
    exec_ok("INSERT INTO corr_inner VALUES (4, 10)");

    // Correlated: WHERE id IN (SELECT val FROM corr_inner WHERE corr_inner.cat = corr_outer.cat)
    // Semantically correct result:
    //   id=1, cat=10 → inner WHERE cat=10 → {1,4} → 1 IN {1,4} → TRUE → pass
    //   id=2, cat=20 → inner WHERE cat=20 → {2}   → 2 IN {2}   → TRUE → pass
    //   id=3, cat=10 → inner WHERE cat=10 → {1,4} → 3 IN {1,4} → FALSE → filtered
    // Correct: 2 rows {1, 2}
    auto r = engine_->execute(
        "SELECT id FROM corr_outer "
        "WHERE id IN (SELECT val FROM corr_inner WHERE corr_inner.cat = corr_outer.cat)");

    if (!r.has_value()) {
        // Error on correlated subquery is acceptable (NOT_IMPLEMENTED or similar).
        // Only INTERNAL_ERROR or a crash would be a bug.
        EXPECT_NE(r.error().code, StatusCode::INTERNAL_ERROR)
            << "Correlated IN subquery must not produce INTERNAL_ERROR";
        // Log the actual error for the QA report.
        GTEST_SKIP() << "Correlated IN subquery returned error (acceptable): "
                     << r.error().message;
    }

    // If it returned a result, verify it's not silently wrong.
    // Correct answer is 2 rows {1, 2}.
    auto& qr = *r;
    auto ids = col0_sorted(qr);

    // Any of these outcomes is a correctness trap if wrong:
    // - 3 rows (ignoring the filter) → wrong
    // - 0 rows (over-filtering) → wrong
    // - 2 rows but wrong IDs → wrong
    ASSERT_EQ(qr.rows.size(), 2u)
        << "Correlated IN subquery returned wrong row count; "
           "if correlation is unsupported this must be an error, not wrong results";
    EXPECT_EQ(ids[0], 1);
    EXPECT_EQ(ids[1], 2);
}

// =============================================================================
// 12. NOT IN with NULL in subquery — mixed null/non-null, detailed 3VL check
// =============================================================================

/// NOT IN where subquery has {5, NULL}:
///   5 NOT IN (5, NULL) → FALSE → filtered
///   10 NOT IN (5, NULL) → UNKNOWN → filtered
///   99 NOT IN (5, NULL) → UNKNOWN → filtered
/// Result: 0 rows.
TEST_F(QA_GDB837, NotInMixedNullSubquery_Detailed3VL) {
    exec_ok("CREATE TABLE three_vl_outer (id INT)");
    exec_ok("INSERT INTO three_vl_outer VALUES (5)");
    exec_ok("INSERT INTO three_vl_outer VALUES (10)");
    exec_ok("INSERT INTO three_vl_outer VALUES (99)");

    exec_ok("CREATE TABLE three_vl_inner (v INT)");
    exec_ok("INSERT INTO three_vl_inner VALUES (5)");
    exec_ok("INSERT INTO three_vl_inner (v) VALUES (NULL)");

    auto qr = exec_ok(
        "SELECT id FROM three_vl_outer WHERE id NOT IN (SELECT v FROM three_vl_inner)");
    EXPECT_EQ(qr.rows.size(), 0u)
        << "NOT IN with NULL in subquery: 5→FALSE, others→UNKNOWN, result must be 0 rows";
}

/// IN where subquery has {5, NULL}:
///   5 IN (5, NULL) → TRUE → passes
///   10 IN (5, NULL) → UNKNOWN → filtered
///   99 IN (5, NULL) → UNKNOWN → filtered
/// Result: 1 row {5}.
TEST_F(QA_GDB837, InMixedNullSubquery_Detailed3VL) {
    exec_ok("CREATE TABLE in3vl_outer (id INT)");
    exec_ok("INSERT INTO in3vl_outer VALUES (5)");
    exec_ok("INSERT INTO in3vl_outer VALUES (10)");
    exec_ok("INSERT INTO in3vl_outer VALUES (99)");

    exec_ok("CREATE TABLE in3vl_inner (v INT)");
    exec_ok("INSERT INTO in3vl_inner VALUES (5)");
    exec_ok("INSERT INTO in3vl_inner (v) VALUES (NULL)");

    auto qr = exec_ok(
        "SELECT id FROM in3vl_outer WHERE id IN (SELECT v FROM in3vl_inner)");
    ASSERT_EQ(qr.rows.size(), 1u)
        << "IN with NULL in subquery: only the TRUE case (5) should pass";
    EXPECT_EQ(qr.rows[0][0].as_int32(), 5);
}

// =============================================================================
// 13. NOT IN — outer row that MATCHES the set is excluded; others included
//     Extra check: single match in larger outer.
// =============================================================================

TEST_F(QA_GDB837, NotInPreciseBoundary_ExactMatchExcluded) {
    exec_ok("CREATE TABLE boundary_outer (id INT)");
    for (int i = 1; i <= 10; ++i) {
        exec_ok("INSERT INTO boundary_outer VALUES (" + std::to_string(i) + ")");
    }

    exec_ok("CREATE TABLE boundary_inner (v INT)");
    exec_ok("INSERT INTO boundary_inner VALUES (5)");  // only id=5 should be excluded

    auto qr = exec_ok(
        "SELECT id FROM boundary_outer WHERE id NOT IN (SELECT v FROM boundary_inner)");
    ASSERT_EQ(qr.rows.size(), 9u) << "NOT IN: exactly one row (id=5) must be excluded";
    auto ids = col0_sorted(qr);
    for (int i = 0; i < 9; ++i) {
        int expected = (i < 4) ? i + 1 : i + 2; // skip 5
        EXPECT_EQ(ids[i], expected);
    }
}

// =============================================================================
// 14. IN subquery where outer table is empty → 0 rows (no crash)
// =============================================================================

TEST_F(QA_GDB837, InSubqueryEmptyOuterTable) {
    exec_ok("CREATE TABLE empty_outer (id INT)");
    // no rows

    exec_ok("CREATE TABLE nonempty_inner (v INT)");
    exec_ok("INSERT INTO nonempty_inner VALUES (1)");
    exec_ok("INSERT INTO nonempty_inner VALUES (2)");

    auto qr = exec_ok("SELECT id FROM empty_outer WHERE id IN (SELECT v FROM nonempty_inner)");
    EXPECT_EQ(qr.rows.size(), 0u) << "Empty outer table must yield 0 rows";
}

// =============================================================================
// 15. NOT IN subquery where outer table is empty → 0 rows (no crash)
// =============================================================================

TEST_F(QA_GDB837, NotInSubqueryEmptyOuterTable) {
    exec_ok("CREATE TABLE empty_outer2 (id INT)");

    exec_ok("CREATE TABLE some_inner (v INT)");
    exec_ok("INSERT INTO some_inner VALUES (42)");

    auto qr = exec_ok("SELECT id FROM empty_outer2 WHERE id NOT IN (SELECT v FROM some_inner)");
    EXPECT_EQ(qr.rows.size(), 0u) << "Empty outer + NOT IN must still yield 0 rows";
}
