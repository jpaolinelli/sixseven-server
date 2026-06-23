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
///
/// TEST 6 exercises the GDB-1276 fix: StorageManager::set_wal_writer()
/// propagates the writer to all managed heaps.  Without that wiring, every
/// heap has wal_==nullptr, no WAL records are produced by DML, and recovery
/// sees records_redone==0 even after a crash.

#include "sixseven/catalog/schema.h"
#include "sixseven/executor/storage_manager.h"
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
// TEST 3 - Uncommitted data is rolled back (undo phase) after a crash.
//
// Writes a BEGIN record for a real (non-frozen) txn_id, inserts tuples tagged
// with that txn_id so their WAL records carry the in-flight id, then simulates
// a crash WITHOUT writing COMMIT or ABORT.  Recovery must classify the txn as
// aborted/in-progress, undo each INSERT (removing the slot), and report
// records_undone > 0.  The undo phase (Phase 3, wal_recovery.cpp ~lines
// 233-243) is the code path exercised; without it this test fails because the
// inserted tuples remain visible in the recovery heap.
//
// MUTATION GRADE: commenting out the undo loop in WalRecovery::recover()
// causes the raw_slot checks below to return non-empty instead of empty,
// failing the EXPECT_TRUE assertions.  Commenting out the active_txns
// insertion into aborted_txns causes records_undone to stay 0.
// =============================================================================

TEST_F(CrashRecoveryIntegrationTest, UncommittedDataRolledBackAfterCrashRecovery) {
    // Choose a real txn_id that is not frozen_txn_id and not invalid_txn_id.
    // The value just needs to be distinct from frozen_txn_id (~0ULL).
    constexpr txn_id_t kInflightTxnId = 42;

    // Phase 1: open the primary heap and write an in-flight transaction.
    std::vector<RID> rids;
    {
        open_primary();

        // Write a BEGIN record for the in-flight txn so the analysis phase
        // adds it to active_txns (and therefore aborted_txns at crash time).
        WalRecord begin_rec;
        begin_rec.type = WalRecordType::BEGIN;
        begin_rec.txn_id = kInflightTxnId;
        auto begin_lsn = wal_->append(begin_rec);
        ASSERT_TRUE(begin_lsn.has_value()) << begin_lsn.error().message;

        // Insert tuples stamped with the in-flight txn_id.  insert_tuple()
        // passes xmin as the WAL record's txn_id, so recovery sees these
        // INSERT records as belonging to kInflightTxnId.
        for (uint8_t i = 1; i <= 3; ++i) {
            auto rid = primary_heap_->insert_tuple(make_payload(64, i), kInflightTxnId);
            ASSERT_TRUE(rid.has_value()) << rid.error().message;
            rids.push_back(*rid);
        }

        // Crash: flush WAL and pages without writing COMMIT or ABORT, and
        // without writing a clean-shutdown marker.
        simulate_crash();
    }
    ASSERT_EQ(rids.size(), 3u);
    ASSERT_FALSE(CleanShutdownMarker(data_dir_).exists());

    // Phase 2: run recovery into a fresh empty heap.
    open_recovery_heap();

    TableHeapRecoveryHandler handler;
    handler.register_table(kTableId, recovery_heap_.get());
    WalRecovery wal_recovery(wal_dir_, handler);
    auto result = wal_recovery.recover();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // The undo phase must have run and processed all three INSERT records.
    EXPECT_GT(result->records_undone, 0u)
        << "records_undone=" << result->records_undone
        << " — undo phase did not run; in-flight txn was not classified as aborted";
    EXPECT_EQ(result->records_undone, 3u) << "expected 3 INSERTs undone (one per in-flight tuple)";

    // The aborted/in-progress count must include our txn.
    EXPECT_GE(result->aborted_txns, 1u);
    EXPECT_TRUE(result->aborted_txn_ids.count(kInflightTxnId) > 0)
        << "txn " << kInflightTxnId << " not found in aborted_txn_ids";

    // The undo handler removes INSERT slots, so every RID written by the
    // in-flight txn must be absent from the recovered heap.
    for (const auto& rid : rids) {
        auto raw = raw_slot(*recovery_bpm_, rid);
        EXPECT_TRUE(raw.empty())
            << "RID{" << rid.page_id << "," << rid.slot_id
            << "} still present after undo — uncommitted tuple was not rolled back";
    }
}

