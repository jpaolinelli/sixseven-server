#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/external_sort.h"
#include "sixseven/executor/iterator.h"
#include "sixseven/executor/seq_scan.h"
#include "sixseven/executor/sort.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace sixseven;

// =============================================================================
// In-memory source iterator (produces tuples from a vector — no disk needed)
// =============================================================================

namespace {

class VectorSource : public Iterator {
public:
    VectorSource(std::vector<Tuple> tuples, OutputSchema schema)
        : tuples_(std::move(tuples)), schema_(std::move(schema)) {}

    const OutputSchema& output_schema() const override { return schema_; }

protected:
    Result<void> do_open() override {
        cursor_ = 0;
        return ok();
    }

    Result<std::optional<Tuple>> do_next() override {
        if (cursor_ >= tuples_.size()) {
            return ok(std::optional<Tuple>(std::nullopt));
        }
        // Copy, not move — allows re-open.
        return ok(std::optional<Tuple>(tuples_[cursor_++]));
    }

    void do_close() override { cursor_ = 0; }

private:
    std::vector<Tuple> tuples_;
    OutputSchema schema_;
    size_t cursor_ = 0;
};

// -- Expression helpers -------------------------------------------------------

ExprPtr col_ref(const std::string& name) {
    auto e = std::make_unique<ColumnRefExpr>();
    e->column = name;
    return e;
}

// -- Tuple construction helpers -----------------------------------------------

Tuple make_row(int32_t id, const std::string& name, int32_t age) {
    return Tuple{{Value(id), Value(name), Value(age)}, std::nullopt};
}

Tuple make_row_nullable(int32_t id, std::optional<std::string> name, int32_t age) {
    Value name_val = name.has_value() ? Value(*name) : Value::make_null();
    return Tuple{{Value(id), name_val, Value(age)}, std::nullopt};
}

} // namespace

// =============================================================================
// Test fixture
// =============================================================================

class ExternalSortTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_esort";
        std::filesystem::remove_all(temp_dir_);

        OutputColumn c1{"", "id", TypeId::INT32, false, 0};
        OutputColumn c2{"", "name", TypeId::STRING, true, 0};
        OutputColumn c3{"", "age", TypeId::INT32, true, 0};
        output_schema_ = OutputSchema({c1, c2, c3});
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    std::unique_ptr<VectorSource> make_source(std::vector<Tuple> tuples) {
        return std::make_unique<VectorSource>(std::move(tuples), output_schema_);
    }

    /// Drain all tuples from an iterator.
    std::vector<Tuple> drain(Iterator& iter) {
        auto open = iter.open();
        EXPECT_TRUE(open.has_value()) << open.error().message;
        std::vector<Tuple> results;
        while (true) {
            auto row = iter.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row || !row->has_value()) {
                break;
            }
            results.push_back(std::move(row->value()));
        }
        iter.close();
        return results;
    }

    /// Generate N tuples with sequential ids, random names, and ages.
    std::vector<Tuple> generate_tuples(size_t n, unsigned seed = 42) {
        std::mt19937 rng(seed);
        std::vector<Tuple> tuples;
        tuples.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            auto age = static_cast<int32_t>(rng() % 100);
            std::string name = "name_" + std::to_string(i);
            tuples.push_back(make_row(static_cast<int32_t>(i), name, age));
        }
        return tuples;
    }

    std::filesystem::path temp_dir_;
    OutputSchema output_schema_;
};

// =============================================================================
// In-memory mode (small dataset, no temp files)
// =============================================================================

