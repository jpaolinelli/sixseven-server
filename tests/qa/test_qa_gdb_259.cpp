/// @file test_qa_gdb_259.cpp
/// @brief Adversarial QA tests for GDB-259: CREATE TABLE never registers
///        EMBEDDING column metadata in catalog.
///
/// Tests edge cases, boundary values, error paths, and state consistency
/// for the EMBEDDING column registration fix in execute_create_table().

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
#include <limits>
#include <memory>
#include <string>

using namespace sixseven;

// =============================================================================
// Test fixture — mirrors GDB259 regression test fixture
// =============================================================================

class QA_GDB259 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb259";
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
        if (!result.has_value()) {
            ADD_FAILURE() << result.error().message;
            return {};
        }
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
// Boundary: Minimum valid dimension (1)
// =============================================================================

TEST_F(QA_GDB259, EmbeddingWithDimensionOne) {
    exec_ok("CREATE TABLE small_emb ("
            "  id INT,"
            "  name VARCHAR,"
            "  vec EMBEDDING(1, name, 'builtin/1')"
            ")");

    auto schema = catalog_.get_table(default_database_id, "small_emb");
    ASSERT_TRUE(schema.has_value()) << schema.error().message;

    auto embs = catalog_.list_embedding_columns(schema->table_id);
    ASSERT_EQ(embs.size(), 1u);
    EXPECT_EQ(embs[0].dimension, 1);
    EXPECT_EQ(embs[0].source_expr, "name");
    EXPECT_EQ(embs[0].provider, "builtin/1");

    // Companion HNSW index should exist.
    auto indexes = catalog_.list_indexes(schema->table_id);
    ASSERT_EQ(indexes.size(), 1u);
    EXPECT_EQ(indexes[0].name, "hnsw_small_emb_vec");
}

// =============================================================================
// Boundary: Large dimension
// =============================================================================

TEST_F(QA_GDB259, EmbeddingWithLargeDimension) {
    exec_ok("CREATE TABLE large_dim ("
            "  id INT,"
            "  content VARCHAR,"
            "  vec EMBEDDING(4096, content, 'builtin/4096')"
            ")");

    auto schema = catalog_.get_table(default_database_id, "large_dim");
    ASSERT_TRUE(schema.has_value()) << schema.error().message;

    auto embs = catalog_.list_embedding_columns(schema->table_id);
    ASSERT_EQ(embs.size(), 1u);
    EXPECT_EQ(embs[0].dimension, 4096);
}

// =============================================================================
// Embedding column as the FIRST column (ordinal 0)
// =============================================================================

TEST_F(QA_GDB259, EmbeddingAtOrdinalZero) {
    exec_ok("CREATE TABLE emb_first ("
            "  vec EMBEDDING(128, name, 'builtin/128'),"
            "  name VARCHAR,"
            "  id INT"
            ")");

    auto schema = catalog_.get_table(default_database_id, "emb_first");
    ASSERT_TRUE(schema.has_value()) << schema.error().message;

    auto embs = catalog_.list_embedding_columns(schema->table_id);
    ASSERT_EQ(embs.size(), 1u);
    EXPECT_EQ(embs[0].column_id, 0); // First column — ordinal 0.
    EXPECT_EQ(embs[0].dimension, 128);
    EXPECT_EQ(embs[0].source_expr, "name");

    // Verify HNSW index uses the actual column name, not "col0".
    auto indexes = catalog_.list_indexes(schema->table_id);
    ASSERT_EQ(indexes.size(), 1u);
    EXPECT_EQ(indexes[0].name, "hnsw_emb_first_vec");
    EXPECT_EQ(indexes[0].columns, "vec");
}

// =============================================================================
// Multiple EMBEDDING columns referencing the SAME source column
// =============================================================================

TEST_F(QA_GDB259, TwoEmbeddingsOnSameSource) {
    exec_ok("CREATE TABLE dual ("
            "  id INT,"
            "  title VARCHAR,"
            "  small_vec EMBEDDING(128, title, 'builtin/128'),"
            "  large_vec EMBEDDING(768, title, 'builtin/768')"
            ")");

    auto schema = catalog_.get_table(default_database_id, "dual");
    ASSERT_TRUE(schema.has_value()) << schema.error().message;

    auto embs = catalog_.list_embedding_columns(schema->table_id);
    ASSERT_EQ(embs.size(), 2u);

    // Both reference "title" but with different dimensions/providers.
    EXPECT_EQ(embs[0].source_expr, "title");
    EXPECT_EQ(embs[0].dimension, 128);
    EXPECT_EQ(embs[1].source_expr, "title");
    EXPECT_EQ(embs[1].dimension, 768);

    // Both should get separate HNSW indexes.
    auto indexes = catalog_.list_indexes(schema->table_id);
    ASSERT_EQ(indexes.size(), 2u);
    EXPECT_EQ(indexes[0].name, "hnsw_dual_small_vec");
    EXPECT_EQ(indexes[1].name, "hnsw_dual_large_vec");
}

