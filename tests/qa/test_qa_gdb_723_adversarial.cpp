/// @file test_qa_gdb_723_adversarial.cpp
/// @brief Adversarial QA tests for GDB-723: HNSW distance-metric correctness.
///
/// These tests probe edge cases, parity at scale, persistence round-trips,
/// metric normalization, and mutation tripwires beyond the baseline tests in
/// test_qa_gdb_723.cpp.

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
#include "sixseven/vector/hnsw_page.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Shared fixture
// ---------------------------------------------------------------------------

class QA723Adversarial : public ::testing::Test {
protected:
    void SetUp() override {
        // Unique paths per test instance
        std::string suffix = std::to_string(reinterpret_cast<uintptr_t>(this));
        table_path_ = std::filesystem::temp_directory_path() /
                      ("sixseven_qa723adv_table_" + suffix + ".db");
        hnsw_path_ = std::filesystem::temp_directory_path() /
                     ("sixseven_qa723adv_hnsw_" + suffix + ".db");

        std::filesystem::remove(table_path_);
        std::filesystem::remove(hnsw_path_);

        auto tfid = dm_.create_file(table_path_, false, true);
        ASSERT_TRUE(tfid.has_value()) << tfid.error().message;
        table_fid_ = *tfid;
        table_bpm_ = std::make_unique<BufferPoolManager>(dm_, table_fid_, 256);

        auto hfid = dm_.create_file(hnsw_path_, false, true);
        ASSERT_TRUE(hfid.has_value()) << hfid.error().message;
        hnsw_fid_ = *hfid;
        hnsw_bpm_ = std::make_unique<BufferPoolManager>(dm_, hnsw_fid_, 512);

        storage_schema_ = Schema({
            {"id", TypeId::INT32},
            {"embedding", TypeId::EMBEDDING},
        });

        output_cols_ = {
            {"", "id", TypeId::INT32, false, 0},
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

    void reset_storage() {
        hnsw_bpm_.reset();
        table_bpm_.reset();
        (void)dm_.close_file(table_fid_);
        (void)dm_.close_file(hnsw_fid_);
        std::filesystem::remove(table_path_);
        std::filesystem::remove(hnsw_path_);

        auto tfid = dm_.create_file(table_path_, false, true);
        ASSERT_TRUE(tfid.has_value());
        table_fid_ = *tfid;
        table_bpm_ = std::make_unique<BufferPoolManager>(dm_, table_fid_, 256);

        auto hfid = dm_.create_file(hnsw_path_, false, true);
        ASSERT_TRUE(hfid.has_value());
        hnsw_fid_ = *hfid;
        hnsw_bpm_ = std::make_unique<BufferPoolManager>(dm_, hnsw_fid_, 512);
    }

    void insert_row(TableHeap& heap, int32_t id, const Embedding& emb) {
        std::vector<Value> vals = {Value(id), Value(emb)};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        ASSERT_TRUE(bytes.has_value()) << bytes.error().message;
        auto rid = heap.insert_tuple(*bytes);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    HnswIndexConfig make_config(uint32_t dim, DistanceMetric metric,
                                uint16_t m = 8, uint16_t efc = 50, uint16_t efs = 50) {
        HnswIndexConfig c;
        c.dimension = dim;
        c.metric = metric;
        c.m = m;
        c.ef_construction = efc;
        c.ef_search = efs;
        return c;
    }

    std::vector<Tuple> run_nearest(TableHeap& heap, DistanceMetric metric,
                                   HnswIndex* hnsw, uint32_t k,
                                   const std::vector<float>& query) {
        NearestScanConfig config;
        config.k = k;
        config.query_vector = query;
        config.metric = metric;
        config.embedding_column_index = 1; // 2-column schema: id, embedding

        OutputSchema schema(output_cols_);
        BoundStatement bound;
        NearestScanOperator op(
            heap, storage_schema_, std::move(config), std::move(schema),
            nullptr, bound, hnsw);
        auto open_res = op.open();
        EXPECT_TRUE(open_res.has_value()) << open_res.error().message;

        std::vector<Tuple> results;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row->has_value()) {
                break;
            }
            results.push_back(std::move(row->value()));
        }
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

// ---------------------------------------------------------------------------
// Test 1: Parity stress — 50 vectors, mixed magnitudes, top-3 ordering
// ---------------------------------------------------------------------------
// Verify HNSW and brute-force agree on top-3 ids+distances for a larger
// dataset under all three sort metrics. High ef_search keeps HNSW exact at
// this scale; only top-3 is asserted for resilience to approximation.
TEST_F(QA723Adversarial, ParityStress50VectorsMixedMagnitudes) {
    const uint32_t DIM = 4;
    const uint32_t N = 50;

    // Vectors with varied magnitudes: mix tiny, unit, and large components
    std::vector<Embedding> vecs(N);
    for (uint32_t i = 0; i < N; ++i) {
        float scale = (i % 7 == 0) ? 0.001F : (i % 5 == 0) ? 10.0F : 1.0F;
        vecs[i] = Embedding{
            scale * std::cos(static_cast<float>(i)),
            scale * std::sin(static_cast<float>(i)),
            scale * (i % 3 == 0 ? -1.0F : 1.0F),
            scale * (static_cast<float>(i) / static_cast<float>(N)),
        };
    }

    const std::vector<float> query = {1.0F, 0.0F, 0.0F, 0.0F};

    const DistanceMetric metrics[] = {
        DistanceMetric::L2,
        DistanceMetric::COSINE,
        DistanceMetric::INNER_PRODUCT,
    };

    for (auto metric : metrics) {
        reset_storage();

        TableHeap heap(*table_bpm_, dm_, table_fid_);
        HnswIndex hnsw(*hnsw_bpm_, nullptr);
        ASSERT_TRUE(hnsw.create(make_config(DIM, metric, 16, 200, 200)).has_value());

        for (uint32_t i = 0; i < N; ++i) {
            insert_row(heap, static_cast<int32_t>(i + 1), vecs[i]);
            Embedding v = vecs[i];
            ASSERT_TRUE(hnsw.insert(v).has_value());
        }

        auto via_hnsw = run_nearest(heap, metric, &hnsw, 3, query);
        auto via_brute = run_nearest(heap, metric, nullptr, 3, query);

        ASSERT_EQ(via_hnsw.size(), 3u) << "metric=" << distance_metric_name(metric);
        ASSERT_EQ(via_brute.size(), 3u) << "metric=" << distance_metric_name(metric);

        // Top-3 ordering and distance parity
        for (size_t r = 0; r < 3; ++r) {
            EXPECT_EQ(via_hnsw[r].values[0].as_int32(), via_brute[r].values[0].as_int32())
                << "metric=" << distance_metric_name(metric) << " rank=" << r;
            EXPECT_NEAR(
                via_hnsw[r].values[2].as_float64(),
                via_brute[r].values[2].as_float64(),
                1e-4)
                << "metric=" << distance_metric_name(metric) << " rank=" << r;
        }
    }
}

// ---------------------------------------------------------------------------
// Test 2: Zero vector — no crash, deterministic result
// ---------------------------------------------------------------------------
// Cosine distance with a zero vector is undefined (division by zero in
// normalization). The implementation must not crash and must return a
// deterministic result on repeated queries.
TEST_F(QA723Adversarial, ZeroVectorNoCrashDeterministic) {
    const uint32_t DIM = 3;

    TableHeap heap(*table_bpm_, dm_, table_fid_);
    HnswIndex hnsw(*hnsw_bpm_, nullptr);
    ASSERT_TRUE(hnsw.create(make_config(DIM, DistanceMetric::COSINE)).has_value());

    Embedding zero_emb = {0.0F, 0.0F, 0.0F};
    Embedding e1 = {1.0F, 0.0F, 0.0F};
    Embedding e2 = {0.0F, 1.0F, 0.0F};

    insert_row(heap, 0, zero_emb);
    insert_row(heap, 1, e1);
    insert_row(heap, 2, e2);

    ASSERT_TRUE(hnsw.insert(zero_emb).has_value());
    ASSERT_TRUE(hnsw.insert(e1).has_value());
    ASSERT_TRUE(hnsw.insert(e2).has_value());

    // Query with zero vector — must not crash
    std::vector<float> zero_query = {0.0F, 0.0F, 0.0F};
    auto r1 = run_nearest(heap, DistanceMetric::COSINE, &hnsw, 3, zero_query);
    auto r2 = run_nearest(heap, DistanceMetric::COSINE, &hnsw, 3, zero_query);

    // Must return something (not crash)
    ASSERT_GE(r1.size(), 1u) << "zero-vector query returned nothing";

    // Must be deterministic: same ordering both times
    ASSERT_EQ(r1.size(), r2.size());
    for (size_t i = 0; i < r1.size(); ++i) {
        EXPECT_EQ(r1[i].values[0].as_int32(), r2[i].values[0].as_int32())
            << "non-deterministic result at rank " << i;
    }
}

// ---------------------------------------------------------------------------
// Test 3: Duplicate vectors — no crash, correct distances
// ---------------------------------------------------------------------------
TEST_F(QA723Adversarial, DuplicateVectorsNoCrash) {
    const uint32_t DIM = 2;
    Embedding dup = {0.6F, 0.8F}; // unit vector, cosine dist from [1,0] = 0.4

    TableHeap heap(*table_bpm_, dm_, table_fid_);
    HnswIndex hnsw(*hnsw_bpm_, nullptr);
    ASSERT_TRUE(hnsw.create(make_config(DIM, DistanceMetric::COSINE)).has_value());

    for (int i = 0; i < 5; ++i) {
        insert_row(heap, i, dup);
        ASSERT_TRUE(hnsw.insert(dup).has_value());
    }

    std::vector<float> query = {1.0F, 0.0F};
    auto results = run_nearest(heap, DistanceMetric::COSINE, &hnsw, 5, query);

    // Must not crash and must return rows
    ASSERT_GE(results.size(), 1u);

    // All returned distances should be cosine distance from [1,0] to [0.6,0.8]
    // = 1 - 0.6 = 0.4
    for (const auto& row : results) {
        EXPECT_NEAR(row.values[2].as_float64(), 0.4, 1e-5)
            << "unexpected distance for duplicate vector";
    }
}

// ---------------------------------------------------------------------------
// Test 4: Single vector — exact match with cosine distance ~0
// ---------------------------------------------------------------------------
TEST_F(QA723Adversarial, SingleVectorExactMatch) {
    const uint32_t DIM = 3;
    Embedding stored = {0.5F, 0.5F, 0.707107F};

    TableHeap heap(*table_bpm_, dm_, table_fid_);
    HnswIndex hnsw(*hnsw_bpm_, nullptr);
    ASSERT_TRUE(hnsw.create(make_config(DIM, DistanceMetric::COSINE)).has_value());

    insert_row(heap, 42, stored);
    ASSERT_TRUE(hnsw.insert(stored).has_value());

    std::vector<float> query(stored.begin(), stored.end());
    auto results = run_nearest(heap, DistanceMetric::COSINE, &hnsw, 1, query);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].values[0].as_int32(), 42);
    EXPECT_NEAR(results[0].values[2].as_float64(), 0.0, 1e-5)
        << "exact match should have cosine distance ~0";
}

// ---------------------------------------------------------------------------
// Test 5: Query equals stored vector — L2 distance = 0
// ---------------------------------------------------------------------------
TEST_F(QA723Adversarial, QueryEqualsStoredVectorL2IsZero) {
    const uint32_t DIM = 4;
    Embedding stored = {3.0F, -1.5F, 0.0F, 2.0F};
    Embedding other = {1.0F, 2.0F, 3.0F, 4.0F};

    TableHeap heap(*table_bpm_, dm_, table_fid_);
    HnswIndex hnsw(*hnsw_bpm_, nullptr);
    ASSERT_TRUE(hnsw.create(make_config(DIM, DistanceMetric::L2)).has_value());

    insert_row(heap, 7, stored);
    insert_row(heap, 8, other);
    ASSERT_TRUE(hnsw.insert(stored).has_value());
    ASSERT_TRUE(hnsw.insert(other).has_value());

    std::vector<float> query(stored.begin(), stored.end());
    auto results = run_nearest(heap, DistanceMetric::L2, &hnsw, 1, query);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].values[0].as_int32(), 7);
    EXPECT_NEAR(results[0].values[2].as_float64(), 0.0, 1e-5)
        << "identical vector should have L2 distance = 0";
}

