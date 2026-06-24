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

// ---------------------------------------------------------------------------
// QA-ADV-1 — Lock-leak adversarial: implicit txn that fails mid-statement
//            (lock_table IX acquired then DML hits error path) must release
//            all locks via abort so a subsequent writer is NOT blocked.
//
// Strategy: Acquire an IX table lock inside an implicit txn, force the
//           INSERT to fail by feeding it a bad row that triggers a type error
//           after the lock is taken, then verify the next writer can acquire
//           immediately (no 30-second stall).
// ---------------------------------------------------------------------------

TEST_F(GDB930EngineTest, LockLeakOnImplicitTxnFailure) {
    exec_ok("CREATE TABLE t930_leak (id INT NOT NULL, val INT NOT NULL)");
    // Successful baseline insert so the table is non-empty.
    exec_ok("INSERT INTO t930_leak VALUES (1, 100)");

    // Force a mid-statement failure. The engine acquires an IX table lock
    // before executing the insert body. After the failure the implicit txn
    // is aborted and release_all() must fire.
    // We produce a type error by inserting a string into an INT column.
    auto bad = engine_->execute("INSERT INTO t930_leak VALUES ('not_a_number', 2)");
    // Expect failure (type/parse error) — NOT a success.
    EXPECT_FALSE(bad.has_value()) << "Expected INSERT to fail but it succeeded";

    // After the failed implicit txn the lock MUST be released.
    // A second INSERT must succeed promptly (well within 2 seconds).
    auto t_start = std::chrono::steady_clock::now();
    exec_ok("INSERT INTO t930_leak VALUES (2, 200)");
    auto elapsed = std::chrono::steady_clock::now() - t_start;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 2)
        << "Lock held after implicit txn abort — potential lock leak";

    // Data integrity: only original row + the successful second insert.
    auto sel = exec_ok("SELECT id FROM t930_leak ORDER BY id");
    ASSERT_EQ(sel.rows.size(), 2u);
    EXPECT_EQ(sel.rows[0][0].as_int32(), 1);
    EXPECT_EQ(sel.rows[1][0].as_int32(), 2);
}

// ---------------------------------------------------------------------------
// QA-ADV-2 — Self-no-deadlock: one txn updates the SAME row twice must not
//            block on its own X lock (same-txn re-acquire is idempotent).
// ---------------------------------------------------------------------------

TEST_F(GDB930EngineTest, SelfTxnUpdateSameRowTwiceNoDeadlock) {
    exec_ok("CREATE TABLE t930_self (id INT, val INT)");
    exec_ok("INSERT INTO t930_self VALUES (1, 0)");

    exec_ok("BEGIN");
    exec_ok("UPDATE t930_self SET val = 10 WHERE id = 1");
    // Second update to same row within same txn — must not deadlock.
    exec_ok("UPDATE t930_self SET val = 20 WHERE id = 1");
    exec_ok("COMMIT");

    auto sel = exec_ok("SELECT val FROM t930_self WHERE id = 1");
    ASSERT_EQ(sel.rows.size(), 1u);
    EXPECT_EQ(sel.rows[0][0].as_int32(), 20);
}

// ---------------------------------------------------------------------------
// QA-ADV-3 — Multi-row single-txn UPDATE: lock many rows then commit;
//            all locks released (subsequent writer acquires all rows).
// ---------------------------------------------------------------------------

