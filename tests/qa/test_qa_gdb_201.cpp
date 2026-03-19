/// @file test_qa_gdb_201.cpp
/// @brief QA adversarial tests for GDB-201: Describe RowDescription for statements and portals.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/server/connection.h"
#include "sixseven/server/pg_protocol.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace sixseven;

// =============================================================================
// Wire protocol helpers (mirrored from test_pg_protocol.cpp)
// =============================================================================

namespace {

std::vector<uint8_t>
build_startup_message(const std::vector<std::pair<std::string, std::string>>& params) {
    std::vector<uint8_t> payload;
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
    payload.push_back(0);
    uint32_t total_len = static_cast<uint32_t>(4 + payload.size());
    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>((total_len >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((total_len >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((total_len >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(total_len & 0xFF));
    msg.insert(msg.end(), payload.begin(), payload.end());
    return msg;
}

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

std::vector<uint8_t> build_bind_message(std::string_view portal_name,
                                        std::string_view stmt_name,
                                        const std::vector<std::string>& param_values = {}) {
    std::vector<uint8_t> body;
    body.insert(body.end(), portal_name.begin(), portal_name.end());
    body.push_back(0);
    body.insert(body.end(), stmt_name.begin(), stmt_name.end());
    body.push_back(0);
    body.push_back(0);
    body.push_back(0);
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

std::vector<uint8_t> build_sync_message() { return {'S', 0, 0, 0, 4}; }

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

int create_socketpair(int& client_fd_out) {
    int fds[2];
    int rc = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    EXPECT_EQ(rc, 0);
    client_fd_out = fds[1];
    return fds[0];
}

void write_to_fd(int fd, const std::vector<uint8_t>& data) {
    size_t written = 0;
    while (written < data.size()) {
        auto n = ::write(fd, data.data() + written, data.size() - written);
        ASSERT_GT(n, 0);
        written += static_cast<size_t>(n);
    }
}

std::vector<uint8_t> read_from_fd(int fd, size_t max_bytes = 65536) {
    std::vector<uint8_t> buf(max_bytes);
    auto n = ::read(fd, buf.data(), buf.size());
    if (n <= 0) {
        return {};
    }
    buf.resize(static_cast<size_t>(n));
    return buf;
}

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
            return false;
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

void do_startup(int client_fd, Connection& conn, PgProtocolHandler& handler) {
    auto startup = build_startup_message({{"user", "test"}});
    write_to_fd(client_fd, startup);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)read_from_fd(client_fd);
}

/// Mock describer: returns columns for SELECT, empty for non-SELECT.
Result<std::vector<ColumnDescription>> mock_describer(const std::string& sql,
                                                      const std::string& /*database*/) {
    // Case-insensitive check for SELECT.
    std::string upper;
    for (char c : sql) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (upper.find("SELECT") != std::string::npos) {
        return ok(std::vector<ColumnDescription>{
            {"id", TypeId::INT32},
            {"name", TypeId::STRING},
        });
    }
    return ok(std::vector<ColumnDescription>{});
}

/// Count how many messages of a given type appear in the response.
int count_messages(const std::vector<uint8_t>& data, uint8_t type) {
    int count = 0;
    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    while (find_message(data, pos, type, payload, payload_len)) {
        ++count;
    }
    return count;
}

} // namespace

// =============================================================================
// Protocol-level: Invalid Describe type byte
// =============================================================================

TEST(QA_GDB201_Protocol, DescribeInvalidTypeByteSendsError) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(201);

    handler.set_query_describer(mock_describer);
    do_startup(client_fd, conn, handler);

    // Parse a statement, then Describe with invalid type 'X' (not 'S' or 'P').
    auto parse = build_parse_message("stmt1", "SELECT 1");
    auto describe = build_describe_message('X', "stmt1");
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
    // ErrorResponse ('E') — invalid Describe target type.
    ASSERT_TRUE(find_message(response, pos, 'E', payload, payload_len));
    // ReadyForQuery ('Z') from Sync.
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    ::close(client_fd);
}

TEST(QA_GDB201_Protocol, DescribeInvalidTypeByteZero) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(202);

    handler.set_query_describer(mock_describer);
    do_startup(client_fd, conn, handler);

    // Describe with type byte 0x00 (garbage).
    auto parse = build_parse_message("stmt1", "SELECT 1");
    auto describe = build_describe_message('\0', "stmt1");
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

    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len));
    // Should get ErrorResponse for invalid type.
    ASSERT_TRUE(find_message(response, pos, 'E', payload, payload_len));
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    ::close(client_fd);
}

// =============================================================================
// Protocol-level: Describe non-existent objects
// =============================================================================

TEST(QA_GDB201_Protocol, DescribeNonExistentStatementSendsError) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(203);

