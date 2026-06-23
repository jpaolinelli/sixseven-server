/// QA regression tests for GDB-838: last_insert_page_ data race fixed via
/// std::atomic<PageId>.
///
/// Adversarial focus:
///  1. Higher-contention stress (16/32 threads, small rows, large rows).
///  2. Mixed workload: concurrent insert + delete + scan.
///  3. Interleaved insert_batch and insert_tuple under concurrency.
///  4. Hint overflow: many threads target the same full page simultaneously.
///  5. Row count correctness under concurrent insert + delete.
///  6. adjust_row_count race: concurrent callers must not lose updates.
///
/// Note: TSan is not available on this Windows/vcpkg host.  All tests verify
/// functional correctness.  A data race that produces torn reads or lost rows
/// is observable as a count mismatch or missing tuple and will fail an ASSERT.

#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class QA_GDB838 : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                "sixseven_qa_gdb838_test.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;

        // Large pool so threads are rarely starved.
        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 512);
    }

    void TearDown() override {
        bpm_.reset();
        (void)dm_.close_file(file_id_);
        std::filesystem::remove(path_);
    }

    static std::vector<uint8_t> make_tuple(size_t len, uint8_t fill = 0xAA) {
        return std::vector<uint8_t>(len, fill);
    }

    // Encode (thread_id, row_index) as first two bytes; rest is sentinel.
    static std::vector<uint8_t> make_keyed_tuple(size_t len, int tid, int row) {
        auto t = std::vector<uint8_t>(len, 0xCD);
        t[0] = static_cast<uint8_t>(tid);
        t[1] = static_cast<uint8_t>(row);
        return t;
    }

    // Collect all RIDs returned by all workers into all_rids under rid_mutex.
    // Returns the number of insert errors.
    int run_concurrent_inserts(TableHeap& heap,
                               int n_threads,
                               int rows_per_thread,
                               size_t tuple_size,
                               std::vector<RID>& all_rids,
                               std::string* first_error_msg = nullptr) {
        std::mutex rid_mutex;
        all_rids.reserve(static_cast<size_t>(n_threads * rows_per_thread));
        std::atomic<int> errors{0};
        std::mutex err_mutex;

        auto worker = [&](int tid) {
            for (int r = 0; r < rows_per_thread; ++r) {
                auto tup = make_keyed_tuple(tuple_size, tid, r);
                auto result = heap.insert_tuple(tup);
                if (!result.has_value()) {
                    if (++errors == 1 && first_error_msg != nullptr) {
                        std::lock_guard<std::mutex> lk(err_mutex);
                        *first_error_msg = result.error().message;
                    }
                    continue;
                }
                std::lock_guard<std::mutex> lk(rid_mutex);
                all_rids.push_back(*result);
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(n_threads));
        for (int t = 0; t < n_threads; ++t) {
            threads.emplace_back(worker, t);
        }
        for (auto& th : threads) {
            th.join();
        }
        return errors.load();
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
};

// ---------------------------------------------------------------------------
// 1a. 16-thread, small-row stress (maximises hint contention on same page)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB838, HighContention_16Threads_SmallRows_GDB838) {
    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr int kThreads = 16;
    constexpr int kRows = 100;   // 1600 total
    constexpr size_t kTupleSize = 32; // many fit per page

    std::vector<RID> all_rids;
    int errors = run_concurrent_inserts(heap, kThreads, kRows, kTupleSize, all_rids);

    constexpr int kTotal = kThreads * kRows;

    ASSERT_EQ(errors, 0) << "insert errors detected";
    ASSERT_EQ(static_cast<int>(all_rids.size()), kTotal)
        << "lost inserts: expected " << kTotal << " RIDs";

    // All RIDs unique — no two threads wrote to the same slot.
    std::unordered_set<uint64_t> rid_set;
    rid_set.reserve(static_cast<size_t>(kTotal));
    for (const RID& rid : all_rids) {
        uint64_t key = (static_cast<uint64_t>(rid.page_id) << 32) | rid.slot_id;
        EXPECT_TRUE(rid_set.insert(key).second)
            << "duplicate RID page=" << rid.page_id << " slot=" << rid.slot_id;
    }

    // Full scan must equal total.
    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value()) << it_result.error().message;
    auto it = std::move(*it_result);
    int scanned = 0;
    for (;;) {
        auto r = it.next();
        ASSERT_TRUE(r.has_value());
        if (!r->has_value()) { break; }
        ++scanned;
    }
    EXPECT_EQ(scanned, kTotal) << "scan found " << scanned << " rows, expected " << kTotal;

    // row_count() agrees.
    EXPECT_EQ(static_cast<int>(heap.row_count()), kTotal);
}

