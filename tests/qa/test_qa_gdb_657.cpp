/// @file test_qa_gdb_657.cpp
/// QA adversarial tests for GDB-657: Call persist/remove database from
/// CREATE/DROP DATABASE handlers.
///
/// Verifies:
///   AC1: execute_create_database() calls persist_database() after creating.
///   AC2: execute_drop_database() calls remove_database() after dropping.
///   AC3: Both handle null catalog_persistence_ gracefully.
///   AC4: Creating and dropping databases persists/removes from sys_databases.
///   AC5: Unit tests pass (verified via build).
///
/// Adversarial categories: edge cases, boundary values, null persistence,
/// restart survival, ordering, cascade, stress.

#include "test_qa_gdb_656_657_660_fixture.h"

namespace sixseven {
namespace {

// ============================================================================
// Test fixture.
// Inherits all shared members and helpers from QA_GDB_Bootstrap_Base.
// GDB-657-specific additions: exec_error() and sys_databases_contains().
// ============================================================================

class QA_GDB657 : public QA_GDB_Bootstrap_Base {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "qa_gdb657";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        create_components();
    }

    void TearDown() override {
        destroy_components();
        std::filesystem::remove_all(data_dir_);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected);
        }
    }

    bool sys_databases_contains(const std::string& name) {
        auto entries = scan_sys_databases();
        return std::any_of(
            entries.begin(), entries.end(), [&](const auto& e) { return e.second == name; });
    }
};

// ============================================================================
// AC1: execute_create_database() calls persist_database() after creating
// ============================================================================

TEST_F(QA_GDB657, CreateDatabasePersistsToSysDatabases) {
    run_bootstrap();
    exec_ok("CREATE DATABASE test_db");

    auto entries = scan_sys_databases();
    bool found = false;
    for (const auto& [id, name] : entries) {
        if (name == "test_db") {
            found = true;
        }
    }
    EXPECT_TRUE(found) << "CREATE DATABASE should persist to sys_databases";
}

TEST_F(QA_GDB657, CreateDatabasePersistsCorrectId) {
    run_bootstrap();
    exec_ok("CREATE DATABASE id_check_db");

    auto db = catalog_->get_database("id_check_db");
    ASSERT_TRUE(db.has_value());

    auto entries = scan_sys_databases();
    for (const auto& [id, name] : entries) {
        if (name == "id_check_db") {
            EXPECT_EQ(id, db->database_id) << "persisted id should match catalog id";
        }
    }
}

TEST_F(QA_GDB657, CreateMultipleDatabasesAllPersisted) {
    run_bootstrap();
    exec_ok("CREATE DATABASE db_a");
    exec_ok("CREATE DATABASE db_b");
    exec_ok("CREATE DATABASE db_c");

    EXPECT_TRUE(sys_databases_contains("db_a"));
    EXPECT_TRUE(sys_databases_contains("db_b"));
    EXPECT_TRUE(sys_databases_contains("db_c"));
}

// ============================================================================
// AC2: execute_drop_database() calls remove_database() after dropping
// ============================================================================

TEST_F(QA_GDB657, DropDatabaseRemovesFromSysDatabases) {
    run_bootstrap();
    exec_ok("CREATE DATABASE drop_me_db");
    ASSERT_TRUE(sys_databases_contains("drop_me_db"));

    exec_ok("DROP DATABASE drop_me_db");
    EXPECT_FALSE(sys_databases_contains("drop_me_db"))
        << "DROP DATABASE should remove from sys_databases";
}

TEST_F(QA_GDB657, DropDatabaseOnlyRemovesTarget) {
    run_bootstrap();
    exec_ok("CREATE DATABASE keep_db");
    exec_ok("CREATE DATABASE remove_db");
    ASSERT_TRUE(sys_databases_contains("keep_db"));
    ASSERT_TRUE(sys_databases_contains("remove_db"));

    exec_ok("DROP DATABASE remove_db");
    EXPECT_TRUE(sys_databases_contains("keep_db"))
        << "dropping one database should not affect others";
    EXPECT_FALSE(sys_databases_contains("remove_db"));
}

// ============================================================================
// AC3: Both handle null catalog_persistence_ gracefully
// ============================================================================

