#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/nearest_scan.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/graph/graph_engine.h"
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
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class NearestScanTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Set up table storage.
        table_path_ = std::filesystem::temp_directory_path() / "sixseven_test_nearest.db";
        std::filesystem::remove(table_path_);

        auto fid = dm_.create_file(table_path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        table_file_id_ = *fid;

        table_bpm_ = std::make_unique<BufferPoolManager>(dm_, table_file_id_, 64);

        // Set up HNSW index storage.
        hnsw_path_ = std::filesystem::temp_directory_path() / "sixseven_test_nearest_hnsw.db";
        std::filesystem::remove(hnsw_path_);

        auto hfid = dm_.create_file(hnsw_path_, false, true);
        ASSERT_TRUE(hfid.has_value()) << hfid.error().message;
        hnsw_file_id_ = *hfid;

        hnsw_bpm_ = std::make_unique<BufferPoolManager>(dm_, hnsw_file_id_, 256);

        // Table schema: id (INT32), name (STRING), embedding (EMBEDDING).
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
        (void)dm_.close_file(hnsw_file_id_);
        (void)dm_.close_file(table_file_id_);
        std::filesystem::remove(hnsw_path_);
        std::filesystem::remove(table_path_);
    }

    /// Insert a row with an embedding into the table heap.
    RID
    insert_row(TableHeap& heap, int32_t id, const std::string& name, const Embedding& embedding) {
        std::vector<Value> vals = {Value(id), Value(name), Value(embedding)};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        EXPECT_TRUE(bytes.has_value()) << bytes.error().message;
        auto rid = heap.insert_tuple(*bytes);
        EXPECT_TRUE(rid.has_value()) << rid.error().message;
        return *rid;
    }

    /// Helper: drain all results from an opened operator.
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

    /// Helper: create expression helpers.
    static ExprPtr lit_int(const std::string& v) {
        auto e = std::make_unique<LiteralExpr>();
        e->kind = LiteralKind::INTEGER;
        e->value = v;
        return e;
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
    FileId table_file_id_ = 0;
    FileId hnsw_file_id_ = 0;
    std::unique_ptr<BufferPoolManager> table_bpm_;
    std::unique_ptr<BufferPoolManager> hnsw_bpm_;
    Schema storage_schema_;
    std::vector<OutputColumn> output_cols_;
};

// =============================================================================
// Brute-force NEAREST tests (no HNSW index)
// =============================================================================

TEST_F(NearestScanTest, BruteForceBasicNearest) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    // Insert 5 rows with 3D embeddings.
    insert_row(heap, 1, "alpha", {1.0F, 0.0F, 0.0F});
    insert_row(heap, 2, "beta", {0.0F, 1.0F, 0.0F});
    insert_row(heap, 3, "gamma", {0.0F, 0.0F, 1.0F});
    insert_row(heap, 4, "delta", {1.0F, 1.0F, 0.0F});
    insert_row(heap, 5, "epsilon", {0.5F, 0.5F, 0.5F});

    // Query: find 3 nearest to [1, 0, 0] using L2 distance.
    NearestScanConfig config;
    config.k = 3;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    auto open_res = op.open();
    ASSERT_TRUE(open_res.has_value()) << open_res.error().message;

    auto results = drain(op);
    ASSERT_EQ(results.size(), 3u);

    // Results should be sorted by distance ASC.
    // Row 1: [1,0,0] → L2 distance 0
    // Row 4: [1,1,0] → L2 distance (0+1+0) = 1.0
    // Row 5: [0.5,0.5,0.5] → L2 distance (0.25+0.25+0.25) = 0.75
    // So order: row 1 (0), row 5 (0.75), row 4 (1.0)
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_EQ(results[1].values[0].as_int32(), 5);
    EXPECT_EQ(results[2].values[0].as_int32(), 4);

    // Verify _distance column is present and correct.
    EXPECT_NEAR(results[0].values[3].as_float64(), 0.0, 1e-5);
    EXPECT_NEAR(results[1].values[3].as_float64(), 0.75, 1e-5);
    EXPECT_NEAR(results[2].values[3].as_float64(), 1.0, 1e-5);

    op.close();
}

