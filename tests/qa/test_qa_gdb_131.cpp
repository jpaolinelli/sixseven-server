/// @file test_qa_gdb_131.cpp
/// @brief QA adversarial tests for GDB-131: NEAREST query execution.
///
/// Targets edge cases: k=0, null embedding rows, graph scope with empty set,
/// graph scope with all excluded, WHERE that filters everything, identical
/// vectors, DOT_PRODUCT metric, HNSW vs brute-force consistency, dimension
/// mismatch, re-open idempotency.

#include "sixseven/catalog/catalog.h"
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
#include "sixseven/vector/hnsw_index.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace sixseven;

// =============================================================================
// Shared fixture
// =============================================================================

class QA131NearestScanTest : public ::testing::Test {
protected:
    void SetUp() override {
        table_path_ = std::filesystem::temp_directory_path() / "sixseven_qa131_table.db";
        std::filesystem::remove(table_path_);
        auto fid = dm_.create_file(table_path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        table_fid_ = *fid;
        table_bpm_ = std::make_unique<BufferPoolManager>(dm_, table_fid_, 64);

        hnsw_path_ = std::filesystem::temp_directory_path() / "sixseven_qa131_hnsw.db";
        std::filesystem::remove(hnsw_path_);
        auto hfid = dm_.create_file(hnsw_path_, false, true);
        ASSERT_TRUE(hfid.has_value()) << hfid.error().message;
        hnsw_fid_ = *hfid;
        hnsw_bpm_ = std::make_unique<BufferPoolManager>(dm_, hnsw_fid_, 256);

        storage_schema_ = Schema({
            {"id", TypeId::INT32},
            {"name", TypeId::STRING},
            {"embedding", TypeId::EMBEDDING},
        });

        output_cols_ = {
            {"", "id", TypeId::INT32, false, 0},
            {"", "name", TypeId::STRING, true, 0},
            {"", "embedding", TypeId::EMBEDDING, true, 0},
            {"", "_distance", TypeId::FLOAT64, false, 0},
        };
    }

    void TearDown() override {
        hnsw_bpm_.reset();
        table_bpm_.reset();
        (void)dm_.close_file(hnsw_fid_);
        (void)dm_.close_file(table_fid_);
        std::filesystem::remove(hnsw_path_);
        std::filesystem::remove(table_path_);
    }

    RID
    insert_row(TableHeap& heap, int32_t id, const std::string& name, const Embedding& embedding) {
        std::vector<Value> vals = {Value(id), Value(name), Value(embedding)};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        EXPECT_TRUE(bytes.has_value()) << bytes.error().message;
        if (!bytes.has_value()) {
            return RID{};
        }
        auto rid = heap.insert_tuple(*bytes);
        EXPECT_TRUE(rid.has_value()) << rid.error().message;
        return rid ? *rid : RID{};
    }

    RID insert_row_null_embedding(TableHeap& heap, int32_t id, const std::string& name) {
        std::vector<Value> vals = {Value(id), Value(name), Value()};
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
    std::filesystem::path hnsw_path_;
    DiskManager dm_;
    FileId table_fid_ = 0;
    FileId hnsw_fid_ = 0;
    std::unique_ptr<BufferPoolManager> table_bpm_;
    std::unique_ptr<BufferPoolManager> hnsw_bpm_;
    Schema storage_schema_;
    std::vector<OutputColumn> output_cols_;
};

// ---------------------------------------------------------------------------
// K=0 edge case
// ---------------------------------------------------------------------------

TEST_F(QA131NearestScanTest, KZeroReturnsEmpty) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    insert_row(heap, 1, "a", {1.0F, 0.0F, 0.0F});

    NearestScanConfig config;
    config.k = 0;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    EXPECT_TRUE(results.empty());
    op.close();
}

// ---------------------------------------------------------------------------
// Null embedding rows
// ---------------------------------------------------------------------------

TEST_F(QA131NearestScanTest, NullEmbeddingsSkipped) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    insert_row(heap, 1, "valid1", {1.0F, 0.0F, 0.0F});
    insert_row_null_embedding(heap, 2, "null_emb");
    insert_row(heap, 3, "valid2", {0.0F, 1.0F, 0.0F});
    insert_row_null_embedding(heap, 4, "null_emb2");

    NearestScanConfig config;
    config.k = 10;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    // Only 2 valid embeddings.
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_EQ(results[1].values[0].as_int32(), 3);
    op.close();
}

TEST_F(QA131NearestScanTest, AllNullEmbeddingsReturnsEmpty) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    insert_row_null_embedding(heap, 1, "null1");
    insert_row_null_embedding(heap, 2, "null2");

    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    EXPECT_TRUE(results.empty());
    op.close();
}

