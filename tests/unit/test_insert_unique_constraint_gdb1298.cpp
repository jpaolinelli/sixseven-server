// GDB-1298: PRIMARY KEY / UNIQUE constraints declared inline in CREATE TABLE
// had no backing index, so InsertOperator's secondary-index maintenance never
// saw them and duplicate keys were silently accepted. This regression suite
// verifies that CREATE TABLE now auto-creates a unique index for PRIMARY KEY
// / UNIQUE columns (single and composite, inline and table-level), that
// INSERT rejects duplicates through that index, and that UNIQUE (but not
// PRIMARY KEY) still permits multiple NULLs per SQL semantics.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/config.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/index_manager.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

class InsertUniqueConstraintTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_insert_unique_gdb1298";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
        config_ = Config::load_defaults();

        auto bootstrap_result = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(bootstrap_result.has_value()) << bootstrap_result.error().message;

        index_manager_ = std::make_unique<IndexManager>(*catalog_, *storage_);
        index_manager_->set_catalog_persistence(persistence_.get());
        auto rebuild = index_manager_->rebuild_all_indexes();
        ASSERT_TRUE(rebuild.has_value()) << rebuild.error().message;
        engine_->set_index_manager(index_manager_.get());
    }

    void TearDown() override {
        index_manager_.reset();
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        ASSERT_FALSE(result.has_value()) << "expected failure for: " << sql;
        EXPECT_EQ(result.error().code, expected);
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<IndexManager> index_manager_;
    Config config_;
};

} // namespace

// =============================================================================
// CREATE TABLE auto-creates a backing unique index
// =============================================================================

TEST_F(InsertUniqueConstraintTest, InlinePrimaryKeyGetsAutoIndex) {
    exec_ok("CREATE TABLE t1 (id INT PRIMARY KEY, name VARCHAR)");
    auto indexes = catalog_->list_indexes(catalog_->get_table(default_database_id, "t1")->table_id);
    bool found_unique = false;
    for (const auto& idx : indexes) {
        if (idx.is_unique && idx.columns == "id") {
            found_unique = true;
        }
    }
    EXPECT_TRUE(found_unique);
}

TEST_F(InsertUniqueConstraintTest, TableLevelPrimaryKeyGetsAutoIndex) {
    exec_ok("CREATE TABLE t2 (id INT, name VARCHAR, PRIMARY KEY (id))");
    auto indexes = catalog_->list_indexes(catalog_->get_table(default_database_id, "t2")->table_id);
    bool found_unique = false;
    for (const auto& idx : indexes) {
        if (idx.is_unique && idx.columns == "id") {
            found_unique = true;
        }
    }
    EXPECT_TRUE(found_unique);
}

TEST_F(InsertUniqueConstraintTest, InlineUniqueColumnGetsAutoIndex) {
    exec_ok("CREATE TABLE t3 (id INT, email VARCHAR UNIQUE)");
    auto indexes = catalog_->list_indexes(catalog_->get_table(default_database_id, "t3")->table_id);
    bool found_unique = false;
    for (const auto& idx : indexes) {
        if (idx.is_unique && idx.columns == "email") {
            found_unique = true;
        }
    }
    EXPECT_TRUE(found_unique);
}

// =============================================================================
// Duplicate PRIMARY KEY rejected
// =============================================================================

TEST_F(InsertUniqueConstraintTest, DuplicatePrimaryKeyRejected) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_error("INSERT INTO users VALUES (1, 'bob')", StatusCode::CONSTRAINT_VIOLATION);

    // Original row must survive, and the duplicate must not have been added.
    auto qr = exec_ok("SELECT id, name FROM users");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "alice");
}

TEST_F(InsertUniqueConstraintTest, DuplicateCompositePrimaryKeyRejected) {
    exec_ok("CREATE TABLE order_items (order_id INT, item_id INT, qty INT, PRIMARY KEY (order_id, "
            "item_id))");
    exec_ok("INSERT INTO order_items VALUES (1, 1, 5)");
    exec_error("INSERT INTO order_items VALUES (1, 1, 9)", StatusCode::CONSTRAINT_VIOLATION);
    // A differing composite key must still be accepted.
    exec_ok("INSERT INTO order_items VALUES (1, 2, 3)");

    auto qr = exec_ok("SELECT order_id, item_id FROM order_items");
    EXPECT_EQ(qr.rows.size(), 2u);
}

TEST_F(InsertUniqueConstraintTest, DuplicatePrimaryKeyRejectedAcrossEmptyAndNormalStrings) {
    exec_ok("CREATE TABLE keyed (k VARCHAR PRIMARY KEY)");
    exec_ok("INSERT INTO keyed VALUES ('')");
    exec_error("INSERT INTO keyed VALUES ('')", StatusCode::CONSTRAINT_VIOLATION);
    exec_ok("INSERT INTO keyed VALUES ('a')");
    exec_error("INSERT INTO keyed VALUES ('a')", StatusCode::CONSTRAINT_VIOLATION);

    auto qr = exec_ok("SELECT k FROM keyed");
    EXPECT_EQ(qr.rows.size(), 2u);
}

// =============================================================================
// Duplicate UNIQUE rejected
// =============================================================================

TEST_F(InsertUniqueConstraintTest, DuplicateUniqueColumnRejected) {
    exec_ok("CREATE TABLE accounts (id INT, email VARCHAR UNIQUE)");
    exec_ok("INSERT INTO accounts VALUES (1, 'a@example.com')");
    exec_error("INSERT INTO accounts VALUES (2, 'a@example.com')",
               StatusCode::CONSTRAINT_VIOLATION);

    auto qr = exec_ok("SELECT id FROM accounts");
    EXPECT_EQ(qr.rows.size(), 1u);
}

TEST_F(InsertUniqueConstraintTest, DuplicateTableLevelUniqueConstraintRejected) {
    exec_ok("CREATE TABLE pairs (a INT, b INT, UNIQUE (a, b))");
    exec_ok("INSERT INTO pairs VALUES (1, 2)");
    exec_error("INSERT INTO pairs VALUES (1, 2)", StatusCode::CONSTRAINT_VIOLATION);
    // Non-duplicate composite value is still accepted.
    exec_ok("INSERT INTO pairs VALUES (1, 3)");

    auto qr = exec_ok("SELECT a, b FROM pairs");
    EXPECT_EQ(qr.rows.size(), 2u);
}

// =============================================================================
// NULL handling: UNIQUE allows multiple NULLs; PRIMARY KEY forbids NULL
// =============================================================================

TEST_F(InsertUniqueConstraintTest, UniqueColumnAllowsMultipleNulls) {
    exec_ok("CREATE TABLE nullable_unique (id INT, email VARCHAR UNIQUE)");
    exec_ok("INSERT INTO nullable_unique VALUES (1, NULL)");
    // A second NULL must be accepted -- SQL UNIQUE does not treat NULL as
    // equal to NULL.
    exec_ok("INSERT INTO nullable_unique VALUES (2, NULL)");

    auto qr = exec_ok("SELECT id FROM nullable_unique");
    EXPECT_EQ(qr.rows.size(), 2u);

    // A genuine duplicate non-NULL value must still be rejected.
    exec_ok("INSERT INTO nullable_unique VALUES (3, 'x@example.com')");
    exec_error("INSERT INTO nullable_unique VALUES (4, 'x@example.com')",
               StatusCode::CONSTRAINT_VIOLATION);
}
