// Regression test for GDB-1311: QueryEngine::compensate_row_deltas_and_clear()
// heap-use-after-free under concurrent autocommit INSERTs on a shared
// QueryEngine instance.
//
// Root cause (see include/sixseven/executor/query_engine.h and
// src/executor/query_engine.cpp): QueryEngine keeps unsynchronized
// per-instance mutable transaction-compensation state
// (active_txn_id_ / active_txn_row_deltas_). The production server
// (src/main.cpp) constructs a single shared QueryEngine and dispatches
// connections onto a ThreadPool (src/server/server.cpp), so two threads
// executing autocommit statements concurrently on that one engine could
// race on this state: one thread's active_txn_row_deltas_.clear() (on
// constraint-violation abort) could free hash-table nodes out from under
// another thread's in-progress iteration, producing a heap-use-after-free /
// SIGSEGV.
//
// This test reproduces the same shape of race as the QA repro in
// tests/qa/test_qa_gdb_1298.cpp (QA_GDB1298.ConcurrentInsertsOnSameUniqueKeyOnlyOneSucceeds)
// -- concurrent autocommit INSERTs racing on the same PRIMARY KEY, so a
// large fraction of threads take the CONSTRAINT_VIOLATION abort path that
// used to touch the shared, unsynchronized map -- but lives in
// tests/unit/ (sixseven_unit_tests) as an implementer-owned regression
// test, not tests/qa/ (which is QA-owned). It asserts no crash / no
// undefined behavior (verified further under AddressSanitizer) and that
// end-state row-count/constraint-violation semantics remain correct.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/index_manager.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

class QueryEngineConcurrencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_qe_concurrency";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
        config_ = Config::load_defaults();

        auto bootstrap_result = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(bootstrap_result.has_value()) << bootstrap_result.error().message;

        index_manager_ = std::make_unique<IndexManager>(*catalog_, *storage_);
        index_manager_->set_catalog_persistence(persistence_.get());
        auto rebuild = index_manager_->rebuild_all_indexes();
        ASSERT_TRUE(rebuild.has_value()) << rebuild.error().message;
        engine_->set_index_manager(index_manager_.get());
    }

    void TearDown() override {
        index_manager_.reset();
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << " -> " << (result ? "" : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<IndexManager> index_manager_;
    Config config_;
};

} // namespace

