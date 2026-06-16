/// QA regression tests for GDB-1267: insert_tuple retry loop (kMaxAllocRetries=64)
/// and adjust_row_count CAS fix.
///
/// Adversarial focus (per handoff):
///  1. Near-page-size tuples at 8/16/32/64 thread counts, many rows, multiple
///     iterations — assert ZERO dropped rows, ZERO duplicate RIDs, exact count,
///     full scan == expected, every RID in range.
///  2. Retry-exhaustion boundary: 64+ threads all inserting single-tuple-per-page
///     simultaneously.  If exhaustion fires, confirm INTERNAL_ERROR (no crash,
///     no silent drop, catalog/heap consistent).
///  3. adjust_row_count under mixed positive/negative concurrency: never
///     underflows below 0, exactly equals live rows after all ops.
///  4. Pin-leak detection under retry pressure: sustained inserts in a long
///     loop; if the retry path leaked page pins the buffer pool would exhaust
///     and inserts would start failing partway through.
///  5. Interaction: concurrent insert_tuple + insert_batch + delete + scan on
///     one TableHeap; persistence/reopen after heavy concurrent insert.
///
/// Note: TSan/ASan presets are broken on this Windows/vcpkg host.  All tests
/// verify functional correctness.  A data race that produces torn reads or lost
/// rows is observable as a count mismatch or missing tuple and will fail an
/// ASSERT.

#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class QA_GDB1267 : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1267_test.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;

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

    static std::vector<uint8_t> make_keyed_tuple(size_t len, int tid, int row) {
        auto t = std::vector<uint8_t>(len, 0xCD);
        if (len >= 2) {
            t[0] = static_cast<uint8_t>(tid & 0xFF);
            t[1] = static_cast<uint8_t>(row & 0xFF);
        }
        return t;
    }

    // Returns number of errors; fills all_rids and optionally first_error_msg.
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
// Helper: assert zero errors, exact RID count, all unique, all in-range, scan
// and row_count agree with expected total.
// ---------------------------------------------------------------------------

static void verify_insert_results(TableHeap& heap,
                                  const std::vector<RID>& all_rids,
                                  int expected_total,
                                  int errors,
                                  const std::string& label) {
    ASSERT_EQ(errors, 0) << label << ": insert errors";
    ASSERT_EQ(static_cast<int>(all_rids.size()), expected_total)
        << label << ": lost " << (expected_total - static_cast<int>(all_rids.size()))
        << " inserts";

    // Unique RIDs — no two threads shared a slot.
    std::unordered_set<uint64_t> rid_set;
    rid_set.reserve(static_cast<size_t>(expected_total));
    for (const RID& rid : all_rids) {
        uint64_t key = (static_cast<uint64_t>(rid.page_id) << 32) | rid.slot_id;
        EXPECT_TRUE(rid_set.insert(key).second)
            << label << ": duplicate RID page=" << rid.page_id << " slot=" << rid.slot_id;
        // Every page_id must be >= 1 (page 0 is the header page).
        EXPECT_GE(rid.page_id, 1u) << label << ": invalid page_id";
    }

    // Full scan must equal expected_total.
    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value()) << label << ": begin() failed";
    auto it = std::move(*it_result);
    int scanned = 0;
    while (it.next().has_value()) {
        ++scanned;
    }
    EXPECT_EQ(scanned, expected_total) << label << ": scan mismatch";

    // row_count must agree.
    EXPECT_EQ(static_cast<int>(heap.row_count()), expected_total)
        << label << ": row_count mismatch";
}

