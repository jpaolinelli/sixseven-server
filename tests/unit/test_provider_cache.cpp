#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/secrets_manager.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/provider_cache.h"
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
// Test fixture for provider cache and sys_providers (GDB-190)
// =============================================================================

class ProviderCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_provider_cache";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        config_ = Config::load_defaults();
        cache_ = std::make_unique<ProviderCache>();

        // Bootstrap system database + sys_settings + sys_providers.
        auto boot = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(boot.has_value()) << boot.error().message;

        // Load provider cache.
        auto load = cache_->load(*engine_);
        ASSERT_TRUE(load.has_value()) << load.error().message;

        // Wire up the cache.
        engine_->set_provider_cache(cache_.get());

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
        return result ? std::move(*result) : QueryResult{};
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

    /// Insert a provider via SQL. Returns the provider_id used.
    int32_t insert_provider(const std::string& name,
                            const std::string& type,
                            const std::string& endpoint,
                            const std::string& model,
                            const std::string& api_key = "",
                            bool is_default = false) {
        static int32_t next_id = 1;
        int32_t id = next_id++;

        use_database(system_database_name);

        std::string sql = "INSERT INTO sys_providers VALUES (" + std::to_string(id) + ", '" + name +
                          "', '" + type + "', '" + endpoint + "', '" + model + "', " +
                          (api_key.empty() ? "NULL" : "'" + api_key + "'") + ", " +
                          (is_default ? "TRUE" : "FALSE") + ", NULL)";
        exec_ok(sql);

        use_database("demo");
        return id;
    }

    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<ProviderCache> cache_;
    Config config_;
};

// =============================================================================
// AC: sys_providers table created in sixseven_system on bootstrap
// =============================================================================

TEST_F(ProviderCacheTest, SysProvidersTableCreated) {
    auto schema = catalog_->get_table(system_database_id, "sys_providers");
    ASSERT_TRUE(schema.has_value()) << schema.error().message;
    EXPECT_EQ(schema->name, "sys_providers");
    ASSERT_EQ(schema->columns.size(), 8u);
    EXPECT_EQ(schema->columns[0].name, "provider_id");
    EXPECT_EQ(schema->columns[0].type_id, TypeId::INT32);
    EXPECT_EQ(schema->columns[1].name, "name");
    EXPECT_EQ(schema->columns[1].type_id, TypeId::STRING);
    EXPECT_EQ(schema->columns[2].name, "type");
    EXPECT_EQ(schema->columns[2].type_id, TypeId::STRING);
    EXPECT_EQ(schema->columns[3].name, "endpoint");
    EXPECT_EQ(schema->columns[3].type_id, TypeId::STRING);
    EXPECT_EQ(schema->columns[4].name, "model");
    EXPECT_EQ(schema->columns[4].type_id, TypeId::STRING);
    EXPECT_EQ(schema->columns[5].name, "api_key_encrypted");
    EXPECT_EQ(schema->columns[5].type_id, TypeId::STRING);
    EXPECT_EQ(schema->columns[6].name, "is_default");
    EXPECT_EQ(schema->columns[6].type_id, TypeId::BOOL);
    EXPECT_EQ(schema->columns[7].name, "created_at");
    EXPECT_EQ(schema->columns[7].type_id, TypeId::TIMESTAMP);
    EXPECT_EQ(schema->pk_columns, "provider_id");
}

TEST_F(ProviderCacheTest, SysProvidersSchemaDefinition) {
    auto schema = sys_providers_schema();
    EXPECT_EQ(schema.name, "sys_providers");
    ASSERT_EQ(schema.columns.size(), 8u);
    EXPECT_EQ(schema.columns[0].name, "provider_id");
    EXPECT_EQ(schema.columns[1].name, "name");
    EXPECT_EQ(schema.columns[2].name, "type");
    EXPECT_EQ(schema.columns[3].name, "endpoint");
    EXPECT_EQ(schema.columns[4].name, "model");
    EXPECT_EQ(schema.columns[5].name, "api_key_encrypted");
    EXPECT_EQ(schema.columns[6].name, "is_default");
    EXPECT_EQ(schema.columns[7].name, "created_at");
    EXPECT_EQ(schema.pk_columns, "provider_id");
}

// =============================================================================
// AC: Providers can be added via standard SQL
// =============================================================================

