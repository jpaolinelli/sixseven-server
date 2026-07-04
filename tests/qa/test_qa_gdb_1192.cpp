// QA regression / adversarial tests for GDB-1192.
//
// GDB-1192 strengthened VacuumTest.VacuumFullCompactsAllPages (a dev test in
// tests/unit/test_vacuum.cpp) so it actually discriminates a no-op
// Vacuum::run_full() compaction from a real one, by creating genuine page
// fragmentation via an in-place tuple shrink (Page::update_tuple) rather than
// relying on dead-tuple removal.
//
// This is a test-only change (no production code touched). QA focus per the
// audit-lead ticket:
//   1. Does the strengthened test genuinely fail if compact() is a no-op?
//      (fault injection was already done by the implementer and reverted;
//      we independently re-verify the discriminating properties here by
//      exercising Vacuum::run_full()/Page::compact() directly through the
//      public API in adversarial configurations.)
//   2. run_full() on pages with NO fragmentation should report 0 compacted
//      and not crash.
//   3. Multiple fragmented pages should all be compacted and their stats
//      aggregated correctly.
//   4. Interaction between dead MVCC tuples and in-place-shrink holes in the
//      same page.
//   5. Stats fields (pages_compacted, bytes_reclaimed) must be consistent
//      with actual page state (free_space growth).

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

namespace {

class QaVacuumGdb1192Test : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_qa_test_vacuum_gdb1192.db";
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

    /// Insert a tuple with an MVCC header into page 1 (allocating it on
    /// first use). Returns {page_id, slot_id}.
    std::pair<PageId, SlotId> insert_mvcc_tuple(txn_id_t xmin, txn_id_t xmax = 0) {
        MvccTupleHeader header;
        header.xmin = xmin;
        header.xmax = xmax;

        std::vector<uint8_t> user_data(32, 0xAA);
        auto combined = prepend_mvcc_header(header, user_data);

        auto page_result = bpm_->fetch_page(1);
        Page* page = nullptr;
        if (!page_result) {
            auto new_page_result = bpm_->new_page();
            EXPECT_TRUE(new_page_result.has_value());
            if (!new_page_result.has_value()) {
                return {};
            }
            page = *new_page_result;
        } else {
            page = *page_result;
        }

        auto slot_result = page->insert_tuple(combined);
        EXPECT_TRUE(slot_result.has_value());
        if (!slot_result.has_value()) {
            return {};
        }
        auto slot = *slot_result;
        PageId pid = page->page_id();

        auto unpin = bpm_->unpin_page(pid, true);
        (void)unpin;
        return {pid, slot};
    }

    /// Force allocation of a brand-new page (bypassing page-1 reuse), so
    /// multi-page scenarios can be constructed deterministically.
    PageId new_page() {
        auto new_page_result = bpm_->new_page();
        EXPECT_TRUE(new_page_result.has_value());
        PageId pid = (*new_page_result)->page_id();
        auto unpin = bpm_->unpin_page(pid, true);
        (void)unpin;
        return pid;
    }

    std::pair<PageId, SlotId> insert_mvcc_tuple_on_page(PageId pid, txn_id_t xmin, txn_id_t xmax = 0) {
        MvccTupleHeader header;
        header.xmin = xmin;
        header.xmax = xmax;
        std::vector<uint8_t> user_data(32, 0xAA);
        auto combined = prepend_mvcc_header(header, user_data);

        auto page_result = bpm_->fetch_page(pid);
        EXPECT_TRUE(page_result.has_value());
        if (!page_result.has_value()) {
            return {};
        }
        Page* page = *page_result;
        auto slot_result = page->insert_tuple(combined);
        EXPECT_TRUE(slot_result.has_value());
        if (!slot_result.has_value()) {
            return {};
        }
        auto slot = *slot_result;
        auto unpin = bpm_->unpin_page(pid, true);
        (void)unpin;
        return {pid, slot};
    }

    /// Shrink a tuple in-place (keeping the MVCC header, truncating the
    /// user payload), producing a hole that only compact() reclaims.
    void shrink_tuple(PageId pid, SlotId slot, size_t new_total_size) {
        auto page_result = bpm_->fetch_page(pid);
        ASSERT_TRUE(page_result.has_value());
        Page* page = *page_result;
        auto tuple = page->get_tuple(slot);
        ASSERT_TRUE(tuple.has_value());
        ASSERT_GE(tuple->size(), new_total_size);
        std::vector<uint8_t> shrunk(tuple->begin(),
                                    tuple->begin() + static_cast<long>(new_total_size));
        ASSERT_TRUE(page->update_tuple(slot, shrunk).has_value());
        auto unpin = bpm_->unpin_page(pid, true);
        (void)unpin;
    }

