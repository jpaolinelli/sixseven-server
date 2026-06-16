// QA adversarial tests for GDB-847
//
// Ticket: Fix PrefilteredEmptyRIDs — the old test exercised a non-empty set
// with an invalid RID, not the documented empty-set fall-through semantics.
// The implementer renamed the test and added PrefilteredEmptyVectorFallsThroughToScan.
//
// This file probes every edge case of the prefiltered_rids dispatch path:
//   - Empty set    → brute-force fall-through (must return rows)
//   - Non-empty set → prefiltered path (must restrict to those RIDs only)
//
// All tests use the NearestScanTest fixture from tests/unit/test_nearest_scan.cpp
// via a shared base. To avoid duplicating the fixture, we replicate it here
// with the identical setup — both targets include different .cpp files.

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
// QA fixture — mirrors NearestScanTest from tests/unit/test_nearest_scan.cpp
// =============================================================================

class QA_GDB847_NearestScan : public ::testing::Test {
protected:
    void SetUp() override {
        table_path_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb847_table.db";
        std::filesystem::remove(table_path_);

        auto fid = dm_.create_file(table_path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        table_file_id_ = *fid;

        table_bpm_ = std::make_unique<BufferPoolManager>(dm_, table_file_id_, 64);

        hnsw_path_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb847_hnsw.db";
        std::filesystem::remove(hnsw_path_);

        auto hfid = dm_.create_file(hnsw_path_, false, true);
        ASSERT_TRUE(hfid.has_value()) << hfid.error().message;
        hnsw_file_id_ = *hfid;

        hnsw_bpm_ = std::make_unique<BufferPoolManager>(dm_, hnsw_file_id_, 256);

        // Schema: id (INT32), name (STRING), embedding (EMBEDDING).
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
// GDB847: Empty prefiltered_rids with k > heap size
//
// Mutation target: if empty-set dispatch is broken (treats empty as zero
// candidates) it returns 0 rows instead of all rows.
// =============================================================================

TEST_F(QA_GDB847_NearestScan, GDB847_EmptyPrefilteredKGreaterThanHeap) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    // Insert 2 rows; k=10 > 2.
    insert_row(heap, 1, "alpha", {1.0F, 0.0F, 0.0F});
    insert_row(heap, 2, "beta", {0.0F, 1.0F, 0.0F});

    NearestScanConfig config;
    config.k = 10; // intentionally larger than row count
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.prefiltered_rids = {}; // empty → must fall through

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    // Must return ALL rows (2), not k rows (10) and not 0 rows.
    ASSERT_EQ(results.size(), 2u);
    // Closest to [1,0,0] is alpha (distance 0).
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_NEAR(results[0].values[3].as_float64(), 0.0, 1e-5);
    op.close();
}

// =============================================================================
// GDB847: Empty prefiltered_rids with k=0
//
// k=0 in the fall-through path should return 0 rows (the loop guard fires
// immediately) — not a crash.
// =============================================================================

TEST_F(QA_GDB847_NearestScan, GDB847_EmptyPrefilteredKEqualsZero) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    insert_row(heap, 1, "alpha", {1.0F, 0.0F, 0.0F});
    insert_row(heap, 2, "beta", {0.0F, 1.0F, 0.0F});

    NearestScanConfig config;
    config.k = 0; // zero — must not crash
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.prefiltered_rids = {}; // fall-through

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    EXPECT_EQ(results.size(), 0u); // k=0 → no results; no crash
    op.close();
}

// =============================================================================
// GDB847: Empty prefiltered_rids with k=1
//
// Confirms the boundary at k=1: exactly one result, and it is the closest row.
// =============================================================================

TEST_F(QA_GDB847_NearestScan, GDB847_EmptyPrefilteredKEqualsOne) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    // alpha is unambiguously closest to [1,0,0] under L2.
    insert_row(heap, 1, "alpha", {1.0F, 0.0F, 0.0F}); // distance 0
    insert_row(heap, 2, "beta", {0.0F, 1.0F, 0.0F});  // distance 2
    insert_row(heap, 3, "gamma", {0.0F, 0.0F, 1.0F}); // distance 2