// ---------------------------------------------------------------------------
// 1a. Scenario 1 — 8 threads, near-page-size (8000-byte), 40 rows each.
//     One tuple fits per page → every insert goes through new_page() path.
//     Run 3 iterations on the same heap to amplify timing variance.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1267, NearPageSize_8Threads_40Rows_3Iters_GDB1267) {
    // Pool large enough that eviction is not a factor.
    bpm_.reset();
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 1024);

    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr int kThreads = 8;
    constexpr int kRows = 40;
    constexpr size_t kTupleSize = 8000;
    constexpr int kPerIter = kThreads * kRows;

    int grand_total = 0;
    for (int iter = 0; iter < 3; ++iter) {
        std::vector<RID> all_rids;
        std::string first_err;
        int errors = run_concurrent_inserts(heap, kThreads, kRows, kTupleSize, all_rids, &first_err);
        std::string label = "iter=" + std::to_string(iter);

        ASSERT_EQ(errors, 0)
            << label << ": near-page-size 8-thread insert failed: " << first_err;
        ASSERT_EQ(static_cast<int>(all_rids.size()), kPerIter)
            << label << ": lost " << (kPerIter - static_cast<int>(all_rids.size())) << " rows";

        // Unique RIDs this iteration.
        std::unordered_set<uint64_t> rid_set;
        for (const RID& rid : all_rids) {
            uint64_t key = (static_cast<uint64_t>(rid.page_id) << 32) | rid.slot_id;
            EXPECT_TRUE(rid_set.insert(key).second)
                << label << ": duplicate RID page=" << rid.page_id;
            EXPECT_GE(rid.page_id, 1u);

            // Every inserted tuple must be readable.
            auto got = heap.get_tuple(rid);
            ASSERT_TRUE(got.has_value())
                << label << ": tuple missing page=" << rid.page_id << " slot=" << rid.slot_id;
            EXPECT_EQ(got->size(), kTupleSize) << label << ": wrong tuple size";
        }

        grand_total += kPerIter;
    }

    // After 3 iterations the scan and row_count must equal the grand total.
    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value());
    auto it = std::move(*it_result);
    int scanned = 0;
    while (it.next().has_value()) {
        ++scanned;
    }
    EXPECT_EQ(scanned, grand_total) << "grand total scan mismatch";
    EXPECT_EQ(static_cast<int>(heap.row_count()), grand_total) << "grand total row_count mismatch";
}

// ---------------------------------------------------------------------------
// 1b. Scenario 1 — 16 threads, near-page-size, 30 rows each.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1267, NearPageSize_16Threads_30Rows_GDB1267) {
    bpm_.reset();
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 1024);

    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr int kThreads = 16;
    constexpr int kRows = 30;
    constexpr size_t kTupleSize = 8000;
    constexpr int kTotal = kThreads * kRows;

    std::vector<RID> all_rids;
    std::string first_err;
    int errors = run_concurrent_inserts(heap, kThreads, kRows, kTupleSize, all_rids, &first_err);

    verify_insert_results(heap, all_rids, kTotal, errors,
                          "NearPageSize_16Threads: " + first_err);
}

// ---------------------------------------------------------------------------
// 1c. Scenario 1 — 32 threads, near-page-size, 20 rows each.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1267, NearPageSize_32Threads_20Rows_GDB1267) {
    bpm_.reset();
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 1024);

    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr int kThreads = 32;
    constexpr int kRows = 20;
    constexpr size_t kTupleSize = 8000;
    constexpr int kTotal = kThreads * kRows;

    std::vector<RID> all_rids;
    std::string first_err;
    int errors = run_concurrent_inserts(heap, kThreads, kRows, kTupleSize, all_rids, &first_err);

    verify_insert_results(heap, all_rids, kTotal, errors,
                          "NearPageSize_32Threads: " + first_err);
}

// ---------------------------------------------------------------------------
// 1d. Scenario 1 — 64 threads, near-page-size, 10 rows each.
//     This is the hardest hammer: 64 threads competing on single-tuple pages.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1267, NearPageSize_64Threads_10Rows_GDB1267) {
    bpm_.reset();
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 2048);

    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr int kThreads = 64;
    constexpr int kRows = 10;
    constexpr size_t kTupleSize = 8000;
    constexpr int kTotal = kThreads * kRows;

    std::vector<RID> all_rids;
    std::string first_err;
    int errors = run_concurrent_inserts(heap, kThreads, kRows, kTupleSize, all_rids, &first_err);

    verify_insert_results(heap, all_rids, kTotal, errors,
                          "NearPageSize_64Threads: " + first_err);
}

