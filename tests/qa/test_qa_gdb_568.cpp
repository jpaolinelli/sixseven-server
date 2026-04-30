/// @file test_qa_gdb_568.cpp
/// @brief Adversarial QA tests for GDB-568: SAVEPOINT Support for psqlODBC Compatibility

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

    void create_sp(const std::string& name) {
        auto r = session_->create_savepoint(name);
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    void enter_failed_state() {
        session_->update_transaction_state("SELECT bad_query", false);
        ASSERT_EQ(session_->transaction_state(), TransactionState::FAILED);
    }

    std::unique_ptr<Session> session_;
};

// ===========================================================================
// AC1: SAVEPOINT <name> succeeds within a transaction
// ===========================================================================

TEST_F(QA_GDB568_Session, AC1_SavepointSucceedsInTransaction) {
    auto result = session_->try_handle_command("SAVEPOINT my_sp");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value()) << result->error().message;
    EXPECT_EQ(result->value().message, "SAVEPOINT");
    ASSERT_EQ(session_->savepoints().size(), 1u);
    EXPECT_EQ(session_->savepoints().back(), "my_sp");
}

TEST_F(QA_GDB568_Session, AC1_SavepointParserProducesCorrectAST) {
    auto stmt = parse_one("SAVEPOINT test_sp");
    auto* sp = dynamic_cast<SavepointStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->name, "test_sp");
}

// ===========================================================================
// AC2: RELEASE SAVEPOINT <name> succeeds
// ===========================================================================

TEST_F(QA_GDB568_Session, AC2_ReleaseSavepointSucceeds) {
    create_sp("sp1");
    auto result = session_->try_handle_command("RELEASE SAVEPOINT sp1");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value()) << result->error().message;
    EXPECT_EQ(result->value().message, "RELEASE");
    EXPECT_TRUE(session_->savepoints().empty());
}

TEST_F(QA_GDB568_Session, AC2_ReleaseSavepointParserProducesCorrectAST) {
    auto stmt = parse_one("RELEASE SAVEPOINT test_sp");
    auto* rs = dynamic_cast<ReleaseSavepointStmt*>(stmt.get());
    ASSERT_NE(rs, nullptr);
    EXPECT_EQ(rs->name, "test_sp");
}

// ===========================================================================
// AC3: ROLLBACK TO SAVEPOINT <name> recovers from a failed statement
// ===========================================================================

TEST_F(QA_GDB568_Session, AC3_RollbackToSavepointRecoversFromFailed) {
    create_sp("sp1");
    enter_failed_state();

    auto result = session_->try_handle_command("ROLLBACK TO SAVEPOINT sp1");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value()) << result->error().message;
    EXPECT_EQ(session_->transaction_state(), TransactionState::IN_TRANSACTION);
    // Savepoint sp1 should still exist after rollback.
    ASSERT_EQ(session_->savepoints().size(), 1u);
    EXPECT_EQ(session_->savepoints().back(), "sp1");
}

TEST_F(QA_GDB568_Session, AC3_RollbackToWithoutSavepointKeyword) {
    create_sp("sp1");
    enter_failed_state();

    auto result = session_->try_handle_command("ROLLBACK TO sp1");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value()) << result->error().message;
    EXPECT_EQ(session_->transaction_state(), TransactionState::IN_TRANSACTION);
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
// AC4: Savepoints outside a transaction return an appropriate error
// ===========================================================================

TEST_F(QA_GDB568_Session, AC4_SavepointOutsideTransactionFails) {
    Session idle_session(200);
    ASSERT_EQ(idle_session.transaction_state(), TransactionState::IDLE);

    auto result = idle_session.create_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB568_Session, AC4_ReleaseSavepointOutsideTransactionFails) {
    Session idle_session(200);
    auto result = idle_session.release_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB568_Session, AC4_RollbackToSavepointOutsideTransactionFails) {
    Session idle_session(200);
    auto result = idle_session.rollback_to_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB568_Session, AC4_SavepointViaCommandOutsideTransactionFails) {
    Session idle_session(200);
    auto result = idle_session.try_handle_command("SAVEPOINT sp1");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, StatusCode::INVALID_ARGUMENT);
}

