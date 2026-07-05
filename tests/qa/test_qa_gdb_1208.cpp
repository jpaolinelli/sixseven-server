// QA regression / adversarial tests for GDB-1208
// "Several parse errors omit line/column position info, inconsistent with the
// rest of the parser."
//
// This file exercises every fixed call site with deterministic malformed SQL,
// checks the resulting error message contains an "at line X, column Y" clause
// in the same format as neighboring errors, and — critically — checks the
// reported position is *accurate* on multi-line / multi-statement input, not
// a stale/constant value.

#include "sixseven/parser/parser.h"
#include "sixseven/parser/lexer.h"

#include <gtest/gtest.h>

#include <regex>
#include <string>

namespace sixseven {
namespace {

// Parses `sql` and returns the error message of the first failing statement.
// Fails the calling test (via ADD_FAILURE) if parsing unexpectedly succeeds.
std::string parse_expect_error(const std::string& sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens) {
        return tokens.error().message;
    }
    Parser parser(*tokens);
    auto result = parser.parse();
    if (result.has_value()) {
        ADD_FAILURE() << "expected parse error, but parsing succeeded for: " << sql;
        return {};
    }
    return result.error().message;
}

// Same as parse_expect_error but parses ALL statements in a script via
// parse_all() — needed for multi-statement scripts where the first
// statement(s) are valid and the error is buried in a later statement.
std::string parse_all_expect_error(const std::string& sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens) {
        return tokens.error().message;
    }
    Parser parser(*tokens);
    auto result = parser.parse_all();
    if (result.has_value()) {
        ADD_FAILURE() << "expected parse error, but parsing succeeded for: " << sql;
        return {};
    }
    return result.error().message;
}

// Extracts (line, column) from a message formatted as "... at line L, column C".
// Returns {-1, -1} if the pattern is not found.
std::pair<int, int> extract_position(const std::string& message) {
    static const std::regex re(R"(at line (\d+), column (\d+))");
    std::smatch m;
    if (std::regex_search(message, m, re)) {
        return {std::stoi(m[1].str()), std::stoi(m[2].str())};
    }
    return {-1, -1};
}

bool has_position(const std::string& message) {
    auto [line, col] = extract_position(message);
    return line != -1 && col != -1;
}

// Counts how many times the "at line" clause appears — must be exactly 1
// (no double-annotation / message corruption).
int count_position_clauses(const std::string& message) {
    static const std::regex re(R"(at line \d+, column \d+)");
    auto begin = std::sregex_iterator(message.begin(), message.end(), re);
    auto end = std::sregex_iterator();
    return static_cast<int>(std::distance(begin, end));
}

}  // namespace

// ---------------------------------------------------------------------------
// (1) Every fixed site now reports a position, in the standard format.
// ---------------------------------------------------------------------------

TEST(QA_GDB1208, MatchMissingToReportsPosition) {
    auto msg = parse_expect_error("SELECT * FROM t WHERE MATCH(col) 'query';");
    EXPECT_NE(msg.find("expected TO after MATCH(column)"), std::string::npos);
    EXPECT_TRUE(has_position(msg)) << "message: " << msg;
    EXPECT_EQ(count_position_clauses(msg), 1) << "message: " << msg;
}

TEST(QA_GDB1208, NearestMissingToReportsPosition) {
    auto msg = parse_expect_error("SELECT * FROM t WHERE NEAREST(col, 5) [1,2,3];");
    EXPECT_NE(msg.find("expected TO after NEAREST(column, k)"), std::string::npos);
    EXPECT_TRUE(has_position(msg)) << "message: " << msg;
    EXPECT_EQ(count_position_clauses(msg), 1) << "message: " << msg;
}

// Named-parameter EMBEDDING syntax is always EMBEDDING(dim, name=val, name=val)
// -- the dimension is mandatory and always first, even in named-param form.

TEST(QA_GDB1208, EmbeddingDuplicateSourceReportsPosition) {
    auto msg = parse_expect_error(
        "CREATE TABLE t (v EMBEDDING(384, source='a', source='b'));");
    EXPECT_NE(msg.find("duplicate 'source' parameter"), std::string::npos) << msg;
    EXPECT_TRUE(has_position(msg)) << "message: " << msg;
    EXPECT_EQ(count_position_clauses(msg), 1) << "message: " << msg;
}

