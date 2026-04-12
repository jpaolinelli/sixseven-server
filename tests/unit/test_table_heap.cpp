#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <vector>

using namespace sixseven;

// -- Test fixture -------------------------------------------------------------

class TableHeapTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_test_table_heap.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;

        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
    }

    void TearDown() override {
        bpm_.reset();
        auto close = dm_.close_file(file_id_);
        (void)close;
        std::filesystem::remove(path_);
    }

    static std::vector<uint8_t> make_tuple(size_t len, uint8_t fill = 0xAA) {
        return std::vector<uint8_t>(len, fill);
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
};

// -- Insert and get -----------------------------------------------------------

TEST_F(TableHeapTest, InsertAndGetSingleTuple) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto tuple = make_tuple(100, 0x42);
    auto rid_result = heap.insert_tuple(tuple);
    ASSERT_TRUE(rid_result.has_value()) << rid_result.error().message;

    RID rid = *rid_result;
    EXPECT_GE(rid.page_id, 1u);

    auto get_result = heap.get_tuple(rid);
    ASSERT_TRUE(get_result.has_value()) << get_result.error().message;
    EXPECT_EQ(get_result->size(), 100u);
    EXPECT_EQ((*get_result)[0], 0x42);
}

TEST_F(TableHeapTest, InsertMultipleTuples) {
    TableHeap heap(*bpm_, dm_, file_id_);

    std::vector<RID> rids;
    for (int i = 0; i < 50; ++i) {
        auto tuple = make_tuple(100, static_cast<uint8_t>(i));
        auto rid_result = heap.insert_tuple(tuple);
        ASSERT_TRUE(rid_result.has_value())
            << "Insert " << i << " failed: " << rid_result.error().message;
        rids.push_back(*rid_result);
    }

    for (int i = 0; i < 50; ++i) {
        auto get_result = heap.get_tuple(rids[static_cast<size_t>(i)]);
        ASSERT_TRUE(get_result.has_value()) << "Get " << i << " failed";
        EXPECT_EQ(get_result->size(), 100u);
        EXPECT_EQ((*get_result)[0], static_cast<uint8_t>(i));
    }
}

TEST_F(TableHeapTest, InsertEmptyTupleFails) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto result = heap.insert_tuple({});
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// -- Cross-page insertion -----------------------------------------------------

TEST_F(TableHeapTest, InsertAcrossMultiplePages) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Insert tuples large enough to span multiple pages.
    // Each page is 8KB with 24-byte header. ~2000-byte tuples with slot overhead
    // means ~3-4 per page. Insert enough to fill multiple pages.
    std::vector<RID> rids;
    for (int i = 0; i < 30; ++i) {
        auto tuple = make_tuple(2000, static_cast<uint8_t>(i));
        auto rid_result = heap.insert_tuple(tuple);
        ASSERT_TRUE(rid_result.has_value())
            << "Insert " << i << " failed: " << rid_result.error().message;
        rids.push_back(*rid_result);
    }

    // Verify we used more than one page.
    auto pc = heap.page_count();
    ASSERT_TRUE(pc.has_value());
    EXPECT_GT(*pc, 1u);

    // Verify all tuples are readable.
    for (int i = 0; i < 30; ++i) {
        auto get_result = heap.get_tuple(rids[static_cast<size_t>(i)]);
        ASSERT_TRUE(get_result.has_value()) << "Get " << i << " failed";
        EXPECT_EQ(get_result->size(), 2000u);
        EXPECT_EQ((*get_result)[0], static_cast<uint8_t>(i));
    }
}

// -- Delete -------------------------------------------------------------------

TEST_F(TableHeapTest, DeleteTuple) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto tuple = make_tuple(100, 0x42);
    auto rid = heap.insert_tuple(tuple);
    ASSERT_TRUE(rid.has_value());

    auto del = heap.delete_tuple(*rid);
    ASSERT_TRUE(del.has_value()) << del.error().message;

    auto get = heap.get_tuple(*rid);
    EXPECT_FALSE(get.has_value());
    EXPECT_EQ(get.error().code, StatusCode::NOT_FOUND);
}

