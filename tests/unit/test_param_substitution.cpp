#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/server/connection.h"
#include "sixseven/server/pg_protocol.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace sixseven;

// =============================================================================
// substitute_parameters() unit tests
// =============================================================================

TEST(ParamSubstitution, NoParameters) {
    auto result = substitute_parameters("SELECT 1", {}, {});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT 1");
}

TEST(ParamSubstitution, SingleIntParam) {
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {23}; // INT4
    auto result = substitute_parameters("SELECT * FROM t WHERE id = $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT * FROM t WHERE id = 42");
}

TEST(ParamSubstitution, MultipleParams) {
    std::vector<std::optional<std::string>> params = {"42", "hello"};
    std::vector<uint32_t> oids = {23, 25}; // INT4, TEXT
    auto result =
        substitute_parameters("SELECT * FROM t WHERE id = $1 AND name = $2", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT * FROM t WHERE id = 42 AND name = 'hello'");
}

TEST(ParamSubstitution, NullParam) {
    std::vector<std::optional<std::string>> params = {std::nullopt};
    std::vector<uint32_t> oids = {23}; // INT4
    auto result = substitute_parameters("SELECT * FROM t WHERE id = $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT * FROM t WHERE id = NULL");
}

TEST(ParamSubstitution, MixedNullAndValue) {
    std::vector<std::optional<std::string>> params = {"42", std::nullopt, "world"};
    std::vector<uint32_t> oids = {23, 25, 25}; // INT4, TEXT, TEXT
    auto result =
        substitute_parameters("INSERT INTO t (a, b, c) VALUES ($1, $2, $3)", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "INSERT INTO t (a, b, c) VALUES (42, NULL, 'world')");
}

TEST(ParamSubstitution, BoolParamTrue) {
    std::vector<std::optional<std::string>> params = {"t"};
    std::vector<uint32_t> oids = {16}; // BOOL
    auto result = substitute_parameters("SELECT * FROM t WHERE active = $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT * FROM t WHERE active = TRUE");
}

TEST(ParamSubstitution, BoolParamFalse) {
    std::vector<std::optional<std::string>> params = {"f"};
    std::vector<uint32_t> oids = {16}; // BOOL
    auto result = substitute_parameters("SELECT * FROM t WHERE active = $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT * FROM t WHERE active = FALSE");
}

TEST(ParamSubstitution, BoolParamInvalid) {
    std::vector<std::optional<std::string>> params = {"maybe"};
    std::vector<uint32_t> oids = {16}; // BOOL
    auto result = substitute_parameters("SELECT * FROM t WHERE active = $1", params, oids);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(ParamSubstitution, FloatParam) {
    std::vector<std::optional<std::string>> params = {"3.14"};
    std::vector<uint32_t> oids = {701}; // FLOAT8
    auto result = substitute_parameters("SELECT * FROM t WHERE x = $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT * FROM t WHERE x = 3.14");
}

TEST(ParamSubstitution, NegativeNumericParam) {
    std::vector<std::optional<std::string>> params = {"-99"};
    std::vector<uint32_t> oids = {23}; // INT4
    auto result = substitute_parameters("SELECT * FROM t WHERE id = $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT * FROM t WHERE id = -99");
}

TEST(ParamSubstitution, ScientificNotation) {
    std::vector<std::optional<std::string>> params = {"1.5e10"};
    std::vector<uint32_t> oids = {701}; // FLOAT8
    auto result = substitute_parameters("SELECT * FROM t WHERE x = $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT * FROM t WHERE x = 1.5e10");
}

TEST(ParamSubstitution, InvalidNumericParam) {
    std::vector<std::optional<std::string>> params = {"abc"};
    std::vector<uint32_t> oids = {23}; // INT4
    auto result = substitute_parameters("SELECT * FROM t WHERE id = $1", params, oids);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(ParamSubstitution, SqlInjectionPrevention) {
    std::vector<std::optional<std::string>> params = {"'; DROP TABLE users; --"};
    std::vector<uint32_t> oids = {25}; // TEXT
    auto result = substitute_parameters("SELECT * FROM t WHERE name = $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Single quotes should be doubled, making injection impossible.
    EXPECT_EQ(*result, "SELECT * FROM t WHERE name = '''; DROP TABLE users; --'");
}

TEST(ParamSubstitution, SqlInjectionNumericType) {
    // Even with numeric OID, non-numeric values are rejected.
    std::vector<std::optional<std::string>> params = {"1; DROP TABLE users"};
    std::vector<uint32_t> oids = {23}; // INT4
    auto result = substitute_parameters("SELECT * FROM t WHERE id = $1", params, oids);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(ParamSubstitution, StringWithSingleQuotes) {
    std::vector<std::optional<std::string>> params = {"it's a test"};
    std::vector<uint32_t> oids = {25}; // TEXT
    auto result = substitute_parameters("SELECT * FROM t WHERE name = $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT * FROM t WHERE name = 'it''s a test'");
}

TEST(ParamSubstitution, EmptyStringParam) {
    std::vector<std::optional<std::string>> params = {""};
    std::vector<uint32_t> oids = {25}; // TEXT
    auto result = substitute_parameters("SELECT * FROM t WHERE name = $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT * FROM t WHERE name = ''");
}

TEST(ParamSubstitution, DateParam) {
    std::vector<std::optional<std::string>> params = {"2024-01-15"};
    std::vector<uint32_t> oids = {1082}; // DATE
    auto result = substitute_parameters("SELECT * FROM t WHERE d = $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT * FROM t WHERE d = '2024-01-15'");
}

TEST(ParamSubstitution, TimestampParam) {
    std::vector<std::optional<std::string>> params = {"2024-01-15 10:30:00"};
    std::vector<uint32_t> oids = {1114}; // TIMESTAMP
    auto result = substitute_parameters("SELECT * FROM t WHERE ts = $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT * FROM t WHERE ts = '2024-01-15 10:30:00'");
}

TEST(ParamSubstitution, UnspecifiedOidTreatedAsText) {
    // OID = 0 means unspecified — treat as text (quoted).
    std::vector<std::optional<std::string>> params = {"hello"};
    std::vector<uint32_t> oids = {0};
    auto result = substitute_parameters("SELECT * FROM t WHERE name = $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT * FROM t WHERE name = 'hello'");
}

TEST(ParamSubstitution, MissingOidTreatedAsText) {
    // Fewer OIDs than params — missing OIDs default to 0 (text).
    std::vector<std::optional<std::string>> params = {"hello"};
    std::vector<uint32_t> oids = {};
    auto result = substitute_parameters("SELECT * FROM t WHERE name = $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT * FROM t WHERE name = 'hello'");
}

TEST(ParamSubstitution, ParamOutOfRange) {
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {23};
    auto result = substitute_parameters("SELECT $1, $2", params, oids);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(ParamSubstitution, ParamZeroOutOfRange) {
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {23};
    // $0 is not valid — parameters are 1-indexed.
    auto result = substitute_parameters("SELECT $0", params, oids);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(ParamSubstitution, VeryLargeParamNumberDoesNotCrash) {
    // $99999999999 exceeds INT_MAX — must return an error, not throw.
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {23};
    auto result = substitute_parameters("SELECT $99999999999", params, oids);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(ParamSubstitution, ParamNumberJustOverIntMaxDoesNotCrash) {
    // $2147483648 is INT_MAX + 1 — must return an error, not throw.
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {23};
    auto result = substitute_parameters("SELECT $2147483648", params, oids);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(ParamSubstitution, DollarInsideQuotedStringNotSubstituted) {
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {23};
    auto result = substitute_parameters("SELECT '$1' AS literal, $1 AS val", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT '$1' AS literal, 42 AS val");
}

TEST(ParamSubstitution, DollarInsideDoubleQuotedIdNotSubstituted) {
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {23};
    auto result = substitute_parameters("SELECT \"$1\" AS col, $1 AS val", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT \"$1\" AS col, 42 AS val");
}

TEST(ParamSubstitution, RepeatedParam) {
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {23};
    auto result = substitute_parameters("SELECT $1, $1 + 1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT 42, 42 + 1");
}

TEST(ParamSubstitution, MultiDigitParam) {
    // Test with more than 9 parameters to verify multi-digit parsing ($10).
    std::vector<std::optional<std::string>> params;
    std::vector<uint32_t> oids;
    for (int i = 1; i <= 10; ++i) {
        params.emplace_back(std::to_string(i * 10));
        oids.push_back(23); // INT4
    }
    auto result = substitute_parameters("SELECT $10", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT 100");
}

// =============================================================================
// SELECT parameterized queries
// =============================================================================

TEST(ParamSubstitution, SelectWithWhereInt) {
    std::vector<std::optional<std::string>> params = {"5"};
    std::vector<uint32_t> oids = {23};
    auto result = substitute_parameters("SELECT name FROM users WHERE id = $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT name FROM users WHERE id = 5");
}

TEST(ParamSubstitution, SelectWithMultipleConditions) {
    std::vector<std::optional<std::string>> params = {"active", "25"};
    std::vector<uint32_t> oids = {25, 23}; // TEXT, INT4
    auto result =
        substitute_parameters("SELECT * FROM users WHERE status = $1 AND age > $2", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT * FROM users WHERE status = 'active' AND age > 25");
}

// =============================================================================
// INSERT parameterized queries
// =============================================================================

TEST(ParamSubstitution, InsertWithParams) {
    std::vector<std::optional<std::string>> params = {"alice", "30"};
    std::vector<uint32_t> oids = {25, 23}; // TEXT, INT4
    auto result =
        substitute_parameters("INSERT INTO users (name, age) VALUES ($1, $2)", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "INSERT INTO users (name, age) VALUES ('alice', 30)");
}

TEST(ParamSubstitution, InsertWithNull) {
    std::vector<std::optional<std::string>> params = {"bob", std::nullopt};
    std::vector<uint32_t> oids = {25, 23}; // TEXT, INT4
    auto result =
        substitute_parameters("INSERT INTO users (name, age) VALUES ($1, $2)", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "INSERT INTO users (name, age) VALUES ('bob', NULL)");
}

// =============================================================================
// UPDATE parameterized queries
// =============================================================================

TEST(ParamSubstitution, UpdateWithParams) {
    std::vector<std::optional<std::string>> params = {"new_name", "1"};
    std::vector<uint32_t> oids = {25, 23}; // TEXT, INT4
    auto result = substitute_parameters("UPDATE users SET name = $1 WHERE id = $2", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "UPDATE users SET name = 'new_name' WHERE id = 1");
}

// =============================================================================
// DELETE parameterized queries
// =============================================================================

TEST(ParamSubstitution, DeleteWithParam) {
    std::vector<std::optional<std::string>> params = {"5"};
    std::vector<uint32_t> oids = {23}; // INT4
    auto result = substitute_parameters("DELETE FROM users WHERE id = $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "DELETE FROM users WHERE id = 5");
}

TEST(ParamSubstitution, DeleteWithTextParam) {
    std::vector<std::optional<std::string>> params = {"inactive"};
    std::vector<uint32_t> oids = {25}; // TEXT
    auto result = substitute_parameters("DELETE FROM users WHERE status = $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "DELETE FROM users WHERE status = 'inactive'");
}

// =============================================================================
// Wire protocol integration tests (Parse/Bind/Execute with parameters)
// =============================================================================

namespace {

int create_socketpair_for_test(int& client_fd) {
    int fds[2];
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    client_fd = fds[0];
    return fds[1];
}

void write_to_fd_for_test(int fd, const std::vector<uint8_t>& data) {
    size_t written = 0;
    while (written < data.size()) {
        auto n = ::write(fd, data.data() + written, data.size() - written);
        ASSERT_GT(n, 0);
        written += static_cast<size_t>(n);
    }
}

std::vector<uint8_t> read_from_fd_for_test(int fd) {
    std::vector<uint8_t> buf(8192);
    auto n = ::read(fd, buf.data(), buf.size());
    if (n <= 0) {
        return {};
    }
    buf.resize(static_cast<size_t>(n));
    return buf;
}

std::vector<uint8_t>
build_startup_msg(const std::vector<std::pair<std::string, std::string>>& params) {
    std::vector<uint8_t> body;
    // Protocol version 3.0.
    int32_t version = 196608;
    body.push_back(static_cast<uint8_t>((version >> 24) & 0xFF));
    body.push_back(static_cast<uint8_t>((version >> 16) & 0xFF));
    body.push_back(static_cast<uint8_t>((version >> 8) & 0xFF));
    body.push_back(static_cast<uint8_t>(version & 0xFF));
    for (const auto& [key, val] : params) {
        body.insert(body.end(), key.begin(), key.end());
        body.push_back(0);
        body.insert(body.end(), val.begin(), val.end());
        body.push_back(0);
    }
    body.push_back(0);
    int32_t total_len = static_cast<int32_t>(4 + body.size());
    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>((total_len >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((total_len >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((total_len >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(total_len & 0xFF));
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

std::vector<uint8_t> build_parse_msg(std::string_view stmt_name,
                                     std::string_view sql,
                                     const std::vector<uint32_t>& param_oids = {}) {
    std::vector<uint8_t> body;
    body.insert(body.end(), stmt_name.begin(), stmt_name.end());
    body.push_back(0);
    body.insert(body.end(), sql.begin(), sql.end());
    body.push_back(0);
    auto num = static_cast<uint16_t>(param_oids.size());
    body.push_back(static_cast<uint8_t>((num >> 8) & 0xFF));
    body.push_back(static_cast<uint8_t>(num & 0xFF));
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

/// Build a Bind message with optional NULL support.
/// Use std::nullopt in param_values to send a NULL parameter (length = -1).
std::vector<uint8_t>
build_bind_msg(std::string_view portal_name,
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
    auto num = static_cast<uint16_t>(param_values.size());
    body.push_back(static_cast<uint8_t>((num >> 8) & 0xFF));
    body.push_back(static_cast<uint8_t>(num & 0xFF));
    for (const auto& val : param_values) {
        if (!val.has_value()) {
            // NULL: length = -1.
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

std::vector<uint8_t> build_execute_msg(std::string_view portal_name, int32_t max_rows = 0) {
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

std::vector<uint8_t> build_sync_msg() {
    return {'S', 0, 0, 0, 4};
}

/// Build a simple Query message.
std::vector<uint8_t> build_query_msg(std::string_view sql) {
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

void do_startup_for_test(int client_fd, Connection& conn, PgProtocolHandler& handler) {
    auto startup = build_startup_msg({{"user", "test"}});
    write_to_fd_for_test(client_fd, startup);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)read_from_fd_for_test(client_fd);
}

} // namespace

TEST(ParamSubstitutionWire, ParameterSubstitutionPassesSqlToExecutor) {
    int client_fd = -1;
    int server_fd = create_socketpair_for_test(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(100);

    std::string received_sql;
    handler.set_query_executor([&](const std::string& sql) -> Result<QueryResult> {
        received_sql = sql;
        QueryResult qr;
        qr.column_names = {"id"};
        qr.column_types = {TypeId::INT32};
        qr.rows = {{Value(static_cast<int32_t>(42))}};
        return ok(std::move(qr));
    });

    do_startup_for_test(client_fd, conn, handler);

    // Parse with INT4 OID, Bind with "42", Execute.
    std::vector<uint8_t> batch;
    auto parse = build_parse_msg("", "SELECT * FROM t WHERE id = $1", {23});
    auto bind = build_bind_msg("", "", {std::optional<std::string>("42")});
    auto execute = build_execute_msg("");
    auto sync = build_sync_msg();

    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), execute.begin(), execute.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd_for_test(client_fd, batch);
    (void)conn.read_from_socket();
    auto result = handler.process(conn);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Verify the SQL passed to the executor has $1 substituted with 42.
    EXPECT_EQ(received_sql, "SELECT * FROM t WHERE id = 42");

    conn.close();
    ::close(client_fd);
}

TEST(ParamSubstitutionWire, NullParameterSubstitution) {
    int client_fd = -1;
    int server_fd = create_socketpair_for_test(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(101);

    std::string received_sql;
    handler.set_query_executor([&](const std::string& sql) -> Result<QueryResult> {
        received_sql = sql;
        QueryResult qr;
        qr.affected_rows = 1;
        qr.message = "UPDATE";
        return ok(std::move(qr));
    });

    do_startup_for_test(client_fd, conn, handler);

    std::vector<uint8_t> batch;
    auto parse = build_parse_msg("", "UPDATE t SET name = $1 WHERE id = $2", {25, 23});
    auto bind = build_bind_msg("", "", {std::nullopt, std::optional<std::string>("5")});
    auto execute = build_execute_msg("");
    auto sync = build_sync_msg();

    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), execute.begin(), execute.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd_for_test(client_fd, batch);
    (void)conn.read_from_socket();
    auto result = handler.process(conn);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(received_sql, "UPDATE t SET name = NULL WHERE id = 5");

    conn.close();
    ::close(client_fd);
}

TEST(ParamSubstitutionWire, StringParameterWithEscaping) {
    int client_fd = -1;
    int server_fd = create_socketpair_for_test(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(102);

    std::string received_sql;
    handler.set_query_executor([&](const std::string& sql) -> Result<QueryResult> {
        received_sql = sql;
        QueryResult qr;
        qr.affected_rows = 1;
        qr.message = "INSERT";
        return ok(std::move(qr));
    });

    do_startup_for_test(client_fd, conn, handler);

    std::vector<uint8_t> batch;
    auto parse = build_parse_msg("", "INSERT INTO t (name) VALUES ($1)", {25});
    auto bind = build_bind_msg("", "", {std::optional<std::string>("it's a test")});
    auto execute = build_execute_msg("");
    auto sync = build_sync_msg();

    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), execute.begin(), execute.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd_for_test(client_fd, batch);
    (void)conn.read_from_socket();
    auto result = handler.process(conn);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(received_sql, "INSERT INTO t (name) VALUES ('it''s a test')");

    conn.close();
    ::close(client_fd);
}

TEST(ParamSubstitutionWire, DeleteWithParameter) {
    int client_fd = -1;
    int server_fd = create_socketpair_for_test(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(103);

    std::string received_sql;
    handler.set_query_executor([&](const std::string& sql) -> Result<QueryResult> {
        received_sql = sql;
        QueryResult qr;
        qr.affected_rows = 3;
        qr.message = "DELETE";
        return ok(std::move(qr));
    });

    do_startup_for_test(client_fd, conn, handler);

    std::vector<uint8_t> batch;
    auto parse = build_parse_msg("", "DELETE FROM t WHERE status = $1", {25});
    auto bind = build_bind_msg("", "", {std::optional<std::string>("inactive")});
    auto execute = build_execute_msg("");
    auto sync = build_sync_msg();

    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), execute.begin(), execute.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd_for_test(client_fd, batch);
    (void)conn.read_from_socket();
    auto result = handler.process(conn);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(received_sql, "DELETE FROM t WHERE status = 'inactive'");

    conn.close();
    ::close(client_fd);
}

TEST(ParamSubstitutionWire, NoParamsPassthroughUnchanged) {
    int client_fd = -1;
    int server_fd = create_socketpair_for_test(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(104);

    std::string received_sql;
    handler.set_query_executor([&](const std::string& sql) -> Result<QueryResult> {
        received_sql = sql;
        QueryResult qr;
        qr.column_names = {"x"};
        qr.column_types = {TypeId::INT32};
        qr.rows = {{Value(static_cast<int32_t>(1))}};
        return ok(std::move(qr));
    });

    do_startup_for_test(client_fd, conn, handler);

    std::vector<uint8_t> batch;
    auto parse = build_parse_msg("", "SELECT 1", {});
    auto bind = build_bind_msg("", "", {});
    auto execute = build_execute_msg("");
    auto sync = build_sync_msg();

    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), execute.begin(), execute.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd_for_test(client_fd, batch);
    (void)conn.read_from_socket();
    auto result = handler.process(conn);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(received_sql, "SELECT 1");

    conn.close();
    ::close(client_fd);
}

// =============================================================================
// SQL-level EXECUTE with parameters
// =============================================================================

TEST(ParamSubstitutionWire, SqlLevelExecuteWithParams) {
    int client_fd = -1;
    int server_fd = create_socketpair_for_test(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(105);

    std::string received_sql;
    int call_count = 0;
    handler.set_query_executor([&](const std::string& sql) -> Result<QueryResult> {
        ++call_count;
        received_sql = sql;
        QueryResult qr;
        qr.column_names = {"id"};
        qr.column_types = {TypeId::INT32};
        qr.rows = {{Value(static_cast<int32_t>(42))}};
        return ok(std::move(qr));
    });

    do_startup_for_test(client_fd, conn, handler);

    // First, PREPARE the statement via simple query.
    auto prepare_msg = build_query_msg("PREPARE myquery (int) AS SELECT * FROM t WHERE id = $1");
    write_to_fd_for_test(client_fd, prepare_msg);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)read_from_fd_for_test(client_fd);

    // Now EXECUTE it with a parameter.
    auto execute_msg = build_query_msg("EXECUTE myquery (42)");
    write_to_fd_for_test(client_fd, execute_msg);
    (void)conn.read_from_socket();
    auto result = handler.process(conn);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // The executor should have received the SQL with $1 replaced by 42.
    EXPECT_EQ(received_sql, "SELECT * FROM t WHERE id = 42");

    conn.close();
    ::close(client_fd);
}
