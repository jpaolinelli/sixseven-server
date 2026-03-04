/// @file test_qa_gdb_104.cpp
/// QA adversarial tests for GDB-104: Parser for DML and expressions.
///
/// Tests cover: INSERT/UPDATE/DELETE/LINK/UNLINK edge cases, expression
/// operator precedence, special expressions (IS NULL, IN, BETWEEN, LIKE,
/// CASE, CAST), function calls, and error paths.

#include "giodb/parser/lexer.h"
#include "giodb/parser/parser.h"

#include <gtest/gtest.h>

using namespace giodb;

// -- Helpers ------------------------------------------------------------------

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

static void expect_parse_error(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return;
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_FALSE(stmts.has_value()) << "expected parse error for: " << sql;
}

// =============================================================================
// INSERT tests
// =============================================================================

TEST(QA_GDB104, InsertBasic) {
    auto stmt = parse_one("INSERT INTO users (name, age) VALUES ('alice', 30)");
    auto* ins = dynamic_cast<InsertStmt*>(stmt.get());
    ASSERT_NE(ins, nullptr);
    EXPECT_EQ(ins->table_name, "users");
    EXPECT_EQ(ins->columns.size(), 2u);
    EXPECT_EQ(ins->values.size(), 1u);
    EXPECT_EQ(ins->values[0].size(), 2u);
}

TEST(QA_GDB104, InsertMultipleRows) {
    auto stmt = parse_one("INSERT INTO t (a, b) VALUES (1, 2), (3, 4), (5, 6)");
    auto* ins = dynamic_cast<InsertStmt*>(stmt.get());
    ASSERT_NE(ins, nullptr);
    EXPECT_EQ(ins->values.size(), 3u);
    for (auto& row : ins->values) {
        EXPECT_EQ(row.size(), 2u);
    }
}

TEST(QA_GDB104, InsertWithoutColumnList) {
    auto stmt = parse_one("INSERT INTO t VALUES (1, 'hello', TRUE)");
    auto* ins = dynamic_cast<InsertStmt*>(stmt.get());
    ASSERT_NE(ins, nullptr);
    EXPECT_TRUE(ins->columns.empty());
    EXPECT_EQ(ins->values.size(), 1u);
    EXPECT_EQ(ins->values[0].size(), 3u);
}

TEST(QA_GDB104, InsertWithSelect) {
    auto stmt = parse_one("INSERT INTO t (a, b) SELECT x, y FROM s");
    auto* ins = dynamic_cast<InsertStmt*>(stmt.get());
    ASSERT_NE(ins, nullptr);
    EXPECT_NE(ins->select, nullptr);
    EXPECT_TRUE(ins->values.empty());
}

TEST(QA_GDB104, InsertWithReturning) {
    auto stmt = parse_one("INSERT INTO t (a) VALUES (1) RETURNING a, b");
    auto* ins = dynamic_cast<InsertStmt*>(stmt.get());
    ASSERT_NE(ins, nullptr);
    EXPECT_EQ(ins->returning.size(), 2u);
}

TEST(QA_GDB104, InsertWithReturningStar) {
    auto stmt = parse_one("INSERT INTO t (a) VALUES (1) RETURNING *");
    auto* ins = dynamic_cast<InsertStmt*>(stmt.get());
    ASSERT_NE(ins, nullptr);
    EXPECT_EQ(ins->returning.size(), 1u);
    EXPECT_TRUE(ins->returning[0].is_star);
}

TEST(QA_GDB104, InsertMissingInto) {
    expect_parse_error("INSERT t (a) VALUES (1)");
}

TEST(QA_GDB104, InsertMissingValues) {
    expect_parse_error("INSERT INTO t (a)");
}

TEST(QA_GDB104, InsertEmptyValues) {
    // INSERT INTO t VALUES () — empty row (should error: no expressions)
    expect_parse_error("INSERT INTO t VALUES ()");
}

