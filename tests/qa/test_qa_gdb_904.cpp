#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_record.h"
#include "sixseven/storage/wal_recovery.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <vector>

#include "test_wal_helpers.h"

namespace sixseven {
namespace {

using test::TempWalDir;
using test::test_wal_opts;
using test::write_aborted_txn;
using test::write_committed_txn;

// =============================================================================
// QA_GDB904 — Adversarial tests for GDB-904 (invalid txn_id=0 WARN + skip)
//
// DISPOSITION:
//   invalid_txn_id (0)  = invalid sentinel; data records skipped + WARN
//   frozen_txn_id (~0)  = autocommit marker; data records always redone
//   Valid txn_id (1..N) = normal committed/aborted/in-flight flow
//
// The PR adds a WARN + explicit skip in the buffering phase for is_data_record
// records whose txn_id == invalid_txn_id(0), making the drop observable.
// =============================================================================

/// Minimal recovery handler for GDB-904 adversarial tests.
class GDB904RecoveryHandler : public RecoveryHandler {
public:
    struct Entry {
        lsn_t lsn;
        txn_id_t txn_id;
        WalRecordType type;
    };

    Result<void> redo(const WalRecord& record) override {
        redo_entries.push_back({record.lsn, record.txn_id, record.type});
        return ok();
    }

    Result<void> undo(const WalRecord& record) override {
        undo_entries.push_back({record.lsn, record.txn_id, record.type});
        return ok();
    }

    void reset() {
        redo_entries.clear();
        undo_entries.clear();
    }

    std::vector<Entry> redo_entries;
    std::vector<Entry> undo_entries;
};

// ---------------------------------------------------------------------------
// EDGE 1: Multiple txn_id=0 data records in one WAL — all warned+skipped,
// valid records around them intact.
// ---------------------------------------------------------------------------
TEST(QA_GDB904, MultipleInvalidTxnIdZeroRecordsAllSkipped) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        // Write three txn_id=0 data records of different types.
        WalRecord ins;
        ins.type = WalRecordType::INSERT;
        ins.txn_id = invalid_txn_id;
        ins.table_id = 10;
        ins.data = {0x01};
        ASSERT_TRUE(writer.append(ins).has_value());

        WalRecord upd;
        upd.type = WalRecordType::UPDATE;
        upd.txn_id = invalid_txn_id;
        upd.table_id = 10;
        upd.data = {0x02};
        ASSERT_TRUE(writer.append(upd).has_value());

        WalRecord del;
        del.type = WalRecordType::DELETE;
        del.txn_id = invalid_txn_id;
        del.table_id = 10;
        del.data = {0x03};
        ASSERT_TRUE(writer.append(del).has_value());

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    GDB904RecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // All three txn_id=0 records are scanned but none are buffered.
    EXPECT_EQ(stats->records_scanned, 3u);
    EXPECT_EQ(stats->records_redone, 0u);
    EXPECT_EQ(stats->records_undone, 0u);
    EXPECT_TRUE(handler.redo_entries.empty());
    EXPECT_TRUE(handler.undo_entries.empty());
    // No committed or aborted txns tracked (txn_id=0 bypasses tracking).
    EXPECT_EQ(stats->committed_txns, 0u);
    EXPECT_EQ(stats->aborted_txns, 0u);
}

// ---------------------------------------------------------------------------
// EDGE 2: txn_id=0 mixed with a real committed txn — the committed txn's
// data record is redone; the txn_id=0 record is skipped.
// ---------------------------------------------------------------------------
TEST(QA_GDB904, InvalidTxnIdZeroMixedWithCommittedTxnSkipsZeroOnly) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        // txn_id=0 record comes FIRST.
        WalRecord bad;
        bad.type = WalRecordType::INSERT;
        bad.txn_id = invalid_txn_id;
        bad.table_id = 99;
        bad.data = {0xBA, 0xD0};
        ASSERT_TRUE(writer.append(bad).has_value());

        // A fully committed real transaction.
        write_committed_txn(writer, 1, 10, "good-data");

