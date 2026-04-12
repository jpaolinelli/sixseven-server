// Regression tests for EmbeddingWorkerPool back-pressure (Round 2, Fix B).
//
// Pre-Round-2, enqueue() / enqueue_batch() never blocked the producer:
// under 10M+ row ingest with a slow embedder the in-memory queue grew
// without bound, consuming tens of GB and taking 18+ hours to drain.
// Round 2 caps the queue at EmbeddingWorkerConfig::queue_max_size and
// blocks producers when the cap is reached, converting the embedding
// backlog into natural back-pressure that paces ingest to the
// embedder's sustained throughput.
//
// These tests assert:
//   1. A single enqueue() blocks when the queue is full and unblocks
//      as soon as workers drain a slot.
//   2. enqueue_batch() blocks until there is room for the full batch.
//   3. stop() wakes producers blocked on back-pressure (no deadlock on
//      shutdown).
//   4. queue_max_size = 0 disables back-pressure (legacy behavior).
//   5. The queue_depth stat never exceeds queue_max_size under sustained
//      producer pressure.

#include "sixseven/vector/embedding_worker.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace sixseven;
using namespace std::chrono_literals;

namespace {

// A slow provider that blocks embed_batch() on an atomic gate so the test
// can deterministically control embedder throughput and observe the queue
// filling up.
class GatedProvider : public EmbeddingProvider {
public:
    explicit GatedProvider(size_t dim) : dim_(dim) {}

    Result<std::vector<float>> embed(const std::string&) override {
        return make_error(StatusCode::INTERNAL_ERROR, "unused");
    }

    Result<std::vector<std::vector<float>>>
    embed_batch(const std::vector<std::string>& texts) override {
        // Wait for the test to release us.
        while (!release_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
        std::vector<std::vector<float>> out;
        out.reserve(texts.size());
        for (size_t i = 0; i < texts.size(); ++i) {
            out.emplace_back(dim_, 0.0F);
        }
        processed_.fetch_add(texts.size(), std::memory_order_relaxed);
        return ok(std::move(out));
    }

    std::string name() const override { return "gated"; }
    size_t dimension() const override { return dim_; }
    Result<void> health_check() override { return ok(); }

    void release() { release_.store(true, std::memory_order_release); }
    size_t processed() const { return processed_.load(std::memory_order_relaxed); }

private:
    size_t dim_;
    std::atomic<bool> release_{false};
    std::atomic<size_t> processed_{0};
};

EmbeddingJob make_job(int64_t row_id) {
    EmbeddingJob j;
    j.table_id = 1;
    j.row_id = row_id;
    j.column_id = 0;
    j.source_text = "txt_" + std::to_string(row_id);
    j.provider = "gated";
    j.dimension = 4;
    j.type = EmbeddingJob::Type::INSERT;
    j.retry_count = 0;
    return j;
}

} // namespace

// -- 1. Single enqueue blocks when queue is full and unblocks on drain -------

TEST(EmbeddingWorkerBackpressure, SingleEnqueueBlocksUntilSpaceIsAvailable) {
    EmbeddingWorkerConfig cfg;
    cfg.num_workers = 0; // Tight control: no worker drains in this test.
    cfg.queue_max_size = 4;
    EmbeddingWorkerPool pool(cfg);

    // Fill to capacity.
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(pool.enqueue(make_job(i)).has_value());
    }
    EXPECT_EQ(pool.pending_count(), 4u);