TEST_F(ExternalSortTest, InMemorySortAscending) {
    auto tuples = std::vector<Tuple>{
        make_row(3, "charlie", 35),
        make_row(1, "alice", 30),
        make_row(2, "bob", 25),
    };

    auto source = make_source(std::move(tuples));
    BoundStatement bound;

    auto age_expr = col_ref("age");
    std::vector<SortKey> keys = {{age_expr.get(), SortDirection::ASC}};

    ExternalSortOperator sort(std::move(source),
                              std::move(keys),
                              bound,
                              1024 * 1024, // 1MB — plenty for 3 tuples
                              128,
                              temp_dir_);

    auto results = drain(sort);

    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].values[2].as_int32(), 25); // bob
    EXPECT_EQ(results[1].values[2].as_int32(), 30); // alice
    EXPECT_EQ(results[2].values[2].as_int32(), 35); // charlie
}

TEST_F(ExternalSortTest, InMemorySortDescending) {
    auto tuples = std::vector<Tuple>{
        make_row(1, "alice", 30),
        make_row(2, "bob", 25),
        make_row(3, "charlie", 35),
    };

    auto source = make_source(std::move(tuples));
    BoundStatement bound;

    auto age_expr = col_ref("age");
    std::vector<SortKey> keys = {{age_expr.get(), SortDirection::DESC}};

    ExternalSortOperator sort(
        std::move(source), std::move(keys), bound, 1024 * 1024, 128, temp_dir_);

    auto results = drain(sort);

    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].values[2].as_int32(), 35);
    EXPECT_EQ(results[1].values[2].as_int32(), 30);
    EXPECT_EQ(results[2].values[2].as_int32(), 25);
}

TEST_F(ExternalSortTest, InMemoryMultiColumnSort) {
    auto tuples = std::vector<Tuple>{
        make_row(1, "alice", 30),
        make_row(2, "bob", 30),
        make_row(3, "charlie", 25),
    };

    auto source = make_source(std::move(tuples));
    BoundStatement bound;

    // ORDER BY age ASC, name DESC
    auto age_expr = col_ref("age");
    auto name_expr = col_ref("name");
    std::vector<SortKey> keys = {
        {age_expr.get(), SortDirection::ASC},
        {name_expr.get(), SortDirection::DESC},
    };

    ExternalSortOperator sort(
        std::move(source), std::move(keys), bound, 1024 * 1024, 128, temp_dir_);

    auto results = drain(sort);

    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].values[1].as_string(), "charlie"); // age 25
    EXPECT_EQ(results[1].values[1].as_string(), "bob");     // age 30, name DESC
    EXPECT_EQ(results[2].values[1].as_string(), "alice");   // age 30, name DESC
}

TEST_F(ExternalSortTest, EmptyInput) {
    auto source = make_source({});
    BoundStatement bound;

    auto age_expr = col_ref("age");
    std::vector<SortKey> keys = {{age_expr.get(), SortDirection::ASC}};

    ExternalSortOperator sort(
        std::move(source), std::move(keys), bound, 1024 * 1024, 128, temp_dir_);

    auto results = drain(sort);
    EXPECT_TRUE(results.empty());
}

TEST_F(ExternalSortTest, SingleTuple) {
    auto tuples = std::vector<Tuple>{make_row(1, "alice", 30)};
    auto source = make_source(std::move(tuples));
    BoundStatement bound;

    auto age_expr = col_ref("age");
    std::vector<SortKey> keys = {{age_expr.get(), SortDirection::ASC}};

    ExternalSortOperator sort(
        std::move(source), std::move(keys), bound, 1024 * 1024, 128, temp_dir_);

    auto results = drain(sort);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
}

// =============================================================================
// External sort (dataset exceeds work_mem)
// =============================================================================

TEST_F(ExternalSortTest, ExternalSortCorrectness) {
    // Generate 200 tuples and use a tiny work_mem to force external sort.
    auto tuples = generate_tuples(200);
    auto source = make_source(tuples);
    BoundStatement bound;

    auto age_expr = col_ref("age");
    std::vector<SortKey> keys = {{age_expr.get(), SortDirection::ASC}};

    // ~100 bytes per tuple estimate; 512 bytes work_mem → several runs.
    ExternalSortOperator sort(std::move(source),
                              std::move(keys),
                              bound,
                              512, // very small work_mem
                              128,
                              temp_dir_);

    auto results = drain(sort);

    ASSERT_EQ(results.size(), 200u);

    // Verify sorted order.
    for (size_t i = 1; i < results.size(); ++i) {
        EXPECT_LE(results[i - 1].values[2].as_int32(), results[i].values[2].as_int32())
            << "Sort violation at index " << i;
    }
}

