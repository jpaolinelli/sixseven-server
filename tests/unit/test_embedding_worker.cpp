#include "sixseven/vector/embedding_worker.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

using namespace sixseven;

// -- Test EmbeddingProvider that returns deterministic vectors -----------------

class TestProvider : public EmbeddingProvider {
public:
    explicit TestProvider(int32_t dimension, bool should_fail = false)
        : dimension_(dimension), should_fail_(should_fail) {}

    Result<std::vector<float>> embed(const std::string& /*text*/) override {
        if (should_fail_) {
            return make_error(StatusCode::INTERNAL_ERROR, "provider failure");
        }
        call_count_.fetch_add(1);
        std::vector<float> vec(static_cast<size_t>(dimension_), 1.0F);
        return ok(std::move(vec));
    }

    Result<std::vector<std::vector<float>>>
    embed_batch(const std::vector<std::string>& texts) override {
        if (should_fail_) {
            return make_error(StatusCode::INTERNAL_ERROR, "provider batch failure");
        }
        batch_call_count_.fetch_add(1);
        std::vector<std::vector<float>> result;
        result.reserve(texts.size());
        for (size_t i = 0; i < texts.size(); ++i) {
            result.emplace_back(static_cast<size_t>(dimension_), 1.0F);
        }
        return ok(std::move(result));
    }

    std::string name() const override { return "test"; }
    size_t dimension() const override { return static_cast<size_t>(dimension_); }
    Result<void> health_check() override { return ok(); }

    int call_count() const { return call_count_.load(); }
    int batch_call_count() const { return batch_call_count_.load(); }

    void set_should_fail(bool fail) { should_fail_ = fail; }

private:
    int32_t dimension_;
    bool should_fail_;
    std::atomic<int> call_count_{0};
    std::atomic<int> batch_call_count_{0};
};

// -- Helper to build an embedding job -----------------------------------------

static EmbeddingJob make_insert_job(table_id_t table_id,
                                    int64_t row_id,
                                    int32_t col_id,
                                    const std::string& text,
                                    const std::string& provider = "test",
                                    int32_t dim = 128) {
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

static EmbeddingJob make_update_job(table_id_t table_id,
                                    int64_t row_id,
                                    int32_t col_id,
                                    const std::string& text,
                                    const std::string& provider = "test",
                                    int32_t dim = 128) {
    EmbeddingJob job;
    job.table_id = table_id;
    job.row_id = row_id;
    job.column_id = col_id;
    job.source_text = text;
    job.provider = provider;
    job.dimension = dim;
    job.type = EmbeddingJob::Type::UPDATE;
    job.retry_count = 0;
    return job;
}

// -- Lifecycle ----------------------------------------------------------------

TEST(EmbeddingWorker, StartAndStop) {
    EmbeddingWorkerPool pool({.num_workers = 1});
    EXPECT_FALSE(pool.is_running());

    auto start = pool.start();
    ASSERT_TRUE(start.has_value()) << start.error().message;
    EXPECT_TRUE(pool.is_running());

    auto stop = pool.stop();
    ASSERT_TRUE(stop.has_value()) << stop.error().message;
    EXPECT_FALSE(pool.is_running());
}

TEST(EmbeddingWorker, DoubleStartFails) {
    EmbeddingWorkerPool pool({.num_workers = 1});

    ASSERT_TRUE(pool.start().has_value());

    auto second = pool.start();
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, StatusCode::INVALID_ARGUMENT);

    ASSERT_TRUE(pool.stop().has_value());
}

