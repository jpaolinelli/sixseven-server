#include "sixseven/server/server.h"

#include "sixseven/common/logging.h"
#include "sixseven/common/platform.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>

namespace sixseven {

namespace {

Result<void> set_nonblocking(int fd) {
#if defined(_WIN32)
    if (sixseven_platform::set_socket_nonblocking(fd) != 0) {
        return make_error(StatusCode::IO_ERROR,
                          std::string("ioctlsocket(FIONBIO): WSAError=") +
                              std::to_string(WSAGetLastError()));
    }
#else
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return make_error(StatusCode::IO_ERROR,
                          std::string("fcntl(F_GETFL): ") + std::strerror(errno));
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return make_error(StatusCode::IO_ERROR,
                          std::string("fcntl(F_SETFL): ") + std::strerror(errno));
    }
#endif
    return ok();
}

} // namespace

Server::Server(Config config) : config_(std::move(config)) {
    auto method = parse_auth_method(config_.auth_method);
    if (method) {
        auth_method_ = *method;
    }
}

void Server::set_query_executor(QueryExecutor executor) {
    query_executor_ = std::move(executor);
}

void Server::set_query_describer(QueryDescriber describer) {
    query_describer_ = std::move(describer);
}

void Server::set_user_manager(UserManager* user_mgr) {
    user_mgr_ = user_mgr;
}

Server::~Server() {
    request_shutdown();
    do_shutdown();
    if (listen_fd_ >= 0) {
        sixseven_platform::socket_close(listen_fd_);
        listen_fd_ = -1;
    }
}

Result<void> Server::setup_listener() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        return make_error(StatusCode::IO_ERROR, std::string("socket(): ") + std::strerror(errno));
    }

    // Allow address reuse.
    int opt = 1;
    ::setsockopt(
        listen_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    auto nb_result = set_nonblocking(listen_fd_);
    if (!nb_result) {
        return nb_result;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config_.port);

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        return make_error(StatusCode::IO_ERROR, std::string("bind(): ") + std::strerror(errno));
    }

    if (::listen(listen_fd_, SOMAXCONN) < 0) {
        return make_error(StatusCode::IO_ERROR, std::string("listen(): ") + std::strerror(errno));
    }

    // Retrieve the actual bound port (important when config_.port == 0).
    struct sockaddr_in bound_addr{};
    socklen_t addr_len = sizeof(bound_addr);
    if (::getsockname(listen_fd_, reinterpret_cast<struct sockaddr*>(&bound_addr), &addr_len) ==
        0) {
        bound_port_ = ntohs(bound_addr.sin_port);
    }

    return ok();
}

Result<void> Server::start() {
    auto setup_result = setup_listener();
    if (!setup_result) {
        return setup_result;
    }

    event_loop_ = EventLoop::create();
    auto init_result = event_loop_->init();
    if (!init_result) {
        return init_result;
    }

    auto add_result = event_loop_->add_fd(listen_fd_, EventType::READ);
    if (!add_result) {
        return add_result;
    }

    thread_pool_ = std::make_unique<ThreadPool>(DEFAULT_THREAD_POOL_SIZE);
    start_time_ = std::chrono::steady_clock::now();
    running_.store(true, std::memory_order_release);

    SIXSEVEN_LOG_INFO("SixSevenDB server listening on port {}", bound_port_);

    run_event_loop();

    // Event loop exited — perform the actual shutdown cleanup on this thread
    // (safe: no signal-handler context, no mutex contention risk).
    do_shutdown();
    return ok();
}

void Server::request_shutdown() {
    // Signal-safe: only touches an atomic.
    running_.store(false, std::memory_order_release);
}

void Server::shutdown() {
    request_shutdown();
}

void Server::do_shutdown() {
    SIXSEVEN_LOG_INFO("initiating graceful shutdown (timeout={}s)", config_.shutdown_timeout_s);

    // Step 1: Stop accepting new connections.
    if (listen_fd_ >= 0) {
        (void)event_loop_->remove_fd(listen_fd_);
        sixseven_platform::socket_close(listen_fd_);
        listen_fd_ = -1;
        SIXSEVEN_LOG_INFO("shutdown: stopped accepting new connections");
    }

    // Step 2: Wait for the thread pool to drain pending work.
    if (thread_pool_) {
        SIXSEVEN_LOG_INFO("shutdown: draining thread pool ({} pending tasks)",
                          thread_pool_->pending_tasks());
        thread_pool_->shutdown();
        SIXSEVEN_LOG_INFO("shutdown: thread pool drained");
    }

    // Step 3: Close all client connections.
    {
        std::lock_guard lock(connections_mutex_);
        inflight_fds_.clear();
        SIXSEVEN_LOG_INFO("shutdown: closing {} client connections", connections_.size());
        for (auto& [fd, conn] : connections_) {
            conn.close();
        }
        connections_.clear();
        protocol_handlers_.clear();
    }

    SIXSEVEN_LOG_INFO("shutdown: complete");
}

