#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/server/connection.h"
#include "sixseven/server/pg_protocol.h"

#include <gtest/gtest.h>

#include "sixseven/common/platform.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace sixseven;

// =============================================================================
// Type OID mapping tests
// =============================================================================

TEST(PgProtocol, TypeToOidKnownTypes) {
    EXPECT_EQ(type_to_pg_oid(TypeId::BOOL), 16u);
    EXPECT_EQ(type_to_pg_oid(TypeId::INT16), 21u);
    EXPECT_EQ(type_to_pg_oid(TypeId::INT32), 23u);
    EXPECT_EQ(type_to_pg_oid(TypeId::INT64), 20u);
    EXPECT_EQ(type_to_pg_oid(TypeId::FLOAT32), 700u);
    EXPECT_EQ(type_to_pg_oid(TypeId::FLOAT64), 701u);
    EXPECT_EQ(type_to_pg_oid(TypeId::STRING), 25u);
    EXPECT_EQ(type_to_pg_oid(TypeId::BLOB), 17u);
    EXPECT_EQ(type_to_pg_oid(TypeId::DATE), 1082u);
    EXPECT_EQ(type_to_pg_oid(TypeId::TIME), 1083u);
    EXPECT_EQ(type_to_pg_oid(TypeId::TIMESTAMP), 1114u);
    EXPECT_EQ(type_to_pg_oid(TypeId::INTERVAL), 1186u);
    EXPECT_EQ(type_to_pg_oid(TypeId::POINT), 600u);
    EXPECT_EQ(type_to_pg_oid(TypeId::JSON), 114u);
    EXPECT_EQ(type_to_pg_oid(TypeId::UUID), 2950u);
    EXPECT_EQ(type_to_pg_oid(TypeId::EMBEDDING), 100000u);
}

TEST(PgProtocol, OidToTypeRoundTrip) {
    auto result = pg_oid_to_type(23);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, TypeId::INT32);

    auto result2 = pg_oid_to_type(25);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(*result2, TypeId::STRING);
}

