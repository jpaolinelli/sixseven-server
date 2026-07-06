#pragma once

#include <atomic>
#include <chrono>
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
    /// Unbounded -- waits indefinitely for all queued and in-flight tasks
    /// to complete. Used by the destructor and any caller that wants a
    /// guaranteed-complete drain (e.g. tests).
    void shutdown();

    /// Bounded (finish-then-hard-cap) shutdown: in-flight and queued tasks
    /// are allowed to run to completion, but only until `deadline` elapses
    /// from the start of this call. At the deadline:
    ///   - any task still sitting in the queue is dropped (abandoned,
    ///     never executed), and
    ///   - any worker thread still busy running a task is force-abandoned:
    ///     it is detached rather than joined, so this call returns promptly
    ///     at ~the deadline instead of blocking on however long that task
    ///     takes to finish. The detached thread finishes its current task
    ///     (or runs forever, e.g. if truly hung) in the background and is
    ///     never joined; it is the caller's responsibility to also stop
    ///     depending on pool-owned state (tasks_/mutex_ are only touched by
    ///     worker threads, so this is safe from *this's point of view, but
    ///     the abandoned task's side effects race the rest of process
    ///     shutdown).
    /// Workers that finish before the deadline (including picking up and
    /// completing further queued tasks) are joined normally.
    ///
    /// `deadline == std::chrono::seconds(0)` means an immediate hard cap:
    /// don't wait at all -- drop the queue and force-abandon any worker
    /// that isn't already idle (0 is never interpreted as "unbounded").
    void shutdown(std::chrono::seconds deadline);

    bool is_running() const { return running_.load(std::memory_order_relaxed); }
    size_t num_workers() const { return workers_.size(); }
    size_t pending_tasks() const;

private:
    void worker_loop(size_t worker_index);

    std::vector<std::thread> workers_;
    // worker_done_[i] is set to true by worker i just before its worker_loop
    // returns (thread function exit is imminent). Used by the bounded
    // shutdown() to decide, per worker and without blocking, whether it is
    // safe to join() or whether the worker must be force-abandoned (detached)
    // because it is still stuck inside a task past the deadline.
    std::vector<std::unique_ptr<std::atomic<bool>>> worker_done_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{true};
};

} // namespace sixseven