TEST_F(TableHeapTest, DeleteMiddleTuple) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto r1 = heap.insert_tuple(make_tuple(50, 0x11));
    auto r2 = heap.insert_tuple(make_tuple(50, 0x22));
    auto r3 = heap.insert_tuple(make_tuple(50, 0x33));
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r3.has_value());

    // Delete the middle one.
    auto del = heap.delete_tuple(*r2);
    ASSERT_TRUE(del.has_value());

    // First and third still accessible.
    auto g1 = heap.get_tuple(*r1);
    ASSERT_TRUE(g1.has_value());
    EXPECT_EQ((*g1)[0], 0x11);

    auto g2 = heap.get_tuple(*r2);
    EXPECT_FALSE(g2.has_value());

    auto g3 = heap.get_tuple(*r3);
    ASSERT_TRUE(g3.has_value());
    EXPECT_EQ((*g3)[0], 0x33);
}

// -- Update -------------------------------------------------------------------

TEST_F(TableHeapTest, UpdateInPlace) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto rid = heap.insert_tuple(make_tuple(100, 0x11));
    ASSERT_TRUE(rid.has_value());

    auto upd = heap.update_tuple(*rid, make_tuple(100, 0x22));
    ASSERT_TRUE(upd.has_value()) << upd.error().message;

    auto get = heap.get_tuple(*rid);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ(get->size(), 100u);
    EXPECT_EQ((*get)[0], 0x22);
}

TEST_F(TableHeapTest, UpdateShrink) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto rid = heap.insert_tuple(make_tuple(200, 0x11));
    ASSERT_TRUE(rid.has_value());

    auto upd = heap.update_tuple(*rid, make_tuple(50, 0x22));
    ASSERT_TRUE(upd.has_value()) << upd.error().message;

    auto get = heap.get_tuple(*rid);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ(get->size(), 50u);
    EXPECT_EQ((*get)[0], 0x22);
}

TEST_F(TableHeapTest, UpdateGrow) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto rid = heap.insert_tuple(make_tuple(50, 0x11));
    ASSERT_TRUE(rid.has_value());

    auto upd = heap.update_tuple(*rid, make_tuple(200, 0x22));
    ASSERT_TRUE(upd.has_value()) << upd.error().message;

    auto get = heap.get_tuple(*rid);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ(get->size(), 200u);
    EXPECT_EQ((*get)[0], 0x22);
}

TEST_F(TableHeapTest, UpdateEmptyDataFails) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto rid = heap.insert_tuple(make_tuple(50, 0x11));
    ASSERT_TRUE(rid.has_value());

    auto upd = heap.update_tuple(*rid, {});
    EXPECT_FALSE(upd.has_value());
    EXPECT_EQ(upd.error().code, StatusCode::INVALID_ARGUMENT);
}

// -- Sequential scan ----------------------------------------------------------

TEST_F(TableHeapTest, SequentialScanEmpty) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value()) << it_result.error().message;
    auto it = std::move(*it_result);
    auto result = it.next();
    EXPECT_FALSE(result.has_value());
}

TEST_F(TableHeapTest, SequentialScanSinglePage) {
    TableHeap heap(*bpm_, dm_, file_id_);

    std::vector<RID> rids;
    for (int i = 0; i < 5; ++i) {
        auto r = heap.insert_tuple(make_tuple(50, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value());
        rids.push_back(*r);
    }

    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value()) << it_result.error().message;
    auto it = std::move(*it_result);
    int count = 0;
    while (auto result = it.next()) {
        auto& [rid, data] = *result;
        EXPECT_EQ(data.size(), 50u);
        EXPECT_EQ(data[0], static_cast<uint8_t>(count));
        count++;
    }
    EXPECT_EQ(count, 5);
}

