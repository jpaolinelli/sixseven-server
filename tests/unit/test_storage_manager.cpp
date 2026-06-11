#include "sixseven/catalog/schema.h"
#include "sixseven/common/types.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class StorageManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_storage_mgr";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
    }

    void TearDown() override { std::filesystem::remove_all(data_dir_); }

    /// Build a simple single-column table schema for testing.
    TableSchema make_schema(table_id_t tid, const std::string& name) {
        TableSchema ts;
        ts.table_id = tid;
        ts.name = name;
        CatalogColumnDef col;
        col.ordinal = 0;
        col.name = "id";
        col.type_id = TypeId::INT32;
        ts.columns.push_back(col);
        return ts;
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
};

// =============================================================================
// Default database directory
// =============================================================================

TEST_F(StorageManagerTest, CreateDatabaseStorageCreatesDefaultDatabaseDir) {
    StorageManager sm(dm_, data_dir_);

    // Explicitly create the default database storage directory.
    auto result = sm.create_database_storage(default_database_id);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto default_db_dir = data_dir_ / "databases" / "1" / "tables";
    EXPECT_TRUE(std::filesystem::exists(default_db_dir));
    EXPECT_TRUE(std::filesystem::is_directory(default_db_dir));
}

// =============================================================================
// create_database_storage
// =============================================================================

TEST_F(StorageManagerTest, CreateDatabaseStorage) {
    StorageManager sm(dm_, data_dir_);

    auto result = sm.create_database_storage(42);
    ASSERT_TRUE(result.has_value());

    auto db_dir = data_dir_ / "databases" / "42";
    EXPECT_TRUE(std::filesystem::exists(db_dir));
    EXPECT_TRUE(std::filesystem::is_directory(db_dir));

    auto tables_dir = db_dir / "tables";
    EXPECT_TRUE(std::filesystem::exists(tables_dir));
    EXPECT_TRUE(std::filesystem::is_directory(tables_dir));
}

TEST_F(StorageManagerTest, CreateDatabaseStorageIdempotent) {
    StorageManager sm(dm_, data_dir_);

    auto result1 = sm.create_database_storage(42);
    ASSERT_TRUE(result1.has_value());

    // Creating the same directory structure again should succeed.
    auto result2 = sm.create_database_storage(42);
    EXPECT_TRUE(result2.has_value());
}

// =============================================================================
// drop_database_storage
// =============================================================================

TEST_F(StorageManagerTest, DropDatabaseStorage) {
    StorageManager sm(dm_, data_dir_);

    auto create = sm.create_database_storage(42);
    ASSERT_TRUE(create.has_value());

    auto db_dir = data_dir_ / "databases" / "42";
    ASSERT_TRUE(std::filesystem::exists(db_dir));

    auto drop = sm.drop_database_storage(42);
    ASSERT_TRUE(drop.has_value());

    EXPECT_FALSE(std::filesystem::exists(db_dir));
}

TEST_F(StorageManagerTest, DropNonExistentDatabaseStorageSucceeds) {
    StorageManager sm(dm_, data_dir_);

    // Dropping a non-existent database directory should succeed (no-op).
    auto drop = sm.drop_database_storage(999);
    EXPECT_TRUE(drop.has_value());
}

// =============================================================================
// Table storage with database-scoped paths
// =============================================================================

TEST_F(StorageManagerTest, CreateTableStorageUsesDbScopedPath) {
    StorageManager sm(dm_, data_dir_);

    auto schema = make_schema(10, "users");
    auto result = sm.create_table_storage(default_database_id, 10, schema);
    ASSERT_TRUE(result.has_value());

    // Verify the file is in the database-scoped location.
    auto expected_path = data_dir_ / "databases" / "1" / "tables" / "table_10.db";
    EXPECT_TRUE(std::filesystem::exists(expected_path));
}

TEST_F(StorageManagerTest, CreateTableStorageInCustomDatabase) {
    StorageManager sm(dm_, data_dir_);

    auto db_create = sm.create_database_storage(5);
    ASSERT_TRUE(db_create.has_value());

    auto schema = make_schema(20, "orders");
    auto result = sm.create_table_storage(5, 20, schema);
    ASSERT_TRUE(result.has_value());

    auto expected_path = data_dir_ / "databases" / "5" / "tables" / "table_20.db";
    EXPECT_TRUE(std::filesystem::exists(expected_path));
}

