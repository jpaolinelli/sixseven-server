#include "sixseven/vector/embedding_worker.h"

#include "sixseven/common/logging.h"
#include "sixseven/vector/provider_registry.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <unordered_set>

namespace sixseven {

EmbeddingWorkerPool::EmbeddingWorkerPool(EmbeddingWorkerConfig config)
    : config_(std::move(config)) {}

EmbeddingWorkerPool::~EmbeddingWorkerPool() {
    if (running_.load()) {
        auto result = stop();
        if (!result.has_value()) {
            SIXSEVEN_LOG_ERROR("failed to stop embedding worker pool: {}", result.error().message);
        }
    }
}

void EmbeddingWorkerPool::register_provider(const std::string& name,
                                            std::shared_ptr<EmbeddingProvider> provider) {
    std::lock_guard lock(provider_mu_);
    providers_[name] = std::move(provider);
}

void EmbeddingWorkerPool::set_provider_registry(ProviderRegistry* registry) {
    provider_registry_ = registry;
}

void EmbeddingWorkerPool::set_store_callback(EmbeddingStoreCallback callback) {
    store_callback_ = std::move(callback);
}

void EmbeddingWorkerPool::set_batch_store_callback(EmbeddingBatchStoreCallback callback) {
    batch_store_callback_ = std::move(callback);
}

void EmbeddingWorkerPool::set_persistence(EmbeddingJobPersistence persistence) {
    persistence_ = std::move(persistence);
}

Result<void> EmbeddingWorkerPool::start() {
    if (running_.load()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "worker pool already running");
    }

    // Load persisted jobs before starting workers.  A load failure is
    // non-fatal — we log the error and start workers anyway so that new
    // jobs from INSERT can still be processed.
    if (persistence_.has_value() && persistence_->load) {
        auto loaded = persistence_->load();
        if (!loaded.has_value()) {
            SIXSEVEN_LOG_WARN("failed to load persisted embedding jobs (non-fatal): {}",
                           loaded.error().message);
        } else if (!loaded->empty()) {
            std::lock_guard lock(queue_mu_);
            for (auto& job : *loaded) {
                if (job.type == EmbeddingJob::Type::INSERT) {
                    insert_queue_.push_back(std::move(job));
                } else {
                    update_queue_.push_back(std::move(job));
                }
            }
            SIXSEVEN_LOG_INFO("loaded {} persisted embedding jobs", loaded->size());
        }
    }

    running_.store(true);
    stopping_.store(false);
    workers_.reserve(config_.num_workers);

    for (uint32_t i = 0; i < config_.num_workers; ++i) {
        workers_.emplace_back([this]() { worker_loop(); });
    }

    // Launch the background recovery thread if persistence is configured.
    if (persistence_.has_value() && persistence_->load) {
        recovery_thread_ = std::thread([this]() { recovery_loop(); });
    }

    SIXSEVEN_LOG_INFO("embedding worker pool started: workers={}", config_.num_workers);
    return ok();
}

Result<void> EmbeddingWorkerPool::stop() {
    if (!running_.load()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "worker pool not running");
    }

    stopping_.store(true);
    running_.store(false);
    queue_cv_.notify_all();
    // Wake any producers blocked in enqueue() back-pressure so they can
    // observe stopping_ and return an error rather than hang forever.
    space_cv_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

    if (recovery_thread_.joinable()) {
        recovery_thread_.join();
    }

    SIXSEVEN_LOG_INFO("embedding worker pool stopped: processed={}, failed={}",
                   jobs_processed_.load(),
                   jobs_failed_.load());
    return ok();
}

Result<void> EmbeddingWorkerPool::enqueue(EmbeddingJob job) {
    // Persist to durable storage before enqueuing.
    if (persistence_.has_value() && persistence_->persist) {
        auto persist_result = persistence_->persist(job);
        if (!persist_result.has_value()) {
            SIXSEVEN_LOG_WARN("failed to persist embedding job: {}", persist_result.error().message);
        }
    }

    {
        std::unique_lock lock(queue_mu_);

        // Back-pressure: if the queue is at capacity, block until workers
        // drain enough to make room. This propagates up to the INSERT path
        // so ingest naturally runs at the embedder's sustained throughput
        // instead of accumulating an unbounded backlog.
        if (config_.queue_max_size > 0) {
            size_t current = insert_queue_.size() + update_queue_.size();
            if (current >= config_.queue_max_size) {
                SIXSEVEN_LOG_WARN(
                    "embedding queue full ({}/{}), back-pressuring producer",
                    current,
                    config_.queue_max_size);
                space_cv_.wait(lock, [this]() {
                    return stopping_.load() ||
                           (insert_queue_.size() + update_queue_.size()) <
                               config_.queue_max_size;
                });
                if (stopping_.load()) {
                    return make_error(StatusCode::INVALID_ARGUMENT,
                                      "embedding worker pool is stopping");
                }
            }
        }

        auto total = insert_queue_.size() + update_queue_.size();
        if (total >= config_.queue_warning_threshold) {
            SIXSEVEN_LOG_WARN("embedding queue depth exceeds threshold: {}", total);
        }

        if (job.type == EmbeddingJob::Type::INSERT) {
            insert_queue_.push_back(std::move(job));
        } else {
            update_queue_.push_back(std::move(job));
        }
    }
    queue_cv_.notify_one();
    return ok();
}

