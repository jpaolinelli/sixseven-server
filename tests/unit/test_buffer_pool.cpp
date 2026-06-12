#include "sixseven/common/platform.h"
#include "sixseven/storage/buffer_pool.h"

#include <gtest/gtest.h>

#include <fcntl.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <random>
#include <set>
#include <thread>
#include <vector>

using namespace sixseven;

// =============================================================================
// LRU-K Replacer Tests
// =============================================================================

TEST(LRUKReplacer, EmptyReplacerHasSizeZero) {
    LRUKReplacer replacer(10);
    EXPECT_EQ(replacer.size(), 0u);
}

TEST(LRUKReplacer, EvictEmptyReplacerFails) {
    LRUKReplacer replacer(10);
    auto result = replacer.evict();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(LRUKReplacer, SingleEvictableFrame) {
    LRUKReplacer replacer(5);
    replacer.record_access(0);
    replacer.set_evictable(0, true);

    EXPECT_EQ(replacer.size(), 1u);

    auto result = replacer.evict();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0u);
    EXPECT_EQ(replacer.size(), 0u);
}

TEST(LRUKReplacer, NonEvictableFrameNotEvicted) {
    LRUKReplacer replacer(5);
    replacer.record_access(0);
    replacer.set_evictable(0, false); // Pinned.

    EXPECT_EQ(replacer.size(), 0u);

    auto result = replacer.evict();
    ASSERT_FALSE(result.has_value());
}

TEST(LRUKReplacer, InfDistanceEvictedBeforeFinite) {
    // With K=2:
    //   Frame 0: 2 accesses (finite K-distance)
    //   Frame 1: 1 access  (infinite K-distance)
    // Frame 1 should be evicted first.
    LRUKReplacer replacer(5, 2);

    replacer.record_access(0);
    replacer.record_access(0); // 2 accesses -> finite distance.
    replacer.set_evictable(0, true);

    replacer.record_access(1); // 1 access -> infinite distance.
    replacer.set_evictable(1, true);

    auto result = replacer.evict();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1u); // Infinite distance evicted first.
}

TEST(LRUKReplacer, AmongInfDistanceOldestFirstAccessEvicted) {
    // Both frames have 1 access (infinite K-distance with K=2).
    // Frame 0 was accessed first -> evicted first (FIFO among new pages).
    LRUKReplacer replacer(5, 2);

    replacer.record_access(0); // First access at t=1.
    replacer.record_access(1); // First access at t=2.
    replacer.set_evictable(0, true);
    replacer.set_evictable(1, true);

    auto result = replacer.evict();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0u); // Oldest first access.
}

TEST(LRUKReplacer, LargestKDistanceEvicted) {
    // Both frames have >= K accesses. Frame with the largest backward K-distance
    // (oldest Kth access) should be evicted.
    LRUKReplacer replacer(5, 2);

    // Frame 0: accesses at t=1, t=2. K-distance from current = high.
    replacer.record_access(0);
    replacer.record_access(0);

    // Frame 1: accesses at t=3, t=4. K-distance from current = lower.
    replacer.record_access(1);
    replacer.record_access(1);

    // Frame 2: accesses at t=5, t=6. K-distance from current = lowest.
    replacer.record_access(2);
    replacer.record_access(2);

    replacer.set_evictable(0, true);
    replacer.set_evictable(1, true);
    replacer.set_evictable(2, true);

    // Frame 0 has the oldest Kth access -> largest backward K-distance.
    auto result = replacer.evict();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0u);

    // Next eviction: Frame 1.
    auto result2 = replacer.evict();
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(*result2, 1u);
}

TEST(LRUKReplacer, SetEvictableToggle) {
    LRUKReplacer replacer(5);
    replacer.record_access(0);

    EXPECT_EQ(replacer.size(), 0u);

    replacer.set_evictable(0, true);
    EXPECT_EQ(replacer.size(), 1u);

    replacer.set_evictable(0, false);
    EXPECT_EQ(replacer.size(), 0u);

    replacer.set_evictable(0, true);
    EXPECT_EQ(replacer.size(), 1u);
}

TEST(LRUKReplacer, RemoveClearsHistory) {
    LRUKReplacer replacer(5, 2);

    replacer.record_access(0);
    replacer.record_access(0);
    replacer.set_evictable(0, true);
    EXPECT_EQ(replacer.size(), 1u);

    replacer.remove(0);
    EXPECT_EQ(replacer.size(), 0u);

    // After remove, frame 0's history is cleared.
    // Adding it back should start with a fresh history.
    replacer.record_access(0);
    replacer.set_evictable(0, true);
    EXPECT_EQ(replacer.size(), 1u);
}

TEST(LRUKReplacer, EvictMultipleInOrder) {
    LRUKReplacer replacer(5, 2);

    // Add 4 frames, all with 1 access (infinite K-distance).
    for (FrameId i = 0; i < 4; ++i) {
        replacer.record_access(i);
        replacer.set_evictable(i, true);
    }

    EXPECT_EQ(replacer.size(), 4u);

    // Should evict in order of first access: 0, 1, 2, 3.
    for (FrameId expected = 0; expected < 4; ++expected) {
        auto result = replacer.evict();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, expected);
    }

    EXPECT_EQ(replacer.size(), 0u);
}

TEST(LRUKReplacer, RecentAccessUpdatesKDistance) {
    LRUKReplacer replacer(5, 2);

    // Frame 0: access at t=1, t=2.
    replacer.record_access(0);
    replacer.record_access(0);
    // Frame 1: access at t=3, t=4.
    replacer.record_access(1);
    replacer.record_access(1);

    // Frame 0 has older Kth access -> should be evicted.
    // But now give frame 0 a fresh access.
    replacer.record_access(0); // History becomes [t=2, t=5] (t=1 dropped).

    replacer.set_evictable(0, true);
    replacer.set_evictable(1, true);

    // Frame 1's history is [t=3, t=4], Kth access at t=3.
    // Frame 0's history is [t=2, t=5], Kth access at t=2.
    // Frame 0 has larger K-distance (current_ts=5, 5-2=3 vs 5-3=2).
    // Wait, that means frame 0 should still be evicted.
    // Let me reconsider...
    // Actually with the way I set it up, after access(0) at t=5:
    //   Frame 0 history: [t=2, t=5]. Kth (2nd oldest) = t=2.
    //   Frame 1 history: [t=3, t=4]. Kth (2nd oldest) = t=3.
    // current_timestamp_ = 5.
    //   Frame 0 K-distance: 5 - 2 = 3.
    //   Frame 1 K-distance: 5 - 3 = 2.
    // Largest K-distance is frame 0. So frame 0 is still evicted.
    // That's because K=2 and frame 0's oldest kept access is t=2.
    // For the re-access to fully help, we'd need 2 more accesses to push
    // t=2 out. Let's give frame 0 one more access.
    replacer.record_access(0); // History becomes [t=5, t=6].

    // Now:
    //   Frame 0 history: [t=5, t=6]. Kth = t=5. Distance = 6-5 = 1.
    //   Frame 1 history: [t=3, t=4]. Kth = t=3. Distance = 6-3 = 3.
    // Frame 1 has larger K-distance -> evicted first.
    auto result = replacer.evict();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1u); // Frame 1 has larger K-distance now.
}

// =============================================================================
// Buffer Pool Manager Tests
// =============================================================================

class BufferPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        temp_dir_ = std::filesystem::temp_directory_path() /
                    ("sixseven_test_" + std::string(info->test_suite_name()) + "_" + info->name());
        std::filesystem::create_directories(temp_dir_);

        auto create_result = dm_.create_file(temp_dir_ / "test.gdb");
        ASSERT_TRUE(create_result.has_value());
        file_id_ = *create_result;
    }

    void TearDown() override {
        (void)dm_.close_file(file_id_);
        std::filesystem::remove_all(temp_dir_);
    }

    [[nodiscard]] std::filesystem::path test_file() const { return temp_dir_ / "test.gdb"; }

    std::filesystem::path temp_dir_;
    DiskManager dm_;
    FileId file_id_ = 0;
};

TEST_F(BufferPoolTest, NewPageReturnsValidPage) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto result = bpm.new_page();
    ASSERT_TRUE(result.has_value());
    Page* page = *result;

    EXPECT_NE(page, nullptr);
    EXPECT_GE(page->page_id(), 1u); // User pages start at 1.
    EXPECT_EQ(bpm.pool_page_count(), 1u);
}

