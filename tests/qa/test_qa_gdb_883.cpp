/// @file test_qa_gdb_883.cpp
/// @brief Regression tests for GDB-883: Wire-protocol SAVEPOINT / ROLLBACK TO
///        SAVEPOINT are name-tracking no-ops that report success.
///
/// Audit finding C5 (severity CRITICAL, verified 2026-06-10):
/// The original create_savepoint / release_savepoint / rollback_to_savepoint
/// manipulated an in-memory deque and returned ok(), with NO engine call and
/// NO data rollback.  A client doing:
///
///   BEGIN; SAVEPOINT s; UPDATE t SET x=1; ROLLBACK TO s; COMMIT;
///
/// would receive fake success from ROLLBACK TO s, then COMMIT would silently
/// persist the update -- a critical data-integrity violation.
///
/// Fix (GDB-883, option A): all three execution handlers return NOT_IMPLEMENTED
/// so clients receive an honest error instead of silent data loss.

#include "sixseven/executor/query_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/server/session.h"

#include <gtest/gtest.h>

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

// ---------------------------------------------------------------------------
// Fixture: Session inside an active transaction
// ---------------------------------------------------------------------------

class QA_GDB883 : public ::testing::Test {
protected:
    void SetUp() override {
        session_ = std::make_unique<Session>(883);
        session_->update_transaction_state("BEGIN", true);
        ASSERT_EQ(session_->transaction_state(), TransactionState::IN_TRANSACTION);
    }

    std::unique_ptr<Session> session_;
};

// ===========================================================================
// Core regression: the audit C5 scenario
//
// Before GDB-883: SAVEPOINT returned ok() and ROLLBACK TO returned ok(),
// so a subsequent COMMIT would silently persist the mutation.
//
// After GDB-883: SAVEPOINT returns NOT_IMPLEMENTED -- the silent-success
// path is closed before any mutation can be paired with a fake rollback.
// ===========================================================================

TEST_F(QA_GDB883, CoreRegression_SavepointReturnsNotImplemented) {
    // Simulate: BEGIN; SAVEPOINT s; <mutation would follow>; ...
    // SAVEPOINT must not return ok() -- that is the entry-point of the bug.
    auto sp_result = session_->create_savepoint("s");
    ASSERT_FALSE(sp_result.has_value())
        << "SAVEPOINT returned ok() -- silent-commit data-integrity bug is present";
    EXPECT_EQ(sp_result.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB883, CoreRegression_RollbackToReturnsNotImplemented) {
    // Simulate: ... ROLLBACK TO s; -- must not fake success.
    auto rb_result = session_->rollback_to_savepoint("s");
    ASSERT_FALSE(rb_result.has_value())
        << "ROLLBACK TO SAVEPOINT returned ok() -- silent-commit data-integrity bug is present";
    EXPECT_EQ(rb_result.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB883, CoreRegression_ReleaseSavepointReturnsNotImplemented) {
    auto rel_result = session_->release_savepoint("s");
    ASSERT_FALSE(rel_result.has_value())
        << "RELEASE SAVEPOINT returned ok() -- silent-commit data-integrity bug is present";
    EXPECT_EQ(rel_result.error().code, StatusCode::NOT_IMPLEMENTED);
}

// ===========================================================================
// Wire-path regression: the same error must propagate via try_handle_command
// (the path driven by the PostgreSQL wire protocol).
// ===========================================================================

TEST_F(QA_GDB883, WirePath_SavepointCommandReturnsNotImplemented) {
    auto result = session_->try_handle_command("SAVEPOINT s");
    // Command was intercepted (result has a value -- it's not nullopt).
    ASSERT_TRUE(result.has_value());
    // The inner Result must be an error.
    ASSERT_FALSE(result->has_value())
        << "try_handle_command(SAVEPOINT) returned ok() -- silent-commit bug present";
    EXPECT_EQ(result->error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB883, WirePath_RollbackToCommandReturnsNotImplemented) {
    auto result = session_->try_handle_command("ROLLBACK TO SAVEPOINT s");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value())
        << "try_handle_command(ROLLBACK TO SAVEPOINT) returned ok() -- silent-commit bug present";
    EXPECT_EQ(result->error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB883, WirePath_RollbackToWithoutKeyword_ReturnsNotImplemented) {
    auto result = session_->try_handle_command("ROLLBACK TO s");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB883, WirePath_ReleaseSavepointCommandReturnsNotImplemented) {
    auto result = session_->try_handle_command("RELEASE SAVEPOINT s");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, StatusCode::NOT_IMPLEMENTED);
}

// ===========================================================================
// State invariant: ROLLBACK TO must not silently recover a FAILED transaction.
// Before GDB-883 rollback_to_savepoint() flipped FAILED -> IN_TRANSACTION as
// a side-effect of its fake success.  After the fix that cannot happen.
// ===========================================================================

TEST_F(QA_GDB883, StateInvariant_RollbackToDoesNotRecoverFailedTransaction) {
    // Drive the transaction into FAILED state.
    session_->update_transaction_state("SELECT * FROM nonexistent", false);
    ASSERT_EQ(session_->transaction_state(), TransactionState::FAILED);

    // ROLLBACK TO must return an error, not silently recover the transaction.
    auto result = session_->rollback_to_savepoint("s");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);

    // Transaction state must remain FAILED -- no silent state mutation.
    EXPECT_EQ(session_->transaction_state(), TransactionState::FAILED)
        << "rollback_to_savepoint() silently recovered FAILED state -- bug present";
}

TEST_F(QA_GDB883, StateInvariant_WirePath_RollbackToDoesNotRecoverFailedTransaction) {
    session_->update_transaction_state("bad query", false);
    ASSERT_EQ(session_->transaction_state(), TransactionState::FAILED);

    auto result = session_->try_handle_command("ROLLBACK TO SAVEPOINT s");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, StatusCode::NOT_IMPLEMENTED);
    EXPECT_EQ(session_->transaction_state(), TransactionState::FAILED);
}

