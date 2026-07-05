#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"

#include <gtest/gtest.h>

// GDB-1205: Parser::parse_with now rejects WITH RECURSIVE with a clear
// PARSE_ERROR instead of silently treating RECURSIVE as the CTE name.
//
// Adversarial focus:
//  1. WITH RECURSIVE is always rejected with a clear, non-crashing error.
//  2. RECURSIVE must NOT be wrongly rejected anywhere it is legitimately an
//     identifier (column name, table name, alias, CTE-body column) -- the
//     ONLY rejection point is immediately after a bare WITH.
//  3. Case/whitespace variants of RECURSIVE all behave consistently.
//  4. Nested WITH RECURSIVE (in subqueries) and multi-CTE lists are rejected.
//  5. Degenerate/truncated WITH RECURSIVE statements produce a clean parse
//     error -- never a crash or hang.

using namespace sixseven;

namespace {

std::vector<StmtPtr> parse_ok(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    EXPECT_TRUE(tokens.has_value()) << tokens.error().message;
    if (!tokens)
        return {};

    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_TRUE(stmts.has_value()) << (stmts ? "" : stmts.error().message);
    return stmts ? std::move(*stmts) : std::vector<StmtPtr>{};
}

StmtPtr parse_one(std::string_view sql) {
    auto stmts = parse_ok(sql);
    EXPECT_EQ(stmts.size(), 1u) << "sql: " << sql;
    if (stmts.size() != 1)
        return nullptr;
    return std::move(stmts[0]);
}

// Returns the parse error, or asserts a failure occurred without crashing.
Result<std::vector<StmtPtr>> parse_result(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens) {
        return tl::unexpected(tokens.error());
    }
    Parser parser(std::move(*tokens));
    return parser.parse_all();
}

void expect_recursive_parse_error(std::string_view sql) {
    auto result = parse_result(sql);
    ASSERT_FALSE(result.has_value()) << "expected parse error for: " << sql;
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    EXPECT_NE(result.error().message.find("recursive"), std::string::npos)
        << "sql: " << sql << " actual message: " << result.error().message;
    EXPECT_NE(result.error().message.find("not supported"), std::string::npos)
        << "sql: " << sql << " actual message: " << result.error().message;
}

} // namespace

// -- 1. WITH RECURSIVE is always rejected clearly -----------------------------

TEST(QA_GDB1205, BasicWithRecursiveRejected) {
    expect_recursive_parse_error(
        "WITH RECURSIVE t AS (SELECT 1) SELECT * FROM t");
}

TEST(QA_GDB1205, WithRecursiveMultipleCTEsRejected) {
    expect_recursive_parse_error(
        "WITH RECURSIVE a AS (SELECT 1), b AS (SELECT 2) "
        "SELECT * FROM a, b");
}

// -- 2. No false positives: RECURSIVE remains valid elsewhere ------------------

TEST(QA_GDB1205, ColumnNamedRecursiveIsAccepted) {
    auto stmt = parse_one("SELECT recursive FROM t");
    ASSERT_NE(stmt, nullptr);
}

TEST(QA_GDB1205, TableNamedRecursiveIsAccepted) {
    auto stmt = parse_one("SELECT * FROM recursive");
    ASSERT_NE(stmt, nullptr);
}

TEST(QA_GDB1205, CTEProjectingColumnAliasedRecursiveIsAccepted) {
    auto stmt = parse_one(
        "WITH x AS (SELECT 1 AS recursive) SELECT recursive FROM x");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->ctes.size(), 1u);
    EXPECT_EQ(sel->ctes[0].name, "x");
}

TEST(QA_GDB1205, ColumnAliasedAsRecursiveIsAccepted) {
    auto stmt = parse_one("SELECT col AS recursive FROM t");
    ASSERT_NE(stmt, nullptr);
}

