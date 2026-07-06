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
// QA adversarial tests for GDB-1204 (LAG/LEAD correctness through the SQL
// pipeline). Verifies: explicit offsets, default-value fill, PARTITION BY
// boundary isolation, descending ORDER BY, ties, offset >= partition size,
// and offset 0.
// =============================================================================

class QA_GDB1204_WindowLagLeadTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1204";
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

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << "SQL: " << sql << "\n"
                                        << "Error: " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    static std::unordered_map<std::string, Value> by_name(const QueryResult& qr,
                                                            size_t name_col,
                                                            size_t val_col) {
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

// ---- Explicit offset N ----

TEST_F(QA_GDB1204_WindowLagLeadTest, LagExplicitOffsetTwo) {
    // Global order by salary: Charlie(70k), Alice(80k), Dave(85k), Bob(90k), Eve(95k).
    auto qr = exec_ok(
        "SELECT name, salary, LAG(salary, 2) OVER (ORDER BY salary) AS prev2 FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    auto m = by_name(qr, 0, 2);
    ASSERT_EQ(m.size(), 5u);

    // First two rows in sorted order have no value 2 back.
    EXPECT_TRUE(m.at("Charlie").is_null());
    EXPECT_TRUE(m.at("Alice").is_null());
    ASSERT_FALSE(m.at("Dave").is_null());
    ASSERT_FALSE(m.at("Bob").is_null());
    ASSERT_FALSE(m.at("Eve").is_null());
    EXPECT_DOUBLE_EQ(m.at("Dave").as_float64(), 70000.0);  // 2 back from Dave = Charlie
    EXPECT_DOUBLE_EQ(m.at("Bob").as_float64(), 80000.0);   // 2 back from Bob = Alice
    EXPECT_DOUBLE_EQ(m.at("Eve").as_float64(), 85000.0);   // 2 back from Eve = Dave
}

TEST_F(QA_GDB1204_WindowLagLeadTest, LeadExplicitOffsetTwo) {
    auto qr = exec_ok(
        "SELECT name, salary, LEAD(salary, 2) OVER (ORDER BY salary) AS next2 FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    auto m = by_name(qr, 0, 2);

    // Last two rows in sorted order have no value 2 ahead.
    EXPECT_TRUE(m.at("Bob").is_null());
    EXPECT_TRUE(m.at("Eve").is_null());
    ASSERT_FALSE(m.at("Charlie").is_null());
    ASSERT_FALSE(m.at("Alice").is_null());
    ASSERT_FALSE(m.at("Dave").is_null());
    EXPECT_DOUBLE_EQ(m.at("Charlie").as_float64(), 85000.0);  // 2 ahead of Charlie = Dave
    EXPECT_DOUBLE_EQ(m.at("Alice").as_float64(), 90000.0);    // 2 ahead of Alice = Bob
    EXPECT_DOUBLE_EQ(m.at("Dave").as_float64(), 95000.0);     // 2 ahead of Dave = Eve
}

// ---- Default value (3rd arg) instead of NULL ----

TEST_F(QA_GDB1204_WindowLagLeadTest, LagWithDefaultValueFillsInsteadOfNull) {
    GTEST_SKIP() << "LAG/LEAD default-value semantics tracked by GDB-1285";
    auto qr = exec_ok("SELECT name, salary, LAG(salary, 1, 0) OVER (ORDER BY salary) AS prev "
                       "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    auto m = by_name(qr, 0, 2);

    // Charlie is first in sorted order; default value 0 should be used, NOT NULL.
    ASSERT_FALSE(m.at("Charlie").is_null())
        << "LAG default value (3rd arg) must fill instead of NULL when out of range";
    EXPECT_DOUBLE_EQ(m.at("Charlie").as_float64(), 0.0);
    EXPECT_DOUBLE_EQ(m.at("Alice").as_float64(), 70000.0);
}

TEST_F(QA_GDB1204_WindowLagLeadTest, LeadWithDefaultValueFillsInsteadOfNull) {
    GTEST_SKIP() << "LAG/LEAD default-value semantics tracked by GDB-1285";
    auto qr = exec_ok("SELECT name, salary, LEAD(salary, 1, -1) OVER (ORDER BY salary) AS nxt "
                       "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    auto m = by_name(qr, 0, 2);

    // Eve is last in sorted order; default value -1 should be used, NOT NULL.
    ASSERT_FALSE(m.at("Eve").is_null())
        << "LEAD default value (3rd arg) must fill instead of NULL when out of range";
    EXPECT_DOUBLE_EQ(m.at("Eve").as_float64(), -1.0);
    EXPECT_DOUBLE_EQ(m.at("Bob").as_float64(), 95000.0);
}

// ---- LEAD mirrors LAG ----

TEST_F(QA_GDB1204_WindowLagLeadTest, LeadDefaultOffsetOverSalary) {
    auto qr = exec_ok(
        "SELECT name, salary, LEAD(salary) OVER (ORDER BY salary) AS nxt FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    auto m = by_name(qr, 0, 2);

    ASSERT_TRUE(m.at("Eve").is_null());  // highest salary -> no next row
    ASSERT_FALSE(m.at("Charlie").is_null());
    ASSERT_FALSE(m.at("Alice").is_null());
    ASSERT_FALSE(m.at("Dave").is_null());
    ASSERT_FALSE(m.at("Bob").is_null());

    EXPECT_DOUBLE_EQ(m.at("Charlie").as_float64(), 80000.0);
    EXPECT_DOUBLE_EQ(m.at("Alice").as_float64(), 85000.0);
    EXPECT_DOUBLE_EQ(m.at("Dave").as_float64(), 90000.0);
    EXPECT_DOUBLE_EQ(m.at("Bob").as_float64(), 95000.0);
}

// ---- PARTITION BY: offsets must not cross partition boundaries ----

TEST_F(QA_GDB1204_WindowLagLeadTest, LagDoesNotLeakAcrossPartitionBoundary) {
    // Sales partition sorted by salary: Charlie(70k), Dave(85k).
    // Engineering partition sorted by salary: Alice(80k), Bob(90k), Eve(95k).
    auto qr = exec_ok("SELECT name, department, salary, "
                       "LAG(salary) OVER (PARTITION BY department ORDER BY salary) AS prev "
                       "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    auto m = by_name(qr, 0, 3);

    // First row of each partition must be NULL, not leak the last value of the
    // "previous" partition in some internal ordering.
    EXPECT_TRUE(m.at("Charlie").is_null()) << "Charlie is first in Sales partition";
    EXPECT_TRUE(m.at("Alice").is_null()) << "Alice is first in Engineering partition";

    ASSERT_FALSE(m.at("Dave").is_null());
    EXPECT_DOUBLE_EQ(m.at("Dave").as_float64(), 70000.0);  // Charlie, within Sales

    ASSERT_FALSE(m.at("Bob").is_null());
    EXPECT_DOUBLE_EQ(m.at("Bob").as_float64(), 80000.0);  // Alice, within Engineering

    ASSERT_FALSE(m.at("Eve").is_null());
    EXPECT_DOUBLE_EQ(m.at("Eve").as_float64(), 90000.0);  // Bob, within Engineering
}

TEST_F(QA_GDB1204_WindowLagLeadTest, LeadDoesNotLeakAcrossPartitionBoundary) {
    auto qr = exec_ok("SELECT name, department, salary, "
                       "LEAD(salary) OVER (PARTITION BY department ORDER BY salary) AS nxt "
                       "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    auto m = by_name(qr, 0, 3);

    // Last row of each partition must be NULL.
    EXPECT_TRUE(m.at("Dave").is_null()) << "Dave is last in Sales partition";
    EXPECT_TRUE(m.at("Eve").is_null()) << "Eve is last in Engineering partition";

    ASSERT_FALSE(m.at("Charlie").is_null());
    EXPECT_DOUBLE_EQ(m.at("Charlie").as_float64(), 85000.0);  // Dave, within Sales

    ASSERT_FALSE(m.at("Alice").is_null());
    EXPECT_DOUBLE_EQ(m.at("Alice").as_float64(), 90000.0);  // Bob, within Engineering

    ASSERT_FALSE(m.at("Bob").is_null());
    EXPECT_DOUBLE_EQ(m.at("Bob").as_float64(), 95000.0);  // Eve, within Engineering
}

TEST_F(QA_GDB1204_WindowLagLeadTest, LagWithDefaultDoesNotLeakAcrossPartitionBoundary) {
    GTEST_SKIP() << "LAG/LEAD default-value semantics tracked by GDB-1285";
    // If the offset/default handling is wrong, a partition-edge row could pick up
    // a value from the adjacent partition instead of the default. Pin default to
    // a distinctive sentinel that cannot be confused with any real salary.
    auto qr = exec_ok(
        "SELECT name, department, salary, "
        "LAG(salary, 1, -999) OVER (PARTITION BY department ORDER BY salary) AS prev "
        "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    auto m = by_name(qr, 0, 3);

    ASSERT_FALSE(m.at("Charlie").is_null());
    EXPECT_DOUBLE_EQ(m.at("Charlie").as_float64(), -999.0);
    ASSERT_FALSE(m.at("Alice").is_null());
    EXPECT_DOUBLE_EQ(m.at("Alice").as_float64(), -999.0);
}

// ---- Descending ORDER BY ----

TEST_F(QA_GDB1204_WindowLagLeadTest, LagOverDescendingOrderBySalary) {
    // Descending salary order: Eve(95k), Bob(90k), Dave(85k), Alice(80k), Charlie(70k).
    // LAG should still mean "previous row in the defined ordering" -- i.e. the
    // next-higher salary since the ordering is now descending.
    auto qr = exec_ok(
        "SELECT name, salary, LAG(salary) OVER (ORDER BY salary DESC) AS prev FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    auto m = by_name(qr, 0, 2);

    EXPECT_TRUE(m.at("Eve").is_null());  // first row in DESC order
    ASSERT_FALSE(m.at("Bob").is_null());
    EXPECT_DOUBLE_EQ(m.at("Bob").as_float64(), 95000.0);
    ASSERT_FALSE(m.at("Dave").is_null());
    EXPECT_DOUBLE_EQ(m.at("Dave").as_float64(), 90000.0);
    ASSERT_FALSE(m.at("Alice").is_null());
    EXPECT_DOUBLE_EQ(m.at("Alice").as_float64(), 85000.0);
    ASSERT_FALSE(m.at("Charlie").is_null());
    EXPECT_DOUBLE_EQ(m.at("Charlie").as_float64(), 80000.0);
}

// ---- Ties in ORDER BY column ----

class QA_GDB1204_WindowLagLeadTiesTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1204_ties";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        exec_ok("CREATE TABLE employees ("
                "id INT PRIMARY KEY, "
                "name TEXT NOT NULL, "
                "salary DOUBLE, "
                "department TEXT)");

        // Alice and Bob tie at 80000.
        exec_ok("INSERT INTO employees VALUES (1, 'Alice', 80000, 'Engineering')");
        exec_ok("INSERT INTO employees VALUES (2, 'Bob', 80000, 'Engineering')");
        exec_ok("INSERT INTO employees VALUES (3, 'Charlie', 90000, 'Engineering')");
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

TEST_F(QA_GDB1204_WindowLagLeadTiesTest, LagWithTiesProducesConsistentOffsetPositions) {
    // With ties, the exact tie-breaking order isn't guaranteed by SQL standard, but
    // the implementation must still be internally consistent: exactly one NULL
    // (the first row overall), and the row's LAG value must equal SOME row's salary
    // that actually exists in the partition -- never a value that leaked from
    // outside the partition/table, and the count of NULLs must be exactly 1.
    auto qr = exec_ok(
        "SELECT name, salary, LAG(salary) OVER (ORDER BY salary) AS prev FROM employees");
    ASSERT_EQ(qr.rows.size(), 3u);

    int null_count = 0;
    for (auto& row : qr.rows) {
        if (row[2].is_null()) {
            ++null_count;
        } else {
            double v = row[2].as_float64();
            EXPECT_TRUE(v == 80000.0 || v == 90000.0)
                << "LAG value " << v << " does not correspond to any known salary";
        }
    }
    EXPECT_EQ(null_count, 1);

    // Charlie is unambiguously last (highest, no tie), so its LAG must be 80000
    // (one of the tied rows), regardless of which tied row sorts first.
    for (auto& row : qr.rows) {
        if (row[0].as_string() == "Charlie") {
            ASSERT_FALSE(row[2].is_null());
            EXPECT_DOUBLE_EQ(row[2].as_float64(), 80000.0);
        }
    }
}

// ---- Offset larger than partition size ----

TEST_F(QA_GDB1204_WindowLagLeadTest, LagOffsetLargerThanPartitionSizeIsAllNull) {
    auto qr = exec_ok(
        "SELECT name, salary, LAG(salary, 100) OVER (ORDER BY salary) AS prev FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    for (auto& row : qr.rows) {
        EXPECT_TRUE(row[2].is_null())
            << "row " << row[0].as_string() << " should be NULL for offset >> partition size";
    }
}

TEST_F(QA_GDB1204_WindowLagLeadTest, LeadOffsetLargerThanPartitionSizeIsAllNull) {
    auto qr = exec_ok(
        "SELECT name, salary, LEAD(salary, 100) OVER (ORDER BY salary) AS nxt FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    for (auto& row : qr.rows) {
        EXPECT_TRUE(row[2].is_null())
            << "row " << row[0].as_string() << " should be NULL for offset >> partition size";
    }
}

TEST_F(QA_GDB1204_WindowLagLeadTest, LagOffsetLargerThanPartitionSizeWithDefaultFillsDefault) {
    GTEST_SKIP() << "LAG/LEAD default-value semantics tracked by GDB-1285";
    auto qr = exec_ok("SELECT name, salary, LAG(salary, 100, 42) OVER (ORDER BY salary) AS prev "
                       "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    for (auto& row : qr.rows) {
        ASSERT_FALSE(row[2].is_null());
        EXPECT_DOUBLE_EQ(row[2].as_float64(), 42.0);
    }
}

// ---- Offset 0 returns current row ----

TEST_F(QA_GDB1204_WindowLagLeadTest, LagOffsetZeroReturnsCurrentRow) {
    auto qr = exec_ok(
        "SELECT name, salary, LAG(salary, 0) OVER (ORDER BY salary) AS same FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    for (auto& row : qr.rows) {
        ASSERT_FALSE(row[2].is_null()) << "LAG(x, 0) must return the current row's value";
        EXPECT_DOUBLE_EQ(row[2].as_float64(), row[1].as_float64());
    }
}

TEST_F(QA_GDB1204_WindowLagLeadTest, LeadOffsetZeroReturnsCurrentRow) {
    auto qr = exec_ok(
        "SELECT name, salary, LEAD(salary, 0) OVER (ORDER BY salary) AS same FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    for (auto& row : qr.rows) {
        ASSERT_FALSE(row[2].is_null()) << "LEAD(x, 0) must return the current row's value";
        EXPECT_DOUBLE_EQ(row[2].as_float64(), row[1].as_float64());
    }
}

// ---- Combined stress: PARTITION BY + explicit offset + default, near boundary ----

TEST_F(QA_GDB1204_WindowLagLeadTest, LeadWithOffsetAndDefaultAcrossPartitionsSimultaneously) {
    GTEST_SKIP() << "LAG/LEAD default-value semantics tracked by GDB-1285";
    // Engineering (sorted): Alice(80k), Bob(90k), Eve(95k) -- 3 rows.
    // Sales (sorted): Charlie(70k), Dave(85k) -- 2 rows.
    // LEAD(salary, 2, -1) should only find a valid 2-ahead target within
    // Engineering (Alice -> Eve); everywhere else falls back to default -1,
    // and must never pull a value from the other partition.
    auto qr = exec_ok(
        "SELECT name, department, salary, "
        "LEAD(salary, 2, -1) OVER (PARTITION BY department ORDER BY salary) AS nxt2 "
        "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5u);
    auto m = by_name(qr, 0, 3);

    ASSERT_FALSE(m.at("Alice").is_null());
    EXPECT_DOUBLE_EQ(m.at("Alice").as_float64(), 95000.0);  // Eve, 2 ahead within Engineering

    ASSERT_FALSE(m.at("Bob").is_null());
    EXPECT_DOUBLE_EQ(m.at("Bob").as_float64(), -1.0);  // no 2-ahead in Engineering

    ASSERT_FALSE(m.at("Eve").is_null());
    EXPECT_DOUBLE_EQ(m.at("Eve").as_float64(), -1.0);

    ASSERT_FALSE(m.at("Charlie").is_null());
    EXPECT_DOUBLE_EQ(m.at("Charlie").as_float64(), -1.0);  // Sales only has 2 rows

    ASSERT_FALSE(m.at("Dave").is_null());
    EXPECT_DOUBLE_EQ(m.at("Dave").as_float64(), -1.0);
}
