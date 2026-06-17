// QA adversarial tests for GDB-848
// Hardens latency/stats tracking in EmbeddingWorkerPool:
//   - total_latency_ms accumulates across multiple slow jobs
//   - jobs_processed increments exactly once per successful job
//   - failed embed_batch: latency IS recorded, jobs_processed is NOT
//   - zero-sleep provider: jobs_processed still correct, no false >0 assert
//   - concurrency: exact jobs_processed count, no lost fetch_add updates

#include "sixseven/vector/embedding_worker.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace sixseven;

// =============================================================================
// QA848SlowProvider — sleeps ~20ms per embed_batch call to produce measurable
// latency without any wall-clock upper-bound assertions.
// =============================================================================

class QA848SlowProvider : public EmbeddingProvider {
public:
    explicit QA848SlowProvider(int32_t dimension,
                               std::chrono::milliseconds sleep_ms = std::chrono::milliseconds(20))
        : dimension_(dimension), sleep_ms_(sleep_ms) {}

    Result<std::vector<float>> embed(const std::string& /*text*/) override {
        std::this_thread::sleep_for(sleep_ms_);
        embed_calls_.fetch_add(1);
        return ok(std::vector<float>(static_cast<size_t>(dimension_), 1.0F));
    }

    Result<std::vector<std::vector<float>>>
    embed_batch(const std::vector<std::string>& texts) override {
        std::this_thread::sleep_for(sleep_ms_);
        batch_calls_.fetch_add(1);
        std::vector<std::vector<float>> result;
        result.reserve(texts.size());
        for (size_t i = 0; i < texts.size(); ++i) {
            result.emplace_back(static_cast<size_t>(dimension_), 1.0F);
        }
        return ok(std::move(result));
    }

    std::string name() const override { return "qa848_slow"; }
    size_t dimension() const override { return static_cast<size_t>(dimension_); }
    Result<void> health_check() override { return ok(); }

    int embed_calls() const { return embed_calls_.load(); }
    int batch_calls() const { return batch_calls_.load(); }

private:
    int32_t dimension_;
    std::chrono::milliseconds sleep_ms_;
    std::atomic<int> embed_calls_{0};
    std::atomic<int> batch_calls_{0};
};

// =============================================================================
// QA848FailProvider — embed_batch always fails (no sleep, immediate error).
// Used to verify latency IS still recorded on the error path, and
// jobs_processed is NOT incremented.
// =============================================================================

class QA848FailProvider : public EmbeddingProvider {
public:
    explicit QA848FailProvider(int32_t dimension,
                               std::chrono::milliseconds sleep_ms = std::chrono::milliseconds(0))
        : dimension_(dimension), sleep_ms_(sleep_ms) {}

    Result<std::vector<float>> embed(const std::string& /*text*/) override {
        return make_error(StatusCode::INTERNAL_ERROR, "qa848 fail provider");
    }

    Result<std::vector<std::vector<float>>>
    embed_batch(const std::vector<std::string>& /*texts*/) override {
        if (sleep_ms_.count() > 0) {
            std::this_thread::sleep_for(sleep_ms_);
        }
        return make_error(StatusCode::INTERNAL_ERROR, "qa848 batch fail");
    }

    std::string name() const override { return "qa848_fail"; }
    size_t dimension() const override { return static_cast<size_t>(dimension_); }
    Result<void> health_check() override { return ok(); }

private:
    int32_t dimension_;
    std::chrono::milliseconds sleep_ms_;
};

// =============================================================================
// QA848FastProvider — zero sleep, returns immediately. Used to verify
// jobs_processed is correct even when total_latency_ms may be 0.
// =============================================================================

class QA848FastProvider : public EmbeddingProvider {
public:
    explicit QA848FastProvider(int32_t dimension) : dimension_(dimension) {}

    Result<std::vector<float>> embed(const std::string& /*text*/) override {
        return ok(std::vector<float>(static_cast<size_t>(dimension_), 0.5F));
    }

    Result<std::vector<std::vector<float>>>
    embed_batch(const std::vector<std::string>& texts) override {
        batch_calls_.fetch_add(1);
        std::vector<std::vector<float>> result;
        result.reserve(texts.size());
        for (size_t i = 0; i < texts.size(); ++i) {
            result.emplace_back(static_cast<size_t>(dimension_), 0.5F);
        }
        return ok(std::move(result));
    }

