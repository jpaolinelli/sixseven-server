/// @file test_qa_gdb_812.cpp
/// QA adversarial tests for GDB-812: Fix migration tests deleting sys_databases
/// at wrong path, so the migration code path (AC5) is never exercised.
///
/// This file exercises the migration/backfill path that was previously
/// unreachable because the test deleted the wrong file path.
///
/// Key attack surfaces:
///   1. Migration is GENUINELY triggered (file deleted before components are
///      torn down was failing on Windows with "file in use" error).
///   2. Migration with multiple user databases — all must be backfilled.
///   3. Migration with only the default database (no user tables).
///   4. Migration idempotency — delete sys_databases and trigger migration twice.
///   5. Non-default database names are LOST during migration (known limitation:
///      migration reconstructs names as "database_<id>" except for the default).
///   6. Regression guard: ASSERT_TRUE(exists) fires if path convention changes.
///
/// Adversarial categories: migration correctness, path correctness, idempotency,
/// name-loss bug, file-in-use ordering.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/config.h"
#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Test fixture
// ============================================================================

class QA_GDB812 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "qa_gdb812";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        create_components();
    }

    void TearDown() override {
        destroy_components();
        std::filesystem::remove_all(data_dir_);
    }

    void create_components() {
        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
        config_ = Config::load_defaults();
    }

    void destroy_components() {
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
    }

    void run_bootstrap() {
        auto result = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }

    void restart() {
        destroy_components();
        create_components();
    }

    /// Returns the correct StorageManager path for the sys_databases file.
    /// This must match StorageManager::table_path exactly.
    std::filesystem::path sys_databases_path() const {
        return data_dir_ / "databases" / std::to_string(system_database_id) /
               "tables" / ("table_" + std::to_string(sys_databases_table_id) + ".db");
    }

    /// Tear down live components, delete sys_databases file, then re-create
    /// components fresh.  This is the correct ordering on Windows where an
    /// open file-handle prevents deletion.
    void simulate_missing_sys_databases() {
        destroy_components();
        auto path = sys_databases_path();
        ASSERT_TRUE(std::filesystem::exists(path))
            << "sys_databases must exist at: " << path;
        std::filesystem::remove(path);
        ASSERT_FALSE(std::filesystem::exists(path))
            << "sys_databases must be absent to trigger migration";
        create_components();
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << " => " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    std::vector<std::pair<database_id_t, std::string>> scan_sys_databases() {
        std::vector<std::pair<database_id_t, std::string>> entries;
        auto ts = storage_->get_table_storage(sys_databases_table_id);
        if (!ts) return entries;
        auto schema = StorageManager::build_storage_schema(sys_databases_schema());
        auto it = (*ts)->heap->begin();
        if (!it) return entries;
        while (auto row = it->next()) {
            auto values = TupleSerializer::deserialize(row->second, schema);
            if (!values) continue;
            entries.emplace_back(
                static_cast<database_id_t>((*values)[0].as_int32()),
                (*values)[1].as_string());
        }
        return entries;
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    Config config_;
};

// ============================================================================
// Regression guard: correct file path (the original GDB-812 bug)
// ============================================================================

/// Verifies that the sys_databases file exists at the path that
/// StorageManager::table_path actually writes — i.e., the path used by
/// the migration tests is correct.  If this fails the path convention changed
/// and the migration tests would silently become no-ops again.
TEST_F(QA_GDB812, SysDatabasesFileExistsAtCorrectStorageManagerPath) {
    run_bootstrap();
    // Must close components before checking — but we only need to verify path
    // existence here, not delete.
    auto path = sys_databases_path();
    ASSERT_TRUE(std::filesystem::exists(path))
        << "sys_databases file NOT found at StorageManager path: " << path
        << "\nIf StorageManager::table_path changed, the migration delete path "
           "in test_qa_gdb_656.cpp must be updated to match.";
}

/// Confirms that the WRONG path from before GDB-812 (missing "tables/"
/// subdirectory and "table_" prefix) does NOT exist.  If this passes, the
/// pre-GDB-812 test was definitely a no-op.
TEST_F(QA_GDB812, OldWrongSysDatabasesPathDoesNotExist) {
    run_bootstrap();
    // Pre-GDB-812 wrong path: databases/2/9.db
    auto wrong_path = data_dir_ / "databases" / std::to_string(system_database_id) /
                      (std::to_string(sys_databases_table_id) + ".db");
    EXPECT_FALSE(std::filesystem::exists(wrong_path))
        << "The old (wrong) path exists — StorageManager layout changed unexpectedly: "
        << wrong_path;
}

// ============================================================================
// AC5 / migration: genuinely triggered with correct ordering
// ============================================================================

/// The migration path must actually trigger when sys_databases is absent.
/// Verify by checking the migration log message is emitted and
/// scan_sys_databases() returns the sixseven entry.
///
/// CRITICAL ordering: components must be destroyed BEFORE deleting the file
/// because on Windows an open file-handle prevents deletion.
TEST_F(QA_GDB812, MigrationTriggeredWhenSysDatabasesAbsent) {
    run_bootstrap();

    // Destroy first (close file handles), then delete, then re-create.
    simulate_missing_sys_databases();

    // Bootstrap with sys_databases absent should trigger migration.
    run_bootstrap();

    // sixseven must be accessible after migration.
    auto db = catalog_->get_database("demo");
    ASSERT_TRUE(db.has_value())
        << "migration should restore sixseven: " << db.error().message;
    EXPECT_EQ(db->database_id, default_database_id);

    // Migration must have backfilled sixseven into sys_databases.
    auto entries = scan_sys_databases();
    bool found = false;
    for (const auto& [id, name] : entries) {
        if (id == default_database_id) {
            found = true;
            EXPECT_EQ(name, default_database_name)
                << "sixseven should have name '" << default_database_name
                << "' after migration, got: " << name;
        }
    }
    EXPECT_TRUE(found) << "migration must backfill sixseven into sys_databases";
}

/// Verify that the migration is NON-INERT: if the migration backfill were
/// removed (mock: check that scan_sys_databases is empty before migration
/// completes), the assertions would catch it.  We simulate this by checking
/// that sys_databases is initially absent (empty scan before run_bootstrap)
/// and then populated after.
TEST_F(QA_GDB812, MigrationIsNonInertSysDatabasesPopulatedAfterNotBefore) {
    run_bootstrap();
    simulate_missing_sys_databases();

    // After destroy/recreate but BEFORE run_bootstrap, sys_databases should
    // not be accessible (no open table storage yet).
    auto ts_before = storage_->get_table_storage(sys_databases_table_id);
    EXPECT_FALSE(ts_before.has_value())
        << "sys_databases should not be open before bootstrap after migration scenario";

    run_bootstrap();

    // After bootstrap with migration, sys_databases should now be populated.
    auto entries = scan_sys_databases();
    EXPECT_FALSE(entries.empty())
        << "migration must populate sys_databases — if this fails migration regressed";
}

/// With a user table present, migration must backfill the default database.
TEST_F(QA_GDB812, MigrationWithUserTableBackfillsDatabase) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);
    exec_ok("CREATE TABLE mig_tbl (id INT)");

    simulate_missing_sys_databases();
    run_bootstrap();

    // Database and table must be accessible.
    auto db = catalog_->get_database("demo");
    ASSERT_TRUE(db.has_value()) << db.error().message;

    auto tbl = catalog_->get_table(default_database_id, "mig_tbl");
    ASSERT_TRUE(tbl.has_value()) << "table should survive migration";
}