TEST(PgProtocol, OidToTypeUnknownOid) {
    auto result = pg_oid_to_type(99999);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// SQLSTATE mapping tests
// =============================================================================

TEST(PgProtocol, StatusToSqlstateKnownCodes) {
    EXPECT_EQ(status_to_sqlstate(StatusCode::OK), "00000");
    EXPECT_EQ(status_to_sqlstate(StatusCode::NOT_FOUND), "42P01");
    EXPECT_EQ(status_to_sqlstate(StatusCode::ALREADY_EXISTS), "42P07");
    EXPECT_EQ(status_to_sqlstate(StatusCode::PARSE_ERROR), "42601");
    EXPECT_EQ(status_to_sqlstate(StatusCode::TYPE_ERROR), "42804");
    EXPECT_EQ(status_to_sqlstate(StatusCode::INTERNAL_ERROR), "XX000");
    EXPECT_EQ(status_to_sqlstate(StatusCode::AUTH_ERROR), "28000");
    EXPECT_EQ(status_to_sqlstate(StatusCode::DEADLOCK), "40P01");
    EXPECT_EQ(status_to_sqlstate(StatusCode::READ_ONLY), "25006");
}

// =============================================================================
// Value text formatting tests
// =============================================================================

TEST(PgProtocol, ValueToTextInt) {
    EXPECT_EQ(value_to_pg_text(Value(static_cast<int32_t>(42))), "42");
    EXPECT_EQ(value_to_pg_text(Value(static_cast<int64_t>(-100))), "-100");
    EXPECT_EQ(value_to_pg_text(Value(static_cast<int16_t>(0))), "0");
}

TEST(PgProtocol, ValueToTextBool) {
    EXPECT_EQ(value_to_pg_text(Value(true)), "t");
    EXPECT_EQ(value_to_pg_text(Value(false)), "f");
}

TEST(PgProtocol, ValueToTextString) {
    EXPECT_EQ(value_to_pg_text(Value(std::string("hello world"))), "hello world");
    EXPECT_EQ(value_to_pg_text(Value(std::string(""))), "");
}

TEST(PgProtocol, ValueToTextFloat) {
    auto text = value_to_pg_text(Value(3.14f));
    EXPECT_FALSE(text.empty());
    // Should contain "3.14" (exact format depends on stream).
    EXPECT_NE(text.find("3.14"), std::string::npos);
}

TEST(PgProtocol, ValueToTextUuid) {
    Uuid uuid = {0x01,
                 0x23,
                 0x45,
                 0x67,
                 0x89,
                 0xab,
                 0xcd,
                 0xef,
                 0x01,
                 0x23,
                 0x45,
                 0x67,
                 0x89,
                 0xab,
                 0xcd,
                 0xef};
    auto text = value_to_pg_text(Value(uuid));
    EXPECT_EQ(text, "01234567-89ab-cdef-0123-456789abcdef");
}

TEST(PgProtocol, ValueToTextBlob) {
    Blob blob = {0xDE, 0xAD, 0xBE, 0xEF};
    auto text = value_to_pg_text(Value(blob));
    EXPECT_EQ(text, "\\xdeadbeef");
}

TEST(PgProtocol, ValueToTextEmbedding) {
    Embedding emb = {1.0f, 2.0f, 3.0f};
    auto text = value_to_pg_text(Value(emb));
    EXPECT_EQ(text, "[1,2,3]");
}

TEST(PgProtocol, ValueToTextDate) {
    // 2024-01-15 is day 19737 since epoch (1970-01-01).
    Date date{19737};
    auto text = value_to_pg_text(Value(date));
    EXPECT_EQ(text, "2024-01-15");
}

TEST(PgProtocol, ValueToTextTime) {
    // 13:45:30.123456
    Time time{49530123456LL};
    auto text = value_to_pg_text(Value(time));
    EXPECT_EQ(text, "13:45:30.123456");
}

TEST(PgProtocol, ValueToTextJson) {
    JsonString js{"{\"key\":\"value\"}"};
    EXPECT_EQ(value_to_pg_text(Value(js)), "{\"key\":\"value\"}");
}

TEST(PgProtocol, ValueToTextPoint) {
    Point p{1.5, 2.5};
    EXPECT_EQ(value_to_pg_text(Value(p)), "(1.5,2.5)");
}

// =============================================================================
// MessageWriter / MessageReader tests
// =============================================================================

TEST(PgProtocol, MessageWriterReadRoundTrip) {
    MessageWriter w;
    w.begin_message('T'); // RowDescription type byte.
    w.write_int16(2);     // 2 columns.
    w.write_cstring("id");
    w.write_int32(0);
    w.write_int16(0);
    w.write_int32(23); // int4 OID.
    w.write_int16(-1);
    w.write_int32(-1);
    w.write_int16(0);
    w.write_cstring("name");
    w.write_int32(0);
    w.write_int16(0);
    w.write_int32(25); // text OID.
    w.write_int16(-1);
    w.write_int32(-1);
    w.write_int16(0);
    auto msg = w.finish();

    // Verify: type byte + 4-byte length + payload.
    ASSERT_GE(msg.size(), 5u);
    EXPECT_EQ(msg[0], 'T');

    // Read back the body (skip type byte and length).
    MessageReader r(msg.data() + 5, msg.size() - 5);

    int16_t num_cols = r.read_int16();
    EXPECT_EQ(num_cols, 2);

    auto col1_name = r.read_cstring();
    EXPECT_EQ(col1_name, "id");
    r.read_int32(); // table OID.
    r.read_int16(); // attr num.
    int32_t col1_type = r.read_int32();
    EXPECT_EQ(col1_type, 23);
    r.read_int16(); // type size.
    r.read_int32(); // type mod.
    r.read_int16(); // format.

    auto col2_name = r.read_cstring();
    EXPECT_EQ(col2_name, "name");
}

TEST(PgProtocol, MessageWriterNoType) {
    MessageWriter w;
    w.begin_message_no_type();
    w.write_int32(42);
    auto msg = w.finish();

    // No type byte — just 4-byte length + 4-byte payload.
    ASSERT_EQ(msg.size(), 8u);

    // Length field includes itself: 4 (length) + 4 (int32) = 8.
    MessageReader r(msg.data(), msg.size());
    int32_t length = r.read_int32();
    EXPECT_EQ(length, 8);
    int32_t value = r.read_int32();
    EXPECT_EQ(value, 42);
}

TEST(PgProtocol, MessageWriterLengthFieldIncludesSelf) {
    MessageWriter w;
    w.begin_message('Z');
    w.write_byte('I');
    auto msg = w.finish();

    // 'Z' + int32(5) + 'I' = 6 bytes total.
    ASSERT_EQ(msg.size(), 6u);
    EXPECT_EQ(msg[0], 'Z');
    // Length = 5 (includes the 4-byte length field + 1-byte payload).
    EXPECT_EQ(msg[1], 0);
    EXPECT_EQ(msg[2], 0);
    EXPECT_EQ(msg[3], 0);
    EXPECT_EQ(msg[4], 5);
    EXPECT_EQ(msg[5], 'I');
}

TEST(PgProtocol, MessageReaderBoundaryCheck) {
    uint8_t data[] = {0x00, 0x2A}; // Just 2 bytes.
    MessageReader r(data, sizeof(data));

    // read_int16 should work (2 bytes available).
    int16_t val = r.read_int16();
    EXPECT_EQ(val, 42);

    // No more data.
    EXPECT_FALSE(r.has_remaining());
    EXPECT_EQ(r.remaining(), 0u);
}

// =============================================================================
// Protocol handler tests
// =============================================================================

namespace {

/// Helper: build a PG v3 StartupMessage.
std::vector<uint8_t>
build_startup_message(const std::vector<std::pair<std::string, std::string>>& params) {
    // StartupMessage: int32 length + int32 protocol_version + params.
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

/// Helper: build a PG Query message.
std::vector<uint8_t> build_query_message(std::string_view sql) {
    // Query: 'Q' + int32 length + sql + '\0'.
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

/// Helper: build a PG Terminate message.
std::vector<uint8_t> build_terminate_message() {
    // Terminate: 'X' + int32(4).
    return {'X', 0, 0, 0, 4};
}

/// Helper: build an SSL request.
std::vector<uint8_t> build_ssl_request() {
    // SSLRequest: int32(8) + int32(80877103).
    uint32_t code = 80877103;
    return {0,
            0,
            0,
            8,
            static_cast<uint8_t>((code >> 24) & 0xFF),
            static_cast<uint8_t>((code >> 16) & 0xFF),
            static_cast<uint8_t>((code >> 8) & 0xFF),
            static_cast<uint8_t>(code & 0xFF)};
}

/// Helper: create a socketpair and return the server-side fd.
/// The client-side fd is stored in client_fd_out.
int create_socketpair(int& client_fd_out) {
    int fds[2];
    int rc = sixseven_platform::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    EXPECT_EQ(rc, 0);
    client_fd_out = fds[1];
    return fds[0]; // Server side.
}

/// Helper: write data to a fd.
void write_to_fd(int fd, const std::vector<uint8_t>& data) {
    size_t written = 0;
    while (written < data.size()) {
        auto n = ::write(fd, data.data() + written, data.size() - written);
        ASSERT_GT(n, 0);
        written += static_cast<size_t>(n);
    }
}

/// Helper: read all available data from a fd into a vector.
std::vector<uint8_t> read_from_fd(int fd, size_t max_bytes = 8192) {
    std::vector<uint8_t> buf(max_bytes);
    auto n = ::read(fd, buf.data(), buf.size());
    if (n <= 0) {
        return {};
    }
    buf.resize(static_cast<size_t>(n));
    return buf;
}

/// Helper: find a backend message of given type in raw bytes.
/// Returns the payload (after type byte and length) and advances pos past the message.
/// Returns empty span if not found.
bool find_message(const std::vector<uint8_t>& data,
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
        if (pos + total > data.size()) {
            return false; // Incomplete message.
        }
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

} // namespace

TEST(PgProtocol, StartupHandshake) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(1);

    EXPECT_EQ(handler.state(), ProtocolState::WAIT_FOR_STARTUP);

    // Client sends startup message.
    auto startup = build_startup_message({{"user", "test"}, {"database", "testdb"}});
    write_to_fd(client_fd, startup);

    // Server reads and processes.
    auto read_result = conn.read_from_socket();
    ASSERT_TRUE(read_result.has_value());

    auto process_result = handler.process(conn);
    ASSERT_TRUE(process_result.has_value());

    EXPECT_EQ(handler.state(), ProtocolState::READY);

    // Flush writes to client.
    auto write_result = conn.write_to_socket();
    ASSERT_TRUE(write_result.has_value());

    // Read the server's response from the client side.
    auto response = read_from_fd(client_fd);
    ASSERT_FALSE(response.empty());

    // Verify we got: AuthOk ('R'), ParameterStatus ('S')*, BackendKeyData ('K'),
    // ReadyForQuery ('Z').
    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    // AuthenticationOk: 'R', length=8, type=0.
    ASSERT_TRUE(find_message(response, pos, 'R', payload, payload_len));
    ASSERT_EQ(payload_len, 4u);
    MessageReader auth_reader(payload, payload_len);
    EXPECT_EQ(auth_reader.read_int32(), 0); // AuthenticationOk.

    // Should have at least one ParameterStatus.
    ASSERT_TRUE(find_message(response, pos, 'S', payload, payload_len));

    // Find BackendKeyData.
    bool found_key = false;
    size_t search_pos = 0;
    while (find_message(response, search_pos, 'K', payload, payload_len)) {
        found_key = true;
        ASSERT_EQ(payload_len, 8u);
        MessageReader key_reader(payload, payload_len);
        int32_t pid = key_reader.read_int32();
        int32_t secret = key_reader.read_int32();
        EXPECT_EQ(pid, 1); // Our backend_pid.
        EXPECT_NE(secret, 0);
        break;
    }
    EXPECT_TRUE(found_key);

    // Find ReadyForQuery.
    search_pos = 0;
    bool found_ready = false;
    while (find_message(response, search_pos, 'Z', payload, payload_len)) {
        found_ready = true;
        ASSERT_EQ(payload_len, 1u);
        EXPECT_EQ(payload[0], 'I'); // Idle status.
        break;
    }
    EXPECT_TRUE(found_ready);

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, SslRequestDenied) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(2);

    auto ssl_req = build_ssl_request();
    write_to_fd(client_fd, ssl_req);

    auto read_result = conn.read_from_socket();
    ASSERT_TRUE(read_result.has_value());

    auto process_result = handler.process(conn);
    ASSERT_TRUE(process_result.has_value());

    // Still waiting for startup (SSL was just a probe).
    EXPECT_EQ(handler.state(), ProtocolState::WAIT_FOR_STARTUP);

    auto write_result = conn.write_to_socket();
    ASSERT_TRUE(write_result.has_value());

    auto response = read_from_fd(client_fd);
    ASSERT_EQ(response.size(), 1u);
    EXPECT_EQ(response[0], 'N'); // SSL not supported.

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, SimpleQuerySelect) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(3);

    // Set up a mock query executor that returns a simple result.
    handler.set_query_executor([](const std::string& /*sql*/, const std::string& /*database*/) -> Result<QueryResult> {
        QueryResult qr;
        qr.column_names = {"id", "name"};
        qr.column_types = {TypeId::INT32, TypeId::STRING};
        qr.rows = {
            {Value(static_cast<int32_t>(1)), Value(std::string("alice"))},
            {Value(static_cast<int32_t>(2)), Value(std::string("bob"))},
        };
        return ok(std::move(qr));
    });

    // Startup handshake first.
    auto startup = build_startup_message({{"user", "test"}});
    write_to_fd(client_fd, startup);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)read_from_fd(client_fd); // Discard startup response.

    ASSERT_EQ(handler.state(), ProtocolState::READY);

    // Send a query.
    auto query = build_query_message("SELECT id, name FROM users");
    write_to_fd(client_fd, query);
    (void)conn.read_from_socket();
    auto process_result = handler.process(conn);
    ASSERT_TRUE(process_result.has_value());

    (void)conn.write_to_socket();
    auto response = read_from_fd(client_fd);
    ASSERT_FALSE(response.empty());

    // Expect: RowDescription ('T') + DataRow ('D') * 2 + CommandComplete ('C')
    // + ReadyForQuery ('Z').
    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    // RowDescription.
    ASSERT_TRUE(find_message(response, pos, 'T', payload, payload_len));
    MessageReader rd_reader(payload, payload_len);
    int16_t num_cols = rd_reader.read_int16();
    EXPECT_EQ(num_cols, 2);

    // First DataRow.
    ASSERT_TRUE(find_message(response, pos, 'D', payload, payload_len));

    // Second DataRow.
    ASSERT_TRUE(find_message(response, pos, 'D', payload, payload_len));

    // CommandComplete.
    ASSERT_TRUE(find_message(response, pos, 'C', payload, payload_len));
    MessageReader cc_reader(payload, payload_len);
    auto tag = cc_reader.read_cstring();
    EXPECT_EQ(tag, "SELECT 2");

    // ReadyForQuery.
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));
    EXPECT_EQ(payload[0], 'I');

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, SimpleQueryError) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(4);

    handler.set_query_executor([](const std::string& /*sql*/, const std::string& /*database*/) -> Result<QueryResult> {
        return make_error(StatusCode::PARSE_ERROR, "syntax error at position 1");
    });

    // Startup handshake.
    auto startup = build_startup_message({{"user", "test"}});
    write_to_fd(client_fd, startup);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)read_from_fd(client_fd);

    // Send a bad query.
    auto query = build_query_message("SELEKT * FROM oops");
    write_to_fd(client_fd, query);
    (void)conn.read_from_socket();
    auto process_result = handler.process(conn);
    ASSERT_TRUE(process_result.has_value());

    (void)conn.write_to_socket();
    auto response = read_from_fd(client_fd);
    ASSERT_FALSE(response.empty());

    // Expect: ErrorResponse ('E') + ReadyForQuery ('Z').
    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    ASSERT_TRUE(find_message(response, pos, 'E', payload, payload_len));

    // Parse error response fields.
    MessageReader err_reader(payload, payload_len);
    bool found_severity = false;
    bool found_sqlstate = false;
    bool found_message = false;

    while (err_reader.has_remaining()) {
        uint8_t field = err_reader.read_byte();
        if (field == 0) {
            break; // Terminator.
        }
        auto val = err_reader.read_cstring();
        switch (field) {
        case 'S':
            EXPECT_EQ(val, "ERROR");
            found_severity = true;
            break;
        case 'C':
            EXPECT_EQ(val, "42601"); // SQLSTATE for parse error.
            found_sqlstate = true;
            break;
        case 'M':
            EXPECT_EQ(val, "syntax error at position 1");
            found_message = true;
            break;
        default:
            break;
        }
    }

    EXPECT_TRUE(found_severity);
    EXPECT_TRUE(found_sqlstate);
    EXPECT_TRUE(found_message);

    // ReadyForQuery.
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));
    EXPECT_EQ(payload[0], 'I');

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, EmptyQuery) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(5);

    handler.set_query_executor(
        [](const std::string& /*sql*/, const std::string& /*database*/) -> Result<QueryResult> { return ok(QueryResult{}); });

    // Startup handshake.
    auto startup = build_startup_message({{"user", "test"}});
    write_to_fd(client_fd, startup);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)read_from_fd(client_fd);

    // Send empty query.
    auto query = build_query_message("");
    write_to_fd(client_fd, query);
    (void)conn.read_from_socket();
    auto process_result = handler.process(conn);
    ASSERT_TRUE(process_result.has_value());

    (void)conn.write_to_socket();
    auto response = read_from_fd(client_fd);
    ASSERT_FALSE(response.empty());

    // Expect: EmptyQueryResponse ('I') + ReadyForQuery ('Z').
    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    ASSERT_TRUE(find_message(response, pos, 'I', payload, payload_len));
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));
    EXPECT_EQ(payload[0], 'I');

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, TerminateMessage) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(6);

    // Startup handshake.
    auto startup = build_startup_message({{"user", "test"}});
    write_to_fd(client_fd, startup);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)read_from_fd(client_fd);

    ASSERT_EQ(handler.state(), ProtocolState::READY);

    // Send terminate.
    auto terminate = build_terminate_message();
    write_to_fd(client_fd, terminate);
    (void)conn.read_from_socket();
    auto process_result = handler.process(conn);
    ASSERT_TRUE(process_result.has_value());

    EXPECT_EQ(handler.state(), ProtocolState::CLOSED);

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, DmlQueryInsert) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(7);

    handler.set_query_executor([](const std::string& /*sql*/, const std::string& /*database*/) -> Result<QueryResult> {
        QueryResult qr;
        qr.affected_rows = 3;
        qr.message = "INSERT";
        return ok(std::move(qr));
    });

    // Startup handshake.
    auto startup = build_startup_message({{"user", "test"}});
    write_to_fd(client_fd, startup);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)read_from_fd(client_fd);

    // Send insert query.
    auto query = build_query_message("INSERT INTO t VALUES (1, 2, 3)");
    write_to_fd(client_fd, query);
    (void)conn.read_from_socket();
    auto process_result = handler.process(conn);
    ASSERT_TRUE(process_result.has_value());

    (void)conn.write_to_socket();
    auto response = read_from_fd(client_fd);
    ASSERT_FALSE(response.empty());

    // Expect: CommandComplete ('C') + ReadyForQuery ('Z').
    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    ASSERT_TRUE(find_message(response, pos, 'C', payload, payload_len));
    MessageReader cc_reader(payload, payload_len);
    auto tag = cc_reader.read_cstring();
    EXPECT_EQ(tag, "INSERT 0 3");

    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, DdlQuery) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(8);

    handler.set_query_executor([](const std::string& /*sql*/, const std::string& /*database*/) -> Result<QueryResult> {
        QueryResult qr;
        qr.message = "CREATE TABLE";
        return ok(std::move(qr));
    });

    // Startup handshake.
    auto startup = build_startup_message({{"user", "test"}});
    write_to_fd(client_fd, startup);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)read_from_fd(client_fd);

    auto query = build_query_message("CREATE TABLE t (id INT)");
    write_to_fd(client_fd, query);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    auto response = read_from_fd(client_fd);

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    ASSERT_TRUE(find_message(response, pos, 'C', payload, payload_len));
    MessageReader cc_reader(payload, payload_len);
    auto tag = cc_reader.read_cstring();
    EXPECT_EQ(tag, "CREATE TABLE");

    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, PartialMessageReassembly) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(9);

    // Send startup message in two parts.
    auto startup = build_startup_message({{"user", "test"}});

    // Split at halfway point.
    size_t split = startup.size() / 2;
    std::vector<uint8_t> part1(startup.begin(), startup.begin() + static_cast<ptrdiff_t>(split));
    std::vector<uint8_t> part2(startup.begin() + static_cast<ptrdiff_t>(split), startup.end());

    // Send first part.
    write_to_fd(client_fd, part1);
    (void)conn.read_from_socket();
    auto result1 = handler.process(conn);
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(handler.state(), ProtocolState::WAIT_FOR_STARTUP); // Still waiting.

    // Send second part.
    write_to_fd(client_fd, part2);
    (void)conn.read_from_socket();
    auto result2 = handler.process(conn);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(handler.state(), ProtocolState::READY); // Now complete.

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, DataRowNullValues) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(10);

    handler.set_query_executor([](const std::string& /*sql*/, const std::string& /*database*/) -> Result<QueryResult> {
        QueryResult qr;
        qr.column_names = {"val"};
        qr.column_types = {TypeId::STRING};
        qr.rows = {{Value::make_null()}};
        return ok(std::move(qr));
    });

    // Startup.
    auto startup = build_startup_message({{"user", "test"}});
    write_to_fd(client_fd, startup);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)read_from_fd(client_fd);

    // Query.
    auto query = build_query_message("SELECT val FROM t");
    write_to_fd(client_fd, query);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    auto response = read_from_fd(client_fd);

    // Find the DataRow and verify it has a -1 length (NULL indicator).
    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    ASSERT_TRUE(find_message(response, pos, 'T', payload, payload_len)); // Skip RowDescription.
    ASSERT_TRUE(find_message(response, pos, 'D', payload, payload_len));

    MessageReader dr_reader(payload, payload_len);
    int16_t num_fields = dr_reader.read_int16();
    EXPECT_EQ(num_fields, 1);
    int32_t field_len = dr_reader.read_int32();
    EXPECT_EQ(field_len, -1); // NULL.

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

