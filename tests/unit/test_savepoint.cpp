#include "sixseven/executor/query_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/server/session.h"

#include <gtest/gtest.h>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Parser tests for savepoint statements
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
// Session savepoint stack tests
// ---------------------------------------------------------------------------

class SessionSavepointTest : public ::testing::Test {
protected:
    void SetUp() override {
        session_ = std::make_unique<Session>(1);
        // Put session into IN_TRANSACTION state.
        session_->update_transaction_state("BEGIN", true);
    }

    /// Helper to create a savepoint with assertion.
    void create_sp(const std::string& name) {
        auto r = session_->create_savepoint(name);
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    std::unique_ptr<Session> session_;
};

TEST_F(SessionSavepointTest, CreateSavepointInTransaction) {
    auto result = session_->create_savepoint("sp1");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(session_->savepoints().size(), 1u);
    EXPECT_EQ(session_->savepoints().back(), "sp1");
}

TEST_F(SessionSavepointTest, CreateSavepointOutsideTransactionFails) {
    Session idle_session(2);
    auto result = idle_session.create_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(SessionSavepointTest, ReleaseSavepoint) {
    create_sp("sp1");
    auto result = session_->release_savepoint("sp1");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(session_->savepoints().empty());
}

TEST_F(SessionSavepointTest, ReleaseNonexistentSavepointFails) {
    auto result = session_->release_savepoint("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(SessionSavepointTest, ReleaseSavepointOutsideTransactionFails) {
    Session idle_session(2);
    auto result = idle_session.release_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(SessionSavepointTest, RollbackToSavepoint) {
    create_sp("sp1");
    create_sp("sp2");
    auto result = session_->rollback_to_savepoint("sp1");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // sp2 should be gone, sp1 should remain.
    ASSERT_EQ(session_->savepoints().size(), 1u);
    EXPECT_EQ(session_->savepoints().back(), "sp1");
}

TEST_F(SessionSavepointTest, RollbackToSavepointRecoveryFromFailed) {
    create_sp("sp1");
    // Simulate a failed statement.
    session_->update_transaction_state("SELECT bad", false);
    ASSERT_EQ(session_->transaction_state(), TransactionState::FAILED);

    auto result = session_->rollback_to_savepoint("sp1");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(session_->transaction_state(), TransactionState::IN_TRANSACTION);
    // sp1 should still be there.
    ASSERT_EQ(session_->savepoints().size(), 1u);
    EXPECT_EQ(session_->savepoints().back(), "sp1");
}

TEST_F(SessionSavepointTest, RollbackToNonexistentSavepointFails) {
    auto result = session_->rollback_to_savepoint("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(SessionSavepointTest, RollbackToSavepointOutsideTransactionFails) {
    Session idle_session(2);
    auto result = idle_session.rollback_to_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(SessionSavepointTest, NestedSavepoints) {
    create_sp("a");
    create_sp("b");
    create_sp("c");
    ASSERT_EQ(session_->savepoints().size(), 3u);

    // Rolling back to "a" should remove "b" and "c" but keep "a".
    auto result = session_->rollback_to_savepoint("a");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(session_->savepoints().size(), 1u);
    EXPECT_EQ(session_->savepoints().back(), "a");
}

TEST_F(SessionSavepointTest, NestedSavepointsRollbackToMiddle) {
    create_sp("a");
    create_sp("b");
    create_sp("c");

    // Rolling back to "b" should remove "c" but keep "a" and "b".
    auto result = session_->rollback_to_savepoint("b");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(session_->savepoints().size(), 2u);
    EXPECT_EQ(session_->savepoints()[0], "a");
    EXPECT_EQ(session_->savepoints()[1], "b");
}

TEST_F(SessionSavepointTest, DuplicateSavepointNames) {
    // PostgreSQL allows duplicate savepoint names; the most recent one wins.
    create_sp("sp1");
    create_sp("sp1");
    ASSERT_EQ(session_->savepoints().size(), 2u);

    // Rollback to "sp1" finds the most recent one (search from back).
    auto result = session_->rollback_to_savepoint("sp1");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // The second sp1 is found; nothing after it to remove. Both remain.
    ASSERT_EQ(session_->savepoints().size(), 2u);
}

TEST_F(SessionSavepointTest, CommitClearsSavepoints) {
    create_sp("sp1");
    create_sp("sp2");
    session_->update_transaction_state("COMMIT", true);
    EXPECT_TRUE(session_->savepoints().empty());
    EXPECT_EQ(session_->transaction_state(), TransactionState::IDLE);
}

TEST_F(SessionSavepointTest, RollbackClearsSavepoints) {
    create_sp("sp1");
    session_->update_transaction_state("ROLLBACK", true);
    EXPECT_TRUE(session_->savepoints().empty());
    EXPECT_EQ(session_->transaction_state(), TransactionState::IDLE);
}

TEST_F(SessionSavepointTest, CleanupClearsSavepoints) {
    create_sp("sp1");
    session_->cleanup();
    EXPECT_TRUE(session_->savepoints().empty());
}

// ---------------------------------------------------------------------------
// Session command handling tests for savepoint commands
// ---------------------------------------------------------------------------

TEST_F(SessionSavepointTest, TryHandleSavepointCommand) {
    auto result = session_->try_handle_command("SAVEPOINT sp1");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value()) << result->error().message;
    EXPECT_EQ(result->value().message, "SAVEPOINT");
    ASSERT_EQ(session_->savepoints().size(), 1u);
}

TEST_F(SessionSavepointTest, TryHandleReleaseSavepointCommand) {
    create_sp("sp1");
    auto result = session_->try_handle_command("RELEASE SAVEPOINT sp1");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value()) << result->error().message;
    EXPECT_EQ(result->value().message, "RELEASE");
    EXPECT_TRUE(session_->savepoints().empty());
}

TEST_F(SessionSavepointTest, TryHandleRollbackToCommand) {
    create_sp("sp1");
    auto result = session_->try_handle_command("ROLLBACK TO sp1");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value()) << result->error().message;
    EXPECT_EQ(result->value().message, "ROLLBACK");
}

TEST_F(SessionSavepointTest, TryHandleRollbackToSavepointCommand) {
    create_sp("sp1");
    auto result = session_->try_handle_command("ROLLBACK TO SAVEPOINT sp1");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value()) << result->error().message;
    EXPECT_EQ(result->value().message, "ROLLBACK");
}

TEST_F(SessionSavepointTest, SavepointErrorRecoveryWorkflow) {
    // Simulates the psqlODBC pattern:
    // BEGIN -> SAVEPOINT sp1 -> failing query -> ROLLBACK TO sp1 -> continue
    EXPECT_EQ(session_->transaction_state(), TransactionState::IN_TRANSACTION);

    // Create savepoint.
    auto sp_result = session_->try_handle_command("SAVEPOINT sp1");
    ASSERT_TRUE(sp_result.has_value());
    ASSERT_TRUE(sp_result->has_value());

    // Simulate a failing statement.
    session_->update_transaction_state("SELECT * FROM nonexistent", false);
    EXPECT_EQ(session_->transaction_state(), TransactionState::FAILED);

    // Rollback to savepoint should recover.
    auto rb_result = session_->try_handle_command("ROLLBACK TO SAVEPOINT sp1");
    ASSERT_TRUE(rb_result.has_value());
    ASSERT_TRUE(rb_result->has_value()) << rb_result->error().message;
    EXPECT_EQ(session_->transaction_state(), TransactionState::IN_TRANSACTION);

    // Now we can continue with more statements.
    auto sp2_result = session_->try_handle_command("SAVEPOINT sp2");
    ASSERT_TRUE(sp2_result.has_value());
    ASSERT_TRUE(sp2_result->has_value());
}

TEST_F(SessionSavepointTest, CreateSavepointInFailedStateFails) {
    session_->update_transaction_state("SELECT bad", false);
    ASSERT_EQ(session_->transaction_state(), TransactionState::FAILED);

    auto result = session_->create_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TXN_ABORTED);
}

TEST_F(SessionSavepointTest, ReleaseSavepointInFailedStateFails) {
    create_sp("sp1");
    session_->update_transaction_state("SELECT bad", false);
    ASSERT_EQ(session_->transaction_state(), TransactionState::FAILED);

    auto result = session_->release_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TXN_ABORTED);
}