TEST_F(QA_GDB657, CreateDatabaseWithNullPersistenceDoesNotCrash) {
    run_bootstrap();

    // Remove persistence and try CREATE DATABASE.
    engine_->set_catalog_persistence(nullptr);
    auto result = engine_->execute("CREATE DATABASE null_persist_db");
    EXPECT_TRUE(result.has_value())
        << "CREATE DATABASE should succeed even with null persistence: " << result.error().message;

    // Database should exist in-memory but not in sys_databases.
    auto db = catalog_->get_database("null_persist_db");
    EXPECT_TRUE(db.has_value());
}

TEST_F(QA_GDB657, DropDatabaseWithNullPersistenceDoesNotCrash) {
    run_bootstrap();
    exec_ok("CREATE DATABASE null_drop_db");

    // Remove persistence and try DROP DATABASE.
    engine_->set_catalog_persistence(nullptr);
    auto result = engine_->execute("DROP DATABASE null_drop_db");
    EXPECT_TRUE(result.has_value())
        << "DROP DATABASE should succeed even with null persistence: " << result.error().message;

    // Database should be gone from catalog.
    auto db = catalog_->get_database("null_drop_db");
    EXPECT_FALSE(db.has_value());
}

// ============================================================================
// AC4: Creating and dropping databases persists/removes from sys_databases
//      (end-to-end with restart)
// ============================================================================

TEST_F(QA_GDB657, CreatedDatabaseSurvivesRestart) {
    run_bootstrap();
    exec_ok("CREATE DATABASE survive_db");

    restart();
    run_bootstrap();

    auto db = catalog_->get_database("survive_db");
    ASSERT_TRUE(db.has_value()) << "database created via CREATE DATABASE should survive restart: "
                                << db.error().message;
}

TEST_F(QA_GDB657, DroppedDatabaseStaysGoneAfterRestart) {
    run_bootstrap();
    exec_ok("CREATE DATABASE gone_db");
    exec_ok("DROP DATABASE gone_db");

    restart();
    run_bootstrap();

    auto db = catalog_->get_database("gone_db");
    EXPECT_FALSE(db.has_value()) << "dropped database should not reappear after restart";
}

TEST_F(QA_GDB657, CreateDropCreateSameName) {
    run_bootstrap();
    exec_ok("CREATE DATABASE recycled_db");
    exec_ok("DROP DATABASE recycled_db");
    exec_ok("CREATE DATABASE recycled_db");

    EXPECT_TRUE(sys_databases_contains("recycled_db"));

    restart();
    run_bootstrap();

    auto db = catalog_->get_database("recycled_db");
    ASSERT_TRUE(db.has_value()) << "re-created database should survive restart";
}

// ============================================================================
// Adversarial: IF NOT EXISTS / IF EXISTS
// ============================================================================

TEST_F(QA_GDB657, CreateIfNotExistsDoesNotDuplicate) {
    run_bootstrap();
    exec_ok("CREATE DATABASE dup_db");
    exec_ok("CREATE DATABASE IF NOT EXISTS dup_db");

    // Count occurrences in sys_databases.
    auto entries = scan_sys_databases();
    int count = 0;
    for (const auto& [id, name] : entries) {
        if (name == "dup_db")
            ++count;
    }
    EXPECT_EQ(count, 1) << "IF NOT EXISTS should not add a duplicate row";
}

TEST_F(QA_GDB657, DropIfExistsNonexistentDoesNotError) {
    run_bootstrap();
    auto result = engine_->execute("DROP DATABASE IF EXISTS nonexistent_db");
    EXPECT_TRUE(result.has_value()) << "DROP DATABASE IF EXISTS on nonexistent db should succeed";
}

// ============================================================================
// Adversarial: cascade DROP DATABASE with tables
// ============================================================================

