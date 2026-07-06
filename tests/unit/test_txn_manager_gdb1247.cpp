/// Regression tests for GDB-1247: TransactionManager::next_txn_id_ used to
/// reset to 1 on every process start. Tuples persisted by a previous run
/// carry xmin/xmax stamped with ids from that run; once a new process
/// allocates the same ids to live transactions, get_status()/MVCC visibility
/// can no longer distinguish "old committed row" from "current in-flight
/// txn" -- a live, in-flight transaction id could be misread as committed
/// (harmless collision) or, worse, an old committed row's xmin could match a
/// transaction that later aborts, resurrecting/hiding rows incorrectly.
///
/// The fix: persist the txn-id high-water mark via a dedicated
/// TXN_ID_WATERMARK WAL record, batched like PostgreSQL's XID allocation
/// (TransactionManager::txn_id_batch_size ids per persisted ceiling, ceiling
/// persisted BEFORE any id in the batch is handed out). On restart,
/// WalRecovery::recover() reports RecoveryStats::max_txn_id_seen, and
/// TransactionManager::init_next_txn_id() seeds next_txn_id_ strictly above
/// it.
///
/// These tests simulate a restart by:
///   1. Constructing a TransactionManager + WalWriter pointed at a temp WAL
///      dir, stamping rows via a real MVCC TableHeap, and letting the
///      manager persist watermark(s) as transactions begin.
///   2. Destroying both and reconstructing fresh instances pointed at the
///      same WAL dir (the actual restart path: WalRecovery::recover() then
///      TransactionManager::init_next_txn_id()).
///   3. Asserting the new manager's next_txn_id_ never reuses an id that
///      could collide with a persisted xmin/xmax, and that old rows remain
///      correctly visible to a freshly begun transaction.

#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_recovery.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/txn/txn_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

using namespace sixseven;

namespace {

/// Minimal RecoveryHandler: this suite never needs redo/undo (no data
/// records are involved), but WalRecovery::recover() requires a handler.
class NoOpRecoveryHandler : public RecoveryHandler {
public:
    Result<void> redo(const WalRecord&) override { return ok(); }
    Result<void> undo(const WalRecord&) override { return ok(); }
};

class TxnManagerGdb1247Test : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = std::filesystem::temp_directory_path() / "sixseven_test_gdb1247.db";
        wal_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_gdb1247_wal";
        std::filesystem::remove(db_path_);
        std::filesystem::remove_all(wal_dir_);
        std::filesystem::create_directories(wal_dir_);

        auto fid = dm_.create_file(db_path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;
        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
    }

    void TearDown() override {
        bpm_.reset();
        (void)dm_.close_file(file_id_);
        std::error_code ec;
        std::filesystem::remove(db_path_, ec);
        std::filesystem::remove_all(wal_dir_, ec);
    }

    /// Recover the persisted txn-id watermark from wal_dir_, exactly as
    /// main.cpp does after a crash (WalRecovery::recover() ->
    /// RecoveryStats::max_txn_id_seen).
    txn_id_t recover_max_txn_id() {
        NoOpRecoveryHandler handler;
        WalRecovery recovery(wal_dir_, handler);
        auto stats = recovery.recover();
        EXPECT_TRUE(stats.has_value()) << stats.error().message;
        return stats.has_value() ? stats->max_txn_id_seen : 0;
    }

    std::filesystem::path db_path_;
    std::filesystem::path wal_dir_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
};

} // namespace

// =============================================================================
// AC: after a simulated restart, next_txn_id_ never reuses an id already
// stamped into a persisted row's xmin, and the old row remains visible.
TEST_F(TxnManagerGdb1247Test, RestartDoesNotReuseStampedTxnId) {
    TableHeap heap(*bpm_, dm_, file_id_, TableHeapOptions{.mvcc_headers = true});

    txn_id_t stamped_xmin = 0;
    RID stamped_rid;

    // ---- "Run 1": stamp a row with a live (not-yet-committed) txn id. ----
    {
        WalWriter wal(wal_dir_);
        ASSERT_TRUE(wal.open().has_value());

        TransactionManager txn_mgr;
        txn_mgr.set_wal_writer(&wal);
        heap.attach_txn_manager(&txn_mgr);

        auto* txn = txn_mgr.begin().value();
        stamped_xmin = txn->txn_id;

        std::vector<uint8_t> row = {1, 2, 3, 4};
        auto rid = heap.insert_tuple(row, stamped_xmin);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
        stamped_rid = *rid;

        ASSERT_TRUE(txn_mgr.commit(stamped_xmin).has_value());

        ASSERT_TRUE(wal.flush().has_value());
        ASSERT_TRUE(wal.close().has_value());
        // txn_mgr and wal are destroyed here -- simulates process exit.
        heap.attach_txn_manager(nullptr);
    }

    // ---- Simulated restart: recover the watermark, seed a fresh manager. ----
    txn_id_t recovered = recover_max_txn_id();
    ASSERT_GE(recovered, stamped_xmin) << "watermark must cover the id used in run 1";

    WalWriter wal2(wal_dir_);
    ASSERT_TRUE(wal2.open().has_value());

    TransactionManager txn_mgr2;
    txn_mgr2.set_wal_writer(&wal2);
    txn_mgr2.init_next_txn_id(recovered + 1);
    heap.attach_txn_manager(&txn_mgr2);

    // Drive new-process txn ids until we reach (and pass) the old stamped id
    // to prove no collision is possible even with many new transactions.
    Transaction* last = nullptr;
    for (txn_id_t i = 0; i < 5; ++i) {
        auto begin_result = txn_mgr2.begin();
        ASSERT_TRUE(begin_result.has_value());
        last = *begin_result;
        // A freshly begun transaction id in the new process must never equal
        // the id stamped into the old row -- that would make the old
        // committed row indistinguishable from this new, still-active txn.
        ASSERT_NE(last->txn_id, stamped_xmin)
            << "new-process txn id collided with a persisted xmin!";
        ASSERT_GT(last->txn_id, stamped_xmin)
            << "new-process txn ids must be allocated strictly above every "
               "id used by the prior process";
    }

    // The old row, stamped and committed by the prior process, must still be
    // visible: get_tuple must succeed and return the original bytes -- it
    // must NOT be treated as written by a currently-active transaction
    // (which would happen if get_status() returned ACTIVE, e.g. if the id
    // were reused and collided with `last`, still ACTIVE at this point).
    auto tuple = heap.get_tuple(stamped_rid);
    ASSERT_TRUE(tuple.has_value()) << tuple.error().message;
    std::vector<uint8_t> expected = {1, 2, 3, 4};
    EXPECT_EQ(*tuple, expected);

    ASSERT_TRUE(wal2.flush().has_value());
    ASSERT_TRUE(wal2.close().has_value());
    heap.attach_txn_manager(nullptr);
}

