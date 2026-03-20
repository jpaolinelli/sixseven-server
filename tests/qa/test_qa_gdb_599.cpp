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
#include <vector>

using namespace sixseven;

// =============================================================================
// QA Test fixture for GDB-599: Window function column resolution fix
// =============================================================================

class GDB599WindowFunctionQA : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb599";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
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

    void exec_fail(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "Expected failure for SQL: " << sql;
    }

    /// Extract an integer from a Value regardless of INT32/INT64 storage.
    static int64_t get_int(const Value& v) {
        if (v.is_null()) {
            ADD_FAILURE() << "get_int called on NULL value";
            return 0;
        }
        switch (v.type_id()) {
        case TypeId::INT8:
            return v.as_int8();
        case TypeId::INT16:
            return v.as_int16();
        case TypeId::INT32:
            return v.as_int32();
        case TypeId::INT64:
            return v.as_int64();
        case TypeId::UINT8:
            return v.as_uint8();
        case TypeId::UINT16:
            return v.as_uint16();
        case TypeId::UINT32:
            return v.as_uint32();
        case TypeId::UINT64:
            return static_cast<int64_t>(v.as_uint64());
        default:
            ADD_FAILURE() << "get_int called on non-integer type: "
                          << static_cast<int>(v.type_id());
            return 0;
        }
    }

    /// Extract a double from a Value regardless of numeric storage.
    static double get_double(const Value& v) {
        if (v.is_null()) {
            ADD_FAILURE() << "get_double called on NULL value";
            return 0.0;
        }
        switch (v.type_id()) {
        case TypeId::FLOAT32:
            return v.as_float32();
        case TypeId::FLOAT64:
            return v.as_float64();
        case TypeId::INT32:
            return v.as_int32();
        case TypeId::INT64:
            return v.as_int64();
        default:
            ADD_FAILURE() << "get_double called on unexpected type: "
                          << static_cast<int>(v.type_id());
            return 0.0;
        }
    }

    void setup_employees() {
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

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// AC1: Bug report reproduction — exact queries from the ticket
// =============================================================================

TEST_F(GDB599WindowFunctionQA, AC1_RowNumberOverOrderById) {
    setup_employees();
    auto qr = exec_ok("SELECT id, ROW_NUMBER() OVER (ORDER BY id) AS rn FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);
    ASSERT_GE(qr.column_names.size(), 2);
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "rn");
    // Verify sequential row numbers
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(qr.rows[i][1].as_int64(), static_cast<int64_t>(i + 1));
    }
}

