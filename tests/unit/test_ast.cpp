// test_ast.cpp — AST/parser tests.
//
// Strategy: replace the original getter==setter vacuous tests with parse-based
// tests (parse a SQL string and assert the resulting AST shape against
// hard-coded expected values) or real AST-method tests (move semantics,
// child-wiring). Hardcoded expected values are derived by hand from the SQL
// text — a regression in the parser or AST makes the test FAIL.

#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"

#include <gtest/gtest.h>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helper: parse one statement from SQL text. Calls ADD_FAILURE() on lex/parse
// error so the calling test gets a clear failure message; returns nullptr.
// ---------------------------------------------------------------------------
static StmtPtr parse_one(const std::string& sql) {
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

// Typed cast helper: dynamic_cast and emit ADD_FAILURE on nullptr.
template <typename T>
static T* as(StmtPtr& stmt) {
    auto* p = dynamic_cast<T*>(stmt.get());
    if (p == nullptr) {
        ADD_FAILURE() << "Statement is not of expected type";
    }
    return p;
}

// -- Literal expression tests (parse-based) ----------------------------------

TEST(Ast, LiteralExprInteger) {
    // Parser must produce a LiteralExpr with kind INTEGER and value "42".
    auto stmt = parse_one("SELECT 42");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* lit = dynamic_cast<LiteralExpr*>(sel->items[0].expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::INTEGER);
    EXPECT_EQ(lit->value, "42");
}

TEST(Ast, LiteralExprString) {
    auto stmt = parse_one("SELECT 'hello'");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* lit = dynamic_cast<LiteralExpr*>(sel->items[0].expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::STRING);
    EXPECT_EQ(lit->value, "hello");
}

TEST(Ast, LiteralExprNull) {
    auto stmt = parse_one("SELECT NULL");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* lit = dynamic_cast<LiteralExpr*>(sel->items[0].expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::NULL_LITERAL);
    EXPECT_TRUE(lit->value.empty());
}

TEST(Ast, LiteralExprBoolean) {
    auto stmt = parse_one("SELECT TRUE");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* lit = dynamic_cast<LiteralExpr*>(sel->items[0].expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::BOOLEAN);
    EXPECT_EQ(lit->value, "true");
}

TEST(Ast, LiteralExprFloat) {
    auto stmt = parse_one("SELECT 3.14");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* lit = dynamic_cast<LiteralExpr*>(sel->items[0].expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::FLOAT);
    EXPECT_EQ(lit->value, "3.14");
}

// -- Column reference tests (parse-based) ------------------------------------

TEST(Ast, ColumnRefUnqualified) {
    auto stmt = parse_one("SELECT name FROM users");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* col = dynamic_cast<ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_TRUE(col->table.empty());
    EXPECT_EQ(col->column, "name");
}

TEST(Ast, ColumnRefQualified) {
    auto stmt = parse_one("SELECT users.id FROM users");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* col = dynamic_cast<ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->table, "users");
    EXPECT_EQ(col->column, "id");
}

// -- Binary expression tests (parse-based) -----------------------------------

TEST(Ast, BinaryExpr) {
    // Parser must produce BinaryExpr ADD with literal children 1 and 2.
    auto stmt = parse_one("SELECT 1 + 2");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* bin = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::ADD);
    auto* lhs = dynamic_cast<LiteralExpr*>(bin->lhs.get());
    auto* rhs = dynamic_cast<LiteralExpr*>(bin->rhs.get());
    ASSERT_NE(lhs, nullptr);
    ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(lhs->value, "1");
    EXPECT_EQ(rhs->value, "2");
}

TEST(Ast, DeepBinaryTree) {
    // Parser must build the correct tree for (1 + 2) * (3 - 4): top is MUL,
    // left child is ADD, right child is SUBTRACT.
    auto stmt = parse_one("SELECT (1 + 2) * (3 - 4)");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* mul = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op, BinaryOp::MULTIPLY);
    auto* left = dynamic_cast<BinaryExpr*>(mul->lhs.get());
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(left->op, BinaryOp::ADD);
    auto* right = dynamic_cast<BinaryExpr*>(mul->rhs.get());
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(right->op, BinaryOp::SUBTRACT);
}

// -- Unary expression tests (parse-based) ------------------------------------

TEST(Ast, UnaryExpr) {
    auto stmt = parse_one("SELECT -5");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* neg = dynamic_cast<UnaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(neg, nullptr);
    EXPECT_EQ(neg->op, UnaryOp::NEGATE);
    auto* operand = dynamic_cast<LiteralExpr*>(neg->operand.get());
    ASSERT_NE(operand, nullptr);
    EXPECT_EQ(operand->value, "5");
}

TEST(Ast, UnaryNot) {
    auto stmt = parse_one("SELECT NOT TRUE");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* not_expr = dynamic_cast<UnaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(not_expr, nullptr);
    EXPECT_EQ(not_expr->op, UnaryOp::NOT);
    auto* inner = dynamic_cast<LiteralExpr*>(not_expr->operand.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->kind, LiteralKind::BOOLEAN);
}

// -- Function call tests (parse-based) ----------------------------------------

TEST(Ast, FunctionCallExpr) {
    auto stmt = parse_one("SELECT COUNT(DISTINCT id) FROM users");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* fn = dynamic_cast<FunctionCallExpr*>(sel->items[0].expr.get());
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->name, "COUNT");
    EXPECT_TRUE(fn->distinct);
    ASSERT_EQ(fn->args.size(), 1u);
    auto* arg = dynamic_cast<ColumnRefExpr*>(fn->args[0].get());
    ASSERT_NE(arg, nullptr);
    EXPECT_EQ(arg->column, "id");
}