// ---------------------------------------------------------------------------
// Identical vectors
// ---------------------------------------------------------------------------

TEST_F(QA131NearestScanTest, IdenticalVectorsAllDistanceZero) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    for (int i = 0; i < 5; ++i) {
        insert_row(heap, i + 1, "same", {1.0F, 0.0F, 0.0F});
    }

    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 5u);
    for (const auto& r : results) {
        EXPECT_NEAR(r.values[3].as_float64(), 0.0, 1e-5);
    }
    op.close();
}

// ---------------------------------------------------------------------------
// Graph scope edge cases
// ---------------------------------------------------------------------------

TEST_F(QA131NearestScanTest, GraphScopeEmptyAllowedSet) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    insert_row(heap, 1, "a", {1.0F, 0.0F, 0.0F});
    insert_row(heap, 2, "b", {0.0F, 1.0F, 0.0F});

    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    // Empty set means "no scope restriction" — all rows eligible.
    config.allowed_rids = {};

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    EXPECT_EQ(results.size(), 2u);
    op.close();
}

TEST_F(QA131NearestScanTest, GraphScopeAllExcluded) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    insert_row(heap, 1, "a", {1.0F, 0.0F, 0.0F}); // ordinal 0
    insert_row(heap, 2, "b", {0.0F, 1.0F, 0.0F}); // ordinal 1

    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    // Only allow RIDs that don't exist in the heap.
    config.allowed_rids = {RID{9999, 99}, RID{9999, 100}};

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    EXPECT_TRUE(results.empty());
    op.close();
}

TEST_F(QA131NearestScanTest, GraphScopeSingleNodeAllowed) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    insert_row(heap, 1, "a", {1.0F, 0.0F, 0.0F});
    auto rid_b = insert_row(heap, 2, "b", {0.0F, 1.0F, 0.0F});
    insert_row(heap, 3, "c", {0.5F, 0.5F, 0.0F});

    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.allowed_rids = {rid_b}; // Only row id=2.

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].values[0].as_int32(), 2);
    op.close();
}

// ---------------------------------------------------------------------------
// DOT_PRODUCT metric
// ---------------------------------------------------------------------------

TEST_F(QA131NearestScanTest, DotProductMetric) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    insert_row(heap, 1, "aligned", {1.0F, 0.0F, 0.0F});
    insert_row(heap, 2, "partial", {0.5F, 0.5F, 0.0F});
    insert_row(heap, 3, "orthogonal", {0.0F, 1.0F, 0.0F});

    NearestScanConfig config;
    config.k = 3;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::DOT_PRODUCT;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 3u);
    // GDB-717 / GDB-731: dot product is a similarity (higher = more similar),
    // so NEAREST must rank the highest dot product first.
    // aligned: dot=1.0, partial: dot=0.5, orthogonal: dot=0.0
    // So order: aligned, partial, orthogonal.
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_EQ(results[1].values[0].as_int32(), 2);
    EXPECT_EQ(results[2].values[0].as_int32(), 3);
    // The emitted _distance column reports the raw dot product.
    EXPECT_NEAR(results[0].values[3].as_float64(), 1.0, 1e-5);
    EXPECT_NEAR(results[1].values[3].as_float64(), 0.5, 1e-5);
    EXPECT_NEAR(results[2].values[3].as_float64(), 0.0, 1e-5);
    op.close();
}

// ---------------------------------------------------------------------------
// WHERE filter that rejects everything
// ---------------------------------------------------------------------------

TEST_F(QA131NearestScanTest, WhereFilterRejectsAll) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    insert_row(heap, 1, "alpha", {1.0F, 0.0F, 0.0F});
    insert_row(heap, 2, "beta", {0.0F, 1.0F, 0.0F});

    // WHERE name = 'nonexistent'
    auto where = binary_expr(BinaryOp::EQUAL, col_ref("name"), lit_string("nonexistent"));
    BoundStatement bound;
    auto* col_expr =
        dynamic_cast<const ColumnRefExpr*>(dynamic_cast<const BinaryExpr*>(where.get())->lhs.get());
    bound.expr_types[col_expr] = {TypeId::STRING, false, false};
    auto* lit_expr =
        dynamic_cast<const LiteralExpr*>(dynamic_cast<const BinaryExpr*>(where.get())->rhs.get());
    bound.expr_types[lit_expr] = {TypeId::STRING, false, false};
    bound.expr_types[where.get()] = {TypeId::BOOL, false, false};

    NearestScanConfig config;
    config.k = 10;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), where.get(), bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    EXPECT_TRUE(results.empty());
    op.close();
}