TEST_F(ExternalSortTest, ExternalSortDescending) {
    auto tuples = generate_tuples(200);
    auto source = make_source(tuples);
    BoundStatement bound;

    auto age_expr = col_ref("age");
    std::vector<SortKey> keys = {{age_expr.get(), SortDirection::DESC}};

    ExternalSortOperator sort(std::move(source), std::move(keys), bound, 512, 128, temp_dir_);

    auto results = drain(sort);

    ASSERT_EQ(results.size(), 200u);
    for (size_t i = 1; i < results.size(); ++i) {
        EXPECT_GE(results[i - 1].values[2].as_int32(), results[i].values[2].as_int32())
            << "Descending sort violation at index " << i;
    }
}

TEST_F(ExternalSortTest, ExternalSortMultiColumn) {
    // Tuples with duplicate ages to test multi-column sort.
    std::vector<Tuple> tuples;
    for (int32_t i = 0; i < 100; ++i) {
        auto age = i % 10; // Many duplicates
        std::string name = "name_" + std::to_string(99 - i);
        tuples.push_back(make_row(i, name, age));
    }

    auto source = make_source(tuples);
    BoundStatement bound;

    // ORDER BY age ASC, name ASC
    auto age_expr = col_ref("age");
    auto name_expr = col_ref("name");
    std::vector<SortKey> keys = {
        {age_expr.get(), SortDirection::ASC},
        {name_expr.get(), SortDirection::ASC},
    };

    ExternalSortOperator sort(std::move(source), std::move(keys), bound, 512, 128, temp_dir_);

    auto results = drain(sort);

    ASSERT_EQ(results.size(), 100u);
    for (size_t i = 1; i < results.size(); ++i) {
        auto prev_age = results[i - 1].values[2].as_int32();
        auto curr_age = results[i].values[2].as_int32();
        EXPECT_LE(prev_age, curr_age) << "Age sort violation at index " << i;
        if (prev_age == curr_age) {
            EXPECT_LE(results[i - 1].values[1].as_string(), results[i].values[1].as_string())
                << "Name sort violation at index " << i;
        }
    }
}

TEST_F(ExternalSortTest, ExternalSortMixedAscDesc) {
    std::vector<Tuple> tuples;
    for (int32_t i = 0; i < 100; ++i) {
        auto age = i % 5;
        std::string name = "name_" + std::to_string(i);
        tuples.push_back(make_row(i, name, age));
    }

    auto source = make_source(tuples);
    BoundStatement bound;

    // ORDER BY age ASC, name DESC
    auto age_expr = col_ref("age");
    auto name_expr = col_ref("name");
    std::vector<SortKey> keys = {
        {age_expr.get(), SortDirection::ASC},
        {name_expr.get(), SortDirection::DESC},
    };

    ExternalSortOperator sort(std::move(source), std::move(keys), bound, 512, 128, temp_dir_);

    auto results = drain(sort);

    ASSERT_EQ(results.size(), 100u);
    for (size_t i = 1; i < results.size(); ++i) {
        auto prev_age = results[i - 1].values[2].as_int32();
        auto curr_age = results[i].values[2].as_int32();
        EXPECT_LE(prev_age, curr_age) << "Age sort violation at index " << i;
        if (prev_age == curr_age) {
            EXPECT_GE(results[i - 1].values[1].as_string(), results[i].values[1].as_string())
                << "Name DESC violation at index " << i;
        }
    }
}