TEST(Ast, FunctionCallMultipleArgs) {
    auto stmt = parse_one("SELECT COALESCE(a, b, 0) FROM t");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* fn = dynamic_cast<FunctionCallExpr*>(sel->items[0].expr.get());
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->name, "COALESCE");
    EXPECT_EQ(fn->args.size(), 3u);
}

// -- CAST expression (parse-based) --------------------------------------------

TEST(Ast, CastExpr) {
    auto stmt = parse_one("SELECT CAST('42' AS INT)");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* cast = dynamic_cast<CastExpr*>(sel->items[0].expr.get());
    ASSERT_NE(cast, nullptr);
    EXPECT_EQ(cast->target_type.name, "INT");
    auto* inner = dynamic_cast<LiteralExpr*>(cast->expr.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->value, "42");
}

// -- CASE expression (parse-based) --------------------------------------------

TEST(Ast, CaseExprSearched) {
    // Searched CASE: no operand between CASE and WHEN.
    auto stmt = parse_one("SELECT CASE WHEN TRUE THEN 'yes' ELSE 'no' END");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* case_expr = dynamic_cast<CaseExpr*>(sel->items[0].expr.get());
    ASSERT_NE(case_expr, nullptr);
    EXPECT_EQ(case_expr->operand, nullptr); // no operand => searched form
    ASSERT_EQ(case_expr->whens.size(), 1u);
    EXPECT_NE(case_expr->else_expr, nullptr);
    auto* result = dynamic_cast<LiteralExpr*>(case_expr->whens[0].result.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->value, "yes");
}

TEST(Ast, CaseExprSimple) {
    // Simple CASE: has an operand between CASE and WHEN.
    auto stmt = parse_one("SELECT CASE status WHEN 1 THEN 'active' END");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* case_expr = dynamic_cast<CaseExpr*>(sel->items[0].expr.get());
    ASSERT_NE(case_expr, nullptr);
    EXPECT_NE(case_expr->operand, nullptr); // has operand => simple form
    auto* op = dynamic_cast<ColumnRefExpr*>(case_expr->operand.get());
    ASSERT_NE(op, nullptr);
    EXPECT_EQ(op->column, "status");
    ASSERT_EQ(case_expr->whens.size(), 1u);
}

// -- IN expression (parse-based) ----------------------------------------------

TEST(Ast, InExprValues) {
    auto stmt = parse_one("SELECT * FROM t WHERE id IN (1, 2, 3)");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_NE(sel->where_expr, nullptr);
    auto* in_expr = dynamic_cast<InExpr*>(sel->where_expr.get());
    ASSERT_NE(in_expr, nullptr);
    EXPECT_EQ(in_expr->values.size(), 3u);
    EXPECT_FALSE(in_expr->negated);
    EXPECT_EQ(in_expr->subquery, nullptr);
}

TEST(Ast, InExprNotIn) {
    auto stmt = parse_one("SELECT * FROM t WHERE id NOT IN (1)");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    auto* in_expr = dynamic_cast<InExpr*>(sel->where_expr.get());
    ASSERT_NE(in_expr, nullptr);
    EXPECT_TRUE(in_expr->negated);
    ASSERT_EQ(in_expr->values.size(), 1u);
}

// -- BETWEEN expression (parse-based) -----------------------------------------

TEST(Ast, BetweenExpr) {
    auto stmt = parse_one("SELECT * FROM t WHERE age BETWEEN 18 AND 65");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    auto* between = dynamic_cast<BetweenExpr*>(sel->where_expr.get());
    ASSERT_NE(between, nullptr);
    EXPECT_FALSE(between->negated);
    auto* lo = dynamic_cast<LiteralExpr*>(between->low.get());
    auto* hi = dynamic_cast<LiteralExpr*>(between->high.get());
    ASSERT_NE(lo, nullptr);
    ASSERT_NE(hi, nullptr);
    EXPECT_EQ(lo->value, "18");
    EXPECT_EQ(hi->value, "65");
}

// -- IS NULL / IS NOT NULL (parse-based) --------------------------------------

TEST(Ast, IsNullExpr) {
    auto stmt = parse_one("SELECT * FROM t WHERE email IS NULL");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    auto* is_null = dynamic_cast<IsNullExpr*>(sel->where_expr.get());
    ASSERT_NE(is_null, nullptr);
    EXPECT_FALSE(is_null->negated);
    auto* col = dynamic_cast<ColumnRefExpr*>(is_null->expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "email");
}

TEST(Ast, IsNotNullExpr) {
    auto stmt = parse_one("SELECT * FROM t WHERE email IS NOT NULL");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    auto* is_null = dynamic_cast<IsNullExpr*>(sel->where_expr.get());
    ASSERT_NE(is_null, nullptr);
    EXPECT_TRUE(is_null->negated);
}

// -- LIKE (parse-based) -------------------------------------------------------

TEST(Ast, LikeExpr) {
    auto stmt = parse_one("SELECT * FROM t WHERE name LIKE '%smith%'");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    auto* like = dynamic_cast<LikeExpr*>(sel->where_expr.get());
    ASSERT_NE(like, nullptr);
    EXPECT_FALSE(like->negated);
    auto* col = dynamic_cast<ColumnRefExpr*>(like->expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "name");
    auto* pat = dynamic_cast<LiteralExpr*>(like->pattern.get());
    ASSERT_NE(pat, nullptr);
    EXPECT_EQ(pat->value, "%smith%");
}