HealthInfo Server::health() const {
    HealthInfo info;
    info.version = VERSION;
    if (running_.load(std::memory_order_acquire)) {
        auto now = std::chrono::steady_clock::now();
        info.uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_);
    }
    {
        std::lock_guard lock(connections_mutex_);
        info.active_connections = connections_.size();
    }
    info.max_connections = config_.max_connections;
    return info;
}

void Server::run_event_loop() {
    while (running_.load(std::memory_order_acquire)) {
        auto poll_result = event_loop_->poll(100); // 100 ms timeout for shutdown checks.
        if (!poll_result) {
            SIXSEVEN_LOG_ERROR("event loop poll error: {}", poll_result.error().message);
            break;
        }

        for (const auto& event : *poll_result) {
            if (event.fd == listen_fd_) {
                accept_connection();
            } else {
                if (event.type == EventType::READ || event.type == EventType::READ_WRITE) {
                    handle_read(event.fd);
                }
                if (event.type == EventType::WRITE || event.type == EventType::READ_WRITE) {
                    handle_write(event.fd);
                }
            }
        }

        // Re-enable connections whose queries finished on the thread pool.
        process_completed_queries();
    }
}

void Server::accept_connection() {
    struct sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);
    int client_fd =
        ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
    if (client_fd < 0) {
#if defined(_WIN32)
        if (sixseven_platform::is_socket_would_block()) {
            return; // No pending connections.
        }
        SIXSEVEN_LOG_WARN("accept() failed: WSAError={}", WSAGetLastError());
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return; // No pending connections.
        }
        SIXSEVEN_LOG_WARN("accept() failed: {}", std::strerror(errno));
#endif
        return;
    }

    // Enforce max connections.
    {
        std::lock_guard lock(connections_mutex_);
        if (connections_.size() >= config_.max_connections) {
            SIXSEVEN_LOG_WARN("max connections ({}) reached, rejecting fd={}",
                              config_.max_connections,
                              client_fd);
            sixseven_platform::socket_close(client_fd);
            return;
        }
    }

    auto nb_result = set_nonblocking(client_fd);
    if (!nb_result) {
        SIXSEVEN_LOG_WARN(
            "failed to set non-blocking on fd={}: {}", client_fd, nb_result.error().message);
        sixseven_platform::socket_close(client_fd);
        return;
    }

    // Belt-and-suspenders SIGPIPE suppression for macOS/BSD where MSG_NOSIGNAL
    // is unavailable.  Linux uses MSG_NOSIGNAL in Connection::write_to_socket.
    // Windows has no SIGPIPE, so this is a no-op there.
#if defined(SO_NOSIGPIPE)
    {
        int nosig = 1;
        ::setsockopt(client_fd,
                     SOL_SOCKET,
                     SO_NOSIGPIPE,
                     reinterpret_cast<const char*>(&nosig),
                     sizeof(nosig));
    }
#endif

    auto add_result = event_loop_->add_fd(client_fd, EventType::READ);
    if (!add_result) {
        SIXSEVEN_LOG_WARN(
            "failed to add fd={} to event loop: {}", client_fd, add_result.error().message);
        sixseven_platform::socket_close(client_fd);
        return;
    }

    Connection conn(client_fd);
    // Connection now owns client_fd — do not call ::close(client_fd) directly.

    char addr_str[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
    SIXSEVEN_LOG_INFO(
        "accepted connection fd={} from {}:{}", client_fd, addr_str, ntohs(client_addr.sin_port));

    // Create a PG protocol handler for this connection.
    int32_t pid = next_backend_pid_.fetch_add(1, std::memory_order_relaxed);
    PgProtocolHandler handler(pid);
    if (query_executor_) {
        handler.set_query_executor(query_executor_);
    }
    if (query_describer_) {
        handler.set_query_describer(query_describer_);
    }
    handler.set_auth(auth_method_, user_mgr_);

    // Wire cancellation callbacks (GDB-956).
    handler.set_cancel_requester([this](int32_t cancel_pid, int32_t secret) {
        cancel_registry_.request_cancel(cancel_pid, secret);
    });
    handler.set_cancel_connection_registrar([this](int32_t reg_pid, int32_t secret) {
        cancel_registry_.register_connection(reg_pid, secret);
    });
    handler.set_cancel_flag_registrar(
        [this](int32_t reg_pid, std::shared_ptr<std::atomic<bool>> flag) {
            cancel_registry_.set_cancel_flag(reg_pid, std::move(flag));
        });
    handler.set_cancel_flag_clearer(
        [this](int32_t clr_pid) { cancel_registry_.clear_cancel_flag(clr_pid); });

    std::lock_guard lock(connections_mutex_);
    connections_.emplace(client_fd, std::move(conn));
    protocol_handlers_.emplace(client_fd, std::move(handler));
}

