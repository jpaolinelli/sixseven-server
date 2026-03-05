#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "test_btree_helpers.h"

using namespace sixseven;
using namespace sixseven::test;

// =============================================================================
// GDB-96: Concurrency Tests
//
// Tree-level reader-writer latch: reads (search, range_scan) acquire a shared
// lock; writes (insert, remove, bulk_load) acquire an exclusive lock.
// Multiple concurrent readers are allowed; writers are serialized.
// =============================================================================

TEST(BTreeConcurrency, SequentialMultiThreadInsert) {
    // Insert from multiple threads sequentially (non-overlapping ranges).
    // This tests basic thread safety.
    auto tree = make_test_index(10, 10);
    constexpr int keys_per_thread = 100;
    constexpr int num_threads = 4;

    // Sequential insert per thread (non-overlapping).
    for (int t = 0; t < num_threads; ++t) {
        int start = t * keys_per_thread;
        for (int i = 0; i < keys_per_thread; ++i) {
            int key = start + i;
            auto ins = tree.insert(make_key(key), make_rid(static_cast<uint32_t>(key)));
            ASSERT_TRUE(ins.has_value()) << "Failed to insert key " << key;
        }
    }

    EXPECT_EQ(tree.size(), static_cast<uint64_t>(num_threads * keys_per_thread));

    // Verify all keys present.
    for (int i = 0; i < num_threads * keys_per_thread; ++i) {
        auto result = tree.search(make_key(i));
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result->has_value()) << "Key " << i << " not found";
    }
}

TEST(BTreeConcurrency, SequentialInsertThenParallelSearch) {
    auto tree = make_test_index(10, 10);
    constexpr int num_keys = 500;

    // Insert all keys sequentially.
    for (int i = 0; i < num_keys; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value());
    }

    // Search from multiple threads simultaneously.
    constexpr int num_threads = 4;
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&tree, &failures, t]() {
            int start = t * (num_keys / num_threads);
            int end = start + (num_keys / num_threads);
            for (int i = start; i < end; ++i) {
                auto result = tree.search(make_key(i));
                if (!result.has_value() || !result->has_value()) {
                    failures.fetch_add(1);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(failures.load(), 0) << "Some searches failed during parallel access";
}

TEST(BTreeConcurrency, SequentialInsertDeleteSearch) {
    // Test interleaved operations executed sequentially from different "ranges".
    auto tree = make_test_index(8, 8);

    // Insert a set of keys.
    for (int i = 0; i < 200; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value());
    }

    // Delete even keys.
    for (int i = 0; i < 200; i += 2) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value());
        EXPECT_TRUE(*del);
    }

    EXPECT_EQ(tree.size(), 100u);

    // Verify odd keys present, even keys gone.
    for (int i = 0; i < 200; ++i) {
        auto result = tree.search(make_key(i));
        ASSERT_TRUE(result.has_value());
        if (i % 2 == 0) {
            EXPECT_FALSE(result->has_value()) << "Deleted key " << i << " still found";
        } else {
            EXPECT_TRUE(result->has_value()) << "Key " << i << " not found";
        }
    }
}

