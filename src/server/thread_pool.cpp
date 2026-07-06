#include "sixseven/server/thread_pool.h"

#include "sixseven/common/logging.h"

namespace sixseven {

ThreadPool::ThreadPool(size_t num_workers) {
    workers_.reserve(num_workers);
    worker_done_.reserve(num_workers);
    for (size_t i = 0; i < num_workers; ++i) {
        worker_done_.push_back(std::make_unique<std::atomic<bool>>(false));
    }
    for (size_t i = 0; i < num_workers; ++i) {
        workers_.emplace_back([this, i] { worker_loop(i); });
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
    // task at the deadline is force-abandoned: detach it so this call
    // returns promptly instead of blocking on however long that task takes.
    // The detached thread keeps running in the background (and, if truly
    // hung, never exits) but no longer holds up shutdown. We use the
    // per-worker worker_done_ flag (set by worker_loop just before it
    // returns) to poll completion without blocking, since std::thread has no
    // join-with-timeout.
    size_t force_abandoned = 0;
    for (size_t i = 0; i < workers_.size(); ++i) {
        auto& w = workers_[i];
        if (!w.joinable()) {
            continue;
        }

        while (!worker_done_[i]->load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline_point) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        if (worker_done_[i]->load(std::memory_order_acquire)) {
            w.join();
        } else {
            SIXSEVEN_LOG_WARN("thread pool shutdown deadline reached: force-abandoning a "
                              "still-running worker thread");
            w.detach();
            ++force_abandoned;
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

void ThreadPool::worker_loop(size_t worker_index) {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] {
                return !running_.load(std::memory_order_relaxed) || !tasks_.empty();
            });
            if (!running_.load(std::memory_order_relaxed) && tasks_.empty()) {
                worker_done_[worker_index]->store(true, std::memory_order_release);
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
