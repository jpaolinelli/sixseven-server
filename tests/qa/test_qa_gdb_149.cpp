#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/server/pg_protocol.h"
#include "sixseven/server/session.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace sixseven;

// =============================================================================
// QA_GDB_149: Session management — adversarial tests
// =============================================================================

// -- SET command parsing edge cases -------------------------------------------

TEST(QA_GDB_149, SetWithLeadingTrailingWhitespace) {
    Session s(1);
    auto r = s.try_handle_command("   SET work_mem = '16MB'   ");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(*s.get_variable("work_mem"), "16MB");
}

TEST(QA_GDB_149, SetWithExtraInternalWhitespace) {
    Session s(1);
    auto r = s.try_handle_command("SET   work_mem   =   '32MB'");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(*s.get_variable("work_mem"), "32MB");
}

TEST(QA_GDB_149, SetWithNoNameOrValue) {
    Session s(1);
    // "SET " with nothing after should return nullopt (empty rest after trim).
    auto r = s.try_handle_command("SET ");
    EXPECT_FALSE(r.has_value());
}

TEST(QA_GDB_149, SetNameOnlyNoSeparator) {
    Session s(1);
    // "SET work_mem" with no = or TO — should return nullopt.
    auto r = s.try_handle_command("SET work_mem");
    EXPECT_FALSE(r.has_value());
}

TEST(QA_GDB_149, SetEmptyNameWithEquals) {
    Session s(1);
    // "SET = value" — empty name should return nullopt.
    auto r = s.try_handle_command("SET = value");
    EXPECT_FALSE(r.has_value());
}

TEST(QA_GDB_149, SetEmptyValueAfterEquals) {
    Session s(1);
    // "SET work_mem = " — empty value (trimmed) is an empty string.
    auto r = s.try_handle_command("SET work_mem = ");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(*s.get_variable("work_mem"), "");
}

TEST(QA_GDB_149, SetValueWithEmbeddedEquals) {
    Session s(1);
    // Value containing '=' — only first '=' is used as separator.
    auto r = s.try_handle_command("SET embedding_provider_url = 'http://host?key=val'");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(*s.get_variable("embedding_provider_url"), "http://host?key=val");
}

TEST(QA_GDB_149, SetValueWithToKeywordUsingEquals) {
    Session s(1);
    // Value containing "to" — should not confuse parsing when '=' is used.
    auto r = s.try_handle_command("SET embedding_provider_url = 'http://to.example.com'");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(*s.get_variable("embedding_provider_url"), "http://to.example.com");
}

TEST(QA_GDB_149, SetWithToSyntax) {
    Session s(1);
    auto r = s.try_handle_command("SET work_mem TO '64MB'");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(*s.get_variable("work_mem"), "64MB");
}

TEST(QA_GDB_149, SetWithToSyntaxCaseInsensitive) {
    Session s(1);
    auto r = s.try_handle_command("SET work_mem to '64MB'");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(*s.get_variable("work_mem"), "64MB");
}

TEST(QA_GDB_149, SetCaseInsensitiveCommand) {
    Session s(1);
    // "set" in lowercase should still be recognized.
    // starts_with_ci checks case-insensitively.
    auto r = s.try_handle_command("set work_mem = '8MB'");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(*s.get_variable("work_mem"), "8MB");
}

TEST(QA_GDB_149, SetMixedCaseCommand) {
    Session s(1);
    auto r = s.try_handle_command("SeT work_mem = '8MB'");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(*s.get_variable("work_mem"), "8MB");
}

TEST(QA_GDB_149, SetValueWithoutQuotes) {
    Session s(1);
    auto r = s.try_handle_command("SET statement_timeout = 5000");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(*s.get_variable("statement_timeout"), "5000");
}

TEST(QA_GDB_149, SetStripsSingleQuotesOnly) {
    Session s(1);
    // Double quotes should NOT be stripped — only single quotes.
    auto r = s.try_handle_command("SET work_mem = \"16MB\"");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(*s.get_variable("work_mem"), "\"16MB\"");
}

TEST(QA_GDB_149, SetOverwriteValueMultipleTimes) {
    Session s(1);
    (void)s.try_handle_command("SET work_mem = '1MB'");
    (void)s.try_handle_command("SET work_mem = '2MB'");
    (void)s.try_handle_command("SET work_mem = '3MB'");
    EXPECT_EQ(*s.get_variable("work_mem"), "3MB");
}

