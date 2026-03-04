/// @file test_qa_gdb_297.cpp
/// QA regression tests for GDB-297: Batch embedding failure poisoning valid jobs
/// — one null source text causes entire batch to lose embeddings.
///
/// The fix has two layers:
///   1. InsertOperator::enqueue_embedding_jobs() skips creating jobs when
///      source_text is empty (NULL source → no job enqueued).
///   2. EmbeddingWorkerPool::process_batch() defensively filters empty-text
///      jobs before calling embed_batch, so even if such a job gets enqueued,
///      it won't poison the batch.

#include "giodb/catalog/catalog.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/query_engine.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/storage/disk_manager.h"
#include "giodb/vector/builtin_provider.h"
#include "giodb/vector/embedding_worker.h"
#include "giodb/vector/provider_registry.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace giodb;

// =============================================================================
// End-to-end fixture (full query engine pipeline)
// =============================================================================

class QA_GDB297 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "giodb_qa_gdb297";
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

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return std::move(*result);
    }

    bool wait_for_pool(std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) {
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

    DiskManager dm_;
    Catalog catalog_;
    ProviderRegistry provider_registry_{catalog_};
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<EmbeddingWorkerPool> pool_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// Core bug reproduction: batch poisoning with mixed NULL/valid
// =============================================================================

TEST_F(QA_GDB297, MixedNullValidBatchDoesNotPoisonValidEmbeddings) {
    // This is the exact reproduction case from the bug report.
    exec_ok("CREATE TABLE articles ("
            "  id INT, title TEXT,"
            "  title_vec EMBEDDING(4, title, 'builtin/4')"
            ")");

    exec_ok("INSERT INTO articles (id, title) VALUES "
            "(1, 'valid text one'), "
            "(2, NULL), "
            "(3, 'valid text two'), "
            "(4, NULL)");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, title_vec FROM articles ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 4u);

    // Rows 1 and 3 (valid source text) MUST have embeddings.
    EXPECT_FALSE(qr.rows[0][1].is_null()) << "row 1 embedding lost — batch poisoning";
    EXPECT_EQ(qr.rows[0][1].as_embedding().size(), 4u);

    EXPECT_FALSE(qr.rows[2][1].is_null()) << "row 3 embedding lost — batch poisoning";
    EXPECT_EQ(qr.rows[2][1].as_embedding().size(), 4u);

    // Rows 2 and 4 (NULL source) MUST have NULL embeddings.
    EXPECT_TRUE(qr.rows[1][1].is_null());
    EXPECT_TRUE(qr.rows[3][1].is_null());
}

// =============================================================================
// Boundary: NULL placement patterns within multi-row INSERT
// =============================================================================

TEST_F(QA_GDB297, NullFirstInBatchDoesNotPoisonRest) {
    exec_ok("CREATE TABLE t_nf (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");

    // NULL is the very first row in the batch.
    exec_ok("INSERT INTO t_nf (id, title) VALUES "
            "(1, NULL), (2, 'second'), (3, 'third')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec FROM t_nf ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_TRUE(qr.rows[0][1].is_null());
    EXPECT_FALSE(qr.rows[1][1].is_null()) << "row 2 poisoned by leading NULL";
    EXPECT_FALSE(qr.rows[2][1].is_null()) << "row 3 poisoned by leading NULL";
}

TEST_F(QA_GDB297, NullLastInBatchDoesNotPoisonRest) {
    exec_ok("CREATE TABLE t_nl (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");

    // NULL is the very last row in the batch.
    exec_ok("INSERT INTO t_nl (id, title) VALUES "
            "(1, 'first'), (2, 'second'), (3, NULL)");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec FROM t_nl ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_FALSE(qr.rows[0][1].is_null()) << "row 1 poisoned by trailing NULL";
    EXPECT_FALSE(qr.rows[1][1].is_null()) << "row 2 poisoned by trailing NULL";
    EXPECT_TRUE(qr.rows[2][1].is_null());
}

TEST_F(QA_GDB297, AlternatingNullValidPattern) {
    exec_ok("CREATE TABLE t_alt (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");

    // Interleaved: NULL, valid, NULL, valid, NULL
    exec_ok("INSERT INTO t_alt (id, title) VALUES "
            "(1, NULL), (2, 'a'), (3, NULL), (4, 'b'), (5, NULL)");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec FROM t_alt ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 5u);
    EXPECT_TRUE(qr.rows[0][1].is_null());
    EXPECT_FALSE(qr.rows[1][1].is_null()) << "row 2 should have embedding";
    EXPECT_TRUE(qr.rows[2][1].is_null());
    EXPECT_FALSE(qr.rows[3][1].is_null()) << "row 4 should have embedding";
    EXPECT_TRUE(qr.rows[4][1].is_null());
}

