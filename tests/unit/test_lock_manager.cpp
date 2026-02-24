#include "giodb/txn/deadlock_detector.h"
#include "giodb/txn/lock_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace giodb;

// =============================================================================
// Lock compatibility matrix tests
// =============================================================================

TEST(LockCompatibility, SharedSharedCompatible) {
    EXPECT_TRUE(locks_compatible(LockMode::S, LockMode::S));
}

TEST(LockCompatibility, SharedExclusiveIncompatible) {
    EXPECT_FALSE(locks_compatible(LockMode::S, LockMode::X));
    EXPECT_FALSE(locks_compatible(LockMode::X, LockMode::S));
}

TEST(LockCompatibility, ExclusiveExclusiveIncompatible) {
    EXPECT_FALSE(locks_compatible(LockMode::X, LockMode::X));
}

TEST(LockCompatibility, IntentSharedIntentExclusiveCompatible) {
    EXPECT_TRUE(locks_compatible(LockMode::IS, LockMode::IX));
    EXPECT_TRUE(locks_compatible(LockMode::IX, LockMode::IS));
}

TEST(LockCompatibility, IntentSharedSharedCompatible) {
    EXPECT_TRUE(locks_compatible(LockMode::IS, LockMode::S));
    EXPECT_TRUE(locks_compatible(LockMode::S, LockMode::IS));
}

TEST(LockCompatibility, IntentSharedExclusiveIncompatible) {
    EXPECT_FALSE(locks_compatible(LockMode::IS, LockMode::X));
    EXPECT_FALSE(locks_compatible(LockMode::X, LockMode::IS));
}

TEST(LockCompatibility, IntentExclusiveSharedIncompatible) {
    EXPECT_FALSE(locks_compatible(LockMode::IX, LockMode::S));
    EXPECT_FALSE(locks_compatible(LockMode::S, LockMode::IX));
}

TEST(LockCompatibility, IntentExclusiveExclusiveIncompatible) {
    EXPECT_FALSE(locks_compatible(LockMode::IX, LockMode::X));
    EXPECT_FALSE(locks_compatible(LockMode::X, LockMode::IX));
}

TEST(LockCompatibility, IntentSharedIntentSharedCompatible) {
    EXPECT_TRUE(locks_compatible(LockMode::IS, LockMode::IS));
}

TEST(LockCompatibility, IntentExclusiveIntentExclusiveCompatible) {
    EXPECT_TRUE(locks_compatible(LockMode::IX, LockMode::IX));
}

// =============================================================================
// Lock mode name tests
// =============================================================================

TEST(LockMode, NameStrings) {
    EXPECT_STREQ(lock_mode_name(LockMode::IS), "IS");
    EXPECT_STREQ(lock_mode_name(LockMode::IX), "IX");
    EXPECT_STREQ(lock_mode_name(LockMode::S), "S");
    EXPECT_STREQ(lock_mode_name(LockMode::X), "X");
}

// =============================================================================
// DeadlockDetector tests
// =============================================================================

TEST(DeadlockDetector, EmptyGraph) {
    DeadlockDetector dd;
    EXPECT_TRUE(dd.empty());
    EXPECT_EQ(dd.detect_cycle(1), invalid_txn_id);
}

TEST(DeadlockDetector, NoCycle) {
    DeadlockDetector dd;
    dd.add_edge(1, 2);
    dd.add_edge(2, 3);
    EXPECT_FALSE(dd.empty());
    EXPECT_EQ(dd.detect_cycle(1), invalid_txn_id);
}

TEST(DeadlockDetector, SimpleCycle) {
    DeadlockDetector dd;
    dd.add_edge(1, 2);
    dd.add_edge(2, 1);
    // Cycle: 1 -> 2 -> 1. Youngest is 2.
    EXPECT_EQ(dd.detect_cycle(1), 2u);
    EXPECT_EQ(dd.detect_cycle(2), 2u);
}

TEST(DeadlockDetector, ThreeNodeCycle) {
    DeadlockDetector dd;
    dd.add_edge(1, 2);
    dd.add_edge(2, 3);
    dd.add_edge(3, 1);
    // Cycle: 1 -> 2 -> 3 -> 1. Youngest is 3.
    txn_id_t victim = dd.detect_cycle(1);
    EXPECT_EQ(victim, 3u);
}

TEST(DeadlockDetector, SelfEdgeIgnored) {
    DeadlockDetector dd;
    dd.add_edge(1, 1);
    EXPECT_TRUE(dd.empty());
    EXPECT_EQ(dd.detect_cycle(1), invalid_txn_id);
}

TEST(DeadlockDetector, RemoveWaiter) {
    DeadlockDetector dd;
    dd.add_edge(1, 2);
    dd.add_edge(2, 1);
    dd.remove_waiter(1);
    // No more cycle since 1 is no longer waiting.
    EXPECT_EQ(dd.detect_cycle(2), invalid_txn_id);
}

