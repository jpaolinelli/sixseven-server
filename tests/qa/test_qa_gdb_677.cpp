/// @file test_qa_gdb_677.cpp
/// @brief QA adversarial tests for GDB-677: parse `WITH TRACE` on TRAVERSE.
///
/// Exercises the lexer keyword, the TraverseStmt.trace AST flag, and the
/// parse_traverse() handling of the optional `WITH TRACE` clause. Covers the
/// story's acceptance criteria plus adversarial edge cases: case-insensitivity,
/// error paths (WITH without TRACE), clause ordering, CTE-WITH non-conflict,
/// and trailing tokens. The reserved-keyword regression that making TRACE a
/// keyword introduced was fixed by GDB-693; the QA_GDB677_Regression cases here
/// now pin the restored behavior (see test_qa_gdb_693.cpp for full coverage).

#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/parser/token.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace sixseven;

namespace {

// Parse `sql`, expecting exactly one successful statement. Returns nullptr (and
// records a gtest failure) otherwise.
StmtPtr parse_first(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    EXPECT_TRUE(tokens.has_value()) << "lex failed: " << sql;
    if (!tokens)
        return nullptr;
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_TRUE(stmts.has_value())
        << "parse failed for: " << sql << " -> " << (stmts ? "" : stmts.error().message);
    if (!stmts)
        return nullptr;
    EXPECT_EQ(stmts->size(), 1u) << "expected exactly one statement for: " << sql;
    if (stmts->size() != 1)
        return nullptr;
    return std::move((*stmts)[0]);
}

// Returns true if `sql` fails to lex or parse (a hard parse error).
bool is_parse_error(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return true; // lexer error counts as a parse failure
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    return !stmts.has_value();
}

// Convenience: cast the first parsed statement to a TraverseStmt.
const TraverseStmt* as_traverse(const StmtPtr& s) {
    return dynamic_cast<const TraverseStmt*>(s.get());
}

} // namespace

// =============================================================================
// Lexer: TRACE keyword recognition (AC: TRACE is a recognized keyword token)
// =============================================================================

TEST(QA_GDB677_Lexer, TraceUppercaseTokenizedAsKeyword) {
    Lexer lexer("TRACE");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_FALSE(tokens->empty());
    EXPECT_EQ((*tokens)[0].type, TokenType::TRACE);
}

TEST(QA_GDB677_Lexer, TraceLowercaseTokenizedAsKeyword) {
    Lexer lexer("trace");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_FALSE(tokens->empty());
    EXPECT_EQ((*tokens)[0].type, TokenType::TRACE);
}

TEST(QA_GDB677_Lexer, TraceMixedCaseTokenizedAsKeyword) {
    Lexer lexer("TrAcE");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_FALSE(tokens->empty());
    EXPECT_EQ((*tokens)[0].type, TokenType::TRACE);
}

TEST(QA_GDB677_Lexer, TokenTypeNameRoundTrips) {
    EXPECT_EQ(token_type_name(TokenType::TRACE), "TRACE");
}

// A word that merely contains "trace" must NOT be lexed as the keyword.
TEST(QA_GDB677_Lexer, TraceIdSubstringNotKeyword) {
    Lexer lexer("trace_id");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_FALSE(tokens->empty());
    EXPECT_EQ((*tokens)[0].type, TokenType::IDENTIFIER);
}

// =============================================================================
// AST default + happy-path acceptance criteria
// =============================================================================

TEST(QA_GDB677_Parser, TraceDefaultsFalseNoClause) {
    auto stmt = parse_first("TRAVERSE follows FROM users(1)");
    const auto* tr = as_traverse(stmt);
    ASSERT_NE(tr, nullptr);
    EXPECT_FALSE(tr->trace);
    EXPECT_FALSE(tr->fetch);
}

TEST(QA_GDB677_Parser, WithTraceSetsTrueOnly) {
    auto stmt = parse_first("TRAVERSE follows FROM users(1) WITH TRACE");
    const auto* tr = as_traverse(stmt);
    ASSERT_NE(tr, nullptr);
    EXPECT_TRUE(tr->trace);
    EXPECT_FALSE(tr->fetch);
    EXPECT_EQ(tr->edge_type, "follows");
    EXPECT_EQ(tr->from_table, "users");
}