TEST_F(ProviderCacheTest, InsertProvider) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-test123",
                    true);

    auto provider = cache_->get("openai-prod");
    ASSERT_TRUE(provider.has_value());
    EXPECT_EQ(provider->name, "openai-prod");
    EXPECT_EQ(provider->type, "openai");
    EXPECT_EQ(provider->endpoint, "https://api.openai.com/v1");
    EXPECT_EQ(provider->model, "text-embedding-3-small");
    EXPECT_EQ(provider->api_key, "sk-test123");
    EXPECT_TRUE(provider->is_default);
}

TEST_F(ProviderCacheTest, InsertMultipleProviders) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-test123",
                    true);
    insert_provider("ollama-local", "ollama", "http://localhost:11434", "nomic-embed-text");
    insert_provider("onnx-bert", "onnx", "file:///models/bert", "bert-base");

    EXPECT_EQ(cache_->size(), 3u);

    auto providers = cache_->get_all();
    ASSERT_EQ(providers.size(), 3u);
    // Sorted by name.
    EXPECT_EQ(providers[0].name, "ollama-local");
    EXPECT_EQ(providers[1].name, "onnx-bert");
    EXPECT_EQ(providers[2].name, "openai-prod");
}

TEST_F(ProviderCacheTest, InsertProviderWithoutApiKey) {
    insert_provider("ollama-local", "ollama", "http://localhost:11434", "nomic-embed-text");

    auto provider = cache_->get("ollama-local");
    ASSERT_TRUE(provider.has_value());
    EXPECT_EQ(provider->api_key, "");
    EXPECT_FALSE(provider->is_default);
}

// =============================================================================
// AC: Providers can be updated via standard SQL
// =============================================================================

TEST_F(ProviderCacheTest, UpdateProviderModel) {
    insert_provider(
        "openai-prod", "openai", "https://api.openai.com/v1", "text-embedding-3-small", "sk-test");

    use_database(system_database_name);
    exec_ok("UPDATE sys_providers SET model = 'text-embedding-3-large' "
            "WHERE name = 'openai-prod'");
    use_database("demo");

    auto provider = cache_->get("openai-prod");
    ASSERT_TRUE(provider.has_value());
    EXPECT_EQ(provider->model, "text-embedding-3-large");
}

TEST_F(ProviderCacheTest, UpdateProviderEndpoint) {
    insert_provider("ollama-local", "ollama", "http://localhost:11434", "nomic-embed-text");

    use_database(system_database_name);
    exec_ok("UPDATE sys_providers SET endpoint = 'http://gpu-server:11434' "
            "WHERE name = 'ollama-local'");
    use_database("demo");

    auto provider = cache_->get("ollama-local");
    ASSERT_TRUE(provider.has_value());
    EXPECT_EQ(provider->endpoint, "http://gpu-server:11434");
}

// =============================================================================
// AC: Providers can be listed via SHOW PROVIDERS
// =============================================================================

TEST_F(ProviderCacheTest, ShowProvidersEmpty) {
    auto qr = exec_ok("SHOW PROVIDERS");
    ASSERT_EQ(qr.column_names.size(), 5u);
    EXPECT_EQ(qr.column_names[0], "name");
    EXPECT_EQ(qr.column_names[1], "type");
    EXPECT_EQ(qr.column_names[2], "endpoint");
    EXPECT_EQ(qr.column_names[3], "model");
    EXPECT_EQ(qr.column_names[4], "is_default");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(ProviderCacheTest, ShowProvidersWithData) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-test",
                    true);
    insert_provider("ollama-local", "ollama", "http://localhost:11434", "nomic-embed-text");

    auto qr = exec_ok("SHOW PROVIDERS");
    ASSERT_EQ(qr.rows.size(), 2u);

    // Sorted by name.
    EXPECT_EQ(qr.rows[0][0].as_string(), "ollama-local");
    EXPECT_EQ(qr.rows[0][1].as_string(), "ollama");
    EXPECT_EQ(qr.rows[0][2].as_string(), "http://localhost:11434");
    EXPECT_EQ(qr.rows[0][3].as_string(), "nomic-embed-text");
    EXPECT_FALSE(qr.rows[0][4].as_bool());

    EXPECT_EQ(qr.rows[1][0].as_string(), "openai-prod");
    EXPECT_EQ(qr.rows[1][1].as_string(), "openai");
    EXPECT_EQ(qr.rows[1][2].as_string(), "https://api.openai.com/v1");
    EXPECT_EQ(qr.rows[1][3].as_string(), "text-embedding-3-small");
    EXPECT_TRUE(qr.rows[1][4].as_bool());
}

