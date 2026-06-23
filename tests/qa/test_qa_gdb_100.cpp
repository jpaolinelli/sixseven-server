/// QA adversarial tests for GDB-100: TableHeap tuple storage with sequential scan.
/// Tests edge cases in insert, get, update, delete, and iteration.

#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <numeric>
#include <set>
#include <vector>

using namespace sixseven;

// =============================================================================
// Test Fixture
// =============================================================================

class QA_GDB100 : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb100.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;

        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
    }

    void TearDown() override {
        bpm_.reset();
        (void)dm_.close_file(file_id_);
        std::filesystem::remove(path_);
    }

    static std::vector<uint8_t> make_tuple(size_t len, uint8_t fill = 0xAA) {
        return std::vector<uint8_t>(len, fill);
    }

    // Make a tuple with distinguishable content
    static std::vector<uint8_t> make_unique_tuple(size_t len, uint8_t id) {
        std::vector<uint8_t> data(len, id);
        // Embed id at the start for identification
        if (len >= 1)
            data[0] = id;
        return data;
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
};

// =============================================================================
// Insert Edge Cases
// =============================================================================

TEST_F(QA_GDB100, InsertEmptyTupleFails) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto r = heap.insert_tuple(std::vector<uint8_t>{});
    ASSERT_FALSE(r.has_value());
}

TEST_F(QA_GDB100, InsertOneByteTuple) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto data = make_tuple(1, 0xFF);
    auto r = heap.insert_tuple(data);
    ASSERT_TRUE(r.has_value());

    auto get = heap.get_tuple(*r);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ(get->size(), 1u);
    EXPECT_EQ((*get)[0], 0xFF);
}

TEST_F(QA_GDB100, InsertManySmallTuples) {
    TableHeap heap(*bpm_, dm_, file_id_);

    std::vector<RID> rids;
    for (int i = 0; i < 200; ++i) {
        auto r = heap.insert_tuple(make_unique_tuple(20, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value()) << "Insert " << i << " failed: " << r.error().message;
        rids.push_back(*r);
    }

    // Verify all retrievable
    for (int i = 0; i < 200; ++i) {
        auto get = heap.get_tuple(rids[i]);
        ASSERT_TRUE(get.has_value()) << "Get " << i << " failed";
        EXPECT_EQ((*get)[0], static_cast<uint8_t>(i));
    }
}

TEST_F(QA_GDB100, InsertLargeTuples) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Insert tuples that are large relative to page size (~2KB each)
    std::vector<RID> rids;
    for (int i = 0; i < 10; ++i) {
        auto data = make_tuple(2000, static_cast<uint8_t>(i));
        auto r = heap.insert_tuple(data);
        ASSERT_TRUE(r.has_value()) << "Insert " << i << " failed: " << r.error().message;
        rids.push_back(*r);
    }

    for (int i = 0; i < 10; ++i) {
        auto get = heap.get_tuple(rids[i]);
        ASSERT_TRUE(get.has_value());
        EXPECT_EQ(get->size(), 2000u);
        EXPECT_EQ((*get)[0], static_cast<uint8_t>(i));
    }
}

TEST_F(QA_GDB100, InsertMixedSizes) {
    TableHeap heap(*bpm_, dm_, file_id_);

    std::vector<std::pair<RID, size_t>> entries;
    size_t sizes[] = {1, 10, 100, 500, 1000, 50, 5, 200, 2000, 3};

    for (size_t sz : sizes) {
        auto data = make_tuple(sz, static_cast<uint8_t>(sz % 256));
        auto r = heap.insert_tuple(data);
        ASSERT_TRUE(r.has_value()) << "Insert size " << sz << " failed";
        entries.emplace_back(*r, sz);
    }

    // Verify all
    for (auto& [rid, sz] : entries) {
        auto get = heap.get_tuple(rid);
        ASSERT_TRUE(get.has_value());
        EXPECT_EQ(get->size(), sz);
    }
}

// =============================================================================
// Get Edge Cases
// =============================================================================

TEST_F(QA_GDB100, GetInvalidRid) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto r = heap.get_tuple(RID{999, 0});
    ASSERT_FALSE(r.has_value());
}

TEST_F(QA_GDB100, GetDeletedTuple) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto rid_r = heap.insert_tuple(make_tuple(50));
    ASSERT_TRUE(rid_r.has_value());

    auto del = heap.delete_tuple(*rid_r);
    ASSERT_TRUE(del.has_value());

    auto get = heap.get_tuple(*rid_r);
    ASSERT_FALSE(get.has_value());
}

// =============================================================================
// Delete Edge Cases
// =============================================================================