TEST_F(BufferPoolTest, NewPageIncrementingIds) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto p1 = bpm.new_page();
    ASSERT_TRUE(p1.has_value());
    auto p2 = bpm.new_page();
    ASSERT_TRUE(p2.has_value());
    auto p3 = bpm.new_page();
    ASSERT_TRUE(p3.has_value());

    EXPECT_EQ((*p1)->page_id(), 1u);
    EXPECT_EQ((*p2)->page_id(), 2u);
    EXPECT_EQ((*p3)->page_id(), 3u);
}

TEST_F(BufferPoolTest, FetchPageReadsFromDisk) {
    // Pre-allocate and write a page via DiskManager directly.
    auto alloc_result = dm_.allocate_page(file_id_);
    ASSERT_TRUE(alloc_result.has_value());
    PageId pid = *alloc_result;

    Page disk_page(pid, PageType::DATA);
    auto tuple = std::vector<uint8_t>(100, 0xAB);
    ASSERT_TRUE(disk_page.insert_tuple(tuple).has_value());
    ASSERT_TRUE(dm_.write_page(file_id_, pid, disk_page).has_value());

    // Fetch through buffer pool.
    BufferPoolManager bpm(dm_, file_id_, 4);
    auto fetch_result = bpm.fetch_page(pid);
    ASSERT_TRUE(fetch_result.has_value());
    Page* page = *fetch_result;

    EXPECT_EQ(page->page_id(), pid);
    EXPECT_EQ(page->page_type(), PageType::DATA);

    auto get_result = page->get_tuple(0);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ(get_result->size(), 100u);
    EXPECT_EQ((*get_result)[0], 0xAB);
}

TEST_F(BufferPoolTest, FetchCachedPageNoSecondDiskRead) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto new_result = bpm.new_page();
    ASSERT_TRUE(new_result.has_value());
    PageId pid = (*new_result)->page_id();

    // Write some data.
    auto data = std::vector<uint8_t>(50, 0xCC);
    ASSERT_TRUE((*new_result)->insert_tuple(data).has_value());

    ASSERT_TRUE(bpm.unpin_page(pid, true).has_value());

    // Fetch the same page again — should return from cache.
    auto fetch_result = bpm.fetch_page(pid);
    ASSERT_TRUE(fetch_result.has_value());
    Page* page = *fetch_result;

    EXPECT_EQ(page->page_id(), pid);
    auto get_result = page->get_tuple(0);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ((*get_result)[0], 0xCC);
}

TEST_F(BufferPoolTest, UnpinPageDecrementsPinCount) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto result = bpm.new_page();
    ASSERT_TRUE(result.has_value());
    PageId pid = (*result)->page_id();

    // Page is pinned (pin_count=1). Unpin it.
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());

    // Double unpin should fail (pin_count already 0).
    auto double_unpin = bpm.unpin_page(pid, false);
    ASSERT_FALSE(double_unpin.has_value());
    EXPECT_EQ(double_unpin.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(BufferPoolTest, UnpinPageNotInPoolFails) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto result = bpm.unpin_page(999, false);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(BufferPoolTest, DirtyPageFlushedOnEviction) {
    // Pool of size 2: fill it up, then adding a 3rd page forces eviction.
    BufferPoolManager bpm(dm_, file_id_, 2);

    // Page 1: write data and mark dirty.
    auto p1 = bpm.new_page();
    ASSERT_TRUE(p1.has_value());
    PageId pid1 = (*p1)->page_id();
    auto tuple1 = std::vector<uint8_t>(80, 0x11);
    ASSERT_TRUE((*p1)->insert_tuple(tuple1).has_value());
    ASSERT_TRUE(bpm.unpin_page(pid1, /*is_dirty=*/true).has_value());

    // Page 2: also dirty so eviction uses LRU-K order (both dirty).
    auto p2 = bpm.new_page();
    ASSERT_TRUE(p2.has_value());
    PageId pid2 = (*p2)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid2, /*is_dirty=*/true).has_value());

    // Page 3: this should evict page 1 (LRU-K: page 1 was accessed first).
    auto p3 = bpm.new_page();
    ASSERT_TRUE(p3.has_value());
    ASSERT_TRUE(bpm.unpin_page((*p3)->page_id(), false).has_value());

    // Verify page 1 was persisted to disk during eviction.
    Page read_page(0, PageType::DATA);
    auto read_result = dm_.read_page(file_id_, pid1, read_page);
    ASSERT_TRUE(read_result.has_value());

    auto get_result = read_page.get_tuple(0);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ(get_result->size(), 80u);
    EXPECT_EQ((*get_result)[0], 0x11);
}

TEST_F(BufferPoolTest, PinnedPageNotEvicted) {
    // Pool of size 2.
    BufferPoolManager bpm(dm_, file_id_, 2);

    // Page 1: keep pinned.
    auto p1 = bpm.new_page();
    ASSERT_TRUE(p1.has_value());
    // Don't unpin p1 — it stays pinned.

    // Page 2: unpin so it's evictable.
    auto p2 = bpm.new_page();
    ASSERT_TRUE(p2.has_value());
    PageId pid2 = (*p2)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid2, false).has_value());

    // Page 3: should evict page 2 (page 1 is pinned, page 2 is evictable).
    auto p3 = bpm.new_page();
    ASSERT_TRUE(p3.has_value());

    // Page 2 should be evicted — fetch it back from disk.
    ASSERT_TRUE(bpm.unpin_page((*p3)->page_id(), false).has_value());

    // Page 1 is still in the pool (was pinned).
    ASSERT_TRUE(bpm.unpin_page((*p1)->page_id(), false).has_value());
}

TEST_F(BufferPoolTest, PoolFullAllPinnedFails) {
    // Pool of size 2.
    BufferPoolManager bpm(dm_, file_id_, 2);

    auto p1 = bpm.new_page();
    ASSERT_TRUE(p1.has_value());
    // Don't unpin — stays pinned.

    auto p2 = bpm.new_page();
    ASSERT_TRUE(p2.has_value());
    // Don't unpin — stays pinned.

    // Pool is full, both pages pinned.
    auto p3 = bpm.new_page();
    ASSERT_FALSE(p3.has_value());
    EXPECT_EQ(p3.error().code, StatusCode::INTERNAL_ERROR);
}

TEST_F(BufferPoolTest, FlushPage) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto result = bpm.new_page();
    ASSERT_TRUE(result.has_value());
    Page* page = *result;
    PageId pid = page->page_id();

    auto tuple = std::vector<uint8_t>(60, 0xDD);
    ASSERT_TRUE(page->insert_tuple(tuple).has_value());
    ASSERT_TRUE(bpm.unpin_page(pid, /*is_dirty=*/true).has_value());

    // Flush to disk.
    ASSERT_TRUE(bpm.flush_page(pid).has_value());

    // Verify on disk.
    Page read_page(0, PageType::DATA);
    auto read_result = dm_.read_page(file_id_, pid, read_page);
    ASSERT_TRUE(read_result.has_value());

    auto get_result = read_page.get_tuple(0);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ((*get_result)[0], 0xDD);
}

TEST_F(BufferPoolTest, FlushPageNotInPoolFails) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto result = bpm.flush_page(999);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(BufferPoolTest, FlushAll) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    // Create and dirty 3 pages.
    std::vector<PageId> page_ids;
    for (int i = 0; i < 3; ++i) {
        auto result = bpm.new_page();
        ASSERT_TRUE(result.has_value());
        Page* page = *result;
        PageId pid = page->page_id();
        page_ids.push_back(pid);

        auto tuple = std::vector<uint8_t>(40, static_cast<uint8_t>(i + 1));
        ASSERT_TRUE(page->insert_tuple(tuple).has_value());
        ASSERT_TRUE(bpm.unpin_page(pid, /*is_dirty=*/true).has_value());
    }

    // Flush all.
    ASSERT_TRUE(bpm.flush_all().has_value());

    // Verify all pages on disk.
    for (size_t i = 0; i < page_ids.size(); ++i) {
        Page read_page(0, PageType::DATA);
        auto read_result = dm_.read_page(file_id_, page_ids[i], read_page);
        ASSERT_TRUE(read_result.has_value()) << "Failed to read page " << page_ids[i];

        auto get_result = read_page.get_tuple(0);
        ASSERT_TRUE(get_result.has_value());
        EXPECT_EQ((*get_result)[0], static_cast<uint8_t>(i + 1));
    }
}

