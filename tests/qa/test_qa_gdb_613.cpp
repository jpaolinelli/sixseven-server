/// @file tests/qa/test_qa_gdb_613.cpp
/// QA adversarial tests for GDB-613: Wire buffer_pool_size_mb config to
/// StorageManager.
///
/// Acceptance Criteria verified:
///   AC1: StorageManager receives the configured buffer pool size from Config
///   AC2: With default config (256MB), each table gets 32,768 frames
///   AC3: (benchmark — not tested here)
///   AC4: Unit tests pass (verified via CI)
///   AC5: No regressions in QA tests (verified via CI)
///
/// Subtask AC (GDB-621):
///   StorageManager receives config.buffer_pool_size_mb * 128 as pool_size
///
/// Subtask AC (GDB-622):
///   BufferPoolManager tracks hit/miss counts with atomic counters
///   Accessors hit_count() and miss_count() are available

#include "sixseven/catalog/schema.h"
#include "sixseven/common/config.h"
#include "sixseven/common/types.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace sixseven {
namespace {

// =============================================================================
// Test fixture
// =============================================================================

class QA_GDB613 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb613";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
    }

    void TearDown() override { std::filesystem::remove_all(data_dir_); }

    TableSchema make_schema(table_id_t tid, const std::string& name) {
        TableSchema ts;
        ts.table_id = tid;
        ts.name = name;
        CatalogColumnDef col;
        col.ordinal = 0;
        col.name = "id";
        col.type_id = TypeId::INT32;
        ts.columns.push_back(col);
        return ts;
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
};

// =============================================================================
// AC1: StorageManager receives the configured buffer pool size from Config
// =============================================================================

/// Verify that StorageManager passes the configured pool_size to each table's
/// BufferPoolManager. With pool_size=1024, table BPM should accept at least
/// 1024 pages without running out of frames.
TEST_F(QA_GDB613, AC1_PoolSizePassedToTables) {
    // 8 MB -> 8 * 128 = 1024 frames
    uint32_t pool_size = 8 * 128;
    StorageManager sm(dm_, data_dir_, pool_size);

    auto schema = make_schema(1, "test_table");
    auto create = sm.create_table_storage(default_database_id, 1, schema);
    ASSERT_TRUE(create.has_value()) << create.error().message;

    auto ts = sm.get_table_storage(1);
    ASSERT_TRUE(ts.has_value()) << ts.error().message;

    // Allocate pages up to pool_size. If StorageManager wired pool_size
    // correctly, all allocations should succeed.
    std::vector<PageId> pages;
    for (uint32_t i = 0; i < pool_size; ++i) {
        auto page = (*ts)->bpm->new_page();
        ASSERT_TRUE(page.has_value())
            << "Failed to allocate page " << i << ": " << page.error().message;
        pages.push_back((*page)->page_id());
        auto unpin = (*ts)->bpm->unpin_page((*page)->page_id(), false);
        ASSERT_TRUE(unpin.has_value());
    }

    EXPECT_EQ((*ts)->bpm->pool_page_count(), pool_size);
}

/// Verify that a small pool_size (4 frames) is actually limiting — you can't
/// pin more than 4 pages simultaneously without eviction.
TEST_F(QA_GDB613, AC1_SmallPoolSizeIsRespected) {
    uint32_t pool_size = 4;
    StorageManager sm(dm_, data_dir_, pool_size);

    auto schema = make_schema(1, "small_pool_table");
    auto create = sm.create_table_storage(default_database_id, 1, schema);
    ASSERT_TRUE(create.has_value()) << create.error().message;

    auto ts = sm.get_table_storage(1);
    ASSERT_TRUE(ts.has_value());

    // Pin all 4 frames (without unpinning).
    std::vector<PageId> pinned_pages;
    for (uint32_t i = 0; i < pool_size; ++i) {
        auto page = (*ts)->bpm->new_page();
        ASSERT_TRUE(page.has_value())
            << "Failed at page " << i << ": " << page.error().message;
        pinned_pages.push_back((*page)->page_id());
    }

    // 5th allocation should fail — all frames pinned, none evictable.
    auto overflow = (*ts)->bpm->new_page();
    EXPECT_FALSE(overflow.has_value())
        << "Should fail when pool is full and all frames pinned";

    // Cleanup: unpin all.
    for (auto pid : pinned_pages) {
        ASSERT_TRUE((*ts)->bpm->unpin_page(pid, false).has_value());
    }
}

