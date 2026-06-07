#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/catalog_persistence.h"
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

// =============================================================================
// Test fixture for system bootstrap (GDB-188)
// =============================================================================

class SystemBootstrapTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_bootstrap";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        config_ = Config::load_defaults();
    }

    void TearDown() override {
        engine_.reset();
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
        EXPECT_EQ(result.error().code, expected);
    }

    /// Switch engine to a different database by name.
    void use_database(const std::string& name) {
        auto db = catalog_->get_database(name);
        ASSERT_TRUE(db.has_value()) << "database '" << name << "' not found";
        engine_->set_current_database(db->database_id);
    }

    /// Run bootstrap and assert it succeeds.
    void run_bootstrap() {
        auto result = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }

    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    Config config_;
};

// =============================================================================
// AC: sixseven_system database created automatically on first init
// =============================================================================

TEST_F(SystemBootstrapTest, SystemDatabaseExistsOnInit) {
    auto db = catalog_->get_database(system_database_name);
    ASSERT_TRUE(db.has_value()) << db.error().message;
    EXPECT_EQ(db->database_id, system_database_id);
    EXPECT_EQ(db->name, system_database_name);
}

TEST_F(SystemBootstrapTest, SystemDatabaseInList) {
    auto dbs = catalog_->list_databases();
    bool found = false;
    for (const auto& db : dbs) {
        if (db.name == system_database_name) {
            found = true;
            EXPECT_EQ(db.database_id, system_database_id);
        }
    }
    EXPECT_TRUE(found) << "sixseven_system not found in database list";
}

// =============================================================================
// AC: sys_settings table populated with default settings
// =============================================================================

TEST_F(SystemBootstrapTest, SysSettingsTableCreated) {
    run_bootstrap();

    auto schema = catalog_->get_table(system_database_id, "sys_settings");
    ASSERT_TRUE(schema.has_value()) << schema.error().message;
    EXPECT_EQ(schema->name, "sys_settings");
    ASSERT_EQ(schema->columns.size(), 5u);
    EXPECT_EQ(schema->columns[0].name, "key");
    EXPECT_EQ(schema->columns[0].type_id, TypeId::STRING);
    EXPECT_EQ(schema->columns[1].name, "value");
    EXPECT_EQ(schema->columns[1].type_id, TypeId::STRING);
    EXPECT_EQ(schema->columns[2].name, "category");
    EXPECT_EQ(schema->columns[2].type_id, TypeId::STRING);
    EXPECT_EQ(schema->columns[3].name, "description");
    EXPECT_EQ(schema->columns[3].type_id, TypeId::STRING);
    EXPECT_EQ(schema->columns[4].name, "is_runtime_mutable");
    EXPECT_EQ(schema->columns[4].type_id, TypeId::BOOL);
    EXPECT_EQ(schema->pk_columns, "key");
}

TEST_F(SystemBootstrapTest, DefaultSettingsSeeded) {
    run_bootstrap();

    use_database(system_database_name);

    auto qr = exec_ok("SELECT key, value, category FROM sys_settings");
    EXPECT_GE(qr.rows.size(), 6u) << "expected at least 6 default settings";

    // Verify specific settings exist.
    bool found_port = false;
    bool found_max_conn = false;
    bool found_data_dir = false;
    bool found_buffer_pool = false;
    bool found_wal = false;
    bool found_log_level = false;

    for (const auto& row : qr.rows) {
        auto key = row[0].as_string();
        auto value = row[1].as_string();
        auto category = row[2].as_string();

        if (key == "server.port") {
            found_port = true;
            EXPECT_EQ(value, std::to_string(config_.port));
            EXPECT_EQ(category, "server");
        } else if (key == "server.max_connections") {
            found_max_conn = true;
            EXPECT_EQ(value, std::to_string(config_.max_connections));
            EXPECT_EQ(category, "server");
        } else if (key == "storage.data_dir") {
            found_data_dir = true;
            EXPECT_EQ(value, config_.data_dir);
            EXPECT_EQ(category, "storage");
        } else if (key == "storage.buffer_pool_size_mb") {
            found_buffer_pool = true;
            EXPECT_EQ(value, std::to_string(config_.buffer_pool_size_mb));
            EXPECT_EQ(category, "storage");
        } else if (key == "storage.wal_segment_size_mb") {
            found_wal = true;
            EXPECT_EQ(value, std::to_string(config_.wal_segment_size_mb));
            EXPECT_EQ(category, "storage");
        } else if (key == "logging.level") {
            found_log_level = true;
            EXPECT_EQ(value, config_.log_level);
            EXPECT_EQ(category, "logging");
        }
    }

    EXPECT_TRUE(found_port) << "server.port not found";
    EXPECT_TRUE(found_max_conn) << "server.max_connections not found";
    EXPECT_TRUE(found_data_dir) << "storage.data_dir not found";
    EXPECT_TRUE(found_buffer_pool) << "storage.buffer_pool_size_mb not found";
    EXPECT_TRUE(found_wal) << "storage.wal_segment_size_mb not found";
    EXPECT_TRUE(found_log_level) << "logging.level not found";
}

