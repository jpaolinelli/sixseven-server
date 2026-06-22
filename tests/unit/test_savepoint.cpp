#include "sixseven/executor/query_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/server/session.h"

#include <gtest/gtest.h>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Parser tests for savepoint statements
// (unchanged — parsing still works, only execution returns NOT_IMPLEMENTED)
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

static bool parse_fails(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return true;
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    return !stmts.has_value();
}

TEST(SavepointParser, SavepointStatement) {
    auto stmt = parse_one("SAVEPOINT sp1");
    auto* sp = dynamic_cast<SavepointStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->name, "sp1");
}

TEST(SavepointParser, ReleaseSavepointStatement) {
    auto stmt = parse_one("RELEASE SAVEPOINT sp1");
    auto* rs = dynamic_cast<ReleaseSavepointStmt*>(stmt.get());
    ASSERT_NE(rs, nullptr);
    EXPECT_EQ(rs->name, "sp1");
}

TEST(SavepointParser, RollbackToSavepoint) {
    auto stmt = parse_one("ROLLBACK TO sp1");
    auto* rb = dynamic_cast<RollbackStmt*>(stmt.get());
    ASSERT_NE(rb, nullptr);
    EXPECT_EQ(rb->savepoint, "sp1");
}

TEST(SavepointParser, RollbackToSavepointWithKeyword) {
    auto stmt = parse_one("ROLLBACK TO SAVEPOINT sp1");
    auto* rb = dynamic_cast<RollbackStmt*>(stmt.get());
    ASSERT_NE(rb, nullptr);
    EXPECT_EQ(rb->savepoint, "sp1");
}

TEST(SavepointParser, PlainRollbackHasNoSavepoint) {
    auto stmt = parse_one("ROLLBACK");
    auto* rb = dynamic_cast<RollbackStmt*>(stmt.get());
    ASSERT_NE(rb, nullptr);
    EXPECT_TRUE(rb->savepoint.empty());
}

TEST(SavepointParser, ReleaseWithoutSavepointKeywordFails) {
    EXPECT_TRUE(parse_fails("RELEASE sp1"));
}

// ---------------------------------------------------------------------------
// Session savepoint execution tests
// GDB-883: all three execution commands now return NOT_IMPLEMENTED instead of
// fake success to prevent silent data-integrity violations (audit finding C5).
// ---------------------------------------------------------------------------

class SessionSavepointTest : public ::testing::Test {
protected:
    void SetUp() override {
        session_ = std::make_unique<Session>(1);
        // Put session into IN_TRANSACTION state.
        session_->update_transaction_state("BEGIN", true);
    }

    std::unique_ptr<Session> session_;
};

// -- create_savepoint ---------------------------------------------------------

TEST_F(SessionSavepointTest, CreateSavepointReturnsNotImplemented) {
    // GDB-883: SAVEPOINT must not fake success; it has no engine backing.
    auto result = session_->create_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
    // savepoints_ must remain empty -- no deque mutation.
    EXPECT_TRUE(session_->savepoints().empty());
}

TEST_F(SessionSavepointTest, CreateSavepointOutsideTransactionReturnsNotImplemented) {
    Session idle_session(2);
    auto result = idle_session.create_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(SessionSavepointTest, CreateSavepointInFailedStateReturnsNotImplemented) {
    session_->update_transaction_state("SELECT bad", false);
    ASSERT_EQ(session_->transaction_state(), TransactionState::FAILED);
    auto result = session_->create_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

// -- release_savepoint --------------------------------------------------------

TEST_F(SessionSavepointTest, ReleaseSavepointReturnsNotImplemented) {
    auto result = session_->release_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(SessionSavepointTest, ReleaseSavepointOutsideTransactionReturnsNotImplemented) {
    Session idle_session(2);
    auto result = idle_session.release_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(SessionSavepointTest, ReleaseSavepointInFailedStateReturnsNotImplemented) {
    session_->update_transaction_state("SELECT bad", false);
    ASSERT_EQ(session_->transaction_state(), TransactionState::FAILED);
    auto result = session_->release_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

// -- rollback_to_savepoint ----------------------------------------------------

TEST_F(SessionSavepointTest, RollbackToSavepointReturnsNotImplemented) {
    auto result = session_->rollback_to_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(SessionSavepointTest, RollbackToSavepointOutsideTransactionReturnsNotImplemented) {
    Session idle_session(2);
    auto result = idle_session.rollback_to_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(SessionSavepointTest, RollbackToSavepointDoesNotRecoverFailedState) {
    // GDB-883: rollback_to previously flipped FAILED->IN_TRANSACTION as a
    // side-effect of fake success. With NOT_IMPLEMENTED it must not do so.
    session_->update_transaction_state("SELECT bad", false);
    ASSERT_EQ(session_->transaction_state(), TransactionState::FAILED);

    auto result = session_->rollback_to_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
    // State must remain FAILED -- no silent recovery.
    EXPECT_EQ(session_->transaction_state(), TransactionState::FAILED);
}

// -- savepoints() accessor ----------------------------------------------------

TEST_F(SessionSavepointTest, SavepointsDequeAlwaysEmpty) {
    // create_savepoint no longer pushes, so the deque stays empty.
    (void)session_->create_savepoint("sp1");
    (void)session_->create_savepoint("sp2");
    EXPECT_TRUE(session_->savepoints().empty());
}

// -- COMMIT/ROLLBACK/cleanup still work as before -----------------------------

TEST_F(SessionSavepointTest, CommitTransitionsToIdle) {
    session_->update_transaction_state("COMMIT", true);
    EXPECT_TRUE(session_->savepoints().empty());
    EXPECT_EQ(session_->transaction_state(), TransactionState::IDLE);
}

TEST_F(SessionSavepointTest, RollbackTransitionsToIdle) {
    session_->update_transaction_state("ROLLBACK", true);
    EXPECT_TRUE(session_->savepoints().empty());
    EXPECT_EQ(session_->transaction_state(), TransactionState::IDLE);
}

TEST_F(SessionSavepointTest, CleanupResetsState) {
    session_->cleanup();
    EXPECT_TRUE(session_->savepoints().empty());
}

// ---------------------------------------------------------------------------
// Wire-command dispatch tests (try_handle_command)
// ---------------------------------------------------------------------------

TEST_F(SessionSavepointTest, TryHandleSavepointCommandReturnsNotImplemented) {
    auto result = session_->try_handle_command("SAVEPOINT sp1");
    ASSERT_TRUE(result.has_value()); // command was intercepted
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(SessionSavepointTest, TryHandleReleaseSavepointCommandReturnsNotImplemented) {
    auto result = session_->try_handle_command("RELEASE SAVEPOINT sp1");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(SessionSavepointTest, TryHandleRollbackToCommandReturnsNotImplemented) {
    auto result = session_->try_handle_command("ROLLBACK TO sp1");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(SessionSavepointTest, TryHandleRollbackToSavepointCommandReturnsNotImplemented) {
    auto result = session_->try_handle_command("ROLLBACK TO SAVEPOINT sp1");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(SessionSavepointTest, PlainRollbackNotIntercepted) {
    // "ROLLBACK" (without TO) is not a savepoint command -- must pass through.
    auto result = session_->try_handle_command("ROLLBACK");
    EXPECT_FALSE(result.has_value());
}
