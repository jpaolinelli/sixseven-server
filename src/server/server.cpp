#include "sixseven/server/server.h"

#include "sixseven/common/logging.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace sixseven {

namespace {

Result<void> set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return make_error(StatusCode::IO_ERROR,
                          std::string("fcntl(F_GETFL): ") + std::strerror(errno));
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return make_error(StatusCode::IO_ERROR,
                          std::string("fcntl(F_SETFL): ") + std::strerror(errno));
    }
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
        ::close(listen_fd_);
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
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

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
        ::close(listen_fd_);
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
    }
}

void Server::accept_connection() {
    struct sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);
    int client_fd =
        ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return; // No pending connections.
        }
        SIXSEVEN_LOG_WARN("accept() failed: {}", std::strerror(errno));
        return;
    }

    // Enforce max connections.
    {
        std::lock_guard lock(connections_mutex_);
        if (connections_.size() >= config_.max_connections) {
            SIXSEVEN_LOG_WARN("max connections ({}) reached, rejecting fd={}",
                           config_.max_connections,
                           client_fd);
            ::close(client_fd);
            return;
        }
    }

    auto nb_result = set_nonblocking(client_fd);
    if (!nb_result) {
        SIXSEVEN_LOG_WARN(
            "failed to set non-blocking on fd={}: {}", client_fd, nb_result.error().message);
        ::close(client_fd);
        return;
    }

    auto add_result = event_loop_->add_fd(client_fd, EventType::READ);
    if (!add_result) {
        SIXSEVEN_LOG_WARN(
            "failed to add fd={} to event loop: {}", client_fd, add_result.error().message);
        ::close(client_fd);
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

    std::lock_guard lock(connections_mutex_);
    connections_.emplace(client_fd, std::move(conn));
    protocol_handlers_.emplace(client_fd, std::move(handler));
}

void Server::handle_read(int fd) {
    std::lock_guard lock(connections_mutex_);
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

    // Process data through the PostgreSQL wire protocol handler.
    auto handler_it = protocol_handlers_.find(fd);
    if (handler_it == protocol_handlers_.end()) {
        SIXSEVEN_LOG_WARN("no protocol handler for fd={}", fd);
        close_connection(fd);
        return;
    }

    auto& handler = handler_it->second;
    auto process_result = handler.process(conn);
    if (!process_result) {
        SIXSEVEN_LOG_WARN("protocol error on fd={}: {}", fd, process_result.error().message);
        close_connection(fd);
        return;
    }

    // Close connection if the protocol handler says we're done (Terminate message).
    if (handler.state() == ProtocolState::CLOSED) {
        SIXSEVEN_LOG_DEBUG("connection fd={} terminated by protocol", fd);
        close_connection(fd);
        return;
    }

    // Enable write monitoring so the response gets flushed.
    if (conn.has_pending_writes()) {
        auto mod_result = event_loop_->modify_fd(fd, EventType::READ_WRITE);
        if (!mod_result) {
            SIXSEVEN_LOG_WARN("failed to modify fd={} for write: {}", fd, mod_result.error().message);
        }
    }
}

void Server::handle_write(int fd) {
    std::lock_guard lock(connections_mutex_);
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

void Server::close_connection(int fd) {
    // Note: caller must hold connections_mutex_.
    (void)event_loop_->remove_fd(fd);
    connections_.erase(fd);
    protocol_handlers_.erase(fd);
    SIXSEVEN_LOG_DEBUG("connection fd={} removed", fd);
}

} // namespace sixseven