TEST(DeadlockDetector, RemoveTransaction) {
    DeadlockDetector dd;
    dd.add_edge(1, 2);
    dd.add_edge(2, 3);
    dd.add_edge(3, 1);
    dd.remove_transaction(2);
    // Cycle broken.
    EXPECT_EQ(dd.detect_cycle(1), invalid_txn_id);
    EXPECT_EQ(dd.detect_cycle(3), invalid_txn_id);
}

// =============================================================================
// LockManager — basic lock / release tests
// =============================================================================

TEST(LockManager, SharedLocksAllowConcurrentReaders) {
    LockManager lm;
    txn_id_t txn1 = 1;
    txn_id_t txn2 = 2;

    ASSERT_TRUE(lm.lock_table(txn1, 100, LockMode::S).has_value());
    ASSERT_TRUE(lm.lock_table(txn2, 100, LockMode::S).has_value());

    lm.release_all(txn1);
    lm.release_all(txn2);
}

TEST(LockManager, ExclusiveLockBlocksOtherAccess) {
    LockManager lm(std::chrono::milliseconds(100));
    txn_id_t txn1 = 1;
    txn_id_t txn2 = 2;

    ASSERT_TRUE(lm.lock_table(txn1, 100, LockMode::X).has_value());

    // txn2 trying to get S lock should timeout since txn1 holds X.
    auto result = lm.lock_table(txn2, 100, LockMode::S);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::LOCK_TIMEOUT);

    lm.release_all(txn1);
}

TEST(LockManager, ExclusiveLockBlocksExclusive) {
    LockManager lm(std::chrono::milliseconds(100));
    txn_id_t txn1 = 1;
    txn_id_t txn2 = 2;

    ASSERT_TRUE(lm.lock_table(txn1, 100, LockMode::X).has_value());

    auto result = lm.lock_table(txn2, 100, LockMode::X);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::LOCK_TIMEOUT);

    lm.release_all(txn1);
}

TEST(LockManager, ReleaseAllGrantsWaiting) {
    LockManager lm(std::chrono::milliseconds(5000));
    txn_id_t txn1 = 1;
    txn_id_t txn2 = 2;

    ASSERT_TRUE(lm.lock_table(txn1, 100, LockMode::X).has_value());

    // Launch txn2 in a thread — it will block waiting for the lock.
    std::atomic<bool> txn2_granted{false};
    std::thread t([&] {
        auto result = lm.lock_table(txn2, 100, LockMode::S);
        if (result.has_value()) {
            txn2_granted = true;
        }
    });

    // Give txn2 time to queue its request.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Release txn1 — should grant txn2's waiting request.
    lm.release_all(txn1);
    t.join();

    EXPECT_TRUE(txn2_granted.load());
    lm.release_all(txn2);
}

TEST(LockManager, SameTransactionReacquireIdemponent) {
    LockManager lm;
    txn_id_t txn1 = 1;

    ASSERT_TRUE(lm.lock_table(txn1, 100, LockMode::S).has_value());
    // Re-acquiring same lock should succeed immediately.
    ASSERT_TRUE(lm.lock_table(txn1, 100, LockMode::S).has_value());

    lm.release_all(txn1);
}

TEST(LockManager, SameTransactionUpgradeToExclusive) {
    LockManager lm;
    txn_id_t txn1 = 1;

    ASSERT_TRUE(lm.lock_table(txn1, 100, LockMode::S).has_value());
    // Upgrade S -> X should succeed when we're the only holder.
    ASSERT_TRUE(lm.lock_table(txn1, 100, LockMode::X).has_value());

    lm.release_all(txn1);
}

// =============================================================================
// LockManager — row-level lock tests
// =============================================================================

TEST(LockManager, RowLockAcquiresIntentLock) {
    LockManager lm;
    txn_id_t txn1 = 1;

    // lock_row should automatically acquire IS/IX on the table.
    ASSERT_TRUE(lm.lock_row(txn1, 100, 1, 0, LockMode::S).has_value());
    lm.release_all(txn1);
}

TEST(LockManager, RowSharedLocksAllow_ConcurrentReaders) {
    LockManager lm;
    txn_id_t txn1 = 1;
    txn_id_t txn2 = 2;

    ASSERT_TRUE(lm.lock_row(txn1, 100, 1, 0, LockMode::S).has_value());
    ASSERT_TRUE(lm.lock_row(txn2, 100, 1, 0, LockMode::S).has_value());

    lm.release_all(txn1);
    lm.release_all(txn2);
}

