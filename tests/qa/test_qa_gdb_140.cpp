/// @file test_qa_gdb_140.cpp
/// @brief QA adversarial tests for GDB-140: IndexScan operator with predicate
///        pushdown.
///
/// Targets edge cases: single-row table, duplicate keys, max/min key values,
/// empty range, full-table index scan, index-only with predicate, bitmap
/// deduplication, bitmap AND empty intersection, bitmap OR overlapping ranges,
/// hash index scan, close-then-reopen, plan node names, large composite keys.

#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/bitmap_scan.h"
#include "sixseven/executor/hash_index_scan.h"
#include "sixseven/executor/index_scan.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/index/btree_index.h"
#include "sixseven/index/hash_index.h"
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

class QA140IndexScanTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_qa140_index_scan.db";
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

    RID insert_row(TableHeap& heap, int64_t id, const std::string& name, int32_t age) {
        std::vector<Value> vals = {Value(id), Value(name), Value(age)};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        EXPECT_TRUE(bytes.has_value()) << bytes.error().message;
        auto rid = heap.insert_tuple(*bytes);
        EXPECT_TRUE(rid.has_value()) << rid.error().message;
        return *rid;
    }

    RID insert_indexed_row(
        TableHeap& heap, BTreeIndex& index, int64_t id, const std::string& name, int32_t age) {
        auto rid = insert_row(heap, id, name, age);
        KeyType key = {Value(id)};
        auto ins = index.insert(key, rid);
        EXPECT_TRUE(ins.has_value()) << ins.error().message;
        return rid;
    }

    BTreeIndex make_id_index(bool is_unique = true) {
        BTreeConfig config;
        config.key_types = {TypeId::INT64};
        config.leaf_max_keys = 4;
        config.internal_max_keys = 4;
        config.is_unique = is_unique;
        return BTreeIndex(std::move(config));
    }

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

// ---------------------------------------------------------------------------
// IndexScan: single row table
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, SingleRowPointLookup) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 42, "only", 99);

    auto pred = binary_expr(BinaryOp::EQUAL, col_ref("id"), lit_int("42"));
    BoundStatement bound;

    IndexScanOperator scan(index,
                           heap,
                           storage_schema_,
                           output_schema_,
                           KeyType{Value(static_cast<int64_t>(42))},
                           std::nullopt,
                           {0},
                           false,
                           pred.get(),
                           &bound);

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(std::get<int64_t>(results[0][0].data()), 42);
}

TEST_F(QA140IndexScanTest, SingleRowMiss) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 42, "only", 99);

    auto pred = binary_expr(BinaryOp::EQUAL, col_ref("id"), lit_int("999"));
    BoundStatement bound;

    IndexScanOperator scan(index,
                           heap,
                           storage_schema_,
                           output_schema_,
                           KeyType{Value(static_cast<int64_t>(999))},
                           std::nullopt,
                           {0},
                           false,
                           pred.get(),
                           &bound);

    auto results = collect_all(scan);
    EXPECT_EQ(results.size(), 0u);
}

// ---------------------------------------------------------------------------
// IndexScan: duplicate keys (non-unique index)
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, DuplicateKeysAllReturned) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index(false); // non-unique

    insert_indexed_row(heap, index, 10, "alice", 30);
    insert_indexed_row(heap, index, 10, "alice2", 31);
    insert_indexed_row(heap, index, 10, "alice3", 32);
    insert_indexed_row(heap, index, 20, "bob", 25);

    // Scan from key=10, with predicate id = 10.
    auto pred = binary_expr(BinaryOp::EQUAL, col_ref("id"), lit_int("10"));
    BoundStatement bound;

    IndexScanOperator scan(index,
                           heap,
                           storage_schema_,
                           output_schema_,
                           KeyType{Value(static_cast<int64_t>(10))},
                           KeyType{Value(static_cast<int64_t>(11))},
                           {0},
                           false,
                           pred.get(),
                           &bound);

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 3u);
    for (const auto& r : results) {
        EXPECT_EQ(std::get<int64_t>(r[0].data()), 10);
    }
}

