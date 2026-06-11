#include "sixseven/parser/lexer.h"
#include "sixseven/parser/token.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace sixseven;

// -- Helper -------------------------------------------------------------------

static std::vector<Token> tokenize_ok(std::string_view sql) {
    Lexer lexer(sql);
    auto result = lexer.tokenize();
    EXPECT_TRUE(result.has_value()) << result.error().message;
    return result.has_value() ? std::move(*result) : std::vector<Token>{};
}

static Error tokenize_err(std::string_view sql) {
    Lexer lexer(sql);
    auto result = lexer.tokenize();
    EXPECT_FALSE(result.has_value()) << "Expected error but got success";
    return result.has_value() ? Error{StatusCode::INTERNAL_ERROR, "unexpected success"}
                              : result.error();
}

// =============================================================================
// Number parsing edge cases
// =============================================================================

TEST(QA_Lexer, MultipleDots) {
    // "1.2.3" → FLOAT(1.2), FLOAT(.3)
    auto tokens = tokenize_ok("1.2.3");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "1.2");
    EXPECT_EQ(tokens[1].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[1].lexeme, ".3");
}

TEST(QA_Lexer, DotAlone) {
    // A lone "." is a DOT token.
    auto tokens = tokenize_ok(".");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::DOT);
}

TEST(QA_Lexer, LeadingZeros) {
    // "007" is a valid integer literal (lexer doesn't enforce no-leading-zeros).
    auto tokens = tokenize_ok("007");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "007");
}

TEST(QA_Lexer, NumberFollowedByIdentifier) {
    // "123abc" → INTEGER(123) IDENTIFIER(abc)
    auto tokens = tokenize_ok("123abc");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "123");
    EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].lexeme, "abc");
}

TEST(QA_Lexer, FloatFollowedByIdentifier) {
    // "1.5abc" → FLOAT(1.5) IDENTIFIER(abc)
    auto tokens = tokenize_ok("1.5abc");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "1.5");
    EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].lexeme, "abc");
}

TEST(QA_Lexer, DotBetweenNumberAndKeyword) {
    // "1.name" → INTEGER(1) DOT IDENTIFIER(name)
    // The dot is NOT part of the number because next char is an ident start.
    auto tokens = tokenize_ok("1.name");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "1");
    EXPECT_EQ(tokens[1].type, TokenType::DOT);
    EXPECT_EQ(tokens[2].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[2].lexeme, "name");
}

TEST(QA_Lexer, VeryLargeInteger) {
    // Lexer does not validate integer range — that's the parser/evaluator's job.
    std::string big = "99999999999999999999999999999999";
    auto tokens = tokenize_ok(big);
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, big);
}

TEST(QA_Lexer, ScientificPositiveExponent) {
    auto tokens = tokenize_ok("1.5e+10");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "1.5e+10");
}

TEST(QA_Lexer, ScientificUpperCaseE) {
    auto tokens = tokenize_ok("3.14E2");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "3.14E2");
}

TEST(QA_Lexer, IntegerScientificPositiveSign) {
    auto tokens = tokenize_ok("5E+3");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "5E+3");
}

TEST(QA_Lexer, FloatTrailingDotWithExponent) {
    // "5.e3" — dot followed by 'e' (ident start), so dot is NOT consumed.
    // → INTEGER(5), DOT, IDENTIFIER(e3)
    auto tokens = tokenize_ok("5.e3");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "5");
    EXPECT_EQ(tokens[1].type, TokenType::DOT);
    EXPECT_EQ(tokens[2].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[2].lexeme, "e3");
}

TEST(QA_Lexer, ZeroFloat) {
    auto tokens = tokenize_ok("0.0");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "0.0");
}

TEST(QA_Lexer, FloatLeadingDotZero) {
    auto tokens = tokenize_ok(".0");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, ".0");
}

TEST(QA_Lexer, MalformedExponentMinusNoDigits) {
    // "1.5e-" → FLOAT(1.5), IDENTIFIER(e), MINUS
    auto tokens = tokenize_ok("1.5e-");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "1.5");
    EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].lexeme, "e");
    EXPECT_EQ(tokens[2].type, TokenType::MINUS);
}

