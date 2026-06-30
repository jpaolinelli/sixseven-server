// GDB-1046: DECIMAL(p,s) precision/scale parsed but silently discarded by catalog.
//
// Regression test verifying that precision and scale survive the full
// CatalogColumnDef -> sys_columns serialization -> deserialization round-trip.
// Prior to the fix the CatalogColumnDef struct had no precision/scale fields,
// so these assertions could not even compile against origin/main -- the
// strongest possible mutation guard.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/config.h"
#include "sixseven/common/types.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

using namespace sixseven;

// =============================================================================
// Fixture: mirrors the pattern from test_qa_gdb_215.cpp
// =============================================================================

class QA_GDB1046 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_1046";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
        config_ = Config::load_defaults();
    }

    void TearDown() override {
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void run_bootstrap() {
        auto result = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }

    void restart() {
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    Config config_;
};

// =============================================================================
// AC: precision/scale survive the CREATE TABLE -> catalog in-memory path
// =============================================================================

TEST_F(QA_GDB1046, DecimalPrecisionScaleStoredInCatalog) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    // DECIMAL(10,2): precision=10, scale=2.
    // INT column: precision=0, scale=0 (unspecified -> default).
    exec_ok("CREATE TABLE t (d DECIMAL(10, 2), n INT)");

    auto schema = catalog_->get_table(default_database_id, "t");
    ASSERT_TRUE(schema.has_value()) << "table 't' not found in catalog";
    ASSERT_EQ(schema->columns.size(), 2u);

    // Column 0: d DECIMAL(10,2)
    EXPECT_EQ(schema->columns[0].name, "d");
    EXPECT_EQ(schema->columns[0].type_id, TypeId::DECIMAL);
    EXPECT_EQ(schema->columns[0].precision, 10);
    EXPECT_EQ(schema->columns[0].scale, 2);

    // Column 1: n INT -- precision/scale default to 0
    EXPECT_EQ(schema->columns[1].name, "n");
    EXPECT_EQ(schema->columns[1].type_id, TypeId::INT32);
    EXPECT_EQ(schema->columns[1].precision, 0);
    EXPECT_EQ(schema->columns[1].scale, 0);
}

// =============================================================================
// AC: precision/scale survive serialization -> deserialization (persistence
//     round-trip).  This is the core regression: old code had no fields to
//     persist or restore, so the round-trip silently dropped them.
// =============================================================================

TEST_F(QA_GDB1046, DecimalPrecisionScaleRoundTripPersistence) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE decimal_rt (price DECIMAL(18, 4), qty DECIMAL(8, 0), id BIGINT)");

    // Verify in-memory state before restart.
    {
        auto schema = catalog_->get_table(default_database_id, "decimal_rt");
        ASSERT_TRUE(schema.has_value());
        ASSERT_EQ(schema->columns.size(), 3u);
        EXPECT_EQ(schema->columns[0].precision, 18);
        EXPECT_EQ(schema->columns[0].scale, 4);
        EXPECT_EQ(schema->columns[1].precision, 8);
        EXPECT_EQ(schema->columns[1].scale, 0);
        EXPECT_EQ(schema->columns[2].precision, 0);
        EXPECT_EQ(schema->columns[2].scale, 0);
    }

    // Simulate server restart: reload catalog from disk.
    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    // Verify post-restart: precision/scale must survive the round-trip.
    auto schema = catalog_->get_table(default_database_id, "decimal_rt");
    ASSERT_TRUE(schema.has_value()) << "table not found after restart";
    ASSERT_EQ(schema->columns.size(), 3u);

    EXPECT_EQ(schema->columns[0].name, "price");
    EXPECT_EQ(schema->columns[0].type_id, TypeId::DECIMAL);
    EXPECT_EQ(schema->columns[0].precision, 18);
    EXPECT_EQ(schema->columns[0].scale, 4);

    EXPECT_EQ(schema->columns[1].name, "qty");
    EXPECT_EQ(schema->columns[1].type_id, TypeId::DECIMAL);
    EXPECT_EQ(schema->columns[1].precision, 8);
    EXPECT_EQ(schema->columns[1].scale, 0);

    EXPECT_EQ(schema->columns[2].name, "id");
    EXPECT_EQ(schema->columns[2].type_id, TypeId::INT64); // BIGINT -> INT64
    EXPECT_EQ(schema->columns[2].precision, 0);
    EXPECT_EQ(schema->columns[2].scale, 0);
}

// =============================================================================
// AC: sys_columns_schema() now declares 9 columns (7 original + precision/scale)
// =============================================================================

TEST(QA_GDB1046_Schema, SysColumnsSchemaHasNineColumns) {
    auto schema = sys_columns_schema();
    ASSERT_EQ(schema.columns.size(), 9u);
    EXPECT_EQ(schema.columns[7].name, "precision");
    EXPECT_EQ(schema.columns[7].type_id, TypeId::INT32);
    EXPECT_EQ(schema.columns[8].name, "scale");
    EXPECT_EQ(schema.columns[8].type_id, TypeId::INT32);
}
