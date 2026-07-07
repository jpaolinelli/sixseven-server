#include "sixseven/common/platform.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/server/connection.h"
#include "sixseven/server/pg_protocol.h"
#include "sixseven/server/session.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace sixseven;

// =============================================================================
// Session unit tests (direct API)
// =============================================================================

TEST(Session, DefaultVariablesInitialized) {
    Session session(1);

    auto work_mem = session.get_variable("work_mem");
    ASSERT_TRUE(work_mem.has_value());
    EXPECT_EQ(*work_mem, "4MB");

    auto isolation = session.get_variable("default_transaction_isolation");
    ASSERT_TRUE(isolation.has_value());
    EXPECT_EQ(*isolation, "read committed");

    auto search_path = session.get_variable("search_path");
    ASSERT_TRUE(search_path.has_value());
    EXPECT_EQ(*search_path, "public");

    auto timeout = session.get_variable("statement_timeout");
    ASSERT_TRUE(timeout.has_value());
    EXPECT_EQ(*timeout, "0");

    auto provider_url = session.get_variable("embedding_provider_url");
    ASSERT_TRUE(provider_url.has_value());
    EXPECT_EQ(*provider_url, "");

    auto api_key = session.get_variable("embedding_api_key");
    ASSERT_TRUE(api_key.has_value());
    EXPECT_EQ(*api_key, "");
}

TEST(Session, SetVariableUpdatesValue) {
    Session session(1);

    auto result = session.set_variable("work_mem", "8MB");
    ASSERT_TRUE(result.has_value());

    auto val = session.get_variable("work_mem");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "8MB");
}

TEST(Session, SetVariableIsCaseInsensitive) {
    Session session(1);

    auto result = session.set_variable("WORK_MEM", "16MB");
    ASSERT_TRUE(result.has_value());

    auto val = session.get_variable("work_mem");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "16MB");
}

TEST(Session, SetUnknownVariableReturnsError) {
    Session session(1);
    auto result = session.set_variable("nonexistent", "value");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(Session, GetUnknownVariableReturnsNullopt) {
    Session session(1);
    auto val = session.get_variable("nonexistent");
    EXPECT_FALSE(val.has_value());
}

TEST(Session, ResetVariableRestoresDefault) {
    Session session(1);

    (void)session.set_variable("work_mem", "16MB");
    EXPECT_EQ(*session.get_variable("work_mem"), "16MB");

    auto result = session.reset_variable("work_mem");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*session.get_variable("work_mem"), "4MB");
}

TEST(Session, ResetUnknownVariableReturnsError) {
    Session session(1);
    auto result = session.reset_variable("nonexistent");
    ASSERT_FALSE(result.has_value());
}

TEST(Session, ResetAllRestoresDefaults) {
    Session session(1);

    (void)session.set_variable("work_mem", "16MB");
    (void)session.set_variable("statement_timeout", "5000");

    session.reset_all_variables();

    EXPECT_EQ(*session.get_variable("work_mem"), "4MB");
    EXPECT_EQ(*session.get_variable("statement_timeout"), "0");
}

TEST(Session, GetAllVariablesSortedByName) {
    Session session(1);
    auto vars = session.get_all_variables();

    ASSERT_EQ(vars.size(), 18u);

    // Verify sorted by name.
    for (size_t i = 1; i < vars.size(); ++i) {
        EXPECT_LT(vars[i - 1].first, vars[i].first);
    }
}

TEST(Session, IsSessionVariable) {
    Session session(1);

    EXPECT_TRUE(session.is_session_variable("work_mem"));
    EXPECT_TRUE(session.is_session_variable("WORK_MEM"));
    EXPECT_TRUE(session.is_session_variable("statement_timeout"));
    EXPECT_FALSE(session.is_session_variable("nonexistent"));
}

// =============================================================================
// Session isolation tests
// =============================================================================

TEST(Session, VariablesIsolatedBetweenSessions) {
    Session session1(1);
    Session session2(2);

    (void)session1.set_variable("work_mem", "16MB");

    // session2 should still have the default.
    EXPECT_EQ(*session1.get_variable("work_mem"), "16MB");
    EXPECT_EQ(*session2.get_variable("work_mem"), "4MB");
}

