/// @file test_qa_gdb_717.cpp
/// @brief QA regression tests for GDB-717: NEAREST ... USING DOT returned the
///        k LEAST similar rows (raw dot product sorted ascending).
///
/// The dot product is a similarity (higher = more similar). Before the fix,
/// the planner mapped USING DOT to DistanceMetric::DOT_PRODUCT (raw dot) and
/// NearestScanOperator sorted candidates ascending, silently inverting the
/// results. The fix maps USING DOT to the negated INNER_PRODUCT sort key and
/// reports the raw dot product in the user-visible _distance column.
///
/// Covers: end-to-end SQL (parser -> planner -> executor) ordering for DOT,
/// the _distance column value (raw dot, including negative dots), the
/// brute-force and btree-prefiltered operator paths, WHERE post-filtering,
/// and L2/COSINE ordering staying unchanged. Also locks in the corrected
/// ordering that GDB-731 previously asserted inverted.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/nearest_scan.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"
#include "sixseven/vector/distance.h"
#include "sixseven/vector/provider_registry.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// End-to-end SQL pipeline fixture (parser -> planner -> executor)
// =============================================================================

class QA717NearestDotEndToEndTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb717";
        std::error_code ec;
        std::filesystem::remove_all(data_dir_, ec);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());
        provider_registry_ = std::make_unique<ProviderRegistry>(catalog_);

        exec_ok("CREATE TABLE items (id INT PRIMARY KEY, name VARCHAR, vec EMBEDDING)");

        auto items = catalog_.get_table(default_database_id, "items");
        ASSERT_TRUE(items.has_value());
        register_embedding(items->table_id, 2, 4, "name", "builtin/4");
        engine_->set_provider_registry(provider_registry_.get());

        // Dot products against the query vector [1, 0, 0, 0]:
        //   best:  2.0, good: 0.9, weak: 0.1, ortho: 0.0, anti: -1.0
        exec_ok("INSERT INTO items VALUES (1, 'best', [2.0, 0.0, 0.0, 0.0])");
        exec_ok("INSERT INTO items VALUES (2, 'good', [0.9, 0.0, 0.0, 0.0])");
        exec_ok("INSERT INTO items VALUES (3, 'weak', [0.1, 0.0, 0.0, 0.0])");
        exec_ok("INSERT INTO items VALUES (4, 'ortho', [0.0, 1.0, 0.0, 0.0])");
        exec_ok("INSERT INTO items VALUES (5, 'anti', [-1.0, 0.0, 0.0, 0.0])");
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        provider_registry_.reset();
        std::error_code ec;
        std::filesystem::remove_all(data_dir_, ec);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << "SQL: " << sql << "\nError: " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    void register_embedding(table_id_t table_id,
                            int32_t col_id,
                            int32_t dim,
                            const std::string& source,
                            const std::string& provider) {
        EmbeddingColumnDef emb_def;
        emb_def.table_id = table_id;
        emb_def.column_id = col_id;
        emb_def.dimension = dim;
        emb_def.source_expr = source;
        emb_def.provider = provider;
        ASSERT_TRUE(catalog_.register_embedding_column(emb_def).has_value());
        if (catalog_.get_embedding_provider(provider).has_value()) {
            return;
        }
        EmbeddingProviderConfig prov;
        prov.name = provider;
        prov.type = "builtin";
        prov.dimension = dim;
        ASSERT_TRUE(catalog_.register_embedding_provider(prov).has_value());
    }

    std::vector<std::string> names_in_order(const QueryResult& qr) {
        std::vector<std::string> out;
        out.reserve(qr.rows.size());
        for (const auto& row : qr.rows) {
            out.push_back(row[0].as_string());
        }
        return out;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<ProviderRegistry> provider_registry_;
};

// The core GDB-717 regression: USING DOT with k=2 must return the two rows
// with the HIGHEST dot product. Before the fix it returned the two lowest
// ('anti' and 'ortho').
TEST_F(QA717NearestDotEndToEndTest, UsingDotTopKReturnsMostSimilarRows) {
    auto qr = exec_ok("SELECT name FROM items "
                      "WHERE NEAREST(vec, 2) TO [1.0, 0.0, 0.0, 0.0] USING DOT");

    auto got = names_in_order(qr);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], "best");
    EXPECT_EQ(got[1], "good");
}

// Full ordering: highest dot product first, lowest (negative) last.
TEST_F(QA717NearestDotEndToEndTest, UsingDotFullOrderingHighestDotFirst) {
    auto qr = exec_ok("SELECT name FROM items "
                      "WHERE NEAREST(vec, 5) TO [1.0, 0.0, 0.0, 0.0] USING DOT");

    auto got = names_in_order(qr);
    ASSERT_EQ(got.size(), 5u);
    EXPECT_EQ(got[0], "best");
    EXPECT_EQ(got[1], "good");
    EXPECT_EQ(got[2], "weak");
    EXPECT_EQ(got[3], "ortho");
    EXPECT_EQ(got[4], "anti");
}

