#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"
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
#include <vector>

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class InsertEmbeddingTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_insert_emb";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);

        // Create worker pool with fast processing for tests.
        pool_ = std::make_unique<EmbeddingWorkerPool>(
            EmbeddingWorkerConfig{.num_workers = 1, .max_batch_size = 32});
        pool_->register_provider("builtin/4", std::make_shared<BuiltinProvider>(4));
        pool_->register_provider("builtin/8", std::make_shared<BuiltinProvider>(8));

        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
        engine_->set_provider_registry(&provider_registry_);
        engine_->set_embedding_worker_pool(pool_.get());

        auto start = pool_->start();
        ASSERT_TRUE(start.has_value()) << start.error().message;
    }

    void TearDown() override {
        if (pool_ && pool_->is_running()) {
            auto stop = pool_->stop();
            EXPECT_TRUE(stop.has_value());
        }
        engine_.reset();
        pool_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    /// Execute SQL, assert success, return the result.
    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return std::move(*result);
    }

    /// Wait for the worker pool to process all pending jobs.
    /// Returns true if drained within timeout, false on timeout.
    bool wait_for_pool(std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pool_->pending_count() == 0) {
                // Give the store callback a moment to complete.
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return false;
    }

    DiskManager dm_;
    Catalog catalog_;
    ProviderRegistry provider_registry_{catalog_};
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<EmbeddingWorkerPool> pool_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// Tests
// =============================================================================

TEST_F(InsertEmbeddingTest, InsertTriggersEmbeddingGeneration) {
    // Create a table with an EMBEDDING column sourced from 'title'.
    exec_ok("CREATE TABLE articles ("
            "  id INT,"
            "  title TEXT,"
            "  title_vec EMBEDDING(4, title, 'builtin/4')"
            ")");

    // Insert a row with source text.
    auto qr = exec_ok("INSERT INTO articles (id, title) VALUES (1, 'hello world')");
    EXPECT_EQ(qr.affected_rows, 1);

    // Wait for async embedding generation.
    ASSERT_TRUE(wait_for_pool()) << "embedding worker pool did not drain in time";

    // Verify the embedding was stored.
    auto select = exec_ok("SELECT id, title, title_vec FROM articles WHERE id = 1");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 1);
    EXPECT_EQ(select.rows[0][1].as_string(), "hello world");

    // The embedding should be non-null (builtin provider generates deterministic vectors).
    EXPECT_FALSE(select.rows[0][2].is_null());
    EXPECT_EQ(select.rows[0][2].type_id(), TypeId::EMBEDDING);
    EXPECT_EQ(select.rows[0][2].as_embedding().size(), 4u);
}

TEST_F(InsertEmbeddingTest, MultiRowInsertTriggersEmbeddings) {
    exec_ok("CREATE TABLE docs ("
            "  id INT,"
            "  body TEXT,"
            "  body_vec EMBEDDING(4, body, 'builtin/4')"
            ")");

    exec_ok("INSERT INTO docs (id, body) VALUES "
            "(1, 'first document'), "
            "(2, 'second document'), "
            "(3, 'third document')");

    ASSERT_TRUE(wait_for_pool());

    auto select = exec_ok("SELECT id, body_vec FROM docs");
    ASSERT_EQ(select.rows.size(), 3u);

    // All three rows should have embeddings generated.
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_FALSE(select.rows[i][1].is_null()) << "row " << i << " embedding is null";
        EXPECT_EQ(select.rows[i][1].type_id(), TypeId::EMBEDDING);
        EXPECT_EQ(select.rows[i][1].as_embedding().size(), 4u);
    }
}

TEST_F(InsertEmbeddingTest, InsertWithNullSourceTextLeavesEmbeddingNull) {
    exec_ok("CREATE TABLE notes ("
            "  id INT,"
            "  content TEXT,"
            "  content_vec EMBEDDING(4, content, 'builtin/4')"
            ")");

    // Insert a row with NULL source text — the embedding job is created but
    // BuiltinProvider rejects empty text, so the embedding stays null.
    exec_ok("INSERT INTO notes (id, content) VALUES (1, NULL)");

    // Wait for the pool to attempt processing (and fail gracefully).
    ASSERT_TRUE(wait_for_pool());

    auto select = exec_ok("SELECT id, content_vec FROM notes WHERE id = 1");
    ASSERT_EQ(select.rows.size(), 1u);
    // Embedding remains null because the source text was empty.
    EXPECT_TRUE(select.rows[0][1].is_null());
}

TEST_F(InsertEmbeddingTest, TableWithoutEmbeddingColumnsUnaffected) {
    // A regular table without EMBEDDING columns should work normally.
    exec_ok("CREATE TABLE plain (id INT, name TEXT)");

    auto qr = exec_ok("INSERT INTO plain VALUES (1, 'test')");
    EXPECT_EQ(qr.affected_rows, 1);

    // No jobs should have been enqueued.
    EXPECT_EQ(pool_->pending_count(), 0u);

    auto select = exec_ok("SELECT id, name FROM plain");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 1);
    EXPECT_EQ(select.rows[0][1].as_string(), "test");
}

