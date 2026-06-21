/// QA adversarial tests for GDB-96: B+ tree tree-level latching (single
/// tree-wide std::shared_mutex) for concurrency.
/// Tests thread safety of concurrent reads, writes, and mixed operations.
/// NOTE: The implementation uses a COARSE tree-level read/write lock — one
/// shared_mutex guards the entire tree.  This is NOT fine-grained latch
/// crabbing (a.k.a. lock coupling), which would hold per-node latches only
/// along the traversal path.  Tests in this file are written to match the
/// coarse design; if the locking strategy is ever refined to per-node
/// crabbing, any test that assumes tree-wide exclusion must be revisited.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

#include "test_btree_helpers.h"

using namespace sixseven;
using namespace sixseven::test;

// =============================================================================
// Concurrent Insert — Overlapping Ranges (races on same keys)
// =============================================================================

TEST(QA_GDB96_Insert, ConcurrentOverlappingRanges) {
    auto tree = make_test_index(6, 6);
    constexpr int num_threads = 4;
    constexpr int n = 200;

    std::vector<std::thread> threads;
    std::atomic<int> failures{0};

    // All threads insert the SAME key range [0, n) — non-unique index.
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&tree, &failures, t]() {
            for (int i = 0; i < n; ++i) {
                auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(t * n + i)));
                if (!ins.has_value()) {
                    failures.fetch_add(1);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(num_threads * n));
}

TEST(QA_GDB96_Insert, ConcurrentSmallCapacity) {
    // Small capacity forces many splits, testing split safety under contention.
    auto tree = make_test_index(2, 2);
    constexpr int num_threads = 4;
    constexpr int keys_per_thread = 50;

    std::vector<std::thread> threads;
    std::atomic<int> failures{0};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&tree, &failures, t]() {
            int start = t * keys_per_thread;
            for (int i = 0; i < keys_per_thread; ++i) {
                auto ins =
                    tree.insert(make_key(start + i), make_rid(static_cast<uint32_t>(start + i)));
                if (!ins.has_value()) {
                    failures.fetch_add(1);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(num_threads * keys_per_thread));

    // Verify all keys present.
    for (int i = 0; i < num_threads * keys_per_thread; ++i) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value()) << "Key " << i << " not found";
    }
}

// =============================================================================
// Concurrent Delete — Multiple threads deleting disjoint ranges
// =============================================================================