TEST_F(GDB599WindowFunctionQA, AC1_LagOverOrderBySalary) {
    setup_employees();
    auto qr =
        exec_ok("SELECT name, salary, LAG(salary) OVER (ORDER BY salary) AS prev FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);
    ASSERT_GE(qr.column_names.size(), 3);
    // Exactly one row should have NULL LAG (the first in salary order)
    int null_count = 0;
    for (auto& row : qr.rows) {
        if (row[2].is_null()) {
            ++null_count;
        }
    }
    EXPECT_EQ(null_count, 1);
}

TEST_F(GDB599WindowFunctionQA, AC1_SelectStarWithRowNumber) {
    setup_employees();
    auto qr = exec_ok("SELECT *, ROW_NUMBER() OVER (ORDER BY id) AS rn FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);
    // Should have id, name, salary, department, rn = 5 columns
    ASSERT_GE(qr.column_names.size(), 5);
    size_t last = qr.column_names.size() - 1;
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(qr.rows[i][last].as_int64(), static_cast<int64_t>(i + 1));
    }
}

// =============================================================================
// Edge case: Empty table
// =============================================================================

TEST_F(GDB599WindowFunctionQA, EmptyTable_WindowFunctionReturnsNoRows) {
    exec_ok("CREATE TABLE empty_t (id INT PRIMARY KEY, val INT)");
    auto qr = exec_ok("SELECT id, ROW_NUMBER() OVER (ORDER BY id) AS rn FROM empty_t");
    EXPECT_EQ(qr.rows.size(), 0);
}

// =============================================================================
// Edge case: Single row
// =============================================================================

TEST_F(GDB599WindowFunctionQA, SingleRow_RowNumber) {
    exec_ok("CREATE TABLE single (id INT PRIMARY KEY, val TEXT)");
    exec_ok("INSERT INTO single VALUES (1, 'only')");
    auto qr = exec_ok("SELECT id, ROW_NUMBER() OVER (ORDER BY id) AS rn FROM single");
    ASSERT_EQ(qr.rows.size(), 1);
    EXPECT_EQ(qr.rows[0][1].as_int64(), 1);
}

TEST_F(GDB599WindowFunctionQA, SingleRow_LagIsNull) {
    exec_ok("CREATE TABLE single2 (id INT PRIMARY KEY, val INT)");
    exec_ok("INSERT INTO single2 VALUES (1, 100)");
    auto qr = exec_ok("SELECT id, LAG(val) OVER (ORDER BY id) AS prev FROM single2");
    ASSERT_EQ(qr.rows.size(), 1);
    EXPECT_TRUE(qr.rows[0][1].is_null());
}

TEST_F(GDB599WindowFunctionQA, SingleRow_LeadIsNull) {
    exec_ok("CREATE TABLE single3 (id INT PRIMARY KEY, val INT)");
    exec_ok("INSERT INTO single3 VALUES (1, 100)");
    auto qr = exec_ok("SELECT id, LEAD(val) OVER (ORDER BY id) AS nxt FROM single3");
    ASSERT_EQ(qr.rows.size(), 1);
    EXPECT_TRUE(qr.rows[0][1].is_null());
}

// =============================================================================
// NULL handling in window function arguments
// =============================================================================

TEST_F(GDB599WindowFunctionQA, NullValues_InArgumentColumn) {
    exec_ok("CREATE TABLE nullvals (id INT PRIMARY KEY, val INT)");
    exec_ok("INSERT INTO nullvals VALUES (1, 10)");
    exec_ok("INSERT INTO nullvals VALUES (2, NULL)");
    exec_ok("INSERT INTO nullvals VALUES (3, 30)");

    auto qr = exec_ok("SELECT id, LAG(val) OVER (ORDER BY id) AS prev FROM nullvals");
    ASSERT_EQ(qr.rows.size(), 3);
    // Row 1 (id=1): LAG is NULL (no previous)
    EXPECT_TRUE(qr.rows[0][1].is_null());
    // Row 2 (id=2): LAG should be 10 (previous row's val)
    EXPECT_EQ(get_int(qr.rows[1][1]), 10);
    // Row 3 (id=3): LAG should be NULL (previous row's val is NULL)
    EXPECT_TRUE(qr.rows[2][1].is_null());
}

TEST_F(GDB599WindowFunctionQA, NullValues_SumOverWithNulls) {
    exec_ok("CREATE TABLE nullsum (id INT PRIMARY KEY, val INT)");
    exec_ok("INSERT INTO nullsum VALUES (1, 10)");
    exec_ok("INSERT INTO nullsum VALUES (2, NULL)");
    exec_ok("INSERT INTO nullsum VALUES (3, 30)");

    auto qr = exec_ok(
        "SELECT id, SUM(val) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) "
        "AS running_sum FROM nullsum");
    ASSERT_EQ(qr.rows.size(), 3);
    // Running sum should skip NULLs: 10, 10, 40
    EXPECT_EQ(get_int(qr.rows[0][1]), 10);
    EXPECT_EQ(get_int(qr.rows[1][1]), 10);
    EXPECT_EQ(get_int(qr.rows[2][1]), 40);
}

// =============================================================================
// NULL handling in PARTITION BY
// =============================================================================

TEST_F(GDB599WindowFunctionQA, NullPartitionBy_GroupsNullsTogether) {
    exec_ok("CREATE TABLE nullpart (id INT PRIMARY KEY, grp TEXT, val INT)");
    exec_ok("INSERT INTO nullpart VALUES (1, 'A', 10)");
    exec_ok("INSERT INTO nullpart VALUES (2, NULL, 20)");
    exec_ok("INSERT INTO nullpart VALUES (3, 'A', 30)");
    exec_ok("INSERT INTO nullpart VALUES (4, NULL, 40)");

    auto qr = exec_ok("SELECT id, grp, ROW_NUMBER() OVER (PARTITION BY grp ORDER BY id) AS rn "
                      "FROM nullpart");
    ASSERT_EQ(qr.rows.size(), 4);

    // Build map of id -> rn for verification
    std::unordered_map<int64_t, int64_t> id_to_rn;
    for (auto& row : qr.rows) {
        id_to_rn[get_int(row[0])] = get_int(row[2]);
    }

    // Partition 'A': id=1 -> rn=1, id=3 -> rn=2
    EXPECT_EQ(id_to_rn[1], 1);
    EXPECT_EQ(id_to_rn[3], 2);
    // Partition NULL: id=2 -> rn=1, id=4 -> rn=2
    EXPECT_EQ(id_to_rn[2], 1);
    EXPECT_EQ(id_to_rn[4], 2);
}

// =============================================================================
// Multiple window functions in same SELECT
// =============================================================================

TEST_F(GDB599WindowFunctionQA, MultipleWindowFunctions_SameSelect) {
    setup_employees();
    auto qr = exec_ok("SELECT id, name, "
                      "ROW_NUMBER() OVER (ORDER BY id) AS rn, "
                      "RANK() OVER (ORDER BY id) AS rnk "
                      "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);
    ASSERT_GE(qr.column_names.size(), 4);

    // Both should be sequential since id has no ties
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(qr.rows[i][2].as_int64(), static_cast<int64_t>(i + 1));
        EXPECT_EQ(qr.rows[i][3].as_int64(), static_cast<int64_t>(i + 1));
    }
}

// =============================================================================
// Window function with WHERE clause
// =============================================================================

TEST_F(GDB599WindowFunctionQA, WindowFunctionWithWhere) {
    setup_employees();
    auto qr = exec_ok("SELECT id, name, ROW_NUMBER() OVER (ORDER BY id) AS rn "
                      "FROM employees WHERE department = 'Engineering'");
    ASSERT_EQ(qr.rows.size(), 3);
    // Row numbers should be 1, 2, 3 for the filtered set
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(qr.rows[i][2].as_int64(), static_cast<int64_t>(i + 1));
    }
}