// ---------------------------------------------------------------------------
// Test 6: Persistence round-trip — metric byte survives serialize/deserialize
// ---------------------------------------------------------------------------
TEST_F(QA723Adversarial, PersistenceRoundTripMetricSurvives) {
    const DistanceMetric test_metrics[] = {
        DistanceMetric::L2,
        DistanceMetric::COSINE,
        DistanceMetric::INNER_PRODUCT,
        DistanceMetric::DOT_PRODUCT,
    };

    for (auto m : test_metrics) {
        HnswMeta meta;
        meta.entry_point_id = 0;
        meta.max_layer = 0;
        meta.m_param = 16;
        meta.ef_construction = 200;
        meta.ef_search = 64;
        meta.dimension = 3;
        meta.node_count = 1;
        meta.tombstone_count = 0;
        meta.next_node_id = 1;
        meta.metric = m;

        auto bytes = serialize_hnsw_meta(meta);
        auto result = deserialize_hnsw_meta(bytes);
        ASSERT_TRUE(result.has_value())
            << "deserialize failed for metric=" << distance_metric_name(m)
            << ": " << (result.has_value() ? "" : result.error().message);
        EXPECT_EQ(result->metric, m)
            << "metric not preserved: " << distance_metric_name(m);
    }
}

// ---------------------------------------------------------------------------
// Test 7: Legacy (no metric byte) deserialization defaults to L2
// ---------------------------------------------------------------------------
TEST_F(QA723Adversarial, LegacyMetaMissingByteParsesAsL2) {
    HnswMeta meta;
    meta.entry_point_id = 0xFFFFFFFF;
    meta.max_layer = 0;
    meta.m_param = 16;
    meta.ef_construction = 200;
    meta.ef_search = 64;
    meta.dimension = 4;
    meta.node_count = 0;
    meta.tombstone_count = 0;
    meta.next_node_id = 0;
    meta.metric = DistanceMetric::L2;

    auto full_bytes = serialize_hnsw_meta(meta);

    // Strip the trailing metric byte to simulate legacy format
    ASSERT_GE(full_bytes.size(), 1u);
    std::vector<uint8_t> legacy_bytes(full_bytes.begin(), full_bytes.end() - 1);

    auto result = deserialize_hnsw_meta(legacy_bytes);
    ASSERT_TRUE(result.has_value()) << "legacy meta should parse without metric byte";
    EXPECT_EQ(result->metric, DistanceMetric::L2)
        << "legacy meta without metric byte must default to L2";
}

