// QA regression / adversarial tests for GDB-1311: QueryEngine
// heap-use-after-free on shared txn-compensation state under concurrent
// autocommit statements.
//
// The implementer's fix (src/executor/query_engine.cpp,
// include/sixseven/executor/query_engine.h) adds txn_state_mutex_ to guard
// active_txn_id_ / active_txn_row_deltas_ for explicit BEGIN/COMMIT/ROLLBACK
// bookkeeping, and reroutes the autocommit/implicit-txn abort path onto a
// new lock-free compensate_table_row_delta(table_id, delta) that only
// touches TableHeap::adjust_row_count() (itself CAS-protected).
//
// This file adversarially re-verifies:
//   1. The original GDB-1298-QA repro shape (many threads racing INSERTs on
//      the same PRIMARY KEY on one shared QueryEngine) across many
//      iterations, not a single run -- concurrency bugs are probabilistic.
//   2. The lock-free compensate_table_row_delta() path under heavier
//      concurrency (more threads, more tables) racing against explicit-txn
//      bookkeeping that holds txn_state_mutex_, looking for any crash or
//      corrupted row_count_.
//   3. Mixed workloads: concurrent autocommit INSERTs on different
//      keys/tables interleaved with explicit BEGIN/COMMIT/ROLLBACK
//      sequences on other threads of the same shared engine, probing the
//      disclosed single-active_txn_id_-slot limitation for any NEW
//      memory-safety issue (semantic races there are known/accepted; a
//      crash is not).
//
// Deliberately out of scope (per GDB-1311 handoff): GDB-1298's TOCTOU
// PRIMARY KEY uniqueness race is not re-litigated here.

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

class QaGdb1311Test : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_1311";
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