// ---------------------------------------------------------------------------
// HNSW vs brute-force consistency
// ---------------------------------------------------------------------------

TEST_F(QA131NearestScanTest, HnswMatchesBruteForceTopResult) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    HnswIndex hnsw(*hnsw_bpm_, nullptr);

    HnswIndexConfig hnsw_config;
    hnsw_config.dimension = 3;
    hnsw_config.m = 8;
    hnsw_config.ef_construction = 50;
    hnsw_config.ef_search = 50;
    ASSERT_TRUE(hnsw.create(hnsw_config).has_value());

    Embedding embs[] = {
        {1.0F, 0.0F, 0.0F},
        {0.0F, 1.0F, 0.0F},
        {0.0F, 0.0F, 1.0F},
        {0.5F, 0.5F, 0.0F},
        {0.3F, 0.3F, 0.3F},
        {0.9F, 0.1F, 0.0F},
    };

    for (int i = 0; i < 6; ++i) {
        insert_row(heap, i + 1, "r" + std::to_string(i), embs[i]);
        ASSERT_TRUE(hnsw.insert(embs[i]).has_value());
    }

    std::vector<float> query = {1.0F, 0.0F, 0.0F};

    // Brute force.
    {
        NearestScanConfig config;
        config.k = 1;
        config.query_vector = query;
        config.metric = DistanceMetric::L2;
        config.embedding_column_index = 2;

        OutputSchema schema(output_cols_);
        BoundStatement bound;
        NearestScanOperator op(
            heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

        ASSERT_TRUE(op.open().has_value());
        auto bf_results = drain(op);
        ASSERT_EQ(bf_results.size(), 1u);
        EXPECT_EQ(bf_results[0].values[0].as_int32(), 1);
        op.close();
    }

    // HNSW.
    {
        NearestScanConfig config;
        config.k = 1;
        config.query_vector = query;
        config.metric = DistanceMetric::L2;
        config.embedding_column_index = 2;

        OutputSchema schema(output_cols_);
        BoundStatement bound;
        NearestScanOperator op(
            heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound, &hnsw);

        ASSERT_TRUE(op.open().has_value());
        auto hnsw_results = drain(op);
        ASSERT_EQ(hnsw_results.size(), 1u);
        EXPECT_EQ(hnsw_results[0].values[0].as_int32(), 1);
        op.close();
    }
}

// ---------------------------------------------------------------------------
// HNSW with graph scope + WHERE combined
// ---------------------------------------------------------------------------

TEST_F(QA131NearestScanTest, HnswGraphScopeFilteredSearch) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    HnswIndex hnsw(*hnsw_bpm_, nullptr);

    HnswIndexConfig hnsw_config;
    hnsw_config.dimension = 2;
    hnsw_config.m = 8;
    hnsw_config.ef_construction = 50;
    hnsw_config.ef_search = 50;
    ASSERT_TRUE(hnsw.create(hnsw_config).has_value());

    Embedding embs[] = {
        {1.0F, 0.0F},
        {0.9F, 0.1F},
        {0.8F, 0.2F},
        {0.5F, 0.5F},
        {0.0F, 1.0F},
    };

    std::vector<RID> rids;
    for (int i = 0; i < 5; ++i) {
        rids.push_back(insert_row(heap, i + 1, (i % 2 == 0) ? "even" : "odd", embs[i]));
        ASSERT_TRUE(hnsw.insert(embs[i]).has_value());
    }

    // Graph scope allows rows 1-4 (by RID).
    // WHERE name = 'even' → ids 1 and 3 pass.
    auto where = binary_expr(BinaryOp::EQUAL, col_ref("name"), lit_string("even"));
    BoundStatement bound;
    auto* col_expr =
        dynamic_cast<const ColumnRefExpr*>(dynamic_cast<const BinaryExpr*>(where.get())->lhs.get());
    bound.expr_types[col_expr] = {TypeId::STRING, false, false};
    auto* lit_expr =
        dynamic_cast<const LiteralExpr*>(dynamic_cast<const BinaryExpr*>(where.get())->rhs.get());
    bound.expr_types[lit_expr] = {TypeId::STRING, false, false};
    bound.expr_types[where.get()] = {TypeId::BOOL, false, false};

    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.allowed_rids = {rids[0], rids[1], rids[2], rids[3]};

    OutputSchema schema(output_cols_);
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), where.get(), bound, &hnsw);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    // Only "even" rows within graph scope: ordinals 0 (id=1), 2 (id=3).
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_EQ(results[1].values[0].as_int32(), 3);
    for (const auto& r : results) {
        EXPECT_EQ(r.values[1].as_string(), "even");
    }
    op.close();
}

