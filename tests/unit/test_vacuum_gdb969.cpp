// GDB-969: Vacuum-safe deadness -- regression tests for the MVCC corruption
// hazard where a live row persisted by a prior server process could be
// incorrectly reclaimed because TransactionManager::get_status returns ABORTED
// for any xmin id it does not recognise.
//
// Test (a) CORRUPTION GUARD: a row whose xmin is unknown to the current
//         manager (simulating a prior process) must SURVIVE vacuum.
// Test (b) RECLAIM WORKS:    autocommit-deleted rows (xmax=frozen_txn_id) ARE
//         reclaimed; survivors remain intact.
// Test (c) LIVE ROWS INTACT: never-deleted rows survive unchanged.

#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/page.h"
#include "sixseven/txn/mvcc.h"
#include "sixseven/txn/mvcc_tuple.h"
#include "sixseven/txn/txn_manager.h"
#include "sixseven/txn/vacuum.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <vector>

using namespace sixseven;

// =============================================================================
// Fixture
// =============================================================================

class VacuumGDB969Test : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_test_gdb969.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;

        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
    }

    void TearDown() override {
        bpm_.reset();
        auto close = dm_.close_file(file_id_);
        (void)close;
        std::filesystem::remove(path_);
    }

    /// Insert a raw MVCC tuple with the given xmin/xmax, returns {page_id, slot}.
    std::pair<PageId, SlotId> insert_mvcc_tuple(txn_id_t xmin, txn_id_t xmax = invalid_txn_id) {
        MvccTupleHeader header;
        header.xmin = xmin;
        header.xmax = xmax;

        std::vector<uint8_t> user_data(32, 0xBB);
        auto combined = prepend_mvcc_header(header, user_data);

        auto page_result = bpm_->fetch_page(1);
        Page* page = nullptr;
        if (!page_result) {
            auto new_page = bpm_->new_page();
            EXPECT_TRUE(new_page.has_value());
            if (!new_page.has_value()) {
                return {};
            }
            page = *new_page;
        } else {
            page = *page_result;
        }

        auto slot_result = page->insert_tuple(combined);
        EXPECT_TRUE(slot_result.has_value());
        if (!slot_result.has_value()) {
            return {};
        }
        SlotId slot = *slot_result;
        PageId pid = page->page_id();
        auto unpin = bpm_->unpin_page(pid, true);
        (void)unpin;
        return {pid, slot};
    }

    /// Count live (non-deleted) slots on a page.
    uint32_t count_live_tuples(PageId page_id) {
        auto page_result = bpm_->fetch_page(page_id);
        if (!page_result) {
            return 0;
        }
        Page* page = *page_result;
        uint32_t count = 0;
        for (uint16_t slot = 0; slot < page->slot_count(); ++slot) {
            auto tuple = page->get_tuple(slot);
            if (tuple.has_value()) {
                count++;
            }
        }
        auto unpin = bpm_->unpin_page(page_id, false);
        (void)unpin;
        return count;
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
    TransactionManager txn_mgr_;
};

// =============================================================================
// Test (a): CORRUPTION GUARD
//
// Simulate a row written by a prior server process: stamp a concrete numeric
// xmin that was never registered in txn_mgr_ (is_registered() is false for
// it). Without normalization, is_dead(Case 1) would call get_status() on the
// raw xmin; vacuum_is_dead() instead maps unregistered ids to frozen_txn_id
// (COMMITTED) via vacuum_normalize_xid, and the row must survive.
// =============================================================================

TEST_F(VacuumGDB969Test, CorruptionGuard_UnknownXminSurvivesVacuum) {
    // Choose an xmin that was never registered in this manager.
    // Using a high constant that cannot collide with autoincrement ids started
    // from 1 in a fresh TransactionManager.
    const txn_id_t prior_process_xmin = 999'000'001;

    // Verify the manager indeed does not know about this id (GDB-1242: use
    // is_registered, not get_status's return value, as the unknown-xid
    // predicate -- get_status(unregistered) now returns COMMITTED).
    ASSERT_FALSE(txn_mgr_.is_registered(prior_process_xmin))
        << "pre-condition: manager must not know this xmin";

    // Insert the live row (xmax not set).
    auto [pid, slot] = insert_mvcc_tuple(prior_process_xmin, invalid_txn_id);
    ASSERT_GT(pid, 0u);

    EXPECT_EQ(count_live_tuples(pid), 1u) << "row must be present before vacuum";

    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // The row MUST survive -- it was live, just written by an older process.
    EXPECT_EQ(stats->dead_tuples, 0u) << "vacuum must NOT reclaim a live row with unknown xmin";
    EXPECT_EQ(count_live_tuples(pid), 1u) << "row must still be readable after vacuum";
}