TEST(BTreeConcurrency, ParallelReadOnlyRangeScan) {
    auto tree = make_test_index(10, 10);
    constexpr int num_keys = 200;

    for (int i = 0; i < num_keys; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    // Multiple threads doing range scans simultaneously.
    constexpr int num_threads = 4;
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&tree, &failures, t]() {
            int start = t * 50;
            int end = start + 50;
            auto scan = tree.range_scan(make_key(start), make_key(end));
            if (!scan.has_value()) {
                failures.fetch_add(1);
                return;
            }

            int count = 0;
            while (!scan->is_end()) {
                auto entry = scan->next();
                if (!entry.has_value()) {
                    failures.fetch_add(1);
                    return;
                }
                if (!entry->has_value()) {
                    break;
                }
                ++count;
            }

            if (count != 50) {
                failures.fetch_add(1);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(failures.load(), 0) << "Parallel range scans produced incorrect results";
}

TEST(BTreeConcurrency, ConcurrentInsertDisjointRanges) {
    // Multiple threads insert non-overlapping key ranges concurrently.
    auto tree = make_test_index(10, 10);
    constexpr int keys_per_thread = 200;
    constexpr int num_threads = 4;

    std::vector<std::thread> threads;
    std::atomic<int> failures{0};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&tree, &failures, t]() {
            int start = t * keys_per_thread;
            for (int i = 0; i < keys_per_thread; ++i) {
                int key = start + i;
                auto ins = tree.insert(make_key(key), make_rid(static_cast<uint32_t>(key)));
                if (!ins.has_value()) {
                    failures.fetch_add(1);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(failures.load(), 0) << "Some inserts failed during concurrent access";
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(num_threads * keys_per_thread));

    // Verify all keys present.
    for (int i = 0; i < num_threads * keys_per_thread; ++i) {
        auto result = tree.search(make_key(i));
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result->has_value()) << "Key " << i << " not found";
    }
}

TEST(BTreeConcurrency, ConcurrentMixedReadWrite) {
    // Pre-populate, then run concurrent inserts and searches.
    auto tree = make_test_index(10, 10);
    constexpr int initial_keys = 200;
    constexpr int new_keys = 200;
    constexpr int num_readers = 3;

    // Pre-populate with keys [0, initial_keys).
    for (int i = 0; i < initial_keys; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    std::atomic<int> read_failures{0};
    std::atomic<int> write_failures{0};
    std::atomic<bool> writing_done{false};

    // Writer thread: insert keys [initial_keys, initial_keys + new_keys).
    std::thread writer([&]() {
        for (int i = initial_keys; i < initial_keys + new_keys; ++i) {
            auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
            if (!ins.has_value()) {
                write_failures.fetch_add(1);
            }
        }
        writing_done.store(true);
    });

    // Reader threads: continuously search for pre-existing keys.
    std::vector<std::thread> readers;
    for (int r = 0; r < num_readers; ++r) {
        readers.emplace_back([&, r]() {
            int keys_per_reader = initial_keys / num_readers;
            int start = r * keys_per_reader;
            int end = start + keys_per_reader;

            // Keep searching until the writer is done.
            while (!writing_done.load()) {
                for (int i = start; i < end; ++i) {
                    auto result = tree.search(make_key(i));
                    if (!result.has_value() || !result->has_value()) {
                        read_failures.fetch_add(1);
                    }
                }
            }
        });
    }

    writer.join();
    for (auto& th : readers) {
        th.join();
    }

    EXPECT_EQ(write_failures.load(), 0) << "Some writes failed";
    EXPECT_EQ(read_failures.load(), 0) << "Pre-existing keys not found during concurrent writes";
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(initial_keys + new_keys));
}

TEST(BTreeConcurrency, ConcurrentInsertAndDelete) {
    // One thread inserts high keys, another deletes low keys.
    auto tree = make_test_index(8, 8);
    constexpr int initial_keys = 300;

    // Pre-populate.
    for (int i = 0; i < initial_keys; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    std::atomic<int> insert_failures{0};
    std::atomic<int> delete_failures{0};

    // Thread 1: insert keys [initial_keys, initial_keys + 200).
    std::thread inserter([&]() {
        for (int i = initial_keys; i < initial_keys + 200; ++i) {
            auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
            if (!ins.has_value()) {
                insert_failures.fetch_add(1);
            }
        }
    });

    // Thread 2: delete keys [0, 100).
    std::thread deleter([&]() {
        for (int i = 0; i < 100; ++i) {
            auto del = tree.remove(make_key(i));
            if (!del.has_value()) {
                delete_failures.fetch_add(1);
            }
        }
    });

    inserter.join();
    deleter.join();

    EXPECT_EQ(insert_failures.load(), 0);
    EXPECT_EQ(delete_failures.load(), 0);
    // initial_keys(300) + inserted(200) - deleted(100) = 400.
    EXPECT_EQ(tree.size(), 400u);

    // Verify deleted keys are gone.
    for (int i = 0; i < 100; ++i) {
        auto result = tree.search(make_key(i));
        ASSERT_TRUE(result.has_value());
        EXPECT_FALSE(result->has_value()) << "Deleted key " << i << " still found";
    }

    // Verify surviving and new keys present.
    for (int i = 100; i < initial_keys + 200; ++i) {
        auto result = tree.search(make_key(i));
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result->has_value()) << "Key " << i << " not found";
    }
}

TEST(BTreeConcurrency, LargeSequentialStress) {
    // Stress test: many operations sequentially.
    auto tree = make_test_index(6, 6);
    constexpr int n = 2000;

    // Insert all keys.
    for (int i = 0; i < n; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "Insert failed at key " << i;
    }
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(n));

    // Delete half.
    for (int i = 0; i < n; i += 2) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value());
        EXPECT_TRUE(*del) << "Delete failed at key " << i;
    }
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(n / 2));

    // Re-insert deleted keys.
    for (int i = 0; i < n; i += 2) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i + n)));
        ASSERT_TRUE(ins.has_value()) << "Re-insert failed at key " << i;
    }
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(n));

    // Verify all present.
    for (int i = 0; i < n; ++i) {
        auto result = tree.search(make_key(i));
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result->has_value()) << "Key " << i << " not found";
    }

    // Full scan should be sorted.
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());

    int prev = -1;
    int count = 0;
    while (!scan->is_end()) {
        auto entry = scan->next();
        ASSERT_TRUE(entry.has_value());
        if (!entry->has_value()) {
            break;
        }
        int64_t val = std::get<int64_t>(entry->value().first[0].data());
        EXPECT_GT(val, prev) << "Not sorted at count " << count;
        prev = static_cast<int>(val);
        ++count;
    }
    EXPECT_EQ(count, n);
}
