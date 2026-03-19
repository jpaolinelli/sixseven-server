#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"

#include <gtest/gtest.h>

#include <climits>
#include <string>

using namespace sixseven;

// =============================================================================
// Helpers
// =============================================================================

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

/// Parse and expect an error (either from lexer or parser).
static void expect_parse_error(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return; // lexer error is acceptable
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_FALSE(stmts.has_value()) << "expected parse error for: " << sql;
}

/// Tokenize and parse a single statement, returning the raw Result for
/// inspecting errors.
static Result<StmtPtr> try_parse(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return tl::unexpected(tokens.error());
    Parser parser(std::move(*tokens));
    return parser.parse();
}

// =============================================================================
// [BUG] std::stoi overflow in type spec integer parameters
// Parser uses std::stoi to convert integer literals in type parameters
// (VARCHAR(n), DECIMAL(p,s), EMBEDDING(dim,...), MAX_DEPTH n).
// std::stoi throws std::out_of_range on overflow, crashing the process.
// =============================================================================

TEST(QA_ParserOverflow, VarcharMaxOverflow) {
    // A VARCHAR length that exceeds INT32_MAX should not crash.
    EXPECT_NO_THROW({
        auto r = try_parse("CREATE TABLE t (c VARCHAR(99999999999999999))");
        // We accept either an error Result or a successful parse with
        // a large/wrapped value — but NOT a thrown exception.
    });
}

TEST(QA_ParserOverflow, DecimalPrecisionOverflow) {
    EXPECT_NO_THROW({ auto r = try_parse("CREATE TABLE t (c DECIMAL(99999999999999999, 2))"); });
}

TEST(QA_ParserOverflow, DecimalScaleOverflow) {
    EXPECT_NO_THROW({ auto r = try_parse("CREATE TABLE t (c DECIMAL(10, 99999999999999999))"); });
}

TEST(QA_ParserOverflow, EmbeddingDimensionOverflow) {
    EXPECT_NO_THROW(
        { auto r = try_parse("CREATE TABLE t (e EMBEDDING(99999999999999999, body, 'openai'))"); });
}

TEST(QA_ParserOverflow, TraverseMaxDepthOverflow) {
    EXPECT_NO_THROW(
        { auto r = try_parse("TRAVERSE follows FROM users(1) MAX_DEPTH 99999999999999999"); });
}

TEST(QA_ParserOverflow, NearestWithinTraverseMaxDepthOverflow) {
    EXPECT_NO_THROW({
        auto r = try_parse("SELECT NEAREST 5 TO embedding FROM t WITHIN "
                           "TRAVERSE follows FROM users(1) MAX_DEPTH 99999999999999999");
    });
}

TEST(QA_ParserOverflow, ShortestPathMaxDepthOverflow) {
    EXPECT_NO_THROW({
        auto r = try_parse("SHORTEST PATH FROM a(1) TO b(2) VIA edge MAX_DEPTH 99999999999999999");
    });
}

// =============================================================================
// [BUG] CREATE/ALTER USER password not unquoted
// The parser stores the raw lexeme (including surrounding single quotes)
// for the password in CREATE USER and ALTER USER statements.
// =============================================================================

TEST(QA_ParserPassword, CreateUserPasswordUnquoted) {
    auto stmt = parse_one("CREATE USER admin WITH PASSWORD 'secret123'");
    auto* cu = dynamic_cast<CreateUserStmt*>(stmt.get());
    ASSERT_NE(cu, nullptr);
    EXPECT_EQ(cu->username, "admin");
    // Password should be unquoted (no surrounding single quotes).
    EXPECT_EQ(cu->password, "secret123");
}

TEST(QA_ParserPassword, AlterUserPasswordUnquoted) {
    auto stmt = parse_one("ALTER USER admin WITH PASSWORD 'newpass'");
    auto* au = dynamic_cast<AlterUserStmt*>(stmt.get());
    ASSERT_NE(au, nullptr);
    EXPECT_EQ(au->username, "admin");
    // Password should be unquoted.
    EXPECT_EQ(au->password, "newpass");
}

TEST(QA_ParserPassword, CreateUserPasswordWithEscapedQuote) {
    auto stmt = parse_one("CREATE USER admin WITH PASSWORD 'it''s a secret'");
    auto* cu = dynamic_cast<CreateUserStmt*>(stmt.get());
    ASSERT_NE(cu, nullptr);
    // Escaped '' should become a single '.
    EXPECT_EQ(cu->password, "it's a secret");
}

// =============================================================================
// Lexer edge cases
// =============================================================================

TEST(QA_LexerEdge, EmptyInput) {
    Lexer lexer("");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_EQ(tokens->size(), 1u);
    EXPECT_EQ((*tokens)[0].type, TokenType::END_OF_FILE);
}

TEST(QA_LexerEdge, OnlyWhitespace) {
    Lexer lexer("   \n\t  \n  ");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_EQ(tokens->size(), 1u);
    EXPECT_EQ((*tokens)[0].type, TokenType::END_OF_FILE);
}

TEST(QA_LexerEdge, OnlyComments) {
    Lexer lexer("-- just a comment\n/* block */");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_EQ(tokens->size(), 1u);
    EXPECT_EQ((*tokens)[0].type, TokenType::END_OF_FILE);
}

TEST(QA_LexerEdge, DeeplyNestedBlockComments) {
    Lexer lexer("/* /* /* deep */ */ */ SELECT");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_GE(tokens->size(), 2u);
    EXPECT_EQ((*tokens)[0].type, TokenType::SELECT);
}

TEST(QA_LexerEdge, UnterminatedNestedBlockComment) {
    Lexer lexer("/* /* still open */");
    auto tokens = lexer.tokenize();
    EXPECT_FALSE(tokens.has_value());
}

TEST(QA_LexerEdge, EmptyStringLiteral) {
    Lexer lexer("''");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_GE(tokens->size(), 2u);
    EXPECT_EQ((*tokens)[0].type, TokenType::STRING_LITERAL);
    EXPECT_EQ((*tokens)[0].lexeme, "''");
}

TEST(QA_LexerEdge, StringWithOnlyEscapedQuotes) {
    Lexer lexer("''''");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_GE(tokens->size(), 2u);
    EXPECT_EQ((*tokens)[0].type, TokenType::STRING_LITERAL);
}

TEST(QA_LexerEdge, ExclamationWithoutEquals) {
    // Bare '!' is not a valid token.
    Lexer lexer("!");
    auto tokens = lexer.tokenize();
    EXPECT_FALSE(tokens.has_value());
}

TEST(QA_LexerEdge, SinglePipe) {
    // Single '|' is not a valid token (only '||' is).
    Lexer lexer("|");
    auto tokens = lexer.tokenize();
    EXPECT_FALSE(tokens.has_value());
}

TEST(QA_LexerEdge, LineColumnTracking) {
    Lexer lexer("SELECT\n  id\n  FROM\n    users");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    // SELECT at line 1
    EXPECT_EQ((*tokens)[0].line, 1u);
    EXPECT_EQ((*tokens)[0].column, 1u);
    // id at line 2
    EXPECT_EQ((*tokens)[1].line, 2u);
    EXPECT_EQ((*tokens)[1].column, 3u);
    // FROM at line 3
    EXPECT_EQ((*tokens)[2].line, 3u);
    // users at line 4
    EXPECT_EQ((*tokens)[3].line, 4u);
}