    // Next enqueue should block. Launch it on a thread and prove the thread
    // has not returned after a short window.
    std::atomic<bool> returned{false};
    std::thread producer([&]() {
        (void)pool.enqueue(make_job(99));
        returned.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(returned.load(std::memory_order_acquire))
        << "enqueue() did not block when queue was at queue_max_size";

    // Drain one job — this should wake the producer.
    auto drained = pool.drain();
    EXPECT_EQ(drained.size(), 4u);

    // Wait up to 1 second for the producer to unblock.
    auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline &&
           !returned.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(returned.load(std::memory_order_acquire))
        << "producer was not woken after queue drained";

    producer.join();
    EXPECT_EQ(pool.pending_count(), 1u);
}

// -- 2. enqueue_batch blocks until there is room for the full batch ---------

TEST(EmbeddingWorkerBackpressure, BatchEnqueueBlocksForFullBatchRoom) {
    EmbeddingWorkerConfig cfg;
    cfg.num_workers = 0;
    cfg.queue_max_size = 10;
    EmbeddingWorkerPool pool(cfg);

    // Fill to 8 of 10.
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(pool.enqueue(make_job(i)).has_value());
    }

    // A batch of 5 won't fit (8 + 5 > 10), must block.
    std::atomic<bool> returned{false};
    std::thread producer([&]() {
        std::vector<EmbeddingJob> batch;
        for (int i = 0; i < 5; ++i) {
            batch.push_back(make_job(100 + i));
        }
        (void)pool.enqueue_batch(std::move(batch));
        returned.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(returned.load(std::memory_order_acquire));

    // Drain — queue empty, producer can fit its batch.
    (void)pool.drain();

    auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline &&
           !returned.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(returned.load(std::memory_order_acquire));
    producer.join();
    EXPECT_EQ(pool.pending_count(), 5u);
}

// -- 3. Oversized batch on empty queue is accepted (no deadlock) ------------

TEST(EmbeddingWorkerBackpressure, OversizedBatchOnEmptyQueueIsAccepted) {
    EmbeddingWorkerConfig cfg;
    cfg.num_workers = 0;
    cfg.queue_max_size = 4;
    EmbeddingWorkerPool pool(cfg);

    // Batch larger than queue_max_size, but queue is empty — must succeed
    // rather than deadlocking the producer forever.
    std::vector<EmbeddingJob> batch;
    for (int i = 0; i < 10; ++i) {
        batch.push_back(make_job(i));
    }
    ASSERT_TRUE(pool.enqueue_batch(std::move(batch)).has_value());
    EXPECT_EQ(pool.pending_count(), 10u);
}

// -- 4. stop() wakes producers blocked on back-pressure ---------------------

TEST(EmbeddingWorkerBackpressure, StopWakesBlockedProducer) {
    EmbeddingWorkerConfig cfg;
    cfg.num_workers = 0; // No workers means the queue never drains on its own.
    cfg.queue_max_size = 2;
    EmbeddingWorkerPool pool(cfg);

    // start() must succeed even with 0 workers — this just flips running_
    // to true so stop() later can notify.
    ASSERT_TRUE(pool.start().has_value());

    // Fill to capacity. These both succeed (no back-pressure yet).
    ASSERT_TRUE(pool.enqueue(make_job(0)).has_value());
    ASSERT_TRUE(pool.enqueue(make_job(1)).has_value());

    // Next enqueue must block — queue is full and no worker will ever drain.
    std::atomic<bool> returned{false};
    std::atomic<bool> got_error{false};
    std::thread producer([&]() {
        auto r = pool.enqueue(make_job(999));
        if (!r.has_value()) got_error.store(true, std::memory_order_release);
        returned.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(50ms);
    ASSERT_FALSE(returned.load(std::memory_order_acquire))
        << "producer did not block on full queue";

    // Only stop() can rescue the producer here. This is the critical
    // invariant: a blocked producer must observe stopping_ and return.
    ASSERT_TRUE(pool.stop().has_value());

    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline &&
           !returned.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(returned.load(std::memory_order_acquire))
        << "producer was not woken by stop()";
    producer.join();
    EXPECT_TRUE(got_error.load(std::memory_order_acquire))
        << "producer woken by stop() should return an error";
}

// -- 5. queue_max_size = 0 disables back-pressure (legacy behavior) ---------

TEST(EmbeddingWorkerBackpressure, ZeroMaxSizeDisablesBackpressure) {
    EmbeddingWorkerConfig cfg;
    cfg.num_workers = 0;
    cfg.queue_max_size = 0; // unlimited
    EmbeddingWorkerPool pool(cfg);

    // 1000 enqueues must all return without blocking, even with no workers.
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(pool.enqueue(make_job(i)).has_value());
    }
    EXPECT_EQ(pool.pending_count(), 1000u);
}

// -- 6. Queue depth stays <= queue_max_size under sustained producer load --

TEST(EmbeddingWorkerBackpressure, QueueDepthBoundedUnderSustainedPressure) {
    EmbeddingWorkerConfig cfg;
    cfg.num_workers = 2;
    cfg.queue_max_size = 16;
    cfg.max_batch_size = 4;
    EmbeddingWorkerPool pool(cfg);

    auto provider = std::make_shared<GatedProvider>(4);
    pool.register_provider("gated", provider);
    ASSERT_TRUE(pool.start().has_value());
    provider->release(); // Let the embedder run.

    std::atomic<size_t> max_observed{0};
    std::atomic<bool> stop{false};

    std::thread sampler([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            size_t d = pool.pending_count();
            size_t prev = max_observed.load(std::memory_order_relaxed);
            while (d > prev &&
                   !max_observed.compare_exchange_weak(prev, d,
                                                       std::memory_order_relaxed)) {
            }
            std::this_thread::sleep_for(100us);
        }
    });

    // Sustained producer — tries to enqueue 500 jobs as fast as possible.
    // Without back-pressure, pending_count() would spike well above 16.
    for (int i = 0; i < 500; ++i) {
        ASSERT_TRUE(pool.enqueue(make_job(i)).has_value());
    }

    stop.store(true, std::memory_order_release);
    sampler.join();

    EXPECT_LE(max_observed.load(std::memory_order_relaxed), 16u)
        << "queue depth exceeded queue_max_size — back-pressure failed";

    ASSERT_TRUE(pool.stop().has_value());
}

// -- 7. try_enqueue_batch returns error on timeout -------------------------

TEST(EmbeddingWorkerBackpressure, TryEnqueueBatchTimesOutWhenQueueFull) {
    // Queue capacity = 2, no workers running → queue stays full once filled.
    EmbeddingWorkerConfig cfg;
    cfg.num_workers = 0;
    cfg.queue_max_size = 2;
    EmbeddingWorkerPool pool(cfg);
    ASSERT_TRUE(pool.start().has_value());

    // Fill the queue to capacity.
    ASSERT_TRUE(pool.enqueue(make_job(0)).has_value());
    ASSERT_TRUE(pool.enqueue(make_job(1)).has_value());

    // try_enqueue_batch with a 50ms timeout should fail promptly.
    auto start = std::chrono::steady_clock::now();
    std::vector<EmbeddingJob> batch;
    batch.push_back(make_job(2));
    auto result = pool.try_enqueue_batch(std::move(batch), 50ms);
    auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INTERNAL_ERROR);
    // Should have waited ~50ms, not blocked indefinitely.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500);

    ASSERT_TRUE(pool.stop().has_value());
}

// -- 8. try_enqueue_batch succeeds when queue drains in time ---------------

TEST(EmbeddingWorkerBackpressure, TryEnqueueBatchSucceedsWhenDrained) {
    EmbeddingWorkerConfig cfg;
    cfg.num_workers = 1;
    cfg.queue_max_size = 2;
    cfg.max_batch_size = 4;
    EmbeddingWorkerPool pool(cfg);

    auto provider = std::make_shared<GatedProvider>(4);
    pool.register_provider("gated", provider);
    provider->release(); // Let workers process immediately.
    ASSERT_TRUE(pool.start().has_value());

    // Fill the queue.
    ASSERT_TRUE(pool.enqueue(make_job(0)).has_value());
    ASSERT_TRUE(pool.enqueue(make_job(1)).has_value());

    // The worker should drain the queue within 2 seconds; try_enqueue_batch
    // should succeed before the 2s timeout.
    std::vector<EmbeddingJob> batch;
    batch.push_back(make_job(2));
    auto result = pool.try_enqueue_batch(std::move(batch), 2000ms);
    EXPECT_TRUE(result.has_value()) << result.error().message;

    ASSERT_TRUE(pool.stop().has_value());
}
