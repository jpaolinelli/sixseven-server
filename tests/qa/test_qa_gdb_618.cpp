/// QA adversarial tests for GDB-618: Async dirty page eviction in buffer pool
///
/// Tests are organized into sections:
///   1. Dirty ratio tracking edge cases (GDB-639)
///   2. Threshold-based flusher adversarial tests (GDB-640)
///   3. Clean-preferred eviction adversarial tests (GDB-641)
///   4. Dirty count accuracy under mixed operations
///   5. Data integrity after eviction
///   6. Concurrency stress tests

#include "sixseven/storage/buffer_pool.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>
#include <vector>

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class GDB618_BufferPool : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        temp_dir_ = std::filesystem::temp_directory_path() /
                    ("sixseven_qa_gdb618_" + std::string(info->test_suite_name()) + "_" +
                     info->name());
        std::filesystem::create_directories(temp_dir_);

        auto create_result = dm_.create_file(temp_dir_ / "test.gdb");
        ASSERT_TRUE(create_result.has_value()) << create_result.error().message;
        file_id_ = *create_result;
    }

    void TearDown() override {
        (void)dm_.close_file(file_id_);
        std::filesystem::remove_all(temp_dir_);
    }

    std::filesystem::path temp_dir_;
    DiskManager dm_;
    FileId file_id_ = 0;
};

// =============================================================================
// Section 1: Dirty ratio tracking edge cases (GDB-639)
// =============================================================================

/// AC: Background flusher triggers on dirty page ratio threshold.
/// Verify dirty_ratio() returns correct values at boundaries.
TEST_F(GDB618_BufferPool, DirtyRatioAccurateAtBoundaries) {
    constexpr uint32_t pool_size = 4;
    BufferPoolManager bpm(dm_, file_id_, pool_size);

    EXPECT_DOUBLE_EQ(bpm.dirty_ratio(), 0.0);

    // Fill all frames as dirty, checking ratio at each step.
    std::vector<PageId> pids;
    for (uint32_t i = 0; i < pool_size; ++i) {
        auto p = bpm.new_page();
        ASSERT_TRUE(p.has_value());
        pids.push_back((*p)->page_id());
        ASSERT_TRUE(bpm.unpin_page(pids.back(), true).has_value());

        double expected = static_cast<double>(i + 1) / static_cast<double>(pool_size);
        EXPECT_NEAR(bpm.dirty_ratio(), expected, 1e-9)
            << "After dirtying " << (i + 1) << " of " << pool_size << " frames";
    }

    // All frames dirty -> ratio = 1.0.
    EXPECT_DOUBLE_EQ(bpm.dirty_ratio(), 1.0);
    EXPECT_EQ(bpm.dirty_count(), pool_size);
}

/// Dirty count must not go negative when flushing a clean page.
TEST_F(GDB618_BufferPool, FlushCleanPageDoesNotDecrementCount) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto p = bpm.new_page();
    ASSERT_TRUE(p.has_value());
    PageId pid = (*p)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());

    // Flush a clean page — dirty_count should stay 0.
    ASSERT_TRUE(bpm.flush_page(pid).has_value());
    EXPECT_EQ(bpm.dirty_count(), 0u);
}

/// Dirty count accuracy after repeated dirty/flush cycles on the same page.
TEST_F(GDB618_BufferPool, DirtyCountAfterRepeatedDirtyFlushCycles) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto p = bpm.new_page();
    ASSERT_TRUE(p.has_value());
    PageId pid = (*p)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());

    for (int cycle = 0; cycle < 50; ++cycle) {
        // Fetch, dirty, unpin.
        auto f = bpm.fetch_page(pid);
        ASSERT_TRUE(f.has_value());
        ASSERT_TRUE(bpm.unpin_page(pid, true).has_value());
        EXPECT_EQ(bpm.dirty_count(), 1u) << "cycle " << cycle << " after dirty";

        // Flush.
        ASSERT_TRUE(bpm.flush_page(pid).has_value());
        EXPECT_EQ(bpm.dirty_count(), 0u) << "cycle " << cycle << " after flush";
    }
}

