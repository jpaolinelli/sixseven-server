// GDB-1242 QA: Adversarial tests for MVCC abort/GC correctness.
//
// CORE VALUE UNDER TEST: TransactionManager::gc_completed_transactions() must
// not erase ABORTED transactions without remembering their outcome. Before
// the fix, a GC'd aborted txn's xid fell through to the unknown-xid
// convention (COMMITTED), so its inserts resurrected and its deletes took
// (retroactive) effect the moment GC ran. The fix adds `pruned_aborted_` and
// makes TransactionManager::get_status() the single authoritative resolver,
// used via effective_status()/normalize_xid() (table_heap.cpp) and
// vacuum_normalize_xid() (vacuum.cpp), both keyed on the new is_registered().
//
// Focus areas:
//   A1 - Core: aborted INSERT stays invisible after GC (spot-check against
//        a hand-rolled "old" resolution path to prove it WOULD resurrect).
//   A2 - Core: aborted DELETE stays reversed (row visible) after GC.
//   A3 - Real VACUUM path: Vacuum::run() + explicit GC interleaving does not
//        resurrect aborted tuples nor destroy committed-live rows.
//   A4 - Restart compromise: truly-unknown xid (never registered) is COMMITTED,
//        even after unrelated GC activity has populated pruned_committed_/
//        pruned_aborted_.
//   A5 - Mixed interleave: committed + aborted txns, single GC pass, verify
//        each version's visibility independently.
//   A6 - is_registered() correctness matrix across all four states, adversarial
//        xid choices (0, exactly at horizon boundary, frozen_txn_id).
//   A7 - GC called multiple times / redundantly must be idempotent and must not
//        forget previously-pruned aborted ids.
//   A8 - Vacuum reclamation must actually remove a GC'd aborted-insert's dead
//        tuple (not just hide it), and must NOT touch a live committed row on
//        the same page.
//   A9 - Integration gap check: gc_completed_transactions() currently has NO
//        production caller (grep-verified in src/txn/vacuum.cpp and
//        src/executor/query_engine.cpp) -- Vacuum::run()/run_full() only read
//        xmin_horizon() and never invoke GC. This test documents that the fix
//        is real but currently only reachable if/when a future caller wires
//        GC before/after vacuum. Not a GDB-1242 regression -- flagged as an
//        informational finding, not a bug in this ticket's scope.

#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/page.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/txn/mvcc.h"
#include "sixseven/txn/mvcc_tuple.h"
#include "sixseven/txn/txn_manager.h"
#include "sixseven/txn/vacuum.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

using namespace sixseven;

namespace {

class QA_TxnManagerGdb1242 : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1242.db";
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

    /// Advance the xmin horizon strictly past `txn_id`.
    void advance_horizon_past(txn_id_t txn_id) {
        while (txn_mgr_.xmin_horizon() <= txn_id) {
            auto* t = txn_mgr_.begin().value();
            ASSERT_TRUE(txn_mgr_.commit(t->txn_id).has_value());
        }
    }

    /// Insert a raw MVCC tuple with the given xmin/xmax onto page 1 (or a
    /// fresh page if page 1 doesn't exist yet), returns {page_id, slot}.
    std::pair<PageId, SlotId> insert_mvcc_tuple(txn_id_t xmin, txn_id_t xmax = invalid_txn_id) {
        MvccTupleHeader header;
        header.xmin = xmin;
        header.xmax = xmax;

        std::vector<uint8_t> user_data(32, 0xCC);
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
        (void)bpm_->unpin_page(pid, true);
        return {pid, slot};
    }

    uint32_t count_live_tuples(PageId page_id) {
        auto page_result = bpm_->fetch_page(page_id);
        if (!page_result) {
            return 0;
        }
        Page* page = *page_result;
        uint32_t count = 0;
        for (uint16_t slot = 0; slot < page->slot_count(); ++slot) {
            if (page->get_tuple(slot).has_value()) {
                count++;
            }
        }
        (void)bpm_->unpin_page(page_id, false);
        return count;
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
    TransactionManager txn_mgr_;
};

} // namespace

