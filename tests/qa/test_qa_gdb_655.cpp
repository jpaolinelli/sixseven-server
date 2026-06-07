/// @file test_qa_gdb_655.cpp
/// QA adversarial tests for GDB-655: Remove hardcoded sixseven database from
/// Catalog constructor.
///
/// Verifies:
///   AC1: Catalog constructor only creates sixseven_system.
///   AC2: sixseven (default_database_id) is droppable — no hardcoded protection.
///   AC3: restore_database() method exists and works.
///   AC4: restore_database() advances next_database_id_ past restored ID.
///   AC5: StorageManager no longer auto-creates default DB directory.
///   AC6: Unit tests pass (verified separately via build).
///
/// Adversarial categories: edge cases, boundary values, duplicate detection,
/// ordering, re-restore after drop, stress.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/common/types.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sixseven {
namespace {

// Helper to build a simple table schema.
static TableSchema make_schema(const std::string& name) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {
        {0, "id", TypeId::INT32, false, ""},
        {1, "val", TypeId::STRING, true, ""},
    };
    schema.pk_columns = "id";
    return schema;
}

// ============================================================================
// AC1: Catalog constructor only creates sixseven_system
// ============================================================================

TEST(QA_GDB655_Constructor, OnlySystemDatabaseExists) {
    Catalog catalog;
    auto dbs = catalog.list_databases();
    ASSERT_EQ(dbs.size(), 1u);
    EXPECT_EQ(dbs[0].database_id, system_database_id);
    EXPECT_EQ(dbs[0].name, "sixseven_system");
}

TEST(QA_GDB655_Constructor, DefaultDatabaseDoesNotExist) {
    Catalog catalog;
    auto result = catalog.get_database("demo");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_GDB655_Constructor, CreateTableInDefaultDbFailsBeforeRestore) {
    Catalog catalog;
    auto result = catalog.create_table(default_database_id, make_schema("t1"));
    EXPECT_FALSE(result.has_value());
}

// ============================================================================
// AC2: sixseven is droppable — no hardcoded protection
// ============================================================================

TEST(QA_GDB655_DropDefault, CanDropDefaultDatabase) {
    Catalog catalog;
    auto r = catalog.restore_database(default_database_id, "demo");
    ASSERT_TRUE(r.has_value()) << r.error().message;

    auto drop = catalog.drop_database(default_database_id, /*cascade=*/false);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // Verify it's gone.
    auto get = catalog.get_database("demo");
    EXPECT_FALSE(get.has_value());
}

TEST(QA_GDB655_DropDefault, CanDropDefaultWithCascade) {
    Catalog catalog;
    auto r = catalog.restore_database(default_database_id, "demo");
    ASSERT_TRUE(r.has_value()) << r.error().message;

    // Create a table so cascade has work to do.
    auto tid = catalog.create_table(default_database_id, make_schema("users"));
    ASSERT_TRUE(tid.has_value()) << tid.error().message;

    // Non-cascade should fail because tables exist.
    auto fail = catalog.drop_database(default_database_id, /*cascade=*/false);
    EXPECT_FALSE(fail.has_value());
    EXPECT_EQ(fail.error().code, StatusCode::CONSTRAINT_VIOLATION);

    // Cascade should succeed.
    auto drop = catalog.drop_database(default_database_id, /*cascade=*/true);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    auto dbs = catalog.list_databases();
    EXPECT_EQ(dbs.size(), 1u); // Only system remains.
}