// ===========================================================================
// Deque invariant: savepoints_ must remain empty (no silent push on failure).
// ===========================================================================

TEST_F(QA_GDB883, DequeInvariant_SavepointsDequeRemainsEmpty) {
    (void)session_->create_savepoint("sp1");
    (void)session_->create_savepoint("sp2");
    EXPECT_TRUE(session_->savepoints().empty())
        << "savepoints_ was mutated despite create_savepoint returning an error";
}

// ===========================================================================
// Parser unchanged: parsing SAVEPOINT / RELEASE / ROLLBACK TO still works.
// ===========================================================================

TEST(QA_GDB883_Parser, SavepointParseStillWorks) {
    auto stmt = parse_one("SAVEPOINT my_sp");
    auto* sp = dynamic_cast<SavepointStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->name, "my_sp");
}

TEST(QA_GDB883_Parser, ReleaseSavepointParseStillWorks) {
    auto stmt = parse_one("RELEASE SAVEPOINT my_sp");
    auto* rs = dynamic_cast<ReleaseSavepointStmt*>(stmt.get());
    ASSERT_NE(rs, nullptr);
    EXPECT_EQ(rs->name, "my_sp");
}

TEST(QA_GDB883_Parser, RollbackToSavepointParseStillWorks) {
    auto stmt = parse_one("ROLLBACK TO SAVEPOINT my_sp");
    auto* rb = dynamic_cast<RollbackStmt*>(stmt.get());
    ASSERT_NE(rb, nullptr);
    EXPECT_EQ(rb->savepoint, "my_sp");
}

// ===========================================================================
// Regression: plain ROLLBACK (no savepoint) is unaffected.
// ===========================================================================

TEST_F(QA_GDB883, Regression_PlainRollbackNotIntercepted) {
    // "ROLLBACK" (no "TO") must not be handled by the savepoint path.
    auto result = session_->try_handle_command("ROLLBACK");
    EXPECT_FALSE(result.has_value())
        << "Plain ROLLBACK was incorrectly intercepted by savepoint handler";
}

TEST_F(QA_GDB883, Regression_BeginCommitUnaffected) {
    auto r1 = session_->try_handle_command("BEGIN");
    EXPECT_FALSE(r1.has_value());

    auto r2 = session_->try_handle_command("COMMIT");
    EXPECT_FALSE(r2.has_value());
}

// ===========================================================================
// Additional adversarial tests added by QA (GDB-883 audit)
// ===========================================================================

// Adversarial: SAVEPOINT must return NOT_IMPLEMENTED regardless of state.
// Idle, IN_TRANSACTION, and FAILED all must error -- no state has a "working"
// savepoint path.

