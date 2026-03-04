/// @file test_qa_gdb_146.cpp
/// @brief QA adversarial tests for GDB-146: PostgreSQL wire protocol v3 simple query flow.
///
/// Targets: MessageWriter/Reader edge cases, split_sql_statements edge cases,
/// value_to_pg_text edge cases, full protocol handshake + query via socketpair,
/// malformed messages, partial reads, multi-statement queries.

#include "giodb/common/result.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/query_engine.h"
#include "giodb/server/connection.h"
#include "giodb/server/pg_protocol.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace giodb;

// =============================================================================
// split_sql_statements adversarial tests
// =============================================================================

TEST(QA146SplitSql, EmptyString) {
    auto r = split_sql_statements("");
    EXPECT_TRUE(r.empty());
}

TEST(QA146SplitSql, WhitespaceOnly) {
    auto r = split_sql_statements("   \t\n\r  ");
    EXPECT_TRUE(r.empty());
}

TEST(QA146SplitSql, SingleStatementNoSemicolon) {
    auto r = split_sql_statements("SELECT 1");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0], "SELECT 1");
}

TEST(QA146SplitSql, MultipleSemicolons) {
    auto r = split_sql_statements(";;;");
    EXPECT_TRUE(r.empty());
}

TEST(QA146SplitSql, TrailingWhitespace) {
    auto r = split_sql_statements("  SELECT 1  ;  SELECT 2  ");
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0], "SELECT 1");
    EXPECT_EQ(r[1], "SELECT 2");
}

TEST(QA146SplitSql, SemicolonInsideSingleQuotedString) {
    auto r = split_sql_statements("SELECT 'a;b'");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0], "SELECT 'a;b'");
}

TEST(QA146SplitSql, SemicolonInsideDoubleQuotedIdentifier) {
    auto r = split_sql_statements("SELECT \"a;b\"");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0], "SELECT \"a;b\"");
}

TEST(QA146SplitSql, SemicolonInsideDollarQuoted) {
    auto r = split_sql_statements("SELECT $$ hello; world $$");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0], "SELECT $$ hello; world $$");
}

TEST(QA146SplitSql, EscapedSingleQuote) {
    // SQL standard: '' is an escaped single quote inside a string.
    auto r = split_sql_statements("SELECT 'it''s'; SELECT 2");
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0], "SELECT 'it''s'");
    EXPECT_EQ(r[1], "SELECT 2");
}

TEST(QA146SplitSql, UnterminatedSingleQuote) {
    // Unterminated quote — just consumes the rest.
    auto r = split_sql_statements("SELECT 'unterminated");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0], "SELECT 'unterminated");
}

TEST(QA146SplitSql, UnterminatedDoubleQuote) {
    auto r = split_sql_statements("SELECT \"unterminated");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0], "SELECT \"unterminated");
}

TEST(QA146SplitSql, UnterminatedDollarQuote) {
    auto r = split_sql_statements("SELECT $$unterminated");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0], "SELECT $$unterminated");
}

TEST(QA146SplitSql, ManyStatements) {
    std::string sql;
    for (int i = 0; i < 100; ++i) {
        sql += "SELECT " + std::to_string(i) + "; ";
    }
    auto r = split_sql_statements(sql);
    ASSERT_EQ(r.size(), 100u);
    EXPECT_EQ(r[99], "SELECT 99");
}

// =============================================================================
// MessageWriter / MessageReader adversarial tests
// =============================================================================

TEST(QA146MessageWriter, EmptyMessage) {
    MessageWriter w;
    w.begin_message('Z');
    auto buf = w.finish();
    // Type byte + 4-byte length (= 4, length includes itself).
    ASSERT_EQ(buf.size(), 5u);
    EXPECT_EQ(buf[0], 'Z');
    // Length should be 4 (just the length field itself).
    int32_t len = (buf[1] << 24) | (buf[2] << 16) | (buf[3] << 8) | buf[4];
    EXPECT_EQ(len, 4);
}

