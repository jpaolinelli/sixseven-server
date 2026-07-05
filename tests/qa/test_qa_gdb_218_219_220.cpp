#include "sixseven/catalog/catalog.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/planner/binder.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "test_qa_helpers.h"

namespace sixseven {

// ===========================================================================
// Helper: tokenize and assert success
// ===========================================================================

// GDB-1224: Token::lexeme is a non-owning std::string_view into the Lexer's
// source buffer. The parameter here MUST be std::string_view (not
// `const std::string&`): a `const std::string&` parameter still binds to a
// temporary std::string materialized from a string literal argument (e.g.
// tokenize_ok(".0e0")), and that temporary is destroyed at the end of the
// full expression -- after tokenize_ok() returns but its Token::lexeme
// views are still referenced by the caller. That produced a genuine
// use-after-free (observed as the leading byte of every returned lexeme
// silently becoming NUL, e.g. ".0e0" read back as "\00e0"). Taking
// std::string_view directly binds to the string literal's static storage
// instead, which lives for the entire program -- matching the safe idiom
// already used by tests/unit/test_lexer.cpp's tokenize_ok().
static std::vector<Token> tokenize_ok(std::string_view src) {
    Lexer lexer(src);
    auto result = lexer.tokenize();
    if (!result.has_value()) {
        ADD_FAILURE() << "Lex failed: " << result.error().message;
        return {};
    }
    return std::move(*result);
}

// ===========================================================================
// GDB-218 — Lexer: Dot-prefixed floats with scientific notation
// ===========================================================================

// --- Boundary values ---

TEST(QA_Lexer_218, DotZeroE0) {
    // Edge: zero mantissa and zero exponent
    auto tokens = tokenize_ok(".0e0");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, ".0e0");
}

TEST(QA_Lexer_218, DotSingleDigitPositiveExponent) {
    auto tokens = tokenize_ok(".1e5");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, ".1e5");
}

TEST(QA_Lexer_218, DotMultipleDecimalDigitsExponent) {
    auto tokens = tokenize_ok(".12345e67");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, ".12345e67");
}

TEST(QA_Lexer_218, DotExplicitPlusExponent) {
    auto tokens = tokenize_ok(".3e+7");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, ".3e+7");
}

TEST(QA_Lexer_218, DotNegativeExponentUpperE) {
    auto tokens = tokenize_ok(".42E-10");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, ".42E-10");
}

TEST(QA_Lexer_218, DotLargeExponent) {
    auto tokens = tokenize_ok(".1e999");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, ".1e999");
}

// --- Invalid exponent (should NOT consume e/E) ---

TEST(QA_Lexer_218, DotNoExponentDigitsUpperE) {
    // .5E without digits after E — should NOT consume E
    auto tokens = tokenize_ok(".5E");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, ".5");
}

TEST(QA_Lexer_218, DotSignButNoExponentDigits) {
    // .5e+ without digits after sign — should NOT consume e+
    auto tokens = tokenize_ok(".5e+");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, ".5");
}

TEST(QA_Lexer_218, DotMinusSignButNoExponentDigits) {
    // .5e- without digits — should NOT consume
    auto tokens = tokenize_ok(".5e-");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, ".5");
}

// --- Context: in SQL expressions ---

TEST(QA_Lexer_218, DotSciNotationInArithmetic) {
    // .5e2 + .3e1 → two dot-prefixed sci-notation floats
    auto tokens = tokenize_ok(".5e2 + .3e1");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, ".5e2");
    EXPECT_EQ(tokens[1].type, TokenType::PLUS);
    EXPECT_EQ(tokens[2].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[2].lexeme, ".3e1");
}

TEST(QA_Lexer_218, DotSciNotationFollowedByDot) {
    // .1e2.3 → .1e2 then .3 (both dot-prefixed floats)
    auto tokens = tokenize_ok(".1e2.3");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, ".1e2");
    EXPECT_EQ(tokens[1].type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(tokens[1].lexeme, ".3");
}

TEST(QA_Lexer_218, DotNotFollowedByDigitIsDot) {
    // Ensure .abc is DOT + IDENTIFIER, not treated as float
    auto tokens = tokenize_ok(".abc");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::DOT);
    EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER);
}

