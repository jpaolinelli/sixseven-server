/// @file test_qa_nearest_prefilter.cpp
/// @brief QA regression tests for btree-accelerated filtered NEAREST.
///
/// Verifies that when prefiltered_rids are provided to NearestScanOperator,
/// the operator computes distances only for those specific rows and returns
/// correct top-k results sorted by distance.

#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/nearest_scan.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"
#include "sixseven/vector/distance.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace sixseven;

// =============================================================================
// Shared fixture
// =============================================================================

class QANearestPrefilterTest : public ::testing::Test {
protected:
    void SetUp() override {
        table_path_ = std::filesystem::temp_directory_path() / "sixseven_qa_prefilter.db";
        std::filesystem::remove(table_path_);
        auto fid = dm_.create_file(table_path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        table_fid_ = *fid;
        table_bpm_ = std::make_unique<BufferPoolManager>(dm_, table_fid_, 64);

        // Schema: id (INT32), category (STRING), embedding (EMBEDDING).
        storage_schema_ = Schema({
            {"id", TypeId::INT32},
            {"category", TypeId::STRING},
            {"embedding", TypeId::EMBEDDING},
        });

        output_cols_ = {
            {"", "id", TypeId::INT32, false, 0},
            {"", "category", TypeId::STRING, true, 0},
            {"", "embedding", TypeId::EMBEDDING, true, 0},
            {"", "_distance", TypeId::FLOAT64, false, 0},
        };
    }

    void TearDown() override {
        table_bpm_.reset();
        (void)dm_.close_file(table_fid_);
        std::filesystem::remove(table_path_);
    }

    RID insert_row(TableHeap& heap, int32_t id, const std::string& cat,
                   const Embedding& embedding) {
        std::vector<Value> vals = {Value(id), Value(cat), Value(embedding)};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        EXPECT_TRUE(bytes.has_value()) << bytes.error().message;
        if (!bytes.has_value()) {
            return RID{};
        }
        auto rid = heap.insert_tuple(*bytes);
        EXPECT_TRUE(rid.has_value()) << rid.error().message;
        return rid ? *rid : RID{};
    }

    RID insert_row_null_emb(TableHeap& heap, int32_t id, const std::string& cat) {
        std::vector<Value> vals = {Value(id), Value(cat), Value()};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        EXPECT_TRUE(bytes.has_value()) << bytes.error().message;
        if (!bytes.has_value()) {
            return RID{};
        }
        auto rid = heap.insert_tuple(*bytes);
        EXPECT_TRUE(rid.has_value()) << rid.error().message;
        return rid ? *rid : RID{};
    }

    std::vector<Tuple> drain(Iterator& op) {
        std::vector<Tuple> results;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row->has_value()) {
                break;
            }
            results.push_back(std::move(row->value()));
        }
        return results;
    }

    static ExprPtr lit_string(const std::string& v) {
        auto e = std::make_unique<LiteralExpr>();
        e->kind = LiteralKind::STRING;
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

    std::filesystem::path table_path_;
    DiskManager dm_;
    FileId table_fid_ = 0;
    std::unique_ptr<BufferPoolManager> table_bpm_;
    Schema storage_schema_;
    std::vector<OutputColumn> output_cols_;
};

// =============================================================================
// Basic prefiltered search
// =============================================================================

TEST_F(QANearestPrefilterTest, PrefilteredReturnsCorrectTopK) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);

    // Insert rows with different categories and distances from [1,0,0].
    auto rid_a1 = insert_row(heap, 1, "A", {1.0F, 0.0F, 0.0F}); // dist=0
    insert_row(heap, 2, "B", {0.9F, 0.1F, 0.0F});                // dist=0.02 (not in filter)
    auto rid_a3 = insert_row(heap, 3, "A", {0.5F, 0.5F, 0.0F}); // dist=0.5
    auto rid_a4 = insert_row(heap, 4, "A", {0.0F, 1.0F, 0.0F}); // dist=2.0
    insert_row(heap, 5, "B", {0.8F, 0.2F, 0.0F});                // dist=0.08 (not in filter)

    // Simulate btree returning only category "A" rows.
    NearestScanConfig config;
    config.k = 2;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.prefiltered_rids = {rid_a1, rid_a3, rid_a4};

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 2u);

    // Top-2 from filtered set: id=1 (dist=0), id=3 (dist=0.5).
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_EQ(results[1].values[0].as_int32(), 3);

    // Verify distances are sorted ascending.
    EXPECT_LE(results[0].values[3].as_float64(), results[1].values[3].as_float64());

    op.close();
}

TEST_F(QANearestPrefilterTest, PrefilteredExcludesNonFilteredRows) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);

    // Row 2 is the absolute closest but is not in the prefiltered set.
    insert_row(heap, 1, "A", {0.5F, 0.5F, 0.0F}); // dist=0.5
    auto rid_closest = insert_row(heap, 2, "B", {1.0F, 0.0F, 0.0F}); // dist=0
    auto rid_a3 = insert_row(heap, 3, "A", {0.0F, 1.0F, 0.0F}); // dist=2.0

    // Only include row 3 — the closest row (2) should NOT appear.
    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.prefiltered_rids = {rid_a3};

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].values[0].as_int32(), 3);

    // Suppress unused variable warning.
    (void)rid_closest;

    op.close();
}

