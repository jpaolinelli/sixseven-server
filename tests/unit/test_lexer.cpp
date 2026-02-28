#include "giodb/parser/lexer.h"
#include "giodb/parser/token.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace giodb;

// -- Helper: tokenize and assert success --------------------------------------

static std::vector<Token> tokenize_ok(std::string_view sql) {
    Lexer lexer(sql);
    auto result = lexer.tokenize();
    EXPECT_TRUE(result.has_value()) << result.error().message;
    return result.has_value() ? std::move(*result) : std::vector<Token>{};
}

// -- Empty input and EOF ------------------------------------------------------

TEST(Lexer, EmptyInput) {
    auto tokens = tokenize_ok("");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::END_OF_FILE);
}

TEST(Lexer, WhitespaceOnly) {
    auto tokens = tokenize_ok("   \t\n  \r\n  ");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::END_OF_FILE);
}

// -- SQL keywords (case-insensitive) ------------------------------------------

TEST(Lexer, SelectKeyword) {
    auto tokens = tokenize_ok("SELECT");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[0].lexeme, "SELECT");
}

TEST(Lexer, KeywordsCaseInsensitive) {
    auto tokens = tokenize_ok("select FROM Where");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[0].lexeme, "select");
    EXPECT_EQ(tokens[1].type, TokenType::FROM);
    EXPECT_EQ(tokens[1].lexeme, "FROM");
    EXPECT_EQ(tokens[2].type, TokenType::WHERE);
    EXPECT_EQ(tokens[2].lexeme, "Where");
}

TEST(Lexer, AllSQLKeywords) {
    // Test a representative set of SQL keywords.
    struct KW {
        const char* text;
        TokenType expected;
    };
    KW keywords[] = {
        {"ALL", TokenType::ALL},
        {"ALTER", TokenType::ALTER},
        {"AND", TokenType::AND},
        {"AS", TokenType::AS},
        {"ASC", TokenType::ASC},
        {"BEGIN", TokenType::BEGIN},
        {"BETWEEN", TokenType::BETWEEN},
        {"BY", TokenType::BY},
        {"CASCADE", TokenType::CASCADE},
        {"CASE", TokenType::CASE},
        {"CHECK", TokenType::CHECK},
        {"COLUMN", TokenType::COLUMN},
        {"COMMIT", TokenType::COMMIT},
        {"CONSTRAINT", TokenType::CONSTRAINT},
        {"CREATE", TokenType::CREATE},
        {"CROSS", TokenType::CROSS},
        {"DEFAULT", TokenType::DEFAULT},
        {"DELETE", TokenType::DELETE},
        {"DESC", TokenType::DESC},
        {"DISTINCT", TokenType::DISTINCT},
        {"DROP", TokenType::DROP},
        {"ELSE", TokenType::ELSE},
        {"END", TokenType::END},
        {"EXCEPT", TokenType::EXCEPT},
        {"EXISTS", TokenType::EXISTS},
        {"FALSE", TokenType::FALSE_KW},
        {"FOREIGN", TokenType::FOREIGN},
        {"FROM", TokenType::FROM},
        {"FULL", TokenType::FULL},
        {"GROUP", TokenType::GROUP},
        {"HAVING", TokenType::HAVING},
        {"IF", TokenType::IF},
        {"IN", TokenType::IN},
        {"INDEX", TokenType::INDEX},
        {"INNER", TokenType::INNER},
        {"INSERT", TokenType::INSERT},
        {"INTERSECT", TokenType::INTERSECT},
        {"INTO", TokenType::INTO},
        {"IS", TokenType::IS},
        {"JOIN", TokenType::JOIN},
        {"KEY", TokenType::KEY},
        {"LEFT", TokenType::LEFT},
        {"LIKE", TokenType::LIKE},
        {"LIMIT", TokenType::LIMIT},
        {"NOT", TokenType::NOT},
        {"NULL", TokenType::NULL_KW},
        {"OFFSET", TokenType::OFFSET},
        {"ON", TokenType::ON},
        {"OR", TokenType::OR},
        {"ORDER", TokenType::ORDER},
        {"OUTER", TokenType::OUTER},
        {"PRIMARY", TokenType::PRIMARY},
        {"REFERENCES", TokenType::REFERENCES},
        {"RESTRICT", TokenType::RESTRICT},
        {"RETURNING", TokenType::RETURNING},
        {"RIGHT", TokenType::RIGHT},
        {"ROLLBACK", TokenType::ROLLBACK},
        {"SELECT", TokenType::SELECT},
        {"SET", TokenType::SET},
        {"TABLE", TokenType::TABLE},
        {"THEN", TokenType::THEN},
        {"TRANSACTION", TokenType::TRANSACTION},
        {"TRUE", TokenType::TRUE_KW},
        {"UNION", TokenType::UNION},
        {"UNIQUE", TokenType::UNIQUE},
        {"UPDATE", TokenType::UPDATE},
        {"VALUES", TokenType::VALUES},
        {"WHEN", TokenType::WHEN},
        {"WHERE", TokenType::WHERE},
        {"WITH", TokenType::WITH},
    };

    for (auto& [text, expected] : keywords) {
        auto tokens = tokenize_ok(text);
        ASSERT_GE(tokens.size(), 2u) << "Keyword: " << text;
        EXPECT_EQ(tokens[0].type, expected) << "Keyword: " << text;
    }
}