TEST(Session, PreparedStatementsIsolatedBetweenSessions) {
    Session session1(1);
    Session session2(2);

    PreparedStatement stmt;
    stmt.name = "my_stmt";
    stmt.sql = "SELECT 1";
    session1.add_prepared_statement("my_stmt", std::move(stmt));

    EXPECT_NE(session1.get_prepared_statement("my_stmt"), nullptr);
    EXPECT_EQ(session2.get_prepared_statement("my_stmt"), nullptr);
}

// =============================================================================
// Prepared statement tests
// =============================================================================

TEST(Session, AddAndGetPreparedStatement) {
    Session session(1);

    PreparedStatement stmt;
    stmt.name = "test";
    stmt.sql = "SELECT $1";
    stmt.param_oids = {23};
    session.add_prepared_statement("test", std::move(stmt));

    const auto* found = session.get_prepared_statement("test");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name, "test");
    EXPECT_EQ(found->sql, "SELECT $1");
    ASSERT_EQ(found->param_oids.size(), 1u);
    EXPECT_EQ(found->param_oids[0], 23u);
}

TEST(Session, RemovePreparedStatement) {
    Session session(1);

    PreparedStatement stmt;
    stmt.name = "test";
    stmt.sql = "SELECT 1";
    session.add_prepared_statement("test", std::move(stmt));

    session.remove_prepared_statement("test");
    EXPECT_EQ(session.get_prepared_statement("test"), nullptr);
}

TEST(Session, RemoveAllPreparedStatements) {
    Session session(1);

    PreparedStatement stmt1, stmt2;
    stmt1.name = "s1";
    stmt1.sql = "SELECT 1";
    stmt2.name = "s2";
    stmt2.sql = "SELECT 2";
    session.add_prepared_statement("s1", std::move(stmt1));
    session.add_prepared_statement("s2", std::move(stmt2));

    session.remove_all_prepared_statements();
    EXPECT_TRUE(session.prepared_statements().empty());
}

// =============================================================================
// Portal tests
// =============================================================================

TEST(Session, AddAndGetPortal) {
    Session session(1);

    Portal portal;
    portal.name = "p1";
    portal.sql = "SELECT 1";
    session.add_portal("p1", std::move(portal));

    const auto* found = session.get_portal("p1");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name, "p1");
    EXPECT_EQ(found->sql, "SELECT 1");
}

TEST(Session, RemovePortal) {
    Session session(1);

    Portal portal;
    portal.name = "p1";
    portal.sql = "SELECT 1";
    session.add_portal("p1", std::move(portal));

    session.remove_portal("p1");
    EXPECT_EQ(session.get_portal("p1"), nullptr);
}

// =============================================================================
// Transaction state tests
// =============================================================================

TEST(Session, DefaultTransactionStateIsIdle) {
    Session session(1);
    EXPECT_EQ(session.transaction_state(), TransactionState::IDLE);
    EXPECT_EQ(session.ready_for_query_status(), 'I');
}

TEST(Session, BeginSetsInTransaction) {
    Session session(1);
    session.update_transaction_state("BEGIN", true);
    EXPECT_EQ(session.transaction_state(), TransactionState::IN_TRANSACTION);
    EXPECT_EQ(session.ready_for_query_status(), 'T');
}

TEST(Session, CommitResetsToIdle) {
    Session session(1);
    session.update_transaction_state("BEGIN", true);
    session.update_transaction_state("COMMIT", true);
    EXPECT_EQ(session.transaction_state(), TransactionState::IDLE);
    EXPECT_EQ(session.ready_for_query_status(), 'I');
}

TEST(Session, RollbackResetsToIdle) {
    Session session(1);
    session.update_transaction_state("BEGIN", true);
    session.update_transaction_state("ROLLBACK", true);
    EXPECT_EQ(session.transaction_state(), TransactionState::IDLE);
    EXPECT_EQ(session.ready_for_query_status(), 'I');
}

TEST(Session, ErrorInTransactionSetsFailed) {
    Session session(1);
    session.update_transaction_state("BEGIN", true);
    // A statement fails while in a transaction.
    session.update_transaction_state("SELECT bad", false);
    EXPECT_EQ(session.transaction_state(), TransactionState::FAILED);
    EXPECT_EQ(session.ready_for_query_status(), 'E');
}

TEST(Session, RollbackFromFailedResetsToIdle) {
    Session session(1);
    session.update_transaction_state("BEGIN", true);
    session.update_transaction_state("SELECT bad", false);
    EXPECT_EQ(session.transaction_state(), TransactionState::FAILED);

    session.update_transaction_state("ROLLBACK", true);
    EXPECT_EQ(session.transaction_state(), TransactionState::IDLE);
    EXPECT_EQ(session.ready_for_query_status(), 'I');
}

