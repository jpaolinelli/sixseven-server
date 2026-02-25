#include "giodb/common/coercion.h"
#include "giodb/planner/statistics.h"
#include "giodb/storage/buffer_pool.h"
#include "giodb/storage/disk_manager.h"
#include "giodb/table/table_heap.h"
#include "giodb/table/tuple.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <vector>

using namespace giodb;

// =============================================================================
// StatisticsStore tests
// =============================================================================

TEST(StatisticsStoreTest, TableStatsSetAndGet) {
    StatisticsStore store;

    TableStats ts;
    ts.table_id = 1;
    ts.row_count = 1000;
    ts.page_count = 10;
    ts.avg_row_width = 50.0;

    store.set_table_stats(1, ts);

    const auto* got = store.get_table_stats(1);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->row_count, 1000u);
    EXPECT_EQ(got->page_count, 10u);
    EXPECT_DOUBLE_EQ(got->avg_row_width, 50.0);
}

TEST(StatisticsStoreTest, TableStatsNotFound) {
    StatisticsStore store;
    EXPECT_EQ(store.get_table_stats(999), nullptr);
}

TEST(StatisticsStoreTest, ColumnStatsSetAndGet) {
    StatisticsStore store;

    ColumnStats cs;
    cs.table_id = 1;
    cs.column_ordinal = 2;
    cs.column_name = "age";
    cs.type_id = TypeId::INT32;
    cs.null_fraction = 0.05;
    cs.ndistinct = 50;

    store.set_column_stats(1, 2, cs);

    const auto* got = store.get_column_stats(1, 2);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->column_name, "age");
    EXPECT_EQ(got->ndistinct, 50u);
    EXPECT_DOUBLE_EQ(got->null_fraction, 0.05);
}

TEST(StatisticsStoreTest, ColumnStatsNotFound) {
    StatisticsStore store;
    EXPECT_EQ(store.get_column_stats(1, 0), nullptr);
}

TEST(StatisticsStoreTest, RemoveTable) {
    StatisticsStore store;

    TableStats ts;
    ts.table_id = 1;
    store.set_table_stats(1, ts);

    ColumnStats cs;
    cs.table_id = 1;
    cs.column_ordinal = 0;
    store.set_column_stats(1, 0, cs);

    cs.column_ordinal = 1;
    store.set_column_stats(1, 1, cs);

    store.remove_table(1);

    EXPECT_EQ(store.get_table_stats(1), nullptr);
    EXPECT_EQ(store.get_column_stats(1, 0), nullptr);
    EXPECT_EQ(store.get_column_stats(1, 1), nullptr);
}

TEST(StatisticsStoreTest, Clear) {
    StatisticsStore store;

    TableStats ts;
    ts.table_id = 1;
    store.set_table_stats(1, ts);

    ColumnStats cs;
    cs.table_id = 1;
    cs.column_ordinal = 0;
    store.set_column_stats(1, 0, cs);

    store.clear();

    EXPECT_EQ(store.get_table_stats(1), nullptr);
    EXPECT_EQ(store.get_column_stats(1, 0), nullptr);
}

// =============================================================================
// ANALYZE tests with real TableHeap
// =============================================================================

class AnalyzeTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "giodb_test_statistics.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;

        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
        heap_ = std::make_unique<TableHeap>(*bpm_, dm_, file_id_);

        // Schema: id (INT32), name (STRING), score (FLOAT64)
        std::vector<ColumnDef> cols = {
            {"id", TypeId::INT32},
            {"name", TypeId::STRING},
            {"score", TypeId::FLOAT64},
        };
        schema_ = Schema(cols);

        table_schema_.table_id = 1;
        table_schema_.name = "test_table";
        table_schema_.columns = {
            {0, "id", TypeId::INT32, false, ""},
            {1, "name", TypeId::STRING, true, ""},
            {2, "score", TypeId::FLOAT64, true, ""},
        };
    }

    void TearDown() override {
        heap_.reset();
        bpm_.reset();
        auto close = dm_.close_file(file_id_);
        (void)close;
        std::filesystem::remove(path_);
    }

    void insert_row(int32_t id, const std::string& name, double score) {
        std::vector<Value> values = {Value(id), Value(name), Value(score)};
        auto data = TupleSerializer::serialize(values, schema_);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = heap_->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    void insert_row_with_null_name(int32_t id, double score) {
        std::vector<Value> values = {Value(id), Value::make_null(), Value(score)};
        auto data = TupleSerializer::serialize(values, schema_);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = heap_->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<TableHeap> heap_;
    Schema schema_;
    TableSchema table_schema_;
    StatisticsStore store_;
};

TEST_F(AnalyzeTest, EmptyTable) {
    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* ts = store_.get_table_stats(1);
    ASSERT_NE(ts, nullptr);
    EXPECT_EQ(ts->row_count, 0u);
    EXPECT_DOUBLE_EQ(ts->avg_row_width, 0.0);
}

TEST_F(AnalyzeTest, BasicTableStats) {
    // Insert 100 rows.
    for (int32_t i = 0; i < 100; ++i) {
        insert_row(i, "name_" + std::to_string(i), static_cast<double>(i) * 1.5);
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* ts = store_.get_table_stats(1);
    ASSERT_NE(ts, nullptr);
    EXPECT_EQ(ts->row_count, 100u);
    EXPECT_GT(ts->page_count, 0u);
    EXPECT_GT(ts->avg_row_width, 0.0);
}

TEST_F(AnalyzeTest, ColumnStatsDistinctValues) {
    // Insert rows with known distribution.
    for (int32_t i = 0; i < 100; ++i) {
        insert_row(i, "name_" + std::to_string(i % 10), static_cast<double>(i));
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // ID column: 100 distinct values.
    const auto* id_stats = store_.get_column_stats(1, 0);
    ASSERT_NE(id_stats, nullptr);
    EXPECT_EQ(id_stats->ndistinct, 100u);
    EXPECT_DOUBLE_EQ(id_stats->null_fraction, 0.0);

    // Name column: 10 distinct values.
    const auto* name_stats = store_.get_column_stats(1, 1);
    ASSERT_NE(name_stats, nullptr);
    EXPECT_EQ(name_stats->ndistinct, 10u);
}

TEST_F(AnalyzeTest, ColumnStatsNullFraction) {
    // Insert 80 normal rows and 20 rows with NULL name.
    for (int32_t i = 0; i < 80; ++i) {
        insert_row(i, "name_" + std::to_string(i), static_cast<double>(i));
    }
    for (int32_t i = 80; i < 100; ++i) {
        insert_row_with_null_name(i, static_cast<double>(i));
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* name_stats = store_.get_column_stats(1, 1);
    ASSERT_NE(name_stats, nullptr);
    EXPECT_NEAR(name_stats->null_fraction, 0.2, 0.01);
}

TEST_F(AnalyzeTest, ColumnStatsMinMax) {
    for (int32_t i = 10; i < 60; ++i) {
        insert_row(i, "name", static_cast<double>(i));
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* id_stats = store_.get_column_stats(1, 0);
    ASSERT_NE(id_stats, nullptr);
    EXPECT_FALSE(id_stats->min_value.is_null());
    EXPECT_FALSE(id_stats->max_value.is_null());
    EXPECT_EQ(id_stats->min_value.as_int32(), 10);
    EXPECT_EQ(id_stats->max_value.as_int32(), 59);
}

TEST_F(AnalyzeTest, McvList) {
    // Insert skewed data: "alice" appears 30 times, "bob" 20, rest 5 each.
    for (int32_t i = 0; i < 30; ++i) {
        insert_row(i, "alice", 1.0);
    }
    for (int32_t i = 30; i < 50; ++i) {
        insert_row(i, "bob", 2.0);
    }
    for (int32_t i = 50; i < 55; ++i) {
        insert_row(i, "carol", 3.0);
    }
    for (int32_t i = 55; i < 60; ++i) {
        insert_row(i, "dave", 4.0);
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* name_stats = store_.get_column_stats(1, 1);
    ASSERT_NE(name_stats, nullptr);
    ASSERT_GE(name_stats->mcv_list.size(), 2u);

    // First MCV should be "alice" with ~50% frequency.
    EXPECT_EQ(name_stats->mcv_list[0].value.as_string(), "alice");
    EXPECT_NEAR(name_stats->mcv_list[0].frequency, 0.5, 0.01);

    // Second MCV should be "bob" with ~33% frequency.
    EXPECT_EQ(name_stats->mcv_list[1].value.as_string(), "bob");
    EXPECT_NEAR(name_stats->mcv_list[1].frequency, 1.0 / 3.0, 0.02);
}

TEST_F(AnalyzeTest, Histogram) {
    // Insert 100 distinct integer values.
    AnalyzeConfig config;
    config.histogram_buckets = 10;
    config.mcv_count = 0; // No MCVs so all values go to histogram.

    for (int32_t i = 0; i < 100; ++i) {
        insert_row(i, "name", static_cast<double>(i));
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_, config);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* id_stats = store_.get_column_stats(1, 0);
    ASSERT_NE(id_stats, nullptr);
    EXPECT_EQ(id_stats->histogram.size(), 10u);

    // Each bucket should have ~10 values.
    for (const auto& bucket : id_stats->histogram) {
        EXPECT_EQ(bucket.count, 10u);
    }

    // First bucket should start at 0.
    EXPECT_EQ(id_stats->histogram[0].lower.as_int32(), 0);
    // Last bucket should end at 99.
    EXPECT_EQ(id_stats->histogram.back().upper.as_int32(), 99);
}

TEST_F(AnalyzeTest, ReservoirSampling) {
    // Insert more rows than the sample size.
    AnalyzeConfig config;
    config.sample_size = 50;

    for (int32_t i = 0; i < 200; ++i) {
        insert_row(i, "name_" + std::to_string(i % 20), static_cast<double>(i));
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_, config);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* ts = store_.get_table_stats(1);
    ASSERT_NE(ts, nullptr);
    // Total row count should reflect all rows, not just sampled.
    EXPECT_EQ(ts->row_count, 200u);

    // Column stats should be computed from sample, so ndistinct might be lower.
    const auto* name_stats = store_.get_column_stats(1, 1);
    ASSERT_NE(name_stats, nullptr);
    EXPECT_GT(name_stats->ndistinct, 0u);
    EXPECT_LE(name_stats->ndistinct, 20u);
}

// =============================================================================
// Selectivity estimation tests
// =============================================================================

class SelectivityTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create column stats with known distribution.
        // 100 non-null values, 10% null.
        // MCV: value 5 appears 20%, value 10 appears 15%.
        // Histogram covers remaining values 1-100 (excluding MCVs).
        stats_.table_id = 1;
        stats_.column_ordinal = 0;
        stats_.column_name = "col";
        stats_.type_id = TypeId::INT32;
        stats_.null_fraction = 0.1;
        stats_.ndistinct = 50;
        stats_.min_value = Value(int32_t{1});
        stats_.max_value = Value(int32_t{100});

        // MCV list.
        stats_.mcv_list = {
            {Value(int32_t{5}), 0.20},  // 20% of non-null
            {Value(int32_t{10}), 0.15}, // 15% of non-null
        };

        // Histogram: 5 buckets covering values 1-100 (excluding MCVs).
        // Each bucket covers ~20 value range.
        stats_.histogram = {
            {Value(int32_t{1}), Value(int32_t{20}), 13, 13},
            {Value(int32_t{21}), Value(int32_t{40}), 13, 13},
            {Value(int32_t{41}), Value(int32_t{60}), 13, 13},
            {Value(int32_t{61}), Value(int32_t{80}), 13, 13},
            {Value(int32_t{81}), Value(int32_t{100}), 13, 13},
        };
    }

    ColumnStats stats_;
};

TEST_F(SelectivityTest, EqualityMcvHit) {
    // Value 5 is in MCV with 20% frequency.
    double sel = estimate_equality_selectivity(stats_, Value(int32_t{5}));
    // Expected: 0.20 * (1 - 0.1) = 0.18
    EXPECT_NEAR(sel, 0.18, 0.01);
}

TEST_F(SelectivityTest, EqualityMcvMiss) {
    // Value 50 is not in MCV.
    double sel = estimate_equality_selectivity(stats_, Value(int32_t{50}));
    // Remaining fraction = (1 - 0.1) * (1 - 0.35) = 0.9 * 0.65 = 0.585
    // Remaining distinct = 50 - 2 = 48
    // Expected: 0.585 / 48 ≈ 0.0122
    EXPECT_GT(sel, 0.0);
    EXPECT_LT(sel, 0.05);
}

TEST_F(SelectivityTest, EqualityNull) {
    double sel = estimate_equality_selectivity(stats_, Value::make_null());
    EXPECT_DOUBLE_EQ(sel, 0.1);
}

TEST_F(SelectivityTest, IsNull) {
    double sel = estimate_is_null_selectivity(stats_);
    EXPECT_DOUBLE_EQ(sel, 0.1);
}

TEST_F(SelectivityTest, IsNotNull) {
    double sel = estimate_is_not_null_selectivity(stats_);
    EXPECT_DOUBLE_EQ(sel, 0.9);
}

TEST_F(SelectivityTest, RangeLess) {
    // col < 50: should include some histogram buckets + MCVs < 50.
    double sel = estimate_range_selectivity(stats_, BinaryOp::LESS, Value(int32_t{50}));
    EXPECT_GT(sel, 0.0);
    EXPECT_LT(sel, 1.0);
}

TEST_F(SelectivityTest, RangeGreater) {
    // col > 50: should be complement of col <= 50 (excluding nulls).
    double sel = estimate_range_selectivity(stats_, BinaryOp::GREATER, Value(int32_t{50}));
    EXPECT_GT(sel, 0.0);
    EXPECT_LT(sel, 1.0);
}

TEST_F(SelectivityTest, RangeLessEqual) {
    double sel = estimate_range_selectivity(stats_, BinaryOp::LESS_EQUAL, Value(int32_t{100}));
    // col <= 100: should be close to (1 - null_fraction).
    EXPECT_GT(sel, 0.7);
}

TEST_F(SelectivityTest, RangeGreaterEqual) {
    double sel = estimate_range_selectivity(stats_, BinaryOp::GREATER_EQUAL, Value(int32_t{1}));
    // col >= 1: should be close to (1 - null_fraction).
    EXPECT_GT(sel, 0.7);
}

TEST_F(SelectivityTest, Between) {
    double sel = estimate_between_selectivity(stats_, Value(int32_t{20}), Value(int32_t{40}));
    EXPECT_GT(sel, 0.0);
    EXPECT_LT(sel, 1.0);
}

TEST_F(SelectivityTest, BetweenFullRange) {
    double sel = estimate_between_selectivity(stats_, Value(int32_t{1}), Value(int32_t{100}));
    // Full range should be close to (1 - null_fraction).
    EXPECT_GT(sel, 0.5);
}

TEST_F(SelectivityTest, EmptyStats) {
    ColumnStats empty;
    empty.ndistinct = 0;

    double sel = estimate_range_selectivity(empty, BinaryOp::LESS, Value(int32_t{50}));
    EXPECT_DOUBLE_EQ(sel, kDefaultSelectivity);
}

// =============================================================================
// Integration: ANALYZE + selectivity
// =============================================================================

class AnalyzeSelectivityTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "giodb_test_analyze_sel.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;

        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
        heap_ = std::make_unique<TableHeap>(*bpm_, dm_, file_id_);

        std::vector<ColumnDef> cols = {
            {"id", TypeId::INT32},
            {"value", TypeId::INT32},
        };
        schema_ = Schema(cols);

        table_schema_.table_id = 1;
        table_schema_.name = "test_table";
        table_schema_.columns = {
            {0, "id", TypeId::INT32, false, ""},
            {1, "value", TypeId::INT32, true, ""},
        };
    }

    void TearDown() override {
        heap_.reset();
        bpm_.reset();
        auto close = dm_.close_file(file_id_);
        (void)close;
        std::filesystem::remove(path_);
    }

    void insert_row(int32_t id, int32_t value) {
        std::vector<Value> values = {Value(id), Value(value)};
        auto data = TupleSerializer::serialize(values, schema_);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = heap_->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<TableHeap> heap_;
    Schema schema_;
    TableSchema table_schema_;
    StatisticsStore store_;
};

TEST_F(AnalyzeSelectivityTest, SelectivePredicateHasLowSelectivity) {
    // Insert 1000 rows with value 1-1000.
    for (int32_t i = 1; i <= 1000; ++i) {
        insert_row(i, i);
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* value_stats = store_.get_column_stats(1, 1);
    ASSERT_NE(value_stats, nullptr);

    // Equality on a single value out of 1000: selectivity should be low.
    double sel = estimate_equality_selectivity(*value_stats, Value(int32_t{500}));
    EXPECT_LT(sel, 0.05); // Should be around 1/1000 = 0.001
}

TEST_F(AnalyzeSelectivityTest, RangePredicateReasonableEstimate) {
    // Insert 100 rows with value 1-100.
    for (int32_t i = 1; i <= 100; ++i) {
        insert_row(i, i);
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* value_stats = store_.get_column_stats(1, 1);
    ASSERT_NE(value_stats, nullptr);

    // col < 50: should be roughly 50% of non-null values.
    double sel = estimate_range_selectivity(*value_stats, BinaryOp::LESS, Value(int32_t{50}));
    EXPECT_GT(sel, 0.2);
    EXPECT_LT(sel, 0.8);
}
