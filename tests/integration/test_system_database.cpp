#include "giodb/catalog/catalog.h"
#include "giodb/catalog/schema.h"
#include "giodb/common/config.h"
#include "giodb/common/result.h"
#include "giodb/common/secrets_manager.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/provider_cache.h"
#include "giodb/executor/query_engine.h"
#include "giodb/executor/settings_cache.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/executor/system_bootstrap.h"
#include "giodb/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace giodb;
namespace fs = std::filesystem;

// =============================================================================
// Test fixture — full-stack system database integration
//
// Sets up: DiskManager, Catalog, StorageManager, QueryEngine, SecretsManager,
// SystemBootstrap, SettingsCache, ProviderCache — the complete system database
// stack. All subsystems are wired together exactly as they would be in a
// running server.
// =============================================================================

class SystemDatabaseIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = fs::temp_directory_path() / "giodb_test_sysdb_integration";
        fs::remove_all(data_dir_);
        fs::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        config_ = Config::load_defaults();

        // Create secrets manager with master key in data_dir_.
        auto key_path = (data_dir_ / "master.key").string();
        auto mgr = SecretsManager::create(key_path);
        ASSERT_TRUE(mgr.has_value()) << mgr.error().message;
        secrets_manager_ = std::make_unique<SecretsManager>(std::move(*mgr));

        // Bootstrap system database.
        auto boot = SystemBootstrap::bootstrap(*engine_, *catalog_, *storage_, config_, data_dir_);
        ASSERT_TRUE(boot.has_value()) << boot.error().message;

        // Create and load caches.
        settings_cache_ = std::make_unique<SettingsCache>();
        auto load_settings = settings_cache_->load(*engine_);
        ASSERT_TRUE(load_settings.has_value()) << load_settings.error().message;

        provider_cache_ = std::make_unique<ProviderCache>();
        provider_cache_->set_secrets_manager(secrets_manager_.get());
        auto load_providers = provider_cache_->load(*engine_);
        ASSERT_TRUE(load_providers.has_value()) << load_providers.error().message;

        // Wire caches into the engine.
        engine_->set_settings_cache(settings_cache_.get());
        engine_->set_provider_cache(provider_cache_.get());

        // Switch to default database for user operations.
        use_database("giodb");
    }

    void TearDown() override {
        engine_.reset();
        provider_cache_.reset();
        settings_cache_.reset();
        secrets_manager_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
        fs::remove_all(data_dir_);
    }

    /// Simulate persistence verification by creating fresh caches and reloading
    /// from the system tables. This verifies that data written by SET/INSERT
    /// is persisted in the database and can be loaded by new cache instances.
    void reload_caches() {
        // Detach old caches from the engine.
        engine_->set_settings_cache(nullptr);
        engine_->set_provider_cache(nullptr);

        // Create fresh caches and reload from system tables.
        settings_cache_ = std::make_unique<SettingsCache>();
        auto load_settings = settings_cache_->load(*engine_);
        ASSERT_TRUE(load_settings.has_value()) << load_settings.error().message;

        provider_cache_ = std::make_unique<ProviderCache>();
        provider_cache_->set_secrets_manager(secrets_manager_.get());
        auto load_providers = provider_cache_->load(*engine_);
        ASSERT_TRUE(load_providers.has_value()) << load_providers.error().message;

        // Re-wire fresh caches.
        engine_->set_settings_cache(settings_cache_.get());
        engine_->set_provider_cache(provider_cache_.get());
    }

    /// Execute SQL and assert success.
    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return std::move(*result);
    }

    /// Execute SQL and assert failure with the expected status code.
    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value());
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected);
        }
    }

    /// Switch the engine to a different database by name.
    void use_database(const std::string& name) {
        auto db = catalog_->get_database(name);
        ASSERT_TRUE(db.has_value()) << "database '" << name << "' not found";
        engine_->set_current_database(db->database_id);
    }

    /// Insert a provider via SQL. Returns the provider_id used.
    int32_t insert_provider(const std::string& name,
                            const std::string& type,
                            const std::string& endpoint,
                            const std::string& model,
                            const std::string& api_key = "",
                            bool is_default = false) {
        int32_t id = next_provider_id_++;

        use_database(system_database_name);

        std::string sql = "INSERT INTO sys_providers VALUES (" + std::to_string(id) + ", '" + name +
                          "', '" + type + "', '" + endpoint + "', '" + model + "', " +
                          (api_key.empty() ? "NULL" : "'" + api_key + "'") + ", " +
                          (is_default ? "TRUE" : "FALSE") + ", NULL)";
        exec_ok(sql);

        use_database("giodb");
        return id;
    }

    /// Read the raw api_key_encrypted value from the database (bypassing masking).
    std::string read_raw_api_key(int32_t provider_id) {
        use_database(system_database_name);
        engine_->push_skip_masking();
        auto qr = exec_ok("SELECT api_key_encrypted FROM sys_providers "
                          "WHERE provider_id = " +
                          std::to_string(provider_id));
        engine_->pop_skip_masking();
        use_database("giodb");

        if (qr.rows.empty() || qr.rows[0][0].is_null()) {
            return "";
        }
        return qr.rows[0][0].as_string();
    }

    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    fs::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<SettingsCache> settings_cache_;
    std::unique_ptr<ProviderCache> provider_cache_;
    std::unique_ptr<SecretsManager> secrets_manager_;
    Config config_;
    int32_t next_provider_id_ = 1;
};

