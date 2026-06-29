#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/bm25_scan.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/index/bm25_index.h"
#include "sixseven/index/rid.h"
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

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class Bm25ScanTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_test_bm25_scan.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;

        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);

        // Storage schema: id (INT32), body (STRING).
        storage_schema_ = Schema({
            {"id", TypeId::INT32},
            {"body", TypeId::STRING},
        });

        // Output schema: same two columns plus trailing _score (FLOAT64).
        OutputColumn c_id{"", "id", TypeId::INT32, false, 0};
        OutputColumn c_body{"", "body", TypeId::STRING, true, 0};
        OutputColumn c_score{"", "_score", TypeId::FLOAT64, true, 0};
        output_schema_ = OutputSchema({c_id, c_body, c_score});

        index_.create(Bm25Config{});
    }

    void TearDown() override {
        bpm_.reset();
        auto close = dm_.close_file(file_id_);
        (void)close;
        std::filesystem::remove(path_);
    }

    // Insert one row into the heap. Returns the RID (invalid on failure).
    RID insert_row(TableHeap& heap, int32_t id, const std::string& body) {
        std::vector<Value> vals = {Value(id), Value(body)};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        EXPECT_TRUE(bytes.has_value()) << bytes.error().message;
        if (!bytes.has_value()) {
            return RID{};
        }
        auto rid = heap.insert_tuple(*bytes);
        EXPECT_TRUE(rid.has_value()) << rid.error().message;
        return rid ? *rid : RID{};
    }

    // Drain all tuples from an already-opened operator into a vector.
    std::vector<Tuple> drain(Bm25ScanOperator& op) {
        std::vector<Tuple> rows;
        while (true) {
            auto next = op.next();
            EXPECT_TRUE(next.has_value()) << next.error().message;
            if (!next.has_value() || !next->has_value()) {
                break;
            }
            rows.push_back(std::move(**next));
        }
        return rows;
    }

    // Helper: build a literal integer expression.
    static ExprPtr lit_int(const std::string& v) {
        auto e = std::make_unique<LiteralExpr>();
        e->kind = LiteralKind::INTEGER;
        e->value = v;
        return e;
    }

    // Helper: build a column reference expression.
    static ExprPtr col_ref(const std::string& name) {
        auto e = std::make_unique<ColumnRefExpr>();
        e->column = name;
        return e;
    }

    // Helper: build a binary expression.
    static ExprPtr binary_expr(BinaryOp op, ExprPtr lhs, ExprPtr rhs) {
        auto e = std::make_unique<BinaryExpr>();
        e->op = op;
        e->lhs = std::move(lhs);
        e->rhs = std::move(rhs);
        return e;
    }

    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
    Schema storage_schema_;
    OutputSchema output_schema_;
    Bm25Index index_;
    std::filesystem::path path_;
};

// ---------------------------------------------------------------------------
// Test 1: Lifecycle and descending score order
//
// Three documents with clearly different "database" term frequencies so the
// BM25 ranking is unambiguous:
//   doc A: "database" x1  -- lowest score
//   doc B: "database database database database database" x5  -- highest score
//   doc C: "database database" x2  -- middle score
// Expected emission order: B > C > A, all scores > 0, non-increasing.
// ---------------------------------------------------------------------------
TEST_F(Bm25ScanTest, LifecycleAndDescendingScoreOrder) {
    TableHeap heap(*bpm_, dm_, file_id_);

    RID rid_a = insert_row(heap, 1, "database");
    RID rid_b = insert_row(heap, 2, "database database database database database");
    RID rid_c = insert_row(heap, 3, "database database");

    ASSERT_TRUE(index_.add_document(rid_a, "database").has_value());
    ASSERT_TRUE(
        index_.add_document(rid_b, "database database database database database").has_value());
    ASSERT_TRUE(index_.add_document(rid_c, "database database").has_value());

    BoundStatement bound;
    Bm25ScanConfig cfg{"database", 0};
    Bm25ScanOperator op(heap, storage_schema_, cfg, output_schema_, nullptr, bound, &index_);

    auto open = op.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto rows = drain(op);
    op.close();

    // All three matching documents must be returned.
    ASSERT_EQ(rows.size(), 3u);

    // Each tuple must have the table columns (id, body) plus _score = 3 values.
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 3u);
    }

    // Scores must be positive and non-increasing.
    double prev_score = std::numeric_limits<double>::max();
    for (const auto& row : rows) {
        double score = row.values[2].as_float64();
        EXPECT_GT(score, 0.0);
        EXPECT_LE(score, prev_score);
        prev_score = score;
    }

    // The top result must be doc B (id=2, five repetitions).
    EXPECT_EQ(rows[0].values[0].as_int32(), 2);
    // The last result must be doc A (id=1, single occurrence).
    EXPECT_EQ(rows[2].values[0].as_int32(), 1);
}