TEST_F(StorageManagerTest, CreateTableStorageDuplicate) {
    StorageManager sm(dm_, data_dir_);

    auto schema = make_schema(10, "users");
    auto result1 = sm.create_table_storage(default_database_id, 10, schema);
    ASSERT_TRUE(result1.has_value());

    auto result2 = sm.create_table_storage(default_database_id, 10, schema);
    ASSERT_FALSE(result2.has_value());
    EXPECT_EQ(result2.error().code, StatusCode::ALREADY_EXISTS);
}

TEST_F(StorageManagerTest, GetTableStorage) {
    StorageManager sm(dm_, data_dir_);

    auto schema = make_schema(10, "users");
    auto create = sm.create_table_storage(default_database_id, 10, schema);
    ASSERT_TRUE(create.has_value());

    auto get = sm.get_table_storage(10);
    ASSERT_TRUE(get.has_value());
    EXPECT_NE(*get, nullptr);
    EXPECT_NE((*get)->heap, nullptr);
    EXPECT_NE((*get)->bpm, nullptr);
}

TEST_F(StorageManagerTest, GetTableStorageNotFound) {
    StorageManager sm(dm_, data_dir_);

    auto get = sm.get_table_storage(999);
    ASSERT_FALSE(get.has_value());
    EXPECT_EQ(get.error().code, StatusCode::NOT_FOUND);
}

TEST_F(StorageManagerTest, DropTableStorage) {
    StorageManager sm(dm_, data_dir_);

    auto schema = make_schema(10, "users");
    auto create = sm.create_table_storage(default_database_id, 10, schema);
    ASSERT_TRUE(create.has_value());

    auto expected_path = data_dir_ / "databases" / "1" / "tables" / "table_10.db";
    EXPECT_TRUE(std::filesystem::exists(expected_path));

    auto drop = sm.drop_table_storage(default_database_id, 10);
    ASSERT_TRUE(drop.has_value());

    // File should be removed.
    EXPECT_FALSE(std::filesystem::exists(expected_path));

    // Storage should no longer be accessible.
    auto get = sm.get_table_storage(10);
    EXPECT_FALSE(get.has_value());
}

TEST_F(StorageManagerTest, DropTableStorageNotFound) {
    StorageManager sm(dm_, data_dir_);

    auto drop = sm.drop_table_storage(default_database_id, 999);
    ASSERT_FALSE(drop.has_value());
    EXPECT_EQ(drop.error().code, StatusCode::NOT_FOUND);
}

TEST_F(StorageManagerTest, DropDatabaseStorageRemovesTableFiles) {
    StorageManager sm(dm_, data_dir_);

    auto db_create = sm.create_database_storage(7);
    ASSERT_TRUE(db_create.has_value());

    // Create two tables in database 7.
    auto s1 = make_schema(100, "t1");
    auto s2 = make_schema(101, "t2");
    ASSERT_TRUE(sm.create_table_storage(7, 100, s1).has_value());
    ASSERT_TRUE(sm.create_table_storage(7, 101, s2).has_value());

    auto t1_path = data_dir_ / "databases" / "7" / "tables" / "table_100.db";
    auto t2_path = data_dir_ / "databases" / "7" / "tables" / "table_101.db";
    EXPECT_TRUE(std::filesystem::exists(t1_path));
    EXPECT_TRUE(std::filesystem::exists(t2_path));

    // Drop the entire database directory.
    auto drop = sm.drop_database_storage(7);
    ASSERT_TRUE(drop.has_value());

    // Everything should be gone.
    EXPECT_FALSE(std::filesystem::exists(data_dir_ / "databases" / "7"));
}

TEST_F(StorageManagerTest, MultipleTablesInDifferentDatabases) {
    StorageManager sm(dm_, data_dir_);

    ASSERT_TRUE(sm.create_database_storage(2).has_value());
    ASSERT_TRUE(sm.create_database_storage(3).has_value());

    auto s1 = make_schema(10, "t1");
    auto s2 = make_schema(20, "t2");

    ASSERT_TRUE(sm.create_table_storage(2, 10, s1).has_value());
    ASSERT_TRUE(sm.create_table_storage(3, 20, s2).has_value());

    // Both tables accessible.
    auto g1 = sm.get_table_storage(10);
    auto g2 = sm.get_table_storage(20);
    EXPECT_TRUE(g1.has_value());
    EXPECT_TRUE(g2.has_value());

    // Files at correct locations.
    EXPECT_TRUE(std::filesystem::exists(data_dir_ / "databases" / "2" / "tables" / "table_10.db"));
    EXPECT_TRUE(std::filesystem::exists(data_dir_ / "databases" / "3" / "tables" / "table_20.db"));
}