TEST_F(QA_GDB297, SingleNullAmongManyValid) {
    exec_ok("CREATE TABLE t_snmv (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");

    // One poisoner among many valid rows.
    exec_ok("INSERT INTO t_snmv (id, title) VALUES "
            "(1, 'a'), (2, 'b'), (3, 'c'), (4, NULL), "
            "(5, 'd'), (6, 'e'), (7, 'f')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec FROM t_snmv ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 7u);

    for (int i = 0; i < 7; ++i) {
        if (i == 3) {
            EXPECT_TRUE(qr.rows[i][1].is_null()) << "row 4 should be NULL";
        } else {
            EXPECT_FALSE(qr.rows[i][1].is_null())
                << "row " << (i + 1) << " embedding lost — batch poisoning";
        }
    }
}

// =============================================================================
// Multiple embedding columns with mixed NULLs
// =============================================================================

TEST_F(QA_GDB297, TwoEmbColsOneSourceNullOtherValid) {
    // Two embedding columns from different source columns.
    // One source is NULL, the other is valid.
    exec_ok("CREATE TABLE t_2emb ("
            "  id INT, title TEXT, body TEXT,"
            "  title_vec EMBEDDING(4, title, 'builtin/4'),"
            "  body_vec EMBEDDING(8, body, 'builtin/8')"
            ")");

    exec_ok("INSERT INTO t_2emb (id, title, body) VALUES "
            "(1, 'has title', NULL),"
            "(2, NULL, 'has body'),"
            "(3, 'both', 'both'),"
            "(4, NULL, NULL)");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, title_vec, body_vec FROM t_2emb ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 4u);

    // Row 1: title valid, body NULL
    EXPECT_FALSE(qr.rows[0][1].is_null()) << "row 1 title_vec should exist";
    EXPECT_TRUE(qr.rows[0][2].is_null()) << "row 1 body_vec should be NULL";

    // Row 2: title NULL, body valid
    EXPECT_TRUE(qr.rows[1][1].is_null()) << "row 2 title_vec should be NULL";
    EXPECT_FALSE(qr.rows[1][2].is_null()) << "row 2 body_vec should exist";

    // Row 3: both valid
    EXPECT_FALSE(qr.rows[2][1].is_null()) << "row 3 title_vec should exist";
    EXPECT_FALSE(qr.rows[2][2].is_null()) << "row 3 body_vec should exist";

    // Row 4: both NULL
    EXPECT_TRUE(qr.rows[3][1].is_null()) << "row 4 title_vec should be NULL";
    EXPECT_TRUE(qr.rows[3][2].is_null()) << "row 4 body_vec should be NULL";
}

// =============================================================================
// Empty string (not NULL) in mixed batch
// =============================================================================

