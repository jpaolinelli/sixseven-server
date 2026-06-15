/// QA adversarial tests for GDB-801: Verify SelectNegativeInteger correction
/// after GDB-661 fixed the unary-minus fast path in SELECT-without-FROM.
///
/// Attack surface:
///   - SELECT -0, SELECT -(literal), double-negation
///   - Negative integers in WHERE predicates
///   - Negative integers in arithmetic expressions
///   - INT32 boundary values (INT32_MIN negate overflow)
///   - Large negative literals
///   - Negative float literals
///   - Negative in column type assertions
///   - Negative NULL propagation (NEGATE NULL)

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/logging.h"
#include "sixseven/common/result.h"
#include "sixseven/executor/pg_catalog_tables.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <climits>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace sixseven;

// =============================================================================
// Fixture
// =============================================================================

class QA_GDB801 : public ::testing::Test {
protected:
    void SetUp() override {
        auto r = catalog_.restore_database(default_database_id, "demo");
        (void)r;
        register_pg_catalog_tables(catalog_);
        data_dir_ = fs::temp_directory_path() / "sixseven_qa_gdb801";
        fs::remove_all(data_dir_);
        fs::create_directories(data_dir_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        fs::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << "SQL: " << sql << "\n  " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "SQL should have failed: " << sql;
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected);
        }
    }

    DiskManager dm_;
    Catalog catalog_;
    fs::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// 1. Core regression: SELECT -5 passes and returns correct value/type/name
// =============================================================================

TEST_F(QA_GDB801, SelectNegativeFiveValue) {
    auto qr = exec_ok("SELECT -5 AS neg");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), -5);
}

TEST_F(QA_GDB801, SelectNegativeFiveColumnType) {
    auto qr = exec_ok("SELECT -5 AS neg");
    ASSERT_EQ(qr.column_types.size(), 1u);
    EXPECT_EQ(qr.column_types[0], TypeId::INT32);
}

TEST_F(QA_GDB801, SelectNegativeFiveColumnName) {
    auto qr = exec_ok("SELECT -5 AS neg");
    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "neg");
}

TEST_F(QA_GDB801, SelectNegativeFiveRowCount) {
    auto qr = exec_ok("SELECT -5 AS neg");
    EXPECT_EQ(qr.rows.size(), 1u);
}

// =============================================================================
// 2. SELECT -0 — negative zero edge case
// =============================================================================

TEST_F(QA_GDB801, SelectNegativeZeroInteger) {
    // -0 in integer context should equal 0.
    auto qr = exec_ok("SELECT -0 AS nz");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 0);
}

TEST_F(QA_GDB801, SelectNegativeZeroFloat) {
    // -0.0 in float context: IEEE 754 negative zero; value comparison should be 0.
    auto qr = exec_ok("SELECT -0.0 AS nzf");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_NEAR(qr.rows[0][0].as_float64(), 0.0, 1e-15);
}

// =============================================================================
// 3. Double negation: SELECT --5
// Known limitation: consecutive unary minus signs are rejected by the parser
// with "expected expression ... got END_OF_FILE". This is a separate defect
// from the GDB-661 fix (which only addressed single unary minus). These tests
// document the current behavior as a regression guard.
// =============================================================================

TEST_F(QA_GDB801, SelectDoubleNegationParserRejects) {
    // BUG (Medium): Parser rejects --5 with a parse error. Standard SQL allows
    // double negation. Tracked as a known limitation post GDB-661; do NOT
    // revert GDB-661 — the single-minus path is correct.
    auto result = engine_->execute("SELECT --5 AS pos");
    // Document current behavior: parser rejects consecutive unary minus.
    // When this is fixed, update this test to assert success (value == 5).
    EXPECT_FALSE(result.has_value())
        << "Parser currently rejects '--5'. If this now passes, update the test "
           "to assert as_int32() == 5 and close the tracked bug.";
}

TEST_F(QA_GDB801, SelectTripleNegationParserRejects) {
    // BUG (Medium): Same parser limitation with three consecutive minuses.
    auto result = engine_->execute("SELECT ---5 AS neg");
    EXPECT_FALSE(result.has_value())
        << "Parser currently rejects '---5'. If this now passes, update the "
           "test to assert as_int32() == -5 and close the tracked bug.";
}

// =============================================================================
// 4. Large negative integer literals
// =============================================================================

TEST_F(QA_GDB801, SelectLargeNegativeInteger) {
    auto qr = exec_ok("SELECT -1000000 AS big_neg");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), -1000000);
}

TEST_F(QA_GDB801, SelectNegativeOne) {
    auto qr = exec_ok("SELECT -1 AS n");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), -1);
}

TEST_F(QA_GDB801, SelectNegativeTwoBillion) {
    // -2147483647 is INT32_MIN+1, stays representable after negation.
    auto qr = exec_ok("SELECT -2147483647 AS v");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), -2147483647);
}

// =============================================================================
// 5. Negative float literals
// =============================================================================

TEST_F(QA_GDB801, SelectNegativeFloat) {
    auto qr = exec_ok("SELECT -3.14 AS nf");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_NEAR(qr.rows[0][0].as_float64(), -3.14, 0.0001);
}

TEST_F(QA_GDB801, SelectNegativeFloatType) {
    auto qr = exec_ok("SELECT -1.5 AS nf");
    ASSERT_EQ(qr.column_types.size(), 1u);
    EXPECT_EQ(qr.column_types[0], TypeId::FLOAT64);
}