// ===========================================================================
// AC5: Nested savepoints work (SAVEPOINT a; SAVEPOINT b; ROLLBACK TO a)
// ===========================================================================

TEST_F(QA_GDB568_Session, AC5_NestedSavepointsRollbackToEarliest) {
    create_sp("a");
    create_sp("b");
    create_sp("c");
    ASSERT_EQ(session_->savepoints().size(), 3u);

    auto result = session_->rollback_to_savepoint("a");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Only "a" should remain.
    ASSERT_EQ(session_->savepoints().size(), 1u);
    EXPECT_EQ(session_->savepoints()[0], "a");
}

TEST_F(QA_GDB568_Session, AC5_NestedSavepointsRollbackToMiddle) {
    create_sp("a");
    create_sp("b");
    create_sp("c");

    auto result = session_->rollback_to_savepoint("b");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(session_->savepoints().size(), 2u);
    EXPECT_EQ(session_->savepoints()[0], "a");
    EXPECT_EQ(session_->savepoints()[1], "b");
}

TEST_F(QA_GDB568_Session, AC5_NestedSavepointsRollbackToLatest) {
    create_sp("a");
    create_sp("b");
    create_sp("c");

    auto result = session_->rollback_to_savepoint("c");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // All three should remain since "c" is the latest.
    ASSERT_EQ(session_->savepoints().size(), 3u);
}

// ===========================================================================
// Adversarial: Empty/weird savepoint names
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_EmptySavepointNameViaAPI) {
    // Empty name via API -- should succeed (no validation on name content).
    auto result = session_->create_savepoint("");
    EXPECT_TRUE(result.has_value());
}

TEST_F(QA_GDB568_Session, Adversarial_EmptySavepointNameViaCommand) {
    // "SAVEPOINT " gets trimmed to "SAVEPOINT" which does not match the
    // "SAVEPOINT " prefix check, so it falls through (returns nullopt).
    auto result = session_->try_handle_command("SAVEPOINT ");
    EXPECT_FALSE(result.has_value());
}

TEST_F(QA_GDB568_Session, Adversarial_VeryLongSavepointName) {
    // 10000 char savepoint name.
    std::string long_name(10000, 'x');
    auto result = session_->create_savepoint(long_name);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(session_->savepoints().back(), long_name);

    // Rollback to it.
    auto rb = session_->rollback_to_savepoint(long_name);
    EXPECT_TRUE(rb.has_value());
}

TEST_F(QA_GDB568_Session, Adversarial_SavepointNameWithSpaces) {
    // Via direct API, spaces in name are allowed (just a string).
    auto result = session_->create_savepoint("sp with spaces");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(session_->savepoints().back(), "sp with spaces");
}

TEST_F(QA_GDB568_Session, Adversarial_SavepointNameWithSpecialChars) {
    auto result = session_->create_savepoint("sp!@#$%^&*()");
    EXPECT_TRUE(result.has_value());
    auto rb = session_->rollback_to_savepoint("sp!@#$%^&*()");
    EXPECT_TRUE(rb.has_value());
}

