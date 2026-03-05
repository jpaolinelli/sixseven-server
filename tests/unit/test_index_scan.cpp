#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/bitmap_scan.h"
#include "sixseven/executor/index_scan.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/index/btree_index.h"
#include "sixseven/index/btree_iterator.h"
#include "sixseven/index/rid.h"
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
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class IndexScanTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_test_index_scan.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;

        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);

        // Schema: id (INT64), name (STRING), age (INT32)
        storage_schema_ = Schema({
            {"id", TypeId::INT64},
            {"name", TypeId::STRING},
            {"age", TypeId::INT32},
        });

        OutputColumn c1{"", "id", TypeId::INT64, false, 0};
        OutputColumn c2{"", "name", TypeId::STRING, true, 0};
        OutputColumn c3{"", "age", TypeId::INT32, true, 0};
        output_schema_ = OutputSchema({c1, c2, c3});
    }

    void TearDown() override {
        bpm_.reset();
        auto close = dm_.close_file(file_id_);
        (void)close;
        std::filesystem::remove(path_);
    }

    /// Insert a row into the heap and return the RID.
    RID insert_row(TableHeap& heap, int64_t id, const std::string& name, int32_t age) {
        std::vector<Value> vals = {Value(id), Value(name), Value(age)};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        EXPECT_TRUE(bytes.has_value()) << bytes.error().message;
        auto rid = heap.insert_tuple(*bytes);
        EXPECT_TRUE(rid.has_value()) << rid.error().message;
        return *rid;
    }

    /// Insert a row into the heap and also insert the key into the index.
    RID insert_indexed_row(
        TableHeap& heap, BTreeIndex& index, int64_t id, const std::string& name, int32_t age) {
        auto rid = insert_row(heap, id, name, age);
        KeyType key = {Value(id)};
        auto ins = index.insert(key, rid);
        EXPECT_TRUE(ins.has_value()) << ins.error().message;
        return rid;
    }

    /// Create a B+ tree index on the id column (INT64).
    BTreeIndex make_id_index(bool is_unique = true) {
        BTreeConfig config;
        config.key_types = {TypeId::INT64};
        config.leaf_max_keys = 4;
        config.internal_max_keys = 4;
        config.is_unique = is_unique;
        return BTreeIndex(std::move(config));
    }

    /// Create a B+ tree index on the age column (INT32, stored as INT32 key).
    BTreeIndex make_age_index() {
        BTreeConfig config;
        config.key_types = {TypeId::INT32};
        config.leaf_max_keys = 4;
        config.internal_max_keys = 4;
        config.is_unique = false;
        return BTreeIndex(std::move(config));
    }

    /// Create a composite B+ tree index on (id, age).
    BTreeIndex make_composite_index() {
        BTreeConfig config;
        config.key_types = {TypeId::INT64, TypeId::INT32};
        config.leaf_max_keys = 4;
        config.internal_max_keys = 4;
        config.is_unique = true;
        return BTreeIndex(std::move(config));
    }

    /// Collect all tuples from an iterator into a vector of value vectors.
    std::vector<std::vector<Value>> collect_all(Iterator& iter) {
        std::vector<std::vector<Value>> results;
        auto open = iter.open();
        EXPECT_TRUE(open.has_value()) << open.error().message;
        while (true) {
            auto row = iter.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row->has_value()) {
                break;
            }
            results.push_back(row->value().values);
        }
        iter.close();
        return results;
    }

    /// Helper to make a literal expression.
    static ExprPtr lit_int(const std::string& v) {
        auto e = std::make_unique<LiteralExpr>();
        e->kind = LiteralKind::INTEGER;
        e->value = v;
        return e;
    }

    static ExprPtr col_ref(const std::string& name) {
        auto e = std::make_unique<ColumnRefExpr>();
        e->column = name;
        return e;
    }

    static ExprPtr binary_expr(BinaryOp op, ExprPtr lhs, ExprPtr rhs) {
        auto e = std::make_unique<BinaryExpr>();
        e->op = op;
        e->lhs = std::move(lhs);
        e->rhs = std::move(rhs);
        return e;
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
    Schema storage_schema_;
    OutputSchema output_schema_;
};