TEST_F(BufferPoolTest, DeletePage) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto result = bpm.new_page();
    ASSERT_TRUE(result.has_value());
    PageId pid = (*result)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());

    EXPECT_EQ(bpm.pool_page_count(), 1u);

    ASSERT_TRUE(bpm.delete_page(pid).has_value());
    EXPECT_EQ(bpm.pool_page_count(), 0u);

    // Fetching deleted page should re-read from disk (but it was never written,
    // so it will fail with checksum error since the on-disk page is uninitialized).
    auto fetch = bpm.fetch_page(pid);
    ASSERT_FALSE(fetch.has_value());
}

TEST_F(BufferPoolTest, DeletePinnedPageFails) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto result = bpm.new_page();
    ASSERT_TRUE(result.has_value());
    PageId pid = (*result)->page_id();
    // Don't unpin.

    auto del_result = bpm.delete_page(pid);
    ASSERT_FALSE(del_result.has_value());
    EXPECT_EQ(del_result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(BufferPoolTest, DeletePageNotInPoolFails) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto result = bpm.delete_page(999);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(BufferPoolTest, DeletedFrameReusedByNewPage) {
    // Pool of size 2.
    BufferPoolManager bpm(dm_, file_id_, 2);

    auto p1 = bpm.new_page();
    ASSERT_TRUE(p1.has_value());
    PageId pid1 = (*p1)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid1, false).has_value());

    auto p2 = bpm.new_page();
    ASSERT_TRUE(p2.has_value());
    ASSERT_TRUE(bpm.unpin_page((*p2)->page_id(), false).has_value());

    // Pool is full (2 pages). Delete page 1 to free a frame.
    ASSERT_TRUE(bpm.delete_page(pid1).has_value());
    EXPECT_EQ(bpm.pool_page_count(), 1u);

    // Now new_page should succeed by using the freed frame.
    auto p3 = bpm.new_page();
    ASSERT_TRUE(p3.has_value());
    EXPECT_EQ(bpm.pool_page_count(), 2u);
}

TEST_F(BufferPoolTest, EvictionFollowsLRUKOrder) {
    // Pool of size 3. We'll create 4 pages to trigger eviction.
    BufferPoolManager bpm(dm_, file_id_, 3);

    // Create pages 1, 2, 3.
    auto p1 = bpm.new_page();
    ASSERT_TRUE(p1.has_value());
    PageId pid1 = (*p1)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid1, false).has_value());

    auto p2 = bpm.new_page();
    ASSERT_TRUE(p2.has_value());
    PageId pid2 = (*p2)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid2, false).has_value());

    auto p3 = bpm.new_page();
    ASSERT_TRUE(p3.has_value());
    PageId pid3 = (*p3)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid3, false).has_value());

    // All 3 pages have 1 access each (infinite K-distance with K=2).
    // Page 1 was accessed first -> should be evicted first.

    // Fetch page 2 again to give it 2 accesses (finite K-distance).
    auto fetch2 = bpm.fetch_page(pid2);
    ASSERT_TRUE(fetch2.has_value());
    ASSERT_TRUE(bpm.unpin_page(pid2, false).has_value());

    // Now: Page 1 has 1 access (inf), Page 2 has 2 accesses (finite), Page 3 has 1 access (inf).
    // Among infinite: Page 1 (first access earlier) evicted before Page 3.

    // Create page 4: should evict page 1.
    auto p4 = bpm.new_page();
    ASSERT_TRUE(p4.has_value());
    ASSERT_TRUE(bpm.unpin_page((*p4)->page_id(), false).has_value());

    // Page 1 should be evicted. Verify it's no longer in the pool.
    // Fetch page 1: should need to read from disk.
    // But page 1 was never written to disk (only new_page, no write_page),
    // so the on-disk page is uninitialized -> checksum error.
    // That confirms page 1 was evicted from the pool.
    // Instead, let's verify that page 2 and 3 are still accessible.
    auto check2 = bpm.fetch_page(pid2);
    ASSERT_TRUE(check2.has_value());
    EXPECT_EQ((*check2)->page_id(), pid2);
    ASSERT_TRUE(bpm.unpin_page(pid2, false).has_value());
}

TEST_F(BufferPoolTest, MultiplePinsSamePageIncrementCount) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto p = bpm.new_page();
    ASSERT_TRUE(p.has_value());
    PageId pid = (*p)->page_id();

    // Fetch the same page twice more (each increments pin count).
    auto f1 = bpm.fetch_page(pid);
    ASSERT_TRUE(f1.has_value());
    auto f2 = bpm.fetch_page(pid);
    ASSERT_TRUE(f2.has_value());

    // Pin count should be 3 (new_page + 2 fetches).
    // We need to unpin 3 times before the page becomes evictable.
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());

    // Fourth unpin should fail.
    auto extra_unpin = bpm.unpin_page(pid, false);
    ASSERT_FALSE(extra_unpin.has_value());
}

TEST_F(BufferPoolTest, StickyDirtyFlag) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto p = bpm.new_page();
    ASSERT_TRUE(p.has_value());
    PageId pid = (*p)->page_id();

    auto tuple = std::vector<uint8_t>(30, 0xEE);
    ASSERT_TRUE((*p)->insert_tuple(tuple).has_value());

    // Unpin as dirty, then fetch and unpin as clean.
    ASSERT_TRUE(bpm.unpin_page(pid, /*is_dirty=*/true).has_value());

    auto f = bpm.fetch_page(pid);
    ASSERT_TRUE(f.has_value());
    ASSERT_TRUE(bpm.unpin_page(pid, /*is_dirty=*/false).has_value());

    // Flush should still write the page (dirty flag is sticky).
    ASSERT_TRUE(bpm.flush_page(pid).has_value());

    // Verify data on disk.
    Page read_page(0, PageType::DATA);
    auto read_result = dm_.read_page(file_id_, pid, read_page);
    ASSERT_TRUE(read_result.has_value());

    auto get_result = read_page.get_tuple(0);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ((*get_result)[0], 0xEE);
}

TEST_F(BufferPoolTest, FetchAfterEvictionReloadsFromDisk) {
    // Pool of size 1.
    BufferPoolManager bpm(dm_, file_id_, 1);

    // Create and write page 1.
    auto p1 = bpm.new_page();
    ASSERT_TRUE(p1.has_value());
    PageId pid1 = (*p1)->page_id();
    auto tuple1 = std::vector<uint8_t>(50, 0xAA);
    ASSERT_TRUE((*p1)->insert_tuple(tuple1).has_value());
    ASSERT_TRUE(bpm.unpin_page(pid1, /*is_dirty=*/true).has_value());

    // Create page 2: evicts page 1 (dirty flush to disk).
    auto p2 = bpm.new_page();
    ASSERT_TRUE(p2.has_value());
    ASSERT_TRUE(bpm.unpin_page((*p2)->page_id(), false).has_value());

    // Fetch page 1 again: should reload from disk (evicts page 2).
    auto f1 = bpm.fetch_page(pid1);
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ((*f1)->page_id(), pid1);

    auto get_result = (*f1)->get_tuple(0);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ(get_result->size(), 50u);
    EXPECT_EQ((*get_result)[0], 0xAA);
}

TEST_F(BufferPoolTest, PoolPageCountAccurate) {
    BufferPoolManager bpm(dm_, file_id_, 10);

    EXPECT_EQ(bpm.pool_page_count(), 0u);

    auto p1 = bpm.new_page();
    ASSERT_TRUE(p1.has_value());
    EXPECT_EQ(bpm.pool_page_count(), 1u);

    auto p2 = bpm.new_page();
    ASSERT_TRUE(p2.has_value());
    EXPECT_EQ(bpm.pool_page_count(), 2u);

    ASSERT_TRUE(bpm.unpin_page((*p1)->page_id(), false).has_value());
    ASSERT_TRUE(bpm.delete_page((*p1)->page_id()).has_value());
    EXPECT_EQ(bpm.pool_page_count(), 1u);
}