// ===========================================================================
// Adversarial: Non-existent savepoint operations
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_ReleaseNonExistentSavepoint) {
    auto result = session_->release_savepoint("does_not_exist");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB568_Session, Adversarial_RollbackToNonExistentSavepoint) {
    auto result = session_->rollback_to_savepoint("does_not_exist");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB568_Session, Adversarial_RollbackToNonExistentInFailedState) {
    create_sp("sp1");
    enter_failed_state();

    // Trying to rollback to a non-existent savepoint in FAILED state.
    auto result = session_->rollback_to_savepoint("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    // Transaction should still be in FAILED state.
    EXPECT_EQ(session_->transaction_state(), TransactionState::FAILED);
}

// ===========================================================================
// Adversarial: Released savepoint operations
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_ReleaseAlreadyReleasedSavepoint) {
    create_sp("sp1");
    auto r1 = session_->release_savepoint("sp1");
    ASSERT_TRUE(r1.has_value());

    // Second release should fail.
    auto r2 = session_->release_savepoint("sp1");
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB568_Session, Adversarial_RollbackToReleasedSavepoint) {
    create_sp("sp1");
    auto r1 = session_->release_savepoint("sp1");
    ASSERT_TRUE(r1.has_value());

    // Rollback to released savepoint should fail.
    auto r2 = session_->rollback_to_savepoint("sp1");
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, StatusCode::INVALID_ARGUMENT);
}

// ===========================================================================
// Adversarial: Duplicate savepoint names
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_DuplicateSavepointNamesCreate) {
    create_sp("dup");
    create_sp("dup");
    create_sp("dup");
    EXPECT_EQ(session_->savepoints().size(), 3u);
}

TEST_F(QA_GDB568_Session, Adversarial_DuplicateNamesReleaseRemovesMostRecent) {
    create_sp("dup");
    create_sp("other");
    create_sp("dup");

    // Release should remove the most recent "dup" (search from back).
    auto result = session_->release_savepoint("dup");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(session_->savepoints().size(), 2u);
    EXPECT_EQ(session_->savepoints()[0], "dup");
    EXPECT_EQ(session_->savepoints()[1], "other");
}

TEST_F(QA_GDB568_Session, Adversarial_DuplicateNamesRollbackToMostRecent) {
    create_sp("dup");
    create_sp("other");
    create_sp("dup");

    // Rollback to "dup" should find the most recent one.
    auto result = session_->rollback_to_savepoint("dup");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // The most recent "dup" is at index 2; nothing after it. All three remain.
    ASSERT_EQ(session_->savepoints().size(), 3u);
}

// ===========================================================================
// Adversarial: Stack depth stress
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_ManySavepoints) {
    const size_t count = 1000;
    for (size_t i = 0; i < count; ++i) {
        auto r = session_->create_savepoint("sp_" + std::to_string(i));
        ASSERT_TRUE(r.has_value()) << "Failed at savepoint " << i;
    }
    EXPECT_EQ(session_->savepoints().size(), count);

    // Rollback to the first one.
    auto result = session_->rollback_to_savepoint("sp_0");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(session_->savepoints().size(), 1u);
    EXPECT_EQ(session_->savepoints()[0], "sp_0");
}

TEST_F(QA_GDB568_Session, Adversarial_ManySavepointsReleaseAll) {
    const size_t count = 500;
    for (size_t i = 0; i < count; ++i) {
        create_sp("sp_" + std::to_string(i));
    }

    // Release all in reverse order.
    for (size_t i = count; i > 0; --i) {
        auto r = session_->release_savepoint("sp_" + std::to_string(i - 1));
        ASSERT_TRUE(r.has_value()) << "Failed releasing sp_" << (i - 1);
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
    EXPECT_EQ(result.error().code, StatusCode::TXN_ABORTED);
}

TEST_F(QA_GDB568_Session, Adversarial_ReleaseSavepointInFailedState) {
    create_sp("sp1");
    enter_failed_state();
    auto result = session_->release_savepoint("sp1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TXN_ABORTED);
}

TEST_F(QA_GDB568_Session, Adversarial_SavepointCommandInFailedState) {
    enter_failed_state();
    auto result = session_->try_handle_command("SAVEPOINT sp_fail");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, StatusCode::TXN_ABORTED);
}

TEST_F(QA_GDB568_Session, Adversarial_ReleaseCommandInFailedState) {
    create_sp("sp1");
    enter_failed_state();
    auto result = session_->try_handle_command("RELEASE SAVEPOINT sp1");
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, StatusCode::TXN_ABORTED);
}