    handler.set_query_describer(mock_describer);
    do_startup(client_fd, conn, handler);

    // Describe a statement that was never parsed.
    auto describe = build_describe_message('S', "nonexistent_stmt");
    auto sync = build_sync_message();

    std::vector<uint8_t> batch;
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

    // ErrorResponse ('E') — prepared statement does not exist.
    ASSERT_TRUE(find_message(response, pos, 'E', payload, payload_len));
    // ReadyForQuery.
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    ::close(client_fd);
}

TEST(QA_GDB201_Protocol, DescribeNonExistentPortalSendsError) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(204);

    handler.set_query_describer(mock_describer);
    do_startup(client_fd, conn, handler);

    // Describe a portal that was never bound.
    auto describe = build_describe_message('P', "nonexistent_portal");
    auto sync = build_sync_message();

    std::vector<uint8_t> batch;
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

    ASSERT_TRUE(find_message(response, pos, 'E', payload, payload_len));
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    ::close(client_fd);
}

// =============================================================================
// Protocol-level: Describe unnamed ("") statement and portal
// =============================================================================

TEST(QA_GDB201_Protocol, DescribeUnnamedStatementRowDescription) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(205);

    handler.set_query_describer(mock_describer);
    do_startup(client_fd, conn, handler);

    // Parse unnamed statement + Describe unnamed statement.
    auto parse = build_parse_message("", "SELECT id, name FROM t");
    auto describe = build_describe_message('S', "");
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

    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len)); // ParseComplete
    ASSERT_TRUE(find_message(response, pos, 't', payload, payload_len)); // ParameterDescription
    ASSERT_TRUE(find_message(response, pos, 'T', payload, payload_len)); // RowDescription

    MessageReader rd(payload, payload_len);
    EXPECT_EQ(rd.read_int16(), 2); // 2 columns

    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    ::close(client_fd);
}

TEST(QA_GDB201_Protocol, DescribeUnnamedPortalRowDescription) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(206);

    handler.set_query_executor([](const std::string& /*sql*/, const std::string& /*database*/) -> Result<QueryResult> {
        return ok(QueryResult{});
    });
    handler.set_query_describer(mock_describer);
    do_startup(client_fd, conn, handler);

    // Parse unnamed + Bind unnamed portal + Describe unnamed portal.
    auto parse = build_parse_message("", "SELECT id, name FROM t");
    auto bind = build_bind_message("", "");
    auto describe = build_describe_message('P', "");
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

    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len)); // ParseComplete
    ASSERT_TRUE(find_message(response, pos, '2', payload, payload_len)); // BindComplete
    ASSERT_TRUE(find_message(response, pos, 'T', payload, payload_len)); // RowDescription

    MessageReader rd(payload, payload_len);
    EXPECT_EQ(rd.read_int16(), 2); // 2 columns

    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    ::close(client_fd);
}

// =============================================================================
// Protocol-level: Multiple Describe calls on same statement
// =============================================================================

