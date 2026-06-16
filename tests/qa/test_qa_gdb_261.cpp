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

} // namespace
} // namespace sixseven