TEST(QA_GDB677_Parser, FetchWithTraceSetsBoth) {
    auto stmt = parse_first("TRAVERSE follows FROM users(1) FETCH WITH TRACE");
    const auto* tr = as_traverse(stmt);
    ASSERT_NE(tr, nullptr);
    EXPECT_TRUE(tr->fetch);
    EXPECT_TRUE(tr->trace);
}

TEST(QA_GDB677_Parser, WithTraceInSelectContext) {
    auto stmt = parse_first("SELECT * FROM TRAVERSE follows FROM users(1) WITH TRACE");
    const auto* sel = dynamic_cast<const SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    ASSERT_NE(sel->from[0].traverse_source, nullptr);
    const auto* tr = dynamic_cast<const TraverseStmt*>(sel->from[0].traverse_source.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_TRUE(tr->trace);
}

// WITH TRACE combined with every preceding optional clause, to ensure the
// trailing-clause position does not break the rest of the grammar.
TEST(QA_GDB677_Parser, WithTraceAfterAllOptionalClauses) {
    auto stmt = parse_first(
        "TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 3 MODE EDGES "
        "WHERE id > 0 FETCH WITH TRACE");
    const auto* tr = as_traverse(stmt);
    ASSERT_NE(tr, nullptr);
    EXPECT_TRUE(tr->trace);
    EXPECT_TRUE(tr->fetch);
}

// =============================================================================
// Case-insensitivity of the WITH TRACE clause
// =============================================================================

TEST(QA_GDB677_Parser, WithTraceLowercaseClause) {
    auto stmt = parse_first("TRAVERSE follows FROM users(1) with trace");
    const auto* tr = as_traverse(stmt);
    ASSERT_NE(tr, nullptr);
    EXPECT_TRUE(tr->trace);
}

TEST(QA_GDB677_Parser, WithTraceMixedCaseClause) {
    auto stmt = parse_first("TRAVERSE follows FROM users(1) WiTh TrAcE");
    const auto* tr = as_traverse(stmt);
    ASSERT_NE(tr, nullptr);
    EXPECT_TRUE(tr->trace);
}

TEST(QA_GDB677_Parser, WithTraceExtraWhitespaceAndNewlines) {
    auto stmt = parse_first("TRAVERSE follows FROM users(1)   WITH\n\t  TRACE");
    const auto* tr = as_traverse(stmt);
    ASSERT_NE(tr, nullptr);
    EXPECT_TRUE(tr->trace);
}

TEST(QA_GDB677_Parser, WithTraceTrailingSemicolon) {
    auto stmt = parse_first("TRAVERSE follows FROM users(1) WITH TRACE;");
    const auto* tr = as_traverse(stmt);
    ASSERT_NE(tr, nullptr);
    EXPECT_TRUE(tr->trace);
}

// =============================================================================
// Error paths (AC from GDB-683: WITH without TRACE must error)
// =============================================================================

TEST(QA_GDB677_Parser, WithoutTraceAfterWithIsError) {
    EXPECT_TRUE(is_parse_error("TRAVERSE follows FROM users(1) WITH"));
}

TEST(QA_GDB677_Parser, WithWrongKeywordAfterWithIsError) {
    EXPECT_TRUE(is_parse_error("TRAVERSE follows FROM users(1) WITH FETCH"));
}

TEST(QA_GDB677_Parser, WithIdentifierAfterWithIsError) {
    EXPECT_TRUE(is_parse_error("TRAVERSE follows FROM users(1) WITH foo"));
}

// Bare TRACE with no preceding WITH: the keyword is left dangling and must not
// be silently swallowed.
TEST(QA_GDB677_Parser, BareTraceWithoutWithIsError) {
    EXPECT_TRUE(is_parse_error("TRAVERSE follows FROM users(1) TRACE"));
}

// Two WITH TRACE clauses: the second is dangling and must error.
TEST(QA_GDB677_Parser, DoubleWithTraceIsError) {
    EXPECT_TRUE(is_parse_error("TRAVERSE follows FROM users(1) WITH TRACE WITH TRACE"));
}

// Documented grammar order is FETCH then WITH TRACE; the reverse leaves FETCH
// dangling and must error rather than parse.
TEST(QA_GDB677_Parser, WithTraceBeforeFetchWrongOrderIsError) {
    EXPECT_TRUE(is_parse_error("TRAVERSE follows FROM users(1) WITH TRACE FETCH"));
}

// =============================================================================
// CTE WITH non-conflict (AC: WITH TRACE does not conflict with CTE WITH)
// =============================================================================

TEST(QA_GDB677_Parser, CteWithStillParsesAsCte) {
    auto stmt = parse_first("WITH t AS (SELECT 1) SELECT * FROM t");
    const auto* sel = dynamic_cast<const SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->ctes.size(), 1u);
}