// ---------------------------------------------------------------------------
// IndexScan: boundary key values
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, MaxInt64Key) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    int64_t max_val = std::numeric_limits<int64_t>::max();
    insert_indexed_row(heap, index, max_val, "max", 1);

    IndexScanOperator scan(
        index, heap, storage_schema_, output_schema_, KeyType{Value(max_val)}, std::nullopt, {0});

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(std::get<int64_t>(results[0][0].data()), max_val);
}

TEST_F(QA140IndexScanTest, MinInt64Key) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    int64_t min_val = std::numeric_limits<int64_t>::min();
    insert_indexed_row(heap, index, min_val, "min", 1);

    IndexScanOperator scan(
        index, heap, storage_schema_, output_schema_, std::nullopt, std::nullopt, {0});

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(std::get<int64_t>(results[0][0].data()), min_val);
}

TEST_F(QA140IndexScanTest, NegativeKeys) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, -100, "neg1", 1);
    insert_indexed_row(heap, index, -50, "neg2", 2);
    insert_indexed_row(heap, index, 0, "zero", 3);
    insert_indexed_row(heap, index, 50, "pos1", 4);

    // Range scan [-100, 1) should return -100, -50, 0.
    IndexScanOperator scan(index,
                           heap,
                           storage_schema_,
                           output_schema_,
                           KeyType{Value(static_cast<int64_t>(-100))},
                           KeyType{Value(static_cast<int64_t>(1))},
                           {0});

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(std::get<int64_t>(results[0][0].data()), -100);
    EXPECT_EQ(std::get<int64_t>(results[1][0].data()), -50);
    EXPECT_EQ(std::get<int64_t>(results[2][0].data()), 0);
}

// ---------------------------------------------------------------------------
// IndexScan: empty range (begin >= end)
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, EmptyRangeBeginEqualsEnd) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "alice", 30);
    insert_indexed_row(heap, index, 20, "bob", 25);

    // Range [10, 10) should return nothing.
    IndexScanOperator scan(index,
                           heap,
                           storage_schema_,
                           output_schema_,
                           KeyType{Value(static_cast<int64_t>(10))},
                           KeyType{Value(static_cast<int64_t>(10))},
                           {0});

    auto results = collect_all(scan);
    EXPECT_EQ(results.size(), 0u);
}

TEST_F(QA140IndexScanTest, EmptyRangeBeginGreaterThanEnd) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "alice", 30);

    // Range [20, 10) — begin > end — should return nothing.
    IndexScanOperator scan(index,
                           heap,
                           storage_schema_,
                           output_schema_,
                           KeyType{Value(static_cast<int64_t>(20))},
                           KeyType{Value(static_cast<int64_t>(10))},
                           {0});

    auto results = collect_all(scan);
    EXPECT_EQ(results.size(), 0u);
}

// ---------------------------------------------------------------------------
// IndexScan: index-only with predicate
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, IndexOnlyWithResidualPredicate) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "alice", 30);
    insert_indexed_row(heap, index, 20, "bob", 25);
    insert_indexed_row(heap, index, 30, "charlie", 35);

    OutputColumn id_col{"", "id", TypeId::INT64, false, 0};
    auto index_only_schema = OutputSchema({id_col});

    // Predicate: id > 15
    auto pred = binary_expr(BinaryOp::GREATER, col_ref("id"), lit_int("15"));
    BoundStatement bound;

    IndexScanOperator scan(index,
                           heap,
                           storage_schema_,
                           index_only_schema,
                           std::nullopt,
                           std::nullopt,
                           {0},
                           true,
                           pred.get(),
                           &bound);

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
    scan.close();

    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 20);
    EXPECT_EQ(ids[1], 30);
}

// ---------------------------------------------------------------------------
// IndexScan: index-only RID is populated
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, IndexOnlyRidPopulated) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    auto expected_rid = insert_indexed_row(heap, index, 42, "test", 99);

    OutputColumn id_col{"", "id", TypeId::INT64, false, 0};
    auto index_only_schema = OutputSchema({id_col});

    IndexScanOperator scan(
        index, heap, storage_schema_, index_only_schema, std::nullopt, std::nullopt, {0}, true);

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