TEST(QA_GDB104, InsertNullValue) {
    auto stmt = parse_one("INSERT INTO t (a) VALUES (NULL)");
    auto* ins = dynamic_cast<InsertStmt*>(stmt.get());
    ASSERT_NE(ins, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(ins->values[0][0].get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::NULL_LITERAL);
}

TEST(QA_GDB104, InsertExpressionValues) {
    // Values can contain expressions, not just literals.
    auto stmt = parse_one("INSERT INTO t (a) VALUES (1 + 2 * 3)");
    auto* ins = dynamic_cast<InsertStmt*>(stmt.get());
    ASSERT_NE(ins, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(ins->values[0][0].get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::ADD);
}

// =============================================================================
// UPDATE tests
// =============================================================================

TEST(QA_GDB104, UpdateBasic) {
    auto stmt = parse_one("UPDATE users SET name = 'bob' WHERE id = 1");
    auto* upd = dynamic_cast<UpdateStmt*>(stmt.get());
    ASSERT_NE(upd, nullptr);
    EXPECT_EQ(upd->table_name, "users");
    EXPECT_EQ(upd->assignments.size(), 1u);
    EXPECT_EQ(upd->assignments[0].column, "name");
    EXPECT_NE(upd->where_expr, nullptr);
}

TEST(QA_GDB104, UpdateMultipleAssignments) {
    auto stmt = parse_one("UPDATE t SET a = 1, b = 'hello', c = TRUE WHERE id > 0");
    auto* upd = dynamic_cast<UpdateStmt*>(stmt.get());
    ASSERT_NE(upd, nullptr);
    EXPECT_EQ(upd->assignments.size(), 3u);
}

TEST(QA_GDB104, UpdateWithoutWhere) {
    auto stmt = parse_one("UPDATE t SET a = 1");
    auto* upd = dynamic_cast<UpdateStmt*>(stmt.get());
    ASSERT_NE(upd, nullptr);
    EXPECT_EQ(upd->where_expr, nullptr);
}

TEST(QA_GDB104, UpdateWithReturning) {
    auto stmt = parse_one("UPDATE t SET a = 1 RETURNING a, b");
    auto* upd = dynamic_cast<UpdateStmt*>(stmt.get());
    ASSERT_NE(upd, nullptr);
    EXPECT_EQ(upd->returning.size(), 2u);
}

TEST(QA_GDB104, UpdateMissingSet) {
    expect_parse_error("UPDATE t a = 1");
}

TEST(QA_GDB104, UpdateMissingEquals) {
    expect_parse_error("UPDATE t SET a 1");
}

TEST(QA_GDB104, UpdateWithComplexExpression) {
    auto stmt = parse_one("UPDATE t SET a = a + 1 WHERE b BETWEEN 10 AND 20");
    auto* upd = dynamic_cast<UpdateStmt*>(stmt.get());
    ASSERT_NE(upd, nullptr);
    auto* add = dynamic_cast<BinaryExpr*>(upd->assignments[0].value.get());
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->op, BinaryOp::ADD);
    auto* between = dynamic_cast<BetweenExpr*>(upd->where_expr.get());
    ASSERT_NE(between, nullptr);
}

// =============================================================================
// DELETE tests
// =============================================================================

TEST(QA_GDB104, DeleteBasic) {
    auto stmt = parse_one("DELETE FROM users WHERE id = 1");
    auto* del = dynamic_cast<DeleteStmt*>(stmt.get());
    ASSERT_NE(del, nullptr);
    EXPECT_EQ(del->table_name, "users");
    EXPECT_NE(del->where_expr, nullptr);
}

TEST(QA_GDB104, DeleteWithoutWhere) {
    auto stmt = parse_one("DELETE FROM users");
    auto* del = dynamic_cast<DeleteStmt*>(stmt.get());
    ASSERT_NE(del, nullptr);
    EXPECT_EQ(del->where_expr, nullptr);
}

TEST(QA_GDB104, DeleteWithReturning) {
    auto stmt = parse_one("DELETE FROM t WHERE id = 1 RETURNING *");
    auto* del = dynamic_cast<DeleteStmt*>(stmt.get());
    ASSERT_NE(del, nullptr);
    EXPECT_EQ(del->returning.size(), 1u);
    EXPECT_TRUE(del->returning[0].is_star);
}

TEST(QA_GDB104, DeleteMissingFrom) {
    expect_parse_error("DELETE users WHERE id = 1");
}

// =============================================================================
// LINK tests
// =============================================================================

TEST(QA_GDB104, LinkBasic) {
    auto stmt = parse_one("LINK users(1) TO posts(10) VIA authored");
    auto* lnk = dynamic_cast<LinkStmt*>(stmt.get());
    ASSERT_NE(lnk, nullptr);
    EXPECT_EQ(lnk->source_table, "users");
    EXPECT_EQ(lnk->target_table, "posts");
    EXPECT_EQ(lnk->edge_type, "authored");
    EXPECT_TRUE(lnk->properties.empty());
}

TEST(QA_GDB104, LinkWithProperties) {
    auto stmt = parse_one("LINK users(1) TO posts(10) VIA rated (score = 4.5, review = 'good')");
    auto* lnk = dynamic_cast<LinkStmt*>(stmt.get());
    ASSERT_NE(lnk, nullptr);
    EXPECT_EQ(lnk->properties.size(), 2u);
    EXPECT_EQ(lnk->properties[0].column, "score");
    EXPECT_EQ(lnk->properties[1].column, "review");
}

TEST(QA_GDB104, LinkWithExpressionKeys) {
    auto stmt = parse_one("LINK users(1 + 1) TO posts(2 * 5) VIA follows");
    auto* lnk = dynamic_cast<LinkStmt*>(stmt.get());
    ASSERT_NE(lnk, nullptr);
    auto* src = dynamic_cast<BinaryExpr*>(lnk->source_key.get());
    ASSERT_NE(src, nullptr);
    EXPECT_EQ(src->op, BinaryOp::ADD);
}

TEST(QA_GDB104, LinkMissingVia) {
    expect_parse_error("LINK users(1) TO posts(10)");
}

TEST(QA_GDB104, LinkMissingTo) {
    expect_parse_error("LINK users(1) posts(10) VIA follows");
}

// =============================================================================
// UNLINK tests
// =============================================================================

TEST(QA_GDB104, UnlinkBasic) {
    auto stmt = parse_one("UNLINK users(1) FROM posts(10) VIA authored");
    auto* ulnk = dynamic_cast<UnlinkStmt*>(stmt.get());
    ASSERT_NE(ulnk, nullptr);
    EXPECT_EQ(ulnk->source_table, "users");
    EXPECT_EQ(ulnk->target_table, "posts");
    EXPECT_EQ(ulnk->edge_type, "authored");
    EXPECT_EQ(ulnk->where_expr, nullptr);
}

TEST(QA_GDB104, UnlinkWithWhere) {
    auto stmt = parse_one("UNLINK users(1) FROM posts(10) VIA rated WHERE score < 3");
    auto* ulnk = dynamic_cast<UnlinkStmt*>(stmt.get());
    ASSERT_NE(ulnk, nullptr);
    EXPECT_NE(ulnk->where_expr, nullptr);
}

TEST(QA_GDB104, UnlinkMissingFrom) {
    expect_parse_error("UNLINK users(1) posts(10) VIA follows");
}

TEST(QA_GDB104, UnlinkMissingVia) {
    expect_parse_error("UNLINK users(1) FROM posts(10)");
}

// =============================================================================
// Expression precedence tests
// =============================================================================

TEST(QA_GDB104, PrecedenceMultiplyOverAdd) {
    // 1 + 2 * 3 => 1 + (2 * 3)
    auto stmt = parse_one("SELECT 1 + 2 * 3");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* add = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->op, BinaryOp::ADD);
    // LHS should be literal 1
    auto* lit1 = dynamic_cast<LiteralExpr*>(add->lhs.get());
    ASSERT_NE(lit1, nullptr);
    EXPECT_EQ(lit1->value, "1");
    // RHS should be 2 * 3
    auto* mul = dynamic_cast<BinaryExpr*>(add->rhs.get());
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op, BinaryOp::MULTIPLY);
}

