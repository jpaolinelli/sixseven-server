/// @file test_qa_gdb_552.cpp
/// QA adversarial tests for GDB-552: Lexer consumes `--` in MATCH edge
/// patterns as SQL line comment.
///
/// Verifies the fix: `--` followed by `>`, `[`, or `(` is no longer treated
/// as a line comment.

#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Helpers
// ============================================================================

static std::vector<StmtPtr> parse_ok(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    EXPECT_TRUE(tokens.has_value()) << tokens.error().message;
    if (!tokens)
        return {};
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_TRUE(stmts.has_value()) << stmts.error().message;
    return stmts ? std::move(*stmts) : std::vector<StmtPtr>{};
}

static StmtPtr parse_one(std::string_view sql) {
    auto stmts = parse_ok(sql);
    EXPECT_EQ(stmts.size(), 1u);
    if (stmts.size() != 1)
        return nullptr;
    return std::move(stmts[0]);
}

static void parse_fail(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return; // Lexer failure is acceptable.
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_FALSE(stmts.has_value()) << "Expected parse failure for: " << sql;
}

static MatchStmt* match_from_select(const StmtPtr& stmt) {
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    if (!sel || sel->from.empty() || !sel->from[0].match_source)
        return nullptr;
    return dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
}

// ============================================================================
// Fix verification: -- followed by > is NOT a comment
// ============================================================================

TEST(QA_GDB552, DoubleDashArrowBracket_NotComment) {
    // `-->[e:follows]->` should NOT be consumed as a comment.
    // This must produce a parse error (bracket-less `-->` is invalid syntax),
    // not silently truncate the pattern to a single node.
    parse_fail("SELECT a.name FROM MATCH (a:users)-->[e:follows]->(b:users)");
}

TEST(QA_GDB552, DoubleDashBracket_NotComment) {
    // `--[` should NOT be consumed as a comment. The `--` before `[`
    // is part of undirected edge syntax attempt.
    parse_fail("SELECT a.name FROM MATCH (a:users)--[e:follows]->(b:users)");
}

TEST(QA_GDB552, DoubleDashArrow_TokensNotLost) {
    // Verify `-->` is NOT consumed as a comment by checking that
    // the lexer produces tokens beyond the `--`.
    Lexer lexer("SELECT 1 --> 2");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value()) << tokens.error().message;
    // If `-->` were a comment, we'd get only "SELECT 1 EOF" = 3 tokens.
    // With the fix, we get "SELECT 1 - - > 2 EOF" = more tokens.
    EXPECT_GT(tokens->size(), 3u)
        << "Tokens after --> should not be consumed as comment";
}

TEST(QA_GDB552, DoubleDashParen_TokensNotLost) {
    // Verify `--(` is NOT consumed as a comment.
    Lexer lexer("SELECT 1 --(2)");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value()) << tokens.error().message;
    // If `--(` were a comment, we'd get only "SELECT 1 EOF" = 3 tokens.
    EXPECT_GT(tokens->size(), 3u)
        << "Tokens after --( should not be consumed as comment";
}

// ============================================================================
// Regression: actual line comments still work
// ============================================================================

TEST(QA_GDB552, RegularLineComment_StillWorks) {
    // A real line comment (-- followed by space/text) should still work.
    auto stmt = parse_one("SELECT 1 -- this is a comment\n");
    ASSERT_NE(stmt, nullptr);
}

TEST(QA_GDB552, LineCommentAtEndOfQuery) {
    auto stmt = parse_one("SELECT 1 -- trailing comment");
    ASSERT_NE(stmt, nullptr);
}

TEST(QA_GDB552, LineCommentWithDashDashSpace) {
    // `-- ` (with space) is unambiguously a comment.
    auto stmt = parse_one("SELECT 1 -- comment\n");
    ASSERT_NE(stmt, nullptr);
}

TEST(QA_GDB552, LineCommentBeforeQuery) {
    auto stmt = parse_one("-- comment\nSELECT 1");
    ASSERT_NE(stmt, nullptr);
}

TEST(QA_GDB552, MultipleLineComments) {
    auto stmt = parse_one("-- first\n-- second\nSELECT 1");
    ASSERT_NE(stmt, nullptr);
}

// ============================================================================
// Edge cases around the -- disambiguation
// ============================================================================

TEST(QA_GDB552, TripleDash_IsComment) {
    // `---` followed by a non-special char should be a comment
    // (the first two dashes start the comment).
    auto stmt = parse_one("SELECT 1 ---comment");
    ASSERT_NE(stmt, nullptr);
}

TEST(QA_GDB552, DashDashNumber_IsComment) {
    // `--123` should still be a comment (-- followed by digit).
    auto stmt = parse_one("SELECT 1 --123");
    ASSERT_NE(stmt, nullptr);
}

TEST(QA_GDB552, ValidBracketEdge_StillParses) {
    // The correct bracket syntax should not be affected by the fix.
    auto stmt = parse_one(
        "SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users)");
    auto* m = match_from_select(stmt);
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 2u);
    EXPECT_TRUE(m->pattern[0].outgoing_edge.has_value());
}

TEST(QA_GDB552, IncomingEdge_StillParses) {
    // Incoming edge direction should work.
    auto stmt = parse_one(
        "SELECT a.name FROM MATCH (a:users)<-[e:follows]-(b:users)");
    auto* m = match_from_select(stmt);
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 2u);
}

TEST(QA_GDB552, MultiHopWithComment_Works) {
    // Query with valid edge syntax AND a trailing comment.
    auto stmt = parse_one(
        "SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users) -- get friends\n");
    auto* m = match_from_select(stmt);
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 2u);
}

TEST(QA_GDB552, BlockCommentUnaffected) {
    // Block comments should be unaffected by the fix.
    auto stmt = parse_one("SELECT /* inline */ 1");
    ASSERT_NE(stmt, nullptr);
}

TEST(QA_GDB552, DashDashAtEndOfInput_IsComment) {
    // `--` at the very end (no third char) should be a comment.
    auto stmt = parse_one("SELECT 1 --");
    ASSERT_NE(stmt, nullptr);
}

TEST(QA_GDB552, SubtractionExpression_Unaffected) {
    // `a - -b` or `1 - -1` should not trigger comment logic.
    // The spaces prevent `--` from being adjacent.
    auto stmt = parse_one("SELECT 1 - -1");
    ASSERT_NE(stmt, nullptr);
}

} // namespace
} // namespace sixseven