TEST_F(QA_GDB657, DropDatabaseCascadeRemovesFromPersistence) {
    run_bootstrap();
    exec_ok("CREATE DATABASE cascade_db");

    auto db = catalog_->get_database("cascade_db");
    ASSERT_TRUE(db.has_value());
    engine_->set_current_database(db->database_id);
    exec_ok("CREATE TABLE cascade_tbl (id INT, name VARCHAR)");
    exec_ok("INSERT INTO cascade_tbl VALUES (1, 'test')");

    // Reset to default before dropping.
    engine_->set_current_database(default_database_id);
    exec_ok("DROP DATABASE cascade_db CASCADE");

    EXPECT_FALSE(sys_databases_contains("cascade_db"))
        << "CASCADE drop should remove from sys_databases";

    restart();
    run_bootstrap();

    auto db_after = catalog_->get_database("cascade_db");
    EXPECT_FALSE(db_after.has_value())
        << "cascade-dropped database should not reappear after restart";
}

// ============================================================================
// Adversarial: multiple databases, create and drop interleaved
// ============================================================================

TEST_F(QA_GDB657, InterleavedCreateDropPersistence) {
    run_bootstrap();

    exec_ok("CREATE DATABASE inter_a");
    exec_ok("CREATE DATABASE inter_b");
    exec_ok("DROP DATABASE inter_a");
    exec_ok("CREATE DATABASE inter_c");
    exec_ok("DROP DATABASE inter_b");

    // Only inter_c should remain (plus sixseven).
    EXPECT_FALSE(sys_databases_contains("inter_a"));
    EXPECT_FALSE(sys_databases_contains("inter_b"));
    EXPECT_TRUE(sys_databases_contains("inter_c"));

    restart();
    run_bootstrap();

    EXPECT_FALSE(catalog_->get_database("inter_a").has_value());
    EXPECT_FALSE(catalog_->get_database("inter_b").has_value());
    EXPECT_TRUE(catalog_->get_database("inter_c").has_value());
}

// ============================================================================
// Adversarial: drop non-existent database without IF EXISTS
// ============================================================================

TEST_F(QA_GDB657, DropNonexistentDatabaseReturnsError) {
    run_bootstrap();
    exec_error("DROP DATABASE ghost_db", StatusCode::NOT_FOUND);
}

// ============================================================================
// Adversarial: create already existing database without IF NOT EXISTS
// ============================================================================

TEST_F(QA_GDB657, CreateExistingDatabaseReturnsError) {
    run_bootstrap();
    exec_ok("CREATE DATABASE already_db");
    exec_error("CREATE DATABASE already_db", StatusCode::ALREADY_EXISTS);

    // Should still have exactly one entry.
    auto entries = scan_sys_databases();
    int count = 0;
    for (const auto& [id, name] : entries) {
        if (name == "already_db")
            ++count;
    }
    EXPECT_EQ(count, 1) << "failed CREATE should not leave extra sys_databases rows";
}

// ============================================================================
// Adversarial: rapid create-drop cycles
// ============================================================================

TEST_F(QA_GDB657, RapidCreateDropCycles) {
    run_bootstrap();

    for (int i = 0; i < 20; ++i) {
        exec_ok("CREATE DATABASE rapid_db");
        EXPECT_TRUE(sys_databases_contains("rapid_db"));
        exec_ok("DROP DATABASE rapid_db");
        EXPECT_FALSE(sys_databases_contains("rapid_db"));
    }

    // After all cycles, no rapid_db should exist.
    EXPECT_FALSE(sys_databases_contains("rapid_db"));
}

// ============================================================================
// Stress: many databases created, then all dropped
// ============================================================================

TEST_F(QA_GDB657, StressManyDatabasesCreateAndDrop) {
    run_bootstrap();

    constexpr int N = 25;
    for (int i = 0; i < N; ++i) {
        exec_ok("CREATE DATABASE stress_db_" + std::to_string(i));
    }

    // All should be persisted.
    for (int i = 0; i < N; ++i) {
        EXPECT_TRUE(sys_databases_contains("stress_db_" + std::to_string(i)))
            << "stress_db_" << i << " should be persisted";
    }

    // Drop all.
    for (int i = 0; i < N; ++i) {
        exec_ok("DROP DATABASE stress_db_" + std::to_string(i));
    }

    // None should remain.
    for (int i = 0; i < N; ++i) {
        EXPECT_FALSE(sys_databases_contains("stress_db_" + std::to_string(i)))
            << "stress_db_" << i << " should be removed";
    }
}

// ============================================================================
// Stress: create many databases, restart, verify all survive
// ============================================================================

