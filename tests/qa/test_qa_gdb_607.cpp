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

using namespace sixseven;

// =============================================================================
// QA regression test for GDB-607:
// Window functions use wrong default frame when ORDER BY is absent
//
// Bug: When a window function has no ORDER BY clause, the default frame should
// be ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING (entire partition).
// The implementation was incorrectly defaulting to UNBOUNDED PRECEDING AND
// CURRENT ROW, producing running aggregates instead of partition totals.
// =============================================================================

class GDB607WindowFrameDefaultQA : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb607";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        exec_ok("CREATE TABLE t (id INT PRIMARY KEY, grp TEXT)");
        exec_ok("INSERT INTO t VALUES (1, 'A')");
        exec_ok("INSERT INTO t VALUES (2, 'A')");
        exec_ok("INSERT INTO t VALUES (3, 'A')");
        exec_ok("INSERT INTO t VALUES (4, 'B')");
        exec_ok("INSERT INTO t VALUES (5, 'B')");
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
        return std::move(*result);
    }

    static int64_t get_int(const Value& v) {
        switch (v.type_id()) {
        case TypeId::INT32:
            return v.as_int32();
        case TypeId::INT64:
            return v.as_int64();
        default:
            ADD_FAILURE() << "get_int: unexpected type " << static_cast<int>(v.type_id());
            return 0;
        }
    }

    static double get_double(const Value& v) {
        switch (v.type_id()) {
        case TypeId::FLOAT32:
            return v.as_float32();
        case TypeId::FLOAT64:
            return v.as_float64();
        case TypeId::INT32:
            return static_cast<double>(v.as_int32());
        case TypeId::INT64:
            return static_cast<double>(v.as_int64());
        default:
            ADD_FAILURE() << "get_double: unexpected type " << static_cast<int>(v.type_id());
            return 0.0;
        }
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// Ticket reproduction: exact steps from the bug report
// =============================================================================

// Reproduce exact steps from ticket: COUNT(*) OVER (PARTITION BY grp) should
// return partition totals, not running counts.
TEST_F(GDB607WindowFrameDefaultQA, ReproSteps_CountPartitionByGrp) {
    auto qr = exec_ok("SELECT id, grp, COUNT(*) OVER (PARTITION BY grp) AS total FROM t");
    ASSERT_EQ(qr.rows.size(), 5);
    for (auto& row : qr.rows) {
        auto grp = row[1].as_string();
        auto total = get_int(row[2]);
        if (grp == "A") {
            EXPECT_EQ(total, 3) << "All rows in partition 'A' should show total=3";
        } else {
            EXPECT_EQ(total, 2) << "All rows in partition 'B' should show total=2";
        }
    }
}

// COUNT(*) OVER () — no partition, no order by — should return total row count.
TEST_F(GDB607WindowFrameDefaultQA, CountOverEmpty) {
    auto qr = exec_ok("SELECT id, COUNT(*) OVER () AS total FROM t");
    ASSERT_EQ(qr.rows.size(), 5);
    for (auto& row : qr.rows) {
        EXPECT_EQ(get_int(row[1]), 5) << "COUNT(*) OVER () should return 5 for all rows";
    }
}

// COUNT(*) OVER (ORDER BY id) should still produce running counts (default
// frame WITH ORDER BY is UNBOUNDED PRECEDING to CURRENT ROW).
TEST_F(GDB607WindowFrameDefaultQA, CountWithOrderByIsRunning) {
    auto qr = exec_ok("SELECT id, COUNT(*) OVER (ORDER BY id) AS running FROM t");
    ASSERT_EQ(qr.rows.size(), 5);
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(get_int(qr.rows[i][1]), static_cast<int64_t>(i + 1));
    }
}

// Explicit frame should override the default regardless of ORDER BY presence.
TEST_F(GDB607WindowFrameDefaultQA, ExplicitFrameOverrides) {
    auto qr = exec_ok("SELECT id, COUNT(*) OVER ("
                      "ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW"
                      ") AS running FROM t");
    ASSERT_EQ(qr.rows.size(), 5);
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(get_int(qr.rows[i][1]), static_cast<int64_t>(i + 1));
    }
}

// =============================================================================
// Adversarial: other aggregate functions without ORDER BY
// =============================================================================