TEST(QA_GDB104, PrecedenceParensOverride) {
    // (1 + 2) * 3 => (1 + 2) * 3
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

TEST(QA_GDB104, PrecedenceLeftAssociativeSubtraction) {
    // 10 - 3 - 2 => (10 - 3) - 2
    auto stmt = parse_one("SELECT 10 - 3 - 2");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* sub = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(sub, nullptr);
    EXPECT_EQ(sub->op, BinaryOp::SUBTRACT);
    // LHS should be (10 - 3)
    auto* inner = dynamic_cast<BinaryExpr*>(sub->lhs.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->op, BinaryOp::SUBTRACT);
    // RHS should be 2
    auto* lit2 = dynamic_cast<LiteralExpr*>(sub->rhs.get());
    ASSERT_NE(lit2, nullptr);
    EXPECT_EQ(lit2->value, "2");
}

TEST(QA_GDB104, PrecedenceAndOverOr) {
    // a OR b AND c => a OR (b AND c)
    auto stmt = parse_one("SELECT a OR b AND c");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* or_expr = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(or_expr, nullptr);
    EXPECT_EQ(or_expr->op, BinaryOp::OR);
    auto* and_expr = dynamic_cast<BinaryExpr*>(or_expr->rhs.get());
    ASSERT_NE(and_expr, nullptr);
    EXPECT_EQ(and_expr->op, BinaryOp::AND);
}

TEST(QA_GDB104, PrecedenceNotOverAnd) {
    // NOT a AND b => (NOT a) AND b
    auto stmt = parse_one("SELECT NOT a AND b");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* and_expr = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(and_expr, nullptr);
    EXPECT_EQ(and_expr->op, BinaryOp::AND);
    auto* not_expr = dynamic_cast<UnaryExpr*>(and_expr->lhs.get());
    ASSERT_NE(not_expr, nullptr);
    EXPECT_EQ(not_expr->op, UnaryOp::NOT);
}

TEST(QA_GDB104, PrecedenceComparisonOverLogical) {
    // a > 5 AND b < 10 => (a > 5) AND (b < 10)
    auto stmt = parse_one("SELECT a > 5 AND b < 10");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* and_expr = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(and_expr, nullptr);
    EXPECT_EQ(and_expr->op, BinaryOp::AND);
    auto* gt = dynamic_cast<BinaryExpr*>(and_expr->lhs.get());
    ASSERT_NE(gt, nullptr);
    EXPECT_EQ(gt->op, BinaryOp::GREATER);
    auto* lt = dynamic_cast<BinaryExpr*>(and_expr->rhs.get());
    ASSERT_NE(lt, nullptr);
    EXPECT_EQ(lt->op, BinaryOp::LESS);
}

TEST(QA_GDB104, PrecedenceUnaryNegation) {
    // -a * b => (-a) * b
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

TEST(QA_GDB104, PrecedenceDoubleNegation) {
    // -(-a) => nested negation (note: -- is a SQL comment, so use parens).
    auto stmt = parse_one("SELECT -(-a)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* neg1 = dynamic_cast<UnaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(neg1, nullptr);
    EXPECT_EQ(neg1->op, UnaryOp::NEGATE);
    auto* neg2 = dynamic_cast<UnaryExpr*>(neg1->operand.get());
    ASSERT_NE(neg2, nullptr);
    EXPECT_EQ(neg2->op, UnaryOp::NEGATE);
}

TEST(QA_GDB104, PrecedenceModulo) {
    // a % b + c => (a % b) + c
    auto stmt = parse_one("SELECT a % b + c");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* add = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->op, BinaryOp::ADD);
    auto* mod = dynamic_cast<BinaryExpr*>(add->lhs.get());
    ASSERT_NE(mod, nullptr);
    EXPECT_EQ(mod->op, BinaryOp::MODULO);
}

TEST(QA_GDB104, PrecedenceConcat) {
    // a || b + c => (a || b) + c — both at addition level, left assoc
    auto stmt = parse_one("SELECT a || b");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* concat = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(concat, nullptr);
    EXPECT_EQ(concat->op, BinaryOp::CONCAT);
}

// =============================================================================
// Special expressions
// =============================================================================

TEST(QA_GDB104, IsNull) {
    auto stmt = parse_one("SELECT a IS NULL");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* isn = dynamic_cast<IsNullExpr*>(sel->items[0].expr.get());
    ASSERT_NE(isn, nullptr);
    EXPECT_FALSE(isn->negated);
}

TEST(QA_GDB104, IsNotNull) {
    auto stmt = parse_one("SELECT a IS NOT NULL");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* isn = dynamic_cast<IsNullExpr*>(sel->items[0].expr.get());
    ASSERT_NE(isn, nullptr);
    EXPECT_TRUE(isn->negated);
}

TEST(QA_GDB104, InList) {
    auto stmt = parse_one("SELECT a IN (1, 2, 3)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* in = dynamic_cast<InExpr*>(sel->items[0].expr.get());
    ASSERT_NE(in, nullptr);
    EXPECT_FALSE(in->negated);
    EXPECT_EQ(in->values.size(), 3u);
    EXPECT_EQ(in->subquery, nullptr);
}

TEST(QA_GDB104, NotInList) {
    auto stmt = parse_one("SELECT a NOT IN (1, 2)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* in = dynamic_cast<InExpr*>(sel->items[0].expr.get());
    ASSERT_NE(in, nullptr);
    EXPECT_TRUE(in->negated);
}

TEST(QA_GDB104, InSubquery) {
    auto stmt = parse_one("SELECT a IN (SELECT id FROM t)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* in = dynamic_cast<InExpr*>(sel->items[0].expr.get());
    ASSERT_NE(in, nullptr);
    EXPECT_NE(in->subquery, nullptr);
    EXPECT_TRUE(in->values.empty());
}

TEST(QA_GDB104, BetweenBasic) {
    auto stmt = parse_one("SELECT a BETWEEN 1 AND 10");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* bet = dynamic_cast<BetweenExpr*>(sel->items[0].expr.get());
    ASSERT_NE(bet, nullptr);
    EXPECT_FALSE(bet->negated);
}

TEST(QA_GDB104, NotBetween) {
    auto stmt = parse_one("SELECT a NOT BETWEEN 1 AND 10");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* bet = dynamic_cast<BetweenExpr*>(sel->items[0].expr.get());
    ASSERT_NE(bet, nullptr);
    EXPECT_TRUE(bet->negated);
}

TEST(QA_GDB104, LikeBasic) {
    auto stmt = parse_one("SELECT name LIKE '%alice%'");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* like = dynamic_cast<LikeExpr*>(sel->items[0].expr.get());
    ASSERT_NE(like, nullptr);
    EXPECT_FALSE(like->negated);
}

TEST(QA_GDB104, NotLike) {
    auto stmt = parse_one("SELECT name NOT LIKE '%bob%'");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* like = dynamic_cast<LikeExpr*>(sel->items[0].expr.get());
    ASSERT_NE(like, nullptr);
    EXPECT_TRUE(like->negated);
}

TEST(QA_GDB104, CaseSearched) {
    auto stmt =
        parse_one("SELECT CASE WHEN a > 0 THEN 'pos' WHEN a < 0 THEN 'neg' ELSE 'zero' END");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* ce = dynamic_cast<CaseExpr*>(sel->items[0].expr.get());
    ASSERT_NE(ce, nullptr);
    EXPECT_EQ(ce->operand, nullptr); // searched CASE, no operand
    EXPECT_EQ(ce->whens.size(), 2u);
    EXPECT_NE(ce->else_expr, nullptr);
}

TEST(QA_GDB104, CaseSimple) {
    auto stmt = parse_one("SELECT CASE status WHEN 1 THEN 'active' WHEN 0 THEN 'inactive' END");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* ce = dynamic_cast<CaseExpr*>(sel->items[0].expr.get());
    ASSERT_NE(ce, nullptr);
    EXPECT_NE(ce->operand, nullptr); // simple CASE with operand
    EXPECT_EQ(ce->whens.size(), 2u);
    EXPECT_EQ(ce->else_expr, nullptr);
}

TEST(QA_GDB104, CaseNoWhen) {
    // CASE with no WHEN — missing END will error.
    expect_parse_error("SELECT CASE END");
}

TEST(QA_GDB104, CastFunction) {
    auto stmt = parse_one("SELECT CAST(a AS INT)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* cast = dynamic_cast<CastExpr*>(sel->items[0].expr.get());
    ASSERT_NE(cast, nullptr);
    EXPECT_EQ(cast->target_type.name, "INT");
}

TEST(QA_GDB104, CastPostgresStyle) {
    auto stmt = parse_one("SELECT a::INT");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* cast = dynamic_cast<CastExpr*>(sel->items[0].expr.get());
    ASSERT_NE(cast, nullptr);
    EXPECT_EQ(cast->target_type.name, "INT");
}

TEST(QA_GDB104, CastChained) {
    // a::INT::TEXT — multiple casts chained.
    auto stmt = parse_one("SELECT a::INT::TEXT");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* outer = dynamic_cast<CastExpr*>(sel->items[0].expr.get());
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->target_type.name, "TEXT");
    auto* inner = dynamic_cast<CastExpr*>(outer->expr.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->target_type.name, "INT");
}

TEST(QA_GDB104, ExistsSubquery) {
    auto stmt = parse_one("SELECT EXISTS (SELECT 1 FROM t WHERE a = 1)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* exists = dynamic_cast<ExistsExpr*>(sel->items[0].expr.get());
    ASSERT_NE(exists, nullptr);
    EXPECT_NE(exists->subquery, nullptr);
}

TEST(QA_GDB104, ScalarSubquery) {
    auto stmt = parse_one("SELECT (SELECT MAX(id) FROM t)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* sub = dynamic_cast<SubqueryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(sub, nullptr);
    EXPECT_NE(sub->subquery, nullptr);
}

// =============================================================================
// Function calls
// =============================================================================

TEST(QA_GDB104, FunctionCallNoArgs) {
    auto stmt = parse_one("SELECT now()");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* fn = dynamic_cast<FunctionCallExpr*>(sel->items[0].expr.get());
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->name, "now");
    EXPECT_TRUE(fn->args.empty());
}

TEST(QA_GDB104, FunctionCallMultipleArgs) {
    auto stmt = parse_one("SELECT coalesce(a, b, c)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* fn = dynamic_cast<FunctionCallExpr*>(sel->items[0].expr.get());
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->args.size(), 3u);
}

TEST(QA_GDB104, CountStar) {
    auto stmt = parse_one("SELECT COUNT(*)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* fn = dynamic_cast<FunctionCallExpr*>(sel->items[0].expr.get());
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->name, "COUNT");
    EXPECT_EQ(fn->args.size(), 1u);
    auto* star = dynamic_cast<ColumnRefExpr*>(fn->args[0].get());
    ASSERT_NE(star, nullptr);
    EXPECT_EQ(star->column, "*");
}

TEST(QA_GDB104, CountDistinct) {
    auto stmt = parse_one("SELECT COUNT(DISTINCT name)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* fn = dynamic_cast<FunctionCallExpr*>(sel->items[0].expr.get());
    ASSERT_NE(fn, nullptr);
    EXPECT_TRUE(fn->distinct);
    EXPECT_EQ(fn->args.size(), 1u);
}

TEST(QA_GDB104, NestedFunctionCalls) {
    auto stmt = parse_one("SELECT upper(trim(name))");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* outer = dynamic_cast<FunctionCallExpr*>(sel->items[0].expr.get());
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->name, "upper");
    auto* inner = dynamic_cast<FunctionCallExpr*>(outer->args[0].get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->name, "trim");
}

// =============================================================================
// Column references
// =============================================================================

TEST(QA_GDB104, UnqualifiedColumn) {
    auto stmt = parse_one("SELECT name");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* col = dynamic_cast<ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "name");
    EXPECT_TRUE(col->table.empty());
}

TEST(QA_GDB104, QualifiedColumn) {
    auto stmt = parse_one("SELECT users.name");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* col = dynamic_cast<ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->table, "users");
    EXPECT_EQ(col->column, "name");
}

// =============================================================================
// Literal types
// =============================================================================

TEST(QA_GDB104, LiteralInteger) {
    auto stmt = parse_one("SELECT 42");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(sel->items[0].expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::INTEGER);
    EXPECT_EQ(lit->value, "42");
}

TEST(QA_GDB104, LiteralFloat) {
    auto stmt = parse_one("SELECT 3.14");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(sel->items[0].expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::FLOAT);
}

TEST(QA_GDB104, LiteralString) {
    auto stmt = parse_one("SELECT 'hello world'");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(sel->items[0].expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::STRING);
    EXPECT_EQ(lit->value, "hello world");
}

TEST(QA_GDB104, LiteralStringWithEscapedQuote) {
    auto stmt = parse_one("SELECT 'it''s'");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(sel->items[0].expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->value, "it's");
}

TEST(QA_GDB104, LiteralNull) {
    auto stmt = parse_one("SELECT NULL");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(sel->items[0].expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::NULL_LITERAL);
}

TEST(QA_GDB104, LiteralBoolean) {
    auto stmt = parse_one("SELECT TRUE, FALSE");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* t = dynamic_cast<LiteralExpr*>(sel->items[0].expr.get());
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind, LiteralKind::BOOLEAN);
    EXPECT_EQ(t->value, "true");
    auto* f = dynamic_cast<LiteralExpr*>(sel->items[1].expr.get());
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->value, "false");
}

// =============================================================================
// Array literals
// =============================================================================

TEST(QA_GDB104, ArrayLiteralBasic) {
    auto stmt = parse_one("SELECT [1.0, 2.0, 3.0]");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* arr = dynamic_cast<ArrayExpr*>(sel->items[0].expr.get());
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->elements.size(), 3u);
}

TEST(QA_GDB104, ArrayLiteralEmpty) {
    auto stmt = parse_one("SELECT []");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* arr = dynamic_cast<ArrayExpr*>(sel->items[0].expr.get());
    ASSERT_NE(arr, nullptr);
    EXPECT_TRUE(arr->elements.empty());
}

TEST(QA_GDB104, ArrayLiteralNested) {
    // Not typical SQL but parser should handle expressions in array.
    auto stmt = parse_one("SELECT [1 + 2, 3 * 4]");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* arr = dynamic_cast<ArrayExpr*>(sel->items[0].expr.get());
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->elements.size(), 2u);
    auto* add = dynamic_cast<BinaryExpr*>(arr->elements[0].get());
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->op, BinaryOp::ADD);
}

