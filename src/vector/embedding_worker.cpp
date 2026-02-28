#include "giodb/vector/embedding_worker.h"

#include "giodb/common/logging.h"

#include <algorithm>
#include <chrono>

namespace giodb {

EmbeddingWorkerPool::EmbeddingWorkerPool(EmbeddingWorkerConfig config)
    : config_(std::move(config)) {}

EmbeddingWorkerPool::~EmbeddingWorkerPool() {
    if (running_.load()) {
        auto result = stop();
        if (!result.has_value()) {
            GIODB_LOG_ERROR("failed to stop embedding worker pool: {}", result.error().message);
        }
    }
}

void EmbeddingWorkerPool::register_provider(const std::string& name,
                                            std::shared_ptr<EmbeddingProvider> provider) {
    std::lock_guard lock(provider_mu_);
    providers_[name] = std::move(provider);
}

void EmbeddingWorkerPool::set_store_callback(EmbeddingStoreCallback callback) {
    store_callback_ = std::move(callback);
}

void EmbeddingWorkerPool::set_persistence(EmbeddingJobPersistence persistence) {
    persistence_ = std::move(persistence);
}

Result<void> EmbeddingWorkerPool::start() {
    if (running_.load()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "worker pool already running");
    }

    // Load persisted jobs before starting workers.
    if (persistence_.has_value() && persistence_->load) {
        auto loaded = persistence_->load();
        if (!loaded.has_value()) {
            return tl::unexpected(loaded.error());
        }
        if (!loaded->empty()) {
            std::lock_guard lock(queue_mu_);
            for (auto& job : *loaded) {
                if (job.type == EmbeddingJob::Type::INSERT) {
                    insert_queue_.push_back(std::move(job));
                } else {
                    update_queue_.push_back(std::move(job));
                }
            }
            GIODB_LOG_INFO("loaded {} persisted embedding jobs", loaded->size());
        }
    }

    running_.store(true);
    stopping_.store(false);
    workers_.reserve(config_.num_workers);

    for (uint32_t i = 0; i < config_.num_workers; ++i) {
        workers_.emplace_back([this]() { worker_loop(); });
    }

    GIODB_LOG_INFO("embedding worker pool started: workers={}", config_.num_workers);
    return ok();
}

Result<void> EmbeddingWorkerPool::stop() {
    if (!running_.load()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "worker pool not running");
    }

    stopping_.store(true);
    running_.store(false);
    queue_cv_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

    GIODB_LOG_INFO("embedding worker pool stopped: processed={}, failed={}",
                   jobs_processed_.load(),
                   jobs_failed_.load());
    return ok();
}

Result<void> EmbeddingWorkerPool::enqueue(EmbeddingJob job) {
    // Persist to durable storage before enqueuing.
    if (persistence_.has_value() && persistence_->persist) {
        auto persist_result = persistence_->persist(job);
        if (!persist_result.has_value()) {
            GIODB_LOG_WARN("failed to persist embedding job: {}", persist_result.error().message);
        }
    }

    {
        std::lock_guard lock(queue_mu_);

        auto total = insert_queue_.size() + update_queue_.size();
        if (total >= config_.queue_warning_threshold) {
            GIODB_LOG_WARN("embedding queue depth exceeds threshold: {}", total);
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
    // Persist all jobs to durable storage before enqueuing.
    if (persistence_.has_value() && persistence_->persist) {
        for (const auto& job : jobs) {
            auto persist_result = persistence_->persist(job);
            if (!persist_result.has_value()) {
                GIODB_LOG_WARN("failed to persist embedding job: {}",
                               persist_result.error().message);
            }
        }
    }

    {
        std::lock_guard lock(queue_mu_);

        for (auto& job : jobs) {
            if (job.type == EmbeddingJob::Type::INSERT) {
                insert_queue_.push_back(std::move(job));
            } else {
                update_queue_.push_back(std::move(job));
            }
        }

        auto total = insert_queue_.size() + update_queue_.size();
        if (total >= config_.queue_warning_threshold) {
            GIODB_LOG_WARN("embedding queue depth exceeds threshold: {}", total);
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
    std::lock_guard lock(queue_mu_);

    std::vector<EmbeddingJob> result;
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
                GIODB_LOG_ERROR("no provider registered for: {}", provider_name);
                jobs_failed_.fetch_add(indices.size());
                continue;
            }
            provider = it->second;
        }

        // Collect texts for batch embedding.
        std::vector<std::string> texts;
        texts.reserve(indices.size());
        for (auto idx : indices) {
            texts.push_back(batch[idx].source_text);
        }

        auto start_time = std::chrono::steady_clock::now();

        auto embeddings_result = provider->embed_batch(texts);

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
        total_latency_ms_.fetch_add(static_cast<uint64_t>(elapsed.count()));

        if (!embeddings_result.has_value()) {
            GIODB_LOG_WARN("embedding batch failed for provider {}: {}",
                           provider_name,
                           embeddings_result.error().message);

            // Retry all jobs in this provider group.
            for (auto idx : indices) {
                retry_job(std::move(batch[idx]));
            }
            continue;
        }

        auto& embeddings = *embeddings_result;
        if (embeddings.size() != indices.size()) {
            GIODB_LOG_ERROR("provider {} returned {} embeddings, expected {}",
                            provider_name,
                            embeddings.size(),
                            indices.size());
            for (auto idx : indices) {
                retry_job(std::move(batch[idx]));
            }
            continue;
        }

        // Store each embedding.
        for (size_t i = 0; i < indices.size(); ++i) {
            auto& job = batch[indices[i]];
            auto& embedding = embeddings[i];

            if (store_callback_) {
                auto store_result =
                    store_callback_(job.table_id, job.row_id, job.column_id, embedding);
                if (!store_result.has_value()) {
                    GIODB_LOG_ERROR("failed to store embedding: {}", store_result.error().message);
                    retry_job(std::move(job));
                    continue;
                }
            }

            // Remove from durable storage after successful processing.
            if (persistence_.has_value() && persistence_->remove) {
                auto remove_result = persistence_->remove(job.table_id, job.row_id, job.column_id);
                if (!remove_result.has_value()) {
                    GIODB_LOG_WARN("failed to remove persisted embedding job: {}",
                                   remove_result.error().message);
                }
            }

            jobs_processed_.fetch_add(1);
        }
    }
}

void EmbeddingWorkerPool::retry_job(EmbeddingJob job) {
    job.retry_count++;
    if (job.retry_count > static_cast<int32_t>(config_.max_retries)) {
        GIODB_LOG_ERROR("embedding job exceeded max retries: table={}, row={}, col={}",
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
    GIODB_LOG_DEBUG("retrying embedding job in {}ms: table={}, row={}, col={}, attempt={}",
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

} // namespace giodb
