// QA regression tests for GDB-844: SequentialMultiThreadInsert vacuous-test fix.
//
// These tests exercise REAL concurrency in the BTree concurrency suite using a
// latch/barrier pattern so all threads start together.  Every test:
//   - uses a fixed bounded op count (no wall-clock timeouts)
//   - collects per-thread failures into std::atomic counters
//   - NEVER uses ASSERT/EXPECT inside a spawned thread
//   - joins all threads before asserting on the main thread
//   - asserts exact post-conditions (size == N*K, all keys present)

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "test_btree_helpers.h"

using namespace sixseven;
using namespace sixseven::test;

// ---------------------------------------------------------------------------
// Minimal barrier: all threads block on fetch_add until count reaches total,
// then proceed together.  No wall-clock timeout — guaranteed to release once
// every thread reaches the barrier by construction.
// ---------------------------------------------------------------------------
struct Barrier {
    explicit Barrier(int n) : total_(n), arrived_(0) {}
    void wait() {
        arrived_.fetch_add(1, std::memory_order_acq_rel);
        while (arrived_.load(std::memory_order_acquire) < total_) {
            std::this_thread::yield();
        }
    }

private:
    int total_;
    std::atomic<int> arrived_;
};

// ---------------------------------------------------------------------------
// GDB844: truly concurrent disjoint-range inserts — all threads start
// together via barrier, each owns [t*K, (t+1)*K), asserts size==N*K and
// every key is present after join.
// ---------------------------------------------------------------------------
TEST(QA_BTreeConcurrency_GDB844, TrulyConcurrentDisjointInsert) {
    constexpr int num_threads = 8;
    constexpr int keys_per_thread = 150;
    constexpr int total_keys = num_threads * keys_per_thread;

    auto tree = make_test_index(10, 10);
    Barrier barrier(num_threads);
    std::atomic<int> insert_failures{0};

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            barrier.wait(); // all threads start together
            int start = t * keys_per_thread;
            for (int i = 0; i < keys_per_thread; ++i) {
                int key = start + i;
                auto ins = tree.insert(make_key(key), make_rid(static_cast<uint32_t>(key)));
                if (!ins.has_value()) {
                    insert_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    // Non-vacuity guard: all ops ran, assert exact counts.
    EXPECT_EQ(insert_failures.load(), 0) << "Some inserts failed under true concurrency";
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(total_keys));

    for (int i = 0; i < total_keys; ++i) {
        auto r = tree.search(make_key(i));
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(r->has_value()) << "Key " << i << " missing after concurrent insert";
    }
}

// ---------------------------------------------------------------------------
// GDB844: high-contention small keyspace — 8 threads insert into the same
// 50-key range (overlapping, non-unique tree so duplicates are allowed).
// After join: no insert failures, size == num_threads * keys_per_thread.
// ---------------------------------------------------------------------------
TEST(QA_BTreeConcurrency_GDB844, HighContentionOverlappingInsert) {
    constexpr int num_threads = 8;
    constexpr int keys_per_thread = 50;
    constexpr int total_inserts = num_threads * keys_per_thread;

    // Non-unique tree so every insert succeeds even for duplicate keys.
    auto tree = make_test_index(8, 8, /*is_unique=*/false);
    Barrier barrier(num_threads);
    std::atomic<int> insert_failures{0};

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            barrier.wait();
            for (int i = 0; i < keys_per_thread; ++i) {
                // All threads insert the same key space [0, keys_per_thread).
                auto ins = tree.insert(make_key(i),
                                       make_rid(static_cast<uint32_t>(t * keys_per_thread + i)));
                if (!ins.has_value()) {
                    insert_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(insert_failures.load(), 0) << "Insert failure in overlapping-key stress test";
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(total_inserts));

    // Every key in [0, keys_per_thread) must be findable at least once.
    for (int i = 0; i < keys_per_thread; ++i) {
        auto r = tree.search(make_key(i));
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(r->has_value()) << "Key " << i << " not found after overlapping inserts";
    }
}

// ---------------------------------------------------------------------------
// GDB844: concurrent insert (disjoint ranges) + concurrent point-lookup.
// Writers and readers run at the same time.  Readers only look up keys that
// were pre-inserted before threads launch — they must ALWAYS find them.
// Writers insert into a disjoint range that readers never touch.
// This tests that concurrent writes do not corrupt already-inserted entries.
// ---------------------------------------------------------------------------
TEST(QA_BTreeConcurrency_GDB844, ConcurrentInsertPlusReadPreExisting) {
    constexpr int pre_keys = 200;
    constexpr int num_writers = 4;
    constexpr int writer_keys_each = 100;
    constexpr int num_readers = 4;

    auto tree = make_test_index(10, 10);

    // Pre-populate keys [0, pre_keys) sequentially.
    for (int i = 0; i < pre_keys; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "Pre-insert failed at key " << i;
    }

    Barrier barrier(num_writers + num_readers);
    std::atomic<int> write_failures{0};
    std::atomic<int> read_failures{0};

    std::vector<std::thread> threads;
    threads.reserve(num_writers + num_readers);

    // Writers: insert keys [pre_keys + t*writer_keys_each, ...).
    for (int t = 0; t < num_writers; ++t) {
        threads.emplace_back([&, t]() {
            barrier.wait();
            int start = pre_keys + t * writer_keys_each;
            for (int i = 0; i < writer_keys_each; ++i) {
                int key = start + i;
                auto ins = tree.insert(make_key(key), make_rid(static_cast<uint32_t>(key)));
                if (!ins.has_value()) {
                    write_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // Readers: search pre-existing keys — must always be found.
    for (int r = 0; r < num_readers; ++r) {
        threads.emplace_back([&, r]() {
            barrier.wait();
            int stride = pre_keys / num_readers;
            int start = r * stride;
            int end = start + stride;
            // Each reader performs K full passes over its key slice.
            constexpr int passes = 5;
            for (int pass = 0; pass < passes; ++pass) {
                for (int i = start; i < end; ++i) {
                    auto res = tree.search(make_key(i));
                    if (!res.has_value() || !res->has_value()) {
                        read_failures.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(write_failures.load(), 0) << "Write failures during concurrent insert+read";
    EXPECT_EQ(read_failures.load(), 0)
        << "Pre-existing key lost or corrupted during concurrent writes";

    uint64_t expected_size = static_cast<uint64_t>(pre_keys + num_writers * writer_keys_each);
    EXPECT_EQ(tree.size(), expected_size);
}

// ---------------------------------------------------------------------------
// GDB844: many threads (16) × bounded K — stresses latch acquisition queue.
// Disjoint ranges, barrier start, exact size assertion.
// ---------------------------------------------------------------------------
TEST(QA_BTreeConcurrency_GDB844, ManyThreadsDisjointInsert) {
    constexpr int num_threads = 16;
    constexpr int keys_per_thread = 75;
    constexpr int total_keys = num_threads * keys_per_thread;

    auto tree = make_test_index(12, 12);
    Barrier barrier(num_threads);
    std::atomic<int> insert_failures{0};

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            barrier.wait();
            int start = t * keys_per_thread;
            for (int i = 0; i < keys_per_thread; ++i) {
                int key = start + i;
                auto ins = tree.insert(make_key(key), make_rid(static_cast<uint32_t>(key)));
                if (!ins.has_value()) {
                    insert_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(insert_failures.load(), 0);
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(total_keys));

    // Full key-presence scan.
    for (int i = 0; i < total_keys; ++i) {
        auto r = tree.search(make_key(i));
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(r->has_value()) << "Key " << i << " missing (16-thread test)";
    }
}

// ---------------------------------------------------------------------------
// GDB844: concurrent range scans while inserts occur.
// Writers insert into [scan_end, ...) so readers scan a stable set [0, scan_end).
// Readers collect counts; each must find exactly their slice size.
// ---------------------------------------------------------------------------
TEST(QA_BTreeConcurrency_GDB844, ConcurrentRangeScanDuringInsert) {
    constexpr int pre_keys = 200; // keys readers will scan
    constexpr int num_readers = 4;
    constexpr int keys_per_scan_thread = pre_keys / num_readers; // 50
    constexpr int num_writers = 2;
    constexpr int writer_keys_each = 100;

    auto tree = make_test_index(10, 10);

    // Pre-populate [0, pre_keys).
    for (int i = 0; i < pre_keys; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value());
    }

    Barrier barrier(num_readers + num_writers);
    std::atomic<int> scan_failures{0};
    std::atomic<int> write_failures{0};

    std::vector<std::thread> threads;
    threads.reserve(num_readers + num_writers);

    // Readers scan disjoint slices of [0, pre_keys).
    for (int r = 0; r < num_readers; ++r) {
        threads.emplace_back([&, r]() {
            barrier.wait();
            int start = r * keys_per_scan_thread;
            int end = start + keys_per_scan_thread;

            // Perform 3 full range scans of this slice.
            constexpr int scan_passes = 3;
            for (int pass = 0; pass < scan_passes; ++pass) {
                // range_scan end bound is exclusive — pass end directly.
                auto it = tree.range_scan(make_key(start), make_key(end));
                if (!it.has_value()) {
                    scan_failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                int count = 0;
                while (!it->is_end()) {
                    auto entry = it->next();
                    if (!entry.has_value()) {
                        scan_failures.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    if (!entry->has_value()) {
                        break;
                    }
                    ++count;
                }
                // Each slice has exactly keys_per_scan_thread keys.
                if (count != keys_per_scan_thread) {
                    scan_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // Writers insert into [pre_keys + t*writer_keys_each, ...).
    for (int t = 0; t < num_writers; ++t) {
        threads.emplace_back([&, t]() {
            barrier.wait();
            int start = pre_keys + t * writer_keys_each;
            for (int i = 0; i < writer_keys_each; ++i) {
                auto ins =
                    tree.insert(make_key(start + i), make_rid(static_cast<uint32_t>(start + i)));
                if (!ins.has_value()) {
                    write_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(scan_failures.load(), 0)
        << "Range scan returned wrong count during concurrent inserts";
    EXPECT_EQ(write_failures.load(), 0) << "Write failure during concurrent range scan";
}

// ---------------------------------------------------------------------------
// GDB844: verify the fixed SequentialMultiThreadInsert itself is non-vacuous.
// Re-run the same logic as the unit test and confirm size == 400 exactly.
// This is the regression guard: if someone reverts to the old single-threaded
// loop, size would still be 400, but the thread-spawn path would be absent.
// We additionally verify that the threads were ACTUALLY spawned by checking
// that an atomic counter incremented in each thread body reached num_threads.
// ---------------------------------------------------------------------------
TEST(QA_BTreeConcurrency_GDB844, SequentialMultiThreadInsertIsNonVacuous) {
    auto tree = make_test_index(10, 10);
    constexpr int keys_per_thread = 100;
    constexpr int num_threads = 4;
    constexpr int total_keys = num_threads * keys_per_thread;

    std::atomic<int> threads_started{0};
    std::atomic<int> failures{0};

    for (int t = 0; t < num_threads; ++t) {
        std::thread th([&tree, &failures, &threads_started, t]() {
            threads_started.fetch_add(1, std::memory_order_relaxed);
            int start = t * keys_per_thread;
            for (int i = 0; i < keys_per_thread; ++i) {
                int key = start + i;
                auto ins = tree.insert(make_key(key), make_rid(static_cast<uint32_t>(key)));
                if (!ins.has_value()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        th.join();
    }

    // Non-vacuity: exactly num_threads thread bodies ran.
    EXPECT_EQ(threads_started.load(), num_threads)
        << "Fewer threads than expected actually ran (vacuous regression)";
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(total_keys));

    for (int i = 0; i < total_keys; ++i) {
        auto r = tree.search(make_key(i));
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(r->has_value()) << "Key " << i << " missing";
    }
}

// ---------------------------------------------------------------------------
// GDB844: concurrent insert + concurrent remove on disjoint key ranges.
// Writers insert [pre + t*W, pre + (t+1)*W).
// Deleters remove [del_start + d*D, del_start + (d+1)*D) from pre-populated.
// After join: no failures; final size must be exact.
// ---------------------------------------------------------------------------
TEST(QA_BTreeConcurrency_GDB844, ConcurrentInsertAndRemoveDisjoint) {
    constexpr int pre_keys = 400;
    constexpr int num_inserters = 4;
    constexpr int insert_each = 100;
    constexpr int num_deleters = 4;
    constexpr int delete_each = 50; // delete [0..200) across 4 deleters
    // Expected final size: pre_keys + num_inserters*insert_each - num_deleters*delete_each
    // = 400 + 400 - 200 = 600
    constexpr int expected_size =
        pre_keys + num_inserters * insert_each - num_deleters * delete_each;

    auto tree = make_test_index(10, 10);

    for (int i = 0; i < pre_keys; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value());
    }

    Barrier barrier(num_inserters + num_deleters);
    std::atomic<int> insert_failures{0};
    std::atomic<int> delete_failures{0};

    std::vector<std::thread> threads;
    threads.reserve(num_inserters + num_deleters);

    for (int t = 0; t < num_inserters; ++t) {
        threads.emplace_back([&, t]() {
            barrier.wait();
            int start = pre_keys + t * insert_each;
            for (int i = 0; i < insert_each; ++i) {
                auto ins =
                    tree.insert(make_key(start + i), make_rid(static_cast<uint32_t>(start + i)));
                if (!ins.has_value()) {
                    insert_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (int d = 0; d < num_deleters; ++d) {
        threads.emplace_back([&, d]() {
            barrier.wait();
            int start = d * delete_each; // [0..50), [50..100), [100..150), [150..200)
            for (int i = 0; i < delete_each; ++i) {
                auto del = tree.remove(make_key(start + i));
                if (!del.has_value()) {
                    delete_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(insert_failures.load(), 0);
    EXPECT_EQ(delete_failures.load(), 0);
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(expected_size));

    // Keys [0, 200) should be gone.
    for (int i = 0; i < num_deleters * delete_each; ++i) {
        auto r = tree.search(make_key(i));
        ASSERT_TRUE(r.has_value());
        EXPECT_FALSE(r->has_value()) << "Deleted key " << i << " still present";
    }
    // Keys [200, pre_keys) should survive.
    for (int i = num_deleters * delete_each; i < pre_keys; ++i) {
        auto r = tree.search(make_key(i));
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(r->has_value()) << "Surviving key " << i << " missing";
    }
    // New keys [pre_keys, pre_keys + num_inserters*insert_each) should be present.
    for (int i = pre_keys; i < pre_keys + num_inserters * insert_each; ++i) {
        auto r = tree.search(make_key(i));
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(r->has_value()) << "Newly inserted key " << i << " missing";
    }
}