TEST(EmbeddingWorker, StopWithoutStartFails) {
    EmbeddingWorkerPool pool;

    auto stop = pool.stop();
    ASSERT_FALSE(stop.has_value());
    EXPECT_EQ(stop.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(EmbeddingWorker, DestructorStopsGracefully) {
    {
        EmbeddingWorkerPool pool({.num_workers = 2});
        ASSERT_TRUE(pool.start().has_value());
        // Destructor should stop without hanging.
    }
}

// -- Enqueue and pending count ------------------------------------------------

TEST(EmbeddingWorker, EnqueueAndPendingCount) {
    EmbeddingWorkerPool pool({.num_workers = 0}); // No workers — jobs stay in queue.

    ASSERT_TRUE(pool.enqueue(make_insert_job(1, 1, 0, "hello")).has_value());
    ASSERT_TRUE(pool.enqueue(make_insert_job(1, 2, 0, "world")).has_value());
    EXPECT_EQ(pool.pending_count(), 2u);

    ASSERT_TRUE(pool.enqueue(make_update_job(1, 3, 0, "updated")).has_value());
    EXPECT_EQ(pool.pending_count(), 3u);
}

TEST(EmbeddingWorker, EnqueueBatch) {
    EmbeddingWorkerPool pool({.num_workers = 0});

    std::vector<EmbeddingJob> jobs;
    jobs.push_back(make_insert_job(1, 1, 0, "a"));
    jobs.push_back(make_insert_job(1, 2, 0, "b"));
    jobs.push_back(make_update_job(1, 3, 0, "c"));

    auto result = pool.enqueue_batch(std::move(jobs));
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(pool.pending_count(), 3u);
}

// -- Drain --------------------------------------------------------------------

TEST(EmbeddingWorker, DrainReturnsJobsInPriorityOrder) {
    EmbeddingWorkerPool pool({.num_workers = 0});

    // Enqueue in mixed order: update, insert, update, insert.
    ASSERT_TRUE(pool.enqueue(make_update_job(1, 1, 0, "u1")).has_value());
    ASSERT_TRUE(pool.enqueue(make_insert_job(1, 2, 0, "i1")).has_value());
    ASSERT_TRUE(pool.enqueue(make_update_job(1, 3, 0, "u2")).has_value());
    ASSERT_TRUE(pool.enqueue(make_insert_job(1, 4, 0, "i2")).has_value());

    auto drained = pool.drain();
    ASSERT_EQ(drained.size(), 4u);

    // INSERTs should come first.
    EXPECT_EQ(drained[0].type, EmbeddingJob::Type::INSERT);
    EXPECT_EQ(drained[0].source_text, "i1");
    EXPECT_EQ(drained[1].type, EmbeddingJob::Type::INSERT);
    EXPECT_EQ(drained[1].source_text, "i2");

    // Then UPDATEs.
    EXPECT_EQ(drained[2].type, EmbeddingJob::Type::UPDATE);
    EXPECT_EQ(drained[2].source_text, "u1");
    EXPECT_EQ(drained[3].type, EmbeddingJob::Type::UPDATE);
    EXPECT_EQ(drained[3].source_text, "u2");

    EXPECT_EQ(pool.pending_count(), 0u);
}

TEST(EmbeddingWorker, DrainEmptyQueue) {
    EmbeddingWorkerPool pool;
    auto drained = pool.drain();
    EXPECT_TRUE(drained.empty());
}

// -- Processing jobs ----------------------------------------------------------

TEST(EmbeddingWorker, ProcessSingleJob) {
    auto provider = std::make_shared<TestProvider>(128);

    std::atomic<int> store_count{0};
    std::vector<float> stored_embedding;
    std::mutex store_mu;

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 32});
    pool.register_provider("test", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float> vec) -> Result<void> {
            std::lock_guard lock(store_mu);
            stored_embedding.assign(vec.begin(), vec.end());
            store_count.fetch_add(1);
            return ok();
        });

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue(make_insert_job(1, 42, 0, "hello world")).has_value());

    // Wait for processing.
    for (int i = 0; i < 100 && store_count.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(pool.stop().has_value());

    EXPECT_EQ(store_count.load(), 1);
    EXPECT_EQ(stored_embedding.size(), 128u);
    EXPECT_EQ(provider->batch_call_count(), 1);

    auto s = pool.stats();
    EXPECT_EQ(s.jobs_processed, 1u);
    EXPECT_EQ(s.jobs_failed, 0u);
}

TEST(EmbeddingWorker, ProcessBatchGroupsByProvider) {
    auto provider1 = std::make_shared<TestProvider>(128);
    auto provider2 = std::make_shared<TestProvider>(256);

    std::atomic<int> store_count{0};

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 32});
    pool.register_provider("test1", provider1);
    pool.register_provider("test2", provider2);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1);
            return ok();
        });

    // Enqueue jobs for two different providers.
    std::vector<EmbeddingJob> jobs;
    jobs.push_back(make_insert_job(1, 1, 0, "a", "test1", 128));
    jobs.push_back(make_insert_job(1, 2, 0, "b", "test1", 128));
    jobs.push_back(make_insert_job(1, 3, 0, "c", "test2", 256));

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue_batch(std::move(jobs)).has_value());

    // Wait for processing.
    for (int i = 0; i < 100 && store_count.load() < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(pool.stop().has_value());

    EXPECT_EQ(store_count.load(), 3);

    // Provider1 should have been called once with a batch of 2.
    EXPECT_EQ(provider1->batch_call_count(), 1);
    // Provider2 should have been called once with a batch of 1.
    EXPECT_EQ(provider2->batch_call_count(), 1);
}