TEST(QA_GDB1205, CTENamedRecursiveIshIsAccepted) {
    // A CTE name that merely starts with/contains "recursive" but is not the
    // literal keyword token must not be affected.
    auto stmt = parse_one(
        "WITH recursively AS (SELECT 1) SELECT * FROM recursively");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->ctes.size(), 1u);
    EXPECT_EQ(sel->ctes[0].name, "recursively");
}

TEST(QA_GDB1205, WhereClausePredicateOnColumnNamedRecursive) {
    auto stmt = parse_one("SELECT * FROM t WHERE recursive = 1");
    ASSERT_NE(stmt, nullptr);
}

TEST(QA_GDB1205, InsertIntoColumnNamedRecursive) {
    auto stmt = parse_one("INSERT INTO t (recursive) VALUES (1)");
    ASSERT_NE(stmt, nullptr);
}

// -- 3. Case / whitespace variants ---------------------------------------------

TEST(QA_GDB1205, ExtraWhitespaceBetweenWithAndRecursiveRejected) {
    expect_recursive_parse_error(
        "WITH   RECURSIVE   t AS (SELECT 1) SELECT * FROM t");
}

TEST(QA_GDB1205, LowercaseWithRecursiveRejected) {
    expect_recursive_parse_error(
        "with recursive t as (select 1) select * from t");
}

TEST(QA_GDB1205, MixedCaseWithRecursiveRejected) {
    expect_recursive_parse_error(
        "WITH Recursive t AS (SELECT 1) SELECT * FROM t");
}

TEST(QA_GDB1205, TabNewlineBetweenWithAndRecursiveRejected) {
    expect_recursive_parse_error(
        "WITH\t\nRECURSIVE t AS (SELECT 1) SELECT * FROM t");
}

// -- 4. Nested / multiple ------------------------------------------------------

// NOTE: WITH is not currently accepted as the start of a subquery or CTE
// body at all (pre-existing parser limitation, independent of GDB-1205 --
// parse_subquery/CTE-body parsing only dispatches to SELECT/TRAVERSE/MATCH).
// So "WITH RECURSIVE" nested inside a subquery or CTE body fails with a
// generic "expected SELECT..." error rather than the RECURSIVE-specific
// message. This is expected/unchanged behavior, not a regression from this
// ticket; we assert the (pre-existing) clean parse error here, and note the
// message will differ from the top-level case.
TEST(QA_GDB1205, NestedSubqueryWithRecursiveIsCleanParseError) {
    auto result = parse_result(
        "SELECT * FROM (WITH RECURSIVE t AS (SELECT 1) SELECT * FROM t) sub");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB1205, OuterPlainWithInnerSubqueryRecursiveIsCleanParseError) {
    // Outer WITH is plain (fine); WITH is not valid to open a CTE body at
    // all today (pre-existing gap), so this fails before RECURSIVE handling
    // is ever reached for the inner clause. Must still be a clean
    // PARSE_ERROR, never a crash/hang, and must not corrupt outer parsing.
    auto result = parse_result(
        "WITH outer_cte AS ("
        "  WITH RECURSIVE inner_cte AS (SELECT 1) SELECT * FROM inner_cte"
        ") SELECT * FROM outer_cte");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// -- 5. Degenerate / malformed --------------------------------------------------

TEST(QA_GDB1205, WithRecursiveWithNothingAfterIsCleanParseError) {
    auto result = parse_result("WITH RECURSIVE");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB1205, WithRecursiveFollowedBySelectIsCleanParseError) {
    auto result = parse_result("WITH RECURSIVE SELECT 1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB1205, JustWithRecursiveSemicolonIsCleanParseError) {
    auto result = parse_result("WITH RECURSIVE;");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB1205, WithRecursiveRecursiveDoubledIsCleanParseError) {
    // Degenerate double-keyword input must not crash or infinite-loop.
    auto result = parse_result("WITH RECURSIVE RECURSIVE t AS (SELECT 1) SELECT * FROM t");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB1205, EmptyStatementAfterWithRecursiveNoTrailingContent) {
    auto result = parse_result("WITH RECURSIVE ");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}