// -- Array expression (parse-based via NEAREST) -------------------------------

TEST(Ast, ArrayExpr) {
    // Array literal [1, 2, 3] appears as the NEAREST target.
    // Verify the parser builds an ArrayExpr with 3 elements.
    auto stmt = parse_one("SELECT * FROM t WHERE NEAREST(embedding, 3) TO [1, 2, 3]");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    auto* nearest = dynamic_cast<NearestExpr*>(sel->where_expr.get());
    ASSERT_NE(nearest, nullptr);
    auto* arr = dynamic_cast<ArrayExpr*>(nearest->target.get());
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->elements.size(), 3u);
}

// -- Source location on expressions (parse-based) ----------------------------

TEST(Ast, ExprLineColumn) {
    // The lexer/parser must record line=1 and col>0 for a literal in SELECT.
    auto stmt = parse_one("SELECT 42");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* lit = dynamic_cast<LiteralExpr*>(sel->items[0].expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->line, 1u);
    EXPECT_GT(lit->col, 0u);
}

// -- TypeSpec (parse-based via CREATE TABLE) ----------------------------------

TEST(Ast, TypeSpecBasic) {
    auto stmt = parse_one("CREATE TABLE t (x INT)");
    ASSERT_NE(stmt, nullptr);
    auto* ct = as<CreateTableStmt>(stmt);
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->columns.size(), 1u);
    EXPECT_EQ(ct->columns[0].type.name, "INT");
    EXPECT_FALSE(ct->columns[0].type.param1.has_value());
}

TEST(Ast, TypeSpecVarchar) {
    auto stmt = parse_one("CREATE TABLE t (s VARCHAR(255))");
    ASSERT_NE(stmt, nullptr);
    auto* ct = as<CreateTableStmt>(stmt);
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->columns.size(), 1u);
    EXPECT_EQ(ct->columns[0].type.name, "VARCHAR");
    ASSERT_TRUE(ct->columns[0].type.param1.has_value());
    EXPECT_EQ(ct->columns[0].type.param1.value(), 255);
}

TEST(Ast, TypeSpecDecimal) {
    auto stmt = parse_one("CREATE TABLE t (price DECIMAL(10, 2))");
    ASSERT_NE(stmt, nullptr);
    auto* ct = as<CreateTableStmt>(stmt);
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->columns.size(), 1u);
    EXPECT_EQ(ct->columns[0].type.name, "DECIMAL");
    ASSERT_TRUE(ct->columns[0].type.param1.has_value());
    ASSERT_TRUE(ct->columns[0].type.param2.has_value());
    EXPECT_EQ(ct->columns[0].type.param1.value(), 10);
    EXPECT_EQ(ct->columns[0].type.param2.value(), 2);
}

TEST(Ast, TypeSpecEmbedding) {
    // Positional EMBEDDING syntax: EMBEDDING(dim, source_col, 'provider').
    auto stmt = parse_one("CREATE TABLE t (v EMBEDDING(384, description, 'openai'))");
    ASSERT_NE(stmt, nullptr);
    auto* ct = as<CreateTableStmt>(stmt);
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->columns.size(), 1u);
    const auto& col = ct->columns[0];
    EXPECT_EQ(col.type.name, "EMBEDDING");
    ASSERT_TRUE(col.type.param1.has_value());
    EXPECT_EQ(col.type.param1.value(), 384);
    EXPECT_EQ(col.type.source, "description");
    EXPECT_EQ(col.type.provider, "openai");
}

// -- AstColumnDef (parse-based) -----------------------------------------------

TEST(Ast, AstColumnDefWithDefaults) {
    auto stmt = parse_one("CREATE TABLE t (age INT NOT NULL DEFAULT 0)");
    ASSERT_NE(stmt, nullptr);
    auto* ct = as<CreateTableStmt>(stmt);
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->columns.size(), 1u);
    const auto& col = ct->columns[0];
    EXPECT_EQ(col.name, "age");
    EXPECT_FALSE(col.nullable);
    ASSERT_NE(col.default_expr, nullptr);
    auto* def = dynamic_cast<LiteralExpr*>(col.default_expr.get());
    ASSERT_NE(def, nullptr);
    EXPECT_EQ(def->value, "0");
}

TEST(Ast, AstColumnDefWithForeignKey) {
    auto stmt = parse_one("CREATE TABLE t (user_id INT REFERENCES users(id) ON DELETE CASCADE)");
    ASSERT_NE(stmt, nullptr);
    auto* ct = as<CreateTableStmt>(stmt);
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->columns.size(), 1u);
    const auto& col = ct->columns[0];
    EXPECT_EQ(col.fk_table, "users");
    EXPECT_EQ(col.fk_column, "id");
    EXPECT_EQ(col.fk_on_delete, ReferentialAction::CASCADE);
}

// -- TableConstraint (parse-based) -------------------------------------------

TEST(Ast, TableConstraintPrimaryKey) {
    auto stmt = parse_one("CREATE TABLE t (id INT, PRIMARY KEY (id))");
    ASSERT_NE(stmt, nullptr);
    auto* ct = as<CreateTableStmt>(stmt);
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->constraints.size(), 1u);
    EXPECT_EQ(ct->constraints[0].kind, TableConstraint::Kind::PRIMARY_KEY);
    ASSERT_EQ(ct->constraints[0].columns.size(), 1u);
    EXPECT_EQ(ct->constraints[0].columns[0], "id");
}

