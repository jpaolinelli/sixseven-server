#pragma once

#include "sixseven/common/result.h"
#include "sixseven/vector/embedding_column.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sixseven {

// Forward declaration for lazy provider resolution.
class ProviderRegistry;

/// Abstract interface for embedding generation providers.
///
/// Implementations call external APIs (OpenAI, Ollama, local models, etc.)
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

    /// Return the provider name (e.g., "openai", "ollama", "builtin").
    [[nodiscard]] virtual std::string name() const = 0;

    /// Return the embedding dimension this provider produces.
    [[nodiscard]] virtual size_t dimension() const = 0;

    /// Validate connectivity and model availability.
    /// Returns ok() on success, or an error describing the failure.
    [[nodiscard]] virtual Result<void> health_check() = 0;
};

/// Callback invoked when an embedding is successfully generated.
/// Parameters: (table_id, row_id, column_id, embedding_vector).
using EmbeddingStoreCallback =
    std::function<Result<void>(table_id_t, int64_t, int32_t, std::span<const float>)>;

/// A single embedding store request within a batch write-back.
struct EmbeddingStoreRequest {
    table_id_t table_id = 0;
    int64_t row_id = 0;       // Packed RID (page_id << 32 | slot_id).
    int32_t column_id = 0;
    std::vector<float> embedding;
};

/// Batch callback invoked with all embeddings from a single process_batch run.
/// Returns a vector of row_ids that failed (for retry). Empty = all succeeded.
/// Preferred over the per-embedding EmbeddingStoreCallback to reduce page latch
/// contention by grouping writes per page.
using EmbeddingBatchStoreCallback =
    std::function<Result<std::vector<int64_t>>(std::vector<EmbeddingStoreRequest>)>;

/// Persistence interface for embedding job queue survival across restarts.
struct EmbeddingJobPersistence {
    /// Persist a job to durable storage (e.g., sys_embedding_jobs table).
    std::function<Result<void>(const EmbeddingJob&)> persist;
    /// Persist a batch of jobs in a single heap operation (preferred over per-job persist).
    std::function<Result<void>(const std::vector<EmbeddingJob>&)> persist_batch;
    /// Remove a completed job from durable storage.
    std::function<Result<void>(table_id_t, int64_t, int32_t)> remove;
    /// Load all pending jobs from durable storage on startup.
    std::function<Result<std::vector<EmbeddingJob>>()> load;
};

/// Configuration for the embedding worker pool.
struct EmbeddingWorkerConfig {
    // Default worker count. Production deployments should override this from
    // the server config — e.g. `std::max(4u, std::thread::hardware_concurrency() / 2)`.
    // Raised from 2 in Round 2 because at >10M rows a 2-worker pool cannot
    // keep up with ingest rate and the queue grows unboundedly.
    uint32_t num_workers = 4;
    uint32_t max_batch_size = 32;
    uint32_t max_retries = 5;
    std::chrono::milliseconds base_backoff{1000};
    std::chrono::milliseconds max_backoff{60000};
    uint32_t queue_warning_threshold = 1000;

    // Maximum number of pending jobs in the in-memory queue. When the queue
    // is at capacity, enqueue() and enqueue_batch() block the producer until
    // workers drain enough jobs to make room. This converts the embedding
    // backlog from an unbounded-memory failure into back-pressure that
    // propagates up to the INSERT path — at steady state, ingest runs at
    // the embedder's throughput, not ahead of it.
    //
    // Set to 0 to disable back-pressure (unbounded queue, pre-Round-2
    // behavior). Not recommended except for tests.
    uint32_t queue_max_size = 100000;
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

    /// Set a provider registry for lazy resolution of unknown providers.
    /// When a job references an unregistered provider, the pool will attempt
    /// to resolve it via the registry before failing.
    void set_provider_registry(ProviderRegistry* registry);

    /// Set the callback for storing generated embeddings.
    void set_store_callback(EmbeddingStoreCallback callback);