// =============================================================================
// AC: System database is protected from DROP
// =============================================================================

TEST_F(SystemBootstrapTest, CannotDropSystemDatabase) {
    auto result = catalog_->drop_database(system_database_id, false);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST_F(SystemBootstrapTest, CannotDropSystemDatabaseWithCascade) {
    auto result = catalog_->drop_database(system_database_id, true);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST_F(SystemBootstrapTest, CannotDropSystemDatabaseViaSql) {
    exec_error("DROP DATABASE sixseven_system", StatusCode::CONSTRAINT_VIOLATION);
}

TEST_F(SystemBootstrapTest, CannotDropSystemDatabaseCascadeViaSql) {
    exec_error("DROP DATABASE sixseven_system CASCADE", StatusCode::CONSTRAINT_VIOLATION);
}

// =============================================================================
// AC: System tables are protected from DROP
// =============================================================================

TEST_F(SystemBootstrapTest, CannotDropSystemTable) {
    run_bootstrap();

    auto result = catalog_->drop_table(system_database_id, "sys_settings");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST_F(SystemBootstrapTest, CannotDropSystemTableViaSql) {
    run_bootstrap();

    use_database(system_database_name);
    exec_error("DROP TABLE sys_settings", StatusCode::CONSTRAINT_VIOLATION);
}

// =============================================================================
// AC: Settings persist across server restarts (bootstrap flag)
// =============================================================================

TEST_F(SystemBootstrapTest, BootstrapFlagCreatedOnFirstRun) {
    EXPECT_FALSE(SystemBootstrap::is_bootstrapped(data_dir_));

    run_bootstrap();

    EXPECT_TRUE(SystemBootstrap::is_bootstrapped(data_dir_));
}

TEST_F(SystemBootstrapTest, SecondBootstrapSkipsSeedButCreatesTable) {
    // First run: seeds defaults.
    run_bootstrap();
    EXPECT_TRUE(SystemBootstrap::is_bootstrapped(data_dir_));

    // Simulate "restart" by recreating all components.
    // (The catalog is in-memory, so it gets a fresh sixseven_system DB.)
    engine_.reset();
    storage_.reset();
    catalog_.reset();
    dm_ = std::make_unique<DiskManager>();
    catalog_ = std::make_unique<Catalog>();
    storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
    persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
    engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);

    // Second bootstrap: should still create the table (in-memory catalog is fresh).
    run_bootstrap();

    // sys_settings table should exist.
    auto schema = catalog_->get_table(system_database_id, "sys_settings");
    ASSERT_TRUE(schema.has_value()) << schema.error().message;
}

// =============================================================================
// AC: Config loading respects the priority chain
// =============================================================================

TEST_F(SystemBootstrapTest, LoadSettingsAppliesOverrides) {
    run_bootstrap();

    // Modify a setting in the table directly via SQL.
    use_database(system_database_name);
    exec_ok("UPDATE sys_settings SET value = '9999' WHERE key = 'server.port'");

    // Restore to default database.
    use_database("demo");

    // Load settings from sys_settings.
    Config config = Config::load_defaults();
    EXPECT_EQ(config.port, 6767);

    auto load_result = SystemBootstrap::load_settings(*engine_, *catalog_, config);
    ASSERT_TRUE(load_result.has_value()) << load_result.error().message;

    // Port should now be overridden by sys_settings.
    EXPECT_EQ(config.port, 9999);
}

TEST_F(SystemBootstrapTest, ConfigPriorityChain) {
    // Seed with a non-default config value.
    config_.port = 7777;
    config_.log_level = "debug";

    run_bootstrap();

    // The seeded value should match what we passed in.
    use_database(system_database_name);
    auto qr = exec_ok("SELECT value FROM sys_settings WHERE key = 'server.port'");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "7777");

    // Now load settings into a fresh defaults config.
    use_database("demo");
    Config fresh = Config::load_defaults();
    EXPECT_EQ(fresh.port, 6767); // Default.

    auto load_result = SystemBootstrap::load_settings(*engine_, *catalog_, fresh);
    ASSERT_TRUE(load_result.has_value()) << load_result.error().message;

    // sys_settings (7777) should override default (6767).
    EXPECT_EQ(fresh.port, 7777);
    EXPECT_EQ(fresh.log_level, "debug");
}

// =============================================================================
// Config apply_setting tests
// =============================================================================

TEST_F(SystemBootstrapTest, ApplySettingPort) {
    Config config = Config::load_defaults();
    config.apply_setting("server.port", "9999");
    EXPECT_EQ(config.port, 9999);
}

TEST_F(SystemBootstrapTest, ApplySettingMaxConnections) {
    Config config = Config::load_defaults();
    config.apply_setting("server.max_connections", "500");
    EXPECT_EQ(config.max_connections, 500u);
}

TEST_F(SystemBootstrapTest, ApplySettingDataDir) {
    Config config = Config::load_defaults();
    config.apply_setting("storage.data_dir", "/var/sixseven");
    EXPECT_EQ(config.data_dir, "/var/sixseven");
}

TEST_F(SystemBootstrapTest, ApplySettingBufferPool) {
    Config config = Config::load_defaults();
    config.apply_setting("storage.buffer_pool_size_mb", "1024");
    EXPECT_EQ(config.buffer_pool_size_mb, 1024u);
}

TEST_F(SystemBootstrapTest, ApplySettingWalSegment) {
    Config config = Config::load_defaults();
    config.apply_setting("storage.wal_segment_size_mb", "64");
    EXPECT_EQ(config.wal_segment_size_mb, 64u);
}

TEST_F(SystemBootstrapTest, ApplySettingLogLevel) {
    Config config = Config::load_defaults();
    config.apply_setting("logging.level", "debug");
    EXPECT_EQ(config.log_level, "debug");
}

TEST_F(SystemBootstrapTest, ApplySettingUnknownKeyIgnored) {
    Config config = Config::load_defaults();
    auto original_port = config.port;
    config.apply_setting("unknown.key", "value");
    EXPECT_EQ(config.port, original_port); // Unchanged.
}

// =============================================================================
// sys_settings schema definition
// =============================================================================

TEST_F(SystemBootstrapTest, SysSettingsSchemaDefinition) {
    auto schema = sys_settings_schema();
    EXPECT_EQ(schema.name, "sys_settings");
    ASSERT_EQ(schema.columns.size(), 5u);
    EXPECT_EQ(schema.columns[0].name, "key");
    EXPECT_EQ(schema.columns[0].type_id, TypeId::STRING);
    EXPECT_FALSE(schema.columns[0].nullable);
    EXPECT_EQ(schema.columns[1].name, "value");
    EXPECT_EQ(schema.columns[1].type_id, TypeId::STRING);
    EXPECT_EQ(schema.columns[2].name, "category");
    EXPECT_EQ(schema.columns[2].type_id, TypeId::STRING);
    EXPECT_EQ(schema.columns[3].name, "description");
    EXPECT_EQ(schema.columns[3].type_id, TypeId::STRING);
    EXPECT_EQ(schema.columns[4].name, "is_runtime_mutable");
    EXPECT_EQ(schema.columns[4].type_id, TypeId::BOOL);
    EXPECT_EQ(schema.pk_columns, "key");
}

// =============================================================================
// System database name conflict
// =============================================================================

TEST_F(SystemBootstrapTest, CannotCreateDatabaseWithSystemName) {
    auto result = catalog_->create_database(system_database_name);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::ALREADY_EXISTS);
}