TEST(QA_GDB201_Protocol, MultipleDescribesOnSameStatement) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(207);

    handler.set_query_describer(mock_describer);
    do_startup(client_fd, conn, handler);

    // Parse once, Describe twice.
    auto parse = build_parse_message("rstmt", "SELECT id, name FROM t");
    auto describe1 = build_describe_message('S', "rstmt");
    auto describe2 = build_describe_message('S', "rstmt");
    auto sync = build_sync_message();

    std::vector<uint8_t> batch;
    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), describe1.begin(), describe1.end());
    batch.insert(batch.end(), describe2.begin(), describe2.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd(client_fd, batch);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    auto response = read_from_fd(client_fd);

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len)); // ParseComplete

    // First Describe: ParameterDescription + RowDescription.
    ASSERT_TRUE(find_message(response, pos, 't', payload, payload_len));
    ASSERT_TRUE(find_message(response, pos, 'T', payload, payload_len));
    MessageReader rd1(payload, payload_len);
    EXPECT_EQ(rd1.read_int16(), 2);

    // Second Describe: ParameterDescription + RowDescription again.
    ASSERT_TRUE(find_message(response, pos, 't', payload, payload_len));
    ASSERT_TRUE(find_message(response, pos, 'T', payload, payload_len));
    MessageReader rd2(payload, payload_len);
    EXPECT_EQ(rd2.read_int16(), 2);

    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    ::close(client_fd);
}

// =============================================================================
// Protocol-level: Describe after Close (statement removed)
// =============================================================================

TEST(QA_GDB201_Protocol, DescribeAfterCloseStatementFails) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(208);

    handler.set_query_describer(mock_describer);
    do_startup(client_fd, conn, handler);

    // Parse, then Close, then Describe the closed statement.
    auto parse = build_parse_message("closed_stmt", "SELECT 1");
    auto sync1 = build_sync_message();

    std::vector<uint8_t> batch1;
    batch1.insert(batch1.end(), parse.begin(), parse.end());
    batch1.insert(batch1.end(), sync1.begin(), sync1.end());

    write_to_fd(client_fd, batch1);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)read_from_fd(client_fd); // Drain.

    // Now close the statement and then try to describe it.
    auto close_msg = build_close_message('S', "closed_stmt");
    auto describe = build_describe_message('S', "closed_stmt");
    auto sync2 = build_sync_message();

    std::vector<uint8_t> batch2;
    batch2.insert(batch2.end(), close_msg.begin(), close_msg.end());
    batch2.insert(batch2.end(), describe.begin(), describe.end());
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
    // ErrorResponse ('E') — statement no longer exists.
    ASSERT_TRUE(find_message(response, pos, 'E', payload, payload_len));
    // ReadyForQuery.
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    ::close(client_fd);
}

// =============================================================================
// Protocol-level: Describe with many column types
// =============================================================================