// ---------------------------------------------------------------------------
// Test 8: Invalid metric byte returns INVALID_ARGUMENT
// ---------------------------------------------------------------------------
TEST_F(QA723Adversarial, InvalidMetricByteReturnsError) {
    HnswMeta meta;
    meta.entry_point_id = 0xFFFFFFFF;
    meta.max_layer = 0;
    meta.m_param = 16;
    meta.ef_construction = 200;
    meta.ef_search = 64;
    meta.dimension = 4;
    meta.node_count = 0;
    meta.tombstone_count = 0;
    meta.next_node_id = 0;
    meta.metric = DistanceMetric::L2;

    auto bytes = serialize_hnsw_meta(meta);
    // Overwrite the last byte with an out-of-range metric value
    bytes.back() = 99;

    auto result = deserialize_hnsw_meta(bytes);
    ASSERT_FALSE(result.has_value()) << "invalid metric byte should return error";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Test 9: DOT_PRODUCT config normalizes to INNER_PRODUCT in the index
// ---------------------------------------------------------------------------
TEST_F(QA723Adversarial, DotProductConfigNormalizesToInnerProduct) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    HnswIndex hnsw(*hnsw_bpm_, nullptr);

    ASSERT_TRUE(hnsw.create(make_config(3, DistanceMetric::DOT_PRODUCT)).has_value());

    // After create(), metric() must report INNER_PRODUCT, not DOT_PRODUCT
    EXPECT_EQ(hnsw.metric(), DistanceMetric::INNER_PRODUCT)
        << "DOT_PRODUCT must be normalized to INNER_PRODUCT in the index";

    // Results should match brute-force INNER_PRODUCT search
    Embedding e1 = {5.0F, 0.0F, 0.0F};
    Embedding e2 = {0.9F, 0.1F, 0.0F};
    Embedding e3 = {0.0F, 1.0F, 0.0F};
    insert_row(heap, 1, e1);
    insert_row(heap, 2, e2);
    insert_row(heap, 3, e3);
    ASSERT_TRUE(hnsw.insert(e1).has_value());
    ASSERT_TRUE(hnsw.insert(e2).has_value());
    ASSERT_TRUE(hnsw.insert(e3).has_value());

    std::vector<float> query = {1.0F, 0.0F, 0.0F};
    auto via_hnsw = run_nearest(heap, DistanceMetric::INNER_PRODUCT, &hnsw, 3, query);
    auto via_brute = run_nearest(heap, DistanceMetric::INNER_PRODUCT, nullptr, 3, query);

    ASSERT_EQ(via_hnsw.size(), via_brute.size());
    for (size_t i = 0; i < via_hnsw.size(); ++i) {
        EXPECT_EQ(via_hnsw[i].values[0].as_int32(), via_brute[i].values[0].as_int32())
            << "rank=" << i;
        EXPECT_NEAR(
            via_hnsw[i].values[2].as_float64(),
            via_brute[i].values[2].as_float64(),
            1e-5)
            << "rank=" << i;
    }
}