TEST(Session, ErrorOutsideTransactionStaysIdle) {
    Session session(1);
    // Error outside a transaction should not change state to FAILED.
    session.update_transaction_state("SELECT bad", false);
    EXPECT_EQ(session.transaction_state(), TransactionState::IDLE);
}

// GDB-976: the optional TRANSACTION/WORK suffix and transaction modes are legal
// SQL; classification must key off the leading keyword, not whole-string
// equality (which left ReadyForQuery status desynchronized).
TEST(Session, BeginTransactionKeywordSetsInTransaction) {
    Session session(1);
    session.update_transaction_state("BEGIN TRANSACTION", true);
    EXPECT_EQ(session.transaction_state(), TransactionState::IN_TRANSACTION);
    EXPECT_EQ(session.ready_for_query_status(), 'T');
}

TEST(Session, BeginWorkSetsInTransaction) {
    Session session(1);
    session.update_transaction_state("BEGIN WORK", true);
    EXPECT_EQ(session.transaction_state(), TransactionState::IN_TRANSACTION);
}

TEST(Session, BeginWithTrailingSemicolonSetsInTransaction) {
    Session session(1);
    session.update_transaction_state("BEGIN;", true);
    EXPECT_EQ(session.transaction_state(), TransactionState::IN_TRANSACTION);
}

TEST(Session, StartTransactionWithIsolationModeSetsInTransaction) {
    Session session(1);
    session.update_transaction_state("START TRANSACTION ISOLATION LEVEL SERIALIZABLE", true);
    EXPECT_EQ(session.transaction_state(), TransactionState::IN_TRANSACTION);
}

TEST(Session, CommitTransactionKeywordResetsToIdle) {
    Session session(1);
    session.update_transaction_state("BEGIN TRANSACTION", true);
    session.update_transaction_state("COMMIT TRANSACTION", true);
    EXPECT_EQ(session.transaction_state(), TransactionState::IDLE);
    EXPECT_EQ(session.ready_for_query_status(), 'I');
}

TEST(Session, EndTransactionKeywordResetsToIdle) {
    Session session(1);
    session.update_transaction_state("BEGIN", true);
    session.update_transaction_state("END TRANSACTION", true);
    EXPECT_EQ(session.transaction_state(), TransactionState::IDLE);
}

TEST(Session, RollbackWorkKeywordResetsToIdle) {
    Session session(1);
    session.update_transaction_state("BEGIN", true);
    session.update_transaction_state("ROLLBACK WORK", true);
    EXPECT_EQ(session.transaction_state(), TransactionState::IDLE);
}

TEST(Session, FailureAfterBeginTransactionKeywordSetsFailed) {
    Session session(1);
    session.update_transaction_state("BEGIN TRANSACTION", true);
    session.update_transaction_state("SELECT bad", false);
    EXPECT_EQ(session.transaction_state(), TransactionState::FAILED);
    EXPECT_EQ(session.ready_for_query_status(), 'E');
}

// ROLLBACK TO <savepoint> only unwinds to a savepoint; it must NOT end the
// transaction (status stays 'T'), unlike a bare ROLLBACK.
TEST(Session, RollbackToSavepointKeepsTransactionOpen) {
    Session session(1);
    session.update_transaction_state("BEGIN", true);
    session.update_transaction_state("ROLLBACK TO SAVEPOINT sp1", true);
    EXPECT_EQ(session.transaction_state(), TransactionState::IN_TRANSACTION);
    EXPECT_EQ(session.ready_for_query_status(), 'T');
}

// =============================================================================
// Cleanup test
// =============================================================================

TEST(Session, CleanupReleasesAllResources) {
    Session session(1);

    (void)session.set_variable("work_mem", "16MB");

    PreparedStatement stmt;
    stmt.name = "s1";
    stmt.sql = "SELECT 1";
    session.add_prepared_statement("s1", std::move(stmt));

    Portal portal;
    portal.name = "p1";
    portal.sql = "SELECT 1";
    session.add_portal("p1", std::move(portal));

    session.update_transaction_state("BEGIN", true);
    EXPECT_EQ(session.transaction_state(), TransactionState::IN_TRANSACTION);

    session.cleanup();

    EXPECT_TRUE(session.prepared_statements().empty());
    EXPECT_TRUE(session.portals().empty());
    EXPECT_EQ(session.transaction_state(), TransactionState::IDLE);
    EXPECT_EQ(*session.get_variable("work_mem"), "4MB");
}