TEST(QA_Lexer, MalformedExponentFollowedByLetter) {
    // "1.5e+a" → FLOAT(1.5), IDENTIFIER(e), PLUS, IDENTIFIER(a)
    auto tokens = tokenize_ok("1.5e+a");
    ASSERT_GE(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "1.5");
    EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].lexeme, "e");
    EXPECT_EQ(tokens[2].type, TokenType::PLUS);
    EXPECT_EQ(tokens[3].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[3].lexeme, "a");
}

// =============================================================================
// String parsing edge cases
// =============================================================================

TEST(QA_Lexer, StringWithOnlyEscapedQuotes) {
    // '''' → STRING_LITERAL containing a single escaped quote.
    auto tokens = tokenize_ok("''''");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "''''");
}

TEST(QA_Lexer, StringWithDoubleEscapedQuotes) {
    // '''''' → STRING containing two single quotes.
    auto tokens = tokenize_ok("''''''");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "''''''");
}

TEST(QA_Lexer, OddQuotesUnterminated) {
    // ''''' (5 quotes) → unterminated string
    auto err = tokenize_err("'''''");
    EXPECT_EQ(err.code, StatusCode::PARSE_ERROR);
}

TEST(QA_Lexer, StringWithSpecialChars) {
    auto tokens = tokenize_ok("'hello\\nworld'");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING_LITERAL);
    // Backslash is a regular character in SQL strings (not an escape).
    EXPECT_EQ(tokens[0].lexeme, "'hello\\nworld'");
}

TEST(QA_Lexer, StringContainingCommentSyntax) {
    // Comment syntax inside a string should NOT be treated as a comment.
    auto tokens = tokenize_ok("'/* not a comment */'");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "'/* not a comment */'");
}

TEST(QA_Lexer, StringContainingDashDash) {
    // -- inside a string is NOT a line comment.
    auto tokens = tokenize_ok("'hello -- world'");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "'hello -- world'");
}

TEST(QA_Lexer, StringContainingKeywords) {
    auto tokens = tokenize_ok("'SELECT FROM WHERE'");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING_LITERAL);
}

TEST(QA_Lexer, VeryLongString) {
    // 10000 character string.
    std::string long_str = "'";
    long_str.append(10000, 'x');
    long_str += "'";
    auto tokens = tokenize_ok(long_str);
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING_LITERAL);
    EXPECT_EQ(tokens[0].lexeme.size(), 10002u); // includes quotes
}

TEST(QA_Lexer, ConsecutiveStrings) {
    auto tokens = tokenize_ok("'a' 'b' 'c'");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "'a'");
    EXPECT_EQ(tokens[1].type, TokenType::STRING_LITERAL);
    EXPECT_EQ(tokens[1].lexeme, "'b'");
    EXPECT_EQ(tokens[2].type, TokenType::STRING_LITERAL);
    EXPECT_EQ(tokens[2].lexeme, "'c'");
}

TEST(QA_Lexer, UnterminatedStringAtLineEnd) {
    auto err = tokenize_err("SELECT 'unterminated\n");
    EXPECT_EQ(err.code, StatusCode::PARSE_ERROR);
    EXPECT_NE(err.message.find("unterminated string"), std::string::npos);
}

TEST(QA_Lexer, StringFollowedByOperator) {
    auto tokens = tokenize_ok("'hello'||'world'");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "'hello'");
    EXPECT_EQ(tokens[1].type, TokenType::PIPE_PIPE);
    EXPECT_EQ(tokens[2].type, TokenType::STRING_LITERAL);
    EXPECT_EQ(tokens[2].lexeme, "'world'");
}

// =============================================================================
// Comment edge cases
// =============================================================================

TEST(QA_Lexer, EmptyBlockComment) {
    auto tokens = tokenize_ok("/**/");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::END_OF_FILE);
}

TEST(QA_Lexer, BlockCommentWithStars) {
    auto tokens = tokenize_ok("/*** stars ***/");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::END_OF_FILE);
}

TEST(QA_Lexer, DeeplyNestedBlockComments) {
    auto tokens = tokenize_ok("/* /* /* deep */ */ */");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::END_OF_FILE);
}

TEST(QA_Lexer, UnterminatedNestedComment) {
    // Outer comment closes but inner doesn't have proper nesting.
    auto err = tokenize_err("/* /* only one close */");
    EXPECT_EQ(err.code, StatusCode::PARSE_ERROR);
    EXPECT_NE(err.message.find("unterminated block comment"), std::string::npos);
}