// =============================================================================
// System Database Lifecycle
// =============================================================================

TEST_F(SystemDatabaseIntegrationTest, SystemDatabaseExistsAfterFreshInit) {
    auto db = catalog_->get_database(system_database_name);
    ASSERT_TRUE(db.has_value()) << db.error().message;
    EXPECT_EQ(db->database_id, system_database_id);
    EXPECT_EQ(db->name, system_database_name);
}

TEST_F(SystemDatabaseIntegrationTest, SystemDatabaseCannotBeDropped) {
    exec_error("DROP DATABASE giodb_system", StatusCode::CONSTRAINT_VIOLATION);
    exec_error("DROP DATABASE giodb_system CASCADE", StatusCode::CONSTRAINT_VIOLATION);

    // Verify it still exists.
    auto db = catalog_->get_database(system_database_name);
    EXPECT_TRUE(db.has_value());
}

TEST_F(SystemDatabaseIntegrationTest, SystemTablesExistAndCannotBeDropped) {
    // Verify sys_settings table exists.
    auto settings = catalog_->get_table(system_database_id, "sys_settings");
    ASSERT_TRUE(settings.has_value()) << settings.error().message;
    EXPECT_EQ(settings->name, "sys_settings");

    // Verify sys_providers table exists.
    auto providers = catalog_->get_table(system_database_id, "sys_providers");
    ASSERT_TRUE(providers.has_value()) << providers.error().message;
    EXPECT_EQ(providers->name, "sys_providers");

    // Cannot drop system tables.
    use_database(system_database_name);
    exec_error("DROP TABLE sys_settings", StatusCode::CONSTRAINT_VIOLATION);
    exec_error("DROP TABLE sys_providers", StatusCode::CONSTRAINT_VIOLATION);
}

TEST_F(SystemDatabaseIntegrationTest, ReInitDoesNotDuplicateSystemDatabase) {
    // Count giodb_system databases.
    auto dbs = catalog_->list_databases();
    size_t count = 0;
    for (const auto& db : dbs) {
        if (db.name == system_database_name) {
            ++count;
        }
    }
    EXPECT_EQ(count, 1u);

    // Run bootstrap again on the same engine (simulates re-init path).
    // This should not create a duplicate system database.
    auto boot = SystemBootstrap::bootstrap(*engine_, *catalog_, *storage_, config_, data_dir_);
    // Bootstrap may fail because the tables already exist — that's acceptable.
    // The key assertion is that list_databases still shows exactly one.

    auto dbs_after = catalog_->list_databases();
    size_t count_after = 0;
    for (const auto& db : dbs_after) {
        if (db.name == system_database_name) {
            ++count_after;
        }
    }
    EXPECT_EQ(count_after, 1u);
}