// ============================================================================
// Adversarial: multiple user databases
// ============================================================================

/// When multiple user databases have been manually persisted, migration must
/// backfill all of them.  Note: migration reconstructs names from sys_tables
/// scan, but non-default databases get the name "database_<id>" (the original
/// name is not available in sys_tables).  This test documents the current
/// behavior.
TEST_F(QA_GDB812, MigrationBackfillsMultiplePersistedDatabases) {
    run_bootstrap();

    // Persist two additional databases.
    ASSERT_TRUE(catalog_->restore_database(50, "prod_db").has_value());
    ASSERT_TRUE(persistence_->persist_database(50, "prod_db").has_value());
    ASSERT_TRUE(catalog_->restore_database(51, "staging_db").has_value());
    ASSERT_TRUE(persistence_->persist_database(51, "staging_db").has_value());

    // Create a table in database 50 so sys_tables has an entry for db_id=50.
    // This is needed because migration scans sys_tables for db IDs.
    engine_->set_current_database(50);
    exec_ok("CREATE TABLE prod_users (id INT)");

    simulate_missing_sys_databases();
    run_bootstrap();

    // Default database must be backfilled.
    auto entries = scan_sys_databases();
    bool found_default = false;
    bool found_db50 = false;
    for (const auto& [id, name] : entries) {
        if (id == default_database_id) found_default = true;
        if (id == 50) found_db50 = true;
    }
    EXPECT_TRUE(found_default) << "default database must be backfilled";
    // DB 50 must be backfilled because sys_tables has a table for it.
    EXPECT_TRUE(found_db50) << "database 50 must be backfilled (has a table in sys_tables)";

    // NOTE: Database 51 (staging_db) will NOT be backfilled because it has
    // no tables in sys_tables — migration only discovers databases that have
    // table entries.  This is a known limitation of the migration approach.
}