/// Dirty count accuracy when multiple pages are dirtied, some flushed, some deleted.
TEST_F(GDB618_BufferPool, DirtyCountAccuracyMixedOperations) {
    BufferPoolManager bpm(dm_, file_id_, 8);

    // Create 6 dirty pages.
    std::vector<PageId> pids;
    for (int i = 0; i < 6; ++i) {
        auto p = bpm.new_page();
        ASSERT_TRUE(p.has_value());
        pids.push_back((*p)->page_id());
        ASSERT_TRUE(bpm.unpin_page(pids.back(), true).has_value());
    }
    EXPECT_EQ(bpm.dirty_count(), 6u);

    // Flush 2.
    ASSERT_TRUE(bpm.flush_page(pids[0]).has_value());
    ASSERT_TRUE(bpm.flush_page(pids[1]).has_value());
    EXPECT_EQ(bpm.dirty_count(), 4u);

    // Delete 1 dirty page.
    ASSERT_TRUE(bpm.delete_page(pids[2]).has_value());
    EXPECT_EQ(bpm.dirty_count(), 3u);

    // Delete 1 clean page (already flushed).
    ASSERT_TRUE(bpm.delete_page(pids[0]).has_value());
    EXPECT_EQ(bpm.dirty_count(), 3u);

    // Flush all remaining.
    ASSERT_TRUE(bpm.flush_all().has_value());
    EXPECT_EQ(bpm.dirty_count(), 0u);
}

// =============================================================================
// Section 2: Threshold-based flusher adversarial tests (GDB-640)
// =============================================================================

/// AC: Background flusher triggers on dirty page ratio threshold.
/// Threshold at 0.0 should cause flusher to wake on every dirty page.
TEST_F(GDB618_BufferPool, FlusherThresholdZeroFlushesImmediately) {
    BufferPoolManager bpm(dm_, file_id_, 4);
    bpm.set_dirty_flush_threshold(0.0);

    auto flusher_result = bpm.start_flusher(std::chrono::seconds(60));
    ASSERT_TRUE(flusher_result.has_value());

    // Even a single dirty page should trigger the flusher (0% >= 0.0).
    // But new_page doesn't dirty the page on creation, unpin with dirty does.
    auto p = bpm.new_page();
    ASSERT_TRUE(p.has_value());
    ASSERT_TRUE(bpm.unpin_page((*p)->page_id(), true).has_value());

    // Wait for flusher to react.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(bpm.dirty_count(), 0u);

    bpm.stop_flusher();
}

/// Threshold at exactly 1.0: flusher only wakes when ALL frames are dirty.
TEST_F(GDB618_BufferPool, FlusherThresholdOneRequiresAllDirty) {
    constexpr uint32_t pool_size = 4;
    BufferPoolManager bpm(dm_, file_id_, pool_size);
    bpm.set_dirty_flush_threshold(1.0);

    auto flusher_result = bpm.start_flusher(std::chrono::seconds(60));
    ASSERT_TRUE(flusher_result.has_value());

    // Dirty 3 of 4 frames (75% < 100%).
    std::vector<PageId> pids;
    for (uint32_t i = 0; i < pool_size - 1; ++i) {
        auto p = bpm.new_page();
        ASSERT_TRUE(p.has_value());
        pids.push_back((*p)->page_id());
        ASSERT_TRUE(bpm.unpin_page(pids.back(), true).has_value());
    }

    // Flusher should NOT have woken (75% < 100%).
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(bpm.dirty_count(), pool_size - 1)
        << "Flusher should not wake below threshold=1.0";

    // Dirty the last frame -> 100% = threshold.
    auto p4 = bpm.new_page();
    ASSERT_TRUE(p4.has_value());
    ASSERT_TRUE(bpm.unpin_page((*p4)->page_id(), true).has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(bpm.dirty_count(), 0u) << "Flusher should wake at threshold=1.0";

    bpm.stop_flusher();
}

/// Flusher must not be startable twice.
TEST_F(GDB618_BufferPool, FlusherDoubleStartFails) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    ASSERT_TRUE(bpm.start_flusher(std::chrono::milliseconds(100)).has_value());
    auto second = bpm.start_flusher(std::chrono::milliseconds(100));
    EXPECT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, StatusCode::ALREADY_EXISTS);

    bpm.stop_flusher();
}

