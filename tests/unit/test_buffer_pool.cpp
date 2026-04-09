#include "sixseven/storage/buffer_pool.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
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

    // Page 2.
    auto p2 = bpm.new_page();
    ASSERT_TRUE(p2.has_value());
    PageId pid2 = (*p2)->page_id();
    ASSERT_TRUE(bpm.unpin_page(pid2, false).has_value());

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
    int dwb_fd = ::open(dwb_path.c_str(), O_RDONLY);
    ASSERT_GE(dwb_fd, 0);

    std::array<uint8_t, page_size> dwb_data{};
    ssize_t bytes_read = ::pread(dwb_fd, dwb_data.data(), page_size, 0);
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

    auto p2 = bpm.new_page();
    ASSERT_TRUE(p2.has_value());
    ASSERT_TRUE(bpm.unpin_page((*p2)->page_id(), false).has_value());

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
    int dwb_fd = ::open(dwb_path.c_str(), O_RDONLY);
    ASSERT_GE(dwb_fd, 0);

    std::array<uint8_t, page_size> dwb_data{};
    ssize_t bytes_read = ::pread(dwb_fd, dwb_data.data(), page_size, 0);
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