TEST_F(QA_GDB100, DeleteAllTuples) {
    TableHeap heap(*bpm_, dm_, file_id_);

    std::vector<RID> rids;
    for (int i = 0; i < 20; ++i) {
        auto r = heap.insert_tuple(make_tuple(100, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value());
        rids.push_back(*r);
    }

    for (auto& rid : rids) {
        auto del = heap.delete_tuple(rid);
        ASSERT_TRUE(del.has_value());
    }

    // Scan should be empty
    auto it = heap.begin();
    ASSERT_TRUE(it.has_value());
    auto next = it->next();
    ASSERT_TRUE(next.has_value()) << next.error().message;
    EXPECT_FALSE(next->has_value());
}

TEST_F(QA_GDB100, DeleteFirstAndLastTuples) {
    TableHeap heap(*bpm_, dm_, file_id_);

    std::vector<RID> rids;
    for (int i = 0; i < 10; ++i) {
        auto r = heap.insert_tuple(make_unique_tuple(100, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value());
        rids.push_back(*r);
    }

    // Delete first and last
    (void)heap.delete_tuple(rids.front());
    (void)heap.delete_tuple(rids.back());

    // Scan should have 8 tuples
    auto it = heap.begin();
    ASSERT_TRUE(it.has_value());

    int count = 0;
    for (;;) {
        auto entry = it->next();
        ASSERT_TRUE(entry.has_value()) << entry.error().message;
        if (!entry->has_value()) {
            break;
        }
        ++count;
    }
    EXPECT_EQ(count, 8);
}

// =============================================================================
// Update Edge Cases
// =============================================================================

TEST_F(QA_GDB100, UpdateSameSize) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto data = make_tuple(100, 0x11);
    auto rid = heap.insert_tuple(data);
    ASSERT_TRUE(rid.has_value());

    auto new_data = make_tuple(100, 0x22);
    auto upd = heap.update_tuple(*rid, new_data);
    ASSERT_TRUE(upd.has_value());

    auto get = heap.get_tuple(*rid);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ((*get)[0], 0x22);
    EXPECT_EQ(get->size(), 100u);
}

TEST_F(QA_GDB100, UpdateShrinkData) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto data = make_tuple(500, 0x33);
    auto rid = heap.insert_tuple(data);
    ASSERT_TRUE(rid.has_value());

    auto smaller = make_tuple(50, 0x44);
    auto upd = heap.update_tuple(*rid, smaller);
    ASSERT_TRUE(upd.has_value());

    auto get = heap.get_tuple(*rid);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ(get->size(), 50u);
    EXPECT_EQ((*get)[0], 0x44);
}

TEST_F(QA_GDB100, UpdateGrowData) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto data = make_tuple(50, 0x55);
    auto rid = heap.insert_tuple(data);
    ASSERT_TRUE(rid.has_value());

    auto bigger = make_tuple(200, 0x66);
    auto upd = heap.update_tuple(*rid, bigger);
    ASSERT_TRUE(upd.has_value());

    auto get = heap.get_tuple(*rid);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ(get->size(), 200u);
    EXPECT_EQ((*get)[0], 0x66);
}

TEST_F(QA_GDB100, UpdateEmptyDataFails) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto data = make_tuple(100);
    auto rid = heap.insert_tuple(data);
    ASSERT_TRUE(rid.has_value());

    auto upd = heap.update_tuple(*rid, std::vector<uint8_t>{});
    ASSERT_FALSE(upd.has_value());
}

// =============================================================================
// Sequential Scan
// =============================================================================

TEST_F(QA_GDB100, ScanEmptyTable) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto it = heap.begin();
    ASSERT_TRUE(it.has_value());

    auto next = it->next();
    ASSERT_TRUE(next.has_value()) << next.error().message;
    EXPECT_FALSE(next->has_value());
}

TEST_F(QA_GDB100, ScanReturnsSortedRids) {
    TableHeap heap(*bpm_, dm_, file_id_);

    for (int i = 0; i < 50; ++i) {
        (void)heap.insert_tuple(make_tuple(100));
    }

    auto it = heap.begin();
    ASSERT_TRUE(it.has_value());

    RID prev = RID::invalid();
    int count = 0;
    for (;;) {
        auto entry = it->next();
        ASSERT_TRUE(entry.has_value()) << entry.error().message;
        if (!entry->has_value()) {
            break;
        }
        auto& [rid, data] = **entry;
        if (count > 0) {
            EXPECT_GT(rid, prev) << "RIDs should be in order";
        }
        prev = rid;
        ++count;
    }
    EXPECT_EQ(count, 50);
}

