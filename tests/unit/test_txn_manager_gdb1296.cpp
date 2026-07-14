/// Regression tests for GDB-1296: TransactionManager's pruned_committed_ and
/// pruned_aborted_ sets used to grow without bound -- every transaction that
/// completed and was pruned below the xmin horizon added an entry that was
/// never removed. Over a long-running process with many short transactions
/// this was an unbounded memory leak.
///
/// The fix: once pruned_committed_ exceeds
/// TransactionManager::pruned_committed_compaction_threshold entries,
/// compact_pruned_committed_locked() clears it and records the xmin horizon
/// at the time of the clear as pruned_committed_watermark_. This is safe
/// because every entry in pruned_committed_ was only ever inserted with
/// txn_id < xmin_horizon_locked() at insertion time, the horizon is
/// monotonically non-decreasing, and get_status()'s genuinely-unknown-xid
/// fallback (COMMITTED) already gives the correct answer for a dropped
/// committed id. pruned_aborted_ is deliberately NOT compacted -- an aborted
/// id must never be forgotten, or it would resurrect via the unknown-xid
/// convention (the exact bug GDB-1242 fixed).

#include "sixseven/txn/txn_manager.h"

#include <gtest/gtest.h>

#include <cstdint>

using namespace sixseven;

namespace {

class TxnManagerGdb1296Test : public ::testing::Test {
protected:
    /// Advance the xmin horizon past `txn_id` by beginning and committing
    /// fresh transactions until the horizon moves past it.
    void advance_horizon_past(txn_id_t txn_id) {
        while (txn_mgr_.xmin_horizon() <= txn_id) {
            auto* t = txn_mgr_.begin().value();
            ASSERT_TRUE(txn_mgr_.commit(t->txn_id).has_value());
        }
    }

    /// Begin+commit `count` short transactions and GC after each one, mimicking
    /// a long-running process handling many short-lived transactions.
    void run_many_committed_transactions(size_t count) {
        for (size_t i = 0; i < count; ++i) {
            auto* t = txn_mgr_.begin().value();
            txn_id_t id = t->txn_id;
            ASSERT_TRUE(txn_mgr_.commit(id).has_value());
            advance_horizon_past(id);
            txn_mgr_.gc_completed_transactions();
        }
    }

    TransactionManager txn_mgr_;
};

} // namespace

// =============================================================================
// AC 1: pruned_committed_ does not grow without bound -- once compaction
// triggers, is_registered() for an OLD compacted-away committed id goes back
// to false (it now resolves purely via the unknown-xid fallback), proving the
// set was actually cleared rather than merely capped in size.
// =============================================================================

TEST_F(TxnManagerGdb1296Test, PrunedCommittedIsCompactedAfterManyTransactions) {
    // Commit and GC one transaction, and remember its id -- immediately after,
    // it must be explicitly tracked (matches pre-existing GDB-1242 behavior:
    // is_registered() is true for a freshly pruned committed id).
    auto* first = txn_mgr_.begin().value();
    txn_id_t first_committed_id = first->txn_id;
    ASSERT_TRUE(txn_mgr_.commit(first_committed_id).has_value());
    advance_horizon_past(first_committed_id);
    txn_mgr_.gc_completed_transactions();
    EXPECT_TRUE(txn_mgr_.is_registered(first_committed_id));

    // Drive enough additional committed+GC'd transactions through to push
    // pruned_committed_ past the compaction threshold.
    run_many_committed_transactions(TransactionManager::pruned_committed_compaction_threshold + 1);

    // After compaction, the set has been cleared: the old id is no longer
    // explicitly tracked (is_registered() is now false for it) -- proving
    // pruned_committed_ does not grow without bound.
    EXPECT_FALSE(txn_mgr_.is_registered(first_committed_id));

    // Status resolution must still be correct: it now comes from the
    // genuinely-unknown-xid fallback, but the answer (COMMITTED) is the same
    // either way.
    EXPECT_EQ(txn_mgr_.get_status(first_committed_id), TransactionStatus::COMMITTED);
}

