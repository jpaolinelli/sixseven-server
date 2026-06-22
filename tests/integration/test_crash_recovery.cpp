/// Integration test: WAL crash recovery wired into server startup (GDB-900).
///
/// Tests the full startup decision path using the component-level API
/// (TableHeap + WalWriter + WalRecovery + CleanShutdownMarker), which is
/// exactly what main.cpp uses but without a full server process.
///
/// Crash scenario: no clean-shutdown marker => recovery runs => committed
///   data present on recovered heap (byte-identical raw images).
/// Clean scenario: clean-shutdown marker written => next startup skips recovery.
///
/// Mutation grade: commenting out the WalRecovery::recover() call in
/// run_startup_recovery() causes the committed-data raw-image checks to fail.

#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/clean_shutdown_marker.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_recovery.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/table_wal.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <vector>

using namespace sixseven;
namespace fs = std::filesystem;

// =============================================================================
// Helpers
// =============================================================================

namespace {

constexpr uint32_t kTableId = 7;

std::vector<uint8_t> make_payload(size_t size, uint8_t fill) {
    return std::vector<uint8_t>(size, fill);
}

/// Mirrors the startup decision logic in main.cpp (GDB-900).
/// Returns true if recovery ran, false if clean path (skipped).
/// MUTATION GRADE: removing the recover() call causes TEST 1 to fail.
bool run_startup_recovery(const fs::path& data_dir, uint32_t table_id, TableHeap* recovery_heap) {
    fs::path wal_dir = data_dir / "wal";
    CleanShutdownMarker marker(data_dir);

    if (marker.exists()) {
        (void)marker.remove();
        return false; // clean-path: recovery skipped
    }

    // Crash path.
    TableHeapRecoveryHandler handler;
    handler.register_table(table_id, recovery_heap);
    WalRecovery wal_recovery(wal_dir, handler);
    auto result = wal_recovery.recover();
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
    return true;
}

/// Fetch the raw on-page bytes for a slot (including MVCC header), or empty
/// if the slot is dead. Used to verify byte-identical recovery.
std::vector<uint8_t> raw_slot(BufferPoolManager& bpm, RID rid) {
    auto page = bpm.fetch_page(rid.page_id);
    if (!page) {
        return {};
    }
    auto raw = (*page)->get_tuple(rid.slot_id);
    (void)bpm.unpin_page(rid.page_id, false);
    return raw ? std::move(*raw) : std::vector<uint8_t>{};
}

} // namespace

// =============================================================================
// Fixture
// =============================================================================

class CrashRecoveryIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = fs::temp_directory_path() / "sixseven_test_crash_recovery";
        wal_dir_ = data_dir_ / "wal";
        fs::remove_all(data_dir_);
        fs::create_directories(data_dir_);
        fs::create_directories(wal_dir_);
    }

    void TearDown() override {
        // Destroy smart ptrs in reverse construction order to flush and close
        // file handles before remove_all (Windows blocks removal of open files).
        recovery_heap_.reset();
        recovery_bpm_.reset();
        primary_heap_.reset();
        primary_bpm_.reset();
        if (wal_) {
            (void)wal_->close();
            wal_.reset();
        }
        std::error_code ec;
        fs::remove_all(data_dir_, ec);
    }

    /// Open a new primary heap backed by a WAL writer.
    void open_primary() {
        auto fid = dm_.create_file(data_dir_ / "primary.db", false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        primary_bpm_ = std::make_unique<BufferPoolManager>(dm_, *fid, 64);
        primary_heap_ = std::make_unique<TableHeap>(
            *primary_bpm_, dm_, *fid, TableHeapOptions{.mvcc_headers = true});

        WalWriterOptions opts;
        opts.enable_group_commit = false;
        wal_ = std::make_unique<WalWriter>(wal_dir_, opts);
        ASSERT_TRUE(wal_->open().has_value());
        primary_heap_->attach_wal(wal_.get(), kTableId);
    }

    /// Open a brand-new empty recovery heap (recovery target).
    void open_recovery_heap() {
        auto fid = dm_.create_file(data_dir_ / "recovered.db", false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        recovery_bpm_ = std::make_unique<BufferPoolManager>(dm_, *fid, 64);
        recovery_heap_ = std::make_unique<TableHeap>(
            *recovery_bpm_, dm_, *fid, TableHeapOptions{.mvcc_headers = true});
    }

    /// Flush WAL + pages without writing a clean-shutdown marker (simulates crash).
    void simulate_crash() {
        ASSERT_TRUE(wal_->flush().has_value());
        ASSERT_TRUE(wal_->close().has_value());
        wal_.reset();
        primary_heap_->attach_wal(nullptr, 0);
        ASSERT_TRUE(primary_bpm_->flush_all().has_value());
        // No CleanShutdownMarker::write() — intentional crash simulation.
    }

    DiskManager dm_;
    fs::path data_dir_;
    fs::path wal_dir_;
    std::unique_ptr<BufferPoolManager> primary_bpm_;
    std::unique_ptr<TableHeap> primary_heap_;
    std::unique_ptr<WalWriter> wal_;
    std::unique_ptr<BufferPoolManager> recovery_bpm_;
    std::unique_ptr<TableHeap> recovery_heap_;
};

// =============================================================================
// TEST 1 - Crash scenario: recovery reproduces committed data byte-identically.
//
// Inserts committed tuples through the primary heap (which writes WAL records
// with proper MVCC headers automatically), then simulates a crash, opens a
// fresh recovery heap, and verifies that WalRecovery::recover() reproduces
// the exact same raw on-page bytes as the primary.
//
// MUTATION GRADE: comment out the WalRecovery::recover() call in
// run_startup_recovery() and every raw_slot check fails (empty vs non-empty).
// =============================================================================

TEST_F(CrashRecoveryIntegrationTest, CommittedDataPresentAfterCrashRecovery) {
    // Phase 1: insert committed tuples via primary heap (WAL records generated).
    std::vector<RID> rids;
    {
        open_primary();
        for (uint8_t i = 1; i <= 3; ++i) {
            auto rid = primary_heap_->insert_tuple(make_payload(64, i));
            ASSERT_TRUE(rid.has_value()) << rid.error().message;
            rids.push_back(*rid);
        }
        simulate_crash();
    }
    ASSERT_EQ(rids.size(), 3u);
    ASSERT_FALSE(CleanShutdownMarker(data_dir_).exists());

    // Phase 2: recover into a fresh heap.
    open_recovery_heap();
    bool ran = run_startup_recovery(data_dir_, kTableId, recovery_heap_.get());
    EXPECT_TRUE(ran) << "Expected recovery to run (no clean-shutdown marker present)";

    // Redo must have written byte-identical raw images for all committed RIDs.
    for (const auto& rid : rids) {
        auto primary_raw = raw_slot(*primary_bpm_, rid);
        ASSERT_FALSE(primary_raw.empty()) << "primary slot unexpectedly empty";

        auto recovery_raw = raw_slot(*recovery_bpm_, rid);
        ASSERT_FALSE(recovery_raw.empty())
            << "recovered slot empty — recovery redo did not replay the committed insert";
        EXPECT_EQ(recovery_raw, primary_raw)
            << "byte-image mismatch for RID{" << rid.page_id << "," << rid.slot_id << "}";
    }
}

// =============================================================================
// TEST 2 - Clean shutdown path: marker present => recovery skipped.
// =============================================================================

TEST_F(CrashRecoveryIntegrationTest, CleanShutdownSkipsRecovery) {
    // Phase 1: insert + clean shutdown (write marker).
    RID rid;
    {
        open_primary();
        auto r = primary_heap_->insert_tuple(make_payload(16, 0x42));
        ASSERT_TRUE(r.has_value()) << r.error().message;
        rid = *r;
        ASSERT_TRUE(wal_->flush().has_value());
        ASSERT_TRUE(wal_->close().has_value());
        wal_.reset();
        ASSERT_TRUE(primary_bpm_->flush_all().has_value());

        // Write the clean-shutdown marker (mimics what main.cpp does).
        CleanShutdownMarker marker(data_dir_);
        ASSERT_TRUE(marker.write().has_value());
        ASSERT_TRUE(marker.exists());
    }

    // Phase 2: startup should skip recovery.
    open_recovery_heap();
    bool ran = run_startup_recovery(data_dir_, kTableId, recovery_heap_.get());
    EXPECT_FALSE(ran) << "Recovery should have been skipped (clean-shutdown marker present)";

    // Marker must have been consumed (deleted).
    EXPECT_FALSE(CleanShutdownMarker(data_dir_).exists());

    // Recovery heap must be empty (no redo ran).
    auto raw = raw_slot(*recovery_bpm_, rid);
    EXPECT_TRUE(raw.empty()) << "Recovery heap must stay empty when recovery is skipped";
}

// =============================================================================
// TEST 3 - Recovery on empty WAL directory succeeds with zero stats.
// =============================================================================

TEST_F(CrashRecoveryIntegrationTest, RecoveryOnEmptyWalDirectorySucceeds) {
    open_recovery_heap();

    TableHeapRecoveryHandler handler;
    handler.register_table(kTableId, recovery_heap_.get());
    WalRecovery wal_recovery(wal_dir_, handler);
    auto result = wal_recovery.recover();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->records_scanned, 0u);
    EXPECT_EQ(result->records_redone, 0u);
    EXPECT_EQ(result->records_undone, 0u);
}

// =============================================================================
// TEST 4 - Recovery is idempotent: running recover() twice does not fail.
// =============================================================================

TEST_F(CrashRecoveryIntegrationTest, RecoveryIsIdempotent) {
    // Write committed WAL records via primary heap.
    {
        open_primary();
        auto rid = primary_heap_->insert_tuple(make_payload(24, 0x77));
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
        simulate_crash();
    }

    open_recovery_heap();

    // First recovery run.
    {
        TableHeapRecoveryHandler handler;
        handler.register_table(kTableId, recovery_heap_.get());
        WalRecovery r(wal_dir_, handler);
        ASSERT_TRUE(r.recover().has_value());
    }

    // Second recovery run on the same heap must succeed (idempotent).
    {
        TableHeapRecoveryHandler handler;
        handler.register_table(kTableId, recovery_heap_.get());
        WalRecovery r(wal_dir_, handler);
        auto result = r.recover();
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }
}