TEST(QA_GDB_149, SetAllVariablesInSequence) {
    Session s(1);
    (void)s.try_handle_command("SET work_mem = '8MB'");
    (void)s.try_handle_command("SET default_transaction_isolation = 'serializable'");
    (void)s.try_handle_command("SET search_path = 'myschema'");
    (void)s.try_handle_command("SET embedding_provider_url = 'http://localhost'");
    (void)s.try_handle_command("SET embedding_api_key = 'secret123'");
    (void)s.try_handle_command("SET statement_timeout = '30000'");

    EXPECT_EQ(*s.get_variable("work_mem"), "8MB");
    EXPECT_EQ(*s.get_variable("default_transaction_isolation"), "serializable");
    EXPECT_EQ(*s.get_variable("search_path"), "myschema");
    EXPECT_EQ(*s.get_variable("embedding_provider_url"), "http://localhost");
    EXPECT_EQ(*s.get_variable("embedding_api_key"), "secret123");
    EXPECT_EQ(*s.get_variable("statement_timeout"), "30000");
}

// -- SHOW command edge cases --------------------------------------------------

TEST(QA_GDB_149, ShowEmptyAfterKeyword) {
    Session s(1);
    auto r = s.try_handle_command("SHOW ");
    EXPECT_FALSE(r.has_value());
}

TEST(QA_GDB_149, ShowAllCaseInsensitive) {
    Session s(1);
    auto r = s.try_handle_command("SHOW all");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(r->value().rows.size(), 6u);
}

TEST(QA_GDB_149, ShowAllMixedCase) {
    Session s(1);
    auto r = s.try_handle_command("SHOW All");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(r->value().rows.size(), 6u);
}

TEST(QA_GDB_149, ShowVariableCaseInsensitive) {
    Session s(1);
    auto r = s.try_handle_command("SHOW WORK_MEM");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(r->value().rows[0][0].as_string(), "4MB");
}

TEST(QA_GDB_149, ShowAfterSetReflectsNewValue) {
    Session s(1);
    (void)s.try_handle_command("SET work_mem = '99MB'");
    auto r = s.try_handle_command("SHOW work_mem");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(r->value().rows[0][0].as_string(), "99MB");
}

TEST(QA_GDB_149, ShowAllReturnsSortedRows) {
    Session s(1);
    auto r = s.try_handle_command("SHOW ALL");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    const auto& rows = r->value().rows;
    for (size_t i = 1; i < rows.size(); ++i) {
        EXPECT_LT(rows[i - 1][0].as_string(), rows[i][0].as_string())
            << "SHOW ALL rows not sorted by name";
    }
}

TEST(QA_GDB_149, ShowAllColumnMetadata) {
    Session s(1);
    auto r = s.try_handle_command("SHOW ALL");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    const auto& qr = r->value();
    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "name");
    EXPECT_EQ(qr.column_names[1], "setting");
    ASSERT_EQ(qr.column_types.size(), 2u);
    EXPECT_EQ(qr.column_types[0], TypeId::STRING);
    EXPECT_EQ(qr.column_types[1], TypeId::STRING);
}

TEST(QA_GDB_149, ShowSingleVariableColumnName) {
    Session s(1);
    auto r = s.try_handle_command("SHOW work_mem");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    const auto& qr = r->value();
    ASSERT_EQ(qr.column_names.size(), 1u);
    // Column name should be lowercased variable name.
    EXPECT_EQ(qr.column_names[0], "work_mem");
}

// -- RESET command edge cases -------------------------------------------------

TEST(QA_GDB_149, ResetEmptyAfterKeyword) {
    Session s(1);
    auto r = s.try_handle_command("RESET ");
    EXPECT_FALSE(r.has_value());
}

TEST(QA_GDB_149, ResetUnknownPassesThrough) {
    Session s(1);
    auto r = s.try_handle_command("RESET some_unknown_var");
    EXPECT_FALSE(r.has_value());
}

TEST(QA_GDB_149, ResetAllCaseInsensitive) {
    Session s(1);
    (void)s.set_variable("work_mem", "99MB");
    auto r = s.try_handle_command("RESET all");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(*s.get_variable("work_mem"), "4MB");
}