TEST(Ast, TableConstraintCompositePK) {
    auto stmt = parse_one("CREATE TABLE t (user_id INT, role_id INT, "
                          "PRIMARY KEY (user_id, role_id))");
    ASSERT_NE(stmt, nullptr);
    auto* ct = as<CreateTableStmt>(stmt);
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->constraints.size(), 1u);
    EXPECT_EQ(ct->constraints[0].columns.size(), 2u);
    EXPECT_EQ(ct->constraints[0].columns[0], "user_id");
    EXPECT_EQ(ct->constraints[0].columns[1], "role_id");
}

// -- TableRef (parse-based) ---------------------------------------------------

TEST(Ast, TableRefSimple) {
    auto stmt = parse_one("SELECT * FROM users AS u");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].name, "users");
    EXPECT_EQ(sel->from[0].alias, "u");
    EXPECT_EQ(sel->from[0].subquery, nullptr);
}

// -- ORDER BY (parse-based) ---------------------------------------------------

TEST(Ast, OrderByItem) {
    auto stmt = parse_one("SELECT * FROM t ORDER BY name DESC");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->order_by.size(), 1u);
    EXPECT_EQ(sel->order_by[0].direction, SortDirection::DESC);
    auto* col = dynamic_cast<ColumnRefExpr*>(sel->order_by[0].expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "name");
}

// -- SELECT items (parse-based) -----------------------------------------------

TEST(Ast, SelectItemStar) {
    auto stmt = parse_one("SELECT * FROM t");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    EXPECT_TRUE(sel->items[0].is_star);
    EXPECT_EQ(sel->items[0].expr, nullptr);
}

TEST(Ast, SelectItemTableStar) {
    auto stmt = parse_one("SELECT users.* FROM users");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    EXPECT_TRUE(sel->items[0].is_star);
    EXPECT_EQ(sel->items[0].table_star, "users");
}

TEST(Ast, SelectItemExprAlias) {
    auto stmt = parse_one("SELECT name AS user_name FROM users");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    EXPECT_EQ(sel->items[0].alias, "user_name");
    auto* col = dynamic_cast<ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "name");
}

// -- DDL statement tests (parse-based) ----------------------------------------

TEST(Ast, CreateTableStmt) {
    auto stmt = parse_one("CREATE TABLE IF NOT EXISTS users ("
                          "  id INT NOT NULL,"
                          "  name VARCHAR(100),"
                          "  PRIMARY KEY (id)"
                          ")");
    ASSERT_NE(stmt, nullptr);
    auto* ct = as<CreateTableStmt>(stmt);
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->name, "users");
    EXPECT_TRUE(ct->if_not_exists);
    ASSERT_EQ(ct->columns.size(), 2u);
    EXPECT_EQ(ct->columns[0].name, "id");
    EXPECT_EQ(ct->columns[1].name, "name");
    ASSERT_EQ(ct->constraints.size(), 1u);
    EXPECT_EQ(ct->constraints[0].kind, TableConstraint::Kind::PRIMARY_KEY);
}

TEST(Ast, DropTableStmt) {
    auto stmt = parse_one("DROP TABLE IF EXISTS old_table CASCADE");
    ASSERT_NE(stmt, nullptr);
    auto* dt = as<DropTableStmt>(stmt);
    ASSERT_NE(dt, nullptr);
    EXPECT_EQ(dt->name, "old_table");
    EXPECT_TRUE(dt->if_exists);
    EXPECT_TRUE(dt->cascade);
}

TEST(Ast, AlterTableAddColumn) {
    auto stmt = parse_one("ALTER TABLE users ADD COLUMN email VARCHAR(255)");
    ASSERT_NE(stmt, nullptr);
    auto* alt = as<AlterTableStmt>(stmt);
    ASSERT_NE(alt, nullptr);
    EXPECT_EQ(alt->table_name, "users");
    EXPECT_EQ(alt->action, AlterAction::ADD_COLUMN);
    EXPECT_EQ(alt->column.name, "email");
    EXPECT_EQ(alt->column.type.name, "VARCHAR");
    ASSERT_TRUE(alt->column.type.param1.has_value());
    EXPECT_EQ(alt->column.type.param1.value(), 255);
}

TEST(Ast, AlterTableRenameColumn) {
    auto stmt = parse_one("ALTER TABLE users RENAME COLUMN old_name TO new_name");
    ASSERT_NE(stmt, nullptr);
    auto* alt = as<AlterTableStmt>(stmt);
    ASSERT_NE(alt, nullptr);
    EXPECT_EQ(alt->action, AlterAction::RENAME_COLUMN);
    EXPECT_EQ(alt->column_name, "old_name");
    EXPECT_EQ(alt->new_column_name, "new_name");
}

TEST(Ast, CreateIndexStmt) {
    auto stmt = parse_one("CREATE UNIQUE INDEX idx_users_name ON users "
                          "(last_name, first_name) USING btree");
    ASSERT_NE(stmt, nullptr);
    auto* ci = as<CreateIndexStmt>(stmt);
    ASSERT_NE(ci, nullptr);
    EXPECT_EQ(ci->name, "idx_users_name");
    EXPECT_EQ(ci->table_name, "users");
    ASSERT_EQ(ci->columns.size(), 2u);
    EXPECT_EQ(ci->columns[0], "last_name");
    EXPECT_EQ(ci->columns[1], "first_name");
    EXPECT_TRUE(ci->is_unique);
    EXPECT_EQ(ci->method, "btree");
}

TEST(Ast, DropIndexStmt) {
    auto stmt = parse_one("DROP INDEX IF EXISTS idx_old");
    ASSERT_NE(stmt, nullptr);
    auto* di = as<DropIndexStmt>(stmt);
    ASSERT_NE(di, nullptr);
    EXPECT_EQ(di->name, "idx_old");
    EXPECT_TRUE(di->if_exists);
}