TEST(QA146MessageWriter, NoTypeMessage) {
    MessageWriter w;
    w.begin_message_no_type();
    w.write_int32(42);
    auto buf = w.finish();
    // 4-byte length + 4-byte int32.
    ASSERT_EQ(buf.size(), 8u);
    // Length field should be 8 (length of everything from length field onward).
    int32_t len = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
    EXPECT_EQ(len, 8);
}

TEST(QA146MessageWriter, CstringWithEmptyString) {
    MessageWriter w;
    w.begin_message('T');
    w.write_cstring("");
    auto buf = w.finish();
    // Type(1) + length(4) + null terminator(1) = 6.
    ASSERT_EQ(buf.size(), 6u);
    EXPECT_EQ(buf[5], 0); // Null terminator.
}

TEST(QA146MessageWriter, LargePayload) {
    MessageWriter w;
    w.begin_message('D');
    std::string large(10000, 'x');
    w.write_cstring(large);
    auto buf = w.finish();
    // Type(1) + length(4) + 10000 chars + null(1) = 10006.
    EXPECT_EQ(buf.size(), 10006u);
}

TEST(QA146MessageReader, ReadFromEmptyBuffer) {
    MessageReader r(nullptr, 0);
    EXPECT_EQ(r.read_byte(), 0);
    EXPECT_EQ(r.read_int16(), 0);
    EXPECT_EQ(r.read_int32(), 0);
    EXPECT_FALSE(r.has_remaining());
    EXPECT_EQ(r.remaining(), 0u);
}

TEST(QA146MessageReader, ReadBeyondEnd) {
    uint8_t data[] = {0x42};
    MessageReader r(data, 1);
    EXPECT_EQ(r.read_byte(), 0x42);
    // Now past end.
    EXPECT_EQ(r.read_byte(), 0);
    EXPECT_EQ(r.read_int16(), 0);
    EXPECT_EQ(r.read_int32(), 0);
    EXPECT_EQ(r.read_bytes(1), nullptr);
}

TEST(QA146MessageReader, CstringNoNullTerminator) {
    // Data without null terminator — should read to end.
    uint8_t data[] = {'h', 'e', 'l', 'l', 'o'};
    MessageReader r(data, 5);
    auto sv = r.read_cstring();
    EXPECT_EQ(sv, "hello");
    EXPECT_FALSE(r.has_remaining());
}

TEST(QA146MessageReader, RoundTripInt16) {
    MessageWriter w;
    w.begin_message('T');
    w.write_int16(INT16_MAX);
    w.write_int16(INT16_MIN);
    w.write_int16(0);
    auto buf = w.finish();

    // Skip type byte and length field.
    MessageReader reader(buf.data() + 5, buf.size() - 5);
    EXPECT_EQ(reader.read_int16(), INT16_MAX);
    EXPECT_EQ(reader.read_int16(), INT16_MIN);
    EXPECT_EQ(reader.read_int16(), 0);
}

TEST(QA146MessageReader, RoundTripInt32) {
    MessageWriter w;
    w.begin_message('T');
    w.write_int32(INT32_MAX);
    w.write_int32(INT32_MIN);
    w.write_int32(0);
    auto buf = w.finish();

    MessageReader reader(buf.data() + 5, buf.size() - 5);
    EXPECT_EQ(reader.read_int32(), INT32_MAX);
    EXPECT_EQ(reader.read_int32(), INT32_MIN);
    EXPECT_EQ(reader.read_int32(), 0);
}

// =============================================================================
// value_to_pg_text edge cases
// =============================================================================

TEST(QA146ValueText, NullValue) {
    Value null_val;
    EXPECT_TRUE(null_val.is_null());
    // NULL values should produce empty string (handled by protocol as -1 length).
    auto text = value_to_pg_text(null_val);
    // Implementation-defined, but should not crash.
    SUCCEED();
}

