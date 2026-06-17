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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace sixseven;

// =============================================================================
// End-to-end fixture (full query engine pipeline)
// =============================================================================

class QA_GDB297 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb297";
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
        return result ? std::move(*result) : QueryResult{};
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
        batch_count_.fetch_add(1, std::memory_order_relaxed);
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
                                .persist_batch = {},
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

// =============================================================================
// GDB-854: batch_count_ is incremented by embed_batch (regression guard)
// =============================================================================

// Verify that TrackingProvider::batch_count() accurately reflects the number
// of embed_batch() invocations. Enqueue 3 valid jobs in a single enqueue_batch
// call; the worker pool collects them and issues exactly one embed_batch call,
// so batch_count() must equal 1 after the pool drains.
TEST(QA_GDB297_Worker, BatchCountTracksEmbedBatchInvocations) {
    auto provider = std::make_shared<TrackingProvider>(4);

    std::atomic<int> store_count{0};

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 32});
    pool.register_provider("tracking", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1, std::memory_order_relaxed);
            return ok();
        });

    // Enqueue 3 valid jobs in a single call — the pool should service them in
    // one embed_batch invocation (max_batch_size = 32, so no chunking).
    std::vector<EmbeddingJob> jobs;
    jobs.push_back(make_job(1, 1, 0, "alpha"));
    jobs.push_back(make_job(1, 2, 0, "beta"));
    jobs.push_back(make_job(1, 3, 0, "gamma"));

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue_batch(std::move(jobs)).has_value());

    // Wait until all 3 jobs have been stored.
    for (int i = 0; i < 100 && store_count.load() < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(pool.stop().has_value());

    EXPECT_EQ(store_count.load(), 3);

    // batch_count() must equal 1: the 3 jobs arrived together and were
    // dispatched to embed_batch in a single call.  If batch_count_ were still
    // never incremented this assertion would see 0 and fail — mutation guard.
    EXPECT_EQ(provider->batch_count(), 1)
        << "embed_batch was not counted; batch_count_ is still dead code";
}

// =============================================================================
// GDB-854 adversarial: batch_count tracks REAL embed_batch invocations
// =============================================================================

// AC: N jobs fitting in ONE batch (N <= max_batch_size) → batch_count==1,
//     jobs_processed==N.  Mutation guard: removing fetch_add makes this see 0.
TEST(QA_GDB297_Worker, GDB854_SingleBatch_NJobsFitInOne) {
    auto provider = std::make_shared<TrackingProvider>(4);
    std::atomic<int> store_count{0};

    // max_batch_size=8, enqueue 8 valid jobs → exactly 1 embed_batch call.
    constexpr int N = 8;
    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = static_cast<uint32_t>(N)});
    pool.register_provider("tracking", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1, std::memory_order_relaxed);
            return ok();
        });

    std::vector<EmbeddingJob> jobs;
    for (int i = 0; i < N; ++i) {
        jobs.push_back(make_job(1, i + 1, 0, "text" + std::to_string(i)));
    }

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue_batch(std::move(jobs)).has_value());

    for (int i = 0; i < 200 && store_count.load() < N; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(pool.stop().has_value());

    EXPECT_EQ(store_count.load(), N) << "not all jobs were processed";
    EXPECT_EQ(pool.stats().jobs_processed, static_cast<uint64_t>(N));
    // All N jobs fit in one dequeue → exactly 1 embed_batch invocation.
    EXPECT_EQ(provider->batch_count(), 1)
        << "expected exactly 1 embed_batch call for " << N << " jobs with max_batch_size=" << N;
}

// AC: N jobs EXCEEDING max_batch_size → batch_count==ceil(N/max_batch_size).
// The worker dequeues at most max_batch_size jobs per iteration; each dequeue
// produces one process_batch call → one embed_batch call per provider group.
// With 1 worker and a small max_batch_size we can derive the exact count.
TEST(QA_GDB297_Worker, GDB854_MultipleBatches_ExceedsMaxBatchSize) {
    auto provider = std::make_shared<TrackingProvider>(4);
    std::atomic<int> store_count{0};

    constexpr uint32_t MAX_BATCH = 3;
    constexpr int N = 7; // ceil(7/3) == 3 embed_batch calls
    constexpr int EXPECTED_BATCHES =
        (N + static_cast<int>(MAX_BATCH) - 1) / static_cast<int>(MAX_BATCH); // == 3

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = MAX_BATCH});
    pool.register_provider("tracking", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1, std::memory_order_relaxed);
            return ok();
        });

    std::vector<EmbeddingJob> jobs;
    for (int i = 0; i < N; ++i) {
        jobs.push_back(make_job(1, i + 1, 0, "word" + std::to_string(i)));
    }

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue_batch(std::move(jobs)).has_value());

    for (int i = 0; i < 300 && store_count.load() < N; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(pool.stop().has_value());

    EXPECT_EQ(store_count.load(), N);
    EXPECT_EQ(pool.stats().jobs_processed, static_cast<uint64_t>(N));
    // batch_count must equal ceil(N / MAX_BATCH) — one embed_batch per dequeue.
    EXPECT_EQ(provider->batch_count(), EXPECTED_BATCHES)
        << "expected " << EXPECTED_BATCHES << " embed_batch calls for N=" << N
        << " with max_batch_size=" << MAX_BATCH;
}