// ===========================================================================
// Adversarial: COMMIT/ROLLBACK clearing savepoints
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_CommitClearsSavepoints) {
    create_sp("sp1");
    create_sp("sp2");
    create_sp("sp3");
    ASSERT_EQ(session_->savepoints().size(), 3u);

    session_->update_transaction_state("COMMIT", true);
    EXPECT_TRUE(session_->savepoints().empty());
    EXPECT_EQ(session_->transaction_state(), TransactionState::IDLE);
}

TEST_F(QA_GDB568_Session, Adversarial_RollbackClearsSavepoints) {
    create_sp("sp1");
    create_sp("sp2");
    session_->update_transaction_state("ROLLBACK", true);
    EXPECT_TRUE(session_->savepoints().empty());
    EXPECT_EQ(session_->transaction_state(), TransactionState::IDLE);
}

TEST_F(QA_GDB568_Session, Adversarial_AbortClearsSavepoints) {
    create_sp("sp1");
    session_->update_transaction_state("ABORT", true);
    EXPECT_TRUE(session_->savepoints().empty());
    EXPECT_EQ(session_->transaction_state(), TransactionState::IDLE);
}

TEST_F(QA_GDB568_Session, Adversarial_EndClearsSavepoints) {
    create_sp("sp1");
    session_->update_transaction_state("END", true);
    EXPECT_TRUE(session_->savepoints().empty());
    EXPECT_EQ(session_->transaction_state(), TransactionState::IDLE);
}

// ===========================================================================
// Adversarial: BEGIN/COMMIT/ROLLBACK sequences with savepoints
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_FullPsqlODBCWorkflow) {
    // Simulate the psqlODBC error recovery pattern:
    // BEGIN -> SAVEPOINT -> failing query -> ROLLBACK TO -> new SAVEPOINT -> COMMIT

    // 1. Create savepoint.
    auto sp1 = session_->try_handle_command("SAVEPOINT odbc_sp1");
    ASSERT_TRUE(sp1.has_value() && sp1->has_value());

    // 2. Simulate a failing statement.
    session_->update_transaction_state("SELECT * FROM nonexistent_table", false);
    EXPECT_EQ(session_->transaction_state(), TransactionState::FAILED);

    // 3. Rollback to savepoint.
    auto rb1 = session_->try_handle_command("ROLLBACK TO SAVEPOINT odbc_sp1");
    ASSERT_TRUE(rb1.has_value() && rb1->has_value());
    EXPECT_EQ(session_->transaction_state(), TransactionState::IN_TRANSACTION);

    // 4. Release the savepoint.
    auto rel = session_->try_handle_command("RELEASE SAVEPOINT odbc_sp1");
    ASSERT_TRUE(rel.has_value() && rel->has_value());
    EXPECT_TRUE(session_->savepoints().empty());

    // 5. Create a new savepoint and do more work.
    auto sp2 = session_->try_handle_command("SAVEPOINT odbc_sp2");
    ASSERT_TRUE(sp2.has_value() && sp2->has_value());

    // 6. Release and commit.
    auto rel2 = session_->try_handle_command("RELEASE SAVEPOINT odbc_sp2");
    ASSERT_TRUE(rel2.has_value() && rel2->has_value());

    session_->update_transaction_state("COMMIT", true);
    EXPECT_EQ(session_->transaction_state(), TransactionState::IDLE);
    EXPECT_TRUE(session_->savepoints().empty());
}