TEST(QA_GDB_149, ResetIdempotent) {
    Session s(1);
    // Reset without prior set should still leave default.
    auto r = s.try_handle_command("RESET work_mem");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(*s.get_variable("work_mem"), "4MB");
}

TEST(QA_GDB_149, ResetAllAfterSettingAllVars) {
    Session s(1);
    (void)s.set_variable("work_mem", "X");
    (void)s.set_variable("statement_timeout", "X");
    (void)s.set_variable("search_path", "X");
    (void)s.set_variable("default_transaction_isolation", "X");
    (void)s.set_variable("embedding_provider_url", "X");
    (void)s.set_variable("embedding_api_key", "X");

    auto r = s.try_handle_command("RESET ALL");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());

    EXPECT_EQ(*s.get_variable("work_mem"), "4MB");
    EXPECT_EQ(*s.get_variable("statement_timeout"), "0");
    EXPECT_EQ(*s.get_variable("search_path"), "public");
    EXPECT_EQ(*s.get_variable("default_transaction_isolation"), "read committed");
    EXPECT_EQ(*s.get_variable("embedding_provider_url"), "");
    EXPECT_EQ(*s.get_variable("embedding_api_key"), "");
}

// -- PREPARE command edge cases -----------------------------------------------

TEST(QA_GDB_149, PrepareEmptyStatement) {
    Session s(1);
    // "PREPARE " trims to "PREPARE" which doesn't match "PREPARE " prefix.
    // Passes through to query executor.
    auto r = s.try_handle_command("PREPARE ");
    EXPECT_FALSE(r.has_value());
}