TEST(QA_Lexer, ConsecutiveBlockComments) {
    auto tokens = tokenize_ok("/**/ /**/ SELECT");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
}

TEST(QA_Lexer, LineCommentOnly) {
    auto tokens = tokenize_ok("-- just a comment");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::END_OF_FILE);
}

TEST(QA_Lexer, MultipleLineComments) {
    auto tokens = tokenize_ok("-- first\n-- second\nSELECT");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[0].line, 3u);
}

TEST(QA_Lexer, BlockCommentInsideLineComment) {
    // The /* inside a line comment should be ignored.
    auto tokens = tokenize_ok("-- /* not a block comment\nSELECT");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
}

TEST(QA_Lexer, LineCommentInsideBlockComment) {
    auto tokens = tokenize_ok("/* -- not a line comment */ SELECT");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
}

TEST(QA_Lexer, BlockCommentSpanningManyLines) {
    auto tokens = tokenize_ok("/* line1\nline2\nline3\nline4\n*/ SELECT");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[0].line, 5u);
}

TEST(QA_Lexer, SlashNotFollowedByStar) {
    // A '/' alone is SLASH, not start of comment.
    auto tokens = tokenize_ok("10 / 2");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[1].type, TokenType::SLASH);
    EXPECT_EQ(tokens[2].type, TokenType::INTEGER_LITERAL);
}

TEST(QA_Lexer, DashNotFollowedByDash) {
    // A single '-' is MINUS, not a line comment.
    auto tokens = tokenize_ok("5 - 3");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[1].type, TokenType::MINUS);
    EXPECT_EQ(tokens[2].type, TokenType::INTEGER_LITERAL);
}

// =============================================================================
// Position tracking edge cases
// =============================================================================

TEST(QA_Lexer, PositionAfterMultilineBlockComment) {
    auto tokens = tokenize_ok("/* \n\n\n */ SELECT");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[0].line, 4u);
}

TEST(QA_Lexer, PositionOfMultilineString) {
    // String spanning lines — the token starts at line 1.
    auto tokens = tokenize_ok("'hello\nworld' SELECT");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING_LITERAL);
    EXPECT_EQ(tokens[0].line, 1u);
    EXPECT_EQ(tokens[0].column, 1u);
    // SELECT starts on line 2 after the closing quote.
    EXPECT_EQ(tokens[1].type, TokenType::SELECT);
    EXPECT_EQ(tokens[1].line, 2u);
}

TEST(QA_Lexer, PositionOfEOF) {
    auto tokens = tokenize_ok("SELECT\n");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[1].type, TokenType::END_OF_FILE);
    // EOF is on line 2 (after the newline).
    EXPECT_EQ(tokens[1].line, 2u);
    EXPECT_EQ(tokens[1].column, 1u);
}

TEST(QA_Lexer, PositionWithCRLF) {
    auto tokens = tokenize_ok("SELECT\r\nFROM");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].line, 1u);
    EXPECT_EQ(tokens[1].line, 2u); // FROM should be on line 2
    EXPECT_EQ(tokens[1].column, 1u);
}

TEST(QA_Lexer, PositionOfErrorToken) {
    auto err = tokenize_err("SELECT\n  \n    @");
    EXPECT_EQ(err.code, StatusCode::PARSE_ERROR);
    EXPECT_NE(err.message.find("line 3"), std::string::npos);
    EXPECT_NE(err.message.find("column 5"), std::string::npos);
}

TEST(QA_Lexer, PositionAfterManySpaces) {
    auto tokens = tokenize_ok("          SELECT");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].column, 11u);
}

// =============================================================================
// Operator edge cases
// =============================================================================

TEST(QA_Lexer, OperatorSequenceLessEqualGreater) {
    // "<>=" → NOT_EQUAL(<>), EQUAL(=)
    auto tokens = tokenize_ok("<>=");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::NOT_EQUAL);
    EXPECT_EQ(tokens[0].lexeme, "<>");
    EXPECT_EQ(tokens[1].type, TokenType::EQUAL);
}

TEST(QA_Lexer, TripleColon) {
    // ":::" → COLON_COLON, COLON
    auto tokens = tokenize_ok(":::");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::COLON_COLON);
    EXPECT_EQ(tokens[1].type, TokenType::COLON);
}