// ---------------------------------------------------------------------------
// 1b. 16-thread, large-row stress (one per page — races on new_page allocation
//     and last_insert_page_ update after allocation)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB838, HighContention_16Threads_LargeRows_GDB838) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // 2000-byte tuples: ~4 per page on an 8 KiB page.
    // With 16 threads each inserting 30, we get 480 rows across ~120 pages.
    constexpr int kThreads = 16;
    constexpr int kRows = 30;
    constexpr size_t kTupleSize = 2000;
    constexpr int kTotal = kThreads * kRows;

    std::vector<RID> all_rids;
    int errors = run_concurrent_inserts(heap, kThreads, kRows, kTupleSize, all_rids);

    ASSERT_EQ(errors, 0) << "insert errors detected";
    ASSERT_EQ(static_cast<int>(all_rids.size()), kTotal)
        << "lost inserts: expected " << kTotal;

    // All RIDs unique.
    std::unordered_set<uint64_t> rid_set;
    rid_set.reserve(static_cast<size_t>(kTotal));
    for (const RID& rid : all_rids) {
        uint64_t key = (static_cast<uint64_t>(rid.page_id) << 32) | rid.slot_id;
        EXPECT_TRUE(rid_set.insert(key).second)
            << "duplicate RID page=" << rid.page_id << " slot=" << rid.slot_id;
    }

    // Every RID is readable and holds expected data.
    for (const RID& rid : all_rids) {
        auto got = heap.get_tuple(rid);
        ASSERT_TRUE(got.has_value())
            << "tuple at page=" << rid.page_id << " slot=" << rid.slot_id << " unreadable";
        ASSERT_EQ(got->size(), kTupleSize)
            << "wrong tuple size at page=" << rid.page_id << " slot=" << rid.slot_id;
    }

    EXPECT_EQ(static_cast<int>(heap.row_count()), kTotal);
}

// ---------------------------------------------------------------------------
// 1c. 32-thread repeat (shake out rarer races)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB838, HighContention_32Threads_Repeat_GDB838) {
    // Run the test body 3 times on the same heap to shake out rare timing bugs.
    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr int kThreads = 32;
    constexpr int kRows = 20;
    constexpr size_t kTupleSize = 16;
    constexpr int kPerRound = kThreads * kRows;

    int grand_total = 0;

    for (int round = 0; round < 3; ++round) {
        std::vector<RID> all_rids;
        int errors = run_concurrent_inserts(heap, kThreads, kRows, kTupleSize, all_rids);

        ASSERT_EQ(errors, 0) << "round " << round << ": insert errors";
        ASSERT_EQ(static_cast<int>(all_rids.size()), kPerRound)
            << "round " << round << ": lost inserts";

        // Unique RIDs across the round.
        std::unordered_set<uint64_t> rid_set;
        for (const RID& rid : all_rids) {
            uint64_t key = (static_cast<uint64_t>(rid.page_id) << 32) | rid.slot_id;
            EXPECT_TRUE(rid_set.insert(key).second)
                << "round " << round << ": duplicate RID page=" << rid.page_id;
        }

        grand_total += kPerRound;
    }

    // Full scan and row_count after all rounds.
    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value());
    auto it = std::move(*it_result);
    int scanned = 0;
    for (;;) {
        auto r = it.next();
        ASSERT_TRUE(r.has_value());
        if (!r->has_value()) { break; }
        ++scanned;
    }
    EXPECT_EQ(scanned, grand_total);
    EXPECT_EQ(static_cast<int>(heap.row_count()), grand_total);
}

// ---------------------------------------------------------------------------
// 2. Mixed workload: concurrent insert + delete + scan
// ---------------------------------------------------------------------------