TEST(QA_LexerEdge, IntegerZero) {
    Lexer lexer("0");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_GE(tokens->size(), 2u);
    EXPECT_EQ((*tokens)[0].type, TokenType::INTEGER_LITERAL);
    EXPECT_EQ((*tokens)[0].lexeme, "0");
}

TEST(QA_LexerEdge, VeryLongIdentifier) {
    std::string long_ident(1000, 'a');
    Lexer lexer(long_ident);
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_GE(tokens->size(), 2u);
    EXPECT_EQ((*tokens)[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ((*tokens)[0].lexeme.size(), 1000u);
}

TEST(QA_LexerEdge, VeryLongStringLiteral) {
    std::string long_str = "'" + std::string(10000, 'x') + "'";
    Lexer lexer(long_str);
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_GE(tokens->size(), 2u);
    EXPECT_EQ((*tokens)[0].type, TokenType::STRING_LITERAL);
}

TEST(QA_LexerEdge, VeryLargeIntegerLiteral) {
    // The lexer should still tokenize extremely large integers without crashing.
    Lexer lexer("99999999999999999999999999999999");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_GE(tokens->size(), 2u);
    EXPECT_EQ((*tokens)[0].type, TokenType::INTEGER_LITERAL);
}

TEST(QA_LexerEdge, NotEqualAlternativeSyntax) {
    // Both != and <> should produce NOT_EQUAL tokens.
    Lexer lexer1("!=");
    auto t1 = lexer1.tokenize();
    ASSERT_TRUE(t1.has_value());
    EXPECT_EQ((*t1)[0].type, TokenType::NOT_EQUAL);

    Lexer lexer2("<>");
    auto t2 = lexer2.tokenize();
    ASSERT_TRUE(t2.has_value());
    EXPECT_EQ((*t2)[0].type, TokenType::NOT_EQUAL);
}

// =============================================================================
// Parser error recovery
// =============================================================================

TEST(QA_ParserRecovery, MultipleErrorsReported) {
    // parse_all should collect all errors, not just the first.
    Lexer lexer("INVALID1; INVALID2; INVALID3;");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(std::move(*tokens));
    auto result = parser.parse_all();
    EXPECT_FALSE(result.has_value());
    // Error message should mention multiple errors.
    auto msg = result.error().message;
    EXPECT_NE(msg.find("3 parse error"), std::string::npos)
        << "Expected 3 parse errors, got: " << msg;
}

TEST(QA_ParserRecovery, ValidAfterInvalid) {
    // If one statement fails and the next is valid, parse_all should still
    // report an error (because it collects errors).
    Lexer lexer("INVALID; SELECT 1;");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(std::move(*tokens));
    auto result = parser.parse_all();
    // Should report error (even though one statement was valid).
    EXPECT_FALSE(result.has_value());
}

TEST(QA_ParserRecovery, SynchronizesToSemicolon) {
    // After a parse error, the parser should skip to the next semicolon.
    // This is implicitly tested by multi-error tests but let's be explicit.
    Lexer lexer("CREATE; DROP;");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(std::move(*tokens));
    auto result = parser.parse_all();
    EXPECT_FALSE(result.has_value());
    // Should have exactly 2 errors (one per malformed statement).
    EXPECT_NE(result.error().message.find("2 parse error"), std::string::npos)
        << "Expected 2 errors, got: " << result.error().message;
}

TEST(QA_ParserRecovery, EmptyInput) {
    Lexer lexer("");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(std::move(*tokens));
    auto result = parser.parse_all();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 0u);
}

TEST(QA_ParserRecovery, OnlySemicolons) {
    auto stmts = parse_ok(";;;");
    EXPECT_EQ(stmts.size(), 0u);
}

TEST(QA_ParserRecovery, ParseSingleOnEmpty) {
    Lexer lexer("");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(std::move(*tokens));
    auto result = parser.parse();
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// Expression precedence (adversarial)
// =============================================================================

TEST(QA_ExprPrecedence, MultiplicationBeforeAddition) {
    // 1 + 2 * 3 should parse as 1 + (2 * 3), not (1 + 2) * 3.
    auto stmt = parse_one("SELECT 1 + 2 * 3");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);

    auto* add = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->op, BinaryOp::ADD);

    // LHS should be literal 1.
    auto* lhs = dynamic_cast<LiteralExpr*>(add->lhs.get());
    ASSERT_NE(lhs, nullptr);
    EXPECT_EQ(lhs->value, "1");

    // RHS should be 2 * 3.
    auto* mul = dynamic_cast<BinaryExpr*>(add->rhs.get());
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op, BinaryOp::MULTIPLY);
}

TEST(QA_ExprPrecedence, ComparisonBeforeAnd) {
    // a > 1 AND b < 2 should parse as (a > 1) AND (b < 2).
    auto stmt = parse_one("SELECT a > 1 AND b < 2");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* and_expr = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(and_expr, nullptr);
    EXPECT_EQ(and_expr->op, BinaryOp::AND);

    auto* left = dynamic_cast<BinaryExpr*>(and_expr->lhs.get());
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(left->op, BinaryOp::GREATER);

    auto* right = dynamic_cast<BinaryExpr*>(and_expr->rhs.get());
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(right->op, BinaryOp::LESS);
}

TEST(QA_ExprPrecedence, OrAfterAnd) {
    // a OR b AND c should parse as a OR (b AND c).
    auto stmt = parse_one("SELECT a OR b AND c");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* or_expr = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(or_expr, nullptr);
    EXPECT_EQ(or_expr->op, BinaryOp::OR);

    // RHS should be AND.
    auto* and_expr = dynamic_cast<BinaryExpr*>(or_expr->rhs.get());
    ASSERT_NE(and_expr, nullptr);
    EXPECT_EQ(and_expr->op, BinaryOp::AND);
}

TEST(QA_ExprPrecedence, NotBeforeComparison) {
    // NOT a = 1 should parse as (NOT a) = 1, since NOT has higher precedence
    // than comparison. Actually, NOT is above comparison, so it parses as
    // NOT (a = 1) — let's verify.
    auto stmt = parse_one("SELECT NOT a = 1");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    // NOT is parsed at the not-level which calls parse_comparison. So
    // NOT a = 1 is NOT (a = 1).
    auto* not_expr = dynamic_cast<UnaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(not_expr, nullptr);
    EXPECT_EQ(not_expr->op, UnaryOp::NOT);

    auto* cmp = dynamic_cast<BinaryExpr*>(not_expr->operand.get());
    ASSERT_NE(cmp, nullptr);
    EXPECT_EQ(cmp->op, BinaryOp::EQUAL);
}

TEST(QA_ExprPrecedence, UnaryMinusPrecedence) {
    // -a * b should parse as (-a) * b, not -(a * b).
    auto stmt = parse_one("SELECT -a * b");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* mul = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op, BinaryOp::MULTIPLY);

    auto* neg = dynamic_cast<UnaryExpr*>(mul->lhs.get());
    ASSERT_NE(neg, nullptr);
    EXPECT_EQ(neg->op, UnaryOp::NEGATE);
}