// =============================================================================
// Multi-pass merge (more runs than merge width)
// =============================================================================

TEST_F(ExternalSortTest, MultiPassMerge) {
    // 100 tuples, tiny work_mem, merge width of 3.
    // This forces multiple merge passes.
    auto tuples = generate_tuples(100);
    auto source = make_source(tuples);
    BoundStatement bound;

    auto age_expr = col_ref("age");
    std::vector<SortKey> keys = {{age_expr.get(), SortDirection::ASC}};

    ExternalSortOperator sort(std::move(source),
                              std::move(keys),
                              bound,
                              256, // very tiny → many runs
                              3,   // merge width 3 → multi-pass required
                              temp_dir_);

    auto results = drain(sort);

    ASSERT_EQ(results.size(), 100u);
    for (size_t i = 1; i < results.size(); ++i) {
        EXPECT_LE(results[i - 1].values[2].as_int32(), results[i].values[2].as_int32())
            << "Multi-pass sort violation at index " << i;
    }
}

TEST_F(ExternalSortTest, MultiPassMergeWidth2) {
    auto tuples = generate_tuples(50);
    auto source = make_source(tuples);
    BoundStatement bound;

    auto age_expr = col_ref("age");
    std::vector<SortKey> keys = {{age_expr.get(), SortDirection::DESC}};

    ExternalSortOperator sort(std::move(source),
                              std::move(keys),
                              bound,
                              256,
                              2, // merge width 2 → binary merge
                              temp_dir_);

    auto results = drain(sort);

    ASSERT_EQ(results.size(), 50u);
    for (size_t i = 1; i < results.size(); ++i) {
        EXPECT_GE(results[i - 1].values[2].as_int32(), results[i].values[2].as_int32())
            << "Multi-pass DESC violation at index " << i;
    }
}

// =============================================================================
// NULL handling
// =============================================================================

TEST_F(ExternalSortTest, NullsSortLastForAsc) {
    auto tuples = std::vector<Tuple>{
        make_row_nullable(1, "alice", 30),
        make_row_nullable(2, std::nullopt, 25),
        make_row_nullable(3, "charlie", 35),
        make_row_nullable(4, std::nullopt, 20),
    };

    auto source = make_source(std::move(tuples));
    BoundStatement bound;

    auto name_expr = col_ref("name");
    std::vector<SortKey> keys = {{name_expr.get(), SortDirection::ASC}};

    ExternalSortOperator sort(
        std::move(source), std::move(keys), bound, 1024 * 1024, 128, temp_dir_);

    auto results = drain(sort);

    ASSERT_EQ(results.size(), 4u);
    // Non-null names first (alphabetical), then nulls.
    EXPECT_FALSE(results[0].values[1].is_null());
    EXPECT_FALSE(results[1].values[1].is_null());
    EXPECT_TRUE(results[2].values[1].is_null());
    EXPECT_TRUE(results[3].values[1].is_null());
    EXPECT_EQ(results[0].values[1].as_string(), "alice");
    EXPECT_EQ(results[1].values[1].as_string(), "charlie");
}

TEST_F(ExternalSortTest, NullsSortFirstForDesc) {
    auto tuples = std::vector<Tuple>{
        make_row_nullable(1, "alice", 30),
        make_row_nullable(2, std::nullopt, 25),
        make_row_nullable(3, "charlie", 35),
    };

    auto source = make_source(std::move(tuples));
    BoundStatement bound;

    auto name_expr = col_ref("name");
    std::vector<SortKey> keys = {{name_expr.get(), SortDirection::DESC}};

    ExternalSortOperator sort(
        std::move(source), std::move(keys), bound, 1024 * 1024, 128, temp_dir_);

    auto results = drain(sort);

    ASSERT_EQ(results.size(), 3u);
    // NULL first for DESC, then descending names.
    EXPECT_TRUE(results[0].values[1].is_null());
    EXPECT_EQ(results[1].values[1].as_string(), "charlie");
    EXPECT_EQ(results[2].values[1].as_string(), "alice");
}