// =============================================================================
// Window function with LIMIT
// =============================================================================

TEST_F(GDB599WindowFunctionQA, WindowFunctionWithLimit) {
    setup_employees();
    auto qr = exec_ok("SELECT id, ROW_NUMBER() OVER (ORDER BY id) AS rn "
                      "FROM employees LIMIT 3");
    ASSERT_EQ(qr.rows.size(), 3);
}

// =============================================================================
// RANK with ties
// =============================================================================

TEST_F(GDB599WindowFunctionQA, RankWithTies) {
    exec_ok("CREATE TABLE scores (id INT PRIMARY KEY, score INT)");
    exec_ok("INSERT INTO scores VALUES (1, 100)");
    exec_ok("INSERT INTO scores VALUES (2, 90)");
    exec_ok("INSERT INTO scores VALUES (3, 90)");
    exec_ok("INSERT INTO scores VALUES (4, 80)");

    auto qr = exec_ok("SELECT id, score, RANK() OVER (ORDER BY score DESC) AS rnk FROM scores");
    ASSERT_EQ(qr.rows.size(), 4);

    std::unordered_map<int64_t, int64_t> id_to_rank;
    for (auto& row : qr.rows) {
        id_to_rank[get_int(row[0])] = get_int(row[2]);
    }

    // score 100 -> rank 1, score 90 (x2) -> rank 2, score 80 -> rank 4 (skip 3)
    EXPECT_EQ(id_to_rank[1], 1);
    EXPECT_EQ(id_to_rank[2], 2);
    EXPECT_EQ(id_to_rank[3], 2);
    EXPECT_EQ(id_to_rank[4], 4);
}