// ---------------------------------------------------------------------------
// Test 10: reset() preserves metric — REEMBED scenario
// ---------------------------------------------------------------------------
TEST_F(QA723Adversarial, ResetPreservesMetric) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    HnswIndex hnsw(*hnsw_bpm_, nullptr);

    ASSERT_TRUE(hnsw.create(make_config(3, DistanceMetric::COSINE)).has_value());
    EXPECT_EQ(hnsw.metric(), DistanceMetric::COSINE);

    Embedding e1 = {1.0F, 0.0F, 0.0F};
    Embedding e2 = {0.0F, 1.0F, 0.0F};
    insert_row(heap, 1, e1);
    insert_row(heap, 2, e2);
    ASSERT_TRUE(hnsw.insert(e1).has_value());
    ASSERT_TRUE(hnsw.insert(e2).has_value());

    ASSERT_TRUE(hnsw.reset().has_value());

    // Metric must still be COSINE after reset
    EXPECT_EQ(hnsw.metric(), DistanceMetric::COSINE)
        << "reset() must preserve the distance metric";

    // Re-insert and verify results are still COSINE-ordered
    ASSERT_TRUE(hnsw.insert(e1).has_value());
    ASSERT_TRUE(hnsw.insert(e2).has_value());

    std::vector<float> query = {1.0F, 0.0F, 0.0F};
    auto via_hnsw = run_nearest(heap, DistanceMetric::COSINE, &hnsw, 2, query);
    auto via_brute = run_nearest(heap, DistanceMetric::COSINE, nullptr, 2, query);

    ASSERT_EQ(via_hnsw.size(), 2u);
    ASSERT_EQ(via_brute.size(), 2u);
    EXPECT_EQ(via_hnsw[0].values[0].as_int32(), via_brute[0].values[0].as_int32())
        << "post-reset re-insert must search with preserved COSINE metric";
    EXPECT_NEAR(via_hnsw[0].values[2].as_float64(), via_brute[0].values[2].as_float64(), 1e-5);
}

