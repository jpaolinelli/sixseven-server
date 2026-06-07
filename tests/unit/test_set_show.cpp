#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/settings_cache.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/server/replication_slot.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// Test fixture for SET/SHOW execution (GDB-189)
// =============================================================================

class SetShowTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_set_show";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        config_ = Config::load_defaults();
        cache_ = std::make_unique<SettingsCache>();

        // Bootstrap system database + sys_settings.
        auto boot = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(boot.has_value()) << boot.error().message;

        // Load settings cache.
        auto load = cache_->load(*engine_);
        ASSERT_TRUE(load.has_value()) << load.error().message;

        // Wire up the cache.
        engine_->set_settings_cache(cache_.get());

        // Switch to default database for user operations.
        use_database("demo");
    }

    void TearDown() override {
        engine_.reset();
        cache_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value());
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected);
        }
    }

    void use_database(const std::string& name) {
        auto db = catalog_->get_database(name);
        ASSERT_TRUE(db.has_value()) << "database '" << name << "' not found";
        engine_->set_current_database(db->database_id);
    }

    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<SettingsCache> cache_;
    Config config_;
};

// =============================================================================
// AC: SET logging.level = 'debug' changes the log level immediately and persists
// =============================================================================

TEST_F(SetShowTest, SetLoggingLevelChangesAndPersists) {
    auto qr = exec_ok("SET logging.level = 'debug'");
    EXPECT_EQ(qr.message, "SET");

    // Verify the cache was updated.
    auto entry = cache_->get("logging.level");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->value, "debug");

    // Verify it persisted to sys_settings.
    use_database(system_database_name);
    auto check = exec_ok("SELECT value FROM sys_settings WHERE key = 'logging.level'");
    ASSERT_EQ(check.rows.size(), 1u);
    EXPECT_EQ(check.rows[0][0].as_string(), "debug");
}

// =============================================================================
// AC: SET server.port = 5433 returns error (not runtime-mutable)
// =============================================================================

TEST_F(SetShowTest, SetNonMutableReturnsError) {
    exec_error("SET server.port = 5433", StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// AC: SET unknown.param = 'x' returns error (unrecognized)
// =============================================================================

TEST_F(SetShowTest, SetUnknownParamReturnsError) {
    exec_error("SET unknown.param = 'x'", StatusCode::NOT_FOUND);
}

// =============================================================================
// AC: SHOW logging.level returns current value
// =============================================================================

TEST_F(SetShowTest, ShowParameter) {
    auto qr = exec_ok("SHOW logging.level");
    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "key");
    EXPECT_EQ(qr.column_names[1], "value");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "logging.level");
    EXPECT_EQ(qr.rows[0][1].as_string(), config_.log_level);
}

TEST_F(SetShowTest, ShowParameterAfterSet) {
    exec_ok("SET logging.level = 'warn'");

    auto qr = exec_ok("SHOW logging.level");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "warn");
}

TEST_F(SetShowTest, ShowUnknownParameterReturnsError) {
    exec_error("SHOW unknown.param", StatusCode::NOT_FOUND);
}

// =============================================================================
// AC: SHOW ALL / SHOW SETTINGS returns all settings as a result set
// =============================================================================

TEST_F(SetShowTest, ShowAll) {
    auto qr = exec_ok("SHOW ALL");
    ASSERT_EQ(qr.column_names.size(), 4u);
    EXPECT_EQ(qr.column_names[0], "key");
    EXPECT_EQ(qr.column_names[1], "value");
    EXPECT_EQ(qr.column_names[2], "category");
    EXPECT_EQ(qr.column_names[3], "is_runtime_mutable");
    EXPECT_GE(qr.rows.size(), 6u);

    // Verify a known setting is present.
    bool found_log_level = false;
    for (const auto& row : qr.rows) {
        if (row[0].as_string() == "logging.level") {
            found_log_level = true;
            EXPECT_EQ(row[1].as_string(), config_.log_level);
            EXPECT_EQ(row[2].as_string(), "logging");
            EXPECT_TRUE(row[3].as_bool());
        }
    }
    EXPECT_TRUE(found_log_level);
}

TEST_F(SetShowTest, ShowSettings) {
    auto qr = exec_ok("SHOW SETTINGS");
    EXPECT_EQ(qr.column_names.size(), 4u);
    EXPECT_GE(qr.rows.size(), 6u);
}

// =============================================================================
// AC: Settings survive server restart (verified via persistence to sys_settings)
// =============================================================================

