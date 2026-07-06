/// QA adversarial tests for GDB-1246: commit-ack-implies-durability.
///
/// The fix wires WalWriter::flush_until(commit_lsn) into both the explicit
/// COMMIT path (QueryEngine::execute_commit()) and the autocommit/implicit
/// transaction path (QueryEngine::execute_plan()), so that a client never
/// sees a successful commit ack before that transaction's WAL is durably
/// fsynced.
///
/// These tests specifically try to break the durability guarantee under
/// adversarial conditions: real group commit (background flush thread
/// enabled, not the deterministic-disabled test default), concurrent
/// committers, autocommit as the primary target (was the larger hole per the
/// investigation notes in the PR), WAL-disabled configs, and repeated
/// commit/rollback interleavings.

#include "sixseven/catalog/catalog.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/wal.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "test_catalog_helpers.h"
#include "test_wal_helpers.h"

using namespace sixseven;
using namespace sixseven::test;

namespace {

class QaCommitDurabilityGdb1246 : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1246";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        wal_dir_ = data_dir_ / "wal";
        std::filesystem::create_directories(wal_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        if (writer_) {
            writer_->close().has_value();
        }
        writer_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    /// Attach a real WalWriter with group commit ENABLED (the production
    /// default) -- deliberately not test_wal_opts(), which disables the
    /// background flush thread. This is the adversarial case: an ack must
    /// still imply durability even though the flush thread only wakes every
    /// flush_interval on its own, and flush_until() must force it early.
    void attach_wal_group_commit_enabled() {
        WalWriterOptions opts;
        opts.enable_group_commit = true;
        opts.flush_interval = std::chrono::milliseconds(50); // slow, so a bug would show up
        writer_ = std::make_unique<WalWriter>(wal_dir_, opts);
        ASSERT_TRUE(writer_->open().has_value());
        storage_->set_wal_writer(writer_.get());
        engine_->set_wal_writer(writer_.get());
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value())
            << "exec failed for: " << sql
            << " :: " << (result ? std::string{} : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    Catalog catalog_;
    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::filesystem::path wal_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<WalWriter> writer_;
};

// ---------------------------------------------------------------------------
// 1. ACK implies durable, both paths, under REAL group commit timing.
// ---------------------------------------------------------------------------

TEST_F(QaCommitDurabilityGdb1246, ExplicitCommitDurableUnderSlowGroupCommit) {
    attach_wal_group_commit_enabled();
    exec_ok("CREATE TABLE t (id INT)");

    exec_ok("BEGIN");
    exec_ok("INSERT INTO t VALUES (1)");
    lsn_t commit_lsn = writer_->current_lsn() - 1;
    ASSERT_GT(commit_lsn, invalid_lsn);

    auto start = std::chrono::steady_clock::now();
    exec_ok("COMMIT");
    auto elapsed = std::chrono::steady_clock::now() - start;

    // The core AC: ack implies durable, even with a slow (50ms) background
    // flush interval that would otherwise let the ack race ahead of fsync.
    EXPECT_GE(writer_->flushed_lsn(), commit_lsn);
    // Sanity: flush_until forced an early wake -- it should not have taken
    // anywhere near the full flush_interval*N if it were just polling, but
    // more importantly it should not return before the data is durable.
    EXPECT_LT(elapsed, std::chrono::seconds(5)) << "commit hung waiting on durability";
}

TEST_F(QaCommitDurabilityGdb1246, AutocommitInsertDurableUnderSlowGroupCommit) {
    attach_wal_group_commit_enabled();
    exec_ok("CREATE TABLE t (id INT)");

    // No BEGIN/COMMIT: plain autocommit INSERT, the case called out as the
    // bigger hole prior to this fix.
    auto result = engine_->execute("INSERT INTO t VALUES (42)");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    lsn_t commit_lsn = writer_->current_lsn() - 1;
    ASSERT_GT(commit_lsn, invalid_lsn);
    EXPECT_GE(writer_->flushed_lsn(), commit_lsn);
}

TEST_F(QaCommitDurabilityGdb1246, AutocommitUpdateAndDeleteDurableOnAck) {
    attach_wal_group_commit_enabled();
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");

    auto upd = engine_->execute("UPDATE t SET id = 2 WHERE id = 1");
    ASSERT_TRUE(upd.has_value()) << upd.error().message;
    lsn_t upd_lsn = writer_->current_lsn() - 1;
    EXPECT_GE(writer_->flushed_lsn(), upd_lsn);

    auto del = engine_->execute("DELETE FROM t WHERE id = 2");
    ASSERT_TRUE(del.has_value()) << del.error().message;
    lsn_t del_lsn = writer_->current_lsn() - 1;
    EXPECT_GE(writer_->flushed_lsn(), del_lsn);
}

// ---------------------------------------------------------------------------
// 2. Repeated autocommit statements each individually durable (no lagging
//    watermark from a stale commit_lsn capture).
// ---------------------------------------------------------------------------

TEST_F(QaCommitDurabilityGdb1246, ManySequentialAutocommitInsertsEachDurableOnAck) {
    attach_wal_group_commit_enabled();
    exec_ok("CREATE TABLE t (id INT)");

    for (int i = 0; i < 25; ++i) {
        auto result = engine_->execute("INSERT INTO t VALUES (" + std::to_string(i) + ")");
        ASSERT_TRUE(result.has_value()) << result.error().message;
        lsn_t commit_lsn = writer_->current_lsn() - 1;
        EXPECT_GE(writer_->flushed_lsn(), commit_lsn)
            << "autocommit insert " << i << " acked before durable";
    }
}

// ---------------------------------------------------------------------------
// 3. Group commit: concurrent committers don't deadlock or serialize
//    pathologically; all end durable.
// ---------------------------------------------------------------------------

TEST_F(QaCommitDurabilityGdb1246, ConcurrentAutocommitInsertsAllDurableNoDeadlock) {
    attach_wal_group_commit_enabled();
    exec_ok("CREATE TABLE t (id INT)");

    constexpr int kThreads = 8;
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([this, t, &failures]() {
            // Each thread executes on the SAME engine instance concurrently.
            // QueryEngine::execute is documented as usable from multiple
            // threads for autocommit DML (no shared explicit-txn state is
            // touched across threads here). This exercises GDB-769's
            // group-commit sharing under GDB-1246's added flush_until call.
            auto result = engine_->execute("INSERT INTO t VALUES (" + std::to_string(t) + ")");
            if (!result.has_value()) {
                ++failures;
            }
        });
    }

    // Bound the wait: if flush_until() ever deadlocks (e.g. lock held across
    // the wait, contending with the flush thread), this join will hang and
    // the test framework's own timeout will eventually flag it -- but we
    // also assert no failures were recorded assuming it completes at all.
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(failures.load(), 0) << "some concurrent autocommit inserts failed";

    lsn_t tail = writer_->current_lsn() - 1;
    ASSERT_GT(tail, invalid_lsn);
    EXPECT_GE(writer_->flushed_lsn(), tail)
        << "WAL tail not fully durable after concurrent autocommit inserts";
}

TEST_F(QaCommitDurabilityGdb1246, ConcurrentExplicitCommitsOnSeparateEnginesAllDurable) {
    // Each explicit BEGIN/COMMIT sequence mutates active_txn_id_ on the
    // QueryEngine instance, so concurrent explicit transactions on ONE engine
    // are not a supported scenario. Instead, model "concurrent committers"
    // the way the system actually supports it: multiple QueryEngine/session
    // objects sharing one WalWriter (as multiple client sessions would),
    // each performing its own BEGIN...COMMIT, to verify GDB-769 group-commit
    // sharing works correctly with the new flush_until() call inserted on
    // this path.
    WalWriterOptions opts;
    opts.enable_group_commit = true;
    opts.flush_interval = std::chrono::milliseconds(50);
    writer_ = std::make_unique<WalWriter>(wal_dir_, opts);
    ASSERT_TRUE(writer_->open().has_value());
    storage_->set_wal_writer(writer_.get());
    engine_->set_wal_writer(writer_.get());
    exec_ok("CREATE TABLE t (id INT)");

    constexpr int kEngines = 4;
    std::vector<std::unique_ptr<QueryEngine>> engines;
    for (int i = 0; i < kEngines; ++i) {
        auto e = std::make_unique<QueryEngine>(catalog_, *storage_);
        e->set_wal_writer(writer_.get());
        engines.push_back(std::move(e));
    }

    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int i = 0; i < kEngines; ++i) {
        threads.emplace_back([&engines, i, &failures]() {
            auto b = engines[static_cast<size_t>(i)]->execute("BEGIN");
            auto ins = engines[static_cast<size_t>(i)]->execute("INSERT INTO t VALUES (" +
                                                                 std::to_string(i) + ")");
            auto c = engines[static_cast<size_t>(i)]->execute("COMMIT");
            if (!b.has_value() || !ins.has_value() || !c.has_value()) {
                ++failures;
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(failures.load(), 0);
    lsn_t tail = writer_->current_lsn() - 1;
    ASSERT_GT(tail, invalid_lsn);
    EXPECT_GE(writer_->flushed_lsn(), tail);
}

// ---------------------------------------------------------------------------
// 4. WAL disabled: commits still succeed, no crash/hang.
// ---------------------------------------------------------------------------

TEST_F(QaCommitDurabilityGdb1246, NoWalWriterExplicitCommitSucceeds) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("BEGIN");
    exec_ok("INSERT INTO t VALUES (1)");
    auto result = engine_->execute("COMMIT");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->message, "COMMIT");
}

TEST_F(QaCommitDurabilityGdb1246, NoWalWriterAutocommitInsertSucceeds) {
    exec_ok("CREATE TABLE t (id INT)");
    auto result = engine_->execute("INSERT INTO t VALUES (1)");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->affected_rows, 1);
}

TEST_F(QaCommitDurabilityGdb1246, NoWalWriterManyAutocommitInsertsSucceedNoHang) {
    exec_ok("CREATE TABLE t (id INT)");
    for (int i = 0; i < 50; ++i) {
        auto result = engine_->execute("INSERT INTO t VALUES (" + std::to_string(i) + ")");
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }
}

// ---------------------------------------------------------------------------
// 5. Empty/no-op commit paths: commit outside a transaction, empty
//    transaction (BEGIN; COMMIT with no writes) must not hang on an
//    invalid_lsn watermark.
// ---------------------------------------------------------------------------

TEST_F(QaCommitDurabilityGdb1246, CommitWithNoActiveTransactionIsNoopNotHang) {
    attach_wal_group_commit_enabled();
    // No BEGIN was issued -- active_txn_id_ is invalid_txn_id.
    auto result = engine_->execute("COMMIT");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->message, "COMMIT");
}