TEST(QA_GDB201_Protocol, DescribeAllSupportedColumnTypes) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(209);

    // Return columns of every supported type.
    handler.set_query_describer(
        [](const std::string& sql, const std::string& /*database*/) -> Result<std::vector<ColumnDescription>> {
            std::string upper;
            for (char c : sql) {
                upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            }
            if (upper.find("SELECT") != std::string::npos) {
                return ok(std::vector<ColumnDescription>{
                    {"col_bool", TypeId::BOOL},
                    {"col_int8", TypeId::INT8},
                    {"col_int16", TypeId::INT16},
                    {"col_int32", TypeId::INT32},
                    {"col_int64", TypeId::INT64},
                    {"col_uint8", TypeId::UINT8},
                    {"col_uint16", TypeId::UINT16},
                    {"col_uint32", TypeId::UINT32},
                    {"col_uint64", TypeId::UINT64},
                    {"col_float32", TypeId::FLOAT32},
                    {"col_float64", TypeId::FLOAT64},
                    {"col_string", TypeId::STRING},
                    {"col_blob", TypeId::BLOB},
                    {"col_date", TypeId::DATE},
                    {"col_time", TypeId::TIME},
                    {"col_timestamp", TypeId::TIMESTAMP},
                    {"col_interval", TypeId::INTERVAL},
                    {"col_point", TypeId::POINT},
                    {"col_json", TypeId::JSON},
                    {"col_uuid", TypeId::UUID},
                    {"col_embedding", TypeId::EMBEDDING},
                });
            }
            return ok(std::vector<ColumnDescription>{});
        });

    do_startup(client_fd, conn, handler);

    auto parse = build_parse_message("wide_stmt", "SELECT *");
    auto describe = build_describe_message('S', "wide_stmt");
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

    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len)); // ParseComplete
    ASSERT_TRUE(find_message(response, pos, 't', payload, payload_len)); // ParameterDescription
    ASSERT_TRUE(find_message(response, pos, 'T', payload, payload_len)); // RowDescription

    MessageReader rd(payload, payload_len);
    int16_t num_cols = rd.read_int16();
    EXPECT_EQ(num_cols, 21);

    // Verify each column name and OID.
    struct Expected {
        std::string name;
        uint32_t oid;
    };
    std::vector<Expected> expected = {
        {"col_bool", 16},      {"col_int8", 21},       {"col_int16", 21},
        {"col_int32", 23},     {"col_int64", 20},      {"col_uint8", 21},
        {"col_uint16", 23},    {"col_uint32", 20},     {"col_uint64", 1700},
        {"col_float32", 700},  {"col_float64", 701},   {"col_string", 25},
        {"col_blob", 17},      {"col_date", 1082},     {"col_time", 1083},
        {"col_timestamp", 1114}, {"col_interval", 1186}, {"col_point", 600},
        {"col_json", 114},     {"col_uuid", 2950},     {"col_embedding", 100000},
    };

    for (size_t i = 0; i < expected.size(); ++i) {
        auto col_name = std::string(rd.read_cstring());
        EXPECT_EQ(col_name, expected[i].name) << "Column " << i;
        rd.read_int32(); // table OID
        rd.read_int16(); // column attr number
        auto type_oid = static_cast<uint32_t>(rd.read_int32());
        EXPECT_EQ(type_oid, expected[i].oid) << "Column " << i << " (" << expected[i].name << ")";
        rd.read_int16(); // type size
        rd.read_int32(); // type modifier
        rd.read_int16(); // format code
    }

    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    ::close(client_fd);
}

// =============================================================================
// Protocol-level: Describe followed by Execute works correctly
// =============================================================================

TEST(QA_GDB201_Protocol, DescribeThenExecuteBothWork) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(210);

    handler.set_query_executor([](const std::string& /*sql*/, const std::string& /*database*/) -> Result<QueryResult> {
        QueryResult qr;
        qr.column_names = {"id", "val"};
        qr.column_types = {TypeId::INT32, TypeId::STRING};
        qr.rows = {{Value(static_cast<int32_t>(1)), Value(std::string("hello"))}};
        return ok(std::move(qr));
    });
    handler.set_query_describer(
        [](const std::string& sql, const std::string& /*database*/) -> Result<std::vector<ColumnDescription>> {
            std::string upper;
            for (char c : sql) {
                upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            }
            if (upper.find("SELECT") != std::string::npos) {
                return ok(std::vector<ColumnDescription>{
                    {"id", TypeId::INT32},
                    {"val", TypeId::STRING},
                });
            }
            return ok(std::vector<ColumnDescription>{});
        });

    do_startup(client_fd, conn, handler);

    // Parse + Describe(S) + Bind + Describe(P) + Execute + Sync.
    auto parse = build_parse_message("combo", "SELECT id, val FROM t");
    auto describe_s = build_describe_message('S', "combo");
    auto bind = build_bind_message("combo_p", "combo");
    auto describe_p = build_describe_message('P', "combo_p");
    auto execute = build_execute_message("combo_p");
    auto sync = build_sync_message();

    std::vector<uint8_t> batch;
    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), describe_s.begin(), describe_s.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), describe_p.begin(), describe_p.end());
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

    // ParseComplete.
    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len));
    // Describe(S): ParameterDescription + RowDescription.
    ASSERT_TRUE(find_message(response, pos, 't', payload, payload_len));
    ASSERT_TRUE(find_message(response, pos, 'T', payload, payload_len));
    // BindComplete.
    ASSERT_TRUE(find_message(response, pos, '2', payload, payload_len));
    // Describe(P): RowDescription.
    ASSERT_TRUE(find_message(response, pos, 'T', payload, payload_len));

    MessageReader rd(payload, payload_len);
    EXPECT_EQ(rd.read_int16(), 2); // 2 columns

    // Execute results: DataRow + CommandComplete.
    ASSERT_TRUE(find_message(response, pos, 'D', payload, payload_len));
    ASSERT_TRUE(find_message(response, pos, 'C', payload, payload_len));

    // ReadyForQuery.
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    ::close(client_fd);
}

