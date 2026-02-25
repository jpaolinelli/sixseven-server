#include "giodb/server/server.h"

#include "giodb/common/logging.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace giodb {

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

Server::Server(Config config) : config_(std::move(config)) {}

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

    GIODB_LOG_INFO("GioDB server listening on port {}", bound_port_);

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
    GIODB_LOG_INFO("initiating graceful shutdown (timeout={}s)", config_.shutdown_timeout_s);

    // Step 1: Stop accepting new connections.
    if (listen_fd_ >= 0) {
        (void)event_loop_->remove_fd(listen_fd_);
        ::close(listen_fd_);
        listen_fd_ = -1;
        GIODB_LOG_INFO("shutdown: stopped accepting new connections");
    }

    // Step 2: Wait for the thread pool to drain pending work.
    if (thread_pool_) {
        GIODB_LOG_INFO("shutdown: draining thread pool ({} pending tasks)",
                       thread_pool_->pending_tasks());
        thread_pool_->shutdown();
        GIODB_LOG_INFO("shutdown: thread pool drained");
    }

    // Step 3: Close all client connections.
    {
        std::lock_guard lock(connections_mutex_);
        GIODB_LOG_INFO("shutdown: closing {} client connections", connections_.size());
        for (auto& [fd, conn] : connections_) {
            conn.close();
        }
        connections_.clear();
    }

    GIODB_LOG_INFO("shutdown: complete");
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
            GIODB_LOG_ERROR("event loop poll error: {}", poll_result.error().message);
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
        GIODB_LOG_WARN("accept() failed: {}", std::strerror(errno));
        return;
    }

    // Enforce max connections.
    {
        std::lock_guard lock(connections_mutex_);
        if (connections_.size() >= config_.max_connections) {
            GIODB_LOG_WARN("max connections ({}) reached, rejecting fd={}",
                           config_.max_connections,
                           client_fd);
            ::close(client_fd);
            return;
        }
    }

    auto nb_result = set_nonblocking(client_fd);
    if (!nb_result) {
        GIODB_LOG_WARN(
            "failed to set non-blocking on fd={}: {}", client_fd, nb_result.error().message);
        ::close(client_fd);
        return;
    }

    auto add_result = event_loop_->add_fd(client_fd, EventType::READ);
    if (!add_result) {
        GIODB_LOG_WARN(
            "failed to add fd={} to event loop: {}", client_fd, add_result.error().message);
        ::close(client_fd);
        return;
    }

    Connection conn(client_fd);
    // Connection now owns client_fd — do not call ::close(client_fd) directly.

    // Transition INIT -> AUTH (in a real server, we'd send an auth request).
    auto t1 = conn.transition_to(ConnectionState::AUTH);
    if (!t1) {
        GIODB_LOG_WARN("connection state error: {}", t1.error().message);
        (void)event_loop_->remove_fd(client_fd);
        return; // conn destructor closes the fd.
    }
    // For now, skip auth and go straight to READY.
    auto t2 = conn.transition_to(ConnectionState::READY);
    if (!t2) {
        GIODB_LOG_WARN("connection state error: {}", t2.error().message);
        (void)event_loop_->remove_fd(client_fd);
        return; // conn destructor closes the fd.
    }

    char addr_str[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
    GIODB_LOG_INFO(
        "accepted connection fd={} from {}:{}", client_fd, addr_str, ntohs(client_addr.sin_port));

    std::lock_guard lock(connections_mutex_);
    connections_.emplace(client_fd, std::move(conn));
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
        GIODB_LOG_WARN("read error on fd={}: {}", fd, read_result.error().message);
        close_connection(fd);
        return;
    }

    if (!*read_result) {
        // EAGAIN — no data available on this wakeup, try later.
        return;
    }

    if (**read_result == 0) {
        // EOF — peer closed connection.
        GIODB_LOG_DEBUG("connection fd={} closed by peer", fd);
        close_connection(fd);
        return;
    }

    // Echo the received data back (placeholder until wire protocol is implemented).
    const auto& rbuf = conn.read_buffer();
    conn.enqueue_write(rbuf.data(), rbuf.size());
    conn.consume_read(rbuf.size());

    // Enable write monitoring so the response gets flushed.
    if (conn.has_pending_writes()) {
        auto mod_result = event_loop_->modify_fd(fd, EventType::READ_WRITE);
        if (!mod_result) {
            GIODB_LOG_WARN("failed to modify fd={} for write: {}", fd, mod_result.error().message);
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
        GIODB_LOG_WARN("write error on fd={}: {}", fd, write_result.error().message);
        close_connection(fd);
        return;
    }

    // If all pending data is written, stop monitoring for writes.
    if (!conn.has_pending_writes()) {
        auto mod_result = event_loop_->modify_fd(fd, EventType::READ);
        if (!mod_result) {
            GIODB_LOG_WARN(
                "failed to modify fd={} for read-only: {}", fd, mod_result.error().message);
        }
    }
}

void Server::close_connection(int fd) {
    // Note: caller must hold connections_mutex_.
    (void)event_loop_->remove_fd(fd);
    connections_.erase(fd);
    GIODB_LOG_DEBUG("connection fd={} removed", fd);
}

} // namespace giodb