// =============================================================================
// Background Flusher Tests
// =============================================================================

TEST_F(BufferPoolTest, FlusherCleansUnpinnedDirtyPages) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto result = bpm.new_page();
    ASSERT_TRUE(result.has_value());
    PageId pid = (*result)->page_id();
    auto tuple = std::vector<uint8_t>(50, 0xBB);
    ASSERT_TRUE((*result)->insert_tuple(tuple).has_value());
    ASSERT_TRUE(bpm.unpin_page(pid, /*is_dirty=*/true).has_value());

    // Start the flusher with a short interval.
    ASSERT_TRUE(bpm.start_flusher(std::chrono::milliseconds(50)).has_value());

    // Wait for the flusher to run at least once.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    bpm.stop_flusher();

    // Verify the page was written to disk by the flusher.
    Page read_page(0, PageType::DATA);
    auto read_result = dm_.read_page(file_id_, pid, read_page);
    ASSERT_TRUE(read_result.has_value());

    auto get_result = read_page.get_tuple(0);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ((*get_result)[0], 0xBB);
}

TEST_F(BufferPoolTest, FlusherSkipsPinnedDirtyPages) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    // Create a pinned dirty page.
    auto result = bpm.new_page();
    ASSERT_TRUE(result.has_value());
    PageId pid = (*result)->page_id();
    auto tuple = std::vector<uint8_t>(50, 0xCC);
    ASSERT_TRUE((*result)->insert_tuple(tuple).has_value());
    // Unpin to set dirty flag, then re-fetch to re-pin.
    ASSERT_TRUE(bpm.unpin_page(pid, /*is_dirty=*/true).has_value());
    auto fetch = bpm.fetch_page(pid);
    ASSERT_TRUE(fetch.has_value());
    // Now pin_count=1, is_dirty=true (sticky).

    // Create an unpinned dirty page for comparison.
    auto result2 = bpm.new_page();
    ASSERT_TRUE(result2.has_value());
    PageId pid2 = (*result2)->page_id();
    auto tuple2 = std::vector<uint8_t>(50, 0xDD);
    ASSERT_TRUE((*result2)->insert_tuple(tuple2).has_value());
    ASSERT_TRUE(bpm.unpin_page(pid2, /*is_dirty=*/true).has_value());

    // Start flusher.
    ASSERT_TRUE(bpm.start_flusher(std::chrono::milliseconds(50)).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // The unpinned dirty page (pid2) should be flushed by normal flusher cycle.
    Page read_page2(0, PageType::DATA);
    auto read_result2 = dm_.read_page(file_id_, pid2, read_page2);
    ASSERT_TRUE(read_result2.has_value());
    auto get_result2 = read_page2.get_tuple(0);
    ASSERT_TRUE(get_result2.has_value());
    EXPECT_EQ((*get_result2)[0], 0xDD);

    // The pinned dirty page (pid) should NOT be flushed yet during normal cycles.
    Page read_page1(0, PageType::DATA);
    auto read_result1 = dm_.read_page(file_id_, pid, read_page1);
    EXPECT_FALSE(read_result1.has_value()); // Checksum error = page was never written.

    // Stop the flusher (final flush includes pinned pages).
    bpm.stop_flusher();

    // Now the pinned dirty page should be on disk after the shutdown flush.
    Page read_page3(0, PageType::DATA);
    auto read_result3 = dm_.read_page(file_id_, pid, read_page3);
    ASSERT_TRUE(read_result3.has_value());
    auto get_result3 = read_page3.get_tuple(0);
    ASSERT_TRUE(get_result3.has_value());
    EXPECT_EQ((*get_result3)[0], 0xCC);

    // Clean up: unpin the page.
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());
}

TEST_F(BufferPoolTest, DestructorFlushesWithActiveFlusher) {
    PageId pid = 0;
    {
        BufferPoolManager bpm(dm_, file_id_, 4);
        ASSERT_TRUE(bpm.start_flusher(std::chrono::milliseconds(1000)).has_value());

        auto result = bpm.new_page();
        ASSERT_TRUE(result.has_value());
        pid = (*result)->page_id();
        auto tuple = std::vector<uint8_t>(50, 0xDD);
        ASSERT_TRUE((*result)->insert_tuple(tuple).has_value());
        ASSERT_TRUE(bpm.unpin_page(pid, /*is_dirty=*/true).has_value());

        // Destructor should stop flusher and flush all dirty pages.
    }

    // Verify page was written to disk by the destructor.
    Page read_page(0, PageType::DATA);
    auto read_result = dm_.read_page(file_id_, pid, read_page);
    ASSERT_TRUE(read_result.has_value());
    auto get_result = read_page.get_tuple(0);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ((*get_result)[0], 0xDD);
}

TEST_F(BufferPoolTest, StartFlusherTwiceFails) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    ASSERT_TRUE(bpm.start_flusher(std::chrono::milliseconds(100)).has_value());

    auto second = bpm.start_flusher(std::chrono::milliseconds(100));
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, StatusCode::ALREADY_EXISTS);

    bpm.stop_flusher();
}