TEST(QA146ValueText, EmptyString) {
    EXPECT_EQ(value_to_pg_text(Value(std::string(""))), "");
}

TEST(QA146ValueText, StringWithNullByte) {
    // Strings containing embedded null bytes.
    std::string s("a\0b", 3);
    auto text = value_to_pg_text(Value(s));
    // Should not crash. Text representation may truncate at null.
    SUCCEED();
}

TEST(QA146ValueText, MaxInt64) {
    EXPECT_EQ(value_to_pg_text(Value(std::numeric_limits<int64_t>::max())), "9223372036854775807");
}

TEST(QA146ValueText, MinInt64) {
    EXPECT_EQ(value_to_pg_text(Value(std::numeric_limits<int64_t>::min())), "-9223372036854775808");
}

TEST(QA146ValueText, ZeroFloat) {
    auto text = value_to_pg_text(Value(0.0f));
    EXPECT_FALSE(text.empty());
}

// =============================================================================
// Type OID edge cases
// =============================================================================

TEST(QA146TypeOid, AllTypesHaveOids) {
    // Every TypeId should map to a non-zero OID.
    std::vector<TypeId> all_types = {
        TypeId::INT8,    TypeId::INT16,     TypeId::INT32,    TypeId::INT64,   TypeId::UINT8,
        TypeId::UINT16,  TypeId::UINT32,    TypeId::UINT64,   TypeId::FLOAT32, TypeId::FLOAT64,
        TypeId::DECIMAL, TypeId::BOOL,      TypeId::STRING,   TypeId::BLOB,    TypeId::DATE,
        TypeId::TIME,    TypeId::TIMESTAMP, TypeId::INTERVAL, TypeId::POINT,   TypeId::JSON,
        TypeId::UUID,    TypeId::EMBEDDING,
    };
    for (auto type : all_types) {
        EXPECT_GT(type_to_pg_oid(type), 0u) << "TypeId " << static_cast<int>(type) << " has OID 0";
    }
}

TEST(QA146TypeOid, OidRoundTripConsistency) {
    // For types that have a direct PG equivalent, round-trip should work.
    std::vector<TypeId> direct_types = {
        TypeId::BOOL,
        TypeId::INT16,
        TypeId::INT32,
        TypeId::INT64,
        TypeId::FLOAT32,
        TypeId::FLOAT64,
        TypeId::STRING,
        TypeId::BLOB,
        TypeId::DATE,
        TypeId::TIME,
        TypeId::TIMESTAMP,
        TypeId::INTERVAL,
        TypeId::POINT,
        TypeId::JSON,
        TypeId::UUID,
        TypeId::EMBEDDING,
    };
    for (auto type : direct_types) {
        uint32_t oid = type_to_pg_oid(type);
        auto result = pg_oid_to_type(oid);
        ASSERT_TRUE(result.has_value()) << "OID " << oid << " has no reverse mapping";
    }
}

TEST(QA146TypeOid, UnknownOidReturnsError) {
    EXPECT_FALSE(pg_oid_to_type(0).has_value());
    EXPECT_FALSE(pg_oid_to_type(999999).has_value());
    EXPECT_FALSE(pg_oid_to_type(UINT32_MAX).has_value());
}

// =============================================================================
// SQLSTATE mapping edge cases
// =============================================================================

TEST(QA146Sqlstate, AllCodesAreFiveChars) {
    std::vector<StatusCode> codes = {
        StatusCode::OK,
        StatusCode::NOT_FOUND,
        StatusCode::ALREADY_EXISTS,
        StatusCode::PARSE_ERROR,
        StatusCode::TYPE_ERROR,
        StatusCode::INTERNAL_ERROR,
        StatusCode::IO_ERROR,
        StatusCode::INVALID_ARGUMENT,
        StatusCode::AUTH_ERROR,
    };
    for (auto code : codes) {
        auto sqlstate = status_to_sqlstate(code);
        EXPECT_EQ(sqlstate.size(), 5u) << "StatusCode " << static_cast<int>(code) << " has "
                                       << sqlstate.size() << "-char SQLSTATE";
    }
}