// The user-visible _distance column reports the RAW dot product (higher =
// more similar), not the negated internal sort key. Negative dots stay
// negative.
TEST_F(QA717NearestDotEndToEndTest, UsingDotDistanceColumnIsRawDotProduct) {
    auto qr = exec_ok("SELECT name, _distance FROM items "
                      "WHERE NEAREST(vec, 5) TO [1.0, 0.0, 0.0, 0.0] USING DOT");

    ASSERT_EQ(qr.rows.size(), 5u);
    EXPECT_NEAR(qr.rows[0][1].as_float64(), 2.0, 1e-5);
    EXPECT_NEAR(qr.rows[1][1].as_float64(), 0.9, 1e-5);
    EXPECT_NEAR(qr.rows[2][1].as_float64(), 0.1, 1e-5);
    EXPECT_NEAR(qr.rows[3][1].as_float64(), 0.0, 1e-5);
    EXPECT_NEAR(qr.rows[4][1].as_float64(), -1.0, 1e-5);
}

// A sibling AND conjunct post-filters the top-k window as a strict
// intersection: it must not widen past the k-th nearest candidate to
// backfill a slot rejected by the filter (GDB-1229). Top-2 by dot similarity
// are {best(id=1), good(id=2)}; `id > 1` excludes best from that fixed
// window, leaving only good — NOT {good, weak}, since weak is the 3rd
// nearest and outside the top-2 window.
TEST_F(QA717NearestDotEndToEndTest, UsingDotWithWherePostFilter) {
    auto qr = exec_ok("SELECT name FROM items "
                      "WHERE NEAREST(vec, 2) TO [1.0, 0.0, 0.0, 0.0] USING DOT "
                      "AND id > 1");

    auto got = names_in_order(qr);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], "good");
}

// Guard: USING L2 ordering is untouched by the DOT fix (lower L2 first).
// L2 squared distances to [1,0,0,0]: good=0.01, weak=0.81, best=1.0,
// ortho=2.0, anti=4.0.
TEST_F(QA717NearestDotEndToEndTest, UsingL2OrderingUnchanged) {
    auto qr = exec_ok("SELECT name FROM items "
                      "WHERE NEAREST(vec, 3) TO [1.0, 0.0, 0.0, 0.0] USING L2");

    auto got = names_in_order(qr);
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], "good");
    EXPECT_EQ(got[1], "weak");
    EXPECT_EQ(got[2], "best");
}

// Guard: USING COSINE ordering is untouched by the DOT fix. best/good/weak
// are all parallel to the query (cosine distance 0) and must beat ortho
// (distance 1) and anti (distance 2).
TEST_F(QA717NearestDotEndToEndTest, UsingCosineOrderingUnchanged) {
    auto qr = exec_ok("SELECT name FROM items "
                      "WHERE NEAREST(vec, 4) TO [1.0, 0.0, 0.0, 0.0] USING COSINE");

    auto got = names_in_order(qr);
    ASSERT_EQ(got.size(), 4u);
    // The three parallel vectors tie at distance 0 — order among them is
    // unspecified, but all must precede 'ortho'.
    EXPECT_EQ(got[3], "ortho");
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_TRUE(got[i] == "best" || got[i] == "good" || got[i] == "weak")
            << "unexpected row at rank " << i << ": " << got[i];
    }
}

// =============================================================================
// Operator-level fixture (brute-force and btree-prefiltered paths)
// =============================================================================

class QA717NearestScanOperatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        table_path_ = std::filesystem::temp_directory_path() / "sixseven_qa717_table.db";
        std::filesystem::remove(table_path_);
        auto fid = dm_.create_file(table_path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        table_fid_ = *fid;
        table_bpm_ = std::make_unique<BufferPoolManager>(dm_, table_fid_, 64);

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
        table_bpm_.reset();
        (void)dm_.close_file(table_fid_);
        std::filesystem::remove(table_path_);
    }

    RID
    insert_row(TableHeap& heap, int32_t id, const std::string& name, const Embedding& embedding) {
        std::vector<Value> vals = {Value(id), Value(name), Value(embedding)};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        EXPECT_TRUE(bytes.has_value()) << bytes.error().message;
        if (!bytes.has_value()) {
            return RID::invalid();
        }
        auto rid = heap.insert_tuple(*bytes);
        EXPECT_TRUE(rid.has_value()) << rid.error().message;
        if (!rid.has_value()) {
            return RID::invalid();
        }
        return *rid;
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

    std::filesystem::path table_path_;
    DiskManager dm_;
    FileId table_fid_ = 0;
    std::unique_ptr<BufferPoolManager> table_bpm_;
    Schema storage_schema_;
    std::vector<OutputColumn> output_cols_;
};