TEST(Ast, CreateEdgeTypeStmt) {
    // Properties come BEFORE FROM...TO in the grammar:
    //   CREATE EDGE TYPE name (prop type) FROM src TO dst
    auto stmt = parse_one("CREATE EDGE TYPE follows (since TIMESTAMP) FROM users TO users");
    ASSERT_NE(stmt, nullptr);
    auto* ce = as<CreateEdgeTypeStmt>(stmt);
    ASSERT_NE(ce, nullptr);
    EXPECT_EQ(ce->name, "follows");
    EXPECT_EQ(ce->from_table, "users");
    EXPECT_EQ(ce->to_table, "users");
    ASSERT_EQ(ce->properties.size(), 1u);
    EXPECT_EQ(ce->properties[0].name, "since");
    EXPECT_EQ(ce->properties[0].type.name, "TIMESTAMP");
}

TEST(Ast, DropEdgeTypeStmt) {
    auto stmt = parse_one("DROP EDGE TYPE IF EXISTS follows");
    ASSERT_NE(stmt, nullptr);
    auto* de = as<DropEdgeTypeStmt>(stmt);
    ASSERT_NE(de, nullptr);
    EXPECT_EQ(de->name, "follows");
    EXPECT_TRUE(de->if_exists);
}

// -- DML statement tests (parse-based) ----------------------------------------

TEST(Ast, InsertStmtValues) {
    auto stmt = parse_one("INSERT INTO users (name, age) VALUES ('Alice', 30)");
    ASSERT_NE(stmt, nullptr);
    auto* ins = as<InsertStmt>(stmt);
    ASSERT_NE(ins, nullptr);
    EXPECT_EQ(ins->table_name, "users");
    ASSERT_EQ(ins->columns.size(), 2u);
    EXPECT_EQ(ins->columns[0], "name");
    EXPECT_EQ(ins->columns[1], "age");
    ASSERT_EQ(ins->values.size(), 1u);
    ASSERT_EQ(ins->values[0].size(), 2u);
    EXPECT_EQ(ins->select, nullptr);
    auto* name_lit = dynamic_cast<LiteralExpr*>(ins->values[0][0].get());
    ASSERT_NE(name_lit, nullptr);
    EXPECT_EQ(name_lit->value, "Alice");
    auto* age_lit = dynamic_cast<LiteralExpr*>(ins->values[0][1].get());
    ASSERT_NE(age_lit, nullptr);
    EXPECT_EQ(age_lit->value, "30");
}

TEST(Ast, InsertStmtMultipleRows) {
    auto stmt = parse_one("INSERT INTO data (x) VALUES (0), (1), (2)");
    ASSERT_NE(stmt, nullptr);
    auto* ins = as<InsertStmt>(stmt);
    ASSERT_NE(ins, nullptr);
    EXPECT_EQ(ins->values.size(), 3u);
    for (int i = 0; i < 3; ++i) {
        auto* lit = dynamic_cast<LiteralExpr*>(ins->values[static_cast<size_t>(i)][0].get());
        ASSERT_NE(lit, nullptr);
        EXPECT_EQ(lit->value, std::to_string(i));
    }
}

TEST(Ast, UpdateStmt) {
    auto stmt = parse_one("UPDATE users SET name = 'Bob' WHERE id = 1");
    ASSERT_NE(stmt, nullptr);
    auto* upd = as<UpdateStmt>(stmt);
    ASSERT_NE(upd, nullptr);
    EXPECT_EQ(upd->table_name, "users");
    ASSERT_EQ(upd->assignments.size(), 1u);
    EXPECT_EQ(upd->assignments[0].column, "name");
    auto* val = dynamic_cast<LiteralExpr*>(upd->assignments[0].value.get());
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(val->value, "Bob");
    ASSERT_NE(upd->where_expr, nullptr);
    auto* where = dynamic_cast<BinaryExpr*>(upd->where_expr.get());
    ASSERT_NE(where, nullptr);
    EXPECT_EQ(where->op, BinaryOp::EQUAL);
}

TEST(Ast, DeleteStmt) {
    auto stmt = parse_one("DELETE FROM users WHERE id = 99");
    ASSERT_NE(stmt, nullptr);
    auto* del = as<DeleteStmt>(stmt);
    ASSERT_NE(del, nullptr);
    EXPECT_EQ(del->table_name, "users");
    ASSERT_NE(del->where_expr, nullptr);
    auto* where = dynamic_cast<BinaryExpr*>(del->where_expr.get());
    ASSERT_NE(where, nullptr);
    EXPECT_EQ(where->op, BinaryOp::EQUAL);
}

TEST(Ast, LinkStmt) {
    auto stmt = parse_one("LINK users(1) TO users(2) VIA follows (since = '2024-01-01')");
    ASSERT_NE(stmt, nullptr);
    auto* lnk = as<LinkStmt>(stmt);
    ASSERT_NE(lnk, nullptr);
    EXPECT_EQ(lnk->source_table, "users");
    EXPECT_EQ(lnk->target_table, "users");
    EXPECT_EQ(lnk->edge_type, "follows");
    ASSERT_EQ(lnk->properties.size(), 1u);
    EXPECT_EQ(lnk->properties[0].column, "since");
    auto* src_key = dynamic_cast<LiteralExpr*>(lnk->source_key.get());
    ASSERT_NE(src_key, nullptr);
    EXPECT_EQ(src_key->value, "1");
}