// =============================================================================
// Session command handling tests (try_handle_command)
// =============================================================================

TEST(Session, HandleSetVariable) {
    Session session(1);

    auto result = session.try_handle_command("SET work_mem = '16MB'");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().message, "SET");
    EXPECT_EQ(*session.get_variable("work_mem"), "16MB");
}

TEST(Session, HandleSetVariableWithTo) {
    Session session(1);

    auto result = session.try_handle_command("SET work_mem TO '32MB'");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(*session.get_variable("work_mem"), "32MB");
}

TEST(Session, HandleSetVariableWithoutQuotes) {
    Session session(1);

    auto result = session.try_handle_command("SET statement_timeout = 5000");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(*session.get_variable("statement_timeout"), "5000");
}

TEST(Session, HandleSetUnknownVariablePassesThrough) {
    Session session(1);

    // Unknown variables should return nullopt (pass through to query executor).
    auto result = session.try_handle_command("SET unknown_var = 'value'");
    EXPECT_FALSE(result.has_value());
}

TEST(Session, HandleShowVariable) {
    Session session(1);

    auto result = session.try_handle_command("SHOW work_mem");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());

    const auto& qr = result->value();
    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "work_mem");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "4MB");
}

TEST(Session, HandleShowAll) {
    Session session(1);

    auto result = session.try_handle_command("SHOW ALL");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());

    const auto& qr = result->value();
    EXPECT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "name");
    EXPECT_EQ(qr.column_names[1], "setting");
    EXPECT_EQ(qr.rows.size(), 18u);
}

TEST(Session, HandleShowUnknownPassesThrough) {
    Session session(1);

    // SHOW TABLES should pass through.
    auto result = session.try_handle_command("SHOW TABLES");
    EXPECT_FALSE(result.has_value());
}

TEST(Session, HandleResetVariable) {
    Session session(1);

    (void)session.set_variable("work_mem", "16MB");

    auto result = session.try_handle_command("RESET work_mem");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().message, "RESET");
    EXPECT_EQ(*session.get_variable("work_mem"), "4MB");
}

TEST(Session, HandleResetAll) {
    Session session(1);

    (void)session.set_variable("work_mem", "16MB");
    (void)session.set_variable("statement_timeout", "5000");

    auto result = session.try_handle_command("RESET ALL");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(*session.get_variable("work_mem"), "4MB");
    EXPECT_EQ(*session.get_variable("statement_timeout"), "0");
}

TEST(Session, HandlePrepare) {
    Session session(1);

    auto result = session.try_handle_command("PREPARE my_stmt AS SELECT 1");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().message, "PREPARE");

    const auto* stmt = session.get_prepared_statement("my_stmt");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(stmt->sql, "SELECT 1");
}

TEST(Session, HandlePrepareWithParams) {
    Session session(1);

    auto result = session.try_handle_command("PREPARE my_stmt (int, text) AS SELECT $1, $2");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());

    const auto* stmt = session.get_prepared_statement("my_stmt");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(stmt->sql, "SELECT $1, $2");
    EXPECT_EQ(stmt->param_oids.size(), 2u);
}

TEST(Session, HandleDeallocate) {
    Session session(1);

    // Prepare first.
    (void)session.try_handle_command("PREPARE my_stmt AS SELECT 1");
    ASSERT_NE(session.get_prepared_statement("my_stmt"), nullptr);

    // Deallocate.
    auto result = session.try_handle_command("DEALLOCATE my_stmt");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().message, "DEALLOCATE");
    EXPECT_EQ(session.get_prepared_statement("my_stmt"), nullptr);
}

TEST(Session, HandleDeallocateAll) {
    Session session(1);

    (void)session.try_handle_command("PREPARE s1 AS SELECT 1");
    (void)session.try_handle_command("PREPARE s2 AS SELECT 2");

    auto result = session.try_handle_command("DEALLOCATE ALL");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_TRUE(session.prepared_statements().empty());
}