TEST_F(BufferPoolTest, StopFlusherWithoutStartIsNoOp) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    // Should not crash or hang.
    bpm.stop_flusher();
    bpm.stop_flusher();
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST_F(BufferPoolTest, ConcurrentFetchUnpin) {
    BufferPoolManager bpm(dm_, file_id_, 10);

    // Pre-create pages.
    std::vector<PageId> pids;
    for (int i = 0; i < 5; ++i) {
        auto result = bpm.new_page();
        ASSERT_TRUE(result.has_value());
        pids.push_back((*result)->page_id());
        ASSERT_TRUE(bpm.unpin_page(pids.back(), false).has_value());
    }

    // Spawn threads that concurrently fetch and unpin pages.
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&bpm, &pids, &errors, t]() {
            for (int i = 0; i < 100; ++i) {
                size_t idx = static_cast<size_t>(t + i) % pids.size();
                PageId pid = pids[idx];
                auto fetch_result = bpm.fetch_page(pid);
                if (!fetch_result.has_value()) {
                    ++errors;
                    continue;
                }
                auto unpin_result = bpm.unpin_page(pid, false);
                if (!unpin_result.has_value()) {
                    ++errors;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(errors.load(), 0);
}

TEST_F(BufferPoolTest, ConcurrentFetchUnpinWithFlusher) {
    BufferPoolManager bpm(dm_, file_id_, 10);

    // Pre-create dirty pages.
    std::vector<PageId> pids;
    for (int i = 0; i < 5; ++i) {
        auto result = bpm.new_page();
        ASSERT_TRUE(result.has_value());
        Page* page = *result;
        pids.push_back(page->page_id());
        auto tuple = std::vector<uint8_t>(30, static_cast<uint8_t>(i + 1));
        ASSERT_TRUE(page->insert_tuple(tuple).has_value());
        ASSERT_TRUE(bpm.unpin_page(pids.back(), /*is_dirty=*/true).has_value());
    }

    // Start flusher with short interval.
    ASSERT_TRUE(bpm.start_flusher(std::chrono::milliseconds(20)).has_value());

    // Spawn threads that concurrently fetch and unpin while flusher is running.
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&bpm, &pids, &errors, t]() {
            for (int i = 0; i < 100; ++i) {
                size_t idx = static_cast<size_t>(t + i) % pids.size();
                PageId pid = pids[idx];
                auto fetch_result = bpm.fetch_page(pid);
                if (!fetch_result.has_value()) {
                    ++errors;
                    continue;
                }
                auto unpin_result = bpm.unpin_page(pid, (i % 2 == 0));
                if (!unpin_result.has_value()) {
                    ++errors;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    bpm.stop_flusher();

    EXPECT_EQ(errors.load(), 0);
}

// =============================================================================
// Double-Write Buffer Tests
// =============================================================================

TEST_F(BufferPoolTest, DoubleWriteBufferWritesThroughDWB) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto dwb_path = temp_dir_ / "test.dwb";
    ASSERT_TRUE(bpm.enable_double_write(dwb_path).has_value());

    auto result = bpm.new_page();
    ASSERT_TRUE(result.has_value());
    PageId pid = (*result)->page_id();
    auto tuple = std::vector<uint8_t>(80, 0xFF);
    ASSERT_TRUE((*result)->insert_tuple(tuple).has_value());
    ASSERT_TRUE(bpm.unpin_page(pid, /*is_dirty=*/true).has_value());

    // Flush the page (goes through DWB first, then data file).
    ASSERT_TRUE(bpm.flush_page(pid).has_value());

    // Verify the data file has the page.
    Page disk_page(0, PageType::DATA);
    auto read_result = dm_.read_page(file_id_, pid, disk_page);
    ASSERT_TRUE(read_result.has_value());
    auto get_result = disk_page.get_tuple(0);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ((*get_result)[0], 0xFF);

    // Verify the DWB file contains a valid page with correct checksum.
    int dwb_fd = ::open(dwb_path.string().c_str(), O_RDONLY);
    ASSERT_GE(dwb_fd, 0);

    std::array<uint8_t, page_size> dwb_data{};
    ssize_t bytes_read = sixseven_platform::pread(dwb_fd, dwb_data.data(), page_size, 0);
    EXPECT_EQ(static_cast<size_t>(bytes_read), page_size);
    ::close(dwb_fd);

    // Construct a Page from the DWB data and verify checksum and content.
    Page dwb_page(dwb_data);
    uint32_t stored_cksum = dwb_page.checksum();
    uint32_t computed_cksum = compute_page_checksum(dwb_page);
    EXPECT_EQ(stored_cksum, computed_cksum);

    auto dwb_tuple = dwb_page.get_tuple(0);
    ASSERT_TRUE(dwb_tuple.has_value());
    EXPECT_EQ(dwb_tuple->size(), 80u);
    EXPECT_EQ((*dwb_tuple)[0], 0xFF);
}

TEST_F(BufferPoolTest, EnableDoubleWriteTwiceFails) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    ASSERT_TRUE(bpm.enable_double_write(temp_dir_ / "test.dwb").has_value());

    auto second = bpm.enable_double_write(temp_dir_ / "test2.dwb");
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, StatusCode::ALREADY_EXISTS);
}

TEST_F(BufferPoolTest, DoubleWriteBufferWithEviction) {
    // Pool of size 2 with DWB enabled.
    BufferPoolManager bpm(dm_, file_id_, 2);
    ASSERT_TRUE(bpm.enable_double_write(temp_dir_ / "test.dwb").has_value());

    // Fill pool with dirty pages.
    auto p1 = bpm.new_page();
    ASSERT_TRUE(p1.has_value());
    PageId pid1 = (*p1)->page_id();
    auto tuple1 = std::vector<uint8_t>(60, 0x11);
    ASSERT_TRUE((*p1)->insert_tuple(tuple1).has_value());
    ASSERT_TRUE(bpm.unpin_page(pid1, /*is_dirty=*/true).has_value());

    // Page 2: also dirty so eviction uses LRU-K order (both dirty).
    auto p2 = bpm.new_page();
    ASSERT_TRUE(p2.has_value());
    ASSERT_TRUE(bpm.unpin_page((*p2)->page_id(), /*is_dirty=*/true).has_value());

    // Adding a 3rd page forces eviction of page 1 through DWB.
    auto p3 = bpm.new_page();
    ASSERT_TRUE(p3.has_value());
    ASSERT_TRUE(bpm.unpin_page((*p3)->page_id(), false).has_value());

    // Verify evicted page 1 was flushed to disk (via DWB).
    Page read_page(0, PageType::DATA);
    auto read_result = dm_.read_page(file_id_, pid1, read_page);
    ASSERT_TRUE(read_result.has_value());
    auto get_result = read_page.get_tuple(0);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ((*get_result)[0], 0x11);
}

TEST_F(BufferPoolTest, FlusherWithDoubleWriteBuffer) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto dwb_path = temp_dir_ / "test.dwb";
    ASSERT_TRUE(bpm.enable_double_write(dwb_path).has_value());

    auto result = bpm.new_page();
    ASSERT_TRUE(result.has_value());
    PageId pid = (*result)->page_id();
    auto tuple = std::vector<uint8_t>(40, 0xAA);
    ASSERT_TRUE((*result)->insert_tuple(tuple).has_value());
    ASSERT_TRUE(bpm.unpin_page(pid, /*is_dirty=*/true).has_value());

    // Start flusher — it should use DWB when writing dirty pages.
    ASSERT_TRUE(bpm.start_flusher(std::chrono::milliseconds(50)).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    bpm.stop_flusher();

    // Verify the page on disk.
    Page read_page(0, PageType::DATA);
    auto read_result = dm_.read_page(file_id_, pid, read_page);
    ASSERT_TRUE(read_result.has_value());
    auto get_result = read_page.get_tuple(0);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ((*get_result)[0], 0xAA);

    // Verify the DWB file has valid content.
    int dwb_fd = ::open(dwb_path.string().c_str(), O_RDONLY);
    ASSERT_GE(dwb_fd, 0);

    std::array<uint8_t, page_size> dwb_data{};
    ssize_t bytes_read = sixseven_platform::pread(dwb_fd, dwb_data.data(), page_size, 0);
    EXPECT_EQ(static_cast<size_t>(bytes_read), page_size);
    ::close(dwb_fd);

    Page dwb_page(dwb_data);
    EXPECT_EQ(dwb_page.checksum(), compute_page_checksum(dwb_page));
}

// =============================================================================
// Hit/Miss Counter Tests
// =============================================================================

TEST_F(BufferPoolTest, HitMissCountersStartAtZero) {
    BufferPoolManager bpm(dm_, file_id_, 4);
    EXPECT_EQ(bpm.hit_count(), 0u);
    EXPECT_EQ(bpm.miss_count(), 0u);
}

TEST_F(BufferPoolTest, FetchMissIncrementsMissCounter) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    // new_page doesn't go through fetch_page, so no hit/miss.
    auto p = bpm.new_page();
    ASSERT_TRUE(p.has_value());
    PageId pid = (*p)->page_id();
    auto tuple = std::vector<uint8_t>(50, 0xAB);
    ASSERT_TRUE((*p)->insert_tuple(tuple).has_value());
    ASSERT_TRUE(bpm.unpin_page(pid, /*is_dirty=*/true).has_value());
    ASSERT_TRUE(bpm.flush_page(pid).has_value());

    // Delete from pool so next fetch must read from disk.
    ASSERT_TRUE(bpm.delete_page(pid).has_value());

    // Fetch from disk -> miss.
    auto f = bpm.fetch_page(pid);
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(bpm.miss_count(), 1u);
    EXPECT_EQ(bpm.hit_count(), 0u);
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());
}

TEST_F(BufferPoolTest, FetchHitIncrementsHitCounter) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto p = bpm.new_page();
    ASSERT_TRUE(p.has_value());
    PageId pid = (*p)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());

    // Page is still in pool -> hit.
    auto f = bpm.fetch_page(pid);
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(bpm.hit_count(), 1u);
    EXPECT_EQ(bpm.miss_count(), 0u);
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());

    // Fetch again -> another hit.
    auto f2 = bpm.fetch_page(pid);
    ASSERT_TRUE(f2.has_value());
    EXPECT_EQ(bpm.hit_count(), 2u);
    EXPECT_EQ(bpm.miss_count(), 0u);
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());
}

// -- Dirty page ratio tracking tests (GDB-639) --

TEST_F(BufferPoolTest, DirtyCountStartsAtZero) {
    BufferPoolManager bpm(dm_, file_id_, 4);
    EXPECT_EQ(bpm.dirty_count(), 0u);
    EXPECT_DOUBLE_EQ(bpm.dirty_ratio(), 0.0);
}

TEST_F(BufferPoolTest, DirtyCountIncrementedOnUnpin) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto p = bpm.new_page();
    ASSERT_TRUE(p.has_value());
    PageId pid = (*p)->page_id();

    // Unpin as clean — no change.
    ASSERT_TRUE(bpm.unpin_page(pid, false).has_value());
    EXPECT_EQ(bpm.dirty_count(), 0u);

    // Fetch again and unpin as dirty.
    auto f = bpm.fetch_page(pid);
    ASSERT_TRUE(f.has_value());
    ASSERT_TRUE(bpm.unpin_page(pid, true).has_value());
    EXPECT_EQ(bpm.dirty_count(), 1u);
    EXPECT_DOUBLE_EQ(bpm.dirty_ratio(), 1.0 / 4.0);
}