// ---------------------------------------------------------------------------
// Re-open produces same results
// ---------------------------------------------------------------------------

TEST_F(QA131NearestScanTest, ReopenProducesSameResults) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    insert_row(heap, 1, "a", {1.0F, 0.0F});
    insert_row(heap, 2, "b", {0.0F, 1.0F});

    NearestScanConfig config;
    config.k = 2;
    config.query_vector = {1.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;

    std::vector<OutputColumn> cols = {
        {"", "id", TypeId::INT32, false, 0},
        {"", "name", TypeId::STRING, true, 0},
        {"", "embedding", TypeId::EMBEDDING, true, 0},
        {"", "_distance", TypeId::FLOAT64, false, 0},
    };
    OutputSchema schema(cols);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto r1 = drain(op);
    op.close();

    ASSERT_TRUE(op.open().has_value());
    auto r2 = drain(op);
    op.close();

    ASSERT_EQ(r1.size(), r2.size());
    for (size_t i = 0; i < r1.size(); ++i) {
        EXPECT_EQ(r1[i].values[0].as_int32(), r2[i].values[0].as_int32());
    }
}

// ---------------------------------------------------------------------------
// Large dataset brute force stress
// ---------------------------------------------------------------------------

TEST_F(QA131NearestScanTest, LargeDatasetBruteForce) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    // Insert 100 rows.
    for (int i = 0; i < 100; ++i) {
        float x = static_cast<float>(i) / 100.0F;
        float y = 1.0F - x;
        insert_row(heap, i + 1, "r" + std::to_string(i), {x, y, 0.0F});
    }

    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {0.5F, 0.5F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 5u);

    // Distances should be sorted ASC.
    for (size_t i = 1; i < results.size(); ++i) {
        EXPECT_LE(results[i - 1].values[3].as_float64(), results[i].values[3].as_float64());
    }
    op.close();
}

// ---------------------------------------------------------------------------
// Graph scope with null embeddings
// ---------------------------------------------------------------------------

TEST_F(QA131NearestScanTest, GraphScopeWithNullEmbeddingNode) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    auto rid1 = insert_row(heap, 1, "valid", {1.0F, 0.0F, 0.0F});
    auto rid2 = insert_row_null_embedding(heap, 2, "null_emb");
    auto rid3 = insert_row(heap, 3, "valid2", {0.0F, 1.0F, 0.0F});

    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    // Allow all rows including the null-embedding one.
    config.allowed_rids = {rid1, rid2, rid3};

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    // Null embedding should be skipped, only 2 valid results.
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_EQ(results[1].values[0].as_int32(), 3);
    op.close();
}

// ---------------------------------------------------------------------------
// COSINE with unit vectors
// ---------------------------------------------------------------------------

TEST_F(QA131NearestScanTest, CosineUnitVectorsCorrectOrder) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    // All unit vectors in 2D.
    insert_row(heap, 1, "same", {1.0F, 0.0F});      // cos_dist = 0
    insert_row(heap, 2, "45deg", {0.707F, 0.707F}); // cos_dist ~0.293
    insert_row(heap, 3, "opposite", {-1.0F, 0.0F}); // cos_dist = 2.0

    NearestScanConfig config;
    config.k = 3;
    config.query_vector = {1.0F, 0.0F};
    config.metric = DistanceMetric::COSINE;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_EQ(results[1].values[0].as_int32(), 2);
    EXPECT_EQ(results[2].values[0].as_int32(), 3);
    op.close();
}

// ---------------------------------------------------------------------------
// HNSW with empty index falls back to brute force
// ---------------------------------------------------------------------------

TEST_F(QA131NearestScanTest, HnswEmptyIndexFallsToBruteForce) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    HnswIndex hnsw(*hnsw_bpm_, nullptr);

    HnswIndexConfig hnsw_config;
    hnsw_config.dimension = 3;
    hnsw_config.m = 8;
    hnsw_config.ef_construction = 50;
    hnsw_config.ef_search = 50;
    ASSERT_TRUE(hnsw.create(hnsw_config).has_value());
    // Don't insert into HNSW, but insert rows into table.

    insert_row(heap, 1, "a", {1.0F, 0.0F, 0.0F});
    insert_row(heap, 2, "b", {0.0F, 1.0F, 0.0F});

    NearestScanConfig config;
    config.k = 2;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    // Pass the (empty) HNSW index — should fall back to brute force.
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound, &hnsw);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    EXPECT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    op.close();
}