// =============================================================================
// Settings Management
// =============================================================================

TEST_F(SystemDatabaseIntegrationTest, ShowLoggingLevelReturnsDefault) {
    auto qr = exec_ok("SHOW logging.level");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "logging.level");
    EXPECT_EQ(qr.rows[0][1].as_string(), "info");
}

TEST_F(SystemDatabaseIntegrationTest, SetLoggingLevelSucceedsAndShowReflects) {
    auto set_qr = exec_ok("SET logging.level = 'debug'");
    EXPECT_EQ(set_qr.message, "SET");

    auto show_qr = exec_ok("SHOW logging.level");
    ASSERT_EQ(show_qr.rows.size(), 1u);
    EXPECT_EQ(show_qr.rows[0][1].as_string(), "debug");
}

TEST_F(SystemDatabaseIntegrationTest, SetNonMutableSettingFails) {
    exec_error("SET server.port = 1234", StatusCode::INVALID_ARGUMENT);
}

TEST_F(SystemDatabaseIntegrationTest, SetNonexistentSettingFails) {
    exec_error("SET nonexistent.param = 'x'", StatusCode::NOT_FOUND);
}

TEST_F(SystemDatabaseIntegrationTest, SettingsPersistViaReload) {
    // Change a setting.
    exec_ok("SET logging.level = 'warn'");

    // Verify it took effect.
    auto before_qr = exec_ok("SHOW logging.level");
    ASSERT_EQ(before_qr.rows.size(), 1u);
    EXPECT_EQ(before_qr.rows[0][1].as_string(), "warn");

    // Reload caches from the system tables (simulates persistence check).
    reload_caches();

    // Verify the setting survived the reload.
    auto after_qr = exec_ok("SHOW logging.level");
    ASSERT_EQ(after_qr.rows.size(), 1u);
    EXPECT_EQ(after_qr.rows[0][1].as_string(), "warn");
}

TEST_F(SystemDatabaseIntegrationTest, ShowAllReturnsAllSettingsWithCorrectColumns) {
    auto qr = exec_ok("SHOW ALL");
    ASSERT_EQ(qr.column_names.size(), 4u);
    EXPECT_EQ(qr.column_names[0], "key");
    EXPECT_EQ(qr.column_names[1], "value");
    EXPECT_EQ(qr.column_names[2], "category");
    EXPECT_EQ(qr.column_names[3], "is_runtime_mutable");

    // Should have at least the 6 default settings.
    EXPECT_GE(qr.rows.size(), 6u);

    // Verify known settings are present with correct categories.
    bool found_port = false;
    bool found_log_level = false;
    for (const auto& row : qr.rows) {
        auto key = row[0].as_string();
        if (key == "server.port") {
            found_port = true;
            EXPECT_EQ(row[2].as_string(), "server");
            EXPECT_FALSE(row[3].as_bool()); // Not runtime-mutable.
        } else if (key == "logging.level") {
            found_log_level = true;
            EXPECT_EQ(row[2].as_string(), "logging");
            EXPECT_TRUE(row[3].as_bool()); // Runtime-mutable.
        }
    }
    EXPECT_TRUE(found_port) << "server.port missing from SHOW ALL";
    EXPECT_TRUE(found_log_level) << "logging.level missing from SHOW ALL";
}

// =============================================================================
// Provider Configuration
// =============================================================================

TEST_F(SystemDatabaseIntegrationTest, RegisterOllamaProviderNoApiKey) {
    insert_provider("ollama-local", "ollama", "http://localhost:11434", "nomic-embed-text");

    auto qr = exec_ok("SHOW PROVIDERS");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "ollama-local");
    EXPECT_EQ(qr.rows[0][1].as_string(), "ollama");
    EXPECT_EQ(qr.rows[0][2].as_string(), "http://localhost:11434");
    EXPECT_EQ(qr.rows[0][3].as_string(), "nomic-embed-text");
}