TEST_F(ProviderCacheTest, ShowProviderSingular) {
    insert_provider("test-provider", "ollama", "http://localhost:11434", "test-model");

    auto qr = exec_ok("SHOW PROVIDER");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "test-provider");
}

// =============================================================================
// AC: Providers can be removed via standard SQL
// =============================================================================

TEST_F(ProviderCacheTest, DeleteProvider) {
    insert_provider(
        "openai-prod", "openai", "https://api.openai.com/v1", "text-embedding-3-small", "sk-test");
    insert_provider("ollama-local", "ollama", "http://localhost:11434", "nomic-embed-text");
    EXPECT_EQ(cache_->size(), 2u);

    use_database(system_database_name);
    exec_ok("DELETE FROM sys_providers WHERE name = 'openai-prod'");
    use_database("demo");

    EXPECT_EQ(cache_->size(), 1u);
    EXPECT_FALSE(cache_->get("openai-prod").has_value());
    EXPECT_TRUE(cache_->get("ollama-local").has_value());
}

// =============================================================================
// AC: At most one provider marked as default
// =============================================================================

TEST_F(ProviderCacheTest, SingleDefault) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-test",
                    true);

    auto def = cache_->get_default();
    ASSERT_TRUE(def.has_value());
    EXPECT_EQ(def->name, "openai-prod");
}

TEST_F(ProviderCacheTest, NoDefault) {
    insert_provider("ollama-local", "ollama", "http://localhost:11434", "nomic-embed-text");

    auto def = cache_->get_default();
    EXPECT_FALSE(def.has_value());
}

TEST_F(ProviderCacheTest, ChangeDefault) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-test",
                    true);
    insert_provider("ollama-local", "ollama", "http://localhost:11434", "nomic-embed-text");

    // Unset old default and set new default.
    use_database(system_database_name);
    exec_ok("UPDATE sys_providers SET is_default = FALSE WHERE name = 'openai-prod'");
    exec_ok("UPDATE sys_providers SET is_default = TRUE WHERE name = 'ollama-local'");
    use_database("demo");

    auto def = cache_->get_default();
    ASSERT_TRUE(def.has_value());
    EXPECT_EQ(def->name, "ollama-local");

    // Verify old default is no longer default.
    auto old = cache_->get("openai-prod");
    ASSERT_TRUE(old.has_value());
    EXPECT_FALSE(old->is_default);
}

// =============================================================================
// AC: Provider name validated when referenced by EMBEDDING columns
// =============================================================================

TEST_F(ProviderCacheTest, ValidateExistingProvider) {
    insert_provider("openai-prod", "openai", "https://api.openai.com/v1", "text-embedding-3-small");

    auto result = cache_->validate_provider_exists("openai-prod");
    EXPECT_TRUE(result.has_value());
}

TEST_F(ProviderCacheTest, ValidateNonexistentProvider) {
    auto result = cache_->validate_provider_exists("nonexistent-provider");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// =============================================================================
// AC: Cannot delete a provider in use by existing EMBEDDING columns
// =============================================================================

TEST_F(ProviderCacheTest, IsProviderInUseWithNoEmbeddings) {
    insert_provider("openai-prod", "openai", "https://api.openai.com/v1", "text-embedding-3-small");

    EXPECT_FALSE(cache_->is_provider_in_use("openai-prod", *catalog_));
}

TEST_F(ProviderCacheTest, IsProviderInUseWithEmbedding) {
    insert_provider("openai-prod", "openai", "https://api.openai.com/v1", "text-embedding-3-small");

    // Create a table with an EMBEDDING column that references this provider.
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

    EXPECT_TRUE(cache_->is_provider_in_use("openai-prod", *catalog_));
    EXPECT_FALSE(cache_->is_provider_in_use("nonexistent", *catalog_));
}

// =============================================================================
// AC: In-memory cache stays consistent with table
// =============================================================================

TEST_F(ProviderCacheTest, CacheConsistentAfterInsert) {
    EXPECT_EQ(cache_->size(), 0u);

    insert_provider("test-provider", "ollama", "http://localhost:11434", "test-model");

    EXPECT_EQ(cache_->size(), 1u);
    auto p = cache_->get("test-provider");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->type, "ollama");
}