TEST_F(ExternalSortTest, NullsExternalSort) {
    // Test NULL handling with external sort (multiple runs).
    std::vector<Tuple> tuples;
    for (int32_t i = 0; i < 50; ++i) {
        if (i % 5 == 0) {
            tuples.push_back(make_row_nullable(i, std::nullopt, i));
        } else {
            tuples.push_back(make_row_nullable(i, "name_" + std::to_string(100 - i), i));
        }
    }

    auto source = make_source(tuples);
    BoundStatement bound;

    auto name_expr = col_ref("name");
    std::vector<SortKey> keys = {{name_expr.get(), SortDirection::ASC}};

    ExternalSortOperator sort(std::move(source), std::move(keys), bound, 512, 128, temp_dir_);

    auto results = drain(sort);

    ASSERT_EQ(results.size(), 50u);

    // All non-null names should come first (sorted), then all nulls.
    size_t null_start = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i].values[1].is_null()) {
            null_start = i;
            break;
        }
    }

    // Verify non-null section is sorted.
    for (size_t i = 1; i < null_start; ++i) {
        EXPECT_LE(results[i - 1].values[1].as_string(), results[i].values[1].as_string());
    }

    // Verify all remaining are null.
    for (size_t i = null_start; i < results.size(); ++i) {
        EXPECT_TRUE(results[i].values[1].is_null()) << "Expected null at index " << i;
    }
}

// =============================================================================
// Temp file cleanup
// =============================================================================

TEST_F(ExternalSortTest, TempFilesCleanedUpAfterSort) {
    auto tuples = generate_tuples(100);
    auto source = make_source(tuples);
    BoundStatement bound;

    auto age_expr = col_ref("age");
    std::vector<SortKey> keys = {{age_expr.get(), SortDirection::ASC}};

    {
        ExternalSortOperator sort(std::move(source), std::move(keys), bound, 512, 128, temp_dir_);

        auto open = sort.open();
        ASSERT_TRUE(open.has_value()) << open.error().message;

        // Temp files should exist during the sort.
        // (We don't check this directly — just drain and close.)
        while (true) {
            auto row = sort.next();
            ASSERT_TRUE(row.has_value());
            if (!row->has_value()) {
                break;
            }
        }

        sort.close();
    }

    // After close(), the temp directory should be empty or removed.
    if (std::filesystem::exists(temp_dir_)) {
        EXPECT_TRUE(std::filesystem::is_empty(temp_dir_))
            << "Temp directory not cleaned up: " << temp_dir_;
    }
}

TEST_F(ExternalSortTest, TempFilesCleanedUpByDestructor) {
    auto tuples = generate_tuples(100);
    auto source = make_source(tuples);
    BoundStatement bound;

    auto age_expr = col_ref("age");
    std::vector<SortKey> keys = {{age_expr.get(), SortDirection::ASC}};

    {
        ExternalSortOperator sort(std::move(source), std::move(keys), bound, 512, 128, temp_dir_);

        auto open = sort.open();
        ASSERT_TRUE(open.has_value()) << open.error().message;

        // Read some tuples but don't close — let destructor handle cleanup.
        auto row = sort.next();
        ASSERT_TRUE(row.has_value());
        // Destructor runs here.
    }

    // Temp files should be cleaned up by the destructor.
    if (std::filesystem::exists(temp_dir_)) {
        EXPECT_TRUE(std::filesystem::is_empty(temp_dir_))
            << "Destructor did not clean temp files: " << temp_dir_;
    }
}

// =============================================================================
// Tuple size estimation
// =============================================================================

