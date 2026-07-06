/// Regression tests for GDB-1242: TransactionManager GC used to erase
/// ABORTED transactions without remembering them, while both table_heap's
/// effective_status and mvcc.cpp treated unknown xids as COMMITTED (the
/// no-clog cross-restart compromise). Once GC ran (wired via VACUUM /
/// auto-vacuum, GDB-1230/GDB-969), a GC'd aborted transaction's inserts
/// would resurrect and its deletes would take effect -- abort semantics
/// silently inverted.
///
/// The fix: track pruned_aborted_ alongside pruned_committed_, and make
/// TransactionManager::get_status the single authoritative resolver used by
/// every layer (table_heap::effective_status delegates to it directly; the
/// normalize_xid/vacuum_normalize_xid wrappers now use the new
/// is_registered() predicate instead of get_transaction() != nullptr, since
/// get_transaction only sees LIVE transactions and would otherwise treat a
/// GC'd aborted transaction as unregistered).

#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/txn/txn_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>

using namespace sixseven;

namespace {

class TxnManagerGdb1242Test : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_test_gdb1242.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;
        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
    }

    void TearDown() override {
        bpm_.reset();
        (void)dm_.close_file(file_id_);
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    std::unique_ptr<TableHeap> make_mvcc_heap() {
        auto heap = std::make_unique<TableHeap>(
            *bpm_, dm_, file_id_, TableHeapOptions{.mvcc_headers = true});
        heap->attach_txn_manager(&txn_mgr_);
        return heap;
    }

    /// Advance the xmin horizon past `txn_id` by beginning and committing a
    /// fresh transaction (so xmin_horizon_locked() no longer sees the
    /// completed transaction as "possibly still needed").
    void advance_horizon_past(txn_id_t txn_id) {
        while (txn_mgr_.xmin_horizon() <= txn_id) {
            auto* t = txn_mgr_.begin().value();
            ASSERT_TRUE(txn_mgr_.commit(t->txn_id).has_value());
        }
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
    TransactionManager txn_mgr_;
};

} // namespace

// =============================================================================
// Core AC: aborted-then-GC'd INSERT does not resurrect.
// This test FAILS before the fix (row becomes visible again after GC) and
// PASSES after (row stays invisible).
// =============================================================================

TEST_F(TxnManagerGdb1242Test, AbortedInsertStaysInvisibleAfterGc) {
    auto heap = make_mvcc_heap();

    auto* t1 = txn_mgr_.begin().value();
    txn_id_t aborted_id = t1->txn_id;
    auto rid = heap->insert_tuple(std::vector<uint8_t>{1, 2, 3}, aborted_id);
    ASSERT_TRUE(rid.has_value());

    ASSERT_TRUE(txn_mgr_.abort(aborted_id).has_value());

    // Sanity: invisible immediately after abort, before any GC.
    auto pre_gc = heap->get_tuple(*rid);
    EXPECT_FALSE(pre_gc.has_value());

    // Advance the horizon so the aborted transaction is GC-eligible, then run GC.
    advance_horizon_past(aborted_id);
    txn_mgr_.gc_completed_transactions();

    // The core assertion: still invisible after GC. Before the fix, GC erased
    // the aborted txn without remembering it, so get_status(aborted_id) fell
    // through to unknown-xid handling and the row resurrected.
    auto post_gc = heap->get_tuple(*rid);
    EXPECT_FALSE(post_gc.has_value())
        << "CORRUPTION: aborted insert resurrected after GC (abort semantics inverted)";
}

// =============================================================================
// Core AC (b): aborted-then-GC'd DELETE does not take effect -- the deleted
// row stays visible if its delete was aborted.
// =============================================================================

TEST_F(TxnManagerGdb1242Test, AbortedDeleteStaysReversedAfterGc) {
    auto heap = make_mvcc_heap();

    // Insert and commit a live row.
    auto* inserter = txn_mgr_.begin().value();
    auto rid = heap->insert_tuple(std::vector<uint8_t>{9, 9, 9}, inserter->txn_id);
    ASSERT_TRUE(rid.has_value());
    ASSERT_TRUE(txn_mgr_.commit(inserter->txn_id).has_value());

    // A second transaction deletes it, then aborts.
    auto* deleter = txn_mgr_.begin().value();
    txn_id_t aborted_deleter_id = deleter->txn_id;
    ASSERT_TRUE(heap->mark_deleted(*rid, aborted_deleter_id).has_value());
    ASSERT_TRUE(txn_mgr_.abort(aborted_deleter_id).has_value());

    // Sanity: row still visible immediately after the delete was aborted.
    auto pre_gc = heap->get_tuple(*rid);
    EXPECT_TRUE(pre_gc.has_value());

    advance_horizon_past(aborted_deleter_id);
    txn_mgr_.gc_completed_transactions();

    // The delete must remain reversed (row visible) after GC. Before the fix,
    // the GC'd aborted deleter's xid would resolve as unknown -> COMMITTED,
    // making the aborted delete take effect.
    auto post_gc = heap->get_tuple(*rid);
    EXPECT_TRUE(post_gc.has_value())
        << "CORRUPTION: aborted delete took effect after GC (abort semantics inverted)";
}

