/// @file test_qa_gdb_258.cpp
/// @brief Adversarial QA tests for GDB-258: ProviderRegistry never instantiated.
///
/// Tests edge cases, boundary values, error paths, and stress scenarios
/// for the ProviderRegistry wiring fix in main.cpp/query_engine.cpp.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"
#include "sixseven/vector/embedding_column.h"
#include "sixseven/vector/provider_registry.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_qa_helpers.h"

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class QA_GDB258 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_qa_gdb258";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
        provider_registry_ = std::make_unique<ProviderRegistry>(catalog_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        provider_registry_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        if (!result.has_value()) {
            ADD_FAILURE() << "Expected success but got error: " << result.error().message;
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

    void wire_registry() { engine_->set_provider_registry(provider_registry_.get()); }

    void create_embedding_table() {
        exec_ok("CREATE TABLE articles (id INT, title VARCHAR, title_vec EMBEDDING)");

        auto schema = catalog_.get_table(default_database_id, "articles");
        ASSERT_TRUE(schema.has_value());

        EmbeddingColumnDef emb_def;
        emb_def.table_id = schema->table_id;
        emb_def.column_id = 2;
        emb_def.dimension = 4;
        emb_def.source_expr = "title";
        emb_def.provider = "builtin/4";
        auto reg = catalog_.register_embedding_column(emb_def);
        ASSERT_TRUE(reg.has_value()) << reg.error().message;

        EmbeddingProviderConfig prov_config;
        prov_config.name = "builtin/4";
        prov_config.type = "builtin";
        prov_config.dimension = 4;
        auto prov_reg = catalog_.register_embedding_provider(prov_config);
        ASSERT_TRUE(prov_reg.has_value()) << prov_reg.error().message;
    }

    void insert_row(int32_t id, const std::string& title, const Embedding& emb) {
        auto schema = catalog_.get_table(default_database_id, "articles");
        ASSERT_TRUE(schema.has_value());

        auto ts = storage_->get_table_storage(schema->table_id);
        ASSERT_TRUE(ts.has_value());
        auto* table_storage = *ts;

        std::vector<Value> values;
        values.push_back(Value(id));
        values.push_back(Value(title));
        values.push_back(Value(emb));

        auto serialized = TupleSerializer::serialize(values, table_storage->storage_schema);
        ASSERT_TRUE(serialized.has_value()) << serialized.error().message;

        auto rid = table_storage->heap->insert_tuple(*serialized);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<ProviderRegistry> provider_registry_;
};

// =============================================================================
// Boundary: NEAREST on empty table
// =============================================================================

TEST_F(QA_GDB258, NearestTextQueryOnEmptyTable) {
    wire_registry();
    create_embedding_table();

    // No rows inserted — NEAREST should return empty, not crash.
    auto qr = exec_ok("SELECT * FROM articles WHERE NEAREST(title_vec, 5) TO 'hello world'");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(QA_GDB258, NearestVectorQueryOnEmptyTable) {
    wire_registry();
    create_embedding_table();

    auto qr = exec_ok("SELECT * FROM articles WHERE NEAREST(title_vec, 5) TO [1.0, 0.0, 0.0, 0.0]");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// Boundary: k = 0 and k > row count
// =============================================================================

TEST_F(QA_GDB258, NearestWithKZero) {
    wire_registry();
    create_embedding_table();
    insert_row(1, "test", {1.0F, 0.0F, 0.0F, 0.0F});

    // k=0 should return 0 rows.
    auto qr = exec_ok("SELECT * FROM articles WHERE NEAREST(title_vec, 0) TO 'test'");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(QA_GDB258, NearestWithKLargerThanRowCount) {
    wire_registry();
    create_embedding_table();
    insert_row(1, "a", {1.0F, 0.0F, 0.0F, 0.0F});
    insert_row(2, "b", {0.0F, 1.0F, 0.0F, 0.0F});

    // k=100 but only 2 rows — should return 2, not error.
    auto qr = exec_ok("SELECT * FROM articles WHERE NEAREST(title_vec, 100) TO 'test'");
    EXPECT_EQ(qr.rows.size(), 2u);
}

// =============================================================================
// Null handling: empty text target
// =============================================================================

TEST_F(QA_GDB258, NearestWithEmptyStringTarget) {
    wire_registry();
    create_embedding_table();
    insert_row(1, "hello", {1.0F, 0.0F, 0.0F, 0.0F});

    // Empty string target — the provider correctly rejects empty text.
    auto result = engine_->execute("SELECT * FROM articles WHERE NEAREST(title_vec, 1) TO ''");
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// Error paths: NEAREST on non-embedding column
// =============================================================================

TEST_F(QA_GDB258, NearestOnNonEmbeddingColumn) {
    wire_registry();
    exec_ok("CREATE TABLE simple (id INT, name VARCHAR)");

    // NEAREST on a VARCHAR column — should fail with a clear error.
    auto result = engine_->execute("SELECT * FROM simple WHERE NEAREST(name, 5) TO 'test'");
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// Error paths: NEAREST on non-existent table
// =============================================================================

TEST_F(QA_GDB258, NearestOnNonExistentTable) {
    wire_registry();

    // GDB-739: assert specific NOT_FOUND code + message names the missing table.
    auto result = engine_->execute("SELECT * FROM ghost WHERE NEAREST(vec, 5) TO 'test'");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
    EXPECT_NE(result.error().message.find("ghost"), std::string::npos)
        << "Error message should name the missing table; got: " << result.error().message;
}

// =============================================================================
// Error paths: REEMBED on table with no embedding columns
// =============================================================================

TEST_F(QA_GDB258, ReembedOnTableWithNoEmbeddingColumns) {
    wire_registry();
    exec_ok("CREATE TABLE plain (id INT, name VARCHAR)");

    exec_error("REEMBED TABLE plain", StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// Error paths: REEMBED on non-existent table
// =============================================================================

TEST_F(QA_GDB258, ReembedOnNonExistentTable) {
    wire_registry();

    // GDB-739: assert specific NOT_FOUND code + message names the missing table.
    auto result = engine_->execute("REEMBED TABLE does_not_exist");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
    EXPECT_NE(result.error().message.find("does_not_exist"), std::string::npos)
        << "Error message should name the missing table; got: " << result.error().message;
}

// =============================================================================
// REEMBED on empty table (boundary)
// =============================================================================

TEST_F(QA_GDB258, ReembedOnEmptyTable) {
    wire_registry();
    create_embedding_table();

    // No rows — REEMBED should succeed with 0 affected rows, not crash.
    auto qr = exec_ok("REEMBED TABLE articles");
    EXPECT_EQ(qr.affected_rows, 0);
}

// =============================================================================
// Sequence: set_provider_registry called multiple times
// =============================================================================

TEST_F(QA_GDB258, SetProviderRegistryMultipleTimes) {
    // Wire, unwire (nullptr), then re-wire. Should still work.
    wire_registry();
    create_embedding_table();
    insert_row(1, "test", {1.0F, 0.0F, 0.0F, 0.0F});

    auto qr = exec_ok("SELECT * FROM articles WHERE NEAREST(title_vec, 1) TO 'test'");
    EXPECT_EQ(qr.rows.size(), 1u);

    // Unwire
    engine_->set_provider_registry(nullptr);
    exec_error("SELECT * FROM articles WHERE NEAREST(title_vec, 1) TO 'test'",
               StatusCode::NOT_IMPLEMENTED);

    // Re-wire
    wire_registry();
    auto qr2 = exec_ok("SELECT * FROM articles WHERE NEAREST(title_vec, 1) TO 'test'");
    EXPECT_EQ(qr2.rows.size(), 1u);
}

// =============================================================================
// EXPLAIN NEAREST text target without registry (error path)
// =============================================================================

TEST_F(QA_GDB258, ExplainNearestTextFailsWithoutRegistry) {
    // The EXPLAIN path also uses a Planner. Without registry, text target should fail.
    create_embedding_table();
    insert_row(1, "test", {1.0F, 0.0F, 0.0F, 0.0F});

    exec_error("EXPLAIN SELECT * FROM articles WHERE NEAREST(title_vec, 1) TO 'test'",
               StatusCode::NOT_IMPLEMENTED);
}

// =============================================================================
// EXPLAIN NEAREST vector target should work without registry
// =============================================================================

TEST_F(QA_GDB258, ExplainNearestVectorWorksWithoutRegistry) {
    create_embedding_table();
    insert_row(1, "test", {1.0F, 0.0F, 0.0F, 0.0F});

    auto qr = exec_ok(
        "EXPLAIN SELECT * FROM articles WHERE NEAREST(title_vec, 1) TO [1.0, 0.0, 0.0, 0.0]");
    EXPECT_FALSE(qr.rows.empty());
}

// =============================================================================
// Stress: NEAREST with many rows
// =============================================================================

TEST_F(QA_GDB258, NearestTextQueryWithManyRows) {
    wire_registry();
    create_embedding_table();

    // Insert 100 rows with different embeddings.
    for (int32_t i = 0; i < 100; ++i) {
        Embedding emb = {static_cast<float>(i % 10),
                         static_cast<float>(i / 10),
                         static_cast<float>(i % 3),
                         static_cast<float>(i % 7)};
        insert_row(i, "row_" + std::to_string(i), emb);
    }

    auto qr = exec_ok("SELECT * FROM articles WHERE NEAREST(title_vec, 10) TO 'test query'");
    EXPECT_EQ(qr.rows.size(), 10u);
}

// =============================================================================
// Stress: REEMBED with many rows
// =============================================================================

TEST_F(QA_GDB258, ReembedWithManyRows) {
    wire_registry();
    create_embedding_table();

    // Insert 50 rows with zero embeddings — REEMBED should regenerate all.
    for (int32_t i = 0; i < 50; ++i) {
        insert_row(i, "article_" + std::to_string(i), {0.0F, 0.0F, 0.0F, 0.0F});
    }

    auto qr = exec_ok("REEMBED TABLE articles");
    EXPECT_EQ(qr.affected_rows, 50);
}

// =============================================================================
// Null handling: rows with NULL embedding values in NEAREST
// =============================================================================

TEST_F(QA_GDB258, NearestSkipsRowsWithNullEmbedding) {
    wire_registry();
    create_embedding_table();

    // Insert one row with a valid embedding, one with NULL embedding.
    insert_row(1, "valid", {1.0F, 0.0F, 0.0F, 0.0F});

    auto schema = catalog_.get_table(default_database_id, "articles");
    ASSERT_TRUE(schema.has_value());
    auto ts = storage_->get_table_storage(schema->table_id);
    ASSERT_TRUE(ts.has_value());
    auto* table_storage = *ts;

    std::vector<Value> null_values;
    null_values.push_back(Value(int32_t{2}));
    null_values.push_back(Value(std::string("null_emb")));
    null_values.emplace_back(); // NULL embedding

    auto serialized = TupleSerializer::serialize(null_values, table_storage->storage_schema);
    ASSERT_TRUE(serialized.has_value()) << serialized.error().message;
    auto rid = table_storage->heap->insert_tuple(*serialized);
    ASSERT_TRUE(rid.has_value()) << rid.error().message;

    // NEAREST k=5 on 2 rows (one NULL). The NULL-embedding row must be
    // skipped, not returned or scored as garbage: with k=5 there is room for
    // both rows, so exactly the one valid row may come back, and it must be
    // id 1 (GDB-781).
    auto qr = exec_ok("SELECT * FROM articles WHERE NEAREST(title_vec, 5) TO [1.0, 0.0, 0.0, 0.0]");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
}

// =============================================================================
// REEMBED with rows where source text column is NULL
// =============================================================================

TEST_F(QA_GDB258, ReembedHandlesNullSourceText) {
    wire_registry();
    create_embedding_table();

    // Insert row with valid title.
    insert_row(1, "valid title", {0.0F, 0.0F, 0.0F, 0.0F});

    // Insert row with NULL title.
    auto schema = catalog_.get_table(default_database_id, "articles");
    ASSERT_TRUE(schema.has_value());
    auto ts = storage_->get_table_storage(schema->table_id);
    ASSERT_TRUE(ts.has_value());
    auto* table_storage = *ts;

    std::vector<Value> null_title_values;
    null_title_values.push_back(Value(int32_t{2}));
    null_title_values.emplace_back(); // NULL title
    null_title_values.push_back(Value(Embedding{0.0F, 0.0F, 0.0F, 0.0F}));

    auto serialized = TupleSerializer::serialize(null_title_values, table_storage->storage_schema);
    ASSERT_TRUE(serialized.has_value()) << serialized.error().message;
    auto rid = table_storage->heap->insert_tuple(*serialized);
    ASSERT_TRUE(rid.has_value()) << rid.error().message;

    // REEMBED should handle the NULL source text gracefully.
    auto qr = exec_ok("REEMBED TABLE articles");
    EXPECT_EQ(qr.affected_rows, 2);
}

// =============================================================================
// NEAREST with k=1 returns exactly one result
// =============================================================================

TEST_F(QA_GDB258, NearestK1ReturnsExactlyOneResult) {
    wire_registry();
    create_embedding_table();
    insert_row(1, "first", {1.0F, 0.0F, 0.0F, 0.0F});
    insert_row(2, "second", {0.0F, 1.0F, 0.0F, 0.0F});
    insert_row(3, "third", {0.0F, 0.0F, 1.0F, 0.0F});

    auto qr = exec_ok("SELECT * FROM articles WHERE NEAREST(title_vec, 1) TO 'find nearest'");
    EXPECT_EQ(qr.rows.size(), 1u);
}

// =============================================================================
// EXPLAIN ANALYZE NEAREST text with registry (both paths covered)
// =============================================================================

TEST_F(QA_GDB258, ExplainAnalyzeNearestTextWithRegistry) {
    wire_registry();
    create_embedding_table();
    insert_row(1, "test", {1.0F, 0.0F, 0.0F, 0.0F});

    auto qr = exec_ok(
        "EXPLAIN ANALYZE SELECT * FROM articles WHERE NEAREST(title_vec, 1) TO 'analyze text'");
    EXPECT_FALSE(qr.rows.empty());
}