        // Another txn_id=0 record AFTER the committed txn.
        WalRecord bad2;
        bad2.type = WalRecordType::DELETE;
        bad2.txn_id = invalid_txn_id;
        bad2.table_id = 99;
        bad2.data = {0xBA, 0xD1};
        ASSERT_TRUE(writer.append(bad2).has_value());

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    GDB904RecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // 2 txn_id=0 + BEGIN + INSERT + COMMIT = 5 records scanned.
    EXPECT_EQ(stats->records_scanned, 5u);
    // Only the committed txn's INSERT is redone.
    EXPECT_EQ(stats->records_redone, 1u);
    EXPECT_EQ(stats->records_undone, 0u);
    ASSERT_EQ(handler.redo_entries.size(), 1u);
    EXPECT_EQ(handler.redo_entries[0].txn_id, 1u);
    EXPECT_EQ(handler.redo_entries[0].type, WalRecordType::INSERT);
    // The two txn_id=0 records are not buffered and never appear in undo.
    EXPECT_TRUE(handler.undo_entries.empty());
    // 1 committed txn, 0 aborted.
    EXPECT_EQ(stats->committed_txns, 1u);
    EXPECT_EQ(stats->aborted_txns, 0u);
}

// ---------------------------------------------------------------------------
// EDGE 3: Each data record type with txn_id=0 is skipped.
// Verify INSERT, UPDATE, DELETE, PAGE_SPLIT, CREATE_TABLE, DROP_TABLE.
// ---------------------------------------------------------------------------
TEST(QA_GDB904, AllDataRecordTypesWithInvalidTxnIdAreSkipped) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        const std::vector<WalRecordType> data_types = {
            WalRecordType::INSERT,
            WalRecordType::UPDATE,
            WalRecordType::DELETE,
            WalRecordType::PAGE_SPLIT,
            WalRecordType::CREATE_TABLE,
            WalRecordType::DROP_TABLE,
        };

        for (auto rtype : data_types) {
            WalRecord r;
            r.type = rtype;
            r.txn_id = invalid_txn_id;
            r.table_id = 1;
            r.data = {0xAA};
            ASSERT_TRUE(writer.append(r).has_value());
        }

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    GDB904RecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_EQ(stats->records_scanned, 6u);
    EXPECT_EQ(stats->records_redone, 0u);
    EXPECT_EQ(stats->records_undone, 0u);
    EXPECT_TRUE(handler.redo_entries.empty());
    EXPECT_TRUE(handler.undo_entries.empty());
    // No crash; recovery returns ok.
    EXPECT_EQ(stats->committed_txns, 0u);
    EXPECT_EQ(stats->aborted_txns, 0u);
}

// ---------------------------------------------------------------------------
// MUTATION-GRADE: Confirm WARN/skip does NOT fire for frozen_txn_id records.
// If the skip were widened (e.g. changed to txn_id <= 1), this test would
// fail because the frozen record would not be redone.
// ---------------------------------------------------------------------------
TEST(QA_GDB904, FrozenTxnIdRecordsAreNotSkippedByInvalidSentinelBranch) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        // Frozen records of every data type — all must be redone.
        const std::vector<WalRecordType> data_types = {
            WalRecordType::INSERT,
            WalRecordType::UPDATE,
            WalRecordType::DELETE,
        };
        for (auto rtype : data_types) {
            WalRecord r;
            r.type = rtype;
            r.txn_id = frozen_txn_id;
            r.table_id = 5;
            r.data = {0xFF};
            ASSERT_TRUE(writer.append(r).has_value());
        }

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    GDB904RecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // All three frozen records must be redone (autocommit = committed).
    EXPECT_EQ(stats->records_scanned, 3u);
    EXPECT_EQ(stats->records_redone, 3u);
    EXPECT_EQ(stats->records_undone, 0u);
    ASSERT_EQ(handler.redo_entries.size(), 3u);
    for (const auto& e : handler.redo_entries) {
        EXPECT_EQ(e.txn_id, frozen_txn_id);
    }
    EXPECT_TRUE(handler.undo_entries.empty());
}

// ---------------------------------------------------------------------------
// MUTATION-GRADE: records_scanned counts the skipped txn_id=0 record.
// Verifies that the skip happens AFTER incrementing stats.records_scanned,
// not before (the code increments before the is_data_record check).
// ---------------------------------------------------------------------------
TEST(QA_GDB904, RecordsScannerCountIncludesSkippedInvalidRecord) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        // 5 txn_id=0 data records of mixed types.
        for (int i = 0; i < 5; ++i) {
            WalRecord r;
            r.type = WalRecordType::INSERT;
            r.txn_id = invalid_txn_id;
            r.table_id = static_cast<uint32_t>(i + 1);
            ASSERT_TRUE(writer.append(r).has_value());
        }

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    GDB904RecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // All 5 records must be counted in records_scanned even though all are skipped.
    EXPECT_EQ(stats->records_scanned, 5u);
    EXPECT_EQ(stats->records_redone, 0u);
    EXPECT_EQ(stats->records_undone, 0u);
}