TEST_F(ExternalSortTest, TupleSizeEstimation) {
    auto t1 = make_row(1, "hello", 30);
    auto size1 = ExternalSortOperator::estimate_tuple_size(t1);
    EXPECT_GT(size1, 0u);

    // A tuple with a longer string should be estimated as larger.
    auto t2 = make_row(1, std::string(1000, 'x'), 30);
    auto size2 = ExternalSortOperator::estimate_tuple_size(t2);
    EXPECT_GT(size2, size1);
}

// =============================================================================
// Larger dataset — verifies no OOM and correctness
// =============================================================================

TEST_F(ExternalSortTest, LargeDataset) {
    // 1000 tuples with tiny work_mem.
    auto tuples = generate_tuples(1000);
    auto source = make_source(tuples);
    BoundStatement bound;

    auto age_expr = col_ref("age");
    std::vector<SortKey> keys = {{age_expr.get(), SortDirection::ASC}};

    ExternalSortOperator sort(std::move(source),
                              std::move(keys),
                              bound,
                              1024, // 1KB work_mem
                              16,
                              temp_dir_);

    auto results = drain(sort);

    ASSERT_EQ(results.size(), 1000u);
    for (size_t i = 1; i < results.size(); ++i) {
        EXPECT_LE(results[i - 1].values[2].as_int32(), results[i].values[2].as_int32())
            << "Large dataset sort violation at index " << i;
    }
}

// =============================================================================
// String column sort (variable-length serialization)
// =============================================================================

TEST_F(ExternalSortTest, ExternalSortOnStringColumn) {
    std::vector<Tuple> tuples;
    for (int32_t i = 0; i < 50; ++i) {
        // Reverse alphabetical names.
        std::string name(1, static_cast<char>('z' - (i % 26)));
        name += "_" + std::to_string(i);
        tuples.push_back(make_row(i, name, i));
    }

    auto source = make_source(tuples);
    BoundStatement bound;

    auto name_expr = col_ref("name");
    std::vector<SortKey> keys = {{name_expr.get(), SortDirection::ASC}};

    ExternalSortOperator sort(std::move(source), std::move(keys), bound, 512, 128, temp_dir_);

    auto results = drain(sort);

    ASSERT_EQ(results.size(), 50u);
    for (size_t i = 1; i < results.size(); ++i) {
        EXPECT_LE(results[i - 1].values[1].as_string(), results[i].values[1].as_string())
            << "String sort violation at index " << i;
    }
}

// =============================================================================
// Preserves all tuple data through serialization round-trip
// =============================================================================

TEST_F(ExternalSortTest, DataPreservedThroughExternalSort) {
    auto tuples = std::vector<Tuple>{
        make_row(42, "hello world", 99),
        make_row(7, "foo bar", 10),
        make_row(100, "", 50),
    };

    auto source = make_source(tuples);
    BoundStatement bound;

    auto age_expr = col_ref("age");
    std::vector<SortKey> keys = {{age_expr.get(), SortDirection::ASC}};

    // Use tiny work_mem to force external sort.
    ExternalSortOperator sort(std::move(source),
                              std::move(keys),
                              bound,
                              1, // 1 byte → forces external
                              128,
                              temp_dir_);

    auto results = drain(sort);

    ASSERT_EQ(results.size(), 3u);

    // Sorted by age: 10, 50, 99.
    EXPECT_EQ(results[0].values[0].as_int32(), 7);
    EXPECT_EQ(results[0].values[1].as_string(), "foo bar");
    EXPECT_EQ(results[0].values[2].as_int32(), 10);

    EXPECT_EQ(results[1].values[0].as_int32(), 100);
    EXPECT_EQ(results[1].values[1].as_string(), "");
    EXPECT_EQ(results[1].values[2].as_int32(), 50);

    EXPECT_EQ(results[2].values[0].as_int32(), 42);
    EXPECT_EQ(results[2].values[1].as_string(), "hello world");
    EXPECT_EQ(results[2].values[2].as_int32(), 99);
}

// =============================================================================
// output_schema passthrough
// =============================================================================