TEST(Ast, UnlinkStmt) {
    auto stmt = parse_one("UNLINK users(1) FROM users(2) VIA follows");
    ASSERT_NE(stmt, nullptr);
    auto* unl = as<UnlinkStmt>(stmt);
    ASSERT_NE(unl, nullptr);
    EXPECT_EQ(unl->source_table, "users");
    EXPECT_EQ(unl->target_table, "users");
    EXPECT_EQ(unl->edge_type, "follows");
    EXPECT_EQ(unl->where_expr, nullptr);
}

// -- Query statement tests (parse-based) --------------------------------------

TEST(Ast, SimpleSelectStmt) {
    auto stmt = parse_one("SELECT * FROM users");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_FALSE(sel->distinct);
    ASSERT_EQ(sel->items.size(), 1u);
    EXPECT_TRUE(sel->items[0].is_star);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].name, "users");
}

TEST(Ast, SelectWithJoin) {
    auto stmt = parse_one("SELECT * FROM users AS u "
                          "LEFT JOIN orders AS o ON u.id = o.user_id");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->joins.size(), 1u);
    EXPECT_EQ(sel->joins[0].type, JoinType::LEFT);
    EXPECT_EQ(sel->joins[0].table.name, "orders");
    EXPECT_EQ(sel->joins[0].table.alias, "o");
    auto* on = dynamic_cast<BinaryExpr*>(sel->joins[0].on_expr.get());
    ASSERT_NE(on, nullptr);
    EXPECT_EQ(on->op, BinaryOp::EQUAL);
}

TEST(Ast, SelectWithGroupByHaving) {
    auto stmt = parse_one("SELECT status FROM t GROUP BY status HAVING COUNT(id) > 5");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->group_by.size(), 1u);
    auto* gb = dynamic_cast<ColumnRefExpr*>(sel->group_by[0].get());
    ASSERT_NE(gb, nullptr);
    EXPECT_EQ(gb->column, "status");
    ASSERT_NE(sel->having_expr, nullptr);
    auto* having = dynamic_cast<BinaryExpr*>(sel->having_expr.get());
    ASSERT_NE(having, nullptr);
    EXPECT_EQ(having->op, BinaryOp::GREATER);
}

TEST(Ast, SelectWithOrderByLimitOffset) {
    auto stmt = parse_one("SELECT * FROM t ORDER BY name ASC LIMIT 10 OFFSET 20");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->order_by.size(), 1u);
    EXPECT_EQ(sel->order_by[0].direction, SortDirection::ASC);
    ASSERT_NE(sel->limit, nullptr);
    auto* lim = dynamic_cast<LiteralExpr*>(sel->limit.get());
    ASSERT_NE(lim, nullptr);
    EXPECT_EQ(lim->value, "10");
    ASSERT_NE(sel->offset, nullptr);
    auto* off = dynamic_cast<LiteralExpr*>(sel->offset.get());
    ASSERT_NE(off, nullptr);
    EXPECT_EQ(off->value, "20");
}

TEST(Ast, SelectWithCTE) {
    auto stmt = parse_one("WITH active_users AS (SELECT * FROM users) "
                          "SELECT * FROM active_users");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->ctes.size(), 1u);
    EXPECT_EQ(sel->ctes[0].name, "active_users");
    ASSERT_NE(sel->ctes[0].query, nullptr);
    auto* inner = dynamic_cast<SelectStmt*>(sel->ctes[0].query.get());
    ASSERT_NE(inner, nullptr);
    ASSERT_EQ(inner->from.size(), 1u);
    EXPECT_EQ(inner->from[0].name, "users");
}

TEST(Ast, SelectWithUnion) {
    auto stmt = parse_one("SELECT name FROM a UNION SELECT name FROM b");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->set_op, SelectStmt::SetOp::UNION);
    ASSERT_NE(sel->set_rhs, nullptr);
    auto* rhs = dynamic_cast<SelectStmt*>(sel->set_rhs.get());
    ASSERT_NE(rhs, nullptr);
    ASSERT_EQ(rhs->from.size(), 1u);
    EXPECT_EQ(rhs->from[0].name, "b");
}

TEST(Ast, TraverseStmt) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 3 FETCH");
    ASSERT_NE(stmt, nullptr);
    auto* trav = as<TraverseStmt>(stmt);
    ASSERT_NE(trav, nullptr);
    EXPECT_EQ(trav->edge_type, "follows");
    EXPECT_EQ(trav->from_table, "users");
    EXPECT_EQ(trav->direction, TraverseDirection::OUT);
    ASSERT_TRUE(trav->max_depth.has_value());
    EXPECT_EQ(trav->max_depth.value(), 3);
    EXPECT_TRUE(trav->fetch);
    auto* key = dynamic_cast<LiteralExpr*>(trav->from_key.get());
    ASSERT_NE(key, nullptr);
    EXPECT_EQ(key->value, "1");
}

TEST(Ast, NearestExpr) {
    auto stmt = parse_one("SELECT * FROM t WHERE NEAREST(embedding, 10) TO [1, 2] USING COSINE");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    auto* nearest = dynamic_cast<NearestExpr*>(sel->where_expr.get());
    ASSERT_NE(nearest, nullptr);
    auto* col = dynamic_cast<ColumnRefExpr*>(nearest->column.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "embedding");
    auto* k = dynamic_cast<LiteralExpr*>(nearest->k.get());
    ASSERT_NE(k, nullptr);
    EXPECT_EQ(k->value, "10");
    EXPECT_EQ(nearest->metric, NearestMetric::COSINE);
    auto* target = dynamic_cast<ArrayExpr*>(nearest->target.get());
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->elements.size(), 2u);
}