TEST_F(QaCommitDurabilityGdb1246, EmptyTransactionBeginCommitNoWritesSucceeds) {
    attach_wal_group_commit_enabled();
    // BEGIN with nothing written before COMMIT: commit_lsn_watermark() may
    // return invalid_lsn if nothing has ever been appended, or the tail of
    // any prior CREATE TABLE catalog activity. Either way this must not
    // block forever waiting on a watermark that will never be reached.
    exec_ok("BEGIN");
    auto result = engine_->execute("COMMIT");
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

TEST_F(QaCommitDurabilityGdb1246, RollbackDoesNotWaitOnDurability) {
    attach_wal_group_commit_enabled();
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("BEGIN");
    exec_ok("INSERT INTO t VALUES (1)");
    // ROLLBACK is explicitly out of scope for the durability wait (aborted
    // data need not be durable). Just confirm it still succeeds promptly.
    auto result = engine_->execute("ROLLBACK");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->message, "ROLLBACK");
}

// ---------------------------------------------------------------------------
// 6. Repeated BEGIN/COMMIT/ROLLBACK interleavings stay durable and don't
//    corrupt the watermark across statement boundaries.
// ---------------------------------------------------------------------------

TEST_F(QaCommitDurabilityGdb1246, InterleavedCommitRollbackEachCorrectlyDurable) {
    attach_wal_group_commit_enabled();
    exec_ok("CREATE TABLE t (id INT)");

    for (int i = 0; i < 10; ++i) {
        exec_ok("BEGIN");
        exec_ok("INSERT INTO t VALUES (" + std::to_string(i) + ")");
        if (i % 2 == 0) {
            lsn_t commit_lsn = writer_->current_lsn() - 1;
            exec_ok("COMMIT");
            EXPECT_GE(writer_->flushed_lsn(), commit_lsn) << "iter " << i;
        } else {
            exec_ok("ROLLBACK");
        }
    }

    // Follow with one more durable autocommit insert to confirm the engine's
    // watermark tracking is still sane after the interleaving.
    auto result = engine_->execute("INSERT INTO t VALUES (999)");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    lsn_t final_lsn = writer_->current_lsn() - 1;
    EXPECT_GE(writer_->flushed_lsn(), final_lsn);
}