TEST(Lexer, TypeKeywords) {
    struct KW {
        const char* text;
        TokenType expected;
    };
    KW keywords[] = {
        {"BIGINT", TokenType::BIGINT},     {"BLOB", TokenType::BLOB_KW},
        {"BOOLEAN", TokenType::BOOLEAN},   {"CHAR", TokenType::CHAR},
        {"DATE", TokenType::DATE},         {"DECIMAL", TokenType::DECIMAL},
        {"DOUBLE", TokenType::DOUBLE},     {"FLOAT", TokenType::FLOAT},
        {"INT", TokenType::INT},           {"INTEGER", TokenType::INTEGER},
        {"INTERVAL", TokenType::INTERVAL}, {"JSON", TokenType::JSON_KW},
        {"NUMERIC", TokenType::NUMERIC},   {"POINT", TokenType::POINT_KW},
        {"SMALLINT", TokenType::SMALLINT}, {"TEXT", TokenType::TEXT},
        {"TIME", TokenType::TIME},         {"TIMESTAMP", TokenType::TIMESTAMP},
        {"TINYINT", TokenType::TINYINT},   {"UUID", TokenType::UUID_KW},
        {"VARCHAR", TokenType::VARCHAR},
    };

    for (auto& [text, expected] : keywords) {
        auto tokens = tokenize_ok(text);
        ASSERT_GE(tokens.size(), 2u) << "Type keyword: " << text;
        EXPECT_EQ(tokens[0].type, expected) << "Type keyword: " << text;
    }
}

TEST(Lexer, AggregateKeywords) {
    auto tokens = tokenize_ok("AVG COUNT MAX MIN SUM");
    ASSERT_GE(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].type, TokenType::AVG);
    EXPECT_EQ(tokens[1].type, TokenType::COUNT);
    EXPECT_EQ(tokens[2].type, TokenType::MAX_KW);
    EXPECT_EQ(tokens[3].type, TokenType::MIN_KW);
    EXPECT_EQ(tokens[4].type, TokenType::SUM);
}

// -- GioDB-specific keywords --------------------------------------------------

TEST(Lexer, GioDBKeywords) {
    struct KW {
        const char* text;
        TokenType expected;
    };
    KW keywords[] = {
        {"DIRECTION", TokenType::DIRECTION},
        {"EDGE", TokenType::EDGE},
        {"EMBEDDING", TokenType::EMBEDDING},
        {"FETCH", TokenType::FETCH},
        {"LINK", TokenType::LINK},
        {"MATCH", TokenType::MATCH},
        {"MAX_DEPTH", TokenType::MAX_DEPTH},
        {"NEAREST", TokenType::NEAREST},
        {"PATH", TokenType::PATH},
        {"RECURSIVE", TokenType::RECURSIVE},
        {"REEMBED", TokenType::REEMBED},
        {"SHORTEST", TokenType::SHORTEST},
        {"TRAVERSE", TokenType::TRAVERSE},
        {"TYPE", TokenType::TYPE},
        {"UNLINK", TokenType::UNLINK},
        {"VIA", TokenType::VIA},
    };

    for (auto& [text, expected] : keywords) {
        auto tokens = tokenize_ok(text);
        ASSERT_GE(tokens.size(), 2u) << "GioDB keyword: " << text;
        EXPECT_EQ(tokens[0].type, expected) << "GioDB keyword: " << text;
    }
}