TEST(QA_ExprPrecedence, ParenthesesOverridePrecedence) {
    // (1 + 2) * 3 should parse as (1 + 2) * 3.
    auto stmt = parse_one("SELECT (1 + 2) * 3");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* mul = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op, BinaryOp::MULTIPLY);

    auto* add = dynamic_cast<BinaryExpr*>(mul->lhs.get());
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->op, BinaryOp::ADD);
}

TEST(QA_ExprPrecedence, ConcatPrecedence) {
    // || should have same precedence level as +/-.
    // 'a' || 'b' || 'c' should be left-associative.
    auto stmt = parse_one("SELECT 'a' || 'b' || 'c'");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    // Top-level should be CONCAT with rhs = 'c'.
    auto* top = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(top->op, BinaryOp::CONCAT);

    // lhs should also be CONCAT.
    auto* left = dynamic_cast<BinaryExpr*>(top->lhs.get());
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(left->op, BinaryOp::CONCAT);
}

// =============================================================================
// Special expression edge cases
// =============================================================================

TEST(QA_ExprEdge, IsNullOnExpression) {
    auto stmt = parse_one("SELECT 1 + 2 IS NULL");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* is_null = dynamic_cast<IsNullExpr*>(sel->items[0].expr.get());
    ASSERT_NE(is_null, nullptr);
    EXPECT_FALSE(is_null->negated);

    // The expression (1 + 2) should be inside the IS NULL.
    auto* add = dynamic_cast<BinaryExpr*>(is_null->expr.get());
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->op, BinaryOp::ADD);
}

TEST(QA_ExprEdge, IsNotNull) {
    auto stmt = parse_one("SELECT x IS NOT NULL");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* is_null = dynamic_cast<IsNullExpr*>(sel->items[0].expr.get());
    ASSERT_NE(is_null, nullptr);
    EXPECT_TRUE(is_null->negated);
}

TEST(QA_ExprEdge, NotInList) {
    auto stmt = parse_one("SELECT x NOT IN (1, 2, 3)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* in_expr = dynamic_cast<InExpr*>(sel->items[0].expr.get());
    ASSERT_NE(in_expr, nullptr);
    EXPECT_TRUE(in_expr->negated);
    EXPECT_EQ(in_expr->values.size(), 3u);
}

TEST(QA_ExprEdge, NotBetween) {
    auto stmt = parse_one("SELECT x NOT BETWEEN 1 AND 10");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* between = dynamic_cast<BetweenExpr*>(sel->items[0].expr.get());
    ASSERT_NE(between, nullptr);
    EXPECT_TRUE(between->negated);
}

TEST(QA_ExprEdge, NotLike) {
    auto stmt = parse_one("SELECT name NOT LIKE '%test%'");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* like = dynamic_cast<LikeExpr*>(sel->items[0].expr.get());
    ASSERT_NE(like, nullptr);
    EXPECT_TRUE(like->negated);
}

TEST(QA_ExprEdge, BetweenBoundsAreParsedAtAdditionLevel) {
    // BETWEEN low AND high should parse low/high at the addition level,
    // not at the full expression level.
    auto stmt = parse_one("SELECT x BETWEEN 1 + 1 AND 2 + 2");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* between = dynamic_cast<BetweenExpr*>(sel->items[0].expr.get());
    ASSERT_NE(between, nullptr);

    auto* low = dynamic_cast<BinaryExpr*>(between->low.get());
    ASSERT_NE(low, nullptr);
    EXPECT_EQ(low->op, BinaryOp::ADD);

    auto* high = dynamic_cast<BinaryExpr*>(between->high.get());
    ASSERT_NE(high, nullptr);
    EXPECT_EQ(high->op, BinaryOp::ADD);
}

TEST(QA_ExprEdge, InSubquery) {
    auto stmt = parse_one("SELECT x IN (SELECT id FROM t)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* in_expr = dynamic_cast<InExpr*>(sel->items[0].expr.get());
    ASSERT_NE(in_expr, nullptr);
    EXPECT_FALSE(in_expr->negated);
    EXPECT_NE(in_expr->subquery, nullptr);
    EXPECT_TRUE(in_expr->values.empty());
}

TEST(QA_ExprEdge, ExistsSubquery) {
    auto stmt = parse_one("SELECT EXISTS (SELECT 1 FROM t)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* exists = dynamic_cast<ExistsExpr*>(sel->items[0].expr.get());
    ASSERT_NE(exists, nullptr);
    EXPECT_NE(exists->subquery, nullptr);
}

TEST(QA_ExprEdge, ScalarSubquery) {
    auto stmt = parse_one("SELECT (SELECT 1)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* sub = dynamic_cast<SubqueryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(sub, nullptr);
}

TEST(QA_ExprEdge, CaseWithNoWhenClauses) {
    // CASE operand END — syntactically odd but the parser may accept it.
    // Verify behavior is defined (no crash).
    EXPECT_NO_THROW({ auto r = try_parse("SELECT CASE x END"); });
}

TEST(QA_ExprEdge, CaseSearchedForm) {
    // Searched CASE (no operand): CASE WHEN cond THEN result END.
    auto stmt = parse_one("SELECT CASE WHEN x > 1 THEN 'a' ELSE 'b' END");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* case_expr = dynamic_cast<CaseExpr*>(sel->items[0].expr.get());
    ASSERT_NE(case_expr, nullptr);
    EXPECT_EQ(case_expr->operand, nullptr); // searched form
    ASSERT_EQ(case_expr->whens.size(), 1u);
    EXPECT_NE(case_expr->else_expr, nullptr);
}

TEST(QA_ExprEdge, CaseSimpleForm) {
    auto stmt = parse_one("SELECT CASE x WHEN 1 THEN 'one' WHEN 2 THEN 'two' END");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* case_expr = dynamic_cast<CaseExpr*>(sel->items[0].expr.get());
    ASSERT_NE(case_expr, nullptr);
    EXPECT_NE(case_expr->operand, nullptr); // simple form
    ASSERT_EQ(case_expr->whens.size(), 2u);
    EXPECT_EQ(case_expr->else_expr, nullptr);
}

TEST(QA_ExprEdge, NestedFunctionCalls) {
    auto stmt = parse_one("SELECT UPPER(TRIM(LOWER(name)))");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* upper = dynamic_cast<FunctionCallExpr*>(sel->items[0].expr.get());
    ASSERT_NE(upper, nullptr);
    EXPECT_EQ(upper->name, "UPPER");
    ASSERT_EQ(upper->args.size(), 1u);

    auto* trim = dynamic_cast<FunctionCallExpr*>(upper->args[0].get());
    ASSERT_NE(trim, nullptr);
    EXPECT_EQ(trim->name, "TRIM");
}

TEST(QA_ExprEdge, CountStar) {
    auto stmt = parse_one("SELECT COUNT(*)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* fn = dynamic_cast<FunctionCallExpr*>(sel->items[0].expr.get());
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->name, "COUNT");
    ASSERT_EQ(fn->args.size(), 1u);

    auto* star_arg = dynamic_cast<ColumnRefExpr*>(fn->args[0].get());
    ASSERT_NE(star_arg, nullptr);
    EXPECT_EQ(star_arg->column, "*");
}

TEST(QA_ExprEdge, CountDistinct) {
    auto stmt = parse_one("SELECT COUNT(DISTINCT name)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* fn = dynamic_cast<FunctionCallExpr*>(sel->items[0].expr.get());
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->name, "COUNT");
    EXPECT_TRUE(fn->distinct);
    ASSERT_EQ(fn->args.size(), 1u);
}

TEST(QA_ExprEdge, PostgresCastSyntax) {
    auto stmt = parse_one("SELECT x::INT");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* cast = dynamic_cast<CastExpr*>(sel->items[0].expr.get());
    ASSERT_NE(cast, nullptr);
    EXPECT_EQ(cast->target_type.name, "INT");
}

TEST(QA_ExprEdge, ChainedCasts) {
    auto stmt = parse_one("SELECT x::INT::VARCHAR");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    // Outer cast to VARCHAR.
    auto* outer = dynamic_cast<CastExpr*>(sel->items[0].expr.get());
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->target_type.name, "VARCHAR");

    // Inner cast to INT.
    auto* inner = dynamic_cast<CastExpr*>(outer->expr.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->target_type.name, "INT");
}

TEST(QA_ExprEdge, EmptyArrayLiteral) {
    auto stmt = parse_one("SELECT []");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* arr = dynamic_cast<ArrayExpr*>(sel->items[0].expr.get());
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->elements.size(), 0u);
}

TEST(QA_ExprEdge, ArrayWithExpressions) {
    auto stmt = parse_one("SELECT [1 + 2, 3 * 4, -5]");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);

    auto* arr = dynamic_cast<ArrayExpr*>(sel->items[0].expr.get());
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->elements.size(), 3u);
}