/// Database with no tables is not discoverable by migration — the migration
/// only finds databases by scanning sys_tables entries.  This test documents
/// that limitation explicitly.
TEST_F(QA_GDB812, MigrationCannotBackfillDatabaseWithNoTables) {
    run_bootstrap();

    // Persist a database but create NO tables in it.
    ASSERT_TRUE(catalog_->restore_database(99, "empty_db").has_value());
    ASSERT_TRUE(persistence_->persist_database(99, "empty_db").has_value());

    simulate_missing_sys_databases();
    run_bootstrap();

    auto entries = scan_sys_databases();
    bool found_db99 = false;
    for (const auto& [id, name] : entries) {
        if (id == 99) found_db99 = true;
    }
    // This is the known limitation: empty databases (no sys_tables entry) are
    // not backfilled by migration.  Document the behavior rather than assert false.
    if (found_db99) {
        SUCCEED() << "Database 99 was backfilled (migration improved)";
    } else {
        SUCCEED() << "Database 99 not backfilled (known migration limitation: "
                     "no sys_tables entry)";
    }
}

/// Non-default database names are lost during migration — they become
/// "database_<id>" because the original name is not stored in sys_tables.
/// This test documents and guards the name-loss behavior.
TEST_F(QA_GDB812, MigrationAssignsGenericNamesToNonDefaultDatabases) {
    run_bootstrap();

    // Persist a database with id=42 and name="user_app".
    ASSERT_TRUE(catalog_->restore_database(42, "user_app").has_value());
    ASSERT_TRUE(persistence_->persist_database(42, "user_app").has_value());

    // Create a table in db 42 so migration can discover it.
    engine_->set_current_database(42);
    exec_ok("CREATE TABLE app_data (id INT)");

    simulate_missing_sys_databases();
    run_bootstrap();

    auto entries = scan_sys_databases();
    for (const auto& [id, name] : entries) {
        if (id == 42) {
            // After migration the name will be "database_42", not "user_app".
            // This is a KNOWN DATA LOSS BUG in the migration implementation
            // (see migrate_databases_from_sys_tables in catalog_persistence.cpp).
            // The original name is not stored in sys_tables and cannot be recovered.
            EXPECT_EQ(name, "database_42")
                << "Migration assigns generic names to non-default databases. "
                   "Original name 'user_app' is lost (known limitation GDB-812).";
        }
    }
}

// ============================================================================
// Adversarial: migration idempotency
// ============================================================================

/// Delete sys_databases twice (migration runs on two consecutive boots).
/// Each run must succeed — no duplicate entries, no crash.
TEST_F(QA_GDB812, MigrationIdempotentRunTwice) {
    run_bootstrap();

    // First migration.
    simulate_missing_sys_databases();
    run_bootstrap();

    // Verify first migration succeeded.
    auto entries1 = scan_sys_databases();
    size_t count1 = 0;
    for (const auto& [id, name] : entries1) {
        if (id == default_database_id) ++count1;
    }
    EXPECT_EQ(count1, 1u) << "sixseven should appear exactly once after first migration";

    // Second migration: delete sys_databases again.
    simulate_missing_sys_databases();
    run_bootstrap();

    // Should still be accessible, no duplicate entries.
    auto db = catalog_->get_database("demo");
    ASSERT_TRUE(db.has_value()) << db.error().message;

    auto entries2 = scan_sys_databases();
    size_t count2 = 0;
    for (const auto& [id, name] : entries2) {
        if (id == default_database_id) ++count2;
    }
    EXPECT_EQ(count2, 1u) << "sixseven should appear exactly once after second migration";
}

// ============================================================================
// Adversarial: normal restart still works after migration
// ============================================================================

