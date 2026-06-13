/// GDB-750 QA: Verify ErrorResponse wire bytes contain correct SQLSTATE ('C' field)
/// and Position ('P' field) for syntax errors in the simple-query protocol.
///
/// We test at the pg_protocol layer by encoding an ErrorResponse and inspecting
/// the raw bytes, avoiding the need for a full network round-trip.

#include "sixseven/common/platform.h"
#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/server/connection.h"
#include "sixseven/server/pg_protocol.h"

#include <gtest/gtest.h>

// Portable socket-handle type: on Windows the socket APIs (send/recv/closesocket)
// take a SOCKET; on POSIX they take a plain int file descriptor. SOCKET is only
// defined when <winsock2.h> is pulled in (i.e. on _WIN32), so guard the alias to
// keep this QA test compiling on macOS/Linux. See GDB-1255.
#if defined(_WIN32)
using gdb750_socket_handle = SOCKET;
#else
using gdb750_socket_handle = int;
#endif

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sixseven {

// Forward declarations — exposed from pg_protocol.cpp for QA testing.
std::string_view status_to_sqlstate(StatusCode code);

} // namespace sixseven

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helper: parse a query and return the Error (expects failure).
// ---------------------------------------------------------------------------

static Error parse_expect_error(const std::string& sql) {
    Lexer lex(sql);
    auto tokens = lex.tokenize();
    if (!tokens.has_value()) {
        return tokens.error();
    }
    Parser parser(std::move(*tokens));
    auto stmt = parser.parse();
    if (stmt.has_value()) {
        // Return a fake error so the test can assert later.
        return Error(StatusCode::OK, "unexpectedly succeeded");
    }
    return stmt.error();
}

// ---------------------------------------------------------------------------
// GDB-750 AC1: Syntax error carries a Position ('P') pointing at the bad token
// ---------------------------------------------------------------------------

TEST(GDB750, SyntaxErrorHasQueryPos) {
    // "SELECT FROM users" — FROM is the bad token at byte 8 (1-based).
    auto err = parse_expect_error("SELECT FROM users");
    EXPECT_EQ(err.code, StatusCode::PARSE_ERROR) << "expected PARSE_ERROR, got: " << err.message;
    ASSERT_TRUE(err.query_pos.has_value())
        << "parse error must carry query_pos; message was: " << err.message;
    EXPECT_EQ(*err.query_pos, 8u) << "FROM in 'SELECT FROM users' starts at byte offset 8";
}

TEST(GDB750, SyntaxErrorPositionIsPositive) {
    auto err = parse_expect_error("@@");
    // Lexer will fail here; if it passes the parser then position must be >= 1.
    if (err.code == StatusCode::PARSE_ERROR && err.query_pos.has_value()) {
        EXPECT_GE(*err.query_pos, 1u);
    }
    // Either the lexer or the parser must reject this; either way not OK.
    EXPECT_NE(err.code, StatusCode::OK);
}

TEST(GDB750, PositionPointsInsideQuery) {
    std::string sql = "SELECT id FROM";
    auto err = parse_expect_error(sql);
    EXPECT_EQ(err.code, StatusCode::PARSE_ERROR);
    ASSERT_TRUE(err.query_pos.has_value());
    EXPECT_GE(*err.query_pos, 1u);
    EXPECT_LE(*err.query_pos, static_cast<uint32_t>(sql.size() + 1));
}

// ---------------------------------------------------------------------------
// GDB-750 AC2: SQLSTATE mapping — all codes produce a 5-char PG code,
// key codes spot-checked
// ---------------------------------------------------------------------------

