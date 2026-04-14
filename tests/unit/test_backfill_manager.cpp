/// @file test_backfill_manager.cpp
/// @brief Unit tests for BackfillManager.
///
/// Tests the BackfillManager's ability to scan tables, identify NULL embedding
/// columns, enqueue jobs, track progress, and reject duplicate backfills.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/parser/ast.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"
#include "sixseven/vector/backfill_manager.h"
#include "sixseven/vector/builtin_provider.h"
#include "sixseven/vector/embedding_worker.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class BackfillManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_backfill";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);

        // Create a test database in the catalog.
        auto db_id = catalog_.create_database("testdb");
        ASSERT_TRUE(db_id.has_value()) << db_id.error().message;
        test_db_id_ = *db_id;

        pool_ = std::make_unique<EmbeddingWorkerPool>(
            EmbeddingWorkerConfig{.num_workers = 1,
                                  .max_batch_size = 32,
                                  .max_retries = 2,
                                  .base_backoff = std::chrono::milliseconds{10},
                                  .max_backoff = std::chrono::milliseconds{50}});
        pool_->register_provider("builtin/4", std::make_shared<BuiltinProvider>(4));

        auto start = pool_->start();
        ASSERT_TRUE(start.has_value()) << start.error().message;

        mgr_ = std::make_unique<BackfillManager>(catalog_, *storage_, *pool_);
    }

    void TearDown() override {
        mgr_.reset();
        if (pool_ && pool_->is_running()) {
            auto stop = pool_->stop();
            EXPECT_TRUE(stop.has_value());
        }
        pool_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    /// Create a test table with an EMBEDDING column and insert rows.
    /// Returns the table_id.
    table_id_t create_test_table(int num_rows, int num_with_embeddings = 0) {
        // Register the table in the catalog.
        CatalogColumnDef id_col;
        id_col.name = "id";
        id_col.type_id = TypeId::INT32;
        id_col.ordinal = 0;

        CatalogColumnDef text_col;
        text_col.name = "body";
        text_col.type_id = TypeId::STRING;
        text_col.ordinal = 1;

        CatalogColumnDef emb_col;
        emb_col.name = "vec";
        emb_col.type_id = TypeId::EMBEDDING;
        emb_col.ordinal = 2;

        TableSchema ts;
        ts.name = "test_table";
        ts.columns = {id_col, text_col, emb_col};
        ts.pk_columns = "id";

        auto table_id = catalog_.create_table(test_db_id_, ts);
        EXPECT_TRUE(table_id.has_value()) << table_id.error().message;

        // Register embedding column metadata.
        EmbeddingColumnDef emb_def;
        emb_def.table_id = *table_id;
        emb_def.column_id = 2;
        emb_def.dimension = 4;
        emb_def.source_expr = "body";
        emb_def.provider = "builtin/4";
        auto reg = catalog_.register_embedding_column(emb_def);
        EXPECT_TRUE(reg.has_value()) << reg.error().message;

        // Create table storage.
        auto create_storage = storage_->create_table_storage(test_db_id_, *table_id, ts);
        EXPECT_TRUE(create_storage.has_value()) << create_storage.error().message;

        auto storage_ptr = storage_->get_table_storage(*table_id);
        EXPECT_TRUE(storage_ptr.has_value());

        Schema schema({
            {"id", TypeId::INT32},
            {"body", TypeId::STRING},
            {"vec", TypeId::EMBEDDING},
        });

        // Insert rows.
        for (int i = 0; i < num_rows; ++i) {
            std::vector<Value> vals;
            vals.push_back(Value(static_cast<int32_t>(i)));
            vals.push_back(Value(std::string("text " + std::to_string(i))));
            if (i < num_with_embeddings) {
                vals.push_back(Value(Embedding{1.0F, 0.0F, 0.0F, 0.0F}));
            } else {
                vals.push_back(Value()); // NULL embedding.
            }

            auto bytes = TupleSerializer::serialize(vals, schema);
            EXPECT_TRUE(bytes.has_value()) << bytes.error().message;
            auto rid = (*storage_ptr)->heap->insert_tuple(*bytes);
            EXPECT_TRUE(rid.has_value()) << rid.error().message;
        }

        return *table_id;
    }

    DiskManager dm_;
    Catalog catalog_;
    database_id_t test_db_id_ = 0;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<EmbeddingWorkerPool> pool_;
    std::unique_ptr<BackfillManager> mgr_;
};