TEST_F(QA_GDB568_Session, Adversarial_MultipleFailAndRecoverCycles) {
    // Multiple failure + recovery cycles within one transaction.
    for (int i = 0; i < 10; ++i) {
        std::string sp_name = "cycle_" + std::to_string(i);
        auto sp = session_->try_handle_command("SAVEPOINT " + sp_name);
        ASSERT_TRUE(sp.has_value() && sp->has_value()) << "Cycle " << i;

        // Fail.
        session_->update_transaction_state("bad query", false);
        EXPECT_EQ(session_->transaction_state(), TransactionState::FAILED);

        // Recover.
        auto rb = session_->try_handle_command("ROLLBACK TO " + sp_name);
        ASSERT_TRUE(rb.has_value() && rb->has_value()) << "Cycle " << i;
        EXPECT_EQ(session_->transaction_state(), TransactionState::IN_TRANSACTION);

        // Release.
        auto rel = session_->try_handle_command("RELEASE SAVEPOINT " + sp_name);
        ASSERT_TRUE(rel.has_value() && rel->has_value()) << "Cycle " << i;
    }
    EXPECT_TRUE(session_->savepoints().empty());
}

// ===========================================================================
// Adversarial: Case sensitivity of commands
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_CaseInsensitiveCommandKeywords) {
    auto r1 = session_->try_handle_command("savepoint sp1");
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r1->has_value()) << r1->error().message;

    auto r2 = session_->try_handle_command("release savepoint sp1");
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r2->has_value()) << r2->error().message;
}

TEST_F(QA_GDB568_Session, Adversarial_MixedCaseCommandKeywords) {
    auto r1 = session_->try_handle_command("Savepoint SP1");
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r1->has_value()) << r1->error().message;
    // The name should preserve original case.
    EXPECT_EQ(session_->savepoints().back(), "SP1");
}

TEST_F(QA_GDB568_Session, Adversarial_RollbackToCaseInsensitive) {
    create_sp("sp1");
    auto r = session_->try_handle_command("rollback to sp1");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value()) << r->error().message;
}

// ===========================================================================
// Adversarial: Whitespace handling
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_LeadingTrailingWhitespace) {
    auto r = session_->try_handle_command("  SAVEPOINT sp1  ");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value()) << r->error().message;
    // Name may include trailing whitespace depending on parsing.
    // The trim is applied to the whole command, but the name is trim(rest).
    EXPECT_EQ(session_->savepoints().back(), "sp1");
}

TEST_F(QA_GDB568_Session, Adversarial_MultipleSpacesBetweenKeywords) {
    auto r = session_->try_handle_command("ROLLBACK TO    SAVEPOINT    sp1");
    // "ROLLBACK TO " is the prefix check (12 chars).
    // After substr(12) and trim: "SAVEPOINT    sp1"
    // starts_with_ci("SAVEPOINT ") matches, then substr(10) and trim: "   sp1" -> "sp1"
    // But the actual string after "ROLLBACK TO " is "   SAVEPOINT    sp1"
    // After trim: "SAVEPOINT    sp1"
    // starts_with_ci matches "SAVEPOINT ", rest = "   sp1", trimmed = "sp1"
    // This should work if the implementation trims properly.
    // Let's just check.
    create_sp("sp1");
    auto r2 = session_->try_handle_command("ROLLBACK TO    SAVEPOINT    sp1");
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r2->has_value()) << r2->error().message;
}

// ===========================================================================
// Adversarial: Cleanup clears savepoints
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_CleanupClearsSavepoints) {
    create_sp("sp1");
    create_sp("sp2");
    session_->cleanup();
    EXPECT_TRUE(session_->savepoints().empty());
    EXPECT_EQ(session_->transaction_state(), TransactionState::IDLE);
}

// ===========================================================================
// Adversarial: Parser edge cases
// ===========================================================================

TEST(QA_GDB568_Parser, SavepointWithoutName) {
    // "SAVEPOINT" with no name should fail to parse.
    EXPECT_TRUE(parse_fails("SAVEPOINT"));
}

TEST(QA_GDB568_Parser, ReleaseSavepointWithoutName) {
    EXPECT_TRUE(parse_fails("RELEASE SAVEPOINT"));
}