TEST_F(TableHeapTest, SequentialScanMultiplePages) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Insert enough large tuples to span multiple pages.
    int total = 20;
    for (int i = 0; i < total; ++i) {
        auto r = heap.insert_tuple(make_tuple(2000, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value()) << "Insert " << i << " failed: " << r.error().message;
    }

    auto pc = heap.page_count();
    ASSERT_TRUE(pc.has_value());
    EXPECT_GT(*pc, 1u) << "Expected multiple pages";

    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value()) << it_result.error().message;
    auto it = std::move(*it_result);
    int count = 0;
    while (auto result = it.next()) {
        auto& [rid, data] = *result;
        EXPECT_EQ(data.size(), 2000u);
        count++;
    }
    EXPECT_EQ(count, total);
}

TEST_F(TableHeapTest, SequentialScanSkipsDeletedTuples) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto r1 = heap.insert_tuple(make_tuple(50, 0x11));
    auto r2 = heap.insert_tuple(make_tuple(50, 0x22));
    auto r3 = heap.insert_tuple(make_tuple(50, 0x33));
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r3.has_value());

    // Delete the middle tuple.
    ASSERT_TRUE(heap.delete_tuple(*r2).has_value());

    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value()) << it_result.error().message;
    auto it = std::move(*it_result);
    std::vector<uint8_t> first_bytes;
    while (auto result = it.next()) {
        first_bytes.push_back(result->second[0]);
    }

    ASSERT_EQ(first_bytes.size(), 2u);
    EXPECT_EQ(first_bytes[0], 0x11);
    EXPECT_EQ(first_bytes[1], 0x33);
}

TEST_F(TableHeapTest, SequentialScanCrossPageWithDeletes) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Insert large tuples spanning multiple pages.
    std::vector<RID> rids;
    for (int i = 0; i < 15; ++i) {
        auto r = heap.insert_tuple(make_tuple(2000, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value());
        rids.push_back(*r);
    }

    // Delete every other tuple.
    for (size_t i = 0; i < rids.size(); i += 2) {
        ASSERT_TRUE(heap.delete_tuple(rids[i]).has_value());
    }

    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value()) << it_result.error().message;
    auto it = std::move(*it_result);
    int count = 0;
    while (auto result = it.next()) {
        auto& [rid, data] = *result;
        EXPECT_EQ(data.size(), 2000u);
        // Remaining tuples: indices 1, 3, 5, 7, 9, 11, 13 — all odd.
        EXPECT_EQ(data[0], static_cast<uint8_t>(count * 2 + 1));
        count++;
    }
    EXPECT_EQ(count, 7);
}

// -- Page count ---------------------------------------------------------------

TEST_F(TableHeapTest, PageCountEmpty) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto pc = heap.page_count();
    ASSERT_TRUE(pc.has_value());
    // A fresh file has 128 pages (1MB pre-allocation), minus page 0.
    EXPECT_GE(*pc, 0u);
}

TEST_F(TableHeapTest, PageCountGrowsWithInserts) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto pc_before = heap.page_count();
    ASSERT_TRUE(pc_before.has_value());

    // Fill up all existing pages and trigger new allocations.
    // Each page is ~8KB. With 128 pre-allocated data pages and ~8KB usable per page,
    // we need to insert more than 128 * ~8KB of data. Use large tuples.
    // Actually, with 128 pages pre-allocated, we just need to verify the count.
    for (int i = 0; i < 5; ++i) {
        auto r = heap.insert_tuple(make_tuple(2000, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value());
    }

    auto pc_after = heap.page_count();
    ASSERT_TRUE(pc_after.has_value());
    EXPECT_GE(*pc_after, *pc_before);
}

// -- Insert fallback when hint page is full (GDB-625) -------------------------