// ---------------------------------------------------------------------------
// Test 11: Metric mismatch fallback produces exact brute-force distances
// ---------------------------------------------------------------------------
// When HNSW was built with L2 but the query uses COSINE, the fallback path
// must produce distances identical to the no-index brute-force path to
// floating-point precision.
TEST_F(QA723Adversarial, MetricMismatchFallbackExactDistances) {
    const uint32_t DIM = 3;

    // Non-unit-norm vectors where L2 and cosine orderings genuinely differ
    Embedding v1 = {10.0F, 0.0F, 0.0F};   // huge magnitude, perfectly aligned
    Embedding v2 = {0.5F, 0.5F, 0.0F};    // moderate, 45 deg
    Embedding v3 = {0.0F, 0.0F, -1.0F};   // perpendicular, negative

    TableHeap heap(*table_bpm_, dm_, table_fid_);
    HnswIndex hnsw(*hnsw_bpm_, nullptr);
    ASSERT_TRUE(hnsw.create(make_config(DIM, DistanceMetric::L2)).has_value());

    insert_row(heap, 1, v1);
    insert_row(heap, 2, v2);
    insert_row(heap, 3, v3);
    ASSERT_TRUE(hnsw.insert(v1).has_value());
    ASSERT_TRUE(hnsw.insert(v2).has_value());
    ASSERT_TRUE(hnsw.insert(v3).has_value());

    std::vector<float> query = {1.0F, 0.0F, 0.0F};
    auto with_mismatch = run_nearest(heap, DistanceMetric::COSINE, &hnsw, 3, query);
    auto no_index = run_nearest(heap, DistanceMetric::COSINE, nullptr, 3, query);

    ASSERT_EQ(with_mismatch.size(), no_index.size());
    for (size_t i = 0; i < with_mismatch.size(); ++i) {
        EXPECT_EQ(with_mismatch[i].values[0].as_int32(), no_index[i].values[0].as_int32())
            << "rank=" << i << " ordering differs";
        // Exact floating-point match since both paths compute identically
        EXPECT_DOUBLE_EQ(
            with_mismatch[i].values[2].as_float64(),
            no_index[i].values[2].as_float64())
            << "rank=" << i << " distance differs";
    }
}

