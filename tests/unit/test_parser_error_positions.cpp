// GDB-1208: Several direct make_error() call sites in the expression-context
// parser (MATCH/NEAREST predicates, EMBEDDING type-spec parameters, and
// safe_stoi callers) omitted the "at line X, column Y" position info that the
// rest of the parser (error()/expect()/parse_name()) attaches consistently.
//
// These tests assert that the fixed sites now include position info in their
// error messages, matching the format used elsewhere in the parser.

#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"

#include <gtest/gtest.h>

using namespace sixseven;

namespace {

// Parses `sql` expecting a parse failure, and returns the error message.
std::string parse_error_message(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens) {
        return tokens.error().message;
    }
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_FALSE(stmts.has_value()) << "expected parse error for: " << sql;
    if (stmts.has_value()) {
        return {};
    }
    return stmts.error().message;
}

} // namespace

// -- MATCH(column) missing TO --------------------------------------------------

TEST(ParserErrorPositions, MatchMissingToHasPosition) {
    auto msg = parse_error_message("SELECT * FROM t WHERE MATCH(body) 'query'");
    EXPECT_NE(msg.find("expected TO after MATCH(column)"), std::string::npos) << msg;
    EXPECT_NE(msg.find("line"), std::string::npos) << msg;
    EXPECT_NE(msg.find("column"), std::string::npos) << msg;
}

// -- NEAREST(column, k) missing TO ---------------------------------------------

TEST(ParserErrorPositions, NearestMissingToHasPosition) {
    auto msg = parse_error_message("SELECT * FROM t WHERE NEAREST(embedding, 5) [1, 2, 3]");
    EXPECT_NE(msg.find("expected TO after NEAREST(column, k)"), std::string::npos) << msg;
    EXPECT_NE(msg.find("line"), std::string::npos) << msg;
    EXPECT_NE(msg.find("column"), std::string::npos) << msg;
}

// -- EMBEDDING named-parameter errors -------------------------------------------

TEST(ParserErrorPositions, EmbeddingDuplicateSourceHasPosition) {
    auto msg = parse_error_message("CREATE TABLE t (e EMBEDDING(384, source='a', source='b'))");
    EXPECT_NE(msg.find("duplicate 'source' parameter"), std::string::npos) << msg;
    EXPECT_NE(msg.find("line"), std::string::npos) << msg;
    EXPECT_NE(msg.find("column"), std::string::npos) << msg;
}

TEST(ParserErrorPositions, EmbeddingDuplicateProviderHasPosition) {
    auto msg = parse_error_message("CREATE TABLE t (e EMBEDDING(384, provider='a', provider='b'))");
    EXPECT_NE(msg.find("duplicate 'provider' parameter"), std::string::npos) << msg;
    EXPECT_NE(msg.find("line"), std::string::npos) << msg;
    EXPECT_NE(msg.find("column"), std::string::npos) << msg;
}

TEST(ParserErrorPositions, EmbeddingUnknownParameterHasPosition) {
    auto msg = parse_error_message("CREATE TABLE t (e EMBEDDING(384, bogus='a'))");
    EXPECT_NE(msg.find("unknown EMBEDDING parameter"), std::string::npos) << msg;
    EXPECT_NE(msg.find("line"), std::string::npos) << msg;
    EXPECT_NE(msg.find("column"), std::string::npos) << msg;
}

// Note: the "EMBEDDING missing required 'source'/'provider' parameter" checks
// in parser.cpp are currently unreachable in practice -- the named-parameter
// loop always consumes exactly two comma-separated params, and each one
// either sets source/provider, reports a duplicate, or reports unknown, so
// got_source/got_provider are always true by the time the loop exits. They
// are left annotated with position (via parse_error_here) for consistency
// and defense-in-depth, but are not covered here since no input reaches them.

// -- safe_stoi callers -----------------------------------------------------

TEST(ParserErrorPositions, EmbeddingDimensionOutOfRangeHasPosition) {
    // A dimension literal that overflows int triggers safe_stoi's out-of-range
    // error; the call site must annotate it with position.
    auto msg = parse_error_message(
        "CREATE TABLE t (e EMBEDDING(99999999999999999999, source='a', provider='b'))");
    EXPECT_NE(msg.find("out of range"), std::string::npos) << msg;
    EXPECT_NE(msg.find("line"), std::string::npos) << msg;
    EXPECT_NE(msg.find("column"), std::string::npos) << msg;
}

TEST(ParserErrorPositions, VarcharParamOutOfRangeHasPosition) {
    auto msg = parse_error_message("CREATE TABLE t (s VARCHAR(99999999999999999999))");
    EXPECT_NE(msg.find("out of range"), std::string::npos) << msg;
    EXPECT_NE(msg.find("line"), std::string::npos) << msg;
    EXPECT_NE(msg.find("column"), std::string::npos) << msg;
}

TEST(ParserErrorPositions, DecimalSecondParamOutOfRangeHasPosition) {
    auto msg = parse_error_message("CREATE TABLE t (d DECIMAL(10, 99999999999999999999))");
    EXPECT_NE(msg.find("out of range"), std::string::npos) << msg;
    EXPECT_NE(msg.find("line"), std::string::npos) << msg;
    EXPECT_NE(msg.find("column"), std::string::npos) << msg;
}

TEST(ParserErrorPositions, ParamRefOutOfRangeHasPosition) {
    auto msg = parse_error_message("SELECT * FROM t WHERE id = $99999999999999999999");
    EXPECT_NE(msg.find("out of range"), std::string::npos) << msg;
    EXPECT_NE(msg.find("line"), std::string::npos) << msg;
    EXPECT_NE(msg.find("column"), std::string::npos) << msg;
}

TEST(ParserErrorPositions, TraverseMaxDepthOutOfRangeHasPosition) {
    auto msg = parse_error_message(
        "TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 99999999999999999999 FETCH");
    EXPECT_NE(msg.find("out of range"), std::string::npos) << msg;
    EXPECT_NE(msg.find("line"), std::string::npos) << msg;
    EXPECT_NE(msg.find("column"), std::string::npos) << msg;
}