TEST(Ast, ShortestPathStmt) {
    auto stmt = parse_one("SHORTEST PATH FROM users(1) TO users(100) VIA knows "
                          "DIRECTION BOTH MAX_DEPTH 6");
    ASSERT_NE(stmt, nullptr);
    auto* sp = as<ShortestPathStmt>(stmt);
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->from_table, "users");
    EXPECT_EQ(sp->to_table, "users");
    EXPECT_EQ(sp->edge_type, "knows");
    EXPECT_EQ(sp->direction, TraverseDirection::BOTH);
    ASSERT_TRUE(sp->max_depth.has_value());
    EXPECT_EQ(sp->max_depth.value(), 6);
}

// -- TCL statement tests (parse-based) ----------------------------------------

TEST(Ast, BeginStmt) {
    auto stmt = parse_one("BEGIN");
    ASSERT_NE(stmt, nullptr);
    // BeginStmt must derive from Stmt.
    EXPECT_NE(dynamic_cast<Stmt*>(stmt.get()), nullptr);
    EXPECT_NE(dynamic_cast<BeginStmt*>(stmt.get()), nullptr);
}

TEST(Ast, CommitStmt) {
    auto stmt = parse_one("COMMIT");
    ASSERT_NE(stmt, nullptr);
    EXPECT_NE(dynamic_cast<CommitStmt*>(stmt.get()), nullptr);
}

TEST(Ast, RollbackStmtPlain) {
    auto stmt = parse_one("ROLLBACK");
    ASSERT_NE(stmt, nullptr);
    auto* rb = as<RollbackStmt>(stmt);
    ASSERT_NE(rb, nullptr);
    EXPECT_TRUE(rb->savepoint.empty());
}

TEST(Ast, RollbackToSavepoint) {
    auto stmt = parse_one("ROLLBACK TO SAVEPOINT sp1");
    ASSERT_NE(stmt, nullptr);
    auto* rb = as<RollbackStmt>(stmt);
    ASSERT_NE(rb, nullptr);
    EXPECT_EQ(rb->savepoint, "sp1");
}

TEST(Ast, SavepointStmt) {
    auto stmt = parse_one("SAVEPOINT before_update");
    ASSERT_NE(stmt, nullptr);
    auto* sp = as<SavepointStmt>(stmt);
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->name, "before_update");
}

// -- Admin statement tests (parse-based) -------------------------------------

TEST(Ast, SetStmt) {
    auto stmt = parse_one("SET max_connections = 100");
    ASSERT_NE(stmt, nullptr);
    auto* set = as<SetStmt>(stmt);
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->parameter, "max_connections");
    auto* val = dynamic_cast<LiteralExpr*>(set->value.get());
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(val->value, "100");
}

TEST(Ast, ShowTablesStmt) {
    auto stmt = parse_one("SHOW TABLES");
    ASSERT_NE(stmt, nullptr);
    auto* show = as<ShowStmt>(stmt);
    ASSERT_NE(show, nullptr);
    EXPECT_EQ(show->target, ShowTarget::TABLES);
}

TEST(Ast, ShowColumnsStmt) {
    auto stmt = parse_one("SHOW COLUMNS FROM users");
    ASSERT_NE(stmt, nullptr);
    auto* show = as<ShowStmt>(stmt);
    ASSERT_NE(show, nullptr);
    EXPECT_EQ(show->target, ShowTarget::COLUMNS);
    EXPECT_EQ(show->name, "users");
}

TEST(Ast, ExplainStmt) {
    auto stmt = parse_one("EXPLAIN ANALYZE SELECT * FROM users");
    ASSERT_NE(stmt, nullptr);
    auto* exp = as<ExplainStmt>(stmt);
    ASSERT_NE(exp, nullptr);
    EXPECT_TRUE(exp->analyze);
    ASSERT_NE(exp->statement, nullptr);
    auto* inner = dynamic_cast<SelectStmt*>(exp->statement.get());
    ASSERT_NE(inner, nullptr);
    ASSERT_EQ(inner->from.size(), 1u);
    EXPECT_EQ(inner->from[0].name, "users");
}

TEST(Ast, DescribeStmt) {
    auto stmt = parse_one("DESCRIBE users");
    ASSERT_NE(stmt, nullptr);
    auto* desc = as<DescribeStmt>(stmt);
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->table_name, "users");
}

TEST(Ast, ReembedStmt) {
    auto stmt = parse_one("REEMBED TABLE products");
    ASSERT_NE(stmt, nullptr);
    auto* re = as<ReembedStmt>(stmt);
    ASSERT_NE(re, nullptr);
    EXPECT_EQ(re->table_name, "products");
}

TEST(Ast, VacuumStmt) {
    auto stmt = parse_one("VACUUM users");
    ASSERT_NE(stmt, nullptr);
    auto* vac = as<VacuumStmt>(stmt);
    ASSERT_NE(vac, nullptr);
    EXPECT_EQ(vac->table_name, "users");
}

TEST(Ast, VacuumStmtAll) {
    auto stmt = parse_one("VACUUM");
    ASSERT_NE(stmt, nullptr);
    auto* vac = as<VacuumStmt>(stmt);
    ASSERT_NE(vac, nullptr);
    EXPECT_TRUE(vac->table_name.empty());
}

TEST(Ast, AnalyzeStmt) {
    auto stmt = parse_one("ANALYZE orders");
    ASSERT_NE(stmt, nullptr);
    auto* anl = as<AnalyzeStmt>(stmt);
    ASSERT_NE(anl, nullptr);
    EXPECT_EQ(anl->table_name, "orders");
}

