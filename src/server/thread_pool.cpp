#include "sixseven/server/thread_pool.h"

#include "sixseven/common/logging.h"

namespace sixseven {

ThreadPool::ThreadPool(size_t num_workers) {
    workers_.reserve(num_workers);
    for (size_t i = 0; i < num_workers; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
    SIXSEVEN_LOG_DEBUG("thread pool started with {} workers", num_workers);
}

ThreadPool::~ThreadPool() {
    shutdown();
}

bool ThreadPool::submit(std::function<void()> task) {
    if (!running_.load(std::memory_order_relaxed)) {
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void ThreadPool::shutdown() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        return; // Already shut down.
    }
    cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) {
            w.join();
        }
    }
    SIXSEVEN_LOG_DEBUG("thread pool shut down");
}

size_t ThreadPool::pending_tasks() const {
    std::lock_guard lock(mutex_);
    return tasks_.size();
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] {
                return !running_.load(std::memory_order_relaxed) || !tasks_.empty();
            });
            if (!running_.load(std::memory_order_relaxed) && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        try {
            task();
        } catch (const std::exception& e) {
            SIXSEVEN_LOG_ERROR("thread pool task threw exception: {}", e.what());
        } catch (...) {
            SIXSEVEN_LOG_ERROR("thread pool task threw unknown exception");
        }
    }
}

} // namespace sixseven