// (1) Repeat the exact GDB-1298 repro shape many times in one process, not
// once. A single pass could get lucky; each iteration recreates a fresh
// table so it is an independent trial of the race window inside
// compensate_row_deltas_and_clear() / compensate_table_row_delta().
TEST_F(QaGdb1311Test, RepeatedConcurrentAutocommitSameKeyManyIterationsNoCrash) {
    constexpr int kOuterIterations = 15;
    constexpr int kThreads = 8;
    constexpr int kItersPerThread = 20;

    for (int outer = 0; outer < kOuterIterations; ++outer) {
        std::string table = "race_iter" + std::to_string(outer);
        exec_ok("CREATE TABLE " + table + " (id INT PRIMARY KEY, worker INT)");

        std::atomic<int> successes{0};
        std::atomic<int> failures{0};
        std::atomic<int> unexpected_errors{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                for (int i = 0; i < kItersPerThread; ++i) {
                    auto result = engine_->execute("INSERT INTO " + table + " VALUES (1, " +
                                                    std::to_string(t * 1000 + i) + ")");
                    if (result.has_value()) {
                        successes.fetch_add(1, std::memory_order_relaxed);
                    } else if (result.error().code == StatusCode::CONSTRAINT_VIOLATION) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        unexpected_errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& th : threads) th.join();

        // NOTE: this test intentionally does NOT assert that exactly one row
        // survives the race (that would re-litigate GDB-1298's separate,
        // out-of-scope TOCTOU PRIMARY KEY uniqueness race -- duplicate-key
        // inserts are not reliably rejected under concurrency independent of
        // this ticket's fix). GDB-1311 is only about memory safety of the
        // shared txn-compensation bookkeeping; this test's job is to prove
        // that bookkeeping stays internally consistent (every statement's
        // outcome is accounted for, and row_count_ matches the number of
        // statements the engine itself reported as successful) even though
        // some/none of those "successes" may be logical duplicates.
        EXPECT_EQ(unexpected_errors.load(), 0)
            << "iteration " << outer << ": unexpected error code instead of clean "
            << "CONSTRAINT_VIOLATION or success";
        EXPECT_EQ(successes.load() + failures.load(), kThreads * kItersPerThread)
            << "iteration " << outer;

        auto count_result = exec_ok("SELECT COUNT(*) FROM " + table);
        ASSERT_EQ(count_result.rows.size(), 1u);
        EXPECT_EQ(count_result.rows[0][0].as_int64(), successes.load())
            << "iteration " << outer
            << ": row_count_ does not match the engine's own reported successful-insert count "
               "(compensation bookkeeping drifted from reality)";
    }
}

// (2) Stress the lock-free compensate_table_row_delta path specifically:
// many threads doing autocommit inserts on DISTINCT keys of the same table
// (so most succeed, no constraint violation, but the compensation code path
// is still exercised for the failures that do occur), racing against other
// threads which are opening/closing explicit transactions on the SAME
// shared engine (so txn_state_mutex_ is being taken concurrently by threads
// touching active_txn_id_ / active_txn_row_deltas_, while the lock-free
// path concurrently touches only TableHeap::row_count_ via CAS). This
// probes exactly the seam the handoff called out: is row_count_ genuinely
// safe to touch from both paths concurrently.
TEST_F(QaGdb1311Test, LockFreeCompensationRacesWithExplicitTxnBookkeeping) {
    exec_ok("CREATE TABLE stress_tbl (id INT PRIMARY KEY, worker INT)");
    exec_ok("CREATE TABLE side_tbl (id INT PRIMARY KEY, val INT)");

    constexpr int kAutocommitThreads = 6;
    constexpr int kExplicitTxnThreads = 4;
    constexpr int kIters = 40;

    std::atomic<bool> stop{false};
    std::atomic<int> autocommit_unexpected{0};
    std::atomic<int> explicit_unexpected{0};
    std::vector<std::thread> threads;

    // Autocommit inserts racing on the SAME key across threads (forces the
    // CONSTRAINT_VIOLATION -> compensate_table_row_delta() path repeatedly),
    // interleaved with distinct-key inserts (forces the success path).
    for (int t = 0; t < kAutocommitThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kIters; ++i) {
                // Every other statement targets the shared contested key 1;
                // the rest use a per-thread-unique key so most succeed.
                int key = (i % 2 == 0) ? 1 : (t * 10000 + i);
                auto result = engine_->execute("INSERT INTO stress_tbl VALUES (" +
                                                std::to_string(key) + ", " +
                                                std::to_string(t * 1000 + i) + ")");
                if (!result.has_value() && result.error().code != StatusCode::CONSTRAINT_VIOLATION) {
                    autocommit_unexpected.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // Concurrent explicit BEGIN/INSERT/COMMIT or ROLLBACK sequences on the
    // SAME shared engine (single active_txn_id_ slot, per the disclosed
    // limitation) -- semantic interleaving is accepted/known, but must not
    // crash or corrupt memory.
    for (int t = 0; t < kExplicitTxnThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kIters; ++i) {
                auto begin_result = engine_->execute("BEGIN");
                if (!begin_result.has_value()) {
                    explicit_unexpected.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                int key = 100000 + t * 1000 + i;
                auto ins_result = engine_->execute("INSERT INTO side_tbl VALUES (" +
                                                    std::to_string(key) + ", " +
                                                    std::to_string(i) + ")");
                // ins_result may legitimately fail if a concurrent thread's
                // BEGIN stole the single active_txn_id_ slot mid-sequence
                // (known semantic limitation) -- only crash-class outcomes
                // are failures here, so no assertion on ins_result itself.
                (void)ins_result;
                if (i % 2 == 0) {
                    auto commit_result = engine_->execute("COMMIT");
                    (void)commit_result;
                } else {
                    auto rollback_result = engine_->execute("ROLLBACK");
                    (void)rollback_result;
                }
            }
        });
    }

    for (auto& th : threads) th.join();
    stop = true;

    EXPECT_EQ(autocommit_unexpected.load(), 0)
        << "autocommit path surfaced a non-CONSTRAINT_VIOLATION error under mixed load";
    EXPECT_EQ(explicit_unexpected.load(), 0) << "BEGIN itself failed under mixed load";

    // No crash reaching here is itself the primary signal (ASan run
    // catches memory errors); additionally sanity-check row_count_ is a
    // plausible non-negative value bounded by total statement volume, i.e.
    // it wasn't corrupted into a wild/negative-wrapped value.
    auto count_result = exec_ok("SELECT COUNT(*) FROM stress_tbl");
    ASSERT_EQ(count_result.rows.size(), 1u);
    int64_t final_count = count_result.rows[0][0].as_int64();
    EXPECT_GE(final_count, 0);
    EXPECT_LE(final_count, static_cast<int64_t>(kAutocommitThreads) * kIters);

    auto side_count_result = exec_ok("SELECT COUNT(*) FROM side_tbl");
    ASSERT_EQ(side_count_result.rows.size(), 1u);
    int64_t side_final_count = side_count_result.rows[0][0].as_int64();
    EXPECT_GE(side_final_count, 0);
    EXPECT_LE(side_final_count, static_cast<int64_t>(kExplicitTxnThreads) * kIters);
}

// (3) active_transaction_id() is now lock-guarded (GDB-1311). Hammer it
// concurrently with BEGIN/COMMIT/ROLLBACK on other threads to confirm the
// accessor itself is race-free (it is a trivial read but must not race with
// active_txn_id_ writes under TSan/ASan).
TEST_F(QaGdb1311Test, ActiveTransactionIdAccessorRacesWithBeginCommit) {
    exec_ok("CREATE TABLE txn_probe_tbl (id INT PRIMARY KEY, val INT)");

    constexpr int kReaderThreads = 4;
    constexpr int kWriterThreads = 4;
    constexpr int kIters = 200;

    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;

    for (int t = 0; t < kReaderThreads; ++t) {
        threads.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                // Just observe; any value is legal, we're checking for
                // crashes / torn reads under sanitizers, not a specific
                // sequence.
                volatile auto id = engine_->active_transaction_id();
                (void)id;
            }
        });
    }

    for (int t = 0; t < kWriterThreads; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < kIters; ++i) {
                auto begin_result = engine_->execute("BEGIN");
                (void)begin_result;
                if (i % 3 == 0) {
                    auto rollback_result = engine_->execute("ROLLBACK");
                    (void)rollback_result;
                } else {
                    auto commit_result = engine_->execute("COMMIT");
                    (void)commit_result;
                }
            }
        });
    }

    for (int t = 0; t < kWriterThreads; ++t) {
        threads[kReaderThreads + t].join();
    }
    stop = true;
    for (int t = 0; t < kReaderThreads; ++t) {
        threads[t].join();
    }

    // Reaching here without crashing/hanging under ASan is the assertion.
    // Engine must end in a sane state usable for further statements.
    exec_ok("INSERT INTO txn_probe_tbl VALUES (1, 1)");
    auto count_result = exec_ok("SELECT COUNT(*) FROM txn_probe_tbl");
    ASSERT_EQ(count_result.rows.size(), 1u);
    EXPECT_EQ(count_result.rows[0][0].as_int64(), 1);
}