// =============================================================================
// Extended query protocol tests
// =============================================================================

namespace {

/// Helper: build a Parse message.
std::vector<uint8_t> build_parse_message(std::string_view stmt_name,
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

/// Helper: build a Bind message.
std::vector<uint8_t> build_bind_message(std::string_view portal_name,
                                        std::string_view stmt_name,
                                        const std::vector<std::string>& param_values = {}) {
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
        auto len = static_cast<uint32_t>(val.size());
        body.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
        body.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
        body.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        body.push_back(static_cast<uint8_t>(len & 0xFF));
        body.insert(body.end(), val.begin(), val.end());
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

/// Helper: build an Execute message.
std::vector<uint8_t> build_execute_message(std::string_view portal_name, int32_t max_rows = 0) {
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

/// Helper: build a Sync message.
std::vector<uint8_t> build_sync_message() {
    return {'S', 0, 0, 0, 4};
}

/// Helper: build a Close message.
std::vector<uint8_t> build_close_message(char type, std::string_view name) {
    std::vector<uint8_t> msg;
    msg.push_back('C');
    uint32_t body_len = static_cast<uint32_t>(4 + 1 + name.size() + 1);
    msg.push_back(static_cast<uint8_t>((body_len >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(body_len & 0xFF));
    msg.push_back(static_cast<uint8_t>(type));
    msg.insert(msg.end(), name.begin(), name.end());
    msg.push_back(0);
    return msg;
}

/// Helper: build a Describe message.
std::vector<uint8_t> build_describe_message(char type, std::string_view name) {
    std::vector<uint8_t> msg;
    msg.push_back('D');
    uint32_t body_len = static_cast<uint32_t>(4 + 1 + name.size() + 1);
    msg.push_back(static_cast<uint8_t>((body_len >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(body_len & 0xFF));
    msg.push_back(static_cast<uint8_t>(type));
    msg.insert(msg.end(), name.begin(), name.end());
    msg.push_back(0);
    return msg;
}

/// Helper: do a startup handshake and drain the response.
void do_startup(int client_fd, Connection& conn, PgProtocolHandler& handler) {
    auto startup = build_startup_message({{"user", "test"}});
    write_to_fd(client_fd, startup);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)read_from_fd(client_fd);
}

} // namespace

TEST(PgProtocol, ParseBindExecuteSync) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(11);

    handler.set_query_executor([](const std::string& /*sql*/, const std::string& /*database*/) -> Result<QueryResult> {
        QueryResult qr;
        qr.column_names = {"id"};
        qr.column_types = {TypeId::INT32};
        qr.rows = {{Value(static_cast<int32_t>(42))}};
        return ok(std::move(qr));
    });

    do_startup(client_fd, conn, handler);

    // Build a full extended query batch: Parse + Bind + Execute + Sync.
    std::vector<uint8_t> batch;
    auto parse = build_parse_message("", "SELECT id FROM t WHERE id = $1", {23});
    auto bind = build_bind_message("", "", {"42"});
    auto execute = build_execute_message("");
    auto sync = build_sync_message();

    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), execute.begin(), execute.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd(client_fd, batch);
    (void)conn.read_from_socket();
    auto process_result = handler.process(conn);
    ASSERT_TRUE(process_result.has_value());

    (void)conn.write_to_socket();
    auto response = read_from_fd(client_fd);
    ASSERT_FALSE(response.empty());

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    // ParseComplete ('1').
    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len));
    // BindComplete ('2').
    ASSERT_TRUE(find_message(response, pos, '2', payload, payload_len));
    // DataRow ('D').
    ASSERT_TRUE(find_message(response, pos, 'D', payload, payload_len));
    // CommandComplete ('C').
    ASSERT_TRUE(find_message(response, pos, 'C', payload, payload_len));
    // ReadyForQuery ('Z').
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));
    EXPECT_EQ(payload[0], 'I');

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, NamedPreparedStatementPersists) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(12);

    int call_count = 0;
    handler.set_query_executor([&call_count](const std::string& /*sql*/, const std::string& /*database*/) -> Result<QueryResult> {
        ++call_count;
        QueryResult qr;
        qr.message = "OK";
        return ok(std::move(qr));
    });

    do_startup(client_fd, conn, handler);

    // Parse once with a named statement.
    auto parse = build_parse_message("mystmt", "SELECT 1");
    auto sync1 = build_sync_message();
    std::vector<uint8_t> batch1;
    batch1.insert(batch1.end(), parse.begin(), parse.end());
    batch1.insert(batch1.end(), sync1.begin(), sync1.end());

    write_to_fd(client_fd, batch1);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)read_from_fd(client_fd);

    // Verify the prepared statement is stored.
    ASSERT_EQ(handler.prepared_statements().count("mystmt"), 1u);
    EXPECT_EQ(handler.prepared_statements().at("mystmt").sql, "SELECT 1");

    // Bind and execute the named statement twice.
    for (int i = 0; i < 2; ++i) {
        auto bind = build_bind_message("", "mystmt");
        auto execute = build_execute_message("");
        auto sync = build_sync_message();
        std::vector<uint8_t> batch;
        batch.insert(batch.end(), bind.begin(), bind.end());
        batch.insert(batch.end(), execute.begin(), execute.end());
        batch.insert(batch.end(), sync.begin(), sync.end());

        write_to_fd(client_fd, batch);
        (void)conn.read_from_socket();
        (void)handler.process(conn);
        (void)conn.write_to_socket();
        (void)read_from_fd(client_fd);
    }

    EXPECT_EQ(call_count, 2);

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, CloseStatement) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(13);

    do_startup(client_fd, conn, handler);

    // Parse a named statement.
    auto parse = build_parse_message("todelete", "SELECT 1");
    auto sync1 = build_sync_message();
    std::vector<uint8_t> batch1;
    batch1.insert(batch1.end(), parse.begin(), parse.end());
    batch1.insert(batch1.end(), sync1.begin(), sync1.end());

    write_to_fd(client_fd, batch1);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)read_from_fd(client_fd);

    ASSERT_EQ(handler.prepared_statements().count("todelete"), 1u);

    // Close the statement.
    auto close_msg = build_close_message('S', "todelete");
    auto sync2 = build_sync_message();
    std::vector<uint8_t> batch2;
    batch2.insert(batch2.end(), close_msg.begin(), close_msg.end());
    batch2.insert(batch2.end(), sync2.begin(), sync2.end());

    write_to_fd(client_fd, batch2);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    auto response = read_from_fd(client_fd);

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    // CloseComplete ('3').
    ASSERT_TRUE(find_message(response, pos, '3', payload, payload_len));
    // ReadyForQuery.
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    // Statement should be gone.
    EXPECT_EQ(handler.prepared_statements().count("todelete"), 0u);

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, DescribeStatement) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(14);

    do_startup(client_fd, conn, handler);

    // Parse with parameter OIDs.
    auto parse = build_parse_message("desc_test", "SELECT $1", {23, 25});
    auto describe = build_describe_message('S', "desc_test");
    auto sync = build_sync_message();

    std::vector<uint8_t> batch;
    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), describe.begin(), describe.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd(client_fd, batch);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    auto response = read_from_fd(client_fd);

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    // ParseComplete ('1').
    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len));
    // ParameterDescription ('t').
    ASSERT_TRUE(find_message(response, pos, 't', payload, payload_len));
    MessageReader pd_reader(payload, payload_len);
    int16_t num_params = pd_reader.read_int16();
    EXPECT_EQ(num_params, 2);
    EXPECT_EQ(pd_reader.read_int32(), 23); // int4 OID.
    EXPECT_EQ(pd_reader.read_int32(), 25); // text OID.
    // NoData ('n').
    ASSERT_TRUE(find_message(response, pos, 'n', payload, payload_len));
    // ReadyForQuery.
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, DescribeStatementRowDescription) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(30);

    // Set a describer that returns columns for SELECT statements.
    handler.set_query_describer(
        [](const std::string& sql, const std::string& /*database*/) -> Result<std::vector<ColumnDescription>> {
            if (sql.find("SELECT") != std::string::npos ||
                sql.find("select") != std::string::npos) {
                return ok(std::vector<ColumnDescription>{
                    {"id", TypeId::INT32},
                    {"name", TypeId::STRING},
                });
            }
            return ok(std::vector<ColumnDescription>{});
        });

    do_startup(client_fd, conn, handler);

    // Parse a SELECT + Describe.
    auto parse = build_parse_message("sel_stmt", "SELECT id, name FROM t", {});
    auto describe = build_describe_message('S', "sel_stmt");
    auto sync = build_sync_message();

    std::vector<uint8_t> batch;
    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), describe.begin(), describe.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd(client_fd, batch);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    auto response = read_from_fd(client_fd);

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    // ParseComplete ('1').
    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len));
    // ParameterDescription ('t') — 0 params.
    ASSERT_TRUE(find_message(response, pos, 't', payload, payload_len));
    // RowDescription ('T') — 2 columns.
    ASSERT_TRUE(find_message(response, pos, 'T', payload, payload_len));

    MessageReader rd_reader(payload, payload_len);
    int16_t num_cols = rd_reader.read_int16();
    EXPECT_EQ(num_cols, 2);

    // Column 1: "id", INT32 (OID 23).
    auto col1_name = std::string(rd_reader.read_cstring());
    EXPECT_EQ(col1_name, "id");
    rd_reader.read_int32(); // table OID
    rd_reader.read_int16(); // column attr number
    int32_t col1_oid = rd_reader.read_int32();
    EXPECT_EQ(col1_oid, 23); // int4
    rd_reader.read_int16();  // type size
    rd_reader.read_int32();  // type modifier
    rd_reader.read_int16();  // format code

    // Column 2: "name", STRING (OID 25).
    auto col2_name = std::string(rd_reader.read_cstring());
    EXPECT_EQ(col2_name, "name");
    rd_reader.read_int32(); // table OID
    rd_reader.read_int16(); // column attr number
    int32_t col2_oid = rd_reader.read_int32();
    EXPECT_EQ(col2_oid, 25); // text
    rd_reader.read_int16();  // type size
    rd_reader.read_int32();  // type modifier
    rd_reader.read_int16();  // format code

    // ReadyForQuery.
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, DescribeStatementNoDataForDML) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(31);

    // Describer returns empty for non-SELECT.
    handler.set_query_describer(
        [](const std::string& /*sql*/, const std::string& /*database*/) -> Result<std::vector<ColumnDescription>> {
            return ok(std::vector<ColumnDescription>{});
        });

    do_startup(client_fd, conn, handler);

    // Parse an INSERT + Describe.
    auto parse = build_parse_message("ins_stmt", "INSERT INTO t VALUES (1)", {});
    auto describe = build_describe_message('S', "ins_stmt");
    auto sync = build_sync_message();

    std::vector<uint8_t> batch;
    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), describe.begin(), describe.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd(client_fd, batch);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    auto response = read_from_fd(client_fd);

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    // ParseComplete ('1').
    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len));
    // ParameterDescription ('t').
    ASSERT_TRUE(find_message(response, pos, 't', payload, payload_len));
    // NoData ('n') — not a SELECT.
    ASSERT_TRUE(find_message(response, pos, 'n', payload, payload_len));
    // ReadyForQuery.
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, DescribePortalRowDescription) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(32);

    handler.set_query_executor([](const std::string& /*sql*/, const std::string& /*database*/) -> Result<QueryResult> {
        QueryResult qr;
        qr.column_names = {"id", "name"};
        qr.column_types = {TypeId::INT32, TypeId::STRING};
        qr.rows = {{Value(static_cast<int32_t>(1)), Value(std::string("alice"))}};
        return ok(std::move(qr));
    });

    handler.set_query_describer(
        [](const std::string& sql, const std::string& /*database*/) -> Result<std::vector<ColumnDescription>> {
            if (sql.find("SELECT") != std::string::npos ||
                sql.find("select") != std::string::npos) {
                return ok(std::vector<ColumnDescription>{
                    {"id", TypeId::INT32},
                    {"name", TypeId::STRING},
                });
            }
            return ok(std::vector<ColumnDescription>{});
        });

    do_startup(client_fd, conn, handler);

    // Parse + Bind + Describe Portal + Sync.
    auto parse = build_parse_message("", "SELECT id, name FROM t");
    auto bind = build_bind_message("myportal", "");
    auto describe = build_describe_message('P', "myportal");
    auto sync = build_sync_message();

    std::vector<uint8_t> batch;
    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), describe.begin(), describe.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd(client_fd, batch);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    auto response = read_from_fd(client_fd);

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    // ParseComplete ('1').
    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len));
    // BindComplete ('2').
    ASSERT_TRUE(find_message(response, pos, '2', payload, payload_len));
    // RowDescription ('T') for portal.
    ASSERT_TRUE(find_message(response, pos, 'T', payload, payload_len));

    MessageReader rd_reader(payload, payload_len);
    int16_t num_cols = rd_reader.read_int16();
    EXPECT_EQ(num_cols, 2);

    auto col1_name = std::string(rd_reader.read_cstring());
    EXPECT_EQ(col1_name, "id");
    rd_reader.read_int32();                // table OID
    rd_reader.read_int16();                // column attr number
    EXPECT_EQ(rd_reader.read_int32(), 23); // INT32 OID
    rd_reader.read_int16();                // type size
    rd_reader.read_int32();                // type modifier
    rd_reader.read_int16();                // format code

    auto col2_name = std::string(rd_reader.read_cstring());
    EXPECT_EQ(col2_name, "name");
    rd_reader.read_int32();
    rd_reader.read_int16();
    EXPECT_EQ(rd_reader.read_int32(), 25); // STRING OID
    rd_reader.read_int16();
    rd_reader.read_int32();
    rd_reader.read_int16();

    // ReadyForQuery.
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, DescribePortalNoDataForDML) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(33);

    handler.set_query_executor(
        [](const std::string& /*sql*/, const std::string& /*database*/) -> Result<QueryResult> { return ok(QueryResult{}); });

    handler.set_query_describer(
        [](const std::string& /*sql*/, const std::string& /*database*/) -> Result<std::vector<ColumnDescription>> {
            return ok(std::vector<ColumnDescription>{});
        });

    do_startup(client_fd, conn, handler);

    // Parse INSERT + Bind + Describe Portal + Sync.
    auto parse = build_parse_message("", "INSERT INTO t VALUES (1)");
    auto bind = build_bind_message("myportal", "");
    auto describe = build_describe_message('P', "myportal");
    auto sync = build_sync_message();

    std::vector<uint8_t> batch;
    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), describe.begin(), describe.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd(client_fd, batch);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    auto response = read_from_fd(client_fd);

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    // ParseComplete ('1').
    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len));
    // BindComplete ('2').
    ASSERT_TRUE(find_message(response, pos, '2', payload, payload_len));
    // NoData ('n') for DML portal.
    ASSERT_TRUE(find_message(response, pos, 'n', payload, payload_len));
    // ReadyForQuery.
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, DescribeStatementDescriberError) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(34);

    // Describer that always returns an error (e.g., table not found during bind).
    handler.set_query_describer(
        [](const std::string& /*sql*/, const std::string& /*database*/) -> Result<std::vector<ColumnDescription>> {
            return make_error(StatusCode::NOT_FOUND, "table \"nonexistent\" does not exist");
        });

    do_startup(client_fd, conn, handler);

    auto parse = build_parse_message("bad_stmt", "SELECT * FROM nonexistent");
    auto describe = build_describe_message('S', "bad_stmt");
    auto sync = build_sync_message();

    std::vector<uint8_t> batch;
    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), describe.begin(), describe.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd(client_fd, batch);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    auto response = read_from_fd(client_fd);

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    // ParseComplete ('1').
    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len));
    // ParameterDescription ('t').
    ASSERT_TRUE(find_message(response, pos, 't', payload, payload_len));
    // ErrorResponse ('E') — describer failed.
    ASSERT_TRUE(find_message(response, pos, 'E', payload, payload_len));
    // ReadyForQuery ('Z') from Sync.
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, DescribePortalDescriberError) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(35);

    handler.set_query_executor(
        [](const std::string& /*sql*/, const std::string& /*database*/) -> Result<QueryResult> { return ok(QueryResult{}); });

    handler.set_query_describer(
        [](const std::string& /*sql*/, const std::string& /*database*/) -> Result<std::vector<ColumnDescription>> {
            return make_error(StatusCode::NOT_FOUND, "table \"nonexistent\" does not exist");
        });

    do_startup(client_fd, conn, handler);

    auto parse = build_parse_message("", "SELECT * FROM nonexistent");
    auto bind = build_bind_message("bad_portal", "");
    auto describe = build_describe_message('P', "bad_portal");
    auto sync = build_sync_message();

    std::vector<uint8_t> batch;
    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), describe.begin(), describe.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd(client_fd, batch);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    auto response = read_from_fd(client_fd);

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    // ParseComplete ('1').
    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len));
    // BindComplete ('2').
    ASSERT_TRUE(find_message(response, pos, '2', payload, payload_len));
    // ErrorResponse ('E') — describer failed.
    ASSERT_TRUE(find_message(response, pos, 'E', payload, payload_len));
    // ReadyForQuery ('Z') from Sync.
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, ErrorInBatchSkipsUntilSync) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(15);

    int call_count = 0;
    handler.set_query_executor([&call_count](const std::string& /*sql*/, const std::string& /*database*/) -> Result<QueryResult> {
        ++call_count;
        return ok(QueryResult{});
    });

    do_startup(client_fd, conn, handler);

    // Bind to a non-existent statement (will fail), then Execute (should be skipped).
    auto bind = build_bind_message("", "nonexistent");
    auto execute = build_execute_message("");
    auto sync = build_sync_message();

    std::vector<uint8_t> batch;
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), execute.begin(), execute.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd(client_fd, batch);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    auto response = read_from_fd(client_fd);

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    // ErrorResponse ('E') for the failed bind.
    ASSERT_TRUE(find_message(response, pos, 'E', payload, payload_len));
    // ReadyForQuery ('Z') from Sync — Execute was skipped.
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    // The query executor should NOT have been called (Execute was skipped).
    EXPECT_EQ(call_count, 0);

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