TEST_F(InsertEmbeddingTest, MultipleEmbeddingColumns) {
    exec_ok("CREATE TABLE multi ("
            "  id INT,"
            "  title TEXT,"
            "  body TEXT,"
            "  title_vec EMBEDDING(4, title, 'builtin/4'),"
            "  body_vec EMBEDDING(8, body, 'builtin/8')"
            ")");

    exec_ok("INSERT INTO multi (id, title, body) VALUES (1, 'My Title', 'My Body')");

    ASSERT_TRUE(wait_for_pool());

    auto select = exec_ok("SELECT id, title_vec, body_vec FROM multi WHERE id = 1");
    ASSERT_EQ(select.rows.size(), 1u);

    // title_vec: dimension 4
    EXPECT_FALSE(select.rows[0][1].is_null());
    EXPECT_EQ(select.rows[0][1].as_embedding().size(), 4u);

    // body_vec: dimension 8
    EXPECT_FALSE(select.rows[0][2].is_null());
    EXPECT_EQ(select.rows[0][2].as_embedding().size(), 8u);
}

// -- GDB-297: Batch poisoning — NULL source must not lose valid embeddings -----

TEST_F(InsertEmbeddingTest, MixedNullAndValidSourceTextGeneratesValidEmbeddings) {
    exec_ok("CREATE TABLE articles ("
            "  id INT,"
            "  title TEXT,"
            "  title_vec EMBEDDING(4, title, 'builtin/4')"
            ")");

    // Multi-row INSERT with a mix of NULL and non-NULL source texts.
    exec_ok("INSERT INTO articles (id, title) VALUES "
            "(1, 'valid text one'), "
            "(2, NULL), "
            "(3, 'valid text two'), "
            "(4, NULL)");

    ASSERT_TRUE(wait_for_pool());

    auto select = exec_ok("SELECT id, title_vec FROM articles");
    ASSERT_EQ(select.rows.size(), 4u);

    // Rows 1 and 3 (valid source text) must have embeddings.
    EXPECT_FALSE(select.rows[0][1].is_null()) << "row 1 should have embedding";
    EXPECT_EQ(select.rows[0][1].as_embedding().size(), 4u);

    EXPECT_FALSE(select.rows[2][1].is_null()) << "row 3 should have embedding";
    EXPECT_EQ(select.rows[2][1].as_embedding().size(), 4u);

    // Rows 2 and 4 (NULL source text) must have NULL embeddings.
    EXPECT_TRUE(select.rows[1][1].is_null()) << "row 2 should be null";
    EXPECT_TRUE(select.rows[3][1].is_null()) << "row 4 should be null";
}

TEST_F(InsertEmbeddingTest, AllNullSourceTextEnqueuesNoJobs) {
    exec_ok("CREATE TABLE all_null ("
            "  id INT,"
            "  content TEXT,"
            "  content_vec EMBEDDING(4, content, 'builtin/4')"
            ")");

    auto before = pool_->stats();

    exec_ok("INSERT INTO all_null (id, content) VALUES (1, NULL), (2, NULL), (3, NULL)");

    ASSERT_TRUE(wait_for_pool());

    // No jobs should have been enqueued for all-NULL source texts.
    auto after = pool_->stats();
    EXPECT_EQ(after.jobs_processed, before.jobs_processed);
    EXPECT_EQ(after.jobs_failed, before.jobs_failed);
}

TEST_F(InsertEmbeddingTest, WhitespaceOnlySourceDoesNotPoisonValidEmbeddings) {
    exec_ok("CREATE TABLE ws_test ("
            "  id INT,"
            "  title TEXT,"
            "  title_vec EMBEDDING(4, title, 'builtin/4')"
            ")");

    // Mix of valid text, whitespace-only, and NULL source texts.
    exec_ok("INSERT INTO ws_test (id, title) VALUES "
            "(1, 'valid text'), "
            "(2, '   '), "
            "(3, 'also valid'), "
            "(4, NULL)");

    ASSERT_TRUE(wait_for_pool());

    auto select = exec_ok("SELECT id, title_vec FROM ws_test");
    ASSERT_EQ(select.rows.size(), 4u);

    // Rows 1 and 3 (valid source text) must have embeddings.
    EXPECT_FALSE(select.rows[0][1].is_null()) << "row 1 should have embedding";
    EXPECT_EQ(select.rows[0][1].as_embedding().size(), 4u);

    EXPECT_FALSE(select.rows[2][1].is_null()) << "row 3 should have embedding";
    EXPECT_EQ(select.rows[2][1].as_embedding().size(), 4u);

    // Rows 2 (whitespace-only) and 4 (NULL) should have NULL embeddings.
    EXPECT_TRUE(select.rows[1][1].is_null()) << "row 2 (whitespace) should be null";
    EXPECT_TRUE(select.rows[3][1].is_null()) << "row 4 (NULL) should be null";
}

TEST_F(InsertEmbeddingTest, WorkerPoolStatsReflectInsertJobs) {
    exec_ok("CREATE TABLE stats_test ("
            "  id INT,"
            "  text TEXT,"
            "  vec EMBEDDING(4, text, 'builtin/4')"
            ")");

    auto before = pool_->stats();
    auto before_processed = before.jobs_processed;

    exec_ok("INSERT INTO stats_test (id, text) VALUES (1, 'sample')");

    ASSERT_TRUE(wait_for_pool());

    auto after = pool_->stats();
    EXPECT_GT(after.jobs_processed, before_processed);
}