    NearestScanConfig config;
    config.k = 1;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.prefiltered_rids = {}; // fall-through

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].values[0].as_int32(), 1); // alpha — hardcoded
    EXPECT_NEAR(results[0].values[3].as_float64(), 0.0, 1e-5);
    op.close();
}

// =============================================================================
// GDB847: Non-empty prefiltered_rids with mix of valid + invalid RIDs
//
// The prefiltered path must skip invalid/deleted RIDs silently and return
// ONLY the valid subset, without crashing.
// =============================================================================

TEST_F(QA_GDB847_NearestScan, GDB847_PrefilteredMixValidAndInvalidRids) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    auto rid1 = insert_row(heap, 1, "alpha", {1.0F, 0.0F, 0.0F});
    auto rid2 = insert_row(heap, 2, "beta", {0.9F, 0.1F, 0.0F});

    // RID{9999, 9999} is invalid (page does not exist).
    // RID{8888, 8888} is also invalid.
    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.prefiltered_rids = {rid1, RID{9999, 9999}, rid2, RID{8888, 8888}};

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    // Exactly the 2 valid RIDs should be returned; no crash from the 2 invalid ones.
    ASSERT_EQ(results.size(), 2u);
    std::vector<int32_t> ids;
    for (const auto& t : results) {
        ids.push_back(t.values[0].as_int32());
    }
    EXPECT_NE(std::find(ids.begin(), ids.end(), 1), ids.end()); // alpha present
    EXPECT_NE(std::find(ids.begin(), ids.end(), 2), ids.end()); // beta present
    op.close();
}

// =============================================================================
// GDB847: Non-empty prefiltered_rids, ALL valid, fewer than k
//
// The operator must return exactly the valid subset (not pad with extra rows).
// =============================================================================

TEST_F(QA_GDB847_NearestScan, GDB847_PrefilteredAllValidFewerThanK) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    auto rid1 = insert_row(heap, 1, "alpha", {1.0F, 0.0F, 0.0F});
    auto rid2 = insert_row(heap, 2, "beta", {0.0F, 1.0F, 0.0F});
    // Insert more rows to the heap that are NOT in the prefilter.
    insert_row(heap, 3, "gamma", {0.5F, 0.5F, 0.0F});
    insert_row(heap, 4, "delta", {0.2F, 0.2F, 0.0F});

    NearestScanConfig config;
    config.k = 10; // k=10 >> prefilter size of 2
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.prefiltered_rids = {rid1, rid2}; // only 2 candidates

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    // Must return exactly 2 — the valid prefiltered rows.
    // Must NOT include gamma or delta (outside the prefilter set).
    ASSERT_EQ(results.size(), 2u);
    std::vector<int32_t> ids;
    for (const auto& t : results) {
        ids.push_back(t.values[0].as_int32());
    }
    EXPECT_NE(std::find(ids.begin(), ids.end(), 1), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), 2), ids.end());
    EXPECT_EQ(std::find(ids.begin(), ids.end(), 3), ids.end()); // gamma excluded
    EXPECT_EQ(std::find(ids.begin(), ids.end(), 4), ids.end()); // delta excluded
    op.close();
}

// =============================================================================
// GDB847: Prefilter including every row == same candidate set as empty prefilter
//
// When prefiltered_rids contains every RID in the heap, the result must be
// identical to an empty prefilter (fall-through brute force) — same ids,
// same distances.  This confirms both paths agree on the candidate set when
// the prefilter is the universal set.
// =============================================================================