    /// Set the batch store callback. When set, this is preferred over the
    /// per-embedding store_callback_ for reduced page latch contention.
    void set_batch_store_callback(EmbeddingBatchStoreCallback callback);

    /// Set the persistence interface for job queue durability.
    /// When set, jobs are persisted on enqueue and removed on completion.
    void set_persistence(EmbeddingJobPersistence persistence);

    /// Start the worker threads. If persistence is configured, loads
    /// pending jobs from durable storage before starting workers.
    [[nodiscard]] Result<void> start();

    /// Stop the worker threads gracefully. Finishes the current batch,
    /// then returns any remaining jobs via drain().
    [[nodiscard]] Result<void> stop();

    /// Enqueue a single embedding job.
    [[nodiscard]] Result<void> enqueue(EmbeddingJob job);

    /// Enqueue multiple embedding jobs.
    [[nodiscard]] Result<void> enqueue_batch(std::vector<EmbeddingJob> jobs);

    /// Try to enqueue multiple embedding jobs with a timeout. Returns an error
    /// if the queue remains full after the deadline expires, rather than
    /// blocking indefinitely. Use this from query execution paths to avoid
    /// starving the thread pool.
    [[nodiscard]] Result<void> try_enqueue_batch(
        std::vector<EmbeddingJob> jobs,
        std::chrono::milliseconds timeout);

    /// Get current worker pool statistics.
    [[nodiscard]] EmbeddingWorkerStats stats() const;

    /// Get the number of pending jobs in the queue.
    [[nodiscard]] size_t pending_count() const;

    /// Drain all pending jobs from the queue and return them.
    [[nodiscard]] std::vector<EmbeddingJob> drain();

    /// Check if the worker pool is running.
    [[nodiscard]] bool is_running() const;

    /// Compute the exponential backoff delay (base * 2^(retry-1), capped at
    /// max_backoff) for a given retry count. Exposed for unit testing of the
    /// backoff schedule; also used internally by retry_job().
    [[nodiscard]] std::chrono::milliseconds backoff_delay(int32_t retry_count) const;

private:
    /// Main loop executed by each worker thread.
    void worker_loop();

    /// Dequeue up to max_batch_size jobs, prioritizing INSERT over UPDATE.
    std::vector<EmbeddingJob> dequeue_batch();

    /// Process a batch of jobs by grouping them by provider and calling embed_batch.
    void process_batch(std::vector<EmbeddingJob>& batch);

    /// Re-enqueue a job with incremented retry count.
    void retry_job(EmbeddingJob job);

    /// Background recovery loop that re-enqueues persisted jobs when the
    /// in-memory queue is empty. Runs in recovery_thread_.
    void recovery_loop();

    EmbeddingWorkerConfig config_;

    // Job queue (INSERTs at front, UPDATEs at back).
    mutable std::mutex queue_mu_;
    // Signaled when new jobs arrive; workers wait on this in dequeue_batch.
    std::condition_variable queue_cv_;
    // Signaled when workers drain jobs. Producers blocked on back-pressure
    // in enqueue() / enqueue_batch() wait on this for room to open up.
    std::condition_variable space_cv_;
    std::deque<EmbeddingJob> insert_queue_;
    std::deque<EmbeddingJob> update_queue_;

    // Provider registry.
    mutable std::mutex provider_mu_;
    std::unordered_map<std::string, std::shared_ptr<EmbeddingProvider>> providers_;
    ProviderRegistry* provider_registry_ = nullptr;

    // Store callbacks.
    EmbeddingStoreCallback store_callback_;
    EmbeddingBatchStoreCallback batch_store_callback_;

    // Optional persistence for job queue durability across restarts.
    std::optional<EmbeddingJobPersistence> persistence_;

    // Worker threads.
    std::vector<std::thread> workers_;
    std::thread recovery_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    // Stats.
    std::atomic<uint64_t> jobs_processed_{0};
    std::atomic<uint64_t> jobs_failed_{0};
    std::atomic<uint64_t> total_latency_ms_{0};
};

} // namespace sixseven
