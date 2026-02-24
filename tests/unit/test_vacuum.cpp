#include "giodb/storage/buffer_pool.h"
#include "giodb/storage/disk_manager.h"
#include "giodb/storage/page.h"
#include "giodb/txn/mvcc.h"
#include "giodb/txn/mvcc_tuple.h"
#include "giodb/txn/txn_manager.h"
#include "giodb/txn/vacuum.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <vector>

using namespace giodb;

// =============================================================================
// Test fixture with a temp file, buffer pool, and transaction manager
// =============================================================================

class VacuumTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "giodb_test_vacuum.db";
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

    /// Insert a tuple with MVCC header into the heap.
    /// Returns the page_id and slot_id where it was inserted.
    std::pair<PageId, SlotId> insert_mvcc_tuple(txn_id_t xmin, txn_id_t xmax = 0) {
        MvccTupleHeader header;
        header.xmin = xmin;
        header.xmax = xmax;

        // User data: 32 bytes of filler.
        std::vector<uint8_t> user_data(32, 0xAA);
        auto combined = prepend_mvcc_header(header, user_data);

        // Try to insert into page 1 (allocate if needed).
        auto page_result = bpm_->fetch_page(1);
        Page* page = nullptr;
        if (!page_result) {
            // Allocate a new page.
            auto new_page_result = bpm_->new_page();
            EXPECT_TRUE(new_page_result.has_value());
            page = *new_page_result;
        } else {
            page = *page_result;
        }

        auto slot_result = page->insert_tuple(combined);
        EXPECT_TRUE(slot_result.has_value());
        auto slot = *slot_result;
        PageId pid = page->page_id();

        auto unpin = bpm_->unpin_page(pid, true);
        (void)unpin;
        return {pid, slot};
    }

    /// Count live (non-deleted) tuples on a page.
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
// Basic vacuum tests
// =============================================================================

TEST_F(VacuumTest, VacuumRemovesDeadTuples) {
    // T1 inserts a tuple and commits.
    auto* t1 = txn_mgr_.begin().value();
    auto [pid, slot] = insert_mvcc_tuple(t1->txn_id);
    ASSERT_TRUE(txn_mgr_.commit(t1->txn_id).has_value());

    // T2 "deletes" the tuple by setting xmax, then commits.
    auto* t2 = txn_mgr_.begin().value();
    {
        auto page_result = bpm_->fetch_page(pid);
        ASSERT_TRUE(page_result.has_value());
        Page* page = *page_result;
        auto tuple = page->get_tuple(slot);
        ASSERT_TRUE(tuple.has_value());
        // Write xmax in-place.
        // We need the mutable data pointer — get it via raw().
        auto slot_data = *tuple;
        // The tuple data is inside page raw — compute offset.
        uint8_t* mutable_ptr = page->raw().data() + (slot_data.data() - page->raw().data());
        write_mvcc_xmax(mutable_ptr, t2->txn_id);
        auto unpin = bpm_->unpin_page(pid, true);
        (void)unpin;
    }
    ASSERT_TRUE(txn_mgr_.commit(t2->txn_id).has_value());

    // Verify tuple is still on the page (but dead).
    EXPECT_EQ(count_live_tuples(pid), 1u);

    // Run vacuum — should remove the dead tuple.
    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->dead_tuples, 1u);
    EXPECT_GE(stats->pages_scanned, 1u);

    // Verify tuple is gone from the page.
    EXPECT_EQ(count_live_tuples(pid), 0u);
}

TEST_F(VacuumTest, VacuumDoesNotRemoveLiveTuples) {
    // T1 inserts a tuple and commits (no xmax = live tuple).
    auto* t1 = txn_mgr_.begin().value();
    auto [pid, slot] = insert_mvcc_tuple(t1->txn_id);
    ASSERT_TRUE(txn_mgr_.commit(t1->txn_id).has_value());

    EXPECT_EQ(count_live_tuples(pid), 1u);

    // Run vacuum — should NOT remove the live tuple.
    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->dead_tuples, 0u);

    EXPECT_EQ(count_live_tuples(pid), 1u);
}