TEST_F(NearestScanTest, BruteForceKLargerThanRows) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    insert_row(heap, 1, "a", {1.0F, 0.0F});
    insert_row(heap, 2, "b", {0.0F, 1.0F});

    NearestScanConfig config;
    config.k = 10; // More than available rows.
    config.query_vector = {1.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    EXPECT_EQ(results.size(), 2u);
    op.close();
}

TEST_F(NearestScanTest, BruteForceEmptyTable) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

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

TEST_F(NearestScanTest, BruteForceCosineMetric) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    // Insert vectors with different directions.
    insert_row(heap, 1, "same", {1.0F, 0.0F, 0.0F});
    insert_row(heap, 2, "opposite", {-1.0F, 0.0F, 0.0F});
    insert_row(heap, 3, "orthogonal", {0.0F, 1.0F, 0.0F});

    NearestScanConfig config;
    config.k = 3;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::COSINE;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 3u);

    // Cosine: same direction → 0, orthogonal → 1, opposite → 2.
    EXPECT_EQ(results[0].values[0].as_int32(), 1); // cosine_dist = 0
    EXPECT_EQ(results[1].values[0].as_int32(), 3); // cosine_dist = 1
    EXPECT_EQ(results[2].values[0].as_int32(), 2); // cosine_dist = 2

    op.close();
}

TEST_F(NearestScanTest, BruteForceDistanceColumnIncluded) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    insert_row(heap, 1, "near", {0.9F, 0.1F, 0.0F});
    insert_row(heap, 2, "far", {0.0F, 0.0F, 10.0F});

    NearestScanConfig config;
    config.k = 2;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 2u);

    // Verify output schema has 4 columns (id, name, embedding, _distance).
    EXPECT_EQ(op.output_schema().column_count(), 4u);
    EXPECT_EQ(op.output_schema().column(3).name, "_distance");
    EXPECT_EQ(op.output_schema().column(3).type_id, TypeId::FLOAT64);

    // Verify distances are non-negative and sorted.
    double d0 = results[0].values[3].as_float64();
    double d1 = results[1].values[3].as_float64();
    EXPECT_GE(d0, 0.0);
    EXPECT_GE(d1, 0.0);
    EXPECT_LE(d0, d1);

    op.close();
}

// =============================================================================
// WHERE filter tests
// =============================================================================

TEST_F(NearestScanTest, BruteForceWithWhereFilter) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    insert_row(heap, 1, "alpha", {1.0F, 0.0F, 0.0F}); // closest, name=alpha
    insert_row(heap, 2, "beta", {0.9F, 0.1F, 0.0F});  // 2nd closest, name=beta
    insert_row(heap, 3, "alpha", {0.5F, 0.5F, 0.0F}); // 3rd closest, name=alpha
    insert_row(heap, 4, "beta", {0.0F, 1.0F, 0.0F});  // further, name=beta

    // WHERE name = 'beta' — should skip alpha rows.
    auto where = binary_expr(BinaryOp::EQUAL, col_ref("name"), lit_string("beta"));

    // Register the WHERE expression types in the BoundStatement.
    BoundStatement bound;
    // The ColumnRefExpr needs a type entry in the bound statement.
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

    // Build output schema without _distance for the table columns
    // (the WHERE filter operates on table columns).
    OutputSchema schema(output_cols_);

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), where.get(), bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);

    // Should get only beta rows.
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].values[1].as_string(), "beta");
    EXPECT_EQ(results[1].values[1].as_string(), "beta");

    // Sorted by distance: row 2 should be closer than row 4.
    EXPECT_EQ(results[0].values[0].as_int32(), 2);
    EXPECT_EQ(results[1].values[0].as_int32(), 4);

    op.close();
}

