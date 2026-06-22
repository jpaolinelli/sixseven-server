#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/index_manager.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"
#include "sixseven/vector/builtin_provider.h"
#include "sixseven/vector/embedding_column.h"
#include "sixseven/vector/hnsw_index.h"
#include "sixseven/vector/provider_registry.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class ReembedTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_reembed";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
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

    /// Helper: execute SQL, assert success, return the result.
    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    /// Helper: execute SQL, assert failure with the expected code.
    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, expected);
    }

    /// Helper: create a table with an EMBEDDING column and register the
    /// embedding metadata in the catalog.
    void create_embedding_table() {
        exec_ok("CREATE TABLE products (id INT, name VARCHAR, emb EMBEDDING)");

        auto schema = catalog_.get_table(default_database_id, "products");
        ASSERT_TRUE(schema.has_value());

        // Register the EMBEDDING column metadata.
        EmbeddingColumnDef emb_def;
        emb_def.table_id = schema->table_id;
        emb_def.column_id = 2; // ordinal of 'emb' column
        emb_def.dimension = 4;
        emb_def.source_expr = "name";
        emb_def.provider = "builtin/4";
        auto reg = catalog_.register_embedding_column(emb_def);
        ASSERT_TRUE(reg.has_value()) << reg.error().message;

        // Register the builtin provider.
        EmbeddingProviderConfig prov_config;
        prov_config.name = "builtin/4";
        prov_config.type = "builtin";
        prov_config.dimension = 4;
        auto prov_reg = catalog_.register_embedding_provider(prov_config);
        ASSERT_TRUE(prov_reg.has_value()) << prov_reg.error().message;

        // Wire up the provider registry to the engine.
        engine_->set_provider_registry(provider_registry_.get());
    }

    /// Helper: insert a row with a dummy embedding.
    void insert_row(int id, const std::string& name) {
        auto schema = catalog_.get_table(default_database_id, "products");
        ASSERT_TRUE(schema.has_value());

        auto ts = storage_->get_table_storage(schema->table_id);
        ASSERT_TRUE(ts.has_value());
        auto* table_storage = *ts;

        // Build values: id, name, dummy embedding.
        std::vector<Value> values;
        values.push_back(Value(static_cast<int32_t>(id)));
        values.push_back(Value(name));
        // Dummy embedding (all zeros).
        values.push_back(Value(Embedding{0.0F, 0.0F, 0.0F, 0.0F}));

        auto serialized = TupleSerializer::serialize(values, table_storage->storage_schema);
        ASSERT_TRUE(serialized.has_value()) << serialized.error().message;

        auto rid = table_storage->heap->insert_tuple(*serialized);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    /// Helper: read all rows and return their embedding values.
    std::vector<Embedding> read_all_embeddings() {
        auto schema = catalog_.get_table(default_database_id, "products");
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
            // Column 2 is the embedding.
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

// =============================================================================
// Error handling tests
// =============================================================================

TEST_F(ReembedTest, NonExistentTableReturnsError) {
    exec_error("REEMBED TABLE nonexistent", StatusCode::NOT_FOUND);
}

TEST_F(ReembedTest, TableWithoutEmbeddingColumnsReturnsError) {
    exec_ok("CREATE TABLE plain (id INT, name VARCHAR)");
    exec_error("REEMBED TABLE plain", StatusCode::INVALID_ARGUMENT);
}

TEST_F(ReembedTest, MissingProviderRegistryReturnsError) {
    // Create embedding table but DON'T set the provider registry.
    exec_ok("CREATE TABLE products (id INT, name VARCHAR, emb EMBEDDING)");

    auto schema = catalog_.get_table(default_database_id, "products");
    ASSERT_TRUE(schema.has_value());

    EmbeddingColumnDef emb_def;
    emb_def.table_id = schema->table_id;
    emb_def.column_id = 2;
    emb_def.dimension = 4;
    emb_def.source_expr = "name";
    emb_def.provider = "builtin/4";
    auto reg = catalog_.register_embedding_column(emb_def);
    ASSERT_TRUE(reg.has_value());

    // No provider registry set — should fail.
    exec_error("REEMBED TABLE products", StatusCode::INTERNAL_ERROR);
}

// =============================================================================
// Functional tests
// =============================================================================

TEST_F(ReembedTest, EmptyTableSucceeds) {
    create_embedding_table();

    auto qr = exec_ok("REEMBED TABLE products");
    EXPECT_EQ(qr.affected_rows, 0);
    EXPECT_EQ(qr.message, "REEMBED 0 rows");
}

TEST_F(ReembedTest, SingleRowRegeneratesEmbedding) {
    create_embedding_table();
    insert_row(1, "laptop computer");

    // Verify initial embedding is all zeros.
    auto before = read_all_embeddings();
    ASSERT_EQ(before.size(), 1u);
    EXPECT_EQ(before[0], (Embedding{0.0F, 0.0F, 0.0F, 0.0F}));

    // Run REEMBED.
    auto qr = exec_ok("REEMBED TABLE products");
    EXPECT_EQ(qr.affected_rows, 1);

    // Verify embedding was regenerated (no longer all zeros).
    auto after = read_all_embeddings();
    ASSERT_EQ(after.size(), 1u);
    EXPECT_NE(after[0], (Embedding{0.0F, 0.0F, 0.0F, 0.0F}));
    EXPECT_EQ(after[0].size(), 4u);
}

TEST_F(ReembedTest, MultipleRowsAllRegenerated) {
    create_embedding_table();
    insert_row(1, "laptop computer");
    insert_row(2, "wireless mouse");
    insert_row(3, "mechanical keyboard");

    // All embeddings start as zeros.
    auto before = read_all_embeddings();
    ASSERT_EQ(before.size(), 3u);
    for (const auto& emb : before) {
        EXPECT_EQ(emb, (Embedding{0.0F, 0.0F, 0.0F, 0.0F}));
    }

    // Run REEMBED.
    auto qr = exec_ok("REEMBED TABLE products");
    EXPECT_EQ(qr.affected_rows, 3);

    // All embeddings should be regenerated (non-zero).
    auto after = read_all_embeddings();
    ASSERT_EQ(after.size(), 3u);
    for (const auto& emb : after) {
        EXPECT_NE(emb, (Embedding{0.0F, 0.0F, 0.0F, 0.0F}));
        EXPECT_EQ(emb.size(), 4u);
    }

    // Different source texts should produce different embeddings.
    EXPECT_NE(after[0], after[1]);
    EXPECT_NE(after[1], after[2]);
}

TEST_F(ReembedTest, DeterministicEmbeddings) {
    create_embedding_table();
    insert_row(1, "hello world");

    // Run REEMBED twice — embeddings should be identical.
    exec_ok("REEMBED TABLE products");
    auto first = read_all_embeddings();

    exec_ok("REEMBED TABLE products");
    auto second = read_all_embeddings();

    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(first[0], second[0]);
}

TEST_F(ReembedTest, BatchProcessingHandlesMoreThan32Rows) {
    create_embedding_table();

    // Insert 50 rows (more than batch_size = 32).
    for (int i = 1; i <= 50; ++i) {
        insert_row(i, "product " + std::to_string(i));
    }

    auto qr = exec_ok("REEMBED TABLE products");
    EXPECT_EQ(qr.affected_rows, 50);

    auto after = read_all_embeddings();
    ASSERT_EQ(after.size(), 50u);

    // All embeddings should be non-zero.
    for (const auto& emb : after) {
        EXPECT_NE(emb, (Embedding{0.0F, 0.0F, 0.0F, 0.0F}));
    }
}

TEST_F(ReembedTest, MessageIncludesRowCount) {
    create_embedding_table();
    insert_row(1, "test");
    insert_row(2, "test2");

    auto qr = exec_ok("REEMBED TABLE products");
    EXPECT_EQ(qr.message, "REEMBED 2 rows");
}

TEST_F(ReembedTest, ReembedWithOptionalTableKeyword) {
    create_embedding_table();
    insert_row(1, "test");

    // REEMBED TABLE products (with TABLE keyword).
    auto qr1 = exec_ok("REEMBED TABLE products");
    EXPECT_EQ(qr1.affected_rows, 1);

    // REEMBED products (without TABLE keyword).
    auto qr2 = exec_ok("REEMBED products");
    EXPECT_EQ(qr2.affected_rows, 1);
}

// =============================================================================
// HNSW index rebuild tests
// =============================================================================

TEST_F(ReembedTest, HnswIndexIsRebuiltAfterReembed) {
    create_embedding_table();
    insert_row(1, "laptop computer");
    insert_row(2, "wireless mouse");
    insert_row(3, "mechanical keyboard");

    // Set up an HNSW index backed by a real buffer pool.
    auto hnsw_path = std::filesystem::temp_directory_path() / "sixseven_test_reembed_hnsw.db";
    std::filesystem::remove(hnsw_path);

    auto hfid = dm_.create_file(hnsw_path, false, true);
    ASSERT_TRUE(hfid.has_value()) << hfid.error().message;

    auto hnsw_bpm = std::make_unique<BufferPoolManager>(dm_, *hfid, 256);
    auto hnsw = std::make_unique<HnswIndex>(*hnsw_bpm);

    auto create_result = hnsw->create({.dimension = 4});
    ASSERT_TRUE(create_result.has_value()) << create_result.error().message;

    // Insert dummy vectors (all zeros) to simulate initial state.
    for (int i = 0; i < 3; ++i) {
        std::vector<float> dummy(4, 0.0F);
        auto ins = hnsw->insert(std::span<const float>(dummy));
        ASSERT_TRUE(ins.has_value()) << ins.error().message;
        EXPECT_EQ(*ins, static_cast<uint32_t>(i));
    }
    EXPECT_EQ(hnsw->node_count(), 3u);

    // Create the HNSW IndexDef in the catalog so lookup by index_id works.
    auto table_schema = catalog_.get_table(default_database_id, "products");
    ASSERT_TRUE(table_schema.has_value());
    IndexDef hnsw_def;
    hnsw_def.table_id = table_schema->table_id;
    hnsw_def.name = "hnsw_products_emb";
    hnsw_def.index_type = "hnsw";
    hnsw_def.columns = "emb";
    hnsw_def.is_unique = false;
    auto idx_id = catalog_.create_index(hnsw_def);
    ASSERT_TRUE(idx_id.has_value()) << idx_id.error().message;

    // Wire the HNSW index into the engine keyed by index_id.
    std::unordered_map<index_id_t, HnswIndex*> hnsw_map;
    hnsw_map[*idx_id] = hnsw.get();
    engine_->set_hnsw_indexes(&hnsw_map);

    // Run REEMBED.
    auto qr = exec_ok("REEMBED TABLE products");
    EXPECT_EQ(qr.affected_rows, 3);

    // HNSW index should have 3 nodes with IDs 0, 1, 2 (rebuilt from scratch).
    EXPECT_EQ(hnsw->node_count(), 3u);

    // Verify node IDs are 0, 1, 2 by checking locations exist.
    for (uint32_t n = 0; n < 3; ++n) {
        auto loc = hnsw->node_location(n);
        EXPECT_TRUE(loc.has_value()) << "node " << n << " should exist after rebuild";
    }

    // Verify the vectors in the index are non-zero (regenerated, not the old dummies).
    for (uint32_t n = 0; n < 3; ++n) {
        auto loc = hnsw->node_location(n);
        ASSERT_TRUE(loc.has_value());
        auto node = hnsw->read_node(*loc);
        ASSERT_TRUE(node.has_value());
        auto vec = hnsw->read_vector(node->vector_page_id, node->vector_slot_id);
        ASSERT_TRUE(vec.has_value());
        EXPECT_EQ(vec->size(), 4u);

        bool all_zero = true;
        for (float v : *vec) {
            if (v != 0.0F) {
                all_zero = false;
                break;
            }
        }
        EXPECT_FALSE(all_zero) << "node " << n << " vector should be non-zero after REEMBED";
    }

    // Verify search works: query with the first row's embedding should return node 0.
    auto embeddings = read_all_embeddings();
    ASSERT_EQ(embeddings.size(), 3u);
    auto results = hnsw->search(std::span<const float>(embeddings[0]), 1);
    ASSERT_TRUE(results.has_value()) << results.error().message;
    ASSERT_EQ(results->size(), 1u);
    EXPECT_EQ((*results)[0].node_id, 0u);
    EXPECT_FLOAT_EQ((*results)[0].distance, 0.0F);

    // Cleanup.
    engine_->set_hnsw_indexes(nullptr);
    hnsw.reset();
    hnsw_bpm.reset();
    (void)dm_.close_file(*hfid);
    std::filesystem::remove(hnsw_path);
}
