// GDB-1077: Recovery discards pre-checkpoint records of transactions still
// active at the checkpoint, making their undo impossible.
//
// These tests drive WalRecovery directly with a synthesised WAL and a
// recording RecoveryHandler.  No real crash or POSIX fork/kill is needed;
// the tests are Windows-portable.
//
// WHY CASE (i) FAILS ON ORIGIN/MAIN (before this fix):
//   In recover(), when a CHECKPOINT record is encountered, the old code
//   unconditionally calls post_checkpoint_records.clear() (wal_recovery.cpp
//   line ~132) BEFORE examining which transactions were active.  So any data
//   record written by transaction T before the checkpoint is silently dropped.
//   The undo phase iterates only post_checkpoint_records; T's pre-checkpoint
//   INSERT (R1) is no longer in that container, so handler_.undo() is never
//   called for R1.  The test asserts that undo IS called for R1 -- this
//   assertion fails on old main, confirming the mutation guard.

#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_record.h"
#include "sixseven/storage/wal_recovery.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// Recording handler
// ---------------------------------------------------------------------------

class RecordingHandler : public RecoveryHandler {
public:
    struct Entry {
        lsn_t lsn;
        txn_id_t txn_id;
        WalRecordType type;
    };

    Result<void> redo(const WalRecord& rec) override {
        redo_log.push_back({rec.lsn, rec.txn_id, rec.type});
        return ok();
    }

    Result<void> undo(const WalRecord& rec) override {
        undo_log.push_back({rec.lsn, rec.txn_id, rec.type});
        return ok();
    }

    bool undo_contains_lsn(lsn_t lsn) const {
        for (const auto& e : undo_log) {
            if (e.lsn == lsn) {
                return true;
            }
        }
        return false;
    }

    bool redo_contains_lsn(lsn_t lsn) const {
        for (const auto& e : redo_log) {
            if (e.lsn == lsn) {
                return true;
            }
        }
        return false;
    }

    std::vector<Entry> redo_log;
    std::vector<Entry> undo_log;
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class QA_GDB1077 : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<int> counter{0};
        wal_dir_ =
            std::filesystem::temp_directory_path() / ("qa_gdb1077_" + std::to_string(counter++));
        std::filesystem::remove_all(wal_dir_);
        std::filesystem::create_directories(wal_dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(wal_dir_, ec);
    }

    WalWriterOptions opts() const {
        WalWriterOptions o;
        o.enable_group_commit = false;
        return o;
    }

    // Append a record and return its LSN.
    lsn_t append(WalWriter& w, WalRecord rec) {
        auto r = w.append(rec);
        EXPECT_TRUE(r.has_value()) << r.error().message;
        return r.value_or(0);
    }

    // Write CHECKPOINT with the given active txn list and return its LSN.
    lsn_t write_checkpoint(WalWriter& w, const std::vector<txn_id_t>& active) {
        WalRecord ckpt;
        ckpt.type = WalRecordType::CHECKPOINT;
        ckpt.txn_id = 0;
        ckpt.data = serialize_checkpoint_data(active);
        return append(w, ckpt);
    }

    std::filesystem::path wal_dir_;
};

// ---------------------------------------------------------------------------
// AC-i: pre-checkpoint record of a loser txn is undone (mutation guard).
//
// WAL sequence:
//   BEGIN T=1
//   INSERT T=1 table=10  <-- R1 (pre-checkpoint)
//   CHECKPOINT active={1}
//   INSERT T=1 table=10  <-- R2 (post-checkpoint)
//   <crash -- no COMMIT for T=1>
//
// Expected: undo called for BOTH R1 and R2.
// On OLD main: undo called for R2 only (R1 is cleared at checkpoint) -- FAILS.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB1077, PreCheckpointRecordUndoneForLoserTxn) {
    const txn_id_t T = 1;
    const uint32_t table_id = 10;

    lsn_t lsn_r1 = 0;
    lsn_t lsn_r2 = 0;

    {
        WalWriter w(wal_dir_, opts());
        ASSERT_TRUE(w.open().has_value());

        WalRecord begin;
        begin.type = WalRecordType::BEGIN;
        begin.txn_id = T;
        append(w, begin);

        WalRecord r1;
        r1.type = WalRecordType::INSERT;
        r1.txn_id = T;
        r1.table_id = table_id;
        r1.data = {0x01};
        lsn_r1 = append(w, r1);

        write_checkpoint(w, {T});

        WalRecord r2;
        r2.type = WalRecordType::INSERT;
        r2.txn_id = T;
        r2.table_id = table_id;
        r2.data = {0x02};
        lsn_r2 = append(w, r2);

        ASSERT_TRUE(w.flush().has_value());
        ASSERT_TRUE(w.close().has_value());
    }

    RecordingHandler handler;
    WalRecovery recovery(wal_dir_, handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // Both R1 (pre-checkpoint) and R2 (post-checkpoint) must be undone.
    EXPECT_TRUE(handler.undo_contains_lsn(lsn_r1))
        << "R1 (pre-checkpoint INSERT for loser txn) was NOT undone -- GDB-1077 bug present";
    EXPECT_TRUE(handler.undo_contains_lsn(lsn_r2))
        << "R2 (post-checkpoint INSERT for loser txn) was NOT undone";

    // Neither R1 nor R2 should be redone (T never committed).
    EXPECT_FALSE(handler.redo_contains_lsn(lsn_r1)) << "R1 should not be redone";
    EXPECT_FALSE(handler.redo_contains_lsn(lsn_r2)) << "R2 should not be redone";

    // T is a loser so it appears in aborted_txn_ids.
    EXPECT_TRUE(stats->aborted_txn_ids.count(T) != 0);
}