// =============================================================================
// Complex / nested expressions
// =============================================================================

TEST(QA_GDB104, ComplexWhereClause) {
    auto stmt = parse_one("SELECT a FROM t WHERE (a > 5 AND b < 10) OR c IS NOT NULL");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* or_expr = dynamic_cast<BinaryExpr*>(sel->where_expr.get());
    ASSERT_NE(or_expr, nullptr);
    EXPECT_EQ(or_expr->op, BinaryOp::OR);
}

TEST(QA_GDB104, BetweenInWhere) {
    auto stmt = parse_one("SELECT * FROM t WHERE a BETWEEN 1 AND 10 AND b = 'x'");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    // The AND between BETWEEN bounds should not interfere with outer AND.
    auto* and_expr = dynamic_cast<BinaryExpr*>(sel->where_expr.get());
    ASSERT_NE(and_expr, nullptr);
    EXPECT_EQ(and_expr->op, BinaryOp::AND);
    auto* bet = dynamic_cast<BetweenExpr*>(and_expr->lhs.get());
    ASSERT_NE(bet, nullptr);
}

TEST(QA_GDB104, DeeplyNestedParens) {
    auto stmt = parse_one("SELECT ((((1 + 2))))");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* add = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->op, BinaryOp::ADD);
}

TEST(QA_GDB104, ExpressionInInsertValue) {
    auto stmt = parse_one("INSERT INTO t (a) VALUES (CASE WHEN 1 > 0 THEN 'yes' ELSE 'no' END)");
    auto* ins = dynamic_cast<InsertStmt*>(stmt.get());
    ASSERT_NE(ins, nullptr);
    auto* ce = dynamic_cast<CaseExpr*>(ins->values[0][0].get());
    ASSERT_NE(ce, nullptr);
}