TEST_F(SystemDatabaseIntegrationTest, RegisterOpenAIProviderKeyEncrypted) {
    auto id = insert_provider("openai-prod",
                              "openai",
                              "https://api.openai.com/v1",
                              "text-embedding-3-small",
                              "sk-secret-key-123");

    // Verify the provider appears in SHOW PROVIDERS.
    auto show_qr = exec_ok("SHOW PROVIDERS");
    ASSERT_EQ(show_qr.rows.size(), 1u);
    EXPECT_EQ(show_qr.rows[0][0].as_string(), "openai-prod");

    // Verify the raw value on disk is encrypted.
    std::string raw = read_raw_api_key(id);
    EXPECT_NE(raw, "sk-secret-key-123");
    EXPECT_TRUE(SecretsManager::looks_encrypted(raw));

    // Verify the in-memory cache has the decrypted key.
    auto provider = provider_cache_->get("openai-prod");
    ASSERT_TRUE(provider.has_value());
    EXPECT_EQ(provider->api_key, "sk-secret-key-123");
}

TEST_F(SystemDatabaseIntegrationTest, SetProviderAsDefaultVerifyOnlyOneDefault) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-key1",
                    true);
    insert_provider("ollama-local", "ollama", "http://localhost:11434", "nomic-embed-text");

    // openai-prod should be the default.
    auto def1 = provider_cache_->get_default();
    ASSERT_TRUE(def1.has_value());
    EXPECT_EQ(def1->name, "openai-prod");

    // Set ollama-local as default (this auto-unsets old default).
    use_database(system_database_name);
    exec_ok("UPDATE sys_providers SET is_default = TRUE WHERE name = 'ollama-local'");
    use_database("giodb");

    // Verify only one default exists.
    auto def2 = provider_cache_->get_default();
    ASSERT_TRUE(def2.has_value());
    EXPECT_EQ(def2->name, "ollama-local");

    // Old default should be unset.
    auto old = provider_cache_->get("openai-prod");
    ASSERT_TRUE(old.has_value());
    EXPECT_FALSE(old->is_default);
}

TEST_F(SystemDatabaseIntegrationTest, UpdateProviderModelPersists) {
    insert_provider(
        "openai-prod", "openai", "https://api.openai.com/v1", "text-embedding-3-small", "sk-key");

    // Update model.
    use_database(system_database_name);
    exec_ok("UPDATE sys_providers SET model = 'text-embedding-3-large' "
            "WHERE name = 'openai-prod'");
    use_database("giodb");

    // Verify cache updated.
    auto provider = provider_cache_->get("openai-prod");
    ASSERT_TRUE(provider.has_value());
    EXPECT_EQ(provider->model, "text-embedding-3-large");

    // Reload caches and verify persistence.
    reload_caches();

    auto after = provider_cache_->get("openai-prod");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->model, "text-embedding-3-large");
}

TEST_F(SystemDatabaseIntegrationTest, DeleteUnusedProviderSucceeds) {
    insert_provider("to-delete", "ollama", "http://localhost:11434", "test-model");
    EXPECT_EQ(provider_cache_->size(), 1u);

    use_database(system_database_name);
    exec_ok("DELETE FROM sys_providers WHERE name = 'to-delete'");
    use_database("giodb");

    EXPECT_EQ(provider_cache_->size(), 0u);
    EXPECT_FALSE(provider_cache_->get("to-delete").has_value());
}

