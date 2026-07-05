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
// Test fixture for window function SQL integration tests (GDB-599)
// =============================================================================

class WindowFunctionSQLTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_win";
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

// =============================================================================
// Bug report reproduction tests (GDB-599)
// =============================================================================

TEST_F(WindowFunctionSQLTest, RowNumberOverOrderById) {
    auto qr = exec_ok("SELECT id, ROW_NUMBER() OVER (ORDER BY id) AS rn FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);
    ASSERT_GE(qr.column_names.size(), 2);
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "rn");

    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(qr.rows[i][1].as_int64(), static_cast<int64_t>(i + 1));
    }
}

TEST_F(WindowFunctionSQLTest, LagOverOrderBySalary) {
    auto qr =
        exec_ok("SELECT name, salary, LAG(salary) OVER (ORDER BY salary) AS prev FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);

    // WindowOperator preserves original insertion order but computes LAG in salary order.
    // Salary order: Charlie(70k), Alice(80k), Dave(85k), Bob(90k), Eve(95k).
    // Each row's LAG(salary) should equal the salary of the row immediately preceding it
    // in ascending-salary order; the lowest-salary row (Charlie) should have a NULL LAG.
    // A LAG/LEAD swap would instead put the NULL on the highest-salary row (Eve) and shift
    // every value to the *next* higher salary rather than the previous (lower) one, so this
    // pins down the direction as well as the NULL placement.
    std::unordered_map<std::string, Value> lag_by_name;
    for (auto& row : qr.rows) {
        lag_by_name.emplace(row[0].as_string(), row[2]);
    }
    ASSERT_EQ(lag_by_name.size(), 5u);

    ASSERT_TRUE(lag_by_name.at("Charlie").is_null());
    ASSERT_FALSE(lag_by_name.at("Alice").is_null());
    ASSERT_FALSE(lag_by_name.at("Dave").is_null());
    ASSERT_FALSE(lag_by_name.at("Bob").is_null());
    ASSERT_FALSE(lag_by_name.at("Eve").is_null());

    EXPECT_DOUBLE_EQ(lag_by_name.at("Alice").as_float64(), 70000.0);
    EXPECT_DOUBLE_EQ(lag_by_name.at("Dave").as_float64(), 80000.0);
    EXPECT_DOUBLE_EQ(lag_by_name.at("Bob").as_float64(), 85000.0);
    EXPECT_DOUBLE_EQ(lag_by_name.at("Eve").as_float64(), 90000.0);
}

TEST_F(WindowFunctionSQLTest, SelectStarWithRowNumber) {
    auto qr = exec_ok("SELECT *, ROW_NUMBER() OVER (ORDER BY id) AS rn FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);
    ASSERT_GE(qr.column_names.size(), 5);

    // Last column should be the row number.
    size_t last = qr.column_names.size() - 1;
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(qr.rows[i][last].as_int64(), static_cast<int64_t>(i + 1));
    }
}

// =============================================================================
// Ranking function tests
// =============================================================================

TEST_F(WindowFunctionSQLTest, RankPartitionByDept) {
    // Seeded salaries: Alice(Eng,80k), Bob(Eng,90k), Eve(Eng,95k), Charlie(Sales,70k),
    // Dave(Sales,85k). RANK() PARTITION BY department ORDER BY salary DESC gives, per
    // partition (descending salary; all salaries within each partition are distinct,
    // so there are no ties to skip over):
    //   Engineering: Eve=1, Bob=2, Alice=3
    //   Sales:       Dave=1, Charlie=2
    auto qr = exec_ok("SELECT name, department, salary, "
                      "RANK() OVER (PARTITION BY department ORDER BY salary DESC) AS rnk "
                      "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);

    // Build a name -> rank map so the assertions are robust to output row order.
    std::unordered_map<std::string, int64_t> rank_by_name;
    for (auto& row : qr.rows) {
        rank_by_name[row[0].as_string()] = row[3].as_int64();
    }
    ASSERT_EQ(rank_by_name.size(), 5u);

    EXPECT_EQ(rank_by_name.at("Eve"), 1);
    EXPECT_EQ(rank_by_name.at("Bob"), 2);
    EXPECT_EQ(rank_by_name.at("Alice"), 3);

    EXPECT_EQ(rank_by_name.at("Dave"), 1);
    EXPECT_EQ(rank_by_name.at("Charlie"), 2);
}

TEST_F(WindowFunctionSQLTest, DenseRank) {
    // Seeded salaries (ascending, all distinct -- no ties):
    // Charlie(70k), Alice(80k), Dave(85k), Bob(90k), Eve(95k).
    // DENSE_RANK() OVER (ORDER BY salary) assigns consecutive ranks with no gaps
    // since there are no ties: Charlie=1, Alice=2, Dave=3, Bob=4, Eve=5.
    auto qr = exec_ok("SELECT name, salary, "
                      "DENSE_RANK() OVER (ORDER BY salary) AS dr "
                      "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);

    std::unordered_map<std::string, int64_t> dr_by_name;
    for (auto& row : qr.rows) {
        dr_by_name[row[0].as_string()] = row[2].as_int64();
    }
    ASSERT_EQ(dr_by_name.size(), 5u);

    EXPECT_EQ(dr_by_name.at("Charlie"), 1);
    EXPECT_EQ(dr_by_name.at("Alice"), 2);
    EXPECT_EQ(dr_by_name.at("Dave"), 3);
    EXPECT_EQ(dr_by_name.at("Bob"), 4);
    EXPECT_EQ(dr_by_name.at("Eve"), 5);
}

// =============================================================================
// Aggregate window function tests
// =============================================================================

TEST_F(WindowFunctionSQLTest, CountWindowFunction) {
    auto qr = exec_ok("SELECT name, "
                      "COUNT(*) OVER (ORDER BY id) AS running_count "
                      "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);

    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(qr.rows[i][1].as_int64(), static_cast<int64_t>(i + 1));
    }
}

// =============================================================================
// LEAD function test
// =============================================================================

TEST_F(WindowFunctionSQLTest, LeadFunction) {
    auto qr = exec_ok("SELECT name, salary, "
                      "LEAD(salary) OVER (ORDER BY salary) AS next_salary "
                      "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);

    // Last row's LEAD should be NULL.
    EXPECT_TRUE(qr.rows[4][2].is_null());

    // Other rows should have next salary value.
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_FALSE(qr.rows[i][2].is_null());
    }
}

// =============================================================================
// Column resolution test
// =============================================================================

TEST_F(WindowFunctionSQLTest, MixedColumnsAndWindowFunction) {
    auto qr = exec_ok("SELECT id, name, department, "
                      "ROW_NUMBER() OVER (ORDER BY id) AS rn "
                      "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);
    ASSERT_EQ(qr.column_names.size(), 4);
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "name");
    EXPECT_EQ(qr.column_names[2], "department");
    EXPECT_EQ(qr.column_names[3], "rn");
}

// =============================================================================
// Empty OVER clause
// =============================================================================

TEST_F(WindowFunctionSQLTest, WindowFunctionEmptyOver) {
    auto qr = exec_ok("SELECT name, ROW_NUMBER() OVER () AS rn FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);
}

// =============================================================================
// Default frame without ORDER BY (GDB-607)
// =============================================================================

TEST_F(WindowFunctionSQLTest, CountOverEmptyClauseReturnsTotal) {
    // COUNT(*) OVER () should return total row count for every row (full partition).
    auto qr = exec_ok("SELECT id, COUNT(*) OVER () AS total FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);
    for (auto& row : qr.rows) {
        EXPECT_EQ(row[1].as_int64(), 5) << "COUNT(*) OVER () should be 5 for all rows";
    }
}

TEST_F(WindowFunctionSQLTest, CountOverPartitionByWithoutOrderBy) {
    // COUNT(*) OVER (PARTITION BY department) should return partition totals, not running counts.
    // Engineering has 3 employees (Alice, Bob, Eve), Sales has 2 (Charlie, Dave).
    auto qr = exec_ok("SELECT id, department, COUNT(*) OVER (PARTITION BY department) AS total "
                      "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);
    for (auto& row : qr.rows) {
        auto dept = row[1].as_string();
        auto total = row[2].as_int64();
        if (dept == "Engineering") {
            EXPECT_EQ(total, 3) << "Engineering partition should have total=3";
        } else {
            EXPECT_EQ(total, 2) << "Sales partition should have total=2";
        }
    }
}

TEST_F(WindowFunctionSQLTest, SumOverPartitionByWithoutOrderBy) {
    // SUM(salary) OVER (PARTITION BY department) should give partition totals.
    // Engineering: 80000 + 90000 + 95000 = 265000
    // Sales: 70000 + 85000 = 155000
    auto qr = exec_ok("SELECT id, department, "
                      "SUM(salary) OVER (PARTITION BY department) AS dept_total "
                      "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);
    for (auto& row : qr.rows) {
        auto dept = row[1].as_string();
        auto total = row[2].as_float64();
        if (dept == "Engineering") {
            EXPECT_DOUBLE_EQ(total, 265000.0);
        } else {
            EXPECT_DOUBLE_EQ(total, 155000.0);
        }
    }
}

TEST_F(WindowFunctionSQLTest, CountWithOrderByStillGivesRunningCount) {
    // COUNT(*) OVER (ORDER BY id) should still give running counts (default frame
    // with ORDER BY is UNBOUNDED PRECEDING to CURRENT ROW).
    auto qr = exec_ok("SELECT id, COUNT(*) OVER (ORDER BY id) AS running FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(qr.rows[i][1].as_int64(), static_cast<int64_t>(i + 1));
    }
}

TEST_F(WindowFunctionSQLTest, ExplicitFrameOverridesDefault) {
    // An explicit frame should still be respected even without ORDER BY.
    auto qr = exec_ok("SELECT id, COUNT(*) OVER (ROWS BETWEEN UNBOUNDED PRECEDING "
                      "AND CURRENT ROW) AS running FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);
    // With explicit CURRENT ROW bound, should get running counts.
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(qr.rows[i][1].as_int64(), static_cast<int64_t>(i + 1));
    }
}
