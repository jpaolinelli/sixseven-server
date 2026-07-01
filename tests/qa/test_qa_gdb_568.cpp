/// @file test_qa_gdb_568.cpp
/// @brief Adversarial QA tests for GDB-568: SAVEPOINT Support for psqlODBC Compatibility
///
/// GDB-883 update: all execution-path assertions flipped from expect-success
/// to expect-NOT_IMPLEMENTED, reflecting the fix for audit finding C5.
/// Parser tests are unchanged.

#include "sixseven/executor/query_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/server/session.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static StmtPtr parse_one(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return nullptr;
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    if (!stmts || stmts->empty())
        return nullptr;
    return std::move((*stmts)[0]);
}

static bool parse_succeeds(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return false;
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    return stmts.has_value() && !stmts->empty();
}

static bool parse_fails(std::string_view sql) {
    return !parse_succeeds(sql);
}

// ---------------------------------------------------------------------------
// Fixture: Session in IN_TRANSACTION state
// ---------------------------------------------------------------------------

class QA_GDB568_Session : public ::testing::Test {
protected:
    void SetUp() override {
        session_ = std::make_unique<Session>(100);
        session_->update_transaction_state("BEGIN", true);
        ASSERT_EQ(session_->transaction_state(), TransactionState::IN_TRANSACTION);
    }

    void enter_failed_state() {
        session_->update_transaction_state("SELECT bad_query", false);
        ASSERT_EQ(session_->transaction_state(), TransactionState::FAILED);
    }

    std::unique_ptr<Session> session_;
};

// ===========================================================================
// AC1: SAVEPOINT <name> -- parser still works; execution returns NOT_IMPLEMENTED
// ===========================================================================

TEST_F(QA_GDB568_Session, AC1_SavepointReturnsNotImplemented) {
    // GDB-883: execution must not fake success.
    auto result = session_->try_handle_command("SAVEPOINT my_sp");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, StatusCode::NOT_IMPLEMENTED);
    // Deque must remain empty -- no mutation on failure.
    EXPECT_TRUE(session_->savepoints().empty());
}

TEST_F(QA_GDB568_Session, AC1_SavepointParserProducesCorrectAST) {
    auto stmt = parse_one("SAVEPOINT test_sp");
    auto* sp = dynamic_cast<SavepointStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->name, "test_sp");
}

// ===========================================================================
// AC2: RELEASE SAVEPOINT <name> -- parser still works; execution returns NOT_IMPLEMENTED
// ===========================================================================

TEST_F(QA_GDB568_Session, AC2_ReleaseSavepointReturnsNotImplemented) {
    auto result = session_->try_handle_command("RELEASE SAVEPOINT sp1");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB568_Session, AC2_ReleaseSavepointParserProducesCorrectAST) {
    auto stmt = parse_one("RELEASE SAVEPOINT test_sp");
    auto* rs = dynamic_cast<ReleaseSavepointStmt*>(stmt.get());
    ASSERT_NE(rs, nullptr);
    EXPECT_EQ(rs->name, "test_sp");
}

// ===========================================================================
// AC3: ROLLBACK TO SAVEPOINT <name> -- returns NOT_IMPLEMENTED
// ===========================================================================

TEST_F(QA_GDB568_Session, AC3_RollbackToSavepointReturnsNotImplemented) {
    enter_failed_state();
    auto result = session_->try_handle_command("ROLLBACK TO SAVEPOINT sp1");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, StatusCode::NOT_IMPLEMENTED);
    // State must NOT be silently recovered (GDB-883 core fix).
    EXPECT_EQ(session_->transaction_state(), TransactionState::FAILED);
}

TEST_F(QA_GDB568_Session, AC3_RollbackToWithoutSavepointKeywordReturnsNotImplemented) {
    enter_failed_state();
    auto result = session_->try_handle_command("ROLLBACK TO sp1");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB568_Session, AC3_RollbackToParserBothForms) {
    {
        auto stmt = parse_one("ROLLBACK TO sp1");
        auto* rb = dynamic_cast<RollbackStmt*>(stmt.get());
        ASSERT_NE(rb, nullptr);
        EXPECT_EQ(rb->savepoint, "sp1");
    }
    {
        auto stmt = parse_one("ROLLBACK TO SAVEPOINT sp1");
        auto* rb = dynamic_cast<RollbackStmt*>(stmt.get());
        ASSERT_NE(rb, nullptr);
        EXPECT_EQ(rb->savepoint, "sp1");
    }
}