// =============================================================================
// AC: watermark advances as compaction occurs.
// =============================================================================

TEST_F(TxnManagerGdb1296Test, WatermarkAdvancesOnCompaction) {
    EXPECT_EQ(txn_mgr_.pruned_committed_watermark_for_test(), 0u);

    run_many_committed_transactions(TransactionManager::pruned_committed_compaction_threshold + 1);

    // Compaction must have happened at least once, advancing the watermark
    // above its initial value.
    EXPECT_GT(txn_mgr_.pruned_committed_watermark_for_test(), 0u);
}

// =============================================================================
// AC 2: get_status() resolves correctly for committed ids both above and
// below the compaction watermark.
// =============================================================================

TEST_F(TxnManagerGdb1296Test, CommittedStatusCorrectAboveAndBelowWatermark) {
    auto* below = txn_mgr_.begin().value();
    txn_id_t below_id = below->txn_id;
    ASSERT_TRUE(txn_mgr_.commit(below_id).has_value());
    advance_horizon_past(below_id);
    txn_mgr_.gc_completed_transactions();

    // Force compaction so `below_id` is dropped from pruned_committed_ and now
    // resolves purely via the unknown-xid fallback.
    run_many_committed_transactions(TransactionManager::pruned_committed_compaction_threshold + 1);
    ASSERT_GT(txn_mgr_.pruned_committed_watermark_for_test(), below_id);
    EXPECT_EQ(txn_mgr_.get_status(below_id), TransactionStatus::COMMITTED);

    // A freshly committed+GC'd id above the watermark must still resolve
    // correctly (explicitly tracked, not yet compacted away).
    auto* above = txn_mgr_.begin().value();
    txn_id_t above_id = above->txn_id;
    ASSERT_TRUE(txn_mgr_.commit(above_id).has_value());
    advance_horizon_past(above_id);
    txn_mgr_.gc_completed_transactions();
    EXPECT_EQ(txn_mgr_.get_status(above_id), TransactionStatus::COMMITTED);
}

// =============================================================================
// AC 3 (no regression on GDB-1242): an aborted transaction below the
// compaction watermark must never be reported as COMMITTED -- pruned_aborted_
// is not subject to compaction, so it must still be authoritative.
// =============================================================================

TEST_F(TxnManagerGdb1296Test, AbortedIdBelowWatermarkStillReportsAborted) {
    auto* to_abort = txn_mgr_.begin().value();
    txn_id_t aborted_id = to_abort->txn_id;
    ASSERT_TRUE(txn_mgr_.abort(aborted_id).has_value());
    advance_horizon_past(aborted_id);
    txn_mgr_.gc_completed_transactions();
    ASSERT_EQ(txn_mgr_.get_status(aborted_id), TransactionStatus::ABORTED);

    // Drive enough committed+GC'd transactions through to force
    // pruned_committed_ compaction well past the aborted id.
    run_many_committed_transactions(TransactionManager::pruned_committed_compaction_threshold + 1);
    ASSERT_GT(txn_mgr_.pruned_committed_watermark_for_test(), aborted_id);

    // The aborted id must still resolve as ABORTED -- not resurrected as
    // COMMITTED. pruned_aborted_ is untouched by pruned_committed_ compaction.
    EXPECT_TRUE(txn_mgr_.is_registered(aborted_id));
    EXPECT_EQ(txn_mgr_.get_status(aborted_id), TransactionStatus::ABORTED);
}

// =============================================================================
// Sanity: a genuinely-unknown xid remains COMMITTED (restart compromise,
// GDB-747) after heavy compaction activity -- compaction must not change this
// unrelated convention.
// =============================================================================

TEST_F(TxnManagerGdb1296Test, UnknownXidStillTreatedAsCommittedAfterCompaction) {
    const txn_id_t never_registered = 987'654'321;

    run_many_committed_transactions(TransactionManager::pruned_committed_compaction_threshold + 1);

    EXPECT_FALSE(txn_mgr_.is_registered(never_registered));
    EXPECT_EQ(txn_mgr_.get_status(never_registered), TransactionStatus::COMMITTED);
}