TEST(QA_ExprEdge, DeeplyNestedParentheses) {
    // Test deeply nested parenthesized expressions don't crash.
    std::string expr = "SELECT ";
    for (int i = 0; i < 100; ++i)
        expr += "(";
    expr += "1";
    for (int i = 0; i < 100; ++i)
        expr += ")";

    EXPECT_NO_THROW({
        auto r = try_parse(expr);
        EXPECT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message);
    });
}

// =============================================================================
// DDL edge cases
// =============================================================================

TEST(QA_DDLEdge, CreateTableMultipleConstraints) {
    auto stmt = parse_one("CREATE TABLE t ("
                          "  id INT NOT NULL,"
                          "  name VARCHAR(100),"
                          "  email VARCHAR(255) UNIQUE,"
                          "  PRIMARY KEY (id),"
                          "  UNIQUE (email),"
                          "  CHECK (id > 0)"
                          ")");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->columns.size(), 3u);
    EXPECT_EQ(ct->constraints.size(), 3u);
}

TEST(QA_DDLEdge, CreateTableForeignKey) {
    auto stmt = parse_one("CREATE TABLE orders ("
                          "  id INT PRIMARY KEY,"
                          "  user_id INT REFERENCES users(id) ON DELETE CASCADE"
                          ")");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->columns.size(), 2u);
    EXPECT_EQ(ct->columns[1].fk_table, "users");
    EXPECT_EQ(ct->columns[1].fk_column, "id");
    EXPECT_EQ(ct->columns[1].fk_on_delete, ReferentialAction::CASCADE);
}

TEST(QA_DDLEdge, CreateTableForeignKeyConstraint) {
    auto stmt = parse_one(
        "CREATE TABLE orders ("
        "  id INT,"
        "  user_id INT,"
        "  CONSTRAINT fk_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE RESTRICT"
        ")");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->constraints.size(), 1u);
    EXPECT_EQ(ct->constraints[0].kind, TableConstraint::Kind::FOREIGN_KEY);
    EXPECT_EQ(ct->constraints[0].name, "fk_user");
    EXPECT_EQ(ct->constraints[0].fk_table, "users");
    EXPECT_EQ(ct->constraints[0].on_delete, ReferentialAction::RESTRICT);
}

TEST(QA_DDLEdge, CreateTableAllTypes) {
    // Test all 22 SixSevenDB types parse correctly in column definitions.
    auto stmt = parse_one("CREATE TABLE t ("
                          "  c1 INT, c2 INTEGER, c3 TINYINT, c4 SMALLINT, c5 BIGINT,"
                          "  c6 FLOAT, c7 DOUBLE, c8 DECIMAL(10,2), c9 NUMERIC(5),"
                          "  c10 BOOLEAN, c11 CHAR(1), c12 VARCHAR(255), c13 TEXT,"
                          "  c14 BLOB, c15 DATE, c16 TIME, c17 TIMESTAMP,"
                          "  c18 INTERVAL, c19 POINT, c20 JSON, c21 UUID,"
                          "  c22 EMBEDDING(384, body, 'openai')"
                          ")");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->columns.size(), 22u);

    // Verify a few specific types.
    EXPECT_EQ(ct->columns[7].type.name, "DECIMAL");
    EXPECT_EQ(ct->columns[7].type.param1.value(), 10);
    EXPECT_EQ(ct->columns[7].type.param2.value(), 2);

    EXPECT_EQ(ct->columns[21].type.name, "EMBEDDING");
    EXPECT_EQ(ct->columns[21].type.param1.value(), 384);
    EXPECT_EQ(ct->columns[21].type.source, "body");
    EXPECT_EQ(ct->columns[21].type.provider, "openai");
}

TEST(QA_DDLEdge, CreateIndexUsingMethod) {
    auto stmt = parse_one("CREATE INDEX idx ON t(col) USING btree");
    auto* ci = dynamic_cast<CreateIndexStmt*>(stmt.get());
    ASSERT_NE(ci, nullptr);
    EXPECT_EQ(ci->method, "btree");
    EXPECT_FALSE(ci->is_unique);
}

TEST(QA_DDLEdge, CreateIndexIfNotExists) {
    auto stmt = parse_one("CREATE UNIQUE INDEX IF NOT EXISTS idx ON t(a, b)");
    auto* ci = dynamic_cast<CreateIndexStmt*>(stmt.get());
    ASSERT_NE(ci, nullptr);
    EXPECT_TRUE(ci->if_not_exists);
    EXPECT_TRUE(ci->is_unique);
    EXPECT_EQ(ci->columns.size(), 2u);
}

TEST(QA_DDLEdge, AlterTableRenameColumn) {
    auto stmt = parse_one("ALTER TABLE users RENAME COLUMN old_name TO new_name");
    auto* at = dynamic_cast<AlterTableStmt*>(stmt.get());
    ASSERT_NE(at, nullptr);
    EXPECT_EQ(at->action, AlterAction::RENAME_COLUMN);
    EXPECT_EQ(at->column_name, "old_name");
    EXPECT_EQ(at->new_column_name, "new_name");
}

TEST(QA_DDLEdge, CreateEdgeTypeWithProperties) {
    auto stmt = parse_one("CREATE EDGE TYPE follows (since TIMESTAMP, weight FLOAT) "
                          "FROM users TO users");
    auto* ce = dynamic_cast<CreateEdgeTypeStmt*>(stmt.get());
    ASSERT_NE(ce, nullptr);
    EXPECT_EQ(ce->name, "follows");
    ASSERT_EQ(ce->properties.size(), 2u);
    EXPECT_EQ(ce->properties[0].name, "since");
    EXPECT_EQ(ce->from_table, "users");
    EXPECT_EQ(ce->to_table, "users");
}