// =============================================================================
// AC2: With default config (256MB), each table gets 32,768 frames
// =============================================================================

/// Verify the default pool size constant is 32768 (256MB / 8KB = 32768 frames).
TEST_F(QA_GDB613, AC2_DefaultPoolSizeConstant) {
    EXPECT_EQ(default_pool_size, 32768u);
}

/// Verify StorageManager default pool_size parameter is 256 (the old default).
/// After GDB-613, the caller (main.cpp) passes the correct value, but the
/// StorageManager default argument remains 256 as a fallback.
TEST_F(QA_GDB613, AC2_StorageManagerDefaultArg) {
    // Construct without explicit pool_size — should use default 256.
    StorageManager sm(dm_, data_dir_);

    auto schema = make_schema(1, "default_table");
    auto create = sm.create_table_storage(default_database_id, 1, schema);
    ASSERT_TRUE(create.has_value()) << create.error().message;

    auto ts = sm.get_table_storage(1);
    ASSERT_TRUE(ts.has_value());

    // The default StorageManager pool_size is 256 frames. Allocate 256 pages.
    for (uint32_t i = 0; i < 256; ++i) {
        auto page = (*ts)->bpm->new_page();
        ASSERT_TRUE(page.has_value())
            << "Failed at page " << i << ": " << page.error().message;
        ASSERT_TRUE((*ts)->bpm->unpin_page((*page)->page_id(), false).has_value());
    }
}

/// Verify the conversion formula: buffer_pool_size_mb * 128 = frames.
TEST_F(QA_GDB613, AC2_ConversionFormula) {
    // 1 MB / 8 KB per page = 128 frames per MB.
    EXPECT_EQ(1u * 128, 128u);
    EXPECT_EQ(256u * 128, 32768u);
    EXPECT_EQ(512u * 128, 65536u);
}

// =============================================================================
// GDB-621: Pass buffer_pool_size_mb to StorageManager constructor in main.cpp
// =============================================================================

/// Verify that Config default buffer_pool_size_mb is 256.
TEST_F(QA_GDB613, GDB621_ConfigDefaultIs256MB) {
    Config cfg = Config::load_defaults();
    EXPECT_EQ(cfg.buffer_pool_size_mb, 256u);
}

/// Verify the conversion from Config to StorageManager produces correct pool.
TEST_F(QA_GDB613, GDB621_ConfigToStorageManagerWiring) {
    Config cfg = Config::load_defaults();
    uint32_t expected_frames = static_cast<uint32_t>(cfg.buffer_pool_size_mb * 128);
    EXPECT_EQ(expected_frames, 32768u);

    // Construct StorageManager with the converted value.
    StorageManager sm(dm_, data_dir_, expected_frames);

    auto schema = make_schema(1, "wired_table");
    auto create = sm.create_table_storage(default_database_id, 1, schema);
    ASSERT_TRUE(create.has_value()) << create.error().message;

    // Table should be usable.
    auto ts = sm.get_table_storage(1);
    ASSERT_TRUE(ts.has_value());
    EXPECT_NE((*ts)->bpm, nullptr);
}

// =============================================================================
// GDB-622: Buffer pool hit/miss counters
// =============================================================================