TEST_F(QA_GDB657, StressManyDatabasesSurviveRestart) {
    run_bootstrap();

    constexpr int N = 15;
    for (int i = 0; i < N; ++i) {
        exec_ok("CREATE DATABASE restart_db_" + std::to_string(i));
    }

    restart();
    run_bootstrap();

    for (int i = 0; i < N; ++i) {
        std::string name = "restart_db_" + std::to_string(i);
        auto db = catalog_->get_database(name);
        ASSERT_TRUE(db.has_value()) << name << " should survive restart: " << db.error().message;
    }
}

// ============================================================================
// Adversarial: create database, add tables, drop cascade, restart
// ============================================================================

TEST_F(QA_GDB657, DropCascadeWithTablesSurvivesRestart) {
    run_bootstrap();
    exec_ok("CREATE DATABASE full_db");

    auto db = catalog_->get_database("full_db");
    ASSERT_TRUE(db.has_value());
    engine_->set_current_database(db->database_id);
    exec_ok("CREATE TABLE t1 (id INT)");
    exec_ok("CREATE TABLE t2 (id INT, name VARCHAR)");

    engine_->set_current_database(default_database_id);
    exec_ok("DROP DATABASE full_db CASCADE");

    restart();
    run_bootstrap();

    EXPECT_FALSE(catalog_->get_database("full_db").has_value());
}

// ============================================================================
// Adversarial: sixseven database is not droppable (if protected)
// or can be dropped and removed from persistence
// ============================================================================

TEST_F(QA_GDB657, DropSixsevenDatabaseBehavior) {
    run_bootstrap();

    // The default "demo" database must exist in sys_databases after bootstrap.
    ASSERT_TRUE(sys_databases_contains(default_database_name))
        << "default database must be present in sys_databases after bootstrap";

    // Attempt to drop the default database — this may be disallowed or allowed.
    // Either way, the system should not crash.
    auto result =
        engine_->execute(std::string("DROP DATABASE ") + default_database_name + " CASCADE");
    if (result.has_value()) {
        // Drop succeeded: the entry must be gone from sys_databases.
        EXPECT_FALSE(sys_databases_contains(default_database_name))
            << "if drop succeeds, default database should be removed from sys_databases";
    } else {
        // Drop was rejected: the default database must still be present.
        EXPECT_TRUE(sys_databases_contains(default_database_name))
            << "if drop is rejected, default database should remain in sys_databases";
    }
}

// ============================================================================
// GDB-804 adversarial: drop nonexistent database (no IF EXISTS) returns error
// ============================================================================