/// stop_flusher is idempotent — calling it when not running is a no-op.
TEST_F(GDB618_BufferPool, StopFlusherIdempotent) {
    BufferPoolManager bpm(dm_, file_id_, 4);
    // Never started — should not crash.
    bpm.stop_flusher();
    bpm.stop_flusher();
}

/// Flusher stop flushes all dirty pages including pinned ones.
TEST_F(GDB618_BufferPool, StopFlusherFlushesPinnedPages) {
    BufferPoolManager bpm(dm_, file_id_, 4);
    bpm.set_dirty_flush_threshold(1.1); // Never wake on threshold.

    ASSERT_TRUE(bpm.start_flusher(std::chrono::seconds(60)).has_value());

    // Create a pinned dirty page (don't unpin).
    auto p = bpm.new_page();
    ASSERT_TRUE(p.has_value());
    PageId pid = (*p)->page_id();
    // Write some data so we can verify later.
    auto tuple = std::vector<uint8_t>(100, 0xBB);
    ASSERT_TRUE((*p)->insert_tuple(tuple).has_value());
    // Mark dirty via fetch/unpin won't work since it's still pinned.
    // We need to unpin as dirty, then re-pin.
    ASSERT_TRUE(bpm.unpin_page(pid, true).has_value());
    auto f = bpm.fetch_page(pid); // Pin again.
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(bpm.dirty_count(), 1u);

    // stop_flusher should flush even pinned pages.
    bpm.stop_flusher();
    EXPECT_EQ(bpm.dirty_count(), 0u);

    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());
}

/// Multiple threshold crossings should each wake the flusher.
TEST_F(GDB618_BufferPool, FlusherWakesOnRepeatedThresholdCrossings) {
    BufferPoolManager bpm(dm_, file_id_, 4);
    bpm.set_dirty_flush_threshold(0.5); // Wake at 2/4 dirty.

    ASSERT_TRUE(bpm.start_flusher(std::chrono::seconds(60)).has_value());

    for (int round = 0; round < 5; ++round) {
        // Create 2 dirty pages to cross threshold.
        for (int i = 0; i < 2; ++i) {
            auto p = bpm.new_page();
            ASSERT_TRUE(p.has_value());
            ASSERT_TRUE(bpm.unpin_page((*p)->page_id(), true).has_value());
        }

        // Wait for flusher.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        EXPECT_EQ(bpm.dirty_count(), 0u)
            << "Round " << round << ": flusher should have flushed";

        // Delete pages to make room for next round.
        ASSERT_TRUE(bpm.flush_all().has_value());
        // Delete all pages from the pool to free frames.
        // We need to iterate page_table — use pool_page_count to check.
    }

    bpm.stop_flusher();
}

// =============================================================================
// Section 3: Clean-preferred eviction adversarial tests (GDB-641)
// =============================================================================

/// AC: Eviction path prefers clean frames over dirty frames.
/// When all evictable frames are clean, eviction should work normally by K-distance.
TEST(GDB618_LRUKReplacer, EvictAllCleanNoSkipPredicate) {
    LRUKReplacer replacer(4, 2);

    replacer.record_access(0);
    replacer.record_access(1);
    replacer.record_access(2);
    replacer.set_evictable(0, true);
    replacer.set_evictable(1, true);
    replacer.set_evictable(2, true);

    // All clean (skip returns false for all). Should evict frame 0 (oldest).
    auto result = replacer.evict([](FrameId) { return false; });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0u);
}

/// When all evictable frames are dirty (skip=true), falls back to second pass.
TEST(GDB618_LRUKReplacer, EvictAllDirtyFallsBack) {
    LRUKReplacer replacer(4, 2);

    replacer.record_access(0);
    replacer.record_access(1);
    replacer.record_access(2);
    replacer.set_evictable(0, true);
    replacer.set_evictable(1, true);
    replacer.set_evictable(2, true);

    // All dirty (skip returns true for all). Should still evict frame 0 (oldest).
    auto result = replacer.evict([](FrameId) { return true; });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0u);
}