Result<void> EmbeddingWorkerPool::enqueue_batch(std::vector<EmbeddingJob> jobs) {
    if (jobs.empty()) {
        return ok();
    }

    // Persist all jobs to durable storage before enqueuing.  Prefer the batch
    // callback to amortize heap I/O (one insert_batch + one flush vs. N each).
    if (persistence_.has_value() && persistence_->persist_batch) {
        auto persist_result = persistence_->persist_batch(jobs);
        if (!persist_result.has_value()) {
            SIXSEVEN_LOG_WARN("failed to persist embedding job batch: {}",
                           persist_result.error().message);
        }
    } else if (persistence_.has_value() && persistence_->persist) {
        for (const auto& job : jobs) {
            auto persist_result = persistence_->persist(job);
            if (!persist_result.has_value()) {
                SIXSEVEN_LOG_WARN("failed to persist embedding job: {}",
                               persist_result.error().message);
            }
        }
    }

    {
        std::unique_lock lock(queue_mu_);

        // Back-pressure: wait until the queue has enough room for the full
        // batch, unless the queue is currently empty (in which case we
        // accept the batch regardless of its size to avoid a deadlock on
        // batches larger than queue_max_size).
        if (config_.queue_max_size > 0) {
            auto has_room = [this, &jobs]() {
                size_t current = insert_queue_.size() + update_queue_.size();
                if (current == 0) return true;
                return current + jobs.size() <= config_.queue_max_size;
            };
            if (!has_room()) {
                SIXSEVEN_LOG_WARN(
                    "embedding queue full ({}/{}), back-pressuring batch of {}",
                    insert_queue_.size() + update_queue_.size(),
                    config_.queue_max_size,
                    jobs.size());
                space_cv_.wait(lock, [this, &has_room]() {
                    return stopping_.load() || has_room();
                });
                if (stopping_.load()) {
                    return make_error(StatusCode::INVALID_ARGUMENT,
                                      "embedding worker pool is stopping");
                }
            }
        }

        for (auto& job : jobs) {
            if (job.type == EmbeddingJob::Type::INSERT) {
                insert_queue_.push_back(std::move(job));
            } else {
                update_queue_.push_back(std::move(job));
            }
        }

        auto total = insert_queue_.size() + update_queue_.size();
        if (total >= config_.queue_warning_threshold) {
            SIXSEVEN_LOG_WARN("embedding queue depth exceeds threshold: {}", total);
        }
    }
    queue_cv_.notify_all();
    return ok();
}

Result<void> EmbeddingWorkerPool::try_enqueue_batch(
    std::vector<EmbeddingJob> jobs,
    std::chrono::milliseconds timeout) {
    if (jobs.empty()) {
        return ok();
    }

    // NOTE: No persistence on the try_enqueue path.  This method is called
    // from the INSERT hot path where synchronous disk I/O is unacceptable.
    // If the in-memory queue is full, jobs are silently dropped.  The
    // background recovery sweep (recovery_loop) will detect rows with NULL
    // embeddings and re-enqueue them when the queue has capacity.

    {
        std::unique_lock lock(queue_mu_);

        if (config_.queue_max_size > 0) {
            auto has_room = [this, &jobs]() {
                size_t current = insert_queue_.size() + update_queue_.size();
                if (current == 0) return true;
                return current + jobs.size() <= config_.queue_max_size;
            };
            if (!has_room()) {
                // Wait with timeout instead of blocking indefinitely.
                bool ok_wait = space_cv_.wait_for(lock, timeout, [this, &has_room]() {
                    return stopping_.load() || has_room();
                });
                if (!ok_wait) {
                    return make_error(StatusCode::INTERNAL_ERROR,
                                      "embedding queue full; timed out waiting for space");
                }
                if (stopping_.load()) {
                    return make_error(StatusCode::INVALID_ARGUMENT,
                                      "embedding worker pool is stopping");
                }
            }
        }

        for (auto& job : jobs) {
            if (job.type == EmbeddingJob::Type::INSERT) {
                insert_queue_.push_back(std::move(job));
            } else {
                update_queue_.push_back(std::move(job));
            }
        }

        auto total = insert_queue_.size() + update_queue_.size();
        if (total >= config_.queue_warning_threshold) {
            SIXSEVEN_LOG_WARN("embedding queue depth exceeds threshold: {}", total);
        }
    }
    queue_cv_.notify_all();
    return ok();
}