// ===========================================================================
// AC4: Savepoints outside a transaction -- still return NOT_IMPLEMENTED
// ===========================================================================

TEST_F(QA_GDB568_Session, AC4_SavepointOutsideTransactionReturnsNotImplemented) {
    Session idle_session(200);
    ASSERT_EQ(idle_session.transaction_state(), TransactionState::IDLE);

    auto result = idle_session.create_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB568_Session, AC4_ReleaseSavepointOutsideTransactionReturnsNotImplemented) {
    Session idle_session(200);
    auto result = idle_session.release_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB568_Session, AC4_RollbackToSavepointOutsideTransactionReturnsNotImplemented) {
    Session idle_session(200);
    auto result = idle_session.rollback_to_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB568_Session, AC4_SavepointViaCommandOutsideTransactionReturnsNotImplemented) {
    Session idle_session(200);
    auto result = idle_session.try_handle_command("SAVEPOINT sp1");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, StatusCode::NOT_IMPLEMENTED);
}

// ===========================================================================
// AC5: Nested savepoints -- all return NOT_IMPLEMENTED
// ===========================================================================

TEST_F(QA_GDB568_Session, AC5_NestedSavepointsAllReturnNotImplemented) {
    auto r1 = session_->create_savepoint("a");
    ASSERT_FALSE(r1.has_value());
    EXPECT_EQ(r1.error().code, StatusCode::NOT_IMPLEMENTED);

    auto r2 = session_->create_savepoint("b");
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, StatusCode::NOT_IMPLEMENTED);

    auto r3 = session_->rollback_to_savepoint("a");
    ASSERT_FALSE(r3.has_value());
    EXPECT_EQ(r3.error().code, StatusCode::NOT_IMPLEMENTED);

    // Deque always empty.
    EXPECT_TRUE(session_->savepoints().empty());
}

// ===========================================================================
// Adversarial: Empty/weird savepoint names
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_EmptySavepointNameViaAPI) {
    // GDB-883: returns NOT_IMPLEMENTED regardless of name content.
    auto result = session_->create_savepoint("");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB568_Session, Adversarial_EmptySavepointNameViaCommand) {
    // "SAVEPOINT " trimmed to "SAVEPOINT" -- does not match "SAVEPOINT " prefix.
    auto result = session_->try_handle_command("SAVEPOINT ");
    EXPECT_FALSE(result.has_value());
}

TEST_F(QA_GDB568_Session, Adversarial_VeryLongSavepointName) {
    std::string long_name(10000, 'x');
    auto result = session_->create_savepoint(long_name);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB568_Session, Adversarial_SavepointNameWithSpaces) {
    auto result = session_->create_savepoint("sp with spaces");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB568_Session, Adversarial_SavepointNameWithSpecialChars) {
    auto result = session_->create_savepoint("sp!@#$%^&*()");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

// ===========================================================================
// Adversarial: Non-existent savepoint operations
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_ReleaseNonExistentSavepoint) {
    auto result = session_->release_savepoint("does_not_exist");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB568_Session, Adversarial_RollbackToNonExistentSavepoint) {
    auto result = session_->rollback_to_savepoint("does_not_exist");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB568_Session, Adversarial_RollbackToNonExistentInFailedState) {
    enter_failed_state();
    auto result = session_->rollback_to_savepoint("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
    // State unchanged.
    EXPECT_EQ(session_->transaction_state(), TransactionState::FAILED);
}

// ===========================================================================
// Adversarial: Released savepoint operations
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_ReleaseAlreadyReleasedSavepoint) {
    // Both calls return NOT_IMPLEMENTED.
    auto r1 = session_->release_savepoint("sp1");
    ASSERT_FALSE(r1.has_value());
    EXPECT_EQ(r1.error().code, StatusCode::NOT_IMPLEMENTED);

    auto r2 = session_->release_savepoint("sp1");
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB568_Session, Adversarial_RollbackToReleasedSavepoint) {
    auto r1 = session_->release_savepoint("sp1");
    ASSERT_FALSE(r1.has_value());
    EXPECT_EQ(r1.error().code, StatusCode::NOT_IMPLEMENTED);

    auto r2 = session_->rollback_to_savepoint("sp1");
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, StatusCode::NOT_IMPLEMENTED);
}