// =============================================================================
// Full protocol handshake tests via socketpair
// =============================================================================

class QA146ProtocolTest : public ::testing::Test {
protected:
    void SetUp() override {
        int fds[2];
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
        client_fd_ = fds[0];
        server_fd_ = fds[1];
    }

    void TearDown() override {
        if (client_fd_ >= 0) {
            ::close(client_fd_);
        }
    }

    /// Send raw bytes to the server side.
    void send_raw(const void* data, size_t len) {
        ASSERT_EQ(::send(client_fd_, data, len, 0), static_cast<ssize_t>(len));
    }

    /// Build and send a PG StartupMessage.
    void send_startup(const std::string& user = "testuser",
                      const std::string& database = "testdb") {
        // StartupMessage format: Length(int32) + Protocol(int32) + params + \0
        std::vector<uint8_t> msg;
        // Protocol version 3.0.
        uint32_t version = 196608; // (3 << 16)
        // Build parameter string: "user\0testuser\0database\0testdb\0\0"
        std::vector<uint8_t> params;
        auto add_param = [&](const std::string& key, const std::string& val) {
            params.insert(params.end(), key.begin(), key.end());
            params.push_back(0);
            params.insert(params.end(), val.begin(), val.end());
            params.push_back(0);
        };
        add_param("user", user);
        add_param("database", database);
        params.push_back(0); // Final null terminator.

        // Total length: 4 (length field) + 4 (version) + params.
        uint32_t total_len = 4 + 4 + static_cast<uint32_t>(params.size());
        msg.resize(total_len);

        // Write length (big-endian).
        msg[0] = static_cast<uint8_t>((total_len >> 24) & 0xFF);
        msg[1] = static_cast<uint8_t>((total_len >> 16) & 0xFF);
        msg[2] = static_cast<uint8_t>((total_len >> 8) & 0xFF);
        msg[3] = static_cast<uint8_t>(total_len & 0xFF);

        // Write version (big-endian).
        msg[4] = static_cast<uint8_t>((version >> 24) & 0xFF);
        msg[5] = static_cast<uint8_t>((version >> 16) & 0xFF);
        msg[6] = static_cast<uint8_t>((version >> 8) & 0xFF);
        msg[7] = static_cast<uint8_t>(version & 0xFF);

        // Write params.
        std::memcpy(msg.data() + 8, params.data(), params.size());

        send_raw(msg.data(), msg.size());
    }

    /// Build and send a simple Query message.
    void send_query(const std::string& sql) {
        // Query message: type 'Q' + length(int32) + sql\0.
        std::vector<uint8_t> msg;
        msg.push_back('Q');
        uint32_t len = 4 + static_cast<uint32_t>(sql.size()) + 1; // length + string + null.
        msg.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
        msg.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
        msg.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        msg.push_back(static_cast<uint8_t>(len & 0xFF));
        msg.insert(msg.end(), sql.begin(), sql.end());
        msg.push_back(0);

        send_raw(msg.data(), msg.size());
    }

    /// Send a Terminate message.
    void send_terminate() {
        uint8_t msg[] = {'X', 0, 0, 0, 4};
        send_raw(msg, sizeof(msg));
    }

    /// Process all available data from the connection.
    Result<void> process_all(PgProtocolHandler& handler, Connection& conn) {
        auto read_result = conn.read_from_socket();
        if (!read_result) {
            return make_error(read_result.error().code, read_result.error().message);
        }
        return handler.process(conn);
    }