TEST(LockManager, RowExclusiveLockBlocksShared) {
    LockManager lm(std::chrono::milliseconds(100));
    txn_id_t txn1 = 1;
    txn_id_t txn2 = 2;

    ASSERT_TRUE(lm.lock_row(txn1, 100, 1, 0, LockMode::X).has_value());

    auto result = lm.lock_row(txn2, 100, 1, 0, LockMode::S);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::LOCK_TIMEOUT);

    lm.release_all(txn1);
}

TEST(LockManager, DifferentRowsNoConflict) {
    LockManager lm;
    txn_id_t txn1 = 1;
    txn_id_t txn2 = 2;

    ASSERT_TRUE(lm.lock_row(txn1, 100, 1, 0, LockMode::X).has_value());
    // Different row — should succeed immediately.
    ASSERT_TRUE(lm.lock_row(txn2, 100, 1, 1, LockMode::X).has_value());

    lm.release_all(txn1);
    lm.release_all(txn2);
}

// =============================================================================
// LockManager — intent lock conflict tests
// =============================================================================

TEST(LockManager, IntentExclusiveBlocksTableShared) {
    LockManager lm(std::chrono::milliseconds(100));
    txn_id_t txn1 = 1;
    txn_id_t txn2 = 2;

    // txn1 acquires IX on the table (via row lock).
    ASSERT_TRUE(lm.lock_row(txn1, 100, 1, 0, LockMode::X).has_value());

    // txn2 trying to acquire table-level S lock should be blocked by IX.
    auto result = lm.lock_table(txn2, 100, LockMode::S);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::LOCK_TIMEOUT);

    lm.release_all(txn1);
}

TEST(LockManager, IntentSharedAllowsTableShared) {
    LockManager lm;
    txn_id_t txn1 = 1;
    txn_id_t txn2 = 2;

    // txn1 acquires IS on the table (via row read lock).
    ASSERT_TRUE(lm.lock_row(txn1, 100, 1, 0, LockMode::S).has_value());

    // txn2 should be able to get S on the table (IS + S compatible).
    ASSERT_TRUE(lm.lock_table(txn2, 100, LockMode::S).has_value());

    lm.release_all(txn1);
    lm.release_all(txn2);
}

TEST(LockManager, IntentExclusiveBlocksTableExclusive) {
    LockManager lm(std::chrono::milliseconds(100));
    txn_id_t txn1 = 1;
    txn_id_t txn2 = 2;

    // txn1 acquires IX on the table.
    ASSERT_TRUE(lm.lock_row(txn1, 100, 1, 0, LockMode::X).has_value());

    // txn2 trying to acquire table-level X lock should be blocked.
    auto result = lm.lock_table(txn2, 100, LockMode::X);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::LOCK_TIMEOUT);

    lm.release_all(txn1);
}

TEST(LockManager, IntentSharedBlocksTableExclusive) {
    LockManager lm(std::chrono::milliseconds(100));
    txn_id_t txn1 = 1;
    txn_id_t txn2 = 2;

    // txn1 acquires IS on the table.
    ASSERT_TRUE(lm.lock_row(txn1, 100, 1, 0, LockMode::S).has_value());

    // txn2 trying to acquire table-level X lock should be blocked by IS.
    auto result = lm.lock_table(txn2, 100, LockMode::X);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::LOCK_TIMEOUT);

    lm.release_all(txn1);
}

TEST(LockManager, IntentExclusiveIntentExclusiveCompatible) {
    LockManager lm;
    txn_id_t txn1 = 1;
    txn_id_t txn2 = 2;

    // Both transactions get IX on the same table (writing different rows).
    ASSERT_TRUE(lm.lock_row(txn1, 100, 1, 0, LockMode::X).has_value());
    ASSERT_TRUE(lm.lock_row(txn2, 100, 2, 0, LockMode::X).has_value());

    lm.release_all(txn1);
    lm.release_all(txn2);
}

// =============================================================================
// LockManager — lock timeout tests
// =============================================================================

TEST(LockManager, LockTimeoutReturnsError) {
    LockManager lm(std::chrono::milliseconds(50));
    txn_id_t txn1 = 1;
    txn_id_t txn2 = 2;

    ASSERT_TRUE(lm.lock_table(txn1, 100, LockMode::X).has_value());

    auto start = std::chrono::steady_clock::now();
    auto result = lm.lock_table(txn2, 100, LockMode::X);
    auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::LOCK_TIMEOUT);
    // Should have waited at least the timeout duration.
    EXPECT_GE(elapsed, std::chrono::milliseconds(40));

    lm.release_all(txn1);
}

TEST(LockManager, SetLockTimeout) {
    LockManager lm;
    EXPECT_EQ(lm.lock_timeout(), LockManager::default_lock_timeout);

    lm.set_lock_timeout(std::chrono::milliseconds(500));
    EXPECT_EQ(lm.lock_timeout(), std::chrono::milliseconds(500));
}

// =============================================================================
// LockManager — deadlock detection tests
// =============================================================================