// ===========================================================================
// Adversarial: Duplicate savepoint names
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_DuplicateSavepointNamesCreate) {
    // All return NOT_IMPLEMENTED; deque stays empty.
    (void)session_->create_savepoint("dup");
    (void)session_->create_savepoint("dup");
    (void)session_->create_savepoint("dup");
    EXPECT_TRUE(session_->savepoints().empty());
}

TEST_F(QA_GDB568_Session, Adversarial_DuplicateNamesReleaseReturnsNotImplemented) {
    auto result = session_->release_savepoint("dup");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB568_Session, Adversarial_DuplicateNamesRollbackToReturnsNotImplemented) {
    auto result = session_->rollback_to_savepoint("dup");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

// ===========================================================================
// Adversarial: Stack depth stress
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_ManySavepoints) {
    const size_t count = 1000;
    for (size_t i = 0; i < count; ++i) {
        auto r = session_->create_savepoint("sp_" + std::to_string(i));
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code, StatusCode::NOT_IMPLEMENTED);
    }
    // Deque always empty.
    EXPECT_TRUE(session_->savepoints().empty());
}

TEST_F(QA_GDB568_Session, Adversarial_ManySavepointsReleaseAll) {
    const size_t count = 500;
    for (size_t i = 0; i < count; ++i) {
        auto r = session_->release_savepoint("sp_" + std::to_string(i));
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code, StatusCode::NOT_IMPLEMENTED);
    }
    EXPECT_TRUE(session_->savepoints().empty());
}

// ===========================================================================
// Adversarial: Savepoint operations in FAILED state
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_CreateSavepointInFailedState) {
    enter_failed_state();
    auto result = session_->create_savepoint("sp_after_fail");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB568_Session, Adversarial_ReleaseSavepointInFailedState) {
    enter_failed_state();
    auto result = session_->release_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB568_Session, Adversarial_SavepointCommandInFailedState) {
    enter_failed_state();
    auto result = session_->try_handle_command("SAVEPOINT sp_fail");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB568_Session, Adversarial_ReleaseCommandInFailedState) {
    enter_failed_state();
    auto result = session_->try_handle_command("RELEASE SAVEPOINT sp1");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, StatusCode::NOT_IMPLEMENTED);
}

// ===========================================================================
// Adversarial: COMMIT/ROLLBACK still work; savepoint deque always empty
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_CommitTransitionsToIdle) {
    session_->update_transaction_state("COMMIT", true);
    EXPECT_TRUE(session_->savepoints().empty());
    EXPECT_EQ(session_->transaction_state(), TransactionState::IDLE);
}

TEST_F(QA_GDB568_Session, Adversarial_RollbackTransitionsToIdle) {
    session_->update_transaction_state("ROLLBACK", true);
    EXPECT_TRUE(session_->savepoints().empty());
    EXPECT_EQ(session_->transaction_state(), TransactionState::IDLE);
}

TEST_F(QA_GDB568_Session, Adversarial_AbortTransitionsToIdle) {
    session_->update_transaction_state("ABORT", true);
    EXPECT_TRUE(session_->savepoints().empty());
    EXPECT_EQ(session_->transaction_state(), TransactionState::IDLE);
}

TEST_F(QA_GDB568_Session, Adversarial_EndTransitionsToIdle) {
    session_->update_transaction_state("END", true);
    EXPECT_TRUE(session_->savepoints().empty());
    EXPECT_EQ(session_->transaction_state(), TransactionState::IDLE);
}

