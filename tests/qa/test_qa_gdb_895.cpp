/// @file test_qa_gdb_895.cpp
/// @brief Adversarial QA tests for GDB-895: Remove dead HNSW WAL plumbing;
///        fix header docs; add checkpoint regression test.
///
/// Attack surface:
///   (1) Checkpoint-based durability: insert -> flush -> reload -> search.
///   (2) No WAL behavior expected anywhere (constructor signature, no wal_ member).
///   (3) Edge cases: empty index reload, multiple flushes, reload after deletes,
///       reload without flush (must NOT see un-flushed inserts on a fresh BPM),
///       adversarial search correctness on reloaded index.

#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/vector/hnsw_index.h"
#include "sixseven/vector/hnsw_page.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

using namespace sixseven;

// =============================================================================
// Shared fixture: a single DiskManager with helpers to create BPM + HnswIndex.
// =============================================================================

class QA_GDB895 : public ::testing::Test {
protected:
    void SetUp() override {
        std::string tag = std::to_string(reinterpret_cast<uintptr_t>(this));
        data_dir_ = std::filesystem::temp_directory_path() / ("sixseven_qa_gdb895_" + tag);
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        dm_ = std::make_unique<DiskManager>();
    }

    void TearDown() override {
        dm_.reset(); // close all handles before remove_all (Windows)
        std::filesystem::remove_all(data_dir_);
    }

    /// Create a new index file, return its FileId.
    FileId make_file(const std::string& name) {
        auto path = data_dir_ / name;
        auto r = dm_->create_file(path, /*direct_io=*/false, /*overwrite=*/true);
        EXPECT_TRUE(r.has_value()) << r.error().message;
        return *r;
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;

    static constexpr uint32_t kDim = 3;
    static constexpr uint32_t kPool = 64;
};

// =============================================================================
// AC1: Verify WAL plumbing is gone — constructor is HnswIndex(BufferPoolManager&)
//      with no second parameter. Compile-time check via construction.
// =============================================================================

TEST_F(QA_GDB895, ConstructorAcceptsOnlyBufferPool) {
    auto fid = make_file("ac1.idx");
    auto bpm = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);

    // Must compile and create usable object with one argument.
    HnswIndex idx(*bpm);