TEST_F(QA_GDB297, EmptyStringDoesNotPoisonValidEmbeddings) {
    exec_ok("CREATE TABLE t_es (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");

    // '' is not NULL but is empty — should not poison valid texts.
    exec_ok("INSERT INTO t_es (id, title) VALUES "
            "(1, 'valid'), (2, ''), (3, 'also valid')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec FROM t_es ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_FALSE(qr.rows[0][1].is_null()) << "row 1 poisoned by empty string";
    EXPECT_TRUE(qr.rows[1][1].is_null()) << "row 2 empty string should be NULL embedding";
    EXPECT_FALSE(qr.rows[2][1].is_null()) << "row 3 poisoned by empty string";
}

// =============================================================================
// Stress: large mixed batch
// =============================================================================

TEST_F(QA_GDB297, StressLargeMixedBatch) {
    exec_ok("CREATE TABLE t_stress (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");

    // 20-row INSERT: even IDs have NULL source, odd IDs have valid text.
    std::string sql = "INSERT INTO t_stress (id, title) VALUES ";
    for (int i = 1; i <= 20; ++i) {
        if (i > 1)
            sql += ", ";
        if (i % 2 == 0) {
            sql += "(" + std::to_string(i) + ", NULL)";
        } else {
            sql += "(" + std::to_string(i) + ", 'text" + std::to_string(i) + "')";
        }
    }
    exec_ok(sql);

    ASSERT_TRUE(wait_for_pool(std::chrono::milliseconds{10000}));

    auto qr = exec_ok("SELECT id, vec FROM t_stress ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 20u);

    for (int i = 0; i < 20; ++i) {
        int id = i + 1;
        if (id % 2 == 0) {
            EXPECT_TRUE(qr.rows[i][1].is_null())
                << "row " << id << " (NULL source) should have NULL embedding";
        } else {
            EXPECT_FALSE(qr.rows[i][1].is_null())
                << "row " << id << " (valid source) embedding lost — batch poisoning";
        }
    }
}

// =============================================================================
// Stats accuracy: skipped empty-text jobs counted as processed, not failed
// =============================================================================

TEST_F(QA_GDB297, SkippedJobsCountedAsProcessedNotFailed) {
    exec_ok("CREATE TABLE t_sc (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");

    auto before = pool_->stats();

    // 2 valid + 2 NULL = 4 rows, but only 2 jobs should be enqueued.
    exec_ok("INSERT INTO t_sc (id, title) VALUES "
            "(1, 'valid'), (2, NULL), (3, 'valid'), (4, NULL)");

    ASSERT_TRUE(wait_for_pool());

    auto after = pool_->stats();

    // Only the 2 valid jobs get enqueued and processed. NULL rows produce no jobs.
    EXPECT_GE(after.jobs_processed, before.jobs_processed + 2);
    EXPECT_EQ(after.jobs_failed, before.jobs_failed);
}

// =============================================================================
// All NULL batch: no jobs enqueued, no failures
// =============================================================================

TEST_F(QA_GDB297, AllNullBatchNoJobsNoFailures) {
    exec_ok("CREATE TABLE t_an (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");

    auto before = pool_->stats();

    exec_ok("INSERT INTO t_an (id, title) VALUES "
            "(1, NULL), (2, NULL), (3, NULL), (4, NULL), (5, NULL)");

    ASSERT_TRUE(wait_for_pool());

    auto after = pool_->stats();
    // No jobs should have been enqueued at all.
    EXPECT_EQ(after.jobs_processed, before.jobs_processed);
    EXPECT_EQ(after.jobs_failed, before.jobs_failed);
}

// =============================================================================
// Consecutive multi-row INSERTs with mixed NULLs
// =============================================================================

TEST_F(QA_GDB297, ConsecutiveMixedInsertsAllWork) {
    exec_ok("CREATE TABLE t_cons (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");

    // First batch: 1 valid, 1 NULL
    exec_ok("INSERT INTO t_cons (id, title) VALUES (1, 'batch1'), (2, NULL)");
    // Second batch: 1 NULL, 1 valid
    exec_ok("INSERT INTO t_cons (id, title) VALUES (3, NULL), (4, 'batch2')");
    // Third batch: all valid
    exec_ok("INSERT INTO t_cons (id, title) VALUES (5, 'batch3a'), (6, 'batch3b')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec FROM t_cons ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 6u);

    EXPECT_FALSE(qr.rows[0][1].is_null()) << "row 1 should have embedding";
    EXPECT_TRUE(qr.rows[1][1].is_null()) << "row 2 should be NULL";
    EXPECT_TRUE(qr.rows[2][1].is_null()) << "row 3 should be NULL";
    EXPECT_FALSE(qr.rows[3][1].is_null()) << "row 4 should have embedding";
    EXPECT_FALSE(qr.rows[4][1].is_null()) << "row 5 should have embedding";
    EXPECT_FALSE(qr.rows[5][1].is_null()) << "row 6 should have embedding";
}

// =============================================================================
// Worker pool level: direct process_batch with mixed empty/valid jobs
// =============================================================================

/// Custom test provider that tracks which texts were actually embedded.
class TrackingProvider : public EmbeddingProvider {
public:
    explicit TrackingProvider(int32_t dim) : dim_(dim) {}

    Result<std::vector<float>> embed(const std::string& text) override {
        if (text.empty()) {
            return make_error(StatusCode::INVALID_ARGUMENT, "empty text");
        }
        std::lock_guard lock(mu_);
        embedded_texts_.push_back(text);
        return ok(std::vector<float>(static_cast<size_t>(dim_), 1.0F));
    }

    Result<std::vector<std::vector<float>>>
    embed_batch(const std::vector<std::string>& texts) override {
        std::vector<std::vector<float>> results;
        results.reserve(texts.size());
        for (const auto& t : texts) {
            auto r = embed(t);
            if (!r.has_value())
                return tl::unexpected(r.error());
            results.push_back(std::move(*r));
        }
        return ok(std::move(results));
    }

    std::string name() const override { return "tracking"; }
    size_t dimension() const override { return static_cast<size_t>(dim_); }
    Result<void> health_check() override { return ok(); }

    std::vector<std::string> embedded_texts() {
        std::lock_guard lock(mu_);
        return embedded_texts_;
    }

    int batch_count() const { return batch_count_.load(); }

private:
    int32_t dim_;
    std::mutex mu_;
    std::vector<std::string> embedded_texts_;
    std::atomic<int> batch_count_{0};
};

static EmbeddingJob make_job(table_id_t table_id,
                             int64_t row_id,
                             int32_t col_id,
                             const std::string& text,
                             const std::string& provider = "tracking",
                             int32_t dim = 4) {
    EmbeddingJob job;
    job.table_id = table_id;
    job.row_id = row_id;
    job.column_id = col_id;
    job.source_text = text;
    job.provider = provider;
    job.dimension = dim;
    job.type = EmbeddingJob::Type::INSERT;
    job.retry_count = 0;
    return job;
}

TEST(QA_GDB297_Worker, EmptyTextJobsNotSentToProvider) {
    auto provider = std::make_shared<TrackingProvider>(4);

    std::atomic<int> store_count{0};
    std::mutex stored_mu;
    std::vector<int64_t> stored_rows;

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 32});
    pool.register_provider("tracking", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t row_id, int32_t, std::span<const float>) -> Result<void> {
            std::lock_guard lock(stored_mu);
            stored_rows.push_back(row_id);
            store_count.fetch_add(1);
            return ok();
        });

    // Mix of empty and valid source texts.
    std::vector<EmbeddingJob> jobs;
    jobs.push_back(make_job(1, 10, 0, "valid one"));
    jobs.push_back(make_job(1, 20, 0, "")); // empty
    jobs.push_back(make_job(1, 30, 0, "valid two"));
    jobs.push_back(make_job(1, 40, 0, "")); // empty
    jobs.push_back(make_job(1, 50, 0, "valid three"));

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue_batch(std::move(jobs)).has_value());

    // Wait for processing.
    for (int i = 0; i < 100 && store_count.load() < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(pool.stop().has_value());

    // Only the 3 valid jobs should have been stored.
    EXPECT_EQ(store_count.load(), 3);

    // Only valid texts should have been sent to the provider.
    auto embedded = provider->embedded_texts();
    EXPECT_EQ(embedded.size(), 3u);

    // All 5 jobs (3 valid + 2 empty filtered) counted as processed, 0 failed.
    auto s = pool.stats();
    EXPECT_EQ(s.jobs_processed, 5u);
    EXPECT_EQ(s.jobs_failed, 0u);
}

