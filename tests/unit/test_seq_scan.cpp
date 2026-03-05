#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/filter.h"
#include "sixseven/executor/seq_scan.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace sixseven;

// -- Test fixture -------------------------------------------------------------

class SeqScanTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_test_seq_scan.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;

        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);

        // Schema: id (INT32), name (STRING), age (INT32)
        storage_schema_ = Schema({
            {"id", TypeId::INT32},
            {"name", TypeId::STRING},
            {"age", TypeId::INT32},
        });

        OutputColumn c1{"", "id", TypeId::INT32, false, 0};
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
    RID insert_row(TableHeap& heap, int32_t id, const std::string& name, int32_t age) {
        std::vector<Value> vals = {Value(id), Value(name), Value(age)};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        EXPECT_TRUE(bytes.has_value()) << bytes.error().message;
        auto rid = heap.insert_tuple(*bytes);
        EXPECT_TRUE(rid.has_value()) << rid.error().message;
        return *rid;
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

// -- SeqScan tests ------------------------------------------------------------

TEST_F(SeqScanTest, ScanEmptyTable) {
    TableHeap heap(*bpm_, dm_, file_id_);
    SeqScanOperator scan(heap, storage_schema_, output_schema_);

    auto open = scan.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto row = scan.next();
    ASSERT_TRUE(row.has_value()) << row.error().message;
    EXPECT_FALSE(row->has_value()); // No rows.

    scan.close();
}

TEST_F(SeqScanTest, ScanSingleRow) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);

    SeqScanOperator scan(heap, storage_schema_, output_schema_);
    auto open = scan.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto row = scan.next();
    ASSERT_TRUE(row.has_value()) << row.error().message;
    ASSERT_TRUE(row->has_value());

    auto& tuple = row->value();
    ASSERT_EQ(tuple.values.size(), 3u);
    EXPECT_EQ(tuple.values[0].as_int32(), 1);
    EXPECT_EQ(tuple.values[1].as_string(), "alice");
    EXPECT_EQ(tuple.values[2].as_int32(), 30);
    EXPECT_TRUE(tuple.rid.has_value());

    // No more rows.
    auto row2 = scan.next();
    ASSERT_TRUE(row2.has_value()) << row2.error().message;
    EXPECT_FALSE(row2->has_value());

    scan.close();
}

TEST_F(SeqScanTest, ScanMultipleRows) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);
    insert_row(heap, 2, "bob", 25);
    insert_row(heap, 3, "charlie", 35);

    SeqScanOperator scan(heap, storage_schema_, output_schema_);
    auto open = scan.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    int count = 0;
    while (true) {
        auto row = scan.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        ++count;
    }
    EXPECT_EQ(count, 3);

    scan.close();
}

TEST_F(SeqScanTest, ScanVerifiesAllValues) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 42, "test_user", 99);

    SeqScanOperator scan(heap, storage_schema_, output_schema_);
    auto open = scan.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto row = scan.next();
    ASSERT_TRUE(row.has_value()) << row.error().message;
    ASSERT_TRUE(row->has_value());

    auto& t = row->value();
    EXPECT_EQ(t.values[0].as_int32(), 42);
    EXPECT_EQ(t.values[1].as_string(), "test_user");
    EXPECT_EQ(t.values[2].as_int32(), 99);

    scan.close();
}

TEST_F(SeqScanTest, ScanWithPredicate) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);
    insert_row(heap, 2, "bob", 25);
    insert_row(heap, 3, "charlie", 35);

    // Predicate: age > 28
    auto pred = binary_expr(BinaryOp::GREATER, col_ref("age"), lit_int("28"));

    BoundStatement bound;
    SeqScanOperator scan(heap, storage_schema_, output_schema_, pred.get(), &bound);
    auto open = scan.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    std::vector<std::string> names;
    while (true) {
        auto row = scan.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        names.push_back(row->value().values[1].as_string());
    }
    EXPECT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "alice");
    EXPECT_EQ(names[1], "charlie");

    scan.close();
}