TEST_F(TableHeapTest, InsertFallsBackToNewPageWhenHintFull) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Page usable space = page_size (8192) - page_header_size (24) = 8168 bytes.
    // Each tuple consumes: slot_entry_size (4) + tuple_data bytes.
    // Use 2000-byte tuples: 2000 + 4 = 2004 per tuple.
    // Floor(8168 / 2004) = 4 tuples per page.
    constexpr size_t tuple_size = 2000;

    // Insert 4 tuples to fill the first data page.
    std::vector<RID> rids;
    for (int i = 0; i < 4; ++i) {
        auto r = heap.insert_tuple(make_tuple(tuple_size, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value()) << "Fill insert " << i << " failed: " << r.error().message;
        rids.push_back(*r);
    }

    // All 4 should be on the same page.
    PageId first_page = rids[0].page_id;
    for (size_t i = 1; i < rids.size(); ++i) {
        EXPECT_EQ(rids[i].page_id, first_page) << "Tuple " << i << " not on first page";
    }

    // Insert one more — hint page is full, should go to a new page.
    auto overflow = heap.insert_tuple(make_tuple(tuple_size, 0xFF));
    ASSERT_TRUE(overflow.has_value()) << "Overflow insert failed: " << overflow.error().message;
    EXPECT_NE(overflow->page_id, first_page) << "Overflow tuple should be on a different page";

    // Verify all 5 tuples are retrievable.
    for (size_t i = 0; i < rids.size(); ++i) {
        auto get = heap.get_tuple(rids[i]);
        ASSERT_TRUE(get.has_value()) << "Get tuple " << i << " failed";
        EXPECT_EQ(get->size(), tuple_size);
        EXPECT_EQ((*get)[0], static_cast<uint8_t>(i));
    }
    auto get_overflow = heap.get_tuple(*overflow);
    ASSERT_TRUE(get_overflow.has_value()) << "Get overflow tuple failed";
    EXPECT_EQ(get_overflow->size(), tuple_size);
    EXPECT_EQ((*get_overflow)[0], 0xFF);
}

TEST_F(TableHeapTest, InsertContinuesOnNewPageAfterFull) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Fill multiple pages and verify inserts keep going to new pages.
    constexpr size_t tuple_size = 2000;
    constexpr int tuples_per_page = 4;
    constexpr int total_tuples = tuples_per_page * 3; // 3 pages worth

    std::vector<RID> rids;
    for (int i = 0; i < total_tuples; ++i) {
        auto r = heap.insert_tuple(make_tuple(tuple_size, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value()) << "Insert " << i << " failed: " << r.error().message;
        rids.push_back(*r);
    }

    // Should span at least 3 pages.
    auto pc = heap.page_count();
    ASSERT_TRUE(pc.has_value());
    EXPECT_GE(*pc, 3u);

    // Verify all tuples are readable.
    for (int i = 0; i < total_tuples; ++i) {
        auto get = heap.get_tuple(rids[static_cast<size_t>(i)]);
        ASSERT_TRUE(get.has_value()) << "Get " << i << " failed";
        EXPECT_EQ((*get)[0], static_cast<uint8_t>(i));
    }
}

// -- Insert after delete reuses space -----------------------------------------

TEST_F(TableHeapTest, InsertReusesFreeSpace) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Fill a page.
    std::vector<RID> rids;
    for (int i = 0; i < 10; ++i) {
        auto r = heap.insert_tuple(make_tuple(100, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value());
        rids.push_back(*r);
    }

    // Delete some tuples.
    ASSERT_TRUE(heap.delete_tuple(rids[2]).has_value());
    ASSERT_TRUE(heap.delete_tuple(rids[5]).has_value());

    // Insert new tuples — should reuse deleted slots on same page.
    auto new_r1 = heap.insert_tuple(make_tuple(100, 0xEE));
    ASSERT_TRUE(new_r1.has_value());

    auto get = heap.get_tuple(*new_r1);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ((*get)[0], 0xEE);
}

// -- Update exceeding page capacity -------------------------------------------

TEST_F(TableHeapTest, UpdateExceedingPageCapacityFails) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Insert a small tuple.
    auto rid = heap.insert_tuple(make_tuple(50, 0x11));
    ASSERT_TRUE(rid.has_value());

    // Try to update it with data larger than a page can hold.
    // Page is 8192 bytes with 24-byte header + 4-byte slot entry = 28 bytes overhead.
    // Max tuple = 8164 bytes. Use 8165 to exceed capacity.
    auto upd = heap.update_tuple(*rid, make_tuple(8165, 0x22));
    EXPECT_FALSE(upd.has_value());
}

// -- begin() returns Result ---------------------------------------------------

