#pragma once

// tcp_replication_connection.h -- Concrete TCP transport for replication.
//
// Implements ReplicationConnection over a blocking TCP socket.
// Cross-platform: Winsock2 on Windows, BSD sockets on POSIX.
// Pattern mirrors src/cli/socket_client.cpp (GDB-951).
//
// WINDOWS NOTE: The live two-node round-trip (connect -> stream) is
// unverifiable under the MSVC CRT unit-test harness due to the CRT
// fd-assert in Session/PgProtocol.  Construction, connect-to-closed-port,
// and unconnected send/receive are testable without a live peer.

#include "sixseven/common/result.h"
#include "sixseven/server/replication_connection.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace sixseven {

/// Concrete TCP transport implementing ReplicationConnection.
///
/// connect() opens a blocking socket to the primary.
/// send() writes all bytes (handles partial writes).
/// receive(max, timeout) does a timed recv; returns empty vector on timeout,
///   error on disconnect/reset -- matching WalReceiver expectations.
/// close() shuts down and closes the fd plus platform cleanup.
class TcpReplicationConnection : public ReplicationConnection {
public:
    TcpReplicationConnection();
    ~TcpReplicationConnection() override;

    // Non-copyable, movable.
    TcpReplicationConnection(const TcpReplicationConnection&) = delete;
    TcpReplicationConnection& operator=(const TcpReplicationConnection&) = delete;
    TcpReplicationConnection(TcpReplicationConnection&&) noexcept;
    TcpReplicationConnection& operator=(TcpReplicationConnection&&) noexcept;

    /// Connect to host:port (blocking).
    [[nodiscard]] Result<void> connect(const std::string& host, uint16_t port);

    // ReplicationConnection interface -------------------------------------------

    /// Send all bytes to the primary. Handles partial writes.
    [[nodiscard]] Result<void> send(std::span<const uint8_t> data) override;

    /// Receive up to max_bytes within timeout.
    /// Returns empty vector on timeout; error on disconnect/reset.
    [[nodiscard]] Result<std::vector<uint8_t>> receive(size_t max_bytes,
                                                       std::chrono::milliseconds timeout) override;

    /// Shut down and close the socket.
    void close() override;

    /// True if the socket is open.
    [[nodiscard]] bool is_open() const override;

    /// Human-readable description of the peer (e.g., "127.0.0.1:5433").
    [[nodiscard]] std::string peer_description() const override;

private:
    int fd_{-1};
    std::string peer_desc_;
};

/// Factory helper: constructs a connected TcpReplicationConnection.
///
/// Returns an error if host is empty or the TCP connect fails.
[[nodiscard]] Result<std::unique_ptr<ReplicationConnection>>
make_tcp_replication_connection(const std::string& host, uint16_t port);

} // namespace sixseven