// BUG (see QA findings): parse_error_here() is called in the named-EMBEDDING
// parameter loop AFTER the offending "source='b'" token has already been
// fully consumed via expect(). By the time parse_error_here() calls peek(),
// the cursor sits on the NEXT token (the closing ')'), so the reported
// position is off by one token -- not "poor accuracy" but flatly wrong: it
// points at unrelated punctuation, not the duplicate parameter itself.
// This test pins the CURRENT (buggy) behavior so a regression (or a fix) is
// visible; see EmbeddingDuplicateSourcePositionPointsPastOffendingToken.
TEST(QA_GDB1208, EmbeddingDuplicateSourcePositionPointsPastOffendingToken) {
    std::string sql = "CREATE TABLE t (v EMBEDDING(384, source='a', source='b'));";
    auto msg = parse_expect_error(sql);
    auto [line, col] = extract_position(msg);
    // The offending token "source='b'" starts at 1-based column 46.
    // The reported column is 56, which is the closing ')' -- one token past
    // the actual error location.
    EXPECT_EQ(line, 1);
    EXPECT_NE(col, 46) << "if this now equals 46, the position bug has been "
                          "fixed and this test should be updated to assert "
                          "col == 46 instead; message: "
                       << msg;
}

TEST(QA_GDB1208, EmbeddingDuplicateProviderReportsPosition) {
    auto msg = parse_expect_error(
        "CREATE TABLE t (v EMBEDDING(384, provider='openai', provider='cohere'));");
    EXPECT_NE(msg.find("duplicate 'provider' parameter"), std::string::npos) << msg;
    EXPECT_TRUE(has_position(msg)) << "message: " << msg;
    EXPECT_EQ(count_position_clauses(msg), 1) << "message: " << msg;
}

TEST(QA_GDB1208, EmbeddingUnknownParameterReportsPosition) {
    auto msg = parse_expect_error(
        "CREATE TABLE t (v EMBEDDING(384, source='a', bogus='x'));");
    EXPECT_NE(msg.find("unknown EMBEDDING parameter"), std::string::npos) << msg;
    EXPECT_TRUE(has_position(msg)) << "message: " << msg;
    EXPECT_EQ(count_position_clauses(msg), 1) << "message: " << msg;
}

TEST(QA_GDB1208, EmbeddingMissingSourceReportsPosition) {
    // Both named params filled with "provider" style names (using bogus as the
    // second slot would trip "unknown parameter" first) -- to genuinely reach
    // the post-loop "missing required source" check we need both loop
    // iterations to fill 'provider' twice, which itself is a duplicate error.
    // Instead: force provider only + an unknown second key, which errors
    // earlier (unknown parameter) -- so this path is only reachable with
    // exactly one named param when the parser allows it. Given the loop is
    // hardcoded to exactly 2 iterations, we can't hit "missing source" via a
    // single provider-only param list; use provider twice avoided by testing
    // the analogous provider-missing path below, and document this asymmetry
    // as a QA finding rather than assert on it here.
    GTEST_SKIP() << "EMBEDDING named-param loop is hardcoded to exactly 2 "
                    "iterations; 'missing required source' is unreachable "
                    "with a single-parameter list without hitting an earlier "
                    "error first. See QA notes.";
}

TEST(QA_GDB1208, EmbeddingMissingProviderReportsPosition) {
    GTEST_SKIP() << "See EmbeddingMissingSourceReportsPosition note: the "
                    "post-loop 'missing required provider' branch has the "
                    "same reachability caveat.";
}

TEST(QA_GDB1208, EmbeddingOutOfRangeDimensionReportsPosition) {
    // Overflows a 32-bit int parse in safe_stoi.
    auto msg = parse_expect_error(
        "CREATE TABLE t (v EMBEDDING(99999999999999999999, 'src', 'openai'));");
    EXPECT_TRUE(has_position(msg)) << "message: " << msg;
    EXPECT_EQ(count_position_clauses(msg), 1) << "message: " << msg;
}

TEST(QA_GDB1208, VarcharOutOfRangeLengthReportsPosition) {
    auto msg = parse_expect_error(
        "CREATE TABLE t (v VARCHAR(99999999999999999999));");
    EXPECT_TRUE(has_position(msg)) << "message: " << msg;
}

TEST(QA_GDB1208, DecimalOutOfRangePrecisionReportsPosition) {
    auto msg = parse_expect_error(
        "CREATE TABLE t (v DECIMAL(99999999999999999999, 2));");
    EXPECT_TRUE(has_position(msg)) << "message: " << msg;
}

TEST(QA_GDB1208, DecimalOutOfRangeScaleReportsPosition) {
    auto msg = parse_expect_error(
        "CREATE TABLE t (v DECIMAL(10, 99999999999999999999));");
    EXPECT_TRUE(has_position(msg)) << "message: " << msg;
}

TEST(QA_GDB1208, ShortestKOutOfRangeValueReportsPosition) {
    auto msg = parse_expect_error(
        "SELECT * FROM MATCH p = SHORTEST 99999999999999999999 (a)-[e]->(b);");
    EXPECT_TRUE(has_position(msg)) << "message: " << msg;
}