TEST_F(VacuumTest, VacuumDoesNotRemoveTuplesVisibleToActiveTransaction) {
    // T1 inserts and commits.
    auto* t1 = txn_mgr_.begin().value();
    auto [pid, slot] = insert_mvcc_tuple(t1->txn_id);
    ASSERT_TRUE(txn_mgr_.commit(t1->txn_id).has_value());

    // T2 is a long-running transaction that needs to see the tuple.
    auto* t2 = txn_mgr_.begin().value();

    // T3 "deletes" the tuple and commits.
    auto* t3 = txn_mgr_.begin().value();
    {
        auto page_result = bpm_->fetch_page(pid);
        ASSERT_TRUE(page_result.has_value());
        Page* page = *page_result;
        auto tuple = page->get_tuple(slot);
        ASSERT_TRUE(tuple.has_value());
        auto slot_data = *tuple;
        uint8_t* mutable_ptr = page->raw().data() + (slot_data.data() - page->raw().data());
        write_mvcc_xmax(mutable_ptr, t3->txn_id);
        auto unpin = bpm_->unpin_page(pid, true);
        (void)unpin;
    }
    ASSERT_TRUE(txn_mgr_.commit(t3->txn_id).has_value());

    // Vacuum should NOT remove the tuple because T2 still needs it.
    // (xmin_horizon is at T2's txn_id, and xmax == t3 >= horizon)
    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->dead_tuples, 0u);
    EXPECT_EQ(count_live_tuples(pid), 1u);

    // After T2 commits, the tuple should become vacuumable.
    ASSERT_TRUE(txn_mgr_.commit(t2->txn_id).has_value());
    auto stats2 = vac.run();
    ASSERT_TRUE(stats2.has_value());
    EXPECT_EQ(stats2->dead_tuples, 1u);
    EXPECT_EQ(count_live_tuples(pid), 0u);
}

TEST_F(VacuumTest, VacuumRemovesAbortedTuples) {
    // T1 inserts a tuple but aborts.
    auto* t1 = txn_mgr_.begin().value();
    auto [pid, slot] = insert_mvcc_tuple(t1->txn_id);
    ASSERT_TRUE(txn_mgr_.abort(t1->txn_id).has_value());

    EXPECT_EQ(count_live_tuples(pid), 1u);

    // Vacuum should remove the aborted tuple.
    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->dead_tuples, 1u);
    EXPECT_EQ(count_live_tuples(pid), 0u);
}

TEST_F(VacuumTest, VacuumFullCompactsAllPages) {
    // Insert several tuples, then delete some.
    auto* t1 = txn_mgr_.begin().value();
    insert_mvcc_tuple(t1->txn_id);
    insert_mvcc_tuple(t1->txn_id);
    insert_mvcc_tuple(t1->txn_id);
    ASSERT_TRUE(txn_mgr_.commit(t1->txn_id).has_value());

    // VACUUM FULL should compact even with no dead tuples.
    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run_full();
    ASSERT_TRUE(stats.has_value());
    EXPECT_GE(stats->pages_scanned, 1u);
    EXPECT_EQ(stats->dead_tuples, 0u);
}

// =============================================================================
// Auto-vacuum tests
// =============================================================================

TEST_F(VacuumTest, AutoVacuumTriggersOnThreshold) {
    // Insert 5 tuples with committed xmin, then "delete" 4 of them.
    auto* t1 = txn_mgr_.begin().value();
    std::vector<std::pair<PageId, SlotId>> rids;
    for (int i = 0; i < 5; ++i) {
        rids.push_back(insert_mvcc_tuple(t1->txn_id));
    }
    ASSERT_TRUE(txn_mgr_.commit(t1->txn_id).has_value());

    // Delete 4 out of 5 tuples.
    auto* t2 = txn_mgr_.begin().value();
    for (int i = 0; i < 4; ++i) {
        auto [pid, slot] = rids[static_cast<size_t>(i)];
        auto page_result = bpm_->fetch_page(pid);
        ASSERT_TRUE(page_result.has_value());
        Page* page = *page_result;
        auto tuple = page->get_tuple(slot);
        ASSERT_TRUE(tuple.has_value());
        auto slot_data = *tuple;
        uint8_t* mutable_ptr = page->raw().data() + (slot_data.data() - page->raw().data());
        write_mvcc_xmax(mutable_ptr, t2->txn_id);
        auto unpin = bpm_->unpin_page(pid, true);
        (void)unpin;
    }
    ASSERT_TRUE(txn_mgr_.commit(t2->txn_id).has_value());

    // Set up auto-vacuum with 20% threshold (4/1 = 400% dead ratio > 20%).
    AutoVacuumWorker worker(txn_mgr_);
    worker.add_table(*bpm_, dm_, file_id_);
    AutoVacuumConfig config;
    config.dead_tuple_threshold = 0.20;
    worker.set_config(config);

    // Manually trigger a check cycle.
    auto results = worker.check_and_vacuum();
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 1u);
    EXPECT_EQ((*results)[0].dead_tuples, 4u);
}

