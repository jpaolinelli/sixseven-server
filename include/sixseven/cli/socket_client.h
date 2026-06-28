#pragma once

// socket_client.h -- Thin blocking TCP socket wrapper for the CLI.
//
// This is the ONLY file in the CLI subsystem that touches real sockets.
// The codec and formatter are completely independent of this header.
//
// On Windows: uses Winsock2 (ws2_32.lib linked by the cli CMake target).
// On POSIX: uses BSD sockets.
//
// NOTE: Under the MSVC CRT test harness (unit tests) sockets are not
//       exercised -- the live-connect tests must GTEST_SKIP on Windows.

#include "sixseven/cli/pg_wire_codec.h"
#include "sixseven/common/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sixseven::cli {

/// A blocking TCP client that speaks pg-wire v3 to SixSevenDB.
class SocketClient {
public:
    SocketClient() = default;
    ~SocketClient();

    // Non-copyable, movable.
    SocketClient(const SocketClient&) = delete;
    SocketClient& operator=(const SocketClient&) = delete;
    SocketClient(SocketClient&&) noexcept;
    SocketClient& operator=(SocketClient&&) noexcept;

    /// Connect to host:port (blocking).
    [[nodiscard]] Result<void> connect(const std::string& host, uint16_t port);

    /// Disconnect (no-op if not connected).
    void disconnect();

    bool is_connected() const;

    /// Send raw bytes to the server.
    [[nodiscard]] Result<void> send_bytes(const std::vector<uint8_t>& data);

    /// Read exactly one complete server message (blocks until enough data).
    [[nodiscard]] Result<ServerMessage> read_message();

    /// Perform the startup handshake (startup message + auth).
    /// Returns ok() only when ReadyForQuery is received after AuthOk.
    /// Returns error if the server requires MD5 or SCRAM (not implemented).
    [[nodiscard]] Result<void> startup(const std::string& user, const std::string& database);

private:
    int fd_{-1};
    std::vector<uint8_t> recv_buf_;

    [[nodiscard]] Result<void> fill_buffer(size_t need);
};

} // namespace sixseven::cli