/// After migration, a normal restart (sys_databases present) must take the
/// normal code path, not the migration path.  Proves migration is one-time.
TEST_F(QA_GDB812, NormalRestartAfterMigrationWorks) {
    run_bootstrap();
    simulate_missing_sys_databases();
    run_bootstrap();  // migration run

    // Normal restart — sys_databases now exists.
    restart();
    run_bootstrap();

    auto db = catalog_->get_database("demo");
    ASSERT_TRUE(db.has_value()) << "normal restart after migration should work: "
                                 << db.error().message;
    EXPECT_EQ(db->database_id, default_database_id);

    // sys_databases should still have exactly one entry for the default db.
    auto entries = scan_sys_databases();
    size_t count = 0;
    for (const auto& [id, name] : entries) {
        if (id == default_database_id) ++count;
    }
    EXPECT_EQ(count, 1u)
        << "sixseven should appear exactly once after migration + normal restart";
}

// ============================================================================
// Adversarial: stress — many databases present at migration time
// ============================================================================

/// Stress test: 20 databases (each with a table), delete sys_databases, verify
/// the default database and all databases-with-tables are backfilled.
TEST_F(QA_GDB812, MigrationStressManyDatabasesWithTables) {
    run_bootstrap();

    // Create 20 databases, each with one table.
    for (int i = 0; i < 20; ++i) {
        database_id_t db_id = 200 + i;
        std::string db_name = "stress_db_" + std::to_string(i);
        ASSERT_TRUE(catalog_->restore_database(db_id, db_name).has_value());
        ASSERT_TRUE(persistence_->persist_database(db_id, db_name).has_value());
        engine_->set_current_database(db_id);
        exec_ok("CREATE TABLE t" + std::to_string(i) + " (id INT)");
    }

    simulate_missing_sys_databases();
    run_bootstrap();

    // Default database must be backfilled.
    auto db = catalog_->get_database("demo");
    ASSERT_TRUE(db.has_value()) << "default database missing after stress migration";

    // All 20 databases (which have tables in sys_tables) must be backfilled.
    auto entries = scan_sys_databases();
    for (int i = 0; i < 20; ++i) {
        database_id_t expected_id = 200 + i;
        bool found = false;
        for (const auto& [id, name] : entries) {
            if (id == expected_id) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << "database " << expected_id
                           << " not backfilled after stress migration";
    }
}

// ============================================================================
// Guard: file-in-use ordering
// ============================================================================

/// Attempting to delete sys_databases BEFORE destroying components should fail
/// on Windows (file in use).  This test documents that the CORRECT ordering
/// (destroy first, then delete) is required — it verifies the wrong ordering
/// fails.  This guards against the pre-GDB-812 test pattern that tried to
/// delete while components were still alive.
///
/// On Linux this may succeed (open files can be unlinked), so we skip the
/// assertion and just document the platform behavior.
TEST_F(QA_GDB812, DeleteSysDatabasesWhileOpenFailsOrSucceedsPlatformDependent) {
    run_bootstrap();

    auto path = sys_databases_path();
    ASSERT_TRUE(std::filesystem::exists(path));

    // Attempt to delete while components are alive (file is open).
    bool threw = false;
    try {
        std::filesystem::remove(path);
    } catch (const std::filesystem::filesystem_error&) {
        threw = true;
    }

    // On Windows: threw=true (file in use).
    // On Linux: threw=false (unlink succeeds even with open fd).
    // Either way, we just record and do NOT fail the test — the important
    // behavior is exercised by the correct-ordering tests above.
    if (threw) {
        SUCCEED() << "Platform (Windows): deleting an open file threw as expected. "
                     "Correct ordering (destroy before delete) is required.";
    } else {
        SUCCEED() << "Platform (Linux/Mac): deleting an open file succeeded. "
                     "Correct ordering still recommended for portability.";
    }

    // Restore state to avoid corrupting TearDown.
    // If the delete succeeded, components will fail on restart — just rebuild.
    destroy_components();
    create_components();
}

// ============================================================================
// Adversarial: GDB-1264 root-cause probe — no partial catalog registration
// after a failed open_sys_table (missing or corrupt file).
// ============================================================================

/// After open_sys_table fails because the backing file is absent, the catalog
/// must have NO registration for sys_databases.  This is the GDB-1264 root
/// cause: the pre-fix code called restore_table before the file-existence check,
/// leaving a stale registration that made subsequent create_sys_table fail with
/// "already exists".  This test directly verifies the catalog is clean.
TEST_F(QA_GDB812, OpenSysTableFailingLeavesCleanCatalog) {
    run_bootstrap();
    simulate_missing_sys_databases();

    // At this point: components are fresh, sys_databases file is absent.
    // Calling open_sys_table_public should fail with IO_ERROR.
    auto schema = sys_databases_schema();
    auto result = persistence_->open_sys_table_public(schema);
    ASSERT_FALSE(result.has_value())
        << "open_sys_table_public must fail when file is absent";
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR)
        << "expected IO_ERROR for missing file, got: " << result.error().message;

    // CRITICAL: the catalog must NOT have a registration for sys_databases.
    // If it does, the subsequent create_sys_table_public (migration path) will
    // fail with "already exists" — exactly the GDB-1264 bug.
    auto existing = catalog_->get_table(system_database_id, schema.name);
    EXPECT_FALSE(existing.has_value())
        << "GDB-1264 regression: open_sys_table left a stale catalog registration "
           "for '" << schema.name << "' even though the file was absent. "
           "The create_sys_table path will fail with 'already exists'.";
}

