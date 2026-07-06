/// QA adversarial tests for GDB-1234: re-enabling
/// QA_GDB714_StorageAdversarial.ConcurrentInsertsAcrossPages after the
/// TableHeap last_insert_page_ hint race fix (atomic hint + local capture in
/// insert_tuple/insert_batch; row_count_ atomic).
///
/// These tests stress the fix beyond the implementer's 200/200 repeat and the
/// existing regression test's 2 threads x 300 inserts, to build confidence
/// the race is genuinely gone rather than just less likely to reproduce:
///  - more threads, more inserts per thread, larger tuples that force more
///    page-hint transitions;
///  - explicit verification that every returned RID actually holds its
///    tuple's bytes (catches "RID points at the wrong page" corruption);
///  - explicit verification the heap and its buffer pool remain fully usable
///    after the race window (no leaked pin blocking further fetch/evict, no
///    "page N is not pinned" error surfaced anywhere).

#include "sixseven/common/status.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace sixseven;

namespace {
std::vector<uint8_t> bytes(size_t size, uint8_t fill) {
    return std::vector<uint8_t>(size, fill);
}
} // namespace

class QA_GDB1234_ConcurrentInsertStress : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1234_stress.db";
        std::error_code ec;
        std::filesystem::remove(path_, ec);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;
        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
        heap_ = std::make_unique<TableHeap>(*bpm_, dm_, file_id_);
    }

    void TearDown() override {
        heap_.reset();
        bpm_.reset();
        (void)dm_.close_file(file_id_);
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<TableHeap> heap_;
};

// Heavier than the base regression test: 4 threads x 500 inserts of 200-byte
// tuples (~40 tuples/page => many more page-hint transitions than the
// existing 2x300x32-byte regression test).
TEST_F(QA_GDB1234_ConcurrentInsertStress, FourThreadsManyPagesEveryRidHoldsItsOwnBytes) {
    constexpr size_t kThreads = 4;
    constexpr size_t kPerThread = 500;
    constexpr size_t kTupleSize = 200;

    std::atomic<size_t> errors{0};

    auto worker = [&](uint8_t fill) {
        for (size_t i = 0; i < kPerThread; ++i) {
            auto rid = heap_->insert_tuple(bytes(kTupleSize, fill));
            if (!rid.has_value()) {
                ++errors;
                ADD_FAILURE() << "insert failed: " << rid.error().message;
                continue;
            }
            // Immediately verify the returned RID actually holds this
            // tuple's bytes on the page it claims to be on -- catches a
            // stale-hint race that returns a RID pointing at the wrong page
            // or an unpinned/torn page.
            auto data = heap_->get_tuple(*rid);
            if (!data.has_value() || data->size() != kTupleSize || (*data)[0] != fill) {
                ++errors;
                ADD_FAILURE() << "RID " << rid->page_id << ":" << rid->slot_id
                              << " did not hold its own tuple (fill=" << static_cast<int>(fill)
                              << ")";
            }
        }
    };

    std::vector<std::thread> threads;
    for (size_t t = 0; t < kThreads; ++t) {
        threads.emplace_back(worker, static_cast<uint8_t>(0x10 + t));
    }
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(errors.load(), 0u);
    EXPECT_EQ(heap_->row_count(), kThreads * kPerThread);

    // The buffer pool must still be fully usable afterward: no leaked pin
    // should block fetching/evicting any page the race touched. A full scan
    // exercises fetch_page/unpin_page across every page in the file.
    auto it = heap_->begin();
    ASSERT_TRUE(it.has_value()) << it.error().message;
    std::unordered_set<uint32_t> pages_seen;
    size_t scanned = 0;
    for (;;) {
        auto row = it->next();
        ASSERT_TRUE(row.has_value()) << "scan error (would surface a leaked/unpinned page): "
                                     << row.error().message;
        if (!row->has_value()) {
            break;
        }
        pages_seen.insert((*row)->first.page_id);
        ++scanned;
    }
    EXPECT_EQ(scanned, kThreads * kPerThread);
    EXPECT_GT(pages_seen.size(), 1u) << "expected multiple data pages to be touched";

    // The heap must still accept new inserts (no deadlocked/corrupted state
    // left behind by a race that "almost" happened).
    auto post_rid = heap_->insert_tuple(bytes(16, 0xEE));
    ASSERT_TRUE(post_rid.has_value()) << post_rid.error().message;
    auto post_data = heap_->get_tuple(*post_rid);
    ASSERT_TRUE(post_data.has_value());
    EXPECT_EQ(*post_data, bytes(16, 0xEE));
    EXPECT_EQ(heap_->row_count(), kThreads * kPerThread + 1);
}

// insert_batch shares the same last_insert_page_ hint machinery; stress it
// concurrently with insert_tuple to check for cross-path races.
TEST_F(QA_GDB1234_ConcurrentInsertStress, ConcurrentBatchAndSingleInsertsAcrossPages) {
    constexpr size_t kBatchThreads = 2;
    constexpr size_t kBatchesPerThread = 40;
    constexpr size_t kBatchSize = 5;
    constexpr size_t kSingleThreads = 2;
    constexpr size_t kSinglePerThread = 300;

    std::atomic<size_t> errors{0};

    auto batch_worker = [&](uint8_t fill) {
        for (size_t b = 0; b < kBatchesPerThread; ++b) {
            std::vector<std::vector<uint8_t>> owners;
            std::vector<std::span<const uint8_t>> spans;
            for (size_t i = 0; i < kBatchSize; ++i) {
                owners.push_back(bytes(64, fill));
            }
            for (auto& o : owners) {
                spans.emplace_back(o);
            }
            auto rids = heap_->insert_batch(spans);
            if (!rids.has_value() || rids->size() != kBatchSize) {
                ++errors;
                ADD_FAILURE() << "batch insert failed";
                continue;
            }
            for (auto rid : *rids) {
                auto data = heap_->get_tuple(rid);
                if (!data.has_value() || data->size() != 64 || (*data)[0] != fill) {
                    ++errors;
                    ADD_FAILURE() << "batch RID did not hold its own tuple";
                }
            }
        }
    };

    auto single_worker = [&](uint8_t fill) {
        for (size_t i = 0; i < kSinglePerThread; ++i) {
            auto rid = heap_->insert_tuple(bytes(48, fill));
            if (!rid.has_value()) {
                ++errors;
                ADD_FAILURE() << "single insert failed: " << rid.error().message;
                continue;
            }
            auto data = heap_->get_tuple(*rid);
            if (!data.has_value() || (*data)[0] != fill) {
                ++errors;
                ADD_FAILURE() << "single RID did not hold its own tuple";
            }
        }
    };

    std::vector<std::thread> threads;
    for (size_t t = 0; t < kBatchThreads; ++t) {
        threads.emplace_back(batch_worker, static_cast<uint8_t>(0x30 + t));
    }
    for (size_t t = 0; t < kSingleThreads; ++t) {
        threads.emplace_back(single_worker, static_cast<uint8_t>(0x50 + t));
    }
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(errors.load(), 0u);
    EXPECT_EQ(heap_->row_count(),
              kBatchThreads * kBatchesPerThread * kBatchSize + kSingleThreads * kSinglePerThread);

    // Heap remains usable: full scan must succeed with no leaked pins.
    auto it = heap_->begin();
    ASSERT_TRUE(it.has_value()) << it.error().message;
    size_t scanned = 0;
    for (;;) {
        auto row = it->next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        ++scanned;
    }
    EXPECT_EQ(scanned, heap_->row_count());
}