TEST(Session, HandleDeallocateNonexistentReturnsError) {
    Session session(1);

    auto result = session.try_handle_command("DEALLOCATE nonexistent");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(Session, NonSessionCommandReturnsNullopt) {
    Session session(1);

    // Regular SQL should return nullopt.
    EXPECT_FALSE(session.try_handle_command("SELECT 1").has_value());
    EXPECT_FALSE(session.try_handle_command("INSERT INTO t VALUES (1)").has_value());
    EXPECT_FALSE(session.try_handle_command("CREATE TABLE t (id INT)").has_value());
}

// =============================================================================
// Protocol-level session tests (via PgProtocolHandler)
// =============================================================================

namespace {

/// Helper: build a PG v3 StartupMessage.
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

/// Helper: build a PG Query message.
std::vector<uint8_t> build_query_message(std::string_view sql) {
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

/// Helper: create a socketpair and return the server-side fd.
int create_socketpair(int& client_fd_out) {
    int fds[2];
    int rc = sixseven_platform::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    EXPECT_EQ(rc, 0);
    client_fd_out = fds[1];
    return fds[0];
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

/// Helper: do a startup handshake and drain the response.
void do_startup(int client_fd, Connection& conn, PgProtocolHandler& handler) {
    auto startup = build_startup_message({{"user", "test"}});
    write_to_fd(client_fd, startup);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)read_from_fd(client_fd);
}

/// Helper: send a query and return the response bytes.
std::vector<uint8_t>
send_query(int client_fd, Connection& conn, PgProtocolHandler& handler, std::string_view sql) {
    auto query = build_query_message(sql);
    write_to_fd(client_fd, query);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    return read_from_fd(client_fd);
}

} // namespace

TEST(Session, ProtocolSetShowVariable) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(1);
    do_startup(client_fd, conn, handler);

    // SET work_mem.
    auto response = send_query(client_fd, conn, handler, "SET work_mem = '16MB'");
    ASSERT_FALSE(response.empty());

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;

    // Expect: CommandComplete ('C') + ReadyForQuery ('Z').
    ASSERT_TRUE(find_message(response, pos, 'C', payload, payload_len));
    MessageReader cc_reader(payload, payload_len);
    EXPECT_EQ(cc_reader.read_cstring(), "SET");

    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));
    EXPECT_EQ(payload[0], 'I');

    // SHOW work_mem.
    response = send_query(client_fd, conn, handler, "SHOW work_mem");
    ASSERT_FALSE(response.empty());

    pos = 0;
    // Expect: RowDescription ('T') + DataRow ('D') + CommandComplete ('C') + ReadyForQuery ('Z').
    ASSERT_TRUE(find_message(response, pos, 'T', payload, payload_len));
    ASSERT_TRUE(find_message(response, pos, 'D', payload, payload_len));
    // Read the DataRow to verify the value.
    MessageReader dr_reader(payload, payload_len);
    int16_t num_fields = dr_reader.read_int16();
    EXPECT_EQ(num_fields, 1);
    int32_t field_len = dr_reader.read_int32();
    EXPECT_GT(field_len, 0);
    std::string value(
        reinterpret_cast<const char*>(dr_reader.read_bytes(static_cast<size_t>(field_len))),
        static_cast<size_t>(field_len));
    EXPECT_EQ(value, "16MB");

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(Session, ProtocolResetVariable) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(2);
    do_startup(client_fd, conn, handler);

    // SET, then RESET.
    (void)send_query(client_fd, conn, handler, "SET work_mem = '16MB'");
    auto response = send_query(client_fd, conn, handler, "RESET work_mem");
    ASSERT_FALSE(response.empty());

    // Verify via SHOW.
    response = send_query(client_fd, conn, handler, "SHOW work_mem");
    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    ASSERT_TRUE(find_message(response, pos, 'T', payload, payload_len));
    ASSERT_TRUE(find_message(response, pos, 'D', payload, payload_len));
    MessageReader dr_reader(payload, payload_len);
    dr_reader.read_int16();
    int32_t field_len = dr_reader.read_int32();
    std::string value(
        reinterpret_cast<const char*>(dr_reader.read_bytes(static_cast<size_t>(field_len))),
        static_cast<size_t>(field_len));
    EXPECT_EQ(value, "4MB");

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(Session, ProtocolPrepareAndExecute) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(3);

    int exec_count = 0;
    handler.set_query_executor(
        [&exec_count](const std::string& sql,
                      const std::string& /*database*/) -> Result<QueryResult> {
            ++exec_count;
            if (sql == "SELECT 42") {
                QueryResult qr;
                qr.column_names = {"col"};
                qr.column_types = {TypeId::INT32};
                qr.rows = {{Value(static_cast<int32_t>(42))}};
                return ok(std::move(qr));
            }
            return make_error(StatusCode::INTERNAL_ERROR, "unexpected sql: " + sql);
        });

    do_startup(client_fd, conn, handler);

    // PREPARE.
    auto response = send_query(client_fd, conn, handler, "PREPARE my_stmt AS SELECT 42");
    ASSERT_FALSE(response.empty());

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    ASSERT_TRUE(find_message(response, pos, 'C', payload, payload_len));
    MessageReader cc_reader(payload, payload_len);
    EXPECT_EQ(cc_reader.read_cstring(), "PREPARE");

    // EXECUTE — should run the prepared SQL through the query executor.
    response = send_query(client_fd, conn, handler, "EXECUTE my_stmt");
    ASSERT_FALSE(response.empty());

    pos = 0;
    ASSERT_TRUE(find_message(response, pos, 'T', payload, payload_len));
    ASSERT_TRUE(find_message(response, pos, 'D', payload, payload_len));
    ASSERT_TRUE(find_message(response, pos, 'C', payload, payload_len));
    ASSERT_TRUE(find_message(response, pos, 'Z', payload, payload_len));

    EXPECT_EQ(exec_count, 1);

    // Execute again — should work (statement persists).
    response = send_query(client_fd, conn, handler, "EXECUTE my_stmt");
    EXPECT_EQ(exec_count, 2);

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(Session, ProtocolDeallocate) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(4);
    do_startup(client_fd, conn, handler);

    // PREPARE.
    (void)send_query(client_fd, conn, handler, "PREPARE my_stmt AS SELECT 1");
    ASSERT_NE(handler.session().get_prepared_statement("my_stmt"), nullptr);

    // DEALLOCATE.
    auto response = send_query(client_fd, conn, handler, "DEALLOCATE my_stmt");
    ASSERT_FALSE(response.empty());

    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    ASSERT_TRUE(find_message(response, pos, 'C', payload, payload_len));

    EXPECT_EQ(handler.session().get_prepared_statement("my_stmt"), nullptr);

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(Session, ProtocolTransactionStateTracking) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(5);

    handler.set_query_executor(
        [](const std::string& sql, const std::string& /*database*/) -> Result<QueryResult> {
            if (sql == "BEGIN" || sql == "COMMIT" || sql == "ROLLBACK") {
                QueryResult qr;
                qr.message = sql;
                return ok(std::move(qr));
            }
            if (sql == "SELECT 1") {
                QueryResult qr;
                qr.column_names = {"col"};
                qr.column_types = {TypeId::INT32};
                qr.rows = {{Value(static_cast<int32_t>(1))}};
                return ok(std::move(qr));
            }
            return make_error(StatusCode::PARSE_ERROR, "error");
        });

    do_startup(client_fd, conn, handler);

    // Scans `response` for all ReadyForQuery ('Z') messages, asserting that
    // each one's transaction-status byte equals `expected_status`. Returns
    // the number of 'Z' messages found so the caller can assert at least one
    // was present -- otherwise the status-byte EXPECT_EQ below would never
    // execute and the test would pass vacuously.
    auto count_ready_for_query_with_status = [](const std::vector<uint8_t>& resp,
                                                uint8_t expected_status) -> size_t {
        size_t z_count = 0;
        size_t scan_pos = 0;
        while (scan_pos + 5 <= resp.size()) {
            uint8_t msg_type = resp[scan_pos];
            uint32_t length = (static_cast<uint32_t>(resp[scan_pos + 1]) << 24) |
                              (static_cast<uint32_t>(resp[scan_pos + 2]) << 16) |
                              (static_cast<uint32_t>(resp[scan_pos + 3]) << 8) |
                              static_cast<uint32_t>(resp[scan_pos + 4]);
            size_t total = 1 + static_cast<size_t>(length);
            if (scan_pos + total > resp.size()) {
                break;
            }
            if (msg_type == 'Z') {
                ++z_count;
                EXPECT_EQ(resp[scan_pos + 5], expected_status)
                    << "ReadyForQuery status byte mismatch at offset " << scan_pos;
            }
            scan_pos += total;
        }
        return z_count;
    };

    // After BEGIN, ReadyForQuery should be 'T'.
    auto response = send_query(client_fd, conn, handler, "BEGIN");
    size_t z_count = count_ready_for_query_with_status(response, 'T');
    ASSERT_GE(z_count, 1u) << "expected at least one ReadyForQuery ('Z') message after BEGIN";

    // After COMMIT, ReadyForQuery should be 'I'.
    response = send_query(client_fd, conn, handler, "COMMIT");
    z_count = count_ready_for_query_with_status(response, 'I');
    ASSERT_GE(z_count, 1u) << "expected at least one ReadyForQuery ('Z') message after COMMIT";

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(Session, ProtocolSessionVariableIsolation) {
    // Two handlers (simulating two connections) should have independent state.
    int client_fd1 = -1, client_fd2 = -1;
    int server_fd1 = create_socketpair(client_fd1);
    int server_fd2 = create_socketpair(client_fd2);

    Connection conn1(server_fd1);
    Connection conn2(server_fd2);
    PgProtocolHandler handler1(1);
    PgProtocolHandler handler2(2);

    do_startup(client_fd1, conn1, handler1);
    do_startup(client_fd2, conn2, handler2);

    // Set work_mem on handler1 only.
    (void)send_query(client_fd1, conn1, handler1, "SET work_mem = '16MB'");

    // Verify handler1 has 16MB.
    EXPECT_EQ(*handler1.session().get_variable("work_mem"), "16MB");
    // Verify handler2 still has default.
    EXPECT_EQ(*handler2.session().get_variable("work_mem"), "4MB");

    conn1.close();
    conn2.close();
    sixseven_platform::socket_close(client_fd1);
    sixseven_platform::socket_close(client_fd2);
}