    size_t page_free_space(PageId pid) {
        auto page_result = bpm_->fetch_page(pid);
        EXPECT_TRUE(page_result.has_value());
        size_t fs = (*page_result)->free_space();
        auto unpin = bpm_->unpin_page(pid, false);
        (void)unpin;
        return fs;
    }

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

// -----------------------------------------------------------------------------
// 1. Discrimination check: run_full() must not report compaction on a page
//    that has no fragmentation at all (dense-packed, no shrinks, no deletes).
// -----------------------------------------------------------------------------
TEST_F(QaVacuumGdb1192Test, RunFullOnUnfragmentedPageReportsZeroCompacted) {
    auto* t1 = txn_mgr_.begin().value();
    insert_mvcc_tuple(t1->txn_id);
    insert_mvcc_tuple(t1->txn_id);
    insert_mvcc_tuple(t1->txn_id);
    ASSERT_TRUE(txn_mgr_.commit(t1->txn_id).has_value());

    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run_full();
    ASSERT_TRUE(stats.has_value());
    EXPECT_GE(stats->pages_scanned, 1u);
    EXPECT_EQ(stats->dead_tuples, 0u);
    // A tightly packed page has nothing to reclaim: compact() is a true
    // no-op here (free_after == free_before), so pages_compacted/bytes must
    // stay at zero. This is the negative-space complement of the ticket's
    // positive fragmentation case, and it must not crash or false-positive.
    EXPECT_EQ(stats->pages_compacted, 0u);
    EXPECT_EQ(stats->bytes_reclaimed, 0u);
}

// -----------------------------------------------------------------------------
// 2. run_full() on a completely empty table (only the header page, 0 data
//    pages) must not crash and must report zero everything.
// -----------------------------------------------------------------------------
TEST_F(QaVacuumGdb1192Test, RunFullOnEmptyTableIsSafe) {
    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run_full();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->pages_scanned, 0u);
    EXPECT_EQ(stats->pages_compacted, 0u);
    EXPECT_EQ(stats->bytes_reclaimed, 0u);
}

// -----------------------------------------------------------------------------
// 3. Multiple fragmented pages: run_full() must compact every fragmented
//    page and aggregate bytes_reclaimed/pages_compacted across all of them,
//    not just the first.
// -----------------------------------------------------------------------------
TEST_F(QaVacuumGdb1192Test, RunFullCompactsMultipleFragmentedPages) {
    auto* t1 = txn_mgr_.begin().value();

    PageId page_a = new_page();
    PageId page_b = new_page();
    ASSERT_NE(page_a, page_b);

    auto [pa1, sa1] = insert_mvcc_tuple_on_page(page_a, t1->txn_id);
    auto [pa2, sa2] = insert_mvcc_tuple_on_page(page_a, t1->txn_id);
    (void)pa1;
    auto [pb1, sb1] = insert_mvcc_tuple_on_page(page_b, t1->txn_id);
    auto [pb2, sb2] = insert_mvcc_tuple_on_page(page_b, t1->txn_id);
    (void)pb1;
    ASSERT_TRUE(txn_mgr_.commit(t1->txn_id).has_value());

    // Fragment both pages via in-place shrink.
    shrink_tuple(page_a, sa1, mvcc_header_size + 4);
    shrink_tuple(page_b, sb1, mvcc_header_size + 4);
    (void)sa2;
    (void)sb2;

    size_t free_a_before = page_free_space(page_a);
    size_t free_b_before = page_free_space(page_b);

    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run_full();
    ASSERT_TRUE(stats.has_value());

    EXPECT_GE(stats->pages_compacted, 2u) << "expected both fragmented pages to be compacted";
    EXPECT_GT(stats->bytes_reclaimed, 0u);

    size_t free_a_after = page_free_space(page_a);
    size_t free_b_after = page_free_space(page_b);
    EXPECT_GT(free_a_after, free_a_before);
    EXPECT_GT(free_b_after, free_b_before);

    // bytes_reclaimed must be at least the sum of the two pages' individual
    // free-space deltas (it is an aggregate, not a per-page max/last-write).
    size_t delta_a = free_a_after - free_a_before;
    size_t delta_b = free_b_after - free_b_before;
    EXPECT_GE(stats->bytes_reclaimed, delta_a + delta_b);
}