TEST_F(QA_GDB838, MixedInsertDeleteScan_GDB838) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Seed some rows so deleter threads have something to work with.
    constexpr int kSeed = 200;
    std::vector<RID> seed_rids;
    seed_rids.reserve(kSeed);
    for (int i = 0; i < kSeed; ++i) {
        auto tup = make_tuple(64, static_cast<uint8_t>(i));
        auto r = heap.insert_tuple(tup);
        ASSERT_TRUE(r.has_value()) << r.error().message;
        seed_rids.push_back(*r);
    }
    ASSERT_EQ(static_cast<int>(heap.row_count()), kSeed);

    // Shared state for concurrent workers.
    std::mutex rid_mutex;
    std::vector<RID> new_rids;
    std::atomic<int> insert_errors{0};
    std::atomic<int> delete_errors{0};
    std::atomic<int> scan_count{0};

    constexpr int kInsertThreads = 4;
    constexpr int kDeleteThreads = 2;
    constexpr int kScanThreads = 2;
    constexpr int kInsertRows = 50; // each inserter inserts 50 rows

    // Inserter workers.
    auto inserter = [&](int tid) {
        for (int r = 0; r < kInsertRows; ++r) {
            auto tup = make_keyed_tuple(64, tid, r);
            auto result = heap.insert_tuple(tup);
            if (!result.has_value()) {
                ++insert_errors;
                continue;
            }
            std::lock_guard<std::mutex> lk(rid_mutex);
            new_rids.push_back(*result);
        }
    };

    // Deleter workers: delete every other seed row.
    auto deleter = [&](int tid) {
        for (int i = tid; i < kSeed; i += kDeleteThreads) {
            auto result = heap.delete_tuple(seed_rids[static_cast<size_t>(i)]);
            if (!result.has_value()) {
                ++delete_errors;
            }
        }
    };

    // Scanner worker: just count how many tuples the scan sees (not asserting
    // exact count because inserts/deletes race, but it must not crash and must
    // return a non-negative count).
    auto scanner = [&]() {
        auto it_result = heap.begin();
        if (!it_result.has_value()) {
            return;
        }
        auto it = std::move(*it_result);
        int local_count = 0;
        for (;;) {
            auto r = it.next();
            ASSERT_TRUE(r.has_value());
            if (!r->has_value()) { break; }
            ++local_count;
        }
        scan_count.fetch_add(local_count, std::memory_order_relaxed);
    };

    // Launch all threads concurrently.
    std::vector<std::thread> threads;
    for (int t = 0; t < kInsertThreads; ++t) {
        threads.emplace_back(inserter, t);
    }
    for (int t = 0; t < kDeleteThreads; ++t) {
        threads.emplace_back(deleter, t);
    }
    for (int t = 0; t < kScanThreads; ++t) {
        threads.emplace_back(scanner);
    }
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(insert_errors.load(), 0) << "concurrent inserters had errors";
    EXPECT_EQ(delete_errors.load(), 0) << "concurrent deleters had errors";

    // After all threads finish: new inserts + (kSeed - deleted) must still be
    // readable or NOT_FOUND (no crash, no wrong data).
    int new_inserted = static_cast<int>(new_rids.size());

    // Every new RID must be readable and have correct size.
    for (const RID& rid : new_rids) {
        auto got = heap.get_tuple(rid);
        ASSERT_TRUE(got.has_value())
            << "newly inserted tuple missing at page=" << rid.page_id
            << " slot=" << rid.slot_id;
        EXPECT_EQ(got->size(), 64u);
    }

    // row_count() must be non-negative and <= kSeed + new_inserted.
    auto rc = static_cast<int>(heap.row_count());
    EXPECT_GE(rc, 0);
    EXPECT_LE(rc, kSeed + new_inserted);
}

// ---------------------------------------------------------------------------
// 3. Interleaved insert_batch and insert_tuple under concurrency
// ---------------------------------------------------------------------------

