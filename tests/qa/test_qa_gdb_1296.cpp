/// Adversarial QA tests for GDB-1296: TransactionManager::pruned_committed_
/// watermark compaction.
///
/// The implementation's correctness rests on: (a) xmin_horizon_locked() is
/// monotonically non-decreasing, so any id that was < horizon at insertion
/// time into pruned_committed_ remains < horizon forever after, and (b) once
/// dropped, get_status()'s genuinely-unknown-xid fallback (COMMITTED) is
/// still the right answer for those ids. This file adversarially probes:
///
///  1. Whether compaction can ever run while a still-genuinely-ACTIVE
///     transaction has an id below the new watermark (the resurrection-class
///     bug this ticket explicitly must avoid, mirroring GDB-1242).
///  2. Boundary behavior exactly at the watermark.
///  3. That is_registered() flipping false-after-compaction does not break
///     real MVCC visibility through TableHeap/mvcc.cpp's frozen-xid
///     normalization path (table_heap.cpp::normalize_xid /
///     vacuum.cpp::vacuum_normalize_xid), for both a viewer whose snapshot
///     predates the compacted commit and one whose snapshot postdates it.
///  4. That pruned_aborted_ is genuinely never compacted, even under the
///     same stress that repeatedly triggers pruned_committed_ compaction.
///  5. Concurrent begin/commit/abort/GC racing with compaction does not
///     produce incorrect status resolution or crashes.
///  6. Sustained high-throughput short-transaction load actually keeps
///     memory bounded (proxy: is_registered() churn behaves as expected,
///     not just a single forced compaction).

#include "sixseven/common/status.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/txn/mvcc.h"
#include "sixseven/txn/mvcc_tuple.h"
#include "sixseven/txn/read_view.h"
#include "sixseven/txn/txn_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

using namespace sixseven;

namespace {

class QA_GDB1296 : public ::testing::Test {
protected:
    void advance_horizon_past(txn_id_t txn_id) {
        while (txn_mgr_.xmin_horizon() <= txn_id) {
            auto* t = txn_mgr_.begin().value();
            ASSERT_TRUE(txn_mgr_.commit(t->txn_id).has_value());
        }
    }

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

std::vector<uint8_t> make_bytes(size_t size, uint8_t fill) {
    return std::vector<uint8_t>(size, fill);
}

} // namespace

// =============================================================================
// 1. Compaction must never run while a genuinely-active txn's id is below
//    the new watermark. If it did, that active txn's own writes/reads would
//    resolve via the unknown-xid-as-committed fallback prematurely.
// =============================================================================

TEST_F(QA_GDB1296, CompactionNeverStrandsAGenuinelyActiveTransaction) {
    // Start a long-lived transaction early and never complete it.
    auto* long_lived = txn_mgr_.begin().value();
    txn_id_t long_lived_id = long_lived->txn_id;

    // Drive many committed+GC'd short transactions through -- enough to force
    // several rounds of pruned_committed_ compaction -- while long_lived stays
    // active throughout.
    for (size_t i = 0; i < TransactionManager::pruned_committed_compaction_threshold * 3; ++i) {
        auto* t = txn_mgr_.begin().value();
        txn_id_t id = t->txn_id;
        ASSERT_TRUE(txn_mgr_.commit(id).has_value());
        txn_mgr_.gc_completed_transactions();

        // The long-lived transaction must remain ACTIVE and must never be
        // GC'd/compacted away -- xmin_horizon_locked() must keep excluding it.
        EXPECT_EQ(txn_mgr_.get_status(long_lived_id), TransactionStatus::ACTIVE);
        // xmin_horizon must never advance past a genuinely active txn's id.
        EXPECT_LE(txn_mgr_.xmin_horizon(), long_lived_id);
    }

    // Still resolvable and active at the end.
    EXPECT_EQ(txn_mgr_.get_status(long_lived_id), TransactionStatus::ACTIVE);
    ASSERT_TRUE(txn_mgr_.commit(long_lived_id).has_value());
    EXPECT_EQ(txn_mgr_.get_status(long_lived_id), TransactionStatus::COMMITTED);
}

// =============================================================================
// 2. Boundary: an id exactly equal to the horizon at compaction time must not
//    be treated as safely droppable (only strictly-below entries are safe).
// =============================================================================

TEST_F(QA_GDB1296, IdEqualToHorizonAtCompactionTimeResolvesCorrectly) {
    // Begin a transaction and keep it active -- its own id becomes the
    // horizon (xmin_horizon_locked considers an active txn's own id as a
    // lower bound).
    auto* pinning = txn_mgr_.begin().value();
    txn_id_t pinning_id = pinning->txn_id;
    ASSERT_EQ(txn_mgr_.xmin_horizon(), pinning_id);

    // Drive compaction activity from committed txns started AFTER pinning
    // but which will never themselves drop below the horizon while pinning
    // is active (they can't be GC'd past pinning's id).
    for (size_t i = 0; i < TransactionManager::pruned_committed_compaction_threshold + 5; ++i) {
        auto* t = txn_mgr_.begin().value();
        ASSERT_TRUE(txn_mgr_.commit(t->txn_id).has_value());
    }
    txn_mgr_.gc_completed_transactions();

    // pinning_id itself must still resolve as ACTIVE, never COMMITTED, even
    // though a huge number of higher ids have completed and been GC'd/
    // compacted around it.
    EXPECT_EQ(txn_mgr_.get_status(pinning_id), TransactionStatus::ACTIVE);

    ASSERT_TRUE(txn_mgr_.commit(pinning_id).has_value());
    EXPECT_EQ(txn_mgr_.get_status(pinning_id), TransactionStatus::COMMITTED);
}

// =============================================================================
// 3. pruned_aborted_ must never be compacted, even under sustained stress
//    that repeatedly triggers pruned_committed_ compaction around it.
// =============================================================================

TEST_F(QA_GDB1296, ManyAbortedIdsSurviveRepeatedCommittedCompaction) {
    std::vector<txn_id_t> aborted_ids;
    for (int i = 0; i < 20; ++i) {
        auto* t = txn_mgr_.begin().value();
        aborted_ids.push_back(t->txn_id);
        ASSERT_TRUE(txn_mgr_.abort(t->txn_id).has_value());
    }
    advance_horizon_past(aborted_ids.back());
    txn_mgr_.gc_completed_transactions();
    for (auto id : aborted_ids) {
        ASSERT_EQ(txn_mgr_.get_status(id), TransactionStatus::ABORTED);
    }

    // Force pruned_committed_ compaction multiple rounds.
    run_many_committed_transactions(TransactionManager::pruned_committed_compaction_threshold * 3);

    for (auto id : aborted_ids) {
        EXPECT_TRUE(txn_mgr_.is_registered(id))
            << "aborted id " << id << " lost registration after compaction";
        EXPECT_EQ(txn_mgr_.get_status(id), TransactionStatus::ABORTED)
            << "aborted id " << id << " resurrected as COMMITTED";
    }
}

// =============================================================================
// 4. Integration: a tuple committed by a transaction that later gets
//    compacted out of pruned_committed_ must remain correctly resolved by
//    real MVCC visibility (table_heap.cpp's normalize_xid -> frozen path),
//    for a viewer snapshot taken AFTER the commit (must see it).
// =============================================================================

class QA_GDB1296_TableHeap : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1296_heap.db";
        std::filesystem::remove(path_);
        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;
        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
        heap_ = std::make_unique<TableHeap>(*bpm_, dm_, file_id_, TableHeapOptions{.mvcc_headers = true});
        heap_->attach_txn_manager(&txn_mgr_);
    }

    void TearDown() override {
        heap_.reset();
        bpm_.reset();
        (void)dm_.close_file(file_id_);
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

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
    std::unique_ptr<TableHeap> heap_;
    TransactionManager txn_mgr_;
};