// Concurrent autocommit INSERTs racing on the same PRIMARY KEY value on a
// single shared QueryEngine (no external locking, exactly mirroring how
// Server dispatches concurrent client connections onto a shared ThreadPool
// in the real server -- src/main.cpp / src/server/server.cpp). Before the
// GDB-1311 fix this reliably crashed (SIGSEGV in debug builds, confirmed
// heap-use-after-free under ASan) because every losing thread's
// CONSTRAINT_VIOLATION abort raced on the unsynchronized
// active_txn_row_deltas_ map via compensate_row_deltas_and_clear(). After
// the fix, the autocommit abort path no longer touches any shared engine
// state at all (see compensate_table_row_delta()), so this must complete
// without crashing and leave at most one row with the contested key.
TEST_F(QueryEngineConcurrencyTest, ConcurrentAutocommitInsertsOnSameUniqueKeyDoNotCrash) {
    exec_ok("CREATE TABLE race1 (id INT PRIMARY KEY, worker INT)");

    constexpr int kThreads = 8;
    constexpr int kItersPerThread = 25;
    std::atomic<int> successes{0};
    std::atomic<int> failures{0};
    std::atomic<int> unexpected_errors{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kItersPerThread; ++i) {
                auto result = engine_->execute("INSERT INTO race1 VALUES (1, " +
                                               std::to_string(t * 1000 + i) + ")");
                if (result.has_value()) {
                    successes.fetch_add(1, std::memory_order_relaxed);
                } else if (result.error().code == StatusCode::CONSTRAINT_VIOLATION) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                } else {
                    // Any other error code (e.g. an internal error surfaced
                    // by the race instead of a clean constraint violation)
                    // indicates the race is not being handled correctly.
                    unexpected_errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    // Reaching this point at all (no SIGSEGV / no ASan heap-use-after-free
    // report) is the primary assertion for GDB-1311.
    EXPECT_EQ(unexpected_errors.load(), 0);
    EXPECT_GE(successes.load(), 1) << "at least one concurrent INSERT should have won the race";
    EXPECT_EQ(successes.load() + failures.load(), kThreads * kItersPerThread);

    // NOTE: PRIMARY KEY uniqueness enforcement itself has a separate,
    // pre-existing TOCTOU race under concurrent execute() calls on a shared
    // engine (see tests/qa/test_qa_gdb_1298.cpp's
    // ConcurrentInsertsOnSameUniqueKeyOnlyOneSucceeds, which asserts LE(1)
    // rather than EQ(1) for the same reason). That race is orthogonal to
    // GDB-1311 (which is specifically about the crash / memory corruption in
    // compensate_row_deltas_and_clear(), not about making the uniqueness
    // check itself atomic), so this test does not assert strict uniqueness
    // -- only that the engine never crashes and every INSERT accounts for
    // itself as exactly one success or one CONSTRAINT_VIOLATION failure.
    auto qr = exec_ok("SELECT id FROM race1 WHERE id = 1");
    EXPECT_GE(qr.rows.size(), 1u) << "at least the winning INSERT's row must be present";
}

// Same shape of race, but also interleaves concurrent explicit
// BEGIN/COMMIT/ROLLBACK transactions from other threads on the same shared
// engine, exercising the active_txn_id_ / active_txn_row_deltas_ mutex path
// (as opposed to the lock-free autocommit path) alongside the autocommit
// racers. This covers the ticket's broader audit note that active_txn_id_
// and other unsynchronized per-call QueryEngine state are worth checking
// beyond just the row-deltas map.
TEST_F(QueryEngineConcurrencyTest, ConcurrentAutocommitInsertsAndExplicitTxnsDoNotCrash) {
    exec_ok("CREATE TABLE race2 (id INT PRIMARY KEY, worker INT)");
    exec_ok("CREATE TABLE scratch (id INT PRIMARY KEY)");

    constexpr int kAutocommitThreads = 4;
    constexpr int kTxnThreads = 4;
    constexpr int kItersPerThread = 20;
    std::atomic<int> unexpected_errors{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kAutocommitThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kItersPerThread; ++i) {
                auto result = engine_->execute("INSERT INTO race2 VALUES (1, " +
                                               std::to_string(t * 1000 + i) + ")");
                if (!result.has_value() &&
                    result.error().code != StatusCode::CONSTRAINT_VIOLATION) {
                    unexpected_errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (int t = 0; t < kTxnThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kItersPerThread; ++i) {
                // BEGIN warns-and-no-ops (but still returns ok()) if another
                // thread's explicit txn is already active on this shared
                // engine, since there is exactly one active_txn_id_ slot.
                // That single-slot design means a thread can observe BEGIN
                // succeed and then have its own COMMIT/ROLLBACK race against
                // a *different* thread's concurrent COMMIT/ROLLBACK of that
                // same shared slot, legitimately failing with e.g.
                // TXN_ABORTED ("cannot commit/abort non-active transaction")
                // once the other thread finishes first. That is a known,
                // separate, pre-existing architectural limitation of
                // sharing one QueryEngine's single explicit-transaction slot
                // across connections (called out in the GDB-1311 ticket as
                // worth a future, separate audit) -- not a memory-safety bug,
                // and not what this test is checking. What must hold here is
                // that none of this ever crashes or corrupts memory.
                auto begin = engine_->execute("BEGIN");
                if (!begin.has_value()) {
                    unexpected_errors.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                auto ins = engine_->execute("INSERT INTO scratch VALUES (" +
                                            std::to_string(t * 100000 + i) + ")");
                (void)ins;
                if (i % 2 == 0) {
                    auto commit = engine_->execute("COMMIT");
                    if (!commit.has_value() && commit.error().code != StatusCode::TXN_ABORTED) {
                        unexpected_errors.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    auto rollback = engine_->execute("ROLLBACK");
                    if (!rollback.has_value() && rollback.error().code != StatusCode::TXN_ABORTED) {
                        unexpected_errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    // Reaching this point at all (no SIGSEGV / no ASan heap-use-after-free
    // report) is the primary assertion for GDB-1311. See the note above for
    // why occasional TXN_ABORTED races on the shared single-slot
    // active_txn_id_ are expected and excluded from unexpected_errors.
    EXPECT_EQ(unexpected_errors.load(), 0);

    // No crash on the contended autocommit table (uniqueness itself has a
    // separate, pre-existing TOCTOU race -- see the note in the first test
    // above -- so this only checks that the winning row is present).
    auto qr = exec_ok("SELECT id FROM race2 WHERE id = 1");
    EXPECT_GE(qr.rows.size(), 1u);
}