// SUM without ORDER BY should produce partition total, not running sum.
TEST_F(GDB607WindowFrameDefaultQA, SumOverPartitionByNoOrderBy) {
    // Create table with numeric column for SUM testing.
    exec_ok("CREATE TABLE nums (id INT PRIMARY KEY, grp TEXT, val INT)");
    exec_ok("INSERT INTO nums VALUES (1, 'A', 10)");
    exec_ok("INSERT INTO nums VALUES (2, 'A', 20)");
    exec_ok("INSERT INTO nums VALUES (3, 'A', 30)");
    exec_ok("INSERT INTO nums VALUES (4, 'B', 100)");
    exec_ok("INSERT INTO nums VALUES (5, 'B', 200)");

    auto qr = exec_ok("SELECT id, grp, SUM(val) OVER (PARTITION BY grp) AS total FROM nums");
    ASSERT_EQ(qr.rows.size(), 5);
    for (auto& row : qr.rows) {
        auto grp = row[1].as_string();
        auto total = get_int(row[2]);
        if (grp == "A") {
            EXPECT_EQ(total, 60) << "SUM for partition 'A' should be 60 (10+20+30)";
        } else {
            EXPECT_EQ(total, 300) << "SUM for partition 'B' should be 300 (100+200)";
        }
    }
}

// AVG without ORDER BY should average entire partition.
TEST_F(GDB607WindowFrameDefaultQA, AvgOverPartitionByNoOrderBy) {
    exec_ok("CREATE TABLE nums2 (id INT PRIMARY KEY, grp TEXT, val DOUBLE)");
    exec_ok("INSERT INTO nums2 VALUES (1, 'A', 10.0)");
    exec_ok("INSERT INTO nums2 VALUES (2, 'A', 20.0)");
    exec_ok("INSERT INTO nums2 VALUES (3, 'A', 30.0)");
    exec_ok("INSERT INTO nums2 VALUES (4, 'B', 100.0)");
    exec_ok("INSERT INTO nums2 VALUES (5, 'B', 200.0)");

    auto qr = exec_ok("SELECT id, grp, AVG(val) OVER (PARTITION BY grp) AS avg_val FROM nums2");
    ASSERT_EQ(qr.rows.size(), 5);
    for (auto& row : qr.rows) {
        auto grp = row[1].as_string();
        auto avg_val = get_double(row[2]);
        if (grp == "A") {
            EXPECT_DOUBLE_EQ(avg_val, 20.0) << "AVG for 'A' should be 20.0";
        } else {
            EXPECT_DOUBLE_EQ(avg_val, 150.0) << "AVG for 'B' should be 150.0";
        }
    }
}

// MIN without ORDER BY should return min of entire partition.
TEST_F(GDB607WindowFrameDefaultQA, MinOverPartitionByNoOrderBy) {
    exec_ok("CREATE TABLE nums3 (id INT PRIMARY KEY, grp TEXT, val INT)");
    exec_ok("INSERT INTO nums3 VALUES (1, 'A', 30)");
    exec_ok("INSERT INTO nums3 VALUES (2, 'A', 10)");
    exec_ok("INSERT INTO nums3 VALUES (3, 'A', 20)");
    exec_ok("INSERT INTO nums3 VALUES (4, 'B', 200)");
    exec_ok("INSERT INTO nums3 VALUES (5, 'B', 100)");

    auto qr = exec_ok("SELECT id, grp, MIN(val) OVER (PARTITION BY grp) AS min_val FROM nums3");
    ASSERT_EQ(qr.rows.size(), 5);
    for (auto& row : qr.rows) {
        auto grp = row[1].as_string();
        auto min_val = get_int(row[2]);
        if (grp == "A") {
            EXPECT_EQ(min_val, 10) << "MIN for 'A' should be 10";
        } else {
            EXPECT_EQ(min_val, 100) << "MIN for 'B' should be 100";
        }
    }
}