// ---------------------------------------------------------------------------
// Test 2: _score column in output schema and value accuracy
//
// Independently query the index for the same document set and compare the
// float score from the index hit against the double value appended by the
// operator to verify the cast and append are correct.
// ---------------------------------------------------------------------------
TEST_F(Bm25ScanTest, ScoreColumnSchemaAndValueAccuracy) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // output_schema() last column: name "_score", type FLOAT64.
    ASSERT_EQ(output_schema_.column_count(), 3u);
    EXPECT_EQ(output_schema_.column(2).name, "_score");
    EXPECT_EQ(output_schema_.column(2).type_id, TypeId::FLOAT64);

    RID rid_a = insert_row(heap, 10, "engine query search");
    RID rid_b = insert_row(heap, 20, "engine engine engine");

    ASSERT_TRUE(index_.add_document(rid_a, "engine query search").has_value());
    ASSERT_TRUE(index_.add_document(rid_b, "engine engine engine").has_value());

    // Capture the raw index scores before opening the operator.
    auto hits = index_.search("engine", 0);
    ASSERT_GE(hits.size(), 1u);

    BoundStatement bound;
    Bm25ScanConfig cfg{"engine", 0};
    Bm25ScanOperator op(heap, storage_schema_, cfg, output_schema_, nullptr, bound, &index_);

    // output_schema() on the operator must match what was passed in.
    EXPECT_EQ(op.output_schema().column_count(), 3u);
    EXPECT_EQ(op.output_schema().column(2).name, "_score");
    EXPECT_EQ(op.output_schema().column(2).type_id, TypeId::FLOAT64);

    auto open = op.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto rows = drain(op);
    op.close();

    ASSERT_EQ(rows.size(), hits.size());

    // Each operator-emitted _score must exactly equal (double)hit.score for
    // the corresponding hit (hits are already sorted descending by index).
    for (size_t i = 0; i < rows.size(); ++i) {
        double expected = static_cast<double>(hits[i].score);
        double actual = rows[i].values[2].as_float64();
        EXPECT_DOUBLE_EQ(actual, expected);
    }
}

// ---------------------------------------------------------------------------
// Test 3: Deleted-row skip
//
// Two documents are indexed. One row is then deleted from the heap before
// open() is called. The operator must skip the deleted row (get_tuple fails)
// and emit only the surviving document. open() must still return ok().
// ---------------------------------------------------------------------------
TEST_F(Bm25ScanTest, DeletedRowIsSkipped) {
    TableHeap heap(*bpm_, dm_, file_id_);

    RID rid_del = insert_row(heap, 100, "skip this document");
    RID rid_keep = insert_row(heap, 200, "keep this document");

    ASSERT_TRUE(index_.add_document(rid_del, "skip this document").has_value());
    ASSERT_TRUE(index_.add_document(rid_keep, "keep this document").has_value());

    // Delete the first row from the heap so get_tuple(rid_del) will fail.
    auto del = heap.delete_tuple(rid_del);
    ASSERT_TRUE(del.has_value()) << del.error().message;

    BoundStatement bound;
    // Query a term that appears in both documents so both are index hits.
    Bm25ScanConfig cfg{"document", 0};
    Bm25ScanOperator op(heap, storage_schema_, cfg, output_schema_, nullptr, bound, &index_);

    auto open = op.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto rows = drain(op);
    op.close();

    // Only the surviving row should be emitted.
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 200);
}

// ---------------------------------------------------------------------------
// Test 4: k limiting
//
// Five documents all contain "token". With k=1 the operator must return
// exactly one tuple -- the highest-scoring document.
// ---------------------------------------------------------------------------
TEST_F(Bm25ScanTest, KLimitingReturnsTopHit) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Build docs with clearly different term frequencies for unambiguous top.
    // doc 1: "token" x1
    // doc 2: "token token token token token" x5 -- highest score
    // doc 3: "token token" x2
    // doc 4: "token token token" x3
    // doc 5: "token token token token" x4
    std::vector<std::pair<int32_t, std::string>> docs = {
        {1, "token"},
        {2, "token token token token token"},
        {3, "token token"},
        {4, "token token token"},
        {5, "token token token token"},
    };

    for (auto& [id, body] : docs) {
        RID r = insert_row(heap, id, body);
        ASSERT_TRUE(index_.add_document(r, body).has_value());
    }

    BoundStatement bound;
    Bm25ScanConfig cfg{"token", 1}; // k=1
    Bm25ScanOperator op(heap, storage_schema_, cfg, output_schema_, nullptr, bound, &index_);

    auto open = op.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto rows = drain(op);
    op.close();

    ASSERT_EQ(rows.size(), 1u);
    // The single returned hit must be doc 2 (five repetitions = highest score).
    EXPECT_EQ(rows[0].values[0].as_int32(), 2);
    EXPECT_GT(rows[0].values[2].as_float64(), 0.0);
}

// ---------------------------------------------------------------------------
// Test 5: Residual WHERE post-filter on a table column
//
// Three documents all contain "record". A WHERE predicate (id == 30) is
// applied as a residual filter. Only the document with id=30 must survive
// even though all three are BM25 hits. The _score column must still be
// appended correctly to the surviving tuple, proving that build_where_filter_
// schema() correctly strips _score from the predicate evaluation schema.
// ---------------------------------------------------------------------------
TEST_F(Bm25ScanTest, ResidualWhereFilterOnTableColumn) {
    TableHeap heap(*bpm_, dm_, file_id_);

    RID rid10 = insert_row(heap, 10, "record alpha");
    RID rid20 = insert_row(heap, 20, "record beta");
    RID rid30 = insert_row(heap, 30, "record gamma");

    ASSERT_TRUE(index_.add_document(rid10, "record alpha").has_value());
    ASSERT_TRUE(index_.add_document(rid20, "record beta").has_value());
    ASSERT_TRUE(index_.add_document(rid30, "record gamma").has_value());

    // Predicate: id == 30
    auto pred = binary_expr(BinaryOp::EQUAL, col_ref("id"), lit_int("30"));

    BoundStatement bound;
    Bm25ScanConfig cfg{"record", 0};
    Bm25ScanOperator op(heap, storage_schema_, cfg, output_schema_, pred.get(), bound, &index_);

    auto open = op.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto rows = drain(op);
    op.close();

    // Only the row with id=30 passes the WHERE filter.
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 30);

    // The tuple still has 3 columns (id, body, _score).
    ASSERT_EQ(rows[0].values.size(), 3u);

    // _score must be positive (the doc did match the BM25 query).
    EXPECT_GT(rows[0].values[2].as_float64(), 0.0);
}
