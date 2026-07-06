#pragma once

// Shared PG v3 wire-protocol test helpers for unit tests.
//
// Included by test_pg_protocol.cpp and test_param_substitution.cpp.
// All functions are inline to avoid ODR/duplicate-symbol link errors when
// this header is included in multiple translation units.
//
// BYTE CONTRACT: The wire bytes produced by each builder are identical to the
// original per-file copies.  Do not change the framing without updating both
// call sites and bumping this comment.

#include "sixseven/common/platform.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pg_wire_test {

/// Ensure platform-level socket init (e.g. Winsock WSAStartup) has run before
/// any socket operation. Safe to call repeatedly; the underlying init runs
/// exactly once (static-local initialization is thread-safe and lazy).
inline bool ensure_platform_init() {
    static const bool initialized = sixseven::platform_init();
    return initialized;
}

/// Create a Unix-domain socketpair.  Returns the server-side fd; stores the
/// client-side fd in client_fd_out.
inline int create_socketpair(int& client_fd_out) {
    ensure_platform_init();
    int fds[2];
    int rc = sixseven_platform::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    EXPECT_EQ(rc, 0);
    client_fd_out = fds[1];
    return fds[0]; // Server side.
}

/// Write all bytes in data to fd (retrying on short sends). Uses ::send
/// instead of the CRT ::write because on Windows create_socketpair() returns
/// raw SOCKET handles (via loopback TCP emulation), which are NOT CRT file
/// descriptors -- calling ::write on them aborts the process
/// (write.cpp(50): fh >= 0 && (unsigned)fh < (unsigned)_nhandle).
inline void write_to_fd(int fd, const std::vector<uint8_t>& data) {
    size_t written = 0;
    while (written < data.size()) {
        auto n = ::send(fd,
                        reinterpret_cast<const char*>(data.data() + written),
                        static_cast<int>(data.size() - written),
                        0);
        ASSERT_GT(n, 0);
        written += static_cast<size_t>(n);
    }
}