TEST_F(QA_GDB100, ScanSkipsDeletedAcrossPages) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Insert enough to span multiple pages
    std::vector<RID> rids;
    for (int i = 0; i < 100; ++i) {
        auto r = heap.insert_tuple(make_tuple(200, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value());
        rids.push_back(*r);
    }

    // Delete every other tuple
    for (size_t i = 0; i < rids.size(); i += 2) {
        (void)heap.delete_tuple(rids[i]);
    }

    // Scan should return exactly 50 tuples
    auto it = heap.begin();
    ASSERT_TRUE(it.has_value());

    int count = 0;
    for (;;) {
        auto entry = it->next();
        ASSERT_TRUE(entry.has_value()) << entry.error().message;
        if (!entry->has_value()) {
            break;
        }
        auto& [rid, data] = **entry;
        EXPECT_EQ(data.size(), 200u);
        // The fill byte should be an odd number (even-indexed tuples were deleted)
        EXPECT_TRUE(data[0] % 2 == 1)
            << "Expected odd fill byte, got " << static_cast<int>(data[0]);
        ++count;
    }
    EXPECT_EQ(count, 50);
}

TEST_F(QA_GDB100, ScanAfterDeleteAndReinsert) {
    TableHeap heap(*bpm_, dm_, file_id_);

    std::vector<RID> rids;
    for (int i = 0; i < 20; ++i) {
        auto r = heap.insert_tuple(make_unique_tuple(100, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value());
        rids.push_back(*r);
    }

    // Delete half
    for (int i = 0; i < 10; ++i) {
        (void)heap.delete_tuple(rids[i]);
    }

    // Reinsert 10 new
    for (int i = 0; i < 10; ++i) {
        auto r = heap.insert_tuple(make_unique_tuple(100, static_cast<uint8_t>(100 + i)));
        ASSERT_TRUE(r.has_value());
    }

    // Scan should return 20 tuples
    auto it = heap.begin();
    ASSERT_TRUE(it.has_value());

    int count = 0;
    for (;;) {
        auto entry = it->next();
        ASSERT_TRUE(entry.has_value()) << entry.error().message;
        if (!entry->has_value()) {
            break;
        }
        ++count;
    }
    EXPECT_EQ(count, 20);
}

// =============================================================================
// Space Reuse
// =============================================================================

TEST_F(QA_GDB100, SpaceReuseAfterDelete) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Insert tuples
    std::vector<RID> rids;
    for (int i = 0; i < 20; ++i) {
        auto r = heap.insert_tuple(make_tuple(100));
        ASSERT_TRUE(r.has_value());
        rids.push_back(*r);
    }

    auto count_before = heap.page_count();
    ASSERT_TRUE(count_before.has_value());

    // Delete all
    for (auto& rid : rids) {
        (void)heap.delete_tuple(rid);
    }

    // Re-insert same amount — should reuse space
    for (int i = 0; i < 20; ++i) {
        auto r = heap.insert_tuple(make_tuple(100));
        ASSERT_TRUE(r.has_value());
    }

    auto count_after = heap.page_count();
    ASSERT_TRUE(count_after.has_value());
    // Page count should not grow much (space reuse)
    EXPECT_LE(*count_after, *count_before + 1);
}

// =============================================================================
// Page Count
// =============================================================================

TEST_F(QA_GDB100, PageCountEmpty) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto pc = heap.page_count();
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(*pc, 0u);
}

TEST_F(QA_GDB100, PageCountGrowsWithInserts) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto pc0 = heap.page_count();
    ASSERT_TRUE(pc0.has_value());

    // Insert enough large tuples to force multiple pages
    for (int i = 0; i < 20; ++i) {
        (void)heap.insert_tuple(make_tuple(1000));
    }

    auto pc1 = heap.page_count();
    ASSERT_TRUE(pc1.has_value());
    EXPECT_GT(*pc1, *pc0);
}

// =============================================================================
// Stress
// =============================================================================

TEST_F(QA_GDB100, StressInsertDeleteScan) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Insert 300 tuples
    std::vector<RID> rids;
    for (int i = 0; i < 300; ++i) {
        auto data = make_unique_tuple(50 + (i % 200), static_cast<uint8_t>(i % 256));
        auto r = heap.insert_tuple(data);
        ASSERT_TRUE(r.has_value()) << "Insert " << i << " failed";
        rids.push_back(*r);
    }

    // Delete every third
    std::set<RID> deleted;
    for (size_t i = 0; i < rids.size(); i += 3) {
        (void)heap.delete_tuple(rids[i]);
        deleted.insert(rids[i]);
    }

    // Scan and count
    auto it = heap.begin();
    ASSERT_TRUE(it.has_value());

    int count = 0;
    for (;;) {
        auto entry = it->next();
        ASSERT_TRUE(entry.has_value()) << entry.error().message;
        if (!entry->has_value()) {
            break;
        }
        auto& [rid, data] = **entry;
        // Verify this isn't a deleted tuple
        EXPECT_EQ(deleted.count(rid), 0u) << "Scan returned deleted RID";
        ++count;
    }
    EXPECT_EQ(count, 200); // 300 - 100 deleted
}