// =============================================================================
// SQL splitting tests
// =============================================================================

TEST(PgProtocol, SplitSqlBasic) {
    auto stmts = split_sql_statements("SELECT 1; SELECT 2");
    ASSERT_EQ(stmts.size(), 2u);
    EXPECT_EQ(stmts[0], "SELECT 1");
    EXPECT_EQ(stmts[1], "SELECT 2");
}

TEST(PgProtocol, SplitSqlTrailingSemicolon) {
    auto stmts = split_sql_statements("SELECT 1;");
    ASSERT_EQ(stmts.size(), 1u);
    EXPECT_EQ(stmts[0], "SELECT 1");
}

TEST(PgProtocol, SplitSqlEmpty) {
    EXPECT_TRUE(split_sql_statements("").empty());
    EXPECT_TRUE(split_sql_statements("   ").empty());
    EXPECT_TRUE(split_sql_statements(";;;").empty());
}

TEST(PgProtocol, SplitSqlSingleQuotedSemicolon) {
    auto stmts = split_sql_statements("SELECT 'a;b'; SELECT 2");
    ASSERT_EQ(stmts.size(), 2u);
    EXPECT_EQ(stmts[0], "SELECT 'a;b'");
    EXPECT_EQ(stmts[1], "SELECT 2");
}

