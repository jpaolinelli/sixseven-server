/// @file tests/qa/test_qa_gdb_614.cpp
/// QA adversarial tests for GDB-614: Optimize heap insert path — skip first-fit
/// page scan.
///
/// Acceptance Criteria verified:
///   AC1: When last_insert_page_ hint fails, try next sequential page before scanning
///   AC2: If next sequential page also fails, allocate a new page directly
///   AC3: Existing insert behavior is preserved for correctness (all rows stored)
///   AC4: Unit tests for insert with full pages pass
///   AC5: No regressions

#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/page.h"
#include "sixseven/table/table_heap.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <numeric>
#include <set>
#include <vector>

namespace sixseven {
namespace {

// =============================================================================
// Test fixture
// =============================================================================

class GDB614HeapInsertOptimization : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb614.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;
    }

    void TearDown() override {
        bpm_.reset();
        auto close = dm_.close_file(file_id_);
        (void)close;
        std::filesystem::remove(path_);
    }

    void create_bpm(size_t pool_size = 64) {
        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, pool_size);
    }

    static std::vector<uint8_t> make_tuple(size_t len, uint8_t fill = 0xAA) {
        return std::vector<uint8_t>(len, fill);
    }

    // Compute how many tuples of a given size fit in one page.
    static int tuples_per_page(size_t tuple_size) {
        // usable = page_size - page_header_size = 8192 - 24 = 8168
        // each tuple costs: slot_entry_size (4) + tuple_size
        size_t usable = page_size - page_header_size;
        return static_cast<int>(usable / (slot_entry_size + tuple_size));
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
};

// =============================================================================
// AC1: Hint fails → try next sequential page
// =============================================================================