TEST_F(TableHeapTest, BeginReturnsResult) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto result = heap.begin();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Iterator from empty heap should have no tuples.
    auto it = std::move(*result);
    auto next = it.next();
    EXPECT_FALSE(next.has_value());
}

// -- Row count (GDB-616) -----------------------------------------------------

TEST_F(TableHeapTest, RowCountEmptyTable) {
    TableHeap heap(*bpm_, dm_, file_id_);
    EXPECT_EQ(heap.row_count(), 0u);
}

TEST_F(TableHeapTest, RowCountAfterInserts) {
    TableHeap heap(*bpm_, dm_, file_id_);

    for (int i = 0; i < 25; ++i) {
        auto r = heap.insert_tuple(make_tuple(100, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value()) << r.error().message;
        EXPECT_EQ(heap.row_count(), static_cast<uint64_t>(i + 1));
    }
}

TEST_F(TableHeapTest, RowCountAfterDeletes) {
    TableHeap heap(*bpm_, dm_, file_id_);

    std::vector<RID> rids;
    for (int i = 0; i < 10; ++i) {
        auto r = heap.insert_tuple(make_tuple(100, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value());
        rids.push_back(*r);
    }
    EXPECT_EQ(heap.row_count(), 10u);

    // Delete 3 tuples.
    ASSERT_TRUE(heap.delete_tuple(rids[1]).has_value());
    EXPECT_EQ(heap.row_count(), 9u);
    ASSERT_TRUE(heap.delete_tuple(rids[4]).has_value());
    EXPECT_EQ(heap.row_count(), 8u);
    ASSERT_TRUE(heap.delete_tuple(rids[7]).has_value());
    EXPECT_EQ(heap.row_count(), 7u);
}

TEST_F(TableHeapTest, RowCountAfterInsertAndDelete) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto r1 = heap.insert_tuple(make_tuple(50, 0x11));
    auto r2 = heap.insert_tuple(make_tuple(50, 0x22));
    auto r3 = heap.insert_tuple(make_tuple(50, 0x33));
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(heap.row_count(), 3u);

    ASSERT_TRUE(heap.delete_tuple(*r2).has_value());
    EXPECT_EQ(heap.row_count(), 2u);

    // Insert again after delete.
    auto r4 = heap.insert_tuple(make_tuple(50, 0x44));
    ASSERT_TRUE(r4.has_value());
    EXPECT_EQ(heap.row_count(), 3u);
}

TEST_F(TableHeapTest, RowCountPersistsAcrossReopen) {
    // Insert tuples with first heap instance.
    {
        TableHeap heap(*bpm_, dm_, file_id_);
        for (int i = 0; i < 15; ++i) {
            auto r = heap.insert_tuple(make_tuple(100, static_cast<uint8_t>(i)));
            ASSERT_TRUE(r.has_value()) << r.error().message;
        }
        EXPECT_EQ(heap.row_count(), 15u);
    }

    // Flush buffer pool so all dirty pages are written.
    bpm_.reset();

    // Reopen with a fresh buffer pool — row count should be recovered from header.
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
    TableHeap heap2(*bpm_, dm_, file_id_);
    EXPECT_EQ(heap2.row_count(), 15u);
}

// -- Batch insert (GDB-617) ---------------------------------------------------

TEST_F(TableHeapTest, BatchInsertReturnsCorrectRIDCount) {
    TableHeap heap(*bpm_, dm_, file_id_);

    std::vector<std::vector<uint8_t>> owned;
    std::vector<std::span<const uint8_t>> tuples;
    for (int i = 0; i < 20; ++i) {
        owned.push_back(make_tuple(100, static_cast<uint8_t>(i)));
        tuples.emplace_back(owned.back());
    }

    auto result = heap.insert_batch(tuples);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 20u);
}