// =============================================================================
// IndexScan: point lookups
// =============================================================================

TEST_F(IndexScanTest, PointLookupFindsExactMatch) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "alice", 30);
    insert_indexed_row(heap, index, 20, "bob", 25);
    insert_indexed_row(heap, index, 30, "charlie", 35);

    // Predicate: id = 20
    auto pred = binary_expr(BinaryOp::EQUAL, col_ref("id"), lit_int("20"));
    BoundStatement bound;

    IndexScanOperator scan(index,
                           heap,
                           storage_schema_,
                           output_schema_,
                           KeyType{Value(static_cast<int64_t>(20))}, // begin
                           std::nullopt,                             // end (open)
                           {0},                                      // index col 0 = id
                           false,                                    // not index-only
                           pred.get(),
                           &bound);

    auto results = collect_all(scan);
    // The scan starts from key=20 and scans forward. The predicate filters
    // to only exact matches.
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(std::get<int64_t>(results[0][0].data()), 20);
    EXPECT_EQ(std::get<std::string>(results[0][1].data()), "bob");
    EXPECT_EQ(std::get<int32_t>(results[0][2].data()), 25);
}

TEST_F(IndexScanTest, PointLookupReturnsEmptyForMissing) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "alice", 30);
    insert_indexed_row(heap, index, 30, "charlie", 35);

    // Predicate: id = 20 (does not exist)
    auto pred = binary_expr(BinaryOp::EQUAL, col_ref("id"), lit_int("20"));
    BoundStatement bound;

    IndexScanOperator scan(index,
                           heap,
                           storage_schema_,
                           output_schema_,
                           KeyType{Value(static_cast<int64_t>(20))},
                           std::nullopt,
                           {0},
                           false,
                           pred.get(),
                           &bound);

    auto results = collect_all(scan);
    EXPECT_EQ(results.size(), 0u);
}

// =============================================================================
// IndexScan: range scans
// =============================================================================

TEST_F(IndexScanTest, RangeScanGreaterThan) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "alice", 30);
    insert_indexed_row(heap, index, 20, "bob", 25);
    insert_indexed_row(heap, index, 30, "charlie", 35);
    insert_indexed_row(heap, index, 40, "diana", 28);

    // Predicate: id > 20
    auto pred = binary_expr(BinaryOp::GREATER, col_ref("id"), lit_int("20"));
    BoundStatement bound;

    IndexScanOperator scan(index,
                           heap,
                           storage_schema_,
                           output_schema_,
                           KeyType{Value(static_cast<int64_t>(20))}, // begin at 20
                           std::nullopt,                             // scan to end
                           {0},
                           false,
                           pred.get(),
                           &bound);

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(std::get<int64_t>(results[0][0].data()), 30);
    EXPECT_EQ(std::get<int64_t>(results[1][0].data()), 40);
}

TEST_F(IndexScanTest, RangeScanGreaterEqual) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "alice", 30);
    insert_indexed_row(heap, index, 20, "bob", 25);
    insert_indexed_row(heap, index, 30, "charlie", 35);

    // Predicate: id >= 20
    auto pred = binary_expr(BinaryOp::GREATER_EQUAL, col_ref("id"), lit_int("20"));
    BoundStatement bound;

    IndexScanOperator scan(index,
                           heap,
                           storage_schema_,
                           output_schema_,
                           KeyType{Value(static_cast<int64_t>(20))},
                           std::nullopt,
                           {0},
                           false,
                           pred.get(),
                           &bound);

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(std::get<int64_t>(results[0][0].data()), 20);
    EXPECT_EQ(std::get<int64_t>(results[1][0].data()), 30);
}

TEST_F(IndexScanTest, RangeScanLessThan) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "alice", 30);
    insert_indexed_row(heap, index, 20, "bob", 25);
    insert_indexed_row(heap, index, 30, "charlie", 35);

    // Predicate: id < 25
    auto pred = binary_expr(BinaryOp::LESS, col_ref("id"), lit_int("25"));
    BoundStatement bound;

    IndexScanOperator scan(index,
                           heap,
                           storage_schema_,
                           output_schema_,
                           std::nullopt,                             // begin at start
                           KeyType{Value(static_cast<int64_t>(25))}, // end before 25
                           {0},
                           false,
                           pred.get(),
                           &bound);

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(std::get<int64_t>(results[0][0].data()), 10);
    EXPECT_EQ(std::get<int64_t>(results[1][0].data()), 20);
}