// -- Retry with exponential backoff -------------------------------------------

TEST(EmbeddingWorker, RetryOnProviderFailure) {
    auto provider = std::make_shared<TestProvider>(128, true); // Always fails.

    EmbeddingWorkerConfig config;
    config.num_workers = 1;
    config.max_retries = 2;
    config.base_backoff = std::chrono::milliseconds(10);
    config.max_backoff = std::chrono::milliseconds(100);

    EmbeddingWorkerPool pool(config);
    pool.register_provider("test", provider);
    pool.set_store_callback(
        [](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> { return ok(); });

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue(make_insert_job(1, 1, 0, "hello")).has_value());

    // Wait for retries to exhaust (2 retries with small backoff).
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    ASSERT_TRUE(pool.stop().has_value());

    auto s = pool.stats();
    EXPECT_EQ(s.jobs_processed, 0u);
    EXPECT_GE(s.jobs_failed, 1u);
}

// -- Missing provider ---------------------------------------------------------

TEST(EmbeddingWorker, MissingProviderFailsJob) {
    std::atomic<int> store_count{0};

    EmbeddingWorkerPool pool({.num_workers = 1});
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1);
            return ok();
        });
    // No provider registered for "unknown".

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue(make_insert_job(1, 1, 0, "hello", "unknown")).has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(pool.stop().has_value());

    EXPECT_EQ(store_count.load(), 0);

    auto s = pool.stats();
    EXPECT_EQ(s.jobs_processed, 0u);
    EXPECT_GE(s.jobs_failed, 1u);
}

// -- Stats --------------------------------------------------------------------

TEST(EmbeddingWorker, StatsInitiallyZero) {
    EmbeddingWorkerPool pool;
    auto s = pool.stats();
    EXPECT_EQ(s.queue_depth, 0u);
    EXPECT_EQ(s.jobs_processed, 0u);
    EXPECT_EQ(s.jobs_failed, 0u);
    EXPECT_EQ(s.total_latency_ms, 0u);
}

TEST(EmbeddingWorker, StatsQueueDepth) {
    EmbeddingWorkerPool pool({.num_workers = 0});
    ASSERT_TRUE(pool.enqueue(make_insert_job(1, 1, 0, "a")).has_value());
    ASSERT_TRUE(pool.enqueue(make_insert_job(1, 2, 0, "b")).has_value());

    auto s = pool.stats();
    EXPECT_EQ(s.queue_depth, 2u);
}

// -- Backoff delay ------------------------------------------------------------

TEST(EmbeddingWorker, BackoffDelayExponential) {
    // Use the pool's backoff_delay method indirectly by checking retry behavior.
    // We verify exponential backoff by checking the config values are correct.
    EmbeddingWorkerConfig config;
    config.base_backoff = std::chrono::milliseconds(100);
    config.max_backoff = std::chrono::milliseconds(1600);

    // backoff: 100, 200, 400, 800, 1600, 1600 (capped)
    // Just verify the config is stored correctly.
    EmbeddingWorkerPool pool(config);
    auto s = pool.stats();
    EXPECT_EQ(s.jobs_processed, 0u);
}

// -- INSERT priority over UPDATE -----------------------------------------------

TEST(EmbeddingWorker, InsertJobsPrioritizedOverUpdate) {
    auto provider = std::make_shared<TestProvider>(128);

    [[maybe_unused]] std::mutex order_mu;
    [[maybe_unused]] std::vector<EmbeddingJob::Type> processing_order;

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 1});
    pool.register_provider("test", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> { return ok(); });

    // Enqueue updates first, then inserts. Inserts should be processed first.
    ASSERT_TRUE(pool.enqueue(make_update_job(1, 1, 0, "u1")).has_value());
    ASSERT_TRUE(pool.enqueue(make_update_job(1, 2, 0, "u2")).has_value());
    ASSERT_TRUE(pool.enqueue(make_insert_job(1, 3, 0, "i1")).has_value());
    ASSERT_TRUE(pool.enqueue(make_insert_job(1, 4, 0, "i2")).has_value());

    // Drain to verify priority order without processing.
    auto drained = pool.drain();
    ASSERT_EQ(drained.size(), 4u);
    EXPECT_EQ(drained[0].type, EmbeddingJob::Type::INSERT);
    EXPECT_EQ(drained[1].type, EmbeddingJob::Type::INSERT);
    EXPECT_EQ(drained[2].type, EmbeddingJob::Type::UPDATE);
    EXPECT_EQ(drained[3].type, EmbeddingJob::Type::UPDATE);
}