TEST(QA_GDB297_Worker, AllEmptyBatchSkipsProviderEntirely) {
    auto provider = std::make_shared<TrackingProvider>(4);

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 32});
    pool.register_provider("tracking", provider);
    pool.set_store_callback(
        [](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> { return ok(); });

    std::vector<EmbeddingJob> jobs;
    jobs.push_back(make_job(1, 1, 0, ""));
    jobs.push_back(make_job(1, 2, 0, ""));
    jobs.push_back(make_job(1, 3, 0, ""));

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue_batch(std::move(jobs)).has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(pool.stop().has_value());

    // Provider should NEVER have been called.
    auto embedded = provider->embedded_texts();
    EXPECT_TRUE(embedded.empty());

    auto s = pool.stats();
    EXPECT_EQ(s.jobs_processed, 3u);
    EXPECT_EQ(s.jobs_failed, 0u);
}

TEST(QA_GDB297_Worker, PersistenceRemoveCalledForSkippedEmptyJobs) {
    auto provider = std::make_shared<TrackingProvider>(4);

    std::atomic<int> remove_count{0};
    std::mutex remove_mu;
    std::vector<int64_t> removed_rows;

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 32});
    pool.register_provider("tracking", provider);
    pool.set_store_callback(
        [](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> { return ok(); });
    pool.set_persistence(
        EmbeddingJobPersistence{.persist = [](const EmbeddingJob&) -> Result<void> { return ok(); },
                                .remove = [&](table_id_t, int64_t row_id, int32_t) -> Result<void> {
                                    std::lock_guard lock(remove_mu);
                                    removed_rows.push_back(row_id);
                                    remove_count.fetch_add(1);
                                    return ok();
                                },
                                .load = []() -> Result<std::vector<EmbeddingJob>> {
                                    return ok(std::vector<EmbeddingJob>{});
                                }});

    std::vector<EmbeddingJob> jobs;
    jobs.push_back(make_job(1, 100, 0, "valid"));
    jobs.push_back(make_job(1, 200, 0, "")); // empty — should trigger remove
    jobs.push_back(make_job(1, 300, 0, "")); // empty — should trigger remove

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue_batch(std::move(jobs)).has_value());

    for (int i = 0; i < 100 && remove_count.load() < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(pool.stop().has_value());

    // Persistence remove should be called for all 3 jobs:
    // 2 empty (filtered out) + 1 valid (after successful processing).
    std::lock_guard lock(remove_mu);
    EXPECT_GE(remove_count.load(), 3);

    // Verify the empty jobs specifically had remove called.
    bool found_200 = false;
    bool found_300 = false;
    for (auto row : removed_rows) {
        if (row == 200)
            found_200 = true;
        if (row == 300)
            found_300 = true;
    }
    EXPECT_TRUE(found_200) << "persistence remove not called for empty job row 200";
    EXPECT_TRUE(found_300) << "persistence remove not called for empty job row 300";
}