TEST(QA_Lexer, LessEqualEqualGreater) {
    // "<=>" → LESS_EQUAL, GREATER
    auto tokens = tokenize_ok("<=>");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::LESS_EQUAL);
    EXPECT_EQ(tokens[1].type, TokenType::GREATER);
}

TEST(QA_Lexer, BangAtEndOfInput) {
    auto err = tokenize_err("!");
    EXPECT_EQ(err.code, StatusCode::PARSE_ERROR);
}

TEST(QA_Lexer, PipeAtEndOfInput) {
    auto err = tokenize_err("|");
    EXPECT_EQ(err.code, StatusCode::PARSE_ERROR);
}

TEST(QA_Lexer, DoublePipeFollowedByPipe) {
    // "|||" → PIPE_PIPE(||), then error on lone '|'
    Lexer lexer("|||");
    auto result = lexer.tokenize();
    // Should error on the trailing '|'.
    EXPECT_FALSE(result.has_value());
}

TEST(QA_Lexer, NotEqualVariants) {
    // Both != and <> should produce NOT_EQUAL.
    auto tokens1 = tokenize_ok("!=");
    auto tokens2 = tokenize_ok("<>");
    ASSERT_GE(tokens1.size(), 2u);
    ASSERT_GE(tokens2.size(), 2u);
    EXPECT_EQ(tokens1[0].type, TokenType::NOT_EQUAL);
    EXPECT_EQ(tokens2[0].type, TokenType::NOT_EQUAL);
}

TEST(QA_Lexer, AllPunctuationNoSpaces) {
    auto tokens = tokenize_ok(",;.()[]");
    ASSERT_GE(tokens.size(), 8u);
    EXPECT_EQ(tokens[0].type, TokenType::COMMA);
    EXPECT_EQ(tokens[1].type, TokenType::SEMICOLON);
    EXPECT_EQ(tokens[2].type, TokenType::DOT);
    EXPECT_EQ(tokens[3].type, TokenType::LPAREN);
    EXPECT_EQ(tokens[4].type, TokenType::RPAREN);
    EXPECT_EQ(tokens[5].type, TokenType::LBRACKET);
    EXPECT_EQ(tokens[6].type, TokenType::RBRACKET);
}

// =============================================================================
// Keyword / identifier boundary cases
// =============================================================================

TEST(QA_Lexer, KeywordAsIdentifierPrefix) {
    // "SELECTION" should be IDENTIFIER, not SELECT + ION.
    auto tokens = tokenize_ok("SELECTION");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme, "SELECTION");
}

TEST(QA_Lexer, KeywordAsIdentifierSuffix) {
    // "FROMAGE" should be IDENTIFIER, not FROM + AGE.
    auto tokens = tokenize_ok("FROMAGE");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme, "FROMAGE");
}

TEST(QA_Lexer, KeywordWithUnderscore) {
    // "SELECT_ALL" should be IDENTIFIER.
    auto tokens = tokenize_ok("SELECT_ALL");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
}

TEST(QA_Lexer, MixedCaseKeyword) {
    auto tokens = tokenize_ok("sElEcT");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[0].lexeme, "sElEcT");
}

TEST(QA_Lexer, SingleCharIdentifiers) {
    auto tokens = tokenize_ok("a b c x y z");
    ASSERT_GE(tokens.size(), 7u);
    for (int i = 0; i < 6; i++) {
        EXPECT_EQ(tokens[i].type, TokenType::IDENTIFIER) << "Token " << i;
    }
}

TEST(QA_Lexer, IdentifierStartingWithUnderscore) {
    auto tokens = tokenize_ok("_ __ ___ _a _1");
    ASSERT_GE(tokens.size(), 6u);
    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(tokens[i].type, TokenType::IDENTIFIER) << "Token " << i;
    }
}

TEST(QA_Lexer, MaxDepthKeyword) {
    // MAX_DEPTH is a keyword with an underscore.
    auto tokens = tokenize_ok("MAX_DEPTH");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::MAX_DEPTH);
}

// =============================================================================
// Boolean and NULL keywords
// =============================================================================

TEST(QA_Lexer, BooleanLiterals) {
    auto tokens = tokenize_ok("TRUE FALSE");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::TRUE_KW);
    EXPECT_EQ(tokens[1].type, TokenType::FALSE_KW);
}

