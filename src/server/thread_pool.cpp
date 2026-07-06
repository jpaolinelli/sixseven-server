#include "sixseven/server/thread_pool.h"

#include "sixseven/common/logging.h"

namespace sixseven {

ThreadPool::ThreadPool(size_t num_workers) {
    workers_.reserve(num_workers);
    worker_controls_.reserve(num_workers);
    for (size_t i = 0; i < num_workers; ++i) {
        worker_controls_.push_back(std::make_shared<ThreadPoolWorkerControl>());
    }
    for (size_t i = 0; i < num_workers; ++i) {
        // Capture the control block BY VALUE (shared_ptr copy): this keeps
        // it alive for the lifetime of the thread even if the ThreadPool
        // (and worker_controls_[i]) is destroyed first.
        workers_.emplace_back([this, control = worker_controls_[i]] { worker_loop(control); });
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

void ThreadPool::shutdown(std::chrono::seconds deadline) {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        return; // Already shut down.
    }
    cv_.notify_all();

    auto deadline_point = std::chrono::steady_clock::now() + deadline;

    // Finish-then-hard-cap contract: give in-flight/queued tasks until
    // `deadline` to drain naturally. deadline == 0s means don't wait at all
    // (immediate hard cap), so this wait is skipped entirely in that case.
    if (deadline.count() > 0) {
        std::unique_lock lock(mutex_);
        // "Drained" is approximated as tasks_.empty(): every worker has
        // picked up its last task. A worker may still be executing that
        // task when this returns, which is fine -- it is allowed to finish
        // per the per-worker join-with-timeout below.
        cv_.wait_until(lock, deadline_point, [this] { return tasks_.empty(); });
    }

    // Abandon anything still queued at (or past) the deadline: clear it so
    // no worker starts a new task after this point.
    {
        std::lock_guard lock(mutex_);
        size_t abandoned = tasks_.size();
        if (abandoned > 0) {
            SIXSEVEN_LOG_WARN("thread pool shutdown deadline reached: abandoning {} queued task(s)",
                              abandoned);
            std::queue<std::function<void()>> empty;
            std::swap(tasks_, empty);
        }
    }
    cv_.notify_all();

    // Join each worker, but only until the (already-elapsed-or-not)
    // deadline. A worker that was idle, or finishes its in-flight task in
    // time, is joined normally. A worker still stuck inside a long-running
    // task at the deadline is force-abandoned via a CAS handshake against
    // its ThreadPoolWorkerControl::state (see the type's doc comment): we
    // try to flip kRunningTask -> kAbandoned. If that CAS succeeds, the
    // worker is guaranteed to see kAbandoned the next time it checks (right
    // after its current task() call returns) and will exit without touching
    // `this` again, so it is safe to detach. If the CAS fails, the worker
    // already won the race by transitioning itself to kSafeToContinue --
    // meaning it's about to (or already did) go back to touching `this`
    // safely -- so we must NOT detach; we fall back to a real (unbounded)
    // join for that worker instead. Either way, `this` is never touched by
    // an abandoned worker again, so no use-after-free is possible once
    // detached. We poll the per-worker `done` flag (set by worker_loop just
    // before it returns) to check completion without blocking, since
    // std::thread has no join-with-timeout.
    size_t force_abandoned = 0;
    for (size_t i = 0; i < workers_.size(); ++i) {
        auto& w = workers_[i];
        if (!w.joinable()) {
            continue;
        }
        auto& control = *worker_controls_[i];

        while (!control.done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline_point) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        if (control.done.load(std::memory_order_acquire)) {
            w.join();
            continue;
        }

        auto expected = ThreadPoolWorkerControl::State::kRunningTask;
        if (control.state.compare_exchange_strong(
                expected, ThreadPoolWorkerControl::State::kAbandoned, std::memory_order_acq_rel)) {
            // We won: the worker will observe kAbandoned and exit without
            // touching `this` again. Safe to detach.
            SIXSEVEN_LOG_WARN("thread pool shutdown deadline reached: force-abandoning a "
                              "still-running worker thread");
            w.detach();
            ++force_abandoned;
        } else {
            // The worker won the race (already transitioned itself to
            // kSafeToContinue) -- it may touch `this` again, so we cannot
            // detach. Wait it out with a real join; the finish-then-hard-cap
            // contract still holds because a worker only reaches
            // kSafeToContinue between tasks (i.e. it was not actually
            // blocked in a long-running task at that instant), so this join
            // is expected to return promptly in practice.
            w.join();
        }
    }
    if (force_abandoned > 0) {
        SIXSEVEN_LOG_WARN("thread pool bounded shutdown force-abandoned {} worker thread(s)",
                          force_abandoned);
    }
    SIXSEVEN_LOG_DEBUG("thread pool bounded shutdown complete");
}

size_t ThreadPool::pending_tasks() const {
    std::lock_guard lock(mutex_);
    return tasks_.size();
}

void ThreadPool::worker_loop(std::shared_ptr<ThreadPoolWorkerControl> control) {
    // `control` is a local shared_ptr copy (captured by value from the
    // lambda in the constructor), so it -- and the ThreadPoolWorkerControl
    // it points to -- stays alive for this entire function even if the
    // owning ThreadPool is destroyed out from under `this`. Every access to
    // ThreadPool members below (tasks_, mutex_, cv_, running_) happens only
    // in code paths reached BEFORE this worker loses the CAS handshake
    // against a competing bounded shutdown(); once that CAS is lost, this
    // function returns immediately without going near `this` again (see the
    // handshake below and ThreadPoolWorkerControl's doc comment).
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] {
                return !running_.load(std::memory_order_relaxed) || !tasks_.empty();
            });
            if (!running_.load(std::memory_order_relaxed) && tasks_.empty()) {
                control->done.store(true, std::memory_order_release);
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

        // Critical CAS handshake for use-after-free safety (see
        // ThreadPoolWorkerControl's doc comment): try to claim
        // kRunningTask -> kSafeToContinue. If this succeeds, the bounded
        // shutdown()'s competing CAS (kRunningTask -> kAbandoned) is
        // guaranteed to have failed or fail, so it will NOT detach this
        // thread -- it is safe to keep touching `this` (mutex_/cv_/tasks_/
        // running_) below. If this CAS fails, shutdown() already won and
        // will detach us: from this point on we must not touch `this`
        // again, only the shared `control` block, then exit.
        auto expected = ThreadPoolWorkerControl::State::kRunningTask;
        if (!control->state.compare_exchange_strong(expected,
                                                    ThreadPoolWorkerControl::State::kSafeToContinue,
                                                    std::memory_order_acq_rel)) {
            control->done.store(true, std::memory_order_release);
            return;
        }
        // Reset to kRunningTask for the next loop iteration's task() call.
        control->state.store(ThreadPoolWorkerControl::State::kRunningTask,
                             std::memory_order_release);
    }
}

} // namespace sixseven
