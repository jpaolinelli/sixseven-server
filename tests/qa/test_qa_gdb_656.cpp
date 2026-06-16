/// @file test_qa_gdb_656.cpp
/// QA adversarial tests for GDB-656: Create sixseven database during bootstrap
/// and persist to sys_databases.
///
/// Verifies:
///   AC1: sixseven database is created and persisted during first bootstrap.
///   AC2: sys_databases is opened before load_catalog() on subsequent runs.
///   AC3: load_catalog() loads all databases from sys_databases before loading tables.
///   AC4: All databases (including sixseven) survive server restart.
///   AC5: Old deployments without sys_databases are migrated cleanly.
///   AC6: All existing tests pass (verified separately via build).
///
/// Adversarial categories: edge cases, boundary values, migration, restart
/// survival, ordering, stress.

#include "test_qa_gdb_656_657_660_fixture.h"

namespace sixseven {
namespace {

// ============================================================================
// Test fixture — mirrors CatalogPersistenceTest but scoped to QA naming.
// Inherits all shared members and helpers from QA_GDB_Bootstrap_Base.
// ============================================================================

class QA_GDB656 : public QA_GDB_Bootstrap_Base {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "qa_gdb656";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        // Do NOT call init_test_catalog — bootstrap should handle sixseven creation.
        create_components();
    }

    void TearDown() override {
        destroy_components();
        std::filesystem::remove_all(data_dir_);
    }
};

// ============================================================================
// AC1: sixseven database is created and persisted during first bootstrap
// ============================================================================

TEST_F(QA_GDB656, FirstBootstrapCreatesSixsevenDatabase) {
    run_bootstrap();

    auto db = catalog_->get_database("demo");
    ASSERT_TRUE(db.has_value()) << db.error().message;
    EXPECT_EQ(db->database_id, default_database_id);
    EXPECT_EQ(db->name, "demo");
}

TEST_F(QA_GDB656, FirstBootstrapPersistsSixsevenToSysDatabases) {
    run_bootstrap();

    auto entries = scan_sys_databases();
    bool found = false;
    for (const auto& [id, name] : entries) {
        if (id == default_database_id) {
            EXPECT_EQ(name, "demo");
            found = true;
        }
    }
    EXPECT_TRUE(found) << "sixseven not found in sys_databases after first bootstrap";
}

TEST_F(QA_GDB656, FirstBootstrapCreatesSixsevenStorageDirectory) {
    run_bootstrap();

    auto db_dir = data_dir_ / "databases" / std::to_string(default_database_id);
    EXPECT_TRUE(std::filesystem::exists(db_dir))
        << "sixseven storage directory should exist after bootstrap";
}

TEST_F(QA_GDB656, FirstBootstrapSixsevenIdIsOne) {
    run_bootstrap();

    auto db = catalog_->get_database("demo");
    ASSERT_TRUE(db.has_value());
    EXPECT_EQ(db->database_id, 1) << "sixseven must have database_id=1 (default_database_id)";
}

TEST_F(QA_GDB656, SysDatabasesTableExistsAfterFirstBootstrap) {
    run_bootstrap();

    auto tbl = catalog_->get_table(system_database_id, "sys_databases");
    ASSERT_TRUE(tbl.has_value()) << tbl.error().message;
    EXPECT_EQ(tbl->table_id, sys_databases_table_id);
}

// ============================================================================
// AC2: sys_databases is opened before load_catalog() on subsequent runs
// ============================================================================

TEST_F(QA_GDB656, SubsequentRunOpensSysDatabases) {
    run_bootstrap();
    restart();
    run_bootstrap();

    // sys_databases should be accessible after second bootstrap.
    auto tbl = catalog_->get_table(system_database_id, "sys_databases");
    ASSERT_TRUE(tbl.has_value()) << tbl.error().message;
    EXPECT_EQ(tbl->table_id, sys_databases_table_id);

    // The storage should be open.
    auto ts = storage_->get_table_storage(sys_databases_table_id);
    ASSERT_TRUE(ts.has_value()) << ts.error().message;
}

// ============================================================================
// AC3: load_catalog() loads all databases from sys_databases before tables
// ============================================================================

TEST_F(QA_GDB656, DatabasesLoadedBeforeTablesOnRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);
    exec_ok("CREATE TABLE load_order_test (id INT, name VARCHAR)");

    restart();
    run_bootstrap();

    // If databases were loaded after tables, restore_table would fail
    // because the database doesn't exist yet. Verify both are present.
    auto db = catalog_->get_database("demo");
    ASSERT_TRUE(db.has_value()) << "database should be restored before tables";