// =============================================================================
// TEST 4 (was 3) - Recovery on empty WAL directory succeeds with zero stats.
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
// TEST 5 (was 4) - Recovery is idempotent: running recover() twice does not fail.
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

// =============================================================================
// TEST 6 — StorageManager::set_wal_writer() propagates to managed heaps so
//          DML via the StorageManager produces real WAL records (GDB-1276).
//
// This test mirrors the main.cpp wiring: a WalWriter is attached to a
// StorageManager, which in turn attaches it to every heap it manages via
// attach_wal().  A committed INSERT is then run through the managed heap
// (exactly as the QueryEngine's InsertOperator does).  After a simulated
// crash, WalRecovery::recover() is run against the same WAL directory and
// must report records_redone > 0.
//
// MUTATION GRADE: removing the storage.set_wal_writer(&wal_writer) call
// from main.cpp (or from StorageManager::create_table_storage) leaves
// wal_==nullptr on every heap, so no WAL records are written and
// records_redone stays 0 — causing EXPECT_GT below to fail.
// =============================================================================

TEST_F(CrashRecoveryIntegrationTest, StorageManagerPropagatesWalWriterToHeaps) {
    constexpr table_id_t kManagedTableId = 42;
    constexpr database_id_t kDbId = 1;

    // Build a minimal TableSchema with one INT32 column so StorageManager
    // can construct its storage_schema (used by create_table_storage).
    sixseven::TableSchema ts;
    ts.table_id = kManagedTableId;
    ts.name = "test_table";
    sixseven::CatalogColumnDef col;
    col.ordinal = 0;
    col.name = "id";
    col.type_id = sixseven::TypeId::INT32;
    col.nullable = false;
    ts.columns.push_back(col);

    // Phase 1: DML through StorageManager with WAL writer attached.
    std::vector<sixseven::RID> rids;
    {
        sixseven::DiskManager dm;
        sixseven::StorageManager storage(dm, data_dir_, 64);

        // Create database directories so create_table_storage can place its file.
        ASSERT_TRUE(storage.create_database_storage(kDbId).has_value());
        ASSERT_TRUE(storage.create_table_storage(kDbId, kManagedTableId, ts).has_value());

        // Open the WAL writer — same wal_dir_ used by recovery below.
        sixseven::WalWriterOptions opts;
        opts.enable_group_commit = false; // Synchronous for test determinism.
        sixseven::WalWriter wal_writer(wal_dir_, opts);
        ASSERT_TRUE(wal_writer.open().has_value());

        // Wire it — this is the production path from main.cpp.
        storage.set_wal_writer(&wal_writer);

        // Insert tuples through the managed heap.  The heap now has wal_ != null,
        // so insert_tuple() writes WAL records (BEGIN + INSERT + ... emitted by
        // the heap's write_wal_record helper).
        auto ts_result = storage.get_table_storage(kManagedTableId);
        ASSERT_TRUE(ts_result.has_value());
        auto* heap = ts_result.value()->heap.get();

        for (uint8_t i = 1; i <= 4; ++i) {
            auto rid = heap->insert_tuple(make_payload(32, i));
            ASSERT_TRUE(rid.has_value()) << rid.error().message;
            rids.push_back(*rid);
        }

        // Simulate crash: flush WAL, flush pages, no clean-shutdown marker.
        ASSERT_TRUE(wal_writer.flush().has_value());
        ASSERT_TRUE(wal_writer.close().has_value());
        // Detach writer before storage destructor so no further WAL writes.
        storage.set_wal_writer(nullptr);
        // No CleanShutdownMarker::write() — intentional crash simulation.
    }
    ASSERT_EQ(rids.size(), 4u);

    // Phase 2: recovery via the same wal_dir_ path.
    // Open a fresh recovery heap (empty) into which recovery will redo the inserts.
    open_recovery_heap();

    sixseven::TableHeapRecoveryHandler handler;
    handler.register_table(kManagedTableId, recovery_heap_.get());
    sixseven::WalRecovery wal_recovery(wal_dir_, handler);
    auto stats = wal_recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // The critical assertion: WAL records must have been produced by the
    // managed-heap DML and recovered here.  If set_wal_writer() were not
    // wired, records_redone would be 0.
    EXPECT_GT(stats->records_redone, 0u)
        << "records_redone=0 — set_wal_writer() did not propagate writer to the "
           "managed heap; no WAL records were produced by INSERT";
    EXPECT_EQ(stats->records_redone, rids.size())
        << "expected one redo record per committed INSERT";
}