TEST(QA_GDB_149, PrepareNameOnlyNoAs) {
    Session s(1);
    auto r = s.try_handle_command("PREPARE my_stmt");
    ASSERT_TRUE(r.has_value());
    ASSERT_FALSE(r->has_value());
    EXPECT_EQ(r->error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_149, PrepareNameWithSpaceButNoAs) {
    Session s(1);
    auto r = s.try_handle_command("PREPARE my_stmt SELECT 1");
    ASSERT_TRUE(r.has_value());
    ASSERT_FALSE(r->has_value());
    EXPECT_EQ(r->error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_149, PrepareEmptyParamList) {
    Session s(1);
    auto r = s.try_handle_command("PREPARE my_stmt () AS SELECT 1");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    const auto* stmt = s.get_prepared_statement("my_stmt");
    ASSERT_NE(stmt, nullptr);
    EXPECT_TRUE(stmt->param_oids.empty());
    EXPECT_EQ(stmt->sql, "SELECT 1");
}

TEST(QA_GDB_149, PrepareUnclosedParen) {
    Session s(1);
    auto r = s.try_handle_command("PREPARE my_stmt (int AS SELECT 1");
    ASSERT_TRUE(r.has_value());
    ASSERT_FALSE(r->has_value());
    EXPECT_EQ(r->error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_149, PrepareManyParams) {
    Session s(1);
    auto r = s.try_handle_command(
        "PREPARE my_stmt (int, text, bool, float, double) AS SELECT $1, $2, $3, $4, $5");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    const auto* stmt = s.get_prepared_statement("my_stmt");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(stmt->param_oids.size(), 5u);
    // All OIDs stored as 0 (unresolved).
    for (auto oid : stmt->param_oids) {
        EXPECT_EQ(oid, 0u);
    }
}

TEST(QA_GDB_149, PrepareAsEmptyBody) {
    Session s(1);
    auto r = s.try_handle_command("PREPARE my_stmt AS ");
    ASSERT_TRUE(r.has_value());
    ASSERT_FALSE(r->has_value());
    EXPECT_EQ(r->error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_149, PrepareOverwritesExisting) {
    Session s(1);
    (void)s.try_handle_command("PREPARE my_stmt AS SELECT 1");
    auto* stmt1 = s.get_prepared_statement("my_stmt");
    ASSERT_NE(stmt1, nullptr);
    EXPECT_EQ(stmt1->sql, "SELECT 1");

    (void)s.try_handle_command("PREPARE my_stmt AS SELECT 2");
    auto* stmt2 = s.get_prepared_statement("my_stmt");
    ASSERT_NE(stmt2, nullptr);
    EXPECT_EQ(stmt2->sql, "SELECT 2");
}

TEST(QA_GDB_149, PrepareWithTabAfterAs) {
    Session s(1);
    auto r = s.try_handle_command("PREPARE my_stmt AS\tSELECT 1");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    const auto* stmt = s.get_prepared_statement("my_stmt");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(stmt->sql, "SELECT 1");
}

TEST(QA_GDB_149, PrepareWithNewlineAfterAs) {
    Session s(1);
    auto r = s.try_handle_command("PREPARE my_stmt AS\nSELECT 1");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    const auto* stmt = s.get_prepared_statement("my_stmt");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(stmt->sql, "SELECT 1");
}

TEST(QA_GDB_149, PrepareAsCaseInsensitive) {
    Session s(1);
    auto r = s.try_handle_command("PREPARE my_stmt as SELECT 1");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    ASSERT_NE(s.get_prepared_statement("my_stmt"), nullptr);
}

TEST(QA_GDB_149, PrepareMultipleDistinctStatements) {
    Session s(1);
    for (int i = 0; i < 20; ++i) {
        std::string name = "stmt_" + std::to_string(i);
        std::string sql = "PREPARE " + name + " AS SELECT " + std::to_string(i);
        auto r = s.try_handle_command(sql);
        ASSERT_TRUE(r.has_value()) << "Failed for " << name;
        ASSERT_TRUE(r->has_value()) << "Error for " << name;
    }
    EXPECT_EQ(s.prepared_statements().size(), 20u);
    for (int i = 0; i < 20; ++i) {
        std::string name = "stmt_" + std::to_string(i);
        auto* stmt = s.get_prepared_statement(name);
        ASSERT_NE(stmt, nullptr) << name << " not found";
        EXPECT_EQ(stmt->sql, "SELECT " + std::to_string(i));
    }
}

// -- DEALLOCATE command edge cases --------------------------------------------

TEST(QA_GDB_149, DeallocateEmpty) {
    Session s(1);
    // "DEALLOCATE " trims to "DEALLOCATE" which doesn't match "DEALLOCATE " prefix.
    // Passes through to query executor.
    auto r = s.try_handle_command("DEALLOCATE ");
    EXPECT_FALSE(r.has_value());
}

TEST(QA_GDB_149, DeallocateAllCaseInsensitive) {
    Session s(1);
    (void)s.try_handle_command("PREPARE s1 AS SELECT 1");
    auto r = s.try_handle_command("DEALLOCATE all");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_TRUE(s.prepared_statements().empty());
}

TEST(QA_GDB_149, DeallocateAllWhenEmpty) {
    Session s(1);
    // No statements prepared — DEALLOCATE ALL should still succeed.
    auto r = s.try_handle_command("DEALLOCATE ALL");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(r->value().message, "DEALLOCATE ALL");
}

TEST(QA_GDB_149, DeallocateNonexistent) {
    Session s(1);
    auto r = s.try_handle_command("DEALLOCATE ghost");
    ASSERT_TRUE(r.has_value());
    ASSERT_FALSE(r->has_value());
    EXPECT_EQ(r->error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB_149, DeallocateThenReuse) {
    Session s(1);
    (void)s.try_handle_command("PREPARE reused AS SELECT 1");
    (void)s.try_handle_command("DEALLOCATE reused");
    EXPECT_EQ(s.get_prepared_statement("reused"), nullptr);

    // Re-prepare with same name.
    (void)s.try_handle_command("PREPARE reused AS SELECT 2");
    auto* stmt = s.get_prepared_statement("reused");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(stmt->sql, "SELECT 2");
}

// -- Transaction state machine edge cases -------------------------------------

TEST(QA_GDB_149, DoubleBeginStaysInTransaction) {
    Session s(1);
    s.update_transaction_state("BEGIN", true);
    EXPECT_EQ(s.transaction_state(), TransactionState::IN_TRANSACTION);

    // Second BEGIN (success) stays IN_TRANSACTION.
    s.update_transaction_state("BEGIN", true);
    EXPECT_EQ(s.transaction_state(), TransactionState::IN_TRANSACTION);
}

TEST(QA_GDB_149, FailedBeginDoesNotEnterTransaction) {
    Session s(1);
    s.update_transaction_state("BEGIN", false);
    EXPECT_EQ(s.transaction_state(), TransactionState::IDLE);
}

TEST(QA_GDB_149, FailedCommitDoesNotResetToIdle) {
    Session s(1);
    s.update_transaction_state("BEGIN", true);
    EXPECT_EQ(s.transaction_state(), TransactionState::IN_TRANSACTION);

    // Commit fails — should stay IN_TRANSACTION (not go to IDLE).
    s.update_transaction_state("COMMIT", false);
    EXPECT_EQ(s.transaction_state(), TransactionState::IN_TRANSACTION);
}

TEST(QA_GDB_149, RollbackAlwaysResetsRegardlessOfSuccess) {
    Session s(1);
    s.update_transaction_state("BEGIN", true);

    // Rollback with success=false still resets to IDLE.
    s.update_transaction_state("ROLLBACK", false);
    EXPECT_EQ(s.transaction_state(), TransactionState::IDLE);
}

TEST(QA_GDB_149, RollbackFromFailedState) {
    Session s(1);
    s.update_transaction_state("BEGIN", true);
    s.update_transaction_state("bad sql", false);
    EXPECT_EQ(s.transaction_state(), TransactionState::FAILED);

    s.update_transaction_state("ROLLBACK", true);
    EXPECT_EQ(s.transaction_state(), TransactionState::IDLE);
}

TEST(QA_GDB_149, StartTransactionAlternate) {
    Session s(1);
    s.update_transaction_state("START TRANSACTION", true);
    EXPECT_EQ(s.transaction_state(), TransactionState::IN_TRANSACTION);
    EXPECT_EQ(s.ready_for_query_status(), 'T');
}

TEST(QA_GDB_149, EndAlternateForCommit) {
    Session s(1);
    s.update_transaction_state("BEGIN", true);
    s.update_transaction_state("END", true);
    EXPECT_EQ(s.transaction_state(), TransactionState::IDLE);
}

TEST(QA_GDB_149, AbortAlternateForRollback) {
    Session s(1);
    s.update_transaction_state("BEGIN", true);
    s.update_transaction_state("ABORT", true);
    EXPECT_EQ(s.transaction_state(), TransactionState::IDLE);
}

TEST(QA_GDB_149, AbortFromFailedState) {
    Session s(1);
    s.update_transaction_state("BEGIN", true);
    s.update_transaction_state("broken", false);
    EXPECT_EQ(s.transaction_state(), TransactionState::FAILED);

    s.update_transaction_state("ABORT", true);
    EXPECT_EQ(s.transaction_state(), TransactionState::IDLE);
}

TEST(QA_GDB_149, MultipleFailuresStayFailed) {
    Session s(1);
    s.update_transaction_state("BEGIN", true);
    s.update_transaction_state("bad1", false);
    EXPECT_EQ(s.transaction_state(), TransactionState::FAILED);

    // Another failure while FAILED — should stay FAILED (not revert to IDLE).
    s.update_transaction_state("bad2", false);
    EXPECT_EQ(s.transaction_state(), TransactionState::FAILED);
}

TEST(QA_GDB_149, ErrorOutsideTransactionStaysIdle) {
    Session s(1);
    s.update_transaction_state("broken sql", false);
    EXPECT_EQ(s.transaction_state(), TransactionState::IDLE);
}

TEST(QA_GDB_149, SuccessOutsideTransactionStaysIdle) {
    Session s(1);
    s.update_transaction_state("SELECT 1", true);
    EXPECT_EQ(s.transaction_state(), TransactionState::IDLE);
}

TEST(QA_GDB_149, TransactionStateCaseInsensitive) {
    Session s(1);
    // update_transaction_state lowercases internally.
    s.update_transaction_state("BEGIN", true);
    EXPECT_EQ(s.transaction_state(), TransactionState::IN_TRANSACTION);
    s.update_transaction_state("COMMIT", true);
    EXPECT_EQ(s.transaction_state(), TransactionState::IDLE);

    s.update_transaction_state("begin", true);
    EXPECT_EQ(s.transaction_state(), TransactionState::IN_TRANSACTION);
    s.update_transaction_state("commit", true);
    EXPECT_EQ(s.transaction_state(), TransactionState::IDLE);
}

TEST(QA_GDB_149, ReadyForQueryStatusMapping) {
    Session s(1);

    EXPECT_EQ(s.ready_for_query_status(), 'I');

    s.set_transaction_state(TransactionState::IN_TRANSACTION);
    EXPECT_EQ(s.ready_for_query_status(), 'T');

    s.set_transaction_state(TransactionState::FAILED);
    EXPECT_EQ(s.ready_for_query_status(), 'E');

    s.set_transaction_state(TransactionState::IDLE);
    EXPECT_EQ(s.ready_for_query_status(), 'I');
}

TEST(QA_GDB_149, CommitFromIdleStaysIdle) {
    Session s(1);
    // COMMIT when already IDLE (success) — stays IDLE.
    s.update_transaction_state("COMMIT", true);
    EXPECT_EQ(s.transaction_state(), TransactionState::IDLE);
}

TEST(QA_GDB_149, RollbackFromIdleStaysIdle) {
    Session s(1);
    s.update_transaction_state("ROLLBACK", true);
    EXPECT_EQ(s.transaction_state(), TransactionState::IDLE);
}

// -- Cleanup edge cases -------------------------------------------------------

TEST(QA_GDB_149, CleanupFromCleanSession) {
    Session s(1);
    // Cleanup on a fresh session should be safe.
    s.cleanup();
    EXPECT_TRUE(s.prepared_statements().empty());
    EXPECT_TRUE(s.portals().empty());
    EXPECT_EQ(s.transaction_state(), TransactionState::IDLE);
    EXPECT_EQ(*s.get_variable("work_mem"), "4MB");
}

TEST(QA_GDB_149, CleanupFromFailedTransaction) {
    Session s(1);
    s.update_transaction_state("BEGIN", true);
    s.update_transaction_state("bad", false);
    EXPECT_EQ(s.transaction_state(), TransactionState::FAILED);

    s.cleanup();
    EXPECT_EQ(s.transaction_state(), TransactionState::IDLE);
}

TEST(QA_GDB_149, DoubleCleanup) {
    Session s(1);
    (void)s.set_variable("work_mem", "16MB");

    PreparedStatement stmt;
    stmt.name = "s1";
    stmt.sql = "SELECT 1";
    s.add_prepared_statement("s1", std::move(stmt));

    Portal portal;
    portal.name = "p1";
    portal.sql = "SELECT 1";
    s.add_portal("p1", std::move(portal));

    s.cleanup();
    s.cleanup(); // Second cleanup should be safe.

    EXPECT_TRUE(s.prepared_statements().empty());
    EXPECT_TRUE(s.portals().empty());
    EXPECT_EQ(s.transaction_state(), TransactionState::IDLE);
    EXPECT_EQ(*s.get_variable("work_mem"), "4MB");
}

TEST(QA_GDB_149, CleanupResetsAllVarsToDefault) {
    Session s(1);
    (void)s.set_variable("work_mem", "X");
    (void)s.set_variable("statement_timeout", "X");
    (void)s.set_variable("search_path", "X");
    (void)s.set_variable("default_transaction_isolation", "X");
    (void)s.set_variable("embedding_provider_url", "X");
    (void)s.set_variable("embedding_api_key", "X");

    s.cleanup();

    EXPECT_EQ(*s.get_variable("work_mem"), "4MB");
    EXPECT_EQ(*s.get_variable("statement_timeout"), "0");
    EXPECT_EQ(*s.get_variable("search_path"), "public");
    EXPECT_EQ(*s.get_variable("default_transaction_isolation"), "read committed");
    EXPECT_EQ(*s.get_variable("embedding_provider_url"), "");
    EXPECT_EQ(*s.get_variable("embedding_api_key"), "");
}

// -- Portal edge cases --------------------------------------------------------

TEST(QA_GDB_149, GetNonexistentPortal) {
    Session s(1);
    EXPECT_EQ(s.get_portal("ghost"), nullptr);
}

TEST(QA_GDB_149, RemoveNonexistentPortalSafe) {
    Session s(1);
    // Removing a nonexistent portal should be a no-op (no crash).
    s.remove_portal("ghost");
    EXPECT_EQ(s.get_portal("ghost"), nullptr);
}

TEST(QA_GDB_149, OverwritePortal) {
    Session s(1);

    Portal p1;
    p1.name = "p";
    p1.sql = "SELECT 1";
    s.add_portal("p", std::move(p1));

    Portal p2;
    p2.name = "p";
    p2.sql = "SELECT 2";
    s.add_portal("p", std::move(p2));

    auto* found = s.get_portal("p");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->sql, "SELECT 2");
}

TEST(QA_GDB_149, CleanupClearsPortals) {
    Session s(1);

    Portal p;
    p.name = "p1";
    p.sql = "SELECT 1";
    s.add_portal("p1", std::move(p));

    EXPECT_NE(s.get_portal("p1"), nullptr);
    s.cleanup();
    EXPECT_EQ(s.get_portal("p1"), nullptr);
    EXPECT_TRUE(s.portals().empty());
}

// -- Prepared statement edge cases --------------------------------------------

TEST(QA_GDB_149, GetNonexistentPreparedStatement) {
    Session s(1);
    EXPECT_EQ(s.get_prepared_statement("ghost"), nullptr);
}

TEST(QA_GDB_149, RemoveNonexistentPreparedStatementSafe) {
    Session s(1);
    // Removing a nonexistent statement should be a no-op.
    s.remove_prepared_statement("ghost");
    EXPECT_TRUE(s.prepared_statements().empty());
}

TEST(QA_GDB_149, RemoveAllPreparedWhenEmpty) {
    Session s(1);
    s.remove_all_prepared_statements();
    EXPECT_TRUE(s.prepared_statements().empty());
}

// -- Command routing ----------------------------------------------------------

TEST(QA_GDB_149, EmptyCommandReturnsNullopt) {
    Session s(1);
    EXPECT_FALSE(s.try_handle_command("").has_value());
}

TEST(QA_GDB_149, WhitespaceOnlyCommandReturnsNullopt) {
    Session s(1);
    EXPECT_FALSE(s.try_handle_command("   ").has_value());
    EXPECT_FALSE(s.try_handle_command("\t\n").has_value());
}

TEST(QA_GDB_149, SelectReturnsNullopt) {
    Session s(1);
    EXPECT_FALSE(s.try_handle_command("SELECT 1").has_value());
}

TEST(QA_GDB_149, InsertReturnsNullopt) {
    Session s(1);
    EXPECT_FALSE(s.try_handle_command("INSERT INTO t VALUES (1)").has_value());
}

TEST(QA_GDB_149, CreateTableReturnsNullopt) {
    Session s(1);
    EXPECT_FALSE(s.try_handle_command("CREATE TABLE t (id INT)").has_value());
}

TEST(QA_GDB_149, BeginReturnsNullopt) {
    Session s(1);
    // BEGIN is handled by the query executor, not session.
    EXPECT_FALSE(s.try_handle_command("BEGIN").has_value());
}

TEST(QA_GDB_149, CommitReturnsNullopt) {
    Session s(1);
    EXPECT_FALSE(s.try_handle_command("COMMIT").has_value());
}

TEST(QA_GDB_149, RollbackReturnsNullopt) {
    Session s(1);
    EXPECT_FALSE(s.try_handle_command("ROLLBACK").has_value());
}

// -- Backend PID accessor -----------------------------------------------------

TEST(QA_GDB_149, BackendPidAccessor) {
    Session s(42);
    EXPECT_EQ(s.backend_pid(), 42);

    Session s2(-1);
    EXPECT_EQ(s2.backend_pid(), -1);

    Session s3(0);
    EXPECT_EQ(s3.backend_pid(), 0);
}

// -- Session isolation stress -------------------------------------------------

TEST(QA_GDB_149, ManySessions100Independent) {
    std::vector<Session> sessions;
    sessions.reserve(100);
    for (int i = 0; i < 100; ++i) {
        sessions.emplace_back(i);
    }

    // Set a unique value on each session.
    for (int i = 0; i < 100; ++i) {
        auto r =
            sessions[static_cast<size_t>(i)].set_variable("work_mem", std::to_string(i) + "MB");
        ASSERT_TRUE(r.has_value());
    }

    // Verify each session has its own value.
    for (int i = 0; i < 100; ++i) {
        auto val = sessions[static_cast<size_t>(i)].get_variable("work_mem");
        ASSERT_TRUE(val.has_value());
        EXPECT_EQ(*val, std::to_string(i) + "MB");
    }
}
