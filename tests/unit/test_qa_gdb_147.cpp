/// @file test_qa_gdb_147.cpp
/// @brief QA adversarial tests for GDB-147: Extended query protocol (prepared statements).
///
/// Targets edge cases: unnamed/named statement lifecycle, Bind to nonexistent
/// statement, Execute nonexistent portal, Describe invalid target, Close
/// idempotency, error-in-batch skip-until-Sync, Flush no-op, NULL params,
/// overwrite semantics, multiple Syncs, etc.

#include "giodb/common/result.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/query_engine.h"
#include "giodb/server/connection.h"
#include "giodb/server/pg_protocol.h"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace giodb;

// =============================================================================
// Helpers (same patterns as test_pg_protocol.cpp)
// =============================================================================

namespace {

int create_socketpair147(int& client_fd_out) {
    int fds[2];
    int rc = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    EXPECT_EQ(rc, 0);
    client_fd_out = fds[1];
    return fds[0];
}

void write_to_fd147(int fd, const std::vector<uint8_t>& data) {
    size_t written = 0;
    while (written < data.size()) {
        auto n = ::write(fd, data.data() + written, data.size() - written);
        ASSERT_GT(n, 0);
        written += static_cast<size_t>(n);
    }
}

std::vector<uint8_t> read_from_fd147(int fd, size_t max_bytes = 16384) {
    std::vector<uint8_t> buf(max_bytes);
    auto n = ::read(fd, buf.data(), buf.size());
    if (n <= 0) return {};
    buf.resize(static_cast<size_t>(n));
    return buf;
}

bool find_message147(const std::vector<uint8_t>& data, size_t& pos, uint8_t type,
                     const uint8_t*& payload, size_t& payload_len) {
    while (pos + 5 <= data.size()) {
        uint8_t msg_type = data[pos];
        uint32_t length = (static_cast<uint32_t>(data[pos + 1]) << 24) |
                          (static_cast<uint32_t>(data[pos + 2]) << 16) |
                          (static_cast<uint32_t>(data[pos + 3]) << 8) |
                          static_cast<uint32_t>(data[pos + 4]);
        size_t total = 1 + static_cast<size_t>(length);
        if (pos + total > data.size()) return false;
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

bool has_message_type(const std::vector<uint8_t>& data, uint8_t type) {
    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    return find_message147(data, pos, type, payload, payload_len);
}

size_t count_message_type(const std::vector<uint8_t>& data, uint8_t type) {
    size_t pos = 0;
    size_t count = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    while (find_message147(data, pos, type, payload, payload_len)) {
        ++count;
    }
    return count;
}

std::vector<uint8_t> build_startup147(
    const std::vector<std::pair<std::string, std::string>>& params) {
    std::vector<uint8_t> payload;
    uint32_t version = 0x00030000;
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

std::vector<uint8_t> build_parse147(std::string_view stmt_name, std::string_view sql,
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

std::vector<uint8_t> build_bind147(std::string_view portal_name, std::string_view stmt_name,
                                   const std::vector<std::string>& param_values = {}) {
    std::vector<uint8_t> body;
    body.insert(body.end(), portal_name.begin(), portal_name.end());
    body.push_back(0);
    body.insert(body.end(), stmt_name.begin(), stmt_name.end());
    body.push_back(0);
    // 0 param format codes.
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
    // 0 result format codes.
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

/// Build a Bind message with explicit NULL parameters (length = -1).
std::vector<uint8_t> build_bind_with_nulls147(std::string_view portal_name,
                                              std::string_view stmt_name, size_t num_nulls) {
    std::vector<uint8_t> body;
    body.insert(body.end(), portal_name.begin(), portal_name.end());
    body.push_back(0);
    body.insert(body.end(), stmt_name.begin(), stmt_name.end());
    body.push_back(0);
    body.push_back(0);
    body.push_back(0);
    auto np = static_cast<uint16_t>(num_nulls);
    body.push_back(static_cast<uint8_t>((np >> 8) & 0xFF));
    body.push_back(static_cast<uint8_t>(np & 0xFF));
    for (size_t i = 0; i < num_nulls; ++i) {
        // -1 in big-endian = 0xFF 0xFF 0xFF 0xFF
        body.push_back(0xFF);
        body.push_back(0xFF);
        body.push_back(0xFF);
        body.push_back(0xFF);
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

std::vector<uint8_t> build_execute147(std::string_view portal_name, int32_t max_rows = 0) {
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

std::vector<uint8_t> build_sync147() { return {'S', 0, 0, 0, 4}; }

std::vector<uint8_t> build_close147(char type, std::string_view name) {
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

std::vector<uint8_t> build_describe147(char type, std::string_view name) {
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

std::vector<uint8_t> build_flush147() { return {'H', 0, 0, 0, 4}; }

void append(std::vector<uint8_t>& dest, const std::vector<uint8_t>& src) {
    dest.insert(dest.end(), src.begin(), src.end());
}

} // namespace

// =============================================================================
// Test fixture: sets up a connection + handler in READY state
// =============================================================================

class QA147ExtendedTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_fd_ = create_socketpair147(client_fd_);
        conn_ = std::make_unique<Connection>(server_fd_);
        handler_ = std::make_unique<PgProtocolHandler>(42);

        call_count_ = 0;
        handler_->set_query_executor([this](const std::string& sql) -> Result<QueryResult> {
            ++call_count_;
            last_sql_ = sql;
            QueryResult qr;
            qr.message = "OK";
            return ok(std::move(qr));
        });

        // Complete startup handshake.
        auto startup = build_startup147({{"user", "test"}, {"database", "testdb"}});
        write_to_fd147(client_fd_, startup);
        auto rr = conn_->read_from_socket();
        ASSERT_TRUE(rr.has_value());
        auto pr = handler_->process(*conn_);
        ASSERT_TRUE(pr.has_value());
        ASSERT_EQ(handler_->state(), ProtocolState::READY);

        // Drain startup response from write buffer.
        (void)conn_->write_to_socket();
        (void)read_from_fd147(client_fd_);
    }

    void TearDown() override {
        conn_->close();
        ::close(client_fd_);
    }

    /// Send a batch of messages and process them.
    std::vector<uint8_t> send_and_process(const std::vector<uint8_t>& batch) {
        write_to_fd147(client_fd_, batch);
        auto rr = conn_->read_from_socket();
        EXPECT_TRUE(rr.has_value());
        auto pr = handler_->process(*conn_);
        EXPECT_TRUE(pr.has_value());
        (void)conn_->write_to_socket();
        return read_from_fd147(client_fd_);
    }

    int client_fd_ = -1;
    int server_fd_ = -1;
    std::unique_ptr<Connection> conn_;
    std::unique_ptr<PgProtocolHandler> handler_;
    int call_count_ = 0;
    std::string last_sql_;
};

// =============================================================================
// Parse tests
// =============================================================================

TEST_F(QA147ExtendedTest, ParseEmptySql) {
    std::vector<uint8_t> batch;
    append(batch, build_parse147("", ""));
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    // ParseComplete ('1') should be sent even for empty SQL.
    EXPECT_TRUE(has_message_type(resp, '1'));
    // ReadyForQuery ('Z') from Sync.
    EXPECT_TRUE(has_message_type(resp, 'Z'));
}

TEST_F(QA147ExtendedTest, ParseWithManyParamOids) {
    std::vector<uint32_t> oids(100, 23); // 100 int4 params.
    std::vector<uint8_t> batch;
    append(batch, build_parse147("many_params", "SELECT 1", oids));
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    EXPECT_TRUE(has_message_type(resp, '1'));
    ASSERT_EQ(handler_->prepared_statements().count("many_params"), 1u);
    EXPECT_EQ(handler_->prepared_statements().at("many_params").param_oids.size(), 100u);
}

TEST_F(QA147ExtendedTest, ParseOverwritesExistingNamedStatement) {
    // Parse "s1" with SQL "SELECT 1".
    {
        std::vector<uint8_t> batch;
        append(batch, build_parse147("s1", "SELECT 1"));
        append(batch, build_sync147());
        send_and_process(batch);
    }
    ASSERT_EQ(handler_->prepared_statements().at("s1").sql, "SELECT 1");

    // Parse "s1" again with SQL "SELECT 2" — should overwrite.
    {
        std::vector<uint8_t> batch;
        append(batch, build_parse147("s1", "SELECT 2"));
        append(batch, build_sync147());
        send_and_process(batch);
    }
    EXPECT_EQ(handler_->prepared_statements().at("s1").sql, "SELECT 2");
}

TEST_F(QA147ExtendedTest, ParseUnnamedOverwritten) {
    {
        std::vector<uint8_t> batch;
        append(batch, build_parse147("", "SELECT 'first'"));
        append(batch, build_sync147());
        send_and_process(batch);
    }
    ASSERT_EQ(handler_->prepared_statements().at("").sql, "SELECT 'first'");

    {
        std::vector<uint8_t> batch;
        append(batch, build_parse147("", "SELECT 'second'"));
        append(batch, build_sync147());
        send_and_process(batch);
    }
    EXPECT_EQ(handler_->prepared_statements().at("").sql, "SELECT 'second'");
}

// =============================================================================
// Bind tests
// =============================================================================

TEST_F(QA147ExtendedTest, BindNonexistentStatement) {
    std::vector<uint8_t> batch;
    append(batch, build_bind147("", "nonexistent"));
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    // Should get ErrorResponse, NOT BindComplete.
    EXPECT_TRUE(has_message_type(resp, 'E'));
    EXPECT_FALSE(has_message_type(resp, '2'));
    // ReadyForQuery from Sync.
    EXPECT_TRUE(has_message_type(resp, 'Z'));
}

TEST_F(QA147ExtendedTest, BindWithNullParams) {
    // Parse a statement first.
    {
        std::vector<uint8_t> batch;
        append(batch, build_parse147("nulltest", "SELECT $1, $2"));
        append(batch, build_sync147());
        send_and_process(batch);
    }

    // Bind with 2 NULL params.
    std::vector<uint8_t> batch;
    append(batch, build_bind_with_nulls147("", "nulltest", 2));
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    EXPECT_TRUE(has_message_type(resp, '2'));
    // Portal should exist with 2 param values (empty strings for NULLs).
    ASSERT_EQ(handler_->portals().count(""), 1u);
    EXPECT_EQ(handler_->portals().at("").param_values.size(), 2u);
}

TEST_F(QA147ExtendedTest, BindOverwritesExistingPortal) {
    {
        std::vector<uint8_t> batch;
        append(batch, build_parse147("s1", "SELECT $1"));
        append(batch, build_sync147());
        send_and_process(batch);
    }

    // Bind portal "p1" with param "first".
    {
        std::vector<uint8_t> batch;
        append(batch, build_bind147("p1", "s1", {"first"}));
        append(batch, build_sync147());
        send_and_process(batch);
    }
    ASSERT_EQ(handler_->portals().at("p1").param_values[0], "first");

    // Bind portal "p1" again with param "second" — should overwrite.
    {
        std::vector<uint8_t> batch;
        append(batch, build_bind147("p1", "s1", {"second"}));
        append(batch, build_sync147());
        send_and_process(batch);
    }
    EXPECT_EQ(handler_->portals().at("p1").param_values[0], "second");
}

// =============================================================================
// Execute tests
// =============================================================================

TEST_F(QA147ExtendedTest, ExecuteNonexistentPortal) {
    std::vector<uint8_t> batch;
    append(batch, build_execute147("ghost_portal"));
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    EXPECT_TRUE(has_message_type(resp, 'E'));
    EXPECT_EQ(call_count_, 0); // Executor should NOT be called.
}

TEST_F(QA147ExtendedTest, ExecuteWithoutQueryExecutor) {
    // Remove the query executor.
    handler_->set_query_executor(nullptr);

    // Parse + Bind + Execute.
    std::vector<uint8_t> batch;
    append(batch, build_parse147("", "SELECT 1"));
    append(batch, build_bind147("", ""));
    append(batch, build_execute147(""));
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    // Parse and Bind succeed ('1', '2'), Execute gets ErrorResponse.
    EXPECT_TRUE(has_message_type(resp, '1'));
    EXPECT_TRUE(has_message_type(resp, '2'));
    EXPECT_TRUE(has_message_type(resp, 'E'));
}

TEST_F(QA147ExtendedTest, ExecuteCalledWithPortalSql) {
    std::vector<uint8_t> batch;
    append(batch, build_parse147("", "SELECT 42"));
    append(batch, build_bind147("", ""));
    append(batch, build_execute147(""));
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    EXPECT_EQ(call_count_, 1);
    EXPECT_EQ(last_sql_, "SELECT 42");
}

TEST_F(QA147ExtendedTest, NamedStatementReusedMultipleTimes) {
    // Parse once.
    {
        std::vector<uint8_t> batch;
        append(batch, build_parse147("reuse", "SELECT 1"));
        append(batch, build_sync147());
        send_and_process(batch);
    }

    // Bind + Execute 5 times.
    for (int i = 0; i < 5; ++i) {
        std::vector<uint8_t> batch;
        append(batch, build_bind147("", "reuse"));
        append(batch, build_execute147(""));
        append(batch, build_sync147());
        auto resp = send_and_process(batch);
        EXPECT_TRUE(has_message_type(resp, '2')) << "iteration " << i;
    }

    EXPECT_EQ(call_count_, 5);
    // Statement still exists.
    EXPECT_EQ(handler_->prepared_statements().count("reuse"), 1u);
}

// =============================================================================
// Describe tests
// =============================================================================

TEST_F(QA147ExtendedTest, DescribeNonexistentStatement) {
    std::vector<uint8_t> batch;
    append(batch, build_describe147('S', "nope"));
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    EXPECT_TRUE(has_message_type(resp, 'E'));
}

TEST_F(QA147ExtendedTest, DescribeNonexistentPortal) {
    std::vector<uint8_t> batch;
    append(batch, build_describe147('P', "nope"));
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    EXPECT_TRUE(has_message_type(resp, 'E'));
}

TEST_F(QA147ExtendedTest, DescribeInvalidType) {
    std::vector<uint8_t> batch;
    append(batch, build_describe147('X', "anything"));
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    EXPECT_TRUE(has_message_type(resp, 'E'));
}

TEST_F(QA147ExtendedTest, DescribeStatementSendsParameterDescription) {
    // Parse with 2 param OIDs.
    {
        std::vector<uint8_t> batch;
        append(batch, build_parse147("desc_s", "SELECT $1, $2", {23, 25}));
        append(batch, build_sync147());
        send_and_process(batch);
    }

    // Describe the statement.
    std::vector<uint8_t> batch;
    append(batch, build_describe147('S', "desc_s"));
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    // Should get ParameterDescription ('t') and NoData ('n') (since RowDescription is TODO).
    EXPECT_TRUE(has_message_type(resp, 't'));
    EXPECT_TRUE(has_message_type(resp, 'n'));
}

TEST_F(QA147ExtendedTest, DescribePortalSendsNoData) {
    {
        std::vector<uint8_t> batch;
        append(batch, build_parse147("", "SELECT 1"));
        append(batch, build_bind147("myp", ""));
        append(batch, build_sync147());
        send_and_process(batch);
    }

    std::vector<uint8_t> batch;
    append(batch, build_describe147('P', "myp"));
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    // Should get NoData ('n') since RowDescription is TODO.
    EXPECT_TRUE(has_message_type(resp, 'n'));
}

// =============================================================================
// Close tests
// =============================================================================

TEST_F(QA147ExtendedTest, CloseNonexistentStatementIsSafe) {
    std::vector<uint8_t> batch;
    append(batch, build_close147('S', "doesntexist"));
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    // Should get CloseComplete ('3') even for nonexistent — per PG spec.
    EXPECT_TRUE(has_message_type(resp, '3'));
}

TEST_F(QA147ExtendedTest, CloseNonexistentPortalIsSafe) {
    std::vector<uint8_t> batch;
    append(batch, build_close147('P', "doesntexist"));
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    EXPECT_TRUE(has_message_type(resp, '3'));
}

TEST_F(QA147ExtendedTest, CloseStatementRemovesIt) {
    {
        std::vector<uint8_t> batch;
        append(batch, build_parse147("todel", "SELECT 1"));
        append(batch, build_sync147());
        send_and_process(batch);
    }
    ASSERT_EQ(handler_->prepared_statements().count("todel"), 1u);

    {
        std::vector<uint8_t> batch;
        append(batch, build_close147('S', "todel"));
        append(batch, build_sync147());
        auto resp = send_and_process(batch);
        EXPECT_TRUE(has_message_type(resp, '3'));
    }
    EXPECT_EQ(handler_->prepared_statements().count("todel"), 0u);
}

TEST_F(QA147ExtendedTest, ClosePortalRemovesIt) {
    {
        std::vector<uint8_t> batch;
        append(batch, build_parse147("", "SELECT 1"));
        append(batch, build_bind147("portdel", ""));
        append(batch, build_sync147());
        send_and_process(batch);
    }
    ASSERT_EQ(handler_->portals().count("portdel"), 1u);

    {
        std::vector<uint8_t> batch;
        append(batch, build_close147('P', "portdel"));
        append(batch, build_sync147());
        auto resp = send_and_process(batch);
        EXPECT_TRUE(has_message_type(resp, '3'));
    }
    EXPECT_EQ(handler_->portals().count("portdel"), 0u);
}

// =============================================================================
// Sync / error-in-batch tests
// =============================================================================

TEST_F(QA147ExtendedTest, SyncAloneReturnsReadyForQuery) {
    std::vector<uint8_t> batch;
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    EXPECT_TRUE(has_message_type(resp, 'Z'));
}

TEST_F(QA147ExtendedTest, MultipleSyncsReturnMultipleReadyForQuery) {
    std::vector<uint8_t> batch;
    append(batch, build_sync147());
    append(batch, build_sync147());
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    EXPECT_EQ(count_message_type(resp, 'Z'), 3u);
}

TEST_F(QA147ExtendedTest, ErrorInBatchSkipsExecute) {
    // Bind to nonexistent statement → error → Execute should be skipped.
    std::vector<uint8_t> batch;
    append(batch, build_bind147("", "nonexistent"));
    append(batch, build_execute147(""));
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    EXPECT_TRUE(has_message_type(resp, 'E'));
    EXPECT_TRUE(has_message_type(resp, 'Z'));
    EXPECT_EQ(call_count_, 0); // Execute was skipped.
}

TEST_F(QA147ExtendedTest, ErrorResetAfterSync) {
    // First batch: error.
    {
        std::vector<uint8_t> batch;
        append(batch, build_bind147("", "nonexistent"));
        append(batch, build_sync147());
        send_and_process(batch);
    }

    // Second batch: should work normally (error state reset by Sync).
    {
        std::vector<uint8_t> batch;
        append(batch, build_parse147("", "SELECT 1"));
        append(batch, build_bind147("", ""));
        append(batch, build_execute147(""));
        append(batch, build_sync147());
        auto resp = send_and_process(batch);

        EXPECT_TRUE(has_message_type(resp, '1'));
        EXPECT_TRUE(has_message_type(resp, '2'));
        EXPECT_EQ(call_count_, 1);
    }
}

TEST_F(QA147ExtendedTest, ErrorSkipsAllRemainingUntilSync) {
    // Bind(fail) + Parse(skip) + Bind(skip) + Execute(skip) + Sync.
    std::vector<uint8_t> batch;
    append(batch, build_bind147("", "nonexistent"));  // fails
    append(batch, build_parse147("late", "SELECT 1")); // skipped
    append(batch, build_bind147("", "late"));           // skipped
    append(batch, build_execute147(""));                // skipped
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    EXPECT_TRUE(has_message_type(resp, 'E'));
    EXPECT_TRUE(has_message_type(resp, 'Z'));
    // Parse was skipped, so "late" should NOT be in prepared_statements.
    EXPECT_EQ(handler_->prepared_statements().count("late"), 0u);
    EXPECT_EQ(call_count_, 0);
}

// =============================================================================
// Flush message
// =============================================================================

TEST_F(QA147ExtendedTest, FlushIsNoOp) {
    std::vector<uint8_t> batch;
    append(batch, build_flush147());
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    // Only ReadyForQuery from Sync.
    EXPECT_TRUE(has_message_type(resp, 'Z'));
}

// =============================================================================
// Full Parse+Bind+Describe+Execute+Sync flow
// =============================================================================

TEST_F(QA147ExtendedTest, FullExtendedQueryFlow) {
    std::vector<uint8_t> batch;
    append(batch, build_parse147("", "SELECT 1", {23}));
    append(batch, build_bind147("", "", {"42"}));
    append(batch, build_describe147('P', ""));
    append(batch, build_execute147(""));
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    EXPECT_TRUE(has_message_type(resp, '1')); // ParseComplete
    EXPECT_TRUE(has_message_type(resp, '2')); // BindComplete
    EXPECT_TRUE(has_message_type(resp, 'n')); // NoData (from Describe)
    EXPECT_TRUE(has_message_type(resp, 'Z')); // ReadyForQuery
    EXPECT_EQ(call_count_, 1);
}

// =============================================================================
// Edge: Close statement that has a bound portal
// =============================================================================

TEST_F(QA147ExtendedTest, CloseStatementWhilePortalExists) {
    // Parse + Bind.
    {
        std::vector<uint8_t> batch;
        append(batch, build_parse147("stmt_x", "SELECT 99"));
        append(batch, build_bind147("port_x", "stmt_x"));
        append(batch, build_sync147());
        send_and_process(batch);
    }
    ASSERT_EQ(handler_->prepared_statements().count("stmt_x"), 1u);
    ASSERT_EQ(handler_->portals().count("port_x"), 1u);

    // Close the statement (portal still exists with a copy of the SQL).
    {
        std::vector<uint8_t> batch;
        append(batch, build_close147('S', "stmt_x"));
        append(batch, build_sync147());
        send_and_process(batch);
    }
    EXPECT_EQ(handler_->prepared_statements().count("stmt_x"), 0u);
    // Portal should still exist.
    EXPECT_EQ(handler_->portals().count("port_x"), 1u);

    // Execute the portal — should still work since portal has its own SQL copy.
    {
        std::vector<uint8_t> batch;
        append(batch, build_execute147("port_x"));
        append(batch, build_sync147());
        auto resp = send_and_process(batch);

        EXPECT_EQ(call_count_, 1);
        EXPECT_EQ(last_sql_, "SELECT 99");
    }
}

// =============================================================================
// Edge: Multiple named statements + portals coexist
// =============================================================================

TEST_F(QA147ExtendedTest, MultipleNamedStatementsCoexist) {
    // Parse 3 different named statements.
    for (int i = 0; i < 3; ++i) {
        std::string name = "stmt" + std::to_string(i);
        std::string sql = "SELECT " + std::to_string(i);
        std::vector<uint8_t> batch;
        append(batch, build_parse147(name, sql));
        append(batch, build_sync147());
        send_and_process(batch);
    }

    EXPECT_EQ(handler_->prepared_statements().size(), 3u);
    EXPECT_EQ(handler_->prepared_statements().at("stmt0").sql, "SELECT 0");
    EXPECT_EQ(handler_->prepared_statements().at("stmt1").sql, "SELECT 1");
    EXPECT_EQ(handler_->prepared_statements().at("stmt2").sql, "SELECT 2");
}

// =============================================================================
// Edge: Execute with query executor that returns error
// =============================================================================

TEST_F(QA147ExtendedTest, ExecuteWithFailingQueryExecutor) {
    handler_->set_query_executor([](const std::string& /*sql*/) -> Result<QueryResult> {
        return Result<QueryResult>(make_error(StatusCode::PARSE_ERROR, "syntax error near X"));
    });

    std::vector<uint8_t> batch;
    append(batch, build_parse147("", "BAD SQL"));
    append(batch, build_bind147("", ""));
    append(batch, build_execute147(""));
    append(batch, build_sync147());
    auto resp = send_and_process(batch);

    // Parse + Bind succeed, Execute fails.
    EXPECT_TRUE(has_message_type(resp, '1'));
    EXPECT_TRUE(has_message_type(resp, '2'));
    EXPECT_TRUE(has_message_type(resp, 'E'));
    EXPECT_TRUE(has_message_type(resp, 'Z'));
}

// =============================================================================
// Edge: Unknown frontend message type
// =============================================================================

TEST_F(QA147ExtendedTest, UnknownMessageType) {
    // Send a message with type byte 0xFF.
    std::vector<uint8_t> msg = {0xFF, 0, 0, 0, 4};
    auto resp = send_and_process(msg);

    // Should get ErrorResponse + ReadyForQuery.
    EXPECT_TRUE(has_message_type(resp, 'E'));
    EXPECT_TRUE(has_message_type(resp, 'Z'));
    // Handler should still be in READY state.
    EXPECT_EQ(handler_->state(), ProtocolState::READY);
}
