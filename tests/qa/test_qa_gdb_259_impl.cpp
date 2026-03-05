/// @file test_gdb_259.cpp
/// @brief Regression tests for GDB-259: CREATE TABLE never registers EMBEDDING
///        column metadata in catalog.
///
/// Verifies that execute_create_table() calls EmbeddingColumnManager to
/// register embedding metadata and auto-create companion HNSW indexes.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/vector/embedding_column.h"
#include "sixseven/vector/provider_registry.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class GDB259EmbeddingRegistrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_gdb259";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
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

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// Core regression: embedding metadata is registered during CREATE TABLE
// =============================================================================

TEST_F(GDB259EmbeddingRegistrationTest, CreateTableRegistersEmbeddingMetadata) {
    exec_ok("CREATE TABLE articles ("
            "  id INT,"
            "  title VARCHAR,"
            "  title_vec EMBEDDING(384, title, 'builtin/384')"
            ")");

    auto schema = catalog_.get_table(default_database_id, "articles");
    ASSERT_TRUE(schema.has_value()) << schema.error().message;

    auto embs = catalog_.list_embedding_columns(schema->table_id);
    ASSERT_EQ(embs.size(), 1u);
    EXPECT_EQ(embs[0].column_id, 2);
    EXPECT_EQ(embs[0].dimension, 384);
    EXPECT_EQ(embs[0].source_expr, "title");
    EXPECT_EQ(embs[0].provider, "builtin/384");
}

TEST_F(GDB259EmbeddingRegistrationTest, CreateTableAutoCreatesHnswIndex) {
    exec_ok("CREATE TABLE articles ("
            "  id INT,"
            "  title VARCHAR,"
            "  title_vec EMBEDDING(384, title, 'builtin/384')"
            ")");

    auto schema = catalog_.get_table(default_database_id, "articles");
    ASSERT_TRUE(schema.has_value()) << schema.error().message;

    auto indexes = catalog_.list_indexes(schema->table_id);
    ASSERT_EQ(indexes.size(), 1u);
    EXPECT_EQ(indexes[0].name, "hnsw_articles_title_vec");
    EXPECT_EQ(indexes[0].index_type, "hnsw");
    EXPECT_EQ(indexes[0].columns, "title_vec");
}

// =============================================================================
// Multiple EMBEDDING columns
// =============================================================================

TEST_F(GDB259EmbeddingRegistrationTest, MultipleEmbeddingColumnsAllRegistered) {
    exec_ok("CREATE TABLE docs ("
            "  id INT,"
            "  title VARCHAR,"
            "  body VARCHAR,"
            "  title_vec EMBEDDING(384, title, 'builtin/384'),"
            "  body_vec EMBEDDING(768, body, 'builtin/768')"
            ")");

    auto schema = catalog_.get_table(default_database_id, "docs");
    ASSERT_TRUE(schema.has_value()) << schema.error().message;

    auto embs = catalog_.list_embedding_columns(schema->table_id);
    ASSERT_EQ(embs.size(), 2u);

    // First embedding column (title_vec at ordinal 3).
    EXPECT_EQ(embs[0].column_id, 3);
    EXPECT_EQ(embs[0].dimension, 384);
    EXPECT_EQ(embs[0].source_expr, "title");
    EXPECT_EQ(embs[0].provider, "builtin/384");

    // Second embedding column (body_vec at ordinal 4).
    EXPECT_EQ(embs[1].column_id, 4);
    EXPECT_EQ(embs[1].dimension, 768);
    EXPECT_EQ(embs[1].source_expr, "body");
    EXPECT_EQ(embs[1].provider, "builtin/768");

    // Both should get companion HNSW indexes.
    auto indexes = catalog_.list_indexes(schema->table_id);
    ASSERT_EQ(indexes.size(), 2u);
    EXPECT_EQ(indexes[0].name, "hnsw_docs_title_vec");
    EXPECT_EQ(indexes[1].name, "hnsw_docs_body_vec");
}

// =============================================================================
// Table without EMBEDDING columns is unaffected
// =============================================================================

TEST_F(GDB259EmbeddingRegistrationTest, NonEmbeddingTableHasNoRegistration) {
    exec_ok("CREATE TABLE users (id INT, name VARCHAR, age INT)");

    auto schema = catalog_.get_table(default_database_id, "users");
    ASSERT_TRUE(schema.has_value()) << schema.error().message;

    auto embs = catalog_.list_embedding_columns(schema->table_id);
    EXPECT_TRUE(embs.empty());

    auto indexes = catalog_.list_indexes(schema->table_id);
    EXPECT_TRUE(indexes.empty());
}

