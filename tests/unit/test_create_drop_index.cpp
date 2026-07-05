#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
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

class CreateDropIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_create_drop_index";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        // Create a test table for index operations.
        exec_ok("CREATE TABLE users (id INT, email VARCHAR, age INT)");
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    /// Helper: execute SQL, assert success, return the result.
    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    /// Helper: execute SQL, assert failure with the expected code.
    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, expected);
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// CREATE INDEX
// =============================================================================

TEST_F(CreateDropIndexTest, CreateIndexBasic) {
    auto qr = exec_ok("CREATE INDEX idx_email ON users(email)");
    EXPECT_EQ(qr.message, "CREATE INDEX");

    // Verify the index exists in the catalog.
    auto idx = catalog_.get_index(default_database_id, "idx_email");
    ASSERT_TRUE(idx.has_value()) << idx.error().message;
    EXPECT_EQ(idx->name, "idx_email");
    EXPECT_EQ(idx->columns, "email");
    EXPECT_FALSE(idx->is_unique);
    EXPECT_EQ(idx->index_type, "btree");
}

TEST_F(CreateDropIndexTest, CreateUniqueIndex) {
    auto qr = exec_ok("CREATE UNIQUE INDEX idx_email_unique ON users(email)");
    EXPECT_EQ(qr.message, "CREATE INDEX");

    auto idx = catalog_.get_index(default_database_id, "idx_email_unique");
    ASSERT_TRUE(idx.has_value()) << idx.error().message;
    EXPECT_TRUE(idx->is_unique);
}

TEST_F(CreateDropIndexTest, CreateIndexMultipleColumns) {
    auto qr = exec_ok("CREATE INDEX idx_multi ON users(email, age)");
    EXPECT_EQ(qr.message, "CREATE INDEX");

    auto idx = catalog_.get_index(default_database_id, "idx_multi");
    ASSERT_TRUE(idx.has_value()) << idx.error().message;
    EXPECT_EQ(idx->columns, "email,age");
}

TEST_F(CreateDropIndexTest, CreateIndexUsingMethod) {
    auto qr = exec_ok("CREATE INDEX idx_hash ON users(email) USING hash");
    EXPECT_EQ(qr.message, "CREATE INDEX");

    auto idx = catalog_.get_index(default_database_id, "idx_hash");
    ASSERT_TRUE(idx.has_value()) << idx.error().message;
    EXPECT_EQ(idx->index_type, "hash");
}

// GDB-1221: index method normalization (query_engine.cpp) must fold case the
// same way regardless of how the user writes the USING clause.
TEST_F(CreateDropIndexTest, CreateIndexUsingMethodUppercase) {
    auto qr = exec_ok("CREATE INDEX idx_hash_upper ON users(email) USING HASH");
    EXPECT_EQ(qr.message, "CREATE INDEX");

    auto idx = catalog_.get_index(default_database_id, "idx_hash_upper");
    ASSERT_TRUE(idx.has_value()) << idx.error().message;
    EXPECT_EQ(idx->index_type, "hash");
}

TEST_F(CreateDropIndexTest, CreateIndexUsingMethodMixedCase) {
    auto qr = exec_ok("CREATE INDEX idx_hash_mixed ON users(email) USING HaSh");
    EXPECT_EQ(qr.message, "CREATE INDEX");

    auto idx = catalog_.get_index(default_database_id, "idx_hash_mixed");
    ASSERT_TRUE(idx.has_value()) << idx.error().message;
    EXPECT_EQ(idx->index_type, "hash");
}

TEST_F(CreateDropIndexTest, CreateIndexIfNotExistsNew) {
    auto qr = exec_ok("CREATE INDEX IF NOT EXISTS idx_email ON users(email)");
    EXPECT_EQ(qr.message, "CREATE INDEX");

    auto idx = catalog_.get_index(default_database_id, "idx_email");
    ASSERT_TRUE(idx.has_value()) << idx.error().message;
}

TEST_F(CreateDropIndexTest, CreateIndexIfNotExistsAlreadyExists) {
    exec_ok("CREATE INDEX idx_email ON users(email)");

    // Should succeed silently with IF NOT EXISTS.
    auto qr = exec_ok("CREATE INDEX IF NOT EXISTS idx_email ON users(email)");
    EXPECT_EQ(qr.message, "CREATE INDEX");
}