TEST(PgProtocol, SplitSqlDoubleQuotedSemicolon) {
    auto stmts = split_sql_statements("SELECT \"col;name\" FROM t; SELECT 2");
    ASSERT_EQ(stmts.size(), 2u);
    EXPECT_EQ(stmts[0], "SELECT \"col;name\" FROM t");
    EXPECT_EQ(stmts[1], "SELECT 2");
}

TEST(PgProtocol, SplitSqlDollarQuotedSemicolon) {
    auto stmts = split_sql_statements("SELECT $$a;b$$; SELECT 2");
    ASSERT_EQ(stmts.size(), 2u);
    EXPECT_EQ(stmts[0], "SELECT $$a;b$$");
    EXPECT_EQ(stmts[1], "SELECT 2");
}

TEST(PgProtocol, SplitSqlEscapedQuote) {
    // '' inside single-quoted string is an escaped single quote.
    auto stmts = split_sql_statements("SELECT 'it''s'; SELECT 2");
    ASSERT_EQ(stmts.size(), 2u);
    EXPECT_EQ(stmts[0], "SELECT 'it''s'");
    EXPECT_EQ(stmts[1], "SELECT 2");
}

TEST(PgProtocol, SplitSqlWhitespaceTrimming) {
    auto stmts = split_sql_statements("  SELECT 1  ;  SELECT 2  ");
    ASSERT_EQ(stmts.size(), 2u);
    EXPECT_EQ(stmts[0], "SELECT 1");
    EXPECT_EQ(stmts[1], "SELECT 2");
}