TEST(QA_GDB1208, MaxDepthOutOfRangeValueReportsPosition) {
    auto msg = parse_expect_error(
        "TRAVERSE edge FROM n(1) MAX_DEPTH 99999999999999999999;");
    EXPECT_TRUE(has_position(msg)) << "message: " << msg;
}

TEST(QA_GDB1208, HopQuantifierMinOutOfRangeValueReportsPosition) {
    auto msg = parse_expect_error(
        "SELECT * FROM MATCH (a)-[e*99999999999999999999..5]->(b);");
    EXPECT_TRUE(has_position(msg)) << "message: " << msg;
}

TEST(QA_GDB1208, HopQuantifierMaxOutOfRangeValueReportsPosition) {
    auto msg = parse_expect_error(
        "SELECT * FROM MATCH (a)-[e*1..99999999999999999999]->(b);");
    EXPECT_TRUE(has_position(msg)) << "message: " << msg;
}

TEST(QA_GDB1208, ParamRefBadIndexReportsPosition) {
    auto msg = parse_expect_error(
        "SELECT * FROM t WHERE col = $99999999999999999999;");
    EXPECT_TRUE(has_position(msg)) << "message: " << msg;
    EXPECT_EQ(count_position_clauses(msg), 1) << "message: " << msg;
}

// ---------------------------------------------------------------------------
// (2) Position ACCURACY on multi-line input — the core regression-hunt.
// A stale/constant "line 1, column 1" (or any position that doesn't track
// the actual offending line) is the most likely real bug in this ticket.
// ---------------------------------------------------------------------------

TEST(QA_GDB1208, MatchMissingToMultiLineReportsCorrectLine) {
    std::string sql =
        "SELECT *\n"
        "FROM t\n"
        "WHERE MATCH(col) 'query';\n";
    auto msg = parse_expect_error(sql);
    auto [line, col] = extract_position(msg);
    EXPECT_EQ(line, 3) << "message: " << msg;
    EXPECT_GT(col, 1) << "message: " << msg;  // should point at/near 'query' token, not col 1
}

TEST(QA_GDB1208, NearestMissingToMultiLineReportsCorrectLine) {
    std::string sql =
        "SELECT *\n"
        "FROM t\n"
        "WHERE\n"
        "NEAREST(col, 5) [1,2,3];\n";
    auto msg = parse_expect_error(sql);
    auto [line, col] = extract_position(msg);
    EXPECT_EQ(line, 4) << "message: " << msg;
    (void)col;
}

// NOTE: These pin the CURRENT (buggy) behavior -- see
// EmbeddingDuplicateSourcePositionPointsPastOffendingToken and the QA
// findings. The offending "source='b'" / "bogus='x'" token is on line 5, but
// because parse_error_here() runs after the token is already consumed, the
// parser cursor -- and therefore the reported position -- has moved to the
// ')' on line 6. A correct implementation would report line 5.
TEST(QA_GDB1208, EmbeddingDuplicateSourceMultiLineReportsWrongLineBug) {
    std::string sql =
        "CREATE TABLE t (\n"
        "  v EMBEDDING(\n"
        "    384,\n"
        "    source='a',\n"
        "    source='b'\n"
        "  )\n"
        ");\n";
    auto msg = parse_expect_error(sql);
    auto [line, col] = extract_position(msg);
    EXPECT_EQ(line, 6) << "actual offending token is on line 5 -- this "
                          "documents the current off-by-one-token bug; "
                          "message: "
                       << msg;
    (void)col;
}

TEST(QA_GDB1208, EmbeddingUnknownParameterMultiLineReportsWrongLineBug) {
    std::string sql =
        "CREATE TABLE t (\n"
        "  v EMBEDDING(\n"
        "    384,\n"
        "    source='a',\n"
        "    bogus='x'\n"
        "  )\n"
        ");\n";
    auto msg = parse_expect_error(sql);
    auto [line, col] = extract_position(msg);
    EXPECT_EQ(line, 6) << "actual offending token is on line 5 -- this "
                          "documents the current off-by-one-token bug; "
                          "message: "
                       << msg;
    (void)col;
}

TEST(QA_GDB1208, EmbeddingBadDimensionMultiLineReportsCorrectLine) {
    std::string sql =
        "CREATE TABLE t (\n"
        "  v EMBEDDING(\n"
        "    99999999999999999999,\n"
        "    'src',\n"
        "    'openai'\n"
        "  )\n"
        ");\n";
    auto msg = parse_expect_error(sql);
    auto [line, col] = extract_position(msg);
    EXPECT_EQ(line, 3) << "message: " << msg;
    (void)col;
}

