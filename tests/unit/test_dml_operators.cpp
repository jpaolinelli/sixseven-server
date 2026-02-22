#include "giodb/executor/delete.h"
#include "giodb/executor/insert.h"
#include "giodb/executor/seq_scan.h"
#include "giodb/executor/update.h"

#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/tuple.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"
#include "giodb/storage/buffer_pool.h"
#include "giodb/storage/disk_manager.h"
#include "giodb/table/table_heap.h"
#include "giodb/table/tuple.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace giodb;

// -- Expression helpers -------------------------------------------------------

namespace {

ExprPtr make_lit_int(const std::string& v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::INTEGER;
    e->value = v;
    return e;
}

ExprPtr make_lit_string(const std::string& v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::STRING;
    e->value = v;
    return e;
}

ExprPtr make_col_ref(const std::string& name) {
    auto e = std::make_unique<ColumnRefExpr>();
    e->column = name;
    return e;
}

ExprPtr make_binary(BinaryOp op, ExprPtr lhs, ExprPtr rhs) {
    auto e = std::make_unique<BinaryExpr>();
    e->op = op;
    e->lhs = std::move(lhs);
    e->rhs = std::move(rhs);
    return e;
}

} // namespace

// -- Test fixture -------------------------------------------------------------

class DmlOperatorsTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "giodb_test_dml.db";
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

    /// Insert a row using TupleSerializer directly (test setup helper).
    void insert_raw(TableHeap& heap, int32_t id, const std::string& name, int32_t age) {
        std::vector<Value> vals = {Value(id), Value(name), Value(age)};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        ASSERT_TRUE(bytes.has_value()) << bytes.error().message;
        auto rid = heap.insert_tuple(*bytes);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    /// Count all rows in the heap via SeqScan.
    int count_rows(TableHeap& heap) {
        SeqScanOperator scan(heap, storage_schema_, output_schema_);
        auto open = scan.open();
        EXPECT_TRUE(open.has_value()) << open.error().message;
        int count = 0;
        while (true) {
            auto row = scan.next();
            EXPECT_TRUE(row.has_value());
            if (!row || !row->has_value()) {
                break;
            }
            ++count;
        }
        scan.close();
        return count;
    }

    /// Collect all tuples from a scan.
    std::vector<Tuple> scan_all(TableHeap& heap) {
        SeqScanOperator scan(heap, storage_schema_, output_schema_);
        auto open = scan.open();
        EXPECT_TRUE(open.has_value()) << open.error().message;
        std::vector<Tuple> results;
        while (true) {
            auto row = scan.next();
            EXPECT_TRUE(row.has_value());
            if (!row || !row->has_value()) {
                break;
            }
            results.push_back(std::move(row->value()));
        }
        scan.close();
        return results;
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
    Schema storage_schema_;
    OutputSchema output_schema_;
};

// =============================================================================
// INSERT tests
// =============================================================================

TEST_F(DmlOperatorsTest, InsertSingleRow) {
    TableHeap heap(*bpm_, dm_, file_id_);
    BoundStatement bound;

    // INSERT INTO t VALUES (1, 'alice', 30)
    auto id_expr = make_lit_int("1");
    auto name_expr = make_lit_string("alice");
    auto age_expr = make_lit_int("30");
    std::vector<std::vector<const Expr*>> rows = {
        {id_expr.get(), name_expr.get(), age_expr.get()},
    };

    InsertOperator insert(heap, storage_schema_, std::move(rows), bound);

    auto open = insert.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto result = insert.next();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().values[0].as_int64(), 1); // 1 row inserted

    // Verify via scan.
    auto tuples = scan_all(heap);
    ASSERT_EQ(tuples.size(), 1u);
    EXPECT_EQ(tuples[0].values[0].as_int32(), 1);
    EXPECT_EQ(tuples[0].values[1].as_string(), "alice");
    EXPECT_EQ(tuples[0].values[2].as_int32(), 30);

    insert.close();
}