TEST(QA_GDB883_Adversarial, SavepointFromIdleState_NotImplemented) {
    Session s(1);
    ASSERT_EQ(s.transaction_state(), TransactionState::IDLE);
    auto r = s.create_savepoint("sp1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST(QA_GDB883_Adversarial, ReleaseFromIdleState_NotImplemented) {
    Session s(1);
    auto r = s.release_savepoint("sp1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST(QA_GDB883_Adversarial, RollbackToFromIdleState_NotImplemented) {
    Session s(1);
    auto r = s.rollback_to_savepoint("sp1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_IMPLEMENTED);
}

// Adversarial: error messages must be non-empty and informative, not
// "not implemented" with an empty string.
TEST_F(QA_GDB883, ErrorMessages_Savepoint_NonEmpty) {
    auto r = session_->create_savepoint("sp1");
    ASSERT_FALSE(r.has_value());
    EXPECT_FALSE(r.error().message.empty()) << "create_savepoint error message must not be empty";
    EXPECT_NE(r.error().message.find("SAVEPOINT"), std::string::npos)
        << "create_savepoint error message should mention SAVEPOINT";
}

TEST_F(QA_GDB883, ErrorMessages_ReleaseSavepoint_NonEmpty) {
    auto r = session_->release_savepoint("sp1");
    ASSERT_FALSE(r.has_value());
    EXPECT_FALSE(r.error().message.empty());
}

TEST_F(QA_GDB883, ErrorMessages_RollbackTo_NonEmpty) {
    auto r = session_->rollback_to_savepoint("sp1");
    ASSERT_FALSE(r.has_value());
    EXPECT_FALSE(r.error().message.empty());
}

// Adversarial: deque must stay empty even after many successive calls.
// (No accumulation from repeated calls.)
TEST_F(QA_GDB883, DequeInvariant_RepeatedCallsNeverAccumulate) {
    for (int i = 0; i < 100; ++i) {
        (void)session_->create_savepoint("sp" + std::to_string(i));
    }
    EXPECT_TRUE(session_->savepoints().empty())
        << "savepoints_ deque grew despite all create_savepoint calls failing";
}

// Adversarial: an empty savepoint name must still return NOT_IMPLEMENTED
// (not some other error code like PARSE_ERROR or INVALID_ARGUMENT).
TEST_F(QA_GDB883, EmptyNamePassedDirectly_NotImplemented) {
    auto r1 = session_->create_savepoint("");
    ASSERT_FALSE(r1.has_value());
    EXPECT_EQ(r1.error().code, StatusCode::NOT_IMPLEMENTED);

    auto r2 = session_->release_savepoint("");
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, StatusCode::NOT_IMPLEMENTED);

    auto r3 = session_->rollback_to_savepoint("");
    ASSERT_FALSE(r3.has_value());
    EXPECT_EQ(r3.error().code, StatusCode::NOT_IMPLEMENTED);
}

// Adversarial: transaction state must not change at all after any savepoint
// command -- not IDLE->FAILED, not FAILED->IN_TRANSACTION.
TEST_F(QA_GDB883, TransactionState_UnchangedAfterSavepointError_InTransaction) {
    ASSERT_EQ(session_->transaction_state(), TransactionState::IN_TRANSACTION);
    (void)session_->create_savepoint("sp1");
    EXPECT_EQ(session_->transaction_state(), TransactionState::IN_TRANSACTION)
        << "create_savepoint error must not change transaction state";
    (void)session_->release_savepoint("sp1");
    EXPECT_EQ(session_->transaction_state(), TransactionState::IN_TRANSACTION);
    (void)session_->rollback_to_savepoint("sp1");
    EXPECT_EQ(session_->transaction_state(), TransactionState::IN_TRANSACTION);
}

TEST_F(QA_GDB883, TransactionState_UnchangedAfterSavepointError_Failed) {
    session_->update_transaction_state("bad", false);
    ASSERT_EQ(session_->transaction_state(), TransactionState::FAILED);
    (void)session_->create_savepoint("sp1");
    EXPECT_EQ(session_->transaction_state(), TransactionState::FAILED);
    (void)session_->release_savepoint("sp1");
    EXPECT_EQ(session_->transaction_state(), TransactionState::FAILED);
    (void)session_->rollback_to_savepoint("sp1");
    EXPECT_EQ(session_->transaction_state(), TransactionState::FAILED);
}

// Adversarial: plain ROLLBACK (no "TO") from FAILED state still resets to IDLE.
// This is non-savepoint behavior that must be unaffected by GDB-883.
TEST_F(QA_GDB883, PlainRollback_FromFailed_ResetsToIdle) {
    session_->update_transaction_state("bad", false);
    ASSERT_EQ(session_->transaction_state(), TransactionState::FAILED);
    session_->update_transaction_state("ROLLBACK", true);
    EXPECT_EQ(session_->transaction_state(), TransactionState::IDLE)
        << "Plain ROLLBACK from FAILED state must reset to IDLE (unaffected by GDB-883)";
}

// Adversarial: case variants of ROLLBACK TO must all be caught.
TEST_F(QA_GDB883, WirePath_RollbackToLowercase_NotImplemented) {
    auto r = session_->try_handle_command("rollback to savepoint mysp");
    ASSERT_TRUE(r.has_value());
    ASSERT_FALSE(r->has_value());
    EXPECT_EQ(r->error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB883, WirePath_SavepointMixedCase_NotImplemented) {
    auto r = session_->try_handle_command("Savepoint MySP");
    ASSERT_TRUE(r.has_value());
    ASSERT_FALSE(r->has_value());
    EXPECT_EQ(r->error().code, StatusCode::NOT_IMPLEMENTED);
}