TEST_F(ProviderCacheTest, CacheConsistentAfterUpdate) {
    insert_provider("test-provider", "ollama", "http://localhost:11434", "old-model");

    use_database(system_database_name);
    exec_ok("UPDATE sys_providers SET model = 'new-model' WHERE name = 'test-provider'");
    use_database("demo");

    auto p = cache_->get("test-provider");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->model, "new-model");
}

TEST_F(ProviderCacheTest, CacheConsistentAfterDelete) {
    insert_provider("test-provider", "ollama", "http://localhost:11434", "test-model");
    EXPECT_EQ(cache_->size(), 1u);

    use_database(system_database_name);
    exec_ok("DELETE FROM sys_providers WHERE name = 'test-provider'");
    use_database("demo");

    EXPECT_EQ(cache_->size(), 0u);
    EXPECT_FALSE(cache_->get("test-provider").has_value());
}

TEST_F(ProviderCacheTest, CacheReloadPicksUpPersistedChanges) {
    insert_provider("test-provider", "ollama", "http://localhost:11434", "test-model");

    // Create a fresh cache and reload from sys_providers.
    auto fresh_cache = std::make_unique<ProviderCache>();
    auto load = fresh_cache->load(*engine_);
    ASSERT_TRUE(load.has_value()) << load.error().message;

    auto p = fresh_cache->get("test-provider");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->type, "ollama");
}

// =============================================================================
// AC: Standard SQL SELECT works against sys_providers
// =============================================================================

TEST_F(ProviderCacheTest, SelectFromSysProviders) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-test",
                    true);
    insert_provider("ollama-local", "ollama", "http://localhost:11434", "nomic-embed-text");

    use_database(system_database_name);
    auto qr = exec_ok("SELECT name, type, model FROM sys_providers");
    use_database("demo");

    ASSERT_EQ(qr.rows.size(), 2u);
}

TEST_F(ProviderCacheTest, SelectWithWhereClause) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-test",
                    true);
    insert_provider("ollama-local", "ollama", "http://localhost:11434", "nomic-embed-text");

    use_database(system_database_name);
    auto qr = exec_ok("SELECT name FROM sys_providers WHERE type = 'openai'");
    use_database("demo");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "openai-prod");
}

// =============================================================================
// ProviderConfig struct tests
// =============================================================================

TEST_F(ProviderCacheTest, ProviderConfigDefaults) {
    ProviderConfig config;
    EXPECT_EQ(config.provider_id, 0);
    EXPECT_TRUE(config.name.empty());
    EXPECT_TRUE(config.type.empty());
    EXPECT_TRUE(config.endpoint.empty());
    EXPECT_TRUE(config.model.empty());
    EXPECT_TRUE(config.api_key.empty());
    EXPECT_FALSE(config.is_default);
}

// =============================================================================
// Cache method unit tests
// =============================================================================

TEST_F(ProviderCacheTest, GetNonexistentProvider) {
    auto p = cache_->get("does-not-exist");
    EXPECT_FALSE(p.has_value());
}

TEST_F(ProviderCacheTest, GetAllEmpty) {
    auto providers = cache_->get_all();
    EXPECT_EQ(providers.size(), 0u);
}

TEST_F(ProviderCacheTest, SizeEmpty) {
    EXPECT_EQ(cache_->size(), 0u);
}

// =============================================================================
// E2E: EMBEDDING column creation validates provider exists (GDB-190 review fix)
// =============================================================================