// =============================================================================
// Multi-column source expression (via direct API — parser only supports single)
// =============================================================================

TEST_F(QA_GDB259, MultiColumnSourceViaDirectApi) {
    // The SQL parser only accepts a single source column name.
    // But multi-column source_expr ("title, body") is valid in the catalog.
    // Test via the EmbeddingColumnManager API.
    TableSchema ts;
    ts.name = "multi_src";
    ts.columns = {{0, "id", TypeId::INT32, false, ""},
                  {1, "title", TypeId::STRING, true, ""},
                  {2, "body", TypeId::STRING, true, ""},
                  {3, "vec", TypeId::EMBEDDING, true, ""}};
    auto tid = catalog_.create_table(default_database_id, std::move(ts));
    ASSERT_TRUE(tid.has_value()) << tid.error().message;

    EmbeddingColumnDef def;
    def.table_id = *tid;
    def.column_id = 3;
    def.dimension = 384;
    def.source_expr = "title, body";
    def.provider = "test";

    EmbeddingColumnManager mgr(catalog_);
    auto result = mgr.register_table_embeddings(*tid, {def});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto embs = catalog_.list_embedding_columns(*tid);
    ASSERT_EQ(embs.size(), 1u);
    EXPECT_EQ(embs[0].source_expr, "title, body");
}

// =============================================================================
// State consistency: Create → Drop → Re-create
// =============================================================================

TEST_F(QA_GDB259, CreateDropRecreateEmbeddingTable) {
    // First create.
    exec_ok("CREATE TABLE articles ("
            "  id INT,"
            "  title VARCHAR,"
            "  vec EMBEDDING(384, title, 'builtin/384')"
            ")");

    auto schema1 = catalog_.get_table(default_database_id, "articles");
    ASSERT_TRUE(schema1.has_value());
    auto embs1 = catalog_.list_embedding_columns(schema1->table_id);
    ASSERT_EQ(embs1.size(), 1u);

    // Drop.
    exec_ok("DROP TABLE articles");

    // Verify embedding metadata is gone after drop.
    EXPECT_TRUE(catalog_.list_all_embedding_columns().empty());

    // Re-create with different dimensions.
    exec_ok("CREATE TABLE articles ("
            "  id INT,"
            "  title VARCHAR,"
            "  vec EMBEDDING(768, title, 'builtin/768')"
            ")");

    auto schema2 = catalog_.get_table(default_database_id, "articles");
    ASSERT_TRUE(schema2.has_value());

    auto embs2 = catalog_.list_embedding_columns(schema2->table_id);
    ASSERT_EQ(embs2.size(), 1u);
    EXPECT_EQ(embs2[0].dimension, 768); // Should be new dimension, not old.
    EXPECT_EQ(embs2[0].provider, "builtin/768");

    // Should have a new HNSW index.
    auto indexes = catalog_.list_indexes(schema2->table_id);
    ASSERT_EQ(indexes.size(), 1u);
    EXPECT_EQ(indexes[0].name, "hnsw_articles_vec");
}

// =============================================================================
// Drop table cleans up ALL embedding metadata and indexes
// =============================================================================

TEST_F(QA_GDB259, DropTableCleansUpMultipleEmbeddings) {
    exec_ok("CREATE TABLE docs ("
            "  id INT,"
            "  title VARCHAR,"
            "  body VARCHAR,"
            "  t_vec EMBEDDING(128, title, 'builtin/128'),"
            "  b_vec EMBEDDING(256, body, 'builtin/256')"
            ")");

    auto schema = catalog_.get_table(default_database_id, "docs");
    ASSERT_TRUE(schema.has_value());
    ASSERT_EQ(catalog_.list_embedding_columns(schema->table_id).size(), 2u);
    ASSERT_EQ(catalog_.list_indexes(schema->table_id).size(), 2u);

    exec_ok("DROP TABLE docs");

    // All embedding columns and HNSW indexes must be gone.
    EXPECT_TRUE(catalog_.list_all_embedding_columns().empty());
    EXPECT_FALSE(catalog_.get_index("hnsw_docs_t_vec").has_value());
    EXPECT_FALSE(catalog_.get_index("hnsw_docs_b_vec").has_value());
}