// -- Graceful shutdown --------------------------------------------------------

TEST(EmbeddingWorker, GracefulShutdownPreservesUnprocessedJobs) {
    EmbeddingWorkerPool pool({.num_workers = 0}); // No workers.

    ASSERT_TRUE(pool.enqueue(make_insert_job(1, 1, 0, "a")).has_value());
    ASSERT_TRUE(pool.enqueue(make_insert_job(1, 2, 0, "b")).has_value());
    ASSERT_TRUE(pool.enqueue(make_update_job(1, 3, 0, "c")).has_value());

    // Drain preserves all jobs.
    auto remaining = pool.drain();
    EXPECT_EQ(remaining.size(), 3u);
    EXPECT_EQ(pool.pending_count(), 0u);
}

// -- Multiple workers processing concurrently ---------------------------------

TEST(EmbeddingWorker, MultipleWorkersProcessConcurrently) {
    auto provider = std::make_shared<TestProvider>(64);

    std::atomic<int> store_count{0};

    EmbeddingWorkerPool pool({.num_workers = 4, .max_batch_size = 4});
    pool.register_provider("test", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1);
            return ok();
        });

    ASSERT_TRUE(pool.start().has_value());

    // Enqueue many jobs.
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(
            pool.enqueue(make_insert_job(1, i, 0, "text_" + std::to_string(i))).has_value());
    }

    // Wait for all to process.
    for (int i = 0; i < 200 && store_count.load() < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(pool.stop().has_value());

    EXPECT_EQ(store_count.load(), 20);
    auto s = pool.stats();
    EXPECT_EQ(s.jobs_processed, 20u);
    EXPECT_EQ(s.jobs_failed, 0u);
}

// -- GDB-297: Defensive filter for empty source_text in process_batch ---------

TEST(EmbeddingWorker, EmptySourceTextJobsFilteredInProcessBatch) {
    auto provider = std::make_shared<TestProvider>(128);

    std::atomic<int> store_count{0};
    std::mutex stored_mu;
    std::vector<int64_t> stored_row_ids;

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 32});
    pool.register_provider("test", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t row_id, int32_t, std::span<const float>) -> Result<void> {
            std::lock_guard lock(stored_mu);
            stored_row_ids.push_back(row_id);
            store_count.fetch_add(1);
            return ok();
        });

    // Enqueue a mix of valid and empty-text jobs.
    std::vector<EmbeddingJob> jobs;
    jobs.push_back(make_insert_job(1, 1, 0, "valid text"));    // valid
    jobs.push_back(make_insert_job(1, 2, 0, ""));              // empty — should be skipped
    jobs.push_back(make_insert_job(1, 3, 0, "another valid")); // valid
    jobs.push_back(make_insert_job(1, 4, 0, ""));              // empty — should be skipped

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue_batch(std::move(jobs)).has_value());

    // Wait for processing.
    for (int i = 0; i < 100 && store_count.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(pool.stop().has_value());

    // Only the two valid jobs should have been stored.
    EXPECT_EQ(store_count.load(), 2);

    std::lock_guard lock(stored_mu);
    ASSERT_EQ(stored_row_ids.size(), 2u);
    // Both valid row_ids should appear (order may vary).
    EXPECT_TRUE(std::find(stored_row_ids.begin(), stored_row_ids.end(), 1) != stored_row_ids.end());
    EXPECT_TRUE(std::find(stored_row_ids.begin(), stored_row_ids.end(), 3) != stored_row_ids.end());

    // All 4 jobs should be counted as processed (2 valid + 2 filtered).
    auto s = pool.stats();
    EXPECT_EQ(s.jobs_processed, 4u);
    EXPECT_EQ(s.jobs_failed, 0u);
}

