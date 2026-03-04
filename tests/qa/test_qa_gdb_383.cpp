/// @file test_qa_gdb_383.cpp
/// @brief QA adversarial tests for GDB-383: std::stoi overflow crash on large parameter numbers.
///
/// The bug: substitute_parameters() and substitute_sql_expressions() used std::stoi() to parse
/// parameter numbers from $N placeholders. When N exceeds INT_MAX, std::stoi throws
/// std::out_of_range, crashing the server. The fix replaces std::stoi with std::from_chars.

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

#include <climits>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace giodb;

namespace {

constexpr uint32_t OID_INT4 = 23;
constexpr uint32_t OID_TEXT = 25;

} // namespace

// =============================================================================
// substitute_parameters() — overflow boundary tests
// =============================================================================

TEST(QA_GDB383_Overflow, ParamNumberExactlyIntMax) {
    // $2147483647 (INT_MAX) is a valid int parse but way out of range for param count.
    // Must return INVALID_ARGUMENT (out of range), not crash.
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    std::string sql = "SELECT $" + std::to_string(INT_MAX);
    auto result = substitute_parameters(sql, params, oids);
    ASSERT_FALSE(result.has_value()) << "INT_MAX param number should be out of range";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB383_Overflow, ParamNumberIntMaxPlusOne) {
    // $2147483648 overflows int. Must return error, not throw/crash.
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $2147483648", params, oids);
    ASSERT_FALSE(result.has_value()) << "INT_MAX+1 should be rejected";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB383_Overflow, ParamNumberWayOverIntMax) {
    // $99999999999 — well beyond INT_MAX.
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $99999999999", params, oids);
    ASSERT_FALSE(result.has_value()) << "Huge param number should be rejected";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB383_Overflow, ParamNumber20DigitsAllNines) {
    // A 20-digit number that exceeds even int64 max.
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $99999999999999999999", params, oids);
    ASSERT_FALSE(result.has_value()) << "20-digit param number should be rejected";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB383_Overflow, ParamNumber100Digits) {
    // Extremely long digit string — stress test the parser.
    std::string huge_num(100, '9');
    std::string sql = "SELECT $" + huge_num;
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters(sql, params, oids);
    ASSERT_FALSE(result.has_value()) << "100-digit param number should be rejected";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB383_Overflow, ParamNumberUint32Max) {
    // $4294967295 (UINT32_MAX) overflows signed int.
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $4294967295", params, oids);
    ASSERT_FALSE(result.has_value()) << "UINT32_MAX should overflow int and be rejected";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB383_Overflow, ParamNumberInt64Max) {
    // $9223372036854775807 (INT64_MAX) far exceeds int.
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    std::string sql = "SELECT $" + std::to_string(INT64_MAX);
    auto result = substitute_parameters(sql, params, oids);
    ASSERT_FALSE(result.has_value()) << "INT64_MAX param number should be rejected";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// Multiple overflow parameters in a single query
// =============================================================================

TEST(QA_GDB383_Overflow, MultipleOverflowParamsInOneQuery) {
    // Both $99999999999 and $88888888888 overflow.
    // The function should fail on the first one encountered.
    std::vector<std::optional<std::string>> params = {"42", "43"};
    std::vector<uint32_t> oids = {OID_INT4, OID_INT4};
    auto result = substitute_parameters("SELECT $99999999999, $88888888888", params, oids);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB383_Overflow, MixedValidAndOverflowParams) {
    // $1 is valid, $2147483648 overflows. Must still return error.
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $1, $2147483648", params, oids);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// Overflow inside quoted strings — should NOT be detected as a param
// =============================================================================

TEST(QA_GDB383_Overflow, OverflowParamInsideSingleQuotedString) {
    // $99999999999 inside single quotes is a literal string, not a param.
    std::vector<std::optional<std::string>> params = {};
    std::vector<uint32_t> oids = {};
    auto result = substitute_parameters("SELECT '$99999999999'", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT '$99999999999'");
}

TEST(QA_GDB383_Overflow, OverflowParamInsideDoubleQuotedIdentifier) {
    // $99999999999 inside double quotes is an identifier, not a param.
    std::vector<std::optional<std::string>> params = {};
    std::vector<uint32_t> oids = {};
    auto result = substitute_parameters("SELECT \"$99999999999\"", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT \"$99999999999\"");
}

// =============================================================================
// Error message quality — must mention the parameter number
// =============================================================================

TEST(QA_GDB383_Overflow, ErrorMessageContainsParamNumber) {
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $2147483648", params, oids);
    ASSERT_FALSE(result.has_value());
    // The error message should include the offending parameter number.
    EXPECT_NE(result.error().message.find("2147483648"), std::string::npos)
        << "Error message should include the overflowed param number, got: "
        << result.error().message;
}

TEST(QA_GDB383_Overflow, ErrorMessageContainsHugeParamNumber) {
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $99999999999", params, oids);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("99999999999"), std::string::npos)
        << "Error message should include the overflowed param number, got: "
        << result.error().message;
}

// =============================================================================
// No-throw guarantee — explicitly verify no exception escapes
// =============================================================================

TEST(QA_GDB383_NoThrow, SubstituteParamsDoesNotThrowOnOverflow) {
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    EXPECT_NO_THROW({
        auto result = substitute_parameters("SELECT $99999999999", params, oids);
        (void)result;
    });
}

TEST(QA_GDB383_NoThrow, SubstituteParamsDoesNotThrowOnIntMaxPlusOne) {
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    EXPECT_NO_THROW({
        auto result = substitute_parameters("SELECT $2147483648", params, oids);
        (void)result;
    });
}

TEST(QA_GDB383_NoThrow, SubstituteParamsDoesNotThrowOn100DigitNumber) {
    std::string huge_num(100, '9');
    std::string sql = "SELECT $" + huge_num;
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    EXPECT_NO_THROW({
        auto result = substitute_parameters(sql, params, oids);
        (void)result;
    });
}

// =============================================================================
// Wire protocol EXECUTE path — tests substitute_sql_expressions() overflow
// (substitute_sql_expressions is file-scoped, so we test it through the wire)
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

TEST(QA_GDB383_WireExecute, OverflowParamInSqlExecuteDoesNotCrash) {
    // Tests that substitute_sql_expressions() handles overflow param numbers
    // via the SQL-level EXECUTE path (PREPARE + EXECUTE with $N overflow).
    int client_fd = -1;
    int server_fd = create_socketpair_for_test(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(383);

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

    // PREPARE a statement with $1 placeholder.
    auto prepare_msg = build_query_msg("PREPARE overflow_test (int) AS SELECT * FROM t WHERE id = $1");
    write_to_fd_for_test(client_fd, prepare_msg);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)read_from_fd_for_test(client_fd);

    // EXECUTE with an overflowing parameter reference $99999999999.
    // The EXECUTE substitution path uses substitute_sql_expressions().
    // This should NOT crash the server.
    EXPECT_NO_THROW({
        auto execute_msg = build_query_msg("EXECUTE overflow_test ($99999999999)");
        write_to_fd_for_test(client_fd, execute_msg);
        (void)conn.read_from_socket();
        auto result = handler.process(conn);
        // We expect an error (not a crash). The server should handle this gracefully.
        (void)result;
    });

    conn.close();
    ::close(client_fd);
}

TEST(QA_GDB383_WireExecute, IntMaxPlusOneParamInSqlExecuteDoesNotCrash) {
    // Tests INT_MAX+1 through the substitute_sql_expressions() path.
    int client_fd = -1;
    int server_fd = create_socketpair_for_test(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(384);

    handler.set_query_executor([&](const std::string& sql) -> Result<QueryResult> {
        (void)sql;
        QueryResult qr;
        qr.column_names = {"id"};
        qr.column_types = {TypeId::INT32};
        qr.rows = {{Value(static_cast<int32_t>(1))}};
        return ok(std::move(qr));
    });

    do_startup_for_test(client_fd, conn, handler);

    auto prepare_msg =
        build_query_msg("PREPARE intmax_test (int) AS SELECT * FROM t WHERE id = $1");
    write_to_fd_for_test(client_fd, prepare_msg);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)read_from_fd_for_test(client_fd);

    EXPECT_NO_THROW({
        auto execute_msg = build_query_msg("EXECUTE intmax_test ($2147483648)");
        write_to_fd_for_test(client_fd, execute_msg);
        (void)conn.read_from_socket();
        auto result = handler.process(conn);
        (void)result;
    });

    conn.close();
    ::close(client_fd);
}

// =============================================================================
// Regression: valid params still work after the fix
// =============================================================================

TEST(QA_GDB383_Regression, ValidSmallParamStillWorks) {
    // Ensure the fix didn't break normal param substitution.
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT 42");
}

TEST(QA_GDB383_Regression, ValidMultiDigitParamStillWorks) {
    // $10 with 10 params should still work.
    std::vector<std::optional<std::string>> params;
    std::vector<uint32_t> oids;
    for (int i = 0; i < 10; ++i) {
        params.emplace_back(std::to_string(i + 1));
        oids.push_back(OID_INT4);
    }
    auto result = substitute_parameters("SELECT $10", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT 10");
}

TEST(QA_GDB383_Regression, ValidTextParamStillWorks) {
    std::vector<std::optional<std::string>> params = {"hello"};
    std::vector<uint32_t> oids = {OID_TEXT};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT 'hello'");
}

TEST(QA_GDB383_Regression, ParamNumberOneIsValid) {
    // Smallest valid param number.
    std::vector<std::optional<std::string>> params = {"1"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT 1");
}

TEST(QA_GDB383_Regression, ParamNumberZeroStillRejected) {
    // $0 should still be rejected as out of range (1-indexed).
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $0", params, oids);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}