// =============================================================================
// Graph-scoped NEAREST tests
// =============================================================================

TEST_F(NearestScanTest, BruteForceWithGraphScope) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    insert_row(heap, 1, "a", {1.0F, 0.0F, 0.0F}); // node_ordinal 0
    insert_row(heap, 2, "b", {0.9F, 0.1F, 0.0F}); // node_ordinal 1
    insert_row(heap, 3, "c", {0.8F, 0.2F, 0.0F}); // node_ordinal 2
    insert_row(heap, 4, "d", {0.1F, 0.9F, 0.0F}); // node_ordinal 3
    insert_row(heap, 5, "e", {0.0F, 1.0F, 0.0F}); // node_ordinal 4

    // Only allow node ordinals 0, 2, 4 (simulating graph scope).
    NearestScanConfig config;
    config.k = 3;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.allowed_node_ids = {0, 2, 4};

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);

    // Should only contain rows with ordinals 0, 2, 4 → ids 1, 3, 5.
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].values[0].as_int32(), 1); // closest among allowed
    EXPECT_EQ(results[1].values[0].as_int32(), 3); // next closest
    EXPECT_EQ(results[2].values[0].as_int32(), 5); // furthest of allowed

    op.close();
}

// =============================================================================
// HNSW index search tests
// =============================================================================

TEST_F(NearestScanTest, HnswIndexBasicSearch) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);
    HnswIndex hnsw(*hnsw_bpm_, nullptr);

    // Create a 3-dimensional HNSW index.
    HnswIndexConfig hnsw_config;
    hnsw_config.dimension = 3;
    hnsw_config.m = 8;
    hnsw_config.ef_construction = 50;
    hnsw_config.ef_search = 50;
    ASSERT_TRUE(hnsw.create(hnsw_config).has_value());

    // Insert rows and vectors in the same order.
    Embedding emb1 = {1.0F, 0.0F, 0.0F};
    Embedding emb2 = {0.0F, 1.0F, 0.0F};
    Embedding emb3 = {0.0F, 0.0F, 1.0F};
    Embedding emb4 = {1.0F, 1.0F, 0.0F};
    Embedding emb5 = {0.5F, 0.5F, 0.5F};

    insert_row(heap, 1, "alpha", emb1);
    insert_row(heap, 2, "beta", emb2);
    insert_row(heap, 3, "gamma", emb3);
    insert_row(heap, 4, "delta", emb4);
    insert_row(heap, 5, "epsilon", emb5);

    // Insert vectors into HNSW in the same order (node_id 0..4).
    ASSERT_TRUE(hnsw.insert(emb1).has_value());
    ASSERT_TRUE(hnsw.insert(emb2).has_value());
    ASSERT_TRUE(hnsw.insert(emb3).has_value());
    ASSERT_TRUE(hnsw.insert(emb4).has_value());
    ASSERT_TRUE(hnsw.insert(emb5).has_value());

    NearestScanConfig config;
    config.k = 3;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound, &hnsw);

    auto open_res = op.open();
    ASSERT_TRUE(open_res.has_value()) << open_res.error().message;

    auto results = drain(op);
    ASSERT_EQ(results.size(), 3u);

    // With HNSW search, the top-3 closest to [1,0,0] should be:
    // alpha (0), epsilon (0.5), delta (1.0) — same as brute force.
    EXPECT_EQ(results[0].values[0].as_int32(), 1);

    // Verify distance column is present.
    EXPECT_NEAR(results[0].values[3].as_float64(), 0.0, 1e-3);

    // Verify sorted by distance.
    for (size_t i = 1; i < results.size(); ++i) {
        EXPECT_LE(results[i - 1].values[3].as_float64(), results[i].values[3].as_float64());
    }

    op.close();
}

