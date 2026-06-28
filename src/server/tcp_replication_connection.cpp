#include "sixseven/server/tcp_replication_connection.h"

#include "sixseven/common/logging.h"
#include "sixseven/common/platform.h"

#include <cerrno>
#include <cstring>
#include <string>

// Platform socket headers (platform.h already pulled them in via #ifdef).
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace sixseven {

namespace {

std::string last_socket_error() {
#if defined(_WIN32)
    int err = WSAGetLastError();
    char buf[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr,
                   err,
                   0,
                   buf,
                   sizeof(buf),
                   nullptr);
    return std::string(buf);
#else
    return std::string(std::strerror(errno));
#endif
}

void close_socket(int fd) {
#if defined(_WIN32)
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::close(fd);
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// TcpReplicationConnection
// ---------------------------------------------------------------------------

TcpReplicationConnection::TcpReplicationConnection() = default;

TcpReplicationConnection::~TcpReplicationConnection() {
    close();
}

TcpReplicationConnection::TcpReplicationConnection(TcpReplicationConnection&& other) noexcept
    : fd_(other.fd_), peer_desc_(std::move(other.peer_desc_)) {
    other.fd_ = -1;
}

TcpReplicationConnection&
TcpReplicationConnection::operator=(TcpReplicationConnection&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        peer_desc_ = std::move(other.peer_desc_);
        other.fd_ = -1;
    }
    return *this;
}

Result<void> TcpReplicationConnection::connect(const std::string& host, uint16_t port) {
    if (host.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "TcpReplicationConnection::connect: host is empty");
    }

    if (fd_ >= 0) {
        close();
    }

    // Resolve host.
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port);
    int rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0) {
#if defined(_WIN32)
        return make_error(StatusCode::NETWORK_ERROR,
                          "getaddrinfo failed for " + host + ":" + port_str + ": " +
                              std::to_string(rc));
#else
        return make_error(StatusCode::NETWORK_ERROR,
                          "getaddrinfo failed for " + host + ":" + port_str + ": " +
                              std::string(gai_strerror(rc)));
#endif
    }

    int sock = static_cast<int>(::socket(res->ai_family, res->ai_socktype, res->ai_protocol));
    if (sock < 0) {
        ::freeaddrinfo(res);
        return make_error(StatusCode::NETWORK_ERROR, "socket(): " + last_socket_error());
    }

    if (::connect(sock, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
        const std::string err = last_socket_error();
        ::freeaddrinfo(res);
        close_socket(sock);
        return make_error(StatusCode::NETWORK_ERROR,
                          "connect() to " + host + ":" + port_str + " failed: " + err);
    }

    ::freeaddrinfo(res);
    fd_ = sock;
    peer_desc_ = host + ":" + port_str;
    return ok();
}

Result<void> TcpReplicationConnection::send(std::span<const uint8_t> data) {
    if (fd_ < 0) {
        return make_error(StatusCode::NETWORK_ERROR,
                          "TcpReplicationConnection::send: not connected");
    }

    size_t sent = 0;
    while (sent < data.size()) {
#if defined(_WIN32)
        auto n = ::send(static_cast<SOCKET>(fd_),
                        reinterpret_cast<const char*>(data.data() + sent),
                        static_cast<int>(data.size() - sent),
                        0);
        if (n == SOCKET_ERROR) {
#else
        auto n =
            ::send(fd_, reinterpret_cast<const char*>(data.data() + sent), data.size() - sent, 0);
        if (n < 0) {
#endif
            return make_error(StatusCode::NETWORK_ERROR, "send() failed: " + last_socket_error());
        }
        sent += static_cast<size_t>(n);
    }
    return ok();
}

Result<std::vector<uint8_t>> TcpReplicationConnection::receive(size_t max_bytes,
                                                               std::chrono::milliseconds timeout) {
    if (fd_ < 0) {
        return make_error(StatusCode::NETWORK_ERROR,
                          "TcpReplicationConnection::receive: not connected");
    }

    if (max_bytes == 0) {
        return ok(std::vector<uint8_t>{});
    }

    // Use select() to implement the timeout without blocking indefinitely.
    fd_set read_fds;
    FD_ZERO(&read_fds);
#if defined(_WIN32)
    FD_SET(static_cast<SOCKET>(fd_), &read_fds);
#else
    FD_SET(fd_, &read_fds);
#endif

    const auto timeout_us = std::chrono::duration_cast<std::chrono::microseconds>(timeout).count();
    struct timeval tv{};
    tv.tv_sec = static_cast<long>(timeout_us / 1'000'000);
    tv.tv_usec = static_cast<long>(timeout_us % 1'000'000);

#if defined(_WIN32)
    int ready = ::select(0, &read_fds, nullptr, nullptr, &tv);
#else
    int ready = ::select(fd_ + 1, &read_fds, nullptr, nullptr, &tv);
#endif

    if (ready == 0) {
        // Timeout: no data within the window.
        return ok(std::vector<uint8_t>{});
    }

    if (ready < 0) {
        return make_error(StatusCode::NETWORK_ERROR, "select() failed: " + last_socket_error());
    }

    // Data available: read up to max_bytes.
    std::vector<uint8_t> buf(max_bytes);
#if defined(_WIN32)
    auto n = ::recv(static_cast<SOCKET>(fd_),
                    reinterpret_cast<char*>(buf.data()),
                    static_cast<int>(max_bytes),
                    0);
    if (n == 0) {
        return make_error(StatusCode::NETWORK_ERROR, "primary closed the replication connection");
    }
    if (n == SOCKET_ERROR) {
#else
    auto n = ::recv(fd_, buf.data(), max_bytes, 0);
    if (n == 0) {
        return make_error(StatusCode::NETWORK_ERROR, "primary closed the replication connection");
    }
    if (n < 0) {
#endif
        return make_error(StatusCode::NETWORK_ERROR, "recv() failed: " + last_socket_error());
    }

    buf.resize(static_cast<size_t>(n));
    return ok(std::move(buf));
}

void TcpReplicationConnection::close() {
    if (fd_ >= 0) {
        close_socket(fd_);
        fd_ = -1;
        SIXSEVEN_LOG_DEBUG("TcpReplicationConnection: closed connection to {}", peer_desc_);
        peer_desc_.clear();
    }
}

bool TcpReplicationConnection::is_open() const {
    return fd_ >= 0;
}

std::string TcpReplicationConnection::peer_description() const {
    return peer_desc_.empty() ? "(not connected)" : peer_desc_;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

Result<std::unique_ptr<ReplicationConnection>>
make_tcp_replication_connection(const std::string& host, uint16_t port) {
    if (host.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "make_tcp_replication_connection: host is empty");
    }

    auto conn = std::make_unique<TcpReplicationConnection>();
    auto result = conn->connect(host, port);
    if (!result) {
        return make_error(result.error().code, result.error().message);
    }

    return ok(std::unique_ptr<ReplicationConnection>(std::move(conn)));
}

} // namespace sixseven
