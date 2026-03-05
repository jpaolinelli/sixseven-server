#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace sixseven {

/// Fixed-size thread pool for executing query work off the event loop thread.
class ThreadPool {
public:
    /// Create a thread pool with the given number of worker threads.
    /// Workers start immediately.
    explicit ThreadPool(size_t num_workers);

    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /// Submit a task for execution. Returns false if the pool is shut down.
    bool submit(std::function<void()> task);

    /// Signal all workers to finish and wait for them to join.
    /// Pending tasks in the queue are still executed before shutdown.
    void shutdown();

    bool is_running() const { return running_.load(std::memory_order_relaxed); }
    size_t num_workers() const { return workers_.size(); }
    size_t pending_tasks() const;

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{true};
};

} // namespace sixseven