TEST(QA_GDB655_DropDefault, SystemDatabaseStillProtected) {
    Catalog catalog;
    auto drop = catalog.drop_database(system_database_id, /*cascade=*/false);
    EXPECT_FALSE(drop.has_value());
    EXPECT_EQ(drop.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST(QA_GDB655_DropDefault, DropNonExistentFails) {
    Catalog catalog;
    // default_database_id was never restored, so dropping it should fail NOT_FOUND.
    auto drop = catalog.drop_database(default_database_id, /*cascade=*/false);
    EXPECT_FALSE(drop.has_value());
    EXPECT_EQ(drop.error().code, StatusCode::NOT_FOUND);
}

// ============================================================================
// AC3: restore_database() works correctly
// ============================================================================

TEST(QA_GDB655_Restore, BasicRestore) {
    Catalog catalog;
    auto r = catalog.restore_database(default_database_id, "demo");
    ASSERT_TRUE(r.has_value()) << r.error().message;

    auto db = catalog.get_database("demo");
    ASSERT_TRUE(db.has_value()) << db.error().message;
    EXPECT_EQ(db->database_id, default_database_id);
    EXPECT_EQ(db->name, "demo");
}

TEST(QA_GDB655_Restore, RestoreWithArbitraryId) {
    Catalog catalog;
    auto r = catalog.restore_database(42, "mydb");
    ASSERT_TRUE(r.has_value()) << r.error().message;

    auto db = catalog.get_database("mydb");
    ASSERT_TRUE(db.has_value()) << db.error().message;
    EXPECT_EQ(db->database_id, 42);
}

TEST(QA_GDB655_Restore, RestoreMultipleDatabases) {
    Catalog catalog;
    ASSERT_TRUE(catalog.restore_database(10, "db_a").has_value());
    ASSERT_TRUE(catalog.restore_database(20, "db_b").has_value());
    ASSERT_TRUE(catalog.restore_database(5, "db_c").has_value());

    auto dbs = catalog.list_databases();
    // system + 3 restored = 4
    EXPECT_EQ(dbs.size(), 4u);
}

TEST(QA_GDB655_Restore, DuplicateIdFails) {
    Catalog catalog;
    ASSERT_TRUE(catalog.restore_database(10, "first").has_value());
    auto r = catalog.restore_database(10, "second");
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::ALREADY_EXISTS);
}

TEST(QA_GDB655_Restore, DuplicateNameFails) {
    Catalog catalog;
    ASSERT_TRUE(catalog.restore_database(10, "mydb").has_value());
    auto r = catalog.restore_database(20, "mydb");
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::ALREADY_EXISTS);
}

TEST(QA_GDB655_Restore, RestoreSystemIdFails) {
    Catalog catalog;
    // system_database_id is already created by constructor.
    auto r = catalog.restore_database(system_database_id, "another_system");
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::ALREADY_EXISTS);
}

TEST(QA_GDB655_Restore, RestoreSystemNameFails) {
    Catalog catalog;
    auto r = catalog.restore_database(99, system_database_name);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::ALREADY_EXISTS);
}

TEST(QA_GDB655_Restore, RestoreEmptyName) {
    Catalog catalog;
    // Empty name should succeed (no validation rule against it).
    auto r = catalog.restore_database(10, "");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto db = catalog.get_database("");
    ASSERT_TRUE(db.has_value());
    EXPECT_EQ(db->database_id, 10);
}

TEST(QA_GDB655_Restore, RestoreThenCreateTable) {
    Catalog catalog;
    ASSERT_TRUE(catalog.restore_database(default_database_id, "demo").has_value());

    auto tid = catalog.create_table(default_database_id, make_schema("users"));
    ASSERT_TRUE(tid.has_value()) << tid.error().message;

    auto ts = catalog.get_table(default_database_id, "users");
    ASSERT_TRUE(ts.has_value()) << ts.error().message;
    EXPECT_EQ(ts->name, "users");
}

TEST(QA_GDB655_Restore, RestoreAfterDrop) {
    Catalog catalog;
    ASSERT_TRUE(catalog.restore_database(default_database_id, "demo").has_value());
    ASSERT_TRUE(catalog.drop_database(default_database_id, false).has_value());

    // Re-restore with the same id and name.
    auto r = catalog.restore_database(default_database_id, "demo");
    ASSERT_TRUE(r.has_value()) << r.error().message;

    auto db = catalog.get_database("demo");
    ASSERT_TRUE(db.has_value());
    EXPECT_EQ(db->database_id, default_database_id);
}

// ============================================================================
// AC4: restore_database() advances next_database_id_
// ============================================================================

TEST(QA_GDB655_NextId, AdvancesPastRestoredId) {
    Catalog catalog;
    // Restore a database with a high ID.
    ASSERT_TRUE(catalog.restore_database(100, "high_db").has_value());

    // create_database should assign id > 100.
    auto new_id = catalog.create_database("new_db");
    ASSERT_TRUE(new_id.has_value()) << new_id.error().message;
    EXPECT_GT(*new_id, 100);
}