// -- Identifiers --------------------------------------------------------------

TEST(Lexer, SimpleIdentifier) {
    auto tokens = tokenize_ok("my_table");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme, "my_table");
}

TEST(Lexer, IdentifierWithDigits) {
    auto tokens = tokenize_ok("table1");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme, "table1");
}

TEST(Lexer, UnderscoreIdentifier) {
    auto tokens = tokenize_ok("_private _x2");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme, "_private");
    EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].lexeme, "_x2");
}

// -- Integer literals ---------------------------------------------------------

TEST(Lexer, IntegerLiteral) {
    auto tokens = tokenize_ok("42");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "42");
}

TEST(Lexer, IntegerLiteralZero) {
    auto tokens = tokenize_ok("0");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "0");
}

TEST(Lexer, IntegerLiteralMultipleDigits) {
    auto tokens = tokenize_ok("123456789");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "123456789");
}

// -- Float literals -----------------------------------------------------------

TEST(Lexer, FloatLiteral) {
    auto tokens = tokenize_ok("3.14");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "3.14");
}

TEST(Lexer, FloatLiteralLeadingDot) {
    auto tokens = tokenize_ok(".5");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, ".5");
}

TEST(Lexer, FloatLiteralTrailingDot) {
    auto tokens = tokenize_ok("5.");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "5.");
}

TEST(Lexer, FloatScientificNotation) {
    auto tokens = tokenize_ok("1.5e10");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "1.5e10");
}

TEST(Lexer, FloatScientificNegativeExponent) {
    auto tokens = tokenize_ok("2.5E-3");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "2.5E-3");
}

TEST(Lexer, IntegerScientificNotation) {
    auto tokens = tokenize_ok("1e6");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "1e6");
}

TEST(Lexer, LeadingDotScientificNotation) {
    auto tokens = tokenize_ok(".123e4");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, ".123e4");
}

TEST(Lexer, LeadingDotScientificNegativeExponent) {
    auto tokens = tokenize_ok(".5e-3");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, ".5e-3");
}

TEST(Lexer, LeadingDotScientificUpperE) {
    auto tokens = tokenize_ok(".9E+2");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, ".9E+2");
}

TEST(Lexer, LeadingDotNoExponentDigitsStaysFloat) {
    // .5e without digits after e — should NOT consume the 'e'
    auto tokens = tokenize_ok(".5e");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, ".5");
}

// -- String literals ----------------------------------------------------------

TEST(Lexer, StringLiteral) {
    auto tokens = tokenize_ok("'hello'");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "'hello'");
}

TEST(Lexer, StringLiteralEmpty) {
    auto tokens = tokenize_ok("''");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "''");
}

TEST(Lexer, StringLiteralWithEscapedQuote) {
    auto tokens = tokenize_ok("'it''s'");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "'it''s'");
}

TEST(Lexer, StringLiteralMultipleEscapes) {
    auto tokens = tokenize_ok("'a''b''c'");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "'a''b''c'");
}

TEST(Lexer, UnterminatedStringFails) {
    Lexer lexer("'unterminated");
    auto result = lexer.tokenize();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(Lexer, StringLiteralWithNewline) {
    auto tokens = tokenize_ok("'line1\nline2'");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING_LITERAL);
}

// -- Operators ----------------------------------------------------------------

TEST(Lexer, ArithmeticOperators) {
    auto tokens = tokenize_ok("+ - * / %");
    ASSERT_GE(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].type, TokenType::PLUS);
    EXPECT_EQ(tokens[1].type, TokenType::MINUS);
    EXPECT_EQ(tokens[2].type, TokenType::STAR);
    EXPECT_EQ(tokens[3].type, TokenType::SLASH);
    EXPECT_EQ(tokens[4].type, TokenType::PERCENT);
}

TEST(Lexer, ComparisonOperators) {
    auto tokens = tokenize_ok("= != < > <= >=");
    ASSERT_GE(tokens.size(), 7u);
    EXPECT_EQ(tokens[0].type, TokenType::EQUAL);
    EXPECT_EQ(tokens[1].type, TokenType::NOT_EQUAL);
    EXPECT_EQ(tokens[2].type, TokenType::LESS);
    EXPECT_EQ(tokens[3].type, TokenType::GREATER);
    EXPECT_EQ(tokens[4].type, TokenType::LESS_EQUAL);
    EXPECT_EQ(tokens[5].type, TokenType::GREATER_EQUAL);
}