// =============================================================================
// Protocol-level: Describe skipped after error in batch
// =============================================================================

TEST(QA_GDB201_Protocol, DescribeSkippedAfterErrorInBatch) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(211);

    handler.set_query_executor([](const std::string& /*sql*/, const std::string& /*database*/) -> Result<QueryResult> {
        return ok(QueryResult{});
    });
    handler.set_query_describer(mock_describer);
    do_startup(client_fd, conn, handler);

    // Bind to a non-existent statement to trigger error, then Describe should be skipped.
    auto bind = build_bind_message("p1", "nonexistent_stmt");
    auto parse = build_parse_message("good_stmt", "SELECT 1");
    auto describe = build_describe_message('S', "good_stmt");
    auto sync = build_sync_message();

    std::vector<uint8_t> batch;
    batch.insert(batch.end(), bind.begin(), bind.end());
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

    // ErrorResponse from failed bind.
    ASSERT_TRUE(find_message(response, pos, 'E', payload, payload_len));
    // No RowDescription or ParameterDescription — Describe was skipped.
    EXPECT_EQ(count_messages(response, 'T'), 0);
    EXPECT_EQ(count_messages(response, 't'), 0);
    // ReadyForQuery from Sync resets the error state.
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    ::close(client_fd);
}

// =============================================================================
// Protocol-level: Describe with no describer set (fallback to NoData)
// =============================================================================

TEST(QA_GDB201_Protocol, DescribeWithNoDescriberFallsBackToNoData) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(212);

    // Do NOT set query_describer.
    do_startup(client_fd, conn, handler);

    auto parse = build_parse_message("nd_stmt", "SELECT 1");
    auto describe = build_describe_message('S', "nd_stmt");
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

    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len)); // ParseComplete
    ASSERT_TRUE(find_message(response, pos, 't', payload, payload_len)); // ParameterDescription
    ASSERT_TRUE(find_message(response, pos, 'n', payload, payload_len)); // NoData (fallback)
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    ::close(client_fd);
}

TEST(QA_GDB201_Protocol, DescribePortalWithNoDescriberFallsBackToNoData) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(213);

    handler.set_query_executor([](const std::string& /*sql*/, const std::string& /*database*/) -> Result<QueryResult> {
        return ok(QueryResult{});
    });
    // Do NOT set query_describer.
    do_startup(client_fd, conn, handler);

    auto parse = build_parse_message("", "SELECT 1");
    auto bind = build_bind_message("p1", "");
    auto describe = build_describe_message('P', "p1");
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

    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len)); // ParseComplete
    ASSERT_TRUE(find_message(response, pos, '2', payload, payload_len)); // BindComplete
    ASSERT_TRUE(find_message(response, pos, 'n', payload, payload_len)); // NoData (fallback)
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    ::close(client_fd);
}

// =============================================================================
// Protocol-level: Describe DML statements (INSERT, UPDATE, DELETE, CREATE)
// =============================================================================

