/// GDB-965 QA: body_len < 4 underflow guard in handle_frontend_message and
/// handle_auth_message.
///
/// When a client sends a message whose 4-byte body-length field contains a
/// value in [0, 3], the subtraction `body_len - 4` would underflow to ~SIZE_MAX
/// on an unguarded path, causing the handler to construct a string_view over
/// gigabytes of unowned memory (out-of-bounds read / crash / DoS).
///
/// The fix adds an early-return guard in both handle_frontend_message and
/// handle_auth_message that sends an ErrorResponse (FATAL, SQLSTATE 08P01)
/// and closes the connection before the subtraction executes.
///
/// This test exercises the live framing path via a real socketpair, which
/// crashes under the Windows CRT fd-assert.  The entire socket-driving body is
/// therefore wrapped in #ifndef _WIN32 / GTEST_SKIP so the test builds and
/// skips cleanly on Windows while running fully on POSIX / CI.

#include "sixseven/common/platform.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/server/connection.h"
#include "sixseven/server/pg_protocol.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Wire helpers (local copies avoid cross-TU linkage issues).
// ---------------------------------------------------------------------------

#ifndef _WIN32

static int gdb965_create_socketpair(int& client_fd_out) {
    int fds[2];
    int rc = sixseven_platform::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    if (rc != 0) {
        client_fd_out = -1;
        return -1;
    }
    client_fd_out = fds[1];
    return fds[0];
}

static void gdb965_write_fd(int fd, const std::vector<uint8_t>& data) {
    size_t written = 0;
    while (written < data.size()) {
        auto n = ::write(fd, data.data() + written, data.size() - written);
        ASSERT_GT(n, 0);
        written += static_cast<size_t>(n);
    }
}

static std::vector<uint8_t> gdb965_read_fd(int fd) {
    std::vector<uint8_t> buf(16384);
    auto n = ::read(fd, buf.data(), buf.size());
    if (n <= 0)
        return {};
    buf.resize(static_cast<size_t>(n));
    return buf;
}

// Scan raw wire bytes for a message of the given type.  Returns true and sets
// payload/payload_len on success.
static bool gdb965_find_message(const std::vector<uint8_t>& data,
                                size_t& pos,
                                uint8_t type,
                                const uint8_t*& payload,
                                size_t& payload_len) {
    while (pos + 5 <= data.size()) {
        uint8_t msg_type = data[pos];
        uint32_t length = (static_cast<uint32_t>(data[pos + 1]) << 24) |
                          (static_cast<uint32_t>(data[pos + 2]) << 16) |
                          (static_cast<uint32_t>(data[pos + 3]) << 8) |
                          static_cast<uint32_t>(data[pos + 4]);
        size_t total = 1 + static_cast<size_t>(length);
        if (pos + total > data.size())
            return false;
        if (msg_type == type) {
            payload = data.data() + pos + 5;
            payload_len = (length >= 4) ? (length - 4) : 0;
            pos += total;
            return true;
        }
        pos += total;
    }
    return false;
}

// Extract a named field from an ErrorResponse payload.
static std::optional<std::string>
gdb965_error_field(const uint8_t* payload, size_t payload_len, uint8_t tag) {
    size_t i = 0;
    while (i < payload_len) {
        uint8_t t = payload[i];
        if (t == 0)
            break;
        ++i;
        size_t start = i;
        while (i < payload_len && payload[i] != 0)
            ++i;
        std::string val(reinterpret_cast<const char*>(payload + start), i - start);
        if (i < payload_len)
            ++i;
        if (t == tag)
            return val;
    }
    return std::nullopt;
}