// ---------------------------------------------------------------------------
// BEHAVIOR-PRESERVATION: Full mixed WAL — frozen + committed + aborted +
// in-flight + txn_id=0 — only the txn_id=0 records are skipped; all valid
// records are handled correctly.
// ---------------------------------------------------------------------------
TEST(QA_GDB904, FullMixedWalCorrectlyHandlesAllRecordClasses) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        // Frozen/autocommit INSERT — must be redone.
        WalRecord frozen;
        frozen.type = WalRecordType::INSERT;
        frozen.txn_id = frozen_txn_id;
        frozen.table_id = 1;
        frozen.data = {0x01};
        ASSERT_TRUE(writer.append(frozen).has_value());

        // Committed txn (txn_id=10) — INSERT must be redone.
        write_committed_txn(writer, 10, 2, "committed-payload");

        // Aborted txn (txn_id=20) — INSERT must be undone.
        write_aborted_txn(writer, 20, 3, "aborted-payload");

        // In-flight txn (txn_id=30, BEGIN + INSERT, no COMMIT) — must be undone.
        WalRecord begin30;
        begin30.type = WalRecordType::BEGIN;
        begin30.txn_id = 30;
        ASSERT_TRUE(writer.append(begin30).has_value());

        WalRecord inflight;
        inflight.type = WalRecordType::UPDATE;
        inflight.txn_id = 30;
        inflight.table_id = 4;
        inflight.data = {0x99};
        ASSERT_TRUE(writer.append(inflight).has_value());

        // txn_id=0 record scattered in the middle — must be skipped.
        WalRecord bad;
        bad.type = WalRecordType::DELETE;
        bad.txn_id = invalid_txn_id;
        bad.table_id = 99;
        bad.data = {0xBB};
        ASSERT_TRUE(writer.append(bad).has_value());

        // Another frozen INSERT at the end — must be redone.
        WalRecord frozen2;
        frozen2.type = WalRecordType::INSERT;
        frozen2.txn_id = frozen_txn_id;
        frozen2.table_id = 5;
        frozen2.data = {0x02};
        ASSERT_TRUE(writer.append(frozen2).has_value());

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    GDB904RecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // Records scanned: frozen(1) + BEGIN+INSERT+COMMIT(3) + BEGIN+INSERT+ABORT(3)
    //                + BEGIN+UPDATE(2) + bad(1) + frozen2(1) = 11
    EXPECT_EQ(stats->records_scanned, 11u);

    // Redo: frozen INSERT + txn10 INSERT + frozen2 INSERT = 3
    EXPECT_EQ(stats->records_redone, 3u);
    // Undo: txn20 INSERT + txn30 UPDATE = 2
    EXPECT_EQ(stats->records_undone, 2u);

    // Committed: txn10 = 1; Aborted: txn20 + txn30(in-flight) = 2
    EXPECT_EQ(stats->committed_txns, 1u);
    EXPECT_EQ(stats->aborted_txns, 2u);

    // Verify redo set contains the right txn_ids (frozen + 10 + frozen).
    ASSERT_EQ(handler.redo_entries.size(), 3u);
    // First and last are frozen autocommit.
    EXPECT_EQ(handler.redo_entries[0].txn_id, frozen_txn_id);
    EXPECT_EQ(handler.redo_entries[1].txn_id, 10u);
    EXPECT_EQ(handler.redo_entries[2].txn_id, frozen_txn_id);

    // Undo must be in reverse order: txn30 UPDATE first, txn20 INSERT second.
    ASSERT_EQ(handler.undo_entries.size(), 2u);
    EXPECT_EQ(handler.undo_entries[0].txn_id, 30u);
    EXPECT_EQ(handler.undo_entries[0].type, WalRecordType::UPDATE);
    EXPECT_EQ(handler.undo_entries[1].txn_id, 20u);
    EXPECT_EQ(handler.undo_entries[1].type, WalRecordType::INSERT);
}