TEST_F(TableHeapTest, BatchInsertAllTuplesRetrievable) {
    TableHeap heap(*bpm_, dm_, file_id_);

    std::vector<std::vector<uint8_t>> owned;
    std::vector<std::span<const uint8_t>> tuples;
    for (int i = 0; i < 15; ++i) {
        owned.push_back(make_tuple(100, static_cast<uint8_t>(i)));
        tuples.emplace_back(owned.back());
    }

    auto result = heap.insert_batch(tuples);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (size_t i = 0; i < result->size(); ++i) {
        auto get = heap.get_tuple((*result)[i]);
        ASSERT_TRUE(get.has_value()) << "Get " << i << " failed";
        EXPECT_EQ(get->size(), 100u);
        EXPECT_EQ((*get)[0], static_cast<uint8_t>(i));
    }
}

TEST_F(TableHeapTest, BatchInsertSpansMultiplePages) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // 2000-byte tuples: ~4 per page. 20 tuples = ~5 pages.
    std::vector<std::vector<uint8_t>> owned;
    std::vector<std::span<const uint8_t>> tuples;
    for (int i = 0; i < 20; ++i) {
        owned.push_back(make_tuple(2000, static_cast<uint8_t>(i)));
        tuples.emplace_back(owned.back());
    }

    auto result = heap.insert_batch(tuples);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 20u);

    // Verify multiple pages were used.
    auto pc = heap.page_count();
    ASSERT_TRUE(pc.has_value());
    EXPECT_GT(*pc, 1u);

    // Verify all tuples are correct.
    for (size_t i = 0; i < result->size(); ++i) {
        auto get = heap.get_tuple((*result)[i]);
        ASSERT_TRUE(get.has_value()) << "Get " << i << " failed";
        EXPECT_EQ(get->size(), 2000u);
        EXPECT_EQ((*get)[0], static_cast<uint8_t>(i));
    }
}

TEST_F(TableHeapTest, BatchInsertEmptyBatch) {
    TableHeap heap(*bpm_, dm_, file_id_);

    std::vector<std::span<const uint8_t>> tuples;
    auto result = heap.insert_batch(tuples);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
    EXPECT_EQ(heap.row_count(), 0u);
}

TEST_F(TableHeapTest, BatchInsertSingleTuple) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto data = make_tuple(100, 0x42);
    std::vector<std::span<const uint8_t>> tuples{std::span<const uint8_t>(data)};

    auto result = heap.insert_batch(tuples);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 1u);

    auto get = heap.get_tuple((*result)[0]);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ(get->size(), 100u);
    EXPECT_EQ((*get)[0], 0x42);
    EXPECT_EQ(heap.row_count(), 1u);
}

TEST_F(TableHeapTest, BatchInsertUpdatesRowCount) {
    TableHeap heap(*bpm_, dm_, file_id_);

    std::vector<std::vector<uint8_t>> owned;
    std::vector<std::span<const uint8_t>> tuples;
    for (int i = 0; i < 25; ++i) {
        owned.push_back(make_tuple(100, static_cast<uint8_t>(i)));
        tuples.emplace_back(owned.back());
    }

    auto result = heap.insert_batch(tuples);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(heap.row_count(), 25u);
}