// =============================================================================
// Multiple independent tables with EMBEDDING columns — isolation
// =============================================================================

TEST_F(QA_GDB259, MultipleTablesEmbeddingsAreIsolated) {
    exec_ok("CREATE TABLE t1 ("
            "  id INT, name VARCHAR,"
            "  vec EMBEDDING(128, name, 'builtin/128')"
            ")");
    exec_ok("CREATE TABLE t2 ("
            "  id INT, desc_ VARCHAR,"
            "  vec EMBEDDING(256, desc_, 'builtin/256')"
            ")");

    auto s1 = catalog_.get_table(default_database_id, "t1");
    auto s2 = catalog_.get_table(default_database_id, "t2");
    ASSERT_TRUE(s1.has_value());
    ASSERT_TRUE(s2.has_value());

    auto emb1 = catalog_.list_embedding_columns(s1->table_id);
    auto emb2 = catalog_.list_embedding_columns(s2->table_id);

    ASSERT_EQ(emb1.size(), 1u);
    ASSERT_EQ(emb2.size(), 1u);
    EXPECT_EQ(emb1[0].dimension, 128);
    EXPECT_EQ(emb2[0].dimension, 256);

    // Drop t1 — t2 embeddings should be unaffected.
    exec_ok("DROP TABLE t1");
    EXPECT_TRUE(catalog_.list_embedding_columns(s1->table_id).empty());
    EXPECT_EQ(catalog_.list_embedding_columns(s2->table_id).size(), 1u);

    // Global list should have only t2's embedding.
    auto all = catalog_.list_all_embedding_columns();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].table_id, s2->table_id);
}

// =============================================================================
// SHOW EMBEDDINGS on table with no EMBEDDING columns — returns 0 rows
// =============================================================================

