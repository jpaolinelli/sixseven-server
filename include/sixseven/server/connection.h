#pragma once

#include "sixseven/common/result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sixseven {

/// Connection lifecycle states.
enum class ConnectionState : uint8_t {
    INIT,
    AUTH,
    READY,
    QUERY,
    CLOSED,
};

/// Returns a human-readable name for a ConnectionState.
const char* connection_state_name(ConnectionState state);

/// Per-client connection holding socket, state, and I/O buffers.
class Connection {
public:
    explicit Connection(int fd);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    /// Transition to a new state. Returns error on invalid transitions.
    [[nodiscard]] Result<void> transition_to(ConnectionState new_state);

    /// Read available data from the socket into the read buffer.
    /// Returns std::nullopt on EAGAIN (no data available, try later),
    /// 0 on EOF (peer closed), or N > 0 bytes read.
    [[nodiscard]] Result<std::optional<size_t>> read_from_socket();

    /// Write pending data from the write buffer to the socket.
    /// Returns the number of bytes written.
    [[nodiscard]] Result<size_t> write_to_socket();

    /// Append data to the write buffer for later flushing.
    void enqueue_write(const uint8_t* data, size_t len);
    void enqueue_write(const std::string& data);

    /// Consume bytes from the front of the read buffer.
    void consume_read(size_t len);

    int fd() const { return fd_; }
    ConnectionState state() const { return state_; }

    const std::vector<uint8_t>& read_buffer() const { return read_buffer_; }
    bool has_pending_writes() const { return !write_buffer_.empty(); }

    /// Close the underlying socket. Transitions to CLOSED.
    void close();

private:
    int fd_;
    ConnectionState state_;
    std::vector<uint8_t> read_buffer_;
    std::vector<uint8_t> write_buffer_;
};

} // namespace sixseven