TEST(GDB750, SqlstateMappingCompleteness) {
    const StatusCode all_codes[] = {
        StatusCode::OK,
        StatusCode::NOT_FOUND,
        StatusCode::ALREADY_EXISTS,
        StatusCode::INVALID_ARGUMENT,
        StatusCode::INTERNAL_ERROR,
        StatusCode::NOT_IMPLEMENTED,
        StatusCode::IO_ERROR,
        StatusCode::PARSE_ERROR,
        StatusCode::TYPE_ERROR,
        StatusCode::CONSTRAINT_VIOLATION,
        StatusCode::TXN_CONFLICT,
        StatusCode::TXN_ABORTED,
        StatusCode::NETWORK_ERROR,
        StatusCode::AUTH_ERROR,
        StatusCode::REPLICATION_ERROR,
        StatusCode::READ_ONLY,
        StatusCode::LOCK_TIMEOUT,
        StatusCode::DEADLOCK,
        StatusCode::QUERY_CANCELED,
    };
    for (const auto code : all_codes) {
        auto state = status_to_sqlstate(code);
        EXPECT_EQ(state.size(), 5u) << "SQLSTATE for " << status_code_name(code)
                                    << " must be 5 chars, got '" << state << "'";
    }
}

TEST(GDB750, SqlstateParseErrorIs42601) {
    EXPECT_EQ(status_to_sqlstate(StatusCode::PARSE_ERROR), "42601")
        << "syntax error must map to 42601";
}

TEST(GDB750, SqlstateInternalErrorIsXX000) {
    EXPECT_EQ(status_to_sqlstate(StatusCode::INTERNAL_ERROR), "XX000");
}

TEST(GDB750, SqlstateTypeErrorIs42804) {
    EXPECT_EQ(status_to_sqlstate(StatusCode::TYPE_ERROR), "42804");
}

// ---------------------------------------------------------------------------
// GDB-750 wire-level: P field in ErrorResponse emitted by send_error_response
// carries the exact query_pos value.  Uses a real socketpair + PgProtocolHandler
// so that a mutation of the emit site (e.g. *position - 1) is caught.
// ---------------------------------------------------------------------------

namespace {

// Helpers duplicated locally to avoid cross-TU linkage issues.

int gdb750_create_socketpair(int& client_fd_out) {
    int fds[2];
    int rc = sixseven_platform::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    EXPECT_EQ(rc, 0);
    client_fd_out = fds[1];
    return fds[0];
}

void gdb750_write_fd(int fd, const std::vector<uint8_t>& data) {
    size_t written = 0;
    while (written < data.size()) {
        auto n = ::send(static_cast<gdb750_socket_handle>(fd),
                        reinterpret_cast<const char*>(data.data() + written),
                        static_cast<int>(data.size() - written),
                        0);
        ASSERT_GT(n, 0);
        written += static_cast<size_t>(n);
    }
}

std::vector<uint8_t> gdb750_read_fd(int fd) {
    std::vector<uint8_t> buf(16384);
    auto n = ::recv(static_cast<gdb750_socket_handle>(fd),
                    reinterpret_cast<char*>(buf.data()),
                    static_cast<int>(buf.size()),
                    0);
    if (n <= 0)
        return {};
    buf.resize(static_cast<size_t>(n));
    return buf;
}

// Scan raw wire bytes for the next message of the given type byte.
// Returns true and sets payload/payload_len on success.
bool gdb750_find_message(const std::vector<uint8_t>& data,
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
            payload_len = length - 4;
            pos += total;
            return true;
        }
        pos += total;
    }
    return false;
}

// Scan an ErrorResponse payload for a field tag and return its cstring value.
std::optional<std::string>
gdb750_error_field(const uint8_t* payload, size_t payload_len, uint8_t field_tag) {
    size_t i = 0;
    while (i < payload_len) {
        uint8_t tag = payload[i];
        if (tag == 0)
            break;
        ++i;
        size_t start = i;
        while (i < payload_len && payload[i] != 0)
            ++i;
        std::string value(reinterpret_cast<const char*>(payload + start), i - start);
        if (i < payload_len)
            ++i; // consume null
        if (tag == field_tag)
            return value;
    }
    return std::nullopt;
}

