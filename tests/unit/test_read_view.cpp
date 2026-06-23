/// Unit tests for the per-statement MVCC read view (GDB-777): the
/// thread-local MvccReadViewGuard and the snapshot-based visibility filtering
/// it activates in TableHeap reads (get_tuple / TableIterator).

#include "sixseven/common/status.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/txn/mvcc_tuple.h"
#include "sixseven/txn/read_view.h"
#include "sixseven/txn/txn_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

using namespace sixseven;

// =============================================================================
// Guard semantics
// =============================================================================

TEST(MvccReadViewGuard, NoViewInstalledByDefault) {
    EXPECT_EQ(current_mvcc_read_view(), nullptr);
}

TEST(MvccReadViewGuard, InstallsAndClearsView) {
    TransactionManager txn_mgr;
    {
        MvccReadViewGuard guard(MvccReadView{txn_mgr.take_snapshot(), invalid_txn_id});
        const MvccReadView* view = current_mvcc_read_view();
        ASSERT_NE(view, nullptr);
        EXPECT_EQ(view->viewer_txn_id, invalid_txn_id);
    }
    EXPECT_EQ(current_mvcc_read_view(), nullptr);
}

TEST(MvccReadViewGuard, NestedGuardsSaveAndRestore) {
    TransactionManager txn_mgr;
    auto* txn = txn_mgr.begin().value();

    MvccReadViewGuard outer(MvccReadView{txn->snapshot, txn->txn_id});
    ASSERT_NE(current_mvcc_read_view(), nullptr);
    EXPECT_EQ(current_mvcc_read_view()->viewer_txn_id, txn->txn_id);
    {
        MvccReadViewGuard inner(MvccReadView{txn_mgr.take_snapshot(), invalid_txn_id});
        ASSERT_NE(current_mvcc_read_view(), nullptr);
        EXPECT_EQ(current_mvcc_read_view()->viewer_txn_id, invalid_txn_id);
    }
    ASSERT_NE(current_mvcc_read_view(), nullptr);
    EXPECT_EQ(current_mvcc_read_view()->viewer_txn_id, txn->txn_id);
}

// =============================================================================
// TableHeap filtering under a read view
// =============================================================================

class ReadViewHeapTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_test_read_view.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;
        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
        heap_ = std::make_unique<TableHeap>(
            *bpm_, dm_, file_id_, TableHeapOptions{.mvcc_headers = true});
        heap_->attach_txn_manager(&txn_mgr_);
    }

    void TearDown() override {
        heap_.reset();
        bpm_.reset();
        (void)dm_.close_file(file_id_);
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    RID insert(uint8_t fill, txn_id_t xmin) {
        std::vector<uint8_t> data(32, fill);
        auto rid = heap_->insert_tuple(data, xmin);
        EXPECT_TRUE(rid.has_value());
        return rid ? *rid : RID::invalid();
    }

    size_t count_scanned() {
        auto it = heap_->begin();
        EXPECT_TRUE(it.has_value());
        size_t n = 0;
        for (;;) {
            auto r = it->next();
            EXPECT_TRUE(r.has_value());
            if (!r || !r->has_value()) {
                break;
            }
            ++n;
        }
        return n;
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<TableHeap> heap_;
    TransactionManager txn_mgr_;
};

TEST_F(ReadViewHeapTest, UncommittedInsertInvisibleToOtherViewer) {
    auto* writer = txn_mgr_.begin().value();
    RID rid = insert(0xAA, writer->txn_id);

    // Outside viewer with a fresh snapshot: invisible.
    MvccReadViewGuard guard(MvccReadView{txn_mgr_.take_snapshot(), invalid_txn_id});
    EXPECT_EQ(count_scanned(), 0u);
    auto fetched = heap_->get_tuple(rid);
    ASSERT_FALSE(fetched.has_value());
    EXPECT_EQ(fetched.error().code, StatusCode::NOT_FOUND);
}

TEST_F(ReadViewHeapTest, UncommittedInsertVisibleToSelf) {
    auto* writer = txn_mgr_.begin().value();
    RID rid = insert(0xAB, writer->txn_id);

    MvccReadViewGuard guard(
        MvccReadView{txn_mgr_.get_statement_snapshot(writer->txn_id), writer->txn_id});
    EXPECT_EQ(count_scanned(), 1u);
    EXPECT_TRUE(heap_->get_tuple(rid).has_value());
}

TEST_F(ReadViewHeapTest, CommittedInsertVisibleToFreshSnapshot) {
    auto* writer = txn_mgr_.begin().value();
    insert(0xAC, writer->txn_id);
    ASSERT_TRUE(txn_mgr_.commit(writer->txn_id).has_value());

    MvccReadViewGuard guard(MvccReadView{txn_mgr_.take_snapshot(), invalid_txn_id});
    EXPECT_EQ(count_scanned(), 1u);
}

TEST_F(ReadViewHeapTest, AbortedInsertNeverVisible) {
    auto* writer = txn_mgr_.begin().value();
    insert(0xAD, writer->txn_id);
    ASSERT_TRUE(txn_mgr_.abort(writer->txn_id).has_value());

    MvccReadViewGuard guard(MvccReadView{txn_mgr_.take_snapshot(), invalid_txn_id});
    EXPECT_EQ(count_scanned(), 0u);
}

TEST_F(ReadViewHeapTest, UncommittedDeleteStillVisibleToOthersInvisibleToSelf) {
    insert(0xAE, frozen_txn_id); // Committed row.

    auto* deleter = txn_mgr_.begin().value();
    // Locate the row and stamp xmax with the deleter's txn id.
    auto it = heap_->begin();
    ASSERT_TRUE(it.has_value());
    auto row = it->next();
    ASSERT_TRUE(row.has_value()) << row.error().message;
    ASSERT_TRUE(row->has_value());
    ASSERT_TRUE(heap_->mark_deleted((*row)->first, deleter->txn_id).has_value());

    {
        // Another viewer still sees the row (deleter has not committed).
        MvccReadViewGuard guard(MvccReadView{txn_mgr_.take_snapshot(), invalid_txn_id});
        EXPECT_EQ(count_scanned(), 1u);
    }
    {
        // The deleter itself no longer sees the row.
        MvccReadViewGuard guard(
            MvccReadView{txn_mgr_.get_statement_snapshot(deleter->txn_id), deleter->txn_id});
        EXPECT_EQ(count_scanned(), 0u);
    }

    ASSERT_TRUE(txn_mgr_.commit(deleter->txn_id).has_value());
    {
        // After commit, a fresh snapshot no longer sees the row.
        MvccReadViewGuard guard(MvccReadView{txn_mgr_.take_snapshot(), invalid_txn_id});
        EXPECT_EQ(count_scanned(), 0u);
    }
}

TEST_F(ReadViewHeapTest, NoReadViewFallsBackToStatusFiltering) {
    // Without an installed read view the GDB-747 status-based behavior is
    // preserved: uncommitted (active) inserts are visible.
    auto* writer = txn_mgr_.begin().value();
    insert(0xAF, writer->txn_id);
    EXPECT_EQ(count_scanned(), 1u);
}

TEST_F(ReadViewHeapTest, UnknownXminTreatedAsCommittedUnderReadView) {
    // Rows persisted by a previous process carry txn ids unknown to this
    // manager; they must stay readable under a read view (restart durability).
    insert(0xB0, /*xmin=*/987654);

    MvccReadViewGuard guard(MvccReadView{txn_mgr_.take_snapshot(), invalid_txn_id});
    EXPECT_EQ(count_scanned(), 1u);
}