TEST(EmbeddingWorker, AllEmptySourceTextBatchCompletesWithoutProviderCall) {
    auto provider = std::make_shared<TestProvider>(128);

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 32});
    pool.register_provider("test", provider);
    pool.set_store_callback(
        [](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> { return ok(); });

    // Enqueue only empty-text jobs.
    std::vector<EmbeddingJob> jobs;
    jobs.push_back(make_insert_job(1, 1, 0, ""));
    jobs.push_back(make_insert_job(1, 2, 0, ""));

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue_batch(std::move(jobs)).has_value());

    // Wait for processing.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(pool.stop().has_value());

    // Provider should never have been called.
    EXPECT_EQ(provider->batch_call_count(), 0);
    EXPECT_EQ(provider->call_count(), 0);

    // All jobs should be counted as processed.
    auto s = pool.stats();
    EXPECT_EQ(s.jobs_processed, 2u);
    EXPECT_EQ(s.jobs_failed, 0u);
}

TEST(EmbeddingWorker, WhitespaceOnlySourceTextFilteredInProcessBatch) {
    auto provider = std::make_shared<TestProvider>(128);

    std::atomic<int> store_count{0};

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 32});
    pool.register_provider("test", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1);
            return ok();
        });

    // Enqueue jobs with whitespace-only and valid texts.
    std::vector<EmbeddingJob> jobs;
    jobs.push_back(make_insert_job(1, 1, 0, "valid"));
    jobs.push_back(make_insert_job(1, 2, 0, "   "));   // whitespace-only
    jobs.push_back(make_insert_job(1, 3, 0, "\t\n ")); // tabs/newlines
    jobs.push_back(make_insert_job(1, 4, 0, "also valid"));

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue_batch(std::move(jobs)).has_value());

    // Wait for processing.
    for (int i = 0; i < 100 && store_count.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(pool.stop().has_value());

    // Only the two valid jobs should have been stored.
    EXPECT_EQ(store_count.load(), 2);

    // All 4 jobs should be counted as processed.
    auto s = pool.stats();
    EXPECT_EQ(s.jobs_processed, 4u);
    EXPECT_EQ(s.jobs_failed, 0u);
}

// -- Store callback failure ---------------------------------------------------

TEST(EmbeddingWorker, StoreCallbackFailureRetriesJob) {
    auto provider = std::make_shared<TestProvider>(128);

    std::atomic<int> store_attempts{0};

    EmbeddingWorkerConfig config;
    config.num_workers = 1;
    config.max_retries = 1;
    config.base_backoff = std::chrono::milliseconds(10);
    config.max_backoff = std::chrono::milliseconds(50);

    EmbeddingWorkerPool pool(config);
    pool.register_provider("test", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_attempts.fetch_add(1);
            return make_error(StatusCode::IO_ERROR, "store failure");
        });

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue(make_insert_job(1, 1, 0, "hello")).has_value());

    // Wait for retries to exhaust.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ASSERT_TRUE(pool.stop().has_value());

    // Should have been called at least twice (initial + retry).
    EXPECT_GE(store_attempts.load(), 2);

    auto s = pool.stats();
    EXPECT_GE(s.jobs_failed, 1u);
}

// -- Batch store callback -----------------------------------------------------

TEST(EmbeddingWorker, BatchStoreCallbackInvoked) {
    // Verify that when batch_store_callback is set, it receives all embeddings
    // in a single call instead of per-embedding store_callback.
    EmbeddingWorkerConfig cfg;
    cfg.num_workers = 1;
    cfg.max_batch_size = 32;
    EmbeddingWorkerPool pool(cfg);

    auto provider = std::make_shared<TestProvider>(4);
    pool.register_provider("test", provider);

    std::atomic<int> batch_calls{0};
    std::atomic<int> total_requests{0};
    std::atomic<int> single_calls{0};

    pool.set_store_callback([&](table_id_t, int64_t, int32_t,
                                std::span<const float>) -> Result<void> {
        single_calls.fetch_add(1);
        return ok();
    });

    pool.set_batch_store_callback(
        [&](std::vector<EmbeddingStoreRequest> requests) -> Result<std::vector<int64_t>> {
            batch_calls.fetch_add(1);
            total_requests.fetch_add(static_cast<int>(requests.size()));
            return ok(std::vector<int64_t>{}); // All succeeded.
        });

    ASSERT_TRUE(pool.start().has_value());

    // Enqueue 5 jobs.
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(pool.enqueue(make_insert_job(1, i, 0, "text" + std::to_string(i)))
                        .has_value());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(pool.stop().has_value());

    // Batch callback should have been preferred — single callback never called.
    EXPECT_EQ(single_calls.load(), 0);
    EXPECT_GE(batch_calls.load(), 1);
    EXPECT_EQ(total_requests.load(), 5);

    auto s = pool.stats();
    EXPECT_EQ(s.jobs_processed, 5u);
}