    std::string name() const override { return "qa848_fast"; }
    size_t dimension() const override { return static_cast<size_t>(dimension_); }
    Result<void> health_check() override { return ok(); }

    int batch_calls() const { return batch_calls_.load(); }

private:
    int32_t dimension_;
    std::atomic<int> batch_calls_{0};
};

// =============================================================================
// Helpers
// =============================================================================

static EmbeddingJob make_848_insert(table_id_t table_id,
                                    int64_t row_id,
                                    int32_t col_id,
                                    const std::string& text,
                                    const std::string& provider,
                                    int32_t dim) {
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

// Drain-based await: spin until store_count reaches expected, then stop.
// Max wait is bounded; deterministic (count-based, not wall-clock).
static void await_store_count(const std::atomic<int>& counter, int expected, int max_iters = 500) {
    for (int i = 0; i < max_iters && counter.load() < expected; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// =============================================================================
// GDB848 — Latency and stats hardening tests
// =============================================================================

// AC1: Three slow jobs → total_latency_ms > 0, jobs_processed == 3.
// Mutation kill: removing the fetch_add at embedding_worker.cpp:461 causes
// total_latency_ms to remain 0, failing EXPECT_GT.
TEST(QA_EmbeddingWorker, GDB848_ThreeSlowJobsLatencyStrictlyPositive) {
    auto provider = std::make_shared<QA848SlowProvider>(32);
    std::atomic<int> store_count{0};

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 32});
    pool.register_provider("qa848_slow", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1);
            return ok();
        });

    ASSERT_TRUE(pool.start().has_value());

    // Enqueue 3 separate jobs so each goes through its own embed_batch timing.
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(
            pool.enqueue(make_848_insert(1, i, 0, "text_" + std::to_string(i), "qa848_slow", 32))
                .has_value());
    }

    await_store_count(store_count, 3);
    ASSERT_TRUE(pool.stop().has_value());

    auto s = pool.stats();
    EXPECT_EQ(s.jobs_processed, 3u);
    // Each batch call slept ~20ms — total must be strictly > 0.
    EXPECT_GT(s.total_latency_ms, 0u);
}

// AC2: Latency is monotonic — after N slow jobs it is >= after N-1.
// Snapshot stats after job 1, then after job 2; second snapshot must be >=
// first (never decreases). Also verifies each job adds to the counter.
TEST(QA_EmbeddingWorker, GDB848_LatencyMonotonicallyNonDecreasing) {
    auto provider = std::make_shared<QA848SlowProvider>(32);
    std::atomic<int> store_count{0};

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 1});
    pool.register_provider("qa848_slow", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1);
            return ok();
        });

    ASSERT_TRUE(pool.start().has_value());

    // Job 1.
    ASSERT_TRUE(pool.enqueue(make_848_insert(1, 1, 0, "job1", "qa848_slow", 32)).has_value());
    await_store_count(store_count, 1);
    uint64_t latency_after_1 = pool.stats().total_latency_ms;
    uint64_t processed_after_1 = pool.stats().jobs_processed;

    // Job 2.
    ASSERT_TRUE(pool.enqueue(make_848_insert(1, 2, 0, "job2", "qa848_slow", 32)).has_value());
    await_store_count(store_count, 2);
    uint64_t latency_after_2 = pool.stats().total_latency_ms;
    uint64_t processed_after_2 = pool.stats().jobs_processed;

    ASSERT_TRUE(pool.stop().has_value());

    // Latency never shrinks.
    EXPECT_GE(latency_after_2, latency_after_1);
    // Each job added at least something (provider slept 20ms).
    EXPECT_GT(latency_after_2, latency_after_1);

    // jobs_processed incremented exactly once per job.
    EXPECT_EQ(processed_after_1, 1u);
    EXPECT_EQ(processed_after_2, 2u);
}

