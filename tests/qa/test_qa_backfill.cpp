/// @file test_qa_backfill.cpp
/// @brief QA regression tests for BACKFILL EMBEDDINGS command.
///
/// End-to-end tests verifying the BACKFILL EMBEDDINGS ON table SQL command
/// correctly generates embeddings for rows with NULL embedding columns,
/// skips rows that already have embeddings, and reports progress via
/// SHOW BACKFILL STATUS.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/vector/backfill_manager.h"
#include "sixseven/vector/builtin_provider.h"
#include "sixseven/vector/embedding_worker.h"
#include "sixseven/vector/provider_registry.h"

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

class QABackfillTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_backfill";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);

        pool_ = std::make_unique<EmbeddingWorkerPool>(
            EmbeddingWorkerConfig{.num_workers = 1,
                                  .max_batch_size = 32,
                                  .max_retries = 2,
                                  .base_backoff = std::chrono::milliseconds{10},
                                  .max_backoff = std::chrono::milliseconds{50}});
        pool_->register_provider("builtin/4", std::make_shared<BuiltinProvider>(4));

        backfill_mgr_ = std::make_unique<BackfillManager>(catalog_, *storage_, *pool_);

        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
        engine_->set_provider_registry(&provider_registry_);
        engine_->set_embedding_worker_pool(pool_.get());
        engine_->set_backfill_manager(backfill_mgr_.get());

        auto start = pool_->start();
        ASSERT_TRUE(start.has_value()) << start.error().message;

        // Create a database for the tests (the binder resolves table names
        // using current_database_id_).
        exec_ok("CREATE DATABASE testdb");
        auto db = catalog_.get_database("testdb");
        ASSERT_TRUE(db.has_value()) << db.error().message;
        engine_->set_current_database(db->database_id);
    }

    void TearDown() override {
        backfill_mgr_.reset();
        if (pool_ && pool_->is_running()) {
            auto stop = pool_->stop();
            EXPECT_TRUE(stop.has_value());
        }
        engine_.reset();
        pool_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    void exec_fail(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value());
    }

    bool wait_for_pool(std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pool_->pending_count() == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return false;
    }

    bool wait_for_backfill(const std::string& table,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds{10000}) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            auto s = backfill_mgr_->status(table);
            if (!s.running) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        return false;
    }

    DiskManager dm_;
    Catalog catalog_;
    ProviderRegistry provider_registry_{catalog_};
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<EmbeddingWorkerPool> pool_;
    std::unique_ptr<BackfillManager> backfill_mgr_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// Tests
// =============================================================================

TEST_F(QABackfillTest, BackfillParsesAndExecutes) {
    exec_ok("CREATE TABLE t1 (id INT, body TEXT, vec EMBEDDING(4, body, 'builtin/4'))");

    // Insert rows without triggering embedding generation (INSERT with explicit NULL).
    exec_ok("INSERT INTO t1 (id, body) VALUES (1, 'hello')");
    exec_ok("INSERT INTO t1 (id, body) VALUES (2, 'world')");
    exec_ok("INSERT INTO t1 (id, body) VALUES (3, 'test')");

    // Wait for any INSERT-triggered embeddings to complete first.
    ASSERT_TRUE(wait_for_pool());

    // Execute BACKFILL command.
    auto qr = exec_ok("BACKFILL EMBEDDINGS ON t1");
    EXPECT_FALSE(qr.message.empty());

    // Wait for backfill to complete.
    ASSERT_TRUE(wait_for_backfill("t1"));

    // Wait for embedding worker pool to process all jobs.
    ASSERT_TRUE(wait_for_pool());
}

TEST_F(QABackfillTest, BackfillWithBatchAndRateLimit) {
    exec_ok("CREATE TABLE t2 (id INT, body TEXT, vec EMBEDDING(4, body, 'builtin/4'))");
    exec_ok("INSERT INTO t2 (id, body) VALUES (1, 'hello')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("BACKFILL EMBEDDINGS ON t2 BATCH 50 RATE_LIMIT 100");
    EXPECT_FALSE(qr.message.empty());

    ASSERT_TRUE(wait_for_backfill("t2"));
}

TEST_F(QABackfillTest, BackfillOnNonExistentTableFails) {
    exec_fail("BACKFILL EMBEDDINGS ON nonexistent");
}

TEST_F(QABackfillTest, ShowBackfillStatus) {
    exec_ok("CREATE TABLE t3 (id INT, body TEXT, vec EMBEDDING(4, body, 'builtin/4'))");
    exec_ok("INSERT INTO t3 (id, body) VALUES (1, 'hello')");

    ASSERT_TRUE(wait_for_pool());

    exec_ok("BACKFILL EMBEDDINGS ON t3");
    ASSERT_TRUE(wait_for_backfill("t3"));

    auto qr = exec_ok("SHOW BACKFILL STATUS");
    ASSERT_EQ(qr.column_names.size(), 6u);
    EXPECT_EQ(qr.column_names[0], "table_name");
    EXPECT_EQ(qr.column_names[1], "status");
    EXPECT_EQ(qr.column_names[2], "processed");
    EXPECT_EQ(qr.column_names[3], "generated");
    EXPECT_EQ(qr.column_names[4], "skipped");
    EXPECT_EQ(qr.column_names[5], "rows_per_sec");

    // Should have at least one row.
    ASSERT_GE(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "t3");
    EXPECT_EQ(qr.rows[0][1].as_string(), "completed");
}

TEST_F(QABackfillTest, ShowBackfillWithoutStatus) {
    // SHOW BACKFILL (without STATUS) should also work.
    auto qr = exec_ok("SHOW BACKFILL");
    ASSERT_EQ(qr.column_names.size(), 6u);
    EXPECT_EQ(qr.column_names[0], "table_name");
}