TEST(QA_Lexer, BooleanCaseInsensitive) {
    auto tokens = tokenize_ok("true false True False");
    ASSERT_GE(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].type, TokenType::TRUE_KW);
    EXPECT_EQ(tokens[1].type, TokenType::FALSE_KW);
    EXPECT_EQ(tokens[2].type, TokenType::TRUE_KW);
    EXPECT_EQ(tokens[3].type, TokenType::FALSE_KW);
}

TEST(QA_Lexer, NullKeyword) {
    auto tokens = tokenize_ok("NULL null Null");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::NULL_KW);
    EXPECT_EQ(tokens[1].type, TokenType::NULL_KW);
    EXPECT_EQ(tokens[2].type, TokenType::NULL_KW);
}

// =============================================================================
// UUID as string literal
// =============================================================================

TEST(QA_Lexer, UUIDAsStringLiteral) {
    // UUIDs are tokenized as STRING_LITERAL in SQL (same as PostgreSQL).
    auto tokens = tokenize_ok("'550e8400-e29b-41d4-a716-446655440000'");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING_LITERAL);
}

// =============================================================================
// Array / vector literal edge cases
// =============================================================================

TEST(QA_Lexer, EmptyArray) {
    auto tokens = tokenize_ok("[]");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::LBRACKET);
    EXPECT_EQ(tokens[1].type, TokenType::RBRACKET);
}

TEST(QA_Lexer, SingleElementArray) {
    auto tokens = tokenize_ok("[42]");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::LBRACKET);
    EXPECT_EQ(tokens[1].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[2].type, TokenType::RBRACKET);
}

TEST(QA_Lexer, NestedBrackets) {
    auto tokens = tokenize_ok("[[1, 2], [3, 4]]");
    ASSERT_GE(tokens.size(), 12u);
    EXPECT_EQ(tokens[0].type, TokenType::LBRACKET);
    EXPECT_EQ(tokens[1].type, TokenType::LBRACKET);
}

TEST(QA_Lexer, MixedIntFloatArray) {
    auto tokens = tokenize_ok("[1, 2.5, 3, 4.0]");
    ASSERT_GE(tokens.size(), 10u);
    EXPECT_EQ(tokens[1].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[3].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[5].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[7].type, TokenType::FLOAT_LITERAL);
}

// =============================================================================
// Error character coverage
// =============================================================================

TEST(QA_Lexer, HashIsError) {
    auto err = tokenize_err("#");
    EXPECT_EQ(err.code, StatusCode::PARSE_ERROR);
    EXPECT_NE(err.message.find("#"), std::string::npos);
}

TEST(QA_Lexer, AtSignIsError) {
    auto err = tokenize_err("@");
    EXPECT_EQ(err.code, StatusCode::PARSE_ERROR);
}

TEST(QA_Lexer, BacktickIsError) {
    auto err = tokenize_err("`");
    EXPECT_EQ(err.code, StatusCode::PARSE_ERROR);
}

TEST(QA_Lexer, TildeIsError) {
    auto err = tokenize_err("~");
    EXPECT_EQ(err.code, StatusCode::PARSE_ERROR);
}

TEST(QA_Lexer, DollarIsError) {
    auto err = tokenize_err("$");
    EXPECT_EQ(err.code, StatusCode::PARSE_ERROR);
}

TEST(QA_Lexer, ErrorAfterValidTokens) {
    // Error should occur after successfully tokenizing SELECT.
    Lexer lexer("SELECT @");
    auto result = lexer.tokenize();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// =============================================================================
// Sequences and ordering edge cases
// =============================================================================

TEST(QA_Lexer, EmptySemicolon) {
    auto tokens = tokenize_ok(";");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::SEMICOLON);
}

TEST(QA_Lexer, MultipleSemicolons) {
    auto tokens = tokenize_ok(";;;");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::SEMICOLON);
    EXPECT_EQ(tokens[1].type, TokenType::SEMICOLON);
    EXPECT_EQ(tokens[2].type, TokenType::SEMICOLON);
}

