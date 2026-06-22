#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/vector/hnsw_index.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

using namespace sixseven;

// =============================================================================
// GDB-895: Regression test -- HNSW persistence is checkpoint-based (no WAL).
//
// Verifies that vectors inserted into an HnswIndex survive a flush + reload
// round-trip. Specifically: insert vectors, flush buffer pool dirty pages to
// disk, destroy the HnswIndex and its BufferPoolManager, construct a fresh
// HnswIndex over the same file (reusing the DiskManager and FileId), load it,
// then confirm search returns the originally inserted vectors.
// =============================================================================

class HnswCheckpointPersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_hnsw_checkpoint";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        index_path_ = data_dir_ / "hnsw.idx";

        // Use a heap-allocated DiskManager so we can explicitly destroy it
        // before remove_all in TearDown (Windows requires all handles closed).
        dm_ = std::make_unique<DiskManager>();
    }

    void TearDown() override {
        // Destroy DiskManager first to close all file handles before removing
        // the temp directory (required on Windows).
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    std::filesystem::path data_dir_;
    std::filesystem::path index_path_;

    // Heap-allocated so it can be explicitly destroyed before remove_all.
    std::unique_ptr<DiskManager> dm_;

    static constexpr uint32_t kDim = 4;
    static constexpr uint32_t kPoolPages = 128;
};

// Insert vectors, flush (checkpoint), reload from disk, confirm search results.
TEST_F(HnswCheckpointPersistenceTest, FlushAndReloadRoundTrip) {
    PageId meta_page = 0;
    std::vector<uint32_t> inserted_ids;

    // -- Phase 1: create, insert, flush ------------------------------------------
    FileId fid = 0;
    {
        auto fid_result = dm_->create_file(index_path_, /*direct_io=*/false, /*overwrite=*/true);
        ASSERT_TRUE(fid_result.has_value()) << fid_result.error().message;
        fid = *fid_result;

        auto bpm = std::make_unique<BufferPoolManager>(*dm_, fid, kPoolPages);
        auto hnsw = std::make_unique<HnswIndex>(*bpm);

        HnswIndexConfig config;
        config.dimension = kDim;
        auto create = hnsw->create(config);
        ASSERT_TRUE(create.has_value()) << create.error().message;

        meta_page = hnsw->meta_page_id();
        ASSERT_NE(meta_page, 0U);

        const std::vector<std::array<float, kDim>> vecs = {
            {1.0F, 0.0F, 0.0F, 0.0F},
            {0.0F, 1.0F, 0.0F, 0.0F},
            {0.0F, 0.0F, 1.0F, 0.0F},
            {0.0F, 0.0F, 0.0F, 1.0F},
            {0.5F, 0.5F, 0.0F, 0.0F},
        };
        for (const auto& v : vecs) {
            auto r = hnsw->insert(std::span<const float>(v.data(), kDim));
            ASSERT_TRUE(r.has_value()) << r.error().message;
            inserted_ids.push_back(*r);
        }

        ASSERT_EQ(hnsw->node_count(), static_cast<uint32_t>(vecs.size()));

        // Checkpoint: flush all dirty buffer pool pages to disk.
        auto flush = bpm->flush_all();
        ASSERT_TRUE(flush.has_value()) << flush.error().message;

        // bpm and hnsw destroyed here -- simulating a clean shutdown.
        // No WAL involvement: persistence is purely checkpoint-based.
    }

    // -- Phase 2: reload from disk using a fresh index + buffer pool -------------
    {
        // Reuse the same DiskManager and FileId -- the file is already open.
        auto bpm2 = std::make_unique<BufferPoolManager>(*dm_, fid, kPoolPages);
        auto hnsw2 = std::make_unique<HnswIndex>(*bpm2);

        auto load = hnsw2->load(meta_page);
        ASSERT_TRUE(load.has_value()) << load.error().message;

        // Node count must be preserved across the checkpoint boundary.
        ASSERT_EQ(hnsw2->node_count(), static_cast<uint32_t>(inserted_ids.size()));

        // The first inserted vector {1,0,0,0} should be its own nearest
        // neighbor with distance ~0.
        const std::array<float, kDim> query = {1.0F, 0.0F, 0.0F, 0.0F};
        auto results = hnsw2->search(std::span<const float>(query.data(), kDim), 1);
        ASSERT_TRUE(results.has_value()) << results.error().message;
        ASSERT_EQ(results->size(), 1U);
        EXPECT_EQ((*results)[0].node_id, inserted_ids[0]);
        EXPECT_NEAR((*results)[0].distance, 0.0F, 1e-5F);
    }
}

// Verify that HnswIndex takes no WAL parameter -- the dead WAL plumbing is gone.
TEST_F(HnswCheckpointPersistenceTest, ConstructorTakesNoWalParameter) {
    auto fid_result = dm_->create_file(index_path_, /*direct_io=*/false, /*overwrite=*/true);
    ASSERT_TRUE(fid_result.has_value()) << fid_result.error().message;

    auto bpm = std::make_unique<BufferPoolManager>(*dm_, *fid_result, kPoolPages);

    // If this compiles and the object is usable, the constructor signature is
    // correct: HnswIndex(BufferPoolManager&) with no WalWriter* parameter.
    auto hnsw = std::make_unique<HnswIndex>(*bpm);
    ASSERT_NE(hnsw, nullptr);

    HnswIndexConfig config;
    config.dimension = kDim;
    auto create = hnsw->create(config);
    ASSERT_TRUE(create.has_value()) << create.error().message;
    EXPECT_EQ(hnsw->node_count(), 0U);
    EXPECT_EQ(hnsw->dimension(), kDim);
}