// ---------------------------------------------------------------------------
// 7. Large transaction: many statements before commit; the single commit_lsn
//    captured at COMMIT time must cover ALL of them.
// ---------------------------------------------------------------------------

TEST_F(QaCommitDurabilityGdb1246, LargeTransactionAllWritesDurableOnSingleCommit) {
    attach_wal_group_commit_enabled();
    exec_ok("CREATE TABLE t (id INT)");

    exec_ok("BEGIN");
    for (int i = 0; i < 200; ++i) {
        exec_ok("INSERT INTO t VALUES (" + std::to_string(i) + ")");
    }
    lsn_t commit_lsn = writer_->current_lsn() - 1;
    exec_ok("COMMIT");
    EXPECT_GE(writer_->flushed_lsn(), commit_lsn);
}

// ---------------------------------------------------------------------------
// 8. Flush failure / writer shutdown surfaces as a COMMIT error, not a
//    silent success (AC #5: "flush failure -> commit surfaces as ERROR").
// ---------------------------------------------------------------------------

TEST_F(QaCommitDurabilityGdb1246, GracefulWriterCloseWhileCommitWaitingStillDurablyFlushes) {
    // Use a slow flush interval so there is a real window where the commit's
    // flush_until() call is parked waiting on the flush thread, then close()
    // the writer out from under it (simulating a WAL writer shutdown racing
    // an in-flight commit, e.g. server shutdown). WalWriter::close() ->
    // stop_group_commit() causes flush_loop() to run its "final flush on
    // shutdown" block (wal.cpp) BEFORE waking waiters, so a graceful
    // shutdown still durably fsyncs pending data first. The commit should
    // therefore succeed, and flushed_lsn() must cover commit_lsn -- a
    // graceful close is not a lossy failure mode. (Initially written
    // expecting shutdown to fail the commit; verified against wal.cpp's
    // flush_loop() shutdown sequence that the final flush happens first,
    // which is the safer, intended behavior, not a bug.)
    WalWriterOptions opts;
    opts.enable_group_commit = true;
    opts.flush_interval = std::chrono::milliseconds(200); // long enough to reliably race close()
    writer_ = std::make_unique<WalWriter>(wal_dir_, opts);
    ASSERT_TRUE(writer_->open().has_value());
    storage_->set_wal_writer(writer_.get());
    engine_->set_wal_writer(writer_.get());

    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("BEGIN");
    exec_ok("INSERT INTO t VALUES (1)");
    lsn_t commit_lsn = writer_->current_lsn() - 1;

    std::thread closer([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        writer_->close().has_value();
    });

    auto result = engine_->execute("COMMIT");
    closer.join();

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_GE(writer_->flushed_lsn(), commit_lsn);
}

TEST_F(QaCommitDurabilityGdb1246, FlushUntilOnUnappendedLsnIsRejectedNotSilentlyAccepted) {
    // Defense-in-depth: if commit_lsn_watermark() or a future refactor ever
    // computed an LSN ahead of what was actually appended, flush_until()
    // must reject it (INVALID_ARGUMENT) rather than silently waiting forever
    // or returning success for data that doesn't exist yet.
    attach_wal_group_commit_enabled();
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");

    lsn_t not_yet_appended = writer_->current_lsn() + 100;
    auto result = writer_->flush_until(not_yet_appended);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

} // namespace
