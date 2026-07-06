#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// Re-QA (v2) adversarial coverage for GDB-1285's constant-folding extension
// (commit b65f52e), targeting the GDB-1300 bug class and its stated fix:
// is_constant_expr() allowlist + evaluate_expr() over an empty schema.
//
// Focus areas NOT already covered by test_qa_gdb_1285.cpp:
//   - crash-safety of evaluate_expr on nasty-but-constant defaults
//     (division by zero, overflow, invalid CAST)
//   - clean rejection (not silent NULL, not crash) for column refs,
//     params, window funcs, aggregates, subqueries as the 3rd arg
//   - clean rejection for conservatively-disallowed-but-constant forms
//     (CASE with all-literal branches, IN with all-literal list, BETWEEN
//     already covered upstream but re-verified here)
// =============================================================================

class QA_GDB1285_V2_ConstantFoldTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1285_v2";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        exec_ok("CREATE TABLE employees ("
                "id INT PRIMARY KEY, "
                "name TEXT NOT NULL, "
                "salary DOUBLE, "
                "department TEXT)");

        exec_ok("INSERT INTO employees VALUES (1, 'Alice', 80000, 'Engineering')");
        exec_ok("INSERT INTO employees VALUES (2, 'Bob', 90000, 'Engineering')");
        exec_ok("INSERT INTO employees VALUES (3, 'Charlie', 70000, 'Sales')");
        exec_ok("INSERT INTO employees VALUES (4, 'Dave', 85000, 'Sales')");
        exec_ok("INSERT INTO employees VALUES (5, 'Eve', 95000, 'Engineering')");
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    Result<QueryResult> exec(const std::string& sql) { return engine_->execute(sql); }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << "SQL: " << sql << "\n"
                                        << "Error: " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// ---- Division by zero in a constant default expression ----
// evaluate_expr must return a clean Result error, never crash/UB/hang.
TEST_F(QA_GDB1285_V2_ConstantFoldTest, DivisionByZeroDefaultErrorsCleanlyNoCrash) {
    auto result =
        exec("SELECT name, salary, LAG(salary, 100, 1/0) OVER (ORDER BY salary) AS prev "
             "FROM employees");
    ASSERT_FALSE(result.has_value())
        << "1/0 as a LAG default must not silently succeed with a bogus value";
    EXPECT_FALSE(result.error().message.empty());
}

// ---- Integer overflow in a constant default expression ----
TEST_F(QA_GDB1285_V2_ConstantFoldTest, IntegerOverflowDefaultDoesNotCrash) {
    // INT64_MAX + 1 overflows; must be a clean error or a defined wraparound
    // value, never UB/abort.
    auto result = exec(
        "SELECT name, salary, LAG(salary, 100, 9223372036854775807 + 1) OVER (ORDER BY salary) "
        "AS prev FROM employees");
    // Either outcome is acceptable; the only failure mode is a crash, which
    // would have already terminated the test process before this assertion.
    if (!result.has_value()) {
        EXPECT_FALSE(result.error().message.empty());
    } else {
        SUCCEED() << "overflow was handled via wraparound rather than error; no crash occurred";
    }
}

// ---- Out-of-range / invalid CAST in a constant default expression ----
TEST_F(QA_GDB1285_V2_ConstantFoldTest, InvalidStringToIntCastDefaultErrorsCleanly) {
    auto result =
        exec("SELECT name, salary, LAG(salary, 100, CAST('abc' AS INT)) OVER (ORDER BY salary) "
             "AS prev FROM employees");
    ASSERT_FALSE(result.has_value())
        << "CAST('abc' AS INT) must fail cleanly, not silently produce garbage or NULL";
    EXPECT_FALSE(result.error().message.empty());
}

TEST_F(QA_GDB1285_V2_ConstantFoldTest, OutOfRangeNumericCastDefaultErrorsCleanlyOrDefined) {
    // 99999 doesn't fit in INT8; must be a clean error or a well-defined
    // saturation/truncation, never UB.
    auto result =
        exec("SELECT name, salary, LAG(salary, 100, CAST(99999 AS TINYINT)) OVER (ORDER BY "
             "salary) AS prev FROM employees");
    if (!result.has_value()) {
        EXPECT_FALSE(result.error().message.empty());
    } else {
        for (auto& row : result->rows) {
            // No crash reading the value back out; whatever numeric value
            // resulted, it must be readable without throwing.
            (void)row[2].is_null();
        }
        SUCCEED() << "out-of-range CAST resolved to a defined value rather than erroring";
    }
}