    /// Read response bytes from the server side (i.e., what the server wrote).
    std::vector<uint8_t> read_response(size_t max_bytes = 8192) {
        // First flush the server's write buffer, then read from the client end.
        std::vector<uint8_t> buf(max_bytes);
        ssize_t n = ::recv(client_fd_, buf.data(), buf.size(), 0);
        if (n <= 0) {
            return {};
        }
        buf.resize(static_cast<size_t>(n));
        return buf;
    }

    int client_fd_ = -1;
    int server_fd_ = -1;
};

TEST_F(QA146ProtocolTest, StartupHandshakeProducesResponse) {
    Connection conn(server_fd_);
    server_fd_ = -1;
    PgProtocolHandler handler(1);

    send_startup();

    auto r = process_all(handler, conn);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(handler.state(), ProtocolState::READY);

    // Flush write buffer to socket.
    auto wr = conn.write_to_socket();
    ASSERT_TRUE(wr.has_value());

    auto resp = read_response();
    EXPECT_FALSE(resp.empty());

    // Response should contain AuthenticationOk ('R'), ParameterStatus ('S'),
    // BackendKeyData ('K'), ReadyForQuery ('Z').
    bool found_auth = false;
    bool found_ready = false;
    for (size_t i = 0; i < resp.size(); ++i) {
        if (resp[i] == 'R') {
            found_auth = true;
        }
        if (resp[i] == 'Z') {
            found_ready = true;
        }
    }
    EXPECT_TRUE(found_auth);
    EXPECT_TRUE(found_ready);
}

TEST_F(QA146ProtocolTest, SimpleQueryReturnsReadyForQuery) {
    Connection conn(server_fd_);
    server_fd_ = -1;
    PgProtocolHandler handler(1);
    handler.set_query_executor([](const std::string& /*sql*/) -> Result<QueryResult> {
        QueryResult qr;
        qr.message = "SELECT";
        qr.column_names = {"x"};
        qr.column_types = {TypeId::INT32};
        qr.rows = {{Value(static_cast<int32_t>(1))}};
        return ok(std::move(qr));
    });

    send_startup();
    ASSERT_TRUE(process_all(handler, conn).has_value());
    ASSERT_TRUE(conn.write_to_socket().has_value());
    (void)read_response(); // Consume startup response.

    send_query("SELECT 1");
    ASSERT_TRUE(process_all(handler, conn).has_value());
    ASSERT_TRUE(conn.write_to_socket().has_value());

    auto resp = read_response();
    EXPECT_FALSE(resp.empty());

    // Should contain RowDescription ('T'), DataRow ('D'),
    // CommandComplete ('C'), ReadyForQuery ('Z').
    bool found_Z = false;
    for (uint8_t b : resp) {
        if (b == 'Z') {
            found_Z = true;
        }
    }
    EXPECT_TRUE(found_Z);
}

TEST_F(QA146ProtocolTest, ErrorResponseOnBadQuery) {
    Connection conn(server_fd_);
    server_fd_ = -1;
    PgProtocolHandler handler(1);
    handler.set_query_executor([](const std::string& /*sql*/) -> Result<QueryResult> {
        return make_error(StatusCode::PARSE_ERROR, "syntax error at end of input");
    });

    send_startup();
    ASSERT_TRUE(process_all(handler, conn).has_value());
    ASSERT_TRUE(conn.write_to_socket().has_value());
    (void)read_response();

    send_query("INVALID SQL");
    ASSERT_TRUE(process_all(handler, conn).has_value());
    ASSERT_TRUE(conn.write_to_socket().has_value());

    auto resp = read_response();
    EXPECT_FALSE(resp.empty());

    // Should contain ErrorResponse ('E') and ReadyForQuery ('Z').
    bool found_E = false;
    bool found_Z = false;
    for (uint8_t b : resp) {
        if (b == 'E') {
            found_E = true;
        }
        if (b == 'Z') {
            found_Z = true;
        }
    }
    EXPECT_TRUE(found_E);
    EXPECT_TRUE(found_Z);
}