TEST(QA_GDB655_NextId, AdvancesPastMultipleRestoredIds) {
    Catalog catalog;
    ASSERT_TRUE(catalog.restore_database(5, "db_5").has_value());
    ASSERT_TRUE(catalog.restore_database(50, "db_50").has_value());
    ASSERT_TRUE(catalog.restore_database(25, "db_25").has_value());

    // The next auto-assigned id should be > 50 (the max restored).
    auto new_id = catalog.create_database("auto_db");
    ASSERT_TRUE(new_id.has_value()) << new_id.error().message;
    EXPECT_GT(*new_id, 50);
}

TEST(QA_GDB655_NextId, RestoreLowIdDoesNotRegress) {
    Catalog catalog;
    // Restore high first, then low — counter should NOT regress.
    ASSERT_TRUE(catalog.restore_database(100, "high").has_value());
    ASSERT_TRUE(catalog.restore_database(3, "low").has_value());

    auto new_id = catalog.create_database("after_both");
    ASSERT_TRUE(new_id.has_value()) << new_id.error().message;
    EXPECT_GT(*new_id, 100);
}

TEST(QA_GDB655_NextId, CreateThenRestoreHigherDoesNotCollide) {
    Catalog catalog;
    // create_database uses next_database_id_ which starts at system_database_id+1 = 3.
    auto id1 = catalog.create_database("auto1");
    ASSERT_TRUE(id1.has_value());

    // Now restore a much higher id.
    ASSERT_TRUE(catalog.restore_database(200, "restored_high").has_value());

    // Next create should be > 200.
    auto id2 = catalog.create_database("auto2");
    ASSERT_TRUE(id2.has_value()) << id2.error().message;
    EXPECT_GT(*id2, 200);
}

// ============================================================================
// AC5: StorageManager no longer auto-creates default DB directory
// ============================================================================

TEST(QA_GDB655_StorageManager, ConstructorDoesNotCreateDefaultDbDir) {
    auto tmp = std::filesystem::temp_directory_path() / "qa_gdb655_sm";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    DiskManager dm;
    StorageManager sm(dm, tmp, /*pool_size=*/16);

    // The databases directory should NOT exist — StorageManager doesn't create it.
    auto db_dir = tmp / "databases" / std::to_string(default_database_id);
    EXPECT_FALSE(std::filesystem::exists(db_dir));

    std::filesystem::remove_all(tmp);
}

// ============================================================================
// Adversarial: stress / boundary
// ============================================================================

TEST(QA_GDB655_Stress, RestoreManyDatabases) {
    Catalog catalog;
    constexpr int N = 200;
    for (int i = 0; i < N; ++i) {
        // Use IDs starting at 100 to avoid colliding with system_database_id.
        database_id_t id = 100 + i;
        auto r = catalog.restore_database(id, "db_" + std::to_string(i));
        ASSERT_TRUE(r.has_value()) << "Failed at i=" << i << ": " << r.error().message;
    }

    auto dbs = catalog.list_databases();
    EXPECT_EQ(dbs.size(), static_cast<size_t>(N + 1)); // +1 for system

    // Next created id should be past the max restored (100 + N - 1 = 299).
    auto new_id = catalog.create_database("after_stress");
    ASSERT_TRUE(new_id.has_value()) << new_id.error().message;
    EXPECT_GE(*new_id, 100 + N);
}

TEST(QA_GDB655_Stress, DropAndReCreateCycle) {
    Catalog catalog;
    ASSERT_TRUE(catalog.restore_database(default_database_id, "demo").has_value());

    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(catalog.drop_database(default_database_id, false).has_value())
            << "drop failed at iteration " << i;
        ASSERT_TRUE(catalog.restore_database(default_database_id, "demo").has_value())
            << "restore failed at iteration " << i;
    }

    auto db = catalog.get_database("demo");
    ASSERT_TRUE(db.has_value());
    EXPECT_EQ(db->database_id, default_database_id);
}

} // namespace
} // namespace sixseven
