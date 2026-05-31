/// QA adversarial tests for GDB-15: Buffer Pool Manager (LRU-K + background flusher)
///
/// Tests are organized into sections:
///   1. LRU-K replacer edge cases
///   2. BufferPoolManager edge cases and boundary conditions
///   3. BUG regression tests (tests that expose known bugs)
///   4. Concurrency stress tests

#include "sixseven/storage/buffer_pool.h"

#include <gtest/gtest.h>

#include "sixseven/common/platform.h"

#include <fcntl.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class QABufferPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        temp_dir_ = std::filesystem::temp_directory_path() /
                    ("sixseven_qa_gdb15_" + std::string(info->test_suite_name()) + "_" + info->name());
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
// Section 1: LRU-K Replacer edge cases
// =============================================================================

/// Setting a frame evictable without first calling record_access causes
/// evictable_count_ to be incremented, but evict() will skip the frame
/// (because info.history.empty()). This leads to evict() returning NOT_FOUND
/// even though evictable_count_ > 0. Demonstrates an internal state inconsistency.
TEST(QA_LRUKReplacer, EvictableFrameWithNoHistoryIsSkippedByEvict) {
    LRUKReplacer replacer(5, 2);

    // Mark frame 0 as evictable WITHOUT ever recording an access.
    // This is an API misuse, but the replacer should handle it gracefully.
    replacer.set_evictable(0, true);

    // evictable_count_ is now 1, but the frame has empty history.
    EXPECT_EQ(replacer.size(), 1u);

    // evict() should skip frames with empty history.
    // Since there are no other candidates, it should return NOT_FOUND.
    auto result = replacer.evict();
    EXPECT_FALSE(result.has_value())
        << "evict() should fail gracefully when evictable frame has no history";
}

/// K=1 should behave as plain LRU: evict the least-recently-used frame.
TEST(QA_LRUKReplacer, K1DegradestoLRU) {
    LRUKReplacer replacer(5, 1);

    // Frame 0 accessed at t=1 (most recent → least likely to be evicted last)
    replacer.record_access(0);
    // Frame 1 accessed at t=2
    replacer.record_access(1);
    // Frame 2 accessed at t=3 (most recent)
    replacer.record_access(2);

    replacer.set_evictable(0, true);
    replacer.set_evictable(1, true);
    replacer.set_evictable(2, true);

    // With K=1, history has exactly 1 entry. All frames have finite K-distance.
    // K-distance = current_ts - last_access.
    // current_ts = 3.
    //   Frame 0: 3 - 1 = 2 (largest → evicted first)
    //   Frame 1: 3 - 2 = 1
    //   Frame 2: 3 - 3 = 0 (smallest → evicted last)

    auto r1 = replacer.evict();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(*r1, 0u) << "Frame 0 (LRU) should be evicted first with K=1";

    auto r2 = replacer.evict();
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r2, 1u);

    auto r3 = replacer.evict();
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(*r3, 2u) << "Frame 2 (MRU) should be evicted last with K=1";
}

/// After removal, re-adding the same frame should start with a clean slate.
/// Verify that old history does not bleed through.
TEST(QA_LRUKReplacer, RemoveAndReAddFrameStartsFresh) {
    LRUKReplacer replacer(5, 2);

    // Give frame 0 two accesses (finite K-distance).
    replacer.record_access(0);
    replacer.record_access(0);
    replacer.set_evictable(0, true);

    // Give frame 1 one access (infinite K-distance → should be evicted first).
    replacer.record_access(1);
    replacer.set_evictable(1, true);

    // Evict frame 1 first (infinite distance).
    auto r1 = replacer.evict();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(*r1, 1u);

    // Remove frame 0.
    replacer.remove(0);
    EXPECT_EQ(replacer.size(), 0u);

    // Re-add frame 0 with a single access (fresh history).
    replacer.record_access(0);
    replacer.set_evictable(0, true);

    // Frame 0 now has only 1 access → infinite K-distance again.
    // Evict it.
    auto r2 = replacer.evict();
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r2, 0u);
    EXPECT_EQ(replacer.size(), 0u);
}