TEST_F(DmlOperatorsTest, InsertMultipleRows) {
    TableHeap heap(*bpm_, dm_, file_id_);
    BoundStatement bound;

    auto id1 = make_lit_int("1");
    auto name1 = make_lit_string("alice");
    auto age1 = make_lit_int("30");
    auto id2 = make_lit_int("2");
    auto name2 = make_lit_string("bob");
    auto age2 = make_lit_int("25");

    std::vector<std::vector<const Expr*>> rows = {
        {id1.get(), name1.get(), age1.get()},
        {id2.get(), name2.get(), age2.get()},
    };

    InsertOperator insert(heap, storage_schema_, std::move(rows), bound);
    auto open = insert.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto result = insert.next();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().values[0].as_int64(), 2); // 2 rows inserted

    EXPECT_EQ(count_rows(heap), 2);

    insert.close();
}

TEST_F(DmlOperatorsTest, InsertReturnsSingleCountRow) {
    TableHeap heap(*bpm_, dm_, file_id_);
    BoundStatement bound;

    auto id1 = make_lit_int("1");
    auto name1 = make_lit_string("x");
    auto age1 = make_lit_int("10");
    std::vector<std::vector<const Expr*>> rows = {{id1.get(), name1.get(), age1.get()}};

    InsertOperator insert(heap, storage_schema_, std::move(rows), bound);
    auto open = insert.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto r1 = insert.next();
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    ASSERT_TRUE(r1->has_value());

    // Second call should return nullopt (exhausted).
    auto r2 = insert.next();
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    EXPECT_FALSE(r2->has_value());

    insert.close();
}

TEST_F(DmlOperatorsTest, InsertFromSelect) {
    // Set up source table with data.
    auto src_path = std::filesystem::temp_directory_path() / "giodb_test_dml_src.db";
    std::filesystem::remove(src_path);
    auto src_fid = dm_.create_file(src_path, false, true);
    ASSERT_TRUE(src_fid.has_value()) << src_fid.error().message;
    auto src_bpm = std::make_unique<BufferPoolManager>(dm_, *src_fid, 64);
    TableHeap src_heap(*src_bpm, dm_, *src_fid);
    insert_raw(src_heap, 10, "source_row", 50);

    // Target heap (empty).
    TableHeap target_heap(*bpm_, dm_, file_id_);

    // INSERT INTO target SELECT * FROM source
    auto scan = std::make_unique<SeqScanOperator>(src_heap, storage_schema_, output_schema_);
    InsertOperator insert(target_heap, storage_schema_, std::move(scan));

    auto open = insert.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto result = insert.next();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().values[0].as_int64(), 1);

    auto tuples = scan_all(target_heap);
    ASSERT_EQ(tuples.size(), 1u);
    EXPECT_EQ(tuples[0].values[1].as_string(), "source_row");

    insert.close();
    src_bpm.reset();
    (void)dm_.close_file(*src_fid);
    std::filesystem::remove(src_path);
}

// =============================================================================
// UPDATE tests
// =============================================================================

TEST_F(DmlOperatorsTest, UpdateAllRows) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_raw(heap, 1, "alice", 30);
    insert_raw(heap, 2, "bob", 25);

    BoundStatement bound;

    // UPDATE t SET age = 99
    auto age_lit = make_lit_int("99");
    std::vector<UpdateAssignment> assigns = {{2, age_lit.get()}}; // column 2 = age

    auto scan = std::make_unique<SeqScanOperator>(heap, storage_schema_, output_schema_);
    UpdateOperator update(heap, storage_schema_, std::move(scan), std::move(assigns), bound);

    auto open = update.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto result = update.next();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().values[0].as_int64(), 2); // 2 rows updated

    // Verify ages are all 99.
    auto tuples = scan_all(heap);
    ASSERT_EQ(tuples.size(), 2u);
    for (auto& t : tuples) {
        EXPECT_EQ(t.values[2].as_int32(), 99);
    }

    update.close();
}