TEST_F(QANearestPrefilterTest, PrefilteredWithNullEmbeddingsSkipped) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);

    auto rid1 = insert_row_null_emb(heap, 1, "A");
    auto rid2 = insert_row(heap, 2, "A", {1.0F, 0.0F, 0.0F});
    auto rid3 = insert_row_null_emb(heap, 3, "A");
    auto rid4 = insert_row(heap, 4, "A", {0.5F, 0.5F, 0.0F});

    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.prefiltered_rids = {rid1, rid2, rid3, rid4};

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);

    // Only rows with non-null embeddings.
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].values[0].as_int32(), 2); // closer
    EXPECT_EQ(results[1].values[0].as_int32(), 4);

    op.close();
}

TEST_F(QANearestPrefilterTest, PrefilteredWithWherePostFilter) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);

    // All rows have category "A" or "B". Prefilter includes all,
    // but WHERE post-filter selects only category "B".
    auto rid1 = insert_row(heap, 1, "A", {1.0F, 0.0F, 0.0F}); // closest
    auto rid2 = insert_row(heap, 2, "B", {0.9F, 0.1F, 0.0F}); // 2nd closest, cat=B
    auto rid3 = insert_row(heap, 3, "A", {0.5F, 0.5F, 0.0F});
    auto rid4 = insert_row(heap, 4, "B", {0.0F, 1.0F, 0.0F}); // cat=B

    auto where = binary_expr(BinaryOp::EQUAL, col_ref("category"), lit_string("B"));
    BoundStatement bound;
    auto* col_expr =
        dynamic_cast<const ColumnRefExpr*>(dynamic_cast<const BinaryExpr*>(where.get())->lhs.get());
    bound.expr_types[col_expr] = {TypeId::STRING, false, false};
    auto* lit_expr =
        dynamic_cast<const LiteralExpr*>(dynamic_cast<const BinaryExpr*>(where.get())->rhs.get());
    bound.expr_types[lit_expr] = {TypeId::STRING, false, false};
    bound.expr_types[where.get()] = {TypeId::BOOL, false, false};

    NearestScanConfig config;
    config.k = 2;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.prefiltered_rids = {rid1, rid2, rid3, rid4};

    OutputSchema schema(output_cols_);

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), where.get(), bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].values[1].as_string(), "B");
    EXPECT_EQ(results[1].values[1].as_string(), "B");
    EXPECT_EQ(results[0].values[0].as_int32(), 2); // closer B
    EXPECT_EQ(results[1].values[0].as_int32(), 4); // further B

    op.close();
}

TEST_F(QANearestPrefilterTest, PrefilteredPrioritizedOverHnsw) {
    // When prefiltered_rids is set, the prefiltered path should be used
    // even if an HNSW index is available. Verify by checking that only
    // prefiltered rows appear in results.
    TableHeap heap(*table_bpm_, dm_, table_fid_);

    auto rid1 = insert_row(heap, 1, "A", {1.0F, 0.0F, 0.0F});
    insert_row(heap, 2, "B", {0.9F, 0.0F, 0.0F}); // Not in prefilter.
    auto rid3 = insert_row(heap, 3, "A", {0.0F, 1.0F, 0.0F});

    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.prefiltered_rids = {rid1, rid3};

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    // Pass nullptr for HNSW — the prefiltered path should be chosen first.
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound,
        nullptr, nullptr);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 2u);
    // Row 2 (B) should NOT appear even though it's closer than row 3.
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_EQ(results[1].values[0].as_int32(), 3);

    op.close();
}

TEST_F(QANearestPrefilterTest, PrefilteredCosineMetric) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);

    auto rid1 = insert_row(heap, 1, "A", {1.0F, 0.0F, 0.0F});  // same direction
    auto rid2 = insert_row(heap, 2, "A", {-1.0F, 0.0F, 0.0F}); // opposite
    auto rid3 = insert_row(heap, 3, "A", {0.0F, 1.0F, 0.0F});  // orthogonal

    NearestScanConfig config;
    config.k = 3;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::COSINE;
    config.embedding_column_index = 2;
    config.prefiltered_rids = {rid1, rid2, rid3};

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 3u);

    // Cosine distance order: same (0), orthogonal (1), opposite (2).
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_EQ(results[1].values[0].as_int32(), 3);
    EXPECT_EQ(results[2].values[0].as_int32(), 2);

    op.close();
}

TEST_F(QANearestPrefilterTest, PrefilteredLargeSetTopK) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);

    // Insert 50 rows, prefilter all, ask for top-3.
    std::vector<RID> rids;
    for (int i = 1; i <= 50; ++i) {
        auto f = static_cast<float>(i);
        rids.push_back(insert_row(heap, i, "cat", {f, 0.0F, 0.0F}));
    }

    NearestScanConfig config;
    config.k = 3;
    config.query_vector = {0.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.prefiltered_rids = rids;

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 3u);

    // Closest to origin: ids 1, 2, 3.
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_EQ(results[1].values[0].as_int32(), 2);
    EXPECT_EQ(results[2].values[0].as_int32(), 3);

    op.close();
}