// -----------------------------------------------------------------------------
// 4. Interaction: a page with BOTH a dead MVCC tuple (removed by the
//    dead-tuple pass inside run_full) AND an in-place-shrink hole.
//
// FINDING (documented, not a regression introduced by GDB-1192): vacuum_page()
// (src/txn/vacuum.cpp:120-160) already calls page->compact() internally
// whenever dead_count > 0 (line 153-155), BEFORE Vacuum::run_full() takes its
// own free_before/free_after snapshot around its *separate* page->compact()
// call. Because compact() fully repacks the page (reclaiming both the
// dead-tuple gap and any unrelated shrink-hole fragmentation in the same
// pass), run_full()'s own before/after measurement sees zero delta on a page
// that had dead tuples — even though real space was reclaimed. Net effect:
// whenever a page has >=1 dead tuple, VacuumStats::pages_compacted and
// bytes_reclaimed under-report (silently attribute 0 reclaimed bytes to a
// page that in fact shrank). The true end-to-end reclaim (verified below via
// page->free_space() before/after the whole run_full() call) IS correct; only
// the stats bookkeeping inside run_full is inaccurate for this scenario.
// This is pre-existing behavior in production code (src/txn/vacuum.cpp), not
// caused by the GDB-1192 test-only diff, so no bug ticket is filed against
// GDB-1192 itself; see QA report for the recommendation to file a follow-up
// bug against src/txn/vacuum.cpp.
// -----------------------------------------------------------------------------
TEST_F(QaVacuumGdb1192Test, RunFullStatsUndercountWhenDeadTupleAlreadyCompactedPage) {
    auto* t1 = txn_mgr_.begin().value();
    auto [pid, slot_keep] = insert_mvcc_tuple(t1->txn_id);
    auto [pid2, slot_shrink] = insert_mvcc_tuple(t1->txn_id);
    ASSERT_EQ(pid, pid2);
    ASSERT_TRUE(txn_mgr_.commit(t1->txn_id).has_value());

    // Second txn inserts then aborts a tuple on the same page -> dead tuple
    // (xmin belongs to an aborted transaction, so is_dead() is true).
    auto* t2 = txn_mgr_.begin().value();
    auto [pid3, slot_dead] = insert_mvcc_tuple_on_page(pid, t2->txn_id);
    ASSERT_EQ(pid, pid3);
    ASSERT_TRUE(txn_mgr_.abort(t2->txn_id).has_value());

    // Shrink the "keep" tuple's neighbor in place to add a pure hole too.
    shrink_tuple(pid, slot_shrink, mvcc_header_size + 4);

    size_t free_before = page_free_space(pid);

    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run_full();
    ASSERT_TRUE(stats.has_value());

    // The dead tuple is correctly counted...
    EXPECT_EQ(stats->dead_tuples, 1u);

    // ...and end-to-end space WAS reclaimed (page-level ground truth)...
    size_t free_after = page_free_space(pid);
    EXPECT_GT(free_after, free_before)
        << "run_full must actually reclaim space even though its own "
           "pages_compacted/bytes_reclaimed stats miss it (see comment above)";

    // ...but run_full's own stats do NOT reflect it, because vacuum_page()
    // already compacted the page internally before run_full's measurement
    // window started. This EXPECT (not ASSERT) documents the current,
    // reproducible under-count rather than failing the whole suite; it
    // should be revisited if src/txn/vacuum.cpp is fixed to measure
    // free_space around the entire per-page pass (dead-tuple pass +
    // unconditional compact) rather than just the second compact() call.
    EXPECT_EQ(stats->pages_compacted, 0u)
        << "documents current under-counting behavior (see QA report, GDB-1192)";
    EXPECT_EQ(stats->bytes_reclaimed, 0u)
        << "documents current under-counting behavior (see QA report, GDB-1192)";

    // The kept tuple and the shrunk tuple must both still be live; the
    // aborted tuple must be gone.
    EXPECT_EQ(count_live_tuples(pid), 2u);
    auto dead_check = bpm_->fetch_page(pid);
    ASSERT_TRUE(dead_check.has_value());
    EXPECT_FALSE((*dead_check)->get_tuple(slot_dead).has_value());
    auto unpin = bpm_->unpin_page(pid, false);
    (void)unpin;
}

// -----------------------------------------------------------------------------
// 5. Boundary: shrink a tuple down to the smallest legal size (just the
//    MVCC header, zero-byte user payload isn't allowed by update_tuple's
//    empty-span guard at the Page level, but a 1-byte payload is the
//    smallest allowed) and confirm bytes_reclaimed is still > 0 and never
//    reports a spurious page as compacted when the shrink amount is 0
//    (i.e. re-running run_full on an already-compacted page).
// -----------------------------------------------------------------------------
TEST_F(QaVacuumGdb1192Test, RunFullIsIdempotentAfterFullCompaction) {
    auto* t1 = txn_mgr_.begin().value();
    auto [pid, slot0] = insert_mvcc_tuple(t1->txn_id);
    auto [pid1, slot1] = insert_mvcc_tuple(t1->txn_id);
    (void)pid1;
    ASSERT_TRUE(txn_mgr_.commit(t1->txn_id).has_value());

    shrink_tuple(pid, slot1, mvcc_header_size + 1);
    (void)slot0;

    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats1 = vac.run_full();
    ASSERT_TRUE(stats1.has_value());
    EXPECT_GT(stats1->bytes_reclaimed, 0u);
    EXPECT_GE(stats1->pages_compacted, 1u);

    // Second run_full immediately after: the page is already fully packed,
    // so this pass must be a true no-op (0 bytes, 0 pages) — a buggy
    // compact() that always reports non-zero deltas (e.g. due to stale
    // free_space bookkeeping) would fail this.
    auto stats2 = vac.run_full();
    ASSERT_TRUE(stats2.has_value());
    EXPECT_EQ(stats2->pages_compacted, 0u);
    EXPECT_EQ(stats2->bytes_reclaimed, 0u);
}

} // namespace