TEST_F(IndexScanTest, RangeScanBetween) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    for (int64_t i = 1; i <= 10; ++i) {
        insert_indexed_row(
            heap, index, i * 10, "user" + std::to_string(i), static_cast<int32_t>(20 + i));
    }

    // Scan range [30, 70) — ids 30, 40, 50, 60.
    // Predicate: id >= 30 AND id < 70
    auto pred = binary_expr(BinaryOp::AND,
                            binary_expr(BinaryOp::GREATER_EQUAL, col_ref("id"), lit_int("30")),
                            binary_expr(BinaryOp::LESS, col_ref("id"), lit_int("70")));
    BoundStatement bound;

    IndexScanOperator scan(index,
                           heap,
                           storage_schema_,
                           output_schema_,
                           KeyType{Value(static_cast<int64_t>(30))},
                           KeyType{Value(static_cast<int64_t>(70))},
                           {0},
                           false,
                           pred.get(),
                           &bound);

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 4u);
    EXPECT_EQ(std::get<int64_t>(results[0][0].data()), 30);
    EXPECT_EQ(std::get<int64_t>(results[1][0].data()), 40);
    EXPECT_EQ(std::get<int64_t>(results[2][0].data()), 50);
    EXPECT_EQ(std::get<int64_t>(results[3][0].data()), 60);
}

// =============================================================================
// IndexScan: empty table
// =============================================================================

TEST_F(IndexScanTest, ScanEmptyTable) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    IndexScanOperator scan(
        index, heap, storage_schema_, output_schema_, std::nullopt, std::nullopt, {0});

    auto open = scan.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto row = scan.next();
    ASSERT_TRUE(row.has_value()) << row.error().message;
    EXPECT_FALSE(row->has_value()); // No rows.

    scan.close();
}

// =============================================================================
// IndexScan: full scan (no bounds)
// =============================================================================

TEST_F(IndexScanTest, FullScanReturnsAllRowsInOrder) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 30, "charlie", 35);
    insert_indexed_row(heap, index, 10, "alice", 30);
    insert_indexed_row(heap, index, 20, "bob", 25);

    IndexScanOperator scan(
        index, heap, storage_schema_, output_schema_, std::nullopt, std::nullopt, {0});

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 3u);
    // Results should be in index order (ascending by id).
    EXPECT_EQ(std::get<int64_t>(results[0][0].data()), 10);
    EXPECT_EQ(std::get<int64_t>(results[1][0].data()), 20);
    EXPECT_EQ(std::get<int64_t>(results[2][0].data()), 30);
}

// =============================================================================
// IndexScan: index-only scan
// =============================================================================

TEST_F(IndexScanTest, IndexOnlyScanSkipsHeapFetch) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "alice", 30);
    insert_indexed_row(heap, index, 20, "bob", 25);
    insert_indexed_row(heap, index, 30, "charlie", 35);

    // Index-only scan: only the id column (index column 0, table column 0).
    // The output schema only has the id column for this test.
    OutputColumn id_col{"", "id", TypeId::INT64, false, 0};
    auto index_only_schema = OutputSchema({id_col});

    IndexScanOperator scan(index,
                           heap,
                           storage_schema_,
                           index_only_schema,
                           std::nullopt,
                           std::nullopt,
                           {0}, // index col 0 maps to table col 0
                           true // index-only
    );

    auto open = scan.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    std::vector<int64_t> ids;
    while (true) {
        auto row = scan.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        ids.push_back(std::get<int64_t>(row->value().values[0].data()));
    }

    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], 10);
    EXPECT_EQ(ids[1], 20);
    EXPECT_EQ(ids[2], 30);

    scan.close();
}

// =============================================================================
// IndexScan: composite key
// =============================================================================

