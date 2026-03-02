/// @file test_qa_gdb_137.cpp
/// @brief QA adversarial tests for GDB-137: Statistics gathering and ANALYZE.
///
/// Targets edge cases: empty/single-row tables, all-null columns, identical
/// values, column_key encoding, stats overwrite, selectivity boundaries,
/// histogram interpolation, MCV edge cases, BETWEEN with reversed/equal bounds,
/// reservoir sampling with extreme configs.

#include "giodb/planner/statistics.h"
#include "giodb/storage/buffer_pool.h"
#include "giodb/storage/disk_manager.h"
#include "giodb/table/table_heap.h"
#include "giodb/table/tuple.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

using namespace giodb;

// =============================================================================
// StatisticsStore adversarial tests
// =============================================================================

TEST(QA137_StoreTest, OverwriteTableStats) {
    StatisticsStore store;

    TableStats ts1;
    ts1.table_id = 1;
    ts1.row_count = 100;
    store.set_table_stats(1, ts1);

    TableStats ts2;
    ts2.table_id = 1;
    ts2.row_count = 200;
    store.set_table_stats(1, ts2);

    const auto* got = store.get_table_stats(1);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->row_count, 200u);
}

TEST(QA137_StoreTest, OverwriteColumnStats) {
    StatisticsStore store;

    ColumnStats cs1;
    cs1.table_id = 1;
    cs1.column_ordinal = 0;
    cs1.ndistinct = 10;
    store.set_column_stats(1, 0, cs1);

    ColumnStats cs2;
    cs2.table_id = 1;
    cs2.column_ordinal = 0;
    cs2.ndistinct = 20;
    store.set_column_stats(1, 0, cs2);

    const auto* got = store.get_column_stats(1, 0);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->ndistinct, 20u);
}

TEST(QA137_StoreTest, RemoveNonExistentTable) {
    StatisticsStore store;
    // Should not crash.
    store.remove_table(999);
    EXPECT_EQ(store.get_table_stats(999), nullptr);
}

TEST(QA137_StoreTest, MultipleTablesIndependent) {
    StatisticsStore store;

    TableStats ts1;
    ts1.table_id = 1;
    ts1.row_count = 100;
    store.set_table_stats(1, ts1);

    TableStats ts2;
    ts2.table_id = 2;
    ts2.row_count = 200;
    store.set_table_stats(2, ts2);

    ColumnStats cs1;
    cs1.table_id = 1;
    cs1.column_ordinal = 0;
    cs1.ndistinct = 10;
    store.set_column_stats(1, 0, cs1);

    ColumnStats cs2;
    cs2.table_id = 2;
    cs2.column_ordinal = 0;
    cs2.ndistinct = 20;
    store.set_column_stats(2, 0, cs2);

    // Remove table 1 — table 2 unaffected.
    store.remove_table(1);
    EXPECT_EQ(store.get_table_stats(1), nullptr);
    EXPECT_EQ(store.get_column_stats(1, 0), nullptr);

    const auto* t2 = store.get_table_stats(2);
    ASSERT_NE(t2, nullptr);
    EXPECT_EQ(t2->row_count, 200u);

    const auto* c2 = store.get_column_stats(2, 0);
    ASSERT_NE(c2, nullptr);
    EXPECT_EQ(c2->ndistinct, 20u);
}

TEST(QA137_StoreTest, ClearThenReAdd) {
    StatisticsStore store;

    TableStats ts;
    ts.table_id = 1;
    ts.row_count = 100;
    store.set_table_stats(1, ts);
    store.clear();
    EXPECT_EQ(store.get_table_stats(1), nullptr);

    // Re-add after clear.
    ts.row_count = 300;
    store.set_table_stats(1, ts);
    const auto* got = store.get_table_stats(1);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->row_count, 300u);
}

// =============================================================================
// Analyze adversarial tests with real TableHeap
// =============================================================================