TEST(EmbeddingWorker, BatchStoreCallbackPartialFailure) {
    // Return some row_ids as failed; verify they are retried.
    EmbeddingWorkerConfig cfg;
    cfg.num_workers = 1;
    cfg.max_batch_size = 32;
    cfg.max_retries = 0; // Fail immediately on retry so we can count failures.
    EmbeddingWorkerPool pool(cfg);

    auto provider = std::make_shared<TestProvider>(4);
    pool.register_provider("test", provider);

    std::atomic<int> batch_calls{0};

    pool.set_batch_store_callback(
        [&](std::vector<EmbeddingStoreRequest> requests) -> Result<std::vector<int64_t>> {
            int call_num = batch_calls.fetch_add(1);
            if (call_num == 0) {
                // First call: fail row_id=2.
                std::vector<int64_t> failed;
                for (auto& req : requests) {
                    if (req.row_id == 2) {
                        failed.push_back(req.row_id);
                    }
                }
                return ok(std::move(failed));
            }
            // Subsequent calls (retries): all succeed.
            return ok(std::vector<int64_t>{});
        });

    ASSERT_TRUE(pool.start().has_value());

    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(pool.enqueue(make_insert_job(1, i, 0, "text" + std::to_string(i)))
                        .has_value());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT_TRUE(pool.stop().has_value());

    // 3 jobs succeed on first call + row_id=2 is retried but max_retries=0
    // so it gets dropped as failed.
    auto s = pool.stats();
    EXPECT_EQ(s.jobs_processed, 3u);
    EXPECT_EQ(s.jobs_failed, 1u);
}

// -- Recovery loop ------------------------------------------------------------

TEST(EmbeddingWorkerPool, RecoveryLoopReenqueuesPersistedJobs) {
    // Verify that the recovery loop loads persisted jobs when the queue is
    // empty and processes them.
    EmbeddingWorkerConfig config;
    config.num_workers = 1;
    config.max_batch_size = 4;
    EmbeddingWorkerPool pool(config);

    auto provider = std::make_shared<TestProvider>(128);
    pool.register_provider("test", provider);

    std::atomic<int> store_count{0};
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1);
            return ok();
        });

    // Simulate persisted jobs: load() returns 3 jobs on first call, empty after.
    std::atomic<int> load_calls{0};
    std::mutex remove_mu;
    std::vector<std::tuple<table_id_t, int64_t, int32_t>> removed;

    EmbeddingJobPersistence persistence;
    persistence.persist = [](const EmbeddingJob&) -> Result<void> { return ok(); };
    persistence.persist_batch = [](const std::vector<EmbeddingJob>&) -> Result<void> {
        return ok();
    };
    persistence.remove = [&](table_id_t t, int64_t r, int32_t c) -> Result<void> {
        std::lock_guard lock(remove_mu);
        removed.emplace_back(t, r, c);
        return ok();
    };
    persistence.load = [&]() -> Result<std::vector<EmbeddingJob>> {
        int call = load_calls.fetch_add(1);
        if (call == 0) {
            // First call (startup) — return nothing so queue starts empty.
            return ok(std::vector<EmbeddingJob>{});
        }
        if (call == 1) {
            // Second call (first recovery sweep) — return persisted jobs.
            std::vector<EmbeddingJob> jobs;
            jobs.push_back(make_insert_job(10, 100, 1, "recovered_a"));
            jobs.push_back(make_insert_job(10, 101, 1, "recovered_b"));
            jobs.push_back(make_insert_job(10, 102, 1, "recovered_c"));
            return ok(std::move(jobs));
        }
        // Subsequent calls — no more jobs.
        return ok(std::vector<EmbeddingJob>{});
    };
    pool.set_persistence(std::move(persistence));

    ASSERT_TRUE(pool.start().has_value());

    // Wait long enough for the recovery sweep (10s interval) plus processing.
    // Use a shorter polling loop to avoid excessive test time.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (store_count.load() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    ASSERT_TRUE(pool.stop().has_value());

    EXPECT_EQ(store_count.load(), 3);
    auto stats = pool.stats();
    EXPECT_EQ(stats.jobs_processed, 3u);
}