// ---------------------------------------------------------------------------
// 2. Retry-exhaustion boundary test.
//    Drive 64 threads all simultaneously inserting a single near-page-size
//    tuple.  Under normal operation the retry loop succeeds; if the pool is
//    intentionally constrained so pages cannot be allocated we expect
//    INTERNAL_ERROR — never a crash, never a silent drop.
//
//    We use a tiny pool (64 frames for 64 threads + some overhead) so there is
//    real pin pressure.  If exhaustion fires for any thread the returned error
//    must be INTERNAL_ERROR (not any other code) and the heap must remain
//    consistent: row_count() + scan count == successful inserts.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1267, RetryExhaustion_64Threads_TinyPool_ErrorIsInternalError_GDB1267) {
    // Reset to a tiny pool: 128 frames for 64 threads each needing 1 pin at a
    // time.  This creates real contention but should not exhaust new_page()
    // because the disk manager has no capacity limit.  The test checks that if
    // INTERNAL_ERROR ever fires, the heap is still consistent.
    bpm_.reset();
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 128);

    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr int kThreads = 64;
    constexpr size_t kTupleSize = 8000;

    std::mutex rid_mutex;
    std::vector<RID> all_rids;
    std::atomic<int> internal_errors{0};
    std::atomic<int> other_errors{0};

    // All threads start at the same moment for maximum contention.
    std::barrier<> gate{kThreads};

    auto worker = [&]() {
        gate.arrive_and_wait();
        auto tup = make_tuple(kTupleSize, 0xEE);
        auto result = heap.insert_tuple(tup);
        if (!result.has_value()) {
            // If retry limit is reached, the error MUST be INTERNAL_ERROR.
            // Any other code is a bug (e.g. silent INVALID_ARGUMENT propagation).
            if (result.error().code == StatusCode::INTERNAL_ERROR) {
                ++internal_errors;
            } else {
                ++other_errors;
            }
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

    // Non-INTERNAL_ERROR failures are bugs — the retry path must not leak
    // INVALID_ARGUMENT (space error) to the caller.
    EXPECT_EQ(other_errors.load(), 0)
        << "insert_tuple returned a non-INTERNAL_ERROR failure: check that"
        << " space errors are NOT propagated from the retry path";

    int successful = static_cast<int>(all_rids.size());

    // All successful RIDs must be unique.
    std::unordered_set<uint64_t> rid_set;
    rid_set.reserve(static_cast<size_t>(successful));
    for (const RID& rid : all_rids) {
        uint64_t key = (static_cast<uint64_t>(rid.page_id) << 32) | rid.slot_id;
        EXPECT_TRUE(rid_set.insert(key).second)
            << "duplicate RID page=" << rid.page_id << " slot=" << rid.slot_id;
        EXPECT_GE(rid.page_id, 1u);
    }

    // Heap consistency: row_count() and scan count must both equal successful.
    EXPECT_EQ(static_cast<int>(heap.row_count()), successful)
        << "row_count mismatch after exhaustion scenario";

    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value());
    auto it = std::move(*it_result);
    int scanned = 0;
    while (it.next().has_value()) {
        ++scanned;
    }
    EXPECT_EQ(scanned, successful) << "scan count mismatch after exhaustion scenario";

    // Report how many threads hit the retry cap (informational, not a failure).
    if (internal_errors.load() > 0) {
        SUCCEED() << "NOTE: " << internal_errors.load()
                  << " threads hit kMaxAllocRetries=64 under pathological load "
                     "(clean INTERNAL_ERROR, no data drop — this is acceptable)";
    }
}

