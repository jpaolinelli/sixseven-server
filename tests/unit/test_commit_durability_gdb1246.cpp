/// Regression tests for GDB-1246.
///
/// Nothing in the transaction commit path used to wait on WAL durability:
/// QueryEngine::execute_commit() called TransactionManager::commit() and
/// returned success immediately, up to WalWriterOptions::flush_interval
/// (~10ms) before the transaction's WAL records were actually fsynced. An
/// acknowledged transaction could therefore be lost on power failure.
///
/// The fix captures the WAL tail LSN (current_lsn()) before commit and calls
/// WalWriter::flush_until(commit_lsn) before acknowledging success. There is
/// no dedicated COMMIT WAL record appended anywhere in this codebase today
/// (TransactionManager::commit() never touches the WAL, and WAL recovery's
/// COMMIT handling is unreachable for that reason -- recovery relies on the
/// unknown-xid-is-committed convention documented in
/// TransactionManager::get_status()). All of a transaction's data WAL
/// (INSERT/UPDATE/DELETE, appended synchronously by TableHeap::log_wal
/// during DML) is therefore already <= current_lsn() by the time commit is
/// requested, so current_lsn() is a safe, sufficient commit_lsn.
///
/// These tests assert the core AC: the commit ack implies durability
/// (flushed_lsn() >= commit_lsn), and that WAL-disabled configs still commit
/// successfully without crashing.

#include "sixseven/catalog/catalog.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/wal.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "test_catalog_helpers.h"
#include "test_wal_helpers.h"

using namespace sixseven;
using namespace sixseven::test;

namespace {

class CommitDurabilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ =
            std::filesystem::temp_directory_path() / "sixseven_test_commit_durability_gdb1246";
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

    /// Wire a real WalWriter into both StorageManager (so DML actually
    /// produces data WAL records via TableHeap::log_wal) and QueryEngine (so
    /// execute_commit() can see it via wal_writer_).
    void attach_wal() {
        writer_ = std::make_unique<WalWriter>(wal_dir_, test_wal_opts());
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

// -----------------------------------------------------------------------
// AC: commit ack implies durability.
// -----------------------------------------------------------------------

TEST_F(CommitDurabilityTest, CommitAckImpliesFlushedLsnCoversCommitLsn) {
    attach_wal();

    exec_ok("CREATE TABLE t (id INT)");

    exec_ok("BEGIN");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("INSERT INTO t VALUES (2)");

    // The commit_lsn the fix computes is the last-appended LSN at commit
    // time (current_lsn() - 1, since current_lsn() returns the NEXT LSN to
    // be assigned) -- the WAL tail after all of this transaction's data
    // records were appended but before COMMIT returns (COMMIT itself does
    // not append a WAL record). Capture it the same way here so we can
    // assert the postcondition independently of internal engine state.
    lsn_t commit_lsn = writer_->current_lsn() - 1;
    ASSERT_GT(commit_lsn, invalid_lsn) << "transaction should have produced WAL records";

    exec_ok("COMMIT");

    // The core AC: once execute_commit() has returned success, the WAL must
    // already be durably flushed at least to commit_lsn. Before the GDB-1246
    // fix, flushed_lsn() could lag commit_lsn by up to flush_interval,
    // because nothing blocked the ack on a flush.
    EXPECT_GE(writer_->flushed_lsn(), commit_lsn);
}

TEST_F(CommitDurabilityTest, MultipleSequentialCommitsEachDurableOnAck) {
    attach_wal();

    exec_ok("CREATE TABLE t (id INT)");

    for (int i = 0; i < 5; ++i) {
        exec_ok("BEGIN");
        exec_ok("INSERT INTO t VALUES (" + std::to_string(i) + ")");
        lsn_t commit_lsn = writer_->current_lsn() - 1;
        exec_ok("COMMIT");
        EXPECT_GE(writer_->flushed_lsn(), commit_lsn)
            << "commit " << i << " acked before its WAL was durable";
    }
}

TEST_F(CommitDurabilityTest, AutocommitInsertAckImpliesFlushedLsnCoversCommitLsn) {
    attach_wal();

    exec_ok("CREATE TABLE t (id INT)");

    // No explicit BEGIN: this INSERT runs under an implicit/autocommit
    // statement transaction (QueryEngine::execute_plan()'s implicit_txn
    // path), which commits at a separate call site from execute_commit().
    // GDB-1246 initially only fixed the explicit COMMIT path; this exercises
    // the autocommit path, which is the common case for the majority of
    // writes (any DML without an explicit BEGIN). CREATE TABLE is DDL and
    // does not itself append data WAL records (only TableHeap::log_wal-backed
    // INSERT/UPDATE/DELETE do), so the pre-INSERT watermark may legitimately
    // still be invalid_lsn here -- that's fine, we only need it as a
    // baseline to prove the INSERT below advanced the WAL.
    lsn_t before_insert_lsn = writer_->current_lsn() - 1;

    auto result = engine_->execute("INSERT INTO t VALUES (1)");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // The autocommit statement's own INSERT WAL record is the new watermark.
    lsn_t insert_commit_lsn = writer_->current_lsn() - 1;
    ASSERT_GT(insert_commit_lsn, before_insert_lsn)
        << "INSERT should have appended new WAL records";

    // Core AC for the autocommit path: once execute() has returned success
    // for the implicit-txn INSERT, the WAL must already be durably flushed
    // at least to the statement's own commit_lsn.
    EXPECT_GE(writer_->flushed_lsn(), insert_commit_lsn);
}

// -----------------------------------------------------------------------
// WAL-disabled configs must still commit successfully, without crashing.
// -----------------------------------------------------------------------

TEST_F(CommitDurabilityTest, CommitWithoutWalWriterStillSucceeds) {
    // Deliberately do not call attach_wal(): wal_writer_ stays nullptr in
    // both StorageManager and QueryEngine, exercising the guarded no-WAL
    // path in execute_commit().
    exec_ok("CREATE TABLE t (id INT)");

    exec_ok("BEGIN");
    exec_ok("INSERT INTO t VALUES (1)");

    auto result = engine_->execute("COMMIT");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->message, "COMMIT");
}

} // namespace