// =============================================================================
// End-to-end: REEMBED works after CREATE TABLE (no manual registration)
// =============================================================================

TEST_F(GDB259EmbeddingRegistrationTest, ReembedWorksAfterCreateTable) {
    // Register a builtin provider so REEMBED can resolve it.
    EmbeddingProviderConfig prov_config;
    prov_config.name = "builtin/4";
    prov_config.type = "builtin";
    prov_config.dimension = 4;
    auto prov_reg = catalog_.register_embedding_provider(prov_config);
    ASSERT_TRUE(prov_reg.has_value()) << prov_reg.error().message;

    auto provider_registry = std::make_unique<ProviderRegistry>(catalog_);
    engine_->set_provider_registry(provider_registry.get());

    // CREATE TABLE should auto-register the embedding column.
    exec_ok("CREATE TABLE articles ("
            "  id INT,"
            "  title VARCHAR,"
            "  title_vec EMBEDDING(4, title, 'builtin/4')"
            ")");

    // Verify metadata was registered (previously empty).
    auto schema = catalog_.get_table(default_database_id, "articles");
    ASSERT_TRUE(schema.has_value());
    auto embs = catalog_.list_embedding_columns(schema->table_id);
    ASSERT_EQ(embs.size(), 1u);

    // Insert test data.
    auto ts = storage_->get_table_storage(schema->table_id);
    ASSERT_TRUE(ts.has_value());
    auto* table_storage = *ts;

    std::vector<Value> row_values;
    row_values.push_back(Value(static_cast<int32_t>(1)));
    row_values.push_back(Value(std::string("hello world")));
    row_values.push_back(Value(Embedding{0.0F, 0.0F, 0.0F, 0.0F}));

    auto serialized = TupleSerializer::serialize(row_values, table_storage->storage_schema);
    ASSERT_TRUE(serialized.has_value()) << serialized.error().message;

    auto rid = table_storage->heap->insert_tuple(*serialized);
    ASSERT_TRUE(rid.has_value()) << rid.error().message;

    // REEMBED should now find the embedding column (previously failed with
    // "table 'articles' has no EMBEDDING columns").
    auto qr = exec_ok("REEMBED TABLE articles");
    EXPECT_EQ(qr.affected_rows, 1);
}

// =============================================================================
// SHOW EMBEDDINGS works after CREATE TABLE
// =============================================================================

TEST_F(GDB259EmbeddingRegistrationTest, ShowEmbeddingsReturnsData) {
    exec_ok("CREATE TABLE articles ("
            "  id INT,"
            "  title VARCHAR,"
            "  title_vec EMBEDDING(384, title, 'builtin/384')"
            ")");

    auto qr = exec_ok("SHOW EMBEDDINGS FROM articles");
    ASSERT_EQ(qr.rows.size(), 1u);
    // Columns: table_name, column_name, dimension, source_expr, provider.
    EXPECT_EQ(qr.rows[0][0].as_string(), "articles");
    EXPECT_EQ(qr.rows[0][1].as_string(), "title_vec");
    EXPECT_EQ(qr.rows[0][2].as_int32(), 384);
    EXPECT_EQ(qr.rows[0][3].as_string(), "title");
    EXPECT_EQ(qr.rows[0][4].as_string(), "builtin/384");
}

// =============================================================================
// IF NOT EXISTS with EMBEDDING — second call is a no-op
// =============================================================================

TEST_F(GDB259EmbeddingRegistrationTest, IfNotExistsSkipsRegistration) {
    exec_ok("CREATE TABLE articles ("
            "  id INT,"
            "  title VARCHAR,"
            "  title_vec EMBEDDING(384, title, 'builtin/384')"
            ")");

    // Second CREATE with IF NOT EXISTS should succeed without double-registering.
    auto qr = exec_ok("CREATE TABLE IF NOT EXISTS articles ("
                      "  id INT,"
                      "  title VARCHAR,"
                      "  title_vec EMBEDDING(384, title, 'builtin/384')"
                      ")");
    EXPECT_EQ(qr.message, "CREATE TABLE");

    // Still only one embedding column registered.
    auto schema = catalog_.get_table(default_database_id, "articles");
    ASSERT_TRUE(schema.has_value());
    auto embs = catalog_.list_embedding_columns(schema->table_id);
    EXPECT_EQ(embs.size(), 1u);
}