TEST_F(GDB614HeapInsertOptimization, AC1_HintFailsTryNextPage) {
    create_bpm();
    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr size_t tuple_size = 2000;
    int per_page = tuples_per_page(tuple_size); // 4

    // Fill page 1 (the first data page).
    std::vector<RID> page1_rids;
    for (int i = 0; i < per_page; ++i) {
        auto r = heap.insert_tuple(make_tuple(tuple_size, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value()) << r.error().message;
        page1_rids.push_back(*r);
    }

    // All on same page.
    PageId first_page = page1_rids[0].page_id;
    for (auto& rid : page1_rids) {
        EXPECT_EQ(rid.page_id, first_page);
    }

    // Next insert: hint page is full, should go to next page (or new page).
    auto overflow = heap.insert_tuple(make_tuple(tuple_size, 0xFF));
    ASSERT_TRUE(overflow.has_value()) << overflow.error().message;
    EXPECT_NE(overflow->page_id, first_page)
        << "Should have moved to a different page when hint was full";
}

// =============================================================================
// AC2: Both hint and hint+1 full → allocate new page
// =============================================================================

TEST_F(GDB614HeapInsertOptimization, AC2_HintAndNextFullAllocatesNew) {
    create_bpm();
    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr size_t tuple_size = 2000;
    int per_page = tuples_per_page(tuple_size); // 4

    // Fill exactly 2 pages worth of tuples.
    std::vector<RID> rids;
    for (int i = 0; i < per_page * 2; ++i) {
        auto r = heap.insert_tuple(make_tuple(tuple_size, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value()) << "Insert " << i << " failed: " << r.error().message;
        rids.push_back(*r);
    }

    // After filling 2 pages, hint points to page 2.
    // Next insert: hint page (page 2) is full, hint+1 (page 3) doesn't exist
    // or is empty. Should still succeed by allocating a new page.
    auto next = heap.insert_tuple(make_tuple(tuple_size, 0xBB));
    ASSERT_TRUE(next.has_value()) << next.error().message;

    // Verify the new tuple is retrievable.
    auto get = heap.get_tuple(*next);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ((*get)[0], 0xBB);
}

// =============================================================================
// AC3: All rows are stored correctly (correctness)
// =============================================================================

TEST_F(GDB614HeapInsertOptimization, AC3_AllRowsStoredCorrectly) {
    create_bpm();
    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr int total = 200;
    std::vector<RID> rids;
    rids.reserve(total);

    for (int i = 0; i < total; ++i) {
        // Use varied tuple sizes.
        size_t sz = 50 + (static_cast<size_t>(i) % 150);
        auto r = heap.insert_tuple(make_tuple(sz, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value()) << "Insert " << i << " failed: " << r.error().message;
        rids.push_back(*r);
    }

    // Verify every single row.
    for (int i = 0; i < total; ++i) {
        auto get = heap.get_tuple(rids[static_cast<size_t>(i)]);
        ASSERT_TRUE(get.has_value()) << "Get " << i << " failed";
        size_t expected_sz = 50 + (static_cast<size_t>(i) % 150);
        EXPECT_EQ(get->size(), expected_sz);
        EXPECT_EQ((*get)[0], static_cast<uint8_t>(i));
    }
}

TEST_F(GDB614HeapInsertOptimization, AC3_SequentialScanReturnsAllRows) {
    create_bpm();
    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr int total = 100;
    for (int i = 0; i < total; ++i) {
        auto r = heap.insert_tuple(make_tuple(200, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value()) << "Insert " << i << " failed: " << r.error().message;
    }

    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value()) << it_result.error().message;
    auto it = std::move(*it_result);
    int count = 0;
    // GDB-1224: next() returns Result<optional<...>>. At end-of-scan it
    // returns ok(nullopt) -- a *successful* Result -- so `while (result)`
    // alone (relying on tl::expected::operator bool(), i.e. has_value())
    // never sees the scan end and loops forever. Must check both that
    // next() succeeded AND that it produced a tuple.
    while (true) {
        auto result = it.next();
        ASSERT_TRUE(result.has_value()) << result.error().message;
        if (!result->has_value())
            break;
        count++;
    }
    EXPECT_EQ(count, total) << "Sequential scan must return all inserted rows";
}

// =============================================================================
// AC4: Insert with full pages (various scenarios)
// =============================================================================

TEST_F(GDB614HeapInsertOptimization, AC4_InsertAfterManyFullPages) {
    create_bpm();
    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr size_t tuple_size = 2000;
    int tpp = tuples_per_page(tuple_size);

    // Fill 10 pages.
    for (int p = 0; p < 10; ++p) {
        for (int t = 0; t < tpp; ++t) {
            auto r = heap.insert_tuple(make_tuple(tuple_size, static_cast<uint8_t>(p)));
            ASSERT_TRUE(r.has_value())
                << "Page " << p << " tuple " << t << " failed: " << r.error().message;
        }
    }

    // One more insert must succeed on a new page.
    auto r = heap.insert_tuple(make_tuple(tuple_size, 0xCC));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto get = heap.get_tuple(*r);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ((*get)[0], 0xCC);
}

// =============================================================================
// Adversarial: Edge cases and boundary values
// =============================================================================

TEST_F(GDB614HeapInsertOptimization, Adversarial_SingleByteInsert) {
    create_bpm();
    TableHeap heap(*bpm_, dm_, file_id_);

    // Minimum tuple size: 1 byte.
    auto r = heap.insert_tuple(make_tuple(1, 0x42));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto get = heap.get_tuple(*r);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ(get->size(), 1u);
    EXPECT_EQ((*get)[0], 0x42);
}

TEST_F(GDB614HeapInsertOptimization, Adversarial_MaxTupleSizeFitsPage) {
    create_bpm();
    TableHeap heap(*bpm_, dm_, file_id_);

    // Max tuple that fits: page_size - page_header_size - slot_entry_size
    // = 8192 - 24 - 4 = 8164 bytes.
    constexpr size_t max_tuple = page_size - page_header_size - slot_entry_size;
    auto r = heap.insert_tuple(make_tuple(max_tuple, 0x77));
    ASSERT_TRUE(r.has_value()) << r.error().message;

    auto get = heap.get_tuple(*r);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ(get->size(), max_tuple);
}

TEST_F(GDB614HeapInsertOptimization, Adversarial_TupleExceedingPageFails) {
    create_bpm();
    TableHeap heap(*bpm_, dm_, file_id_);

    // One byte over max.
    constexpr size_t too_big = page_size - page_header_size - slot_entry_size + 1;
    auto r = heap.insert_tuple(make_tuple(too_big, 0x99));
    EXPECT_FALSE(r.has_value()) << "Tuple exceeding page capacity should fail";
}

TEST_F(GDB614HeapInsertOptimization, Adversarial_MaxTupleThenOverflow) {
    create_bpm();
    TableHeap heap(*bpm_, dm_, file_id_);

    // Insert a max-size tuple (fills entire page).
    constexpr size_t max_tuple = page_size - page_header_size - slot_entry_size;
    auto r1 = heap.insert_tuple(make_tuple(max_tuple, 0x11));
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    // Next insert (even 1 byte) must go to a new page since hint page is full.
    auto r2 = heap.insert_tuple(make_tuple(1, 0x22));
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    EXPECT_NE(r1->page_id, r2->page_id) << "Second tuple must be on a different page";

    // Both readable.
    auto g1 = heap.get_tuple(*r1);
    ASSERT_TRUE(g1.has_value());
    EXPECT_EQ(g1->size(), max_tuple);

    auto g2 = heap.get_tuple(*r2);
    ASSERT_TRUE(g2.has_value());
    EXPECT_EQ(g2->size(), 1u);
}

TEST_F(GDB614HeapInsertOptimization, Adversarial_EmptyTupleRejected) {
    create_bpm();
    TableHeap heap(*bpm_, dm_, file_id_);

    auto r = heap.insert_tuple({});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(GDB614HeapInsertOptimization, Adversarial_ColdStartNoHint) {
    create_bpm();
    TableHeap heap(*bpm_, dm_, file_id_);

    // Very first insert — last_insert_page_ is 0, no hint, no sequential
    // fallback. Should allocate a new page directly.
    auto r = heap.insert_tuple(make_tuple(100, 0xAA));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_GE(r->page_id, 1u) << "First insert should be on a data page (>= 1)";

    auto get = heap.get_tuple(*r);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ((*get)[0], 0xAA);
}

TEST_F(GDB614HeapInsertOptimization, Adversarial_UniqueRIDs) {
    create_bpm();
    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr int total = 300;
    std::set<std::pair<uint32_t, uint16_t>> rid_set;

    for (int i = 0; i < total; ++i) {
        auto r = heap.insert_tuple(make_tuple(100, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value()) << "Insert " << i << " failed: " << r.error().message;
        auto [_, inserted] = rid_set.insert({r->page_id, r->slot_id});
        EXPECT_TRUE(inserted) << "Duplicate RID at insert " << i << ": (" << r->page_id << ", "
                              << r->slot_id << ")";
    }
}

TEST_F(GDB614HeapInsertOptimization, Adversarial_NoPageScanOnFullHint) {
    // With a very small buffer pool (4 frames), the old O(N) scan would cause
    // eviction storms. The new code should handle this gracefully.
    create_bpm(4);
    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr size_t tuple_size = 2000;

    // Insert enough to fill several pages.
    constexpr int total = 20;
    std::vector<RID> rids;
    for (int i = 0; i < total; ++i) {
        auto r = heap.insert_tuple(make_tuple(tuple_size, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value()) << "Insert " << i << " failed: " << r.error().message;
        rids.push_back(*r);
    }

    // Verify all tuples are retrievable even with tiny buffer pool.
    for (int i = 0; i < total; ++i) {
        auto get = heap.get_tuple(rids[static_cast<size_t>(i)]);
        ASSERT_TRUE(get.has_value()) << "Get " << i << " failed";
        EXPECT_EQ((*get)[0], static_cast<uint8_t>(i));
    }
}

// =============================================================================
// Adversarial: Insert/delete interleaving
// =============================================================================

TEST_F(GDB614HeapInsertOptimization, Adversarial_InsertDeleteInsertPattern) {
    create_bpm();
    TableHeap heap(*bpm_, dm_, file_id_);

    // Fill a page, delete some tuples, then insert more.
    // With the new optimization, inserts after the hint may skip pages with
    // freed space — this is the known trade-off (TODO GDB-626).
    // Verify correctness: all non-deleted rows are retrievable.
    constexpr size_t tuple_size = 200;

    std::vector<RID> rids;
    for (int i = 0; i < 50; ++i) {
        auto r = heap.insert_tuple(make_tuple(tuple_size, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value()) << r.error().message;
        rids.push_back(*r);
    }

    // Delete every 3rd tuple.
    for (size_t i = 0; i < rids.size(); i += 3) {
        auto del = heap.delete_tuple(rids[i]);
        ASSERT_TRUE(del.has_value()) << del.error().message;
    }

    // Insert more tuples.
    std::vector<RID> new_rids;
    for (int i = 0; i < 20; ++i) {
        auto r = heap.insert_tuple(make_tuple(tuple_size, static_cast<uint8_t>(0xD0 + i)));
        ASSERT_TRUE(r.has_value())
            << "Post-delete insert " << i << " failed: " << r.error().message;
        new_rids.push_back(*r);
    }

    // Surviving (non-deleted) original tuples must still be readable with
    // their original data. Deleted slots may have been reused by new inserts,
    // so we don't assert they are unreadable.
    for (size_t i = 0; i < rids.size(); ++i) {
        if (i % 3 != 0) {
            auto get = heap.get_tuple(rids[i]);
            ASSERT_TRUE(get.has_value()) << "Surviving tuple " << i << " should be readable";
            EXPECT_EQ((*get)[0], static_cast<uint8_t>(i));
        }
    }

    // All new tuples should be readable.
    for (size_t i = 0; i < new_rids.size(); ++i) {
        auto get = heap.get_tuple(new_rids[i]);
        ASSERT_TRUE(get.has_value()) << "New tuple " << i << " not readable";
        EXPECT_EQ((*get)[0], static_cast<uint8_t>(0xD0 + i));
    }

    // Sequential scan should return exactly the right count:
    // 50 original - 17 deleted + 20 new = 53 live tuples.
    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value()) << it_result.error().message;
    auto it = std::move(*it_result);
    int scan_count = 0;
    // GDB-1224: see the identical fix + comment in AC3_SequentialScanReturnsAllRows
    // above -- `while (result)` alone never observes end-of-scan because
    // next() returns ok(nullopt) (a successful Result) when exhausted.
    while (true) {
        auto result = it.next();
        ASSERT_TRUE(result.has_value()) << result.error().message;
        if (!result->has_value())
            break;
        scan_count++;
    }
    EXPECT_EQ(scan_count, 50 - 17 + 20);
}

// =============================================================================
// Adversarial: Stress test — many pages
// =============================================================================

TEST_F(GDB614HeapInsertOptimization, Stress_ManyPagesSmallPool) {
    // Stress: insert 500 tuples spanning many pages with a small buffer pool.
    // This is the scenario GDB-614 was designed to fix — the old first-fit scan
    // would thrash the buffer pool. The new code should handle this without issues.
    create_bpm(8);
    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr size_t tuple_size = 2000;
    constexpr int total = 500;

    std::vector<RID> rids;
    rids.reserve(total);
    for (int i = 0; i < total; ++i) {
        auto r = heap.insert_tuple(make_tuple(tuple_size, static_cast<uint8_t>(i & 0xFF)));
        ASSERT_TRUE(r.has_value()) << "Insert " << i << " failed: " << r.error().message;
        rids.push_back(*r);
    }

    // Spot-check a sample of tuples.
    for (int i = 0; i < total; i += 50) {
        auto get = heap.get_tuple(rids[static_cast<size_t>(i)]);
        ASSERT_TRUE(get.has_value()) << "Get " << i << " failed";
        EXPECT_EQ(get->size(), tuple_size);
        EXPECT_EQ((*get)[0], static_cast<uint8_t>(i & 0xFF));
    }
}

TEST_F(GDB614HeapInsertOptimization, Stress_RapidPageBoundaryTransitions) {
    create_bpm();
    TableHeap heap(*bpm_, dm_, file_id_);

    // Use a tuple size that fits exactly 1 per page to maximize page transitions.
    // max per page = floor((8168) / (4 + tuple_size))
    // With tuple_size = 8164: 8168 / 8168 = 1 exactly.
    constexpr size_t tuple_size = page_size - page_header_size - slot_entry_size;

    constexpr int total = 50;
    std::vector<RID> rids;
    for (int i = 0; i < total; ++i) {
        auto r = heap.insert_tuple(make_tuple(tuple_size, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value()) << "Insert " << i << " failed: " << r.error().message;
        rids.push_back(*r);
    }

    // Each tuple should be on its own page.
    std::set<PageId> pages;
    for (auto& rid : rids) {
        pages.insert(rid.page_id);
    }
    EXPECT_EQ(pages.size(), static_cast<size_t>(total))
        << "Each max-size tuple should be on its own page";

    // All readable.
    for (int i = 0; i < total; ++i) {
        auto get = heap.get_tuple(rids[static_cast<size_t>(i)]);
        ASSERT_TRUE(get.has_value()) << "Get " << i << " failed";
        EXPECT_EQ(get->size(), tuple_size);
    }
}

} // namespace
} // namespace sixseven