TEST_F(QA_GDB838, ConcurrentBatchAndSingleInsert_GDB838) {
    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr int kBatchThreads = 4;
    constexpr int kSingleThreads = 4;
    constexpr int kBatchSize = 20;   // tuples per batch call
    constexpr int kSingleRows = 50;
    constexpr size_t kTupleSize = 64;

    std::mutex rid_mutex;
    std::vector<RID> all_rids;
    std::atomic<int> errors{0};

    // Batch inserter.
    auto batch_worker = [&]() {
        std::vector<std::vector<uint8_t>> owned;
        std::vector<std::span<const uint8_t>> spans;
        owned.reserve(kBatchSize);
        spans.reserve(kBatchSize);
        for (int i = 0; i < kBatchSize; ++i) {
            owned.push_back(make_tuple(kTupleSize, static_cast<uint8_t>(i)));
            spans.emplace_back(owned.back());
        }
        auto result = heap.insert_batch(spans);
        if (!result.has_value()) {
            ++errors;
            return;
        }
        std::lock_guard<std::mutex> lk(rid_mutex);
        for (const RID& rid : *result) {
            all_rids.push_back(rid);
        }
    };

    // Single inserter.
    auto single_worker = [&](int tid) {
        for (int r = 0; r < kSingleRows; ++r) {
            auto tup = make_keyed_tuple(kTupleSize, tid, r);
            auto result = heap.insert_tuple(tup);
            if (!result.has_value()) {
                ++errors;
                continue;
            }
            std::lock_guard<std::mutex> lk(rid_mutex);
            all_rids.push_back(*result);
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kBatchThreads; ++t) {
        threads.emplace_back(batch_worker);
    }
    for (int t = 0; t < kSingleThreads; ++t) {
        threads.emplace_back(single_worker, t);
    }
    for (auto& th : threads) {
        th.join();
    }

    ASSERT_EQ(errors.load(), 0) << "insert errors";

    constexpr int kExpected = kBatchThreads * kBatchSize + kSingleThreads * kSingleRows;
    ASSERT_EQ(static_cast<int>(all_rids.size()), kExpected)
        << "lost inserts: expected " << kExpected;

    // All RIDs unique.
    std::unordered_set<uint64_t> rid_set;
    rid_set.reserve(static_cast<size_t>(kExpected));
    for (const RID& rid : all_rids) {
        uint64_t key = (static_cast<uint64_t>(rid.page_id) << 32) | rid.slot_id;
        EXPECT_TRUE(rid_set.insert(key).second)
            << "duplicate RID page=" << rid.page_id << " slot=" << rid.slot_id;
    }

    // Full scan matches.
    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value());
    auto it = std::move(*it_result);
    int scanned = 0;
    for (;;) {
        auto r = it.next();
        ASSERT_TRUE(r.has_value());
        if (!r->has_value()) { break; }
        ++scanned;
    }
    EXPECT_EQ(scanned, kExpected);
    EXPECT_EQ(static_cast<int>(heap.row_count()), kExpected);
}

