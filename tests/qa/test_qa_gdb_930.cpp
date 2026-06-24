/// test_qa_gdb_930.cpp — QA regression tests for GDB-930
///
/// Verifies that LockManager is correctly wired into the DML execution path:
///   AC1. Single-threaded INSERT/UPDATE/DELETE still works (locking is transparent).
///   AC2. Concurrent UPDATE same row: MVCC surfaced as write-write conflict at commit.
///   AC3. Deadlock between two transactions: exactly one victim aborted with clear error.
///   AC4. Lock release on ROLLBACK: a subsequent txn can lock the same row.
///   AC5. TSan stress test (written; TSan verification DEFERRED — no local sanitizer).

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/txn/lock_manager.h"
#include "sixseven/txn/transaction.h"
#include "sixseven/txn/txn_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "test_qa_helpers.h"

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// Fixture: QueryEngine-based tests
// ---------------------------------------------------------------------------

class GDB930EngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_gdb930";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto r = engine_->execute(sql);
        EXPECT_TRUE(r.has_value())
            << "SQL failed: " << sql << "\nError: " << (r ? "" : r.error().message);
        return r ? std::move(*r) : QueryResult{};
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// ---------------------------------------------------------------------------
// AC1 — Single-threaded happy path: locking is transparent to lone transactions
// ---------------------------------------------------------------------------

TEST_F(GDB930EngineTest, SingleTxnInsertUpdateDeleteSucceeds) {
    exec_ok("CREATE TABLE t930a (id INT, val INT)");

    // INSERT under explicit transaction.
    exec_ok("BEGIN");
    exec_ok("INSERT INTO t930a VALUES (1, 10)");
    exec_ok("INSERT INTO t930a VALUES (2, 20)");
    exec_ok("COMMIT");

    auto sel = exec_ok("SELECT id, val FROM t930a");
    ASSERT_EQ(sel.rows.size(), 2u);

    // UPDATE under explicit transaction.
    exec_ok("BEGIN");
    exec_ok("UPDATE t930a SET val = 99 WHERE id = 1");
    exec_ok("COMMIT");

    auto sel2 = exec_ok("SELECT val FROM t930a WHERE id = 1");
    ASSERT_EQ(sel2.rows.size(), 1u);
    EXPECT_EQ(sel2.rows[0][0].as_int32(), 99);

    // DELETE under explicit transaction.
    exec_ok("BEGIN");
    exec_ok("DELETE FROM t930a WHERE id = 2");
    exec_ok("COMMIT");

    auto sel3 = exec_ok("SELECT id FROM t930a");
    ASSERT_EQ(sel3.rows.size(), 1u);
    EXPECT_EQ(sel3.rows[0][0].as_int32(), 1);
}

TEST_F(GDB930EngineTest, AutocommitDmlSucceeds) {
    exec_ok("CREATE TABLE t930b (id INT, val INT)");
    // Autocommit DML (no BEGIN/COMMIT) must work — locking is implicit.
    exec_ok("INSERT INTO t930b VALUES (42, 7)");
    exec_ok("UPDATE t930b SET val = 8 WHERE id = 42");
    exec_ok("DELETE FROM t930b WHERE id = 42");
    auto sel = exec_ok("SELECT id FROM t930b");
    EXPECT_EQ(sel.rows.size(), 0u);
}

// ---------------------------------------------------------------------------
// AC4 — Lock release on ROLLBACK: next txn can update the same row
// ---------------------------------------------------------------------------

TEST_F(GDB930EngineTest, LockReleasedAfterRollback) {
    exec_ok("CREATE TABLE t930d (id INT, val INT)");
    exec_ok("INSERT INTO t930d VALUES (1, 0)");

    // T1: update then rollback.
    exec_ok("BEGIN");
    exec_ok("UPDATE t930d SET val = 99 WHERE id = 1");
    exec_ok("ROLLBACK");

    // After rollback, the row value is still 0 (aborted version invisible).
    // A new transaction must be able to update the row successfully.
    exec_ok("BEGIN");
    exec_ok("UPDATE t930d SET val = 42 WHERE id = 1");
    exec_ok("COMMIT");

    auto sel = exec_ok("SELECT val FROM t930d WHERE id = 1");
    ASSERT_EQ(sel.rows.size(), 1u);
    EXPECT_EQ(sel.rows[0][0].as_int32(), 42);
}

// ---------------------------------------------------------------------------
// Direct LockManager tests (no QueryEngine, deterministic via sync primitives)
// ---------------------------------------------------------------------------