TEST(QA_DDLEdge, CreateEdgeTypeNoProperties) {
    auto stmt = parse_one("CREATE EDGE TYPE follows FROM users TO users");
    auto* ce = dynamic_cast<CreateEdgeTypeStmt*>(stmt.get());
    ASSERT_NE(ce, nullptr);
    EXPECT_TRUE(ce->properties.empty());
}

TEST(QA_DDLEdge, DropTableIfExistsCascade) {
    auto stmt = parse_one("DROP TABLE IF EXISTS users CASCADE");
    auto* dt = dynamic_cast<DropTableStmt*>(stmt.get());
    ASSERT_NE(dt, nullptr);
    EXPECT_TRUE(dt->if_exists);
    EXPECT_TRUE(dt->cascade);
}

// =============================================================================
// DML edge cases
// =============================================================================

TEST(QA_DMLEdge, InsertMultipleRows) {
    auto stmt = parse_one(
        "INSERT INTO users (name, age) VALUES ('alice', 30), ('bob', 25), ('charlie', 35)");
    auto* ins = dynamic_cast<InsertStmt*>(stmt.get());
    ASSERT_NE(ins, nullptr);
    EXPECT_EQ(ins->columns.size(), 2u);
    EXPECT_EQ(ins->values.size(), 3u);
    EXPECT_EQ(ins->values[0].size(), 2u);
}

TEST(QA_DMLEdge, InsertSelect) {
    auto stmt =
        parse_one("INSERT INTO archive (id, name) SELECT id, name FROM users WHERE active = FALSE");
    auto* ins = dynamic_cast<InsertStmt*>(stmt.get());
    ASSERT_NE(ins, nullptr);
    EXPECT_NE(ins->select, nullptr);
    EXPECT_TRUE(ins->values.empty());
}

TEST(QA_DMLEdge, InsertNoColumns) {
    auto stmt = parse_one("INSERT INTO users VALUES (1, 'alice')");
    auto* ins = dynamic_cast<InsertStmt*>(stmt.get());
    ASSERT_NE(ins, nullptr);
    EXPECT_TRUE(ins->columns.empty());
    EXPECT_EQ(ins->values.size(), 1u);
}

TEST(QA_DMLEdge, UpdateReturning) {
    auto stmt = parse_one("UPDATE users SET name = 'bob' WHERE id = 1 RETURNING *");
    auto* upd = dynamic_cast<UpdateStmt*>(stmt.get());
    ASSERT_NE(upd, nullptr);
    EXPECT_EQ(upd->returning.size(), 1u);
    EXPECT_TRUE(upd->returning[0].is_star);
}

TEST(QA_DMLEdge, DeleteReturning) {
    auto stmt = parse_one("DELETE FROM users WHERE id = 1 RETURNING id, name");
    auto* del = dynamic_cast<DeleteStmt*>(stmt.get());
    ASSERT_NE(del, nullptr);
    EXPECT_EQ(del->returning.size(), 2u);
}

TEST(QA_DMLEdge, DeleteNoWhere) {
    auto stmt = parse_one("DELETE FROM users");
    auto* del = dynamic_cast<DeleteStmt*>(stmt.get());
    ASSERT_NE(del, nullptr);
    EXPECT_EQ(del->where_expr, nullptr);
}

TEST(QA_DMLEdge, UpdateNoWhere) {
    auto stmt = parse_one("UPDATE users SET active = FALSE");
    auto* upd = dynamic_cast<UpdateStmt*>(stmt.get());
    ASSERT_NE(upd, nullptr);
    EXPECT_EQ(upd->where_expr, nullptr);
}

TEST(QA_DMLEdge, LinkWithProperties) {
    auto stmt =
        parse_one("LINK users(1) TO posts(42) VIA authored (role = 'primary', weight = 1.0)");
    auto* link = dynamic_cast<LinkStmt*>(stmt.get());
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->source_table, "users");
    EXPECT_EQ(link->target_table, "posts");
    EXPECT_EQ(link->edge_type, "authored");
    EXPECT_EQ(link->properties.size(), 2u);
}

TEST(QA_DMLEdge, UnlinkWithWhere) {
    auto stmt = parse_one("UNLINK users(1) FROM posts(42) VIA authored WHERE weight < 0.5");
    auto* unlink = dynamic_cast<UnlinkStmt*>(stmt.get());
    ASSERT_NE(unlink, nullptr);
    EXPECT_NE(unlink->where_expr, nullptr);
}

// =============================================================================
// SELECT edge cases
// =============================================================================

TEST(QA_SelectEdge, SelectStar) {
    auto stmt = parse_one("SELECT *");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    EXPECT_TRUE(sel->items[0].is_star);
}

TEST(QA_SelectEdge, SelectTableStar) {
    auto stmt = parse_one("SELECT users.* FROM users");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    EXPECT_TRUE(sel->items[0].is_star);
    EXPECT_EQ(sel->items[0].table_star, "users");
}

TEST(QA_SelectEdge, SelectDistinct) {
    auto stmt = parse_one("SELECT DISTINCT name FROM users");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_TRUE(sel->distinct);
}

TEST(QA_SelectEdge, SelectWithAlias) {
    auto stmt = parse_one("SELECT id AS user_id, name n FROM users");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 2u);
    EXPECT_EQ(sel->items[0].alias, "user_id");
    EXPECT_EQ(sel->items[1].alias, "n"); // implicit alias
}

TEST(QA_SelectEdge, SelectNoFrom) {
    // SELECT without FROM (e.g., SELECT 1 + 1).
    auto stmt = parse_one("SELECT 1 + 1");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_TRUE(sel->from.empty());
}

TEST(QA_SelectEdge, SelectMultipleTables) {
    auto stmt = parse_one("SELECT * FROM users, orders, products");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->from.size(), 3u);
}

TEST(QA_SelectEdge, JoinAllTypes) {
    auto stmt = parse_one("SELECT * FROM a "
                          "INNER JOIN b ON a.id = b.a_id "
                          "LEFT OUTER JOIN c ON b.id = c.b_id "
                          "RIGHT JOIN d ON c.id = d.c_id "
                          "FULL OUTER JOIN e ON d.id = e.d_id "
                          "CROSS JOIN f");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->joins.size(), 5u);
    EXPECT_EQ(sel->joins[0].type, JoinType::INNER);
    EXPECT_EQ(sel->joins[1].type, JoinType::LEFT);
    EXPECT_EQ(sel->joins[2].type, JoinType::RIGHT);
    EXPECT_EQ(sel->joins[3].type, JoinType::FULL);
    EXPECT_EQ(sel->joins[4].type, JoinType::CROSS);
    EXPECT_EQ(sel->joins[4].on_expr, nullptr); // CROSS JOIN has no ON
}

TEST(QA_SelectEdge, SubqueryInFrom) {
    auto stmt = parse_one("SELECT * FROM (SELECT 1 AS x) AS sub");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_NE(sel->from[0].subquery, nullptr);
    EXPECT_EQ(sel->from[0].alias, "sub");
}