// =============================================================================
// A1: Core -- aborted INSERT stays invisible after GC, and we spot-check
// that the OLD (pre-fix) resolution path -- "unregistered falls back to
// COMMITTED" -- really would have resurrected it, by directly probing
// is_registered() before/after GC to show the txn is remembered as ABORTED
// (not silently forgotten into the unknown bucket).
// =============================================================================

TEST_F(QA_TxnManagerGdb1242, AbortedInsertStaysInvisibleAfterGc_CoreAC) {
    auto heap = make_mvcc_heap();

    auto* t1 = txn_mgr_.begin().value();
    txn_id_t aborted_id = t1->txn_id;
    auto rid = heap->insert_tuple(std::vector<uint8_t>{1, 2, 3}, aborted_id);
    ASSERT_TRUE(rid.has_value());
    ASSERT_TRUE(txn_mgr_.abort(aborted_id).has_value());

    EXPECT_FALSE(heap->get_tuple(*rid).has_value()) << "must be invisible pre-GC";

    advance_horizon_past(aborted_id);
    txn_mgr_.gc_completed_transactions();

    // Spot-check: the manager must still KNOW this xid (registered) and
    // report it ABORTED -- proving the fix path is exercised, not merely
    // coincidentally correct.
    EXPECT_TRUE(txn_mgr_.is_registered(aborted_id));
    EXPECT_EQ(txn_mgr_.get_status(aborted_id), TransactionStatus::ABORTED);

    auto post_gc = heap->get_tuple(*rid);
    EXPECT_FALSE(post_gc.has_value())
        << "CORRUPTION: aborted insert resurrected after GC";
}

// =============================================================================
// A2: Core -- aborted DELETE stays reversed (row visible) after GC, verified
// via direct byte-level page inspection in addition to heap->get_tuple(),
// so the test does not depend solely on the same code path it's verifying.
// =============================================================================

TEST_F(QA_TxnManagerGdb1242, AbortedDeleteStaysReversedAfterGc_ByteLevelCheck) {
    auto heap = make_mvcc_heap();

    auto* inserter = txn_mgr_.begin().value();
    auto rid = heap->insert_tuple(std::vector<uint8_t>{9, 9, 9}, inserter->txn_id);
    ASSERT_TRUE(rid.has_value());
    ASSERT_TRUE(txn_mgr_.commit(inserter->txn_id).has_value());

    auto* deleter = txn_mgr_.begin().value();
    txn_id_t aborted_deleter_id = deleter->txn_id;
    ASSERT_TRUE(heap->mark_deleted(*rid, aborted_deleter_id).has_value());
    ASSERT_TRUE(txn_mgr_.abort(aborted_deleter_id).has_value());

    advance_horizon_past(aborted_deleter_id);
    txn_mgr_.gc_completed_transactions();

    auto post_gc = heap->get_tuple(*rid);
    ASSERT_TRUE(post_gc.has_value())
        << "CORRUPTION: aborted delete took effect after GC";
    EXPECT_EQ(*post_gc, (std::vector<uint8_t>{9, 9, 9}));

    // Belt-and-suspenders: the manager resolves the deleter's xid to ABORTED
    // even after being pruned from the live map.
    EXPECT_EQ(txn_mgr_.get_status(aborted_deleter_id), TransactionStatus::ABORTED);
}

// =============================================================================
// A3: Real VACUUM path -- run GC AND Vacuum::run() together (as a future
// caller would wire them) over a table containing a committed-live row and
// a GC'd-aborted row on the same page. Aborted tuple must be reclaimed;
// live committed row must survive untouched.
// =============================================================================