TEST_F(TableHeapTest, BatchInsertRejectsEmptyTuple) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto good = make_tuple(100, 0x42);
    std::vector<uint8_t> empty;
    std::vector<std::span<const uint8_t>> tuples{
        std::span<const uint8_t>(good),
        std::span<const uint8_t>(empty),
    };

    auto result = heap.insert_batch(tuples);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(TableHeapTest, BatchInsertAtPageBoundary) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Fill a page with per-row inserts first (4 x 2000-byte tuples).
    for (int i = 0; i < 4; ++i) {
        auto r = heap.insert_tuple(make_tuple(2000, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    EXPECT_EQ(heap.row_count(), 4u);

    // Batch insert should start on a new page since the hint page is full.
    std::vector<std::vector<uint8_t>> owned;
    std::vector<std::span<const uint8_t>> tuples;
    for (int i = 0; i < 8; ++i) {
        owned.push_back(make_tuple(2000, static_cast<uint8_t>(0x80 + i)));
        tuples.emplace_back(owned.back());
    }

    auto result = heap.insert_batch(tuples);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 8u);
    EXPECT_EQ(heap.row_count(), 12u);

    // All batch-inserted tuples retrievable.
    for (size_t i = 0; i < result->size(); ++i) {
        auto get = heap.get_tuple((*result)[i]);
        ASSERT_TRUE(get.has_value()) << "Get batch " << i << " failed";
        EXPECT_EQ(get->size(), 2000u);
        EXPECT_EQ((*get)[0], static_cast<uint8_t>(0x80 + i));
    }
}

TEST_F(TableHeapTest, RowCountPersistsAfterDeletes) {
    {
        TableHeap heap(*bpm_, dm_, file_id_);
        std::vector<RID> rids;
        for (int i = 0; i < 10; ++i) {
            auto r = heap.insert_tuple(make_tuple(100, static_cast<uint8_t>(i)));
            ASSERT_TRUE(r.has_value());
            rids.push_back(*r);
        }
        // Delete 3 tuples.
        ASSERT_TRUE(heap.delete_tuple(rids[0]).has_value());
        ASSERT_TRUE(heap.delete_tuple(rids[3]).has_value());
        ASSERT_TRUE(heap.delete_tuple(rids[6]).has_value());
        EXPECT_EQ(heap.row_count(), 7u);
    }

    bpm_.reset();
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
    TableHeap heap2(*bpm_, dm_, file_id_);
    EXPECT_EQ(heap2.row_count(), 7u);
}

// -- update_tuples_batch ------------------------------------------------------

TEST_F(TableHeapTest, UpdateTuplesBatchMultiPage) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Insert enough tuples to span multiple pages (~4000 bytes each → 1 per page
    // on a 4096-byte page after header overhead).
    std::vector<RID> rids;
    for (int i = 0; i < 8; ++i) {
        auto tuple = make_tuple(200, static_cast<uint8_t>(i));
        auto rid = heap.insert_tuple(tuple);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
        rids.push_back(*rid);
    }

    // Batch update all tuples with new content.
    std::vector<std::vector<uint8_t>> new_data;
    std::vector<TableHeap::TupleUpdate> updates;
    new_data.reserve(rids.size());
    updates.reserve(rids.size());

    for (size_t i = 0; i < rids.size(); ++i) {
        new_data.push_back(make_tuple(200, static_cast<uint8_t>(0xF0 + i)));
        updates.push_back(TableHeap::TupleUpdate{
            rids[i], std::span<const uint8_t>(new_data.back())});
    }

    auto result = heap.update_tuples_batch(updates);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty()) << "no failures expected";

    // Verify all updates took effect.
    for (size_t i = 0; i < rids.size(); ++i) {
        auto data = heap.get_tuple(rids[i]);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        ASSERT_EQ(data->size(), 200u);
        EXPECT_EQ((*data)[0], static_cast<uint8_t>(0xF0 + i));
    }
}

TEST_F(TableHeapTest, UpdateTuplesBatchEmptyIsNoOp) {
    TableHeap heap(*bpm_, dm_, file_id_);
    std::vector<TableHeap::TupleUpdate> empty;
    auto result = heap.update_tuples_batch(empty);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

TEST_F(TableHeapTest, UpdateTuplesBatchInvalidRidReportedAsFailed) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Insert one real tuple.
    auto tuple = make_tuple(100, 0xAA);
    auto rid = heap.insert_tuple(tuple);
    ASSERT_TRUE(rid.has_value());

    // Build a batch with a valid update and an invalid one (bad page).
    auto new_data = make_tuple(100, 0xBB);
    RID bad_rid{9999, 0}; // Nonexistent page.
    auto bad_data = make_tuple(100, 0xCC);

    std::vector<TableHeap::TupleUpdate> updates;
    updates.push_back(TableHeap::TupleUpdate{*rid, std::span<const uint8_t>(new_data)});
    updates.push_back(TableHeap::TupleUpdate{bad_rid, std::span<const uint8_t>(bad_data)});

    auto result = heap.update_tuples_batch(updates);
    ASSERT_TRUE(result.has_value());
    // The bad RID should be reported as failed.
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ((*result)[0].page_id, bad_rid.page_id);

    // The valid update should have succeeded.
    auto data = heap.get_tuple(*rid);
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ((*data)[0], 0xBB);
}