/// K-distance ordering is respected within each pass (clean pass and dirty pass).
TEST(GDB618_LRUKReplacer, EvictKDistanceWithinDirtyPassCorrect) {
    LRUKReplacer replacer(5, 2);

    // Frames 0,1,2 are dirty; frame 3 is clean but non-evictable.
    // Among dirty frames, frame 0 is oldest -> evict frame 0.
    replacer.record_access(0); // oldest dirty
    replacer.record_access(1);
    replacer.record_access(2); // newest dirty

    replacer.set_evictable(0, true);
    replacer.set_evictable(1, true);
    replacer.set_evictable(2, true);

    auto result = replacer.evict([](FrameId fid) { return fid <= 2; });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0u) << "Should evict oldest dirty frame (frame 0)";
}

/// Mixed K-distance: some frames have K accesses, some have < K.
/// Infinite K-distance frames should be evicted first within each pass.
TEST(GDB618_LRUKReplacer, EvictInfiniteKDistancePreferredWithinPass) {
    LRUKReplacer replacer(5, 2);

    // Frame 0: 2 accesses (finite K-distance) — dirty.
    replacer.record_access(0);
    replacer.record_access(0);
    replacer.set_evictable(0, true);

    // Frame 1: 1 access (infinite K-distance) — clean.
    replacer.record_access(1);
    replacer.set_evictable(1, true);

    // Frame 2: 1 access (infinite K-distance) — dirty.
    replacer.record_access(2);
    replacer.set_evictable(2, true);

    // Clean pass: only frame 1 is clean with infinite K-distance -> evict frame 1.
    auto result = replacer.evict([](FrameId fid) { return fid == 0 || fid == 2; });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1u);
}

/// AC: Eviction path prefers clean frames over dirty frames (integration test).
/// Pool of 3: 2 dirty, 1 clean. Eviction must pick the clean one.
TEST_F(GDB618_BufferPool, EvictionPicksCleanOverOlderDirty) {
    BufferPoolManager bpm(dm_, file_id_, 3);

    // Page 1: dirty, oldest.
    auto p1 = bpm.new_page();
    ASSERT_TRUE(p1.has_value());
    PageId pid1 = (*p1)->page_id();
    auto data1 = std::vector<uint8_t>(64, 0x11);
    ASSERT_TRUE((*p1)->insert_tuple(data1).has_value());
    ASSERT_TRUE(bpm.unpin_page(pid1, true).has_value());

    // Page 2: dirty.
    auto p2 = bpm.new_page();
    ASSERT_TRUE(p2.has_value());
    PageId pid2 = (*p2)->page_id();
    auto data2 = std::vector<uint8_t>(64, 0x22);
    ASSERT_TRUE((*p2)->insert_tuple(data2).has_value());
    ASSERT_TRUE(bpm.unpin_page(pid2, true).has_value());

    // Page 3: clean (youngest).
    auto p3 = bpm.new_page();
    ASSERT_TRUE(p3.has_value());
    PageId pid3 = (*p3)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid3, false).has_value());

    // Force eviction by allocating a 4th page. Should evict clean page 3.
    auto p4 = bpm.new_page();
    ASSERT_TRUE(p4.has_value());
    ASSERT_TRUE(bpm.unpin_page((*p4)->page_id(), false).has_value());

    // Dirty pages 1 and 2 should still be in pool with their data intact.
    auto f1 = bpm.fetch_page(pid1);
    ASSERT_TRUE(f1.has_value());
    auto t1 = (*f1)->get_tuple(0);
    ASSERT_TRUE(t1.has_value());
    EXPECT_EQ((*t1)[0], 0x11);
    ASSERT_TRUE(bpm.unpin_page(pid1, false).has_value());

    auto f2 = bpm.fetch_page(pid2);
    ASSERT_TRUE(f2.has_value());
    auto t2 = (*f2)->get_tuple(0);
    ASSERT_TRUE(t2.has_value());
    EXPECT_EQ((*t2)[0], 0x22);
    ASSERT_TRUE(bpm.unpin_page(pid2, false).has_value());
}