// Regression: a single physical row can be referenced by more than one HNSW
// node (e.g. it was embedded twice during demo bootstrap). NEAREST must still
// return that row exactly once, not once per node.
TEST_F(NearestScanTest, DeduplicatesRowWithMultipleHnswNodes) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);
    HnswIndex hnsw(*hnsw_bpm_, nullptr);

    HnswIndexConfig hnsw_config;
    hnsw_config.dimension = 3;
    hnsw_config.m = 8;
    hnsw_config.ef_construction = 50;
    hnsw_config.ef_search = 50;
    ASSERT_TRUE(hnsw.create(hnsw_config).has_value());

    Embedding emb1 = {1.0F, 0.0F, 0.0F};
    Embedding emb2 = {0.0F, 1.0F, 0.0F};
    RID r1 = insert_row(heap, 1, "alpha", emb1);
    RID r2 = insert_row(heap, 2, "beta", emb2);

    // Row 1 was embedded twice: emb1 appears as two distinct HNSW nodes.
    ASSERT_TRUE(hnsw.insert(emb1).has_value()); // node 0 -> r1
    ASSERT_TRUE(hnsw.insert(emb1).has_value()); // node 1 -> r1 (duplicate)
    ASSERT_TRUE(hnsw.insert(emb2).has_value()); // node 2 -> r2

    // node_id -> RID map with two nodes pointing at the same row.
    std::vector<RID> rid_map = {r1, r1, r2};

    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    NearestScanOperator op(heap,
                           storage_schema_,
                           std::move(config),
                           std::move(schema),
                           nullptr,
                           bound,
                           &hnsw,
                           &rid_map);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);

    // Two distinct rows exist; row 1 must appear exactly once.
    EXPECT_EQ(results.size(), 2u);
    int count_r1 = 0;
    for (const auto& t : results) {
        if (t.values[0].as_int32() == 1) {
            ++count_r1;
        }
    }
    EXPECT_EQ(count_r1, 1);

    op.close();
}

TEST_F(NearestScanTest, HnswIndexFilteredSearch) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);
    HnswIndex hnsw(*hnsw_bpm_, nullptr);

    HnswIndexConfig hnsw_config;
    hnsw_config.dimension = 3;
    hnsw_config.m = 8;
    hnsw_config.ef_construction = 50;
    hnsw_config.ef_search = 50;
    ASSERT_TRUE(hnsw.create(hnsw_config).has_value());

    Embedding emb1 = {1.0F, 0.0F, 0.0F};
    Embedding emb2 = {0.9F, 0.1F, 0.0F};
    Embedding emb3 = {0.5F, 0.5F, 0.0F};
    Embedding emb4 = {0.0F, 1.0F, 0.0F};

    insert_row(heap, 1, "a", emb1);
    insert_row(heap, 2, "b", emb2);
    insert_row(heap, 3, "c", emb3);
    insert_row(heap, 4, "d", emb4);

    ASSERT_TRUE(hnsw.insert(emb1).has_value());
    ASSERT_TRUE(hnsw.insert(emb2).has_value());
    ASSERT_TRUE(hnsw.insert(emb3).has_value());
    ASSERT_TRUE(hnsw.insert(emb4).has_value());

    // Graph-scoped: only allow node ordinals 0, 2 (rows 1, 3).
    NearestScanConfig config;
    config.k = 2;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.allowed_node_ids = {0, 2};

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound, &hnsw);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 2u);

    // Only rows 1 and 3 should be returned.
    std::vector<int32_t> ids;
    for (const auto& r : results) {
        ids.push_back(r.values[0].as_int32());
    }
    EXPECT_TRUE(std::find(ids.begin(), ids.end(), 1) != ids.end());
    EXPECT_TRUE(std::find(ids.begin(), ids.end(), 3) != ids.end());

    op.close();
}

// =============================================================================
// USING metric selection tests
// =============================================================================

