#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace sixseven {

/// Per-worker completion handle, lifetime-independent of the owning
/// ThreadPool. Held by a shared_ptr captured BY VALUE inside the worker's
/// thread function, so it stays alive even if the ThreadPool (and its
/// `this`) is destroyed while that worker is still running -- e.g. after a
/// bounded shutdown() force-abandons (detaches) a worker stuck past the
/// deadline. Once a worker has been abandoned, its thread function must
/// only touch this control block -- never `this`/tasks_/mutex_/cv_, all of
/// which may no longer exist.
///
/// `state` implements a CAS handshake between the worker (transitioning
/// kRunningTask -> kSafeToContinue after each task() call, right before it
/// would otherwise touch ThreadPool members again) and the bounded
/// shutdown() (transitioning kRunningTask -> kAbandoned when the deadline
/// elapses and the worker hasn't finished). Exactly one side's
/// compare_exchange wins:
///   - If the worker's CAS wins, it is safe to keep touching `this` (the
///     shutdown side observed the CAS failure and knows it must NOT detach
///     -- it falls back to actually joining, since the worker has committed
///     to a safe continuation).
///   - If shutdown's CAS wins, the worker's CAS fails and it must exit
///     immediately without touching anything but this control block; the
///     thread is then detached.
/// This closes the race where both sides might otherwise independently
/// decide the "safe" outcome for themselves and disagree.
struct ThreadPoolWorkerControl {
    enum class State : int {
        kRunningTask,    ///< Between tasks / inside worker_loop with `this` still valid.
        kSafeToContinue, ///< Worker won the CAS; free to keep using `this`.
        kAbandoned,      ///< shutdown() won the CAS; worker must not touch `this` again.
    };

    std::atomic<State> state{State::kRunningTask};

    /// Set to true by the worker just before its thread function returns
    /// (either via the normal empty-queue exit or the abandoned exit).
    std::atomic<bool> done{false};
};

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
    ///     takes to finish.
    ///
    /// Lifetime safety: each worker's post-abandonment completion signal
    /// (ThreadPoolWorkerControl) is a separate heap object owned by a
    /// shared_ptr captured BY VALUE in the worker's thread lambda, not a
    /// ThreadPool data member. Once a worker has been detached, it MUST NOT
    /// (and per worker_loop's implementation, does not) touch `this`,
    /// tasks_, mutex_, cv_, or any other ThreadPool member again -- only its
    /// own shared control block, which it keeps alive via its captured
    /// shared_ptr regardless of whether the ThreadPool itself has since been
    /// destroyed. This means the ThreadPool object (and this vector of
    /// std::thread handles) can be safely destroyed while a force-abandoned
    /// worker is still finishing its task in the background.
    ///
    /// `deadline == std::chrono::seconds(0)` means an immediate hard cap:
    /// don't wait at all -- drop the queue and force-abandon any worker
    /// that isn't already idle (0 is never interpreted as "unbounded").
    void shutdown(std::chrono::seconds deadline);

    bool is_running() const { return running_.load(std::memory_order_relaxed); }
    size_t num_workers() const { return workers_.size(); }
    size_t pending_tasks() const;

private:
    // worker_loop runs entirely with access to ThreadPool state (tasks_,
    // mutex_, cv_, running_) via `this`, EXCEPT that after it has picked up
    // and started running a task, it must not assume `this` is still valid
    // by the time that task() call returns -- the bounded shutdown() may
    // have detached this very thread while the task was running. To make
    // that safe, worker_loop takes its own shared control block by value
    // (not by reference to a ThreadPool member) and, before touching `this`
    // again after task() returns, does nothing that isn't safe to skip: it
    // sets control->done and returns immediately without re-locking mutex_
    // or looping back to check tasks_/running_ again once abandoned. Since
    // std::thread has no cancellation, the only way to guarantee this is to
    // have the shutdown path drop its reference to `this` before detaching,
    // and have worker_loop structured so that "control block signal + exit"
    // is the last thing it does after any task -- it never dereferences
    // `this` in that tail. See the .cpp for the exact ordering.
    void worker_loop(std::shared_ptr<ThreadPoolWorkerControl> control);

    std::vector<std::thread> workers_;
    // worker_controls_[i] is a shared_ptr to worker i's completion control
    // block, ALSO captured by value inside worker i's own thread lambda (see
    // worker_loop). This dual ownership is what makes the bounded shutdown()
    // safe: the control block outlives the ThreadPool if the ThreadPool is
    // destroyed while worker i is still detached and running.
    std::vector<std::shared_ptr<ThreadPoolWorkerControl>> worker_controls_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{true};
};

} // namespace sixseven
