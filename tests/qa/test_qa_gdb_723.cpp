/// @file test_qa_gdb_723.cpp
/// @brief QA regression tests for GDB-723: HNSW index hardcoded L2 distance,
/// ignoring the query's requested metric (COSINE/DOT).
///
/// Pre-fix, every distance computation inside HnswIndex (greedy_search_layer,
/// search_layer, and the insert-time neighbor selection that reuses them) was
/// hardcoded L2, and NearestScanOperator::execute_hnsw_search passed the raw
/// index distance through as _distance. Since an HNSW index is auto-created
/// for every EMBEDDING column and preferred whenever it has nodes, the
/// mainline default-COSINE NEAREST path silently returned L2 rankings and L2
/// _distance values; non-unit-norm embeddings ranked wrong.
///
/// Post-fix contract (verified here):
///  - HnswIndexConfig carries a DistanceMetric used for BOTH graph
///    construction and search; DOT_PRODUCT normalizes to INNER_PRODUCT.
///  - The HNSW path and the brute-force path produce IDENTICAL ordering and
///    IDENTICAL _distance values for the same data under all three metrics
///    (datasets are small, so HNSW is exact).
///  - When the query metric differs from the index metric (e.g. a legacy
///    L2-built index serving a COSINE query), the operator falls back to the
///    exact brute-force scan.

#include "sixseven/common/value.h"
#include "sixseven/executor/nearest_scan.h"
#include "sixseven/executor/tuple.h"
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

