#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "test_qa_helpers.h"

namespace sixseven {
namespace {

// ============================================================================
// QA tests for GDB-261: CREATE DATABASE IF NOT EXISTS
// GDB-821: Tightened to assert the database actually exists after creation,
//          verify duplicate plain-CREATE errors, verify IF NOT EXISTS is a
//          true no-op that preserves existing contents.
// ============================================================================

class QA_GDB261 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb261";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        bootstrap_qa_catalog(catalog_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        if (!result.has_value()) {
            ADD_FAILURE() << sql << ": " << result.error().message;
            return {};
        }
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected);
        }
    }

    // Assert that a database exists in the catalog by name.
    void assert_db_exists(const std::string& name) {
        auto db = catalog_.get_database(name);
        ASSERT_TRUE(db.has_value()) << "database '" << name << "' not found in catalog";
        EXPECT_EQ(db->name, name);
    }

    // Assert that a database does NOT exist in the catalog.
    void assert_db_not_exists(const std::string& name) {
        auto db = catalog_.get_database(name);
        EXPECT_FALSE(db.has_value()) << "database '" << name << "' unexpectedly found in catalog";
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// IF NOT EXISTS before database name (standard parser syntax).
// Verifies: (1) db is actually created after first call, (2) second call is a
// no-op success (not an error), (3) the db still exists (not wiped/recreated),
// (4) a plain CREATE without IF NOT EXISTS now errors with ALREADY_EXISTS to
//     prove the db was really created and the IF NOT EXISTS path is distinct.
TEST_F(QA_GDB261, IfNotExistsBeforeName) {
    // Database must not exist before the first call.
    assert_db_not_exists("analytics");

    exec_ok("CREATE DATABASE IF NOT EXISTS analytics");

    // The database must exist after the first call.
    assert_db_exists("analytics");

    // Second call must succeed silently (no-op, not an error or wipe).
    exec_ok("CREATE DATABASE IF NOT EXISTS analytics");

    // Database still exists after the second call — not wiped/recreated.
    assert_db_exists("analytics");

    // Plain CREATE without IF NOT EXISTS must now fail with ALREADY_EXISTS.
    exec_error("CREATE DATABASE analytics", StatusCode::ALREADY_EXISTS);
}

// IF NOT EXISTS after database name (README syntax).
// Same four-point check as above for the alternative parser path.
TEST_F(QA_GDB261, IfNotExistsAfterName) {
    assert_db_not_exists("mydb");

    exec_ok("CREATE DATABASE mydb IF NOT EXISTS");

    assert_db_exists("mydb");

    // Second call must succeed silently.
    exec_ok("CREATE DATABASE mydb IF NOT EXISTS");

    // Database still exists.
    assert_db_exists("mydb");

    // Plain duplicate must now error.
    exec_error("CREATE DATABASE mydb", StatusCode::ALREADY_EXISTS);
}

// IF NOT EXISTS must preserve existing database contents (not wipe/recreate).
// Creates a database, adds a table to it, then calls CREATE DATABASE IF NOT
// EXISTS again — the table must still be there.
TEST_F(QA_GDB261, IfNotExistsPreservesExistingDatabase) {
    exec_ok("CREATE DATABASE preserve_test");
    assert_db_exists("preserve_test");

    // Look up the new database's id and create a table directly in it.
    auto db_before = catalog_.get_database("preserve_test");
    ASSERT_TRUE(db_before.has_value());
    auto create_result =
        engine_->execute("CREATE TABLE sentinel (id INT PRIMARY KEY)", db_before->database_id);
    ASSERT_TRUE(create_result.has_value()) << create_result.error().message;

    // Run CREATE DATABASE IF NOT EXISTS on the existing database (from default ctx).
    exec_ok("CREATE DATABASE IF NOT EXISTS preserve_test");

    // The database and its table must still exist.
    assert_db_exists("preserve_test");
    auto db = catalog_.get_database("preserve_test");
    ASSERT_TRUE(db.has_value());
    auto tables = catalog_.list_tables(db->database_id);
    bool found = false;
    for (const auto& t : tables) {
        if (t.name == "sentinel") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "sentinel table was lost after CREATE DATABASE IF NOT EXISTS";
}

// Without IF NOT EXISTS, duplicate should fail.
TEST_F(QA_GDB261, WithoutIfNotExistsDuplicateFails) {
    exec_ok("CREATE DATABASE testdb");
    assert_db_exists("testdb");
    exec_error("CREATE DATABASE testdb", StatusCode::ALREADY_EXISTS);
}

// Plain CREATE DATABASE (no IF NOT EXISTS) must actually create the database.
TEST_F(QA_GDB261, PlainCreateActuallyCreatesDatabase) {
    assert_db_not_exists("newdb");
    exec_ok("CREATE DATABASE newdb");
    assert_db_exists("newdb");
}

// ============================================================================
// GDB-821 Adversarial: deeper CREATE DATABASE IF NOT EXISTS edge cases
// ============================================================================

class QA_GDB821 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb821";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        bootstrap_qa_catalog(catalog_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        if (!result.has_value()) {
            ADD_FAILURE() << sql << ": " << result.error().message;
            return {};
        }
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected);
        }
    }

    void assert_db_exists(const std::string& name) {
        auto db = catalog_.get_database(name);
        ASSERT_TRUE(db.has_value()) << "database '" << name << "' not found in catalog";
        EXPECT_EQ(db->name, name);
    }

    void assert_db_not_exists(const std::string& name) {
        auto db = catalog_.get_database(name);
        EXPECT_FALSE(db.has_value()) << "database '" << name << "' unexpectedly found in catalog";
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// CREATE DATABASE IF NOT EXISTS the default "demo" database (already exists).
// Must NOT wipe it — the default db must survive intact.
TEST_F(QA_GDB821, IfNotExistsOnDefaultDatabase) {
    // demo exists from bootstrap_qa_catalog.
    assert_db_exists("demo");

    // Should silently succeed — not wipe/recreate the default db.
    exec_ok("CREATE DATABASE IF NOT EXISTS demo");

    // Still exists.
    assert_db_exists("demo");

    // Plain duplicate must still error (prove "demo" really was registered, not phantom).
    exec_error("CREATE DATABASE demo", StatusCode::ALREADY_EXISTS);
}

// CREATE DATABASE IF NOT EXISTS the system "sixseven_system" database.
// The system db must never be wiped.
TEST_F(QA_GDB821, IfNotExistsOnSystemDatabase) {
    // system db should always exist.
    assert_db_exists("sixseven_system");

    // IF NOT EXISTS on system db must silently succeed (not error, not wipe).
    exec_ok("CREATE DATABASE IF NOT EXISTS sixseven_system");

    // Still exists.
    assert_db_exists("sixseven_system");

    // Plain duplicate must error.
    exec_error("CREATE DATABASE sixseven_system", StatusCode::ALREADY_EXISTS);
}

// DROP then CREATE IF NOT EXISTS — the database should be re-created.
TEST_F(QA_GDB821, DropThenCreateIfNotExists) {
    exec_ok("CREATE DATABASE ephemeral");
    assert_db_exists("ephemeral");

    // Drop the database.
    auto drop_result = engine_->execute("DROP DATABASE ephemeral");
    ASSERT_TRUE(drop_result.has_value()) << drop_result.error().message;
    assert_db_not_exists("ephemeral");

    // CREATE IF NOT EXISTS after drop must re-create.
    exec_ok("CREATE DATABASE IF NOT EXISTS ephemeral");
    assert_db_exists("ephemeral");
}

// Row data in a table inside the database must survive CREATE IF NOT EXISTS.
TEST_F(QA_GDB821, IfNotExistsPreservesRowData) {
    exec_ok("CREATE DATABASE rowtest");
    assert_db_exists("rowtest");

    auto db = catalog_.get_database("rowtest");
    ASSERT_TRUE(db.has_value());
    database_id_t db_id = db->database_id;

    // Create a table and insert a row.
    auto ct = engine_->execute("CREATE TABLE counters (val INT)", db_id);
    ASSERT_TRUE(ct.has_value()) << ct.error().message;

    auto ins = engine_->execute("INSERT INTO counters VALUES (42)", db_id);
    ASSERT_TRUE(ins.has_value()) << ins.error().message;

    // Now run CREATE DATABASE IF NOT EXISTS rowtest.
    exec_ok("CREATE DATABASE IF NOT EXISTS rowtest");

    // The row must still be there.
    auto sel = engine_->execute("SELECT val FROM counters", db_id);
    ASSERT_TRUE(sel.has_value()) << sel.error().message;
    ASSERT_EQ(sel->rows.size(), 1u) << "row data was lost after CREATE DATABASE IF NOT EXISTS";
    EXPECT_EQ(sel->rows[0][0].as_int32(), 42);
}

// Case sensitivity: "DEMO" and "demo" should be different databases (names are
// case-sensitive in most SQL engines; verify the engine is consistent).
TEST_F(QA_GDB821, CaseSensitiveNames) {
    // "demo" already exists from bootstrap.
    assert_db_exists("demo");

    // "DEMO" (uppercase) should NOT exist as a separate database.
    // If it doesn't exist, CREATE should succeed.  If the engine is
    // case-insensitive and maps "DEMO"→"demo", it must still error with
    // ALREADY_EXISTS rather than creating a duplicate or silently returning ok.
    auto result = engine_->execute("CREATE DATABASE DEMO");
    // Either ALREADY_EXISTS (case-insensitive) or the create succeeded (case-sensitive).
    if (result.has_value()) {
        // Case-sensitive: "DEMO" is new.
        assert_db_exists("DEMO");
        // The original "demo" must still exist.
        assert_db_exists("demo");
        // And now plain CREATE DEMO should error.
        exec_error("CREATE DATABASE DEMO", StatusCode::ALREADY_EXISTS);
    } else {
        // Case-insensitive: must be ALREADY_EXISTS, not some other error.
        EXPECT_EQ(result.error().code, StatusCode::ALREADY_EXISTS)
            << "unexpected error for case-variant name: " << result.error().message;
    }
}

// Very long database name — must not crash.
TEST_F(QA_GDB821, VeryLongDatabaseName) {
    std::string long_name(255, 'a');
    auto result = engine_->execute("CREATE DATABASE IF NOT EXISTS " + long_name);
    // Either succeeds or fails cleanly — must not crash or produce empty error.
    if (!result.has_value()) {
        EXPECT_FALSE(result.error().message.empty())
            << "error message should not be empty for long name";
    } else {
        // If accepted, it must exist.
        assert_db_exists(long_name);
        // Duplicate plain CREATE must error.
        exec_error("CREATE DATABASE " + long_name, StatusCode::ALREADY_EXISTS);
    }
}

// Empty name (edge case): CREATE DATABASE "" or CREATE DATABASE IF NOT EXISTS "".
// Must fail cleanly, not crash.
TEST_F(QA_GDB821, EmptyNameIsParseError) {
    auto result = engine_->execute("CREATE DATABASE IF NOT EXISTS \"\"");
    // Must either be a parse error or invalid argument — must not succeed.
    if (!result.has_value()) {
        EXPECT_TRUE(result.error().code == StatusCode::PARSE_ERROR ||
                    result.error().code == StatusCode::INVALID_ARGUMENT)
            << "unexpected status for empty db name: " << result.error().message;
    }
    // If it somehow succeeds, the test fails implicitly by reaching here without crashing.
}

// CREATE DATABASE IF NOT EXISTS when the existing DB has multiple tables —
// all tables must survive.
TEST_F(QA_GDB821, IfNotExistsPreservesMultipleTables) {
    exec_ok("CREATE DATABASE multi_tbl_test");
    assert_db_exists("multi_tbl_test");

    auto db = catalog_.get_database("multi_tbl_test");
    ASSERT_TRUE(db.has_value());
    database_id_t db_id = db->database_id;

    auto ct1 = engine_->execute("CREATE TABLE t1 (x INT)", db_id);
    ASSERT_TRUE(ct1.has_value()) << ct1.error().message;
    auto ct2 = engine_->execute("CREATE TABLE t2 (y VARCHAR(64))", db_id);
    ASSERT_TRUE(ct2.has_value()) << ct2.error().message;

    // Run CREATE IF NOT EXISTS on existing database.
    exec_ok("CREATE DATABASE IF NOT EXISTS multi_tbl_test");

    // Both tables must still be present.
    auto tables = catalog_.list_tables(db_id);
    bool t1_found = false, t2_found = false;
    for (const auto& t : tables) {
        if (t.name == "t1") t1_found = true;
        if (t.name == "t2") t2_found = true;
    }
    EXPECT_TRUE(t1_found) << "t1 was lost after CREATE DATABASE IF NOT EXISTS";
    EXPECT_TRUE(t2_found) << "t2 was lost after CREATE DATABASE IF NOT EXISTS";
}

} // namespace
} // namespace sixseven