TEST_F(NearestScanTest, MetricL2ReturnsCorrectOrder) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    insert_row(heap, 1, "near", {0.9F, 0.0F, 0.0F});
    insert_row(heap, 2, "far", {0.0F, 5.0F, 0.0F});
    insert_row(heap, 3, "mid", {0.5F, 0.5F, 0.0F});

    NearestScanConfig config;
    config.k = 3;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 3u);

    // L2: [0.9,0,0] closest (dist=0.01), [0.5,0.5,0] mid (dist=0.5), [0,5,0] far (dist=26.0)
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_EQ(results[1].values[0].as_int32(), 3);
    EXPECT_EQ(results[2].values[0].as_int32(), 2);

    op.close();
}

// =============================================================================
// Output schema tests
// =============================================================================

TEST_F(NearestScanTest, OutputSchemaHasDistanceColumn) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    const auto& out = op.output_schema();
    ASSERT_EQ(out.column_count(), 4u);
    EXPECT_EQ(out.column(0).name, "id");
    EXPECT_EQ(out.column(1).name, "name");
    EXPECT_EQ(out.column(2).name, "embedding");
    EXPECT_EQ(out.column(3).name, "_distance");
    EXPECT_EQ(out.column(3).type_id, TypeId::FLOAT64);
}

// =============================================================================
// Re-open after close tests
// =============================================================================

TEST_F(NearestScanTest, ReopenAfterClose) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    insert_row(heap, 1, "a", {1.0F, 0.0F});
    insert_row(heap, 2, "b", {0.0F, 1.0F});

    NearestScanConfig config;
    config.k = 2;
    config.query_vector = {1.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;

    // Use 2D output schema.
    std::vector<OutputColumn> cols2d = {
        {"", "id", TypeId::INT32, false, 0},
        {"", "name", TypeId::STRING, true, 0},
        {"", "embedding", TypeId::EMBEDDING, true, 0},
        {"", "_distance", TypeId::FLOAT64, false, 0},
    };
    OutputSchema schema(cols2d);
    BoundStatement bound;

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    // First run.
    ASSERT_TRUE(op.open().has_value());
    auto results1 = drain(op);
    EXPECT_EQ(results1.size(), 2u);
    op.close();

    // Re-open and run again.
    ASSERT_TRUE(op.open().has_value());
    auto results2 = drain(op);
    EXPECT_EQ(results2.size(), 2u);
    op.close();
}

// =============================================================================
// K=1 (single result) test
// =============================================================================

TEST_F(NearestScanTest, SingleNearestNeighbor) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    insert_row(heap, 1, "a", {0.0F, 0.0F, 0.0F});
    insert_row(heap, 2, "b", {10.0F, 10.0F, 10.0F});
    insert_row(heap, 3, "c", {5.0F, 5.0F, 5.0F});

    NearestScanConfig config;
    config.k = 1;
    config.query_vector = {0.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_NEAR(results[0].values[3].as_float64(), 0.0, 1e-5);

    op.close();
}

// =============================================================================
// Pre-filtered search tests (btree-accelerated)
// =============================================================================

TEST_F(NearestScanTest, PrefilteredBasicNearest) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    // Insert 5 rows and record their RIDs.
    insert_row(heap, 1, "alpha", {1.0F, 0.0F, 0.0F});
    auto rid2 = insert_row(heap, 2, "beta", {0.0F, 1.0F, 0.0F});
    insert_row(heap, 3, "gamma", {0.0F, 0.0F, 1.0F});
    auto rid4 = insert_row(heap, 4, "delta", {1.0F, 1.0F, 0.0F});
    auto rid5 = insert_row(heap, 5, "epsilon", {0.5F, 0.5F, 0.5F});

    // Pre-filter to only rows 2, 4, 5 (simulating btree lookup result).
    NearestScanConfig config;
    config.k = 2;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.prefiltered_rids = {rid2, rid4, rid5};

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 2u);

    // Among {beta [0,1,0], delta [1,1,0], epsilon [0.5,0.5,0.5]}, nearest to [1,0,0]:
    // delta: L2 = 0+1+0 = 1.0
    // epsilon: L2 = 0.25+0.25+0.25 = 0.75
    // beta: L2 = 1+1+0 = 2.0
    // Top-2: epsilon (0.75), delta (1.0)
    EXPECT_EQ(results[0].values[0].as_int32(), 5); // epsilon
    EXPECT_EQ(results[1].values[0].as_int32(), 4); // delta

    op.close();
}