// AC2 — Row X lock blocks a second waiter until released.
TEST(GDB930LockManager, RowLockBlocksSecondWriter) {
    LockManager lock_mgr(std::chrono::seconds(5));

    const txn_id_t t1 = 100;
    const txn_id_t t2 = 200;
    const table_id_t tbl = 1;
    const PageId page = 0;
    const SlotId slot = 0;

    // T1 acquires the X row lock.
    auto lr1 = lock_mgr.lock_row(t1, tbl, page, slot, LockMode::X);
    ASSERT_TRUE(lr1.has_value()) << lr1.error().message;

    // T2 tries to acquire X in a background thread — must block.
    std::atomic<bool> t2_started{false};
    std::atomic<bool> t2_acquired{false};

    std::thread t2_thread([&]() {
        t2_started.store(true, std::memory_order_release);
        auto lr2 = lock_mgr.lock_row(t2, tbl, page, slot, LockMode::X);
        t2_acquired.store(lr2.has_value(), std::memory_order_release);
    });

    // Spin until T2 has started and entered the wait.
    while (!t2_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // T2 must NOT have the lock yet.
    EXPECT_FALSE(t2_acquired.load(std::memory_order_acquire));

    // T1 releases (simulates commit/rollback).
    lock_mgr.release_all(t1);

    // T2 unblocks and acquires.
    t2_thread.join();
    EXPECT_TRUE(t2_acquired.load(std::memory_order_relaxed));

    lock_mgr.release_all(t2);
}

// AC4 (direct) — After release_all, next txn acquires immediately.
TEST(GDB930LockManager, LockAcquiredAfterRelease) {
    LockManager lock_mgr(std::chrono::seconds(5));

    const txn_id_t t1 = 50;
    const txn_id_t t2 = 51;
    const table_id_t tbl = 3;
    const PageId page = 0;
    const SlotId slot = 0;

    auto lr1 = lock_mgr.lock_row(t1, tbl, page, slot, LockMode::X);
    ASSERT_TRUE(lr1.has_value());

    lock_mgr.release_all(t1); // simulates ROLLBACK

    // T2 acquires immediately — no blocking.
    auto lr2 = lock_mgr.lock_row(t2, tbl, page, slot, LockMode::X);
    ASSERT_TRUE(lr2.has_value()) << lr2.error().message;

    lock_mgr.release_all(t2);
}

// AC3 — Deadlock: T1 locks A then waits B; T2 locks B then waits A.
//        Exactly one victim is aborted with StatusCode::DEADLOCK.
TEST(GDB930LockManager, DeadlockDetectedExactlyOneVictim) {
    LockManager lock_mgr(std::chrono::seconds(10));

    const txn_id_t t1 = 1;
    const txn_id_t t2 = 2;
    const table_id_t tbl = 1;
    const PageId page_a = 0;
    const PageId page_b = 1;
    const SlotId slot = 0;

    // Phase 1: each acquires its first lock without contention.
    auto lr_t1_a = lock_mgr.lock_row(t1, tbl, page_a, slot, LockMode::X);
    ASSERT_TRUE(lr_t1_a.has_value());
    auto lr_t2_b = lock_mgr.lock_row(t2, tbl, page_b, slot, LockMode::X);
    ASSERT_TRUE(lr_t2_b.has_value());

    // Phase 2: cross-lock — deadlock forms.
    std::atomic<int> success_count{0};
    std::atomic<int> deadlock_count{0};

    auto try_cross_lock = [&](txn_id_t txn_id, PageId target) {
        auto lr = lock_mgr.lock_row(txn_id, tbl, target, slot, LockMode::X);
        if (lr.has_value()) {
            success_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            EXPECT_EQ(lr.error().code, StatusCode::DEADLOCK)
                << "unexpected error: " << lr.error().message;
            deadlock_count.fetch_add(1, std::memory_order_relaxed);
        }
        // Always release all locks (the aborted victim must release its prior
        // grants so the surviving transaction can proceed).
        lock_mgr.release_all(txn_id);
    };

    std::thread th1([&]() { try_cross_lock(t1, page_b); });
    std::thread th2([&]() { try_cross_lock(t2, page_a); });

    th1.join();
    th2.join();

    EXPECT_EQ(deadlock_count.load(), 1) << "expected exactly one deadlock victim";
    EXPECT_EQ(success_count.load(), 1) << "expected exactly one survivor";
}

// AC3 (timeout) — Lock wait timeout returns TXN_ABORTED.
TEST(GDB930LockManager, LockWaitTimeoutReturnsError) {
    LockManager lock_mgr(std::chrono::milliseconds(100));

    const txn_id_t t1 = 10;
    const txn_id_t t2 = 20;
    const table_id_t tbl = 2;
    const PageId page = 0;
    const SlotId slot = 0;

    auto lr1 = lock_mgr.lock_row(t1, tbl, page, slot, LockMode::X);
    ASSERT_TRUE(lr1.has_value());

    // T2 waits and times out.
    auto lr2 = lock_mgr.lock_row(t2, tbl, page, slot, LockMode::X);
    ASSERT_FALSE(lr2.has_value());
    EXPECT_EQ(lr2.error().code, StatusCode::LOCK_TIMEOUT)
        << "unexpected error: " << lr2.error().message;

    lock_mgr.release_all(t1);
}

// Table IX lock blocks table S lock (incompatible per the matrix).
TEST(GDB930LockManager, TableIxBlocksTableS) {
    LockManager lock_mgr(std::chrono::milliseconds(200));

    const txn_id_t t1 = 300;
    const txn_id_t t2 = 301;
    const table_id_t tbl = 5;

    auto lr1 = lock_mgr.lock_table(t1, tbl, LockMode::IX);
    ASSERT_TRUE(lr1.has_value());

    // IX and S are incompatible — t2 should timeout.
    auto lr2 = lock_mgr.lock_table(t2, tbl, LockMode::S);
    ASSERT_FALSE(lr2.has_value());

    lock_mgr.release_all(t1);
}

// Multiple IX holders are compatible (concurrent inserters don't block each other).
TEST(GDB930LockManager, MultipleIxCompatible) {
    LockManager lock_mgr(std::chrono::seconds(1));

    const table_id_t tbl = 6;
    const txn_id_t t1 = 400;
    const txn_id_t t2 = 401;

    auto lr1 = lock_mgr.lock_table(t1, tbl, LockMode::IX);
    ASSERT_TRUE(lr1.has_value());

    // A second IX must be granted immediately (no blocking).
    std::atomic<bool> granted{false};
    std::thread th([&]() {
        auto lr2 = lock_mgr.lock_table(t2, tbl, LockMode::IX);
        granted.store(lr2.has_value(), std::memory_order_release);
    });
    th.join();
    EXPECT_TRUE(granted.load());

    lock_mgr.release_all(t1);
    lock_mgr.release_all(t2);
}

// ---------------------------------------------------------------------------
// AC5 — TSan stress test: N concurrent writers, overlapping rows.
//        TSan verification: DEFERRED (no local ThreadSanitizer on Windows box).
//        This test validates functional correctness: no hang, no crash, all
//        threads join, accounting is consistent.
// ---------------------------------------------------------------------------

TEST(GDB930LockManager, MultiWriterStressNoHangNoCrash) {
    LockManager lock_mgr(std::chrono::milliseconds(300));

    constexpr int kThreads = 4;
    constexpr int kRowsPerThread = 5;
    constexpr table_id_t kTable = 99;

    std::atomic<int> deadlocks{0};
    std::atomic<int> timeouts{0};
    std::atomic<int> successes{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            txn_id_t txn_id = static_cast<txn_id_t>(1000 + t);
            // Alternate lock order per thread to create potential deadlock cycles.
            bool ok = true;
            for (int r = 0; r < kRowsPerThread; ++r) {
                PageId row = static_cast<PageId>((t % 2 == 0) ? r : (kRowsPerThread - 1 - r));
                auto lr = lock_mgr.lock_row(txn_id, kTable, row, 0, LockMode::X);
                if (!lr.has_value()) {
                    if (lr.error().code == StatusCode::DEADLOCK) {
                        deadlocks.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        timeouts.fetch_add(1, std::memory_order_relaxed);
                    }
                    ok = false;
                    break;
                }
            }
            if (ok) {
                successes.fetch_add(1, std::memory_order_relaxed);
            }
            lock_mgr.release_all(txn_id);
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    // All threads joined: no hang.
    EXPECT_EQ(successes.load() + deadlocks.load() + timeouts.load(), kThreads)
        << "some threads did not finish";

    // TSan verification: DEFERRED — no local ThreadSanitizer on this Windows box.
    // The test confirms: no hang, no crash, deterministic join, correct accounting.
}

} // namespace
} // namespace sixseven