TEST_F(QA_GDB801, SelectNegativeFloatDoubleNegationParserRejects) {
    // BUG (Medium): Same consecutive-unary-minus parser limitation for floats.
    auto result = engine_->execute("SELECT --2.5 AS pf");
    EXPECT_FALSE(result.has_value())
        << "Parser currently rejects '--2.5'. If this now passes, update the "
           "test to assert as_float64() ~= 2.5 and close the tracked bug.";
}

// =============================================================================
// 6. Negative integers without alias (column name inference)
// =============================================================================

TEST_F(QA_GDB801, SelectNegativeNoAlias) {
    // Column name should reflect the expression, not crash.
    auto qr = exec_ok("SELECT -5");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), -5);
    // Column name should be something sensible (not empty).
    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_FALSE(qr.column_names[0].empty());
}

// =============================================================================
// 7. Multiple negative literals in a single SELECT
// =============================================================================

TEST_F(QA_GDB801, SelectMultipleNegatives) {
    auto qr = exec_ok("SELECT -1 AS a, -2 AS b, -3 AS c");
    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.rows[0].size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), -1);
    EXPECT_EQ(qr.rows[0][1].as_int32(), -2);
    EXPECT_EQ(qr.rows[0][2].as_int32(), -3);
}

TEST_F(QA_GDB801, SelectMixedPositiveAndNegative) {
    auto qr = exec_ok("SELECT 10 AS pos, -10 AS neg");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 10);
    EXPECT_EQ(qr.rows[0][1].as_int32(), -10);
}

// =============================================================================
// 8. Negative integers in WHERE predicates against table rows
// =============================================================================

TEST_F(QA_GDB801, NegativeInWhereEquals) {
    exec_ok("CREATE TABLE neg_test (val INT)");
    exec_ok("INSERT INTO neg_test VALUES (-5)");
    exec_ok("INSERT INTO neg_test VALUES (5)");
    exec_ok("INSERT INTO neg_test VALUES (-10)");
    auto qr = exec_ok("SELECT val FROM neg_test WHERE val = -5");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), -5);
}

TEST_F(QA_GDB801, NegativeInWhereLessThan) {
    exec_ok("CREATE TABLE neg_test2 (val INT)");
    exec_ok("INSERT INTO neg_test2 VALUES (-1)");
    exec_ok("INSERT INTO neg_test2 VALUES (0)");
    exec_ok("INSERT INTO neg_test2 VALUES (1)");
    auto qr = exec_ok("SELECT val FROM neg_test2 WHERE val < 0");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), -1);
}

TEST_F(QA_GDB801, NegativeInWhereGreaterThan) {
    exec_ok("CREATE TABLE neg_test3 (val INT)");
    exec_ok("INSERT INTO neg_test3 VALUES (-5)");
    exec_ok("INSERT INTO neg_test3 VALUES (-3)");
    exec_ok("INSERT INTO neg_test3 VALUES (-1)");
    // Select values > -3 (i.e., -1)
    auto qr = exec_ok("SELECT val FROM neg_test3 WHERE val > -3");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), -1);
}

TEST_F(QA_GDB801, NegativeInWhereNoMatch) {
    exec_ok("CREATE TABLE neg_test4 (val INT)");
    exec_ok("INSERT INTO neg_test4 VALUES (1)");
    exec_ok("INSERT INTO neg_test4 VALUES (2)");
    auto qr = exec_ok("SELECT val FROM neg_test4 WHERE val = -99");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// 9. Negative NULL propagation in const-fold path
// =============================================================================

TEST_F(QA_GDB801, SelectNegateNull) {
    // -NULL should remain NULL.
    auto qr = exec_ok("SELECT -NULL AS negated_null");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_TRUE(qr.rows[0][0].is_null());
}

// =============================================================================
// 10. Negative integer column type precision
// =============================================================================

TEST_F(QA_GDB801, SelectNegativeIntColumnType) {
    auto qr = exec_ok("SELECT -42 AS v");
    ASSERT_EQ(qr.column_types.size(), 1u);
    EXPECT_EQ(qr.column_types[0], TypeId::INT32);
}

// =============================================================================
// 11. Stress: rapid successive SELECT with negative literals
// =============================================================================

TEST_F(QA_GDB801, StressNegativeLiterals) {
    for (int i = 0; i < 100; ++i) {
        auto sql = "SELECT -" + std::to_string(i) + " AS v";
        auto qr = exec_ok(sql);
        ASSERT_EQ(qr.rows.size(), 1u) << "failed on i=" << i;
        EXPECT_EQ(qr.rows[0][0].as_int32(), -i) << "wrong value for i=" << i;
    }
}

// =============================================================================
// 12. Unary minus on non-numeric should error (type safety)
// =============================================================================

TEST_F(QA_GDB801, SelectNegateString) {
    // -'hello' is not valid; should return an error, not crash.
    auto result = engine_->execute("SELECT -'hello' AS bad");
    // Either parse error or type error is acceptable; must not succeed with garbage.
    if (result.has_value()) {
        // If it somehow succeeded, values should not be silently coerced.
        // This is a finding if the value is anything meaningful.
        FAIL() << "Expected failure when negating a string literal, got success";
    }
}

TEST_F(QA_GDB801, SelectNegateBool) {
    // -TRUE is not valid arithmetic negation.
    auto result = engine_->execute("SELECT -TRUE AS bad");
    EXPECT_FALSE(result.has_value())
        << "Expected error when negating a boolean literal";
}
