#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/server/connection.h"
#include "sixseven/server/pg_protocol.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pg_wire_test_helpers.h"

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

void do_startup_for_test(int client_fd, Connection& conn, PgProtocolHandler& handler) {
    auto startup = pg_wire_test::build_startup_message({{"user", "test"}});
    pg_wire_test::write_to_fd(client_fd, startup);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)pg_wire_test::read_from_fd(client_fd);
}

} // namespace

TEST(ParamSubstitutionWire, ParameterSubstitutionPassesSqlToExecutor) {
    int client_fd = -1;
    int server_fd = pg_wire_test::create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(100);

    std::string received_sql;
    handler.set_query_executor(
        [&](const std::string& sql, const std::string& /*database*/) -> Result<QueryResult> {
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
    auto parse = pg_wire_test::build_parse_message("", "SELECT * FROM t WHERE id = $1", {23});
    auto bind = pg_wire_test::build_bind_message("", "", {std::optional<std::string>("42")});
    auto execute = pg_wire_test::build_execute_message("");
    auto sync = pg_wire_test::build_sync_message();

    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), execute.begin(), execute.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    pg_wire_test::write_to_fd(client_fd, batch);
    (void)conn.read_from_socket();
    auto result = handler.process(conn);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Verify the SQL passed to the executor has $1 substituted with 42.
    EXPECT_EQ(received_sql, "SELECT * FROM t WHERE id = 42");

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(ParamSubstitutionWire, NullParameterSubstitution) {
    int client_fd = -1;
    int server_fd = pg_wire_test::create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(101);

    std::string received_sql;
    handler.set_query_executor(
        [&](const std::string& sql, const std::string& /*database*/) -> Result<QueryResult> {
            received_sql = sql;
            QueryResult qr;
            qr.affected_rows = 1;
            qr.message = "UPDATE";
            return ok(std::move(qr));
        });

    do_startup_for_test(client_fd, conn, handler);

    std::vector<uint8_t> batch;
    auto parse =
        pg_wire_test::build_parse_message("", "UPDATE t SET name = $1 WHERE id = $2", {25, 23});
    auto bind =
        pg_wire_test::build_bind_message("", "", {std::nullopt, std::optional<std::string>("5")});
    auto execute = pg_wire_test::build_execute_message("");
    auto sync = pg_wire_test::build_sync_message();

    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), execute.begin(), execute.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    pg_wire_test::write_to_fd(client_fd, batch);
    (void)conn.read_from_socket();
    auto result = handler.process(conn);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(received_sql, "UPDATE t SET name = NULL WHERE id = 5");

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(ParamSubstitutionWire, StringParameterWithEscaping) {
    int client_fd = -1;
    int server_fd = pg_wire_test::create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(102);

    std::string received_sql;
    handler.set_query_executor(
        [&](const std::string& sql, const std::string& /*database*/) -> Result<QueryResult> {
            received_sql = sql;
            QueryResult qr;
            qr.affected_rows = 1;
            qr.message = "INSERT";
            return ok(std::move(qr));
        });

    do_startup_for_test(client_fd, conn, handler);

    std::vector<uint8_t> batch;
    auto parse = pg_wire_test::build_parse_message("", "INSERT INTO t (name) VALUES ($1)", {25});
    auto bind =
        pg_wire_test::build_bind_message("", "", {std::optional<std::string>("it's a test")});
    auto execute = pg_wire_test::build_execute_message("");
    auto sync = pg_wire_test::build_sync_message();

    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), execute.begin(), execute.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    pg_wire_test::write_to_fd(client_fd, batch);
    (void)conn.read_from_socket();
    auto result = handler.process(conn);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(received_sql, "INSERT INTO t (name) VALUES ('it''s a test')");

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(ParamSubstitutionWire, DeleteWithParameter) {
    int client_fd = -1;
    int server_fd = pg_wire_test::create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(103);

    std::string received_sql;
    handler.set_query_executor(
        [&](const std::string& sql, const std::string& /*database*/) -> Result<QueryResult> {
            received_sql = sql;
            QueryResult qr;
            qr.affected_rows = 3;
            qr.message = "DELETE";
            return ok(std::move(qr));
        });

    do_startup_for_test(client_fd, conn, handler);

    std::vector<uint8_t> batch;
    auto parse = pg_wire_test::build_parse_message("", "DELETE FROM t WHERE status = $1", {25});
    auto bind = pg_wire_test::build_bind_message("", "", {std::optional<std::string>("inactive")});
    auto execute = pg_wire_test::build_execute_message("");
    auto sync = pg_wire_test::build_sync_message();

    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), execute.begin(), execute.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    pg_wire_test::write_to_fd(client_fd, batch);
    (void)conn.read_from_socket();
    auto result = handler.process(conn);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(received_sql, "DELETE FROM t WHERE status = 'inactive'");

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(ParamSubstitutionWire, NoParamsPassthroughUnchanged) {
    int client_fd = -1;
    int server_fd = pg_wire_test::create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(104);

    std::string received_sql;
    handler.set_query_executor(
        [&](const std::string& sql, const std::string& /*database*/) -> Result<QueryResult> {
            received_sql = sql;
            QueryResult qr;
            qr.column_names = {"x"};
            qr.column_types = {TypeId::INT32};
            qr.rows = {{Value(static_cast<int32_t>(1))}};
            return ok(std::move(qr));
        });

    do_startup_for_test(client_fd, conn, handler);

    std::vector<uint8_t> batch;
    auto parse = pg_wire_test::build_parse_message("", "SELECT 1", {});
    auto bind = pg_wire_test::build_bind_message("", "", {});
    auto execute = pg_wire_test::build_execute_message("");
    auto sync = pg_wire_test::build_sync_message();

    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), execute.begin(), execute.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    pg_wire_test::write_to_fd(client_fd, batch);
    (void)conn.read_from_socket();
    auto result = handler.process(conn);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(received_sql, "SELECT 1");

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

// =============================================================================
// SQL-level EXECUTE with parameters
// =============================================================================

TEST(ParamSubstitutionWire, SqlLevelExecuteWithParams) {
    int client_fd = -1;
    int server_fd = pg_wire_test::create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(105);

    std::string received_sql;
    int call_count = 0;
    handler.set_query_executor(
        [&](const std::string& sql, const std::string& /*database*/) -> Result<QueryResult> {
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
    auto prepare_msg =
        pg_wire_test::build_query_message("PREPARE myquery (int) AS SELECT * FROM t WHERE id = $1");
    pg_wire_test::write_to_fd(client_fd, prepare_msg);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)pg_wire_test::read_from_fd(client_fd);

    // Now EXECUTE it with a parameter.
    auto execute_msg = pg_wire_test::build_query_message("EXECUTE myquery (42)");
    pg_wire_test::write_to_fd(client_fd, execute_msg);
    (void)conn.read_from_socket();
    auto result = handler.process(conn);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // The executor should have received the SQL with $1 replaced by 42.
    EXPECT_EQ(received_sql, "SELECT * FROM t WHERE id = 42");

    conn.close();
    sixseven_platform::socket_close(client_fd);
}