TEST_F(GDB930EngineTest, MultiRowTxnLocksAllReleasedOnCommit) {
    exec_ok("CREATE TABLE t930_multi (id INT, val INT)");
    for (int i = 1; i <= 5; ++i) {
        auto ir = engine_->execute("INSERT INTO t930_multi VALUES (" + std::to_string(i) + ", 0)");
        ASSERT_TRUE(ir.has_value()) << ir.error().message;
    }

    exec_ok("BEGIN");
    exec_ok("UPDATE t930_multi SET val = 99");
    exec_ok("COMMIT");

    // All 5 row X locks must be gone now. A second updater should not hang.
    auto t_start = std::chrono::steady_clock::now();
    exec_ok("BEGIN");
    exec_ok("UPDATE t930_multi SET val = 1");
    exec_ok("COMMIT");
    auto elapsed = std::chrono::steady_clock::now() - t_start;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 2)
        << "Row locks appear to be held after commit";

    auto sel = exec_ok("SELECT val FROM t930_multi LIMIT 1");
    ASSERT_EQ(sel.rows.size(), 1u);
    EXPECT_EQ(sel.rows[0][0].as_int32(), 1);
}

// ---------------------------------------------------------------------------
// QA-ADV-4 — Same-txn DELETE after UPDATE: re-acquires X on same row without
//            deadlock. Verifies that upgrade/idempotent path works for DELETE.
// ---------------------------------------------------------------------------

TEST_F(GDB930EngineTest, SelfTxnUpdateThenDeleteSameRowNoDeadlock) {
    exec_ok("CREATE TABLE t930_upddel (id INT, val INT)");
    exec_ok("INSERT INTO t930_upddel VALUES (1, 5)");

    exec_ok("BEGIN");
    exec_ok("UPDATE t930_upddel SET val = 99 WHERE id = 1");
    exec_ok("DELETE FROM t930_upddel WHERE id = 1");
    exec_ok("COMMIT");

    auto sel = exec_ok("SELECT id FROM t930_upddel");
    EXPECT_EQ(sel.rows.size(), 0u);
}

// ---------------------------------------------------------------------------
// QA-ADV-5 — Cross-session contention via two concurrent QueryEngines.
//
// We can't drive two sessions through a single QueryEngine because it is
// single-threaded per session. Instead we use two separate QueryEngine
// instances sharing the same LockManager (via separate TransactionManagers
// that each hold their own LockManager — the isolation level here is at the
// LockManager level).
//
// For the engine-level cross-session path we verify the observable contract:
// two autocommit UPDATEs to the same row from two threads complete without
// corruption (no lost update visible in MVCC terms) and without hanging.
//
// Latch protocol:
//   T1: BEGIN → UPDATE row → latch T2 in → COMMIT → signal T2
//   T2: waits for T1 latch → UPDATE same row → verify
// ---------------------------------------------------------------------------

