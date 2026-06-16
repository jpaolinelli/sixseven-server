// test_qa_gdb_831.cpp
// GDB-831 QA: adversarial parser spot-checks confirming test_ast.cpp asserted shapes
// match REAL parser behavior. Each test here represents a distinct attack vector:
// swapped operands, wrong CASE form, wrong EMBEDDING param slot, wrong precedence, etc.

#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"

#include <gtest/gtest.h>

using namespace sixseven;

static StmtPtr parse_one_gdb831(const std::string& sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens) {
        ADD_FAILURE() << "Lex error: " << tokens.error().message;
        return nullptr;
    }
    Parser parser(std::move(*tokens));
    auto stmt = parser.parse();
    if (!stmt) {
        ADD_FAILURE() << "Parse error: " << stmt.error().message;
        return nullptr;
    }
    return std::move(*stmt);
}

// ---------------------------------------------------------------------------
// EMBEDDING positional args: dim -> param1, source -> TypeSpec::source,
// provider -> TypeSpec::provider. Swapping any two would cause a failure here.
// ---------------------------------------------------------------------------

TEST(QA_GDB831, EmbeddingDimInParam1NotParam2) {
    auto s = parse_one_gdb831("CREATE TABLE t (v EMBEDDING(384, description, 'openai'))");
    ASSERT_NE(s, nullptr);
    auto* ct = dynamic_cast<CreateTableStmt*>(s.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->columns.size(), 1u);
    const auto& ts = ct->columns[0].type;
    EXPECT_EQ(ts.name, "EMBEDDING");
    ASSERT_TRUE(ts.param1.has_value());
    EXPECT_EQ(ts.param1.value(), 384);
    // param2 must NOT be populated for EMBEDDING (it has no second numeric param)
    EXPECT_FALSE(ts.param2.has_value());
    EXPECT_EQ(ts.source, "description");
    EXPECT_EQ(ts.provider, "openai");
}

TEST(QA_GDB831, EmbeddingSourceIsColumnNotProvider) {
    // Confirm source and provider are not swapped
    auto s = parse_one_gdb831("CREATE TABLE t (v EMBEDDING(128, my_col, 'bert'))");
    ASSERT_NE(s, nullptr);
    auto* ct = dynamic_cast<CreateTableStmt*>(s.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->columns.size(), 1u);
    EXPECT_EQ(ct->columns[0].type.source, "my_col");
    EXPECT_EQ(ct->columns[0].type.provider, "bert");
    // provider should not equal source
    EXPECT_NE(ct->columns[0].type.source, ct->columns[0].type.provider);
}

// ---------------------------------------------------------------------------
// CREATE EDGE TYPE: grammar requires properties list BEFORE FROM...TO.
// A parser that puts FROM/TO before properties would set from_table="users"
// but leave properties empty — this test catches that.
// ---------------------------------------------------------------------------

TEST(QA_GDB831, EdgeTypePropertiesBeforeFromTo) {
    auto s = parse_one_gdb831(
        "CREATE EDGE TYPE follows (since TIMESTAMP) FROM users TO posts");
    ASSERT_NE(s, nullptr);
    auto* ce = dynamic_cast<CreateEdgeTypeStmt*>(s.get());
    ASSERT_NE(ce, nullptr);
    EXPECT_EQ(ce->name, "follows");
    // Properties must be present (parser didn't skip them)
    ASSERT_EQ(ce->properties.size(), 1u);
    EXPECT_EQ(ce->properties[0].name, "since");
    EXPECT_EQ(ce->properties[0].type.name, "TIMESTAMP");
    // FROM and TO must be correct and not swapped
    EXPECT_EQ(ce->from_table, "users");
    EXPECT_EQ(ce->to_table, "posts");
    EXPECT_NE(ce->from_table, ce->to_table);
}

// ---------------------------------------------------------------------------
// CASE: searched form has nullptr operand; simple form has non-null operand.
// A parser that sets operand on searched CASE, or clears it on simple CASE,
// would fail these tests.
// ---------------------------------------------------------------------------