EmbeddingWorkerStats EmbeddingWorkerPool::stats() const {
    EmbeddingWorkerStats s;
    {
        std::lock_guard lock(queue_mu_);
        s.queue_depth = insert_queue_.size() + update_queue_.size();
    }
    s.jobs_processed = jobs_processed_.load();
    s.jobs_failed = jobs_failed_.load();
    s.total_latency_ms = total_latency_ms_.load();
    return s;
}

size_t EmbeddingWorkerPool::pending_count() const {
    std::lock_guard lock(queue_mu_);
    return insert_queue_.size() + update_queue_.size();
}

std::vector<EmbeddingJob> EmbeddingWorkerPool::drain() {
    std::vector<EmbeddingJob> result;
    {
        std::lock_guard lock(queue_mu_);

        result.reserve(insert_queue_.size() + update_queue_.size());

        // Drain INSERT queue first (higher priority).
        for (auto& job : insert_queue_) {
            result.push_back(std::move(job));
        }
        insert_queue_.clear();

        for (auto& job : update_queue_) {
            result.push_back(std::move(job));
        }
        update_queue_.clear();
    }
    // The queue is now empty — wake any blocked producers.
    space_cv_.notify_all();

    return result;
}

bool EmbeddingWorkerPool::is_running() const {
    return running_.load();
}