/// AC: No data loss — dirty pages are still flushed before eviction.
/// When only dirty frames are available, eviction must flush before reusing.
TEST_F(GDB618_BufferPool, EvictionOfDirtyPageFlushesDataToDisk) {
    BufferPoolManager bpm(dm_, file_id_, 2);

    // Fill pool with 2 dirty pages.
    auto p1 = bpm.new_page();
    ASSERT_TRUE(p1.has_value());
    PageId pid1 = (*p1)->page_id();
    auto data = std::vector<uint8_t>(80, 0xCC);
    ASSERT_TRUE((*p1)->insert_tuple(data).has_value());
    ASSERT_TRUE(bpm.unpin_page(pid1, true).has_value());

    auto p2 = bpm.new_page();
    ASSERT_TRUE(p2.has_value());
    ASSERT_TRUE(bpm.unpin_page((*p2)->page_id(), true).has_value());

    // Force eviction of p1 (oldest dirty). This must flush p1 to disk.
    auto p3 = bpm.new_page();
    ASSERT_TRUE(p3.has_value());
    ASSERT_TRUE(bpm.unpin_page((*p3)->page_id(), false).has_value());

    // Now fetch p1 back from disk — data must be intact.
    // p2 might also have been evicted, so we might need another eviction.
    auto f1 = bpm.fetch_page(pid1);
    ASSERT_TRUE(f1.has_value()) << "Evicted dirty page should be readable from disk";
    auto t1 = (*f1)->get_tuple(0);
    ASSERT_TRUE(t1.has_value());
    EXPECT_EQ((*t1)[0], 0xCC) << "Data must survive dirty eviction";
    ASSERT_TRUE(bpm.unpin_page(pid1, false).has_value());
}

/// Dirty count must be decremented when a dirty frame is evicted.
TEST_F(GDB618_BufferPool, DirtyCountDecrementedOnDirtyEviction) {
    BufferPoolManager bpm(dm_, file_id_, 2);

    // Both pages dirty.
    auto p1 = bpm.new_page();
    ASSERT_TRUE(p1.has_value());
    ASSERT_TRUE(bpm.unpin_page((*p1)->page_id(), true).has_value());

    auto p2 = bpm.new_page();
    ASSERT_TRUE(p2.has_value());
    ASSERT_TRUE(bpm.unpin_page((*p2)->page_id(), true).has_value());
    EXPECT_EQ(bpm.dirty_count(), 2u);

    // Evict: should flush one dirty page and decrement count.
    auto p3 = bpm.new_page();
    ASSERT_TRUE(p3.has_value());
    EXPECT_EQ(bpm.dirty_count(), 1u) << "Eviction of dirty frame must decrement dirty_count";
    ASSERT_TRUE(bpm.unpin_page((*p3)->page_id(), false).has_value());
}

// =============================================================================
// Section 4: Eviction with skip predicate edge cases
// =============================================================================

/// Null skip predicate (from the no-argument evict()) still works.
TEST(GDB618_LRUKReplacer, EvictWithoutPredicateStillWorks) {
    LRUKReplacer replacer(3, 2);
    replacer.record_access(0);
    replacer.record_access(1);
    replacer.set_evictable(0, true);
    replacer.set_evictable(1, true);

    auto result = replacer.evict();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0u); // Oldest access.
}

/// Single frame that is both evictable and skipped — must fall back.
TEST(GDB618_LRUKReplacer, SingleSkippedFrameFallsBack) {
    LRUKReplacer replacer(2, 2);
    replacer.record_access(0);
    replacer.set_evictable(0, true);

    auto result = replacer.evict([](FrameId) { return true; });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0u);
}