// -- Move semantics tests (AST method — not parser-dependent) ----------------

TEST(Ast, ExprMoveSemantics) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::INTEGER;
    e->value = "42";
    ExprPtr moved = std::move(e);
    EXPECT_EQ(e, nullptr); // NOLINT: testing moved-from state
    EXPECT_NE(moved, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(moved.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->value, "42");
}

TEST(Ast, StmtMoveSemantics) {
    auto stmt = std::make_unique<CreateTableStmt>();
    stmt->name = "test";
    StmtPtr moved = std::move(stmt);
    EXPECT_EQ(stmt, nullptr); // NOLINT: testing moved-from state
    auto* ct = dynamic_cast<CreateTableStmt*>(moved.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->name, "test");
}

// -- Subquery tests (parse-based) ---------------------------------------------

TEST(Ast, InExprWithSubquery) {
    auto stmt = parse_one("SELECT * FROM t WHERE user_id IN (SELECT id FROM admins)");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    auto* in_expr = dynamic_cast<InExpr*>(sel->where_expr.get());
    ASSERT_NE(in_expr, nullptr);
    ASSERT_NE(in_expr->subquery, nullptr);
    EXPECT_TRUE(in_expr->values.empty());
    auto* sub = dynamic_cast<SelectStmt*>(in_expr->subquery.get());
    ASSERT_NE(sub, nullptr);
    ASSERT_EQ(sub->from.size(), 1u);
    EXPECT_EQ(sub->from[0].name, "admins");
}

TEST(Ast, ExistsExpr) {
    auto stmt = parse_one("SELECT * FROM t WHERE EXISTS (SELECT 1 FROM admins WHERE id = t.id)");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    auto* exists = dynamic_cast<ExistsExpr*>(sel->where_expr.get());
    ASSERT_NE(exists, nullptr);
    ASSERT_NE(exists->subquery, nullptr);
    auto* sub = dynamic_cast<SelectStmt*>(exists->subquery.get());
    ASSERT_NE(sub, nullptr);
    ASSERT_EQ(sub->from.size(), 1u);
    EXPECT_EQ(sub->from[0].name, "admins");
}

TEST(Ast, SubqueryExpr) {
    // Scalar subquery in SELECT list.
    auto stmt = parse_one("SELECT (SELECT count FROM stats) AS total FROM t");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    EXPECT_EQ(sel->items[0].alias, "total");
    auto* sq = dynamic_cast<SubqueryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(sq, nullptr);
    ASSERT_NE(sq->subquery, nullptr);
    auto* inner = dynamic_cast<SelectStmt*>(sq->subquery.get());
    ASSERT_NE(inner, nullptr);
    ASSERT_EQ(inner->from.size(), 1u);
    EXPECT_EQ(inner->from[0].name, "stats");
}

TEST(Ast, TableRefSubquery) {
    // Derived table in FROM clause.
    auto stmt = parse_one("SELECT * FROM (SELECT * FROM users) AS sub");
    ASSERT_NE(stmt, nullptr);
    auto* sel = as<SelectStmt>(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_TRUE(sel->from[0].name.empty());
    EXPECT_EQ(sel->from[0].alias, "sub");
    ASSERT_NE(sel->from[0].subquery, nullptr);
    auto* inner = dynamic_cast<SelectStmt*>(sel->from[0].subquery.get());
    ASSERT_NE(inner, nullptr);
    ASSERT_EQ(inner->from.size(), 1u);
    EXPECT_EQ(inner->from[0].name, "users");
}

// -- RETURNING clause tests (parse-based) ------------------------------------

TEST(Ast, InsertWithReturning) {
    auto stmt = parse_one("INSERT INTO users (name) VALUES ('Alice') RETURNING id");
    ASSERT_NE(stmt, nullptr);
    auto* ins = as<InsertStmt>(stmt);
    ASSERT_NE(ins, nullptr);
    ASSERT_EQ(ins->returning.size(), 1u);
    auto* ret_col = dynamic_cast<ColumnRefExpr*>(ins->returning[0].expr.get());
    ASSERT_NE(ret_col, nullptr);
    EXPECT_EQ(ret_col->column, "id");
}

TEST(Ast, DeleteWithReturning) {
    auto stmt = parse_one("DELETE FROM users WHERE id = 1 RETURNING *");
    ASSERT_NE(stmt, nullptr);
    auto* del = as<DeleteStmt>(stmt);
    ASSERT_NE(del, nullptr);
    ASSERT_EQ(del->returning.size(), 1u);
    EXPECT_TRUE(del->returning[0].is_star);
}

// -- MATCH statement test (parse-based) ---------------------------------------

TEST(Ast, MatchStmt) {
    auto stmt = parse_one("MATCH (a:users)-[r:knows]->(b:users) RETURN a.name");
    ASSERT_NE(stmt, nullptr);
    auto* match = as<MatchStmt>(stmt);
    ASSERT_NE(match, nullptr);
    ASSERT_EQ(match->pattern.size(), 2u);
    EXPECT_EQ(match->pattern[0].node.variable, "a");
    EXPECT_EQ(match->pattern[0].node.label, "users");
    ASSERT_TRUE(match->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(match->pattern[0].outgoing_edge->variable, "r");
    EXPECT_EQ(match->pattern[0].outgoing_edge->edge_type, "knows");
    EXPECT_EQ(match->pattern[1].node.variable, "b");
    EXPECT_FALSE(match->pattern[1].outgoing_edge.has_value());
    ASSERT_EQ(match->return_items.size(), 1u);
}