// =============================================================================
// DENSE_RANK with ties
// =============================================================================

TEST_F(GDB599WindowFunctionQA, DenseRankWithTies) {
    exec_ok("CREATE TABLE scores2 (id INT PRIMARY KEY, score INT)");
    exec_ok("INSERT INTO scores2 VALUES (1, 100)");
    exec_ok("INSERT INTO scores2 VALUES (2, 90)");
    exec_ok("INSERT INTO scores2 VALUES (3, 90)");
    exec_ok("INSERT INTO scores2 VALUES (4, 80)");

    auto qr =
        exec_ok("SELECT id, score, DENSE_RANK() OVER (ORDER BY score DESC) AS dr FROM scores2");
    ASSERT_EQ(qr.rows.size(), 4);

    std::unordered_map<int64_t, int64_t> id_to_dr;
    for (auto& row : qr.rows) {
        id_to_dr[get_int(row[0])] = get_int(row[2]);
    }

    // score 100 -> 1, score 90 (x2) -> 2, score 80 -> 3 (no skip)
    EXPECT_EQ(id_to_dr[1], 1);
    EXPECT_EQ(id_to_dr[2], 2);
    EXPECT_EQ(id_to_dr[3], 2);
    EXPECT_EQ(id_to_dr[4], 3);
}

// =============================================================================
// NTILE
// =============================================================================

TEST_F(GDB599WindowFunctionQA, NtileDistributesEvenly) {
    exec_ok("CREATE TABLE ntile_t (id INT PRIMARY KEY)");
    for (int i = 1; i <= 10; ++i) {
        exec_ok("INSERT INTO ntile_t VALUES (" + std::to_string(i) + ")");
    }

    auto qr = exec_ok("SELECT id, NTILE(3) OVER (ORDER BY id) AS bucket FROM ntile_t");
    ASSERT_EQ(qr.rows.size(), 10);

    // 10 rows into 3 buckets: 4, 3, 3
    std::vector<int64_t> bucket_counts(4, 0);
    for (auto& row : qr.rows) {
        auto b = row[1].as_int64();
        EXPECT_GE(b, 1);
        EXPECT_LE(b, 3);
        bucket_counts[static_cast<size_t>(b)]++;
    }
    EXPECT_EQ(bucket_counts[1], 4); // first bucket gets extra row
    EXPECT_EQ(bucket_counts[2], 3);
    EXPECT_EQ(bucket_counts[3], 3);
}

TEST_F(GDB599WindowFunctionQA, NtileSingleBucket) {
    exec_ok("CREATE TABLE ntile_single (id INT PRIMARY KEY)");
    exec_ok("INSERT INTO ntile_single VALUES (1)");
    exec_ok("INSERT INTO ntile_single VALUES (2)");

    auto qr = exec_ok("SELECT id, NTILE(1) OVER (ORDER BY id) AS bucket FROM ntile_single");
    ASSERT_EQ(qr.rows.size(), 2);
    for (auto& row : qr.rows) {
        EXPECT_EQ(row[1].as_int64(), 1);
    }
}

// =============================================================================
// LEAD with offset
// =============================================================================

TEST_F(GDB599WindowFunctionQA, LeadWithOffset2) {
    exec_ok("CREATE TABLE lead_t (id INT PRIMARY KEY, val INT)");
    for (int i = 1; i <= 5; ++i) {
        exec_ok("INSERT INTO lead_t VALUES (" + std::to_string(i) + ", " + std::to_string(i * 10) +
                ")");
    }

    auto qr = exec_ok("SELECT id, LEAD(val, 2) OVER (ORDER BY id) AS nxt2 FROM lead_t");
    ASSERT_EQ(qr.rows.size(), 5);

    std::unordered_map<int64_t, Value> id_to_lead;
    for (auto& row : qr.rows) {
        id_to_lead.emplace(get_int(row[0]), row[1]);
    }

    // id=1 -> val at id=3 = 30, id=2 -> 40, id=3 -> 50, id=4 -> NULL, id=5 -> NULL
    EXPECT_EQ(get_int(id_to_lead[1]), 30);
    EXPECT_EQ(get_int(id_to_lead[2]), 40);
    EXPECT_EQ(get_int(id_to_lead[3]), 50);
    EXPECT_TRUE(id_to_lead[4].is_null());
    EXPECT_TRUE(id_to_lead[5].is_null());
}