// AC3: jobs_processed increments correctly — one per successfully stored job.
// Use batch_size=1 so each job is its own embed_batch call.
TEST(QA_EmbeddingWorker, GDB848_JobsProcessedExactCountPerJob) {
    auto provider = std::make_shared<QA848FastProvider>(32);
    std::atomic<int> store_count{0};

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 1});
    pool.register_provider("qa848_fast", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1);
            return ok();
        });

    ASSERT_TRUE(pool.start().has_value());

    constexpr int N = 10;
    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(
            pool.enqueue(make_848_insert(1, i, 0, "t" + std::to_string(i), "qa848_fast", 32))
                .has_value());
    }

    await_store_count(store_count, N);
    ASSERT_TRUE(pool.stop().has_value());

    auto s = pool.stats();
    EXPECT_EQ(s.jobs_processed, static_cast<uint64_t>(N));
    EXPECT_EQ(s.jobs_failed, 0u);
}

// AC4: Failed embed_batch path — latency IS recorded (fetch_add runs before
// error check), jobs_processed is NOT incremented (retry → jobs_failed).
// max_retries=0 so jobs exhaust retries immediately.
// Mutation kill: if the latency fetch_add is inside the success branch only,
// total_latency_ms stays 0 even with a sleeping fail provider — test fails.
TEST(QA_EmbeddingWorker, GDB848_FailedBatchRecordsLatencyNotProcessed) {
    // Fail provider that sleeps 20ms before returning error — gives measurable
    // latency so we can distinguish "latency recorded" from "latency skipped".
    auto provider = std::make_shared<QA848FailProvider>(32, std::chrono::milliseconds(20));

    EmbeddingWorkerConfig cfg;
    cfg.num_workers = 1;
    cfg.max_batch_size = 32;
    cfg.max_retries = 0; // exhaust immediately → jobs_failed
    cfg.base_backoff = std::chrono::milliseconds(1);

    EmbeddingWorkerPool pool(cfg);
    pool.register_provider("qa848_fail", provider);
    // No store callback — if somehow a job "succeeds" that's also a bug.

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue(make_848_insert(1, 1, 0, "hello", "qa848_fail", 32)).has_value());

    // Wait for the job to fail. Poll jobs_failed.
    for (int i = 0; i < 200 && pool.stats().jobs_failed == 0u; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(pool.stop().has_value());

    auto s = pool.stats();
    // Job was attempted but failed — must appear in jobs_failed, not jobs_processed.
    EXPECT_EQ(s.jobs_processed, 0u);
    EXPECT_EQ(s.jobs_failed, 1u);
    // Latency should have been recorded (embed_batch ran for 20ms before erroring).
    EXPECT_GT(s.total_latency_ms, 0u);
}

// AC5: Fast/zero-sleep provider — jobs_processed is still exact; we do NOT
// assert total_latency_ms > 0 (that would be a timing-sensitive false assert
// on this legitimate-zero boundary).
TEST(QA_EmbeddingWorker, GDB848_FastProviderJobsProcessedCorrectNoLatencyAssert) {
    auto provider = std::make_shared<QA848FastProvider>(32);
    std::atomic<int> store_count{0};

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 32});
    pool.register_provider("qa848_fast", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1);
            return ok();
        });

    ASSERT_TRUE(pool.start().has_value());

    constexpr int N = 5;
    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(
            pool.enqueue(make_848_insert(1, i, 0, "fast_" + std::to_string(i), "qa848_fast", 32))
                .has_value());
    }

    await_store_count(store_count, N);
    ASSERT_TRUE(pool.stop().has_value());

    auto s = pool.stats();
    // Count must be exact — do NOT assert total_latency_ms > 0 here.
    EXPECT_EQ(s.jobs_processed, static_cast<uint64_t>(N));
    EXPECT_EQ(s.jobs_failed, 0u);
    // total_latency_ms may be 0 with a zero-sleep provider — only assert it's
    // non-negative (trivially true for uint64_t, documents the contract).
    EXPECT_GE(s.total_latency_ms, 0u);
}