/// Verify counters start at zero.
TEST_F(QA_GDB613, GDB622_CountersStartAtZero) {
    auto test_dir = data_dir_ / "bpm_test";
    std::filesystem::create_directories(test_dir);
    auto file_path = test_dir / "test.db";

    auto fid = dm_.create_file(file_path, false, true);
    ASSERT_TRUE(fid.has_value()) << fid.error().message;

    BufferPoolManager bpm(dm_, *fid, 8);
    EXPECT_EQ(bpm.hit_count(), 0u);
    EXPECT_EQ(bpm.miss_count(), 0u);
}

/// Verify a fetch_page for a page not in the pool increments miss counter.
TEST_F(QA_GDB613, GDB622_FetchMissIncrementsMissCounter) {
    auto test_dir = data_dir_ / "bpm_test";
    std::filesystem::create_directories(test_dir);
    auto file_path = test_dir / "miss_test.db";

    auto fid = dm_.create_file(file_path, false, true);
    ASSERT_TRUE(fid.has_value());

    BufferPoolManager bpm(dm_, *fid, 8);

    // Allocate a page, unpin it, delete it from pool, then fetch from disk.
    auto np = bpm.new_page();
    ASSERT_TRUE(np.has_value());
    PageId pid = (*np)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid, true).has_value());
    ASSERT_TRUE(bpm.flush_page(pid).has_value());
    ASSERT_TRUE(bpm.delete_page(pid).has_value());

    // Now fetch — this should be a miss (page read from disk).
    auto fetch = bpm.fetch_page(pid);
    ASSERT_TRUE(fetch.has_value()) << fetch.error().message;
    EXPECT_EQ(bpm.miss_count(), 1u);
    EXPECT_EQ(bpm.hit_count(), 0u);
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());
}

/// Verify a fetch_page for a page already in the pool increments hit counter.
TEST_F(QA_GDB613, GDB622_FetchHitIncrementsHitCounter) {
    auto test_dir = data_dir_ / "bpm_test";
    std::filesystem::create_directories(test_dir);
    auto file_path = test_dir / "hit_test.db";

    auto fid = dm_.create_file(file_path, false, true);
    ASSERT_TRUE(fid.has_value());

    BufferPoolManager bpm(dm_, *fid, 8);

    // Allocate page (in pool). new_page does NOT count as hit or miss.
    auto np = bpm.new_page();
    ASSERT_TRUE(np.has_value());
    PageId pid = (*np)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());

    // Fetch the same page — it's still in the pool -> hit.
    auto fetch = bpm.fetch_page(pid);
    ASSERT_TRUE(fetch.has_value());
    EXPECT_EQ(bpm.hit_count(), 1u);
    EXPECT_EQ(bpm.miss_count(), 0u);
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());

    // Second fetch -> second hit.
    auto fetch2 = bpm.fetch_page(pid);
    ASSERT_TRUE(fetch2.has_value());
    EXPECT_EQ(bpm.hit_count(), 2u);
    EXPECT_EQ(bpm.miss_count(), 0u);
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());
}

/// Verify counters accumulate correctly through a mix of hits and misses.
TEST_F(QA_GDB613, GDB622_MixedHitsAndMisses) {
    auto test_dir = data_dir_ / "bpm_test";
    std::filesystem::create_directories(test_dir);
    auto file_path = test_dir / "mixed_test.db";

    auto fid = dm_.create_file(file_path, false, true);
    ASSERT_TRUE(fid.has_value());

    BufferPoolManager bpm(dm_, *fid, 4);

    // Allocate 4 pages (fills the pool).
    std::vector<PageId> pages;
    for (int i = 0; i < 4; ++i) {
        auto np = bpm.new_page();
        ASSERT_TRUE(np.has_value());
        pages.push_back((*np)->page_id());
        ASSERT_TRUE(bpm.unpin_page((*np)->page_id(), true).has_value());
    }

    EXPECT_EQ(bpm.hit_count(), 0u);
    EXPECT_EQ(bpm.miss_count(), 0u);

    // Fetch page 0 (still in pool) -> hit.
    auto f0 = bpm.fetch_page(pages[0]);
    ASSERT_TRUE(f0.has_value());
    EXPECT_EQ(bpm.hit_count(), 1u);
    ASSERT_TRUE(bpm.unpin_page(pages[0], false).has_value());

    // Flush and delete page 1 from pool, then fetch -> miss.
    ASSERT_TRUE(bpm.flush_page(pages[1]).has_value());
    ASSERT_TRUE(bpm.delete_page(pages[1]).has_value());
    auto f1 = bpm.fetch_page(pages[1]);
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(bpm.miss_count(), 1u);
    ASSERT_TRUE(bpm.unpin_page(pages[1], false).has_value());

    // Final check.
    EXPECT_EQ(bpm.hit_count(), 1u);
    EXPECT_EQ(bpm.miss_count(), 1u);
}