// =============================================================================
// Multi-statement simple query protocol tests
// =============================================================================

TEST(PgProtocol, MultiStatementSimpleQuery) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(20);

    int call_count = 0;
    handler.set_query_executor([&call_count](const std::string& sql, const std::string& /*database*/) -> Result<QueryResult> {
        ++call_count;
        if (sql == "CREATE TABLE t (id INT)") {
            QueryResult qr;
            qr.message = "CREATE TABLE";
            return ok(std::move(qr));
        }
        if (sql == "INSERT INTO t VALUES (1)") {
            QueryResult qr;
            qr.affected_rows = 1;
            qr.message = "INSERT";
            return ok(std::move(qr));
        }
        if (sql == "SELECT * FROM t") {
            QueryResult qr;
            qr.column_names = {"id"};
            qr.column_types = {TypeId::INT32};
            qr.rows = {{Value(static_cast<int32_t>(1))}};
            return ok(std::move(qr));
        }
        return make_error(StatusCode::INTERNAL_ERROR, "unexpected query");
    });

    do_startup(client_fd, conn, handler);

    // Send three statements in a single Query message.
    auto query =
        build_query_message("CREATE TABLE t (id INT); INSERT INTO t VALUES (1); SELECT * FROM t");
    write_to_fd(client_fd, query);
    (void)conn.read_from_socket();
    auto result = handler.process(conn);
    ASSERT_TRUE(result.has_value());

    (void)conn.write_to_socket();
    auto response = read_from_fd(client_fd);
    ASSERT_FALSE(response.empty());

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    // Statement 1: CommandComplete for CREATE TABLE.
    ASSERT_TRUE(find_message(response, pos, 'C', payload, payload_len));
    MessageReader cc1(payload, payload_len);
    EXPECT_EQ(cc1.read_cstring(), "CREATE TABLE");

    // Statement 2: CommandComplete for INSERT.
    ASSERT_TRUE(find_message(response, pos, 'C', payload, payload_len));
    MessageReader cc2(payload, payload_len);
    EXPECT_EQ(cc2.read_cstring(), "INSERT 0 1");

    // Statement 3: RowDescription + DataRow + CommandComplete for SELECT.
    ASSERT_TRUE(find_message(response, pos, 'T', payload, payload_len));
    ASSERT_TRUE(find_message(response, pos, 'D', payload, payload_len));
    ASSERT_TRUE(find_message(response, pos, 'C', payload, payload_len));
    MessageReader cc3(payload, payload_len);
    EXPECT_EQ(cc3.read_cstring(), "SELECT 1");

    // Single ReadyForQuery at the end.
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));
    EXPECT_EQ(payload[0], 'I');

    // All three statements were executed.
    EXPECT_EQ(call_count, 3);

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(PgProtocol, MultiStatementErrorStopsExecution) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(21);

    int call_count = 0;
    handler.set_query_executor([&call_count](const std::string& sql, const std::string& /*database*/) -> Result<QueryResult> {
        ++call_count;
        if (sql == "SELECT 1") {
            QueryResult qr;
            qr.column_names = {"col"};
            qr.column_types = {TypeId::INT32};
            qr.rows = {{Value(static_cast<int32_t>(1))}};
            return ok(std::move(qr));
        }
        // Second statement fails.
        return make_error(StatusCode::PARSE_ERROR, "syntax error");
    });

    do_startup(client_fd, conn, handler);

    // Three statements; the second will fail.
    auto query = build_query_message("SELECT 1; BAD SQL; SELECT 3");
    write_to_fd(client_fd, query);
    (void)conn.read_from_socket();
    auto result = handler.process(conn);
    ASSERT_TRUE(result.has_value());

    (void)conn.write_to_socket();
    auto response = read_from_fd(client_fd);
    ASSERT_FALSE(response.empty());

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    // Statement 1 succeeds: RowDescription + DataRow + CommandComplete.
    ASSERT_TRUE(find_message(response, pos, 'T', payload, payload_len));
    ASSERT_TRUE(find_message(response, pos, 'D', payload, payload_len));
    ASSERT_TRUE(find_message(response, pos, 'C', payload, payload_len));

    // Statement 2 fails: ErrorResponse.
    ASSERT_TRUE(find_message(response, pos, 'E', payload, payload_len));

    // ReadyForQuery after error — third statement was NOT executed.
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));
    EXPECT_EQ(payload[0], 'I');

    // Only 2 calls: first succeeded, second failed, third skipped.
    EXPECT_EQ(call_count, 2);

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