// AC: Multiple separate enqueue_batch calls → batch_count increments
//     cumulatively per embed_batch invocation (not per enqueue call).
// We serialize the calls: enqueue first batch, drain, then enqueue second.
// This guarantees each batch of 3 goes through its own embed_batch, giving
// a total of 2 embed_batch calls regardless of timing.
TEST(QA_GDB297_Worker, GDB854_MultipleEnqueueCalls_CumulativeBatchCount) {
    auto provider = std::make_shared<TrackingProvider>(4);
    std::atomic<int> store_count{0};

    // max_batch_size=32 — all jobs in each enqueue fit in one batch.
    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 32});
    pool.register_provider("tracking", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1, std::memory_order_relaxed);
            return ok();
        });

    ASSERT_TRUE(pool.start().has_value());

    // First batch: 3 jobs.
    {
        std::vector<EmbeddingJob> jobs;
        jobs.push_back(make_job(1, 1, 0, "first-a"));
        jobs.push_back(make_job(1, 2, 0, "first-b"));
        jobs.push_back(make_job(1, 3, 0, "first-c"));
        ASSERT_TRUE(pool.enqueue_batch(std::move(jobs)).has_value());
        // Wait for all 3 to complete before the second enqueue.
        for (int i = 0; i < 200 && store_count.load() < 3; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_EQ(store_count.load(), 3) << "first batch not fully processed";
    }

    // At this point batch_count must be at least 1 (first batch done).
    EXPECT_GE(provider->batch_count(), 1);

    // Second batch: 2 jobs.
    {
        std::vector<EmbeddingJob> jobs;
        jobs.push_back(make_job(1, 4, 0, "second-a"));
        jobs.push_back(make_job(1, 5, 0, "second-b"));
        ASSERT_TRUE(pool.enqueue_batch(std::move(jobs)).has_value());
        for (int i = 0; i < 200 && store_count.load() < 5; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_EQ(store_count.load(), 5) << "second batch not fully processed";
    }

    ASSERT_TRUE(pool.stop().has_value());

    EXPECT_EQ(pool.stats().jobs_processed, 5u);
    // Each enqueue+drain cycle is one embed_batch call → cumulative total == 2.
    EXPECT_GE(provider->batch_count(), 2)
        << "batch_count must accumulate across separate enqueue calls";
}

// AC: Zero jobs / empty enqueue → batch_count unchanged (stays 0).
// enqueue_batch([]) is a no-op; the worker never calls embed_batch.
TEST(QA_GDB297_Worker, GDB854_EmptyEnqueue_BatchCountUnchanged) {
    auto provider = std::make_shared<TrackingProvider>(4);

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 32});
    pool.register_provider("tracking", provider);
    pool.set_store_callback(
        [](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> { return ok(); });

    ASSERT_TRUE(pool.start().has_value());

    // Enqueue an empty batch — must be a no-op.
    ASSERT_TRUE(pool.enqueue_batch({}).has_value());

    // Give the worker a moment to run (it should idle with nothing to do).
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_TRUE(pool.stop().has_value());

    // No jobs, no embed_batch calls.
    EXPECT_EQ(provider->batch_count(), 0)
        << "embed_batch was called despite empty enqueue — batch_count must stay 0";
    EXPECT_EQ(pool.stats().jobs_processed, 0u);
    EXPECT_EQ(pool.stats().jobs_failed, 0u);
}