// MAX without ORDER BY should return max of entire partition.
TEST_F(GDB607WindowFrameDefaultQA, MaxOverPartitionByNoOrderBy) {
    exec_ok("CREATE TABLE nums4 (id INT PRIMARY KEY, grp TEXT, val INT)");
    exec_ok("INSERT INTO nums4 VALUES (1, 'A', 30)");
    exec_ok("INSERT INTO nums4 VALUES (2, 'A', 10)");
    exec_ok("INSERT INTO nums4 VALUES (3, 'A', 20)");
    exec_ok("INSERT INTO nums4 VALUES (4, 'B', 200)");
    exec_ok("INSERT INTO nums4 VALUES (5, 'B', 100)");

    auto qr = exec_ok("SELECT id, grp, MAX(val) OVER (PARTITION BY grp) AS max_val FROM nums4");
    ASSERT_EQ(qr.rows.size(), 5);
    for (auto& row : qr.rows) {
        auto grp = row[1].as_string();
        auto max_val = get_int(row[2]);
        if (grp == "A") {
            EXPECT_EQ(max_val, 30) << "MAX for 'A' should be 30";
        } else {
            EXPECT_EQ(max_val, 200) << "MAX for 'B' should be 200";
        }
    }
}

// SUM over empty OVER () — entire table as one partition.
TEST_F(GDB607WindowFrameDefaultQA, SumOverEmptyClause) {
    exec_ok("CREATE TABLE nums5 (id INT PRIMARY KEY, val INT)");
    exec_ok("INSERT INTO nums5 VALUES (1, 10)");
    exec_ok("INSERT INTO nums5 VALUES (2, 20)");
    exec_ok("INSERT INTO nums5 VALUES (3, 30)");

    auto qr = exec_ok("SELECT id, SUM(val) OVER () AS total FROM nums5");
    ASSERT_EQ(qr.rows.size(), 3);
    for (auto& row : qr.rows) {
        EXPECT_EQ(get_int(row[1]), 60) << "SUM OVER () should be 60 for all rows";
    }
}

// =============================================================================
// Adversarial: edge cases — single row, single-row partitions
// =============================================================================

// Single row table — COUNT(*) OVER () should return 1.
TEST_F(GDB607WindowFrameDefaultQA, SingleRowTable) {
    exec_ok("CREATE TABLE single (id INT PRIMARY KEY, val INT)");
    exec_ok("INSERT INTO single VALUES (1, 42)");

    auto qr = exec_ok("SELECT id, COUNT(*) OVER () AS total FROM single");
    ASSERT_EQ(qr.rows.size(), 1);
    EXPECT_EQ(get_int(qr.rows[0][1]), 1);
}

// Each row is its own partition — each should show count=1.
TEST_F(GDB607WindowFrameDefaultQA, SingleRowPartitions) {
    auto qr = exec_ok("SELECT id, COUNT(*) OVER (PARTITION BY id) AS total FROM t");
    ASSERT_EQ(qr.rows.size(), 5);
    for (auto& row : qr.rows) {
        EXPECT_EQ(get_int(row[1]), 1) << "Each id is unique, partition count should be 1";
    }
}

// =============================================================================
// Adversarial: empty table
// =============================================================================

TEST_F(GDB607WindowFrameDefaultQA, EmptyTable) {
    exec_ok("CREATE TABLE empty_tbl (id INT PRIMARY KEY, grp TEXT)");

    auto qr = exec_ok("SELECT id, COUNT(*) OVER () AS total FROM empty_tbl");
    EXPECT_EQ(qr.rows.size(), 0);
}

// =============================================================================
// Adversarial: NULL partition keys
// =============================================================================

// NULLs in PARTITION BY column — NULLs should be grouped together.
TEST_F(GDB607WindowFrameDefaultQA, NullPartitionKeys) {
    exec_ok("CREATE TABLE nullgrp (id INT PRIMARY KEY, grp TEXT)");
    exec_ok("INSERT INTO nullgrp VALUES (1, 'A')");
    exec_ok("INSERT INTO nullgrp VALUES (2, NULL)");
    exec_ok("INSERT INTO nullgrp VALUES (3, NULL)");
    exec_ok("INSERT INTO nullgrp VALUES (4, 'A')");

    auto qr =
        exec_ok("SELECT id, grp, COUNT(*) OVER (PARTITION BY grp) AS total FROM nullgrp");
    ASSERT_EQ(qr.rows.size(), 4);
    for (auto& row : qr.rows) {
        auto total = get_int(row[2]);
        if (row[1].is_null()) {
            EXPECT_EQ(total, 2) << "NULL partition should have 2 rows";
        } else {
            EXPECT_EQ(total, 2) << "Partition 'A' should have 2 rows";
        }
    }
}

// =============================================================================
// Adversarial: multiple window functions in same query
// =============================================================================