// ---------------------------------------------------------------------------
// Test 12: display_distance for DOT/INNER_PRODUCT returns raw positive dot
// ---------------------------------------------------------------------------
// Verifies _distance column for DOT queries is the RAW dot product (positive
// = high similarity), not the negated sort key used internally.
TEST_F(QA723Adversarial, DisplayDistanceDotReturnsRawPositiveDot) {
    const uint32_t DIM = 2;

    // id 1: [3, 0] -> dot with [1, 0] = 3.0 (highest similarity, first)
    // id 2: [1, 0] -> dot with [1, 0] = 1.0
    Embedding e1 = {3.0F, 0.0F};
    Embedding e2 = {1.0F, 0.0F};

    TableHeap heap(*table_bpm_, dm_, table_fid_);
    HnswIndex hnsw(*hnsw_bpm_, nullptr);
    ASSERT_TRUE(hnsw.create(make_config(DIM, DistanceMetric::DOT_PRODUCT)).has_value());

    insert_row(heap, 1, e1);
    insert_row(heap, 2, e2);
    ASSERT_TRUE(hnsw.insert(e1).has_value());
    ASSERT_TRUE(hnsw.insert(e2).has_value());

    std::vector<float> query = {1.0F, 0.0F};

    auto via_hnsw = run_nearest(heap, DistanceMetric::DOT_PRODUCT, &hnsw, 2, query);
    auto via_brute = run_nearest(heap, DistanceMetric::DOT_PRODUCT, nullptr, 2, query);

    ASSERT_EQ(via_hnsw.size(), 2u);
    ASSERT_EQ(via_brute.size(), 2u);

    // Most similar first: id 1 (dot=3.0) then id 2 (dot=1.0)
    EXPECT_EQ(via_hnsw[0].values[0].as_int32(), 1)
        << "HNSW: highest dot product must appear first";
    EXPECT_EQ(via_hnsw[1].values[0].as_int32(), 2);

    // _distance must be raw dot product (positive), not the negated sort key
    EXPECT_NEAR(via_hnsw[0].values[2].as_float64(), 3.0, 1e-5)
        << "HNSW path: _distance for DOT must be raw dot product";
    EXPECT_NEAR(via_hnsw[1].values[2].as_float64(), 1.0, 1e-5)
        << "HNSW path: _distance for DOT must be raw dot product";

    // HNSW and brute-force must agree on both ordering and distance
    for (size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(via_hnsw[i].values[0].as_int32(), via_brute[i].values[0].as_int32())
            << "rank=" << i;
        EXPECT_NEAR(via_hnsw[i].values[2].as_float64(), via_brute[i].values[2].as_float64(), 1e-5)
            << "rank=" << i;
    }
}