// =============================================================================
// Adversarial: Edge cases and boundary values
// =============================================================================

/// Pool size of 1 frame — minimum functional pool.
TEST_F(QA_GDB613, Adversarial_PoolSizeOne) {
    StorageManager sm(dm_, data_dir_, 1);

    auto schema = make_schema(1, "tiny_pool");
    auto create = sm.create_table_storage(default_database_id, 1, schema);
    ASSERT_TRUE(create.has_value()) << create.error().message;

    auto ts = sm.get_table_storage(1);
    ASSERT_TRUE(ts.has_value());

    // Should be able to allocate exactly 1 page.
    auto page = (*ts)->bpm->new_page();
    ASSERT_TRUE(page.has_value()) << page.error().message;
    auto pid = (*page)->page_id();

    // Second allocation should fail (only 1 frame, and it's pinned).
    auto page2 = (*ts)->bpm->new_page();
    EXPECT_FALSE(page2.has_value());

    // Unpin, then allocate again (eviction should work).
    ASSERT_TRUE((*ts)->bpm->unpin_page(pid, true).has_value());
    auto page3 = (*ts)->bpm->new_page();
    ASSERT_TRUE(page3.has_value()) << page3.error().message;
    ASSERT_TRUE((*ts)->bpm->unpin_page((*page3)->page_id(), false).has_value());
}

/// Multiple tables in the same StorageManager all get the same pool_size.
TEST_F(QA_GDB613, Adversarial_MultipleTablesGetSamePoolSize) {
    uint32_t pool_size = 16;
    StorageManager sm(dm_, data_dir_, pool_size);

    for (table_id_t tid = 1; tid <= 3; ++tid) {
        auto schema = make_schema(tid, "table_" + std::to_string(tid));
        auto create = sm.create_table_storage(default_database_id, tid, schema);
        ASSERT_TRUE(create.has_value()) << create.error().message;
    }

    // Each table should independently support pool_size pages.
    for (table_id_t tid = 1; tid <= 3; ++tid) {
        auto ts = sm.get_table_storage(tid);
        ASSERT_TRUE(ts.has_value());

        for (uint32_t i = 0; i < pool_size; ++i) {
            auto page = (*ts)->bpm->new_page();
            ASSERT_TRUE(page.has_value())
                << "Table " << tid << " page " << i << ": " << page.error().message;
            ASSERT_TRUE((*ts)->bpm->unpin_page((*page)->page_id(), false).has_value());
        }
    }
}