TEST_F(ProviderCacheTest, CreateTableEmbeddingWithValidProvider) {
    insert_provider("openai-prod", "openai", "https://api.openai.com/v1", "text-embedding-3-small");

    // Creating a table with EMBEDDING referencing an existing provider should succeed.
    auto result = engine_->execute("CREATE TABLE articles ("
                                   "id INT, body VARCHAR, "
                                   "vec EMBEDDING(384, body, 'openai-prod'), "
                                   "PRIMARY KEY (id)"
                                   ")");
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_F(ProviderCacheTest, CreateTableEmbeddingWithUnknownProviderFails) {
    // Creating a table with EMBEDDING referencing a nonexistent provider should fail.
    auto result = engine_->execute("CREATE TABLE articles ("
                                   "id INT, body VARCHAR, "
                                   "vec EMBEDDING(384, body, 'nonexistent-provider'), "
                                   "PRIMARY KEY (id)"
                                   ")");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(ProviderCacheTest, CreateTableEmbeddingWithNoProviderCacheIsPermissive) {
    // Without a provider cache, EMBEDDING columns pass without validation.
    engine_->set_provider_cache(nullptr);

    auto result = engine_->execute("CREATE TABLE articles ("
                                   "id INT, body VARCHAR, "
                                   "vec EMBEDDING(384, body, 'any-provider'), "
                                   "PRIMARY KEY (id)"
                                   ")");
    EXPECT_TRUE(result.has_value()) << result.error().message;

    // Restore cache for TearDown.
    engine_->set_provider_cache(cache_.get());
}

// =============================================================================
// E2E: Cannot DELETE a provider referenced by EMBEDDING columns (GDB-190 review fix)
// =============================================================================

TEST_F(ProviderCacheTest, DeleteInUseProviderFails) {
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

    // Attempting to DELETE the in-use provider should fail.
    use_database(system_database_name);
    auto result = engine_->execute("DELETE FROM sys_providers WHERE name = 'openai-prod'");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION);
    use_database("demo");

    // The provider should still exist in the cache (compensating re-insert).
    EXPECT_TRUE(cache_->get("openai-prod").has_value());
}

TEST_F(ProviderCacheTest, DeleteUnusedProviderSucceeds) {
    insert_provider("openai-prod", "openai", "https://api.openai.com/v1", "text-embedding-3-small");
    insert_provider("ollama-local", "ollama", "http://localhost:11434", "nomic-embed-text");

    // Register an EMBEDDING column referencing only openai-prod.
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

    // Deleting the unused provider (ollama-local) should succeed.
    use_database(system_database_name);
    exec_ok("DELETE FROM sys_providers WHERE name = 'ollama-local'");
    use_database("demo");

    EXPECT_FALSE(cache_->get("ollama-local").has_value());
    EXPECT_TRUE(cache_->get("openai-prod").has_value());
}

// =============================================================================
// E2E: Default uniqueness auto-enforced (GDB-190 review fix)
// =============================================================================

TEST_F(ProviderCacheTest, InsertNewDefaultAutomaticallyUnsetsOld) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-test",
                    true);

    // Verify openai-prod is the default.
    auto def1 = cache_->get_default();
    ASSERT_TRUE(def1.has_value());
    EXPECT_EQ(def1->name, "openai-prod");

    // Insert a new provider with is_default=TRUE. The old default should be auto-unset.
    insert_provider(
        "ollama-local", "ollama", "http://localhost:11434", "nomic-embed-text", "", true);

    // The new default should be ollama-local.
    auto def2 = cache_->get_default();
    ASSERT_TRUE(def2.has_value());
    EXPECT_EQ(def2->name, "ollama-local");

    // The old provider should no longer be default.
    auto old = cache_->get("openai-prod");
    ASSERT_TRUE(old.has_value());
    EXPECT_FALSE(old->is_default);
}

TEST_F(ProviderCacheTest, UpdateToDefaultAutomaticallyUnsetsOld) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-test",
                    true);
    insert_provider("ollama-local", "ollama", "http://localhost:11434", "nomic-embed-text");

    // Update ollama-local to be the new default.
    use_database(system_database_name);
    exec_ok("UPDATE sys_providers SET is_default = TRUE WHERE name = 'ollama-local'");
    use_database("demo");

    // ollama-local should now be the sole default.
    auto def = cache_->get_default();
    ASSERT_TRUE(def.has_value());
    EXPECT_EQ(def->name, "ollama-local");

    // openai-prod should have been auto-unset.
    auto old = cache_->get("openai-prod");
    ASSERT_TRUE(old.has_value());
    EXPECT_FALSE(old->is_default);
}