// ---------------------------------------------------------------------------
// AC-ii: txn that was active at checkpoint but COMMITs after it is NOT undone.
//
// WAL sequence:
//   BEGIN T=2
//   INSERT T=2 table=20  <-- pre-checkpoint
//   CHECKPOINT active={2}
//   INSERT T=2 table=20  <-- post-checkpoint
//   COMMIT T=2
// ---------------------------------------------------------------------------
TEST_F(QA_GDB1077, TxnCommitsAfterCheckpointIsNotUndone) {
    const txn_id_t T = 2;
    const uint32_t table_id = 20;

    lsn_t lsn_r1 = 0;
    lsn_t lsn_r2 = 0;

    {
        WalWriter w(wal_dir_, opts());
        ASSERT_TRUE(w.open().has_value());

        WalRecord begin;
        begin.type = WalRecordType::BEGIN;
        begin.txn_id = T;
        append(w, begin);

        WalRecord r1;
        r1.type = WalRecordType::INSERT;
        r1.txn_id = T;
        r1.table_id = table_id;
        r1.data = {0xAA};
        lsn_r1 = append(w, r1);

        write_checkpoint(w, {T});

        WalRecord r2;
        r2.type = WalRecordType::INSERT;
        r2.txn_id = T;
        r2.table_id = table_id;
        r2.data = {0xBB};
        lsn_r2 = append(w, r2);

        WalRecord commit;
        commit.type = WalRecordType::COMMIT;
        commit.txn_id = T;
        append(w, commit);

        ASSERT_TRUE(w.flush().has_value());
        ASSERT_TRUE(w.close().has_value());
    }

    RecordingHandler handler;
    WalRecovery recovery(wal_dir_, handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // T committed -- nothing should be undone for it.
    EXPECT_FALSE(handler.undo_contains_lsn(lsn_r1)) << "R1 (committed txn) must not be undone";
    EXPECT_FALSE(handler.undo_contains_lsn(lsn_r2)) << "R2 (committed txn) must not be undone";

    // R2 is post-checkpoint and T is committed -- it should be redone.
    EXPECT_TRUE(handler.redo_contains_lsn(lsn_r2))
        << "R2 (post-checkpoint, committed) must be redone";

    EXPECT_TRUE(stats->committed_txn_ids.count(T) != 0);
    EXPECT_EQ(stats->aborted_txn_ids.count(T), 0u);
}

// ---------------------------------------------------------------------------
// AC-iii: txn that committed BEFORE the checkpoint is not undone and its
// post-checkpoint appearance is absent (regression guard for existing behavior).
//
// WAL sequence:
//   BEGIN T=3
//   INSERT T=3 table=30
//   COMMIT T=3
//   CHECKPOINT active={}   <-- no active txns
// ---------------------------------------------------------------------------
TEST_F(QA_GDB1077, TxnCommittedBeforeCheckpointIsNotUndone) {
    const txn_id_t T = 3;
    const uint32_t table_id = 30;

    lsn_t lsn_insert = 0;

    {
        WalWriter w(wal_dir_, opts());
        ASSERT_TRUE(w.open().has_value());

        WalRecord begin;
        begin.type = WalRecordType::BEGIN;
        begin.txn_id = T;
        append(w, begin);

        WalRecord ins;
        ins.type = WalRecordType::INSERT;
        ins.txn_id = T;
        ins.table_id = table_id;
        ins.data = {0xCC};
        lsn_insert = append(w, ins);

        WalRecord commit;
        commit.type = WalRecordType::COMMIT;
        commit.txn_id = T;
        append(w, commit);

        write_checkpoint(w, {});

        ASSERT_TRUE(w.flush().has_value());
        ASSERT_TRUE(w.close().has_value());
    }

    RecordingHandler handler;
    WalRecovery recovery(wal_dir_, handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // T committed before checkpoint -- should not appear in undo.
    EXPECT_FALSE(handler.undo_contains_lsn(lsn_insert));
    // T committed before checkpoint -- insert is pre-checkpoint so NOT redone
    // (redo only covers post-checkpoint records).
    EXPECT_FALSE(handler.redo_contains_lsn(lsn_insert));

    EXPECT_EQ(stats->aborted_txn_ids.count(T), 0u);
}

// ---------------------------------------------------------------------------
// AC-iv: no active txns at checkpoint -- behavior identical to old code
// (regression guard).
//
// WAL sequence:
//   BEGIN T=4
//   INSERT T=4 table=40  R_committed (post-checkpoint)
//   COMMIT T=4
//   CHECKPOINT active={}
//   BEGIN T=5
//   INSERT T=5 table=40  R_loser (post-checkpoint)
//   <crash>
// ---------------------------------------------------------------------------
TEST_F(QA_GDB1077, NoActiveTxnsAtCheckpointRegressionGuard) {
    const txn_id_t T_committed = 4;
    const txn_id_t T_loser = 5;
    const uint32_t table_id = 40;

    lsn_t lsn_committed = 0;
    lsn_t lsn_loser = 0;

    {
        WalWriter w(wal_dir_, opts());
        ASSERT_TRUE(w.open().has_value());

        write_checkpoint(w, {});

        WalRecord begin4;
        begin4.type = WalRecordType::BEGIN;
        begin4.txn_id = T_committed;
        append(w, begin4);

        WalRecord ins4;
        ins4.type = WalRecordType::INSERT;
        ins4.txn_id = T_committed;
        ins4.table_id = table_id;
        ins4.data = {0x04};
        lsn_committed = append(w, ins4);

        WalRecord commit4;
        commit4.type = WalRecordType::COMMIT;
        commit4.txn_id = T_committed;
        append(w, commit4);

        WalRecord begin5;
        begin5.type = WalRecordType::BEGIN;
        begin5.txn_id = T_loser;
        append(w, begin5);

        WalRecord ins5;
        ins5.type = WalRecordType::INSERT;
        ins5.txn_id = T_loser;
        ins5.table_id = table_id;
        ins5.data = {0x05};
        lsn_loser = append(w, ins5);

        ASSERT_TRUE(w.flush().has_value());
        ASSERT_TRUE(w.close().has_value());
    }

    RecordingHandler handler;
    WalRecovery recovery(wal_dir_, handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // Committed txn: redone, NOT undone.
    EXPECT_TRUE(handler.redo_contains_lsn(lsn_committed));
    EXPECT_FALSE(handler.undo_contains_lsn(lsn_committed));

    // Loser txn: undone, NOT redone.
    EXPECT_TRUE(handler.undo_contains_lsn(lsn_loser));
    EXPECT_FALSE(handler.redo_contains_lsn(lsn_loser));

    EXPECT_TRUE(stats->committed_txn_ids.count(T_committed) != 0);
    EXPECT_TRUE(stats->aborted_txn_ids.count(T_loser) != 0);
}

// ---------------------------------------------------------------------------
// AC-v: undo order is LSN descending across carried + post-checkpoint records.
//
// Verify that when a loser txn has both pre- and post-checkpoint records they
// are undone latest-first (R2 before R1).
// ---------------------------------------------------------------------------
TEST_F(QA_GDB1077, UndoOrderIsLsnDescending) {
    const txn_id_t T = 6;
    const uint32_t table_id = 60;

    lsn_t lsn_r1 = 0;
    lsn_t lsn_r2 = 0;

    {
        WalWriter w(wal_dir_, opts());
        ASSERT_TRUE(w.open().has_value());

        WalRecord begin;
        begin.type = WalRecordType::BEGIN;
        begin.txn_id = T;
        append(w, begin);

        WalRecord r1;
        r1.type = WalRecordType::INSERT;
        r1.txn_id = T;
        r1.table_id = table_id;
        r1.data = {0x11};
        lsn_r1 = append(w, r1);

        write_checkpoint(w, {T});

        WalRecord r2;
        r2.type = WalRecordType::INSERT;
        r2.txn_id = T;
        r2.table_id = table_id;
        r2.data = {0x22};
        lsn_r2 = append(w, r2);

        ASSERT_TRUE(w.flush().has_value());
        ASSERT_TRUE(w.close().has_value());
    }

    RecordingHandler handler;
    WalRecovery recovery(wal_dir_, handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    ASSERT_EQ(handler.undo_log.size(), 2u);
    // R2 (higher LSN) must be undone before R1 (lower LSN).
    EXPECT_EQ(handler.undo_log[0].lsn, lsn_r2);
    EXPECT_EQ(handler.undo_log[1].lsn, lsn_r1);
}

} // namespace
} // namespace sixseven