TEST(QA_GDB96_Delete, ConcurrentDisjointDeletes) {
    auto tree = make_test_index(6, 6);
    constexpr int total = 400;

    for (int i = 0; i < total; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    constexpr int num_threads = 4;
    constexpr int per_thread = total / num_threads;
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&tree, &failures, t]() {
            int start = t * per_thread;
            for (int i = start; i < start + per_thread; ++i) {
                auto del = tree.remove(make_key(i));
                if (!del.has_value() || !*del) {
                    failures.fetch_add(1);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(failures.load(), 0);
    EXPECT_TRUE(tree.empty());
}

TEST(QA_GDB96_Delete, ConcurrentSmallCapacityDeletes) {
    // Small capacity forces merges/redistributions under contention.
    auto tree = make_test_index(3, 3);
    constexpr int total = 100;

    for (int i = 0; i < total; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    constexpr int num_threads = 4;
    constexpr int per_thread = total / num_threads;
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&tree, &failures, t]() {
            int start = t * per_thread;
            for (int i = start; i < start + per_thread; ++i) {
                auto del = tree.remove(make_key(i));
                if (!del.has_value() || !*del) {
                    failures.fetch_add(1);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(failures.load(), 0);
    EXPECT_TRUE(tree.empty());
}

// =============================================================================
// Concurrent Mixed Insert+Delete
// =============================================================================

TEST(QA_GDB96_Mixed, ConcurrentInsertAndDeleteSameKeys) {
    // Pre-populate, then one thread deletes while another inserts new keys.
    auto tree = make_test_index(6, 6);
    constexpr int initial = 200;

    for (int i = 0; i < initial; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    std::atomic<int> ins_fail{0};
    std::atomic<int> del_fail{0};

    // Deleter: removes [0, 200)
    std::thread deleter([&]() {
        for (int i = 0; i < initial; ++i) {
            auto del = tree.remove(make_key(i));
            if (!del.has_value()) {
                del_fail.fetch_add(1);
            }
        }
    });

    // Inserter: inserts [200, 400)
    std::thread inserter([&]() {
        for (int i = initial; i < initial * 2; ++i) {
            auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
            if (!ins.has_value()) {
                ins_fail.fetch_add(1);
            }
        }
    });

    deleter.join();
    inserter.join();

    EXPECT_EQ(ins_fail.load(), 0);
    EXPECT_EQ(del_fail.load(), 0);
    // All old keys deleted, all new keys inserted
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(initial));

    // New keys should be present
    for (int i = initial; i < initial * 2; ++i) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value()) << "Key " << i << " not found";
    }
}

TEST(QA_GDB96_Mixed, MultipleWritersMultipleReaders) {
    auto tree = make_test_index(8, 8);
    constexpr int initial = 200;

    for (int i = 0; i < initial; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    std::atomic<int> write_fail{0};
    std::atomic<int> read_fail{0};
    std::atomic<bool> done{false};

    // Two writer threads
    std::thread writer1([&]() {
        for (int i = initial; i < initial + 100; ++i) {
            auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
            if (!ins.has_value())
                write_fail.fetch_add(1);
        }
    });

    std::thread writer2([&]() {
        for (int i = initial + 100; i < initial + 200; ++i) {
            auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
            if (!ins.has_value())
                write_fail.fetch_add(1);
        }
        done.store(true);
    });

    // Two reader threads
    std::vector<std::thread> readers;
    for (int r = 0; r < 2; ++r) {
        readers.emplace_back([&, r]() {
            int start = r * 100;
            while (!done.load()) {
                for (int i = start; i < start + 100; ++i) {
                    auto s = tree.search(make_key(i));
                    if (!s.has_value() || !s->has_value()) {
                        read_fail.fetch_add(1);
                    }
                }
            }
        });
    }

    writer1.join();
    writer2.join();
    for (auto& th : readers)
        th.join();

    EXPECT_EQ(write_fail.load(), 0);
    EXPECT_EQ(read_fail.load(), 0);
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(initial + 200));
}

// =============================================================================
// Concurrent Range Scan + Write
// =============================================================================

TEST(QA_GDB96_Scan, ScanDuringInserts) {
    auto tree = make_test_index(8, 8);
    constexpr int initial = 100;

    for (int i = 0; i < initial; ++i) {
        (void)tree.insert(make_key(i * 2), make_rid(static_cast<uint32_t>(i)));
    }

    std::atomic<int> scan_fail{0};
    std::atomic<int> ins_fail{0};
    std::atomic<bool> done{false};

    // Writer: insert odd keys
    std::thread writer([&]() {
        for (int i = 0; i < initial; ++i) {
            auto ins =
                tree.insert(make_key(i * 2 + 1), make_rid(static_cast<uint32_t>(i + initial)));
            if (!ins.has_value())
                ins_fail.fetch_add(1);
        }
        done.store(true);
    });

    // Reader: repeatedly scans [0, 50)
    std::thread scanner([&]() {
        while (!done.load()) {
            auto scan = tree.range_scan(make_key(0), make_key(50));
            if (!scan.has_value()) {
                scan_fail.fetch_add(1);
                continue;
            }
            // Just exhaust the iterator to test locking.
            while (!scan->is_end()) {
                auto entry = scan->next();
                if (!entry.has_value()) {
                    scan_fail.fetch_add(1);
                    break;
                }
                if (!entry->has_value())
                    break;
            }
        }
    });

    writer.join();
    scanner.join();

    EXPECT_EQ(ins_fail.load(), 0);
    EXPECT_EQ(scan_fail.load(), 0);
}

TEST(QA_GDB96_Scan, MultipleConcurrentScans) {
    auto tree = make_test_index(10, 10);
    constexpr int n = 500;

    for (int i = 0; i < n; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    constexpr int num_scanners = 4;
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};

    for (int t = 0; t < num_scanners; ++t) {
        threads.emplace_back([&tree, &failures]() {
            auto scan = tree.range_scan(std::nullopt, std::nullopt);
            if (!scan.has_value()) {
                failures.fetch_add(1);
                return;
            }

            int count = 0;
            int prev = -1;
            while (!scan->is_end()) {
                auto entry = scan->next();
                if (!entry.has_value()) {
                    failures.fetch_add(1);
                    return;
                }
                if (!entry->has_value())
                    break;
                int64_t val = std::get<int64_t>(entry->value().first[0].data());
                if (val <= prev) {
                    failures.fetch_add(1);
                    return;
                }
                prev = static_cast<int>(val);
                ++count;
            }
            if (count != n) {
                failures.fetch_add(1);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(failures.load(), 0);
}

// =============================================================================
// Iterator lifetime / lock holding
// =============================================================================

// DESIGN PIN: This test intentionally relies on the coarse tree-level locking
// design.  An open iterator acquires and holds the shared tree latch for its
// entire lifetime, which blocks ANY writer tree-wide — even inserts targeting
// a completely different leaf.  This assertion is CORRECT under the current
// single-std::shared_mutex design.  Under fine-grained latch crabbing it would
// NOT hold (a writer on a disjoint leaf path would not be blocked), so this
// test MUST be revisited and updated if per-node / path latching is ever
// introduced.
TEST(QA_GDB96_Iterator, IteratorBlocksWrites) {
    // Verify that holding an iterator (shared lock) prevents writes
    // and that destroying it allows writes to proceed.
    auto tree = make_test_index(10, 10);
    for (int i = 0; i < 50; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    std::atomic<bool> write_started{false};
    std::atomic<bool> write_done{false};

    // Use optional to control iterator lifetime explicitly.
    std::optional<BTreeIterator> iter;
    {
        auto scan = tree.range_scan(std::nullopt, std::nullopt);
        ASSERT_TRUE(scan.has_value());
        iter.emplace(std::move(*scan));
    }

    // Start writer thread — should block because iterator holds shared lock
    std::thread writer([&]() {
        write_started.store(true);
        auto ins = tree.insert(make_key(100), make_rid(100));
        write_done.store(true);
        EXPECT_TRUE(ins.has_value());
    });

    // Give the writer a moment to start and attempt to acquire exclusive lock
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(write_started.load());
    EXPECT_FALSE(write_done.load()) << "Writer should be blocked while iterator is alive";

    // Destroy the iterator to release the shared lock
    iter.reset();

    // Writer should now complete
    writer.join();
    EXPECT_TRUE(write_done.load());
    EXPECT_EQ(tree.size(), 51u);
}

TEST(QA_GDB96_Iterator, MultipleIteratorsAllowed) {
    // Multiple concurrent shared locks (iterators) should coexist.
    auto tree = make_test_index(10, 10);
    for (int i = 0; i < 100; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    // Create multiple iterators simultaneously
    auto scan1 = tree.range_scan(make_key(0), make_key(50));
    ASSERT_TRUE(scan1.has_value());

    auto scan2 = tree.range_scan(make_key(50), make_key(100));
    ASSERT_TRUE(scan2.has_value());

    // Both should work
    int count1 = 0, count2 = 0;
    while (!scan1->is_end()) {
        auto e = scan1->next();
        ASSERT_TRUE(e.has_value());
        if (!e->has_value())
            break;
        ++count1;
    }
    while (!scan2->is_end()) {
        auto e = scan2->next();
        ASSERT_TRUE(e.has_value());
        if (!e->has_value())
            break;
        ++count2;
    }
    EXPECT_EQ(count1, 50);
    EXPECT_EQ(count2, 50);
}

// =============================================================================
// Stress: Many threads, high contention
// =============================================================================

TEST(QA_GDB96_Stress, HighContentionInsertSearchDelete) {
    auto tree = make_test_index(5, 5);
    constexpr int initial = 300;

    for (int i = 0; i < initial; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    std::atomic<int> failures{0};

    std::vector<std::thread> threads;

    // 2 inserters
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&tree, &failures, t]() {
            int start = initial + t * 100;
            for (int i = start; i < start + 100; ++i) {
                auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
                if (!ins.has_value())
                    failures.fetch_add(1);
            }
        });
    }

    // 2 searchers
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&tree, &failures, t]() {
            int start = t * 150;
            for (int i = start; i < start + 150 && i < initial; ++i) {
                auto s = tree.search(make_key(i));
                // Key might have been deleted by the time we search
                if (!s.has_value())
                    failures.fetch_add(1);
            }
        });
    }

    // 2 deleters — delete from opposite ends
    threads.emplace_back([&tree, &failures]() {
        for (int i = 0; i < 50; ++i) {
            auto del = tree.remove(make_key(i));
            if (!del.has_value())
                failures.fetch_add(1);
        }
    });
    threads.emplace_back([&tree, &failures]() {
        for (int i = 250; i < initial; ++i) {
            auto del = tree.remove(make_key(i));
            if (!del.has_value())
                failures.fetch_add(1);
        }
    });

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(failures.load(), 0);
    // initial(300) + inserted(200) - deleted(50 + 50) = 400
    EXPECT_EQ(tree.size(), 400u);
}

TEST(QA_GDB96_Stress, RapidInsertDeleteCycles) {
    auto tree = make_test_index(4, 4);
    constexpr int cycles = 5;
    constexpr int keys_per_cycle = 100;

    std::atomic<int> failures{0};

    for (int c = 0; c < cycles; ++c) {
        // Insert phase: 4 threads insert disjoint ranges
        std::vector<std::thread> inserters;
        for (int t = 0; t < 4; ++t) {
            inserters.emplace_back([&tree, &failures, c, t]() {
                int base = c * keys_per_cycle + t * 25;
                for (int i = 0; i < 25; ++i) {
                    auto ins =
                        tree.insert(make_key(base + i), make_rid(static_cast<uint32_t>(base + i)));
                    if (!ins.has_value())
                        failures.fetch_add(1);
                }
            });
        }
        for (auto& th : inserters)
            th.join();

        // Delete phase: 4 threads delete disjoint ranges
        std::vector<std::thread> deleters;
        for (int t = 0; t < 4; ++t) {
            deleters.emplace_back([&tree, &failures, c, t]() {
                int base = c * keys_per_cycle + t * 25;
                for (int i = 0; i < 25; ++i) {
                    auto del = tree.remove(make_key(base + i));
                    if (!del.has_value() || !*del)
                        failures.fetch_add(1);
                }
            });
        }
        for (auto& th : deleters)
            th.join();
    }

    EXPECT_EQ(failures.load(), 0);
    EXPECT_TRUE(tree.empty());
}

// =============================================================================
// Unique constraint under concurrency
// =============================================================================

TEST(QA_GDB96_Unique, ConcurrentUniqueInsert) {
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT64};
    cfg.leaf_max_keys = 8;
    cfg.internal_max_keys = 8;
    cfg.is_unique = true;
    BTreeIndex tree(cfg);

    constexpr int n = 100;
    constexpr int num_threads = 4;

    std::atomic<int> successes{0};
    std::atomic<int> violations{0};
    std::atomic<int> errors{0};

    // All threads try to insert the SAME keys — only one should succeed per key.
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < n; ++i) {
                auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
                if (ins.has_value()) {
                    successes.fetch_add(1);
                } else if (ins.error().code == StatusCode::CONSTRAINT_VIOLATION) {
                    violations.fetch_add(1);
                } else {
                    errors.fetch_add(1);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(errors.load(), 0);
    // Exactly one thread should succeed per key
    EXPECT_EQ(successes.load(), n);
    EXPECT_EQ(violations.load(), n * (num_threads - 1));
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(n));
}

// =============================================================================
// Bulk load — must be exclusive
// =============================================================================

TEST(QA_GDB96_BulkLoad, BulkLoadThenConcurrentSearch) {
    auto tree = make_test_index(10, 10);

    // Bulk load
    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 0; i < 500; ++i) {
        entries.emplace_back(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }
    auto bl = tree.bulk_load(entries);
    ASSERT_TRUE(bl.has_value());

    // Then concurrent searches
    constexpr int num_threads = 4;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&tree, &failures, t]() {
            int start = t * 125;
            for (int i = start; i < start + 125; ++i) {
                auto s = tree.search(make_key(i));
                if (!s.has_value() || !s->has_value()) {
                    failures.fetch_add(1);
                }
            }
        });
    }

    for (auto& th : threads)
        th.join();
    EXPECT_EQ(failures.load(), 0);
}

// =============================================================================
// Edge case: concurrent ops on empty/near-empty tree
// =============================================================================

TEST(QA_GDB96_Edge, ConcurrentInsertToEmpty) {
    auto tree = make_test_index(4, 4);

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;

    // Multiple threads race to insert the first key
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&tree, &failures, t]() {
            auto ins = tree.insert(make_key(t), make_rid(static_cast<uint32_t>(t)));
            if (!ins.has_value())
                failures.fetch_add(1);
        });
    }

    for (auto& th : threads)
        th.join();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(tree.size(), 4u);
}

TEST(QA_GDB96_Edge, ConcurrentSearchOnEmpty) {
    auto tree = make_test_index(4, 4);

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&tree, &failures, t]() {
            auto s = tree.search(make_key(t));
            if (!s.has_value()) {
                failures.fetch_add(1);
            } else if (s->has_value()) {
                // Should not find anything in empty tree
                failures.fetch_add(1);
            }
        });
    }

    for (auto& th : threads)
        th.join();
    EXPECT_EQ(failures.load(), 0);
}

TEST(QA_GDB96_Edge, ConcurrentDeleteToEmpty) {
    auto tree = make_test_index(4, 4);
    for (int i = 0; i < 4; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;

    // Each thread deletes one key
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&tree, &failures, t]() {
            auto del = tree.remove(make_key(t));
            if (!del.has_value() || !*del) {
                failures.fetch_add(1);
            }
        });
    }

    for (auto& th : threads)
        th.join();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_TRUE(tree.empty());
}