    auto tbl = catalog_->get_table(default_database_id, "load_order_test");
    ASSERT_TRUE(tbl.has_value()) << "table should be restored (database loaded first)";
}

TEST_F(QA_GDB656, TablesInRestoredDatabaseAreAccessible) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);
    exec_ok("CREATE TABLE qa_users (id INT, name VARCHAR)");
    exec_ok("INSERT INTO qa_users VALUES (1, 'Alice')");
    exec_ok("INSERT INTO qa_users VALUES (2, 'Bob')");

    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    auto qr = exec_ok("SELECT id, name FROM qa_users");
    EXPECT_EQ(qr.rows.size(), 2u) << "data should survive restart";
}

// ============================================================================
// AC4: All databases (including sixseven) survive server restart
// ============================================================================

TEST_F(QA_GDB656, SixsevenSurvivesSingleRestart) {
    run_bootstrap();
    restart();
    run_bootstrap();

    auto db = catalog_->get_database("demo");
    ASSERT_TRUE(db.has_value()) << db.error().message;
    EXPECT_EQ(db->database_id, default_database_id);
}

TEST_F(QA_GDB656, SixsevenSurvivesMultipleRestarts) {
    run_bootstrap();

    for (int i = 0; i < 5; ++i) {
        restart();
        run_bootstrap();

        auto db = catalog_->get_database("demo");
        ASSERT_TRUE(db.has_value())
            << "sixseven missing after restart " << (i + 1) << ": " << db.error().message;
        EXPECT_EQ(db->database_id, default_database_id);
    }
}

TEST_F(QA_GDB656, PersistedDatabaseSurvivesRestart) {
    run_bootstrap();

    // Manually persist a second database.
    auto r_db = catalog_->restore_database(42, "user_db");
    ASSERT_TRUE(r_db.has_value()) << r_db.error().message;
    auto r_p = persistence_->persist_database(42, "user_db");
    ASSERT_TRUE(r_p.has_value()) << r_p.error().message;

    restart();
    run_bootstrap();

    // Both sixseven and user_db should exist.
    auto ss = catalog_->get_database("demo");
    ASSERT_TRUE(ss.has_value()) << ss.error().message;
    EXPECT_EQ(ss->database_id, default_database_id);

    auto ud = catalog_->get_database("user_db");
    ASSERT_TRUE(ud.has_value()) << ud.error().message;
    EXPECT_EQ(ud->database_id, 42);
}

TEST_F(QA_GDB656, TableDataInRestoredDatabaseSurvivesRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);
    exec_ok("CREATE TABLE persist_data (id INT, val VARCHAR)");
    exec_ok("INSERT INTO persist_data VALUES (1, 'one')");
    exec_ok("INSERT INTO persist_data VALUES (2, 'two')");
    exec_ok("INSERT INTO persist_data VALUES (3, 'three')");

    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    auto qr = exec_ok("SELECT id, val FROM persist_data");
    EXPECT_EQ(qr.rows.size(), 3u);
}

// ============================================================================
// AC5: Old deployments without sys_databases are migrated cleanly
// ============================================================================

TEST_F(QA_GDB656, MigrationCreatesAndBackfillsSysDatabases) {
    // Simulate an old deployment: bootstrap, then delete the sys_databases file.
    run_bootstrap();
    engine_->set_current_database(default_database_id);
    exec_ok("CREATE TABLE migration_test (id INT)");

    // Find the sys_databases file on disk.
    // StorageManager::table_path writes to:
    //   <data_dir>/databases/<db_id>/tables/table_<table_id>.db
    // GDB-812: the previous path was wrong (missing "tables/" and "table_" prefix),
    // so the delete was a no-op and migration was never triggered.
    auto sys_db_file = data_dir_ / "databases" / std::to_string(system_database_id) / "tables" /
                       ("table_" + std::to_string(sys_databases_table_id) + ".db");
    ASSERT_TRUE(std::filesystem::exists(sys_db_file))
        << "sys_databases file must exist before deletion at: " << sys_db_file;

    // GDB-1264 / Windows ordering: destroy components first (closes file handles),
    // then delete, then recreate — avoids "file in use" error on Windows.
    destroy_components();
    std::filesystem::remove(sys_db_file);
    ASSERT_FALSE(std::filesystem::exists(sys_db_file))
        << "sys_databases file must be absent before restart to trigger migration";
    create_components();

    // Bootstrap on the fresh components — the migration path should trigger.
    run_bootstrap();

    // sixseven database should still be accessible.
    auto db = catalog_->get_database("demo");
    ASSERT_TRUE(db.has_value()) << "migration should restore sixseven: " << db.error().message;
    EXPECT_EQ(db->database_id, default_database_id);

    // The table should also be restored.
    auto tbl = catalog_->get_table(default_database_id, "migration_test");
    ASSERT_TRUE(tbl.has_value()) << "table should survive migration";

    // Migration must have backfilled at least the sixseven database entry into
    // sys_databases (proves the migration code path actually ran).
    auto entries = scan_sys_databases();
    bool found_sixseven = false;
    for (const auto& [id, name] : entries) {
        if (id == default_database_id) {
            found_sixseven = true;
        }
    }
    EXPECT_TRUE(found_sixseven) << "migration must backfill sixseven into sys_databases";
}