TEST_F(DmlOperatorsTest, UpdateWithPredicate) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_raw(heap, 1, "alice", 30);
    insert_raw(heap, 2, "bob", 25);
    insert_raw(heap, 3, "charlie", 35);

    BoundStatement bound;

    // UPDATE t SET name = 'updated' WHERE age > 28
    auto pred = make_binary(BinaryOp::GREATER, make_col_ref("age"), make_lit_int("28"));
    auto name_lit = make_lit_string("updated");
    std::vector<UpdateAssignment> assigns = {{1, name_lit.get()}}; // column 1 = name

    auto scan = std::make_unique<SeqScanOperator>(heap, storage_schema_, output_schema_,
                                                   pred.get(), &bound);
    UpdateOperator update(heap, storage_schema_, std::move(scan), std::move(assigns), bound);

    auto open = update.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto result = update.next();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().values[0].as_int64(), 2); // alice(30) + charlie(35)

    // Verify: bob should be unchanged.
    auto tuples = scan_all(heap);
    ASSERT_EQ(tuples.size(), 3u);
    int updated_count = 0;
    for (auto& t : tuples) {
        if (t.values[1].as_string() == "updated") {
            ++updated_count;
        }
    }
    EXPECT_EQ(updated_count, 2);

    update.close();
}

TEST_F(DmlOperatorsTest, UpdateExpressionValue) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_raw(heap, 1, "alice", 30);

    BoundStatement bound;

    // UPDATE t SET age = age + 10
    auto expr = make_binary(BinaryOp::ADD, make_col_ref("age"), make_lit_int("10"));
    std::vector<UpdateAssignment> assigns = {{2, expr.get()}};

    auto scan = std::make_unique<SeqScanOperator>(heap, storage_schema_, output_schema_);
    UpdateOperator update(heap, storage_schema_, std::move(scan), std::move(assigns), bound);

    auto open = update.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto result = update.next();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().values[0].as_int64(), 1);

    auto tuples = scan_all(heap);
    ASSERT_EQ(tuples.size(), 1u);
    // age was 30 (INT32), age + 10 produces INT64 which is coerced back to
    // INT32 by the UpdateOperator before serialization.
    EXPECT_EQ(tuples[0].values[2].as_int32(), 40);

    update.close();
}

// =============================================================================
// DELETE tests
// =============================================================================

TEST_F(DmlOperatorsTest, DeleteAllRows) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_raw(heap, 1, "alice", 30);
    insert_raw(heap, 2, "bob", 25);

    auto scan = std::make_unique<SeqScanOperator>(heap, storage_schema_, output_schema_);
    DeleteOperator del(heap, std::move(scan));

    auto open = del.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto result = del.next();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().values[0].as_int64(), 2); // 2 rows deleted

    EXPECT_EQ(count_rows(heap), 0);

    del.close();
}

TEST_F(DmlOperatorsTest, DeleteWithPredicate) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_raw(heap, 1, "alice", 30);
    insert_raw(heap, 2, "bob", 25);
    insert_raw(heap, 3, "charlie", 35);

    BoundStatement bound;

    // DELETE FROM t WHERE age < 30
    auto pred = make_binary(BinaryOp::LESS, make_col_ref("age"), make_lit_int("30"));

    auto scan = std::make_unique<SeqScanOperator>(heap, storage_schema_, output_schema_,
                                                   pred.get(), &bound);
    DeleteOperator del(heap, std::move(scan));

    auto open = del.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto result = del.next();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().values[0].as_int64(), 1); // Only bob (age 25)

    auto remaining = scan_all(heap);
    EXPECT_EQ(remaining.size(), 2u);

    del.close();
}