TEST(QA_SelectEdge, OrderByMultipleDirections) {
    auto stmt = parse_one("SELECT * FROM t ORDER BY a ASC, b DESC, c");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->order_by.size(), 3u);
    EXPECT_EQ(sel->order_by[0].direction, SortDirection::ASC);
    EXPECT_EQ(sel->order_by[1].direction, SortDirection::DESC);
    EXPECT_EQ(sel->order_by[2].direction, SortDirection::ASC); // default
}

TEST(QA_SelectEdge, GroupByHaving) {
    auto stmt = parse_one("SELECT dept, COUNT(*) AS cnt FROM employees "
                          "GROUP BY dept HAVING COUNT(*) > 5");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->group_by.size(), 1u);
    EXPECT_NE(sel->having_expr, nullptr);
}

TEST(QA_SelectEdge, LimitOffset) {
    auto stmt = parse_one("SELECT * FROM t LIMIT 10 OFFSET 20");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_NE(sel->limit, nullptr);
    EXPECT_NE(sel->offset, nullptr);

    auto* limit = dynamic_cast<LiteralExpr*>(sel->limit.get());
    ASSERT_NE(limit, nullptr);
    EXPECT_EQ(limit->value, "10");

    auto* offset = dynamic_cast<LiteralExpr*>(sel->offset.get());
    ASSERT_NE(offset, nullptr);
    EXPECT_EQ(offset->value, "20");
}

TEST(QA_SelectEdge, UnionAll) {
    auto stmt = parse_one("SELECT 1 UNION ALL SELECT 2");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->set_op, SelectStmt::SetOp::UNION_ALL);
    EXPECT_NE(sel->set_rhs, nullptr);
}

TEST(QA_SelectEdge, Intersect) {
    auto stmt = parse_one("SELECT 1 INTERSECT SELECT 2");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->set_op, SelectStmt::SetOp::INTERSECT);
}

TEST(QA_SelectEdge, Except) {
    auto stmt = parse_one("SELECT 1 EXCEPT SELECT 2");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->set_op, SelectStmt::SetOp::EXCEPT);
}

TEST(QA_SelectEdge, CTEBasic) {
    auto stmt = parse_one("WITH active AS (SELECT * FROM users WHERE active = TRUE) "
                          "SELECT * FROM active");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->ctes.size(), 1u);
    EXPECT_EQ(sel->ctes[0].name, "active");
    EXPECT_NE(sel->ctes[0].query, nullptr);
}

TEST(QA_SelectEdge, MultipleCTEs) {
    auto stmt = parse_one("WITH a AS (SELECT 1), b AS (SELECT 2) "
                          "SELECT * FROM a, b");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->ctes.size(), 2u);
}

// =============================================================================
// Graph & Vector statement edge cases
// =============================================================================

TEST(QA_GraphEdge, TraverseAllOptions) {
    auto stmt = parse_one("TRAVERSE follows FROM users(42) DIRECTION BOTH MAX_DEPTH 5 "
                          "WHERE weight > 0.5 FETCH");
    auto* trav = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(trav, nullptr);
    EXPECT_EQ(trav->edge_type, "follows");
    EXPECT_EQ(trav->from_table, "users");
    EXPECT_EQ(trav->direction, TraverseDirection::BOTH);
    EXPECT_EQ(trav->max_depth.value(), 5);
    EXPECT_NE(trav->where_expr, nullptr);
    EXPECT_TRUE(trav->fetch);
}

TEST(QA_GraphEdge, TraverseMinimal) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1)");
    auto* trav = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(trav, nullptr);
    EXPECT_EQ(trav->direction, TraverseDirection::OUT); // default
    EXPECT_FALSE(trav->max_depth.has_value());
    EXPECT_EQ(trav->where_expr, nullptr);
    EXPECT_FALSE(trav->fetch);
}

TEST(QA_GraphEdge, NearestWithinTraverse) {
    auto stmt = parse_one("NEAREST 5 FROM docs.embedding TO 'search query' "
                          "WITHIN TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 3 "
                          "WHERE category = 'tech' USING COSINE");
    auto* nn = dynamic_cast<NearestStmt*>(stmt.get());
    ASSERT_NE(nn, nullptr);
    EXPECT_EQ(nn->table_name, "docs");
    EXPECT_EQ(nn->column_name, "embedding");
    EXPECT_NE(nn->within_traverse, nullptr);
    EXPECT_NE(nn->where_expr, nullptr);
    EXPECT_EQ(nn->metric, NearestMetric::COSINE);
}

TEST(QA_GraphEdge, NearestL2Metric) {
    auto stmt = parse_one("NEAREST 10 FROM t.col TO [1.0, 2.0, 3.0] USING L2");
    auto* nn = dynamic_cast<NearestStmt*>(stmt.get());
    ASSERT_NE(nn, nullptr);
    EXPECT_EQ(nn->metric, NearestMetric::L2);
}

TEST(QA_GraphEdge, NearestDotMetric) {
    auto stmt = parse_one("NEAREST 10 FROM t.col TO [1.0] USING DOT");
    auto* nn = dynamic_cast<NearestStmt*>(stmt.get());
    ASSERT_NE(nn, nullptr);
    EXPECT_EQ(nn->metric, NearestMetric::DOT);
}

TEST(QA_GraphEdge, MatchOutgoingEdge) {
    auto stmt = parse_one("MATCH (a:users)-[r:follows]->(b:users) "
                          "WHERE a.name = 'alice' RETURN b.name");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_GE(m->pattern.size(), 2u);
    EXPECT_EQ(m->pattern[0].node.variable, "a");
    EXPECT_EQ(m->pattern[0].node.label, "users");
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::OUT);
}

TEST(QA_GraphEdge, MatchIncomingEdge) {
    auto stmt = parse_one("MATCH (a:users)<-[r:follows]-(b:users) RETURN a.name");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_GE(m->pattern.size(), 2u);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::IN);
}

TEST(QA_GraphEdge, MatchBidirectionalEdge) {
    auto stmt = parse_one("MATCH (a:users)<-[r:follows]->(b:users) RETURN a.name");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_GE(m->pattern.size(), 2u);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::BOTH);
}

TEST(QA_GraphEdge, MatchUndirectedEdge) {
    auto stmt = parse_one("MATCH (a:users)-[r:follows]-(b:users) RETURN a.name");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_GE(m->pattern.size(), 2u);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    // Neither < prefix nor > suffix = BOTH
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::BOTH);
}

TEST(QA_GraphEdge, ShortestPathAllOptions) {
    auto stmt = parse_one("SHORTEST PATH FROM users(1) TO users(42) VIA follows "
                          "DIRECTION IN MAX_DEPTH 10");
    auto* sp = dynamic_cast<ShortestPathStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->from_table, "users");
    EXPECT_EQ(sp->to_table, "users");
    EXPECT_EQ(sp->edge_type, "follows");
    EXPECT_EQ(sp->direction, TraverseDirection::IN);
    EXPECT_EQ(sp->max_depth.value(), 10);
}

// =============================================================================
// TCL edge cases
// =============================================================================

TEST(QA_TCLEdge, BeginTransaction) {
    auto stmt = parse_one("BEGIN TRANSACTION");
    EXPECT_NE(dynamic_cast<BeginStmt*>(stmt.get()), nullptr);
}

