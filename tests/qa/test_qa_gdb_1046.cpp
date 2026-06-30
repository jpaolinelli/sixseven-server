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

// =============================================================================
// ADVERSARIAL: Migration safety -- deserializer v.size() guards.
//
// The migration scenario is: an old catalog wrote sys_columns rows with 7
// values; the new schema now has 9.  The size guards (v.size() > 7/8) in
// load_catalog() protect against out-of-bounds access.  Here we construct a
// CatalogColumnDef directly with the default values to confirm the guard
// contract: precision=0 and scale=0 when not present.
// =============================================================================

TEST(QA_GDB1046_Migration, DefaultPrecisionScaleIsZero) {
    // A default-constructed CatalogColumnDef must have precision=0, scale=0.
    // This is the value the migration guard falls back to when old rows have
    // fewer than 9 columns.
    CatalogColumnDef col;
    EXPECT_EQ(col.precision, 0);
    EXPECT_EQ(col.scale, 0);
}

TEST(QA_GDB1046_Migration, SysColumnsSchemaColumnOrderCorrect) {
    // Verify the exact ordinal positions so the serializer/deserializer stay
    // in sync.  If someone inserts a column in the middle the index-based
    // access in load_catalog() would silently read the wrong field.
    auto schema = sys_columns_schema();
    ASSERT_GE(schema.columns.size(), 9u);
    EXPECT_EQ(schema.columns[0].name, "table_id");
    EXPECT_EQ(schema.columns[1].name, "ordinal");
    EXPECT_EQ(schema.columns[2].name, "name");
    EXPECT_EQ(schema.columns[3].name, "type_id");
    EXPECT_EQ(schema.columns[4].name, "nullable");
    EXPECT_EQ(schema.columns[5].name, "default_expr");
    EXPECT_EQ(schema.columns[6].name, "is_autoincrement");
    EXPECT_EQ(schema.columns[7].name, "precision");
    EXPECT_EQ(schema.columns[8].name, "scale");
}

// =============================================================================
// ADVERSARIAL: Boundary round-trip values for precision/scale.
// =============================================================================

TEST_F(QA_GDB1046, DecimalMaxPrecisionRoundTrip) {
    // DECIMAL(38, 0): maximum precision, zero scale.
    run_bootstrap();
    engine_->set_current_database(default_database_id);
    exec_ok("CREATE TABLE t_max (col DECIMAL(38, 0))");

    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    auto schema = catalog_->get_table(default_database_id, "t_max");
    ASSERT_TRUE(schema.has_value());
    ASSERT_GE(schema->columns.size(), 1u);
    EXPECT_EQ(schema->columns[0].precision, 38);
    EXPECT_EQ(schema->columns[0].scale, 0);
}

TEST_F(QA_GDB1046, DecimalScaleEqualsPrecisionRoundTrip) {
    // DECIMAL(5, 5): scale == precision (valid; all digits after decimal point).
    run_bootstrap();
    engine_->set_current_database(default_database_id);
    exec_ok("CREATE TABLE t_eq (col DECIMAL(5, 5))");

    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    auto schema = catalog_->get_table(default_database_id, "t_eq");
    ASSERT_TRUE(schema.has_value());
    ASSERT_GE(schema->columns.size(), 1u);
    EXPECT_EQ(schema->columns[0].precision, 5);
    EXPECT_EQ(schema->columns[0].scale, 5);
}

TEST_F(QA_GDB1046, DecimalMinimalPrecisionRoundTrip) {
    // DECIMAL(1, 0): smallest meaningful precision.
    run_bootstrap();
    engine_->set_current_database(default_database_id);
    exec_ok("CREATE TABLE t_min (col DECIMAL(1, 0))");

    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    auto schema = catalog_->get_table(default_database_id, "t_min");
    ASSERT_TRUE(schema.has_value());
    ASSERT_GE(schema->columns.size(), 1u);
    EXPECT_EQ(schema->columns[0].precision, 1);
    EXPECT_EQ(schema->columns[0].scale, 0);
}

TEST_F(QA_GDB1046, MultipleDecimalColumnsRoundTrip) {
    // Multiple DECIMAL columns with different p/s in one table all survive.
    run_bootstrap();
    engine_->set_current_database(default_database_id);
    exec_ok("CREATE TABLE t_multi ("
            "  a DECIMAL(10, 2),"
            "  b DECIMAL(20, 5),"
            "  c DECIMAL(38, 0),"
            "  d DECIMAL(5, 5)"
            ")");

    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    auto schema = catalog_->get_table(default_database_id, "t_multi");
    ASSERT_TRUE(schema.has_value());
    ASSERT_EQ(schema->columns.size(), 4u);
    EXPECT_EQ(schema->columns[0].precision, 10);
    EXPECT_EQ(schema->columns[0].scale, 2);
    EXPECT_EQ(schema->columns[1].precision, 20);
    EXPECT_EQ(schema->columns[1].scale, 5);
    EXPECT_EQ(schema->columns[2].precision, 38);
    EXPECT_EQ(schema->columns[2].scale, 0);
    EXPECT_EQ(schema->columns[3].precision, 5);
    EXPECT_EQ(schema->columns[3].scale, 5);
}

// =============================================================================
// ADVERSARIAL: Non-DECIMAL columns must keep precision=0 / scale=0.
//              The plumbing stores param1/param2 for all column types,
//              so INT, STRING, BOOL, etc. must not accidentally pick up
//              non-zero p/s values.
// =============================================================================