TEST_F(NearestScanTest, PrefilteredEmptyRIDs) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    insert_row(heap, 1, "a", {1.0F, 0.0F, 0.0F});

    // Empty prefiltered set should return no results (but NOT fall through
    // to brute-force — empty means "no candidates matched the btree filter").
    // Actually, empty prefiltered_rids means "no prefilter", so it falls through.
    // A non-empty prefiltered_rids with no valid rows is the real test.
    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    // Use an invalid RID that won't match any real tuple.
    config.prefiltered_rids = {RID{9999, 9999}};

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    EXPECT_TRUE(results.empty());
    op.close();
}

TEST_F(NearestScanTest, PrefilteredWithWherePostFilter) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    auto rid1 = insert_row(heap, 1, "alpha", {1.0F, 0.0F, 0.0F});
    auto rid2 = insert_row(heap, 2, "beta", {0.9F, 0.1F, 0.0F});
    auto rid3 = insert_row(heap, 3, "alpha", {0.5F, 0.5F, 0.0F});
    auto rid4 = insert_row(heap, 4, "beta", {0.0F, 1.0F, 0.0F});

    // Pre-filtered RIDs include all rows (simulating btree on a different column),
    // but WHERE post-filter selects only "beta" rows.
    auto where = binary_expr(BinaryOp::EQUAL, col_ref("name"), lit_string("beta"));
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

    // Only beta rows: rid2 (closer) and rid4 (further).
    EXPECT_EQ(results[0].values[1].as_string(), "beta");
    EXPECT_EQ(results[1].values[1].as_string(), "beta");
    EXPECT_EQ(results[0].values[0].as_int32(), 2);
    EXPECT_EQ(results[1].values[0].as_int32(), 4);

    op.close();
}

TEST_F(NearestScanTest, PrefilteredTopKSelection) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    // Insert 10 rows with embeddings at increasing distance from [0,0,0].
    std::vector<RID> rids;
    for (int i = 1; i <= 10; ++i) {
        auto f = static_cast<float>(i);
        rids.push_back(insert_row(heap, i, "row" + std::to_string(i), {f, 0.0F, 0.0F}));
    }

    // Pre-filter all 10, ask for top-3 nearest to origin.
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

    // Closest 3: ids 1, 2, 3.
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_EQ(results[1].values[0].as_int32(), 2);
    EXPECT_EQ(results[2].values[0].as_int32(), 3);

    op.close();
}

TEST_F(NearestScanTest, PrefilteredSkipsNullEmbeddings) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    // Insert a row with a null embedding.
    std::vector<Value> null_vals = {Value(int32_t{1}), Value(std::string("null_emb")), Value()};
    auto bytes = TupleSerializer::serialize(null_vals, storage_schema_);
    ASSERT_TRUE(bytes.has_value());
    auto rid_null = heap.insert_tuple(*bytes);
    ASSERT_TRUE(rid_null.has_value());

    auto rid2 = insert_row(heap, 2, "has_emb", {1.0F, 0.0F, 0.0F});

    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.prefiltered_rids = {*rid_null, rid2};

    OutputSchema schema(output_cols_);
    BoundStatement bound;

    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);

    // Only the row with a non-null embedding should be returned.
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].values[0].as_int32(), 2);

    op.close();
}
