#include "sixseven/server/cancel_registry.h"

#include "sixseven/common/logging.h"

namespace sixseven {

void CancelRegistry::register_connection(int32_t backend_pid, int32_t secret_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.emplace(backend_pid, Entry{secret_key, nullptr});
    SIXSEVEN_LOG_DEBUG("cancel registry: registered pid={}", backend_pid);
}

void CancelRegistry::unregister_connection(int32_t backend_pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(backend_pid);
    SIXSEVEN_LOG_DEBUG("cancel registry: unregistered pid={}", backend_pid);
}

void CancelRegistry::set_cancel_flag(int32_t backend_pid, std::shared_ptr<std::atomic<bool>> flag) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(backend_pid);
    if (it != entries_.end()) {
        it->second.flag = std::move(flag);
    }
}

void CancelRegistry::clear_cancel_flag(int32_t backend_pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(backend_pid);
    if (it != entries_.end()) {
        it->second.flag = nullptr;
    }
}

void CancelRegistry::request_cancel(int32_t backend_pid, int32_t secret_key) {
    std::shared_ptr<std::atomic<bool>> flag;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(backend_pid);
        if (it == entries_.end()) {
            SIXSEVEN_LOG_DEBUG("cancel registry: cancel request for unknown pid={}", backend_pid);
            return;
        }
        if (it->second.secret_key != secret_key) {
            // Wrong secret -- silently ignore (do not log details to prevent
            // information leakage about active connections).
            SIXSEVEN_LOG_WARN(
                "cancel registry: cancel request for pid={} with wrong secret (ignored)",
                backend_pid);
            return;
        }
        if (!it->second.flag) {
            // No statement executing -- cancel is a no-op (the next statement
            // will not inherit this stale cancel because the flag is per-statement).
            SIXSEVEN_LOG_DEBUG(
                "cancel registry: cancel request for pid={} but no statement running", backend_pid);
            return;
        }
        // Copy the shared_ptr so we can set the flag outside the lock.
        flag = it->second.flag;
    }
    // Set without holding the lock to avoid any priority inversion.
    flag->store(true, std::memory_order_release);
    SIXSEVEN_LOG_INFO("cancel registry: cancel requested for pid={}", backend_pid);
}

} // namespace sixseven