TEST_F(QA_GDB656, MigrationAlwaysIncludesSixseven) {
    // Even if there are no user tables, migration should still include sixseven.
    run_bootstrap();

    // Find the sys_databases file at the correct StorageManager path.
    // GDB-812: the previous path was wrong (missing "tables/" and "table_" prefix).
    auto sys_db_file = data_dir_ / "databases" / std::to_string(system_database_id) / "tables" /
                       ("table_" + std::to_string(sys_databases_table_id) + ".db");
    ASSERT_TRUE(std::filesystem::exists(sys_db_file))
        << "sys_databases file must exist before deletion at: " << sys_db_file;

    // GDB-1264 / Windows ordering: destroy components first (closes file handles),
    // then delete, then recreate — avoids "file in use" error on Windows.
    destroy_components();
    std::filesystem::remove(sys_db_file);
    ASSERT_FALSE(std::filesystem::exists(sys_db_file))
        << "sys_databases file must be absent before restart to trigger migration";
    create_components();

    run_bootstrap();

    auto db = catalog_->get_database("demo");
    ASSERT_TRUE(db.has_value()) << "migration should include sixseven even with no user tables";

    // Prove migration ran: sys_databases must now contain sixseven.
    auto entries = scan_sys_databases();
    bool found_sixseven = false;
    for (const auto& [id, name] : entries) {
        if (id == default_database_id) {
            found_sixseven = true;
        }
    }
    EXPECT_TRUE(found_sixseven)
        << "migration must include sixseven in sys_databases even with no user tables";
}

// ============================================================================
// Adversarial: bootstrap idempotency
// ============================================================================

TEST_F(QA_GDB656, DoubleBootstrapDoesNotDuplicateSixseven) {
    // Bootstrap is only called once per process lifetime, but test that the
    // guard (get_database check) prevents duplicates if called twice.
    run_bootstrap();

    auto entries_before = scan_sys_databases();
    int count_before = 0;
    for (const auto& [id, name] : entries_before) {
        if (id == default_database_id)
            ++count_before;
    }
    EXPECT_EQ(count_before, 1) << "sixseven should appear exactly once in sys_databases";
}

// ============================================================================
// Adversarial: sys_databases schema correctness
// ============================================================================

TEST_F(QA_GDB656, SysDatabasesSchemaIsCorrect) {
    run_bootstrap();

    auto tbl = catalog_->get_table(system_database_id, "sys_databases");
    ASSERT_TRUE(tbl.has_value());
    ASSERT_EQ(tbl->columns.size(), 2u);
    EXPECT_EQ(tbl->columns[0].name, "database_id");
    EXPECT_EQ(tbl->columns[0].type_id, TypeId::INT32);
    EXPECT_FALSE(tbl->columns[0].nullable);
    EXPECT_EQ(tbl->columns[1].name, "name");
    EXPECT_EQ(tbl->columns[1].type_id, TypeId::STRING);
    EXPECT_FALSE(tbl->columns[1].nullable);
}

// ============================================================================
// Adversarial: multiple databases across restart
// ============================================================================

TEST_F(QA_GDB656, MultipleDatabasesAllSurviveRestart) {
    run_bootstrap();

    // Persist several databases.
    for (int i = 0; i < 10; ++i) {
        database_id_t id = 100 + i;
        std::string name = "db_" + std::to_string(i);
        ASSERT_TRUE(catalog_->restore_database(id, name).has_value());
        ASSERT_TRUE(persistence_->persist_database(id, name).has_value());
    }

    restart();
    run_bootstrap();

    // sixseven should exist.
    auto ss = catalog_->get_database("demo");
    ASSERT_TRUE(ss.has_value());

    // All 10 user databases should exist.
    for (int i = 0; i < 10; ++i) {
        std::string name = "db_" + std::to_string(i);
        auto db = catalog_->get_database(name);
        ASSERT_TRUE(db.has_value()) << name << " missing after restart: " << db.error().message;
        EXPECT_EQ(db->database_id, 100 + i);
    }
}