TEST(QA_Lexer_218, DotEIdentifierNotFloat) {
    // .e5 → DOT + IDENTIFIER (e is not digit, so dot-float path not entered)
    auto tokens = tokenize_ok(".e5");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::DOT);
}

// ===========================================================================
// GDB-219/220 — Binder: comparison + WHERE validation
// Fixture with standard test tables
// ===========================================================================

class QA_Binder_219_220 : public ::testing::Test {
protected:
    Catalog catalog;
    std::unique_ptr<Binder> binder;

    void SetUp() override {
        bootstrap_qa_catalog(catalog);
        // Table: users(id INT32, name STRING, age INT32, active BOOL)
        {
            TableSchema s;
            s.name = "users";
            s.columns = {
                {0, "id", TypeId::INT32, false, ""},
                {1, "name", TypeId::STRING, true, ""},
                {2, "age", TypeId::INT32, true, ""},
                {3, "active", TypeId::BOOL, false, ""},
            };
            s.pk_columns = "id";
            auto r = catalog.create_table(default_database_id, std::move(s));
            ASSERT_TRUE(r.has_value());
        }

        // Table: metrics(id INT32, value FLOAT64, ts TIMESTAMP, label STRING)
        {
            TableSchema s;
            s.name = "metrics";
            s.columns = {
                {0, "id", TypeId::INT32, false, ""},
                {1, "value", TypeId::FLOAT64, true, ""},
                {2, "ts", TypeId::TIMESTAMP, true, ""},
                {3, "label", TypeId::STRING, true, ""},
            };
            s.pk_columns = "id";
            auto r = catalog.create_table(default_database_id, std::move(s));
            ASSERT_TRUE(r.has_value());
        }

        binder = std::make_unique<Binder>(catalog);
    }

    StmtPtr parse(const std::string& sql) {
        Lexer lexer(sql);
        auto tokens = lexer.tokenize();
        if (!tokens.has_value()) {
            ADD_FAILURE() << "Lex failed: " << tokens.error().message;
            return nullptr;
        }
        Parser parser(std::move(*tokens));
        auto result = parser.parse();
        if (!result.has_value()) {
            ADD_FAILURE() << "Parse failed: " << result.error().message;
            return nullptr;
        }
        return std::move(*result);
    }

    BoundStatement bind_ok(const std::string& sql) {
        auto stmt = parse(sql);
        if (!stmt) {
            return {};
        }
        auto result = binder->bind(*stmt);
        if (!result.has_value()) {
            ADD_FAILURE() << "Bind failed: " << result.error().message;
            return {};
        }
        return std::move(*result);
    }

    void bind_error(const std::string& sql, StatusCode expected) {
        auto stmt = parse(sql);
        if (!stmt) {
            ADD_FAILURE() << "Parse failed unexpectedly for: " << sql;
            return;
        }
        auto result = binder->bind(*stmt);
        EXPECT_FALSE(result.has_value()) << "Expected bind error but succeeded for: " << sql;
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected)
                << "Expected " << status_code_name(expected) << " but got "
                << status_code_name(result.error().code) << ": " << result.error().message;
        }
    }
};

// ===========================================================================
// GDB-219: Comparison type validation — all operators
// ===========================================================================

