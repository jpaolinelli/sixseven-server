#pragma once

/// @file test_qa_gdb_258_fixture.h
/// @brief Shared test fixtures for GDB-258 QA and regression tests.
///
/// Included by test_qa_gdb_258.cpp and test_qa_gdb_258_impl.cpp.
/// All member functions are defined inline within the class bodies so that
/// including this header in two translation units does not cause
/// duplicate-symbol link errors (ODR-safe).

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
// Adversarial QA fixture (used by test_qa_gdb_258.cpp)
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
// Regression fixture (used by test_qa_gdb_258_impl.cpp)
// =============================================================================

class GDB258ProviderRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_gdb258";
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

    /// Create a table with an EMBEDDING column and register the provider.
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

    /// Insert a row with a pre-computed embedding directly into the heap.
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