TEST_F(QA146ProtocolTest, TerminateMessageClosesState) {
    Connection conn(server_fd_);
    server_fd_ = -1;
    PgProtocolHandler handler(1);

    send_startup();
    ASSERT_TRUE(process_all(handler, conn).has_value());
    ASSERT_TRUE(conn.write_to_socket().has_value());
    (void)read_response();

    send_terminate();
    ASSERT_TRUE(process_all(handler, conn).has_value());
    EXPECT_EQ(handler.state(), ProtocolState::CLOSED);
}

TEST_F(QA146ProtocolTest, EmptyQueryResponse) {
    Connection conn(server_fd_);
    server_fd_ = -1;
    PgProtocolHandler handler(1);
    handler.set_query_executor([](const std::string& /*sql*/) -> Result<QueryResult> {
        QueryResult qr;
        return ok(std::move(qr));
    });

    send_startup();
    ASSERT_TRUE(process_all(handler, conn).has_value());
    ASSERT_TRUE(conn.write_to_socket().has_value());
    (void)read_response();

    // Send an empty query.
    send_query("");
    ASSERT_TRUE(process_all(handler, conn).has_value());
    ASSERT_TRUE(conn.write_to_socket().has_value());

    auto resp = read_response();
    EXPECT_FALSE(resp.empty());
    // Should contain EmptyQueryResponse ('I') or just ReadyForQuery.
    bool found_Z = false;
    for (uint8_t b : resp) {
        if (b == 'Z') {
            found_Z = true;
        }
    }
    EXPECT_TRUE(found_Z);
}

TEST_F(QA146ProtocolTest, SSLRequestGetsDeclined) {
    Connection conn(server_fd_);
    server_fd_ = -1;
    PgProtocolHandler handler(1);

    // SSL request: length=8, code=80877103.
    uint8_t ssl_req[] = {0, 0, 0, 8, 0x04, 0xD2, 0x16, 0x2F};
    send_raw(ssl_req, sizeof(ssl_req));

    ASSERT_TRUE(process_all(handler, conn).has_value());
    ASSERT_TRUE(conn.write_to_socket().has_value());

    auto resp = read_response();
    ASSERT_FALSE(resp.empty());
    // Server should respond with 'N' (SSL not supported).
    EXPECT_EQ(resp[0], 'N');

    // After SSL decline, the handler should still be in WAIT_FOR_STARTUP.
    EXPECT_EQ(handler.state(), ProtocolState::WAIT_FOR_STARTUP);
}

TEST_F(QA146ProtocolTest, InvalidProtocolVersionClosesHandler) {
    Connection conn(server_fd_);
    server_fd_ = -1;
    PgProtocolHandler handler(1);

    // Send startup with wrong protocol version (2.0 instead of 3.0).
    uint8_t msg[] = {
        0,
        0,
        0,
        13, // length=13
        0,
        2,
        0,
        0, // version 2.0
        'u',
        's',
        'e',
        'r',
        0 // "user" param
    };
    send_raw(msg, sizeof(msg));

    auto r = process_all(handler, conn);
    // Handler sends a FATAL ErrorResponse and transitions to CLOSED.
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(handler.state(), ProtocolState::CLOSED);

    // Verify client receives an ErrorResponse.
    ASSERT_TRUE(conn.write_to_socket().has_value());
    auto resp = read_response();
    EXPECT_FALSE(resp.empty());
    bool found_E = false;
    for (uint8_t b : resp) {
        if (b == 'E') {
            found_E = true;
        }
    }
    EXPECT_TRUE(found_E);
}

TEST_F(QA146ProtocolTest, BackendPidAndSecretKeyAreSet) {
    PgProtocolHandler handler(42);
    EXPECT_EQ(handler.backend_pid(), 42);
    // Secret key is randomly generated, just check it's set.
    // (Could be any int32 value including 0, so just verify no crash.)
    (void)handler.secret_key();
}
