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

// BackfillGeneratesEmbeddings inserts rows with NULL body so the INSERT path
// enqueues no embedding jobs, leaving the vec column NULL.  BACKFILL must then
// detect those NULLs, skip any non-NULL rows, and write back real embeddings.
TEST_F(QABackfillTest, BackfillGeneratesEmbeddings) {
    exec_ok("CREATE TABLE t1 (id INT, body TEXT, vec EMBEDDING(4, body, 'builtin/4'))");

    // Insert rows with NULL body so the INSERT path enqueues no embedding jobs;
    // the vec column therefore remains NULL.  This is the state BACKFILL is
    // designed to remediate.
    exec_ok("INSERT INTO t1 (id, body) VALUES (1, NULL)");
    exec_ok("INSERT INTO t1 (id, body) VALUES (2, NULL)");
    exec_ok("INSERT INTO t1 (id, body) VALUES (3, NULL)");

    // Now update body values so backfill has source text to embed from.
    exec_ok("UPDATE t1 SET body = 'hello' WHERE id = 1");
    exec_ok("UPDATE t1 SET body = 'world' WHERE id = 2");
    exec_ok("UPDATE t1 SET body = 'test'  WHERE id = 3");

    // Wait for any pool activity from the updates to settle.
    ASSERT_TRUE(wait_for_pool());

    // Confirm vecs are still NULL after the updates (UPDATE does not recompute
    // the embedding column automatically for already-inserted rows).
    {
        auto pre = exec_ok("SELECT vec FROM t1");
        ASSERT_EQ(pre.rows.size(), 3u);
        for (size_t i = 0; i < pre.rows.size(); ++i) {
            EXPECT_TRUE(pre.rows[i][0].is_null())
                << "expected NULL vec before backfill for row " << i;
        }
    }

    // Execute BACKFILL.
    auto qr = exec_ok("BACKFILL EMBEDDINGS ON t1");
    EXPECT_FALSE(qr.message.empty());

    ASSERT_TRUE(wait_for_backfill("t1"));
    ASSERT_TRUE(wait_for_pool());

    // --- BackfillManager::Status assertions ---
    auto s = backfill_mgr_->status("t1");
    // All 3 rows must have been scanned.
    EXPECT_EQ(s.processed, 3);
    // All 3 had NULL vecs and non-NULL body: backfill should have enqueued an
    // embedding job for each.
    EXPECT_EQ(s.generated, 3);
    // None were skipped because none had a pre-existing embedding.
    EXPECT_EQ(s.skipped, 0);
    // Accounting closure: generated + skipped == processed.
    EXPECT_EQ(s.generated + s.skipped, s.processed);

    // --- SELECT to confirm embeddings were written back ---
    auto sel = exec_ok("SELECT vec FROM t1");
    ASSERT_EQ(sel.rows.size(), 3u);
    for (size_t i = 0; i < sel.rows.size(); ++i) {
        ASSERT_EQ(sel.rows[i].size(), 1u);
        EXPECT_FALSE(sel.rows[i][0].is_null())
            << "vec is still NULL for row " << i << " after backfill";
        if (!sel.rows[i][0].is_null()) {
            EXPECT_EQ(sel.rows[i][0].as_embedding().size(), 4u)
                << "wrong embedding dimension for row " << i;
        }
    }
}

// BackfillParsesAndExecutes: baseline smoke test — BACKFILL on pre-embedded rows
// must report processed == row count, accounting closure holds, and all vecs
// remain non-NULL.
TEST_F(QABackfillTest, BackfillParsesAndExecutes) {
    exec_ok("CREATE TABLE t4 (id INT, body TEXT, vec EMBEDDING(4, body, 'builtin/4'))");

    exec_ok("INSERT INTO t4 (id, body) VALUES (1, 'hello')");
    exec_ok("INSERT INTO t4 (id, body) VALUES (2, 'world')");
    exec_ok("INSERT INTO t4 (id, body) VALUES (3, 'test')");

    // Wait for INSERT-triggered embeddings to complete.
    ASSERT_TRUE(wait_for_pool());

    // Execute BACKFILL command.
    auto qr = exec_ok("BACKFILL EMBEDDINGS ON t4");
    EXPECT_FALSE(qr.message.empty());

    ASSERT_TRUE(wait_for_backfill("t4"));
    ASSERT_TRUE(wait_for_pool());

    // All 3 rows were already embedded by INSERT, so backfill must skip them
    // all (generated == 0, skipped == 3).
    auto s = backfill_mgr_->status("t4");
    EXPECT_EQ(s.processed, 3);
    EXPECT_EQ(s.skipped, 3);
    EXPECT_EQ(s.generated, 0);
    EXPECT_EQ(s.generated + s.skipped, s.processed);

    // vecs must still be non-NULL and 4-dimensional.
    auto sel = exec_ok("SELECT vec FROM t4");
    ASSERT_EQ(sel.rows.size(), 3u);
    for (size_t i = 0; i < sel.rows.size(); ++i) {
        EXPECT_FALSE(sel.rows[i][0].is_null()) << "vec is NULL for row " << i;
        if (!sel.rows[i][0].is_null()) {
            EXPECT_EQ(sel.rows[i][0].as_embedding().size(), 4u);
        }
    }
}