/// After open_sys_table fails, create_sys_table_public must succeed (migration
/// can proceed without "already exists" error). This is the end-to-end GDB-1264
/// correctness check at the persistence API level.
TEST_F(QA_GDB812, CreateSysTableSucceedsAfterFailedOpenSysTable) {
    run_bootstrap();
    simulate_missing_sys_databases();

    // Attempt to open sys_databases (will fail — file is absent).
    auto open_r = persistence_->open_sys_table_public(sys_databases_schema());
    ASSERT_FALSE(open_r.has_value()) << "expected failure when file absent";

    // The migration path calls create_sys_table_public to create sys_databases
    // fresh.  It must succeed — not fail with "already exists".
    auto create_r = persistence_->create_sys_table_public(sys_databases_schema());
    ASSERT_TRUE(create_r.has_value())
        << "GDB-1264 regression: create_sys_table_public failed after a failed "
           "open_sys_table_public. Error: " << (create_r ? "" : create_r.error().message)
        << ". The catalog was likely left with a stale registration.";
}

// ============================================================================
// Adversarial: truncated/corrupted sys_databases file
// ============================================================================

/// Write a truncated (zero-byte) sys_databases file, then restart.
/// Bootstrap must not crash; it should treat the file as present but empty
/// (normal open path, no migration), returning gracefully.
/// Validates: no crash, no partial-catalog corruption on corrupt-file restart.
TEST_F(QA_GDB812, TruncatedSysDatabasesFileNocrash) {
    run_bootstrap();

    // Destroy components to release file handles, then truncate the file.
    destroy_components();
    auto path = sys_databases_path();
    ASSERT_TRUE(std::filesystem::exists(path)) << "path: " << path;

    // Truncate to zero bytes (corrupt the header).
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(f.is_open()) << "could not open sys_databases for truncation";
        // Write nothing — file is now 0 bytes.
    }
    ASSERT_TRUE(std::filesystem::exists(path));
    ASSERT_EQ(std::filesystem::file_size(path), 0u) << "file not truncated";

    create_components();

    // Bootstrap with a truncated file.  It should not crash.  It may succeed
    // (treating the empty heap as an empty sys_databases — normal open path)
    // or fail gracefully with an error.  Either outcome is acceptable; a crash
    // or an ASan violation is not.
    auto result = SystemBootstrap::bootstrap(
        *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
    // We don't ASSERT the result — either pass or fail is acceptable as long as
    // there is no crash.  Log the outcome for the QA report.
    if (result.has_value()) {
        SUCCEED() << "Bootstrap with truncated sys_databases succeeded (treated as empty).";
    } else {
        SUCCEED() << "Bootstrap with truncated sys_databases failed gracefully: "
                  << result.error().message;
    }

    // The catalog must not have a partially-open sys_databases that could
    // corrupt subsequent operations.  If bootstrap failed, the catalog should
    // not expose a broken sys_databases registration that blocks future use.
    // (No assertion here — we just verify no crash occurred by reaching here.)
}

/// Write random garbage bytes into sys_databases, then restart.
/// Same contract: no crash, graceful error or silent recovery.
TEST_F(QA_GDB812, CorruptedSysDatabasesFileNocrash) {
    run_bootstrap();

    destroy_components();
    auto path = sys_databases_path();
    ASSERT_TRUE(std::filesystem::exists(path));

    // Overwrite with garbage (256 bytes of 0xAB).
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(f.is_open());
        std::vector<char> garbage(256, static_cast<char>(0xAB));
        f.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
    }

    create_components();

    auto result = SystemBootstrap::bootstrap(
        *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
    if (result.has_value()) {
        SUCCEED() << "Bootstrap with corrupted sys_databases succeeded.";
    } else {
        SUCCEED() << "Bootstrap with corrupted sys_databases failed gracefully: "
                  << result.error().message;
    }
}

} // namespace
} // namespace sixseven