TEST(QA_GDB104, ComparisonAllOps) {
    // Test all 6 comparison operators parse correctly.
    const char* ops[] = {"=", "!=", "<", ">", "<=", ">="};
    BinaryOp expected[] = {
        BinaryOp::EQUAL,
        BinaryOp::NOT_EQUAL,
        BinaryOp::LESS,
        BinaryOp::GREATER,
        BinaryOp::LESS_EQUAL,
        BinaryOp::GREATER_EQUAL,
    };
    for (int i = 0; i < 6; ++i) {
        std::string sql = std::string("SELECT a ") + ops[i] + " b";
        auto s = parse_one(sql);
        auto* sel = dynamic_cast<SelectStmt*>(s.get());
        ASSERT_NE(sel, nullptr) << "Failed for op: " << ops[i];
        auto* bin = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
        ASSERT_NE(bin, nullptr) << "Failed for op: " << ops[i];
        EXPECT_EQ(bin->op, expected[i]) << "Failed for op: " << ops[i];
    }
}

// =============================================================================
// Error paths
// =============================================================================

TEST(QA_GDB104, UnclosedParen) {
    expect_parse_error("SELECT (1 + 2");
}

TEST(QA_GDB104, UnclosedBracket) {
    expect_parse_error("SELECT [1, 2");
}

TEST(QA_GDB104, MissingExpression) {
    expect_parse_error("SELECT 1 +");
}

TEST(QA_GDB104, CaseMissingEnd) {
    expect_parse_error("SELECT CASE WHEN 1 > 0 THEN 'yes'");
}

TEST(QA_GDB104, CastMissingAs) {
    expect_parse_error("SELECT CAST(a INT)");
}

TEST(QA_GDB104, InMissingParen) {
    expect_parse_error("SELECT a IN 1, 2, 3");
}

TEST(QA_GDB104, BetweenMissingAnd) {
    expect_parse_error("SELECT a BETWEEN 1 10");
}

TEST(QA_GDB104, ExistsMissingParen) {
    expect_parse_error("SELECT EXISTS SELECT 1");
}