// Two window functions: one with ORDER BY (running), one without (partition total).
TEST_F(GDB607WindowFrameDefaultQA, MixedOrderByAndNoOrderBy) {
    auto qr = exec_ok("SELECT id, "
                      "COUNT(*) OVER () AS total, "
                      "COUNT(*) OVER (ORDER BY id) AS running "
                      "FROM t");
    ASSERT_EQ(qr.rows.size(), 5);
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(get_int(qr.rows[i][1]), 5) << "OVER () total should be 5";
        EXPECT_EQ(get_int(qr.rows[i][2]), static_cast<int64_t>(i + 1))
            << "OVER (ORDER BY id) should be running count";
    }
}

// Two window functions on partitioned data: one with ORDER BY, one without.
TEST_F(GDB607WindowFrameDefaultQA, MixedWithPartitionBy) {
    auto qr = exec_ok("SELECT id, grp, "
                      "COUNT(*) OVER (PARTITION BY grp) AS total, "
                      "COUNT(*) OVER (PARTITION BY grp ORDER BY id) AS running "
                      "FROM t");
    ASSERT_EQ(qr.rows.size(), 5);
    for (auto& row : qr.rows) {
        auto grp = row[1].as_string();
        auto total = get_int(row[2]);
        auto running = get_int(row[3]);

        if (grp == "A") {
            EXPECT_EQ(total, 3) << "Partition total for 'A' should be 3";
            EXPECT_GE(running, 1);
            EXPECT_LE(running, 3);
        } else {
            EXPECT_EQ(total, 2) << "Partition total for 'B' should be 2";
            EXPECT_GE(running, 1);
            EXPECT_LE(running, 2);
        }
    }
}

// =============================================================================
// Adversarial: FIRST_VALUE / LAST_VALUE without ORDER BY
// =============================================================================

// LAST_VALUE without ORDER BY — frame covers whole partition, so LAST_VALUE
// should be the last row in the partition (not the current row).
TEST_F(GDB607WindowFrameDefaultQA, LastValueNoOrderBy) {
    exec_ok("CREATE TABLE lv (id INT PRIMARY KEY, grp TEXT, val INT)");
    exec_ok("INSERT INTO lv VALUES (1, 'A', 10)");
    exec_ok("INSERT INTO lv VALUES (2, 'A', 20)");
    exec_ok("INSERT INTO lv VALUES (3, 'A', 30)");

    auto qr = exec_ok("SELECT id, LAST_VALUE(val) OVER (PARTITION BY grp) AS lv FROM lv");
    ASSERT_EQ(qr.rows.size(), 3);

    // Without ORDER BY, default frame is entire partition.
    // All rows should see the same LAST_VALUE (the last row in insertion order).
    int64_t first_lv = get_int(qr.rows[0][1]);
    for (auto& row : qr.rows) {
        EXPECT_EQ(get_int(row[1]), first_lv)
            << "LAST_VALUE without ORDER BY should be same for all rows in partition";
    }
}

// FIRST_VALUE without ORDER BY — frame covers whole partition.
TEST_F(GDB607WindowFrameDefaultQA, FirstValueNoOrderBy) {
    exec_ok("CREATE TABLE fv (id INT PRIMARY KEY, grp TEXT, val INT)");
    exec_ok("INSERT INTO fv VALUES (1, 'A', 10)");
    exec_ok("INSERT INTO fv VALUES (2, 'A', 20)");
    exec_ok("INSERT INTO fv VALUES (3, 'A', 30)");

    auto qr = exec_ok("SELECT id, FIRST_VALUE(val) OVER (PARTITION BY grp) AS fv FROM fv");
    ASSERT_EQ(qr.rows.size(), 3);

    // All rows in the partition should see the same FIRST_VALUE.
    int64_t first_fv = get_int(qr.rows[0][1]);
    for (auto& row : qr.rows) {
        EXPECT_EQ(get_int(row[1]), first_fv)
            << "FIRST_VALUE without ORDER BY should be same for all rows";
    }
}

// =============================================================================
// Adversarial: explicit frame WITHOUT ORDER BY should be respected
// =============================================================================

// Explicit CURRENT ROW frame without ORDER BY — should still produce running counts.
TEST_F(GDB607WindowFrameDefaultQA, ExplicitCurrentRowNoOrderBy) {
    auto qr = exec_ok("SELECT id, COUNT(*) OVER ("
                      "ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW"
                      ") AS running FROM t");
    ASSERT_EQ(qr.rows.size(), 5);
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(get_int(qr.rows[i][1]), static_cast<int64_t>(i + 1))
            << "Explicit CURRENT ROW should give running count even without ORDER BY";
    }
}