// ---------------------------------------------------------------------------
// IndexScan: plan node names
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, PlanNodeName) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    IndexScanOperator scan(
        index, heap, storage_schema_, output_schema_, std::nullopt, std::nullopt, {0});

    EXPECT_EQ(scan.plan_node_name(), "Index Scan");
}

TEST_F(QA140IndexScanTest, PlanNodeDetailNoTableName) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    IndexScanOperator scan(
        index, heap, storage_schema_, output_schema_, std::nullopt, std::nullopt, {0});

    // output_schema columns have empty table_name
    EXPECT_EQ(scan.plan_node_detail(), "using index");
}

TEST_F(QA140IndexScanTest, PlanNodeDetailWithTableName) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    OutputColumn c1{"users", "id", TypeId::INT64, false, 0};
    OutputColumn c2{"users", "name", TypeId::STRING, true, 0};
    auto named_schema = OutputSchema({c1, c2});

    IndexScanOperator scan(
        index, heap, storage_schema_, named_schema, std::nullopt, std::nullopt, {0});

    EXPECT_EQ(scan.plan_node_detail(), "on users using index");
}

// ---------------------------------------------------------------------------
// IndexScan: close and reopen
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, CloseAndReopen) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "alice", 30);
    insert_indexed_row(heap, index, 20, "bob", 25);

    IndexScanOperator scan(
        index, heap, storage_schema_, output_schema_, std::nullopt, std::nullopt, {0});

    // First scan.
    auto results1 = collect_all(scan);
    ASSERT_EQ(results1.size(), 2u);

    // Second scan — should return same results.
    auto results2 = collect_all(scan);
    ASSERT_EQ(results2.size(), 2u);
    EXPECT_EQ(std::get<int64_t>(results2[0][0].data()), 10);
    EXPECT_EQ(std::get<int64_t>(results2[1][0].data()), 20);
}

// ---------------------------------------------------------------------------
// IndexScan: no predicate or bound (null predicate skips filtering)
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, NullPredicateReturnsAllInRange) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "alice", 30);
    insert_indexed_row(heap, index, 20, "bob", 25);
    insert_indexed_row(heap, index, 30, "charlie", 35);

    // No predicate (nullptr), no bound (nullptr).
    IndexScanOperator scan(index,
                           heap,
                           storage_schema_,
                           output_schema_,
                           KeyType{Value(static_cast<int64_t>(20))},
                           std::nullopt,
                           {0},
                           false,
                           nullptr,
                           nullptr);

    auto results = collect_all(scan);
    // Should return all rows from key 20 onwards.
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(std::get<int64_t>(results[0][0].data()), 20);
    EXPECT_EQ(std::get<int64_t>(results[1][0].data()), 30);
}

// ---------------------------------------------------------------------------
// IndexScan: large dataset stress
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, LargeDataset100Rows) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index(false);

    for (int64_t i = 1; i <= 100; ++i) {
        insert_indexed_row(
            heap, index, i, "user" + std::to_string(i), static_cast<int32_t>(i % 50));
    }
    EXPECT_EQ(index.size(), 100u);

    // Full scan — should return all 100 in order.
    IndexScanOperator scan(
        index, heap, storage_schema_, output_schema_, std::nullopt, std::nullopt, {0});

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 100u);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(std::get<int64_t>(results[i][0].data()), i + 1);
    }
}