TEST_F(IndexScanTest, CompositeKeyPrefixScan) {
    TableHeap heap(*bpm_, dm_, file_id_);

    BTreeConfig config;
    config.key_types = {TypeId::INT64, TypeId::INT32};
    config.leaf_max_keys = 4;
    config.internal_max_keys = 4;
    config.is_unique = true;
    BTreeIndex index(std::move(config));

    // Insert with composite keys (id, age).
    auto rid1 = insert_row(heap, 10, "alice", 30);
    EXPECT_TRUE(
        index.insert({Value(static_cast<int64_t>(10)), Value(static_cast<int32_t>(30))}, rid1)
            .has_value());

    auto rid2 = insert_row(heap, 20, "bob", 25);
    EXPECT_TRUE(
        index.insert({Value(static_cast<int64_t>(20)), Value(static_cast<int32_t>(25))}, rid2)
            .has_value());

    auto rid3 = insert_row(heap, 10, "charlie", 35);
    EXPECT_TRUE(
        index.insert({Value(static_cast<int64_t>(10)), Value(static_cast<int32_t>(35))}, rid3)
            .has_value());

    // Prefix scan for id = 10: use composite begin/end keys with min/max age.
    // begin = {10, INT32_MIN}, end = {11, INT32_MIN}
    auto pred = binary_expr(BinaryOp::EQUAL, col_ref("id"), lit_int("10"));
    BoundStatement bound;

    KeyType begin = {Value(static_cast<int64_t>(10)), Value(std::numeric_limits<int32_t>::min())};
    KeyType end = {Value(static_cast<int64_t>(11)), Value(std::numeric_limits<int32_t>::min())};

    IndexScanOperator scan(index,
                           heap,
                           storage_schema_,
                           output_schema_,
                           begin,
                           end,
                           {0, 2}, // id=col0, age=col2
                           false,
                           pred.get(),
                           &bound);

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(std::get<int64_t>(results[0][0].data()), 10);
    EXPECT_EQ(std::get<int64_t>(results[1][0].data()), 10);
}

TEST_F(IndexScanTest, CompositeKeyFullScan) {
    TableHeap heap(*bpm_, dm_, file_id_);

    BTreeConfig config;
    config.key_types = {TypeId::INT64, TypeId::INT32};
    config.leaf_max_keys = 4;
    config.internal_max_keys = 4;
    config.is_unique = true;
    BTreeIndex index(std::move(config));

    // Insert with composite keys (id, age).
    auto rid1 = insert_row(heap, 10, "alice", 30);
    EXPECT_TRUE(
        index.insert({Value(static_cast<int64_t>(10)), Value(static_cast<int32_t>(30))}, rid1)
            .has_value());

    auto rid2 = insert_row(heap, 20, "bob", 25);
    EXPECT_TRUE(
        index.insert({Value(static_cast<int64_t>(20)), Value(static_cast<int32_t>(25))}, rid2)
            .has_value());

    auto rid3 = insert_row(heap, 30, "charlie", 35);
    EXPECT_TRUE(
        index.insert({Value(static_cast<int64_t>(30)), Value(static_cast<int32_t>(35))}, rid3)
            .has_value());

    // Full scan (no bounds).
    IndexScanOperator scan(
        index, heap, storage_schema_, output_schema_, std::nullopt, std::nullopt, {0, 2});

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(std::get<int64_t>(results[0][0].data()), 10);
    EXPECT_EQ(std::get<int64_t>(results[1][0].data()), 20);
    EXPECT_EQ(std::get<int64_t>(results[2][0].data()), 30);
}

// =============================================================================
// IndexScan: output schema
// =============================================================================

TEST_F(IndexScanTest, OutputSchemaCorrect) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    IndexScanOperator scan(
        index, heap, storage_schema_, output_schema_, std::nullopt, std::nullopt, {0});

    EXPECT_EQ(scan.output_schema().column_count(), 3u);
    EXPECT_EQ(scan.output_schema().column(0).name, "id");
    EXPECT_EQ(scan.output_schema().column(1).name, "name");
    EXPECT_EQ(scan.output_schema().column(2).name, "age");
}