TEST(Lexer, NotEqualAngleBrackets) {
    auto tokens = tokenize_ok("<>");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::NOT_EQUAL);
    EXPECT_EQ(tokens[0].lexeme, "<>");
}

TEST(Lexer, PipePipeOperator) {
    auto tokens = tokenize_ok("||");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::PIPE_PIPE);
}

TEST(Lexer, ColonColonCast) {
    auto tokens = tokenize_ok("::");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::COLON_COLON);
}

// -- Punctuation --------------------------------------------------------------

TEST(Lexer, Punctuation) {
    auto tokens = tokenize_ok(", ; . ( ) [ ]");
    ASSERT_GE(tokens.size(), 8u);
    EXPECT_EQ(tokens[0].type, TokenType::COMMA);
    EXPECT_EQ(tokens[1].type, TokenType::SEMICOLON);
    EXPECT_EQ(tokens[2].type, TokenType::DOT);
    EXPECT_EQ(tokens[3].type, TokenType::LPAREN);
    EXPECT_EQ(tokens[4].type, TokenType::RPAREN);
    EXPECT_EQ(tokens[5].type, TokenType::LBRACKET);
    EXPECT_EQ(tokens[6].type, TokenType::RBRACKET);
}

// -- Comments -----------------------------------------------------------------

TEST(Lexer, LineComment) {
    auto tokens = tokenize_ok("SELECT -- this is a comment\nFROM");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[1].type, TokenType::FROM);
}

TEST(Lexer, LineCommentAtEnd) {
    auto tokens = tokenize_ok("SELECT -- comment at end");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[1].type, TokenType::END_OF_FILE);
}

TEST(Lexer, BlockComment) {
    auto tokens = tokenize_ok("SELECT /* comment */ FROM");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[1].type, TokenType::FROM);
}

TEST(Lexer, BlockCommentMultiline) {
    auto tokens = tokenize_ok("SELECT /* multi\nline\ncomment */ FROM");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[1].type, TokenType::FROM);
}

TEST(Lexer, NestedBlockComment) {
    auto tokens = tokenize_ok("SELECT /* outer /* inner */ still comment */ FROM");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[1].type, TokenType::FROM);
}

TEST(Lexer, UnterminatedBlockCommentFails) {
    Lexer lexer("SELECT /* unterminated");
    auto result = lexer.tokenize();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// -- Position tracking --------------------------------------------------------

TEST(Lexer, PositionFirstToken) {
    auto tokens = tokenize_ok("SELECT");
    ASSERT_GE(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].line, 1u);
    EXPECT_EQ(tokens[0].column, 1u);
}

TEST(Lexer, PositionSecondLine) {
    auto tokens = tokenize_ok("SELECT\nFROM");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].line, 1u);
    EXPECT_EQ(tokens[0].column, 1u);
    EXPECT_EQ(tokens[1].line, 2u);
    EXPECT_EQ(tokens[1].column, 1u);
}

TEST(Lexer, PositionWithSpaces) {
    auto tokens = tokenize_ok("  SELECT  FROM");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].line, 1u);
    EXPECT_EQ(tokens[0].column, 3u);
    EXPECT_EQ(tokens[1].line, 1u);
    EXPECT_EQ(tokens[1].column, 11u);
}

TEST(Lexer, PositionMultipleLines) {
    auto tokens = tokenize_ok("SELECT\n  id,\n  name\nFROM users");
    ASSERT_GE(tokens.size(), 7u);
    EXPECT_EQ(tokens[0].line, 1u); // SELECT
    EXPECT_EQ(tokens[0].column, 1u);
    EXPECT_EQ(tokens[1].line, 2u); // id
    EXPECT_EQ(tokens[1].column, 3u);
    EXPECT_EQ(tokens[2].line, 2u); // ,
    EXPECT_EQ(tokens[2].column, 5u);
}

// -- Error cases --------------------------------------------------------------