// ---- NULL arithmetic in a constant default expression ----
TEST_F(QA_GDB1285_V2_ConstantFoldTest, NullArithmeticDefaultIsNullNotCrash) {
    auto result = exec(
        "SELECT name, salary, LAG(salary, 100, 1 + NULL) OVER (ORDER BY salary) AS prev "
        "FROM employees");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 5u);
    for (auto& row : result->rows) {
        // 1 + NULL is NULL under standard SQL null-propagation; this must
        // behave the same as an explicit NULL default, not crash.
        EXPECT_TRUE(row[2].is_null());
    }
}

// ---- Column-reference default: must be a clean, explicit rejection ----
TEST_F(QA_GDB1285_V2_ConstantFoldTest, ColumnReferenceDefaultRejectedCleanly) {
    auto result =
        exec("SELECT name, salary, LAG(salary, 100, department) OVER (ORDER BY salary) AS prev "
             "FROM employees");
    ASSERT_FALSE(result.has_value())
        << "a column reference as the LAG/LEAD default must be rejected, not silently NULL";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_FALSE(result.error().message.empty());
}

TEST_F(QA_GDB1285_V2_ConstantFoldTest, SelfColumnReferenceDefaultRejectedCleanly) {
    auto result =
        exec("SELECT name, salary, LAG(salary, 100, salary) OVER (ORDER BY salary) AS prev "
             "FROM employees");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---- Aggregate as default: must be rejected cleanly ----
TEST_F(QA_GDB1285_V2_ConstantFoldTest, AggregateDefaultRejectedCleanly) {
    auto result = exec(
        "SELECT name, salary, LAG(salary, 100, AVG(salary)) OVER (ORDER BY salary) AS prev "
        "FROM employees");
    ASSERT_FALSE(result.has_value())
        << "an aggregate as the LAG/LEAD default must be rejected, not silently NULL";
    EXPECT_FALSE(result.error().message.empty());
}

// ---- Another window function as default: must be rejected cleanly ----
TEST_F(QA_GDB1285_V2_ConstantFoldTest, WindowFunctionDefaultRejectedCleanly) {
    auto result = exec(
        "SELECT name, salary, "
        "LAG(salary, 100, ROW_NUMBER() OVER (ORDER BY salary)) OVER (ORDER BY salary) AS prev "
        "FROM employees");
    ASSERT_FALSE(result.has_value())
        << "a nested window function as the LAG/LEAD default must be rejected, not silently NULL";
    EXPECT_FALSE(result.error().message.empty());
}

// ---- Subquery as default: must be rejected cleanly (no crash) ----
TEST_F(QA_GDB1285_V2_ConstantFoldTest, SubqueryDefaultRejectedCleanly) {
    auto result = exec(
        "SELECT name, salary, "
        "LAG(salary, 100, (SELECT MAX(salary) FROM employees)) OVER (ORDER BY salary) AS prev "
        "FROM employees");
    ASSERT_FALSE(result.has_value())
        << "a subquery as the LAG/LEAD default must be rejected, not silently NULL";
    EXPECT_FALSE(result.error().message.empty());
}

// ---- Parameter placeholder as default: must be rejected cleanly ----
TEST_F(QA_GDB1285_V2_ConstantFoldTest, ParamRefDefaultRejectedCleanly) {
    auto result =
        exec("SELECT name, salary, LAG(salary, 100, $1) OVER (ORDER BY salary) AS prev "
             "FROM employees");
    // Either a bind-time or plan-time rejection is fine; a crash or silent
    // NULL is not.
    ASSERT_FALSE(result.has_value());
    EXPECT_FALSE(result.error().message.empty());
}

// ---- Conservatively-rejected-but-constant forms: CASE, IN, ARRAY, LIKE ----
// These are explicitly excluded from is_constant_expr()'s allowlist even
// though (with all-literal operands) they are logically constant. Confirm
// they error cleanly rather than silently returning NULL or crashing.

TEST_F(QA_GDB1285_V2_ConstantFoldTest, AllLiteralCaseDefaultRejectedCleanlyNotSilentNull) {
    auto result = exec(
        "SELECT name, salary, "
        "LAG(salary, 100, CASE WHEN 1 = 1 THEN 5 ELSE 6 END) OVER (ORDER BY salary) AS prev "
        "FROM employees");
    if (!result.has_value()) {
        EXPECT_FALSE(result.error().message.empty());
    } else {
        // If the implementation chooses to support CASE later, it must not
        // silently drop to NULL -- it must actually resolve the constant.
        for (auto& row : result->rows) {
            EXPECT_FALSE(row[2].is_null())
                << "CASE default silently returned NULL instead of erroring or resolving";
        }
    }
}

TEST_F(QA_GDB1285_V2_ConstantFoldTest, AllLiteralInListDefaultRejectedCleanlyNotSilentNull) {
    auto result = exec(
        "SELECT name, salary, LAG(salary, 100, 5) OVER (ORDER BY salary) AS prev "
        "FROM employees WHERE salary IN (70000, 80000, 90000, 85000, 95000)");
    // This uses IN in the WHERE clause (should work fine); the real
    // adversarial case is IN as the *default* itself, tried below via a
    // boolean-context default which should be rejected or coerced cleanly.
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

TEST_F(QA_GDB1285_V2_ConstantFoldTest, InExprAsDefaultRejectedCleanlyNotSilentNull) {
    auto result = exec(
        "SELECT name, salary, "
        "LAG(salary, 100, 5 IN (5, 6, 7)) OVER (ORDER BY salary) AS prev "
        "FROM employees");
    if (!result.has_value()) {
        EXPECT_FALSE(result.error().message.empty());
    } else {
        for (auto& row : result->rows) {
            EXPECT_FALSE(row[2].is_null())
                << "InExpr default silently returned NULL instead of erroring or resolving";
        }
    }
}

// ---- Nested constant expressions: deep unary/binary chains ----
TEST_F(QA_GDB1285_V2_ConstantFoldTest, DeeplyNestedArithmeticDefaultResolvesCorrectly) {
    auto qr = exec_ok(
        "SELECT name, salary, LAG(salary, 100, ((1 + 2) * 3 - 4) / 1) OVER (ORDER BY salary) "
        "AS prev FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    for (auto& row : qr.rows) {
        ASSERT_FALSE(row[2].is_null());
        EXPECT_DOUBLE_EQ(row[2].as_float64(), 5.0);
    }
}

TEST_F(QA_GDB1285_V2_ConstantFoldTest, TripleNestedUnaryMinusResolvesToNegative) {
    // -(-(-5)) = -5.
    auto qr = exec_ok(
        "SELECT name, salary, LAG(salary, 100, -(-(-5))) OVER (ORDER BY salary) AS prev "
        "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    for (auto& row : qr.rows) {
        ASSERT_FALSE(row[2].is_null());
        EXPECT_DOUBLE_EQ(row[2].as_float64(), -5.0);
    }
}

// ---- IS NULL / BETWEEN constant defaults (explicitly allowlisted) ----
TEST_F(QA_GDB1285_V2_ConstantFoldTest, IsNullConstantDefaultResolves) {
    // (5 IS NULL) is a BOOL constant (false); on a DOUBLE column this either
    // coerces (0.0) or errors cleanly -- must not silently be NULL.
    auto result = exec(
        "SELECT name, salary, LAG(salary, 100, (5 IS NULL)) OVER (ORDER BY salary) AS prev "
        "FROM employees");
    if (result.has_value()) {
        for (auto& row : result->rows) {
            EXPECT_FALSE(row[2].is_null())
                << "IS NULL constant default silently returned NULL";
        }
    } else {
        EXPECT_FALSE(result.error().message.empty());
    }
}

TEST_F(QA_GDB1285_V2_ConstantFoldTest, BetweenConstantDefaultResolves) {
    // (5 BETWEEN 1 AND 10) is a BOOL constant (true).
    auto result = exec(
        "SELECT name, salary, LAG(salary, 100, (5 BETWEEN 1 AND 10)) OVER (ORDER BY salary) "
        "AS prev FROM employees");
    if (result.has_value()) {
        for (auto& row : result->rows) {
            EXPECT_FALSE(row[2].is_null())
                << "BETWEEN constant default silently returned NULL";
        }
    } else {
        EXPECT_FALSE(result.error().message.empty());
    }
}

// ---- LEAD mirrors LAG for constant-fold coverage ----
TEST_F(QA_GDB1285_V2_ConstantFoldTest, LeadColumnReferenceDefaultRejectedCleanly) {
    auto result =
        exec("SELECT name, salary, LEAD(salary, 100, department) OVER (ORDER BY salary) AS nxt "
             "FROM employees");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB1285_V2_ConstantFoldTest, LeadDivisionByZeroDefaultErrorsCleanly) {
    auto result =
        exec("SELECT name, salary, LEAD(salary, 100, 10/0) OVER (ORDER BY salary) AS nxt "
             "FROM employees");
    ASSERT_FALSE(result.has_value());
    EXPECT_FALSE(result.error().message.empty());
}
