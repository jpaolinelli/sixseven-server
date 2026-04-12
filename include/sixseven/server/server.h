#pragma once

#include "sixseven/common/config.h"
#include "sixseven/common/result.h"
#include "sixseven/server/connection.h"
#include "sixseven/server/event_loop.h"
#include "sixseven/server/pg_protocol.h"
#include "sixseven/server/thread_pool.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sixseven {

/// Server health snapshot.
struct HealthInfo {
    std::string version;
    std::chrono::seconds uptime{0};
    size_t active_connections = 0;
    size_t max_connections = 0;
};

/// Event-driven TCP server for SixSevenDB.
///
/// Runs a single acceptor thread with an epoll/kqueue event loop and dispatches
/// query work to a thread pool.
class Server {
public:
    static constexpr const char* VERSION = "0.1.0";
    static constexpr size_t DEFAULT_THREAD_POOL_SIZE = 8;

    explicit Server(Config config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Set the callback used to execute SQL queries via the PG protocol.
    void set_query_executor(QueryExecutor executor);

    /// Set the callback used to describe SQL result columns without executing.
    void set_query_describer(QueryDescriber describer);

    /// Set the user manager for authentication.
    void set_user_manager(UserManager* user_mgr);

    /// Bind, listen, and run the event loop. Blocks until shutdown is requested.
    [[nodiscard]] Result<void> start();

    /// Signal-safe: set the stop flag so the event loop exits on next iteration.
    /// The actual cleanup (drain, close) happens when start() unwinds.
    /// Safe to call from a signal handler.
    void request_shutdown();

    /// Convenience for non-signal callers: equivalent to request_shutdown().
    /// Actual resource cleanup is deferred to when start() returns.
    void shutdown();

    bool is_running() const { return running_.load(std::memory_order_acquire); }
    HealthInfo health() const;

    /// Exposed for testing: the port we actually bound to (useful with port 0).
    uint16_t bound_port() const { return bound_port_; }

private:
    [[nodiscard]] Result<void> setup_listener();
    void run_event_loop();
    void do_shutdown();
    void accept_connection();
    void handle_read(int fd);
    void handle_write(int fd);
    void close_connection(int fd);
    void process_completed_queries();

    Config config_;
    std::unique_ptr<EventLoop> event_loop_;
    std::unique_ptr<ThreadPool> thread_pool_;
    std::unordered_map<int, Connection> connections_;
    std::unordered_map<int, PgProtocolHandler> protocol_handlers_;
    mutable std::mutex connections_mutex_;
    QueryExecutor query_executor_;
    QueryDescriber query_describer_;
    AuthMethod auth_method_ = AuthMethod::TRUST;
    UserManager* user_mgr_ = nullptr;
    int listen_fd_ = -1;
    uint16_t bound_port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int32_t> next_backend_pid_{1};
    std::chrono::steady_clock::time_point start_time_;

    // -- Thread pool query offload state --
    // Connections whose queries are currently executing on the thread pool.
    // Guarded by connections_mutex_.
    std::unordered_set<int> inflight_fds_;

    // Completed queries reported by thread pool workers. Guarded by
    // completed_mutex_ (separate from connections_mutex_ to avoid blocking
    // workers on event-loop lock contention).
    std::mutex completed_mutex_;
    std::vector<int> completed_fds_;
    std::unordered_set<int> close_pending_fds_;
};

} // namespace sixseven
