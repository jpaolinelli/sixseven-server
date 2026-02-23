#pragma once

#include "giodb/common/result.h"
#include "giodb/vector/embedding_column.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace giodb {

/// Abstract interface for embedding generation providers.
///
/// Implementations call external APIs (OpenAI, Cohere, local models, etc.)
/// to convert text into embedding vectors.
class EmbeddingProvider {
public:
    virtual ~EmbeddingProvider() = default;

    /// Generate an embedding for a single text input.
    [[nodiscard]] virtual Result<std::vector<float>> embed(const std::string& text) = 0;

    /// Generate embeddings for a batch of text inputs.
    /// Returns one vector per input, in the same order.
    [[nodiscard]] virtual Result<std::vector<std::vector<float>>>
    embed_batch(const std::vector<std::string>& texts) = 0;

    /// Return the provider name (e.g., "openai", "cohere").
    [[nodiscard]] virtual std::string name() const = 0;
};

/// Callback invoked when an embedding is successfully generated.
/// Parameters: (table_id, row_id, column_id, embedding_vector).
using EmbeddingStoreCallback =
    std::function<Result<void>(table_id_t, int64_t, int32_t, std::span<const float>)>;

/// Configuration for the embedding worker pool.
struct EmbeddingWorkerConfig {
    uint32_t num_workers = 2;
    uint32_t max_batch_size = 32;
    uint32_t max_retries = 5;
    std::chrono::milliseconds base_backoff{1000};
    std::chrono::milliseconds max_backoff{60000};
    uint32_t queue_warning_threshold = 1000;
};

/// Runtime metrics for the embedding worker pool.
struct EmbeddingWorkerStats {
    uint64_t queue_depth = 0;
    uint64_t jobs_processed = 0;
    uint64_t jobs_failed = 0;
    uint64_t total_latency_ms = 0;
};

/// Background worker thread pool that asynchronously generates embeddings.
///
/// Jobs are enqueued via enqueue() and processed by worker threads that call
/// registered EmbeddingProviders to generate vectors. INSERT jobs are
/// prioritized over UPDATE jobs.
///
/// Usage:
/// ```
///   EmbeddingWorkerPool pool({.num_workers = 2});
///   pool.register_provider("openai", std::make_shared<OpenAIProvider>(...));
///   pool.set_store_callback([](table_id_t t, int64_t r, int32_t c, auto vec) { ... });
///   pool.start();
///   pool.enqueue(job);
///   // ... on shutdown:
///   pool.stop();
/// ```
class EmbeddingWorkerPool {
public:
    explicit EmbeddingWorkerPool(EmbeddingWorkerConfig config = {});
    ~EmbeddingWorkerPool();

    EmbeddingWorkerPool(const EmbeddingWorkerPool&) = delete;
    EmbeddingWorkerPool& operator=(const EmbeddingWorkerPool&) = delete;
    EmbeddingWorkerPool(EmbeddingWorkerPool&&) = delete;
    EmbeddingWorkerPool& operator=(EmbeddingWorkerPool&&) = delete;

    /// Register an embedding provider by name.
    void register_provider(const std::string& name, std::shared_ptr<EmbeddingProvider> provider);

    /// Set the callback for storing generated embeddings.
    void set_store_callback(EmbeddingStoreCallback callback);

    /// Start the worker threads.
    [[nodiscard]] Result<void> start();

    /// Stop the worker threads gracefully. Finishes the current batch,
    /// then returns any remaining jobs via drain().
    [[nodiscard]] Result<void> stop();

    /// Enqueue a single embedding job.
    [[nodiscard]] Result<void> enqueue(EmbeddingJob job);

    /// Enqueue multiple embedding jobs.
    [[nodiscard]] Result<void> enqueue_batch(std::vector<EmbeddingJob> jobs);

    /// Get current worker pool statistics.
    [[nodiscard]] EmbeddingWorkerStats stats() const;

    /// Get the number of pending jobs in the queue.
    [[nodiscard]] size_t pending_count() const;

    /// Drain all pending jobs from the queue and return them.
    [[nodiscard]] std::vector<EmbeddingJob> drain();

    /// Check if the worker pool is running.
    [[nodiscard]] bool is_running() const;

private:
    /// Main loop executed by each worker thread.
    void worker_loop();

    /// Dequeue up to max_batch_size jobs, prioritizing INSERT over UPDATE.
    std::vector<EmbeddingJob> dequeue_batch();

    /// Process a batch of jobs by grouping them by provider and calling embed_batch.
    void process_batch(std::vector<EmbeddingJob>& batch);

    /// Re-enqueue a job with incremented retry count.
    void retry_job(EmbeddingJob job);

    /// Compute the backoff delay for a given retry count.
    [[nodiscard]] std::chrono::milliseconds backoff_delay(int32_t retry_count) const;

    EmbeddingWorkerConfig config_;

    // Job queue (INSERTs at front, UPDATEs at back).
    mutable std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::deque<EmbeddingJob> insert_queue_;
    std::deque<EmbeddingJob> update_queue_;

    // Provider registry.
    mutable std::mutex provider_mu_;
    std::unordered_map<std::string, std::shared_ptr<EmbeddingProvider>> providers_;

    // Store callback.
    EmbeddingStoreCallback store_callback_;

    // Worker threads.
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    // Stats.
    std::atomic<uint64_t> jobs_processed_{0};
    std::atomic<uint64_t> jobs_failed_{0};
    std::atomic<uint64_t> total_latency_ms_{0};
};

} // namespace giodb