// AC6: Concurrency — multiple worker threads, many concurrent slow jobs.
// Verifies jobs_processed == total submitted (no lost atomic updates) and
// total_latency_ms > 0. Uses a bounded job count + store_count drain
// (deterministic, NOT wall-clock sampling).
// Mutation kill: removing the fetch_add from jobs_processed_ causes the final
// count to be 0, failing EXPECT_EQ.
TEST(QA_EmbeddingWorker, GDB848_ConcurrentWorkersExactJobsProcessedCount) {
    auto provider = std::make_shared<QA848SlowProvider>(
        32, std::chrono::milliseconds(5)); // 5ms sleep for speed
    std::atomic<int> store_count{0};

    constexpr int NUM_WORKERS = 4;
    constexpr int NUM_JOBS = 40; // 10 per worker; each batch call = 5ms

    EmbeddingWorkerPool pool(
        {.num_workers = static_cast<uint32_t>(NUM_WORKERS), .max_batch_size = 1});
    pool.register_provider("qa848_slow", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1);
            return ok();
        });

    ASSERT_TRUE(pool.start().has_value());

    // Submit all jobs before draining — no interleaved submission/observation.
    for (int i = 0; i < NUM_JOBS; ++i) {
        ASSERT_TRUE(pool.enqueue(make_848_insert(
                                     1, i, 0, "concurrent_" + std::to_string(i), "qa848_slow", 32))
                        .has_value());
    }

    // Drain deterministically by counting store callbacks (not wall-clock).
    await_store_count(store_count, NUM_JOBS, 1000);
    ASSERT_TRUE(pool.stop().has_value());

    auto s = pool.stats();
    // Exact count — any lost atomic update fails this assertion.
    EXPECT_EQ(s.jobs_processed, static_cast<uint64_t>(NUM_JOBS));
    EXPECT_EQ(s.jobs_failed, 0u);
    // total_latency_ms > 0: each job slept 5ms; impossible to be 0.
    EXPECT_GT(s.total_latency_ms, 0u);
    // store_count must match jobs_processed — sanity cross-check.
    EXPECT_EQ(store_count.load(), NUM_JOBS);
}

// AC7: Mixed success/failure concurrent run.
// 20 good jobs + 10 bad jobs (no retries). Assert:
//   jobs_processed == 20  (only successful stores)
//   jobs_failed    == 10  (failed embed_batch, no retries)
//   total          == 30  (no double-count, no silent drop)
// Mutation kill: if a failing job accidentally increments jobs_processed, the
// count exceeds 20, failing EXPECT_EQ.
TEST(QA_EmbeddingWorker, GDB848_MixedSuccessFailureConcurrentExactCounts) {
    auto good = std::make_shared<QA848SlowProvider>(32, std::chrono::milliseconds(5));
    auto bad = std::make_shared<QA848FailProvider>(32, std::chrono::milliseconds(0));
    std::atomic<int> store_count{0};

    EmbeddingWorkerConfig cfg;
    cfg.num_workers = 2;
    cfg.max_batch_size = 1;
    cfg.max_retries = 0;
    cfg.base_backoff = std::chrono::milliseconds(1);

    EmbeddingWorkerPool pool(cfg);
    pool.register_provider("qa848_slow", good);
    pool.register_provider("qa848_fail", bad);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1);
            return ok();
        });

    ASSERT_TRUE(pool.start().has_value());

    constexpr int GOOD_JOBS = 20;
    constexpr int BAD_JOBS = 10;

    for (int i = 0; i < GOOD_JOBS; ++i) {
        ASSERT_TRUE(
            pool.enqueue(make_848_insert(1, i, 0, "good_" + std::to_string(i), "qa848_slow", 32))
                .has_value());
    }
    for (int i = 0; i < BAD_JOBS; ++i) {
        ASSERT_TRUE(
            pool.enqueue(make_848_insert(2, i, 0, "bad_" + std::to_string(i), "qa848_fail", 32))
                .has_value());
    }

    // Wait for all to settle: good jobs via store_count, bad jobs via jobs_failed.
    await_store_count(store_count, GOOD_JOBS, 1000);
    for (int i = 0; i < 500 && pool.stats().jobs_failed < static_cast<uint64_t>(BAD_JOBS); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(pool.stop().has_value());

    auto s = pool.stats();
    EXPECT_EQ(s.jobs_processed, static_cast<uint64_t>(GOOD_JOBS));
    EXPECT_EQ(s.jobs_failed, static_cast<uint64_t>(BAD_JOBS));
    // No job should be silently dropped.
    EXPECT_EQ(s.jobs_processed + s.jobs_failed, static_cast<uint64_t>(GOOD_JOBS + BAD_JOBS));
    EXPECT_EQ(store_count.load(), GOOD_JOBS);
}