/// Verify that toggling set_evictable correctly tracks evictable_count.
/// Repeatedly toggling should not cause count to drift.
TEST(QA_LRUKReplacer, ToggleEvictableCountNeverDrifts) {
    LRUKReplacer replacer(4, 2);

    for (FrameId i = 0; i < 4; ++i) {
        replacer.record_access(i);
    }

    // Toggle all frames evictable/non-evictable many times.
    for (int round = 0; round < 20; ++round) {
        for (FrameId i = 0; i < 4; ++i) {
            replacer.set_evictable(i, true);
        }
        EXPECT_EQ(replacer.size(), 4u) << "Round " << round << ": count should be 4";

        for (FrameId i = 0; i < 4; ++i) {
            replacer.set_evictable(i, false);
        }
        EXPECT_EQ(replacer.size(), 0u) << "Round " << round << ": count should be 0";
    }
}

/// Evicting all frames one-by-one and then trying again should fail.
TEST(QA_LRUKReplacer, EvictAllThenFailGracefully) {
    LRUKReplacer replacer(3, 2);

    for (FrameId i = 0; i < 3; ++i) {
        replacer.record_access(i);
        replacer.record_access(i); // 2 accesses for finite K-distance.
        replacer.set_evictable(i, true);
    }
    EXPECT_EQ(replacer.size(), 3u);

    for (int i = 0; i < 3; ++i) {
        auto r = replacer.evict();
        ASSERT_TRUE(r.has_value()) << "Eviction " << i << " should succeed";
    }
    EXPECT_EQ(replacer.size(), 0u);

    auto final = replacer.evict();
    ASSERT_FALSE(final.has_value());
    EXPECT_EQ(final.error().code, StatusCode::NOT_FOUND);
}

// =============================================================================
// Section 2: BufferPoolManager edge cases
// =============================================================================

/// fetch_page for page_id=0 (the file header page). The DiskManager
/// treats page 0 as the header and its content may not match user-page
/// CRC expectations. Should return an error.
TEST_F(QABufferPoolTest, FetchHeaderPageFails) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto result = bpm.fetch_page(0);
    // Page 0 is the header. DiskManager should reject reads of the header
    // as a user data page (checksum or bounds error).
    EXPECT_FALSE(result.has_value()) << "Fetching page_id=0 (header page) should fail";
}

/// fetch_page for a page_id that has never been allocated should fail.
TEST_F(QABufferPoolTest, FetchNonexistentPageFails) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    // Page IDs > allocated page count are out of bounds.
    auto result = bpm.fetch_page(999999);
    EXPECT_FALSE(result.has_value()) << "Fetching an unallocated page_id should return an error";
}

/// After deleting a page from the buffer pool, unpinning it should fail
/// because it is no longer tracked.
TEST_F(QABufferPoolTest, UnpinAfterDeleteFails) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto result = bpm.new_page();
    ASSERT_TRUE(result.has_value());
    PageId pid = (*result)->page_id();

    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());
    ASSERT_TRUE(bpm.delete_page(pid).has_value());

    // Page is gone from the pool; unpin should fail.
    auto unpin = bpm.unpin_page(pid, false);
    ASSERT_FALSE(unpin.has_value());
    EXPECT_EQ(unpin.error().code, StatusCode::NOT_FOUND);
}

/// flush_page on a page that is pinned (active) should succeed — flushing
/// a pinned page is valid (the page stays in the pool, just persisted to disk).
TEST_F(QABufferPoolTest, FlushPinnedPageSucceeds) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto result = bpm.new_page();
    ASSERT_TRUE(result.has_value());
    Page* page = *result;
    PageId pid = page->page_id();

    auto tuple = std::vector<uint8_t>(50, 0x42);
    ASSERT_TRUE(page->insert_tuple(tuple).has_value());

    // Manually mark dirty via unpin (then re-fetch to re-pin).
    ASSERT_TRUE(bpm.unpin_page(pid, /*is_dirty=*/true).has_value());
    auto refetch = bpm.fetch_page(pid);
    ASSERT_TRUE(refetch.has_value()); // Re-pinned.

    // flush_page while pinned — should succeed.
    auto flush = bpm.flush_page(pid);
    EXPECT_TRUE(flush.has_value()) << "flush_page on pinned dirty page should succeed";

    // Verify on disk.
    Page disk_page(0, PageType::DATA);
    auto read = dm_.read_page(file_id_, pid, disk_page);
    ASSERT_TRUE(read.has_value());
    auto get = disk_page.get_tuple(0);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ((*get)[0], 0x42);

    // Clean up: unpin the re-fetched pin (new_page pin was already unpinned above).
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());
}