TEST(QA_Lexer, TableDotColumn) {
    // "table" and "column" are keywords — lexer correctly identifies them as such.
    // Parser handles keyword-in-identifier-position disambiguation.
    auto tokens = tokenize_ok("schema.table.column");
    ASSERT_GE(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme, "schema");
    EXPECT_EQ(tokens[1].type, TokenType::DOT);
    EXPECT_EQ(tokens[2].type, TokenType::TABLE); // keyword
    EXPECT_EQ(tokens[2].lexeme, "table");
    EXPECT_EQ(tokens[3].type, TokenType::DOT);
    EXPECT_EQ(tokens[4].type, TokenType::COLUMN); // keyword
    EXPECT_EQ(tokens[4].lexeme, "column");
}

TEST(QA_Lexer, TypeCastChain) {
    auto tokens = tokenize_ok("x::INT::TEXT");
    ASSERT_GE(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].type, TokenType::COLON_COLON);
    EXPECT_EQ(tokens[2].type, TokenType::INT);
    EXPECT_EQ(tokens[3].type, TokenType::COLON_COLON);
    EXPECT_EQ(tokens[4].type, TokenType::TEXT);
}

// =============================================================================
// Stress tests
// =============================================================================

TEST(QA_Lexer, ManyTokens) {
    // Generate a SQL statement with 1000+ tokens.
    std::string sql;
    for (int i = 0; i < 500; i++) {
        sql += "SELECT " + std::to_string(i) + "; ";
    }
    Lexer lexer(sql);
    auto result = lexer.tokenize();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // 500 * (SELECT + INT + SEMICOLON) + EOF = 1501
    EXPECT_EQ(result->size(), 1501u);
}

TEST(QA_Lexer, VeryLongIdentifier) {
    std::string ident(1000, 'a');
    auto tokens = tokenize_ok(ident);
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme.size(), 1000u);
}

TEST(QA_Lexer, ManyConsecutiveComments) {
    std::string sql;
    for (int i = 0; i < 100; i++) {
        sql += "-- comment " + std::to_string(i) + "\n";
    }
    sql += "SELECT";
    auto tokens = tokenize_ok(sql);
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[0].line, 101u);
}

TEST(QA_Lexer, AlternatingTokenTypes) {
    auto tokens = tokenize_ok("1 + 'a' , SELECT ( x ) [ 2.5 ] ;");
    ASSERT_GE(tokens.size(), 12u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[1].type, TokenType::PLUS);
    EXPECT_EQ(tokens[2].type, TokenType::STRING_LITERAL);
    EXPECT_EQ(tokens[3].type, TokenType::COMMA);
    EXPECT_EQ(tokens[4].type, TokenType::SELECT);
    EXPECT_EQ(tokens[5].type, TokenType::LPAREN);
    EXPECT_EQ(tokens[6].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[7].type, TokenType::RPAREN);
    EXPECT_EQ(tokens[8].type, TokenType::LBRACKET);
    EXPECT_EQ(tokens[9].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[10].type, TokenType::RBRACKET);
    EXPECT_EQ(tokens[11].type, TokenType::SEMICOLON);
}

// =============================================================================
// Full SQL statement edge cases
// =============================================================================

TEST(QA_Lexer, CreateTableWithAllTypes) {
    auto tokens = tokenize_ok("CREATE TABLE t ("
                              "a INT, b BIGINT, c SMALLINT, d TINYINT, "
                              "e FLOAT, f DOUBLE, g DECIMAL, "
                              "h BOOLEAN, i TEXT, j VARCHAR, k CHAR, "
                              "l BLOB, m DATE, n TIME, o TIMESTAMP, "
                              "p INTERVAL, q POINT, r JSON, s UUID, "
                              "t EMBEDDING"
                              ");");
    ASSERT_TRUE(tokens.size() > 50u);
    // Just verify it tokenizes without error and has correct structure.
    EXPECT_EQ(tokens[0].type, TokenType::CREATE);
    EXPECT_EQ(tokens[1].type, TokenType::TABLE);
    EXPECT_EQ(tokens.back().type, TokenType::END_OF_FILE);
}