// ---------------------------------------------------------------------------
// 4. Hint overflow: many threads target the same nearly-full page boundary
//    Confirm overflow rows spill to a new page, none are lost, RIDs unique.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB838, HintOverflow_PageBoundaryStress_GDB838) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Fill a page to exactly leave room for 1 more tuple (so many threads
    // will race to get that last slot and the rest must overflow).
    // Page: 8192 bytes, header 24 bytes usable = 8168.
    // Tuple overhead (slot entry) = 4 bytes.
    // 2000-byte tuples: 2004 bytes each, floor(8168/2004) = 4 per page.
    // Insert 3 tuples to leave room for 1 more on the hint page.
    constexpr size_t kTupleSize = 2000;
    for (int i = 0; i < 3; ++i) {
        auto r = heap.insert_tuple(make_tuple(kTupleSize, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    ASSERT_EQ(heap.row_count(), 3u);

    // Now launch 16 threads simultaneously; only 1 can fit on the hint page.
    constexpr int kThreads = 16;
    constexpr int kTotal = kThreads; // one insert per thread

    std::mutex rid_mutex;
    std::vector<RID> all_rids;
    std::atomic<int> errors{0};

    // Use a barrier so all threads start at the same moment.
    std::barrier<> start_barrier{kThreads};

    auto worker = [&]() {
        start_barrier.arrive_and_wait();
        auto tup = make_tuple(kTupleSize, 0xBB);
        auto result = heap.insert_tuple(tup);
        if (!result.has_value()) {
            ++errors;
            return;
        }
        std::lock_guard<std::mutex> lk(rid_mutex);
        all_rids.push_back(*result);
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back(worker);
    }
    for (auto& th : threads) {
        th.join();
    }

    ASSERT_EQ(errors.load(), 0) << "insert errors at hint page boundary";
    ASSERT_EQ(static_cast<int>(all_rids.size()), kTotal) << "lost inserts at page boundary";

    // All RIDs unique — no two threads wrote to the same slot.
    std::unordered_set<uint64_t> rid_set;
    rid_set.reserve(static_cast<size_t>(kTotal));
    for (const RID& rid : all_rids) {
        uint64_t key = (static_cast<uint64_t>(rid.page_id) << 32) | rid.slot_id;
        EXPECT_TRUE(rid_set.insert(key).second)
            << "duplicate RID at page=" << rid.page_id << " slot=" << rid.slot_id;
        // slot_id must be in a valid range (not corrupted).
        EXPECT_LT(rid.slot_id, 10u) << "slot_id looks corrupt";
        // page_id must be >= 1 (no header-page corruption).
        EXPECT_GE(rid.page_id, 1u);
    }

    // Total row count: 3 seeds + kTotal new.
    EXPECT_EQ(heap.row_count(), 3u + static_cast<uint64_t>(kTotal));

    // Full scan must find 3 + kTotal tuples.
    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value());
    auto it = std::move(*it_result);
    int scanned = 0;
    for (;;) {
        auto r = it.next();
        ASSERT_TRUE(r.has_value());
        if (!r->has_value()) { break; }
        ++scanned;
    }
    EXPECT_EQ(scanned, 3 + kTotal);
}

// ---------------------------------------------------------------------------
// 5. Row count correctness under concurrent insert + delete
//    After N inserts - M deletes the counter must equal N - M.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB838, RowCountCorrect_ConcurrentInsertDelete_GDB838) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Phase 1: sequential insert of known tuples so deleters have stable RIDs.
    constexpr int kSeed = 100;
    std::vector<RID> seed_rids;
    seed_rids.reserve(kSeed);
    for (int i = 0; i < kSeed; ++i) {
        auto r = heap.insert_tuple(make_tuple(32, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value()) << r.error().message;
        seed_rids.push_back(*r);
    }
    ASSERT_EQ(heap.row_count(), static_cast<uint64_t>(kSeed));

    // Phase 2: 4 inserters + 4 deleters running concurrently.
    constexpr int kInsertThreads = 4;
    constexpr int kDeleteThreads = 4;
    constexpr int kInsertRows = 50;
    // Each deleter deletes kSeed / kDeleteThreads seed rows (non-overlapping).
    constexpr int kDeletesPerThread = kSeed / kDeleteThreads; // 25

    std::atomic<int> insert_errors{0};
    std::atomic<int> delete_errors{0};
    std::atomic<int> new_inserts{0};

    auto inserter = [&](int tid) {
        int local_ok = 0;
        for (int r = 0; r < kInsertRows; ++r) {
            auto tup = make_keyed_tuple(32, tid, r);
            auto result = heap.insert_tuple(tup);
            if (result.has_value()) {
                ++local_ok;
            } else {
                ++insert_errors;
            }
        }
        new_inserts.fetch_add(local_ok, std::memory_order_relaxed);
    };

    auto deleter = [&](int tid) {
        int start = tid * kDeletesPerThread;
        int end = start + kDeletesPerThread;
        for (int i = start; i < end; ++i) {
            auto result = heap.delete_tuple(seed_rids[static_cast<size_t>(i)]);
            if (!result.has_value()) {
                ++delete_errors;
            }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kInsertThreads; ++t) {
        threads.emplace_back(inserter, t);
    }
    for (int t = 0; t < kDeleteThreads; ++t) {
        threads.emplace_back(deleter, t);
    }
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(insert_errors.load(), 0);
    EXPECT_EQ(delete_errors.load(), 0);

    int total_deleted = kDeleteThreads * kDeletesPerThread;
    int expected_rc = kSeed - total_deleted + new_inserts.load();
    EXPECT_EQ(static_cast<int>(heap.row_count()), expected_rc)
        << "row_count mismatch: expected=" << expected_rc
        << " actual=" << heap.row_count();

    // Scan count should also equal expected_rc.
    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value());
    auto it = std::move(*it_result);
    int scanned = 0;
    for (;;) {
        auto r = it.next();
        ASSERT_TRUE(r.has_value());
        if (!r->has_value()) { break; }
        ++scanned;
    }
    EXPECT_EQ(scanned, expected_rc)
        << "scan count mismatch: expected=" << expected_rc << " actual=" << scanned;
}

// ---------------------------------------------------------------------------
// 6. adjust_row_count race: concurrent callers must not lose updates
// ---------------------------------------------------------------------------

TEST_F(QA_GDB838, AdjustRowCount_ConcurrentRace_GDB838) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Start from 1000 so subtractions stay positive.
    constexpr uint64_t kBase = 1000;
    heap.adjust_row_count(static_cast<int64_t>(kBase));
    ASSERT_EQ(heap.row_count(), kBase);

    // 8 threads each increment by 1 (delta = +1) and 8 threads each decrement
    // by 1 (delta = -1), each doing 100 adjustments.  Net delta = 0, so the
    // final row count must still equal kBase.
    constexpr int kThreads = 8;
    constexpr int kAdjustments = 100;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < kAdjustments; ++i) {
                heap.adjust_row_count(+1);
            }
        });
        threads.emplace_back([&]() {
            for (int i = 0; i < kAdjustments; ++i) {
                heap.adjust_row_count(-1);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    // If adjust_row_count has a lost-update bug (load-compute-store without
    // atomic CAS), the final value will be wrong.
    EXPECT_EQ(heap.row_count(), kBase)
        << "adjust_row_count lost updates: final=" << heap.row_count()
        << " expected=" << kBase;
}

// ---------------------------------------------------------------------------
// 7. Persistence consistency after concurrent inserts (row count on re-open)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB838, PersistenceAfterConcurrentInserts_GDB838) {
    {
        TableHeap heap(*bpm_, dm_, file_id_);

        constexpr int kThreads = 8;
        constexpr int kRows = 40;
        constexpr int kTotal = kThreads * kRows;

        std::vector<RID> all_rids;
        int errors = run_concurrent_inserts(heap, kThreads, kRows, 64, all_rids);
        ASSERT_EQ(errors, 0);
        ASSERT_EQ(static_cast<int>(all_rids.size()), kTotal);

        // heap destroyed here — dtor triggers persist_row_count indirectly via
        // buffer pool flush.
    }

    // Flush buffer pool.
    bpm_.reset();

    // Re-open.
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 512);
    TableHeap heap2(*bpm_, dm_, file_id_);

    constexpr uint64_t kExpected = 8u * 40u;
    EXPECT_EQ(heap2.row_count(), kExpected)
        << "row count not persisted correctly after concurrent inserts";
}

