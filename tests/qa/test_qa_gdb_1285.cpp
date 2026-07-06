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
#include <variant>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// QA adversarial tests for GDB-1285 (LAG/LEAD default-value 3rd argument).
// Targets risk areas flagged by the implementer as untested:
//   - non-literal constant expressions as default (1+1, CAST(...))
//   - DECIMAL-typed columns with a default literal
//   - STRING column with numeric default literal (coercion vs error)
//   - double/multiple unary minus, unary minus on non-numeric literal
//   - default type that cannot coerce to column type
// Plus core-behavior adversarial coverage: boundary offsets, 2-arg/1-arg
// regressions, explicit NULL vs omitted 3rd arg, large/zero/negative offsets,
// and combined PARTITION BY + offset + default stress.
// =============================================================================

class QA_GDB1285_WindowLagLeadDefaultTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1285";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        exec_ok("CREATE TABLE employees ("
                "id INT PRIMARY KEY, "
                "name TEXT NOT NULL, "
                "salary DOUBLE, "
                "department TEXT)");

        // Two partitions: Engineering (3 rows), Sales (2 rows).
        // Salary order within Engineering: Alice(80k), Bob(90k), Eve(95k).
        // Salary order within Sales: Charlie(70k), Dave(85k).
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

    // Like exec_ok but does not assert success -- for cases we expect to
    // either error cleanly OR succeed; caller inspects the Result directly.
    Result<QueryResult> exec(const std::string& sql) { return engine_->execute(sql); }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << "SQL: " << sql << "\n"
                                        << "Error: " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    static std::unordered_map<std::string, Value>
    by_name(const QueryResult& qr, size_t name_col, size_t val_col) {
        std::unordered_map<std::string, Value> m;
        for (auto& row : qr.rows) {
            m.emplace(row[name_col].as_string(), row[val_col]);
        }
        return m;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// ---- Non-literal constant expression as default ----
// The implementer's dynamic_cast<LiteralExpr> only recognizes bare literals
// (and unary-negate-of-literal). An arithmetic expression like `1+1` is
// neither, so it silently falls through and desc.default_value stays NULL --
// reproducing the *exact* original bug for this input shape, with no error
// surfaced to the user. This is the highest-value adversarial case.
TEST_F(QA_GDB1285_WindowLagLeadDefaultTest, NonLiteralArithmeticDefaultDoesNotSilentlyReturnNull) {
    auto qr =
        exec_ok("SELECT name, salary, LAG(salary, 100, 1+1) OVER (ORDER BY salary) AS prev "
                "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    for (auto& row : qr.rows) {
        // Whatever the "correct" behavior should be, the default must NOT be
        // silently dropped back to NULL when a valid non-literal default of
        // value 2 was specified and the offset is out of range.
        EXPECT_FALSE(row[2].is_null())
            << "LAG(salary, 100, 1+1): default 1+1 was silently ignored, same bug as GDB-1285 "
               "for a non-literal-but-constant default expression";
    }
}

TEST_F(QA_GDB1285_WindowLagLeadDefaultTest, NonLiteralCastDefaultDoesNotSilentlyReturnNull) {
    auto qr = exec_ok(
        "SELECT name, salary, LAG(salary, 100, CAST(7 AS DOUBLE)) OVER (ORDER BY salary) AS prev "
        "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    for (auto& row : qr.rows) {
        EXPECT_FALSE(row[2].is_null())
            << "LAG(salary, 100, CAST(7 AS DOUBLE)): CAST expression default silently ignored";
    }
}

// ---- Double / multiple unary minus ----

TEST_F(QA_GDB1285_WindowLagLeadDefaultTest, DoubleUnaryMinusDefaultResolvesToPositive) {
    // `--5` is tokenized as a line comment by the lexer (SQL `--` comment
    // syntax), not as two unary minus operators, so it is rejected as a
    // parse error rather than reaching the planner's default-value logic at
    // all (see DoubleDashIsRejectedAsCommentNotDoubleNegation below).
    // Using explicit parens for double negation, `-(-5)`, parses as
    // NEGATE(NEGATE(LiteralExpr(5))). The planner's default-value parser
    // (src/executor/planner.cpp ~2085-2091) only unwraps a *single* layer of
    // UnaryExpr::NEGATE before doing a dynamic_cast<LiteralExpr>. After
    // unwrapping once it is left with NEGATE(5), which is not a LiteralExpr,
    // so the whole `if (auto* lit = ...)` fails silently and
    // desc.default_value stays NULL -- this is the *exact* GDB-1285 bug
    // reappearing for a nested-unary-minus default. Confirmed via QA: this
    // assertion fails (default is silently NULL instead of 5).
    auto qr = exec_ok(
        "SELECT name, salary, LAG(salary, 100, -(-5)) OVER (ORDER BY salary) AS prev "
        "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    ASSERT_FALSE(qr.rows.empty());
    ASSERT_FALSE(qr.rows[0][2].is_null())
        << "BUG: double-negated literal default -(-5) was silently dropped to NULL "
           "(planner only unwraps a single UnaryExpr::NEGATE layer)";
    EXPECT_DOUBLE_EQ(qr.rows[0][2].as_float64(), 5.0)
        << "LAG(x, 100, -(-5)): double unary minus should resolve to +5, got "
        << qr.rows[0][2].as_float64();
}

TEST_F(QA_GDB1285_WindowLagLeadDefaultTest, DoubleDashIsRejectedAsCommentNotDoubleNegation) {
    // Documents the `--` lexer-comment ambiguity discovered above: this must
    // be a clean parse error, never a crash or a silently wrong default.
    auto result = exec(
        "SELECT name, salary, LAG(salary, 100, --5) OVER (ORDER BY salary) AS prev "
        "FROM employees");
    ASSERT_FALSE(result.has_value());
    EXPECT_FALSE(result.error().message.empty());
}

TEST_F(QA_GDB1285_WindowLagLeadDefaultTest, SingleUnaryMinusDefaultIsNegative) {
    auto qr = exec_ok(
        "SELECT name, salary, LAG(salary, 100, -5) OVER (ORDER BY salary) AS prev "
        "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    for (auto& row : qr.rows) {
        ASSERT_FALSE(row[2].is_null());
        EXPECT_DOUBLE_EQ(row[2].as_float64(), -5.0);
    }
}

// ---- STRING-typed LAG/LEAD column with numeric default literal ----

TEST_F(QA_GDB1285_WindowLagLeadDefaultTest, StringColumnWithNumericDefaultCoercesOrErrorsCleanly) {
    auto result =
        exec("SELECT name, LAG(department, 100, 42) OVER (ORDER BY salary) AS prevdept "
             "FROM employees");
    if (result.has_value()) {
        // If it succeeds, the numeric default must have been coerced to a
        // sensible string ("42"), not silently left NULL and not garbage.
        ASSERT_EQ(result->rows.size(), 5u);
        for (auto& row : result->rows) {
            EXPECT_FALSE(row[1].is_null())
                << "numeric default on STRING column silently returned NULL instead of "
                   "coercing or erroring";
            if (!row[1].is_null()) {
                EXPECT_EQ(row[1].as_string(), "42")
                    << "numeric default coerced to STRING column produced unexpected value";
            }
        }
    } else {
        // A clean TYPE_ERROR is acceptable behavior, not a bug.
        EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR)
            << "expected a clean TYPE_ERROR for numeric default vs STRING column, got: "
            << result.error().message;
        EXPECT_FALSE(result.error().message.empty());
    }
}

// ---- Default type that cannot coerce to the column type ----

TEST_F(QA_GDB1285_WindowLagLeadDefaultTest, IncompatibleStringDefaultOnNumericColumnErrorsCleanly) {
    auto result =
        exec("SELECT name, LAG(salary, 100, 'not_a_number') OVER (ORDER BY salary) AS prev "
             "FROM employees");
    // Must be a clean, explicit error -- never silent garbage or a crash.
    ASSERT_FALSE(result.has_value())
        << "LAG(salary, 100, 'not_a_number') should fail to coerce a non-numeric string "
           "default onto a DOUBLE column, but the query succeeded";
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    EXPECT_FALSE(result.error().message.empty());
}

// ---- Unary minus on a non-numeric literal ----

TEST_F(QA_GDB1285_WindowLagLeadDefaultTest, UnaryMinusOnStringLiteralDefaultErrorsCleanly) {
    // -'abc' is nonsensical; the implementation should either reject it at
    // parse/bind time or surface a clean TYPE_ERROR -- never crash or
    // silently coerce to a garbage numeric value.
    auto result = exec(
        "SELECT name, LAG(salary, 100, -'abc') OVER (ORDER BY salary) AS prev FROM employees");
    if (result.has_value()) {
        // If somehow accepted, must not silently produce NULL (the original
        // bug) nor a non-numeric garbage value in a DOUBLE column.
        for (auto& row : result->rows) {
            if (!row[1].is_null()) {
                // as_float64 should not throw/crash; if it returns, ensure
                // it's not some uninitialized garbage sentinel test is at
                // least well-defined (this branch mainly guards against a
                // crash, already implicitly tested by reaching here).
                (void)row[1].as_float64();
            }
        }
    } else {
        EXPECT_FALSE(result.error().message.empty());
    }
}

// ---- Explicit NULL vs omitted 3rd arg ----

TEST_F(QA_GDB1285_WindowLagLeadDefaultTest, ExplicitNullDefaultMatchesOmittedDefault) {
    auto qr_explicit = exec_ok(
        "SELECT name, salary, LAG(salary, 100, NULL) OVER (ORDER BY salary) AS prev "
        "FROM employees");
    auto qr_omitted = exec_ok(
        "SELECT name, salary, LAG(salary, 100) OVER (ORDER BY salary) AS prev FROM employees");
    ASSERT_EQ(qr_explicit.rows.size(), 5u);
    ASSERT_EQ(qr_omitted.rows.size(), 5u);
    for (size_t i = 0; i < qr_explicit.rows.size(); ++i) {
        EXPECT_TRUE(qr_explicit.rows[i][2].is_null())
            << "explicit NULL 3rd arg should behave identically to omitted 3rd arg (NULL)";
        EXPECT_TRUE(qr_omitted.rows[i][2].is_null());
    }
}

// ---- 2-arg and 1-arg forms unaffected (no regression) ----

TEST_F(QA_GDB1285_WindowLagLeadDefaultTest, TwoArgFormStillReturnsNullOutOfRange) {
    auto qr = exec_ok(
        "SELECT name, salary, LAG(salary, 100) OVER (ORDER BY salary) AS prev FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    for (auto& row : qr.rows) {
        EXPECT_TRUE(row[2].is_null());
    }
}

TEST_F(QA_GDB1285_WindowLagLeadDefaultTest, OneArgFormStillWorks) {
    auto qr =
        exec_ok("SELECT name, salary, LAG(salary) OVER (ORDER BY salary) AS prev FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    // Global salary order: Charlie(70k), Alice(80k), Dave(85k), Bob(90k), Eve(95k).
    // The row with the lowest salary (Charlie) has no predecessor -> NULL;
    // every other row should have the previous row's salary (non-NULL).
    // Look up by name rather than assuming a row index, since row order in
    // the result set is not otherwise guaranteed.
    auto m = by_name(qr, 0, 2);
    EXPECT_TRUE(m.at("Charlie").is_null()) << "lowest-salary row must have no LAG predecessor";
    for (const auto& name : {"Alice", "Dave", "Bob", "Eve"}) {
        EXPECT_FALSE(m.at(name).is_null()) << name << " should have a non-NULL LAG value";
    }
}

// ---- Large offset >> partition size, offset 0, negative offset ----

TEST_F(QA_GDB1285_WindowLagLeadDefaultTest, VeryLargeOffsetWithDefaultFillsDefaultNoOverflow) {
    // Offset near INT32 range boundary combined with a default to make sure
    // there's no integer overflow/UB in the (pos - offset) arithmetic.
    auto qr = exec_ok(
        "SELECT name, salary, LAG(salary, 2000000000, -99) OVER (ORDER BY salary) AS prev "
        "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    for (auto& row : qr.rows) {
        ASSERT_FALSE(row[2].is_null());
        EXPECT_DOUBLE_EQ(row[2].as_float64(), -99.0);
    }
}

TEST_F(QA_GDB1285_WindowLagLeadDefaultTest, OffsetZeroWithDefaultReturnsCurrentRowNotDefault) {
    auto qr = exec_ok(
        "SELECT name, salary, LAG(salary, 0, -1) OVER (ORDER BY salary) AS same FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    for (auto& row : qr.rows) {
        ASSERT_FALSE(row[2].is_null());
        EXPECT_DOUBLE_EQ(row[2].as_float64(), row[1].as_float64())
            << "offset 0 must return current row, not the default, regardless of default arg";
    }
}

TEST_F(QA_GDB1285_WindowLagLeadDefaultTest, NegativeOffsetWithDefaultIfParserAllowsIt) {
    // A negative offset for LAG is equivalent to a positive offset for LEAD.
    // Confirm the parser either rejects it cleanly or handles it consistently
    // -- must not crash or silently drop the default.
    auto result = exec("SELECT name, salary, LAG(salary, -1, -1) OVER (ORDER BY salary) AS x "
                       "FROM employees");
    if (result.has_value()) {
        ASSERT_EQ(result->rows.size(), 5u);
        // LAG(x, -1) with an ascending ORDER BY should behave like LEAD(x, 1):
        // last row has no successor and should fall back to the default -1.
        bool any_non_null = false;
        for (auto& row : result->rows) {
            if (!row[2].is_null()) {
                any_non_null = true;
            }
        }
        EXPECT_TRUE(any_non_null) << "negative offset silently produced all-NULL results";
    } else {
        EXPECT_FALSE(result.error().message.empty());
    }
}

// ---- Boundary offsets: exactly at / one beyond partition boundary, no leak across partitions ----

TEST_F(QA_GDB1285_WindowLagLeadDefaultTest, LagOffsetExactlyAtPartitionBoundaryUsesDefault) {
    // Engineering has 3 rows; offset 3 from any row in Engineering is exactly
    // out of range and must use the default, never leak into Sales.
    auto qr = exec_ok(
        "SELECT name, department, salary, "
        "LAG(salary, 3, -7) OVER (PARTITION BY department ORDER BY salary) AS prev "
        "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    auto m = by_name(qr, 0, 3);
    // Every row's offset-3 target is out of its own partition (max partition
    // size is 3), so all should be the default.
    for (auto& row : qr.rows) {
        ASSERT_FALSE(row[3].is_null());
        EXPECT_DOUBLE_EQ(row[3].as_float64(), -7.0)
            << "row " << row[0].as_string() << " should use default at exact boundary";
    }
}

TEST_F(QA_GDB1285_WindowLagLeadDefaultTest, LeadOffsetOneBeyondPartitionBoundaryUsesDefault) {
    // Sales partition (Charlie 70k, Dave 85k) has 2 rows. LEAD(salary, 2, -3)
    // is one beyond the boundary for both rows in Sales.
    auto qr = exec_ok(
        "SELECT name, department, salary, "
        "LEAD(salary, 2, -3) OVER (PARTITION BY department ORDER BY salary) AS nxt "
        "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    auto m = by_name(qr, 0, 3);
    ASSERT_FALSE(m.at("Charlie").is_null());
    EXPECT_DOUBLE_EQ(m.at("Charlie").as_float64(), -3.0);
    ASSERT_FALSE(m.at("Dave").is_null());
    EXPECT_DOUBLE_EQ(m.at("Dave").as_float64(), -3.0);
}

TEST_F(QA_GDB1285_WindowLagLeadDefaultTest, DefaultNeverLeaksAcrossPartitionBoundaryValue) {
    // If default leaked incorrectly, a row near a partition edge might pick
    // up a real value from the *other* partition instead of the default.
    // Verify Charlie (first in Sales, smallest global salary too) doesn't
    // pick up any Engineering value via LAG across the global order.
    auto qr = exec_ok(
        "SELECT name, department, salary, "
        "LAG(salary, 1, -123) OVER (PARTITION BY department ORDER BY salary) AS prev "
        "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    auto m = by_name(qr, 0, 3);
    // Charlie is first in Sales partition -> must be the default, not
    // Eve/Bob/Alice's salary from Engineering.
    ASSERT_FALSE(m.at("Charlie").is_null());
    EXPECT_DOUBLE_EQ(m.at("Charlie").as_float64(), -123.0);
    // Alice is first in Engineering partition -> must also be the default.
    ASSERT_FALSE(m.at("Alice").is_null());
    EXPECT_DOUBLE_EQ(m.at("Alice").as_float64(), -123.0);
}

// ---- Mixed: LEAD with offset+default across multiple partitions simultaneously ----

TEST_F(QA_GDB1285_WindowLagLeadDefaultTest, LeadOffsetAndDefaultAcrossMultiplePartitions) {
    auto qr = exec_ok(
        "SELECT name, department, salary, "
        "LEAD(salary, 1, -999) OVER (PARTITION BY department ORDER BY salary) AS nxt "
        "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    auto m = by_name(qr, 0, 3);
    // Engineering: Alice(80k)->Bob(90k), Bob(90k)->Eve(95k), Eve(95k)->default.
    ASSERT_FALSE(m.at("Alice").is_null());
    EXPECT_DOUBLE_EQ(m.at("Alice").as_float64(), 90000.0);
    ASSERT_FALSE(m.at("Bob").is_null());
    EXPECT_DOUBLE_EQ(m.at("Bob").as_float64(), 95000.0);
    ASSERT_FALSE(m.at("Eve").is_null());
    EXPECT_DOUBLE_EQ(m.at("Eve").as_float64(), -999.0);
    // Sales: Charlie(70k)->Dave(85k), Dave(85k)->default.
    ASSERT_FALSE(m.at("Charlie").is_null());
    EXPECT_DOUBLE_EQ(m.at("Charlie").as_float64(), 85000.0);
    ASSERT_FALSE(m.at("Dave").is_null());
    EXPECT_DOUBLE_EQ(m.at("Dave").as_float64(), -999.0);
}

// ---- DECIMAL-typed column with default literal ----

class QA_GDB1285_WindowLagLeadDecimalTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1285_decimal";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        auto create_result =
            engine_->execute("CREATE TABLE accounts ("
                             "id INT PRIMARY KEY, "
                             "name TEXT NOT NULL, "
                             "balance DECIMAL(10,2))");
        ASSERT_TRUE(create_result.has_value()) << create_result.error().message;

        for (auto& sql : {"INSERT INTO accounts VALUES (1, 'A', 100.50)",
                          "INSERT INTO accounts VALUES (2, 'B', 200.25)",
                          "INSERT INTO accounts VALUES (3, 'C', 50.00)"}) {
            auto r = engine_->execute(sql);
            ASSERT_TRUE(r.has_value()) << r.error().message;
        }
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

TEST_F(QA_GDB1285_WindowLagLeadDecimalTest, DecimalColumnWithIntegerDefaultCoercesCorrectly) {
    auto result = engine_->execute(
        "SELECT name, balance, LAG(balance, 100, 0) OVER (ORDER BY balance) AS prev "
        "FROM accounts");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 3u);
    for (auto& row : result->rows) {
        ASSERT_FALSE(row[2].is_null())
            << "DECIMAL column LAG default should coerce integer literal 0, not stay NULL";
    }
}

TEST_F(QA_GDB1285_WindowLagLeadDecimalTest, DecimalColumnWithDecimalLiteralDefault) {
    auto result = engine_->execute(
        "SELECT name, balance, LEAD(balance, 100, 9.99) OVER (ORDER BY balance) AS nxt "
        "FROM accounts");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 3u);
    for (auto& row : result->rows) {
        // Core assertion for GDB-1285 on a DECIMAL column: the default must
        // not be silently dropped to NULL. DECIMAL columns store values as
        // Decimal128, not double, so as_float64() is the wrong accessor
        // (throws std::bad_variant_access -- see the dedicated test below);
        // we only assert non-null and correct type here rather than decode
        // the raw Decimal128 coefficient/scale, which is an internal detail
        // orthogonal to this ticket's acceptance criteria.
        ASSERT_FALSE(row[2].is_null())
            << "LEAD default 9.99 on a DECIMAL column was silently dropped to NULL";
        EXPECT_EQ(row[2].type_id(), TypeId::DECIMAL)
            << "LEAD default on a DECIMAL column should remain typed as DECIMAL";
    }
}

// The QA process itself discovered that calling as_float64() on a DECIMAL
// Value throws std::bad_variant_access rather than returning a Result error
// or performing an implicit conversion. This is a pre-existing property of
// Value's typed accessors elsewhere in the codebase (not introduced by
// GDB-1285), but worth pinning down here since it was hit directly while
// probing LAG/LEAD default coercion onto a DECIMAL column.
TEST_F(QA_GDB1285_WindowLagLeadDecimalTest, AsFloat64OnDecimalValueThrowsBadVariantAccess) {
    auto result = engine_->execute(
        "SELECT name, balance, LEAD(balance, 100, 9.99) OVER (ORDER BY balance) AS nxt "
        "FROM accounts");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_FALSE(result->rows.empty());
    const Value& v = result->rows[0][2];
    ASSERT_EQ(v.type_id(), TypeId::DECIMAL);
    EXPECT_THROW((void)v.as_float64(), std::bad_variant_access);
}