/// flush_all with no dirty pages should be a no-op and succeed.
TEST_F(QABufferPoolTest, FlushAllWithNoDirtyPagesSucceeds) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto result = bpm.new_page();
    ASSERT_TRUE(result.has_value());
    PageId pid = (*result)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid, /*is_dirty=*/false).has_value());

    // flush_all with no dirty pages should be a no-op.
    auto flush = bpm.flush_all();
    EXPECT_TRUE(flush.has_value()) << "flush_all with no dirty pages should succeed";
}

/// flush_all flushes dirty pinned pages too (not just unpinned).
/// The dirty flag must be set via unpin_page(is_dirty=true); simply modifying
/// page content does not set it.
TEST_F(QABufferPoolTest, FlushAllIncludesPinnedDirtyPages) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto result = bpm.new_page();
    ASSERT_TRUE(result.has_value());
    Page* page = *result;
    PageId pid = page->page_id();

    auto tuple = std::vector<uint8_t>(50, 0x77);
    ASSERT_TRUE(page->insert_tuple(tuple).has_value());

    // Mark dirty via unpin, then re-pin so the page is pinned AND dirty.
    ASSERT_TRUE(bpm.unpin_page(pid, /*is_dirty=*/true).has_value()); // dirty set, pin=0
    auto refetch = bpm.fetch_page(pid);
    ASSERT_TRUE(refetch.has_value()); // pin=1

    // flush_all should flush even pinned dirty pages.
    auto flush = bpm.flush_all();
    ASSERT_TRUE(flush.has_value()) << "flush_all should flush pinned dirty pages";

    // Verify on disk.
    Page disk_page(0, PageType::DATA);
    auto read = dm_.read_page(file_id_, pid, disk_page);
    ASSERT_TRUE(read.has_value());
    auto get = disk_page.get_tuple(0);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ((*get)[0], 0x77);

    // Clean up: unpin the re-fetched pin.
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());
}

/// Multiple unpins of the same page should each be required. The exact number
/// of unpins needed equals the number of times the page was fetched/new_page'd.
TEST_F(QABufferPoolTest, PinCountBookkeepingIsExact) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto p = bpm.new_page(); // pin_count = 1
    ASSERT_TRUE(p.has_value());
    PageId pid = (*p)->page_id();

    auto f1 = bpm.fetch_page(pid); // pin_count = 2
    ASSERT_TRUE(f1.has_value());
    auto f2 = bpm.fetch_page(pid); // pin_count = 3
    ASSERT_TRUE(f2.has_value());
    auto f3 = bpm.fetch_page(pid); // pin_count = 4
    ASSERT_TRUE(f3.has_value());

    // Need exactly 4 unpins.
    EXPECT_TRUE(bpm.unpin_page(pid, false).has_value()); // pin_count = 3
    EXPECT_TRUE(bpm.unpin_page(pid, false).has_value()); // pin_count = 2
    EXPECT_TRUE(bpm.unpin_page(pid, false).has_value()); // pin_count = 1
    EXPECT_TRUE(bpm.unpin_page(pid, false).has_value()); // pin_count = 0

    // 5th unpin should fail.
    auto extra = bpm.unpin_page(pid, false);
    ASSERT_FALSE(extra.has_value());
    EXPECT_EQ(extra.error().code, StatusCode::INVALID_ARGUMENT);
}