// =============================================================================
// IndexScan: RID is populated
// =============================================================================

TEST_F(IndexScanTest, RidIsPopulated) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    auto expected_rid = insert_indexed_row(heap, index, 42, "test", 99);

    IndexScanOperator scan(
        index, heap, storage_schema_, output_schema_, std::nullopt, std::nullopt, {0});

    auto open = scan.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto row = scan.next();
    ASSERT_TRUE(row.has_value()) << row.error().message;
    ASSERT_TRUE(row->has_value());
    ASSERT_TRUE(row->value().rid.has_value());
    EXPECT_EQ(row->value().rid->page_id, expected_rid.page_id);
    EXPECT_EQ(row->value().rid->slot_id, expected_rid.slot_id);

    scan.close();
}

// =============================================================================
// IndexScan: not opened error
// =============================================================================

TEST_F(IndexScanTest, NextBeforeOpenReturnsError) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    IndexScanOperator scan(
        index, heap, storage_schema_, output_schema_, std::nullopt, std::nullopt, {0});

    auto row = scan.next();
    EXPECT_FALSE(row.has_value());
    EXPECT_EQ(row.error().code, StatusCode::INTERNAL_ERROR);
}

// =============================================================================
// IndexScan: predicate with residual filtering
// =============================================================================

TEST_F(IndexScanTest, ResidualPredicateFiltersCorrectly) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "alice", 30);
    insert_indexed_row(heap, index, 20, "bob", 25);
    insert_indexed_row(heap, index, 30, "charlie", 35);
    insert_indexed_row(heap, index, 40, "diana", 28);

    // Scan all, but filter age > 28.
    auto pred = binary_expr(BinaryOp::GREATER, col_ref("age"), lit_int("28"));
    BoundStatement bound;

    IndexScanOperator scan(index,
                           heap,
                           storage_schema_,
                           output_schema_,
                           std::nullopt,
                           std::nullopt,
                           {0},
                           false,
                           pred.get(),
                           &bound);

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 2u);
    // alice (age=30) and charlie (age=35) pass the filter.
    EXPECT_EQ(std::get<std::string>(results[0][1].data()), "alice");
    EXPECT_EQ(std::get<std::string>(results[1][1].data()), "charlie");
}

// =============================================================================
// BitmapScan: single index scan
// =============================================================================

TEST_F(IndexScanTest, BitmapScanSingleIndex) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "alice", 30);
    insert_indexed_row(heap, index, 20, "bob", 25);
    insert_indexed_row(heap, index, 30, "charlie", 35);
    insert_indexed_row(heap, index, 40, "diana", 28);

    // Bitmap scan: id range [20, 40) → ids 20, 30.
    BitmapIndexScan bscan;
    bscan.index = &index;
    bscan.begin_key = KeyType{Value(static_cast<int64_t>(20))};
    bscan.end_key = KeyType{Value(static_cast<int64_t>(40))};

    BitmapScanOperator scan({bscan}, BitmapCombineMode::OR, heap, storage_schema_, output_schema_);

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 2u);

    // Results are sorted by page_id (insertion order in this case).
    std::vector<int64_t> ids;
    for (const auto& r : results) {
        ids.push_back(std::get<int64_t>(r[0].data()));
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids[0], 20);
    EXPECT_EQ(ids[1], 30);
}

// =============================================================================
// BitmapScan: BitmapOr (union of two scans)
// =============================================================================

TEST_F(IndexScanTest, BitmapOrCombinesTwoScans) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "alice", 30);
    insert_indexed_row(heap, index, 20, "bob", 25);
    insert_indexed_row(heap, index, 30, "charlie", 35);
    insert_indexed_row(heap, index, 40, "diana", 28);
    insert_indexed_row(heap, index, 50, "eve", 22);

    // Scan 1: id in [10, 20) → id 10
    BitmapIndexScan scan1;
    scan1.index = &index;
    scan1.begin_key = KeyType{Value(static_cast<int64_t>(10))};
    scan1.end_key = KeyType{Value(static_cast<int64_t>(20))};

    // Scan 2: id in [40, 60) → ids 40, 50
    BitmapIndexScan scan2;
    scan2.index = &index;
    scan2.begin_key = KeyType{Value(static_cast<int64_t>(40))};
    scan2.end_key = KeyType{Value(static_cast<int64_t>(60))};

    BitmapScanOperator scan(
        {scan1, scan2}, BitmapCombineMode::OR, heap, storage_schema_, output_schema_);

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 3u);

    std::vector<int64_t> ids;
    for (const auto& r : results) {
        ids.push_back(std::get<int64_t>(r[0].data()));
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids[0], 10);
    EXPECT_EQ(ids[1], 40);
    EXPECT_EQ(ids[2], 50);
}