TEST(Lexer, UnrecognizedCharacterFails) {
    Lexer lexer("SELECT @ FROM");
    auto result = lexer.tokenize();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(Lexer, ErrorIncludesPosition) {
    Lexer lexer("SELECT\n  @");
    auto result = lexer.tokenize();
    EXPECT_FALSE(result.has_value());
    // Error message should mention line 2.
    EXPECT_NE(result.error().message.find("line 2"), std::string::npos);
}

// -- Array literals (brackets around values) ----------------------------------

TEST(Lexer, ArrayLiteralBrackets) {
    auto tokens = tokenize_ok("[1.0, 2.0, 3.0]");
    ASSERT_GE(tokens.size(), 8u);
    EXPECT_EQ(tokens[0].type, TokenType::LBRACKET);
    EXPECT_EQ(tokens[1].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[2].type, TokenType::COMMA);
    EXPECT_EQ(tokens[3].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[4].type, TokenType::COMMA);
    EXPECT_EQ(tokens[5].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[6].type, TokenType::RBRACKET);
}

// -- Full SQL statements ------------------------------------------------------

TEST(Lexer, SimpleSelect) {
    auto tokens = tokenize_ok("SELECT id, name FROM users WHERE active = TRUE;");
    // SELECT id , name FROM users WHERE active = TRUE ;
    ASSERT_GE(tokens.size(), 11u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].lexeme, "id");
    EXPECT_EQ(tokens[2].type, TokenType::COMMA);
    EXPECT_EQ(tokens[3].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[3].lexeme, "name");
    EXPECT_EQ(tokens[4].type, TokenType::FROM);
    EXPECT_EQ(tokens[5].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[6].type, TokenType::WHERE);
    EXPECT_EQ(tokens[7].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[8].type, TokenType::EQUAL);
    EXPECT_EQ(tokens[9].type, TokenType::TRUE_KW);
    EXPECT_EQ(tokens[10].type, TokenType::SEMICOLON);
}

TEST(Lexer, InsertStatement) {
    auto tokens = tokenize_ok("INSERT INTO users (id, name) VALUES (1, 'Alice');");
    ASSERT_GE(tokens.size(), 14u);
    EXPECT_EQ(tokens[0].type, TokenType::INSERT);
    EXPECT_EQ(tokens[1].type, TokenType::INTO);
    EXPECT_EQ(tokens[2].type, TokenType::IDENTIFIER); // users
    EXPECT_EQ(tokens[3].type, TokenType::LPAREN);
    EXPECT_EQ(tokens[4].type, TokenType::IDENTIFIER); // id
    EXPECT_EQ(tokens[5].type, TokenType::COMMA);
    EXPECT_EQ(tokens[6].type, TokenType::IDENTIFIER); // name
    EXPECT_EQ(tokens[7].type, TokenType::RPAREN);
    EXPECT_EQ(tokens[8].type, TokenType::VALUES);
    EXPECT_EQ(tokens[9].type, TokenType::LPAREN);
    EXPECT_EQ(tokens[10].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[11].type, TokenType::COMMA);
    EXPECT_EQ(tokens[12].type, TokenType::STRING_LITERAL);
    EXPECT_EQ(tokens[13].type, TokenType::RPAREN);
}

TEST(Lexer, CreateTableStatement) {
    auto tokens = tokenize_ok("CREATE TABLE users (\n"
                              "  id INT PRIMARY KEY,\n"
                              "  name VARCHAR NOT NULL\n"
                              ");");
    ASSERT_GE(tokens.size(), 14u);
    EXPECT_EQ(tokens[0].type, TokenType::CREATE);
    EXPECT_EQ(tokens[1].type, TokenType::TABLE);
    EXPECT_EQ(tokens[2].type, TokenType::IDENTIFIER); // users
    EXPECT_EQ(tokens[3].type, TokenType::LPAREN);
}

TEST(Lexer, TraverseStatement) {
    auto tokens = tokenize_ok("TRAVERSE users VIA follows DIRECTION 'out' MAX_DEPTH 3;");
    ASSERT_GE(tokens.size(), 9u);
    EXPECT_EQ(tokens[0].type, TokenType::TRAVERSE);
    EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER); // users
    EXPECT_EQ(tokens[2].type, TokenType::VIA);
    EXPECT_EQ(tokens[3].type, TokenType::IDENTIFIER); // follows
    EXPECT_EQ(tokens[4].type, TokenType::DIRECTION);
    EXPECT_EQ(tokens[5].type, TokenType::STRING_LITERAL); // 'out'
    EXPECT_EQ(tokens[6].type, TokenType::MAX_DEPTH);
    EXPECT_EQ(tokens[7].type, TokenType::INTEGER_LITERAL); // 3
    EXPECT_EQ(tokens[8].type, TokenType::SEMICOLON);
}