// =============================================================================
// Tests
// =============================================================================

TEST_F(BackfillManagerTest, StartBackfillOnTableWithEmbeddings) {
    create_test_table(5, 2); // 5 rows, 2 already have embeddings

    BackfillStmt stmt;
    stmt.table_name = "test_table";
    stmt.batch_size = 10;

    auto result = mgr_->start(stmt, test_db_id_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Wait for the backfill to complete.
    for (int i = 0; i < 100; ++i) {
        auto s = mgr_->status("test_table");
        if (!s.running) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    auto s = mgr_->status("test_table");
    EXPECT_FALSE(s.running);
    EXPECT_EQ(s.processed, 5);
    EXPECT_EQ(s.skipped, 2);   // 2 rows already had embeddings
    EXPECT_EQ(s.generated, 3); // 3 new embedding jobs enqueued
}

TEST_F(BackfillManagerTest, RejectDuplicateBackfill) {
    create_test_table(100, 0); // Many rows to keep it running

    BackfillStmt stmt;
    stmt.table_name = "test_table";
    stmt.batch_size = 1; // Small batches to keep it running longer

    auto result1 = mgr_->start(stmt, test_db_id_);
    ASSERT_TRUE(result1.has_value()) << result1.error().message;

    // Try to start a second backfill on the same table.
    auto result2 = mgr_->start(stmt, test_db_id_);
    EXPECT_FALSE(result2.has_value());
    EXPECT_EQ(result2.error().code, StatusCode::ALREADY_EXISTS);

    // Wait for first to finish.
    for (int i = 0; i < 200; ++i) {
        auto s = mgr_->status("test_table");
        if (!s.running) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

TEST_F(BackfillManagerTest, BackfillNonExistentTableFails) {
    BackfillStmt stmt;
    stmt.table_name = "nonexistent";

    auto result = mgr_->start(stmt, test_db_id_);
    EXPECT_FALSE(result.has_value());
}

TEST_F(BackfillManagerTest, AllStatusesReturnsAllJobs) {
    create_test_table(3, 3); // All rows already have embeddings — backfill finishes instantly

    BackfillStmt stmt;
    stmt.table_name = "test_table";
    stmt.batch_size = 100;

    auto result = mgr_->start(stmt, test_db_id_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Wait for completion.
    for (int i = 0; i < 100; ++i) {
        auto s = mgr_->status("test_table");
        if (!s.running) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    auto statuses = mgr_->all_statuses();
    ASSERT_EQ(statuses.size(), 1u);
    EXPECT_EQ(statuses[0].table_name, "test_table");
    EXPECT_EQ(statuses[0].processed, 3);
    EXPECT_EQ(statuses[0].skipped, 3);
    EXPECT_EQ(statuses[0].generated, 0);
    EXPECT_FALSE(statuses[0].running);
}

TEST_F(BackfillManagerTest, CancelRunningBackfill) {
    create_test_table(100, 0);

    BackfillStmt stmt;
    stmt.table_name = "test_table";
    stmt.batch_size = 1;
    stmt.rate_limit = 10; // Very slow: 10 rows/sec → ~10 seconds total

    auto result = mgr_->start(stmt, test_db_id_);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Let it start processing a few batches.
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    auto cancel = mgr_->cancel("test_table");
    EXPECT_TRUE(cancel.has_value());

    // Wait for it to actually stop.
    for (int i = 0; i < 100; ++i) {
        auto s = mgr_->status("test_table");
        if (!s.running) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    auto s = mgr_->status("test_table");
    EXPECT_FALSE(s.running);
    // Should have processed fewer than all rows due to rate limiting + cancel.
    EXPECT_LT(s.processed, 100);
}

TEST_F(BackfillManagerTest, CancelNonExistentFails) {
    auto result = mgr_->cancel("nonexistent");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(BackfillManagerTest, StatusOfUnknownTableReturnsDefaults) {
    auto s = mgr_->status("unknown_table");
    EXPECT_EQ(s.table_name, "unknown_table");
    EXPECT_EQ(s.processed, 0);
    EXPECT_EQ(s.generated, 0);
    EXPECT_EQ(s.skipped, 0);
    EXPECT_FALSE(s.running);
}
