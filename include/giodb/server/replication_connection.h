#pragma once

#include "giodb/common/result.h"

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace giodb {

/// Abstract connection for replication protocol.
///
/// TCP implementation will be added when the server module is built out
/// (Phase 6). Tests use an in-memory mock.
class ReplicationConnection {
public:
    virtual ~ReplicationConnection() = default;

    /// Send bytes to the replica. Blocks until all bytes are sent or an error.
    [[nodiscard]] virtual Result<void> send(std::span<const uint8_t> data) = 0;

    /// Receive up to max_bytes from the replica within the given timeout.
    /// Returns the bytes read (possibly fewer than max_bytes).
    /// Returns an empty vector if the timeout expires with no data.
    [[nodiscard]] virtual Result<std::vector<uint8_t>>
    receive(size_t max_bytes, std::chrono::milliseconds timeout) = 0;

    /// Close the connection.
    virtual void close() = 0;

    /// Check if the connection is still open.
    [[nodiscard]] virtual bool is_open() const = 0;

    /// Return a human-readable description of the peer (e.g., "127.0.0.1:5433").
    [[nodiscard]] virtual std::string peer_description() const = 0;
};

} // namespace giodb