// ---------------------------------------------------------------------------
// BitmapScan: overlapping ranges with OR — deduplication
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, BitmapOrOverlappingRangesDedup) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "a", 1);
    insert_indexed_row(heap, index, 20, "b", 2);
    insert_indexed_row(heap, index, 30, "c", 3);
    insert_indexed_row(heap, index, 40, "d", 4);

    // Scan 1: [10, 30) → ids 10, 20
    BitmapIndexScan scan1;
    scan1.index = &index;
    scan1.begin_key = KeyType{Value(static_cast<int64_t>(10))};
    scan1.end_key = KeyType{Value(static_cast<int64_t>(30))};

    // Scan 2: [20, 40) → ids 20, 30 (overlaps id=20)
    BitmapIndexScan scan2;
    scan2.index = &index;
    scan2.begin_key = KeyType{Value(static_cast<int64_t>(20))};
    scan2.end_key = KeyType{Value(static_cast<int64_t>(40))};

    BitmapScanOperator scan(
        {scan1, scan2}, BitmapCombineMode::OR, heap, storage_schema_, output_schema_);

    auto results = collect_all(scan);
    // Union of {10,20} and {20,30} = {10,20,30} — id=20 should NOT be
    // duplicated.
    ASSERT_EQ(results.size(), 3u);

    std::vector<int64_t> ids;
    for (const auto& r : results) {
        ids.push_back(std::get<int64_t>(r[0].data()));
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids[0], 10);
    EXPECT_EQ(ids[1], 20);
    EXPECT_EQ(ids[2], 30);
}

// ---------------------------------------------------------------------------
// BitmapScan: AND with disjoint ranges — empty intersection
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, BitmapAndDisjointRangesEmpty) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "a", 1);
    insert_indexed_row(heap, index, 20, "b", 2);
    insert_indexed_row(heap, index, 30, "c", 3);
    insert_indexed_row(heap, index, 40, "d", 4);

    // Scan 1: [10, 20) → id 10
    BitmapIndexScan scan1;
    scan1.index = &index;
    scan1.begin_key = KeyType{Value(static_cast<int64_t>(10))};
    scan1.end_key = KeyType{Value(static_cast<int64_t>(20))};

    // Scan 2: [30, 50) → ids 30, 40
    BitmapIndexScan scan2;
    scan2.index = &index;
    scan2.begin_key = KeyType{Value(static_cast<int64_t>(30))};
    scan2.end_key = KeyType{Value(static_cast<int64_t>(50))};

    BitmapScanOperator scan(
        {scan1, scan2}, BitmapCombineMode::AND, heap, storage_schema_, output_schema_);

    auto results = collect_all(scan);
    EXPECT_EQ(results.size(), 0u);
}

// ---------------------------------------------------------------------------
// BitmapScan: single scan — same as plain bitmap
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, BitmapSingleScanWithAnd) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "a", 1);
    insert_indexed_row(heap, index, 20, "b", 2);

    BitmapIndexScan bscan;
    bscan.index = &index;

    // AND mode with single scan should return all matches.
    BitmapScanOperator scan({bscan}, BitmapCombineMode::AND, heap, storage_schema_, output_schema_);

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 2u);
}

// ---------------------------------------------------------------------------
// BitmapScan: plan node detail
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, BitmapPlanNodeDetailAnd) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();
    BitmapIndexScan bscan;
    bscan.index = &index;

    BitmapScanOperator scan({bscan}, BitmapCombineMode::AND, heap, storage_schema_, output_schema_);
    EXPECT_EQ(scan.plan_node_name(), "Bitmap Scan");
    EXPECT_EQ(scan.plan_node_detail(), "BitmapAnd");
}

TEST_F(QA140IndexScanTest, BitmapPlanNodeDetailOr) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();
    BitmapIndexScan bscan;
    bscan.index = &index;

    BitmapScanOperator scan({bscan}, BitmapCombineMode::OR, heap, storage_schema_, output_schema_);
    EXPECT_EQ(scan.plan_node_detail(), "BitmapOr");
}

// ---------------------------------------------------------------------------
// BitmapScan: null index in scan descriptor
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, BitmapNullIndexReturnsError) {
    TableHeap heap(*bpm_, dm_, file_id_);

    BitmapIndexScan bscan;
    bscan.index = nullptr;

    BitmapScanOperator scan({bscan}, BitmapCombineMode::OR, heap, storage_schema_, output_schema_);

    auto open = scan.open();
    EXPECT_FALSE(open.has_value());
    EXPECT_EQ(open.error().code, StatusCode::INTERNAL_ERROR);
}

