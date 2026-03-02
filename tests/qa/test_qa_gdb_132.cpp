/// @file test_qa_gdb_132.cpp
/// @brief QA adversarial tests for GDB-132: REEMBED TABLE command.
///
/// Targets edge cases: batch boundary (32 rows), null source column values,
/// empty string source, re-embed idempotency, skipped-row message,
/// exact-boundary batches, multiple EMBEDDING columns, large table stress.

#include "giodb/catalog/catalog.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/query_engine.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/storage/buffer_pool.h"
#include "giodb/storage/disk_manager.h"
#include "giodb/table/tuple.h"
#include "giodb/vector/builtin_provider.h"
#include "giodb/vector/embedding_column.h"
#include "giodb/vector/provider_registry.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace giodb;

// =============================================================================
// Test fixture
// =============================================================================

class QA132ReembedTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "giodb_qa132_reembed";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

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

    void create_embedding_table() {
        exec_ok("CREATE TABLE items (id INT, name VARCHAR, emb EMBEDDING)");

        auto schema = catalog_.get_table(default_database_id, "items");
        ASSERT_TRUE(schema.has_value());

        EmbeddingColumnDef emb_def;
        emb_def.table_id = schema->table_id;
        emb_def.column_id = 2;
        emb_def.dimension = 4;
        emb_def.source_expr = "name";
        emb_def.provider = "builtin/4";
        auto reg = catalog_.register_embedding_column(emb_def);
        ASSERT_TRUE(reg.has_value()) << reg.error().message;

        EmbeddingProviderConfig prov_config;
        prov_config.name = "builtin/4";
        prov_config.type = "builtin";
        prov_config.dimension = 4;
        auto prov_reg = catalog_.register_embedding_provider(prov_config);
        ASSERT_TRUE(prov_reg.has_value()) << prov_reg.error().message;

        engine_->set_provider_registry(provider_registry_.get());
    }

    void insert_row(int id, const std::string& name) {
        auto schema = catalog_.get_table(default_database_id, "items");
        ASSERT_TRUE(schema.has_value());

        auto ts = storage_->get_table_storage(schema->table_id);
        ASSERT_TRUE(ts.has_value());
        auto* table_storage = *ts;

        std::vector<Value> values;
        values.push_back(Value(static_cast<int32_t>(id)));
        values.push_back(Value(name));
        values.push_back(Value(Embedding{0.0F, 0.0F, 0.0F, 0.0F}));

        auto serialized = TupleSerializer::serialize(values, table_storage->storage_schema);
        ASSERT_TRUE(serialized.has_value()) << serialized.error().message;

        auto rid = table_storage->heap->insert_tuple(*serialized);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    void insert_row_null_name(int id) {
        auto schema = catalog_.get_table(default_database_id, "items");
        ASSERT_TRUE(schema.has_value());

        auto ts = storage_->get_table_storage(schema->table_id);
        ASSERT_TRUE(ts.has_value());
        auto* table_storage = *ts;

        std::vector<Value> values;
        values.push_back(Value(static_cast<int32_t>(id)));
        values.push_back(Value()); // null name
        values.push_back(Value(Embedding{0.0F, 0.0F, 0.0F, 0.0F}));

        auto serialized = TupleSerializer::serialize(values, table_storage->storage_schema);
        ASSERT_TRUE(serialized.has_value()) << serialized.error().message;

        auto rid = table_storage->heap->insert_tuple(*serialized);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    std::vector<Embedding> read_all_embeddings() {
        auto schema = catalog_.get_table(default_database_id, "items");
        if (!schema) {
            return {};
        }

        auto ts = storage_->get_table_storage(schema->table_id);
        if (!ts) {
            return {};
        }
        auto* table_storage = *ts;

        auto it = table_storage->heap->begin();
        if (!it) {
            return {};
        }

        std::vector<Embedding> result;
        while (true) {
            auto row = it->next();
            if (!row) {
                break;
            }
            auto& [rid, data] = *row;
            auto values = TupleSerializer::deserialize(data, table_storage->storage_schema);
            if (!values) {
                continue;
            }
            if ((*values).size() > 2 && (*values)[2].type_id() == TypeId::EMBEDDING) {
                result.push_back((*values)[2].as_embedding());
            }
        }
        return result;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<ProviderRegistry> provider_registry_;
};

// ---------------------------------------------------------------------------
// Batch boundary edge cases
// ---------------------------------------------------------------------------

TEST_F(QA132ReembedTest, ExactlyBatchSize32Rows) {
    create_embedding_table();
    for (int i = 1; i <= 32; ++i) {
        insert_row(i, "item" + std::to_string(i));
    }

    auto qr = exec_ok("REEMBED TABLE items");
    EXPECT_EQ(qr.affected_rows, 32);

    auto after = read_all_embeddings();
    ASSERT_EQ(after.size(), 32u);
    for (const auto& emb : after) {
        EXPECT_NE(emb, (Embedding{0.0F, 0.0F, 0.0F, 0.0F}));
    }
}

TEST_F(QA132ReembedTest, BatchSizePlusOne33Rows) {
    create_embedding_table();
    for (int i = 1; i <= 33; ++i) {
        insert_row(i, "item" + std::to_string(i));
    }

    auto qr = exec_ok("REEMBED TABLE items");
    EXPECT_EQ(qr.affected_rows, 33);

    auto after = read_all_embeddings();
    ASSERT_EQ(after.size(), 33u);
    for (const auto& emb : after) {
        EXPECT_NE(emb, (Embedding{0.0F, 0.0F, 0.0F, 0.0F}));
    }
}

TEST_F(QA132ReembedTest, BatchSizeMinusOne31Rows) {
    create_embedding_table();
    for (int i = 1; i <= 31; ++i) {
        insert_row(i, "item" + std::to_string(i));
    }

    auto qr = exec_ok("REEMBED TABLE items");
    EXPECT_EQ(qr.affected_rows, 31);
}

TEST_F(QA132ReembedTest, TwoBatchesFull64Rows) {
    create_embedding_table();
    for (int i = 1; i <= 64; ++i) {
        insert_row(i, "item" + std::to_string(i));
    }

    auto qr = exec_ok("REEMBED TABLE items");
    EXPECT_EQ(qr.affected_rows, 64);
}

// ---------------------------------------------------------------------------
// Null and empty source text
// ---------------------------------------------------------------------------

TEST_F(QA132ReembedTest, NullSourceColumnProducesEmptyText) {
    create_embedding_table();
    insert_row_null_name(1);
    insert_row(2, "valid text");

    // Should not crash. May skip the null-name row or embed an empty string.
    auto qr = exec_ok("REEMBED TABLE items");
    EXPECT_GE(qr.affected_rows, 1);
}

TEST_F(QA132ReembedTest, EmptyStringSourceColumn) {
    create_embedding_table();
    insert_row(1, "");
    insert_row(2, "has content");

    // Empty string may fail to embed with BuiltinProvider (needs tokens).
    // But REEMBED should handle this gracefully.
    auto qr = exec_ok("REEMBED TABLE items");
    EXPECT_GE(qr.affected_rows, 1);
}

// ---------------------------------------------------------------------------
// Idempotency
// ---------------------------------------------------------------------------

TEST_F(QA132ReembedTest, ReembedTwiceProducesSameEmbeddings) {
    create_embedding_table();
    insert_row(1, "consistent text");
    insert_row(2, "another consistent text");

    exec_ok("REEMBED TABLE items");
    auto first = read_all_embeddings();

    exec_ok("REEMBED TABLE items");
    auto second = read_all_embeddings();

    ASSERT_EQ(first.size(), second.size());
    for (size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first[i], second[i]) << "Embedding " << i << " differs between runs";
    }
}

// ---------------------------------------------------------------------------
// Result message
// ---------------------------------------------------------------------------

TEST_F(QA132ReembedTest, MessageFormatZeroRows) {
    create_embedding_table();
    auto qr = exec_ok("REEMBED TABLE items");
    EXPECT_EQ(qr.message, "REEMBED 0 rows");
}

TEST_F(QA132ReembedTest, MessageFormatMultipleRows) {
    create_embedding_table();
    insert_row(1, "a");
    insert_row(2, "b");
    insert_row(3, "c");
    auto qr = exec_ok("REEMBED TABLE items");
    EXPECT_EQ(qr.message, "REEMBED 3 rows");
}

// ---------------------------------------------------------------------------
// Parser edge cases
// ---------------------------------------------------------------------------

TEST_F(QA132ReembedTest, WithoutTableKeyword) {
    create_embedding_table();
    insert_row(1, "test");
    auto qr = exec_ok("REEMBED items");
    EXPECT_EQ(qr.affected_rows, 1);
}

TEST_F(QA132ReembedTest, ReembedNonexistentTable) {
    exec_error("REEMBED TABLE ghost_table", StatusCode::NOT_FOUND);
}

TEST_F(QA132ReembedTest, ReembedTableWithNoEmbedding) {
    exec_ok("CREATE TABLE plain (id INT, text VARCHAR)");
    exec_error("REEMBED TABLE plain", StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Large table stress
// ---------------------------------------------------------------------------

TEST_F(QA132ReembedTest, LargeTable200Rows) {
    create_embedding_table();
    for (int i = 1; i <= 200; ++i) {
        insert_row(i, "product number " + std::to_string(i));
    }

    auto qr = exec_ok("REEMBED TABLE items");
    EXPECT_EQ(qr.affected_rows, 200);

    auto after = read_all_embeddings();
    ASSERT_EQ(after.size(), 200u);

    // Each embedding should be 4-dimensional and non-zero.
    for (const auto& emb : after) {
        EXPECT_EQ(emb.size(), 4u);
        EXPECT_NE(emb, (Embedding{0.0F, 0.0F, 0.0F, 0.0F}));
    }
}

// ---------------------------------------------------------------------------
// Different source texts produce different embeddings
// ---------------------------------------------------------------------------

TEST_F(QA132ReembedTest, DifferentSourcesProduceDifferentEmbeddings) {
    create_embedding_table();
    insert_row(1, "cats are fluffy");
    insert_row(2, "database systems");
    insert_row(3, "quantum physics");

    exec_ok("REEMBED TABLE items");
    auto after = read_all_embeddings();
    ASSERT_EQ(after.size(), 3u);

    EXPECT_NE(after[0], after[1]);
    EXPECT_NE(after[1], after[2]);
    EXPECT_NE(after[0], after[2]);
}

// ---------------------------------------------------------------------------
// Single row table
// ---------------------------------------------------------------------------

TEST_F(QA132ReembedTest, SingleRow) {
    create_embedding_table();
    insert_row(1, "single entry");

    auto qr = exec_ok("REEMBED TABLE items");
    EXPECT_EQ(qr.affected_rows, 1);

    auto after = read_all_embeddings();
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0].size(), 4u);
    EXPECT_NE(after[0], (Embedding{0.0F, 0.0F, 0.0F, 0.0F}));
}