// Explicit UNBOUNDED FOLLOWING frame WITH ORDER BY — should give partition total.
TEST_F(GDB607WindowFrameDefaultQA, ExplicitUnboundedFollowingWithOrderBy) {
    auto qr = exec_ok("SELECT id, COUNT(*) OVER ("
                      "ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING"
                      ") AS total FROM t");
    ASSERT_EQ(qr.rows.size(), 5);
    for (auto& row : qr.rows) {
        EXPECT_EQ(get_int(row[1]), 5)
            << "Explicit UNBOUNDED FOLLOWING with ORDER BY should give total";
    }
}

// =============================================================================
// Adversarial: stress test — many rows, single partition
// =============================================================================

TEST_F(GDB607WindowFrameDefaultQA, StressManyRowsSinglePartition) {
    exec_ok("CREATE TABLE stress (id INT PRIMARY KEY, val INT)");
    const int N = 200;
    for (int i = 1; i <= N; ++i) {
        exec_ok("INSERT INTO stress VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")");
    }

    auto qr = exec_ok("SELECT id, COUNT(*) OVER () AS total, "
                      "SUM(val) OVER () AS sum_total FROM stress");
    ASSERT_EQ(qr.rows.size(), static_cast<size_t>(N));

    int64_t expected_sum = static_cast<int64_t>(N) * (N + 1) / 2;
    for (auto& row : qr.rows) {
        EXPECT_EQ(get_int(row[1]), N) << "COUNT(*) OVER () should be " << N;
        EXPECT_EQ(get_int(row[2]), expected_sum) << "SUM OVER () should be " << expected_sum;
    }
}

// =============================================================================
// Adversarial: many partitions, each with one row
// =============================================================================

TEST_F(GDB607WindowFrameDefaultQA, StressManyPartitions) {
    exec_ok("CREATE TABLE many_parts (id INT PRIMARY KEY, grp INT, val INT)");
    const int N = 100;
    for (int i = 1; i <= N; ++i) {
        exec_ok("INSERT INTO many_parts VALUES (" + std::to_string(i) + ", " +
                std::to_string(i) + ", " + std::to_string(i * 10) + ")");
    }

    auto qr = exec_ok("SELECT id, SUM(val) OVER (PARTITION BY grp) AS total FROM many_parts");
    ASSERT_EQ(qr.rows.size(), static_cast<size_t>(N));
    for (auto& row : qr.rows) {
        // Each partition has exactly one row, so SUM should equal the row's val.
        auto id = get_int(row[0]);
        auto total = get_int(row[1]);
        EXPECT_EQ(total, id * 10) << "Single-row partition SUM should equal the row value";
    }
}

// =============================================================================
// Adversarial: ranking functions should NOT be affected by frame default change
// =============================================================================

// ROW_NUMBER OVER () — should still number 1..N regardless of frame default.
TEST_F(GDB607WindowFrameDefaultQA, RowNumberUnaffectedByFrameChange) {
    auto qr = exec_ok("SELECT id, ROW_NUMBER() OVER () AS rn FROM t");
    ASSERT_EQ(qr.rows.size(), 5);
    for (auto& row : qr.rows) {
        auto rn = get_int(row[1]);
        EXPECT_GE(rn, 1);
        EXPECT_LE(rn, 5);
    }
}

// RANK OVER (PARTITION BY grp) — should not be affected by frame.
TEST_F(GDB607WindowFrameDefaultQA, RankUnaffectedByFrameChange) {
    exec_ok("CREATE TABLE rank_tbl (id INT PRIMARY KEY, grp TEXT, val INT)");
    exec_ok("INSERT INTO rank_tbl VALUES (1, 'A', 10)");
    exec_ok("INSERT INTO rank_tbl VALUES (2, 'A', 20)");
    exec_ok("INSERT INTO rank_tbl VALUES (3, 'B', 30)");

    auto qr = exec_ok("SELECT id, RANK() OVER (PARTITION BY grp ORDER BY val) AS rnk "
                      "FROM rank_tbl");
    ASSERT_EQ(qr.rows.size(), 3);
    for (auto& row : qr.rows) {
        EXPECT_GE(get_int(row[1]), 1);
    }
}