// ---------------------------------------------------------------------------
// BitmapScan: close and reopen
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, BitmapCloseAndReopen) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "a", 1);
    insert_indexed_row(heap, index, 20, "b", 2);

    BitmapIndexScan bscan;
    bscan.index = &index;

    BitmapScanOperator scan({bscan}, BitmapCombineMode::OR, heap, storage_schema_, output_schema_);

    auto results1 = collect_all(scan);
    ASSERT_EQ(results1.size(), 2u);

    // Second open should reset and return same results.
    auto results2 = collect_all(scan);
    ASSERT_EQ(results2.size(), 2u);
}

// ---------------------------------------------------------------------------
// HashIndexScan: basic lookup
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, HashIndexBasicLookup) {
    TableHeap heap(*bpm_, dm_, file_id_);

    HashIndexConfig hcfg;
    hcfg.key_types = {TypeId::INT64};
    hcfg.is_unique = true;
    HashIndex hash_index(hcfg);

    auto rid1 = insert_row(heap, 10, "alice", 30);
    EXPECT_TRUE(hash_index.insert({Value(static_cast<int64_t>(10))}, rid1).has_value());
    auto rid2 = insert_row(heap, 20, "bob", 25);
    EXPECT_TRUE(hash_index.insert({Value(static_cast<int64_t>(20))}, rid2).has_value());

    HashIndexScanOperator scan(hash_index,
                               heap,
                               storage_schema_,
                               output_schema_,
                               KeyType{Value(static_cast<int64_t>(10))},
                               {0});

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(std::get<int64_t>(results[0][0].data()), 10);
    EXPECT_EQ(std::get<std::string>(results[0][1].data()), "alice");
}

// ---------------------------------------------------------------------------
// HashIndexScan: key not found
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, HashIndexKeyNotFound) {
    TableHeap heap(*bpm_, dm_, file_id_);

    HashIndexConfig hcfg;
    hcfg.key_types = {TypeId::INT64};
    hcfg.is_unique = true;
    HashIndex hash_index(hcfg);

    auto rid1 = insert_row(heap, 10, "alice", 30);
    EXPECT_TRUE(hash_index.insert({Value(static_cast<int64_t>(10))}, rid1).has_value());

    HashIndexScanOperator scan(hash_index,
                               heap,
                               storage_schema_,
                               output_schema_,
                               KeyType{Value(static_cast<int64_t>(999))},
                               {0});

    auto results = collect_all(scan);
    EXPECT_EQ(results.size(), 0u);
}

// ---------------------------------------------------------------------------
// HashIndexScan: multiple matches (non-unique)
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, HashIndexMultipleMatches) {
    TableHeap heap(*bpm_, dm_, file_id_);

    HashIndexConfig hcfg;
    hcfg.key_types = {TypeId::INT64};
    hcfg.is_unique = false;
    HashIndex hash_index(hcfg);

    auto rid1 = insert_row(heap, 10, "alice", 30);
    EXPECT_TRUE(hash_index.insert({Value(static_cast<int64_t>(10))}, rid1).has_value());
    auto rid2 = insert_row(heap, 10, "alice2", 31);
    EXPECT_TRUE(hash_index.insert({Value(static_cast<int64_t>(10))}, rid2).has_value());
    auto rid3 = insert_row(heap, 20, "bob", 25);
    EXPECT_TRUE(hash_index.insert({Value(static_cast<int64_t>(20))}, rid3).has_value());

    HashIndexScanOperator scan(hash_index,
                               heap,
                               storage_schema_,
                               output_schema_,
                               KeyType{Value(static_cast<int64_t>(10))},
                               {0});

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 2u);
    for (const auto& r : results) {
        EXPECT_EQ(std::get<int64_t>(r[0].data()), 10);
    }
}