// =============================================================================
// LAG with large offset (beyond partition size)
// =============================================================================

TEST_F(GDB599WindowFunctionQA, LagLargeOffset_AllNull) {
    exec_ok("CREATE TABLE lag_big (id INT PRIMARY KEY, val INT)");
    exec_ok("INSERT INTO lag_big VALUES (1, 10)");
    exec_ok("INSERT INTO lag_big VALUES (2, 20)");

    auto qr = exec_ok("SELECT id, LAG(val, 100) OVER (ORDER BY id) AS prev FROM lag_big");
    ASSERT_EQ(qr.rows.size(), 2);
    EXPECT_TRUE(qr.rows[0][1].is_null());
    EXPECT_TRUE(qr.rows[1][1].is_null());
}

// =============================================================================
// Empty OVER clause
// =============================================================================

TEST_F(GDB599WindowFunctionQA, EmptyOver_RowNumber) {
    setup_employees();
    auto qr = exec_ok("SELECT id, ROW_NUMBER() OVER () AS rn FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);
    // All rows should get a row number (order is implementation-defined)
    for (auto& row : qr.rows) {
        EXPECT_GE(row[1].as_int64(), 1);
        EXPECT_LE(row[1].as_int64(), 5);
    }
}

TEST_F(GDB599WindowFunctionQA, EmptyOver_CountStar) {
    setup_employees();
    auto qr = exec_ok("SELECT id, COUNT(*) OVER () AS total FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);
    // Fixed by GDB-607: COUNT(*) OVER () without ORDER BY uses full-partition frame
    // (UNBOUNDED PRECEDING to UNBOUNDED FOLLOWING per SQL standard) and returns 5
    // for every row.
    for (auto& row : qr.rows) {
        EXPECT_EQ(get_int(row[1]), 5)
            << "COUNT(*) OVER () should return total row count for every row";
    }
}

// =============================================================================
// Frame specification: ROWS BETWEEN N PRECEDING AND N FOLLOWING
// =============================================================================

TEST_F(GDB599WindowFunctionQA, FrameSpec_SlidingWindow) {
    exec_ok("CREATE TABLE frame_t (id INT PRIMARY KEY, val INT)");
    for (int i = 1; i <= 5; ++i) {
        exec_ok("INSERT INTO frame_t VALUES (" + std::to_string(i) + ", " + std::to_string(i * 10) +
                ")");
    }

    auto qr = exec_ok("SELECT id, SUM(val) OVER (ORDER BY id "
                      "ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS sliding_sum FROM frame_t");
    ASSERT_EQ(qr.rows.size(), 5);

    std::unordered_map<int64_t, int64_t> id_to_sum;
    for (auto& row : qr.rows) {
        id_to_sum[get_int(row[0])] = get_int(row[1]);
    }

    // id=1: sum(10,20) = 30
    // id=2: sum(10,20,30) = 60
    // id=3: sum(20,30,40) = 90
    // id=4: sum(30,40,50) = 120
    // id=5: sum(40,50) = 90
    EXPECT_EQ(id_to_sum[1], 30);
    EXPECT_EQ(id_to_sum[2], 60);
    EXPECT_EQ(id_to_sum[3], 90);
    EXPECT_EQ(id_to_sum[4], 120);
    EXPECT_EQ(id_to_sum[5], 90);
}

// =============================================================================
// FIRST_VALUE and LAST_VALUE
// =============================================================================

TEST_F(GDB599WindowFunctionQA, FirstValue_Basic) {
    exec_ok("CREATE TABLE fv_t (id INT PRIMARY KEY, val TEXT)");
    exec_ok("INSERT INTO fv_t VALUES (1, 'alpha')");
    exec_ok("INSERT INTO fv_t VALUES (2, 'beta')");
    exec_ok("INSERT INTO fv_t VALUES (3, 'gamma')");

    auto qr = exec_ok("SELECT id, FIRST_VALUE(val) OVER (ORDER BY id) AS first_v FROM fv_t");
    ASSERT_EQ(qr.rows.size(), 3);
    // FIRST_VALUE should always be 'alpha' (first in order)
    for (auto& row : qr.rows) {
        EXPECT_EQ(row[1].as_string(), "alpha");
    }
}

TEST_F(GDB599WindowFunctionQA, LastValue_WithFullFrame) {
    exec_ok("CREATE TABLE lv_t (id INT PRIMARY KEY, val TEXT)");
    exec_ok("INSERT INTO lv_t VALUES (1, 'alpha')");
    exec_ok("INSERT INTO lv_t VALUES (2, 'beta')");
    exec_ok("INSERT INTO lv_t VALUES (3, 'gamma')");

    auto qr =
        exec_ok("SELECT id, LAST_VALUE(val) OVER (ORDER BY id "
                "ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS last_v FROM lv_t");
    ASSERT_EQ(qr.rows.size(), 3);
    // LAST_VALUE with full frame should be 'gamma' for all rows
    for (auto& row : qr.rows) {
        EXPECT_EQ(row[1].as_string(), "gamma");
    }
}

// =============================================================================
// Window aggregate: AVG
// =============================================================================

TEST_F(GDB599WindowFunctionQA, AvgWindowFunction) {
    exec_ok("CREATE TABLE avg_t (id INT PRIMARY KEY, val DOUBLE)");
    exec_ok("INSERT INTO avg_t VALUES (1, 10.0)");
    exec_ok("INSERT INTO avg_t VALUES (2, 20.0)");
    exec_ok("INSERT INTO avg_t VALUES (3, 30.0)");

    auto qr = exec_ok(
        "SELECT id, AVG(val) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) "
        "AS running_avg FROM avg_t");
    ASSERT_EQ(qr.rows.size(), 3);

    // Running averages: 10, 15, 20
    EXPECT_NEAR(qr.rows[0][1].as_float64(), 10.0, 0.001);
    EXPECT_NEAR(qr.rows[1][1].as_float64(), 15.0, 0.001);
    EXPECT_NEAR(qr.rows[2][1].as_float64(), 20.0, 0.001);
}

// =============================================================================
// Window aggregate: MIN and MAX
// =============================================================================

TEST_F(GDB599WindowFunctionQA, MinMaxWindowFunction) {
    exec_ok("CREATE TABLE mm_t (id INT PRIMARY KEY, val INT)");
    exec_ok("INSERT INTO mm_t VALUES (1, 30)");
    exec_ok("INSERT INTO mm_t VALUES (2, 10)");
    exec_ok("INSERT INTO mm_t VALUES (3, 50)");
    exec_ok("INSERT INTO mm_t VALUES (4, 20)");

    auto qr = exec_ok(
        "SELECT id, MIN(val) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) "
        "AS running_min FROM mm_t");
    ASSERT_EQ(qr.rows.size(), 4);

    std::unordered_map<int64_t, int64_t> id_to_min;
    for (auto& row : qr.rows) {
        id_to_min[get_int(row[0])] = get_int(row[1]);
    }

    // Running min: 30, 10, 10, 10
    EXPECT_EQ(id_to_min[1], 30);
    EXPECT_EQ(id_to_min[2], 10);
    EXPECT_EQ(id_to_min[3], 10);
    EXPECT_EQ(id_to_min[4], 10);
}

// =============================================================================
// Error path: Unknown window function
// =============================================================================

TEST_F(GDB599WindowFunctionQA, UnknownWindowFunction_Error) {
    setup_employees();
    exec_fail("SELECT id, FOOBAR() OVER (ORDER BY id) FROM employees");
}

// =============================================================================
// PARTITION BY with multiple columns
// =============================================================================

TEST_F(GDB599WindowFunctionQA, PartitionByMultipleColumns) {
    exec_ok("CREATE TABLE multi_part (id INT PRIMARY KEY, grp1 TEXT, grp2 TEXT, val INT)");
    exec_ok("INSERT INTO multi_part VALUES (1, 'A', 'X', 10)");
    exec_ok("INSERT INTO multi_part VALUES (2, 'A', 'X', 20)");
    exec_ok("INSERT INTO multi_part VALUES (3, 'A', 'Y', 30)");
    exec_ok("INSERT INTO multi_part VALUES (4, 'B', 'X', 40)");

    auto qr = exec_ok("SELECT id, ROW_NUMBER() OVER (PARTITION BY grp1, grp2 ORDER BY id) AS rn "
                      "FROM multi_part");
    ASSERT_EQ(qr.rows.size(), 4);

    std::unordered_map<int64_t, int64_t> id_to_rn;
    for (auto& row : qr.rows) {
        id_to_rn[get_int(row[0])] = get_int(row[1]);
    }

    // Partition (A,X): id=1 -> rn=1, id=2 -> rn=2
    // Partition (A,Y): id=3 -> rn=1
    // Partition (B,X): id=4 -> rn=1
    EXPECT_EQ(id_to_rn[1], 1);
    EXPECT_EQ(id_to_rn[2], 2);
    EXPECT_EQ(id_to_rn[3], 1);
    EXPECT_EQ(id_to_rn[4], 1);
}

// =============================================================================
// Window function with ORDER BY DESC
// =============================================================================

TEST_F(GDB599WindowFunctionQA, OrderByDesc) {
    setup_employees();
    auto qr = exec_ok("SELECT id, ROW_NUMBER() OVER (ORDER BY id DESC) AS rn FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);

    std::unordered_map<int64_t, int64_t> id_to_rn;
    for (auto& row : qr.rows) {
        id_to_rn[get_int(row[0])] = get_int(row[1]);
    }

    // id=5 -> rn=1, id=4 -> rn=2, ..., id=1 -> rn=5
    EXPECT_EQ(id_to_rn[5], 1);
    EXPECT_EQ(id_to_rn[4], 2);
    EXPECT_EQ(id_to_rn[3], 3);
    EXPECT_EQ(id_to_rn[2], 4);
    EXPECT_EQ(id_to_rn[1], 5);
}

// =============================================================================
// Window function column alias used correctly
// =============================================================================

TEST_F(GDB599WindowFunctionQA, ColumnAlias_WindowFunction) {
    setup_employees();
    auto qr =
        exec_ok("SELECT id, ROW_NUMBER() OVER (ORDER BY id) AS my_custom_alias FROM employees");
    ASSERT_EQ(qr.column_names.size(), 2);
    EXPECT_EQ(qr.column_names[1], "my_custom_alias");
}

TEST_F(GDB599WindowFunctionQA, NoAlias_WindowFunction_UsesDefaultName) {
    setup_employees();
    auto qr = exec_ok("SELECT id, ROW_NUMBER() OVER (ORDER BY id) FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);
    // Should still work even without alias
    ASSERT_GE(qr.column_names.size(), 2);
}

// =============================================================================
// Stress test: Many rows
// =============================================================================

TEST_F(GDB599WindowFunctionQA, Stress_100Rows) {
    exec_ok("CREATE TABLE stress (id INT PRIMARY KEY, val INT)");
    for (int i = 1; i <= 100; ++i) {
        exec_ok("INSERT INTO stress VALUES (" + std::to_string(i) + ", " +
                std::to_string(i * 7 % 50) + ")");
    }

    auto qr = exec_ok("SELECT id, ROW_NUMBER() OVER (ORDER BY id) AS rn FROM stress");
    ASSERT_EQ(qr.rows.size(), 100);
    for (size_t i = 0; i < 100; ++i) {
        EXPECT_EQ(qr.rows[i][1].as_int64(), static_cast<int64_t>(i + 1));
    }
}

TEST_F(GDB599WindowFunctionQA, Stress_100Rows_PartitionedRank) {
    exec_ok("CREATE TABLE stress2 (id INT PRIMARY KEY, grp INT, val INT)");
    for (int i = 1; i <= 100; ++i) {
        exec_ok("INSERT INTO stress2 VALUES (" + std::to_string(i) + ", " + std::to_string(i % 5) +
                ", " + std::to_string(i) + ")");
    }

    auto qr = exec_ok("SELECT id, grp, RANK() OVER (PARTITION BY grp ORDER BY val) AS rnk "
                      "FROM stress2");
    ASSERT_EQ(qr.rows.size(), 100);
    // Each partition should have 20 rows, ranks 1-20
    for (auto& row : qr.rows) {
        EXPECT_GE(row[2].as_int64(), 1);
        EXPECT_LE(row[2].as_int64(), 20);
    }
}

// =============================================================================
// Window function with aliased table (should still resolve columns)
// =============================================================================

TEST_F(GDB599WindowFunctionQA, TableAlias_WindowFunction) {
    setup_employees();
    auto qr = exec_ok("SELECT e.id, e.name, ROW_NUMBER() OVER (ORDER BY e.id) AS rn "
                      "FROM employees e");
    ASSERT_EQ(qr.rows.size(), 5);
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(qr.rows[i][2].as_int64(), static_cast<int64_t>(i + 1));
    }
}

// =============================================================================
// COUNT(*) OVER (PARTITION BY ...) — total per partition
// =============================================================================

TEST_F(GDB599WindowFunctionQA, CountStarPartitioned) {
    setup_employees();
    auto qr = exec_ok("SELECT id, department, COUNT(*) OVER (PARTITION BY department) AS dept_cnt "
                      "FROM employees");
    ASSERT_EQ(qr.rows.size(), 5);

    // Fixed by GDB-607: Without ORDER BY, SQL standard says frame is entire
    // partition. Engineering has 3 employees, Sales has 2.
    for (auto& row : qr.rows) {
        auto dept = row[1].as_string();
        auto cnt = get_int(row[2]);
        if (dept == "Engineering") {
            EXPECT_EQ(cnt, 3) << "Engineering partition should have count=3";
        } else {
            EXPECT_EQ(cnt, 2) << "Sales partition should have count=2";
        }
    }
}

// =============================================================================
// All-same values in ORDER BY (everyone is a peer)
// =============================================================================

TEST_F(GDB599WindowFunctionQA, AllSameOrderByValues_Rank) {
    exec_ok("CREATE TABLE same_val (id INT PRIMARY KEY, score INT)");
    exec_ok("INSERT INTO same_val VALUES (1, 50)");
    exec_ok("INSERT INTO same_val VALUES (2, 50)");
    exec_ok("INSERT INTO same_val VALUES (3, 50)");

    auto qr = exec_ok("SELECT id, RANK() OVER (ORDER BY score) AS rnk FROM same_val");
    ASSERT_EQ(qr.rows.size(), 3);
    // All peers -> all rank 1
    for (auto& row : qr.rows) {
        EXPECT_EQ(row[1].as_int64(), 1);
    }
}

// =============================================================================
// Frame: ROWS BETWEEN CURRENT ROW AND CURRENT ROW (single-row frame)
// =============================================================================

TEST_F(GDB599WindowFunctionQA, FrameCurrentRowOnly) {
    exec_ok("CREATE TABLE curr_t (id INT PRIMARY KEY, val INT)");
    exec_ok("INSERT INTO curr_t VALUES (1, 10)");
    exec_ok("INSERT INTO curr_t VALUES (2, 20)");
    exec_ok("INSERT INTO curr_t VALUES (3, 30)");

    auto qr = exec_ok("SELECT id, SUM(val) OVER (ORDER BY id "
                      "ROWS BETWEEN CURRENT ROW AND CURRENT ROW) AS self_sum FROM curr_t");
    ASSERT_EQ(qr.rows.size(), 3);
    // Each row's SUM should be just its own value
    EXPECT_EQ(get_int(qr.rows[0][1]), 10);
    EXPECT_EQ(get_int(qr.rows[1][1]), 20);
    EXPECT_EQ(get_int(qr.rows[2][1]), 30);
}