void EmbeddingWorkerPool::worker_loop() {
    while (running_.load()) {
        auto batch = dequeue_batch();
        if (batch.empty()) {
            continue;
        }
        process_batch(batch);
        // Yield CPU briefly between inference batches so the event loop
        // and query executor threads get scheduled promptly.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::vector<EmbeddingJob> EmbeddingWorkerPool::dequeue_batch() {
    std::unique_lock lock(queue_mu_);

    // Wait until there are jobs or we're stopping.
    queue_cv_.wait(lock, [this]() {
        return !insert_queue_.empty() || !update_queue_.empty() || !running_.load();
    });

    if (!running_.load() && insert_queue_.empty() && update_queue_.empty()) {
        return {};
    }

    std::vector<EmbeddingJob> batch;
    batch.reserve(config_.max_batch_size);

    // Prioritize INSERT jobs.
    while (!insert_queue_.empty() && batch.size() < config_.max_batch_size) {
        batch.push_back(std::move(insert_queue_.front()));
        insert_queue_.pop_front();
    }

    // Fill remaining capacity with UPDATE jobs.
    while (!update_queue_.empty() && batch.size() < config_.max_batch_size) {
        batch.push_back(std::move(update_queue_.front()));
        update_queue_.pop_front();
    }

    // Wake any producers blocked in enqueue() back-pressure now that we
    // just freed up batch.size() slots.
    if (!batch.empty()) {
        lock.unlock();
        space_cv_.notify_all();
    }

    return batch;
}

void EmbeddingWorkerPool::process_batch(std::vector<EmbeddingJob>& batch) {
    // Group jobs by provider for efficient batching.
    std::unordered_map<std::string, std::vector<size_t>> provider_groups;
    for (size_t i = 0; i < batch.size(); ++i) {
        provider_groups[batch[i].provider].push_back(i);
    }

    for (auto& [provider_name, indices] : provider_groups) {
        std::shared_ptr<EmbeddingProvider> provider;
        {
            std::lock_guard lock(provider_mu_);
            auto it = providers_.find(provider_name);
            if (it == providers_.end()) {
                // Try lazy resolution via ProviderRegistry.
                if (provider_registry_) {
                    auto resolved = provider_registry_->resolve(provider_name);
                    if (resolved) {
                        providers_[provider_name] = *resolved;
                        provider = *resolved;
                        SIXSEVEN_LOG_INFO("lazily resolved embedding provider: {}", provider_name);
                    } else {
                        SIXSEVEN_LOG_ERROR("provider resolution failed for '{}': {}",
                                          provider_name, resolved.error().message);
                    }
                }
                if (!provider) {
                    SIXSEVEN_LOG_ERROR("no provider registered for: {} ({} jobs failed)",
                                      provider_name, indices.size());
                    jobs_failed_.fetch_add(indices.size());
                    continue;
                }
            } else {
                provider = it->second;
            }
        }

        // Partition indices: skip jobs with empty/blank source_text (defensive guard).
        // NULL or whitespace-only source means NULL embedding — complete immediately.
        std::vector<size_t> valid_indices;
        valid_indices.reserve(indices.size());
        for (auto idx : indices) {
            const auto& src = batch[idx].source_text;
            if (src.empty() || std::all_of(src.begin(), src.end(), [](unsigned char c) {
                    return std::isspace(c);
                })) {
                // Remove from persistence — nothing to embed for NULL source.
                if (persistence_.has_value() && persistence_->remove) {
                    (void)persistence_->remove(
                        batch[idx].table_id, batch[idx].row_id, batch[idx].column_id);
                }
                jobs_processed_.fetch_add(1);
            } else {
                valid_indices.push_back(idx);
            }
        }

        if (valid_indices.empty()) {
            continue;
        }

        // Collect texts for batch embedding.
        std::vector<std::string> texts;
        texts.reserve(valid_indices.size());
        for (auto idx : valid_indices) {
            texts.push_back(batch[idx].source_text);
        }

        auto start_time = std::chrono::steady_clock::now();

        auto embeddings_result = provider->embed_batch(texts);

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
        total_latency_ms_.fetch_add(static_cast<uint64_t>(elapsed.count()));

        if (!embeddings_result.has_value()) {
            SIXSEVEN_LOG_WARN("embedding batch failed for provider {}: {}",
                           provider_name,
                           embeddings_result.error().message);

            // Retry all valid jobs in this provider group.
            for (auto idx : valid_indices) {
                retry_job(std::move(batch[idx]));
            }
            continue;
        }

        auto& embeddings = *embeddings_result;
        if (embeddings.size() != valid_indices.size()) {
            SIXSEVEN_LOG_ERROR("provider {} returned {} embeddings, expected {}",
                            provider_name,
                            embeddings.size(),
                            valid_indices.size());
            for (auto idx : valid_indices) {
                retry_job(std::move(batch[idx]));
            }
            continue;
        }

        // Store embeddings — prefer the batch callback (page-grouped writes)
        // over the per-embedding callback to reduce page latch contention.
        if (batch_store_callback_) {
            // Collect all store requests for a single batch call.
            std::vector<EmbeddingStoreRequest> store_requests;
            store_requests.reserve(valid_indices.size());

            for (size_t i = 0; i < valid_indices.size(); ++i) {
                auto& job = batch[valid_indices[i]];
                store_requests.push_back(EmbeddingStoreRequest{
                    job.table_id,
                    job.row_id,
                    job.column_id,
                    std::vector<float>(embeddings[i].begin(), embeddings[i].end())});
            }

            auto batch_result = batch_store_callback_(std::move(store_requests));
            if (!batch_result.has_value()) {
                SIXSEVEN_LOG_ERROR("batch store callback FAILED: {}",
                                   batch_result.error().message);
                for (auto idx : valid_indices) {
                    retry_job(std::move(batch[idx]));
                }
            } else {
                // Build a set of failed row_ids for O(1) lookup.
                std::unordered_set<int64_t> failed_set(
                    batch_result->begin(), batch_result->end());

                for (size_t i = 0; i < valid_indices.size(); ++i) {
                    auto& job = batch[valid_indices[i]];
                    if (failed_set.count(job.row_id)) {
                        retry_job(std::move(job));
                        continue;
                    }

                    if (persistence_.has_value() && persistence_->remove) {
                        auto remove_result =
                            persistence_->remove(job.table_id, job.row_id, job.column_id);
                        if (!remove_result.has_value()) {
                            SIXSEVEN_LOG_WARN(
                                "failed to remove persisted embedding job: {}",
                                remove_result.error().message);
                        }
                    }
                    jobs_processed_.fetch_add(1);
                }
            }
        } else if (store_callback_) {
            // Fallback: per-embedding store (backward compatible).
            for (size_t i = 0; i < valid_indices.size(); ++i) {
                auto& job = batch[valid_indices[i]];
                auto& embedding = embeddings[i];

                auto store_result =
                    store_callback_(job.table_id, job.row_id, job.column_id, embedding);
                if (!store_result.has_value()) {
                    SIXSEVEN_LOG_ERROR("failed to store embedding: {}",
                                       store_result.error().message);
                    retry_job(std::move(job));
                    continue;
                }

                if (persistence_.has_value() && persistence_->remove) {
                    auto remove_result =
                        persistence_->remove(job.table_id, job.row_id, job.column_id);
                    if (!remove_result.has_value()) {
                        SIXSEVEN_LOG_WARN(
                            "failed to remove persisted embedding job: {}",
                            remove_result.error().message);
                    }
                }
                jobs_processed_.fetch_add(1);
            }
        } else {
            // No store callback configured — count as processed (no-op store).
            jobs_processed_.fetch_add(
                static_cast<uint64_t>(valid_indices.size()));
        }
    }
}

void EmbeddingWorkerPool::retry_job(EmbeddingJob job) {
    job.retry_count++;
    if (job.retry_count > static_cast<int32_t>(config_.max_retries)) {
        SIXSEVEN_LOG_ERROR("embedding job exceeded max retries: table={}, row={}, col={}",
                        job.table_id,
                        job.row_id,
                        job.column_id);
        // Remove permanently failed job from durable storage.
        if (persistence_.has_value() && persistence_->remove) {
            (void)persistence_->remove(job.table_id, job.row_id, job.column_id);
        }
        jobs_failed_.fetch_add(1);
        return;
    }

    auto delay = backoff_delay(job.retry_count);
    SIXSEVEN_LOG_DEBUG("retrying embedding job in {}ms: table={}, row={}, col={}, attempt={}",
                    delay.count(),
                    job.table_id,
                    job.row_id,
                    job.column_id,
                    job.retry_count);

    // Sleep for the backoff delay, then re-enqueue.
    // In production this should use a timer wheel instead of blocking.
    std::this_thread::sleep_for(delay);

    std::lock_guard lock(queue_mu_);
    if (job.type == EmbeddingJob::Type::INSERT) {
        insert_queue_.push_back(std::move(job));
    } else {
        update_queue_.push_back(std::move(job));
    }
    queue_cv_.notify_one();
}

std::chrono::milliseconds EmbeddingWorkerPool::backoff_delay(int32_t retry_count) const {
    // Exponential backoff: base * 2^(retry-1), capped at max.
    auto delay_ms = config_.base_backoff.count();
    for (int32_t i = 1; i < retry_count; ++i) {
        delay_ms *= 2;
        if (delay_ms > config_.max_backoff.count()) {
            delay_ms = config_.max_backoff.count();
            break;
        }
    }
    return std::chrono::milliseconds(delay_ms);
}

void EmbeddingWorkerPool::recovery_loop() {
    // Interval between recovery sweeps.  Long enough to avoid excessive disk
    // reads, short enough that dropped jobs don't languish for too long.
    static constexpr auto kRecoveryInterval = std::chrono::seconds(10);

    while (!stopping_.load()) {
        // Wait for the interval, waking early on stop.
        {
            std::unique_lock lock(queue_mu_);
            queue_cv_.wait_for(lock, kRecoveryInterval, [this]() {
                return stopping_.load();
            });
            if (stopping_.load()) {
                return;
            }

            // Only sweep when the in-memory queue is empty.  This avoids
            // duplicate detection — if the queue is empty, all persisted
            // jobs are safe to re-enqueue.
            size_t current = insert_queue_.size() + update_queue_.size();
            if (current > 0) {
                continue;
            }
        }

        // Load persisted jobs outside the lock (disk I/O).
        auto loaded = persistence_->load();
        if (!loaded.has_value()) {
            SIXSEVEN_LOG_WARN("recovery sweep: failed to load persisted jobs: {}",
                           loaded.error().message);
            continue;
        }

        if (loaded->empty()) {
            continue;
        }

        SIXSEVEN_LOG_INFO("recovery sweep: re-enqueuing {} persisted jobs", loaded->size());

        // Enqueue the loaded jobs into the in-memory queue.
        {
            std::lock_guard lock(queue_mu_);
            for (auto& job : *loaded) {
                if (job.type == EmbeddingJob::Type::INSERT) {
                    insert_queue_.push_back(std::move(job));
                } else {
                    update_queue_.push_back(std::move(job));
                }
            }
        }
        queue_cv_.notify_all();
    }
}

} // namespace sixseven