std::vector<uint8_t> gdb750_build_startup() {
    std::vector<uint8_t> payload;
    uint32_t version = 196608;
    payload.push_back(static_cast<uint8_t>((version >> 24) & 0xFF));
    payload.push_back(static_cast<uint8_t>((version >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((version >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(version & 0xFF));
    const char* key = "user";
    const char* val = "test";
    payload.insert(payload.end(), key, key + 4);
    payload.push_back(0);
    payload.insert(payload.end(), val, val + 4);
    payload.push_back(0);
    payload.push_back(0); // terminator
    uint32_t total = static_cast<uint32_t>(4 + payload.size());
    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>((total >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((total >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((total >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(total & 0xFF));
    msg.insert(msg.end(), payload.begin(), payload.end());
    return msg;
}

std::vector<uint8_t> gdb750_build_query(std::string_view sql) {
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

} // namespace

// Full-wire test: send a syntactically invalid query through PgProtocolHandler,
// capture the raw ErrorResponse, and verify the P field equals exactly 8 (the
// byte offset of FROM in "SELECT FROM users").  A mutation of *position to
// *position - 1 in send_error_response will produce "7" and fail this test.
TEST(GDB750, WireErrorResponsePFieldExact) {
    int client_fd = -1;
    int server_fd = gdb750_create_socketpair(client_fd);

    sixseven::Connection conn(server_fd);
    sixseven::PgProtocolHandler handler(750);

    // Minimal executor: parses the SQL and forwards parse errors, so that
    // send_error_response is called with the correct query_pos.
    handler.set_query_executor(
        [](const std::string& sql,
           const std::string& /*database*/) -> sixseven::Result<sixseven::QueryResult> {
            sixseven::Lexer lex(sql);
            auto tokens = lex.tokenize();
            if (!tokens.has_value()) {
                return tl::unexpected(tokens.error());
            }
            sixseven::Parser parser(std::move(*tokens));
            auto stmt = parser.parse();
            if (!stmt.has_value()) {
                return tl::unexpected(stmt.error());
            }
            return sixseven::make_error(sixseven::StatusCode::NOT_IMPLEMENTED,
                                        "test executor: execution not supported");
        });

    // Startup handshake.
    auto startup = gdb750_build_startup();
    gdb750_write_fd(client_fd, startup);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)gdb750_read_fd(client_fd); // drain startup response

    // Send "SELECT FROM users" — parser will fail with query_pos=8 (FROM at byte 8).
    auto query = gdb750_build_query("SELECT FROM users");
    gdb750_write_fd(client_fd, query);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();

    auto response = gdb750_read_fd(client_fd);
    ASSERT_FALSE(response.empty()) << "expected wire response after parse error";

    // Find the ErrorResponse ('E') message.
    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    ASSERT_TRUE(gdb750_find_message(response, pos, 'E', payload, payload_len))
        << "ErrorResponse must be present in wire response";

    // Verify SQLSTATE field ('C') is 42601.
    auto c_field = gdb750_error_field(payload, payload_len, 'C');
    ASSERT_TRUE(c_field.has_value()) << "C (SQLSTATE) field must be present";
    EXPECT_EQ(*c_field, "42601") << "parse error SQLSTATE must be 42601";

    // Verify P field equals exactly "8".
    // FROM starts at byte 8 in "SELECT FROM users" (1-based).
    // A position-1 mutation would emit "7"; position+1 would emit "9".
    auto p_field = gdb750_error_field(payload, payload_len, 'P');
    ASSERT_TRUE(p_field.has_value())
        << "P (position) field must be present in ErrorResponse for parse errors";
    EXPECT_EQ(*p_field, "8")
        << "Wire P value must be exactly 8; got '" << *p_field
        << "' — a mutation of send_error_response (e.g. *position-1) would produce 7";

    conn.close();
    sixseven_platform::socket_close(client_fd);
}