// AC: Cross-check batch_count vs jobs_processed — sum of batch sizes ==
//     jobs embedded.  With all-valid jobs and max_batch_size=4, processing
//     12 jobs must yield batch_count==3 and jobs_processed==12.
// batch_count * max_batch_size >= jobs_processed (last batch may be partial).
TEST(QA_GDB297_Worker, GDB854_CrossCheck_BatchCountVsJobsProcessed) {
    auto provider = std::make_shared<TrackingProvider>(4);
    std::atomic<int> store_count{0};

    constexpr uint32_t MAX_BATCH = 4;
    constexpr int N = 12;                                             // exactly 3 full batches
    constexpr int EXPECTED_BATCHES = N / static_cast<int>(MAX_BATCH); // == 3

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = MAX_BATCH});
    pool.register_provider("tracking", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1, std::memory_order_relaxed);
            return ok();
        });

    std::vector<EmbeddingJob> jobs;
    for (int i = 0; i < N; ++i) {
        jobs.push_back(make_job(1, i + 1, 0, "item" + std::to_string(i)));
    }

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue_batch(std::move(jobs)).has_value());

    for (int i = 0; i < 400 && store_count.load() < N; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(pool.stop().has_value());

    const int bc = provider->batch_count();
    const uint64_t jp = pool.stats().jobs_processed;

    EXPECT_EQ(store_count.load(), N);
    EXPECT_EQ(jp, static_cast<uint64_t>(N));
    // Exact: 12 jobs / max_batch_size 4 == 3 embed_batch calls.
    EXPECT_EQ(bc, EXPECTED_BATCHES) << "batch_count=" << bc << " expected=" << EXPECTED_BATCHES;
    // Consistency: batch_count * max_batch_size must be >= jobs_processed
    // (the last batch may be partial, so batch_count * max_batch_size >= N).
    EXPECT_GE(static_cast<uint64_t>(bc) * static_cast<uint64_t>(MAX_BATCH), jp)
        << "batch sizes do not account for all processed jobs";
}

// AC: Concurrency — multiple workers calling embed_batch concurrently must
//     not lose atomic updates to batch_count.  With num_workers=4 and enough
//     jobs for each worker to get its own batch, batch_count must equal the
//     total number of embed_batch calls with no lost updates.
// Strategy: enqueue N*max_batch_size jobs so every worker gets at least N
// batches; drain completely; assert batch_count == jobs_processed / max_batch_size
// (since all batches are full).
TEST(QA_GDB297_Worker, GDB854_Concurrency_AtomicBatchCountNoLostUpdates) {
    auto provider = std::make_shared<TrackingProvider>(4);
    std::atomic<int> store_count{0};

    constexpr uint32_t NUM_WORKERS = 4;
    constexpr uint32_t MAX_BATCH = 5;
    // 60 jobs: 3 full batches per worker (with 4 workers).
    constexpr int N = static_cast<int>(NUM_WORKERS * MAX_BATCH * 3);

    EmbeddingWorkerPool pool({.num_workers = NUM_WORKERS, .max_batch_size = MAX_BATCH});
    pool.register_provider("tracking", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1, std::memory_order_relaxed);
            return ok();
        });

    std::vector<EmbeddingJob> jobs;
    for (int i = 0; i < N; ++i) {
        jobs.push_back(make_job(1, i + 1, 0, "concurrent" + std::to_string(i)));
    }

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue_batch(std::move(jobs)).has_value());

    // Bounded drain: wait for all N jobs (generous timeout for slow CI).
    for (int i = 0; i < 600 && store_count.load() < N; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(pool.stop().has_value());

    EXPECT_EQ(store_count.load(), N) << "not all concurrent jobs processed";

    const int bc = provider->batch_count();
    const uint64_t jp = pool.stats().jobs_processed;

    EXPECT_EQ(jp, static_cast<uint64_t>(N));

    // batch_count must account for every job: bc * MAX_BATCH >= N.
    // (Workers race, so some batches may be smaller than MAX_BATCH —
    //  batch_count could be slightly above N/MAX_BATCH, but never below.)
    EXPECT_GE(bc, 1) << "batch_count must be positive — atomic lost updates?";
    EXPECT_GE(static_cast<uint64_t>(bc) * static_cast<uint64_t>(MAX_BATCH),
              static_cast<uint64_t>(N))
        << "batch_count=" << bc << " does not account for N=" << N
        << " jobs at max_batch_size=" << MAX_BATCH << " — lost atomic update?";
    // Upper bound: bc <= N (each call handles at least 1 job).
    EXPECT_LE(bc, N) << "batch_count exceeds total job count — miscounting";
}