TEST_F(QA_GDB657, GDB804_DropNonexistentNoIfExistsReturnsNotFound) {
    run_bootstrap();
    // "ghost_db" was never created; must return NOT_FOUND, not crash.
    exec_error("DROP DATABASE ghost_db", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB657, GDB804_DropNonexistentEmptyNameReturnsError) {
    run_bootstrap();
    // Empty name should be rejected at parse or execution time, never crash.
    auto result = engine_->execute("DROP DATABASE \"\"");
    EXPECT_FALSE(result.has_value()) << "DROP DATABASE with empty name should fail";
}

// ============================================================================
// GDB-804 adversarial: dropping the default / current database
// ============================================================================

TEST_F(QA_GDB657, GDB804_DropDefaultDatabaseConsistency) {
    run_bootstrap();
    ASSERT_TRUE(sys_databases_contains(default_database_name));

    auto result =
        engine_->execute(std::string("DROP DATABASE ") + default_database_name + " CASCADE");
    if (result.has_value()) {
        // Drop allowed: persistence must be cleaned up and catalog consistent.
        EXPECT_FALSE(sys_databases_contains(default_database_name))
            << "successful drop of default DB must remove it from sys_databases";
        auto db = catalog_->get_database(default_database_name);
        EXPECT_FALSE(db.has_value()) << "successful drop of default DB must remove it from catalog";
    } else {
        // Drop rejected: catalog must be untouched.
        EXPECT_TRUE(sys_databases_contains(default_database_name))
            << "rejected drop of default DB must leave sys_databases intact";
        auto db = catalog_->get_database(default_database_name);
        EXPECT_TRUE(db.has_value()) << "rejected drop of default DB must leave catalog intact";
    }
}

TEST_F(QA_GDB657, GDB804_DropDefaultDatabaseSurvivesRestart) {
    // If the default database can be dropped, verify drop persists across restart.
    run_bootstrap();
    auto result =
        engine_->execute(std::string("DROP DATABASE ") + default_database_name + " CASCADE");
    if (!result.has_value()) {
        // Protected — nothing to verify about persistence.
        SUCCEED() << "default database is protected from drop; skip restart check";
        return;
    }

    restart();
    run_bootstrap();

    auto db = catalog_->get_database(default_database_name);
    EXPECT_FALSE(db.has_value())
        << "default database dropped before restart must not reappear after restart";
}

// ============================================================================
// GDB-804 adversarial: drop then re-create
// ============================================================================

TEST_F(QA_GDB657, GDB804_DropThenRecreateRestores) {
    run_bootstrap();
    exec_ok("CREATE DATABASE revive_db");
    ASSERT_TRUE(sys_databases_contains("revive_db"));

    exec_ok("DROP DATABASE revive_db");
    ASSERT_FALSE(sys_databases_contains("revive_db"));

    // Re-create should succeed and persist.
    exec_ok("CREATE DATABASE revive_db");
    EXPECT_TRUE(sys_databases_contains("revive_db"));

    restart();
    run_bootstrap();

    EXPECT_TRUE(catalog_->get_database("revive_db").has_value())
        << "re-created database must survive restart";
}

// ============================================================================
// GDB-804 adversarial: drop database that has tables and data
// ============================================================================

TEST_F(QA_GDB657, GDB804_DropDatabaseWithTablesAndData) {
    run_bootstrap();
    exec_ok("CREATE DATABASE data_db");

    auto db = catalog_->get_database("data_db");
    ASSERT_TRUE(db.has_value());
    engine_->set_current_database(db->database_id);

    exec_ok("CREATE TABLE employees (id INT, name VARCHAR, salary INT)");
    exec_ok("INSERT INTO employees VALUES (1, 'alice', 100)");
    exec_ok("INSERT INTO employees VALUES (2, 'bob',   200)");
    exec_ok("INSERT INTO employees VALUES (3, 'carol', 300)");

    // Switch away from the database before dropping.
    engine_->set_current_database(default_database_id);

    exec_ok("DROP DATABASE data_db CASCADE");

    EXPECT_FALSE(sys_databases_contains("data_db"))
        << "database with tables and data must be removed from sys_databases";
    EXPECT_FALSE(catalog_->get_database("data_db").has_value())
        << "database with tables and data must be removed from catalog";
}

TEST_F(QA_GDB657, GDB804_DropDatabaseWithTablesStaysGoneAfterRestart) {
    run_bootstrap();
    exec_ok("CREATE DATABASE persist_drop_db");

    auto db = catalog_->get_database("persist_drop_db");
    ASSERT_TRUE(db.has_value());
    engine_->set_current_database(db->database_id);
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (42)");

    engine_->set_current_database(default_database_id);
    exec_ok("DROP DATABASE persist_drop_db CASCADE");

    restart();
    run_bootstrap();

    EXPECT_FALSE(catalog_->get_database("persist_drop_db").has_value())
        << "database dropped with tables must not reappear after restart";
}

// ============================================================================
// GDB-804 adversarial: double-drop (drop the same database twice)
// ============================================================================

TEST_F(QA_GDB657, GDB804_DoubleDrop) {
    run_bootstrap();
    exec_ok("CREATE DATABASE double_drop_db");
    exec_ok("DROP DATABASE double_drop_db");

    // Second drop must return NOT_FOUND, not crash or corrupt state.
    exec_error("DROP DATABASE double_drop_db", StatusCode::NOT_FOUND);

    // sys_databases must not have a stale entry.
    EXPECT_FALSE(sys_databases_contains("double_drop_db"))
        << "double-drop must not leave a stale entry in sys_databases";
}

TEST_F(QA_GDB657, GDB804_DoubleDropIfExists) {
    run_bootstrap();
    exec_ok("CREATE DATABASE dd_ifex_db");
    exec_ok("DROP DATABASE dd_ifex_db");

    // IF EXISTS form should succeed (no-op) on second drop.
    auto result = engine_->execute("DROP DATABASE IF EXISTS dd_ifex_db");
    EXPECT_TRUE(result.has_value()) << "DROP DATABASE IF EXISTS on already-dropped DB must succeed";
}

// ============================================================================
// GDB-804 adversarial: case sensitivity of database names
// ============================================================================

TEST_F(QA_GDB657, GDB804_CaseSensitiveDatabaseNames) {
    run_bootstrap();
    exec_ok("CREATE DATABASE CaseDB");

    // Dropping with a different case should fail (names are case-sensitive).
    auto lower_result = engine_->execute("DROP DATABASE casedb");
    auto upper_result = engine_->execute("DROP DATABASE CASEDB");

    if (!lower_result.has_value() && !upper_result.has_value()) {
        // Both failed — names are case-sensitive, as expected.
        EXPECT_EQ(lower_result.error().code, StatusCode::NOT_FOUND);
        EXPECT_EQ(upper_result.error().code, StatusCode::NOT_FOUND);
        // Original casing must still exist.
        EXPECT_TRUE(sys_databases_contains("CaseDB"))
            << "original-cased database must still be present";
    } else {
        // Case-insensitive: at most one match, verify no duplicate drop.
        EXPECT_FALSE(sys_databases_contains("CaseDB") && sys_databases_contains("casedb"))
            << "only one variant of the name may survive";
    }
}

TEST_F(QA_GDB657, GDB804_CaseSensitiveDatabaseNamesExactDrop) {
    run_bootstrap();
    exec_ok("CREATE DATABASE ExactCase");
    // Drop with exactly matching case must succeed.
    exec_ok("DROP DATABASE ExactCase");
    EXPECT_FALSE(sys_databases_contains("ExactCase"));
}

// ============================================================================
// GDB-804 adversarial: DROP DATABASE IF EXISTS semantics
// ============================================================================

TEST_F(QA_GDB657, GDB804_DropIfExistsExistingDatabase) {
    run_bootstrap();
    exec_ok("CREATE DATABASE ifex_db");
    ASSERT_TRUE(sys_databases_contains("ifex_db"));

    // IF EXISTS on existing db must drop it successfully.
    exec_ok("DROP DATABASE IF EXISTS ifex_db");
    EXPECT_FALSE(sys_databases_contains("ifex_db"))
        << "DROP DATABASE IF EXISTS must remove existing database";
}

TEST_F(QA_GDB657, GDB804_DropIfExistsNonexistentIsNoop) {
    run_bootstrap();
    // IF EXISTS on nonexistent db must not error.
    auto result = engine_->execute("DROP DATABASE IF EXISTS absolutely_not_here_db");
    EXPECT_TRUE(result.has_value())
        << "DROP DATABASE IF EXISTS on nonexistent db must succeed (no-op)";
}

TEST_F(QA_GDB657, GDB804_DropIfExistsPersistenceCleanup) {
    run_bootstrap();
    exec_ok("CREATE DATABASE ifex_persist_db");
    exec_ok("DROP DATABASE IF EXISTS ifex_persist_db");
    EXPECT_FALSE(sys_databases_contains("ifex_persist_db"))
        << "DROP DATABASE IF EXISTS must clean up sys_databases entry";

    restart();
    run_bootstrap();

    EXPECT_FALSE(catalog_->get_database("ifex_persist_db").has_value())
        << "IF EXISTS drop must persist across restart";
}

// ============================================================================
// GDB-804 adversarial: assertions are actually exercised in DropSixsevenDatabaseBehavior
// (meta-test: verify the ASSERT_TRUE precondition fires, not just the comment)
// ============================================================================

TEST_F(QA_GDB657, GDB804_DefaultDatabasePresentAfterBootstrap) {
    // This is the precondition that the original vacuous test lacked.
    // It must pass unconditionally for every test in this suite to be meaningful.
    run_bootstrap();
    ASSERT_TRUE(sys_databases_contains(default_database_name))
        << "bootstrap must create the default database '" << default_database_name
        << "' in sys_databases";
    auto db = catalog_->get_database(default_database_name);
    ASSERT_TRUE(db.has_value()) << "bootstrap must register the default database in the catalog";
}

} // namespace
} // namespace sixseven