TEST_F(ProviderCacheTest, InsertNonDefaultDoesNotAffectExistingDefault) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-test",
                    true);

    // Insert a non-default provider. The existing default should remain.
    insert_provider("ollama-local", "ollama", "http://localhost:11434", "nomic-embed-text");

    auto def = cache_->get_default();
    ASSERT_TRUE(def.has_value());
    EXPECT_EQ(def->name, "openai-prod");
    EXPECT_TRUE(def->is_default);
}

// =============================================================================
// sys_providers table protected from DROP
// =============================================================================

TEST_F(ProviderCacheTest, CannotDropSysProviders) {
    auto result = catalog_->drop_table(system_database_id, "sys_providers");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST_F(ProviderCacheTest, CannotDropSysProvidersViaSql) {
    use_database(system_database_name);
    exec_error("DROP TABLE sys_providers", StatusCode::CONSTRAINT_VIOLATION);
}

// =============================================================================
// GDB-191: Encrypted secrets storage for API keys
// =============================================================================

class EncryptedProviderCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ =
            std::filesystem::temp_directory_path() / "sixseven_test_encrypted_provider_cache";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        config_ = Config::load_defaults();
        cache_ = std::make_unique<ProviderCache>();

        // Create a SecretsManager with a test master key.
        auto key_path = (data_dir_ / "master.key").string();
        auto mgr = SecretsManager::create(key_path);
        ASSERT_TRUE(mgr.has_value()) << mgr.error().message;
        secrets_manager_ = std::make_unique<SecretsManager>(std::move(*mgr));

        // Wire up encryption.
        cache_->set_secrets_manager(secrets_manager_.get());

        // Bootstrap system database + sys_settings + sys_providers.
        auto boot = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(boot.has_value()) << boot.error().message;

        // Load provider cache.
        auto load = cache_->load(*engine_);
        ASSERT_TRUE(load.has_value()) << load.error().message;

        // Wire up the cache.
        engine_->set_provider_cache(cache_.get());

        // Switch to default database for user operations.
        use_database("demo");
    }

    void TearDown() override {
        engine_.reset();
        cache_.reset();
        secrets_manager_.reset();
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

    void use_database(const std::string& name) {
        auto db = catalog_->get_database(name);
        ASSERT_TRUE(db.has_value()) << "database '" << name << "' not found";
        engine_->set_current_database(db->database_id);
    }

    int32_t insert_provider(const std::string& name,
                            const std::string& type,
                            const std::string& endpoint,
                            const std::string& model,
                            const std::string& api_key = "",
                            bool is_default = false) {
        int32_t id = next_id_++;

        use_database(system_database_name);

        std::string sql = "INSERT INTO sys_providers VALUES (" + std::to_string(id) + ", '" + name +
                          "', '" + type + "', '" + endpoint + "', '" + model + "', " +
                          (api_key.empty() ? "NULL" : "'" + api_key + "'") + ", " +
                          (is_default ? "TRUE" : "FALSE") + ", NULL)";
        exec_ok(sql);

        use_database("demo");
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
        use_database("demo");

        if (qr.rows.empty() || qr.rows[0][0].is_null()) {
            return "";
        }
        return qr.rows[0][0].as_string();
    }

    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<ProviderCache> cache_;
    std::unique_ptr<SecretsManager> secrets_manager_;
    Config config_;
    int32_t next_id_ = 1;
};

// --- AC: API keys encrypted before storage in sys_providers ---

TEST_F(EncryptedProviderCacheTest, ApiKeyEncryptedOnDisk) {
    auto id = insert_provider("openai-prod",
                              "openai",
                              "https://api.openai.com/v1",
                              "text-embedding-3-small",
                              "sk-test123");

    // The raw value on disk should be encrypted (not the plaintext key).
    std::string raw = read_raw_api_key(id);
    EXPECT_NE(raw, "sk-test123");
    EXPECT_TRUE(SecretsManager::looks_encrypted(raw)) << "raw value should look encrypted: " << raw;
}

// --- AC: API keys decrypted correctly when loaded into memory ---

TEST_F(EncryptedProviderCacheTest, ApiKeyDecryptedInMemory) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-test123");

    auto provider = cache_->get("openai-prod");
    ASSERT_TRUE(provider.has_value());
    EXPECT_EQ(provider->api_key, "sk-test123");
}

// --- AC: SELECT * FROM sys_providers masks the api_key_encrypted column ---