TEST(Session, ProtocolNonSessionSetPassesThrough) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(6);

    bool query_executor_called = false;
    handler.set_query_executor(
        [&query_executor_called](const std::string& /*sql*/,
                                 const std::string& /*database*/) -> Result<QueryResult> {
            query_executor_called = true;
            QueryResult qr;
            qr.message = "SET";
            return ok(std::move(qr));
        });

    do_startup(client_fd, conn, handler);

    // SET for an unknown variable should pass through to query executor.
    (void)send_query(client_fd, conn, handler, "SET some_unknown_var = 'value'");
    EXPECT_TRUE(query_executor_called);

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(Session, ProtocolShowTablesPassesThrough) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(7);

    bool query_executor_called = false;
    handler.set_query_executor(
        [&query_executor_called](const std::string& /*sql*/,
                                 const std::string& /*database*/) -> Result<QueryResult> {
            query_executor_called = true;
            QueryResult qr;
            qr.column_names = {"table_name"};
            qr.column_types = {TypeId::STRING};
            return ok(std::move(qr));
        });

    do_startup(client_fd, conn, handler);

    // SHOW TABLES should pass through.
    (void)send_query(client_fd, conn, handler, "SHOW TABLES");
    EXPECT_TRUE(query_executor_called);

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(Session, ProtocolSharedPreparedStmtStore) {
    // Verify that SQL-level PREPARE and wire protocol Parse share the same store.
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(8);
    do_startup(client_fd, conn, handler);

    // SQL-level PREPARE.
    (void)send_query(client_fd, conn, handler, "PREPARE sql_stmt AS SELECT 1");

    // Verify the statement is visible through the protocol handler's accessor.
    ASSERT_EQ(handler.prepared_statements().count("sql_stmt"), 1u);
    EXPECT_EQ(handler.prepared_statements().at("sql_stmt").sql, "SELECT 1");

    conn.close();
    sixseven_platform::socket_close(client_fd);
}

TEST(Session, ProtocolCleanupOnTerminate) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(9);
    do_startup(client_fd, conn, handler);

    // Add some state.
    (void)send_query(client_fd, conn, handler, "SET work_mem = '16MB'");
    (void)send_query(client_fd, conn, handler, "PREPARE my_stmt AS SELECT 1");

    // Send Terminate.
    std::vector<uint8_t> terminate = {'X', 0, 0, 0, 4};
    write_to_fd(client_fd, terminate);
    (void)conn.read_from_socket();
    (void)handler.process(conn);

    EXPECT_EQ(handler.state(), ProtocolState::CLOSED);

    // Session should be cleaned up.
    EXPECT_TRUE(handler.prepared_statements().empty());
    EXPECT_EQ(*handler.session().get_variable("work_mem"), "4MB");

    conn.close();
    sixseven_platform::socket_close(client_fd);
}