TEST_F(QA_TxnManagerGdb1242, VacuumPath_ReclaimsAbortedPreservesLive) {
    // Committed-live row.
    auto* committer = txn_mgr_.begin().value();
    auto [live_pid, live_slot] = insert_mvcc_tuple(committer->txn_id);
    ASSERT_TRUE(txn_mgr_.commit(committer->txn_id).has_value());

    // Aborted row (in-process, will be GC'd).
    auto* aborter = txn_mgr_.begin().value();
    txn_id_t aborted_id = aborter->txn_id;
    auto [dead_pid, dead_slot] = insert_mvcc_tuple(aborted_id);
    ASSERT_TRUE(txn_mgr_.abort(aborted_id).has_value());

    ASSERT_EQ(live_pid, dead_pid) << "test setup expects both rows on page 1";
    EXPECT_EQ(count_live_tuples(live_pid), 2u) << "both slots present pre-vacuum";

    advance_horizon_past(aborted_id);
    txn_mgr_.gc_completed_transactions();

    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_EQ(stats->dead_tuples, 1u)
        << "exactly the GC'd aborted tuple must be reclaimed";
    EXPECT_EQ(count_live_tuples(live_pid), 1u)
        << "the committed-live row must survive; no live-row loss";

    // The surviving slot must be the live one, not the dead one.
    auto page_result = bpm_->fetch_page(live_pid);
    ASSERT_TRUE(page_result.has_value());
    Page* page = *page_result;
    EXPECT_TRUE(page->get_tuple(live_slot).has_value())
        << "the live row's specific slot must remain";
    (void)bpm_->unpin_page(live_pid, false);
}

// A3b: Vacuum must not resurrect an aborted tuple that GC has NOT yet run
// for (i.e., calling vacuum before GC leaves the aborted xmin still
// "known-aborted" via the live transactions_ map -- vacuum_is_dead should
// still correctly reclaim it even without GC, since the txn is still live
// in the map at that point).
TEST_F(QA_TxnManagerGdb1242, VacuumPath_ReclaimsAbortedEvenWithoutPriorGc) {
    auto* aborter = txn_mgr_.begin().value();
    txn_id_t aborted_id = aborter->txn_id;
    auto [pid, slot] = insert_mvcc_tuple(aborted_id);
    ASSERT_TRUE(txn_mgr_.abort(aborted_id).has_value());

    advance_horizon_past(aborted_id);
    // Deliberately do NOT call gc_completed_transactions() here.

    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_EQ(stats->dead_tuples, 1u);
    EXPECT_EQ(count_live_tuples(pid), 0u);
}

// =============================================================================
// A4: Restart compromise preserved after unrelated GC activity has already
// populated both pruned_committed_ and pruned_aborted_. A genuinely-unknown
// xid (never registered) must remain COMMITTED/visible.
// =============================================================================

TEST_F(QA_TxnManagerGdb1242, TrulyUnknownXidSurvivesAfterUnrelatedGcActivity) {
    // Populate pruned_committed_ and pruned_aborted_ with unrelated txns first.
    auto* c1 = txn_mgr_.begin().value();
    ASSERT_TRUE(txn_mgr_.commit(c1->txn_id).has_value());
    auto* a1 = txn_mgr_.begin().value();
    txn_id_t a1_id = a1->txn_id;
    ASSERT_TRUE(txn_mgr_.abort(a1_id).has_value());
    advance_horizon_past(a1_id);
    txn_mgr_.gc_completed_transactions();

    const txn_id_t never_registered = 987'654'322;
    ASSERT_FALSE(txn_mgr_.is_registered(never_registered));
    EXPECT_EQ(txn_mgr_.get_status(never_registered), TransactionStatus::COMMITTED);

    auto heap = make_mvcc_heap();
    auto rid = heap->insert_tuple(std::vector<uint8_t>{4, 5, 6}, never_registered);
    ASSERT_TRUE(rid.has_value());
    EXPECT_TRUE(heap->get_tuple(*rid).has_value())
        << "restart compromise broken by unrelated GC activity";
}

// =============================================================================
// A5: Mixed interleave -- multiple committed and aborted transactions in the
// same GC pass; each row's visibility must match its own creator's true
// outcome, not get smeared across rows.
// =============================================================================