TEST_F(DmlOperatorsTest, DeleteFromEmptyTable) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto scan = std::make_unique<SeqScanOperator>(heap, storage_schema_, output_schema_);
    DeleteOperator del(heap, std::move(scan));

    auto open = del.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto result = del.next();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().values[0].as_int64(), 0); // 0 rows deleted

    del.close();
}

TEST_F(DmlOperatorsTest, DeleteReturnsSingleCountRow) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_raw(heap, 1, "x", 10);

    auto scan = std::make_unique<SeqScanOperator>(heap, storage_schema_, output_schema_);
    DeleteOperator del(heap, std::move(scan));

    auto open = del.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto r1 = del.next();
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    ASSERT_TRUE(r1->has_value());

    auto r2 = del.next();
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    EXPECT_FALSE(r2->has_value()); // Exhausted after count row.

    del.close();
}

// =============================================================================
// INSERT + UPDATE + DELETE pipeline
// =============================================================================

TEST_F(DmlOperatorsTest, InsertThenUpdateThenDelete) {
    TableHeap heap(*bpm_, dm_, file_id_);
    BoundStatement bound;

    // Step 1: INSERT 3 rows.
    auto i1 = make_lit_int("1");
    auto n1 = make_lit_string("alice");
    auto a1 = make_lit_int("30");
    auto i2 = make_lit_int("2");
    auto n2 = make_lit_string("bob");
    auto a2 = make_lit_int("25");
    auto i3 = make_lit_int("3");
    auto n3 = make_lit_string("charlie");
    auto a3 = make_lit_int("35");

    std::vector<std::vector<const Expr*>> rows = {
        {i1.get(), n1.get(), a1.get()},
        {i2.get(), n2.get(), a2.get()},
        {i3.get(), n3.get(), a3.get()},
    };

    InsertOperator insert(heap, storage_schema_, std::move(rows), bound);
    auto r_open = insert.open();
    ASSERT_TRUE(r_open.has_value()) << r_open.error().message;
    auto r_ins = insert.next();
    ASSERT_TRUE(r_ins.has_value()) << r_ins.error().message;
    EXPECT_EQ(r_ins->value().values[0].as_int64(), 3);
    insert.close();

    EXPECT_EQ(count_rows(heap), 3);

    // Step 2: UPDATE age = 50 WHERE name = 'bob'
    auto pred_name = make_binary(BinaryOp::EQUAL, make_col_ref("name"), make_lit_string("bob"));
    auto new_age = make_lit_int("50");
    std::vector<UpdateAssignment> assigns = {{2, new_age.get()}};

    auto scan_up = std::make_unique<SeqScanOperator>(heap, storage_schema_, output_schema_,
                                                      pred_name.get(), &bound);
    UpdateOperator update(heap, storage_schema_, std::move(scan_up), std::move(assigns), bound);
    auto u_open = update.open();
    ASSERT_TRUE(u_open.has_value()) << u_open.error().message;
    auto r_up = update.next();
    ASSERT_TRUE(r_up.has_value()) << r_up.error().message;
    EXPECT_EQ(r_up->value().values[0].as_int64(), 1);
    update.close();

    // Step 3: DELETE WHERE age > 40
    auto pred_del = make_binary(BinaryOp::GREATER, make_col_ref("age"), make_lit_int("40"));
    auto scan_del = std::make_unique<SeqScanOperator>(heap, storage_schema_, output_schema_,
                                                       pred_del.get(), &bound);
    DeleteOperator del(heap, std::move(scan_del));
    auto d_open = del.open();
    ASSERT_TRUE(d_open.has_value()) << d_open.error().message;
    auto r_del = del.next();
    ASSERT_TRUE(r_del.has_value()) << r_del.error().message;
    EXPECT_EQ(r_del->value().values[0].as_int64(), 1); // bob (now age 50)
    del.close();

    // Verify: 2 rows remain (alice:30, charlie:35).
    EXPECT_EQ(count_rows(heap), 2);
}