// ===========================================================================
// Adversarial: BEGIN/COMMIT/ROLLBACK sequences with savepoints
// GDB-883: workflow tests now assert NOT_IMPLEMENTED at each savepoint step.
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_FullPsqlODBCWorkflow_ReturnsNotImplemented) {
    // psqlODBC pattern: each savepoint step now returns NOT_IMPLEMENTED.
    auto sp1 = session_->try_handle_command("SAVEPOINT odbc_sp1");
    ASSERT_TRUE(sp1.has_value());
    ASSERT_FALSE(sp1->has_value());
    EXPECT_EQ(sp1->error().code, StatusCode::NOT_IMPLEMENTED);

    session_->update_transaction_state("SELECT * FROM nonexistent_table", false);
    EXPECT_EQ(session_->transaction_state(), TransactionState::FAILED);

    // ROLLBACK TO returns NOT_IMPLEMENTED -- state does NOT recover.
    auto rb1 = session_->try_handle_command("ROLLBACK TO SAVEPOINT odbc_sp1");
    ASSERT_TRUE(rb1.has_value());
    ASSERT_FALSE(rb1->has_value());
    EXPECT_EQ(rb1->error().code, StatusCode::NOT_IMPLEMENTED);
    EXPECT_EQ(session_->transaction_state(), TransactionState::FAILED);
}

TEST_F(QA_GDB568_Session, Adversarial_MultipleFailAndRecoverCycles_AllNotImplemented) {
    for (int i = 0; i < 10; ++i) {
        std::string sp_name = "cycle_" + std::to_string(i);
        auto sp = session_->try_handle_command("SAVEPOINT " + sp_name);
        ASSERT_TRUE(sp.has_value());
        ASSERT_FALSE(sp->has_value()) << "Cycle " << i;
        EXPECT_EQ(sp->error().code, StatusCode::NOT_IMPLEMENTED);
    }
}

