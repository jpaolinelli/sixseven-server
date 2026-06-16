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
#include "sixseven/vector/hnsw_index.h"

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

    RID
    insert_row(TableHeap& heap, int32_t id, const std::string& cat, const Embedding& embedding) {
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
    insert_row(heap, 2, "B", {0.9F, 0.1F, 0.0F});               // dist=0.02 (not in filter)
    auto rid_a3 = insert_row(heap, 3, "A", {0.5F, 0.5F, 0.0F}); // dist=0.5
    auto rid_a4 = insert_row(heap, 4, "A", {0.0F, 1.0F, 0.0F}); // dist=2.0
    insert_row(heap, 5, "B", {0.8F, 0.2F, 0.0F});               // dist=0.08 (not in filter)

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
    insert_row(heap, 1, "A", {0.5F, 0.5F, 0.0F});                    // dist=0.5
    auto rid_closest = insert_row(heap, 2, "B", {1.0F, 0.0F, 0.0F}); // dist=0
    auto rid_a3 = insert_row(heap, 3, "A", {0.0F, 1.0F, 0.0F});      // dist=2.0

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
    // When prefiltered_rids is set, the prefiltered path MUST be chosen even
    // when a populated HNSW index is also supplied.  The original test used
    // nullptr for the HNSW index, so swapping the two dispatch branches in
    // do_open() changed nothing — the test was vacuous.
    //
    // This version builds a real HnswIndex over ALL rows (including the
    // non-prefiltered row 2 which is closest to the query) and passes it
    // together with a populated rid_map.  If the dispatcher regresses and
    // chooses HNSW-first, row 2 appears in the results and the assertions below
    // fail, closing the gap the original test claimed to cover.
    TableHeap heap(*table_bpm_, dm_, table_fid_);

    // Row 2 is the absolute nearest to [1,0,0] (dist≈0.01) but is NOT in the
    // prefilter.  It IS inserted into the HNSW index, so a regressed
    // HNSW-first dispatcher would return it, breaking the exclusion assertion.
    auto rid1 = insert_row(heap, 1, "A", {1.0F, 0.0F, 0.0F});  // dist=0
    auto rid2 = insert_row(heap, 2, "B", {0.99F, 0.0F, 0.0F}); // dist≈0.01, NOT in prefilter
    auto rid3 = insert_row(heap, 3, "A", {0.0F, 1.0F, 0.0F});  // dist=2.0

    // Build a real HNSW index covering all three rows.  Insertion order
    // determines node_id: rid1 → node 0, rid2 → node 1, rid3 → node 2.
    // Use a nested scope so hnsw_bpm is destroyed before we close the file,
    // avoiding BPM flush errors on teardown.
    auto hnsw_path = std::filesystem::temp_directory_path() / "sixseven_qa_prefilter_hnsw.db";
    std::filesystem::remove(hnsw_path);
    auto hnsw_fid = dm_.create_file(hnsw_path, false, true);
    ASSERT_TRUE(hnsw_fid.has_value()) << hnsw_fid.error().message;

    std::vector<RID> rid_map;
    std::vector<Tuple> results;

    {
        BufferPoolManager hnsw_bpm(dm_, *hnsw_fid, 64);
        HnswIndex hnsw_index(hnsw_bpm, nullptr);
        HnswIndexConfig hnsw_cfg;
        hnsw_cfg.dimension = 3;
        hnsw_cfg.m = 4;
        hnsw_cfg.ef_construction = 16;
        hnsw_cfg.ef_search = 16;
        hnsw_cfg.metric = DistanceMetric::L2;
        ASSERT_TRUE(hnsw_index.create(hnsw_cfg).has_value());

        // Insert embeddings in the same order as row insertion so node IDs match.
        std::vector<float> emb1 = {1.0F, 0.0F, 0.0F};
        std::vector<float> emb2 = {0.99F, 0.0F, 0.0F};
        std::vector<float> emb3 = {0.0F, 1.0F, 0.0F};
        auto n1 = hnsw_index.insert(emb1);
        ASSERT_TRUE(n1.has_value());
        auto n2 = hnsw_index.insert(emb2);
        ASSERT_TRUE(n2.has_value());
        auto n3 = hnsw_index.insert(emb3);
        ASSERT_TRUE(n3.has_value());
        ASSERT_EQ(hnsw_index.node_count(), 3u);

        // Build the rid_map: node_id → heap RID, matching insertion order.
        rid_map.resize(*n3 + 1);
        rid_map[*n1] = rid1;
        rid_map[*n2] = rid2;
        rid_map[*n3] = rid3;

        // Prefilter excludes rid2 (the closest row).
        NearestScanConfig config;
        config.k = 5;
        config.query_vector = {1.0F, 0.0F, 0.0F};
        config.metric = DistanceMetric::L2;
        config.embedding_column_index = 2;
        config.prefiltered_rids = {rid1, rid3};

        OutputSchema schema(output_cols_);
        BoundStatement bound;

        // Supply a POPULATED HnswIndex + rid_map.  The dispatcher must choose
        // the prefiltered path FIRST (before the HNSW branch) and honour the
        // filter.
        NearestScanOperator op(heap,
                               storage_schema_,
                               std::move(config),
                               std::move(schema),
                               nullptr,
                               bound,
                               &hnsw_index,
                               &rid_map);

        ASSERT_TRUE(op.open().has_value());
        results = drain(op);
        op.close();
        // hnsw_bpm destroyed here (before file close below).
    }

    (void)dm_.close_file(*hnsw_fid);
    std::filesystem::remove(hnsw_path);

    // The prefiltered set has exactly 2 rows; none should be row 2 (id=2).
    ASSERT_EQ(results.size(), 2u);
    for (const auto& row : results) {
        EXPECT_NE(row.values[0].as_int32(), 2) << "row 2 must be excluded by prefilter";
    }
    // Exact order: rid1 (dist=0) before rid3 (dist=2).
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_EQ(results[1].values[0].as_int32(), 3);
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