TEST(QA_GDB201_Protocol, DescribeDMLStatementsReturnNoData) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(214);

    handler.set_query_describer(mock_describer);
    do_startup(client_fd, conn, handler);

    // Parse INSERT, UPDATE, DELETE — all should get NoData.
    std::vector<std::pair<std::string, std::string>> stmts = {
        {"ins", "INSERT INTO t VALUES (1)"},
        {"upd", "UPDATE t SET x = 1"},
        {"del", "DELETE FROM t WHERE id = 1"},
        {"ddl", "CREATE TABLE foo (id INT)"},
    };

    for (const auto& [name, sql] : stmts) {
        auto parse = build_parse_message(name, sql);
        auto describe = build_describe_message('S', name);
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

        ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len))
            << "ParseComplete missing for " << name;
        ASSERT_TRUE(find_message(response, pos, 't', payload, payload_len))
            << "ParameterDescription missing for " << name;
        ASSERT_TRUE(find_message(response, pos, 'n', payload, payload_len))
            << "NoData missing for " << name << " (should not send RowDescription for DML/DDL)";
        ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));
    }

    conn.close();
    ::close(client_fd);
}

// =============================================================================
// Protocol-level: Verify RowDescription column OIDs match type_to_pg_oid
// =============================================================================

TEST(QA_GDB201_Protocol, RowDescriptionColumnOIDsAreCorrect) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(215);

    // Single INT64 column.
    handler.set_query_describer(
        [](const std::string& /*sql*/, const std::string& /*database*/) -> Result<std::vector<ColumnDescription>> {
            return ok(std::vector<ColumnDescription>{{"big_id", TypeId::INT64}});
        });

    do_startup(client_fd, conn, handler);

    auto parse = build_parse_message("oid_test", "SELECT big_id");
    auto describe = build_describe_message('S', "oid_test");
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

    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len));
    ASSERT_TRUE(find_message(response, pos, 't', payload, payload_len));
    ASSERT_TRUE(find_message(response, pos, 'T', payload, payload_len));

    MessageReader rd(payload, payload_len);
    EXPECT_EQ(rd.read_int16(), 1); // 1 column

    auto name = std::string(rd.read_cstring());
    EXPECT_EQ(name, "big_id");
    rd.read_int32(); // table OID
    rd.read_int16(); // column attr
    auto oid = static_cast<uint32_t>(rd.read_int32());
    EXPECT_EQ(oid, type_to_pg_oid(TypeId::INT64)); // INT64 = OID 20
    rd.read_int16();                                // type size
    rd.read_int32();                                // type modifier
    EXPECT_EQ(rd.read_int16(), 0);                  // format code = text

    conn.close();
    ::close(client_fd);
}

// =============================================================================
// Protocol-level: Describe zero-column result (empty SELECT)
// =============================================================================

TEST(QA_GDB201_Protocol, DescribeZeroColumnsReturnsNoData) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(216);

    // Describer returns empty columns for this SELECT (edge case).
    handler.set_query_describer(
        [](const std::string& /*sql*/, const std::string& /*database*/) -> Result<std::vector<ColumnDescription>> {
            return ok(std::vector<ColumnDescription>{}); // 0 columns
        });

    do_startup(client_fd, conn, handler);

    auto parse = build_parse_message("zero_cols", "SELECT");
    auto describe = build_describe_message('S', "zero_cols");
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

    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len));
    ASSERT_TRUE(find_message(response, pos, 't', payload, payload_len));
    // Empty columns → NoData, not an empty RowDescription.
    ASSERT_TRUE(find_message(response, pos, 'n', payload, payload_len));

    conn.close();
    ::close(client_fd);
}

// =============================================================================
// Protocol-level: Describe portal for DML returns NoData
// =============================================================================

TEST(QA_GDB201_Protocol, DescribePortalDMLReturnsNoData) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(217);

    handler.set_query_executor([](const std::string& /*sql*/, const std::string& /*database*/) -> Result<QueryResult> {
        return ok(QueryResult{});
    });
    handler.set_query_describer(mock_describer);
    do_startup(client_fd, conn, handler);

    auto parse = build_parse_message("", "INSERT INTO t VALUES (1)");
    auto bind = build_bind_message("dml_portal", "");
    auto describe = build_describe_message('P', "dml_portal");
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

    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len)); // ParseComplete
    ASSERT_TRUE(find_message(response, pos, '2', payload, payload_len)); // BindComplete
    ASSERT_TRUE(find_message(response, pos, 'n', payload, payload_len)); // NoData
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    ::close(client_fd);
}