TEST_F(QA_GDB847_NearestScan, GDB847_PrefilteredAllRowsEqualsEmptyPrefilter) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    auto rid1 = insert_row(heap, 1, "alpha", {1.0F, 0.0F, 0.0F});
    auto rid2 = insert_row(heap, 2, "beta", {0.0F, 1.0F, 0.0F});
    auto rid3 = insert_row(heap, 3, "gamma", {0.5F, 0.5F, 0.0F});

    // Run with empty prefilter (brute force).
    auto run = [&](std::vector<RID> rids) {
        NearestScanConfig config;
        config.k = 3;
        config.query_vector = {1.0F, 0.0F, 0.0F};
        config.metric = DistanceMetric::L2;
        config.embedding_column_index = 2;
        config.prefiltered_rids = std::move(rids);

        OutputSchema schema(output_cols_);
        BoundStatement bound;
        NearestScanOperator op(
            heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

        EXPECT_TRUE(op.open().has_value());
        auto results = drain(op);
        op.close();
        return results;
    };

    auto brute = run({});                       // empty prefilter → fall-through
    auto prefiltered = run({rid1, rid2, rid3}); // prefilter with all rows

    // Same count.
    ASSERT_EQ(brute.size(), 3u);
    ASSERT_EQ(prefiltered.size(), 3u);

    // Same ids in the same order (distances are deterministic for unique values).
    for (size_t i = 0; i < brute.size(); ++i) {
        EXPECT_EQ(brute[i].values[0].as_int32(), prefiltered[i].values[0].as_int32())
            << "rank " << i << " differs between brute-force and prefiltered";
        EXPECT_NEAR(brute[i].values[3].as_float64(), prefiltered[i].values[3].as_float64(), 1e-5)
            << "distance at rank " << i << " differs";
    }
}

// =============================================================================
// GDB847: Duplicate RIDs in prefiltered_rids → no duplicate result rows
//
// If the same RID appears twice in prefiltered_rids the row must appear
// exactly once in the output.
// =============================================================================

TEST_F(QA_GDB847_NearestScan, GDB847_PrefilteredDuplicateRidsNoDoubleEmit) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    auto rid1 = insert_row(heap, 1, "alpha", {1.0F, 0.0F, 0.0F});
    auto rid2 = insert_row(heap, 2, "beta", {0.0F, 1.0F, 0.0F});

    NearestScanConfig config;
    config.k = 10;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    // rid1 appears three times, rid2 once.
    config.prefiltered_rids = {rid1, rid1, rid1, rid2};

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);

    // 2 distinct rows — alpha must appear exactly once even though its RID is
    // repeated three times in the prefilter list.
    ASSERT_EQ(results.size(), 2u);
    int alpha_count = 0;
    int beta_count = 0;
    for (const auto& t : results) {
        if (t.values[0].as_int32() == 1) {
            ++alpha_count;
        }
        if (t.values[0].as_int32() == 2) {
            ++beta_count;
        }
    }
    EXPECT_EQ(alpha_count, 1);
    EXPECT_EQ(beta_count, 1);
    op.close();
}

// =============================================================================
// GDB847: Out-of-range page_id invalid RID — no crash
//
// RID{UINT32_MAX, 0} is far outside any real page range. The operator must
// skip it gracefully (get_tuple fails) without crashing or UB.
// =============================================================================

TEST_F(QA_GDB847_NearestScan, GDB847_PrefilteredOutOfRangePageIdNocrash) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    auto rid1 = insert_row(heap, 1, "alpha", {1.0F, 0.0F, 0.0F});

    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    // Various extreme/invalid RID forms.
    config.prefiltered_rids = {
        rid1,
        RID{UINT32_MAX, 0},          // out of range page
        RID{0, UINT16_MAX},          // out of range slot on page 0
        RID{UINT32_MAX, UINT16_MAX}, // both out of range
    };

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    // Only the valid rid1 should be returned; the three invalid RIDs must be
    // silently skipped, no crash.
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    op.close();
}

// =============================================================================
// GDB847: Valid page, wrong slot — gracefully skipped
//
// Slot 9999 on an otherwise-valid page does not exist; must be skipped.
// =============================================================================

TEST_F(QA_GDB847_NearestScan, GDB847_PrefilteredValidPageWrongSlotSkipped) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    auto rid1 = insert_row(heap, 1, "alpha", {1.0F, 0.0F, 0.0F});

    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    // Same page as rid1 but invalid slot.
    RID bad_slot{rid1.page_id, static_cast<uint16_t>(9999)};
    config.prefiltered_rids = {rid1, bad_slot};

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    // Only the real row should appear; bad_slot must be skipped silently.
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    op.close();
}

// =============================================================================
// GDB847: Distance ordering in the fall-through path
//
// The brute-force fall-through must sort by distance ascending (nearest first).
// Hardcode a dataset where every distance is unique so there are no ties.
// =============================================================================