/// No evictable frames — error regardless of predicate.
TEST(GDB618_LRUKReplacer, NoEvictableFramesWithPredicateFails) {
    LRUKReplacer replacer(3, 2);
    replacer.record_access(0);
    replacer.set_evictable(0, false); // Pinned.

    auto result = replacer.evict([](FrameId) { return false; });
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// =============================================================================
// Section 5: Data integrity stress test
// =============================================================================

/// Stress: rapidly dirty, evict, and re-fetch pages verifying data integrity.
TEST_F(GDB618_BufferPool, StressEvictionDataIntegrity) {
    constexpr uint32_t pool_size = 4;
    constexpr int total_pages = 20;
    BufferPoolManager bpm(dm_, file_id_, pool_size);

    // Create total_pages, each with a unique byte pattern.
    std::vector<PageId> pids;
    for (int i = 0; i < total_pages; ++i) {
        auto p = bpm.new_page();
        ASSERT_TRUE(p.has_value());
        PageId pid = (*p)->page_id();
        pids.push_back(pid);

        auto data = std::vector<uint8_t>(64, static_cast<uint8_t>(i));
        ASSERT_TRUE((*p)->insert_tuple(data).has_value());
        // Mark dirty so eviction must flush.
        ASSERT_TRUE(bpm.unpin_page(pid, true).has_value());
    }

    // Now re-fetch every page and verify data.
    for (int i = 0; i < total_pages; ++i) {
        auto f = bpm.fetch_page(pids[i]);
        ASSERT_TRUE(f.has_value()) << "Failed to fetch page " << pids[i];
        auto t = (*f)->get_tuple(0);
        ASSERT_TRUE(t.has_value()) << "Failed to get tuple from page " << pids[i];
        EXPECT_EQ((*t)[0], static_cast<uint8_t>(i))
            << "Data corruption for page " << pids[i] << " (index " << i << ")";
        ASSERT_TRUE(bpm.unpin_page(pids[i], false).has_value());
    }
}

// =============================================================================
// Section 6: Concurrency stress tests
// =============================================================================

/// AC: ThreadSanitizer clean.
/// Concurrent unpin_page calls dirtying different pages with flusher running.
TEST_F(GDB618_BufferPool, ConcurrentDirtyWithFlusherRunning) {
    constexpr uint32_t pool_size = 16;
    BufferPoolManager bpm(dm_, file_id_, pool_size);
    bpm.set_dirty_flush_threshold(0.5);

    ASSERT_TRUE(bpm.start_flusher(std::chrono::seconds(60)).has_value());

    // Pre-allocate pages.
    std::vector<PageId> pids;
    for (uint32_t i = 0; i < pool_size; ++i) {
        auto p = bpm.new_page();
        ASSERT_TRUE(p.has_value());
        pids.push_back((*p)->page_id());
        ASSERT_TRUE(bpm.unpin_page(pids.back(), false).has_value());
    }

    // Concurrently dirty pages from multiple threads.
    constexpr int num_threads = 4;
    constexpr int ops_per_thread = 50;
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&bpm, &pids, t]() {
            for (int op = 0; op < ops_per_thread; ++op) {
                PageId pid = pids[(t * ops_per_thread + op) % pool_size];
                auto f = bpm.fetch_page(pid);
                if (f.has_value()) {
                    (void)bpm.unpin_page(pid, true);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    bpm.stop_flusher();

    // After flusher stops, all pages should be flushed.
    EXPECT_EQ(bpm.dirty_count(), 0u);
}

/// Concurrent fetch/unpin with evictions and flusher.
TEST_F(GDB618_BufferPool, ConcurrentEvictionWithFlusher) {
    constexpr uint32_t pool_size = 4;
    BufferPoolManager bpm(dm_, file_id_, pool_size);
    bpm.set_dirty_flush_threshold(0.5);

    ASSERT_TRUE(bpm.start_flusher(std::chrono::milliseconds(10)).has_value());

    // Pre-create more pages than pool can hold.
    std::vector<PageId> pids;
    for (int i = 0; i < 8; ++i) {
        auto p = bpm.new_page();
        ASSERT_TRUE(p.has_value());
        pids.push_back((*p)->page_id());
        ASSERT_TRUE(bpm.unpin_page(pids.back(), true).has_value());
    }

    // Concurrent random access pattern causing evictions.
    constexpr int num_threads = 3;
    constexpr int ops_per_thread = 30;
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&bpm, &pids, &errors, t]() {
            for (int op = 0; op < ops_per_thread; ++op) {
                PageId pid = pids[(t + op) % pids.size()];
                auto f = bpm.fetch_page(pid);
                if (f.has_value()) {
                    auto r = bpm.unpin_page(pid, (op % 2 == 0));
                    if (!r.has_value()) {
                        errors.fetch_add(1);
                    }
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    bpm.stop_flusher();

    // No fatal errors expected (some might fail due to contention, which is OK).
    // The important thing is no crash and dirty_count is consistent.
    EXPECT_EQ(bpm.dirty_count(), 0u) << "Flusher stop should have flushed everything";
}

/// Stress: start and stop flusher rapidly.
TEST_F(GDB618_BufferPool, FlusherStartStopStress) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(bpm.start_flusher(std::chrono::milliseconds(5)).has_value())
            << "Iteration " << i;

        // Create a dirty page while flusher is running.
        auto p = bpm.new_page();
        if (p.has_value()) {
            (void)bpm.unpin_page((*p)->page_id(), true);
        }

        bpm.stop_flusher();
        EXPECT_EQ(bpm.dirty_count(), 0u) << "Iteration " << i;
    }
}

// =============================================================================
// Section 7: Threshold setter/getter edge cases
// =============================================================================

/// Threshold values at floating-point boundaries.
TEST_F(GDB618_BufferPool, ThresholdEdgeValues) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    bpm.set_dirty_flush_threshold(0.0);
    EXPECT_DOUBLE_EQ(bpm.dirty_flush_threshold(), 0.0);

    bpm.set_dirty_flush_threshold(1.0);
    EXPECT_DOUBLE_EQ(bpm.dirty_flush_threshold(), 1.0);

    // Values > 1.0 effectively disable threshold-based waking.
    bpm.set_dirty_flush_threshold(2.0);
    EXPECT_DOUBLE_EQ(bpm.dirty_flush_threshold(), 2.0);

    // Negative value — dirty_ratio() is always >= 0.0, so this would always trigger.
    bpm.set_dirty_flush_threshold(-1.0);
    EXPECT_DOUBLE_EQ(bpm.dirty_flush_threshold(), -1.0);
}

/// AC: fsync never occurs while holding the buffer pool latch (or duration is minimized).
/// Verify that clean-preferred eviction avoids flushing when clean frames exist.
/// We test this indirectly: with a mix of clean/dirty frames, eviction should
/// not need to call write_page_impl (no fsync under latch).
TEST_F(GDB618_BufferPool, CleanEvictionAvoidsFlushUnderLatch) {
    BufferPoolManager bpm(dm_, file_id_, 3);

    // Page 1: dirty.
    auto p1 = bpm.new_page();
    ASSERT_TRUE(p1.has_value());
    PageId pid1 = (*p1)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid1, true).has_value());

    // Page 2: clean.
    auto p2 = bpm.new_page();
    ASSERT_TRUE(p2.has_value());
    PageId pid2 = (*p2)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid2, false).has_value());

    // Page 3: dirty.
    auto p3 = bpm.new_page();
    ASSERT_TRUE(p3.has_value());
    PageId pid3 = (*p3)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid3, true).has_value());

    uint32_t dirty_before = bpm.dirty_count();
    EXPECT_EQ(dirty_before, 2u);

    // Allocate 4th page — should evict clean page 2 (no flush needed).
    auto p4 = bpm.new_page();
    ASSERT_TRUE(p4.has_value());
    ASSERT_TRUE(bpm.unpin_page((*p4)->page_id(), false).has_value());

    // Dirty count should not decrease (no dirty page was evicted).
    EXPECT_EQ(bpm.dirty_count(), dirty_before)
        << "Clean eviction should not change dirty count";

    // Dirty pages 1 and 3 should still be in pool (cache hits).
    uint64_t hits_before = bpm.hit_count();
    auto f1 = bpm.fetch_page(pid1);
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(bpm.hit_count(), hits_before + 1)
        << "Dirty page 1 should still be in pool (cache hit)";
    ASSERT_TRUE(bpm.unpin_page(pid1, false).has_value());

    auto f3 = bpm.fetch_page(pid3);
    ASSERT_TRUE(f3.has_value());
    EXPECT_EQ(bpm.hit_count(), hits_before + 2)
        << "Dirty page 3 should still be in pool (cache hit)";
    ASSERT_TRUE(bpm.unpin_page(pid3, false).has_value());
}