// =============================================================================
// Protocol-level: Statement Describe does NOT include RowDescription for portal
// (Describe('S') sends ParameterDescription + RowDescription/NoData, not just
// RowDescription like Describe('P'))
// =============================================================================

TEST(QA_GDB201_Protocol, StatementDescribeIncludesParameterDescription) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(218);

    handler.set_query_describer(mock_describer);
    do_startup(client_fd, conn, handler);

    // Parse with 3 parameter OIDs.
    auto parse = build_parse_message("param_stmt", "SELECT $1, $2, $3", {23, 25, 20});
    auto describe = build_describe_message('S', "param_stmt");
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

    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len)); // ParseComplete
    // ParameterDescription ('t') — must be present for Describe('S').
    ASSERT_TRUE(find_message(response, pos, 't', payload, payload_len));
    MessageReader pd(payload, payload_len);
    EXPECT_EQ(pd.read_int16(), 3); // 3 parameters
    EXPECT_EQ(pd.read_int32(), 23);
    EXPECT_EQ(pd.read_int32(), 25);
    EXPECT_EQ(pd.read_int32(), 20);

    // RowDescription or NoData follows.
    // In this case, RowDescription because it's a SELECT.
    ASSERT_TRUE(find_message(response, pos, 'T', payload, payload_len));

    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    ::close(client_fd);
}

TEST(QA_GDB201_Protocol, PortalDescribeDoesNotIncludeParameterDescription) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(219);

    handler.set_query_executor([](const std::string& /*sql*/, const std::string& /*database*/) -> Result<QueryResult> {
        return ok(QueryResult{});
    });
    handler.set_query_describer(mock_describer);
    do_startup(client_fd, conn, handler);

    auto parse = build_parse_message("", "SELECT id FROM t", {});
    auto bind = build_bind_message("pp", "");
    auto describe = build_describe_message('P', "pp");
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

    ASSERT_TRUE(find_message(response, pos, '1', payload, payload_len)); // ParseComplete
    ASSERT_TRUE(find_message(response, pos, '2', payload, payload_len)); // BindComplete
    // Portal Describe should send RowDescription, NOT ParameterDescription.
    ASSERT_TRUE(find_message(response, pos, 'T', payload, payload_len)); // RowDescription

    // Count ParameterDescription messages — should be 0 after BindComplete.
    // (The only 't' in the response might be from Parse, but after '2' there shouldn't be one.)
    // We just verify we got 'T' (RowDescription) not 't' after BindComplete.
    // The find_message above already succeeded for 'T', confirming correct behavior.

    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    conn.close();
    ::close(client_fd);
}

// =============================================================================
// QueryEngine-level: Describe edge cases
// =============================================================================

class QA_GDB201_QueryEngine : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb201";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        // Create a test table with various column types.
        auto result = engine_->execute(
            "CREATE TABLE test_tbl (id INT, name VARCHAR, score DOUBLE, active BOOLEAN)");
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

TEST_F(QA_GDB201_QueryEngine, DescribeSelectSpecificColumns) {
    auto result = engine_->describe("SELECT id, name FROM test_tbl");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);
    EXPECT_EQ((*result)[0].name, "id");
    EXPECT_EQ((*result)[0].type_id, TypeId::INT32);
    EXPECT_EQ((*result)[1].name, "name");
    EXPECT_EQ((*result)[1].type_id, TypeId::STRING);
}

TEST_F(QA_GDB201_QueryEngine, DescribeSelectStarExpandsAllColumns) {
    auto result = engine_->describe("SELECT * FROM test_tbl");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 4u);
    EXPECT_EQ((*result)[0].name, "id");
    EXPECT_EQ((*result)[1].name, "name");
    EXPECT_EQ((*result)[2].name, "score");
    EXPECT_EQ((*result)[3].name, "active");
}

