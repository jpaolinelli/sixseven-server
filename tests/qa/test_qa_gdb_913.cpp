// GDB-913 (audit finding H24): Recovery discards pre-checkpoint records of
// transactions still active at the checkpoint, making their undo impossible.
//
// This is the same underlying defect as GDB-1077 (analysis phase in
// WalRecovery::recover(), src/storage/wal_recovery.cpp) filed independently
// from a separate security/durability audit pass. The fix already lives on
// main (option (a): retain pre-checkpoint records of active-at-checkpoint
// txns in carried_undo_records so the undo phase can reach them, sorted with
// post-checkpoint records by LSN descending). This file adds GDB-913's own
// ticket-scoped regression coverage per the acceptance criteria, driving
// WalRecovery at the API level so it runs on Windows.
//
// RED/GREEN check performed manually while implementing this ticket: with
// the `carried_undo_records`/checkpoint-filtering logic in wal_recovery.cpp
// reverted to the old `post_checkpoint_records.clear()`-only behavior (no
// carry-forward), UndoneWhenNeverCommitted and MultiRecordTxnAcrossCheckpoint
// below both fail (pre-checkpoint LSNs missing from undo_log). With the
// current main/this-branch code, all tests below pass.

#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_record.h"
#include "sixseven/storage/wal_recovery.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace sixseven {
namespace {

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

class QA_GDB913 : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<int> counter{0};
        wal_dir_ =
            std::filesystem::temp_directory_path() / ("qa_gdb913_" + std::to_string(counter++));
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

    lsn_t append(WalWriter& w, WalRecord rec) {
        auto r = w.append(rec);
        EXPECT_TRUE(r.has_value()) << r.error().message;
        return r.value_or(0);
    }

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
// AC-1 (core repro): txn does INSERT, a fuzzy CHECKPOINT is written while the
// txn is still active, and the txn never commits -> the pre-checkpoint
// INSERT must be undone by recovery. This is the exact scenario from the
// ticket description and is the one that silently left corrupted state
// applied under the pre-fix behavior.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB913, PreCheckpointInsertUndoneWhenTxnNeverCommits) {
    const txn_id_t T = 101;
    const uint32_t table_id = 1;

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
        ins.data = {0xAB};
        lsn_insert = append(w, ins);

        // Fuzzy checkpoint: T is still active.
        write_checkpoint(w, {T});

        // Crash: no COMMIT/ABORT ever written for T.
        ASSERT_TRUE(w.flush().has_value());
        ASSERT_TRUE(w.close().has_value());
    }

    RecordingHandler handler;
    WalRecovery recovery(wal_dir_, handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_TRUE(handler.undo_contains_lsn(lsn_insert))
        << "Pre-checkpoint INSERT of a txn active-at-checkpoint that never "
           "committed must be undone (GDB-913 / H24)";
    EXPECT_FALSE(handler.redo_contains_lsn(lsn_insert));
    EXPECT_TRUE(stats->aborted_txn_ids.count(T) != 0);
}

// ---------------------------------------------------------------------------
// AC-2: txn active at checkpoint that LATER commits must NOT be undone; its
// pre-checkpoint change survives (already durable via the fuzzy checkpoint;
// post-checkpoint record is redone).
// ---------------------------------------------------------------------------
TEST_F(QA_GDB913, ActiveAtCheckpointTxnThatLaterCommitsIsNotUndone) {
    const txn_id_t T = 102;
    const uint32_t table_id = 2;

    lsn_t lsn_pre = 0;
    lsn_t lsn_post = 0;

    {
        WalWriter w(wal_dir_, opts());
        ASSERT_TRUE(w.open().has_value());

        WalRecord begin;
        begin.type = WalRecordType::BEGIN;
        begin.txn_id = T;
        append(w, begin);

        WalRecord pre;
        pre.type = WalRecordType::UPDATE;
        pre.txn_id = T;
        pre.table_id = table_id;
        pre.data = {0x01};
        lsn_pre = append(w, pre);

        write_checkpoint(w, {T});

        WalRecord post;
        post.type = WalRecordType::UPDATE;
        post.txn_id = T;
        post.table_id = table_id;
        post.data = {0x02};
        lsn_post = append(w, post);

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

    EXPECT_FALSE(handler.undo_contains_lsn(lsn_pre));
    EXPECT_FALSE(handler.undo_contains_lsn(lsn_post));
    EXPECT_TRUE(handler.redo_contains_lsn(lsn_post));
    EXPECT_TRUE(stats->committed_txn_ids.count(T) != 0);
}

// ---------------------------------------------------------------------------
// AC-3: multi-record txn spanning the checkpoint that never commits -> ALL
// of its records (two pre-checkpoint, one post-checkpoint) roll back, in
// LSN-descending order.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB913, MultiRecordTxnAcrossCheckpointFullyRolledBackInOrder) {
    const txn_id_t T = 103;
    const uint32_t table_id = 3;

    lsn_t lsn_a = 0;
    lsn_t lsn_b = 0;
    lsn_t lsn_c = 0;

    {
        WalWriter w(wal_dir_, opts());
        ASSERT_TRUE(w.open().has_value());

        WalRecord begin;
        begin.type = WalRecordType::BEGIN;
        begin.txn_id = T;
        append(w, begin);

        WalRecord a;
        a.type = WalRecordType::INSERT;
        a.txn_id = T;
        a.table_id = table_id;
        a.data = {0xA1};
        lsn_a = append(w, a);

        WalRecord b;
        b.type = WalRecordType::UPDATE;
        b.txn_id = T;
        b.table_id = table_id;
        b.data = {0xA2};
        lsn_b = append(w, b);

        write_checkpoint(w, {T});

        WalRecord c;
        c.type = WalRecordType::DELETE;
        c.txn_id = T;
        c.table_id = table_id;
        c.data = {0xA3};
        lsn_c = append(w, c);

        // Crash: T never commits or aborts.
        ASSERT_TRUE(w.flush().has_value());
        ASSERT_TRUE(w.close().has_value());
    }

    RecordingHandler handler;
    WalRecovery recovery(wal_dir_, handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_TRUE(handler.undo_contains_lsn(lsn_a));
    EXPECT_TRUE(handler.undo_contains_lsn(lsn_b));
    EXPECT_TRUE(handler.undo_contains_lsn(lsn_c));
    EXPECT_FALSE(handler.redo_contains_lsn(lsn_a));
    EXPECT_FALSE(handler.redo_contains_lsn(lsn_b));
    EXPECT_FALSE(handler.redo_contains_lsn(lsn_c));

    ASSERT_EQ(handler.undo_log.size(), 3u);
    // LSN-descending: c (post-checkpoint, highest) first, then b, then a.
    EXPECT_EQ(handler.undo_log[0].lsn, lsn_c);
    EXPECT_EQ(handler.undo_log[1].lsn, lsn_b);
    EXPECT_EQ(handler.undo_log[2].lsn, lsn_a);

    EXPECT_TRUE(stats->aborted_txn_ids.count(T) != 0);
}

} // namespace
} // namespace sixseven