TEST_F(QA_GDB847_NearestScan, GDB847_EmptyPrefilteredDistanceOrderingCorrect) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    // Query vector: [1, 0, 0].
    // Distances (L2 squared):
    //   id=1: [1,0,0] → 0.0
    //   id=2: [0.5,0,0] → 0.25
    //   id=3: [0,0,0] → 1.0
    //   id=4: [-1,0,0] → 4.0
    insert_row(heap, 1, "d0", {1.0F, 0.0F, 0.0F});
    insert_row(heap, 2, "d0.25", {0.5F, 0.0F, 0.0F});
    insert_row(heap, 3, "d1", {0.0F, 0.0F, 0.0F});
    insert_row(heap, 4, "d4", {-1.0F, 0.0F, 0.0F});

    NearestScanConfig config;
    config.k = 4;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.prefiltered_rids = {}; // empty → fall-through

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 4u);

    // Strictly ascending distance.
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_EQ(results[1].values[0].as_int32(), 2);
    EXPECT_EQ(results[2].values[0].as_int32(), 3);
    EXPECT_EQ(results[3].values[0].as_int32(), 4);

    EXPECT_NEAR(results[0].values[3].as_float64(), 0.0, 1e-5);
    EXPECT_NEAR(results[1].values[3].as_float64(), 0.25, 1e-5);
    EXPECT_NEAR(results[2].values[3].as_float64(), 1.0, 1e-5);
    EXPECT_NEAR(results[3].values[3].as_float64(), 4.0, 1e-5);

    op.close();
}

// =============================================================================
// GDB847: Empty heap + empty prefilter → 0 rows, no crash
//
// Regression guard: the fall-through to brute force on an empty table must
// succeed and return zero rows.
// =============================================================================

TEST_F(QA_GDB847_NearestScan, GDB847_EmptyHeapEmptyPrefilteredZeroRowsNocrash) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);
    // No rows inserted.

    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.prefiltered_rids = {}; // fall-through on empty table

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    EXPECT_EQ(results.size(), 0u);
    op.close();
}

// =============================================================================
// GDB847: Empty heap + non-empty prefilter (all invalid) → 0 rows, no crash
//
// The prefiltered path on an empty table with invalid RIDs must not crash.
// =============================================================================

TEST_F(QA_GDB847_NearestScan, GDB847_EmptyHeapPrefilteredInvalidRidsZeroRowsNocrash) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);
    // No rows inserted.

    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.prefiltered_rids = {RID{9999, 9999}, RID{0, 0}};

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    EXPECT_EQ(results.size(), 0u);
    op.close();
}

// =============================================================================
// GDB847: Closest row in fall-through is strictly closer than second row
//
// Mutation guard: the empty-set-fall-through test must fail if the dispatch
// sends an empty prefilter to execute_prefiltered_search() (zero candidates →
// 0 rows).  This test fails unless fall-through produces non-zero rows AND
// the closest result has a strictly smaller distance than the second.
// =============================================================================

TEST_F(QA_GDB847_NearestScan, GDB847_EmptyPrefilteredClosestIsStrictlyNearest) {
    TableHeap heap(*table_bpm_, dm_, table_file_id_);

    // alpha is unambiguously closest; gap of 2.0 to beta/gamma.
    insert_row(heap, 1, "alpha", {1.0F, 0.0F, 0.0F}); // L2 dist = 0
    insert_row(heap, 2, "beta", {0.0F, 1.0F, 0.0F});  // L2 dist = 2
    insert_row(heap, 3, "gamma", {0.0F, 0.0F, 1.0F}); // L2 dist = 2

    NearestScanConfig config;
    config.k = 2;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::L2;
    config.embedding_column_index = 2;
    config.prefiltered_rids = {}; // empty → must fall through and return rows

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    // If the dispatch is broken (empty → zero candidates) this gives 0 rows.
    ASSERT_EQ(results.size(), 2u);
    // The first result is alpha — hardcoded.
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    // The distance of rank-0 is strictly smaller than rank-1.
    double d0 = results[0].values[3].as_float64();
    double d1 = results[1].values[3].as_float64();
    EXPECT_NEAR(d0, 0.0, 1e-5);
    EXPECT_LT(d0, d1); // strictly nearer, not a tie
    op.close();
}