TEST_F(SetShowTest, SetPersistsToSysSettings) {
    // Change a setting.
    exec_ok("SET logging.level = 'error'");

    // Verify it persisted to the sys_settings table (not just in-memory cache).
    use_database(system_database_name);
    auto check = exec_ok("SELECT value FROM sys_settings WHERE key = 'logging.level'");
    ASSERT_EQ(check.rows.size(), 1u);
    EXPECT_EQ(check.rows[0][0].as_string(), "error");

    // Verify the cache reflects the persisted value.
    auto entry = cache_->get("logging.level");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->value, "error");
}

TEST_F(SetShowTest, CacheReloadPicksUpPersistedChanges) {
    // Change a setting via SET.
    exec_ok("SET logging.level = 'trace'");

    // Create a fresh cache and reload from sys_settings.
    auto fresh_cache = std::make_unique<SettingsCache>();
    auto load = fresh_cache->load(*engine_);
    ASSERT_TRUE(load.has_value()) << load.error().message;

    // Fresh cache should have the persisted value.
    auto entry = fresh_cache->get("logging.level");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->value, "trace");
}

// =============================================================================
// AC: Existing SHOW TABLES/COLUMNS/INDEXES continue to work
// =============================================================================

TEST_F(SetShowTest, ShowTables) {
    exec_ok("CREATE TABLE test_show_tables (id INT, name VARCHAR, PRIMARY KEY (id))");

    auto qr = exec_ok("SHOW TABLES");
    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "table_name");

    bool found = false;
    for (const auto& row : qr.rows) {
        if (row[0].as_string() == "test_show_tables") {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(SetShowTest, ShowColumns) {
    exec_ok("CREATE TABLE test_show_cols (id INT, name VARCHAR, active BOOLEAN, "
            "PRIMARY KEY (id))");

    auto qr = exec_ok("SHOW COLUMNS FROM test_show_cols");
    ASSERT_EQ(qr.column_names.size(), 5u);
    EXPECT_EQ(qr.column_names[0], "column_name");
    EXPECT_EQ(qr.column_names[1], "type");
    EXPECT_EQ(qr.column_names[2], "nullable");
    EXPECT_EQ(qr.column_names[3], "default");
    EXPECT_EQ(qr.column_names[4], "autoincrement");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "id");
    EXPECT_EQ(qr.rows[1][0].as_string(), "name");
    EXPECT_EQ(qr.rows[2][0].as_string(), "active");
}

TEST_F(SetShowTest, ShowColumnsNonexistentTable) {
    exec_error("SHOW COLUMNS FROM nonexistent", StatusCode::NOT_FOUND);
}

TEST_F(SetShowTest, ShowIndexes) {
    auto qr = exec_ok("SHOW INDEXES");
    ASSERT_EQ(qr.column_names.size(), 5u);
    EXPECT_EQ(qr.column_names[0], "index_name");
    EXPECT_EQ(qr.column_names[1], "table_name");
}

TEST_F(SetShowTest, ShowIndexesResolvesTableName) {
    exec_ok("CREATE TABLE idx_test (id INT, name VARCHAR, PRIMARY KEY (id))");

    // Register index via catalog API (CREATE INDEX not yet in query engine).
    auto table = catalog_->get_table(default_database_id, "idx_test");
    ASSERT_TRUE(table.has_value());
    IndexDef idx;
    idx.table_id = table->table_id;
    idx.name = "idx_name";
    idx.index_type = "btree";
    idx.columns = "name";
    idx.is_unique = false;
    ASSERT_TRUE(catalog_->create_index(idx).has_value());

    auto qr = exec_ok("SHOW INDEXES");
    bool found = false;
    for (const auto& row : qr.rows) {
        if (row[0].as_string() == "idx_name") {
            found = true;
            EXPECT_EQ(row[1].as_string(), "idx_test");
            EXPECT_EQ(row[2].as_string(), "name");
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(SetShowTest, ShowEdgeTypes) {
    auto qr = exec_ok("SHOW EDGE TYPES");
    ASSERT_EQ(qr.column_names.size(), 3u);
    EXPECT_EQ(qr.column_names[0], "edge_type");
    EXPECT_EQ(qr.column_names[1], "source_table");
    EXPECT_EQ(qr.column_names[2], "target_table");
}

TEST_F(SetShowTest, ShowEdgeTypesResolvesTableNames) {
    exec_ok("CREATE TABLE et_src (id INT, PRIMARY KEY (id))");
    exec_ok("CREATE TABLE et_tgt (id INT, PRIMARY KEY (id))");

    // Register edge type via catalog API (graph engine not available in test).
    auto src = catalog_->get_table(default_database_id, "et_src");
    auto tgt = catalog_->get_table(default_database_id, "et_tgt");
    ASSERT_TRUE(src.has_value());
    ASSERT_TRUE(tgt.has_value());
    EdgeTypeDef et;
    et.name = "follows";
    et.source_table_id = src->table_id;
    et.target_table_id = tgt->table_id;
    ASSERT_TRUE(catalog_->create_edge_type(default_database_id, et).has_value());

    auto qr = exec_ok("SHOW EDGE TYPES");
    bool found = false;
    for (const auto& row : qr.rows) {
        if (row[0].as_string() == "follows") {
            found = true;
            EXPECT_EQ(row[1].as_string(), "et_src");
            EXPECT_EQ(row[2].as_string(), "et_tgt");
        }
    }
    EXPECT_TRUE(found);
}

// =============================================================================
// SHOW DATABASES (GDB-53)
// =============================================================================

TEST_F(SetShowTest, ShowDatabases) {
    auto qr = exec_ok("SHOW DATABASES");
    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "database_name");

    // Default databases: sixseven and sixseven_system.
    bool found_demo = false;
    bool found_system = false;
    for (const auto& row : qr.rows) {
        if (row[0].as_string() == "demo")
            found_demo = true;
        if (row[0].as_string() == "sixseven_system")
            found_system = true;
    }
    EXPECT_TRUE(found_demo);
    EXPECT_TRUE(found_system);
}

TEST_F(SetShowTest, ShowDatabasesAfterCreate) {
    exec_ok("CREATE DATABASE test_show_db");

    auto qr = exec_ok("SHOW DATABASES");
    bool found = false;
    for (const auto& row : qr.rows) {
        if (row[0].as_string() == "test_show_db")
            found = true;
    }
    EXPECT_TRUE(found);
}

// =============================================================================
// SHOW EMBEDDINGS (GDB-53)
// =============================================================================

TEST_F(SetShowTest, ShowEmbeddingsEmpty) {
    auto qr = exec_ok("SHOW EMBEDDINGS");
    ASSERT_EQ(qr.column_names.size(), 5u);
    EXPECT_EQ(qr.column_names[0], "table_name");
    EXPECT_EQ(qr.column_names[1], "column_name");
    EXPECT_EQ(qr.column_names[2], "dimension");
    EXPECT_EQ(qr.column_names[3], "source_expr");
    EXPECT_EQ(qr.column_names[4], "provider");
    EXPECT_TRUE(qr.rows.empty());
}

TEST_F(SetShowTest, ShowEmbeddingsWithData) {
    exec_ok("CREATE TABLE emb_test (id INT, title VARCHAR, vec INT, PRIMARY KEY (id))");

    // Register embedding metadata via catalog API (EmbeddingColumnManager not
    // available in this test fixture).
    auto table = catalog_->get_table(default_database_id, "emb_test");
    ASSERT_TRUE(table.has_value());
    EmbeddingColumnDef emb;
    emb.table_id = table->table_id;
    emb.column_id = 2; // ordinal of 'vec'
    emb.dimension = 384;
    emb.source_expr = "title";
    emb.provider = "test/model";
    ASSERT_TRUE(catalog_->register_embedding_column(emb).has_value());

    auto qr = exec_ok("SHOW EMBEDDINGS");
    ASSERT_GE(qr.rows.size(), 1u);
    bool found = false;
    for (const auto& row : qr.rows) {
        if (row[0].as_string() == "emb_test" && row[1].as_string() == "vec") {
            found = true;
            EXPECT_EQ(row[2].as_int32(), 384);
            EXPECT_EQ(row[3].as_string(), "title");
            EXPECT_EQ(row[4].as_string(), "test/model");
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(SetShowTest, ShowEmbeddingsFromTable) {
    exec_ok("CREATE TABLE emb_filter (id INT, title VARCHAR, vec INT, PRIMARY KEY (id))");

    auto table = catalog_->get_table(default_database_id, "emb_filter");
    ASSERT_TRUE(table.has_value());
    EmbeddingColumnDef emb;
    emb.table_id = table->table_id;
    emb.column_id = 2;
    emb.dimension = 128;
    emb.source_expr = "title";
    emb.provider = "p1";
    ASSERT_TRUE(catalog_->register_embedding_column(emb).has_value());

    auto qr = exec_ok("SHOW EMBEDDINGS FROM emb_filter");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "emb_filter");
    EXPECT_EQ(qr.rows[0][1].as_string(), "vec");
    EXPECT_EQ(qr.rows[0][2].as_int32(), 128);
}

TEST_F(SetShowTest, ShowEmbeddingsFromNonexistentTable) {
    exec_error("SHOW EMBEDDINGS FROM nonexistent", StatusCode::NOT_FOUND);
}

// =============================================================================
// Parser: dotted names in SET/SHOW
// =============================================================================

TEST_F(SetShowTest, SetDottedNameParsesCorrectly) {
    // Verify SET storage.buffer_pool_size_mb errors as expected
    // (not runtime-mutable, but proves the dotted name parsed correctly).
    exec_error("SET storage.buffer_pool_size_mb = 512", StatusCode::INVALID_ARGUMENT);
}

TEST_F(SetShowTest, ShowDottedNameParsesCorrectly) {
    auto qr = exec_ok("SHOW server.port");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "server.port");
    EXPECT_EQ(qr.rows[0][1].as_string(), std::to_string(config_.port));
}

// =============================================================================
// SettingsCache unit tests
// =============================================================================

TEST_F(SetShowTest, CacheGetNonexistent) {
    auto entry = cache_->get("does.not.exist");
    EXPECT_FALSE(entry.has_value());
}

TEST_F(SetShowTest, CacheGetAll) {
    auto all = cache_->get_all();
    EXPECT_GE(all.size(), 6u);
}

TEST_F(SetShowTest, CacheUpdateMutable) {
    auto result = cache_->update("logging.level", "trace");
    EXPECT_TRUE(result.has_value());
    auto entry = cache_->get("logging.level");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->value, "trace");
}

TEST_F(SetShowTest, CacheUpdateNonMutable) {
    auto result = cache_->update("server.port", "9999");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(SetShowTest, CacheUpdateNonexistent) {
    auto result = cache_->update("nonexistent.key", "value");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// =============================================================================
// SET with integer literal value
// =============================================================================

TEST_F(SetShowTest, SetWithStringLiteralQuotedValue) {
    exec_ok("SET logging.level = 'trace'");
    auto entry = cache_->get("logging.level");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->value, "trace");
}

// =============================================================================
// SHOW REPLICATION SLOTS (GDB-196)
// =============================================================================

TEST_F(SetShowTest, ShowReplicationSlotsEmpty) {
    // Wire up a slot manager with no slots.
    auto tmp = std::filesystem::temp_directory_path() / "sixseven_test_show_slots";
    std::filesystem::create_directories(tmp);
    sixseven::ReplicationSlotManager slot_mgr(tmp);
    engine_->set_slot_manager(&slot_mgr);

    auto qr = exec_ok("SHOW REPLICATION SLOTS");
    EXPECT_EQ(qr.column_names.size(), 5u);
    EXPECT_EQ(qr.column_names[0], "slot_name");
    EXPECT_EQ(qr.column_names[1], "slot_type");
    EXPECT_EQ(qr.column_names[2], "active");
    EXPECT_EQ(qr.column_names[3], "restart_lsn");
    EXPECT_EQ(qr.column_names[4], "confirmed_flush_lsn");
    EXPECT_TRUE(qr.rows.empty());

    std::filesystem::remove_all(tmp);
}

TEST_F(SetShowTest, ShowReplicationSlotsWithData) {
    auto tmp = std::filesystem::temp_directory_path() / "sixseven_test_show_slots2";
    std::filesystem::create_directories(tmp);
    sixseven::ReplicationSlotManager slot_mgr(tmp);
    engine_->set_slot_manager(&slot_mgr);

    ASSERT_TRUE(slot_mgr.create_slot("replica_1").has_value());
    ASSERT_TRUE(slot_mgr.activate_slot("replica_1", 100).has_value());

    auto qr = exec_ok("SHOW REPLICATION SLOTS");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "replica_1");
    EXPECT_EQ(qr.rows[0][1].as_string(), "physical");
    EXPECT_EQ(qr.rows[0][2].as_bool(), true);
    EXPECT_EQ(qr.rows[0][3].as_int64(), 100);

    std::filesystem::remove_all(tmp);
}

TEST_F(SetShowTest, ShowReplicationSlotsNoManager) {
    // Without a slot manager set, should return an error.
    exec_error("SHOW REPLICATION SLOTS", StatusCode::INTERNAL_ERROR);
}