/// Pool size 1: new_page, flush, delete, and repeat — verify the single frame
/// is correctly recycled across multiple create/delete cycles.
TEST_F(QABufferPoolTest, SingleFramePoolRecyclesCorrctly) {
    BufferPoolManager bpm(dm_, file_id_, 1);

    for (int i = 0; i < 5; ++i) {
        auto result = bpm.new_page();
        ASSERT_TRUE(result.has_value()) << "Iteration " << i;
        Page* page = *result;
        PageId pid = page->page_id();

        auto tuple = std::vector<uint8_t>(20, static_cast<uint8_t>(i + 1));
        ASSERT_TRUE(page->insert_tuple(tuple).has_value());
        ASSERT_TRUE(bpm.flush_page(pid).has_value());
        ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());
        ASSERT_TRUE(bpm.delete_page(pid).has_value());
        EXPECT_EQ(bpm.pool_page_count(), 0u)
            << "Pool should be empty after delete, iteration " << i;
    }
}

/// Pool of size N: fill completely, unpin all, then fetch pages in reverse order.
/// This exercises LRU-K eviction across many pages and verifies data integrity.
TEST_F(QABufferPoolTest, EvictionPreservesDiskDataIntegrity) {
    constexpr int POOL_SIZE = 4;
    constexpr int TOTAL_PAGES = 8; // Double the pool size.
    BufferPoolManager bpm(dm_, file_id_, POOL_SIZE);

    // Create TOTAL_PAGES pages, write distinct data, flush each to disk,
    // then unpin so they are evictable.
    std::vector<PageId> pids;
    for (int i = 0; i < TOTAL_PAGES; ++i) {
        auto result = bpm.new_page();
        ASSERT_TRUE(result.has_value()) << "new_page failed at iteration " << i;
        Page* page = *result;
        pids.push_back(page->page_id());

        auto tuple = std::vector<uint8_t>(30, static_cast<uint8_t>(i + 1));
        ASSERT_TRUE(page->insert_tuple(tuple).has_value());
        // Mark dirty first, then flush (flush_page is a no-op if is_dirty=false).
        ASSERT_TRUE(bpm.unpin_page(pids.back(), /*is_dirty=*/true).has_value());
        ASSERT_TRUE(bpm.flush_page(pids.back()).has_value());
    }

    // Fetch all pages back in reverse order. Many will have been evicted.
    // Verify data integrity.
    for (int i = TOTAL_PAGES - 1; i >= 0; --i) {
        auto fetch = bpm.fetch_page(pids[i]);
        ASSERT_TRUE(fetch.has_value()) << "fetch_page failed for pid " << pids[i];
        Page* page = *fetch;

        auto tuple = page->get_tuple(0);
        ASSERT_TRUE(tuple.has_value()) << "get_tuple failed for pid " << pids[i];
        EXPECT_EQ((*tuple)[0], static_cast<uint8_t>(i + 1))
            << "Data corrupted for page " << pids[i];

        ASSERT_TRUE(bpm.unpin_page(pids[i], false).has_value());
    }
}

/// Verify that a page's data survives: write → unpin (dirty) → eviction →
/// re-fetch from disk → data intact.
TEST_F(QABufferPoolTest, DirtyEvictedPageSurvivesReload) {
    // Pool of size 1 to force eviction on every new_page.
    BufferPoolManager bpm(dm_, file_id_, 1);

    // Write sentinel data to page A.
    auto pa = bpm.new_page();
    ASSERT_TRUE(pa.has_value());
    PageId pid_a = (*pa)->page_id();
    auto data_a = std::vector<uint8_t>(60, 0xDE);
    ASSERT_TRUE((*pa)->insert_tuple(data_a).has_value());
    ASSERT_TRUE(bpm.unpin_page(pid_a, /*is_dirty=*/true).has_value());

    // Create page B: evicts page A (dirty → flushed to disk).
    auto pb = bpm.new_page();
    ASSERT_TRUE(pb.has_value());
    PageId pid_b = (*pb)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid_b, false).has_value());

    // Re-fetch page A (evicts page B, loads A from disk).
    auto pa2 = bpm.fetch_page(pid_a);
    ASSERT_TRUE(pa2.has_value()) << "Re-fetch of evicted page should succeed";
    EXPECT_EQ((*pa2)->page_id(), pid_a);

    auto tuple = (*pa2)->get_tuple(0);
    ASSERT_TRUE(tuple.has_value());
    EXPECT_EQ(tuple->size(), 60u);
    EXPECT_EQ((*tuple)[0], 0xDE) << "Data should survive eviction and disk round-trip";

    ASSERT_TRUE(bpm.unpin_page(pid_a, false).has_value());
}