// =============================================================================
// Core AC (c): restart compromise preserved -- a truly-unknown xid (never
// registered with this manager, simulating a row from a prior process) is
// still treated as COMMITTED / visible.
// =============================================================================

TEST_F(TxnManagerGdb1242Test, TrulyUnknownXidStillTreatedAsCommitted) {
    const txn_id_t never_registered = 987'654'321;

    EXPECT_FALSE(txn_mgr_.is_registered(never_registered));
    EXPECT_EQ(txn_mgr_.get_status(never_registered), TransactionStatus::COMMITTED);

    auto heap = make_mvcc_heap();
    auto rid = heap->insert_tuple(std::vector<uint8_t>{4, 5, 6}, never_registered);
    ASSERT_TRUE(rid.has_value());

    // Even without ever registering this xid, the row must be readable --
    // this is the existing-data-across-restart compromise.
    auto tuple = heap->get_tuple(*rid);
    EXPECT_TRUE(tuple.has_value())
        << "restart compromise broken: genuinely-unknown xid must read as committed";
}

// =============================================================================
// (d): is_registered correctness across live / committed-pruned /
// aborted-pruned / genuinely-unknown xids.
// =============================================================================

TEST_F(TxnManagerGdb1242Test, IsRegisteredAcrossAllStates) {
    // Genuinely unknown.
    const txn_id_t unknown_id = 111'222'333;
    EXPECT_FALSE(txn_mgr_.is_registered(unknown_id));

    // Live (active) transaction.
    auto* live = txn_mgr_.begin().value();
    txn_id_t live_id = live->txn_id;
    EXPECT_TRUE(txn_mgr_.is_registered(live_id));

    // Committed, then GC'd -- pruned_committed_.
    auto* to_commit = txn_mgr_.begin().value();
    txn_id_t committed_id = to_commit->txn_id;
    ASSERT_TRUE(txn_mgr_.commit(committed_id).has_value());
    EXPECT_TRUE(txn_mgr_.is_registered(committed_id));

    // Aborted, then GC'd -- pruned_aborted_.
    auto* to_abort = txn_mgr_.begin().value();
    txn_id_t aborted_id = to_abort->txn_id;
    ASSERT_TRUE(txn_mgr_.abort(aborted_id).has_value());
    EXPECT_TRUE(txn_mgr_.is_registered(aborted_id));

    // Commit the still-active `live` txn so it doesn't block the horizon,
    // then advance the horizon past everything and GC.
    ASSERT_TRUE(txn_mgr_.commit(live_id).has_value());
    while (txn_mgr_.xmin_horizon() <= aborted_id) {
        auto* t = txn_mgr_.begin().value();
        ASSERT_TRUE(txn_mgr_.commit(t->txn_id).has_value());
    }
    txn_mgr_.gc_completed_transactions();

    // After GC: both committed_id and aborted_id must still report as
    // registered (remembered in pruned_committed_/pruned_aborted_), with the
    // correct respective status; a live-but-now-old transaction id becomes
    // inapplicable here since we committed it above.
    EXPECT_TRUE(txn_mgr_.is_registered(committed_id));
    EXPECT_EQ(txn_mgr_.get_status(committed_id), TransactionStatus::COMMITTED);

    EXPECT_TRUE(txn_mgr_.is_registered(aborted_id));
    EXPECT_EQ(txn_mgr_.get_status(aborted_id), TransactionStatus::ABORTED);

    // The genuinely-unknown id remains unregistered and COMMITTED (restart
    // compromise) after all this activity.
    EXPECT_FALSE(txn_mgr_.is_registered(unknown_id));
    EXPECT_EQ(txn_mgr_.get_status(unknown_id), TransactionStatus::COMMITTED);
}
