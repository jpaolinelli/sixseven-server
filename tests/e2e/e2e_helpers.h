/// e2e_helpers.h -- shared helpers for end-to-end tests (GDB-959).
///
/// Provides write_server_config, connect_with_retry, and wait_ready_for_query.
/// All helpers are POSIX-only and guarded by #ifndef _WIN32.
///
/// Ports used by the e2e test suite (static registry, POSIX/CI only):
///   16759 -- E2EDurabilityTest
///   16760 -- E2EReplicationTest primary
///   16761 -- E2EReplicationTest standby

#pragma once

#include "sixseven/cli/pg_wire_codec.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace sixseven::e2e {

#ifndef _WIN32

/// Write a minimal JSON config for a sixseven-server instance.
/// When standby=false, primary_host and primary_port are ignored.
inline void write_server_config(const fs::path& cfg_path,
                                const fs::path& data_dir,
                                int port,
                                bool standby = false,
                                const std::string& primary_host = "",
                                int primary_port = 0) {
    std::ofstream f(cfg_path);
    f << "{\n"
      << "  \"data_dir\": \"" << data_dir.string() << "\",\n"
      << "  \"port\": " << port << ",\n"
      << "  \"log_level\": \"warn\",\n"
      << "  \"auth_method\": \"trust\",\n"
      << "  \"buffer_pool_size_mb\": 32";
    if (standby) {
        f << ",\n"
          << "  \"standby_mode\": true,\n"
          << "  \"replication_primary_host\": \"" << primary_host << "\",\n"
          << "  \"replication_primary_port\": " << primary_port << ",\n"
          << "  \"replication_retry_interval_ms\": 500";
    }
    f << "\n}\n";
}

/// Attempt to connect to host:port, retrying until timeout_ms elapses.
/// Returns connected socket fd, or -1 on timeout/failure.
inline int connect_with_retry(const char* host, int port, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        ::inet_pton(AF_INET, host, &addr.sin_addr);
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return -1;
}

/// Blocking read until a pg-wire ReadyForQuery message (type byte == 'Z') is
/// fully received, or until timeout_ms elapses.
///
/// Uses decode_one_message to walk the message framing properly:
///   1 type byte  +  4-byte big-endian length  (length INCLUDES the 4 bytes,
///   NOT the type byte)  =  1 + length bytes total per message.
///
/// This correctly handles 'Z' (0x5A) appearing as a data byte inside
/// DataRow/CommandComplete/ErrorResponse payloads, eliminating the false-
/// positive that the previous byte-scan approach produced.
///
/// Appends all received bytes into out.  Returns true when ReadyForQuery is
/// seen; false on timeout or read error.
inline bool wait_ready_for_query(int fd, std::vector<uint8_t>& out, int timeout_ms) {
    std::vector<uint8_t> recv_buf(4096);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        auto n = ::recv(
            fd, reinterpret_cast<char*>(recv_buf.data()), static_cast<int>(recv_buf.size()), 0);
        if (n <= 0) {
            return false;
        }
        out.insert(out.end(), recv_buf.begin(), recv_buf.begin() + n);

        // Walk accumulated bytes as pg-wire framed messages.  Keep consuming
        // until we either find ReadyForQuery or run out of complete messages.
        size_t offset = 0;
        while (offset < out.size()) {
            size_t consumed = 0;
            // decode_one_message returns NOT_FOUND when the buffer holds a
            // partial message (need more data); any other error is fatal.
            auto msg_result = sixseven::cli::decode_one_message(
                std::vector<uint8_t>(out.begin() + static_cast<ptrdiff_t>(offset), out.end()),
                consumed);
            if (!msg_result) {
                // NOT_FOUND == partial message: break and read more bytes.
                // Any hard protocol error: also break (next recv will return 0
                // or we'll timeout).
                break;
            }
            if (msg_result->tag == sixseven::cli::ServerMsgTag::ReadyForQuery) {
                return true;
            }
            offset += consumed;
        }
    }
    return false;
}

#endif // !_WIN32

} // namespace sixseven::e2e