TEST(QA_GDB831, CaseSearchedOperandIsNull) {
    auto s = parse_one_gdb831("SELECT CASE WHEN TRUE THEN 'yes' ELSE 'no' END");
    ASSERT_NE(s, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(s.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* ce = dynamic_cast<CaseExpr*>(sel->items[0].expr.get());
    ASSERT_NE(ce, nullptr);
    EXPECT_EQ(ce->operand, nullptr) << "Searched CASE must have null operand";
    ASSERT_EQ(ce->whens.size(), 1u);
    EXPECT_NE(ce->else_expr, nullptr);
    auto* result = dynamic_cast<LiteralExpr*>(ce->whens[0].result.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->value, "yes");
}

TEST(QA_GDB831, CaseSimpleOperandIsNotNull) {
    auto s = parse_one_gdb831("SELECT CASE status WHEN 1 THEN 'active' END");
    ASSERT_NE(s, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(s.get());
    ASSERT_NE(sel, nullptr);
    auto* ce = dynamic_cast<CaseExpr*>(sel->items[0].expr.get());
    ASSERT_NE(ce, nullptr);
    EXPECT_NE(ce->operand, nullptr) << "Simple CASE must have non-null operand";
    auto* col = dynamic_cast<ColumnRefExpr*>(ce->operand.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "status");
    // No ELSE clause
    EXPECT_EQ(ce->else_expr, nullptr);
}

// ---------------------------------------------------------------------------
// Operator precedence: * must bind tighter than +
// 1 + 2 * 3 => ADD(1, MUL(2,3)), not MUL(ADD(1,2), 3)
// If parser ignores precedence, this would produce MUL at root and fail.
// ---------------------------------------------------------------------------

TEST(QA_GDB831, MulBindsTighterThanAdd) {
    auto s = parse_one_gdb831("SELECT 1 + 2 * 3");
    ASSERT_NE(s, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(s.get());
    ASSERT_NE(sel, nullptr);
    auto* add = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->op, BinaryOp::ADD) << "Top-level op must be ADD, not MUL";
    auto* mul = dynamic_cast<BinaryExpr*>(add->rhs.get());
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op, BinaryOp::MULTIPLY);
    auto* two = dynamic_cast<LiteralExpr*>(mul->lhs.get());
    auto* three = dynamic_cast<LiteralExpr*>(mul->rhs.get());
    ASSERT_NE(two, nullptr); ASSERT_NE(three, nullptr);
    EXPECT_EQ(two->value, "2");
    EXPECT_EQ(three->value, "3");
}

TEST(QA_GDB831, ParenOverridesPrecedence) {
    // (1+2)*3 => MUL at top
    auto s = parse_one_gdb831("SELECT (1 + 2) * 3");
    ASSERT_NE(s, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(s.get());
    ASSERT_NE(sel, nullptr);
    auto* mul = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op, BinaryOp::MULTIPLY);
    auto* add = dynamic_cast<BinaryExpr*>(mul->lhs.get());
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->op, BinaryOp::ADD);
}

// ---------------------------------------------------------------------------
// Left-associativity: 1 - 2 - 3 = (1-2)-3, not 1-(2-3)
// A right-associative parser would put the inner SUBTRACT on the rhs.
// ---------------------------------------------------------------------------

TEST(QA_GDB831, SubtractIsLeftAssociative) {
    auto s = parse_one_gdb831("SELECT 1 - 2 - 3");
    ASSERT_NE(s, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(s.get());
    ASSERT_NE(sel, nullptr);
    auto* outer = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->op, BinaryOp::SUBTRACT);
    // lhs must be another SUBTRACT (left-associative grouping)
    auto* inner = dynamic_cast<BinaryExpr*>(outer->lhs.get());
    ASSERT_NE(inner, nullptr) << "1-2-3 must be left-associative: (1-2)-3";
    EXPECT_EQ(inner->op, BinaryOp::SUBTRACT);
    // rhs of outer must be literal 3
    auto* three = dynamic_cast<LiteralExpr*>(outer->rhs.get());
    ASSERT_NE(three, nullptr);
    EXPECT_EQ(three->value, "3");
}

// ---------------------------------------------------------------------------
// NOT IN: negated flag must be true; values list must be populated.
// A parser that sets negated=false or puts values in subquery would fail.
// ---------------------------------------------------------------------------

TEST(QA_GDB831, NotInNegatedTrueValuesPopulated) {
    auto s = parse_one_gdb831("SELECT * FROM t WHERE id NOT IN (1, 2)");
    ASSERT_NE(s, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(s.get());
    ASSERT_NE(sel, nullptr);
    auto* in_expr = dynamic_cast<InExpr*>(sel->where_expr.get());
    ASSERT_NE(in_expr, nullptr);
    EXPECT_TRUE(in_expr->negated) << "NOT IN must set negated=true";
    ASSERT_EQ(in_expr->values.size(), 2u);
    EXPECT_EQ(in_expr->subquery, nullptr) << "NOT IN list must use values, not subquery";
}

TEST(QA_GDB831, InNotNegated) {
    // Plain IN must have negated=false
    auto s = parse_one_gdb831("SELECT * FROM t WHERE id IN (1, 2, 3)");
    ASSERT_NE(s, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(s.get());
    ASSERT_NE(sel, nullptr);
    auto* in_expr = dynamic_cast<InExpr*>(sel->where_expr.get());
    ASSERT_NE(in_expr, nullptr);
    EXPECT_FALSE(in_expr->negated) << "IN (without NOT) must have negated=false";
    ASSERT_EQ(in_expr->values.size(), 3u);
}

// ---------------------------------------------------------------------------
// NULL literal: value must be empty string (not "NULL" / "null").
// A parser that stores the token text would set value="NULL" and fail this.
// ---------------------------------------------------------------------------

TEST(QA_GDB831, NullLiteralValueIsEmptyString) {
    auto s = parse_one_gdb831("SELECT NULL");
    ASSERT_NE(s, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(s.get());
    ASSERT_NE(sel, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(sel->items[0].expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::NULL_LITERAL);
    EXPECT_TRUE(lit->value.empty())
        << "NULL literal value must be empty string, got: '" << lit->value << "'";
}

// ---------------------------------------------------------------------------
// BETWEEN: low and high must NOT be swapped.
// A parser that reversed the bounds would fail this test.
// ---------------------------------------------------------------------------

TEST(QA_GDB831, BetweenBoundsNotSwapped) {
    auto s = parse_one_gdb831("SELECT * FROM t WHERE age BETWEEN 18 AND 65");
    ASSERT_NE(s, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(s.get());
    ASSERT_NE(sel, nullptr);
    auto* b = dynamic_cast<BetweenExpr*>(sel->where_expr.get());
    ASSERT_NE(b, nullptr);
    auto* lo = dynamic_cast<LiteralExpr*>(b->low.get());
    auto* hi = dynamic_cast<LiteralExpr*>(b->high.get());
    ASSERT_NE(lo, nullptr); ASSERT_NE(hi, nullptr);
    EXPECT_EQ(lo->value, "18") << "low bound must be 18";
    EXPECT_EQ(hi->value, "65") << "high bound must be 65 (not swapped)";
}

// ---------------------------------------------------------------------------
// BinaryExpr ADD: lhs=1 rhs=2. A parser that swapped operands would fail.
// ---------------------------------------------------------------------------

TEST(QA_GDB831, AddLhsRhsNotSwapped) {
    auto s = parse_one_gdb831("SELECT 1 + 2");
    ASSERT_NE(s, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(s.get());
    ASSERT_NE(sel, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::ADD);
    auto* lhs = dynamic_cast<LiteralExpr*>(bin->lhs.get());
    auto* rhs = dynamic_cast<LiteralExpr*>(bin->rhs.get());
    ASSERT_NE(lhs, nullptr); ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(lhs->value, "1") << "lhs must be 1, not 2";
    EXPECT_EQ(rhs->value, "2") << "rhs must be 2, not 1";
}

// ---------------------------------------------------------------------------
// Multi-arg function: COALESCE(a,b,0) — third arg must be literal "0".
// A parser that dropped args or mis-ordered them would fail.
// ---------------------------------------------------------------------------

TEST(QA_GDB831, FunctionThirdArgIsLiteralZero) {
    auto s = parse_one_gdb831("SELECT COALESCE(a, b, 0) FROM t");
    ASSERT_NE(s, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(s.get());
    ASSERT_NE(sel, nullptr);
    auto* fn = dynamic_cast<FunctionCallExpr*>(sel->items[0].expr.get());
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->name, "COALESCE");
    ASSERT_EQ(fn->args.size(), 3u);
    auto* zero = dynamic_cast<LiteralExpr*>(fn->args[2].get());
    ASSERT_NE(zero, nullptr);
    EXPECT_EQ(zero->value, "0");
    // first two args are column refs
    auto* a = dynamic_cast<ColumnRefExpr*>(fn->args[0].get());
    auto* b = dynamic_cast<ColumnRefExpr*>(fn->args[1].get());
    ASSERT_NE(a, nullptr); ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->column, "a");
    EXPECT_EQ(b->column, "b");
}

// ---------------------------------------------------------------------------
// COUNT(DISTINCT id): distinct=true, 1 arg which is column "id"
// ---------------------------------------------------------------------------

TEST(QA_GDB831, CountDistinctFlagAndArg) {
    auto s = parse_one_gdb831("SELECT COUNT(DISTINCT id) FROM users");
    ASSERT_NE(s, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(s.get());
    ASSERT_NE(sel, nullptr);
    auto* fn = dynamic_cast<FunctionCallExpr*>(sel->items[0].expr.get());
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->name, "COUNT");
    EXPECT_TRUE(fn->distinct) << "COUNT(DISTINCT ...) must set distinct=true";
    ASSERT_EQ(fn->args.size(), 1u);
    auto* col = dynamic_cast<ColumnRefExpr*>(fn->args[0].get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "id");
}

// ---------------------------------------------------------------------------
// No vacuous tests: confirm parse_one_gdb831 actually calls the parser
// (if parse_one returned a hardcoded stub, this empty-SQL test would NOT fail)
// ---------------------------------------------------------------------------

TEST(QA_GDB831, EmptySQLFailsParse) {
    Lexer lexer("");
    auto tokens = lexer.tokenize();
    // Empty input: either fails to lex or fails to parse
    if (tokens) {
        Parser parser(std::move(*tokens));
        auto stmt = parser.parse();
        // Empty should fail to produce a statement
        EXPECT_FALSE(stmt.has_value()) << "Empty SQL should not produce a valid statement";
    }
    // else: lex error is also acceptable
}

TEST(QA_GDB831, GibberishSQLFailsParse) {
    Lexer lexer("THIS IS NOT SQL @@@ ###");
    auto tokens = lexer.tokenize();
    if (tokens) {
        Parser parser(std::move(*tokens));
        auto stmt = parser.parse();
        EXPECT_FALSE(stmt.has_value()) << "Gibberish should not parse successfully";
    }
    // lex error is also fine
}