// ---------------------------------------------------------------------------
// HashIndexScan: index-only mode
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, HashIndexOnlyScan) {
    TableHeap heap(*bpm_, dm_, file_id_);

    HashIndexConfig hcfg;
    hcfg.key_types = {TypeId::INT64};
    hcfg.is_unique = true;
    HashIndex hash_index(hcfg);

    auto rid1 = insert_row(heap, 42, "test", 99);
    EXPECT_TRUE(hash_index.insert({Value(static_cast<int64_t>(42))}, rid1).has_value());

    OutputColumn id_col{"", "id", TypeId::INT64, false, 0};
    auto index_only_schema = OutputSchema({id_col});

    HashIndexScanOperator scan(hash_index,
                               heap,
                               storage_schema_,
                               index_only_schema,
                               KeyType{Value(static_cast<int64_t>(42))},
                               {0},
                               true);

    auto open = scan.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto row = scan.next();
    ASSERT_TRUE(row.has_value()) << row.error().message;
    ASSERT_TRUE(row->has_value());
    EXPECT_EQ(std::get<int64_t>(row->value().values[0].data()), 42);

    auto row2 = scan.next();
    ASSERT_TRUE(row2.has_value()) << row2.error().message;
    EXPECT_FALSE(row2->has_value());

    scan.close();
}

// ---------------------------------------------------------------------------
// HashIndexScan: plan node names
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, HashIndexPlanNodeName) {
    TableHeap heap(*bpm_, dm_, file_id_);
    HashIndexConfig hcfg;
    hcfg.key_types = {TypeId::INT64};
    HashIndex hash_index(hcfg);

    HashIndexScanOperator scan(hash_index,
                               heap,
                               storage_schema_,
                               output_schema_,
                               KeyType{Value(static_cast<int64_t>(1))},
                               {0});

    EXPECT_EQ(scan.plan_node_name(), "Hash Index Scan");
}

TEST_F(QA140IndexScanTest, HashIndexPlanNodeDetailNoTable) {
    TableHeap heap(*bpm_, dm_, file_id_);
    HashIndexConfig hcfg2;
    hcfg2.key_types = {TypeId::INT64};
    HashIndex hash_index(hcfg2);

    HashIndexScanOperator scan(hash_index,
                               heap,
                               storage_schema_,
                               output_schema_,
                               KeyType{Value(static_cast<int64_t>(1))},
                               {0});

    EXPECT_EQ(scan.plan_node_detail(), "using hash index");
}

TEST_F(QA140IndexScanTest, HashIndexPlanNodeDetailWithTable) {
    TableHeap heap(*bpm_, dm_, file_id_);
    HashIndexConfig hcfg3;
    hcfg3.key_types = {TypeId::INT64};
    HashIndex hash_index(hcfg3);

    OutputColumn c1{"products", "id", TypeId::INT64, false, 0};
    auto named_schema = OutputSchema({c1});

    HashIndexScanOperator scan(hash_index,
                               heap,
                               storage_schema_,
                               named_schema,
                               KeyType{Value(static_cast<int64_t>(1))},
                               {0});

    EXPECT_EQ(scan.plan_node_detail(), "on products using hash index");
}

// ---------------------------------------------------------------------------
// HashIndexScan: close and reopen
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, HashIndexCloseAndReopen) {
    TableHeap heap(*bpm_, dm_, file_id_);
    HashIndexConfig hcfg4;
    hcfg4.key_types = {TypeId::INT64};
    hcfg4.is_unique = true;
    HashIndex hash_index(hcfg4);

    auto rid1 = insert_row(heap, 42, "test", 99);
    EXPECT_TRUE(hash_index.insert({Value(static_cast<int64_t>(42))}, rid1).has_value());

    HashIndexScanOperator scan(hash_index,
                               heap,
                               storage_schema_,
                               output_schema_,
                               KeyType{Value(static_cast<int64_t>(42))},
                               {0});

    auto results1 = collect_all(scan);
    ASSERT_EQ(results1.size(), 1u);

    auto results2 = collect_all(scan);
    ASSERT_EQ(results2.size(), 1u);
    EXPECT_EQ(std::get<int64_t>(results2[0][0].data()), 42);
}