// AC: Mixed empty+valid jobs across multiple workers — batch_count counts
//     only REAL embed_batch invocations, not filtered-empty groups.
// Empty jobs are filtered before embed_batch is called; they must NOT inflate
// batch_count.
TEST(QA_GDB297_Worker, GDB854_MixedEmptyValid_BatchCountOnlyCountsRealCalls) {
    auto provider = std::make_shared<TrackingProvider>(4);
    std::atomic<int> store_count{0};

    // 3 valid + 3 empty, max_batch_size=6 → all arrive in one dequeue.
    // Only 1 embed_batch call (for the 3 valid jobs). Empty jobs are filtered.
    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 6});
    pool.register_provider("tracking", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1, std::memory_order_relaxed);
            return ok();
        });

    std::vector<EmbeddingJob> jobs;
    jobs.push_back(make_job(1, 1, 0, "real-a"));
    jobs.push_back(make_job(1, 2, 0, "")); // empty — filtered
    jobs.push_back(make_job(1, 3, 0, "real-b"));
    jobs.push_back(make_job(1, 4, 0, "")); // empty — filtered
    jobs.push_back(make_job(1, 5, 0, "real-c"));
    jobs.push_back(make_job(1, 6, 0, "")); // empty — filtered

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue_batch(std::move(jobs)).has_value());

    // Wait for the 3 valid store callbacks.
    for (int i = 0; i < 200 && store_count.load() < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Give a moment for the empty jobs to be counted as processed.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_TRUE(pool.stop().has_value());

    EXPECT_EQ(store_count.load(), 3) << "only 3 valid jobs should be stored";
    EXPECT_EQ(pool.stats().jobs_processed, 6u) << "all 6 counted as processed";

    // Exactly 1 embed_batch call — empty jobs must not produce an extra call.
    EXPECT_EQ(provider->batch_count(), 1)
        << "batch_count must be 1 (empty jobs are filtered, not batched separately)";
}

// AC: All-empty batch → batch_count stays 0. The worker filters all jobs
//     before calling embed_batch; the provider must never be invoked.
TEST(QA_GDB297_Worker, GDB854_AllEmptyJobs_BatchCountStaysZero) {
    auto provider = std::make_shared<TrackingProvider>(4);

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 32});
    pool.register_provider("tracking", provider);
    pool.set_store_callback(
        [](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> { return ok(); });

    std::vector<EmbeddingJob> jobs;
    for (int i = 0; i < 5; ++i) {
        jobs.push_back(make_job(1, i + 1, 0, "")); // all empty
    }

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue_batch(std::move(jobs)).has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(pool.stop().has_value());

    // Provider never called when all source texts are empty.
    EXPECT_EQ(provider->batch_count(), 0) << "embed_batch must not be called for all-empty jobs";
    EXPECT_EQ(pool.stats().jobs_processed, 5u);
    EXPECT_EQ(pool.stats().jobs_failed, 0u);
}

// AC: Two providers in the same dequeued batch → two embed_batch calls
//     (one per provider), so batch_count on each provider == 1.
// This validates that batch_count tracks per-provider embed_batch calls, not
// per-batch-dequeue calls.
TEST(QA_GDB297_Worker, GDB854_TwoProviders_EachGetsSeparateEmbedBatchCall) {
    auto provider_a = std::make_shared<TrackingProvider>(4);
    auto provider_b = std::make_shared<TrackingProvider>(4);
    std::atomic<int> store_count{0};

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 32});
    pool.register_provider("tracking-a", provider_a);
    pool.register_provider("tracking-b", provider_b);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1, std::memory_order_relaxed);
            return ok();
        });

    // 2 jobs for provider-a, 3 jobs for provider-b — all fit in one dequeue.
    std::vector<EmbeddingJob> jobs;
    jobs.push_back(make_job(1, 1, 0, "a-one", "tracking-a"));
    jobs.push_back(make_job(1, 2, 0, "a-two", "tracking-a"));
    jobs.push_back(make_job(1, 3, 0, "b-one", "tracking-b"));
    jobs.push_back(make_job(1, 4, 0, "b-two", "tracking-b"));
    jobs.push_back(make_job(1, 5, 0, "b-three", "tracking-b"));

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue_batch(std::move(jobs)).has_value());

    for (int i = 0; i < 200 && store_count.load() < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(pool.stop().has_value());

    EXPECT_EQ(store_count.load(), 5);
    // Each provider is called exactly once (all 5 jobs fit in one dequeue).
    EXPECT_EQ(provider_a->batch_count(), 1)
        << "provider-a should have received exactly 1 embed_batch call";
    EXPECT_EQ(provider_b->batch_count(), 1)
        << "provider-b should have received exactly 1 embed_batch call";
}