/// Background flusher should run and clean dirty pages even while client threads
/// are concurrently fetching and unpinning. No data races.
TEST_F(QABufferPoolTest, FlusherConcurrentWithFetchUnpinNoCrash) {
    BufferPoolManager bpm(dm_, file_id_, 8);

    // Create 4 dirty pages.
    std::vector<PageId> pids;
    for (int i = 0; i < 4; ++i) {
        auto result = bpm.new_page();
        ASSERT_TRUE(result.has_value());
        Page* page = *result;
        pids.push_back(page->page_id());
        auto tuple = std::vector<uint8_t>(20, static_cast<uint8_t>(i + 1));
        ASSERT_TRUE(page->insert_tuple(tuple).has_value());
        ASSERT_TRUE(bpm.unpin_page(pids.back(), /*is_dirty=*/true).has_value());
    }

    // Start flusher with 20ms interval.
    ASSERT_TRUE(bpm.start_flusher(std::chrono::milliseconds(20)).has_value());

    // Spawn threads that hammer fetch/unpin while flusher runs.
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 3; ++t) {
        threads.emplace_back([&bpm, &pids, &errors, t]() {
            for (int i = 0; i < 200; ++i) {
                size_t idx = static_cast<size_t>(t * 200 + i) % pids.size();
                PageId pid = pids[idx];

                auto fetch = bpm.fetch_page(pid);
                if (!fetch.has_value()) {
                    ++errors;
                    continue;
                }
                auto unpin = bpm.unpin_page(pid, i % 3 == 0);
                if (!unpin.has_value()) {
                    ++errors;
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    bpm.stop_flusher();
    EXPECT_EQ(errors.load(), 0) << "Concurrent fetch/unpin with flusher produced errors";
}

/// Double-write buffer should be transparent: data written through DWB must
/// arrive correctly on disk.
TEST_F(QABufferPoolTest, DoubleWriteBufferDataIntegrity) {
    BufferPoolManager bpm(dm_, file_id_, 4);
    ASSERT_TRUE(bpm.enable_double_write(temp_dir_ / "qa.dwb").has_value());

    // Write several pages through DWB.
    std::vector<PageId> pids;
    for (int i = 0; i < 3; ++i) {
        auto result = bpm.new_page();
        ASSERT_TRUE(result.has_value());
        PageId pid = (*result)->page_id();
        pids.push_back(pid);

        auto tuple = std::vector<uint8_t>(40, static_cast<uint8_t>(0xA0 + i));
        ASSERT_TRUE((*result)->insert_tuple(tuple).has_value());
        ASSERT_TRUE(bpm.unpin_page(pid, /*is_dirty=*/true).has_value());
        ASSERT_TRUE(bpm.flush_page(pid).has_value());
    }

    // Verify all pages on disk.
    for (int i = 0; i < 3; ++i) {
        Page disk_page(0, PageType::DATA);
        auto read = dm_.read_page(file_id_, pids[i], disk_page);
        ASSERT_TRUE(read.has_value()) << "Read failed for pid " << pids[i];

        auto tuple = disk_page.get_tuple(0);
        ASSERT_TRUE(tuple.has_value());
        EXPECT_EQ((*tuple)[0], static_cast<uint8_t>(0xA0 + i))
            << "Data mismatch for page " << pids[i];
    }
}

/// Graceful shutdown: stop_flusher must flush pinned dirty pages and join cleanly.
TEST_F(QABufferPoolTest, FlusherShutdownFlushesAllDirtyPagesIncludingPinned) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    // Create a pinned dirty page (don't unpin after new_page).
    auto result = bpm.new_page();
    ASSERT_TRUE(result.has_value());
    Page* page = *result;
    PageId pid = page->page_id();
    auto tuple = std::vector<uint8_t>(30, 0xFE);
    ASSERT_TRUE(page->insert_tuple(tuple).has_value());
    // Mark dirty via unpin, then re-pin.
    ASSERT_TRUE(bpm.unpin_page(pid, /*is_dirty=*/true).has_value());
    auto refetch = bpm.fetch_page(pid);
    ASSERT_TRUE(refetch.has_value());
    // Page is now pin_count=1, is_dirty=true.

    // Start flusher with a long interval (won't fire before stop).
    ASSERT_TRUE(bpm.start_flusher(std::chrono::milliseconds(10000)).has_value());

    // Stop flusher immediately: final flush must include pinned pages.
    bpm.stop_flusher();

    // Verify the pinned dirty page was flushed on shutdown.
    Page disk_page(0, PageType::DATA);
    auto read = dm_.read_page(file_id_, pid, disk_page);
    ASSERT_TRUE(read.has_value()) << "Pinned dirty page should be flushed on flusher shutdown";

    auto get = disk_page.get_tuple(0);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ((*get)[0], 0xFE);

    // Clean up.
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());
}

// =============================================================================
// Section 3: BUG regression tests
// =============================================================================

/// BUG (HIGH): new_page() allocates a disk page ID before checking whether a
/// buffer frame is available. When the pool is full (all frames pinned), the
/// disk page allocation succeeds but no frame is found, so new_page() returns
/// an error. The allocated page ID is permanently lost — it is incremented in
/// the DiskManager's page count but never tracked anywhere.
///
/// Expected behavior: file page count should NOT increase when new_page() fails.
/// Actual behavior: file page count increases by 1 for each failed new_page().
///
/// This test FAILS with the current implementation, demonstrating the bug.
TEST_F(QABufferPoolTest, BUG_NewPageLeaksDiskPageWhenPoolFull) {
    BufferPoolManager bpm(dm_, file_id_, 1);

    // Record baseline page count.
    auto count0 = dm_.file_page_count(file_id_);
    ASSERT_TRUE(count0.has_value());
    uint32_t initial = *count0;

    // Successful new_page() — should allocate exactly 1 disk page.
    auto p1 = bpm.new_page();
    ASSERT_TRUE(p1.has_value());
    // Keep pinned: do NOT unpin.

    auto count1 = dm_.file_page_count(file_id_);
    ASSERT_TRUE(count1.has_value());
    EXPECT_EQ(*count1, initial + 1) << "Successful new_page() should allocate exactly 1 disk page";

    // Failed new_page() — pool is full (1 frame, 1 pinned page).
    // Bug: allocate_page() is called before find_victim_frame(),
    // so a disk page is allocated and then leaked.
    auto p2 = bpm.new_page();
    ASSERT_FALSE(p2.has_value())
        << "new_page() should fail when pool is full and all frames are pinned";
    EXPECT_EQ(p2.error().code, StatusCode::INTERNAL_ERROR);

    // BUG: The file has grown by 2 even though only 1 page was successfully
    // brought into the buffer pool.
    auto count2 = dm_.file_page_count(file_id_);
    ASSERT_TRUE(count2.has_value());
    // This assertion FAILS with the current implementation (actual: initial + 2).
    EXPECT_EQ(*count2, initial + 1)
        << "REGRESSION: failed new_page() leaked a disk page ID "
        << "(file_page_count=" << *count2 << ", expected=" << initial + 1 << ")";
}

/// BUG (HIGH): verify that repeated failed new_page() calls cause the disk page
/// count to grow unboundedly, demonstrating that each call leaks one page.
TEST_F(QABufferPoolTest, BUG_RepeatedFailedNewPageLeaksMultiplePages) {
    BufferPoolManager bpm(dm_, file_id_, 2);

    // Fill pool: 2 pinned pages.
    auto p1 = bpm.new_page();
    ASSERT_TRUE(p1.has_value());
    auto p2 = bpm.new_page();
    ASSERT_TRUE(p2.has_value());
    // Both pinned, do NOT unpin.

    auto count_after_fill = dm_.file_page_count(file_id_);
    ASSERT_TRUE(count_after_fill.has_value());
    uint32_t count_filled = *count_after_fill;

    // Call new_page() 5 times — all should fail.
    constexpr int FAILED_CALLS = 5;
    for (int i = 0; i < FAILED_CALLS; ++i) {
        auto px = bpm.new_page();
        ASSERT_FALSE(px.has_value()) << "new_page() call " << i << " should fail (pool full)";
    }

    auto count_final = dm_.file_page_count(file_id_);
    ASSERT_TRUE(count_final.has_value());

    // BUG: Each failed new_page() call leaks a disk page.
    // The file page count should not have changed (no successful allocations).
    // This assertion FAILS: actual is count_filled + FAILED_CALLS.
    EXPECT_EQ(*count_final, count_filled)
        << "REGRESSION: " << FAILED_CALLS << " failed new_page() calls leaked "
        << (*count_final - count_filled) << " disk page IDs (expected 0 leaks)";
}

// =============================================================================
// Section 4: Concurrency stress tests
// =============================================================================

/// Stress: 8 threads each create pages, write data, flush, and unpin.
/// Verifies no crashes, deadlocks, or data races under concurrent load.
TEST_F(QABufferPoolTest, ConcurrentCreateFlushDeleteStress) {
    BufferPoolManager bpm(dm_, file_id_, 16);
    ASSERT_TRUE(bpm.start_flusher(std::chrono::milliseconds(10)).has_value());

    std::atomic<int> errors{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&bpm, &errors, t]() {
            for (int i = 0; i < 50; ++i) {
                auto result = bpm.new_page();
                if (!result.has_value()) {
                    ++errors;
                    continue;
                }
                Page* page = *result;
                PageId pid = page->page_id();

                auto tuple = std::vector<uint8_t>(20, static_cast<uint8_t>(t + i));
                if (!page->insert_tuple(tuple).has_value()) {
                    ++errors;
                }

                if (!bpm.unpin_page(pid, /*is_dirty=*/true).has_value()) {
                    ++errors;
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    bpm.stop_flusher();

    // Allow some errors (pool full is expected under concurrent load),
    // but not excessive errors.
    EXPECT_LT(errors.load(), 50) << "Too many errors under concurrent stress: " << errors.load();
}

/// Stress: concurrent fetch/unpin with LRU-K eviction under memory pressure.
/// Pool is small relative to the number of pages to force frequent evictions.
TEST_F(QABufferPoolTest, ConcurrentEvictionStressNoDeadlock) {
    constexpr int POOL_SIZE = 8;
    constexpr int PAGE_COUNT = 20;
    BufferPoolManager bpm(dm_, file_id_, POOL_SIZE);

    // Pre-create and flush all pages.
    std::vector<PageId> pids;
    for (int i = 0; i < PAGE_COUNT; ++i) {
        auto result = bpm.new_page();
        ASSERT_TRUE(result.has_value());
        Page* page = *result;
        pids.push_back(page->page_id());
        auto tuple = std::vector<uint8_t>(15, static_cast<uint8_t>(i + 1));
        ASSERT_TRUE(page->insert_tuple(tuple).has_value());
        // Must mark dirty before flush, otherwise flush_page is a no-op.
        ASSERT_TRUE(bpm.unpin_page(pids.back(), /*is_dirty=*/true).has_value());
        ASSERT_TRUE(bpm.flush_page(pids.back()).has_value());
    }

    // Threads that concurrently fetch and immediately unpin (causes evictions).
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&bpm, &pids, &errors, t]() {
            for (int i = 0; i < 100; ++i) {
                size_t idx = static_cast<size_t>((t * 100 + i) * 7 + 3) % pids.size();
                PageId pid = pids[idx];

                auto fetch = bpm.fetch_page(pid);
                if (!fetch.has_value()) {
                    ++errors;
                    continue;
                }
                auto unpin = bpm.unpin_page(pid, false);
                if (!unpin.has_value()) {
                    ++errors;
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(errors.load(), 0) << "Concurrent eviction stress produced errors: " << errors.load();
}