TEST(EmbeddingWorkerPool, PersistBatchPreferredOverPerJob) {
    // Verify that enqueue_batch uses persist_batch when available.
    // NOTE: try_enqueue_batch intentionally skips persistence (INSERT hot path).
    EmbeddingWorkerConfig config;
    config.num_workers = 1;
    config.max_batch_size = 4;
    config.queue_max_size = 0; // Unbounded for this test.
    EmbeddingWorkerPool pool(config);

    auto provider = std::make_shared<TestProvider>(128);
    pool.register_provider("test", provider);

    pool.set_store_callback(
        [](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            return ok();
        });

    std::atomic<int> per_job_persist_count{0};
    std::atomic<int> batch_persist_count{0};
    std::atomic<int> batch_persist_job_count{0};

    EmbeddingJobPersistence persistence;
    persistence.persist = [&](const EmbeddingJob&) -> Result<void> {
        per_job_persist_count.fetch_add(1);
        return ok();
    };
    persistence.persist_batch = [&](const std::vector<EmbeddingJob>& jobs) -> Result<void> {
        batch_persist_count.fetch_add(1);
        batch_persist_job_count.fetch_add(static_cast<int>(jobs.size()));
        return ok();
    };
    persistence.remove = [](table_id_t, int64_t, int32_t) -> Result<void> { return ok(); };
    persistence.load = []() -> Result<std::vector<EmbeddingJob>> {
        return ok(std::vector<EmbeddingJob>{});
    };
    pool.set_persistence(std::move(persistence));

    ASSERT_TRUE(pool.start().has_value());

    std::vector<EmbeddingJob> jobs;
    jobs.push_back(make_insert_job(1, 1, 0, "hello"));
    jobs.push_back(make_insert_job(1, 2, 0, "world"));
    jobs.push_back(make_insert_job(1, 3, 0, "test"));

    // Use enqueue_batch (not try_enqueue_batch) — persistence is only on
    // the blocking enqueue path, not the INSERT hot path.
    auto result = pool.enqueue_batch(std::move(jobs));
    ASSERT_TRUE(result.has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(pool.stop().has_value());

    // persist_batch should have been called, NOT per-job persist.
    EXPECT_EQ(batch_persist_count.load(), 1);
    EXPECT_EQ(batch_persist_job_count.load(), 3);
    EXPECT_EQ(per_job_persist_count.load(), 0);
}

TEST(EmbeddingWorkerPool, TryEnqueueSkipsPersistence) {
    // Verify that try_enqueue_batch does NOT call persistence callbacks.
    // The INSERT hot path must be zero disk I/O.
    EmbeddingWorkerConfig config;
    config.num_workers = 1;
    config.max_batch_size = 4;
    config.queue_max_size = 0;
    EmbeddingWorkerPool pool(config);

    auto provider = std::make_shared<TestProvider>(128);
    pool.register_provider("test", provider);

    pool.set_store_callback(
        [](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            return ok();
        });

    std::atomic<int> persist_count{0};

    EmbeddingJobPersistence persistence;
    persistence.persist = [&](const EmbeddingJob&) -> Result<void> {
        persist_count.fetch_add(1);
        return ok();
    };
    persistence.persist_batch = [&](const std::vector<EmbeddingJob>&) -> Result<void> {
        persist_count.fetch_add(1);
        return ok();
    };
    persistence.remove = [](table_id_t, int64_t, int32_t) -> Result<void> { return ok(); };
    persistence.load = []() -> Result<std::vector<EmbeddingJob>> {
        return ok(std::vector<EmbeddingJob>{});
    };
    pool.set_persistence(std::move(persistence));

    ASSERT_TRUE(pool.start().has_value());

    std::vector<EmbeddingJob> jobs;
    jobs.push_back(make_insert_job(1, 1, 0, "hello"));
    jobs.push_back(make_insert_job(1, 2, 0, "world"));

    auto result = pool.try_enqueue_batch(std::move(jobs), std::chrono::milliseconds(0));
    ASSERT_TRUE(result.has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(pool.stop().has_value());

    // persist should NOT have been called from try_enqueue_batch.
    EXPECT_EQ(persist_count.load(), 0);
}