TEST_F(SystemDatabaseIntegrationTest, DeleteInUseProviderFails) {
    insert_provider("openai-prod", "openai", "https://api.openai.com/v1", "text-embedding-3-small");

    // Create a table and register an EMBEDDING column referencing this provider.
    exec_ok("CREATE TABLE articles (id INT, body VARCHAR, PRIMARY KEY (id))");
    auto schema = catalog_->get_table(default_database_id, "articles");
    ASSERT_TRUE(schema.has_value());

    EmbeddingColumnDef ecd;
    ecd.table_id = schema->table_id;
    ecd.column_id = 2;
    ecd.dimension = 1536;
    ecd.source_expr = "body";
    ecd.provider = "openai-prod";
    auto reg = catalog_->register_embedding_column(std::move(ecd));
    ASSERT_TRUE(reg.has_value()) << reg.error().message;

    // Attempt to delete the in-use provider — should fail.
    use_database(system_database_name);
    auto result = engine_->execute("DELETE FROM sys_providers WHERE name = 'openai-prod'");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION);
    use_database("giodb");

    // Provider should still exist.
    EXPECT_TRUE(provider_cache_->get("openai-prod").has_value());
}

TEST_F(SystemDatabaseIntegrationTest, SelectFromSysProvidersMasksApiKeys) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-super-secret-key",
                    true);

    use_database(system_database_name);
    auto qr = exec_ok("SELECT * FROM sys_providers");
    use_database("giodb");

    ASSERT_EQ(qr.rows.size(), 1u);

    // Find the api_key_encrypted column.
    int key_col = -1;
    for (size_t i = 0; i < qr.column_names.size(); ++i) {
        if (qr.column_names[i] == "api_key_encrypted") {
            key_col = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(key_col, 0) << "api_key_encrypted column not found";

    // The value should be masked, not the plaintext or encrypted value.
    auto masked = qr.rows[0][static_cast<size_t>(key_col)].as_string();
    EXPECT_EQ(masked, "********");
    EXPECT_NE(masked, "sk-super-secret-key");
}

// =============================================================================
// Secrets
// =============================================================================

TEST_F(SystemDatabaseIntegrationTest, SecretRoundTripSurvivesReload) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-round-trip-key");

    // Verify decryption works in current session.
    auto before = provider_cache_->get("openai-prod");
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->api_key, "sk-round-trip-key");

    // Reload caches — fresh provider cache decrypts from system table.
    reload_caches();

    // Verify decryption produces the original key after reload.
    auto after = provider_cache_->get("openai-prod");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->api_key, "sk-round-trip-key");
}

TEST_F(SystemDatabaseIntegrationTest, CorruptMasterKeyPreventsCreation) {
    // Write a zero-byte file to simulate a corrupt key file.
    auto key_path = (data_dir_ / "corrupt_master.key").string();
    std::ofstream empty_key(key_path, std::ios::binary);
    empty_key.close();

    // Attempting to create a SecretsManager with a corrupt key should fail.
    auto mgr = SecretsManager::create(key_path);
    EXPECT_FALSE(mgr.has_value());
}

TEST_F(SystemDatabaseIntegrationTest, CorruptEncryptedValueHandledGracefully) {
    // Store a provider with an API key (gets encrypted on disk).
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-test-key");

    // Corrupt the encrypted value in the database.
    use_database(system_database_name);
    engine_->push_skip_masking();
    exec_ok("UPDATE sys_providers SET api_key_encrypted = 'CORRUPT_DATA_NOT_BASE64!!!' "
            "WHERE name = 'openai-prod'");
    engine_->pop_skip_masking();
    use_database("giodb");

    // Create a fresh cache and attempt to load — the corrupt value should
    // be handled gracefully (treated as plaintext and re-encrypted).
    auto fresh_cache = std::make_unique<ProviderCache>();
    fresh_cache->set_secrets_manager(secrets_manager_.get());
    auto load = fresh_cache->load(*engine_);

    // The load should succeed: corrupt values that don't look encrypted
    // are treated as plaintext and re-encrypted in the database.
    ASSERT_TRUE(load.has_value()) << load.error().message;
    auto provider = fresh_cache->get("openai-prod");
    ASSERT_TRUE(provider.has_value());
}