// =============================================================================
// Regression: all-valid batch still works after the fix
// =============================================================================

TEST_F(QA_GDB297, AllValidBatchStillWorks) {
    exec_ok("CREATE TABLE t_av (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");

    exec_ok("INSERT INTO t_av (id, title) VALUES "
            "(1, 'one'), (2, 'two'), (3, 'three'), (4, 'four'), (5, 'five')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec FROM t_av ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(qr.rows[i][1].is_null()) << "row " << (i + 1) << " missing embedding";
        EXPECT_EQ(qr.rows[i][1].as_embedding().size(), 4u);
    }
}

// =============================================================================
// Affected row count is correct even with mixed NULLs
// =============================================================================

TEST_F(QA_GDB297, AffectedRowCountCorrectWithMixedNulls) {
    exec_ok("CREATE TABLE t_arc (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");

    auto qr = exec_ok("INSERT INTO t_arc (id, title) VALUES "
                      "(1, 'a'), (2, NULL), (3, 'c'), (4, NULL)");
    EXPECT_EQ(qr.affected_rows, 4);
}

// =============================================================================
// Whitespace-only and empty string mixed with valid
// =============================================================================

TEST_F(QA_GDB297, WhitespaceOnlyDoesNotPoisonValidEmbeddings) {
    exec_ok("CREATE TABLE t_ws (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");

    exec_ok("INSERT INTO t_ws (id, title) VALUES "
            "(1, 'valid'), (2, '   '), (3, 'also valid')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec FROM t_ws ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u);

    // Row 1: valid source → embedding exists
    EXPECT_FALSE(qr.rows[0][1].is_null()) << "row 1 poisoned by whitespace-only row";

    // Row 2: whitespace-only → tokenizes to zero words → provider error → NULL embedding
    // But it should NOT poison row 1 or 3.
    EXPECT_TRUE(qr.rows[1][1].is_null());

    // Row 3: valid source → embedding exists
    EXPECT_FALSE(qr.rows[2][1].is_null()) << "row 3 poisoned by whitespace-only row";
}