// ---------------------------------------------------------------------------
// HashIndexScan: with residual predicate
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, HashIndexWithResidualPredicate) {
    TableHeap heap(*bpm_, dm_, file_id_);
    HashIndexConfig hcfg5;
    hcfg5.key_types = {TypeId::INT64};
    hcfg5.is_unique = false;
    HashIndex hash_index(hcfg5);

    auto rid1 = insert_row(heap, 10, "alice", 30);
    EXPECT_TRUE(hash_index.insert({Value(static_cast<int64_t>(10))}, rid1).has_value());
    auto rid2 = insert_row(heap, 10, "alice2", 20);
    EXPECT_TRUE(hash_index.insert({Value(static_cast<int64_t>(10))}, rid2).has_value());

    // Residual predicate: age > 25
    auto pred = binary_expr(BinaryOp::GREATER, col_ref("age"), lit_int("25"));
    BoundStatement bound;

    HashIndexScanOperator scan(hash_index,
                               heap,
                               storage_schema_,
                               output_schema_,
                               KeyType{Value(static_cast<int64_t>(10))},
                               {0},
                               false,
                               pred.get(),
                               &bound);

    auto results = collect_all(scan);
    // Only alice (age=30) should pass the predicate.
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(std::get<std::string>(results[0][1].data()), "alice");
}

// ---------------------------------------------------------------------------
// BitmapScan: three-way AND
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, BitmapThreeWayAnd) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "a", 1);
    insert_indexed_row(heap, index, 20, "b", 2);
    insert_indexed_row(heap, index, 30, "c", 3);

    // Scan 1: [5, 25) → ids 10, 20
    BitmapIndexScan scan1;
    scan1.index = &index;
    scan1.begin_key = KeyType{Value(static_cast<int64_t>(5))};
    scan1.end_key = KeyType{Value(static_cast<int64_t>(25))};

    // Scan 2: [15, 35) → ids 20, 30
    BitmapIndexScan scan2;
    scan2.index = &index;
    scan2.begin_key = KeyType{Value(static_cast<int64_t>(15))};
    scan2.end_key = KeyType{Value(static_cast<int64_t>(35))};

    // Scan 3: [18, 40) → ids 20, 30
    BitmapIndexScan scan3;
    scan3.index = &index;
    scan3.begin_key = KeyType{Value(static_cast<int64_t>(18))};
    scan3.end_key = KeyType{Value(static_cast<int64_t>(40))};

    BitmapScanOperator scan(
        {scan1, scan2, scan3}, BitmapCombineMode::AND, heap, storage_schema_, output_schema_);

    auto results = collect_all(scan);
    // Intersection: {10,20} ∩ {20,30} ∩ {20,30} = {20}
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(std::get<int64_t>(results[0][0].data()), 20);
}

// ---------------------------------------------------------------------------
// BitmapScan: three-way OR
// ---------------------------------------------------------------------------

TEST_F(QA140IndexScanTest, BitmapThreeWayOr) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto index = make_id_index();

    insert_indexed_row(heap, index, 10, "a", 1);
    insert_indexed_row(heap, index, 20, "b", 2);
    insert_indexed_row(heap, index, 30, "c", 3);
    insert_indexed_row(heap, index, 40, "d", 4);
    insert_indexed_row(heap, index, 50, "e", 5);

    // Scan 1: [10, 11) → id 10
    BitmapIndexScan scan1;
    scan1.index = &index;
    scan1.begin_key = KeyType{Value(static_cast<int64_t>(10))};
    scan1.end_key = KeyType{Value(static_cast<int64_t>(11))};

    // Scan 2: [30, 31) → id 30
    BitmapIndexScan scan2;
    scan2.index = &index;
    scan2.begin_key = KeyType{Value(static_cast<int64_t>(30))};
    scan2.end_key = KeyType{Value(static_cast<int64_t>(31))};

    // Scan 3: [50, 51) → id 50
    BitmapIndexScan scan3;
    scan3.index = &index;
    scan3.begin_key = KeyType{Value(static_cast<int64_t>(50))};
    scan3.end_key = KeyType{Value(static_cast<int64_t>(51))};

    BitmapScanOperator scan(
        {scan1, scan2, scan3}, BitmapCombineMode::OR, heap, storage_schema_, output_schema_);

    auto results = collect_all(scan);
    ASSERT_EQ(results.size(), 3u);

    std::vector<int64_t> ids;
    for (const auto& r : results) {
        ids.push_back(std::get<int64_t>(r[0].data()));
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids[0], 10);
    EXPECT_EQ(ids[1], 30);
    EXPECT_EQ(ids[2], 50);
}
