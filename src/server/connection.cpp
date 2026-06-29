#include "sixseven/server/connection.h"

#include "sixseven/common/logging.h"
#include "sixseven/common/platform.h"

#include <cerrno>
#include <cstring>

namespace sixseven {

Connection::Connection(int fd) : fd_(fd), state_(ConnectionState::INIT) {}

Connection::~Connection() {
    if (fd_ >= 0) {
        sixseven_platform::socket_close(fd_);
    }
}

Connection::Connection(Connection&& other) noexcept
    : fd_(other.fd_), state_(other.state_), read_buffer_(std::move(other.read_buffer_)),
      write_buffer_(std::move(other.write_buffer_)) {
    other.fd_ = -1;
    other.state_ = ConnectionState::CLOSED;
}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            sixseven_platform::socket_close(fd_);
        }
        fd_ = other.fd_;
        state_ = other.state_;
        read_buffer_ = std::move(other.read_buffer_);
        write_buffer_ = std::move(other.write_buffer_);
        other.fd_ = -1;
        other.state_ = ConnectionState::CLOSED;
    }
    return *this;
}

Result<std::optional<size_t>> Connection::read_from_socket() {
    if (fd_ < 0) {
        return make_error(StatusCode::IO_ERROR, "read on closed connection");
    }

    uint8_t buf[4096];
    ssize_t n = ::recv(fd_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
    if (n < 0) {
#if defined(_WIN32)
        if (sixseven_platform::is_socket_would_block()) {
            return ok(std::optional<size_t>{std::nullopt}); // No data available.
        }
        return make_error(StatusCode::IO_ERROR,
                          std::string("recv(): WSAError=") + std::to_string(WSAGetLastError()));
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return ok(std::optional<size_t>{std::nullopt}); // No data available.
        }
        return make_error(StatusCode::IO_ERROR, std::string("recv(): ") + std::strerror(errno));
#endif
    }
    if (n == 0) {
        // EOF — peer closed connection.
        return ok(std::optional<size_t>{size_t{0}});
    }

    read_buffer_.insert(read_buffer_.end(), buf, buf + n);
    return ok(std::optional<size_t>{static_cast<size_t>(n)});
}

Result<size_t> Connection::write_to_socket() {
    if (fd_ < 0) {
        return make_error(StatusCode::IO_ERROR, "write on closed connection");
    }
    if (write_buffer_.empty()) {
        return ok(size_t{0});
    }

    // Use MSG_NOSIGNAL on Linux so a write to a disconnected peer returns
    // EPIPE (errno) rather than raising SIGPIPE and potentially killing the
    // process.  On macOS/BSD MSG_NOSIGNAL does not exist; SO_NOSIGPIPE is set
    // on the accepted socket instead (see server.cpp accept_connection).  On
    // Windows neither exists and Winsock has no SIGPIPE concept, so flags=0
    // is correct there too.
#if defined(MSG_NOSIGNAL)
    constexpr int kSendFlags = MSG_NOSIGNAL;
#else
    constexpr int kSendFlags = 0;
#endif

    ssize_t n = ::send(fd_,
                       reinterpret_cast<const char*>(write_buffer_.data()),
                       static_cast<int>(write_buffer_.size()),
                       kSendFlags);
    if (n < 0) {
#if defined(_WIN32)
        if (sixseven_platform::is_socket_would_block()) {
            return ok(size_t{0});
        }
        return make_error(StatusCode::IO_ERROR,
                          std::string("send(): WSAError=") + std::to_string(WSAGetLastError()));
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return ok(size_t{0});
        }
        return make_error(StatusCode::IO_ERROR, std::string("send(): ") + std::strerror(errno));
#endif
    }

    write_buffer_.erase(write_buffer_.begin(), write_buffer_.begin() + n);
    return ok(static_cast<size_t>(n));
}

void Connection::enqueue_write(const uint8_t* data, size_t len) {
    write_buffer_.insert(write_buffer_.end(), data, data + len);
}

void Connection::enqueue_write(const std::string& data) {
    enqueue_write(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

void Connection::consume_read(size_t len) {
    if (len >= read_buffer_.size()) {
        read_buffer_.clear();
    } else {
        read_buffer_.erase(read_buffer_.begin(),
                           read_buffer_.begin() + static_cast<ptrdiff_t>(len));
    }
}

void Connection::close() {
    if (fd_ >= 0) {
        sixseven_platform::socket_close(fd_);
        fd_ = -1;
    }
    state_ = ConnectionState::CLOSED;
}

} // namespace sixseven