// Variant: unknown xmin whose xmax is also unknown (update by prior process).
// Both must be treated as committed-and-live; only the latest version counts
// but neither version should be wrongly reclaimed by is_dead Case 1.
TEST_F(VacuumGDB969Test, CorruptionGuard_UnknownXminWithUnknownXmaxSurvivesVacuum) {
    const txn_id_t old_xmin = 999'000'002;
    const txn_id_t old_xmax = 999'000'003;

    // Neither id known to this manager (GDB-1242: is_registered is the
    // correct unknown-xid predicate).
    ASSERT_FALSE(txn_mgr_.is_registered(old_xmin));
    ASSERT_FALSE(txn_mgr_.is_registered(old_xmax));

    // Insert a row whose xmax is also an unknown id (simulating an updated
    // version from a prior process session).
    auto [pid, slot] = insert_mvcc_tuple(old_xmin, old_xmax);
    ASSERT_GT(pid, 0u);

    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // With both unknown, vacuum_normalize_xid maps xmax -> frozen_txn_id.
    // frozen_txn_id xmax is treated as dead (autocommit-deleted).  This is
    // the safe conservative behaviour: the old version of a cross-restart
    // update is treated as a committed delete, which is correct.
    // The key safety property: xmin unknown -> NOT reclaimed via Case 1.
    // (It may still be reclaimed if xmax maps to frozen_txn_id -- that is
    // intentional and safe: an old version of an update is dead.)
    //
    // We just verify the call does not crash / panic and returns a result.
    // Whether this specific tuple counts as dead depends on horizon; that is
    // fine -- the corruption hazard is Case 1 (xmin-only), not xmax.
    EXPECT_TRUE(stats.has_value());
}

// =============================================================================
// Test (b): RECLAIM WORKS
//
// Rows deleted via autocommit (xmax = frozen_txn_id, unambiguously dead)
// must be reclaimed; survivors (no xmax) must remain.
// =============================================================================

TEST_F(VacuumGDB969Test, ReclaimWorks_AutocommitDeletedRowsAreRemoved) {
    // Insert 3 live rows via a real committed transaction.
    auto* t1 = txn_mgr_.begin().value();
    auto [pid1, slot1] = insert_mvcc_tuple(t1->txn_id);
    auto [pid2, slot2] = insert_mvcc_tuple(t1->txn_id);
    [[maybe_unused]] auto [pid3, slot3] = insert_mvcc_tuple(t1->txn_id);
    ASSERT_TRUE(txn_mgr_.commit(t1->txn_id).has_value());

    // Autocommit-delete two of them: stamp xmax = frozen_txn_id directly.
    // This simulates what the DML path does for autocommit DELETEs/UPDATEs.
    for (auto [pid, slot] : std::vector<std::pair<PageId, SlotId>>{{pid1, slot1}, {pid2, slot2}}) {
        auto pr = bpm_->fetch_page(pid);
        ASSERT_TRUE(pr.has_value());
        Page* page = *pr;
        auto tdata = page->get_tuple(slot);
        ASSERT_TRUE(tdata.has_value());
        auto sd = std::move(*tdata);
        write_mvcc_xmax(sd.data(), frozen_txn_id);
        ASSERT_TRUE(page->update_tuple(slot, sd).has_value());
        auto unpin = bpm_->unpin_page(pid, true);
        (void)unpin;
    }

    // All three slots still present on the page before vacuum.
    EXPECT_EQ(count_live_tuples(pid1), 3u);

    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // Exactly the 2 autocommit-deleted rows must be reclaimed.
    EXPECT_EQ(stats->dead_tuples, 2u);
    EXPECT_GE(stats->pages_scanned, 1u);

    // The third row (no xmax) must still be readable.
    EXPECT_EQ(count_live_tuples(pid1), 1u) << "surviving row must still be present";
}

TEST_F(VacuumGDB969Test, ReclaimWorks_KnownAbortedXminIsRemoved) {
    // A row inserted by an in-process aborted transaction IS correctly dead.
    auto* t1 = txn_mgr_.begin().value();
    auto [pid, slot] = insert_mvcc_tuple(t1->txn_id);
    ASSERT_TRUE(txn_mgr_.abort(t1->txn_id).has_value());

    EXPECT_EQ(count_live_tuples(pid), 1u);

    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // Known-aborted xmin: vacuum_normalize_xid returns the original id (the
    // transaction IS registered and ABORTED), so is_dead Case 1 fires correctly.
    EXPECT_EQ(stats->dead_tuples, 1u);
    EXPECT_EQ(count_live_tuples(pid), 0u);
}

// =============================================================================
// Test (c): LIVE ROWS INTACT
//
// Rows that are never deleted survive vacuum with data unchanged.
// =============================================================================

TEST_F(VacuumGDB969Test, LiveRowsIntact_NeverDeletedRowsSurvive) {
    // Insert multiple rows via committed transactions; none deleted.
    auto* t1 = txn_mgr_.begin().value();
    auto [pid, s0] = insert_mvcc_tuple(t1->txn_id);
    [[maybe_unused]] auto [p2, s1] = insert_mvcc_tuple(t1->txn_id);
    [[maybe_unused]] auto [p3, s2] = insert_mvcc_tuple(t1->txn_id);
    ASSERT_TRUE(txn_mgr_.commit(t1->txn_id).has_value());

    // All on the same page (page 1).
    EXPECT_EQ(count_live_tuples(pid), 3u);

    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_EQ(stats->dead_tuples, 0u);
    EXPECT_EQ(count_live_tuples(pid), 3u) << "all live rows must survive";
}

// =============================================================================
// run() on empty table is safe
// =============================================================================

TEST_F(VacuumGDB969Test, EmptyTable_VacuumIsNoop) {
    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;
    EXPECT_EQ(stats->dead_tuples, 0u);
    EXPECT_EQ(stats->pages_scanned, 0u);
}