// Brute-force path with the metric the planner now emits for USING DOT
// (INNER_PRODUCT): most similar row first, _distance = raw dot product.
TEST_F(QA717NearestScanOperatorTest, BruteForceInnerProductRanksMostSimilarFirst) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    insert_row(heap, 1, "aligned", {1.0F, 0.0F, 0.0F});
    insert_row(heap, 2, "partial", {0.5F, 0.5F, 0.0F});
    insert_row(heap, 3, "orthogonal", {0.0F, 1.0F, 0.0F});
    insert_row(heap, 4, "opposite", {-1.0F, 0.0F, 0.0F});

    NearestScanConfig config;
    config.k = 4;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::INNER_PRODUCT;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 4u);
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_EQ(results[1].values[0].as_int32(), 2);
    EXPECT_EQ(results[2].values[0].as_int32(), 3);
    EXPECT_EQ(results[3].values[0].as_int32(), 4);
    // _distance reports the raw dot product, including the negative one.
    EXPECT_NEAR(results[0].values[3].as_float64(), 1.0, 1e-5);
    EXPECT_NEAR(results[1].values[3].as_float64(), 0.5, 1e-5);
    EXPECT_NEAR(results[2].values[3].as_float64(), 0.0, 1e-5);
    EXPECT_NEAR(results[3].values[3].as_float64(), -1.0, 1e-5);
    op.close();
}

// A direct DOT_PRODUCT config (raw similarity) is normalized by the operator:
// it must behave exactly like INNER_PRODUCT instead of inverting the order.
TEST_F(QA717NearestScanOperatorTest, BruteForceRawDotProductConfigNotInverted) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    insert_row(heap, 1, "aligned", {1.0F, 0.0F, 0.0F});
    insert_row(heap, 2, "partial", {0.5F, 0.5F, 0.0F});
    insert_row(heap, 3, "orthogonal", {0.0F, 1.0F, 0.0F});

    NearestScanConfig config;
    config.k = 2;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::DOT_PRODUCT;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    // Before GDB-717 this returned the two LEAST similar rows (3 then 2).
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_EQ(results[1].values[0].as_int32(), 2);
    EXPECT_NEAR(results[0].values[3].as_float64(), 1.0, 1e-5);
    EXPECT_NEAR(results[1].values[3].as_float64(), 0.5, 1e-5);
    op.close();
}

// Btree-prefiltered path (config.prefiltered_rids set): same dot-product
// ordering and _distance semantics as the brute-force path.
TEST_F(QA717NearestScanOperatorTest, PrefilteredDotRanksMostSimilarFirst) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    auto rid1 = insert_row(heap, 1, "aligned", {1.0F, 0.0F, 0.0F});
    auto rid2 = insert_row(heap, 2, "partial", {0.5F, 0.5F, 0.0F});
    auto rid3 = insert_row(heap, 3, "orthogonal", {0.0F, 1.0F, 0.0F});
    auto rid4 = insert_row(heap, 4, "opposite", {-1.0F, 0.0F, 0.0F});

    NearestScanConfig config;
    config.k = 3;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::INNER_PRODUCT;
    config.embedding_column_index = 2;
    config.prefiltered_rids = {rid1, rid2, rid3, rid4};

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
    EXPECT_NEAR(results[0].values[3].as_float64(), 1.0, 1e-5);
    EXPECT_NEAR(results[1].values[3].as_float64(), 0.5, 1e-5);
    EXPECT_NEAR(results[2].values[3].as_float64(), 0.0, 1e-5);
    op.close();
}

// Prefiltered path with a subset of RIDs: only the prefiltered rows are
// eligible, and the best dot product among them wins.
TEST_F(QA717NearestScanOperatorTest, PrefilteredSubsetExcludesBestGlobalRow) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    insert_row(heap, 1, "aligned", {1.0F, 0.0F, 0.0F});
    auto rid2 = insert_row(heap, 2, "partial", {0.5F, 0.5F, 0.0F});
    auto rid3 = insert_row(heap, 3, "orthogonal", {0.0F, 1.0F, 0.0F});

    NearestScanConfig config;
    config.k = 1;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::INNER_PRODUCT;
    config.embedding_column_index = 2;
    // Row 1 (the globally best dot) is NOT in the prefiltered set.
    config.prefiltered_rids = {rid2, rid3};

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].values[0].as_int32(), 2);
    EXPECT_NEAR(results[0].values[3].as_float64(), 0.5, 1e-5);
    op.close();
}

// Guard: L2 at the operator level is untouched by the dot-product fix.
TEST_F(QA717NearestScanOperatorTest, BruteForceL2Unchanged) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    insert_row(heap, 1, "far", {3.0F, 0.0F, 0.0F});
    insert_row(heap, 2, "near", {1.1F, 0.0F, 0.0F});
    insert_row(heap, 3, "mid", {2.0F, 0.0F, 0.0F});

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
    EXPECT_EQ(results[0].values[0].as_int32(), 2);
    EXPECT_EQ(results[1].values[0].as_int32(), 3);
    EXPECT_EQ(results[2].values[0].as_int32(), 1);
    // L2 _distance is emitted as-is (squared distance, lower = nearer).
    EXPECT_NEAR(results[0].values[3].as_float64(), 0.01, 1e-4);
    op.close();
}