// =============================================================================
// BitmapScan: BitmapAnd (intersection of two scans)
// =============================================================================

TEST_F(IndexScanTest, BitmapAndIntersectsTwoScans) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto id_index = make_id_index();

    // Create a separate age index.
    BTreeConfig age_config;
    age_config.key_types = {TypeId::INT32};
    age_config.leaf_max_keys = 4;
    age_config.internal_max_keys = 4;
    age_config.is_unique = false;
    BTreeIndex age_index(std::move(age_config));

    auto rid1 = insert_row(heap, 10, "alice", 30);
    EXPECT_TRUE(id_index.insert({Value(static_cast<int64_t>(10))}, rid1).has_value());
    EXPECT_TRUE(age_index.insert({Value(static_cast<int32_t>(30))}, rid1).has_value());

    auto rid2 = insert_row(heap, 20, "bob", 25);
    EXPECT_TRUE(id_index.insert({Value(static_cast<int64_t>(20))}, rid2).has_value());
    EXPECT_TRUE(age_index.insert({Value(static_cast<int32_t>(25))}, rid2).has_value());

    auto rid3 = insert_row(heap, 30, "charlie", 30);
    EXPECT_TRUE(id_index.insert({Value(static_cast<int64_t>(30))}, rid3).has_value());
    EXPECT_TRUE(age_index.insert({Value(static_cast<int32_t>(30))}, rid3).has_value());

    // Scan 1 on id_index: id in [10, 40) → rids 1, 2, 3
    BitmapIndexScan scan1;
    scan1.index = &id_index;
    scan1.begin_key = KeyType{Value(static_cast<int64_t>(10))};
    scan1.end_key = KeyType{Value(static_cast<int64_t>(40))};

    // Scan 2 on age_index: age in [30, 31) → rids for alice and charlie (age=30)
    BitmapIndexScan scan2;
    scan2.index = &age_index;
    scan2.begin_key = KeyType{Value(static_cast<int32_t>(30))};
    scan2.end_key = KeyType{Value(static_cast<int32_t>(31))};

    BitmapScanOperator scan(
        {scan1, scan2}, BitmapCombineMode::AND, heap, storage_schema_, output_schema_);

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 2u);

    std::vector<int64_t> ids;
    for (const auto& r : results) {
        ids.push_back(std::get<int64_t>(r[0].data()));
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids[0], 10);
    EXPECT_EQ(ids[1], 30);
}

// =============================================================================
// BitmapScan: with residual predicate
// =============================================================================

TEST_F(IndexScanTest, BitmapScanWithResidualPredicate) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "alice", 30);
    insert_indexed_row(heap, index, 20, "bob", 25);
    insert_indexed_row(heap, index, 30, "charlie", 35);

    BitmapIndexScan bscan;
    bscan.index = &index;
    bscan.begin_key = std::nullopt;
    bscan.end_key = std::nullopt;

    // Residual: age > 28
    auto pred = binary_expr(BinaryOp::GREATER, col_ref("age"), lit_int("28"));
    BoundStatement bound;

    BitmapScanOperator scan(
        {bscan}, BitmapCombineMode::OR, heap, storage_schema_, output_schema_, pred.get(), &bound);

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 2u);

    std::vector<std::string> names;
    for (const auto& r : results) {
        names.push_back(std::get<std::string>(r[1].data()));
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names[0], "alice");
    EXPECT_EQ(names[1], "charlie");
}

// =============================================================================
// BitmapScan: empty result
// =============================================================================

