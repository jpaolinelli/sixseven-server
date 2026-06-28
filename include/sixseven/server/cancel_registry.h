#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace sixseven {

/// Thread-safe registry that maps (backend_pid, secret_key) pairs to a
/// per-statement cancel flag.
///
/// Lifecycle:
///   1. register_connection() when a client completes startup (pid + secret
///      are known and advertised via BackendKeyData).
///   2. set_cancel_flag() from the statement executor to install the flag for
///      the currently executing statement; clear_cancel_flag() when done.
///   3. request_cancel(pid, secret) from the CancelRequest handler (runs on a
///      different thread/connection) -- sets the flag if both pid AND secret
///      match.  A wrong secret is silently ignored (protocol requirement).
///   4. unregister_connection() when the connection closes.
///
/// Thread-safety: all methods acquire registry_mutex_; callers must NEVER
/// hold registry_mutex_ while executing a query (no deadlock).  The cancel
/// flag itself is std::atomic<bool> so the executor reads it lock-free.
class CancelRegistry {
public:
    CancelRegistry() = default;

    CancelRegistry(const CancelRegistry&) = delete;
    CancelRegistry& operator=(const CancelRegistry&) = delete;

    /// Register a connection.  No-op if already registered.
    void register_connection(int32_t backend_pid, int32_t secret_key);

    /// Remove a connection's entry.  Safe to call even if not registered.
    void unregister_connection(int32_t backend_pid);

    /// Install the cancel flag for the currently executing statement on this
    /// connection.  The flag must outlive the statement (caller ensures this
    /// by holding a shared_ptr for the statement's duration).
    void set_cancel_flag(int32_t backend_pid, std::shared_ptr<std::atomic<bool>> flag);

    /// Clear the cancel flag for this connection (call after statement ends).
    void clear_cancel_flag(int32_t backend_pid);

    /// Set the cancel flag for the connection identified by (pid, secret).
    /// Silently no-ops if pid is unknown, secret does not match, or no
    /// statement is currently executing.
    void request_cancel(int32_t backend_pid, int32_t secret_key);

private:
    struct Entry {
        int32_t secret_key;
        std::shared_ptr<std::atomic<bool>> flag; // null when idle
    };

    mutable std::mutex mutex_;
    std::unordered_map<int32_t, Entry> entries_;
};

} // namespace sixseven