// A statement-leading `WITH TRACE` is a CTE context, not a traverse modifier.
// After GDB-693, TRACE is a valid name token, so `trace` is an acceptable CTE
// name and FROM table: this parses as a single CTE named `trace` rather than
// erroring. (Before GDB-693 it errored because TRACE was reserved.)
TEST(QA_GDB677_Parser, LeadingWithTraceIsCteNotModifier) {
    auto stmt = parse_first("WITH TRACE AS (SELECT 1) SELECT * FROM TRACE");
    const auto* sel = dynamic_cast<const SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->ctes.size(), 1u);
    EXPECT_EQ(sel->ctes[0].name, "TRACE");
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].name, "TRACE");
}

// CTE feeding a traverse-in-select that itself uses WITH TRACE: both the
// leading CTE-WITH and the trailing modifier-WITH must coexist.
TEST(QA_GDB677_Parser, CteWrappingTraverseWithTrace) {
    auto stmt = parse_first(
        "WITH seed AS (SELECT 1) "
        "SELECT * FROM TRAVERSE follows FROM users(1) WITH TRACE");
    const auto* sel = dynamic_cast<const SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->ctes.size(), 1u);
    ASSERT_EQ(sel->from.size(), 1u);
    ASSERT_NE(sel->from[0].traverse_source, nullptr);
    const auto* tr = dynamic_cast<const TraverseStmt*>(sel->from[0].traverse_source.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_TRUE(tr->trace);
}

// =============================================================================
// Multiple statements: the trace flag is per-statement, not sticky.
// =============================================================================

TEST(QA_GDB677_Parser, PerStatementTraceFlagIsIndependent) {
    Lexer lexer("TRAVERSE follows FROM users(1) WITH TRACE; TRAVERSE follows FROM users(2)");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    ASSERT_TRUE(stmts.has_value()) << stmts.error().message;
    ASSERT_EQ(stmts->size(), 2u);
    const auto* a = dynamic_cast<const TraverseStmt*>((*stmts)[0].get());
    const auto* b = dynamic_cast<const TraverseStmt*>((*stmts)[1].get());
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_TRUE(a->trace);
    EXPECT_FALSE(b->trace);
}

// =============================================================================
// REGRESSION (resolved by GDB-693): `trace` is usable as an identifier again.
//
// GDB-677 added TRACE to the lexer but omitted it from parser.cpp
// is_name_token(), making the bare word `trace` a fully reserved keyword and
// breaking previously-valid SQL. These tests originally characterized that
// regression (asserting a parse error). GDB-693 added `case TokenType::TRACE:`
// to is_name_token(), restoring `trace` as a valid table/column/edge name, so
// the expectations below were flipped to success per the GDB-693 ticket. The
// exhaustive identifier-context coverage now lives in test_qa_gdb_693.cpp; the
// cases retained here guard against this specific regression returning.
// =============================================================================

TEST(QA_GDB677_Regression, TraceAsTableNameAccepted) {
    // Previously regressed to a parse error; restored by GDB-693.
    EXPECT_FALSE(is_parse_error("CREATE TABLE trace (id INT)"));
}

TEST(QA_GDB677_Regression, TraceAsColumnNameAccepted) {
    EXPECT_FALSE(is_parse_error("CREATE TABLE t (trace INT)"));
}

TEST(QA_GDB677_Regression, TraceAsSelectColumnAccepted) {
    EXPECT_FALSE(is_parse_error("SELECT trace FROM events"));
}

TEST(QA_GDB677_Regression, TraceAsEdgeTypeAccepted) {
    // TRAVERSE trace FROM users(1): edge type goes through parse_name(), which
    // now accepts TRACE as a name token.
    EXPECT_FALSE(is_parse_error("TRAVERSE trace FROM users(1)"));
}

// Control: the analogous FETCH modifier keyword is also usable as an
// identifier — `trace` now matches its (correct) behavior.
TEST(QA_GDB677_Regression, FetchAsTableNameStillAccepted) {
    EXPECT_FALSE(is_parse_error("CREATE TABLE fetch (id INT)"));
}
