#include "sixseven/txn/lock_manager.h"

#include "sixseven/common/logging.h"
#include "sixseven/txn/deadlock_detector.h"

#include <algorithm>

namespace sixseven {

LockManager::LockManager(std::chrono::milliseconds lock_timeout) : lock_timeout_(lock_timeout) {}

Result<void> LockManager::lock_table(txn_id_t txn_id, table_id_t table_id, LockMode mode) {
    SIXSEVEN_LOG_DEBUG(
        "lock_table txn_id={} table_id={} mode={}", txn_id, table_id, lock_mode_name(mode));
    return acquire(txn_id, LockKey{TableLockKey{table_id}}, mode);
}

Result<void> LockManager::lock_row(
    txn_id_t txn_id, table_id_t table_id, PageId page_id, SlotId slot_id, LockMode mode) {
    // Acquire intent lock on the table first.
    LockMode intent_mode = (mode == LockMode::S) ? LockMode::IS : LockMode::IX;
    auto result = acquire(txn_id, LockKey{TableLockKey{table_id}}, intent_mode);
    if (!result) {
        return result;
    }

    SIXSEVEN_LOG_DEBUG("lock_row txn_id={} table={} page={} slot={} mode={}",
                    txn_id,
                    table_id,
                    page_id,
                    slot_id,
                    lock_mode_name(mode));
    return acquire(txn_id, LockKey{RowLockKey{table_id, page_id, slot_id}}, mode);
}

void LockManager::release_all(txn_id_t txn_id) {
    std::lock_guard lock(mu_);

    SIXSEVEN_LOG_DEBUG("release_all txn_id={}", txn_id);

    for (auto it = lock_table_.begin(); it != lock_table_.end();) {
        auto& queue = it->second;

        // Remove all requests from this transaction.
        bool had_granted = false;
        for (auto req_it = queue.requests.begin(); req_it != queue.requests.end();) {
            if (req_it->txn_id == txn_id) {
                if (req_it->granted) {
                    had_granted = true;
                }
                req_it = queue.requests.erase(req_it);
            } else {
                ++req_it;
            }
        }

        // If we released a granted lock, try to grant waiting requests.
        if (had_granted) {
            grant_waiting_locked(queue);
        }

        // Clean up empty queues.
        if (queue.requests.empty()) {
            it = lock_table_.erase(it);
        } else {
            ++it;
        }
    }
}

void LockManager::set_lock_timeout(std::chrono::milliseconds timeout) {
    std::lock_guard lock(mu_);
    lock_timeout_ = timeout;
}

std::chrono::milliseconds LockManager::lock_timeout() const {
    std::lock_guard lock(mu_);
    return lock_timeout_;
}

Result<void> LockManager::acquire(txn_id_t txn_id, const LockKey& key, LockMode mode) {
    std::unique_lock lock(mu_);

    auto& queue = lock_table_[key];

    // Check if this transaction already holds a lock on this resource.
    for (auto& req : queue.requests) {
        if (req.txn_id == txn_id && req.granted) {
            if (req.mode == mode || req.mode == LockMode::X) {
                // Already hold this lock or a stronger one.
                return ok();
            }
            if (mode == LockMode::X && req.mode != LockMode::X) {
                // Upgrade to exclusive.
                if (is_compatible_locked(queue, txn_id, mode)) {
                    req.mode = LockMode::X;
                    return ok();
                }
                // Need to wait for upgrade — fall through to blocking path.
                req.granted = false;
                req.mode = mode;
                goto wait_for_lock; // NOLINT
            }
            // For intent lock upgrades: IS -> IX.
            if (mode == LockMode::IX && req.mode == LockMode::IS) {
                if (is_compatible_locked(queue, txn_id, mode)) {
                    req.mode = LockMode::IX;
                    return ok();
                }
                req.granted = false;
                req.mode = mode;
                goto wait_for_lock; // NOLINT
            }
            // Same or weaker lock already held.
            return ok();
        }
    }

    // New lock request.
    if (is_compatible_locked(queue, txn_id, mode)) {
        // Grant immediately.
        queue.requests.push_back({txn_id, mode, /*granted=*/true, /*aborted=*/false});
        return ok();
    }

    // Must wait — add to the queue.
    queue.requests.push_back({txn_id, mode, /*granted=*/false, /*aborted=*/false});

wait_for_lock: {
    // Deadlock detection: build wait-for graph from all queues.
    txn_id_t victim = detect_deadlock_locked(txn_id);
    if (victim != invalid_txn_id) {
        // Mark the victim's waiting request as aborted across all queues.
        abort_victim_locked(victim);

        if (victim == txn_id) {
            // We are the victim — remove our request and return error.
            remove_waiting_request_locked(queue, txn_id);
            SIXSEVEN_LOG_WARN("deadlock detected, aborting txn_id={}", txn_id);
            return make_error(StatusCode::DEADLOCK, "deadlock detected, transaction aborted");
        }
        // Victim is another transaction — it will be woken up and see the aborted flag.
        // We still need to wait for our lock to be granted.
    }

    // Wait for the lock with timeout.
    auto timeout = lock_timeout_;
    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (true) {
        // Find our request in the queue.
        auto req_it = std::find_if(queue.requests.begin(),
                                   queue.requests.end(),
                                   [txn_id](const LockRequest& r) { return r.txn_id == txn_id; });

        if (req_it == queue.requests.end()) {
            return make_error(StatusCode::INTERNAL_ERROR, "lock request disappeared");
        }

        // Check if we were aborted by deadlock detection.
        if (req_it->aborted) {
            queue.requests.erase(req_it);
            SIXSEVEN_LOG_WARN("deadlock detected, aborting txn_id={}", txn_id);
            return make_error(StatusCode::DEADLOCK, "deadlock detected, transaction aborted");
        }

        if (req_it->granted) {
            return ok();
        }

        // Wait with timeout.
        queue.cv.wait_until(
            lock, deadline, [&req_it]() { return req_it->granted || req_it->aborted; });

        // Re-check after wakeup (loop handles all conditions at top).
        if (std::chrono::steady_clock::now() >= deadline) {
            // Check one more time before timing out.
            req_it = std::find_if(queue.requests.begin(),
                                  queue.requests.end(),
                                  [txn_id](const LockRequest& r) { return r.txn_id == txn_id; });
            if (req_it != queue.requests.end()) {
                if (req_it->aborted) {
                    queue.requests.erase(req_it);
                    SIXSEVEN_LOG_WARN("deadlock detected, aborting txn_id={}", txn_id);
                    return make_error(StatusCode::DEADLOCK,
                                      "deadlock detected, transaction aborted");
                }
                if (req_it->granted) {
                    return ok();
                }
                // Timed out — remove request.
                queue.requests.erase(req_it);
            }
            SIXSEVEN_LOG_WARN("lock timeout for txn_id={}", txn_id);
            return make_error(StatusCode::LOCK_TIMEOUT, "lock wait timeout exceeded");
        }
    }
}
}

void LockManager::grant_waiting_locked(LockQueue& queue) {
    bool granted_any = false;

    for (auto& req : queue.requests) {
        if (req.granted) {
            continue;
        }

        if (is_compatible_locked(queue, req.txn_id, req.mode)) {
            req.granted = true;
            granted_any = true;
        }
    }

    if (granted_any) {
        queue.cv.notify_all();
    }
}

bool LockManager::is_compatible_locked(const LockQueue& queue,
                                       txn_id_t txn_id,
                                       LockMode mode) const {
    for (const auto& req : queue.requests) {
        if (!req.granted) {
            continue;
        }
        if (req.txn_id == txn_id) {
            // Same transaction — always compatible with itself.
            continue;
        }
        if (!locks_compatible(req.mode, mode)) {
            return false;
        }
    }
    return true;
}

txn_id_t LockManager::detect_deadlock_locked(txn_id_t txn_id) const {
    DeadlockDetector detector;

    for (const auto& [key, queue] : lock_table_) {
        for (const auto& req : queue.requests) {
            if (!req.granted) {
                for (const auto& holder : queue.requests) {
                    if (holder.granted && holder.txn_id != req.txn_id) {
                        detector.add_edge(req.txn_id, holder.txn_id);
                    }
                }
            }
        }
    }

    return detector.detect_cycle(txn_id);
}

void LockManager::abort_victim_locked(txn_id_t victim) {
    for (auto& [key, queue] : lock_table_) {
        for (auto& req : queue.requests) {
            if (req.txn_id == victim && !req.granted) {
                req.aborted = true;
            }
        }
        queue.cv.notify_all();
    }
}

void LockManager::remove_waiting_request_locked(LockQueue& queue, txn_id_t txn_id) {
    for (auto it = queue.requests.begin(); it != queue.requests.end(); ++it) {
        if (it->txn_id == txn_id && !it->granted) {
            queue.requests.erase(it);
            return;
        }
    }
}

} // namespace sixseven