// ---------------------------------------------------------------------------
// 3. adjust_row_count under concurrent mixed positive + negative deltas.
//    Concurrent inserts + deletes mixing deltas; assert row_count_ never
//    underflows below 0 and exactly equals live rows after all ops.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1267, AdjustRowCount_NeverUnderflows_ExactAfterMixed_GDB1267) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Start with a known count.
    constexpr uint64_t kBase = 500;
    heap.adjust_row_count(static_cast<int64_t>(kBase));
    ASSERT_EQ(heap.row_count(), kBase);

    // 16 threads each do 200 adjustments: alternating +1/-1.
    // 8 threads increment, 8 threads decrement — net delta 0.
    constexpr int kWorkers = 16;
    constexpr int kOps = 200;

    std::atomic<bool> underflow_detected{false};

    std::vector<std::thread> threads;
    threads.reserve(kWorkers);
    for (int t = 0; t < kWorkers; ++t) {
        int64_t delta = (t % 2 == 0) ? +1 : -1;
        threads.emplace_back([&heap, delta, &underflow_detected]() {
            for (int i = 0; i < kOps; ++i) {
                heap.adjust_row_count(delta);
                // row_count is unsigned; if it ever wraps the value will be
                // astronomically large (> 1e18).  Detect wrap-around.
                uint64_t rc = heap.row_count();
                if (rc > static_cast<uint64_t>(1e12)) {
                    underflow_detected.store(true, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_FALSE(underflow_detected.load())
        << "row_count_ wrapped/underflowed during concurrent adjust_row_count";

    // Net delta is 0 → final count must still equal kBase.
    EXPECT_EQ(heap.row_count(), kBase)
        << "adjust_row_count lost updates: final=" << heap.row_count()
        << " expected=" << kBase;
}

// ---------------------------------------------------------------------------
// 3b. adjust_row_count — aggressive subtraction that attempts to underflow.
//     All threads decrement far more than the starting value; result must
//     clamp at 0 and must never wrap.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1267, AdjustRowCount_ClampAtZero_NeverWraps_GDB1267) {
    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr uint64_t kBase = 100;
    heap.adjust_row_count(static_cast<int64_t>(kBase));
    ASSERT_EQ(heap.row_count(), kBase);

    // 32 threads each subtract 50 — far more than the 100 base.
    constexpr int kWorkers = 32;
    constexpr int kDelta = 50;

    std::vector<std::thread> threads;
    threads.reserve(kWorkers);
    for (int t = 0; t < kWorkers; ++t) {
        threads.emplace_back([&heap]() { heap.adjust_row_count(-kDelta); });
    }
    for (auto& th : threads) {
        th.join();
    }

    uint64_t rc = heap.row_count();
    // Must be 0 (clamped), never a wrap-around value.
    EXPECT_EQ(rc, 0u) << "row_count should be 0 after aggressive subtraction, got " << rc;
}

// ---------------------------------------------------------------------------
// 4. Pin-leak detection under retry pressure.
//    Run near-page-size concurrent inserts in a LONG loop (20 iterations of
//    8 threads x 20 rows).  If the retry path leaked page pins the buffer pool
//    would exhaust and later inserts would start failing.  We use a small-ish
//    pool (256 frames) to make exhaustion visible early if pins are leaked.
//
//    (Indirect leak detector — TSan/ASan presets are broken on this Windows
//    host.  Functional correctness under sustained load is the signal.)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1267, PinLeak_SustainedRetryPressure_GDB1267) {
    // 256-frame pool — tight enough to surface pin leaks quickly.
    bpm_.reset();
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 256);

    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr int kIters = 20;
    constexpr int kThreads = 8;
    constexpr int kRows = 20;
    constexpr size_t kTupleSize = 8000;
    constexpr int kPerIter = kThreads * kRows;

    int grand_total = 0;
    for (int iter = 0; iter < kIters; ++iter) {
        std::vector<RID> all_rids;
        std::string first_err;
        int errors =
            run_concurrent_inserts(heap, kThreads, kRows, kTupleSize, all_rids, &first_err);

        // If pins are leaking the pool will exhaust mid-loop and inserts will
        // start returning errors.  Check every iteration.
        ASSERT_EQ(errors, 0) << "iter=" << iter
                             << " insert failed (possible pin leak): " << first_err;
        ASSERT_EQ(static_cast<int>(all_rids.size()), kPerIter)
            << "iter=" << iter << " lost " << (kPerIter - static_cast<int>(all_rids.size()))
            << " rows";

        grand_total += kPerIter;
    }

    // Final scan must equal the grand total.
    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value());
    auto it = std::move(*it_result);
    int scanned = 0;
    while (it.next().has_value()) {
        ++scanned;
    }
    EXPECT_EQ(scanned, grand_total) << "scan total mismatch after sustained inserts";
    EXPECT_EQ(static_cast<int>(heap.row_count()), grand_total)
        << "row_count total mismatch after sustained inserts";
}

// ---------------------------------------------------------------------------
// 5a. Interaction: concurrent insert_tuple + insert_batch + delete + scan.
//     All four operations running simultaneously on one TableHeap.
//     After join: no insert errors, all successful RIDs are unique and readable,
//     row_count in valid range.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1267, Interaction_AllOps_Concurrent_GDB1267) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Seed rows for deleters to target.
    constexpr int kSeed = 100;
    constexpr size_t kTupleSize = 128;

    std::vector<RID> seed_rids;
    seed_rids.reserve(kSeed);
    for (int i = 0; i < kSeed; ++i) {
        auto tup = make_tuple(kTupleSize, static_cast<uint8_t>(i));
        auto r = heap.insert_tuple(tup);
        ASSERT_TRUE(r.has_value()) << r.error().message;
        seed_rids.push_back(*r);
    }
    ASSERT_EQ(static_cast<int>(heap.row_count()), kSeed);

    // Shared containers for new inserts.
    std::mutex rid_mutex;
    std::vector<RID> new_single_rids;
    std::vector<RID> new_batch_rids;
    std::atomic<int> insert_single_errors{0};
    std::atomic<int> insert_batch_errors{0};
    std::atomic<int> delete_errors{0};

    constexpr int kSingleThreads = 4;
    constexpr int kBatchThreads = 2;
    constexpr int kDeleteThreads = 2;
    constexpr int kScanThreads = 2;
    constexpr int kSingleRows = 30;
    constexpr int kBatchSize = 20;

    auto single_inserter = [&](int tid) {
        for (int r = 0; r < kSingleRows; ++r) {
            auto tup = make_keyed_tuple(kTupleSize, tid, r);
            auto result = heap.insert_tuple(tup);
            if (!result.has_value()) {
                ++insert_single_errors;
                continue;
            }
            std::lock_guard<std::mutex> lk(rid_mutex);
            new_single_rids.push_back(*result);
        }
    };

    auto batch_inserter = [&]() {
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
            ++insert_batch_errors;
            return;
        }
        std::lock_guard<std::mutex> lk(rid_mutex);
        for (const RID& rid : *result) {
            new_batch_rids.push_back(rid);
        }
    };

    auto deleter = [&](int tid) {
        int step = kDeleteThreads;
        for (int i = tid; i < kSeed; i += step) {
            (void)heap.delete_tuple(seed_rids[static_cast<size_t>(i)]);
        }
    };

    auto scanner = [&]() {
        auto it_result = heap.begin();
        if (!it_result.has_value()) {
            return;
        }
        auto it = std::move(*it_result);
        while (it.next().has_value()) {
            // just iterate — must not crash
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kSingleThreads; ++t) {
        threads.emplace_back(single_inserter, t);
    }
    for (int t = 0; t < kBatchThreads; ++t) {
        threads.emplace_back(batch_inserter);
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

    EXPECT_EQ(insert_single_errors.load(), 0) << "single-insert errors during mixed ops";
    EXPECT_EQ(insert_batch_errors.load(), 0) << "batch-insert errors during mixed ops";
    EXPECT_EQ(delete_errors.load(), 0) << "delete errors during mixed ops";

    // All new single RIDs must be readable.
    for (const RID& rid : new_single_rids) {
        auto got = heap.get_tuple(rid);
        ASSERT_TRUE(got.has_value())
            << "single-inserted tuple missing page=" << rid.page_id << " slot=" << rid.slot_id;
        EXPECT_EQ(got->size(), kTupleSize);
    }

    // All new batch RIDs must be readable.
    for (const RID& rid : new_batch_rids) {
        auto got = heap.get_tuple(rid);
        ASSERT_TRUE(got.has_value())
            << "batch-inserted tuple missing page=" << rid.page_id << " slot=" << rid.slot_id;
        EXPECT_EQ(got->size(), kTupleSize);
    }

    // row_count must be non-negative and at most kSeed + all new inserts.
    int new_single = static_cast<int>(new_single_rids.size());
    int new_batch = static_cast<int>(new_batch_rids.size());
    auto rc = static_cast<int>(heap.row_count());
    EXPECT_GE(rc, 0);
    EXPECT_LE(rc, kSeed + new_single + new_batch);
}

// ---------------------------------------------------------------------------
// 5b. Persistence/reopen after heavy concurrent inserts: row_count and scan
//     both remain consistent across a buffer pool flush and re-open.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1267, PersistenceReopen_HeavyConcurrentInsert_GDB1267) {
    constexpr int kThreads = 16;
    constexpr int kRows = 30;
    constexpr size_t kTupleSize = 8000; // near-page-size
    constexpr int kTotal = kThreads * kRows;

    {
        bpm_.reset();
        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 1024);
        TableHeap heap(*bpm_, dm_, file_id_);

        std::vector<RID> all_rids;
        std::string first_err;
        int errors =
            run_concurrent_inserts(heap, kThreads, kRows, kTupleSize, all_rids, &first_err);
        ASSERT_EQ(errors, 0) << "pre-persist insert errors: " << first_err;
        ASSERT_EQ(static_cast<int>(all_rids.size()), kTotal) << "lost rows before persist";
    }

    // Flush and re-open.
    bpm_.reset();
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 512);
    TableHeap heap2(*bpm_, dm_, file_id_);

    EXPECT_EQ(static_cast<int>(heap2.row_count()), kTotal)
        << "row_count not persisted correctly after heavy concurrent insert";

    auto it_result = heap2.begin();
    ASSERT_TRUE(it_result.has_value());
    auto it = std::move(*it_result);
    int scanned = 0;
    while (it.next().has_value()) {
        ++scanned;
    }
    EXPECT_EQ(scanned, kTotal) << "scan count mismatch after reopen";
}