// Build a minimal PG v3 StartupMessage (trust auth, no password needed).
static std::vector<uint8_t> gdb965_build_startup() {
    std::vector<uint8_t> payload;
    uint32_t version = 196608;
    payload.push_back(static_cast<uint8_t>((version >> 24) & 0xFF));
    payload.push_back(static_cast<uint8_t>((version >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((version >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(version & 0xFF));
    const char* k = "user";
    const char* v = "testuser";
    payload.insert(payload.end(), k, k + 4);
    payload.push_back(0);
    payload.insert(payload.end(), v, v + 8);
    payload.push_back(0);
    payload.push_back(0);
    uint32_t total = static_cast<uint32_t>(4 + payload.size());
    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>((total >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((total >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((total >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(total & 0xFF));
    msg.insert(msg.end(), payload.begin(), payload.end());
    return msg;
}

// Build a well-formed simple Query ('Q') message.
static std::vector<uint8_t> gdb965_build_query(const std::string& sql) {
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

// Build a crafted 'Q' message with an explicit (possibly underflowing) body_len.
static std::vector<uint8_t> gdb965_build_malformed_query(uint32_t body_len) {
    std::vector<uint8_t> msg;
    msg.push_back('Q');
    msg.push_back(static_cast<uint8_t>((body_len >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(body_len & 0xFF));
    // body_len bytes of payload follow the type byte.  body_len is in [0,3] so
    // we need to supply that many additional bytes so the framing check is
    // satisfied (buf.size() >= 1 + body_len).
    for (uint32_t i = 0; i < body_len; ++i) {
        msg.push_back(0x00);
    }
    return msg;
}

// Perform the startup handshake (trust auth) and drain the server response.
// Returns true on success.
static bool gdb965_do_startup(int server_fd,
                              int client_fd,
                              sixseven::Connection& conn,
                              sixseven::PgProtocolHandler& handler) {
    auto startup = gdb965_build_startup();
    gdb965_write_fd(client_fd, startup);
    auto r1 = conn.read_from_socket();
    if (!r1)
        return false;
    auto r2 = handler.process(conn);
    if (!r2)
        return false;
    auto r3 = conn.write_to_socket();
    if (!r3)
        return false;
    (void)gdb965_read_fd(client_fd); // drain AuthOk + ReadyForQuery
    (void)server_fd;
    return true;
}

#endif // !_WIN32

} // namespace

// ---------------------------------------------------------------------------
// GDB-965 AC: handle_frontend_message rejects body_len in [0,3] with 08P01
//
// This test is POSIX/CI-only.  On _WIN32 the socketpair-driven live path
// crashes under the CRT fd-assert, so the entire body is guarded.
// ---------------------------------------------------------------------------

TEST(GDB965, FrontendMessageBodyLenZeroEmitsError) {
#ifdef _WIN32
    GTEST_SKIP() << "pg-wire socket tests crash under the Windows CRT fd-assert; POSIX/CI only";
#else
    int client_fd = -1;
    int server_fd = gdb965_create_socketpair(client_fd);
    ASSERT_GE(server_fd, 0) << "socketpair failed";
    ASSERT_GE(client_fd, 0);

    sixseven::Connection conn(server_fd);
    sixseven::PgProtocolHandler handler(965);

    ASSERT_TRUE(gdb965_do_startup(server_fd, client_fd, conn, handler));

    // Send 'Q' with body_len=0 (total frame = 1+0 = 1 byte; framing passes but
    // body_len < 4, so the guard must fire before payload_len = body_len - 4).
    auto bad_msg = gdb965_build_malformed_query(0);
    gdb965_write_fd(client_fd, bad_msg);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();

    auto response = gdb965_read_fd(client_fd);
    ASSERT_FALSE(response.empty()) << "expected ErrorResponse for body_len=0";

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    ASSERT_TRUE(gdb965_find_message(response, pos, 'E', payload, payload_len))
        << "server must emit ErrorResponse ('E') for body_len=0";

    auto c = gdb965_error_field(payload, payload_len, 'C');
    ASSERT_TRUE(c.has_value()) << "ErrorResponse must have SQLSTATE (C) field";
    EXPECT_EQ(*c, "08P01") << "SQLSTATE must be 08P01 (protocol violation)";

    auto s = gdb965_error_field(payload, payload_len, 'S');
    ASSERT_TRUE(s.has_value()) << "ErrorResponse must have severity (S) field";
    EXPECT_EQ(*s, "FATAL") << "severity must be FATAL for framing errors";

    conn.close();
    ::close(client_fd);
#endif
}

TEST(GDB965, FrontendMessageBodyLenOneEmitsError) {
#ifdef _WIN32
    GTEST_SKIP() << "pg-wire socket tests crash under the Windows CRT fd-assert; POSIX/CI only";
#else
    int client_fd = -1;
    int server_fd = gdb965_create_socketpair(client_fd);
    ASSERT_GE(server_fd, 0) << "socketpair failed";
    ASSERT_GE(client_fd, 0);

    sixseven::Connection conn(server_fd);
    sixseven::PgProtocolHandler handler(965);

    ASSERT_TRUE(gdb965_do_startup(server_fd, client_fd, conn, handler));

    auto bad_msg = gdb965_build_malformed_query(1);
    gdb965_write_fd(client_fd, bad_msg);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();

    auto response = gdb965_read_fd(client_fd);
    ASSERT_FALSE(response.empty()) << "expected ErrorResponse for body_len=1";

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    ASSERT_TRUE(gdb965_find_message(response, pos, 'E', payload, payload_len));

    auto c = gdb965_error_field(payload, payload_len, 'C');
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(*c, "08P01");

    conn.close();
    ::close(client_fd);
#endif
}

TEST(GDB965, FrontendMessageBodyLenThreeEmitsError) {
#ifdef _WIN32
    GTEST_SKIP() << "pg-wire socket tests crash under the Windows CRT fd-assert; POSIX/CI only";
#else
    int client_fd = -1;
    int server_fd = gdb965_create_socketpair(client_fd);
    ASSERT_GE(server_fd, 0) << "socketpair failed";
    ASSERT_GE(client_fd, 0);

    sixseven::Connection conn(server_fd);
    sixseven::PgProtocolHandler handler(965);

    ASSERT_TRUE(gdb965_do_startup(server_fd, client_fd, conn, handler));

    auto bad_msg = gdb965_build_malformed_query(3);
    gdb965_write_fd(client_fd, bad_msg);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();

    auto response = gdb965_read_fd(client_fd);
    ASSERT_FALSE(response.empty()) << "expected ErrorResponse for body_len=3";

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    ASSERT_TRUE(gdb965_find_message(response, pos, 'E', payload, payload_len));

    auto c = gdb965_error_field(payload, payload_len, 'C');
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(*c, "08P01");

    conn.close();
    ::close(client_fd);
#endif
}

// Positive control: body_len=4 (minimum valid; payload_len=0) must NOT trigger
// the guard.  The handler processes the frame normally (may produce an error
// for empty SQL or unrecognized message, but NOT an 08P01 framing error AND
// must not crash).
TEST(GDB965, FrontendMessageBodyLenFourIsAccepted) {
#ifdef _WIN32
    GTEST_SKIP() << "pg-wire socket tests crash under the Windows CRT fd-assert; POSIX/CI only";
#else
    int client_fd = -1;
    int server_fd = gdb965_create_socketpair(client_fd);
    ASSERT_GE(server_fd, 0) << "socketpair failed";
    ASSERT_GE(client_fd, 0);

    sixseven::Connection conn(server_fd);
    sixseven::PgProtocolHandler handler(965);

    ASSERT_TRUE(gdb965_do_startup(server_fd, client_fd, conn, handler));

    // body_len=4 means payload_len=0, which is a zero-length Query (empty SQL).
    // The handler must not crash and must not send an 08P01 framing error.
    auto minimal_msg = gdb965_build_malformed_query(4);
    gdb965_write_fd(client_fd, minimal_msg);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();

    auto response = gdb965_read_fd(client_fd);
    // Any response (ReadyForQuery, ErrorResponse for empty SQL, etc.) is
    // acceptable -- the only hard requirement is that the SQLSTATE is NOT 08P01
    // and the process did not crash.
    if (!response.empty()) {
        size_t pos = 0;
        const uint8_t* payload = nullptr;
        size_t payload_len = 0;
        if (gdb965_find_message(response, pos, 'E', payload, payload_len)) {
            auto c = gdb965_error_field(payload, payload_len, 'C');
            if (c.has_value()) {
                EXPECT_NE(*c, "08P01") << "body_len=4 is valid framing; must not produce 08P01";
            }
        }
    }

    conn.close();
    ::close(client_fd);
#endif
}