TEST_F(QA_Binder_219_220, NotEqualIncompatibleTypes) {
    bind_error("SELECT name != 42 FROM users", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder_219_220, LessEqualIncompatibleTypes) {
    bind_error("SELECT name <= 10 FROM users", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder_219_220, GreaterEqualIncompatibleTypes) {
    bind_error("SELECT name >= 10 FROM users", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder_219_220, TimestampVsIntComparison) {
    bind_error("SELECT ts > 100 FROM metrics", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder_219_220, TimestampVsStringComparison) {
    bind_error("SELECT ts = 'hello' FROM metrics", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder_219_220, BoolVsStringComparison) {
    bind_error("SELECT active = 'true' FROM users", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder_219_220, BoolLessIntComparison) {
    bind_error("SELECT active < 1 FROM users", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder_219_220, FloatVsStringComparison) {
    bind_error("SELECT value > 'high' FROM metrics", StatusCode::TYPE_ERROR);
}

// --- NULL bypass: comparisons with NULL should succeed ---

TEST_F(QA_Binder_219_220, NullEqualIntOk) {
    auto bound = bind_ok("SELECT NULL = 1 FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::BOOL);
}

TEST_F(QA_Binder_219_220, IntEqualNullOk) {
    auto bound = bind_ok("SELECT id = NULL FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::BOOL);
}

TEST_F(QA_Binder_219_220, NullGreaterStringOk) {
    auto bound = bind_ok("SELECT NULL > 'abc' FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::BOOL);
}

TEST_F(QA_Binder_219_220, NullEqualNullOk) {
    auto bound = bind_ok("SELECT NULL = NULL FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::BOOL);
}

TEST_F(QA_Binder_219_220, NullNotEqualTimestampOk) {
    auto bound = bind_ok("SELECT NULL != ts FROM metrics");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::BOOL);
}

// --- Compatible types should still succeed ---

TEST_F(QA_Binder_219_220, IntComparedToFloatOk) {
    bind_ok("SELECT id > 3.14 FROM users");
}

TEST_F(QA_Binder_219_220, IntEqualIntOk) {
    bind_ok("SELECT id = age FROM users");
}

TEST_F(QA_Binder_219_220, StringComparedToStringOk) {
    bind_ok("SELECT name > label FROM users JOIN metrics ON users.id = metrics.id");
}

TEST_F(QA_Binder_219_220, FloatEqualFloatOk) {
    bind_ok("SELECT value = 2.5 FROM metrics");
}

// --- Comparison in WHERE clause ---

TEST_F(QA_Binder_219_220, WhereIncompatibleComparisonFails) {
    bind_error("SELECT id FROM users WHERE name > 5", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder_219_220, WhereCompatibleComparisonOk) {
    bind_ok("SELECT id FROM users WHERE id > 5");
}

TEST_F(QA_Binder_219_220, WhereNullComparisonOk) {
    bind_ok("SELECT id FROM users WHERE id = NULL");
}

// --- Comparison in HAVING clause ---

TEST_F(QA_Binder_219_220, HavingIncompatibleComparisonFails) {
    bind_error("SELECT name, COUNT(*) FROM users GROUP BY name HAVING name > 1",
               StatusCode::TYPE_ERROR);
}

// --- Comparison with chained AND ---

TEST_F(QA_Binder_219_220, ChainedAndWithIncompatibleFails) {
    bind_error("SELECT id FROM users WHERE id > 0 AND name > 5", StatusCode::TYPE_ERROR);
}

// --- Error message quality ---

TEST_F(QA_Binder_219_220, ComparisonErrorMentionsBothTypes) {
    auto stmt = parse("SELECT name > 42 FROM users");
    ASSERT_NE(stmt, nullptr);
    auto result = binder->bind(*stmt);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    // Error message should mention the types involved
    EXPECT_NE(result.error().message.find("STRING"), std::string::npos)
        << "Error should mention STRING: " << result.error().message;
    EXPECT_NE(result.error().message.find("INT"), std::string::npos)
        << "Error should mention INT: " << result.error().message;
}

// ===========================================================================
// GDB-220: UPDATE/DELETE WHERE boolean validation
// ===========================================================================

// --- UPDATE WHERE non-boolean types ---

TEST_F(QA_Binder_219_220, UpdateWhereFloatFails) {
    bind_error("UPDATE metrics SET value = 0.0 WHERE value", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder_219_220, UpdateWhereTimestampFails) {
    bind_error("UPDATE metrics SET value = 0.0 WHERE ts", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder_219_220, UpdateWhereArithmeticExprFails) {
    // WHERE id + 1 → result is INT, not BOOL
    bind_error("UPDATE users SET age = 0 WHERE id + 1", StatusCode::TYPE_ERROR);
}

// --- DELETE WHERE non-boolean types ---

TEST_F(QA_Binder_219_220, DeleteWhereFloatFails) {
    bind_error("DELETE FROM metrics WHERE value", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder_219_220, DeleteWhereTimestampFails) {
    bind_error("DELETE FROM metrics WHERE ts", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder_219_220, DeleteWhereArithmeticExprFails) {
    bind_error("DELETE FROM users WHERE id * 2", StatusCode::TYPE_ERROR);
}

// --- UPDATE/DELETE WHERE valid boolean expressions ---

TEST_F(QA_Binder_219_220, UpdateWhereComparisonOk) {
    bind_ok("UPDATE users SET age = 30 WHERE id = 1");
}

TEST_F(QA_Binder_219_220, DeleteWhereComparisonOk) {
    bind_ok("DELETE FROM users WHERE id > 0");
}

TEST_F(QA_Binder_219_220, UpdateWhereBoolColumnOk) {
    bind_ok("UPDATE users SET age = 30 WHERE active");
}

TEST_F(QA_Binder_219_220, DeleteWhereBoolColumnOk) {
    bind_ok("DELETE FROM users WHERE active");
}

TEST_F(QA_Binder_219_220, UpdateWhereNotBoolOk) {
    bind_ok("UPDATE users SET age = 0 WHERE NOT active");
}

TEST_F(QA_Binder_219_220, DeleteWhereAndExprOk) {
    bind_ok("DELETE FROM users WHERE active AND id > 0");
}

TEST_F(QA_Binder_219_220, UpdateWhereOrExprOk) {
    bind_ok("UPDATE users SET age = 25 WHERE active OR id < 10");
}

// --- UPDATE/DELETE WHERE error message quality ---

TEST_F(QA_Binder_219_220, UpdateWhereErrorMentionsType) {
    auto stmt = parse("UPDATE users SET age = 0 WHERE name");
    ASSERT_NE(stmt, nullptr);
    auto result = binder->bind(*stmt);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    // Error should mention "boolean" or "BOOL" and the actual type
    EXPECT_NE(result.error().message.find("boolean"), std::string::npos)
        << "Error should mention boolean: " << result.error().message;
    EXPECT_NE(result.error().message.find("STRING"), std::string::npos)
        << "Error should mention the actual type: " << result.error().message;
}

TEST_F(QA_Binder_219_220, DeleteWhereErrorMentionsType) {
    auto stmt = parse("DELETE FROM users WHERE id");
    ASSERT_NE(stmt, nullptr);
    auto result = binder->bind(*stmt);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    EXPECT_NE(result.error().message.find("boolean"), std::string::npos)
        << "Error should mention boolean: " << result.error().message;
    EXPECT_NE(result.error().message.find("INT32"), std::string::npos)
        << "Error should mention the actual type: " << result.error().message;
}

// --- Consistency: SELECT vs UPDATE vs DELETE WHERE validation ---

TEST_F(QA_Binder_219_220, SelectWhereStringAlsoFails) {
    // Verify SELECT WHERE non-bool still fails (baseline consistency check)
    bind_error("SELECT id FROM users WHERE name", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder_219_220, AllDmlWhereStringFails) {
    // All three DML statements should reject non-boolean WHERE
    bind_error("SELECT id FROM users WHERE name", StatusCode::TYPE_ERROR);
    bind_error("UPDATE users SET age = 0 WHERE name", StatusCode::TYPE_ERROR);
    bind_error("DELETE FROM users WHERE name", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder_219_220, AllDmlWhereIntFails) {
    bind_error("SELECT id FROM users WHERE id", StatusCode::TYPE_ERROR);
    bind_error("UPDATE users SET age = 0 WHERE id", StatusCode::TYPE_ERROR);
    bind_error("DELETE FROM users WHERE id", StatusCode::TYPE_ERROR);
}

// --- No WHERE clause: should still succeed ---

TEST_F(QA_Binder_219_220, UpdateWithoutWhereOk) {
    bind_ok("UPDATE users SET age = 99");
}

TEST_F(QA_Binder_219_220, DeleteWithoutWhereOk) {
    bind_ok("DELETE FROM users");
}

// ===========================================================================
// Interaction: GDB-219 + GDB-220 combined
// ===========================================================================

TEST_F(QA_Binder_219_220, UpdateWhereIncompatibleComparisonFails) {
    // Comparison type check (219) inside UPDATE WHERE (220)
    bind_error("UPDATE users SET age = 0 WHERE name > 5", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder_219_220, DeleteWhereIncompatibleComparisonFails) {
    // Comparison type check (219) inside DELETE WHERE (220)
    bind_error("DELETE FROM users WHERE name = 42", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder_219_220, UpdateWhereNullComparisonOk) {
    // NULL bypass (219) inside UPDATE WHERE (220)
    bind_ok("UPDATE users SET age = 0 WHERE id = NULL");
}

TEST_F(QA_Binder_219_220, DeleteWhereNullComparisonOk) {
    bind_ok("DELETE FROM users WHERE id = NULL");
}

} // namespace sixseven