TEST_F(VacuumTest, AutoVacuumDoesNotTriggerBelowThreshold) {
    // Insert 10 tuples, delete 1 (10% dead ratio, below 20% threshold).
    auto* t1 = txn_mgr_.begin().value();
    std::vector<std::pair<PageId, SlotId>> rids;
    for (int i = 0; i < 10; ++i) {
        rids.push_back(insert_mvcc_tuple(t1->txn_id));
    }
    ASSERT_TRUE(txn_mgr_.commit(t1->txn_id).has_value());

    auto* t2 = txn_mgr_.begin().value();
    {
        auto [pid, slot] = rids[0];
        auto page_result = bpm_->fetch_page(pid);
        ASSERT_TRUE(page_result.has_value());
        Page* page = *page_result;
        auto tuple = page->get_tuple(slot);
        ASSERT_TRUE(tuple.has_value());
        auto slot_data = *tuple;
        uint8_t* mutable_ptr = page->raw().data() + (slot_data.data() - page->raw().data());
        write_mvcc_xmax(mutable_ptr, t2->txn_id);
        auto unpin = bpm_->unpin_page(pid, true);
        (void)unpin;
    }
    ASSERT_TRUE(txn_mgr_.commit(t2->txn_id).has_value());

    // Dead ratio: 1/9 ≈ 11%, below 20% threshold.
    AutoVacuumWorker worker(txn_mgr_);
    worker.add_table(*bpm_, dm_, file_id_);

    auto results = worker.check_and_vacuum();
    ASSERT_TRUE(results.has_value());
    EXPECT_EQ(results->size(), 0u); // No vacuum triggered.
}

TEST_F(VacuumTest, AutoVacuumConfigurable) {
    AutoVacuumWorker worker(txn_mgr_);

    AutoVacuumConfig config;
    config.enabled = false;
    config.dead_tuple_threshold = 0.50;
    config.check_interval = std::chrono::seconds(120);
    worker.set_config(config);

    auto read_config = worker.config();
    EXPECT_FALSE(read_config.enabled);
    EXPECT_DOUBLE_EQ(read_config.dead_tuple_threshold, 0.50);
    EXPECT_EQ(read_config.check_interval, std::chrono::seconds(120));
}

TEST_F(VacuumTest, AutoVacuumDisabledNoOp) {
    // Insert and delete tuples, but disable auto-vacuum.
    auto* t1 = txn_mgr_.begin().value();
    insert_mvcc_tuple(t1->txn_id);
    ASSERT_TRUE(txn_mgr_.commit(t1->txn_id).has_value());

    auto* t2 = txn_mgr_.begin().value();
    insert_mvcc_tuple(t2->txn_id, t2->txn_id); // xmin=t2, xmax=t2 → dead.
    ASSERT_TRUE(txn_mgr_.abort(t2->txn_id).has_value());

    AutoVacuumWorker worker(txn_mgr_);
    worker.add_table(*bpm_, dm_, file_id_);
    AutoVacuumConfig config;
    config.enabled = false;
    worker.set_config(config);

    auto results = worker.check_and_vacuum();
    ASSERT_TRUE(results.has_value());
    EXPECT_EQ(results->size(), 0u);
}

TEST_F(VacuumTest, AutoVacuumWorkerStartStop) {
    AutoVacuumWorker worker(txn_mgr_);
    AutoVacuumConfig config;
    config.check_interval = std::chrono::seconds(1);
    worker.set_config(config);

    EXPECT_FALSE(worker.running());
    ASSERT_TRUE(worker.start().has_value());
    EXPECT_TRUE(worker.running());

    // Double start should fail.
    auto result = worker.start();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::ALREADY_EXISTS);

    worker.stop();
    EXPECT_FALSE(worker.running());
}

// =============================================================================
// xmin_horizon tracking tests (integration with vacuum)
// =============================================================================

TEST_F(VacuumTest, XminHorizonTrackedCorrectly) {
    // Verify that xmin_horizon correctly prevents premature vacuuming.
    auto* t1 = txn_mgr_.begin().value();
    auto [pid, slot] = insert_mvcc_tuple(t1->txn_id);
    ASSERT_TRUE(txn_mgr_.commit(t1->txn_id).has_value());

    // T2 deletes the tuple and commits.
    auto* t2 = txn_mgr_.begin().value();
    {
        auto page_result = bpm_->fetch_page(pid);
        ASSERT_TRUE(page_result.has_value());
        Page* page = *page_result;
        auto tuple = page->get_tuple(slot);
        ASSERT_TRUE(tuple.has_value());
        auto slot_data = *tuple;
        uint8_t* mutable_ptr = page->raw().data() + (slot_data.data() - page->raw().data());
        write_mvcc_xmax(mutable_ptr, t2->txn_id);
        auto unpin = bpm_->unpin_page(pid, true);
        (void)unpin;
    }
    ASSERT_TRUE(txn_mgr_.commit(t2->txn_id).has_value());

    // With no active transactions, horizon should be high enough to vacuum.
    auto horizon = txn_mgr_.xmin_horizon();
    EXPECT_GT(horizon, t2->txn_id);

    Vacuum vac(*bpm_, dm_, file_id_, txn_mgr_);
    auto stats = vac.run();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->dead_tuples, 1u);
}