TEST(Lexer, NearestQuery) {
    auto tokens = tokenize_ok("SELECT NEAREST(embedding, [1.0, 2.0], 5) FROM docs;");
    ASSERT_GE(tokens.size(), 13u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[1].type, TokenType::NEAREST);
    EXPECT_EQ(tokens[2].type, TokenType::LPAREN);
    EXPECT_EQ(tokens[3].type, TokenType::EMBEDDING); // embedding (keyword)
    EXPECT_EQ(tokens[4].type, TokenType::COMMA);
    EXPECT_EQ(tokens[5].type, TokenType::LBRACKET);
}

TEST(Lexer, TypeCastExpression) {
    auto tokens = tokenize_ok("value::INT");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].type, TokenType::COLON_COLON);
    EXPECT_EQ(tokens[2].type, TokenType::INT);
}

TEST(Lexer, StringConcatenation) {
    auto tokens = tokenize_ok("'hello' || ' ' || 'world'");
    ASSERT_GE(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING_LITERAL);
    EXPECT_EQ(tokens[1].type, TokenType::PIPE_PIPE);
    EXPECT_EQ(tokens[2].type, TokenType::STRING_LITERAL);
    EXPECT_EQ(tokens[3].type, TokenType::PIPE_PIPE);
    EXPECT_EQ(tokens[4].type, TokenType::STRING_LITERAL);
}

// -- token_type_name ----------------------------------------------------------

TEST(Lexer, TokenTypeNameReturnsCorrectStrings) {
    EXPECT_EQ(token_type_name(TokenType::SELECT), "SELECT");
    EXPECT_EQ(token_type_name(TokenType::IDENTIFIER), "IDENTIFIER");
    EXPECT_EQ(token_type_name(TokenType::INTEGER_LITERAL), "INTEGER_LITERAL");
    EXPECT_EQ(token_type_name(TokenType::FLOAT_LITERAL), "FLOAT_LITERAL");
    EXPECT_EQ(token_type_name(TokenType::STRING_LITERAL), "STRING_LITERAL");
    EXPECT_EQ(token_type_name(TokenType::END_OF_FILE), "END_OF_FILE");
    EXPECT_EQ(token_type_name(TokenType::TRAVERSE), "TRAVERSE");
    EXPECT_EQ(token_type_name(TokenType::NEAREST), "NEAREST");
    EXPECT_EQ(token_type_name(TokenType::EMBEDDING), "EMBEDDING");
    EXPECT_EQ(token_type_name(TokenType::PLUS), "PLUS");
    EXPECT_EQ(token_type_name(TokenType::NOT_EQUAL), "NOT_EQUAL");
    EXPECT_EQ(token_type_name(TokenType::COLON_COLON), "COLON_COLON");
}

// -- Dot disambiguation -------------------------------------------------------

TEST(Lexer, DotBetweenIdentifiers) {
    auto tokens = tokenize_ok("users.id");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].type, TokenType::DOT);
    EXPECT_EQ(tokens[2].type, TokenType::IDENTIFIER);
}

TEST(Lexer, DotFollowedByDigitIsFloat) {
    auto tokens = tokenize_ok(".123");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, ".123");
}

// -- Edge cases ---------------------------------------------------------------

TEST(Lexer, NegativeNumberIsMinusThenInt) {
    auto tokens = tokenize_ok("-42");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::MINUS);
    EXPECT_EQ(tokens[1].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[1].lexeme, "42");
}

TEST(Lexer, ConsecutiveOperators) {
    auto tokens = tokenize_ok("1+2*3");
    ASSERT_GE(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[1].type, TokenType::PLUS);
    EXPECT_EQ(tokens[2].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[3].type, TokenType::STAR);
    EXPECT_EQ(tokens[4].type, TokenType::INTEGER_LITERAL);
}

TEST(Lexer, LinkUnlinkStatements) {
    auto tokens = tokenize_ok("LINK users EDGE follows");
    ASSERT_GE(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].type, TokenType::LINK);
    EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[2].type, TokenType::EDGE);
    EXPECT_EQ(tokens[3].type, TokenType::IDENTIFIER);
}

TEST(Lexer, EmbeddingReembed) {
    auto tokens = tokenize_ok("REEMBED docs EMBEDDING content_emb");
    ASSERT_GE(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].type, TokenType::REEMBED);
    EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[2].type, TokenType::EMBEDDING);
    EXPECT_EQ(tokens[3].type, TokenType::IDENTIFIER);
}