TEST_F(SeqScanTest, ScanPredicateRejectsAll) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);
    insert_row(heap, 2, "bob", 25);

    // Predicate: age > 100
    auto pred = binary_expr(BinaryOp::GREATER, col_ref("age"), lit_int("100"));

    BoundStatement bound;
    SeqScanOperator scan(heap, storage_schema_, output_schema_, pred.get(), &bound);
    auto open = scan.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto row = scan.next();
    ASSERT_TRUE(row.has_value()) << row.error().message;
    EXPECT_FALSE(row->has_value()); // All rejected.

    scan.close();
}

TEST_F(SeqScanTest, RidIsPopulated) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto expected_rid = insert_row(heap, 1, "alice", 30);

    SeqScanOperator scan(heap, storage_schema_, output_schema_);
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

TEST_F(SeqScanTest, OutputSchemaCorrect) {
    TableHeap heap(*bpm_, dm_, file_id_);
    SeqScanOperator scan(heap, storage_schema_, output_schema_);

    EXPECT_EQ(scan.output_schema().column_count(), 3u);
    EXPECT_EQ(scan.output_schema().column(0).name, "id");
    EXPECT_EQ(scan.output_schema().column(1).name, "name");
    EXPECT_EQ(scan.output_schema().column(2).name, "age");
    EXPECT_EQ(scan.output_schema().column(0).type_id, TypeId::INT32);
    EXPECT_EQ(scan.output_schema().column(1).type_id, TypeId::STRING);
}

// -- Filter tests -------------------------------------------------------------

TEST_F(SeqScanTest, FilterPassesMatching) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);
    insert_row(heap, 2, "bob", 25);
    insert_row(heap, 3, "charlie", 35);

    auto child = std::make_unique<SeqScanOperator>(heap, storage_schema_, output_schema_);

    // Filter: age >= 30
    auto pred = binary_expr(BinaryOp::GREATER_EQUAL, col_ref("age"), lit_int("30"));
    BoundStatement bound;
    FilterOperator filter(std::move(child), *pred, bound);

    auto open = filter.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    std::vector<std::string> names;
    while (true) {
        auto row = filter.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        names.push_back(row->value().values[1].as_string());
    }
    EXPECT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "alice");
    EXPECT_EQ(names[1], "charlie");

    filter.close();
}

TEST_F(SeqScanTest, FilterRejectsAll) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);
    insert_row(heap, 2, "bob", 25);

    auto child = std::make_unique<SeqScanOperator>(heap, storage_schema_, output_schema_);

    // Filter: age > 100 — no rows match.
    auto pred = binary_expr(BinaryOp::GREATER, col_ref("age"), lit_int("100"));
    BoundStatement bound;
    FilterOperator filter(std::move(child), *pred, bound);

    auto open = filter.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto row = filter.next();
    ASSERT_TRUE(row.has_value()) << row.error().message;
    EXPECT_FALSE(row->has_value());

    filter.close();
}

TEST_F(SeqScanTest, FilterOutputSchemaMatchesChild) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto child = std::make_unique<SeqScanOperator>(heap, storage_schema_, output_schema_);

    auto pred = binary_expr(BinaryOp::GREATER, col_ref("age"), lit_int("0"));
    BoundStatement bound;
    FilterOperator filter(std::move(child), *pred, bound);

    EXPECT_EQ(filter.output_schema().column_count(), 3u);
    EXPECT_EQ(filter.output_schema().column(0).name, "id");
}

TEST_F(SeqScanTest, FilterWithEqualityPredicate) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);
    insert_row(heap, 2, "bob", 25);
    insert_row(heap, 3, "charlie", 30);

    auto child = std::make_unique<SeqScanOperator>(heap, storage_schema_, output_schema_);

    // Filter: age = 30
    auto pred = binary_expr(BinaryOp::EQUAL, col_ref("age"), lit_int("30"));
    BoundStatement bound;
    FilterOperator filter(std::move(child), *pred, bound);

    auto open = filter.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    std::vector<std::string> names;
    while (true) {
        auto row = filter.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        names.push_back(row->value().values[1].as_string());
    }
    EXPECT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "alice");
    EXPECT_EQ(names[1], "charlie");

    filter.close();
}