TEST_F(QA_GDB259, ShowEmbeddingsOnPlainTable) {
    exec_ok("CREATE TABLE plain (id INT, name VARCHAR)");

    auto qr = exec_ok("SHOW EMBEDDINGS FROM plain");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// SHOW EMBEDDINGS across multiple tables
// =============================================================================

TEST_F(QA_GDB259, ShowEmbeddingsMultipleTables) {
    exec_ok("CREATE TABLE t1 ("
            "  id INT, name VARCHAR,"
            "  vec EMBEDDING(128, name, 'builtin/128')"
            ")");
    exec_ok("CREATE TABLE t2 ("
            "  id INT, desc_ VARCHAR,"
            "  vec EMBEDDING(256, desc_, 'builtin/256')"
            ")");

    // SHOW EMBEDDINGS FROM <table> should return only that table's embeddings.
    auto qr1 = exec_ok("SHOW EMBEDDINGS FROM t1");
    ASSERT_EQ(qr1.rows.size(), 1u);
    EXPECT_EQ(qr1.rows[0][0].as_string(), "t1");
    EXPECT_EQ(qr1.rows[0][2].as_int32(), 128);

    auto qr2 = exec_ok("SHOW EMBEDDINGS FROM t2");
    ASSERT_EQ(qr2.rows.size(), 1u);
    EXPECT_EQ(qr2.rows[0][0].as_string(), "t2");
    EXPECT_EQ(qr2.rows[0][2].as_int32(), 256);
}

// =============================================================================
// Non-EMBEDDING table unaffected — no indexes created
// =============================================================================

TEST_F(QA_GDB259, PlainTableGetsNoEmbeddingMetadataOrIndexes) {
    exec_ok("CREATE TABLE t ("
            "  id INT,"
            "  name VARCHAR,"
            "  score FLOAT"
            ")");

    auto schema = catalog_.get_table(default_database_id, "t");
    ASSERT_TRUE(schema.has_value());
    EXPECT_TRUE(catalog_.list_embedding_columns(schema->table_id).empty());
    EXPECT_TRUE(catalog_.list_indexes(schema->table_id).empty());
}

// =============================================================================
// Embedding column ordinal correctness — embedding at various positions
// =============================================================================

TEST_F(QA_GDB259, EmbeddingAtLastPosition) {
    exec_ok("CREATE TABLE last_pos ("
            "  a INT, b INT, c INT, d INT,"
            "  vec EMBEDDING(64, a, 'builtin/64')"
            ")");

    auto schema = catalog_.get_table(default_database_id, "last_pos");
    ASSERT_TRUE(schema.has_value());

    auto embs = catalog_.list_embedding_columns(schema->table_id);
    ASSERT_EQ(embs.size(), 1u);
    EXPECT_EQ(embs[0].column_id, 4); // 5th column, ordinal 4.
}

TEST_F(QA_GDB259, EmbeddingAtMiddlePosition) {
    exec_ok("CREATE TABLE mid ("
            "  a INT,"
            "  vec EMBEDDING(64, a, 'builtin/64'),"
            "  b INT,"
            "  c INT"
            ")");

    auto schema = catalog_.get_table(default_database_id, "mid");
    ASSERT_TRUE(schema.has_value());

    auto embs = catalog_.list_embedding_columns(schema->table_id);
    ASSERT_EQ(embs.size(), 1u);
    EXPECT_EQ(embs[0].column_id, 1); // 2nd column, ordinal 1.
}

// =============================================================================
// Embedding HNSW index properties — verify type, columns, non-unique
// =============================================================================

TEST_F(QA_GDB259, HnswIndexProperties) {
    exec_ok("CREATE TABLE props ("
            "  id INT,"
            "  title VARCHAR,"
            "  vec EMBEDDING(384, title, 'builtin/384')"
            ")");

    auto schema = catalog_.get_table(default_database_id, "props");
    ASSERT_TRUE(schema.has_value());

    auto idx = catalog_.get_index("hnsw_props_vec");
    ASSERT_TRUE(idx.has_value()) << idx.error().message;
    EXPECT_EQ(idx->table_id, schema->table_id);
    EXPECT_EQ(idx->index_type, "hnsw");
    EXPECT_EQ(idx->columns, "vec");
    EXPECT_FALSE(idx->is_unique);
}

// =============================================================================
// End-to-end: REEMBED with multiple embedding columns
// =============================================================================

TEST_F(QA_GDB259, ReembedWithMultipleEmbeddingColumns) {
    // Register providers.
    EmbeddingProviderConfig p1;
    p1.name = "builtin/4";
    p1.type = "builtin";
    p1.dimension = 4;
    ASSERT_TRUE(catalog_.register_embedding_provider(p1).has_value());

    EmbeddingProviderConfig p2;
    p2.name = "builtin/8";
    p2.type = "builtin";
    p2.dimension = 8;
    ASSERT_TRUE(catalog_.register_embedding_provider(p2).has_value());

    auto provider_registry = std::make_unique<ProviderRegistry>(catalog_);
    engine_->set_provider_registry(provider_registry.get());

    exec_ok("CREATE TABLE docs ("
            "  id INT,"
            "  title VARCHAR,"
            "  body VARCHAR,"
            "  t_vec EMBEDDING(4, title, 'builtin/4'),"
            "  b_vec EMBEDDING(8, body, 'builtin/8')"
            ")");

    auto schema = catalog_.get_table(default_database_id, "docs");
    ASSERT_TRUE(schema.has_value());

    // Insert a row.
    auto ts = storage_->get_table_storage(schema->table_id);
    ASSERT_TRUE(ts.has_value());
    auto* table_storage = *ts;

    std::vector<Value> row;
    row.push_back(Value(static_cast<int32_t>(1)));
    row.push_back(Value(std::string("hello")));
    row.push_back(Value(std::string("world")));
    row.push_back(Value(Embedding{0.0F, 0.0F, 0.0F, 0.0F}));
    row.push_back(Value(Embedding{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F}));

    auto serialized = TupleSerializer::serialize(row, table_storage->storage_schema);
    ASSERT_TRUE(serialized.has_value()) << serialized.error().message;

    auto rid = table_storage->heap->insert_tuple(*serialized);
    ASSERT_TRUE(rid.has_value()) << rid.error().message;

    // REEMBED should find both embedding columns.
    auto qr = exec_ok("REEMBED TABLE docs");
    EXPECT_EQ(qr.affected_rows, 1);
}

// =============================================================================
// SHOW EMBEDDINGS column data matches exactly what was registered
// =============================================================================

TEST_F(QA_GDB259, ShowEmbeddingsDataAccuracy) {
    exec_ok("CREATE TABLE articles ("
            "  id INT,"
            "  title VARCHAR,"
            "  body VARCHAR,"
            "  t_vec EMBEDDING(128, title, 'builtin/128'),"
            "  b_vec EMBEDDING(256, body, 'builtin/256')"
            ")");

    auto qr = exec_ok("SHOW EMBEDDINGS FROM articles");
    ASSERT_EQ(qr.rows.size(), 2u);

    // Row 0: t_vec.
    EXPECT_EQ(qr.rows[0][0].as_string(), "articles");
    EXPECT_EQ(qr.rows[0][1].as_string(), "t_vec");
    EXPECT_EQ(qr.rows[0][2].as_int32(), 128);
    EXPECT_EQ(qr.rows[0][3].as_string(), "title");
    EXPECT_EQ(qr.rows[0][4].as_string(), "builtin/128");

    // Row 1: b_vec.
    EXPECT_EQ(qr.rows[1][0].as_string(), "articles");
    EXPECT_EQ(qr.rows[1][1].as_string(), "b_vec");
    EXPECT_EQ(qr.rows[1][2].as_int32(), 256);
    EXPECT_EQ(qr.rows[1][3].as_string(), "body");
    EXPECT_EQ(qr.rows[1][4].as_string(), "builtin/256");
}

// =============================================================================
// IF NOT EXISTS: second CREATE does not duplicate embedding metadata
// =============================================================================

TEST_F(QA_GDB259, IfNotExistsNoDuplicateMetadata) {
    exec_ok("CREATE TABLE articles ("
            "  id INT,"
            "  title VARCHAR,"
            "  vec EMBEDDING(384, title, 'builtin/384')"
            ")");

    exec_ok("CREATE TABLE IF NOT EXISTS articles ("
            "  id INT,"
            "  title VARCHAR,"
            "  vec EMBEDDING(384, title, 'builtin/384')"
            ")");

    exec_ok("CREATE TABLE IF NOT EXISTS articles ("
            "  id INT,"
            "  title VARCHAR,"
            "  vec EMBEDDING(384, title, 'builtin/384')"
            ")");

    auto schema = catalog_.get_table(default_database_id, "articles");
    ASSERT_TRUE(schema.has_value());

    // Still exactly 1 embedding and 1 HNSW index — no duplicates.
    auto embs = catalog_.list_embedding_columns(schema->table_id);
    EXPECT_EQ(embs.size(), 1u);
    auto indexes = catalog_.list_indexes(schema->table_id);
    EXPECT_EQ(indexes.size(), 1u);
}

// =============================================================================
// Table with ONLY embedding columns (no plain columns)
// =============================================================================

TEST_F(QA_GDB259, EmbeddingColumnWithManyPlainColumns) {
    // Table with many plain columns and one EMBEDDING at the end.
    exec_ok("CREATE TABLE wide ("
            "  a INT, b INT, c INT, d INT, e INT,"
            "  f VARCHAR, g VARCHAR, h VARCHAR,"
            "  vec EMBEDDING(64, f, 'builtin/64')"
            ")");

    auto schema = catalog_.get_table(default_database_id, "wide");
    ASSERT_TRUE(schema.has_value()) << schema.error().message;

    auto embs = catalog_.list_embedding_columns(schema->table_id);
    ASSERT_EQ(embs.size(), 1u);
    EXPECT_EQ(embs[0].column_id, 8); // 9th column, ordinal 8.
    EXPECT_EQ(embs[0].source_expr, "f");

    auto indexes = catalog_.list_indexes(schema->table_id);
    ASSERT_EQ(indexes.size(), 1u);
    EXPECT_EQ(indexes[0].name, "hnsw_wide_vec");
}

// =============================================================================
// EmbeddingColumnManager direct API: negative dimension
// =============================================================================

TEST_F(QA_GDB259, RegisterNegativeDimensionFails) {
    // Create a plain table in catalog.
    TableSchema ts;
    ts.name = "neg_dim";
    ts.columns = {{0, "id", TypeId::INT32, false, ""}, {1, "vec", TypeId::EMBEDDING, true, ""}};
    auto tid = catalog_.create_table(default_database_id, std::move(ts));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.table_id = *tid;
    def.column_id = 1;
    def.dimension = -1;
    def.source_expr = "id";
    def.provider = "test";

    EmbeddingColumnManager mgr(catalog_);
    auto result = mgr.register_table_embeddings(*tid, {def});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// Embedding column with PRIMARY KEY — verify no interference
// =============================================================================

TEST_F(QA_GDB259, EmbeddingWithPrimaryKey) {
    exec_ok("CREATE TABLE pk_emb ("
            "  id INT,"
            "  title VARCHAR,"
            "  vec EMBEDDING(128, title, 'builtin/128'),"
            "  PRIMARY KEY (id)"
            ")");

    auto schema = catalog_.get_table(default_database_id, "pk_emb");
    ASSERT_TRUE(schema.has_value());
    EXPECT_EQ(schema->pk_columns, "id");

    // EMBEDDING column coexists with PK without interference.
    auto embs = catalog_.list_embedding_columns(schema->table_id);
    ASSERT_EQ(embs.size(), 1u);
    EXPECT_EQ(embs[0].dimension, 128);
}

// =============================================================================
// SHOW EMBEDDINGS on non-existent table — error
// =============================================================================

TEST_F(QA_GDB259, ShowEmbeddingsNonexistentTable) {
    auto result = engine_->execute("SHOW EMBEDDINGS FROM ghost");
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// REEMBED on table with no embedding columns — error
// =============================================================================

TEST_F(QA_GDB259, ReembedOnPlainTableFails) {
    exec_ok("CREATE TABLE plain (id INT, name VARCHAR)");

    auto result = engine_->execute("REEMBED TABLE plain");
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// Consecutive EMBEDDING columns
// =============================================================================

TEST_F(QA_GDB259, ConsecutiveEmbeddingColumns) {
    exec_ok("CREATE TABLE triple ("
            "  id INT,"
            "  name VARCHAR,"
            "  a EMBEDDING(64, name, 'builtin/64'),"
            "  b EMBEDDING(128, name, 'builtin/128'),"
            "  c EMBEDDING(256, name, 'builtin/256')"
            ")");

    auto schema = catalog_.get_table(default_database_id, "triple");
    ASSERT_TRUE(schema.has_value());

    auto embs = catalog_.list_embedding_columns(schema->table_id);
    ASSERT_EQ(embs.size(), 3u);
    EXPECT_EQ(embs[0].column_id, 2);
    EXPECT_EQ(embs[0].dimension, 64);
    EXPECT_EQ(embs[1].column_id, 3);
    EXPECT_EQ(embs[1].dimension, 128);
    EXPECT_EQ(embs[2].column_id, 4);
    EXPECT_EQ(embs[2].dimension, 256);

    auto indexes = catalog_.list_indexes(schema->table_id);
    ASSERT_EQ(indexes.size(), 3u);
    EXPECT_EQ(indexes[0].name, "hnsw_triple_a");
    EXPECT_EQ(indexes[1].name, "hnsw_triple_b");
    EXPECT_EQ(indexes[2].name, "hnsw_triple_c");
}

// =============================================================================
// Drop table with embeddings, verify global embedding list is correct
// =============================================================================

TEST_F(QA_GDB259, DropTableCleansGlobalEmbeddingList) {
    exec_ok("CREATE TABLE keep ("
            "  id INT, name VARCHAR,"
            "  vec EMBEDDING(64, name, 'builtin/64')"
            ")");
    exec_ok("CREATE TABLE remove ("
            "  id INT, desc_ VARCHAR,"
            "  vec EMBEDDING(128, desc_, 'builtin/128')"
            ")");

    auto all_before = catalog_.list_all_embedding_columns();
    ASSERT_EQ(all_before.size(), 2u);

    exec_ok("DROP TABLE remove");

    auto all_after = catalog_.list_all_embedding_columns();
    ASSERT_EQ(all_after.size(), 1u);

    auto keep_schema = catalog_.get_table(default_database_id, "keep");
    ASSERT_TRUE(keep_schema.has_value());
    EXPECT_EQ(all_after[0].table_id, keep_schema->table_id);
}

// =============================================================================
// SHOW EMBEDDINGS after DROP TABLE — returns 0 rows for dropped table
// =============================================================================

TEST_F(QA_GDB259, ShowEmbeddingsAfterDropReturnsNothing) {
    exec_ok("CREATE TABLE ephemeral ("
            "  id INT, name VARCHAR,"
            "  vec EMBEDDING(64, name, 'builtin/64')"
            ")");

    auto qr1 = exec_ok("SHOW EMBEDDINGS FROM ephemeral");
    EXPECT_EQ(qr1.rows.size(), 1u);

    exec_ok("DROP TABLE ephemeral");

    // Table no longer exists — SHOW should fail.
    auto result = engine_->execute("SHOW EMBEDDINGS FROM ephemeral");
    EXPECT_FALSE(result.has_value());
}