TEST_F(BufferPoolTest, DirtyCountNotDoubleIncremented) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto p = bpm.new_page();
    ASSERT_TRUE(p.has_value());
    PageId pid = (*p)->page_id();

    // Mark dirty.
    ASSERT_TRUE(bpm.unpin_page(pid, true).has_value());
    EXPECT_EQ(bpm.dirty_count(), 1u);

    // Fetch and unpin as dirty again — already dirty, count should not change.
    auto f = bpm.fetch_page(pid);
    ASSERT_TRUE(f.has_value());
    ASSERT_TRUE(bpm.unpin_page(pid, true).has_value());
    EXPECT_EQ(bpm.dirty_count(), 1u);
}

TEST_F(BufferPoolTest, DirtyCountDecrementedOnFlush) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto p = bpm.new_page();
    ASSERT_TRUE(p.has_value());
    PageId pid = (*p)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid, true).has_value());
    EXPECT_EQ(bpm.dirty_count(), 1u);

    // Flush the page.
    ASSERT_TRUE(bpm.flush_page(pid).has_value());
    EXPECT_EQ(bpm.dirty_count(), 0u);
}

TEST_F(BufferPoolTest, DirtyCountDecrementedOnFlushAll) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    // Create 3 dirty pages.
    for (int i = 0; i < 3; ++i) {
        auto p = bpm.new_page();
        ASSERT_TRUE(p.has_value());
        ASSERT_TRUE(bpm.unpin_page((*p)->page_id(), true).has_value());
    }
    EXPECT_EQ(bpm.dirty_count(), 3u);

    ASSERT_TRUE(bpm.flush_all().has_value());
    EXPECT_EQ(bpm.dirty_count(), 0u);
}

TEST_F(BufferPoolTest, DirtyCountDecrementedOnDeleteDirtyPage) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto p = bpm.new_page();
    ASSERT_TRUE(p.has_value());
    PageId pid = (*p)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid, true).has_value());
    EXPECT_EQ(bpm.dirty_count(), 1u);

    ASSERT_TRUE(bpm.delete_page(pid).has_value());
    EXPECT_EQ(bpm.dirty_count(), 0u);
}

// -- Threshold-based flusher tests (GDB-640) --

TEST_F(BufferPoolTest, DirtyFlushThresholdDefault) {
    BufferPoolManager bpm(dm_, file_id_, 4);
    EXPECT_DOUBLE_EQ(bpm.dirty_flush_threshold(), 0.75);
}

TEST_F(BufferPoolTest, DirtyFlushThresholdConfigurable) {
    BufferPoolManager bpm(dm_, file_id_, 4);
    bpm.set_dirty_flush_threshold(0.5);
    EXPECT_DOUBLE_EQ(bpm.dirty_flush_threshold(), 0.5);
}

TEST_F(BufferPoolTest, FlusherWakesOnThresholdCrossed) {
    // Pool of 4. Threshold at 0.5 means flusher wakes at 2 dirty pages.
    BufferPoolManager bpm(dm_, file_id_, 4);
    bpm.set_dirty_flush_threshold(0.5);

    auto flusher_result = bpm.start_flusher(std::chrono::seconds(60));
    ASSERT_TRUE(flusher_result.has_value());

    // Create 2 dirty pages (50% = threshold).
    for (int i = 0; i < 2; ++i) {
        auto p = bpm.new_page();
        ASSERT_TRUE(p.has_value());
        ASSERT_TRUE(bpm.unpin_page((*p)->page_id(), true).has_value());
    }

    // Wait briefly for the flusher to react to the threshold signal.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // The flusher should have flushed the dirty pages.
    EXPECT_EQ(bpm.dirty_count(), 0u);

    bpm.stop_flusher();
}

TEST_F(BufferPoolTest, FlusherStillWorksOnTimerFallback) {
    BufferPoolManager bpm(dm_, file_id_, 4);
    // Set threshold very high so it never triggers.
    bpm.set_dirty_flush_threshold(1.1);

    auto flusher_result = bpm.start_flusher(std::chrono::milliseconds(50));
    ASSERT_TRUE(flusher_result.has_value());

    auto p = bpm.new_page();
    ASSERT_TRUE(p.has_value());
    ASSERT_TRUE(bpm.unpin_page((*p)->page_id(), true).has_value());
    EXPECT_EQ(bpm.dirty_count(), 1u);

    // Wait for timer-based flush.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(bpm.dirty_count(), 0u);

    bpm.stop_flusher();
}

// -- Clean-frame eviction preference tests (GDB-641) --

TEST(LRUKReplacer, EvictPrefersNonSkippedFrames) {
    LRUKReplacer replacer(5, 2);

    // Frame 0: accessed first (oldest, highest K-distance) — but "dirty".
    replacer.record_access(0);
    replacer.set_evictable(0, true);

    // Frame 1: accessed second — "clean".
    replacer.record_access(1);
    replacer.set_evictable(1, true);

    // Without skip predicate, frame 0 evicted first (oldest access).
    // With skip predicate marking frame 0 as dirty, frame 1 should be preferred.
    auto result = replacer.evict([](FrameId fid) { return fid == 0; });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1u); // Clean frame preferred.
}

TEST(LRUKReplacer, EvictFallsBackToSkippedFrames) {
    LRUKReplacer replacer(5, 2);

    // Only frame 0, which is "dirty".
    replacer.record_access(0);
    replacer.set_evictable(0, true);

    auto result = replacer.evict([](FrameId fid) { return fid == 0; });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0u); // Falls back to dirty frame.
}

TEST(LRUKReplacer, EvictKDistanceWithinCleanGroup) {
    LRUKReplacer replacer(5, 2);

    // Three clean frames. Should evict by K-distance within the clean group.
    replacer.record_access(0); // oldest
    replacer.record_access(1);
    replacer.record_access(2); // newest

    replacer.set_evictable(0, true);
    replacer.set_evictable(1, true);
    replacer.set_evictable(2, true);

    // Frame 3 is dirty.
    replacer.record_access(3);
    replacer.set_evictable(3, true);

    // Skip frame 3 (dirty). Among clean frames, frame 0 has oldest access.
    auto result = replacer.evict([](FrameId fid) { return fid == 3; });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0u);
}

TEST_F(BufferPoolTest, EvictionPrefersCleanOverDirty) {
    // Pool of 2. Page 1 dirty, page 2 clean. Eviction should pick clean page.
    BufferPoolManager bpm(dm_, file_id_, 2);

    auto p1 = bpm.new_page();
    ASSERT_TRUE(p1.has_value());
    PageId pid1 = (*p1)->page_id();
    auto tuple = std::vector<uint8_t>(80, 0xAA);
    ASSERT_TRUE((*p1)->insert_tuple(tuple).has_value());
    ASSERT_TRUE(bpm.unpin_page(pid1, /*is_dirty=*/true).has_value());

    auto p2 = bpm.new_page();
    ASSERT_TRUE(p2.has_value());
    PageId pid2 = (*p2)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid2, /*is_dirty=*/false).has_value());

    // Allocate 3rd page -> should evict clean page 2, not dirty page 1.
    auto p3 = bpm.new_page();
    ASSERT_TRUE(p3.has_value());
    ASSERT_TRUE(bpm.unpin_page((*p3)->page_id(), false).has_value());

    // Page 1 should still be in the pool (not evicted).
    auto f1 = bpm.fetch_page(pid1);
    ASSERT_TRUE(f1.has_value());
    auto get_result = (*f1)->get_tuple(0);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ((*get_result)[0], 0xAA);
    ASSERT_TRUE(bpm.unpin_page(pid1, false).has_value());
}

TEST_F(BufferPoolTest, DirtyCountDecrementedOnEviction) {
    // Pool of 2 frames. Fill both dirty, then force eviction.
    BufferPoolManager bpm(dm_, file_id_, 2);

    auto p1 = bpm.new_page();
    ASSERT_TRUE(p1.has_value());
    ASSERT_TRUE(bpm.unpin_page((*p1)->page_id(), true).has_value());

    auto p2 = bpm.new_page();
    ASSERT_TRUE(p2.has_value());
    ASSERT_TRUE(bpm.unpin_page((*p2)->page_id(), true).has_value());
    EXPECT_EQ(bpm.dirty_count(), 2u);

    // Force eviction by allocating a 3rd page.
    auto p3 = bpm.new_page();
    ASSERT_TRUE(p3.has_value());
    // One dirty frame was evicted (flushed then replaced with clean new page).
    EXPECT_EQ(bpm.dirty_count(), 1u);
    ASSERT_TRUE(bpm.unpin_page((*p3)->page_id(), false).has_value());
}