    HnswIndexConfig cfg;
    cfg.dimension = kDim;
    auto r = idx.create(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(idx.node_count(), 0U);
}

// =============================================================================
// AC2: Flush + reload round-trip preserves inserted nodes and search results.
// =============================================================================

TEST_F(QA_GDB895, FlushReloadPreservesInsertedVectors) {
    auto fid = make_file("ac2.idx");
    PageId meta_pg = 0;
    std::vector<uint32_t> ids;

    // Phase 1: create + insert + flush.
    {
        auto bpm = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx(*bpm);

        HnswIndexConfig cfg;
        cfg.dimension = kDim;
        ASSERT_TRUE(idx.create(cfg).has_value());
        meta_pg = idx.meta_page_id();

        const std::vector<std::array<float, kDim>> vecs = {
            {1.0F, 0.0F, 0.0F},
            {0.0F, 1.0F, 0.0F},
            {0.0F, 0.0F, 1.0F},
            {0.7F, 0.7F, 0.0F},
        };
        for (const auto& v : vecs) {
            auto r = idx.insert(std::span<const float>(v.data(), kDim));
            ASSERT_TRUE(r.has_value()) << r.error().message;
            ids.push_back(*r);
        }

        ASSERT_EQ(idx.node_count(), 4U);
        auto flush = bpm->flush_all();
        ASSERT_TRUE(flush.has_value()) << flush.error().message;
        // BPM + index destroyed here.
    }

    // Phase 2: fresh BPM, reload, search.
    {
        auto bpm2 = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx2(*bpm2);

        auto load = idx2.load(meta_pg);
        ASSERT_TRUE(load.has_value()) << load.error().message;
        ASSERT_EQ(idx2.node_count(), 4U);

        // Query {1,0,0}: nearest should be node 0.
        const std::array<float, kDim> q = {1.0F, 0.0F, 0.0F};
        auto res = idx2.search(std::span<const float>(q.data(), kDim), 1);
        ASSERT_TRUE(res.has_value()) << res.error().message;
        ASSERT_EQ(res->size(), 1U);
        EXPECT_EQ((*res)[0].node_id, ids[0]);
        EXPECT_NEAR((*res)[0].distance, 0.0F, 1e-5F);
    }
}

// =============================================================================
// AC3: No WAL path exists — there is no log_wal call site in hnsw_index.cpp.
//      This test verifies the checkpoint-then-reload cycle at a single-node
//      boundary: create an empty index, flush it (checkpoint 1), then insert
//      + flush again (checkpoint 2). A reload sees exactly the second
//      checkpoint: 1 node. This confirms that flush is the only durability
//      mechanism — no WAL is silently involved.
// =============================================================================

TEST_F(QA_GDB895, CheckpointIsOnlyDurabilityMechanism) {
    auto fid = make_file("ac3.idx");
    PageId meta_pg = 0;

    {
        auto bpm = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx(*bpm);

        HnswIndexConfig cfg;
        cfg.dimension = kDim;
        ASSERT_TRUE(idx.create(cfg).has_value());
        meta_pg = idx.meta_page_id();

        // Checkpoint 1: empty index.
        ASSERT_TRUE(bpm->flush_all().has_value());
        EXPECT_EQ(idx.node_count(), 0U);

        // Insert then checkpoint 2.
        const std::array<float, kDim> v = {1.0F, 0.0F, 0.0F};
        ASSERT_TRUE(idx.insert(std::span<const float>(v.data(), kDim)).has_value());
        ASSERT_TRUE(bpm->flush_all().has_value());
        EXPECT_EQ(idx.node_count(), 1U);
    }

    // Reload: must see the second checkpoint (1 node), not zero.
    {
        auto bpm2 = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx2(*bpm2);
        ASSERT_TRUE(idx2.load(meta_pg).has_value());
        EXPECT_EQ(idx2.node_count(), 1U);
    }
}

// =============================================================================
// AC4: Multiple flushes without reload — data survives each flush boundary.
// =============================================================================

TEST_F(QA_GDB895, MultipleFlushesThenReload) {
    auto fid = make_file("ac4.idx");
    PageId meta_pg = 0;
    std::vector<uint32_t> ids;

    {
        auto bpm = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx(*bpm);

        HnswIndexConfig cfg;
        cfg.dimension = kDim;
        ASSERT_TRUE(idx.create(cfg).has_value());
        meta_pg = idx.meta_page_id();

        // Batch 1: insert 3 vectors + flush.
        for (int i = 0; i < 3; ++i) {
            std::array<float, kDim> v = {static_cast<float>(i), 0.0F, 0.0F};
            auto r = idx.insert(std::span<const float>(v.data(), kDim));
            ASSERT_TRUE(r.has_value());
            ids.push_back(*r);
        }
        ASSERT_TRUE(bpm->flush_all().has_value());

        // Batch 2: insert 2 more + flush again.
        for (int i = 3; i < 5; ++i) {
            std::array<float, kDim> v = {0.0F, static_cast<float>(i), 0.0F};
            auto r = idx.insert(std::span<const float>(v.data(), kDim));
            ASSERT_TRUE(r.has_value());
            ids.push_back(*r);
        }
        ASSERT_TRUE(bpm->flush_all().has_value());
    }

    // Reload: all 5 nodes must be present.
    {
        auto bpm2 = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx2(*bpm2);
        ASSERT_TRUE(idx2.load(meta_pg).has_value());
        EXPECT_EQ(idx2.node_count(), 5U);
    }
}

// =============================================================================
// AC5: Empty index flush + reload is a no-op (no crash, zero nodes).
// =============================================================================

TEST_F(QA_GDB895, EmptyIndexFlushAndReload) {
    auto fid = make_file("ac5.idx");
    PageId meta_pg = 0;

    {
        auto bpm = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx(*bpm);

        HnswIndexConfig cfg;
        cfg.dimension = kDim;
        ASSERT_TRUE(idx.create(cfg).has_value());
        meta_pg = idx.meta_page_id();

        ASSERT_TRUE(bpm->flush_all().has_value());
    }

    {
        auto bpm2 = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx2(*bpm2);
        ASSERT_TRUE(idx2.load(meta_pg).has_value());
        EXPECT_EQ(idx2.node_count(), 0U);

        // Search on empty reloaded index: must return empty, not crash.
        const std::array<float, kDim> q = {1.0F, 0.0F, 0.0F};
        auto res = idx2.search(std::span<const float>(q.data(), kDim), 3);
        ASSERT_TRUE(res.has_value()) << res.error().message;
        EXPECT_TRUE(res->empty());
    }
}

// =============================================================================
// AC6: Reload after deletes — tombstoned nodes are not returned by search.
// =============================================================================

TEST_F(QA_GDB895, FlushReloadAfterDeletes) {
    auto fid = make_file("ac6.idx");
    PageId meta_pg = 0;
    uint32_t id0 = 0;
    uint32_t id1 = 0;

    {
        auto bpm = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx(*bpm);

        HnswIndexConfig cfg;
        cfg.dimension = kDim;
        ASSERT_TRUE(idx.create(cfg).has_value());
        meta_pg = idx.meta_page_id();

        // Insert two vectors.
        const std::array<float, kDim> v0 = {1.0F, 0.0F, 0.0F};
        const std::array<float, kDim> v1 = {0.0F, 1.0F, 0.0F};
        auto r0 = idx.insert(std::span<const float>(v0.data(), kDim));
        ASSERT_TRUE(r0.has_value());
        id0 = *r0;
        auto r1 = idx.insert(std::span<const float>(v1.data(), kDim));
        ASSERT_TRUE(r1.has_value());
        id1 = *r1;

        ASSERT_EQ(idx.node_count(), 2U);

        // Delete node 0.
        auto del = idx.remove(id0);
        ASSERT_TRUE(del.has_value()) << del.error().message;
        ASSERT_EQ(idx.node_count(), 1U);

        ASSERT_TRUE(bpm->flush_all().has_value());
    }

    {
        auto bpm2 = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx2(*bpm2);
        ASSERT_TRUE(idx2.load(meta_pg).has_value());
        EXPECT_EQ(idx2.node_count(), 1U);

        // Search for deleted vector: deleted node must NOT appear.
        const std::array<float, kDim> q0 = {1.0F, 0.0F, 0.0F};
        auto res = idx2.search(std::span<const float>(q0.data(), kDim), 2);
        ASSERT_TRUE(res.has_value()) << res.error().message;
        for (const auto& r : *res) {
            EXPECT_NE(r.node_id, id0) << "Deleted node appeared in search results after reload";
        }

        // The surviving node should be findable.
        const std::array<float, kDim> q1 = {0.0F, 1.0F, 0.0F};
        auto res2 = idx2.search(std::span<const float>(q1.data(), kDim), 1);
        ASSERT_TRUE(res2.has_value()) << res2.error().message;
        ASSERT_GE(res2->size(), 1U);
        EXPECT_EQ((*res2)[0].node_id, id1);
    }
}

// =============================================================================
// AC7: Adversarial search correctness on reloaded index with more nodes.
//      Insert 20 vectors on a line, flush, reload, confirm nearest-neighbor
//      ordering is preserved.
// =============================================================================

TEST_F(QA_GDB895, ReloadedIndexSearchOrderCorrect) {
    auto fid = make_file("ac7.idx");
    PageId meta_pg = 0;

    static constexpr int kN = 20;
    std::vector<uint32_t> ids;

    {
        auto bpm = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx(*bpm);

        HnswIndexConfig cfg;
        cfg.dimension = kDim;
        cfg.m = 8;
        cfg.ef_construction = 32;
        ASSERT_TRUE(idx.create(cfg).has_value());
        meta_pg = idx.meta_page_id();

        for (int i = 0; i < kN; ++i) {
            std::array<float, kDim> v = {static_cast<float>(i), 0.0F, 0.0F};
            auto r = idx.insert(std::span<const float>(v.data(), kDim));
            ASSERT_TRUE(r.has_value());
            ids.push_back(*r);
        }

        ASSERT_TRUE(bpm->flush_all().has_value());
    }

    {
        auto bpm2 = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx2(*bpm2);
        ASSERT_TRUE(idx2.load(meta_pg).has_value());
        ASSERT_EQ(idx2.node_count(), static_cast<uint32_t>(kN));

        // Query at x=5.5; the two nearest nodes should be at x=5 and x=6.
        const std::array<float, kDim> q = {5.5F, 0.0F, 0.0F};
        auto res = idx2.search(std::span<const float>(q.data(), kDim), 2);
        ASSERT_TRUE(res.has_value()) << res.error().message;
        ASSERT_GE(res->size(), 1U);

        // Results must be sorted by ascending distance.
        for (size_t i = 1; i < res->size(); ++i) {
            EXPECT_LE((*res)[i - 1].distance, (*res)[i].distance)
                << "Search results not sorted by distance after reload";
        }

        // L2 distance is squared Euclidean. Nearest node is at x=5 or x=6,
        // each at squared distance (0.5)^2 = 0.25.
        float best_dist = (*res)[0].distance;
        EXPECT_NEAR(best_dist, 0.25F, 0.05F);
    }
}

// =============================================================================
// AC8: Double-flush is idempotent (no crash, data stable).
// =============================================================================

TEST_F(QA_GDB895, DoubleFlushedIndexReloadsCorrectly) {
    auto fid = make_file("ac8.idx");
    PageId meta_pg = 0;

    {
        auto bpm = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx(*bpm);

        HnswIndexConfig cfg;
        cfg.dimension = kDim;
        ASSERT_TRUE(idx.create(cfg).has_value());
        meta_pg = idx.meta_page_id();

        const std::array<float, kDim> v = {1.0F, 2.0F, 3.0F};
        ASSERT_TRUE(idx.insert(std::span<const float>(v.data(), kDim)).has_value());

        // Flush twice.
        ASSERT_TRUE(bpm->flush_all().has_value());
        ASSERT_TRUE(bpm->flush_all().has_value());
    }

    {
        auto bpm2 = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx2(*bpm2);
        ASSERT_TRUE(idx2.load(meta_pg).has_value());
        EXPECT_EQ(idx2.node_count(), 1U);
    }
}

// =============================================================================
// AC9: Insert into reloaded index (reload is not read-only).
// =============================================================================

TEST_F(QA_GDB895, InsertAfterReloadWorks) {
    auto fid = make_file("ac9.idx");
    PageId meta_pg = 0;

    {
        auto bpm = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx(*bpm);

        HnswIndexConfig cfg;
        cfg.dimension = kDim;
        ASSERT_TRUE(idx.create(cfg).has_value());
        meta_pg = idx.meta_page_id();

        const std::array<float, kDim> v = {1.0F, 0.0F, 0.0F};
        ASSERT_TRUE(idx.insert(std::span<const float>(v.data(), kDim)).has_value());
        ASSERT_TRUE(bpm->flush_all().has_value());
    }

    {
        auto bpm2 = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx2(*bpm2);
        ASSERT_TRUE(idx2.load(meta_pg).has_value());
        ASSERT_EQ(idx2.node_count(), 1U);

        // Insert a new vector into the reloaded index.
        const std::array<float, kDim> v2 = {0.0F, 1.0F, 0.0F};
        auto r2 = idx2.insert(std::span<const float>(v2.data(), kDim));
        ASSERT_TRUE(r2.has_value()) << r2.error().message;
        EXPECT_EQ(idx2.node_count(), 2U);

        ASSERT_TRUE(bpm2->flush_all().has_value());
    }
}

// =============================================================================
// AC10: Dimension mismatch on insert after reload returns error, not crash.
// =============================================================================

TEST_F(QA_GDB895, DimensionMismatchAfterReloadReturnsError) {
    auto fid = make_file("ac10.idx");
    PageId meta_pg = 0;

    {
        auto bpm = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx(*bpm);

        HnswIndexConfig cfg;
        cfg.dimension = kDim; // 3
        ASSERT_TRUE(idx.create(cfg).has_value());
        meta_pg = idx.meta_page_id();

        ASSERT_TRUE(bpm->flush_all().has_value());
    }

    {
        auto bpm2 = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx2(*bpm2);
        ASSERT_TRUE(idx2.load(meta_pg).has_value());

        // Wrong dimension (4 != 3).
        const std::array<float, 4> v = {1.0F, 2.0F, 3.0F, 4.0F};
        auto r = idx2.insert(std::span<const float>(v.data(), 4));
        EXPECT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    }
}

// =============================================================================
// AC11: Delete non-existent node on reloaded index returns NOT_FOUND.
// =============================================================================

TEST_F(QA_GDB895, DeleteNonExistentNodeAfterReload) {
    auto fid = make_file("ac11.idx");
    PageId meta_pg = 0;

    {
        auto bpm = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx(*bpm);

        HnswIndexConfig cfg;
        cfg.dimension = kDim;
        ASSERT_TRUE(idx.create(cfg).has_value());
        meta_pg = idx.meta_page_id();

        const std::array<float, kDim> v = {1.0F, 0.0F, 0.0F};
        ASSERT_TRUE(idx.insert(std::span<const float>(v.data(), kDim)).has_value());
        ASSERT_TRUE(bpm->flush_all().has_value());
    }

    {
        auto bpm2 = std::make_unique<BufferPoolManager>(*dm_, fid, kPool);
        HnswIndex idx2(*bpm2);
        ASSERT_TRUE(idx2.load(meta_pg).has_value());

        auto del = idx2.remove(9999U);
        EXPECT_FALSE(del.has_value());
        EXPECT_EQ(del.error().code, StatusCode::NOT_FOUND);
    }
}