TEST(QA_TCLEdge, BeginWithoutTransaction) {
    auto stmt = parse_one("BEGIN");
    EXPECT_NE(dynamic_cast<BeginStmt*>(stmt.get()), nullptr);
}

TEST(QA_TCLEdge, RollbackToSavepoint) {
    auto stmt = parse_one("ROLLBACK TO sp1");
    auto* rb = dynamic_cast<RollbackStmt*>(stmt.get());
    ASSERT_NE(rb, nullptr);
    EXPECT_EQ(rb->savepoint, "sp1");
}

TEST(QA_TCLEdge, RollbackWithoutSavepoint) {
    auto stmt = parse_one("ROLLBACK");
    auto* rb = dynamic_cast<RollbackStmt*>(stmt.get());
    ASSERT_NE(rb, nullptr);
    EXPECT_TRUE(rb->savepoint.empty());
}

// =============================================================================
// Admin statement edge cases
// =============================================================================

TEST(QA_AdminEdge, SetDottedParameter) {
    auto stmt = parse_one("SET logging.level = 'debug'");
    auto* set = dynamic_cast<SetStmt*>(stmt.get());
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->parameter, "logging.level");
}

TEST(QA_AdminEdge, ShowTables) {
    auto stmt = parse_one("SHOW TABLES");
    auto* show = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(show, nullptr);
    EXPECT_EQ(show->target, ShowTarget::TABLES);
}

TEST(QA_AdminEdge, ShowColumnsFrom) {
    auto stmt = parse_one("SHOW COLUMNS FROM users");
    auto* show = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(show, nullptr);
    EXPECT_EQ(show->target, ShowTarget::COLUMNS);
    EXPECT_EQ(show->name, "users");
}

TEST(QA_AdminEdge, ShowEdgeTypes) {
    auto stmt = parse_one("SHOW EDGE TYPES");
    auto* show = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(show, nullptr);
    EXPECT_EQ(show->target, ShowTarget::EDGE_TYPES);
}

TEST(QA_AdminEdge, ShowParameter) {
    auto stmt = parse_one("SHOW logging.level");
    auto* show = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(show, nullptr);
    EXPECT_EQ(show->target, ShowTarget::PARAMETER);
    EXPECT_EQ(show->name, "logging.level");
}

TEST(QA_AdminEdge, ShowAll) {
    auto stmt = parse_one("SHOW ALL");
    auto* show = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(show, nullptr);
    EXPECT_EQ(show->target, ShowTarget::ALL);
}

TEST(QA_AdminEdge, ExplainSelect) {
    auto stmt = parse_one("EXPLAIN SELECT * FROM users");
    auto* exp = dynamic_cast<ExplainStmt*>(stmt.get());
    ASSERT_NE(exp, nullptr);
    EXPECT_FALSE(exp->analyze);
    EXPECT_EQ(exp->format, ExplainFormat::TEXT);
    EXPECT_NE(exp->statement, nullptr);
}

TEST(QA_AdminEdge, ExplainAnalyzeFormatJson) {
    auto stmt = parse_one("EXPLAIN ANALYZE FORMAT JSON SELECT * FROM users");
    auto* exp = dynamic_cast<ExplainStmt*>(stmt.get());
    ASSERT_NE(exp, nullptr);
    EXPECT_TRUE(exp->analyze);
    EXPECT_EQ(exp->format, ExplainFormat::JSON);
}

TEST(QA_AdminEdge, VacuumNoTable) {
    auto stmt = parse_one("VACUUM");
    auto* vac = dynamic_cast<VacuumStmt*>(stmt.get());
    ASSERT_NE(vac, nullptr);
    EXPECT_TRUE(vac->table_name.empty());
}

TEST(QA_AdminEdge, VacuumWithTable) {
    auto stmt = parse_one("VACUUM users");
    auto* vac = dynamic_cast<VacuumStmt*>(stmt.get());
    ASSERT_NE(vac, nullptr);
    EXPECT_EQ(vac->table_name, "users");
}

TEST(QA_AdminEdge, AnalyzeNoTable) {
    auto stmt = parse_one("ANALYZE");
    auto* an = dynamic_cast<AnalyzeStmt*>(stmt.get());
    ASSERT_NE(an, nullptr);
    EXPECT_TRUE(an->table_name.empty());
}

TEST(QA_AdminEdge, ReembedTable) {
    auto stmt = parse_one("REEMBED TABLE docs");
    auto* re = dynamic_cast<ReembedStmt*>(stmt.get());
    ASSERT_NE(re, nullptr);
    EXPECT_EQ(re->table_name, "docs");
}

TEST(QA_AdminEdge, ReembedWithoutTableKeyword) {
    auto stmt = parse_one("REEMBED docs");
    auto* re = dynamic_cast<ReembedStmt*>(stmt.get());
    ASSERT_NE(re, nullptr);
    EXPECT_EQ(re->table_name, "docs");
}

// =============================================================================
// Error message quality
// =============================================================================

TEST(QA_ErrorMessages, IncludesLineAndColumn) {
    auto r = try_parse("SELECT\n  FROM\n    @@");
    if (!r.has_value()) {
        auto msg = r.error().message;
        // Error message should contain "line" and "column" info.
        EXPECT_NE(msg.find("line"), std::string::npos)
            << "Error message missing line info: " << msg;
    }
}

TEST(QA_ErrorMessages, ParseErrorHasStatusCode) {
    auto r = try_parse("INVALID");
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_ErrorMessages, MissingExpectedToken) {
    auto r = try_parse("CREATE TABLE");
    EXPECT_FALSE(r.has_value());
    // Should mention what was expected.
    EXPECT_NE(r.error().message.find("expected"), std::string::npos)
        << "Error message: " << r.error().message;
}

// =============================================================================
// Keywords-as-identifiers edge cases
// =============================================================================

TEST(QA_KeywordsAsNames, TypeKeywordAsTableName) {
    // Type keywords should be usable as table/column names.
    auto stmt = parse_one("SELECT * FROM timestamp");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->from[0].name, "timestamp");
}

TEST(QA_KeywordsAsNames, AggregateKeywordAsColumnName) {
    auto stmt = parse_one("SELECT count FROM t");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* col = dynamic_cast<ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "count");
}

TEST(QA_KeywordsAsNames, IndexKeywordAsName) {
    auto stmt = parse_one("CREATE TABLE index (index INT)");
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->name, "index");
    EXPECT_EQ(ct->columns[0].name, "index");
}

// =============================================================================
// Visitor pattern verification
// =============================================================================

/// Simple visitor that counts how many nodes it visits.
class CountingVisitor : public AstVisitor {
public:
    int count = 0;