// ===========================================================================
// Adversarial: Case sensitivity of commands
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_CaseInsensitiveCommandKeywords) {
    auto r1 = session_->try_handle_command("savepoint sp1");
    ASSERT_TRUE(r1.has_value());
    ASSERT_FALSE(r1->has_value());
    EXPECT_EQ(r1->error().code, StatusCode::NOT_IMPLEMENTED);

    auto r2 = session_->try_handle_command("release savepoint sp1");
    ASSERT_TRUE(r2.has_value());
    ASSERT_FALSE(r2->has_value());
    EXPECT_EQ(r2->error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB568_Session, Adversarial_MixedCaseCommandKeywords) {
    auto r1 = session_->try_handle_command("Savepoint SP1");
    ASSERT_TRUE(r1.has_value());
    ASSERT_FALSE(r1->has_value());
    EXPECT_EQ(r1->error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB568_Session, Adversarial_RollbackToCaseInsensitive) {
    auto r = session_->try_handle_command("rollback to sp1");
    ASSERT_TRUE(r.has_value());
    ASSERT_FALSE(r->has_value());
    EXPECT_EQ(r->error().code, StatusCode::NOT_IMPLEMENTED);
}

// ===========================================================================
// Adversarial: Whitespace handling
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_LeadingTrailingWhitespace) {
    auto r = session_->try_handle_command("  SAVEPOINT sp1  ");
    ASSERT_TRUE(r.has_value());
    ASSERT_FALSE(r->has_value());
    EXPECT_EQ(r->error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB568_Session, Adversarial_MultipleSpacesBetweenKeywords) {
    auto r2 = session_->try_handle_command("ROLLBACK TO    SAVEPOINT    sp1");
    ASSERT_TRUE(r2.has_value());
    ASSERT_FALSE(r2->has_value());
    EXPECT_EQ(r2->error().code, StatusCode::NOT_IMPLEMENTED);
}

// ===========================================================================
// Adversarial: Cleanup clears savepoints (always empty, still transitions)
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_CleanupClearsSavepoints) {
    session_->cleanup();
    EXPECT_TRUE(session_->savepoints().empty());
    EXPECT_EQ(session_->transaction_state(), TransactionState::IDLE);
}

// ===========================================================================
// Adversarial: Parser edge cases (unchanged)
// ===========================================================================

TEST(QA_GDB568_Parser, SavepointWithoutName) {
    EXPECT_TRUE(parse_fails("SAVEPOINT"));
}

TEST(QA_GDB568_Parser, ReleaseSavepointWithoutName) {
    EXPECT_TRUE(parse_fails("RELEASE SAVEPOINT"));
}

TEST(QA_GDB568_Parser, ReleaseWithoutSavepointKeyword) {
    EXPECT_TRUE(parse_fails("RELEASE sp1"));
}

TEST(QA_GDB568_Parser, RollbackToWithoutName) {
    EXPECT_TRUE(parse_fails("ROLLBACK TO"));
}

TEST(QA_GDB568_Parser, RollbackToSavepointWithoutName) {
    EXPECT_TRUE(parse_fails("ROLLBACK TO SAVEPOINT"));
}

TEST(QA_GDB568_Parser, PlainRollbackHasNoSavepoint) {
    auto stmt = parse_one("ROLLBACK");
    auto* rb = dynamic_cast<RollbackStmt*>(stmt.get());
    ASSERT_NE(rb, nullptr);
    EXPECT_TRUE(rb->savepoint.empty());
}

TEST(QA_GDB568_Parser, SavepointNamePreservesCase) {
    auto stmt = parse_one("SAVEPOINT MyMixedCaseName");
    auto* sp = dynamic_cast<SavepointStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    // Parser::parse_name returns the raw lexeme verbatim, so a mixed-case
    // savepoint name must be preserved exactly. A regression that lowercased
    // (or otherwise mangled) the name would break ROLLBACK TO / RELEASE
    // matching for names created through the parser path, yet still pass a
    // mere non-empty check.
    EXPECT_EQ(sp->name, "MyMixedCaseName");
}

TEST(QA_GDB568_Parser, SavepointNameNumeric) {
    auto stmt = parse_one("SAVEPOINT sp123");
    if (stmt) {
        auto* sp = dynamic_cast<SavepointStmt*>(stmt.get());
        ASSERT_NE(sp, nullptr);
        EXPECT_EQ(sp->name, "sp123");
    }
}

// ===========================================================================
// Adversarial: Rollback to savepoint -- NOT_IMPLEMENTED, deque untouched
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_RollbackPreservesNoSavepoints) {
    auto result = session_->rollback_to_savepoint("b");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
    EXPECT_TRUE(session_->savepoints().empty());
}

// ===========================================================================
// Adversarial: Rollback TO in FAILED state -- NOT_IMPLEMENTED, state unchanged
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_FailedRollbackToNonExistentKeepsFailedState) {
    enter_failed_state();

    auto result = session_->rollback_to_savepoint("wrong_name");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
    EXPECT_EQ(session_->transaction_state(), TransactionState::FAILED);
}

// ===========================================================================
// Adversarial: Savepoint after failed state -- still NOT_IMPLEMENTED
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_CreateSavepointAfterFailedState) {
    enter_failed_state();
    auto sp2 = session_->create_savepoint("sp2");
    ASSERT_FALSE(sp2.has_value());
    EXPECT_EQ(sp2.error().code, StatusCode::NOT_IMPLEMENTED);
}

// ===========================================================================
// Regression: No regressions (AC7) -- verify plain ROLLBACK/COMMIT still work
// ===========================================================================

TEST_F(QA_GDB568_Session, Regression_PlainRollbackStillWorks) {
    // "ROLLBACK" (without TO) is not intercepted by try_handle_command.
    auto result = session_->try_handle_command("ROLLBACK");
    EXPECT_FALSE(result.has_value());
}

TEST_F(QA_GDB568_Session, Regression_BeginCommitNotIntercepted) {
    auto r1 = session_->try_handle_command("BEGIN");
    EXPECT_FALSE(r1.has_value());

    auto r2 = session_->try_handle_command("COMMIT");
    EXPECT_FALSE(r2.has_value());
}