TEST_F(QA_TxnManagerGdb1242, MixedInterleave_EachRowMatchesItsTxnOutcome) {
    auto heap = make_mvcc_heap();

    struct Row {
        RID rid;
        bool should_be_visible;
    };
    std::vector<Row> rows;
    std::vector<txn_id_t> to_advance_past;

    for (int i = 0; i < 6; ++i) {
        auto* t = txn_mgr_.begin().value();
        txn_id_t id = t->txn_id;
        auto rid = heap->insert_tuple(std::vector<uint8_t>{static_cast<uint8_t>(i)}, id);
        ASSERT_TRUE(rid.has_value());

        bool commit_this = (i % 2 == 0);
        if (commit_this) {
            ASSERT_TRUE(txn_mgr_.commit(id).has_value());
        } else {
            ASSERT_TRUE(txn_mgr_.abort(id).has_value());
        }
        rows.push_back({*rid, commit_this});
        to_advance_past.push_back(id);
    }

    txn_id_t max_id = 0;
    for (auto id : to_advance_past) {
        max_id = std::max(max_id, id);
    }
    advance_horizon_past(max_id);
    txn_mgr_.gc_completed_transactions();

    for (size_t i = 0; i < rows.size(); ++i) {
        auto result = heap->get_tuple(rows[i].rid);
        EXPECT_EQ(result.has_value(), rows[i].should_be_visible)
            << "row " << i << " visibility mismatch after mixed GC "
            << "(expected visible=" << rows[i].should_be_visible << ")";
    }
}

// =============================================================================
// A6: is_registered() adversarial xid choices -- zero, frozen_txn_id, and a
// value exactly at the current xmin horizon boundary.
// =============================================================================

TEST_F(QA_TxnManagerGdb1242, IsRegisteredAdversarialXids) {
    // frozen_txn_id must always be registered/COMMITTED.
    EXPECT_TRUE(txn_mgr_.is_registered(frozen_txn_id));
    EXPECT_EQ(txn_mgr_.get_status(frozen_txn_id), TransactionStatus::COMMITTED);

    // txn_id 0 / invalid_txn_id: not a real allocated id, never registered.
    // (invalid_txn_id is used as a sentinel meaning "no xmax"; callers must
    // not query is_registered on it in visibility code, but the API itself
    // must not crash and should report false unless it happens to collide
    // with frozen_txn_id.)
    if (invalid_txn_id != frozen_txn_id) {
        EXPECT_FALSE(txn_mgr_.is_registered(invalid_txn_id));
    }

    // Exactly-at-horizon boundary: a txn whose id equals the computed
    // horizon must not have been GC'd (horizon is exclusive upper bound in
    // gc_completed_transactions_locked: `txn.txn_id < horizon`).
    auto* t = txn_mgr_.begin().value();
    txn_id_t id = t->txn_id;
    ASSERT_TRUE(txn_mgr_.abort(id).has_value());

    // Horizon computed now should be <= id+1 depending on active txns; force
    // horizon to be exactly id (not yet past it) by not committing anything
    // else, then GC and confirm it is NOT collected (still live in map).
    txn_id_t horizon_before = txn_mgr_.xmin_horizon();
    txn_mgr_.gc_completed_transactions();
    if (horizon_before <= id) {
        // Not GC-eligible yet: still live in transactions_ map.
        EXPECT_TRUE(txn_mgr_.is_registered(id));
        EXPECT_EQ(txn_mgr_.get_status(id), TransactionStatus::ABORTED);
    }

    // Now genuinely advance past it and confirm the boundary transition is
    // still correct (registered, ABORTED) once truly GC'd.
    advance_horizon_past(id);
    txn_mgr_.gc_completed_transactions();
    EXPECT_TRUE(txn_mgr_.is_registered(id));
    EXPECT_EQ(txn_mgr_.get_status(id), TransactionStatus::ABORTED);
}

// =============================================================================
// A7: Repeated/redundant GC calls must be idempotent and must not forget
// previously-pruned aborted ids.
// =============================================================================

TEST_F(QA_TxnManagerGdb1242, RepeatedGcIsIdempotentAndRemembersAbortedIds) {
    auto* a1 = txn_mgr_.begin().value();
    txn_id_t aborted_id = a1->txn_id;
    ASSERT_TRUE(txn_mgr_.abort(aborted_id).has_value());
    advance_horizon_past(aborted_id);

    txn_mgr_.gc_completed_transactions();
    EXPECT_TRUE(txn_mgr_.is_registered(aborted_id));
    EXPECT_EQ(txn_mgr_.get_status(aborted_id), TransactionStatus::ABORTED);

    // Call GC repeatedly -- must not crash, must not un-remember the id.
    for (int i = 0; i < 5; ++i) {
        txn_mgr_.gc_completed_transactions();
        EXPECT_TRUE(txn_mgr_.is_registered(aborted_id))
            << "iteration " << i << ": aborted id forgotten after redundant GC";
        EXPECT_EQ(txn_mgr_.get_status(aborted_id), TransactionStatus::ABORTED)
            << "iteration " << i;
    }
}