// ---------------------------------------------------------------------------
// 6. Barrier-synchronized burst: every thread starts at exactly the same
//    moment for maximum new_page() / retry-loop contention.
//    Use 32 threads inserting near-page-size tuples.  Run 5 burst rounds.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1267, BarrierBurst_32Threads_NearPageSize_5Rounds_GDB1267) {
    bpm_.reset();
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 1024);

    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr int kThreads = 32;
    constexpr int kRounds = 5;
    constexpr size_t kTupleSize = 8000;

    int grand_total = 0;

    for (int round = 0; round < kRounds; ++round) {
        std::barrier<> gate{kThreads};
        std::mutex rid_mutex;
        std::vector<RID> round_rids;
        std::atomic<int> errors{0};
        std::string first_err;
        std::mutex err_mutex;

        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                gate.arrive_and_wait();
                auto tup = make_keyed_tuple(kTupleSize, t, round);
                auto result = heap.insert_tuple(tup);
                if (!result.has_value()) {
                    if (++errors == 1) {
                        std::lock_guard<std::mutex> lk(err_mutex);
                        first_err = result.error().message;
                    }
                    return;
                }
                std::lock_guard<std::mutex> lk(rid_mutex);
                round_rids.push_back(*result);
            });
        }
        for (auto& th : threads) {
            th.join();
        }

        ASSERT_EQ(errors.load(), 0) << "round=" << round << " burst insert failed: " << first_err;
        ASSERT_EQ(static_cast<int>(round_rids.size()), kThreads)
            << "round=" << round << " lost " << (kThreads - static_cast<int>(round_rids.size()))
            << " rows";

        // Unique RIDs.
        std::unordered_set<uint64_t> rid_set;
        for (const RID& rid : round_rids) {
            uint64_t key = (static_cast<uint64_t>(rid.page_id) << 32) | rid.slot_id;
            EXPECT_TRUE(rid_set.insert(key).second)
                << "round=" << round << " duplicate RID page=" << rid.page_id;
        }

        grand_total += kThreads;
    }

    // Grand total check.
    EXPECT_EQ(static_cast<int>(heap.row_count()), grand_total);

    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value());
    auto it = std::move(*it_result);
    int scanned = 0;
    while (it.next().has_value()) {
        ++scanned;
    }
    EXPECT_EQ(scanned, grand_total);
}