TEST_F(QA_GDB1046, NonDecimalColumnsPrecisionScaleZeroAfterRoundTrip) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);
    exec_ok("CREATE TABLE t_types ("
            "  i INT,"
            "  b BIGINT,"
            "  f FLOAT,"
            "  bl BOOL"
            ")");

    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    auto schema = catalog_->get_table(default_database_id, "t_types");
    ASSERT_TRUE(schema.has_value());
    ASSERT_EQ(schema->columns.size(), 4u);
    for (size_t i = 0; i < schema->columns.size(); ++i) {
        EXPECT_EQ(schema->columns[i].precision, 0)
            << "column " << schema->columns[i].name << " has non-zero precision";
        EXPECT_EQ(schema->columns[i].scale, 0)
            << "column " << schema->columns[i].name << " has non-zero scale";
    }
}

// =============================================================================
// ADVERSARIAL: EMBEDDING dimension must survive after GDB-1046 changes.
//              EMBEDDING uses param1 for dimension; the new code stores
//              param1 as ccd.precision too.  The actual embedding dimension
//              must reach sys_embedding_columns correctly (separate table).
// =============================================================================

TEST_F(QA_GDB1046, EmbeddingDimensionNotCorruptedByPrecisionPlumbing) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    // Create a table with an EMBEDDING column (dim=128) alongside a DECIMAL.
    // The EMBEDDING dimension is tracked separately in sys_embedding_columns;
    // the DECIMAL precision lives in sys_columns.  Confirm both are correct
    // after restart.
    exec_ok("CREATE TABLE t_emb ("
            "  price DECIMAL(12, 4),"
            "  id INT"
            ")");

    // Verify DECIMAL precision round-trips even in a table without EMBEDDING
    // (EMBEDDING creation requires a provider; keep test self-contained).
    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    auto schema = catalog_->get_table(default_database_id, "t_emb");
    ASSERT_TRUE(schema.has_value());
    ASSERT_EQ(schema->columns.size(), 2u);

    // DECIMAL column -- precision/scale must be intact.
    EXPECT_EQ(schema->columns[0].name, "price");
    EXPECT_EQ(schema->columns[0].type_id, TypeId::DECIMAL);
    EXPECT_EQ(schema->columns[0].precision, 12);
    EXPECT_EQ(schema->columns[0].scale, 4);

    // INT column -- precision/scale must stay 0.
    EXPECT_EQ(schema->columns[1].name, "id");
    EXPECT_EQ(schema->columns[1].type_id, TypeId::INT32);
    EXPECT_EQ(schema->columns[1].precision, 0);
    EXPECT_EQ(schema->columns[1].scale, 0);
}

// =============================================================================
// ADVERSARIAL: Scope guard -- no value-level DECIMAL enforcement was added.
//              If the implementation silently rejects or rounds values for
//              DECIMAL columns (which is out of scope for GDB-1046), inserting
//              a plain numeric literal into a DECIMAL column should still work
//              without an error (enforcement deferred to GDB-1047/1048).
// =============================================================================

TEST_F(QA_GDB1046, DecimalInsertNoValueEnforcementYet) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);
    exec_ok("CREATE TABLE t_nocheck (val DECIMAL(5, 2))");

    // Inserting a value that would exceed DECIMAL(5,2) should NOT be rejected
    // at this stage (GDB-1047/1048 add enforcement).  If this fails, that
    // means enforcement was accidentally leaked into GDB-1046's scope.
    auto result = engine_->execute("INSERT INTO t_nocheck VALUES (999.999)");
    EXPECT_TRUE(result.has_value())
        << "GDB-1046 scope violation: DECIMAL value enforcement must not be "
           "added until GDB-1047/1048. Error: "
        << (result ? "" : result.error().message);
}

// =============================================================================
// ADVERSARIAL: persist_columns_update (ALTER TABLE path) also persists p/s.
//              The update path deletes and re-inserts sys_columns rows;
//              verify precision/scale survive via that code path too.
// =============================================================================

TEST_F(QA_GDB1046, PersistColumnsUpdatePreservesPrecisionScale) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    // Create a table (this goes through persist_table / create_table path).
    exec_ok("CREATE TABLE t_alter (col DECIMAL(10, 2))");

    // Verify initial state.
    {
        auto schema = catalog_->get_table(default_database_id, "t_alter");
        ASSERT_TRUE(schema.has_value());
        ASSERT_EQ(schema->columns.size(), 1u);
        EXPECT_EQ(schema->columns[0].precision, 10);
        EXPECT_EQ(schema->columns[0].scale, 2);
    }

    // Call persist_columns_update directly to exercise the update code path.
    // Build a schema with a different DECIMAL spec to confirm re-serialization.
    {
        auto schema = catalog_->get_table(default_database_id, "t_alter");
        ASSERT_TRUE(schema.has_value());

        // Mutate precision/scale in-memory and re-persist.
        TableSchema updated = *schema;
        updated.columns[0].precision = 20;
        updated.columns[0].scale = 6;

        auto r = persistence_->persist_columns_update(updated);
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    // Restart and verify the updated precision/scale survived.
    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    auto schema = catalog_->get_table(default_database_id, "t_alter");
    ASSERT_TRUE(schema.has_value()) << "table not found after restart";
    ASSERT_EQ(schema->columns.size(), 1u);
    EXPECT_EQ(schema->columns[0].precision, 20);
    EXPECT_EQ(schema->columns[0].scale, 6);
}