TEST(QA_GDB1208, MultiStatementScriptReportsLineOfSecondStatement) {
    // First statement is valid; the error is deep in the second statement,
    // on line 3 of a 4-line script. A wrong/constant position (e.g. line 1)
    // would be a real poor-UX finding. Uses parse_all() since parse() only
    // parses a single top-level statement.
    std::string sql =
        "SELECT 1;\n"
        "SELECT * FROM t\n"
        "WHERE MATCH(col) 'q';\n";
    auto msg = parse_all_expect_error(sql);
    auto [line, col] = extract_position(msg);
    EXPECT_EQ(line, 3) << "message: " << msg;
    (void)col;
}

TEST(QA_GDB1208, ParamRefBadIndexMultiLineReportsCorrectLine) {
    std::string sql =
        "SELECT *\n"
        "FROM t\n"
        "WHERE col = $99999999999999999999;\n";
    auto msg = parse_expect_error(sql);
    auto [line, col] = extract_position(msg);
    EXPECT_EQ(line, 3) << "message: " << msg;
    (void)col;
}

TEST(QA_GDB1208, VarcharBadLengthMultiLineReportsCorrectLine) {
    std::string sql =
        "CREATE TABLE t (\n"
        "  a INT,\n"
        "  v VARCHAR(99999999999999999999)\n"
        ");\n";
    auto msg = parse_expect_error(sql);
    auto [line, col] = extract_position(msg);
    EXPECT_EQ(line, 3) << "message: " << msg;
    (void)col;
}

// ---------------------------------------------------------------------------
// (3) No crash / no regression on malformed input; format matches neighbors.
// ---------------------------------------------------------------------------

TEST(QA_GDB1208, NoCrashOnEmptyEmbeddingParams) {
    // Should error cleanly, not crash, even with completely empty parens.
    auto msg = parse_expect_error("CREATE TABLE t (v EMBEDDING());");
    EXPECT_FALSE(msg.empty());
}

TEST(QA_GDB1208, NoCrashOnTruncatedMatchExpression) {
    auto msg = parse_expect_error("SELECT * FROM t WHERE MATCH(col)");
    EXPECT_FALSE(msg.empty());
}

TEST(QA_GDB1208, NoCrashOnTruncatedNearestExpression) {
    auto msg = parse_expect_error("SELECT * FROM t WHERE NEAREST(col, 5)");
    EXPECT_FALSE(msg.empty());
}

TEST(QA_GDB1208, NoCrashOnDollarAloneAsParamRef) {
    auto msg = parse_expect_error("SELECT * FROM t WHERE col = $;");
    EXPECT_FALSE(msg.empty());
}

TEST(QA_GDB1208, ErrorMessageFormatMatchesExpectHelperStyle) {
    // Compare the "at line X, column Y" suffix style against a known
    // preexisting error() call (parse_name / expect) to confirm uniformity.
    auto new_style_msg = parse_expect_error("SELECT * FROM t WHERE MATCH(col) 'q';");
    auto existing_style_msg = parse_expect_error("SELECT * FROM;");  // triggers parse_name/expect

    static const std::regex re(R"(at line \d+, column \d+$)");
    EXPECT_TRUE(std::regex_search(new_style_msg, re)) << new_style_msg;
    // existing_style_msg may have trailing "(got ...)" from parse_name, so
    // just confirm it also contains the same "at line X, column Y" clause.
    EXPECT_TRUE(has_position(existing_style_msg)) << existing_style_msg;
}

// ---------------------------------------------------------------------------
// (4) Valid statements must be unaffected — the change is diagnostics-only.
// ---------------------------------------------------------------------------

TEST(QA_GDB1208, ValidMatchStatementStillParses) {
    Lexer lexer("SELECT * FROM t WHERE MATCH(col) TO 'query';");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(*tokens);
    auto result = parser.parse();
    EXPECT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error().message);
}

TEST(QA_GDB1208, ValidEmbeddingDeclarationStillParses) {
    Lexer lexer(
        "CREATE TABLE t (v EMBEDDING(384, source='a', provider='openai'));");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(*tokens);
    auto result = parser.parse();
    EXPECT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error().message);
}

TEST(QA_GDB1208, ValidVarcharDeclarationStillParses) {
    Lexer lexer("CREATE TABLE t (v VARCHAR(255));");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(*tokens);
    auto result = parser.parse();
    EXPECT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error().message);
}

TEST(QA_GDB1208, ValidParamRefStillParses) {
    Lexer lexer("SELECT * FROM t WHERE col = $1;");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(*tokens);
    auto result = parser.parse();
    EXPECT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error().message);
}

}  // namespace sixseven