TEST_F(EncryptedProviderCacheTest, SelectMasksApiKey) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-test123",
                    true);

    use_database(system_database_name);
    auto qr = exec_ok("SELECT name, api_key_encrypted FROM sys_providers");
    use_database("demo");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "openai-prod");
    EXPECT_EQ(qr.rows[0][1].as_string(), "********");
}

TEST_F(EncryptedProviderCacheTest, SelectStarMasksApiKey) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-test123",
                    true);

    use_database(system_database_name);
    auto qr = exec_ok("SELECT * FROM sys_providers");
    use_database("demo");

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
    EXPECT_EQ(qr.rows[0][static_cast<size_t>(key_col)].as_string(), "********");
}

TEST_F(EncryptedProviderCacheTest, NullApiKeyNotMasked) {
    insert_provider("ollama-local", "ollama", "http://localhost:11434", "nomic-embed-text");

    use_database(system_database_name);
    auto qr = exec_ok("SELECT api_key_encrypted FROM sys_providers");
    use_database("demo");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_TRUE(qr.rows[0][0].is_null());
}

// --- AC: Decrypted keys zeroed from memory on cleanup ---
// (Verified by SecureString tests in test_secrets_manager.cpp)

// --- Encryption round-trip with cache reload ---

TEST_F(EncryptedProviderCacheTest, CacheReloadDecryptsCorrectly) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-test123");

    // Create a fresh cache with the same SecretsManager and reload.
    auto fresh_cache = std::make_unique<ProviderCache>();
    fresh_cache->set_secrets_manager(secrets_manager_.get());
    auto load = fresh_cache->load(*engine_);
    ASSERT_TRUE(load.has_value()) << load.error().message;

    auto provider = fresh_cache->get("openai-prod");
    ASSERT_TRUE(provider.has_value());
    EXPECT_EQ(provider->api_key, "sk-test123");
}

TEST_F(EncryptedProviderCacheTest, UpdateApiKeyReEncrypts) {
    auto id = insert_provider("openai-prod",
                              "openai",
                              "https://api.openai.com/v1",
                              "text-embedding-3-small",
                              "sk-old-key");

    // Update the api_key.
    use_database(system_database_name);
    exec_ok("UPDATE sys_providers SET api_key_encrypted = 'sk-new-key' "
            "WHERE name = 'openai-prod'");
    use_database("demo");

    // The cache should have the new decrypted key.
    auto provider = cache_->get("openai-prod");
    ASSERT_TRUE(provider.has_value());
    EXPECT_EQ(provider->api_key, "sk-new-key");

    // The raw value on disk should be encrypted.
    std::string raw = read_raw_api_key(id);
    EXPECT_NE(raw, "sk-new-key");
    EXPECT_TRUE(SecretsManager::looks_encrypted(raw));
}

TEST_F(EncryptedProviderCacheTest, MultipleProvidersWithKeys) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-openai-123");
    insert_provider(
        "anthropic-prod", "openai", "https://api.anthropic.com/v1", "claude-3-opus", "sk-ant-456");
    insert_provider("ollama-local", "ollama", "http://localhost:11434", "nomic-embed-text");

    EXPECT_EQ(cache_->size(), 3u);

    auto p1 = cache_->get("openai-prod");
    ASSERT_TRUE(p1.has_value());
    EXPECT_EQ(p1->api_key, "sk-openai-123");

    auto p2 = cache_->get("anthropic-prod");
    ASSERT_TRUE(p2.has_value());
    EXPECT_EQ(p2->api_key, "sk-ant-456");

    auto p3 = cache_->get("ollama-local");
    ASSERT_TRUE(p3.has_value());
    EXPECT_EQ(p3->api_key, "");
}

TEST_F(EncryptedProviderCacheTest, ShowProvidersDoesNotExposeKeys) {
    insert_provider("openai-prod",
                    "openai",
                    "https://api.openai.com/v1",
                    "text-embedding-3-small",
                    "sk-secret");

    auto qr = exec_ok("SHOW PROVIDERS");
    ASSERT_EQ(qr.rows.size(), 1u);

    // SHOW PROVIDERS should not include api_key column at all.
    for (const auto& col : qr.column_names) {
        EXPECT_NE(col, "api_key_encrypted");
        EXPECT_NE(col, "api_key");
    }
}