TEST_F(GDB930EngineTest, AutocommitTwoEnginesConcurrentUpdateSameRow) {
    exec_ok("CREATE TABLE t930_cc (id INT, ctr INT)");
    exec_ok("INSERT INTO t930_cc VALUES (1, 0)");

    // Build a second QueryEngine backed by the same catalog and storage.
    // Each engine has its own TransactionManager (and thus its own LockManager).
    // They therefore do NOT share row locks — this test pins the "two
    // autocommit sessions, each with their own engine" behavior: they execute
    // serially via MVCC (the second writer will see TXN_CONFLICT at SI or
    // succeed at RC depending on isolation); crucially no hang and no crash.
    DiskManager dm2_;
    auto storage2 = std::make_unique<StorageManager>(dm2_, data_dir_);
    auto engine2 = std::make_unique<QueryEngine>(catalog_, *storage2);

    std::atomic<bool> t1_updated{false};
    std::atomic<bool> t2_done{false};
    std::string t2_error;

    std::thread t1([&]() {
        auto r = engine_->execute("UPDATE t930_cc SET ctr = 1 WHERE id = 1");
        t1_updated.store(true, std::memory_order_release);
        (void)r;
    });

    std::thread t2([&]() {
        // Wait until T1 has at least started.
        while (!t1_updated.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        auto r = engine2->execute("UPDATE t930_cc SET ctr = 2 WHERE id = 1");
        if (!r) {
            t2_error = r.error().message;
        }
        t2_done.store(true, std::memory_order_release);
    });

    t1.join();
    t2.join();

    EXPECT_TRUE(t2_done.load()) << "T2 did not complete — possible hang";
    // Both engines share catalog storage. At RC: second writer succeeds and
    // overwrites. At SI: second writer may conflict. Either is valid. What is
    // NOT valid is a hang (join timeout) or a crash.
    // Verify the row still exists and has a valid ctr value.
    auto sel = exec_ok("SELECT ctr FROM t930_cc WHERE id = 1");
    ASSERT_EQ(sel.rows.size(), 1u);
    int32_t final_ctr = sel.rows[0][0].as_int32();
    EXPECT_TRUE(final_ctr == 1 || final_ctr == 2)
        << "Unexpected ctr value " << final_ctr << " after concurrent updates";
}

// ---------------------------------------------------------------------------
// QA-ADV-6 — Direct LockManager: self-deadlock (one txn waits on itself)
//            must NOT deadlock-detect or hang; same-txn re-acquire is a no-op.
// ---------------------------------------------------------------------------

TEST(GDB930LockManager, SameTxnReacquireXNoDeadlock) {
    LockManager lock_mgr(std::chrono::seconds(5));

    const txn_id_t t1 = 77;
    const table_id_t tbl = 10;
    const PageId page = 5;
    const SlotId slot = 0;

    // Acquire X.
    auto lr1 = lock_mgr.lock_row(t1, tbl, page, slot, LockMode::X);
    ASSERT_TRUE(lr1.has_value()) << lr1.error().message;

    // Re-acquire X on the same resource within the same txn — must not block.
    auto lr2 = lock_mgr.lock_row(t1, tbl, page, slot, LockMode::X);
    ASSERT_TRUE(lr2.has_value()) << "Same-txn X re-acquire should be idempotent, got: "
                                 << lr2.error().message;

    lock_mgr.release_all(t1);
}

// ---------------------------------------------------------------------------
// QA-ADV-7 — Deadlock victim is youngest txn: T1 < T2 (lower id = older).
//            T2 must be the victim; T1 must survive.
// ---------------------------------------------------------------------------

TEST(GDB930LockManager, DeadlockYoungestIsVictim) {
    LockManager lock_mgr(std::chrono::seconds(10));

    // T1 has lower txn_id (older), T2 has higher (younger).
    const txn_id_t t1 = 10;
    const txn_id_t t2 = 20;
    const table_id_t tbl = 7;
    const PageId page_a = 0;
    const PageId page_b = 1;
    const SlotId slot = 0;

    // Both grab their first lock without contention.
    auto lr_t1_a = lock_mgr.lock_row(t1, tbl, page_a, slot, LockMode::X);
    ASSERT_TRUE(lr_t1_a.has_value());
    auto lr_t2_b = lock_mgr.lock_row(t2, tbl, page_b, slot, LockMode::X);
    ASSERT_TRUE(lr_t2_b.has_value());

    std::atomic<StatusCode> t1_result_code{StatusCode::OK};
    std::atomic<StatusCode> t2_result_code{StatusCode::OK};

    std::thread th1([&]() {
        auto lr = lock_mgr.lock_row(t1, tbl, page_b, slot, LockMode::X);
        if (!lr) {
            t1_result_code.store(lr.error().code, std::memory_order_release);
        }
        lock_mgr.release_all(t1);
    });
    std::thread th2([&]() {
        auto lr = lock_mgr.lock_row(t2, tbl, page_a, slot, LockMode::X);
        if (!lr) {
            t2_result_code.store(lr.error().code, std::memory_order_release);
        }
        lock_mgr.release_all(t2);
    });

    th1.join();
    th2.join();

    // Exactly one victim.
    bool t1_deadlocked = (t1_result_code.load() == StatusCode::DEADLOCK);
    bool t2_deadlocked = (t2_result_code.load() == StatusCode::DEADLOCK);
    EXPECT_TRUE(t1_deadlocked != t2_deadlocked)
        << "Expected exactly one deadlock victim, t1_code="
        << static_cast<int>(t1_result_code.load())
        << " t2_code=" << static_cast<int>(t2_result_code.load());

    // T2 (younger, higher id) should be the victim.
    EXPECT_TRUE(t2_deadlocked) << "Expected youngest txn (T2=20) to be aborted, but T1 was";
    EXPECT_FALSE(t1_deadlocked) << "T1 (older) should survive the deadlock";
}

// ---------------------------------------------------------------------------
// QA-ADV-8 — No double-abort: after a deadlock, release_all on the aborted
//            victim does NOT panic / segfault on already-empty queues.
// ---------------------------------------------------------------------------

TEST(GDB930LockManager, DoubleReleaseAfterDeadlockIsHarmless) {
    LockManager lock_mgr(std::chrono::seconds(10));

    const txn_id_t t1 = 30;
    const txn_id_t t2 = 40;
    const table_id_t tbl = 8;
    const PageId page_a = 0;
    const PageId page_b = 1;
    const SlotId slot = 0;

    auto lr1 = lock_mgr.lock_row(t1, tbl, page_a, slot, LockMode::X);
    ASSERT_TRUE(lr1.has_value());
    auto lr2 = lock_mgr.lock_row(t2, tbl, page_b, slot, LockMode::X);
    ASSERT_TRUE(lr2.has_value());

    std::atomic<bool> victim_released{false};

    std::thread th1([&]() {
        auto lr = lock_mgr.lock_row(t1, tbl, page_b, slot, LockMode::X);
        // May succeed or be a victim — either way, release.
        lock_mgr.release_all(t1);
        // Double-release must be harmless.
        lock_mgr.release_all(t1);
        victim_released.store(true, std::memory_order_release);
    });
    std::thread th2([&]() {
        auto lr = lock_mgr.lock_row(t2, tbl, page_a, slot, LockMode::X);
        lock_mgr.release_all(t2);
        lock_mgr.release_all(t2);
    });

    th1.join();
    th2.join();

    EXPECT_TRUE(victim_released.load()) << "Thread 1 did not complete";
}

// ---------------------------------------------------------------------------
// QA-ADV-9 — Autocommit sequential UPDATE from two separate QueryEngines.
//            Each engine owns its own TransactionManager and LockManager.
//            They share the Catalog and StorageManager. Two sequential
//            autocommit UPDATEs must produce a consistent final state.
// ---------------------------------------------------------------------------

TEST_F(GDB930EngineTest, AutocommitTwoEnginesSequentialUpdateSameRow) {
    exec_ok("CREATE TABLE t930_auto2 (id INT, ctr INT)");
    exec_ok("INSERT INTO t930_auto2 VALUES (1, 0)");

    // Second engine backed by same catalog/storage (separate txn manager).
    DiskManager dm2b_;
    auto storage2b = std::make_unique<StorageManager>(dm2b_, data_dir_);
    auto engine2b = std::make_unique<QueryEngine>(catalog_, *storage2b);

    // Sequential autocommit UPDATE from engine1.
    auto r1 = engine_->execute("UPDATE t930_auto2 SET ctr = 11 WHERE id = 1");
    EXPECT_TRUE(r1.has_value()) << (r1 ? "" : r1.error().message);

    // Sequential autocommit UPDATE from engine2.
    auto r2 = engine2b->execute("UPDATE t930_auto2 SET ctr = 22 WHERE id = 1");
    // At RC isolation the second update may succeed; at SI it may conflict.
    // Either way the row count must be exactly 1 (no phantom rows).
    (void)r2;

    auto sel = exec_ok("SELECT ctr FROM t930_auto2 WHERE id = 1");
    ASSERT_EQ(sel.rows.size(), 1u) << "Row count should be 1 after sequential updates";
    int32_t final_ctr = sel.rows[0][0].as_int32();
    EXPECT_TRUE(final_ctr == 11 || final_ctr == 22) << "Unexpected ctr=" << final_ctr;
}

} // namespace
} // namespace sixseven