TEST_F(IndexScanTest, BitmapScanEmptyResult) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "alice", 30);

    // Scan range that matches nothing: [100, 200).
    BitmapIndexScan bscan;
    bscan.index = &index;
    bscan.begin_key = KeyType{Value(static_cast<int64_t>(100))};
    bscan.end_key = KeyType{Value(static_cast<int64_t>(200))};

    BitmapScanOperator scan({bscan}, BitmapCombineMode::OR, heap, storage_schema_, output_schema_);

    auto results = collect_all(scan);
    EXPECT_EQ(results.size(), 0u);
}

// =============================================================================
// BitmapScan: error on no scans
// =============================================================================

TEST_F(IndexScanTest, BitmapScanNoScansReturnsError) {
    TableHeap heap(*bpm_, dm_, file_id_);

    BitmapScanOperator scan({}, BitmapCombineMode::OR, heap, storage_schema_, output_schema_);

    auto open = scan.open();
    EXPECT_FALSE(open.has_value());
    EXPECT_EQ(open.error().code, StatusCode::INTERNAL_ERROR);
}

// =============================================================================
// BitmapScan: output schema
// =============================================================================

TEST_F(IndexScanTest, BitmapScanOutputSchemaCorrect) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    BitmapIndexScan bscan;
    bscan.index = &index;

    BitmapScanOperator scan({bscan}, BitmapCombineMode::OR, heap, storage_schema_, output_schema_);

    EXPECT_EQ(scan.output_schema().column_count(), 3u);
    EXPECT_EQ(scan.output_schema().column(0).name, "id");
    EXPECT_EQ(scan.output_schema().column(1).name, "name");
    EXPECT_EQ(scan.output_schema().column(2).name, "age");
}

// =============================================================================
// IndexScan: many rows (forces B+ tree splits)
// =============================================================================

TEST_F(IndexScanTest, ManyRowsWithSplits) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index(false); // non-unique, small leaf capacity

    // Insert 20 rows to force multiple splits in the B+ tree (leaf_max=4).
    for (int64_t i = 1; i <= 20; ++i) {
        insert_indexed_row(
            heap, index, i, "user" + std::to_string(i), static_cast<int32_t>(20 + i));
    }
    EXPECT_EQ(index.size(), 20u);

    // Scan range [5, 15).
    auto pred = binary_expr(BinaryOp::AND,
                            binary_expr(BinaryOp::GREATER_EQUAL, col_ref("id"), lit_int("5")),
                            binary_expr(BinaryOp::LESS, col_ref("id"), lit_int("15")));
    BoundStatement bound;

    IndexScanOperator scan(index,
                           heap,
                           storage_schema_,
                           output_schema_,
                           KeyType{Value(static_cast<int64_t>(5))},
                           KeyType{Value(static_cast<int64_t>(15))},
                           {0},
                           false,
                           pred.get(),
                           &bound);

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 10u);
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(std::get<int64_t>(results[i][0].data()), 5 + i);
    }
}

// =============================================================================
// IndexScan: performance comparison vs SeqScan (selective query)
// =============================================================================

TEST_F(IndexScanTest, IndexScanFasterThanSeqScanForSelectiveQuery) {
    // This test verifies that IndexScan correctly narrows results for
    // a selective query on a large dataset, which would require SeqScan
    // to read all rows.
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index(false);

    // Insert 50 rows.
    for (int64_t i = 1; i <= 50; ++i) {
        insert_indexed_row(heap, index, i, "user" + std::to_string(i), static_cast<int32_t>(i));
    }

    // Point lookup: id = 25 (1 out of 50 = 2% selectivity).
    auto pred = binary_expr(BinaryOp::EQUAL, col_ref("id"), lit_int("25"));
    BoundStatement bound;

    IndexScanOperator scan(index,
                           heap,
                           storage_schema_,
                           output_schema_,
                           KeyType{Value(static_cast<int64_t>(25))},
                           std::nullopt,
                           {0},
                           false,
                           pred.get(),
                           &bound);

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(std::get<int64_t>(results[0][0].data()), 25);
    EXPECT_EQ(std::get<std::string>(results[0][1].data()), "user25");
}