class QA723NearestScanTest : public ::testing::Test {
protected:
    void SetUp() override {
        table_path_ = std::filesystem::temp_directory_path() / "sixseven_qa723_table.db";
        std::filesystem::remove(table_path_);
        auto fid = dm_.create_file(table_path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        table_fid_ = *fid;
        table_bpm_ = std::make_unique<BufferPoolManager>(dm_, table_fid_, 64);

        hnsw_path_ = std::filesystem::temp_directory_path() / "sixseven_qa723_hnsw.db";
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

    void insert_row(TableHeap& heap, int32_t id, const std::string& name, const Embedding& emb) {
        std::vector<Value> vals = {Value(id), Value(name), Value(emb)};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        ASSERT_TRUE(bytes.has_value()) << bytes.error().message;
        auto rid = heap.insert_tuple(*bytes);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
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

    std::vector<Tuple> run_nearest(TableHeap& heap, DistanceMetric metric, HnswIndex* hnsw) {
        NearestScanConfig config;
        config.k = 4;
        config.query_vector = {1.0F, 0.0F, 0.0F};
        config.metric = metric;
        config.embedding_column_index = 2;

        OutputSchema schema(output_cols_);
        BoundStatement bound;
        NearestScanOperator op(
            heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound, hnsw);
        EXPECT_TRUE(op.open().has_value());
        auto results = drain(op);
        op.close();
        return results;
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

namespace {

/// Non-unit-norm dataset where L2, COSINE, and DOT orderings all differ for
/// query [1,0,0]:
///   id 1 [0.9, 0.1, 0]: L2 0.02 (nearest), cos ~0.0062, dot 0.9
///   id 2 [5.0, 0.0, 0]: L2 16 (farthest+), cos 0 (best), dot 5.0 (best)
///   id 3 [0.0, 1.0, 0]: L2 2, cos 1, dot 0
///   id 4 [-2.0, 0.0, 0]: L2 9, cos 2 (worst), dot -2 (worst)
const std::vector<Embedding> qa723_embs = {
    {0.9F, 0.1F, 0.0F},
    {5.0F, 0.0F, 0.0F},
    {0.0F, 1.0F, 0.0F},
    {-2.0F, 0.0F, 0.0F},
};

} // anonymous namespace

// Regression repro (audit C2): a default-metric (COSINE) NEAREST query served
// by an HNSW index must return COSINE ordering with cosine-distance values —
// pre-fix it returned L2 ordering (1, 3, 4, 2) with L2-squared distances.
TEST_F(QA723NearestScanTest, CosineQueryOnHnswReturnsCosineOrdering) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    HnswIndex hnsw(*hnsw_bpm_);

    HnswIndexConfig hnsw_config;
    hnsw_config.dimension = 3;
    hnsw_config.m = 8;
    hnsw_config.ef_construction = 50;
    hnsw_config.ef_search = 50;
    hnsw_config.metric = DistanceMetric::COSINE; // production auto-index metric
    ASSERT_TRUE(hnsw.create(hnsw_config).has_value());

    for (size_t i = 0; i < qa723_embs.size(); ++i) {
        insert_row(heap, static_cast<int32_t>(i + 1), "r", qa723_embs[i]);
        ASSERT_TRUE(hnsw.insert(qa723_embs[i]).has_value());
    }

    auto results = run_nearest(heap, DistanceMetric::COSINE, &hnsw);
    ASSERT_EQ(results.size(), 4u);

    // COSINE order: 2 (0) < 1 (~0.0062) < 3 (1) < 4 (2).
    EXPECT_EQ(results[0].values[0].as_int32(), 2);
    EXPECT_EQ(results[1].values[0].as_int32(), 1);
    EXPECT_EQ(results[2].values[0].as_int32(), 3);
    EXPECT_EQ(results[3].values[0].as_int32(), 4);

    // _distance is cosine distance, not L2-squared.
    EXPECT_NEAR(results[0].values[3].as_float64(), 0.0, 1e-5);
    EXPECT_NEAR(results[2].values[3].as_float64(), 1.0, 1e-5);
    EXPECT_NEAR(results[3].values[3].as_float64(), 2.0, 1e-5);
}

// Gold assertion: HNSW path and brute-force path produce identical ordering
// AND identical _distance values under all three metrics.
TEST_F(QA723NearestScanTest, HnswMatchesBruteForceForAllMetrics) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    for (size_t i = 0; i < qa723_embs.size(); ++i) {
        insert_row(heap, static_cast<int32_t>(i + 1), "r", qa723_embs[i]);
    }

    const DistanceMetric metrics[] = {
        DistanceMetric::L2,
        DistanceMetric::COSINE,
        DistanceMetric::INNER_PRODUCT, // what the planner emits for USING DOT
    };

    for (auto metric : metrics) {
        HnswIndex hnsw(*hnsw_bpm_);
        HnswIndexConfig hnsw_config;
        hnsw_config.dimension = 3;
        hnsw_config.m = 8;
        hnsw_config.ef_construction = 50;
        hnsw_config.ef_search = 50;
        hnsw_config.metric = metric;
        ASSERT_TRUE(hnsw.create(hnsw_config).has_value());
        for (const auto& e : qa723_embs) {
            ASSERT_TRUE(hnsw.insert(e).has_value());
        }

        auto via_hnsw = run_nearest(heap, metric, &hnsw);
        auto via_brute = run_nearest(heap, metric, nullptr);

        ASSERT_EQ(via_hnsw.size(), via_brute.size()) << distance_metric_name(metric);
        for (size_t i = 0; i < via_hnsw.size(); ++i) {
            EXPECT_EQ(via_hnsw[i].values[0].as_int32(), via_brute[i].values[0].as_int32())
                << "metric=" << distance_metric_name(metric) << " rank=" << i;
            EXPECT_NEAR(
                via_hnsw[i].values[3].as_float64(), via_brute[i].values[3].as_float64(), 1e-5)
                << "metric=" << distance_metric_name(metric) << " rank=" << i;
        }
    }
}

// DOT semantics on the HNSW path follow the GDB-717 conventions: rows emerge
// most-similar-first with _distance equal to the RAW dot product, descending.
TEST_F(QA723NearestScanTest, DotQueryOnHnswEmitsRawDotDescending) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    HnswIndex hnsw(*hnsw_bpm_);

    HnswIndexConfig hnsw_config;
    hnsw_config.dimension = 3;
    hnsw_config.m = 8;
    hnsw_config.ef_construction = 50;
    hnsw_config.ef_search = 50;
    hnsw_config.metric = DistanceMetric::DOT_PRODUCT; // normalized to INNER_PRODUCT
    ASSERT_TRUE(hnsw.create(hnsw_config).has_value());
    ASSERT_EQ(hnsw.metric(), DistanceMetric::INNER_PRODUCT);

    for (size_t i = 0; i < qa723_embs.size(); ++i) {
        insert_row(heap, static_cast<int32_t>(i + 1), "r", qa723_embs[i]);
        ASSERT_TRUE(hnsw.insert(qa723_embs[i]).has_value());
    }

    auto results = run_nearest(heap, DistanceMetric::INNER_PRODUCT, &hnsw);
    ASSERT_EQ(results.size(), 4u);

    // Dot order: 2 (5.0) > 1 (0.9) > 3 (0.0) > 4 (-2.0).
    EXPECT_EQ(results[0].values[0].as_int32(), 2);
    EXPECT_EQ(results[1].values[0].as_int32(), 1);
    EXPECT_EQ(results[2].values[0].as_int32(), 3);
    EXPECT_EQ(results[3].values[0].as_int32(), 4);

    EXPECT_NEAR(results[0].values[3].as_float64(), 5.0, 1e-5);
    EXPECT_NEAR(results[1].values[3].as_float64(), 0.9, 1e-5);
    EXPECT_NEAR(results[2].values[3].as_float64(), 0.0, 1e-5);
    EXPECT_NEAR(results[3].values[3].as_float64(), -2.0, 1e-5);
}

// Mismatch handling: a legacy L2-built index serving a COSINE query must NOT
// be used — the operator falls back to the exact brute-force scan and the
// results match the no-index path exactly.
TEST_F(QA723NearestScanTest, LegacyL2IndexCosineQueryFallsBackToBruteForce) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    HnswIndex hnsw(*hnsw_bpm_);

    HnswIndexConfig hnsw_config;
    hnsw_config.dimension = 3;
    hnsw_config.m = 8;
    hnsw_config.ef_construction = 50;
    hnsw_config.ef_search = 50;
    // Default metric is L2 — exactly what legacy persisted indexes load as.
    ASSERT_TRUE(hnsw.create(hnsw_config).has_value());
    ASSERT_EQ(hnsw.metric(), DistanceMetric::L2);

    for (size_t i = 0; i < qa723_embs.size(); ++i) {
        insert_row(heap, static_cast<int32_t>(i + 1), "r", qa723_embs[i]);
        ASSERT_TRUE(hnsw.insert(qa723_embs[i]).has_value());
    }

    auto with_index = run_nearest(heap, DistanceMetric::COSINE, &hnsw);
    auto no_index = run_nearest(heap, DistanceMetric::COSINE, nullptr);

    ASSERT_EQ(with_index.size(), 4u);
    ASSERT_EQ(no_index.size(), 4u);
    for (size_t i = 0; i < with_index.size(); ++i) {
        EXPECT_EQ(with_index[i].values[0].as_int32(), no_index[i].values[0].as_int32())
            << "rank=" << i;
        EXPECT_NEAR(with_index[i].values[3].as_float64(), no_index[i].values[3].as_float64(), 1e-9)
            << "rank=" << i;
    }
    // And the ordering is cosine, not L2 (id=2 first, not id=1).
    EXPECT_EQ(with_index[0].values[0].as_int32(), 2);
}