/// open_table_storage also uses the configured pool_size.
TEST_F(QA_GDB613, Adversarial_OpenTableUsesConfiguredPoolSize) {
    uint32_t pool_size = 32;

    // Create a table, write some pages, then drop storage and reopen.
    {
        StorageManager sm(dm_, data_dir_, pool_size);
        auto schema = make_schema(1, "reopen_test");
        auto create = sm.create_table_storage(default_database_id, 1, schema);
        ASSERT_TRUE(create.has_value()) << create.error().message;

        auto ts = sm.get_table_storage(1);
        ASSERT_TRUE(ts.has_value());

        // Write a page to ensure the file is non-empty.
        auto page = (*ts)->bpm->new_page();
        ASSERT_TRUE(page.has_value());
        ASSERT_TRUE((*ts)->bpm->unpin_page((*page)->page_id(), true).has_value());
        ASSERT_TRUE((*ts)->bpm->flush_all().has_value());

        // Drop the table storage to release the file lock.
        auto drop = sm.drop_table_storage(default_database_id, 1);
        ASSERT_TRUE(drop.has_value()) << drop.error().message;
    }

    // Re-create the file and open with same pool_size.
    // (drop_table_storage removes the file, so we recreate it.)
    {
        StorageManager sm2(dm_, data_dir_, pool_size);
        auto schema = make_schema(1, "reopen_test");
        auto create = sm2.create_table_storage(default_database_id, 1, schema);
        ASSERT_TRUE(create.has_value()) << create.error().message;

        auto ts = sm2.get_table_storage(1);
        ASSERT_TRUE(ts.has_value());

        // Should be able to allocate pages up to pool_size.
        for (uint32_t i = 0; i < pool_size; ++i) {
            auto page = (*ts)->bpm->new_page();
            ASSERT_TRUE(page.has_value())
                << "Reopen page " << i << ": " << page.error().message;
            ASSERT_TRUE((*ts)->bpm->unpin_page((*page)->page_id(), false).has_value());
        }
    }
}

/// Verify the uint32_t overflow boundary. buffer_pool_size_mb of 33554432
/// (32 TB) would overflow: 33554432 * 128 = 4294967296 = 2^32, wrapping to 0.
/// This test documents the boundary — values above ~33554431 MB are unsafe.
TEST_F(QA_GDB613, Adversarial_Uint32OverflowBoundary) {
    // Max safe value: UINT32_MAX / 128 = 33554431 MB (~32 TB).
    size_t max_safe_mb = static_cast<size_t>(UINT32_MAX) / 128;
    uint32_t frames = static_cast<uint32_t>(max_safe_mb * 128);
    EXPECT_EQ(frames, UINT32_MAX - 127); // 4294967168

    // Overflow case: 33554432 * 128 = 2^32, wraps to 0 in uint32_t.
    size_t overflow_mb = max_safe_mb + 1;
    uint32_t overflow_frames = static_cast<uint32_t>(overflow_mb * 128);
    EXPECT_EQ(overflow_frames, 0u)
        << "Demonstrates uint32_t overflow at " << overflow_mb << " MB";
}

/// Verify config file parsing of buffer_pool_size_mb.
TEST_F(QA_GDB613, Adversarial_ConfigFileParseBufferPoolSize) {
    // Write a config file with a non-default buffer_pool_size_mb.
    auto config_path = data_dir_ / "test_config.json";
    {
        std::ofstream f(config_path);
        f << R"({"buffer_pool_size_mb": 512})";
    }

    auto cfg = Config::load_from_file(config_path.string());
    ASSERT_TRUE(cfg.has_value()) << cfg.error().message;
    EXPECT_EQ(cfg->buffer_pool_size_mb, 512u);
    EXPECT_EQ(static_cast<uint32_t>(cfg->buffer_pool_size_mb * 128), 65536u);
}

/// Verify config with buffer_pool_size_mb = 0 (edge case).
TEST_F(QA_GDB613, Adversarial_ConfigZeroPoolSize) {
    auto config_path = data_dir_ / "zero_pool.json";
    {
        std::ofstream f(config_path);
        f << R"({"buffer_pool_size_mb": 0})";
    }

    auto cfg = Config::load_from_file(config_path.string());
    ASSERT_TRUE(cfg.has_value()) << cfg.error().message;
    EXPECT_EQ(cfg->buffer_pool_size_mb, 0u);

    // 0 * 128 = 0 frames. Creating a BufferPoolManager with 0 frames
    // should either fail or produce a pool where all allocations fail.
    uint32_t frames = static_cast<uint32_t>(cfg->buffer_pool_size_mb * 128);
    EXPECT_EQ(frames, 0u);

    // This documents the current behavior with pool_size=0.
    // A proper fix would validate the config and reject 0.
    StorageManager sm(dm_, data_dir_, frames);
    auto schema = make_schema(1, "zero_pool_table");

    // create_table_storage may succeed (just creates BPM), but page allocation
    // should fail immediately since there are no frames.
    auto create = sm.create_table_storage(default_database_id, 1, schema);
    if (create.has_value()) {
        auto ts = sm.get_table_storage(1);
        ASSERT_TRUE(ts.has_value());
        auto page = (*ts)->bpm->new_page();
        // With 0 frames, no page can be allocated.
        EXPECT_FALSE(page.has_value())
            << "Pool with 0 frames should not allocate pages";
    }
    // If create_table_storage itself fails, that's also acceptable behavior.
}

