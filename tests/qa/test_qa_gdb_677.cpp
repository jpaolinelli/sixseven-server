/// @file test_qa_gdb_677.cpp
/// @brief QA adversarial tests for GDB-677: parse `WITH TRACE` on TRAVERSE.
///
/// Exercises the lexer keyword, the TraverseStmt.trace AST flag, and the
/// parse_traverse() handling of the optional `WITH TRACE` clause. Covers the
/// story's acceptance criteria plus adversarial edge cases: case-insensitivity,
/// error paths (WITH without TRACE), clause ordering, CTE-WITH non-conflict,
/// trailing tokens, and the reserved-keyword backward-compatibility regression
/// introduced by making TRACE a keyword.

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

// A statement-leading `WITH TRACE` is a CTE context, not a traverse modifier;
// TRACE is not a valid CTE name, so this must error (and must not crash).
TEST(QA_GDB677_Parser, LeadingWithTraceIsCteErrorNotModifier) {
    EXPECT_TRUE(is_parse_error("WITH TRACE AS (SELECT 1) SELECT * FROM TRACE"));
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
// REGRESSION: TRACE is now a fully reserved word.
//
// Before this change, `trace` lexed as an IDENTIFIER and was usable as a
// table/column/edge name. The keyword was added to the lexer but NOT to
// parser.cpp is_name_token() (unlike the analogous traverse modifier FETCH,
// which *is* listed there). As a result previously-valid SQL that uses the
// bare word `trace` as an identifier no longer parses. These tests characterize
// the CURRENT (regressed) behavior; see the filed bug ticket. If TRACE is added
// to is_name_token() the expectations below should flip to success.
// =============================================================================

TEST(QA_GDB677_Regression, TraceAsTableNameRejected) {
    // Previously valid: CREATE TABLE trace (...). Now a parse error.
    EXPECT_TRUE(is_parse_error("CREATE TABLE trace (id INT)"));
}

TEST(QA_GDB677_Regression, TraceAsColumnNameRejected) {
    EXPECT_TRUE(is_parse_error("CREATE TABLE t (trace INT)"));
}

TEST(QA_GDB677_Regression, TraceAsSelectColumnRejected) {
    EXPECT_TRUE(is_parse_error("SELECT trace FROM events"));
}

TEST(QA_GDB677_Regression, TraceAsEdgeTypeRejected) {
    // TRAVERSE trace FROM users(1): edge type goes through parse_name(), which
    // rejects the now-reserved TRACE keyword.
    EXPECT_TRUE(is_parse_error("TRAVERSE trace FROM users(1)"));
}

// Control: the analogous FETCH modifier keyword *is* usable as an identifier,
// demonstrating the inconsistency TRACE introduces.
TEST(QA_GDB677_Regression, FetchAsTableNameStillAccepted) {
    EXPECT_FALSE(is_parse_error("CREATE TABLE fetch (id INT)"));
}