TEST(Lexer, ShortestPathQuery) {
    auto tokens = tokenize_ok("SHORTEST PATH VIA follows");
    ASSERT_GE(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].type, TokenType::SHORTEST);
    EXPECT_EQ(tokens[1].type, TokenType::PATH);
    EXPECT_EQ(tokens[2].type, TokenType::VIA);
    EXPECT_EQ(tokens[3].type, TokenType::IDENTIFIER);
}

TEST(Lexer, EOFToken) {
    auto tokens = tokenize_ok("SELECT");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens.back().type, TokenType::END_OF_FILE);
}

// -- Malformed scientific notation (issue #1) ---------------------------------

TEST(Lexer, MalformedExponentNoDigits) {
    // "1.5e" should parse as FLOAT(1.5) IDENTIFIER(e), not FLOAT(1.5e).
    auto tokens = tokenize_ok("1.5e");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "1.5");
    EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].lexeme, "e");
}

TEST(Lexer, MalformedExponentPlusNoDigits) {
    // "1.5e+" should parse as FLOAT(1.5) IDENTIFIER(e) PLUS.
    auto tokens = tokenize_ok("1.5e+");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "1.5");
    EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].lexeme, "e");
    EXPECT_EQ(tokens[2].type, TokenType::PLUS);
}

TEST(Lexer, MalformedIntExponentNoDigits) {
    // "1e" should parse as INTEGER(1) IDENTIFIER(e).
    auto tokens = tokenize_ok("1e");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "1");
    EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].lexeme, "e");
}

TEST(Lexer, ValidExponentStillWorks) {
    auto tokens = tokenize_ok("1.5e10");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "1.5e10");
}

TEST(Lexer, ValidExponentWithSign) {
    auto tokens = tokenize_ok("2E-3");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "2E-3");
}

// -- Lone operators that should error -----------------------------------------

TEST(Lexer, LonePipeErrors) {
    Lexer lexer("SELECT | FROM");
    auto result = lexer.tokenize();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(Lexer, LoneColonIsToken) {
    Lexer lexer("x : y");
    auto result = lexer.tokenize();
    ASSERT_TRUE(result.has_value());
    auto& tokens = *result;
    ASSERT_EQ(tokens.size(), 4u); // x : y EOF
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].type, TokenType::COLON);
    EXPECT_EQ(tokens[2].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[3].type, TokenType::END_OF_FILE);
}

TEST(Lexer, LoneBangErrors) {
    Lexer lexer("SELECT ! FROM");
    auto result = lexer.tokenize();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// -- Multiple statements ------------------------------------------------------

TEST(Lexer, MultipleStatements) {
    auto tokens = tokenize_ok("SELECT 1; SELECT 2;");
    ASSERT_GE(tokens.size(), 7u);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[1].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[1].lexeme, "1");
    EXPECT_EQ(tokens[2].type, TokenType::SEMICOLON);
    EXPECT_EQ(tokens[3].type, TokenType::SELECT);
    EXPECT_EQ(tokens[4].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ(tokens[4].lexeme, "2");
    EXPECT_EQ(tokens[5].type, TokenType::SEMICOLON);
}

// -- Lexer reuse (issue #7) ---------------------------------------------------

TEST(Lexer, TokenizeTwiceProducesSameResult) {
    Lexer lexer("SELECT 1");
    auto first = lexer.tokenize();
    ASSERT_TRUE(first.has_value());

    auto second = lexer.tokenize();
    ASSERT_TRUE(second.has_value());

    ASSERT_EQ(first->size(), second->size());
    for (size_t i = 0; i < first->size(); ++i) {
        EXPECT_EQ((*first)[i].type, (*second)[i].type);
        EXPECT_EQ((*first)[i].lexeme, (*second)[i].lexeme);
        EXPECT_EQ((*first)[i].line, (*second)[i].line);
        EXPECT_EQ((*first)[i].column, (*second)[i].column);
    }
}

// -- Tab handling in position tracking ----------------------------------------

TEST(Lexer, TabInPositionTracking) {
    auto tokens = tokenize_ok("\tSELECT");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].line, 1u);
    // Tab advances column by 1 (single character).
    EXPECT_EQ(tokens[0].column, 2u);
}