/// Read up to max_bytes from fd into a vector. Uses ::recv instead of the CRT
/// ::read for the same reason as write_to_fd above.
inline std::vector<uint8_t> read_from_fd(int fd, size_t max_bytes = 8192) {
    std::vector<uint8_t> buf(max_bytes);
    auto n = ::recv(fd, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
    if (n <= 0) {
        return {};
    }
    buf.resize(static_cast<size_t>(n));
    return buf;
}

/// Build a PG v3 StartupMessage (no type byte; length field includes itself).
inline std::vector<uint8_t>
build_startup_message(const std::vector<std::pair<std::string, std::string>>& params) {
    std::vector<uint8_t> payload;

    // Protocol version 3.0 = 196608.
    uint32_t version = 196608;
    payload.push_back(static_cast<uint8_t>((version >> 24) & 0xFF));
    payload.push_back(static_cast<uint8_t>((version >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((version >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(version & 0xFF));

    for (const auto& [key, val] : params) {
        payload.insert(payload.end(), key.begin(), key.end());
        payload.push_back(0);
        payload.insert(payload.end(), val.begin(), val.end());
        payload.push_back(0);
    }
    payload.push_back(0); // Terminator.

    // Prepend length (includes itself).
    uint32_t total_len = static_cast<uint32_t>(4 + payload.size());
    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>((total_len >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((total_len >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((total_len >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(total_len & 0xFF));
    msg.insert(msg.end(), payload.begin(), payload.end());
    return msg;
}

/// Build a PG v3 Parse ('P') message.
inline std::vector<uint8_t> build_parse_message(std::string_view stmt_name,
                                                std::string_view sql,
                                                const std::vector<uint32_t>& param_oids = {}) {
    std::vector<uint8_t> body;
    body.insert(body.end(), stmt_name.begin(), stmt_name.end());
    body.push_back(0);
    body.insert(body.end(), sql.begin(), sql.end());
    body.push_back(0);
    auto num_params = static_cast<uint16_t>(param_oids.size());
    body.push_back(static_cast<uint8_t>((num_params >> 8) & 0xFF));
    body.push_back(static_cast<uint8_t>(num_params & 0xFF));
    for (uint32_t oid : param_oids) {
        body.push_back(static_cast<uint8_t>((oid >> 24) & 0xFF));
        body.push_back(static_cast<uint8_t>((oid >> 16) & 0xFF));
        body.push_back(static_cast<uint8_t>((oid >> 8) & 0xFF));
        body.push_back(static_cast<uint8_t>(oid & 0xFF));
    }

    std::vector<uint8_t> msg;
    msg.push_back('P');
    uint32_t body_len = static_cast<uint32_t>(4 + body.size());
    msg.push_back(static_cast<uint8_t>((body_len >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(body_len & 0xFF));
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

/// Build a PG v3 Bind ('B') message with optional NULL support.
/// Pass std::nullopt in param_values to send a NULL parameter (length = -1).
/// For non-null values the bytes produced are identical to the original
/// std::string version (4-byte big-endian length followed by the value bytes).
inline std::vector<uint8_t>
build_bind_message(std::string_view portal_name,
                   std::string_view stmt_name,
                   const std::vector<std::optional<std::string>>& param_values = {}) {
    std::vector<uint8_t> body;
    body.insert(body.end(), portal_name.begin(), portal_name.end());
    body.push_back(0);
    body.insert(body.end(), stmt_name.begin(), stmt_name.end());
    body.push_back(0);
    // 0 parameter format codes (use default text).
    body.push_back(0);
    body.push_back(0);
    // Number of parameters.
    auto num_params = static_cast<uint16_t>(param_values.size());
    body.push_back(static_cast<uint8_t>((num_params >> 8) & 0xFF));
    body.push_back(static_cast<uint8_t>(num_params & 0xFF));
    for (const auto& val : param_values) {
        if (!val.has_value()) {
            // NULL: length = -1 (0xFFFFFFFF as uint32).
            body.push_back(0xFF);
            body.push_back(0xFF);
            body.push_back(0xFF);
            body.push_back(0xFF);
        } else {
            auto len = static_cast<uint32_t>(val->size());
            body.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
            body.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
            body.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            body.push_back(static_cast<uint8_t>(len & 0xFF));
            body.insert(body.end(), val->begin(), val->end());
        }
    }
    // 0 result format codes (use default text).
    body.push_back(0);
    body.push_back(0);

    std::vector<uint8_t> msg;
    msg.push_back('B');
    uint32_t body_len = static_cast<uint32_t>(4 + body.size());
    msg.push_back(static_cast<uint8_t>((body_len >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(body_len & 0xFF));
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

/// Build a PG v3 Execute ('E') message.
inline std::vector<uint8_t> build_execute_message(std::string_view portal_name,
                                                  int32_t max_rows = 0) {
    std::vector<uint8_t> msg;
    msg.push_back('E');
    uint32_t body_len = static_cast<uint32_t>(4 + portal_name.size() + 1 + 4);
    msg.push_back(static_cast<uint8_t>((body_len >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(body_len & 0xFF));
    msg.insert(msg.end(), portal_name.begin(), portal_name.end());
    msg.push_back(0);
    auto mr = static_cast<uint32_t>(max_rows);
    msg.push_back(static_cast<uint8_t>((mr >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((mr >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((mr >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(mr & 0xFF));
    return msg;
}

/// Build a PG v3 Sync ('S') message.
inline std::vector<uint8_t> build_sync_message() {
    return {'S', 0, 0, 0, 4};
}

/// Build a PG v3 Query ('Q') message.
inline std::vector<uint8_t> build_query_message(std::string_view sql) {
    std::vector<uint8_t> msg;
    msg.push_back('Q');
    uint32_t body_len = static_cast<uint32_t>(4 + sql.size() + 1);
    msg.push_back(static_cast<uint8_t>((body_len >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(body_len & 0xFF));
    msg.insert(msg.end(), sql.begin(), sql.end());
    msg.push_back(0);
    return msg;
}

} // namespace pg_wire_test