void Server::handle_read(int fd) {
    std::lock_guard lock(connections_mutex_);

    // Skip if this connection's query is already executing on the thread pool.
    if (inflight_fds_.count(fd)) {
        return;
    }

    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }

    auto& conn = it->second;
    auto read_result = conn.read_from_socket();
    if (!read_result) {
        SIXSEVEN_LOG_WARN("read error on fd={}: {}", fd, read_result.error().message);
        close_connection(fd);
        return;
    }

    if (!*read_result) {
        // EAGAIN — no data available on this wakeup, try later.
        return;
    }

    if (**read_result == 0) {
        // EOF — peer closed connection.
        SIXSEVEN_LOG_DEBUG("connection fd={} closed by peer", fd);
        close_connection(fd);
        return;
    }

    // Check that we have a protocol handler.
    auto handler_it = protocol_handlers_.find(fd);
    if (handler_it == protocol_handlers_.end()) {
        SIXSEVEN_LOG_WARN("no protocol handler for fd={}", fd);
        close_connection(fd);
        return;
    }

    // Mark this connection as in-flight: the event loop will not touch
    // the Connection or PgProtocolHandler while the flag is set.
    inflight_fds_.insert(fd);

    // Remove the fd from the event loop entirely while the thread pool
    // processes the query. Because a connected TCP socket with send-buffer
    // space is always write-ready, leaving it registered (even as WRITE-only)
    // causes a level-triggered busy-poll spin for the full query duration.
    // Removing it means poll() does not return this fd until it is re-added
    // in process_completed_queries(). EOF is detected on the next READ
    // registration after the query completes.
    (void)event_loop_->remove_fd(fd);

    // Submit protocol processing to the thread pool. Pointers into the
    // unordered_maps are stable because we never erase/insert while the
    // fd is in inflight_fds_.
    Connection* conn_ptr = &it->second;
    PgProtocolHandler* handler_ptr = &handler_it->second;

    thread_pool_->submit([this, fd, conn_ptr, handler_ptr]() {
        auto process_result = handler_ptr->process(*conn_ptr);

        bool should_close = false;
        if (!process_result) {
            SIXSEVEN_LOG_WARN("protocol error on fd={}: {}", fd, process_result.error().message);
            should_close = true;
        } else if (handler_ptr->state() == ProtocolState::CLOSED) {
            SIXSEVEN_LOG_DEBUG("connection fd={} terminated by protocol", fd);
            should_close = true;
        }

        // Report completion back to the event loop thread.
        {
            std::lock_guard clock(completed_mutex_);
            completed_fds_.push_back(fd);
            if (should_close) {
                close_pending_fds_.insert(fd);
            }
        }
        event_loop_->wakeup();
    });
}

void Server::handle_write(int fd) {
    std::lock_guard lock(connections_mutex_);

    // Skip if the thread pool is currently processing this connection.
    if (inflight_fds_.count(fd)) {
        return;
    }

    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }

    auto& conn = it->second;
    auto write_result = conn.write_to_socket();
    if (!write_result) {
        SIXSEVEN_LOG_WARN("write error on fd={}: {}", fd, write_result.error().message);
        close_connection(fd);
        return;
    }

    // If all pending data is written, stop monitoring for writes.
    if (!conn.has_pending_writes()) {
        auto mod_result = event_loop_->modify_fd(fd, EventType::READ);
        if (!mod_result) {
            SIXSEVEN_LOG_WARN(
                "failed to modify fd={} for read-only: {}", fd, mod_result.error().message);
        }
    }
}

void Server::process_completed_queries() {
    // Swap completion lists under the lightweight completed_mutex_ to
    // minimise time thread-pool workers spend blocked.
    std::vector<int> completed;
    std::unordered_set<int> to_close;
    {
        std::lock_guard clock(completed_mutex_);
        completed.swap(completed_fds_);
        to_close.swap(close_pending_fds_);
    }

    if (completed.empty()) {
        return;
    }

    std::lock_guard lock(connections_mutex_);
    for (int fd : completed) {
        inflight_fds_.erase(fd);

        if (to_close.count(fd)) {
            close_connection(fd);
            continue;
        }

        auto it = connections_.find(fd);
        if (it == connections_.end()) {
            continue;
        }

        // Re-register the fd now that the query is complete. The fd was fully
        // removed from the event loop in handle_read(), so we use add_fd here
        // (not modify_fd). Also arm WRITE if there is buffered response data.
        auto& conn = it->second;
        EventType type = conn.has_pending_writes() ? EventType::READ_WRITE : EventType::READ;
        auto add_result = event_loop_->add_fd(fd, type);
        if (!add_result) {
            SIXSEVEN_LOG_WARN("failed to re-register fd={}: {}", fd, add_result.error().message);
        }
    }
}

void Server::close_connection(int fd) {
    // Note: caller must hold connections_mutex_.
    inflight_fds_.erase(fd); // Clean up in case of shutdown while in-flight.
    (void)event_loop_->remove_fd(fd);
    connections_.erase(fd);
    // Unregister from the cancel registry before erasing the handler (GDB-956).
    auto handler_it = protocol_handlers_.find(fd);
    if (handler_it != protocol_handlers_.end()) {
        cancel_registry_.unregister_connection(handler_it->second.backend_pid());
    }
    protocol_handlers_.erase(fd);
    SIXSEVEN_LOG_DEBUG("connection fd={} removed", fd);
}

} // namespace sixseven
