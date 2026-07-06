/// GDB-1244 QA: The ErrorResponse 'P' (Position) field must be a 1-based
/// CHARACTER index into the original query string, per the PostgreSQL wire
/// protocol spec -- NOT a byte offset. For ASCII-only queries these are
/// identical (see GDB-750), but a query with multi-byte UTF-8 characters
/// before the error token must NOT have its caret drawn at the byte offset.
///
/// We test at the pg_protocol layer using a real socketpair + PgProtocolHandler
/// so that a regression re-introducing the raw byte offset is caught on the
/// wire, not just in a helper unit test.

#include "sixseven/common/platform.h"
#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/server/connection.h"
#include "sixseven/server/pg_protocol.h"

#include <gtest/gtest.h>

#if defined(_WIN32)
using gdb1244_socket_handle = SOCKET;
#else
using gdb1244_socket_handle = int;
#endif

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sixseven {

// Forward declarations -- exposed from pg_protocol.cpp for QA testing.
std::string_view status_to_sqlstate(StatusCode code);
uint32_t byte_offset_to_char_index_1based(std::string_view query, uint32_t byte_offset_1based);

} // namespace sixseven

using namespace sixseven;

namespace {

int gdb1244_create_socketpair(int& client_fd_out) {
    int fds[2];
    int rc = sixseven_platform::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    EXPECT_EQ(rc, 0);
    client_fd_out = fds[1];
    return fds[0];
}

void gdb1244_write_fd(int fd, const std::vector<uint8_t>& data) {
    size_t written = 0;
    while (written < data.size()) {
        auto n = ::send(static_cast<gdb1244_socket_handle>(fd),
                        reinterpret_cast<const char*>(data.data() + written),
                        static_cast<int>(data.size() - written),
                        0);
        ASSERT_GT(n, 0);
        written += static_cast<size_t>(n);
    }
}

std::vector<uint8_t> gdb1244_read_fd(int fd) {
    std::vector<uint8_t> buf(16384);
    auto n = ::recv(static_cast<gdb1244_socket_handle>(fd),
                    reinterpret_cast<char*>(buf.data()),
                    static_cast<int>(buf.size()),
                    0);
    if (n <= 0)
        return {};
    buf.resize(static_cast<size_t>(n));
    return buf;
}

bool gdb1244_find_message(const std::vector<uint8_t>& data,
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

std::optional<std::string>
gdb1244_error_field(const uint8_t* payload, size_t payload_len, uint8_t field_tag) {
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

std::vector<uint8_t> gdb1244_build_startup() {
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

std::vector<uint8_t> gdb1244_build_query(std::string_view sql) {
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

/// Runs a query through a fresh PgProtocolHandler/Connection pair and returns
/// the raw wire bytes of the resulting ErrorResponse ('E') message, or an
/// empty vector if none was found.
std::vector<uint8_t> gdb1244_run_query_get_error_response(std::string_view sql) {
    int client_fd = -1;
    int server_fd = gdb1244_create_socketpair(client_fd);

    sixseven::Connection conn(server_fd);
    sixseven::PgProtocolHandler handler(1244);

    handler.set_query_executor(
        [](const std::string& query_sql,
           const std::string& /*database*/) -> sixseven::Result<sixseven::QueryResult> {
            sixseven::Lexer lex(query_sql);
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

    auto startup = gdb1244_build_startup();
    gdb1244_write_fd(client_fd, startup);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)gdb1244_read_fd(client_fd); // drain startup response

    auto query = gdb1244_build_query(sql);
    gdb1244_write_fd(client_fd, query);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();

    auto response = gdb1244_read_fd(client_fd);

    conn.close();
    sixseven_platform::socket_close(client_fd);

    return response;
}

} // namespace

// ---------------------------------------------------------------------------
// AC: helper conversion behaves correctly on golden byte/char vectors
// ---------------------------------------------------------------------------

TEST(GDB1244, HelperAsciiByteOffsetEqualsCharIndex) {
    // ASCII-only prefix: byte offset and char index must be identical.
    std::string_view sql = "SELECT FROM users";
    // FROM begins at 1-based byte offset 8 (see GDB-750).
    EXPECT_EQ(byte_offset_to_char_index_1based(sql, 8u), 8u);
}

TEST(GDB1244, HelperMultiByteUtf8PrefixCountsCharsNotBytes) {
    // "SELECT 'café' FROM users" -- 'é' (U+00E9) is a 2-byte UTF-8 sequence.
    // FROM starts at 1-based BYTE offset 16, but 1-based CHARACTER index 15.
    std::string sql = "SELECT 'caf\xC3\xA9' FROM users";
    EXPECT_EQ(byte_offset_to_char_index_1based(sql, 16u), 15u);
}

TEST(GDB1244, HelperOffsetAtStringStartIsOne) {
    std::string_view sql = "anything";
    EXPECT_EQ(byte_offset_to_char_index_1based(sql, 1u), 1u);
}

TEST(GDB1244, HelperOffsetPastEndClampsDefensively) {
    std::string_view sql = "abc";
    // Offset far past the end must not read out of bounds; result should be
    // the full character count of the string plus one.
    EXPECT_EQ(byte_offset_to_char_index_1based(sql, 1000u), 4u);
}

TEST(GDB1244, HelperOffsetZeroTreatedAsOne) {
    std::string_view sql = "abc";
    EXPECT_EQ(byte_offset_to_char_index_1based(sql, 0u), 1u);
}

TEST(GDB1244, HelperMultiByteThreeByteSequence) {
    // U+20AC (EURO SIGN) encodes as 3 bytes in UTF-8: 0xE2 0x82 0xAC.
    // "SELECT '€' FROM" -- verify FROM's exact byte position directly rather
    // than trusting hand arithmetic, then check the char-index conversion.
    std::string sql = "SELECT '\xE2\x82\xAC' FROM";
    auto from_byte_pos = sql.find("FROM");
    ASSERT_NE(from_byte_pos, std::string::npos);
    auto byte_offset_1based = static_cast<uint32_t>(from_byte_pos + 1);
    // Characters before FROM: S-E-L-E-C-T-sp-'-euro-'-sp = 11 characters,
    // so FROM is the 12th character (1-based).
    EXPECT_EQ(byte_offset_to_char_index_1based(sql, byte_offset_1based), 12u);
}

// ---------------------------------------------------------------------------
// AC: ASCII behavior UNCHANGED -- existing GDB-750 wire test must stay green;
// this is an additional pinned wire-level ASCII check scoped to GDB-1244.
// ---------------------------------------------------------------------------

TEST(GDB1244, WireAsciiPFieldUnchanged) {
    auto response = gdb1244_run_query_get_error_response("SELECT FROM users");
    ASSERT_FALSE(response.empty()) << "expected wire response after parse error";

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    ASSERT_TRUE(gdb1244_find_message(response, pos, 'E', payload, payload_len));

    auto p_field = gdb1244_error_field(payload, payload_len, 'P');
    ASSERT_TRUE(p_field.has_value());
    // ASCII: byte offset == char index == 8 (unchanged from GDB-750).
    EXPECT_EQ(*p_field, "8");
}

// ---------------------------------------------------------------------------
// AC: multi-byte UTF-8 query pins the exact emitted 'P' value to the
// CHARACTER index, not the byte offset.
// ---------------------------------------------------------------------------

TEST(GDB1244, WireMultiByteUtf8PFieldIsCharacterIndex) {
    // "SELECT 'café' WHERE FROM users" -- é is a 2-byte UTF-8 sequence.
    // The parser accepts "WHERE" as the start of a WHERE clause but then
    // finds the second "FROM" where it expects an expression, so the parse
    // error is reported at that second FROM. Compute the expected byte
    // offset and character index directly from the string rather than hand
    // arithmetic, so the test documents its own ground truth.
    std::string sql = "SELECT 'caf\xC3\xA9' WHERE FROM users";
    auto from_byte_pos = sql.find("FROM");
    ASSERT_NE(from_byte_pos, std::string::npos);

    // Count UTF-8 characters (non-continuation bytes) before FROM to derive
    // the expected 1-based character index independently of the production
    // helper under test.
    uint32_t expected_char_index = 1;
    for (size_t i = 0; i < from_byte_pos; ++i) {
        auto b = static_cast<unsigned char>(sql[i]);
        if ((b & 0xC0) != 0x80) {
            ++expected_char_index;
        }
    }
    // Sanity: byte offset and char index must actually differ for this query
    // (otherwise the test wouldn't catch a byte-vs-char regression).
    auto expected_byte_offset_1based = static_cast<uint32_t>(from_byte_pos + 1);
    ASSERT_NE(expected_byte_offset_1based, expected_char_index)
        << "test query must contain a multi-byte prefix so byte offset != char index";

    auto response = gdb1244_run_query_get_error_response(sql);
    ASSERT_FALSE(response.empty()) << "expected wire response after parse error";

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    ASSERT_TRUE(gdb1244_find_message(response, pos, 'E', payload, payload_len))
        << "ErrorResponse must be present in wire response";

    auto c_field = gdb1244_error_field(payload, payload_len, 'C');
    ASSERT_TRUE(c_field.has_value());
    EXPECT_EQ(*c_field, "42601");

    auto p_field = gdb1244_error_field(payload, payload_len, 'P');
    ASSERT_TRUE(p_field.has_value())
        << "P (position) field must be present in ErrorResponse for parse errors";
    // Pin the exact emitted value: must equal the CHARACTER index of the
    // erroring token (computed above), not its raw byte offset. A regression
    // back to raw byte_offset would emit the byte offset here and fail.
    EXPECT_EQ(*p_field, std::to_string(expected_char_index))
        << "Wire P value must be the CHARACTER index (" << expected_char_index
        << "), not the byte offset (" << expected_byte_offset_1based << "); got '" << *p_field
        << "'";
}