// =============================================================================
// Binary value formatting tests (GDB-202)
// =============================================================================

// Helper: read a big-endian int16 from bytes.
static int16_t read_be16(const std::vector<uint8_t>& buf, size_t offset = 0) {
    return static_cast<int16_t>((static_cast<uint16_t>(buf[offset]) << 8) |
                                static_cast<uint16_t>(buf[offset + 1]));
}

// Helper: read a big-endian int32 from bytes.
static int32_t read_be32(const std::vector<uint8_t>& buf, size_t offset = 0) {
    return static_cast<int32_t>((static_cast<uint32_t>(buf[offset]) << 24) |
                                (static_cast<uint32_t>(buf[offset + 1]) << 16) |
                                (static_cast<uint32_t>(buf[offset + 2]) << 8) |
                                static_cast<uint32_t>(buf[offset + 3]));
}

// Helper: read a big-endian int64 from bytes.
static int64_t read_be64(const std::vector<uint8_t>& buf, size_t offset = 0) {
    return static_cast<int64_t>((static_cast<uint64_t>(buf[offset]) << 56) |
                                (static_cast<uint64_t>(buf[offset + 1]) << 48) |
                                (static_cast<uint64_t>(buf[offset + 2]) << 40) |
                                (static_cast<uint64_t>(buf[offset + 3]) << 32) |
                                (static_cast<uint64_t>(buf[offset + 4]) << 24) |
                                (static_cast<uint64_t>(buf[offset + 5]) << 16) |
                                (static_cast<uint64_t>(buf[offset + 6]) << 8) |
                                static_cast<uint64_t>(buf[offset + 7]));
}