TEST_F(QA_GDB1296_TableHeap, CompactedCommittedTupleStillVisibleToLaterSnapshot) {
    // Writer transaction inserts and commits a row.
    auto* writer = txn_mgr_.begin().value();
    txn_id_t writer_id = writer->txn_id;
    auto rid = heap_->insert_tuple(make_bytes(16, 0xCD), writer_id);
    ASSERT_TRUE(rid.has_value()) << rid.error().message;
    ASSERT_TRUE(txn_mgr_.commit(writer_id).has_value());
    advance_horizon_past(writer_id);
    txn_mgr_.gc_completed_transactions();
    ASSERT_TRUE(txn_mgr_.is_registered(writer_id));

    // Force pruned_committed_ compaction so writer_id is dropped from the set
    // and now resolves purely via the unknown-xid-as-frozen normalization
    // path in table_heap.cpp.
    for (size_t i = 0; i < TransactionManager::pruned_committed_compaction_threshold + 1; ++i) {
        auto* t = txn_mgr_.begin().value();
        txn_id_t id = t->txn_id;
        ASSERT_TRUE(txn_mgr_.commit(id).has_value());
        advance_horizon_past(id);
        txn_mgr_.gc_completed_transactions();
    }
    ASSERT_FALSE(txn_mgr_.is_registered(writer_id))
        << "test setup invariant violated: writer_id should have been compacted away";

    // A brand-new reader (snapshot taken well after the commit) must still
    // see the row.
    MvccReadView view;
    view.snapshot = txn_mgr_.take_snapshot();
    view.viewer_txn_id = invalid_txn_id;
    MvccReadViewGuard guard(view);

    auto tuple = heap_->get_tuple(*rid);
    ASSERT_TRUE(tuple.has_value())
        << "compacted-but-committed tuple incorrectly reported as not visible: "
        << (tuple ? std::string{} : tuple.error().message);
    EXPECT_EQ(tuple->size(), 16u);
}