// ---------------------------------------------------------------------------
// 8. Concurrent insert of near-page-size tuples (one tuple per page).
//    Reproduces GDB-839: when two threads race on the hint+1 fallback path
//    while a third allocates a new page via new_page(), one thread may attempt
//    to insert into a page that another thread has just filled (only 1 tuple
//    fits), causing "not enough free space in page" errors even on freshly
//    allocated pages.
//
//    Expected: FAIL until GDB-839 is fixed.  This test is a regression guard
//    that documents the bug and will become a passing test once the fix lands.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB838, NearPageSize_ConcurrentInsertRace_GDB839Repro_GDB838) {
    // Pool large enough that eviction is not the cause of failures.
    bpm_.reset();
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 256);

    TableHeap heap(*bpm_, dm_, file_id_);

    // 8000-byte tuples fit exactly one per page (usable = 8164 bytes).
    // With multiple threads concurrently inserting, the hint+1 fallback path
    // can race with new_page(): two threads fetch the same newly-allocated page
    // and the second one gets "not enough free space."
    constexpr int kThreads = 4;
    constexpr int kRows = 20;
    constexpr size_t kTupleSize = 8000;
    constexpr int kTotal = kThreads * kRows;

    std::vector<RID> all_rids;
    std::string first_err;
    int errors = run_concurrent_inserts(heap, kThreads, kRows, kTupleSize, all_rids, &first_err);

    // This assertion documents the expected correct behaviour.
    // It currently FAILS due to GDB-839 (filed as a bug ticket).
    EXPECT_EQ(errors, 0)
        << "GDB-839: near-page-size concurrent inserts fail with: " << first_err;
    EXPECT_EQ(static_cast<int>(all_rids.size()), kTotal)
        << "GDB-839: " << (kTotal - static_cast<int>(all_rids.size()))
        << " tuples were lost";

    // Any tuples that did land should be readable and have unique RIDs.
    std::unordered_set<uint64_t> rid_set;
    for (const RID& rid : all_rids) {
        uint64_t key = (static_cast<uint64_t>(rid.page_id) << 32) | rid.slot_id;
        EXPECT_TRUE(rid_set.insert(key).second)
            << "duplicate RID page=" << rid.page_id << " slot=" << rid.slot_id;
        auto got = heap.get_tuple(rid);
        EXPECT_TRUE(got.has_value())
            << "tuple at page=" << rid.page_id << " slot=" << rid.slot_id << " missing";
        if (got.has_value()) {
            EXPECT_EQ(got->size(), kTupleSize);
        }
    }
}