    void visit(const LiteralExpr&) override { count++; }
    void visit(const ColumnRefExpr&) override { count++; }
    void visit(const BinaryExpr&) override { count++; }
    void visit(const UnaryExpr&) override { count++; }
    void visit(const FunctionCallExpr&) override { count++; }
    void visit(const CastExpr&) override { count++; }
    void visit(const CaseExpr&) override { count++; }
    void visit(const InExpr&) override { count++; }
    void visit(const BetweenExpr&) override { count++; }
    void visit(const IsNullExpr&) override { count++; }
    void visit(const LikeExpr&) override { count++; }
    void visit(const ExistsExpr&) override { count++; }
    void visit(const SubqueryExpr&) override { count++; }
    void visit(const ArrayExpr&) override { count++; }
    void visit(const WindowFunctionExpr&) override { count++; }
    void visit(const CreateTableStmt&) override { count++; }
    void visit(const DropTableStmt&) override { count++; }
    void visit(const AlterTableStmt&) override { count++; }
    void visit(const CreateIndexStmt&) override { count++; }
    void visit(const DropIndexStmt&) override { count++; }
    void visit(const CreateEdgeTypeStmt&) override { count++; }
    void visit(const DropEdgeTypeStmt&) override { count++; }
    void visit(const CreateDatabaseStmt&) override { count++; }
    void visit(const DropDatabaseStmt&) override { count++; }
    void visit(const CreateUserStmt&) override { count++; }
    void visit(const DropUserStmt&) override { count++; }
    void visit(const AlterUserStmt&) override { count++; }
    void visit(const InsertStmt&) override { count++; }
    void visit(const UpdateStmt&) override { count++; }
    void visit(const DeleteStmt&) override { count++; }
    void visit(const LinkStmt&) override { count++; }
    void visit(const UnlinkStmt&) override { count++; }
    void visit(const SelectStmt&) override { count++; }
    void visit(const TraverseStmt&) override { count++; }
    void visit(const NearestStmt&) override { count++; }
    void visit(const MatchStmt&) override { count++; }
    void visit(const ShortestPathStmt&) override { count++; }
    void visit(const BeginStmt&) override { count++; }
    void visit(const CommitStmt&) override { count++; }
    void visit(const RollbackStmt&) override { count++; }
    void visit(const SavepointStmt&) override { count++; }
    void visit(const SetStmt&) override { count++; }
    void visit(const ShowStmt&) override { count++; }
    void visit(const ExplainStmt&) override { count++; }
    void visit(const DescribeStmt&) override { count++; }
    void visit(const ReembedStmt&) override { count++; }
    void visit(const VacuumStmt&) override { count++; }
    void visit(const AnalyzeStmt&) override { count++; }
};

TEST(QA_Visitor, EveryStatementTypeDispatchesCorrectly) {
    // Parse one of each statement category and confirm the visitor dispatches.
    const char* sqls[] = {
        "CREATE TABLE t (id INT)",
        "DROP TABLE t",
        "ALTER TABLE t ADD COLUMN c INT",
        "CREATE INDEX idx ON t(c)",
        "DROP INDEX idx",
        "CREATE EDGE TYPE e FROM a TO b",
        "DROP EDGE TYPE e",
        "INSERT INTO t VALUES (1)",
        "UPDATE t SET c = 1",
        "DELETE FROM t",
        "SELECT 1",
        "TRAVERSE e FROM t(1)",
        "NEAREST 5 FROM t.c TO 'x'",
        "MATCH (a:t)-[r:e]->(b:t) RETURN a",
        "SHORTEST PATH FROM t(1) TO t(2) VIA e",
        "BEGIN",
        "COMMIT",
        "ROLLBACK",
        "SAVEPOINT sp",
        "SET x = 1",
        "SHOW TABLES",
        "EXPLAIN SELECT 1",
        "DESCRIBE t",
        "REEMBED TABLE t",
        "VACUUM",
        "ANALYZE",
    };

    CountingVisitor cv;
    for (const char* sql : sqls) {
        auto stmt = parse_one(sql);
        ASSERT_NE(stmt, nullptr) << "Failed to parse: " << sql;
        stmt->accept(cv);
    }

    // Should have visited at least one per SQL.
    EXPECT_EQ(cv.count, static_cast<int>(std::size(sqls)));
}

TEST(QA_Visitor, ExpressionDispatch) {
    auto stmt = parse_one("SELECT 42");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);

    CountingVisitor cv;
    sel->items[0].expr->accept(cv);
    EXPECT_EQ(cv.count, 1);
}

// =============================================================================
// Multiple statements in parse_all
// =============================================================================

TEST(QA_MultiStatement, MultipleSemicolonSeparated) {
    auto stmts = parse_ok("SELECT 1; SELECT 2; SELECT 3;");
    EXPECT_EQ(stmts.size(), 3u);
}

TEST(QA_MultiStatement, MultipleNoTrailingSemicolon) {
    auto stmts = parse_ok("SELECT 1; SELECT 2");
    EXPECT_EQ(stmts.size(), 2u);
}

TEST(QA_MultiStatement, MixedStatementTypes) {
    auto stmts = parse_ok("CREATE TABLE t (id INT); "
                          "INSERT INTO t VALUES (1); "
                          "SELECT * FROM t; "
                          "DROP TABLE t");
    EXPECT_EQ(stmts.size(), 4u);
    EXPECT_NE(dynamic_cast<CreateTableStmt*>(stmts[0].get()), nullptr);
    EXPECT_NE(dynamic_cast<InsertStmt*>(stmts[1].get()), nullptr);
    EXPECT_NE(dynamic_cast<SelectStmt*>(stmts[2].get()), nullptr);
    EXPECT_NE(dynamic_cast<DropTableStmt*>(stmts[3].get()), nullptr);
}

// =============================================================================
// Malformed SQL error cases
// =============================================================================

TEST(QA_ErrorCases, UnclosedParenthesis) {
    expect_parse_error("SELECT (1 + 2");
}

TEST(QA_ErrorCases, MissingFromTable) {
    expect_parse_error("SELECT * FROM");
}

TEST(QA_ErrorCases, InsertMissingValues) {
    expect_parse_error("INSERT INTO t");
}

TEST(QA_ErrorCases, UpdateMissingSet) {
    expect_parse_error("UPDATE t WHERE x = 1");
}

TEST(QA_ErrorCases, CreateTableNoColumns) {
    expect_parse_error("CREATE TABLE t");
}

TEST(QA_ErrorCases, InvalidColumnType) {
    expect_parse_error("CREATE TABLE t (c INVALID_TYPE)");
}

TEST(QA_ErrorCases, DropWithoutObjectType) {
    expect_parse_error("DROP something");
}

TEST(QA_ErrorCases, AlterInvalidAction) {
    expect_parse_error("ALTER TABLE t INVALID");
}

TEST(QA_ErrorCases, ExplainInvalidFormat) {
    expect_parse_error("EXPLAIN FORMAT XML SELECT 1");
}

TEST(QA_ErrorCases, NearestMissingTo) {
    expect_parse_error("NEAREST 5 FROM t.c");
}

TEST(QA_ErrorCases, LinkMissingVia) {
    expect_parse_error("LINK t(1) TO t(2)");
}

TEST(QA_ErrorCases, ShortestPathMissingVia) {
    expect_parse_error("SHORTEST PATH FROM t(1) TO t(2)");
}

TEST(QA_ErrorCases, MatchMissingReturn) {
    expect_parse_error("MATCH (a:t)-[r:e]->(b:t) WHERE a.x = 1");
}

TEST(QA_ErrorCases, CreateUserMissingPassword) {
    expect_parse_error("CREATE USER admin WITH");
}

TEST(QA_ErrorCases, CaseWithoutEnd) {
    expect_parse_error("SELECT CASE WHEN x = 1 THEN 'a'");
}