TEST(QA_Lexer, ComplexSelectWithComments) {
    auto tokens = tokenize_ok("-- Get active users\n"
                              "SELECT /* columns */ id, name\n"
                              "FROM users -- main table\n"
                              "WHERE active = TRUE\n"
                              "  AND age >= 18 -- adults only\n"
                              "ORDER BY name ASC\n"
                              "LIMIT 10 OFFSET 0;");
    ASSERT_TRUE(tokens.size() > 15u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
}

TEST(QA_Lexer, SixSevenDBTraverseFullQuery) {
    auto tokens = tokenize_ok("TRAVERSE users "
                              "VIA follows "
                              "DIRECTION 'out' "
                              "MAX_DEPTH 5 "
                              "RETURN PATH;");
    // TRAVERSE users VIA follows DIRECTION 'out' MAX_DEPTH 5 RETURN PATH ;
    ASSERT_GE(tokens.size(), 11u);
    EXPECT_EQ(tokens[0].type, TokenType::TRAVERSE);
    EXPECT_EQ(tokens[2].type, TokenType::VIA);
    EXPECT_EQ(tokens[4].type, TokenType::DIRECTION);
    EXPECT_EQ(tokens[6].type, TokenType::MAX_DEPTH);
    EXPECT_EQ(tokens[8].type, TokenType::RETURN);
    EXPECT_EQ(tokens[9].type, TokenType::PATH);
}

TEST(QA_Lexer, ExplainStatement) {
    auto tokens = tokenize_ok("EXPLAIN SELECT * FROM users;");
    ASSERT_GE(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].type, TokenType::EXPLAIN);
    EXPECT_EQ(tokens[1].type, TokenType::SELECT);
    EXPECT_EQ(tokens[2].type, TokenType::STAR);
}

// =============================================================================
// Lexer reuse (calling tokenize twice)
// =============================================================================

TEST(QA_Lexer, TokenizeTwiceAfterError) {
    // First call fails, second call should succeed from clean state.
    Lexer lexer("@");
    auto first = lexer.tokenize();
    EXPECT_FALSE(first.has_value());

    // Tokenize again — should fail again with same error (clean state).
    auto second = lexer.tokenize();
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_Lexer, TokenizeMultipleTimes) {
    Lexer lexer("SELECT 42");
    for (int i = 0; i < 10; i++) {
        auto result = lexer.tokenize();
        ASSERT_TRUE(result.has_value()) << "Iteration " << i;
        ASSERT_EQ(result->size(), 3u) << "Iteration " << i;
        EXPECT_EQ((*result)[0].type, TokenType::SELECT);
        EXPECT_EQ((*result)[1].type, TokenType::INTEGER_LITERAL);
    }
}

// =============================================================================
// Whitespace edge cases
// =============================================================================

TEST(QA_Lexer, FormFeedIsWhitespace) {
    // Form feed (\f) should be treated as whitespace.
    auto tokens = tokenize_ok("SELECT\fFROM");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[1].type, TokenType::FROM);
}

TEST(QA_Lexer, VerticalTabIsWhitespace) {
    auto tokens = tokenize_ok("SELECT\vFROM");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[1].type, TokenType::FROM);
}

TEST(QA_Lexer, MixedWhitespace) {
    auto tokens = tokenize_ok(" \t\n \r\n \t SELECT");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
}

// =============================================================================
// Token lexeme correctness
// =============================================================================

TEST(QA_Lexer, OperatorLexemes) {
    auto tokens = tokenize_ok("+ - * / % = != < > <= >= || :: , ; : . ( ) [ ]");
    ASSERT_GE(tokens.size(), 21u);
    EXPECT_EQ(tokens[0].lexeme, "+");
    EXPECT_EQ(tokens[1].lexeme, "-");
    EXPECT_EQ(tokens[2].lexeme, "*");
    EXPECT_EQ(tokens[3].lexeme, "/");
    EXPECT_EQ(tokens[4].lexeme, "%");
    EXPECT_EQ(tokens[5].lexeme, "=");
    EXPECT_EQ(tokens[6].lexeme, "!=");
    EXPECT_EQ(tokens[7].lexeme, "<");
    EXPECT_EQ(tokens[8].lexeme, ">");
    EXPECT_EQ(tokens[9].lexeme, "<=");
    EXPECT_EQ(tokens[10].lexeme, ">=");
    EXPECT_EQ(tokens[11].lexeme, "||");
    EXPECT_EQ(tokens[12].lexeme, "::");
    EXPECT_EQ(tokens[13].lexeme, ",");
    EXPECT_EQ(tokens[14].lexeme, ";");
    EXPECT_EQ(tokens[15].lexeme, ":");
    EXPECT_EQ(tokens[16].lexeme, ".");
    EXPECT_EQ(tokens[17].lexeme, "(");
    EXPECT_EQ(tokens[18].lexeme, ")");
    EXPECT_EQ(tokens[19].lexeme, "[");
    EXPECT_EQ(tokens[20].lexeme, "]");
}