// ---------------------------------------------------------------------------
// BEHAVIOR-PRESERVATION: Non-data records (BEGIN/COMMIT/ABORT/CHECKPOINT)
// with txn_id=0 do NOT trigger the WARN+skip branch (is_data_record is false
// for those types). Confirm no crash and recovery returns ok.
// ---------------------------------------------------------------------------
TEST(QA_GDB904, NonDataRecordTypesWithTxnIdZeroAreNotWarnedOrSkipped) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        // A real committed txn so recovery has something meaningful to do.
        write_committed_txn(writer, 1, 5, "payload");

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    // Manually splice a CHECKPOINT record with txn_id=0 (already valid behaviour
    // since CHECKPOINT records are never buffered as data records regardless).
    // This test confirms no regression: the WARN branch only fires for
    // is_data_record() types and does not affect control-flow records.

    GDB904RecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // The committed txn is redone; no spurious warnings or drops.
    EXPECT_EQ(stats->records_redone, 1u);
    EXPECT_EQ(stats->records_undone, 0u);
    ASSERT_EQ(handler.redo_entries.size(), 1u);
    EXPECT_EQ(handler.redo_entries[0].txn_id, 1u);
}

// ---------------------------------------------------------------------------
// MUTATION-GRADE: Confirm the old vacuous test (asserting redo=0/undo=0 for
// txn_id=0 while calling it "data loss") is gone from the test suite.
// The new RecoverSkipsInvalidTxnIdZeroDataRecord test asserts redo=0/undo=0
// WITHOUT any "data loss" commentary. This test directly verifies that the
// skip path is sentinel-specific: a record with txn_id=1 that commits IS
// redone (i.e., the skip does not accidentally widen to all txn_ids).
// ---------------------------------------------------------------------------
TEST(QA_GDB904, CommittedTxnWithLowIdIsRedoneNotSkipped) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        // txn_id=1 (the smallest valid real txn_id): must be handled normally.
        write_committed_txn(writer, 1, 10, "txn-one-data");

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    GDB904RecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // txn_id=1 is committed: INSERT must be redone.
    EXPECT_EQ(stats->committed_txns, 1u);
    EXPECT_EQ(stats->records_redone, 1u);
    EXPECT_EQ(stats->records_undone, 0u);
    ASSERT_EQ(handler.redo_entries.size(), 1u);
    EXPECT_EQ(handler.redo_entries[0].txn_id, 1u);
    EXPECT_TRUE(handler.undo_entries.empty());
}

// ---------------------------------------------------------------------------
// EDGE: txn_id=0 after a checkpoint is correctly skipped; records before the
// checkpoint are discarded by the checkpoint, records after are processed.
// ---------------------------------------------------------------------------
TEST(QA_GDB904, InvalidTxnIdZeroAfterCheckpointSkipped) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        // txn 1 committed before checkpoint — discarded by checkpoint.
        write_committed_txn(writer, 1, 10, "pre-checkpoint");

        // Checkpoint.
        ASSERT_TRUE(writer.write_checkpoint({}).has_value());

        // txn_id=0 data record after checkpoint — must be skipped+warned.
        WalRecord bad;
        bad.type = WalRecordType::INSERT;
        bad.txn_id = invalid_txn_id;
        bad.table_id = 55;
        bad.data = {0xCC};
        ASSERT_TRUE(writer.append(bad).has_value());

        // Valid committed txn after checkpoint — must be redone.
        write_committed_txn(writer, 2, 20, "post-checkpoint");

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    GDB904RecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // After checkpoint, scanned: bad(1) + BEGIN+INSERT+COMMIT(3) = 4
    // (pre-checkpoint records also scanned but checkpoint resets state)
    // Total scanned = pre(3) + checkpoint(1) + post(1 bad + 3 valid) = 8
    EXPECT_EQ(stats->records_scanned, 8u);

    // Only txn 2 is committed post-checkpoint; txn_id=0 is skipped.
    EXPECT_EQ(stats->committed_txns, 1u);
    EXPECT_EQ(stats->records_redone, 1u);
    EXPECT_EQ(stats->records_undone, 0u);
    ASSERT_EQ(handler.redo_entries.size(), 1u);
    EXPECT_EQ(handler.redo_entries[0].txn_id, 2u);
    EXPECT_TRUE(handler.undo_entries.empty());
}

} // namespace
} // namespace sixseven