// (4) Boundary: many DISTINCT keys inserted concurrently (no constraint
// violations at all) so compensate_table_row_delta() is never invoked --
// confirms the happy path leaves row_count_ exactly correct under
// concurrency with zero compensation events, isolating that the fix didn't
// regress the non-error path.
TEST_F(QaGdb1311Test, ConcurrentAutocommitDistinctKeysExactRowCount) {
    exec_ok("CREATE TABLE distinct_tbl (id INT PRIMARY KEY, worker INT)");

    constexpr int kThreads = 8;
    constexpr int kItersPerThread = 30;
    std::vector<std::thread> threads;
    std::atomic<int> unexpected_errors{0};

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kItersPerThread; ++i) {
                int key = t * kItersPerThread + i;
                auto result =
                    engine_->execute("INSERT INTO distinct_tbl VALUES (" + std::to_string(key) +
                                      ", " + std::to_string(t) + ")");
                if (!result.has_value()) {
                    unexpected_errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(unexpected_errors.load(), 0);
    auto count_result = exec_ok("SELECT COUNT(*) FROM distinct_tbl");
    ASSERT_EQ(count_result.rows.size(), 1u);
    EXPECT_EQ(count_result.rows[0][0].as_int64(), kThreads * kItersPerThread);
}

// (5) Zero/degenerate concurrency: a single thread doing BEGIN; INSERT;
// ROLLBACK repeatedly on a shared engine (no actual contention) must still
// correctly compensate every time -- baseline correctness check for
// compensate_row_deltas_and_clear() under the new locked implementation,
// unrelated to races but guards against the lock accidentally breaking
// single-threaded semantics.
TEST_F(QaGdb1311Test, SingleThreadedExplicitRollbackCompensationStillCorrect) {
    exec_ok("CREATE TABLE rb_tbl (id INT PRIMARY KEY, val INT)");

    for (int i = 0; i < 25; ++i) {
        exec_ok("BEGIN");
        auto ins = engine_->execute("INSERT INTO rb_tbl VALUES (" + std::to_string(i) + ", 1)");
        ASSERT_TRUE(ins.has_value()) << ins.error().message;
        exec_ok("ROLLBACK");
    }

    auto count_result = exec_ok("SELECT COUNT(*) FROM rb_tbl");
    ASSERT_EQ(count_result.rows.size(), 1u);
    EXPECT_EQ(count_result.rows[0][0].as_int64(), 0)
        << "row_count_ not correctly compensated after repeated explicit ROLLBACK";
}