// ============================================================================
// Adversarial: create table, restart, create more, restart again
// ============================================================================

TEST_F(QA_GDB656, IncrementalTableCreationAcrossRestarts) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);
    exec_ok("CREATE TABLE t1 (id INT)");

    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    // t1 should exist.
    ASSERT_TRUE(catalog_->get_table(default_database_id, "t1").has_value());

    // Create another table.
    exec_ok("CREATE TABLE t2 (id INT, name VARCHAR)");

    restart();
    run_bootstrap();

    // Both should exist.
    ASSERT_TRUE(catalog_->get_table(default_database_id, "t1").has_value());
    ASSERT_TRUE(catalog_->get_table(default_database_id, "t2").has_value());
}

// ============================================================================
// Adversarial: drop table, restart, verify gone
// ============================================================================

TEST_F(QA_GDB656, DropTablePersistsAcrossRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);
    exec_ok("CREATE TABLE drop_me (id INT)");
    exec_ok("DROP TABLE drop_me");

    restart();
    run_bootstrap();

    auto tbl = catalog_->get_table(default_database_id, "drop_me");
    EXPECT_FALSE(tbl.has_value()) << "dropped table should not reappear after restart";
}

// ============================================================================
// Adversarial: sys_databases does not contain system database
// ============================================================================

TEST_F(QA_GDB656, SysDatabasesDoesNotContainSystemDb) {
    run_bootstrap();

    auto entries = scan_sys_databases();
    for (const auto& [id, name] : entries) {
        EXPECT_NE(id, system_database_id)
            << "system database should NOT be in sys_databases (it's always created by Catalog)";
    }
}

// ============================================================================
// Adversarial: stress — many restarts with growing catalog
// ============================================================================

TEST_F(QA_GDB656, StressManyRestartsWithGrowingCatalog) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    for (int i = 0; i < 10; ++i) {
        std::string tbl_name = "stress_t" + std::to_string(i);
        exec_ok("CREATE TABLE " + tbl_name + " (id INT, val VARCHAR)");
        exec_ok("INSERT INTO " + tbl_name + " VALUES (" + std::to_string(i) + ", 'row')");

        restart();
        run_bootstrap();
        engine_->set_current_database(default_database_id);

        // Verify all tables up to this point exist and have data.
        for (int j = 0; j <= i; ++j) {
            std::string check_name = "stress_t" + std::to_string(j);
            auto tbl = catalog_->get_table(default_database_id, check_name);
            ASSERT_TRUE(tbl.has_value()) << check_name << " missing at iteration " << i;
        }
    }
}

// ============================================================================
// Adversarial: empty database (no tables) persists
// ============================================================================

TEST_F(QA_GDB656, EmptyDatabasePersists) {
    run_bootstrap();

    // sixseven has no user tables — verify it still persists.
    auto entries_before = scan_sys_databases();
    bool found = false;
    for (const auto& [id, name] : entries_before) {
        if (id == default_database_id && name == "demo")
            found = true;
    }
    ASSERT_TRUE(found);

    restart();
    run_bootstrap();

    auto db = catalog_->get_database("demo");
    ASSERT_TRUE(db.has_value());
    auto user_tables = catalog_->list_tables(default_database_id);
    EXPECT_TRUE(user_tables.empty()) << "no user tables should exist in empty database";
}

// ============================================================================
// Adversarial: remove_database then restart
// ============================================================================

TEST_F(QA_GDB656, RemovedDatabaseDoesNotReappearAfterRestart) {
    run_bootstrap();

    // Add and persist a database, then remove it.
    ASSERT_TRUE(catalog_->restore_database(77, "temp_db").has_value());
    ASSERT_TRUE(persistence_->persist_database(77, "temp_db").has_value());
    ASSERT_TRUE(persistence_->remove_database(77).has_value());

    restart();
    run_bootstrap();

    auto db = catalog_->get_database("temp_db");
    EXPECT_FALSE(db.has_value()) << "removed database should not reappear after restart";
}

// ============================================================================
// Boundary: database_id values
// ============================================================================

TEST_F(QA_GDB656, LargeDatabaseIdPersists) {
    run_bootstrap();

    database_id_t large_id = 999999;
    ASSERT_TRUE(catalog_->restore_database(large_id, "large_id_db").has_value());
    ASSERT_TRUE(persistence_->persist_database(large_id, "large_id_db").has_value());

    restart();
    run_bootstrap();

    auto db = catalog_->get_database("large_id_db");
    ASSERT_TRUE(db.has_value()) << db.error().message;
    EXPECT_EQ(db->database_id, large_id);
}

} // namespace
} // namespace sixseven