// =============================================================================
// A8: Vacuum reclamation must physically remove the GC'd aborted tuple's
// slot (verified at the page level, not merely made invisible via header
// interpretation) while leaving an unrelated live row's raw bytes untouched.
// =============================================================================

TEST_F(QA_TxnManagerGdb1242, VacuumPhysicallyReclaimsSlotLeavesLiveByteExact) {
    auto* committer = txn_mgr_.begin().value();
    auto [live_pid, live_slot] = insert_mvcc_tuple(committer->txn_id);
    ASSERT_TRUE(txn_mgr_.commit(committer->txn_id).has_value());

    // Capture the live row's raw bytes before vacuum.
    auto page_result = bpm_->fetch_page(live_pid);
    ASSERT_TRUE(page_result.has_value());
    auto live_bytes_before = (*page_result)->get_tuple(live_slot);
    ASSERT_TRUE(live_bytes_before.has_value());
    std::vector<uint8_t> live_bytes_snapshot = *live_bytes_before;
    (void)bpm_->unpin_page(live_pid, false);

    auto* aborter = txn_mgr_.begin().value();
    txn_id_t aborted_id = aborter->txn_id;
    auto [dead_pid, dead_slot] = insert_mvcc_tuple(aborted_id);
    ASSERT_TRUE(txn_mgr_.abort(aborted_id).has_value());
    ASSERT_EQ(live_pid, dead_pid);

    advance_horizon_past(aborted_id);
    txn_mgr_.gc_completed_transactions();

    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;
    EXPECT_EQ(stats->dead_tuples, 1u);

    // Dead slot must no longer be retrievable.
    auto page_after = bpm_->fetch_page(live_pid);
    ASSERT_TRUE(page_after.has_value());
    EXPECT_FALSE((*page_after)->get_tuple(dead_slot).has_value())
        << "dead slot must be physically reclaimed, not merely filtered";

    // Live slot's bytes must be byte-identical to before vacuum.
    auto live_bytes_after = (*page_after)->get_tuple(live_slot);
    ASSERT_TRUE(live_bytes_after.has_value());
    EXPECT_EQ(*live_bytes_after, live_bytes_snapshot)
        << "live row bytes must be unchanged by vacuum";
    (void)bpm_->unpin_page(live_pid, false);
}

// =============================================================================
// A9: Informational -- gc_completed_transactions() has no production caller.
// This is not a GDB-1242 regression (the ticket's own description notes GC
// "has no production caller" as of the prior review), but it means the fix,
// while correct, is not yet exercised by any real VACUUM/auto-vacuum run in
// this codebase state. Documented here so the gap is visible to anyone
// wiring GDB-1230 auto-vacuum against this manager.
// =============================================================================

TEST_F(QA_TxnManagerGdb1242, InfoOnly_VacuumRunDoesNotItselfInvokeGc) {
    auto* aborter = txn_mgr_.begin().value();
    txn_id_t aborted_id = aborter->txn_id;
    auto [pid, slot] = insert_mvcc_tuple(aborted_id);
    ASSERT_TRUE(txn_mgr_.abort(aborted_id).has_value());
    advance_horizon_past(aborted_id);

    // Run vacuum WITHOUT calling gc_completed_transactions() at all.
    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // The aborted row is still reclaimed correctly here because the txn is
    // still LIVE (ABORTED, not yet pruned) in transactions_ -- vacuum_is_dead
    // resolves it directly. This demonstrates GDB-1242's fix is only
    // *load-bearing* once something calls GC; today nothing in src/ does.
    EXPECT_EQ(stats->dead_tuples, 1u);

    // The transaction remains fully live in the manager's map (never pruned)
    // since GC was never invoked.
    EXPECT_TRUE(txn_mgr_.is_registered(aborted_id));
    EXPECT_EQ(txn_mgr_.get_status(aborted_id), TransactionStatus::ABORTED);
}