TEST(PgProtocol, BinaryBool) {
    auto bin_true = value_to_pg_binary(Value(true));
    ASSERT_EQ(bin_true.size(), 1u);
    EXPECT_EQ(bin_true[0], 1);

    auto bin_false = value_to_pg_binary(Value(false));
    ASSERT_EQ(bin_false.size(), 1u);
    EXPECT_EQ(bin_false[0], 0);
}

TEST(PgProtocol, BinaryInt32) {
    auto bin = value_to_pg_binary(Value(static_cast<int32_t>(42)));
    ASSERT_EQ(bin.size(), 4u);
    EXPECT_EQ(read_be32(bin), 42);

    auto bin_neg = value_to_pg_binary(Value(static_cast<int32_t>(-1)));
    ASSERT_EQ(bin_neg.size(), 4u);
    EXPECT_EQ(read_be32(bin_neg), -1);

    auto bin_zero = value_to_pg_binary(Value(static_cast<int32_t>(0)));
    ASSERT_EQ(bin_zero.size(), 4u);
    EXPECT_EQ(read_be32(bin_zero), 0);

    auto bin_max = value_to_pg_binary(Value(std::numeric_limits<int32_t>::max()));
    ASSERT_EQ(bin_max.size(), 4u);
    EXPECT_EQ(read_be32(bin_max), std::numeric_limits<int32_t>::max());

    auto bin_min = value_to_pg_binary(Value(std::numeric_limits<int32_t>::min()));
    ASSERT_EQ(bin_min.size(), 4u);
    EXPECT_EQ(read_be32(bin_min), std::numeric_limits<int32_t>::min());
}

TEST(PgProtocol, BinaryInt64) {
    auto bin = value_to_pg_binary(Value(static_cast<int64_t>(123456789012345LL)));
    ASSERT_EQ(bin.size(), 8u);
    EXPECT_EQ(read_be64(bin), 123456789012345LL);

    auto bin_neg = value_to_pg_binary(Value(static_cast<int64_t>(-100)));
    ASSERT_EQ(bin_neg.size(), 8u);
    EXPECT_EQ(read_be64(bin_neg), -100LL);

    auto bin_max = value_to_pg_binary(Value(std::numeric_limits<int64_t>::max()));
    ASSERT_EQ(bin_max.size(), 8u);
    EXPECT_EQ(read_be64(bin_max), std::numeric_limits<int64_t>::max());
}

TEST(PgProtocol, BinaryInt16) {
    auto bin = value_to_pg_binary(Value(static_cast<int16_t>(1234)));
    ASSERT_EQ(bin.size(), 2u);
    EXPECT_EQ(read_be16(bin), 1234);

    auto bin_neg = value_to_pg_binary(Value(static_cast<int16_t>(-1)));
    ASSERT_EQ(bin_neg.size(), 2u);
    EXPECT_EQ(read_be16(bin_neg), -1);
}

TEST(PgProtocol, BinaryInt8) {
    // INT8 maps to PG int2 (2 bytes).
    auto bin = value_to_pg_binary(Value(static_cast<int8_t>(42)));
    ASSERT_EQ(bin.size(), 2u);
    EXPECT_EQ(read_be16(bin), 42);

    auto bin_neg = value_to_pg_binary(Value(static_cast<int8_t>(-1)));
    ASSERT_EQ(bin_neg.size(), 2u);
    EXPECT_EQ(read_be16(bin_neg), -1);
}

TEST(PgProtocol, BinaryFloat32) {
    float val = 3.14f;
    auto bin = value_to_pg_binary(Value(val));
    ASSERT_EQ(bin.size(), 4u);

    // Reconstruct float from big-endian bytes.
    uint32_t bits = static_cast<uint32_t>(read_be32(bin));
    float result = 0;
    std::memcpy(&result, &bits, sizeof(result));
    EXPECT_FLOAT_EQ(result, val);
}

TEST(PgProtocol, BinaryFloat64) {
    double val = 2.718281828459045;
    auto bin = value_to_pg_binary(Value(val));
    ASSERT_EQ(bin.size(), 8u);

    // Reconstruct double from big-endian bytes.
    uint64_t bits = static_cast<uint64_t>(read_be64(bin));
    double result = 0;
    std::memcpy(&result, &bits, sizeof(result));
    EXPECT_DOUBLE_EQ(result, val);
}

TEST(PgProtocol, BinaryFloat32SpecialValues) {
    // Zero.
    auto bin_zero = value_to_pg_binary(Value(0.0f));
    ASSERT_EQ(bin_zero.size(), 4u);
    uint32_t bits_zero = static_cast<uint32_t>(read_be32(bin_zero));
    float f_zero = 0;
    std::memcpy(&f_zero, &bits_zero, sizeof(f_zero));
    EXPECT_EQ(f_zero, 0.0f);

    // Negative.
    auto bin_neg = value_to_pg_binary(Value(-1.5f));
    ASSERT_EQ(bin_neg.size(), 4u);
    uint32_t bits_neg = static_cast<uint32_t>(read_be32(bin_neg));
    float f_neg = 0;
    std::memcpy(&f_neg, &bits_neg, sizeof(f_neg));
    EXPECT_FLOAT_EQ(f_neg, -1.5f);
}

TEST(PgProtocol, BinaryString) {
    auto bin = value_to_pg_binary(Value(std::string("hello")));
    ASSERT_EQ(bin.size(), 5u);
    EXPECT_EQ(std::string(bin.begin(), bin.end()), "hello");
}

TEST(PgProtocol, BinaryStringEmpty) {
    auto bin = value_to_pg_binary(Value(std::string("")));
    EXPECT_EQ(bin.size(), 0u);
}

TEST(PgProtocol, BinaryNull) {
    Value null_val;
    EXPECT_TRUE(null_val.is_null());
    auto bin = value_to_pg_binary(null_val);
    EXPECT_EQ(bin.size(), 0u);
}

TEST(PgProtocol, BinaryUnsignedTypes) {
    // UINT8 → PG int2 (2 bytes).
    auto bin_u8 = value_to_pg_binary(Value(static_cast<uint8_t>(255)));
    ASSERT_EQ(bin_u8.size(), 2u);
    EXPECT_EQ(read_be16(bin_u8), 255);

    // UINT16 → PG int4 (4 bytes).
    auto bin_u16 = value_to_pg_binary(Value(static_cast<uint16_t>(65535)));
    ASSERT_EQ(bin_u16.size(), 4u);
    EXPECT_EQ(read_be32(bin_u16), 65535);

    // UINT32 → PG int8 (8 bytes).
    auto bin_u32 = value_to_pg_binary(Value(static_cast<uint32_t>(4294967295u)));
    ASSERT_EQ(bin_u32.size(), 8u);
    EXPECT_EQ(read_be64(bin_u32), 4294967295LL);
}

TEST(PgProtocol, BinaryDateIsPgBinaryNotText) {
    // GDB-718: DATE no longer falls back to text — it uses the PostgreSQL
    // binary encoding (int32 days since 2000-01-01, big-endian).
    Date date{19737}; // 2024-01-15 = Unix day 19737 = PG day 8780.
    auto bin = value_to_pg_binary(Value(date));
    ASSERT_EQ(bin.size(), 4u);
    EXPECT_EQ(read_be32(bin), 19737 - 10957);
}