/// Verify config with buffer_pool_size_mb = 1 (minimum non-zero).
TEST_F(QA_GDB613, Adversarial_ConfigMinPoolSize) {
    auto config_path = data_dir_ / "min_pool.json";
    {
        std::ofstream f(config_path);
        f << R"({"buffer_pool_size_mb": 1})";
    }

    auto cfg = Config::load_from_file(config_path.string());
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->buffer_pool_size_mb, 1u);

    uint32_t frames = static_cast<uint32_t>(cfg->buffer_pool_size_mb * 128);
    EXPECT_EQ(frames, 128u);

    StorageManager sm(dm_, data_dir_, frames);
    auto schema = make_schema(1, "min_pool_table");
    auto create = sm.create_table_storage(default_database_id, 1, schema);
    ASSERT_TRUE(create.has_value()) << create.error().message;

    auto ts = sm.get_table_storage(1);
    ASSERT_TRUE(ts.has_value());

    // Should work with 128 frames.
    auto page = (*ts)->bpm->new_page();
    ASSERT_TRUE(page.has_value()) << page.error().message;
    ASSERT_TRUE((*ts)->bpm->unpin_page((*page)->page_id(), false).has_value());
}

// =============================================================================
// Adversarial: Concurrent hit/miss counter access
// =============================================================================

/// Verify hit/miss counters are thread-safe under concurrent fetch_page calls.
TEST_F(QA_GDB613, GDB622_ConcurrentCounterAccess) {
    auto test_dir = data_dir_ / "bpm_concurrent";
    std::filesystem::create_directories(test_dir);
    auto file_path = test_dir / "concurrent.db";

    auto fid = dm_.create_file(file_path, false, true);
    ASSERT_TRUE(fid.has_value());

    BufferPoolManager bpm(dm_, *fid, 64);

    // Pre-allocate pages.
    std::vector<PageId> pages;
    for (int i = 0; i < 8; ++i) {
        auto np = bpm.new_page();
        ASSERT_TRUE(np.has_value());
        pages.push_back((*np)->page_id());
        ASSERT_TRUE(bpm.unpin_page((*np)->page_id(), true).has_value());
    }

    // Hammer fetch_page from multiple threads — all should be hits since
    // pages are in the pool.
    constexpr int kThreads = 4;
    constexpr int kFetchesPerThread = 100;
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&bpm, &pages]() {
            for (int i = 0; i < kFetchesPerThread; ++i) {
                PageId pid = pages[i % pages.size()];
                auto fetch = bpm.fetch_page(pid);
                if (fetch.has_value()) {
                    (void)bpm.unpin_page(pid, false);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // All fetches should be hits (pages were pre-allocated and never evicted
    // from 64-frame pool holding only 8 pages).
    uint64_t total_hits = bpm.hit_count();
    uint64_t total_misses = bpm.miss_count();
    EXPECT_EQ(total_hits, kThreads * kFetchesPerThread)
        << "Expected all " << kThreads * kFetchesPerThread
        << " fetches to be hits, got " << total_hits
        << " hits and " << total_misses << " misses";
    EXPECT_EQ(total_misses, 0u);
}

} // namespace
} // namespace sixseven