TEST(QA_Lexer, KeywordLexemePreservesCase) {
    auto tokens = tokenize_ok("Select fRoM WHere");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[0].lexeme, "Select");
    EXPECT_EQ(tokens[1].type, TokenType::FROM);
    EXPECT_EQ(tokens[1].lexeme, "fRoM");
    EXPECT_EQ(tokens[2].type, TokenType::WHERE);
    EXPECT_EQ(tokens[2].lexeme, "WHere");
}

// =============================================================================
// Every SixSevenDB-specific keyword (comprehensive)
// =============================================================================

TEST(QA_Lexer, AllSixSevenDBKeywordsCaseInsensitive) {
    struct KW {
        const char* text;
        TokenType expected;
    };
    KW keywords[] = {
        {"direction", TokenType::DIRECTION},
        {"edge", TokenType::EDGE},
        {"embedding", TokenType::EMBEDDING},
        {"fetch", TokenType::FETCH},
        {"link", TokenType::LINK},
        {"match", TokenType::MATCH},
        {"max_depth", TokenType::MAX_DEPTH},
        {"nearest", TokenType::NEAREST},
        {"path", TokenType::PATH},
        {"recursive", TokenType::RECURSIVE},
        {"reembed", TokenType::REEMBED},
        {"return", TokenType::RETURN},
        {"shortest", TokenType::SHORTEST},
        {"traverse", TokenType::TRAVERSE},
        {"type", TokenType::TYPE},
        {"unlink", TokenType::UNLINK},
        {"password", TokenType::PASSWORD},
        {"via", TokenType::VIA},
    };

    for (auto& [text, expected] : keywords) {
        auto tokens = tokenize_ok(text);
        ASSERT_GE(tokens.size(), 2u) << "Keyword: " << text;
        EXPECT_EQ(tokens[0].type, expected) << "Keyword: " << text;
    }
}

// =============================================================================
// token_type_name comprehensive coverage
// =============================================================================

TEST(QA_Lexer, TokenTypeNameAllOperators) {
    EXPECT_EQ(token_type_name(TokenType::PLUS), "PLUS");
    EXPECT_EQ(token_type_name(TokenType::MINUS), "MINUS");
    EXPECT_EQ(token_type_name(TokenType::STAR), "STAR");
    EXPECT_EQ(token_type_name(TokenType::SLASH), "SLASH");
    EXPECT_EQ(token_type_name(TokenType::PERCENT), "PERCENT");
    EXPECT_EQ(token_type_name(TokenType::EQUAL), "EQUAL");
    EXPECT_EQ(token_type_name(TokenType::NOT_EQUAL), "NOT_EQUAL");
    EXPECT_EQ(token_type_name(TokenType::LESS), "LESS");
    EXPECT_EQ(token_type_name(TokenType::GREATER), "GREATER");
    EXPECT_EQ(token_type_name(TokenType::LESS_EQUAL), "LESS_EQUAL");
    EXPECT_EQ(token_type_name(TokenType::GREATER_EQUAL), "GREATER_EQUAL");
    EXPECT_EQ(token_type_name(TokenType::PIPE_PIPE), "PIPE_PIPE");
    EXPECT_EQ(token_type_name(TokenType::COLON_COLON), "COLON_COLON");
}

TEST(QA_Lexer, TokenTypeNameAllPunctuation) {
    EXPECT_EQ(token_type_name(TokenType::COMMA), "COMMA");
    EXPECT_EQ(token_type_name(TokenType::SEMICOLON), "SEMICOLON");
    EXPECT_EQ(token_type_name(TokenType::COLON), "COLON");
    EXPECT_EQ(token_type_name(TokenType::DOT), "DOT");
    EXPECT_EQ(token_type_name(TokenType::LPAREN), "LPAREN");
    EXPECT_EQ(token_type_name(TokenType::RPAREN), "RPAREN");
    EXPECT_EQ(token_type_name(TokenType::LBRACKET), "LBRACKET");
    EXPECT_EQ(token_type_name(TokenType::RBRACKET), "RBRACKET");
}