class QA137AnalyzeTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "giodb_qa137_stats.db";
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

    void insert_row_all_null() {
        std::vector<Value> values = {Value(), Value(), Value()};
        auto data = TupleSerializer::serialize(values, schema_);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = heap_->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    void insert_row_null_name(int32_t id, double score) {
        std::vector<Value> values = {Value(id), Value(), Value(score)};
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

// -- Single row table ---------------------------------------------------------

TEST_F(QA137AnalyzeTest, SingleRow) {
    insert_row(42, "only_row", 3.14);

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* ts = store_.get_table_stats(1);
    ASSERT_NE(ts, nullptr);
    EXPECT_EQ(ts->row_count, 1u);

    const auto* id_stats = store_.get_column_stats(1, 0);
    ASSERT_NE(id_stats, nullptr);
    EXPECT_EQ(id_stats->ndistinct, 1u);
    EXPECT_EQ(id_stats->min_value.as_int32(), 42);
    EXPECT_EQ(id_stats->max_value.as_int32(), 42);
    EXPECT_DOUBLE_EQ(id_stats->null_fraction, 0.0);
}

// -- All NULL column ----------------------------------------------------------

TEST_F(QA137AnalyzeTest, AllNullColumn) {
    for (int32_t i = 0; i < 50; ++i) {
        insert_row_null_name(i, static_cast<double>(i));
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* name_stats = store_.get_column_stats(1, 1);
    ASSERT_NE(name_stats, nullptr);
    EXPECT_DOUBLE_EQ(name_stats->null_fraction, 1.0);
    EXPECT_EQ(name_stats->ndistinct, 0u);
    EXPECT_TRUE(name_stats->mcv_list.empty());
    EXPECT_TRUE(name_stats->histogram.empty());
}

// -- All identical values -----------------------------------------------------

TEST_F(QA137AnalyzeTest, AllIdenticalValues) {
    for (int32_t i = 0; i < 100; ++i) {
        insert_row(i, "same", 1.0);
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* name_stats = store_.get_column_stats(1, 1);
    ASSERT_NE(name_stats, nullptr);
    EXPECT_EQ(name_stats->ndistinct, 1u);
    EXPECT_EQ(name_stats->mcv_list.size(), 1u);
    if (!name_stats->mcv_list.empty()) {
        EXPECT_NEAR(name_stats->mcv_list[0].frequency, 1.0, 0.001);
    }

    const auto* score_stats = store_.get_column_stats(1, 2);
    ASSERT_NE(score_stats, nullptr);
    EXPECT_EQ(score_stats->ndistinct, 1u);
    EXPECT_EQ(score_stats->min_value.as_float64(), 1.0);
    EXPECT_EQ(score_stats->max_value.as_float64(), 1.0);
}

// -- All rows have all columns NULL -------------------------------------------

TEST_F(QA137AnalyzeTest, AllRowsAllNull) {
    for (int i = 0; i < 20; ++i) {
        insert_row_all_null();
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* ts = store_.get_table_stats(1);
    ASSERT_NE(ts, nullptr);
    EXPECT_EQ(ts->row_count, 20u);

    for (int ordinal = 0; ordinal < 3; ++ordinal) {
        const auto* cs = store_.get_column_stats(1, ordinal);
        ASSERT_NE(cs, nullptr);
        EXPECT_DOUBLE_EQ(cs->null_fraction, 1.0);
        EXPECT_EQ(cs->ndistinct, 0u);
    }
}

// -- MCV count greater than distinct values -----------------------------------

TEST_F(QA137AnalyzeTest, McvCountExceedsDistinct) {
    AnalyzeConfig config;
    config.mcv_count = 100; // Way more than distinct values.
    config.histogram_buckets = 10;

    // Only 3 distinct names.
    for (int32_t i = 0; i < 30; ++i) {
        insert_row(i, "a", 1.0);
    }
    for (int32_t i = 30; i < 50; ++i) {
        insert_row(i, "b", 2.0);
    }
    for (int32_t i = 50; i < 60; ++i) {
        insert_row(i, "c", 3.0);
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_, config);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* name_stats = store_.get_column_stats(1, 1);
    ASSERT_NE(name_stats, nullptr);
    // MCV list should contain all 3 distinct values.
    EXPECT_EQ(name_stats->mcv_list.size(), 3u);
    // Histogram should be empty since all values are MCVs.
    EXPECT_TRUE(name_stats->histogram.empty());
}

// -- Zero histogram buckets ---------------------------------------------------

TEST_F(QA137AnalyzeTest, ZeroHistogramBuckets) {
    AnalyzeConfig config;
    config.histogram_buckets = 0;

    for (int32_t i = 0; i < 50; ++i) {
        insert_row(i, "name_" + std::to_string(i), static_cast<double>(i));
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_, config);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* id_stats = store_.get_column_stats(1, 0);
    ASSERT_NE(id_stats, nullptr);
    EXPECT_TRUE(id_stats->histogram.empty());
    // MCV and ndistinct should still work.
    EXPECT_GT(id_stats->ndistinct, 0u);
}

// -- Small sample size triggers reservoir sampling ----------------------------

TEST_F(QA137AnalyzeTest, SampleSizeOneTriggersReservoir) {
    AnalyzeConfig config;
    config.sample_size = 1;

    for (int32_t i = 0; i < 100; ++i) {
        insert_row(i, "name_" + std::to_string(i % 10), static_cast<double>(i));
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_, config);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* ts = store_.get_table_stats(1);
    ASSERT_NE(ts, nullptr);
    // Row count is total, not sampled.
    EXPECT_EQ(ts->row_count, 100u);

    // With sample_size=1, we only have 1 sample row.
    const auto* id_stats = store_.get_column_stats(1, 0);
    ASSERT_NE(id_stats, nullptr);
    EXPECT_EQ(id_stats->ndistinct, 1u);
}

// -- Re-analyze updates stats -------------------------------------------------

TEST_F(QA137AnalyzeTest, ReAnalyzeUpdates) {
    insert_row(1, "a", 1.0);

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* ts1 = store_.get_table_stats(1);
    ASSERT_NE(ts1, nullptr);
    EXPECT_EQ(ts1->row_count, 1u);

    // Insert more rows.
    for (int32_t i = 2; i <= 50; ++i) {
        insert_row(i, "name_" + std::to_string(i), static_cast<double>(i));
    }

    result = analyze_table(1, table_schema_, *heap_, schema_, store_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* ts2 = store_.get_table_stats(1);
    ASSERT_NE(ts2, nullptr);
    EXPECT_EQ(ts2->row_count, 50u);
}

// -- Histogram with fewer values than buckets ---------------------------------

TEST_F(QA137AnalyzeTest, HistogramFewerValuesThanBuckets) {
    AnalyzeConfig config;
    config.histogram_buckets = 100;
    config.mcv_count = 0; // No MCVs.

    // Only 5 distinct values.
    for (int32_t i = 0; i < 5; ++i) {
        insert_row(i, "name", static_cast<double>(i));
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_, config);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* id_stats = store_.get_column_stats(1, 0);
    ASSERT_NE(id_stats, nullptr);
    // Bucket count should be capped at distinct values.
    EXPECT_LE(id_stats->histogram.size(), 5u);
    EXPECT_GT(id_stats->histogram.size(), 0u);
}

// -- String column min/max ----------------------------------------------------

TEST_F(QA137AnalyzeTest, StringMinMax) {
    insert_row(1, "zebra", 1.0);
    insert_row(2, "apple", 2.0);
    insert_row(3, "mango", 3.0);

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* name_stats = store_.get_column_stats(1, 1);
    ASSERT_NE(name_stats, nullptr);
    EXPECT_EQ(name_stats->min_value.as_string(), "apple");
    EXPECT_EQ(name_stats->max_value.as_string(), "zebra");
}

// -- Large table stress -------------------------------------------------------

TEST_F(QA137AnalyzeTest, LargeTable5000Rows) {
    for (int32_t i = 0; i < 5000; ++i) {
        insert_row(i, "name_" + std::to_string(i % 100), static_cast<double>(i % 500));
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* ts = store_.get_table_stats(1);
    ASSERT_NE(ts, nullptr);
    EXPECT_EQ(ts->row_count, 5000u);
    EXPECT_GT(ts->page_count, 0u);
    EXPECT_GT(ts->avg_row_width, 0.0);

    // Name column should have ~100 distinct values.
    const auto* name_stats = store_.get_column_stats(1, 1);
    ASSERT_NE(name_stats, nullptr);
    EXPECT_EQ(name_stats->ndistinct, 100u);
    EXPECT_FALSE(name_stats->mcv_list.empty());
}

// =============================================================================
// Selectivity estimation adversarial tests
// =============================================================================

// -- Equality selectivity edge cases ------------------------------------------

TEST(QA137_SelectivityTest, EqualityZeroNdistinct) {
    ColumnStats stats;
    stats.ndistinct = 0;
    stats.null_fraction = 1.0;

    // All-null column — equality on a value should return remaining/1 = 0.
    double sel = estimate_equality_selectivity(stats, Value(int32_t{42}));
    EXPECT_GE(sel, 0.0);
    EXPECT_LE(sel, 1.0);
}

TEST(QA137_SelectivityTest, EqualityAllMcv) {
    ColumnStats stats;
    stats.ndistinct = 2;
    stats.null_fraction = 0.0;
    stats.mcv_list = {
        {Value(int32_t{1}), 0.6},
        {Value(int32_t{2}), 0.4},
    };

    // Value in MCV.
    double sel1 = estimate_equality_selectivity(stats, Value(int32_t{1}));
    EXPECT_NEAR(sel1, 0.6, 0.01);

    // Value NOT in MCV — remaining_distinct = max(0, 0) = 1 (clamp).
    double sel2 = estimate_equality_selectivity(stats, Value(int32_t{99}));
    EXPECT_GE(sel2, 0.0);
    EXPECT_LE(sel2, 1.0);
}

TEST(QA137_SelectivityTest, EqualityNullOnNoNullColumn) {
    ColumnStats stats;
    stats.ndistinct = 10;
    stats.null_fraction = 0.0;

    double sel = estimate_equality_selectivity(stats, Value());
    EXPECT_DOUBLE_EQ(sel, 0.0);
}

// -- Range selectivity edge cases ---------------------------------------------

TEST(QA137_SelectivityTest, RangeBelowAllHistogram) {
    ColumnStats stats;
    stats.ndistinct = 100;
    stats.null_fraction = 0.0;
    stats.histogram = {
        {Value(int32_t{10}), Value(int32_t{20}), 50, 10},
        {Value(int32_t{21}), Value(int32_t{30}), 50, 10},
    };

    // col < 5: below all histogram values.
    double sel = estimate_range_selectivity(stats, BinaryOp::LESS, Value(int32_t{5}));
    EXPECT_GE(sel, 0.0);
    EXPECT_LE(sel, 1.0);
}

TEST(QA137_SelectivityTest, RangeAboveAllHistogram) {
    ColumnStats stats;
    stats.ndistinct = 100;
    stats.null_fraction = 0.0;
    stats.histogram = {
        {Value(int32_t{10}), Value(int32_t{20}), 50, 10},
        {Value(int32_t{21}), Value(int32_t{30}), 50, 10},
    };

    // col > 100: above all histogram values.
    double sel = estimate_range_selectivity(stats, BinaryOp::GREATER, Value(int32_t{100}));
    EXPECT_GE(sel, 0.0);
    EXPECT_LE(sel, 1.0);
}

TEST(QA137_SelectivityTest, RangeEmptyHistogramAndMcv) {
    ColumnStats stats;
    stats.ndistinct = 10;
    stats.null_fraction = 0.0;
    // No histogram, no MCV.

    double sel = estimate_range_selectivity(stats, BinaryOp::LESS, Value(int32_t{50}));
    EXPECT_GT(sel, 0.0);
    EXPECT_LT(sel, 1.0);
}

TEST(QA137_SelectivityTest, RangeDefaultOperator) {
    ColumnStats stats;
    stats.ndistinct = 10;

    // Use an operator that's not a range op — should fall back to default.
    double sel = estimate_range_selectivity(stats, BinaryOp::EQUAL, Value(int32_t{50}));
    EXPECT_DOUBLE_EQ(sel, kDefaultSelectivity);
}

// -- BETWEEN edge cases -------------------------------------------------------

TEST(QA137_SelectivityTest, BetweenSameValue) {
    ColumnStats stats;
    stats.ndistinct = 100;
    stats.null_fraction = 0.0;
    stats.histogram = {
        {Value(int32_t{1}), Value(int32_t{100}), 100, 100},
    };

    // BETWEEN 50 AND 50: single point.
    double sel = estimate_between_selectivity(stats, Value(int32_t{50}), Value(int32_t{50}));
    EXPECT_GE(sel, 0.0);
    EXPECT_LE(sel, 1.0);
}

TEST(QA137_SelectivityTest, BetweenReversedBounds) {
    ColumnStats stats;
    stats.ndistinct = 100;
    stats.null_fraction = 0.0;
    stats.histogram = {
        {Value(int32_t{1}), Value(int32_t{100}), 100, 100},
    };

    // BETWEEN 80 AND 20: reversed bounds — should yield ~0.
    double sel = estimate_between_selectivity(stats, Value(int32_t{80}), Value(int32_t{20}));
    EXPECT_GE(sel, 0.0);
    EXPECT_LE(sel, 1.0);
    // Reversed bounds: should be small.
    EXPECT_LT(sel, 0.5);
}

TEST(QA137_SelectivityTest, BetweenFullRange) {
    ColumnStats stats;
    stats.ndistinct = 100;
    stats.null_fraction = 0.1;
    stats.histogram = {
        {Value(int32_t{1}), Value(int32_t{50}), 50, 50},
        {Value(int32_t{51}), Value(int32_t{100}), 50, 50},
    };

    double sel = estimate_between_selectivity(stats, Value(int32_t{1}), Value(int32_t{100}));
    EXPECT_GT(sel, 0.5);
}

// -- IS NULL / IS NOT NULL edge cases -----------------------------------------

TEST(QA137_SelectivityTest, IsNullAllNull) {
    ColumnStats stats;
    stats.null_fraction = 1.0;

    EXPECT_DOUBLE_EQ(estimate_is_null_selectivity(stats), 1.0);
    EXPECT_DOUBLE_EQ(estimate_is_not_null_selectivity(stats), 0.0);
}

TEST(QA137_SelectivityTest, IsNullNoNull) {
    ColumnStats stats;
    stats.null_fraction = 0.0;

    EXPECT_DOUBLE_EQ(estimate_is_null_selectivity(stats), 0.0);
    EXPECT_DOUBLE_EQ(estimate_is_not_null_selectivity(stats), 1.0);
}

// -- Histogram LE fraction edge cases -----------------------------------------

TEST(QA137_SelectivityTest, HistogramSingleBucket) {
    ColumnStats stats;
    stats.ndistinct = 10;
    stats.null_fraction = 0.0;
    stats.histogram = {
        {Value(int32_t{1}), Value(int32_t{100}), 100, 10},
    };

    // Value at lower bound.
    double sel_low =
        estimate_range_selectivity(stats, BinaryOp::LESS_EQUAL, Value(int32_t{1}));
    EXPECT_GE(sel_low, 0.0);
    EXPECT_LE(sel_low, 1.0);

    // Value at upper bound.
    double sel_high =
        estimate_range_selectivity(stats, BinaryOp::LESS_EQUAL, Value(int32_t{100}));
    EXPECT_GT(sel_high, sel_low);
}

// -- MCV selectivity with high null fraction ----------------------------------

TEST(QA137_SelectivityTest, McvWithHighNullFraction) {
    ColumnStats stats;
    stats.ndistinct = 2;
    stats.null_fraction = 0.9; // 90% null.
    stats.mcv_list = {
        {Value(int32_t{1}), 0.5},
        {Value(int32_t{2}), 0.5},
    };

    // Equality on MCV value: frequency * (1 - null_fraction).
    double sel = estimate_equality_selectivity(stats, Value(int32_t{1}));
    EXPECT_NEAR(sel, 0.5 * 0.1, 0.01); // 0.05
}

// -- Selectivity on string type -----------------------------------------------

TEST(QA137_SelectivityTest, StringRangeSelectivity) {
    ColumnStats stats;
    stats.ndistinct = 100;
    stats.null_fraction = 0.0;
    stats.type_id = TypeId::STRING;
    stats.histogram = {
        {Value(std::string("a")), Value(std::string("m")), 50, 50},
        {Value(std::string("n")), Value(std::string("z")), 50, 50},
    };

    double sel = estimate_range_selectivity(stats, BinaryOp::LESS, Value(std::string("m")));
    EXPECT_GE(sel, 0.0);
    EXPECT_LE(sel, 1.0);
}

// -- Analyze determinism (reservoir sampling with fixed seed) ------------------

TEST_F(QA137AnalyzeTest, DeterministicResults) {
    for (int32_t i = 0; i < 100; ++i) {
        insert_row(i, "name_" + std::to_string(i), static_cast<double>(i));
    }

    StatisticsStore store1;
    auto r1 = analyze_table(1, table_schema_, *heap_, schema_, store1);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    StatisticsStore store2;
    auto r2 = analyze_table(1, table_schema_, *heap_, schema_, store2);
    ASSERT_TRUE(r2.has_value()) << r2.error().message;

    const auto* ts1 = store1.get_table_stats(1);
    const auto* ts2 = store2.get_table_stats(1);
    ASSERT_NE(ts1, nullptr);
    ASSERT_NE(ts2, nullptr);
    EXPECT_EQ(ts1->row_count, ts2->row_count);
    EXPECT_EQ(ts1->page_count, ts2->page_count);
    EXPECT_DOUBLE_EQ(ts1->avg_row_width, ts2->avg_row_width);

    const auto* cs1 = store1.get_column_stats(1, 0);
    const auto* cs2 = store2.get_column_stats(1, 0);
    ASSERT_NE(cs1, nullptr);
    ASSERT_NE(cs2, nullptr);
    EXPECT_EQ(cs1->ndistinct, cs2->ndistinct);
    EXPECT_DOUBLE_EQ(cs1->null_fraction, cs2->null_fraction);
}

// -- MCV frequency ordering ---------------------------------------------------

TEST_F(QA137AnalyzeTest, McvOrderedByFrequency) {
    // "alpha" appears 50 times, "beta" 30, "gamma" 20.
    for (int32_t i = 0; i < 50; ++i) {
        insert_row(i, "alpha", 1.0);
    }
    for (int32_t i = 50; i < 80; ++i) {
        insert_row(i, "beta", 2.0);
    }
    for (int32_t i = 80; i < 100; ++i) {
        insert_row(i, "gamma", 3.0);
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* name_stats = store_.get_column_stats(1, 1);
    ASSERT_NE(name_stats, nullptr);
    ASSERT_GE(name_stats->mcv_list.size(), 3u);

    // MCV should be sorted by frequency descending.
    for (size_t i = 1; i < name_stats->mcv_list.size(); ++i) {
        EXPECT_GE(name_stats->mcv_list[i - 1].frequency,
                  name_stats->mcv_list[i].frequency)
            << "MCV not sorted at index " << i;
    }
}

// -- Histogram covers full range of non-MCV values ----------------------------

TEST_F(QA137AnalyzeTest, HistogramCoverageComplete) {
    AnalyzeConfig config;
    config.mcv_count = 0;
    config.histogram_buckets = 5;

    for (int32_t i = 0; i < 100; ++i) {
        insert_row(i, "name", static_cast<double>(i));
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_, config);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* id_stats = store_.get_column_stats(1, 0);
    ASSERT_NE(id_stats, nullptr);
    ASSERT_EQ(id_stats->histogram.size(), 5u);

    // First bucket lower should be min, last bucket upper should be max.
    EXPECT_EQ(id_stats->histogram[0].lower.as_int32(), 0);
    EXPECT_EQ(id_stats->histogram.back().upper.as_int32(), 99);

    // Total count across buckets should equal total non-null values.
    uint64_t total_count = 0;
    for (const auto& bucket : id_stats->histogram) {
        total_count += bucket.count;
    }
    EXPECT_EQ(total_count, 100u);
}

// -- Mixed nulls: selectivity with BETWEEN ------------------------------------

TEST_F(QA137AnalyzeTest, SelectivityWithMixedNulls) {
    // 70 non-null, 30 null name values.
    for (int32_t i = 0; i < 70; ++i) {
        insert_row(i, "name_" + std::to_string(i), static_cast<double>(i));
    }
    for (int32_t i = 70; i < 100; ++i) {
        insert_row_null_name(i, static_cast<double>(i));
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* id_stats = store_.get_column_stats(1, 0);
    ASSERT_NE(id_stats, nullptr);

    // IS NULL on id (which has no nulls).
    double null_sel = estimate_is_null_selectivity(*id_stats);
    EXPECT_DOUBLE_EQ(null_sel, 0.0);

    // IS NOT NULL on id.
    double not_null_sel = estimate_is_not_null_selectivity(*id_stats);
    EXPECT_DOUBLE_EQ(not_null_sel, 1.0);

    // Name column should have 30% null fraction.
    const auto* name_stats = store_.get_column_stats(1, 1);
    ASSERT_NE(name_stats, nullptr);
    EXPECT_NEAR(name_stats->null_fraction, 0.3, 0.01);
}

// -- Float64 column statistics ------------------------------------------------

TEST_F(QA137AnalyzeTest, Float64MinMaxAndHistogram) {
    AnalyzeConfig config;
    config.mcv_count = 0;
    config.histogram_buckets = 5;

    for (int32_t i = 0; i < 100; ++i) {
        insert_row(i, "name", static_cast<double>(i) * 0.1);
    }

    auto result = analyze_table(1, table_schema_, *heap_, schema_, store_, config);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto* score_stats = store_.get_column_stats(1, 2);
    ASSERT_NE(score_stats, nullptr);
    EXPECT_NEAR(score_stats->min_value.as_float64(), 0.0, 0.001);
    EXPECT_NEAR(score_stats->max_value.as_float64(), 9.9, 0.001);
    EXPECT_EQ(score_stats->histogram.size(), 5u);
}