TEST(LockManager, DeadlockDetectedAndYoungestAborted) {
    LockManager lm(std::chrono::milliseconds(5000));
    txn_id_t txn1 = 1;
    txn_id_t txn2 = 2;

    // txn1 holds X lock on table 100.
    ASSERT_TRUE(lm.lock_table(txn1, 100, LockMode::X).has_value());
    // txn2 holds X lock on table 200.
    ASSERT_TRUE(lm.lock_table(txn2, 200, LockMode::X).has_value());

    // txn2 tries to lock table 100 — blocked by txn1.
    // txn1 tries to lock table 200 — blocked by txn2.
    // This creates a deadlock cycle: txn1 -> txn2 -> txn1.
    // The youngest (txn2) should be aborted.

    std::atomic<bool> txn2_deadlocked{false};

    std::thread t2([&] {
        auto result = lm.lock_table(txn2, 100, LockMode::X);
        if (!result.has_value() && result.error().code == StatusCode::DEADLOCK) {
            txn2_deadlocked = true;
            // On deadlock, the application releases all locks (simulating ROLLBACK).
            lm.release_all(txn2);
        }
    });

    // Give txn2 time to queue and wait.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // txn1 requests table 200 — this should detect the deadlock.
    // After txn2 is aborted and releases its locks, txn1 should succeed.
    auto result = lm.lock_table(txn1, 200, LockMode::X);

    t2.join();

    // txn2 should have been deadlocked (youngest transaction).
    EXPECT_TRUE(txn2_deadlocked.load());
    // txn1 should have succeeded after txn2 released its locks.
    EXPECT_TRUE(result.has_value());

    lm.release_all(txn1);
}

// =============================================================================
// LockManager — concurrent lock contention tests
// =============================================================================

TEST(LockManager, ConcurrentReadersNoConflict) {
    LockManager lm;
    constexpr int num_readers = 8;
    std::atomic<int> success_count{0};

    std::vector<std::thread> threads;
    threads.reserve(num_readers);
    for (int i = 0; i < num_readers; ++i) {
        threads.emplace_back([&, i] {
            txn_id_t txn_id = static_cast<txn_id_t>(i + 1);
            if (lm.lock_row(txn_id, 100, 1, 0, LockMode::S).has_value()) {
                success_count++;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(success_count.load(), num_readers);

    for (int i = 0; i < num_readers; ++i) {
        lm.release_all(static_cast<txn_id_t>(i + 1));
    }
}

TEST(LockManager, ConcurrentWritersSerialized) {
    LockManager lm(std::chrono::milliseconds(2000));
    constexpr int num_writers = 4;
    std::atomic<int> active_writers{0};
    std::atomic<int> max_concurrent{0};
    std::atomic<int> success_count{0};

    std::vector<std::thread> threads;
    threads.reserve(num_writers);
    for (int i = 0; i < num_writers; ++i) {
        threads.emplace_back([&, i] {
            txn_id_t txn_id = static_cast<txn_id_t>(i + 1);
            if (lm.lock_row(txn_id, 100, 1, 0, LockMode::X).has_value()) {
                int current = ++active_writers;
                int expected = max_concurrent.load();
                while (current > expected &&
                       !max_concurrent.compare_exchange_weak(expected, current)) {}
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                --active_writers;
                success_count++;
                lm.release_all(txn_id);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // Exclusive locks mean at most 1 writer at a time.
    EXPECT_LE(max_concurrent.load(), 1);
    EXPECT_EQ(success_count.load(), num_writers);
}

// =============================================================================
// LockManager — release all locks on commit/rollback
// =============================================================================

TEST(LockManager, ReleaseAllFreesResources) {
    LockManager lm;
    txn_id_t txn1 = 1;
    txn_id_t txn2 = 2;

    ASSERT_TRUE(lm.lock_table(txn1, 100, LockMode::X).has_value());
    ASSERT_TRUE(lm.lock_row(txn1, 200, 1, 0, LockMode::S).has_value());

    // Release all txn1 locks.
    lm.release_all(txn1);

    // txn2 should now be able to lock both resources.
    ASSERT_TRUE(lm.lock_table(txn2, 100, LockMode::X).has_value());
    ASSERT_TRUE(lm.lock_row(txn2, 200, 1, 0, LockMode::X).has_value());

    lm.release_all(txn2);
}

// =============================================================================
// Lock key equality tests
// =============================================================================

TEST(LockKey, TableLockKeyEquality) {
    TableLockKey a{100};
    TableLockKey b{100};
    TableLockKey c{200};

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(LockKey, RowLockKeyEquality) {
    RowLockKey a{100, 1, 0};
    RowLockKey b{100, 1, 0};
    RowLockKey c{100, 1, 1};
    RowLockKey d{100, 2, 0};
    RowLockKey e{200, 1, 0};

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a, d);
    EXPECT_NE(a, e);
}