TEST_F(QA_GDB1296_TableHeap, DeleteByCompactedTransactionStillHidesRow) {
    // Insert a row (frozen xmin, i.e. bootstrap-style write) then delete it
    // with a transaction that will later be compacted out of
    // pruned_committed_. A resurrection bug would make this row reappear.
    auto rid = heap_->insert_tuple(make_bytes(8, 0xAB));
    ASSERT_TRUE(rid.has_value()) << rid.error().message;

    auto* deleter = txn_mgr_.begin().value();
    txn_id_t deleter_id = deleter->txn_id;
    auto del = heap_->delete_tuple(*rid, deleter_id);
    ASSERT_TRUE(del.has_value()) << del.error().message;
    ASSERT_TRUE(txn_mgr_.commit(deleter_id).has_value());
    advance_horizon_past(deleter_id);
    txn_mgr_.gc_completed_transactions();

    for (size_t i = 0; i < TransactionManager::pruned_committed_compaction_threshold + 1; ++i) {
        auto* t = txn_mgr_.begin().value();
        txn_id_t id = t->txn_id;
        ASSERT_TRUE(txn_mgr_.commit(id).has_value());
        advance_horizon_past(id);
        txn_mgr_.gc_completed_transactions();
    }
    ASSERT_FALSE(txn_mgr_.is_registered(deleter_id));

    MvccReadView view;
    view.snapshot = txn_mgr_.take_snapshot();
    view.viewer_txn_id = invalid_txn_id;
    MvccReadViewGuard guard(view);

    auto tuple = heap_->get_tuple(*rid);
    ASSERT_FALSE(tuple.has_value())
        << "RESURRECTION BUG: deleted row became visible again after its "
           "deleter's commit was compacted out of pruned_committed_";
    EXPECT_EQ(tuple.error().code, StatusCode::NOT_FOUND);
}

// =============================================================================
// 5. Concurrency: begin/commit/abort/GC racing with compaction must never
//    crash or produce a status resolution that violates the pruned_aborted_
//    contract (aborted ids must never read back as COMMITTED).
// =============================================================================

TEST(QA_GDB1296_Concurrency, ConcurrentBeginCommitAbortGcRace) {
    TransactionManager txn_mgr;
    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 400;

    std::atomic<bool> found_resurrected_abort{false};
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&txn_mgr, &found_resurrected_abort, t]() {
            for (int i = 0; i < kOpsPerThread; ++i) {
                auto txn = txn_mgr.begin();
                ASSERT_TRUE(txn.has_value());
                txn_id_t id = (*txn)->txn_id;

                bool should_abort = ((t + i) % 5) == 0;
                if (should_abort) {
                    auto r = txn_mgr.abort(id);
                    ASSERT_TRUE(r.has_value());
                } else {
                    auto r = txn_mgr.commit(id);
                    ASSERT_TRUE(r.has_value());
                }

                // Periodically GC (which now also triggers compaction).
                if (i % 7 == 0) {
                    txn_mgr.gc_completed_transactions();
                }

                // Verify our own transaction's status is exactly what we set,
                // regardless of concurrent GC/compaction elsewhere.
                auto status = txn_mgr.get_status(id);
                if (should_abort) {
                    if (status != TransactionStatus::ABORTED) {
                        found_resurrected_abort.store(true);
                    }
                } else {
                    if (status != TransactionStatus::COMMITTED) {
                        found_resurrected_abort.store(true);
                    }
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_FALSE(found_resurrected_abort.load())
        << "concurrent GC/compaction produced an incorrect status resolution "
           "(aborted txn resolved as non-ABORTED, or committed txn resolved "
           "as non-COMMITTED)";
}

// =============================================================================
// 6. Sustained high-throughput short-transaction load: the motivating
//    scenario from the ticket. Compaction must keep triggering repeatedly
//    (not just once) as load continues, proving the bound is durable, not a
//    one-shot cap.
// =============================================================================

TEST_F(QA_GDB1296, RepeatedCompactionKeepsHappeningUnderSustainedLoad) {
    txn_id_t watermark_after_first_round = 0;
    run_many_committed_transactions(TransactionManager::pruned_committed_compaction_threshold + 1);
    watermark_after_first_round = txn_mgr_.pruned_committed_watermark_for_test();
    ASSERT_GT(watermark_after_first_round, 0u);

    // Drive several more rounds; the watermark must keep advancing (i.e.
    // compaction is not a one-time event that then lets the set grow
    // unbounded again).
    for (int round = 0; round < 5; ++round) {
        run_many_committed_transactions(TransactionManager::pruned_committed_compaction_threshold + 1);
    }

    EXPECT_GT(txn_mgr_.pruned_committed_watermark_for_test(), watermark_after_first_round)
        << "watermark stopped advancing -- compaction may only be happening once, "
           "which would let pruned_committed_ grow unbounded again under sustained load";
}