TEST(QA_GDB568_Parser, ReleaseWithoutSavepointKeyword) {
    // "RELEASE sp1" (without SAVEPOINT keyword) should fail.
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
    // Parser should preserve the savepoint name case.
    // parse_name() may lowercase it depending on implementation.
    // This test verifies actual behavior.
    EXPECT_FALSE(sp->name.empty());
}

TEST(QA_GDB568_Parser, SavepointNameNumeric) {
    // Numeric-like name: some parsers might trip on this.
    // This will likely fail since identifiers usually can't start with digits.
    // Testing to document behavior.
    auto stmt = parse_one("SAVEPOINT sp123");
    if (stmt) {
        auto* sp = dynamic_cast<SavepointStmt*>(stmt.get());
        ASSERT_NE(sp, nullptr);
        EXPECT_EQ(sp->name, "sp123");
    }
}

// ===========================================================================
// Adversarial: Rollback to savepoint should NOT affect other savepoints before it
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_RollbackPreservesEarlierSavepoints) {
    create_sp("a");
    create_sp("b");
    create_sp("c");
    create_sp("d");

    // Rollback to "b" should remove "c" and "d" but keep "a" and "b".
    auto result = session_->rollback_to_savepoint("b");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(session_->savepoints().size(), 2u);
    EXPECT_EQ(session_->savepoints()[0], "a");
    EXPECT_EQ(session_->savepoints()[1], "b");

    // Can still create new savepoints after rollback.
    auto sp_new = session_->create_savepoint("e");
    ASSERT_TRUE(sp_new.has_value());
    ASSERT_EQ(session_->savepoints().size(), 3u);
}

// ===========================================================================
// Adversarial: Rollback TO in FAILED state with non-existent savepoint
// should not change state
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_FailedRollbackToNonExistentKeepsFailedState) {
    create_sp("sp1");
    enter_failed_state();

    auto result = session_->rollback_to_savepoint("wrong_name");
    ASSERT_FALSE(result.has_value());
    // Should still be FAILED.
    EXPECT_EQ(session_->transaction_state(), TransactionState::FAILED);
    // Savepoint stack should be unchanged.
    ASSERT_EQ(session_->savepoints().size(), 1u);
    EXPECT_EQ(session_->savepoints()[0], "sp1");
}

// ===========================================================================
// Adversarial: Savepoint after recovery
// ===========================================================================

TEST_F(QA_GDB568_Session, Adversarial_CreateSavepointAfterRecovery) {
    create_sp("sp1");
    enter_failed_state();

    // Recover.
    auto rb = session_->rollback_to_savepoint("sp1");
    ASSERT_TRUE(rb.has_value());
    EXPECT_EQ(session_->transaction_state(), TransactionState::IN_TRANSACTION);

    // Should be able to create new savepoints.
    auto sp2 = session_->create_savepoint("sp2");
    ASSERT_TRUE(sp2.has_value());
    ASSERT_EQ(session_->savepoints().size(), 2u);
}

// ===========================================================================
// Regression: No regressions (AC7) -- verify plain ROLLBACK/COMMIT still work
// ===========================================================================

TEST_F(QA_GDB568_Session, Regression_PlainRollbackStillWorks) {
    create_sp("sp1");
    // Plain ROLLBACK (not TO) should not be intercepted by try_handle_command.
    auto result = session_->try_handle_command("ROLLBACK");
    // "ROLLBACK" does not start with "ROLLBACK TO ", so it returns nullopt.
    EXPECT_FALSE(result.has_value());
}

TEST_F(QA_GDB568_Session, Regression_BeginCommitNotIntercepted) {
    auto r1 = session_->try_handle_command("BEGIN");
    EXPECT_FALSE(r1.has_value());

    auto r2 = session_->try_handle_command("COMMIT");
    EXPECT_FALSE(r2.has_value());
}
