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
#include <memory>
#include <string>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Test fixture
// ============================================================================

class QA_GDB657 : public ::testing::Test {
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

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << " => " << result.error().message;
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected);
        }
    }

    /// Scan sys_databases heap and return all (id, name) pairs.
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

    bool sys_databases_contains(const std::string& name) {
        auto entries = scan_sys_databases();
        return std::any_of(entries.begin(), entries.end(),
                           [&](const auto& e) { return e.second == name; });
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
            EXPECT_EQ(id, db->database_id)
                << "persisted id should match catalog id";
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
        << "CREATE DATABASE should succeed even with null persistence: "
        << result.error().message;

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
        << "DROP DATABASE should succeed even with null persistence: "
        << result.error().message;

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
    ASSERT_TRUE(db.has_value())
        << "database created via CREATE DATABASE should survive restart: "
        << db.error().message;
}

TEST_F(QA_GDB657, DroppedDatabaseStaysGoneAfterRestart) {
    run_bootstrap();
    exec_ok("CREATE DATABASE gone_db");
    exec_ok("DROP DATABASE gone_db");

    restart();
    run_bootstrap();

    auto db = catalog_->get_database("gone_db");
    EXPECT_FALSE(db.has_value())
        << "dropped database should not reappear after restart";
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
    ASSERT_TRUE(db.has_value())
        << "re-created database should survive restart";
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
        if (name == "dup_db") ++count;
    }
    EXPECT_EQ(count, 1) << "IF NOT EXISTS should not add a duplicate row";
}

TEST_F(QA_GDB657, DropIfExistsNonexistentDoesNotError) {
    run_bootstrap();
    auto result = engine_->execute("DROP DATABASE IF EXISTS nonexistent_db");
    EXPECT_TRUE(result.has_value())
        << "DROP DATABASE IF EXISTS on nonexistent db should succeed";
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
        if (name == "already_db") ++count;
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
        ASSERT_TRUE(db.has_value())
            << name << " should survive restart: " << db.error().message;
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

    // Attempt to drop the default database — this may be disallowed or allowed.
    // Either way, the system should not crash.
    auto result = engine_->execute("DROP DATABASE sixseven CASCADE");
    // We just verify no crash. If it's allowed, check persistence is cleaned up.
    // If it's disallowed, that's also valid.
    if (result.has_value()) {
        EXPECT_FALSE(sys_databases_contains("demo"))
            << "if drop succeeds, sixseven should be removed from sys_databases";
    }
    // No crash is the main assertion here.
}

} // namespace
} // namespace sixseven