// BackfillWithBatchAndRateLimit verifies that BATCH and RATE_LIMIT options are
// accepted, reach the BackfillManager, and do not prevent the job from
// completing successfully.  The test does not assert wall-clock timing (that
// would be flaky) — it asserts that the job ran to completion with correct
// accounting.
TEST_F(QABackfillTest, BackfillWithBatchAndRateLimit) {
    exec_ok("CREATE TABLE t2 (id INT, body TEXT, vec EMBEDDING(4, body, 'builtin/4'))");
    exec_ok("INSERT INTO t2 (id, body) VALUES (1, 'hello')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("BACKFILL EMBEDDINGS ON t2 BATCH 50 RATE_LIMIT 100");
    EXPECT_FALSE(qr.message.empty());

    ASSERT_TRUE(wait_for_backfill("t2"));
    ASSERT_TRUE(wait_for_pool());

    // The job must have completed and scanned the single row.
    auto s = backfill_mgr_->status("t2");
    EXPECT_FALSE(s.running);
    EXPECT_EQ(s.processed, 1);
    // Row was pre-embedded by INSERT, so it must be skipped (generated == 0).
    EXPECT_EQ(s.generated + s.skipped, s.processed);

    // The vec must be non-NULL with the correct dimension.
    auto sel = exec_ok("SELECT vec FROM t2");
    ASSERT_EQ(sel.rows.size(), 1u);
    EXPECT_FALSE(sel.rows[0][0].is_null());
    if (!sel.rows[0][0].is_null()) {
        EXPECT_EQ(sel.rows[0][0].as_embedding().size(), 4u);
    }
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

    // Should have at least one row for t3.
    ASSERT_GE(qr.rows.size(), 1u);

    // Locate the t3 row (other tests may have inserted entries too).
    int t3_row = -1;
    for (size_t i = 0; i < qr.rows.size(); ++i) {
        if (qr.rows[i][0].as_string() == "t3") {
            t3_row = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(t3_row, 0) << "t3 not found in SHOW BACKFILL STATUS output";

    EXPECT_EQ(qr.rows[static_cast<size_t>(t3_row)][1].as_string(), "completed");

    // processed / generated / skipped must be non-negative integers and must
    // satisfy the accounting closure: generated + skipped == processed.
    auto processed_val = qr.rows[static_cast<size_t>(t3_row)][2].as_int64();
    auto generated_val = qr.rows[static_cast<size_t>(t3_row)][3].as_int64();
    auto skipped_val = qr.rows[static_cast<size_t>(t3_row)][4].as_int64();

    EXPECT_GE(processed_val, 1) << "SHOW BACKFILL STATUS: processed should be >= 1";
    EXPECT_GE(generated_val, 0) << "SHOW BACKFILL STATUS: generated should be >= 0";
    EXPECT_GE(skipped_val, 0) << "SHOW BACKFILL STATUS: skipped should be >= 0";
    EXPECT_EQ(generated_val + skipped_val, processed_val)
        << "SHOW BACKFILL STATUS: generated + skipped must equal processed";
}

TEST_F(QABackfillTest, ShowBackfillWithoutStatus) {
    // SHOW BACKFILL (without STATUS) should also work.
    auto qr = exec_ok("SHOW BACKFILL");
    ASSERT_EQ(qr.column_names.size(), 6u);
    EXPECT_EQ(qr.column_names[0], "table_name");
}
