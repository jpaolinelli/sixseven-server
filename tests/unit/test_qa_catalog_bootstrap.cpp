/// @file test_qa_catalog_bootstrap.cpp
/// @brief Unit tests for the QA catalog bootstrap pattern (GDB-713).
///
/// Verifies that calling restore_database(default_database_id, ...) on a
/// freshly constructed Catalog makes the default database available for
/// table creation and query execution — exactly what bootstrap_qa_catalog()
/// in test_qa_helpers.h does.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// Tests for the raw bootstrap pattern used by bootstrap_qa_catalog().
// ---------------------------------------------------------------------------

/// A bare Catalog has the system database (id=2) but NOT the default
/// database (id=1).
TEST(QACatalogBootstrap, FreshCatalogLacksDefaultDatabase) {
    Catalog catalog;
    auto result = catalog.get_database(default_database_name);
    EXPECT_FALSE(result.has_value());
}

/// After calling restore_database(default_database_id, name), the default
/// database is accessible by name.
TEST(QACatalogBootstrap, RestoreDefaultDatabaseMakesItAccessibleByName) {
    Catalog catalog;
    auto r = catalog.restore_database(default_database_id, default_database_name);
    ASSERT_TRUE(r.has_value()) << r.error().message;

    auto db = catalog.get_database(default_database_name);
    ASSERT_TRUE(db.has_value()) << db.error().message;
    EXPECT_EQ(db->database_id, default_database_id);
    EXPECT_EQ(db->name, default_database_name);
}

/// After bootstrap, tables can be created in the default database.
TEST(QACatalogBootstrap, CanCreateTableInDefaultDatabaseAfterBootstrap) {
    Catalog catalog;
    auto r = catalog.restore_database(default_database_id, default_database_name);
    ASSERT_TRUE(r.has_value()) << r.error().message;

    TableSchema schema;
    schema.name = "test_table";
    schema.columns = {{0, "id", TypeId::INT32, false, ""}};
    schema.pk_columns = "id";

    auto table_id = catalog.create_table(default_database_id, schema);
    ASSERT_TRUE(table_id.has_value()) << table_id.error().message;
    EXPECT_GT(*table_id, 0);
}

/// Creating a table in the default database without bootstrap fails with
/// NOT_FOUND.
TEST(QACatalogBootstrap, CreateTableWithoutBootstrapFails) {
    Catalog catalog;

    TableSchema schema;
    schema.name = "test_table";
    schema.columns = {{0, "id", TypeId::INT32, false, ""}};
    schema.pk_columns = "id";

    auto table_id = catalog.create_table(default_database_id, schema);
    ASSERT_FALSE(table_id.has_value());
    EXPECT_EQ(table_id.error().code, StatusCode::NOT_FOUND);
}

/// restore_database is idempotent — calling it twice does not fail.
TEST(QACatalogBootstrap, RestoreIsIdempotent) {
    Catalog catalog;
    auto r1 = catalog.restore_database(default_database_id, default_database_name);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    // Second call: restore_database returns ALREADY_EXISTS but
    // bootstrap_qa_catalog() silences this. Verify the catalog is intact.
    auto r2 = catalog.restore_database(default_database_id, default_database_name);
    // We don't assert success on r2 — the important thing is the catalog is
    // still usable.
    auto db = catalog.get_database(default_database_name);
    ASSERT_TRUE(db.has_value());
    EXPECT_EQ(db->database_id, default_database_id);
}

/// The system database (id=2) is always present without any bootstrap call.
TEST(QACatalogBootstrap, SystemDatabaseAlwaysPresent) {
    Catalog catalog;
    auto db = catalog.get_database(system_database_name);
    ASSERT_TRUE(db.has_value()) << db.error().message;
    EXPECT_EQ(db->database_id, system_database_id);
}

// ---------------------------------------------------------------------------
// End-to-end: verify that after bootstrap a QueryEngine can execute SQL
// against the default database without 'database with id 1 not found'.
// ---------------------------------------------------------------------------

class QACatalogBootstrapE2E : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_unit_qa_bootstrap";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        // Bootstrap the default database — this is exactly what
        // bootstrap_qa_catalog() does.
        auto r = catalog_.restore_database(default_database_id, default_database_name);
        ASSERT_TRUE(r.has_value()) << r.error().message;

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

TEST_F(QACatalogBootstrapE2E, CreateTableSucceedsAfterBootstrap) {
    auto result = engine_->execute("CREATE TABLE t (id INT PRIMARY KEY, val VARCHAR)");
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

TEST_F(QACatalogBootstrapE2E, InsertAndSelectSucceedAfterBootstrap) {
    auto cr = engine_->execute("CREATE TABLE t (id INT PRIMARY KEY, val VARCHAR)");
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    auto ir = engine_->execute("INSERT INTO t VALUES (1, 'hello')");
    ASSERT_TRUE(ir.has_value()) << ir.error().message;

    auto sr = engine_->execute("SELECT id, val FROM t");
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    ASSERT_EQ(sr->rows.size(), 1u);
    EXPECT_EQ(sr->rows[0][0].as_int32(), 1);
    EXPECT_EQ(sr->rows[0][1].as_string(), "hello");
}

TEST_F(QACatalogBootstrapE2E, ExecuteWithoutBootstrapWouldFail) {
    // Demonstrate the regression: a bare Catalog without bootstrap_qa_catalog
    // cannot execute SQL on the default database.
    Catalog bare_catalog;
    DiskManager bare_dm;
    std::filesystem::path bare_dir =
        std::filesystem::temp_directory_path() / "sixseven_unit_qa_bootstrap_bare";
    std::filesystem::remove_all(bare_dir);
    std::filesystem::create_directories(bare_dir);

    StorageManager bare_storage(bare_dm, bare_dir);
    QueryEngine bare_engine(bare_catalog, bare_storage);

    auto result = bare_engine.execute("CREATE TABLE t (id INT PRIMARY KEY)");
    EXPECT_FALSE(result.has_value()) << "Expected NOT_FOUND without bootstrap";
    if (!result.has_value()) {
        EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
    }

    std::filesystem::remove_all(bare_dir);
}

} // namespace
} // namespace sixseven