TEST_F(SystemDatabaseIntegrationTest, ApiKeyNeverInPlaintextInQueryResults) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-plaintext-must-not-appear");

    // SELECT api_key_encrypted should return masked value, not plaintext.
    use_database(system_database_name);
    auto qr = exec_ok("SELECT api_key_encrypted FROM sys_providers");
    use_database("giodb");

    ASSERT_EQ(qr.rows.size(), 1u);
    if (!qr.rows[0][0].is_null()) {
        EXPECT_NE(qr.rows[0][0].as_string(), "sk-plaintext-must-not-appear")
            << "Plaintext API key leaked in SELECT api_key_encrypted";
        EXPECT_EQ(qr.rows[0][0].as_string(), "********");
    }

    // SELECT name, api_key_encrypted also masks the key column.
    use_database(system_database_name);
    auto qr2 = exec_ok("SELECT name, api_key_encrypted FROM sys_providers");
    use_database("giodb");

    ASSERT_EQ(qr2.rows.size(), 1u);
    EXPECT_EQ(qr2.rows[0][0].as_string(), "openai-prod");
    EXPECT_EQ(qr2.rows[0][1].as_string(), "********");

    // SHOW PROVIDERS does not include api_key column at all.
    auto show_qr = exec_ok("SHOW PROVIDERS");
    for (const auto& col : show_qr.column_names) {
        EXPECT_NE(col, "api_key_encrypted");
        EXPECT_NE(col, "api_key");
    }
}

// =============================================================================
// Config Priority
// =============================================================================

TEST_F(SystemDatabaseIntegrationTest, DefaultValueForSetting) {
    // The default value for logging.level should be "info".
    auto qr = exec_ok("SHOW logging.level");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "info");

    // The default value for server.port should be "6767".
    auto port_qr = exec_ok("SHOW server.port");
    ASSERT_EQ(port_qr.rows.size(), 1u);
    EXPECT_EQ(port_qr.rows[0][1].as_string(), "6767");
}

TEST_F(SystemDatabaseIntegrationTest, OverrideViaConfigFile) {
    // Write a JSON config file with an override.
    auto config_path = data_dir_ / "giodb.json";
    {
        std::ofstream f(config_path);
        f << R"({"port": 9876, "log_level": "warn"})";
    }

    // Load config from file — should override defaults.
    auto config = Config::load_from_file(config_path.string());
    ASSERT_TRUE(config.has_value()) << config.error().message;
    EXPECT_EQ(config->port, 9876);
    EXPECT_EQ(config->log_level, "warn");

    // Defaults should remain for unset fields.
    EXPECT_EQ(config->max_connections, 100u);
    EXPECT_EQ(config->buffer_pool_size_mb, 256u);
}

TEST_F(SystemDatabaseIntegrationTest, SetOverrideTakesHighestPriority) {
    // SET override takes highest priority over both default and file config.
    exec_ok("SET logging.level = 'error'");
    auto show_qr = exec_ok("SHOW logging.level");
    ASSERT_EQ(show_qr.rows.size(), 1u);
    EXPECT_EQ(show_qr.rows[0][1].as_string(), "error");

    // The SET value persists in sys_settings. When load_settings() is called
    // during startup, it applies sys_settings overrides to the config.
    Config fresh = Config::load_defaults();
    EXPECT_EQ(fresh.log_level, "info"); // Default.

    auto load_result = SystemBootstrap::load_settings(*engine_, *catalog_, fresh);
    ASSERT_TRUE(load_result.has_value()) << load_result.error().message;

    // sys_settings override (from SET) should take precedence over default.
    EXPECT_EQ(fresh.log_level, "error");
}

TEST_F(SystemDatabaseIntegrationTest, ConfigPriorityChainDefaultThenFileThenSysSettings) {
    // 1. Default: logging.level = "info", port = 6767.
    Config config = Config::load_defaults();
    EXPECT_EQ(config.log_level, "info");
    EXPECT_EQ(config.port, 6767);

    // 2. File override: port = 7777.
    auto config_path = data_dir_ / "giodb.json";
    {
        std::ofstream f(config_path);
        f << R"({"port": 7777})";
    }
    config = Config::load_from_file(config_path.string()).value();
    EXPECT_EQ(config.port, 7777);
    EXPECT_EQ(config.log_level, "info"); // Not overridden by file.

    // 3. sys_settings override: SET logging.level = 'debug' persisted earlier.
    exec_ok("SET logging.level = 'debug'");
    auto load_result = SystemBootstrap::load_settings(*engine_, *catalog_, config);
    ASSERT_TRUE(load_result.has_value()) << load_result.error().message;

    // sys_settings overrides everything (logging.level from SET, port from seed).
    EXPECT_EQ(config.log_level, "debug");
    // Port from sys_settings (seeded from original defaults = 6767) overrides file.
    EXPECT_EQ(config.port, 6767);
}