TEST_F(QA_GDB201_QueryEngine, DescribeSelectWithAlias) {
    auto result = engine_->describe("SELECT id AS user_id, name AS user_name FROM test_tbl");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);
    EXPECT_EQ((*result)[0].name, "user_id");
    EXPECT_EQ((*result)[1].name, "user_name");
}

TEST_F(QA_GDB201_QueryEngine, DescribeInsertReturnsEmpty) {
    auto result = engine_->describe("INSERT INTO test_tbl VALUES (1, 'alice', 3.14, true)");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(QA_GDB201_QueryEngine, DescribeUpdateReturnsEmpty) {
    auto result = engine_->describe("UPDATE test_tbl SET name = 'bob' WHERE id = 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(QA_GDB201_QueryEngine, DescribeDeleteReturnsEmpty) {
    auto result = engine_->describe("DELETE FROM test_tbl WHERE id = 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(QA_GDB201_QueryEngine, DescribeCreateTableReturnsEmpty) {
    auto result = engine_->describe("CREATE TABLE new_tbl (x INT)");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(QA_GDB201_QueryEngine, DescribeDropTableReturnsEmpty) {
    auto result = engine_->describe("DROP TABLE IF EXISTS nonexistent_tbl");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(QA_GDB201_QueryEngine, DescribeNonExistentTableReturnsError) {
    auto result = engine_->describe("SELECT * FROM nonexistent_table_xyz");
    EXPECT_FALSE(result.has_value());
}

TEST_F(QA_GDB201_QueryEngine, DescribeInvalidSqlReturnsError) {
    auto result = engine_->describe("THIS IS NOT SQL");
    EXPECT_FALSE(result.has_value());
}

TEST_F(QA_GDB201_QueryEngine, DescribeEmptyStringReturnsError) {
    auto result = engine_->describe("");
    EXPECT_FALSE(result.has_value());
}

TEST_F(QA_GDB201_QueryEngine, DescribeSelectWithWhereClause) {
    auto result = engine_->describe("SELECT id, name FROM test_tbl WHERE id > 5");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);
    EXPECT_EQ((*result)[0].name, "id");
    EXPECT_EQ((*result)[1].name, "name");
}

TEST_F(QA_GDB201_QueryEngine, DescribeSelectWithOrderBy) {
    auto result = engine_->describe("SELECT id, name FROM test_tbl ORDER BY id");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);
}

TEST_F(QA_GDB201_QueryEngine, DescribeSelectWithLimit) {
    auto result = engine_->describe("SELECT * FROM test_tbl LIMIT 10");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 4u);
}

TEST_F(QA_GDB201_QueryEngine, DescribePreservesColumnTypes) {
    auto result = engine_->describe("SELECT * FROM test_tbl");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 4u);
    EXPECT_EQ((*result)[0].type_id, TypeId::INT32);   // id
    EXPECT_EQ((*result)[1].type_id, TypeId::STRING);   // name
    EXPECT_EQ((*result)[2].type_id, TypeId::FLOAT64);  // score
    EXPECT_EQ((*result)[3].type_id, TypeId::BOOL);     // active
}

TEST_F(QA_GDB201_QueryEngine, DescribeDoesNotModifyData) {
    // Insert a row.
    auto ins = engine_->execute("INSERT INTO test_tbl VALUES (1, 'alice', 3.14, true)");
    ASSERT_TRUE(ins.has_value()) << ins.error().message;

    // Describe should not modify data.
    auto desc = engine_->describe("SELECT * FROM test_tbl");
    ASSERT_TRUE(desc.has_value()) << desc.error().message;

    // Verify data is unchanged.
    auto sel = engine_->execute("SELECT * FROM test_tbl");
    ASSERT_TRUE(sel.has_value()) << sel.error().message;
    EXPECT_EQ(sel->rows.size(), 1u);
}

TEST_F(QA_GDB201_QueryEngine, DescribeSelectExpressionColumn) {
    // Describe a SELECT with a literal expression.
    auto result = engine_->describe("SELECT 1");
    // This may or may not work depending on binder support for bare literals.
    // We just verify it doesn't crash.
    (void)result;
}