// =============================================================================
// LRU-K Priority Queue Tests (GDB-620)
//
// These tests verify that the indexed-priority-queue implementation of the
// LRU-K replacer evicts in the same order as a reference linear-scan
// implementation. They cover the three eviction regimes called out in the
// AC: all frames have < K accesses (FIFO), all have >= K accesses (oldest
// Kth wins), and mixed access counts.
// =============================================================================

namespace {

/// Reference implementation of the LRU-K eviction policy. Mirrors the
/// linear-scan algorithm that used to live in `LRUKReplacer::evict()` and is
/// used by the differential tests below to oracle the new heap-based code.
struct RefFrameInfo {
    std::vector<uint64_t> history;
    bool evictable = false;
};

struct RefReplacer {
    uint32_t k;
    uint64_t current_timestamp = 0;
    std::vector<RefFrameInfo> frames;

    RefReplacer(uint32_t num_frames, uint32_t k_in) : k(k_in), frames(num_frames) {}

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    void record_access(FrameId frame_id) {
        auto& info = frames[frame_id];
        info.history.push_back(++current_timestamp);
        if (info.history.size() > k) {
            info.history.erase(info.history.begin());
        }
    }

    void set_evictable(FrameId frame_id, bool evictable) { frames[frame_id].evictable = evictable; }

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    std::optional<FrameId> evict() {
        FrameId victim = 0;
        bool found = false;
        bool victim_is_inf = false;
        uint64_t victim_oldest_access = std::numeric_limits<uint64_t>::max();
        uint64_t victim_k_distance = 0;

        for (FrameId i = 0; i < static_cast<FrameId>(frames.size()); ++i) {
            const auto& info = frames[i];
            if (!info.evictable || info.history.empty()) {
                continue;
            }
            // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
            const bool is_inf = info.history.size() < k;
            if (is_inf) {
                if (!found || !victim_is_inf || info.history.front() < victim_oldest_access) {
                    victim = i;
                    found = true;
                    victim_is_inf = true;
                    victim_oldest_access = info.history.front();
                }
            } else if (!victim_is_inf) {
                const uint64_t k_distance = current_timestamp - info.history.front();
                if (!found || k_distance > victim_k_distance) {
                    victim = i;
                    found = true;
                    victim_k_distance = k_distance;
                }
            }
        }

        if (!found) {
            return std::nullopt;
        }
        frames[victim].history.clear();
        frames[victim].evictable = false;
        return victim;
    }
};

/// Apply the same access pattern to both replacers and assert their eviction
/// sequences match. The pattern is a mixed sequence of `record_access`,
/// `set_evictable(true)` and `evict()` calls.
struct TraceOp {
    enum class Kind { ACCESS, EVICTABLE, EVICT };
    Kind kind;
    FrameId frame_id; ///< Used by ACCESS and EVICTABLE.
};

void run_trace_and_compare(uint32_t num_frames, uint32_t k, const std::vector<TraceOp>& trace) {
    LRUKReplacer pq(num_frames, k);
    RefReplacer ref(num_frames, k);

    int evict_index = 0;
    for (const auto& op : trace) {
        switch (op.kind) {
        case TraceOp::Kind::ACCESS:
            pq.record_access(op.frame_id);
            ref.record_access(op.frame_id);
            break;
        case TraceOp::Kind::EVICTABLE:
            pq.set_evictable(op.frame_id, true);
            ref.set_evictable(op.frame_id, true);
            break;
        case TraceOp::Kind::EVICT: {
            auto pq_result = pq.evict();
            auto ref_result = ref.evict();
            ASSERT_EQ(pq_result.has_value(), ref_result.has_value())
                << "Eviction " << evict_index << " disagreement on success/failure";
            if (pq_result.has_value()) {
                EXPECT_EQ(*pq_result, *ref_result)
                    << "Eviction " << evict_index << " picked different victim";
            }
            ++evict_index;
            break;
        }
        }
    }
}

} // namespace

/// All frames have a single access (< K with K=2): FIFO eviction by first
/// access timestamp.
TEST(LRUKReplacerPQ, AllInfiniteKDistanceFIFO) {
    constexpr uint32_t num_frames = 16;
    std::vector<TraceOp> trace;
    for (FrameId i = 0; i < num_frames; ++i) {
        trace.push_back({TraceOp::Kind::ACCESS, i});
        trace.push_back({TraceOp::Kind::EVICTABLE, i});
    }
    for (uint32_t i = 0; i < num_frames; ++i) {
        trace.push_back({TraceOp::Kind::EVICT, 0});
    }
    run_trace_and_compare(num_frames, /*k=*/2, trace);
}

/// All frames have >= K accesses: the frame with the oldest Kth-most-recent
/// access is evicted first.
TEST(LRUKReplacerPQ, AllFiniteKDistanceOldestKthFirst) {
    constexpr uint32_t num_frames = 12;
    std::vector<TraceOp> trace;
    // Two passes of accesses give every frame >= K=2 accesses.
    for (uint32_t pass = 0; pass < 2; ++pass) {
        for (FrameId i = 0; i < num_frames; ++i) {
            trace.push_back({TraceOp::Kind::ACCESS, i});
        }
    }
    for (FrameId i = 0; i < num_frames; ++i) {
        trace.push_back({TraceOp::Kind::EVICTABLE, i});
    }
    for (uint32_t i = 0; i < num_frames; ++i) {
        trace.push_back({TraceOp::Kind::EVICT, 0});
    }
    run_trace_and_compare(num_frames, /*k=*/2, trace);
}

/// Mixed: half the frames have a single access (infinite distance), half
/// have two accesses (finite distance). Infinite frames must be evicted
/// first, then finite frames in oldest-Kth order.
TEST(LRUKReplacerPQ, MixedInfiniteAndFiniteDistance) {
    constexpr uint32_t num_frames = 16;
    std::vector<TraceOp> trace;
    // Frames 0..7: two accesses (finite).
    // Frames 8..15: one access (infinite).
    for (FrameId i = 0; i < 8; ++i) {
        trace.push_back({TraceOp::Kind::ACCESS, i});
        trace.push_back({TraceOp::Kind::ACCESS, i});
    }
    for (FrameId i = 8; i < num_frames; ++i) {
        trace.push_back({TraceOp::Kind::ACCESS, i});
    }
    for (FrameId i = 0; i < num_frames; ++i) {
        trace.push_back({TraceOp::Kind::EVICTABLE, i});
    }
    for (uint32_t i = 0; i < num_frames; ++i) {
        trace.push_back({TraceOp::Kind::EVICT, 0});
    }
    run_trace_and_compare(num_frames, /*k=*/2, trace);
}

/// `record_access` on an already-evictable frame must update its priority in
/// the heap. Sequence: A, A, B, B, evictable(A), evictable(B), access(A) x2,
/// evict — without the heap update, frame A would still appear at the head.
TEST(LRUKReplacerPQ, RecordAccessUpdatesPriorityWhileEvictable) {
    LRUKReplacer pq(2, 2);
    pq.record_access(0);
    pq.record_access(0);
    pq.record_access(1);
    pq.record_access(1);
    pq.set_evictable(0, true);
    pq.set_evictable(1, true);
    // Frame 0 originally has the largest K-distance. Two more accesses on
    // frame 0 push frame 1 to the head.
    pq.record_access(0);
    pq.record_access(0);
    auto result = pq.evict();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1u);
}