TEST_F(ExternalSortTest, OutputSchemaPassthrough) {
    auto source = make_source({});
    BoundStatement bound;

    auto age_expr = col_ref("age");
    std::vector<SortKey> keys = {{age_expr.get(), SortDirection::ASC}};

    ExternalSortOperator sort(
        std::move(source), std::move(keys), bound, 1024 * 1024, 128, temp_dir_);

    EXPECT_EQ(sort.output_schema().column_count(), 3u);
    EXPECT_EQ(sort.output_schema().column(0).name, "id");
    EXPECT_EQ(sort.output_schema().column(1).name, "name");
    EXPECT_EQ(sort.output_schema().column(2).name, "age");
}

// =============================================================================
// PATH round-trip with total_weight (regression test for GDB-799)
// =============================================================================

TEST_F(ExternalSortTest, PathRoundTripWithTotalWeight) {
    // Verify that Path::total_weight survives an external sort spill/reload.
    // Uses a two-column schema (sort_key INT32, path PATH) so we can sort on
    // the int column for deterministic output ordering while exercising the
    // PATH serialization path under spill.
    OutputSchema schema{
        {{"", "sort_key", TypeId::INT32, false, 0}, {"", "path", TypeId::PATH, false, 0}}};

    Path p1;
    p1.total_weight = 10.5;
    p1.steps = {{1, 100}, {2, -1}};
    Tuple t1{{Value(int32_t{1}), Value(std::move(p1))}, std::nullopt};

    Path p2;
    p2.total_weight = 5.25;
    p2.steps = {{3, 200}, {4, 201}, {5, -1}};
    Tuple t2{{Value(int32_t{2}), Value(std::move(p2))}, std::nullopt};

    auto source = std::make_unique<VectorSource>(std::vector<Tuple>{t2, t1}, schema);

    BoundStatement bound;
    auto key_expr = col_ref("sort_key");
    std::vector<SortKey> keys = {{key_expr.get(), SortDirection::ASC}};

    // Use a work_mem_bytes smaller than a single tuple's estimated size (~152 bytes
    // or more on a 64-bit build) so that the mem_used >= work_mem_bytes_ condition
    // fires as soon as the second tuple enters the buffer (buffer.size() > 1).
    // This forces generate_runs() to flush both tuples into a run file via
    // write_tuple, and the merge path reads them back via read_tuple — exercising
    // the exact code that was fixed for PATH total_weight serialization.
    ExternalSortOperator sort(std::move(source), std::move(keys), bound, 64, 128, temp_dir_);

    auto results = drain(sort);

    ASSERT_EQ(results.size(), 2u);

    // sort_key=1 comes first after ascending sort
    const auto& restored1 = results[0].values[1].as_path();
    EXPECT_EQ(restored1.total_weight, 10.5) << "total_weight for path 1 must survive spill";
    ASSERT_EQ(restored1.steps.size(), 2u);
    EXPECT_EQ(restored1.steps[0].node_pk, 1);
    EXPECT_EQ(restored1.steps[0].edge_id, 100);
    EXPECT_EQ(restored1.steps[1].node_pk, 2);
    EXPECT_EQ(restored1.steps[1].edge_id, -1);

    // sort_key=2 comes second
    const auto& restored2 = results[1].values[1].as_path();
    EXPECT_EQ(restored2.total_weight, 5.25) << "total_weight for path 2 must survive spill";
    ASSERT_EQ(restored2.steps.size(), 3u);
    EXPECT_EQ(restored2.steps[0].node_pk, 3);
    EXPECT_EQ(restored2.steps[0].edge_id, 200);
    EXPECT_EQ(restored2.steps[1].node_pk, 4);
    EXPECT_EQ(restored2.steps[1].edge_id, 201);
    EXPECT_EQ(restored2.steps[2].node_pk, 5);
    EXPECT_EQ(restored2.steps[2].edge_id, -1);
}