TEST_F(CreateDropIndexTest, CreateIndexDuplicateNameError) {
    exec_ok("CREATE INDEX idx_email ON users(email)");
    exec_error("CREATE INDEX idx_email ON users(age)", StatusCode::ALREADY_EXISTS);
}

TEST_F(CreateDropIndexTest, CreateIndexNonexistentTable) {
    exec_error("CREATE INDEX idx_foo ON nonexistent(col)", StatusCode::NOT_FOUND);
}

TEST_F(CreateDropIndexTest, CreateIndexNonexistentColumn) {
    exec_error("CREATE INDEX idx_foo ON users(nonexistent)", StatusCode::NOT_FOUND);
}

TEST_F(CreateDropIndexTest, CreateIndexDefaultMethodIsBtree) {
    exec_ok("CREATE INDEX idx_email ON users(email)");

    auto idx = catalog_.get_index(default_database_id, "idx_email");
    ASSERT_TRUE(idx.has_value()) << idx.error().message;
    EXPECT_EQ(idx->index_type, "btree");
}

// =============================================================================
// DROP INDEX
// =============================================================================

TEST_F(CreateDropIndexTest, DropIndexBasic) {
    exec_ok("CREATE INDEX idx_email ON users(email)");

    auto qr = exec_ok("DROP INDEX idx_email");
    EXPECT_EQ(qr.message, "DROP INDEX");

    // Verify the index no longer exists.
    auto idx = catalog_.get_index(default_database_id, "idx_email");
    EXPECT_FALSE(idx.has_value());
}

TEST_F(CreateDropIndexTest, DropIndexIfExistsNotFound) {
    // Should succeed silently with IF EXISTS when index doesn't exist.
    auto qr = exec_ok("DROP INDEX IF EXISTS idx_nonexistent");
    EXPECT_EQ(qr.message, "DROP INDEX");
}

TEST_F(CreateDropIndexTest, DropIndexNonexistentError) {
    exec_error("DROP INDEX idx_nonexistent", StatusCode::NOT_FOUND);
}

TEST_F(CreateDropIndexTest, DropIndexThenRecreate) {
    exec_ok("CREATE INDEX idx_email ON users(email)");
    exec_ok("DROP INDEX idx_email");

    // Should be able to recreate with the same name.
    auto qr = exec_ok("CREATE INDEX idx_email ON users(email)");
    EXPECT_EQ(qr.message, "CREATE INDEX");

    auto idx = catalog_.get_index(default_database_id, "idx_email");
    ASSERT_TRUE(idx.has_value()) << idx.error().message;
}

// =============================================================================
// Integration: CREATE + DROP + table with data
// =============================================================================

TEST_F(CreateDropIndexTest, CreateIndexOnTableWithData) {
    exec_ok("INSERT INTO users VALUES (1, 'alice@example.com', 25)");
    exec_ok("INSERT INTO users VALUES (2, 'bob@example.com', 30)");

    // CREATE INDEX over a populated table must register the index in the catalog
    // with the correct target column. This fixture wires no IndexManager
    // (engine_ is built without set_index_manager), so it deliberately covers
    // only the catalog-bookkeeping half of the lifecycle; the physical backfill
    // of pre-existing rows is covered end to end by
    // IndexManagerTest.CreateAndPopulateAtRuntime in test_index_manager.cpp.
    auto qr = exec_ok("CREATE INDEX idx_email ON users(email)");
    EXPECT_EQ(qr.message, "CREATE INDEX");

    auto idx = catalog_.get_index(default_database_id, "idx_email");
    ASSERT_TRUE(idx.has_value()) << idx.error().message;
    // Pin the metadata so a regression that records the wrong column/table (not
    // just "an index exists") fails here.
    EXPECT_EQ(idx->name, "idx_email");
    EXPECT_EQ(idx->columns, "email");
    EXPECT_EQ(idx->table_id, catalog_.get_table(default_database_id, "users")->table_id);
}

TEST_F(CreateDropIndexTest, MultipleIndexesOnSameTable) {
    exec_ok("CREATE INDEX idx_email ON users(email)");
    exec_ok("CREATE INDEX idx_age ON users(age)");

    auto indexes =
        catalog_.list_indexes(catalog_.get_table(default_database_id, "users")->table_id);
    EXPECT_EQ(indexes.size(), 2u);
}