/// Toggling set_evictable while access history changes must keep the heap
/// consistent. Verified by comparing against the reference for a long
/// pseudo-random trace.
TEST(LRUKReplacerPQ, RandomTraceMatchesReference) {
    constexpr uint32_t num_frames = 64;
    constexpr uint32_t k = 2;
    constexpr int num_ops = 4000;

    std::mt19937 rng(0xC0FFEE);
    std::uniform_int_distribution<int> action_dist(0, 9);
    std::uniform_int_distribution<FrameId> frame_dist(0, num_frames - 1);

    LRUKReplacer pq(num_frames, k);
    RefReplacer ref(num_frames, k);

    // Track which frames are evictable so toggling stays consistent across
    // both implementations.
    std::vector<bool> is_evictable(num_frames, false);

    for (int i = 0; i < num_ops; ++i) {
        // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
        const int action = action_dist(rng);
        if (action < 5) {
            // 50% access.
            const FrameId fid = frame_dist(rng);
            pq.record_access(fid);
            ref.record_access(fid);
        } else if (action < 8) {
            // 30% toggle evictable.
            const FrameId fid = frame_dist(rng);
            // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
            const bool flag = !is_evictable[fid];
            is_evictable[fid] = flag;
            pq.set_evictable(fid, flag);
            ref.set_evictable(fid, flag);
        } else {
            // 20% evict.
            auto pq_result = pq.evict();
            auto ref_result = ref.evict();
            // NOLINTBEGIN(readability-implicit-bool-conversion)
            ASSERT_EQ(pq_result.has_value(), ref_result.has_value())
                << "op " << i << " disagreement on success";
            if (pq_result.has_value()) {
                ASSERT_EQ(*pq_result, *ref_result) << "op " << i << " picked different victim";
                is_evictable[*pq_result] = false;
            }
            // NOLINTEND(readability-implicit-bool-conversion)
        }
    }
}

/// K=1 should degrade to plain LRU. Differential test against the reference.
TEST(LRUKReplacerPQ, K1MatchesPlainLRU) {
    constexpr uint32_t num_frames = 8;
    std::vector<TraceOp> trace;
    // Re-access frames in a specific order so each has a unique last-access
    // timestamp.
    for (FrameId i = 0; i < num_frames; ++i) {
        trace.push_back({TraceOp::Kind::ACCESS, i});
        trace.push_back({TraceOp::Kind::EVICTABLE, i});
    }
    // Touch a few frames again to update their priority.
    trace.push_back({TraceOp::Kind::ACCESS, 2});
    trace.push_back({TraceOp::Kind::ACCESS, 5});
    trace.push_back({TraceOp::Kind::ACCESS, 0});
    for (uint32_t i = 0; i < num_frames; ++i) {
        trace.push_back({TraceOp::Kind::EVICT, 0});
    }
    run_trace_and_compare(num_frames, /*k=*/1, trace);
}

/// `set_evictable(false)` on a frame at the head of the heap must remove it
/// so the next eviction picks a different victim. Verified directly because
/// this is the heap-specific invariant.
TEST(LRUKReplacerPQ, SetEvictableFalseRemovesFromHeap) {
    LRUKReplacer pq(4, 2);
    for (FrameId i = 0; i < 4; ++i) {
        pq.record_access(i);
        pq.set_evictable(i, true);
    }
    // Frame 0 is at the head (oldest). Pin it.
    pq.set_evictable(0, false);
    auto result = pq.evict();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1u);
}

// =============================================================================
// Concurrent Stress Tests (GDB-647)
// =============================================================================

TEST_F(BufferPoolTest, ConcurrentStressTest) {
    // Stress test: multiple threads doing concurrent fetch/unpin/new_page/eviction.
    // Pool is intentionally small to force frequent eviction.
    constexpr uint32_t pool_size = 16;
    constexpr int num_threads = 8;
    constexpr int ops_per_thread = 500;

    BufferPoolManager bpm(dm_, file_id_, pool_size);

    // Pre-create some pages so threads can fetch existing ones.
    std::vector<PageId> page_ids;
    for (int i = 0; i < 32; ++i) {
        auto result = bpm.new_page();
        ASSERT_TRUE(result.has_value());
        PageId pid = (*result)->page_id();
        page_ids.push_back(pid);
        auto data = std::vector<uint8_t>(64, static_cast<uint8_t>(i));
        ASSERT_TRUE((*result)->insert_tuple(data).has_value());
        ASSERT_TRUE(bpm.unpin_page(pid, true).has_value());
    }

    std::atomic<bool> failed{false};
    std::atomic<int> completed{0};

    auto worker = [&](int thread_id) {
        for (int op = 0; op < ops_per_thread && !failed.load(); ++op) {
            int action = (thread_id * 1000 + op) % 4;
            if (action < 2) {
                // Fetch an existing page.
                PageId pid = page_ids[(thread_id + op) % page_ids.size()];
                auto result = bpm.fetch_page(pid);
                if (result.has_value()) {
                    Page* page = *result;
                    if (page->page_id() != pid) {
                        failed.store(true);
                        return;
                    }
                    bool dirty = (op % 3 == 0);
                    auto unpin = bpm.unpin_page(pid, dirty);
                    if (!unpin.has_value()) {
                        failed.store(true);
                        return;
                    }
                }
                // Miss failures (eviction) are expected when pool is full.
            } else if (action == 2) {
                // Allocate a new page.
                auto result = bpm.new_page();
                if (result.has_value()) {
                    PageId pid = (*result)->page_id();
                    auto unpin = bpm.unpin_page(pid, true);
                    if (!unpin.has_value()) {
                        failed.store(true);
                        return;
                    }
                }
            } else {
                // Flush a page.
                PageId pid = page_ids[(thread_id + op) % page_ids.size()];
                (void)bpm.flush_page(pid);
            }
        }
        completed.fetch_add(1);
    };

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_FALSE(failed.load()) << "Data corruption detected in concurrent stress test";
    EXPECT_EQ(completed.load(), num_threads);
}

TEST_F(BufferPoolTest, ConcurrentStressWithFlusher) {
    // Same stress test but with the background flusher running.
    constexpr uint32_t pool_size = 16;
    constexpr int num_threads = 8;
    constexpr int ops_per_thread = 500;

    BufferPoolManager bpm(dm_, file_id_, pool_size);
    ASSERT_TRUE(bpm.start_flusher(std::chrono::milliseconds(50)).has_value());

    // Pre-create pages.
    std::vector<PageId> page_ids;
    for (int i = 0; i < 32; ++i) {
        auto result = bpm.new_page();
        ASSERT_TRUE(result.has_value());
        page_ids.push_back((*result)->page_id());
        ASSERT_TRUE(bpm.unpin_page((*result)->page_id(), true).has_value());
    }

    std::atomic<bool> failed{false};
    std::atomic<int> completed{0};

    auto worker = [&](int thread_id) {
        for (int op = 0; op < ops_per_thread && !failed.load(); ++op) {
            PageId pid = page_ids[(thread_id * 7 + op) % page_ids.size()];
            auto result = bpm.fetch_page(pid);
            if (result.has_value()) {
                auto unpin = bpm.unpin_page(pid, op % 2 == 0);
                if (!unpin.has_value()) {
                    failed.store(true);
                    return;
                }
            }
        }
        completed.fetch_add(1);
    };

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) {
        t.join();
    }

    bpm.stop_flusher();

    EXPECT_FALSE(failed.load());
    EXPECT_EQ(completed.load(), num_threads);
}

// GDB-778: page-write failures in flush_dirty_pages and the destructor are
// logged and counted instead of silently swallowed.
TEST_F(BufferPoolTest, FlushDirtyPagesCountsWriteFailures) {
    BufferPoolManager bpm(dm_, file_id_, 4);

    auto page = bpm.new_page();
    ASSERT_TRUE(page.has_value());
    ASSERT_TRUE(bpm.unpin_page((*page)->page_id(), true).has_value());
    EXPECT_EQ(bpm.flush_error_count(), 0u);

    // Close the backing file out from under the pool so the next write fails.
    ASSERT_TRUE(dm_.close_file(file_id_).has_value());

    // Drive the real background flusher and wait (bounded) for it to hit the
    // write failure at least once.
    ASSERT_TRUE(bpm.start_flusher(std::chrono::milliseconds(1)).has_value());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (bpm.flush_error_count() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    bpm.stop_flusher();

    EXPECT_GE(bpm.flush_error_count(), 1u);
    // The page must stay dirty so a later (healthy) flush can retry it.
    EXPECT_EQ(bpm.dirty_count(), 1u);

    // Reopen so the destructor flush and TearDown behave.
    auto reopened = dm_.open_file(test_file());
    ASSERT_TRUE(reopened.has_value());
    file_id_ = *reopened;
}