TEST_F(SystemDatabaseIntegrationTest, RemoveSetOverrideFallsBackToDefault) {
    // Override logging.level via SET.
    exec_ok("SET logging.level = 'trace'");
    auto before = exec_ok("SHOW logging.level");
    EXPECT_EQ(before.rows[0][1].as_string(), "trace");

    // Revert the SET override by setting back to the default value.
    exec_ok("SET logging.level = 'info'");

    auto after = exec_ok("SHOW logging.level");
    EXPECT_EQ(after.rows[0][1].as_string(), "info");
}

// =============================================================================
// End-to-End: Full lifecycle across all subsystems
// =============================================================================

TEST_F(SystemDatabaseIntegrationTest, FullLifecycleBootstrapSettingsProvidersSecrets) {
    // Step 1: Verify bootstrap created everything.
    EXPECT_TRUE(catalog_->get_database(system_database_name).has_value());
    EXPECT_TRUE(catalog_->get_table(system_database_id, "sys_settings").has_value());
    EXPECT_TRUE(catalog_->get_table(system_database_id, "sys_providers").has_value());

    // Step 2: Settings management.
    auto show_default = exec_ok("SHOW logging.level");
    EXPECT_EQ(show_default.rows[0][1].as_string(), "info");

    exec_ok("SET logging.level = 'debug'");
    auto show_changed = exec_ok("SHOW logging.level");
    EXPECT_EQ(show_changed.rows[0][1].as_string(), "debug");

    // Step 3: Provider registration.
    auto oai_id = insert_provider("openai-prod",
                                  "openai",
                                  "https://api.openai.com/v1",
                                  "text-embedding-3-small",
                                  "sk-lifecycle-key",
                                  true);
    insert_provider("ollama-local", "ollama", "http://localhost:11434", "nomic-embed-text");

    auto show_providers = exec_ok("SHOW PROVIDERS");
    EXPECT_EQ(show_providers.rows.size(), 2u);

    // Step 4: Verify encryption.
    std::string raw = read_raw_api_key(oai_id);
    EXPECT_TRUE(SecretsManager::looks_encrypted(raw));

    auto oai = provider_cache_->get("openai-prod");
    ASSERT_TRUE(oai.has_value());
    EXPECT_EQ(oai->api_key, "sk-lifecycle-key");

    // Step 5: Reload caches and verify everything persisted.
    reload_caches();

    // Settings survived.
    auto post_reload_setting = exec_ok("SHOW logging.level");
    EXPECT_EQ(post_reload_setting.rows[0][1].as_string(), "debug");

    // Providers survived.
    auto post_reload_providers = exec_ok("SHOW PROVIDERS");
    EXPECT_EQ(post_reload_providers.rows.size(), 2u);

    // Encrypted key decrypted correctly after reload.
    auto post_reload_oai = provider_cache_->get("openai-prod");
    ASSERT_TRUE(post_reload_oai.has_value());
    EXPECT_EQ(post_reload_oai->api_key, "sk-lifecycle-key");

    // Step 6: Verify system protection still in effect.
    exec_error("DROP DATABASE giodb_system", StatusCode::CONSTRAINT_VIOLATION);
    use_database(system_database_name);
    exec_error("DROP TABLE sys_settings", StatusCode::CONSTRAINT_VIOLATION);
    exec_error("DROP TABLE sys_providers", StatusCode::CONSTRAINT_VIOLATION);
}