// =============================================================================
// AC: a crash mid-batch (persisted ceiling > last handed-out id) still
// yields no reuse on restart -- restart must start above the persisted
// ceiling, not above the last id actually used.
TEST_F(TxnManagerGdb1247Test, CrashMidBatchStillPreventsReuse) {
    txn_id_t last_handed_out = 0;
    txn_id_t ceiling_after_first_begin = 0;

    {
        WalWriter wal(wal_dir_);
        ASSERT_TRUE(wal.open().has_value());

        TransactionManager txn_mgr;
        txn_mgr.set_wal_writer(&wal);

        // First begin() reserves a full batch and persists its ceiling
        // before handing out id 1. Only a single id is actually used --
        // the rest of the batch is the "unused tail" that a crash leaves
        // behind.
        auto* txn = txn_mgr.begin().value();
        last_handed_out = txn->txn_id;
        ceiling_after_first_begin =
            txn_mgr.next_txn_id() - 1 + TransactionManager::txn_id_batch_size -
            (txn_mgr.next_txn_id() - 1) % TransactionManager::txn_id_batch_size;

        ASSERT_TRUE(wal.flush().has_value());
        ASSERT_TRUE(wal.close().has_value());
        // No commit, no clean shutdown marker, no further begins: this
        // simulates a crash immediately after the first id in the batch was
        // handed out. The persisted watermark (the batch ceiling) is already
        // durable on disk even though only one id was ever used.
    }

    txn_id_t recovered = recover_max_txn_id();
    ASSERT_GE(recovered, last_handed_out);
    // The recovered watermark must be at least the persisted ceiling -- it
    // must not merely equal the one id that was actually handed out, proving
    // recovery reads the *reserved* ceiling, not just observed txn ids.
    EXPECT_GE(recovered, TransactionManager::txn_id_batch_size)
        << "recovered watermark should reflect the persisted batch ceiling, "
           "not just the single id that was handed out before the crash";

    WalWriter wal2(wal_dir_);
    ASSERT_TRUE(wal2.open().has_value());
    TransactionManager txn_mgr2;
    txn_mgr2.set_wal_writer(&wal2);
    txn_mgr2.init_next_txn_id(recovered + 1);

    auto* new_txn = txn_mgr2.begin().value();
    EXPECT_GT(new_txn->txn_id, last_handed_out);
    EXPECT_GT(new_txn->txn_id, ceiling_after_first_begin - 1);

    ASSERT_TRUE(wal2.flush().has_value());
    ASSERT_TRUE(wal2.close().has_value());
}

// =============================================================================
// AC: with no durable store configured (in-memory/test config), begin()
// still works and starts at 1 -- the documented, non-crash-safe fallback.
TEST_F(TxnManagerGdb1247Test, NoWalWriterFallsBackWithoutCrashing) {
    TransactionManager txn_mgr; // set_wal_writer() never called.

    auto begin_result = txn_mgr.begin();
    ASSERT_TRUE(begin_result.has_value());
    EXPECT_EQ((*begin_result)->txn_id, 1u);

    // Many begins still succeed with no durable store attached.
    for (int i = 0; i < 10; ++i) {
        auto r = txn_mgr.begin();
        ASSERT_TRUE(r.has_value());
    }
}

// =============================================================================
// AC: init_next_txn_id only raises the counter, never lowers it.
TEST_F(TxnManagerGdb1247Test, InitNextTxnIdNeverLowersCounter) {
    TransactionManager txn_mgr;
    txn_mgr.init_next_txn_id(500);
    EXPECT_EQ(txn_mgr.next_txn_id(), 500u);

    // A smaller recovered watermark (e.g. stale/zero) must not roll back the
    // counter once it has already been advanced.
    txn_mgr.init_next_txn_id(10);
    EXPECT_EQ(txn_mgr.next_txn_id(), 500u);

    auto* txn = txn_mgr.begin().value();
    EXPECT_EQ(txn->txn_id, 500u);
}
