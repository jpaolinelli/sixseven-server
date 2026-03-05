#include "sixseven/planner/rewrite_rules.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Helpers to build AST nodes
// ============================================================================

static ExprPtr make_int_lit(int64_t v) {
    auto lit = std::make_unique<LiteralExpr>();
    lit->kind = LiteralKind::INTEGER;
    lit->value = std::to_string(v);
    return lit;
}

static ExprPtr make_float_lit(double v) {
    auto lit = std::make_unique<LiteralExpr>();
    lit->kind = LiteralKind::FLOAT;
    lit->value = std::to_string(v);
    return lit;
}

static ExprPtr make_bool_lit(bool v) {
    auto lit = std::make_unique<LiteralExpr>();
    lit->kind = LiteralKind::BOOLEAN;
    lit->value = v ? "true" : "false";
    return lit;
}

static ExprPtr make_string_lit(const std::string& v) {
    auto lit = std::make_unique<LiteralExpr>();
    lit->kind = LiteralKind::STRING;
    lit->value = v;
    return lit;
}

static ExprPtr make_null_lit() {
    auto lit = std::make_unique<LiteralExpr>();
    lit->kind = LiteralKind::NULL_LITERAL;
    lit->value = "NULL";
    return lit;
}

static ExprPtr make_column_ref(const std::string& table, const std::string& column) {
    auto col = std::make_unique<ColumnRefExpr>();
    col->table = table;
    col->column = column;
    return col;
}

static ExprPtr make_binary(BinaryOp op, ExprPtr lhs, ExprPtr rhs) {
    auto bin = std::make_unique<BinaryExpr>();
    bin->op = op;
    bin->lhs = std::move(lhs);
    bin->rhs = std::move(rhs);
    return bin;
}

static ExprPtr make_unary(UnaryOp op, ExprPtr operand) {
    auto u = std::make_unique<UnaryExpr>();
    u->op = op;
    u->operand = std::move(operand);
    return u;
}

static const LiteralExpr* as_lit(const ExprPtr& expr) {
    return dynamic_cast<const LiteralExpr*>(expr.get());
}

// ============================================================================
// Constant folding tests
// ============================================================================

TEST(ConstantFolding, IntegerAdd) {
    auto expr = make_binary(BinaryOp::ADD, make_int_lit(3), make_int_lit(7));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    auto* lit = as_lit(result);
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::INTEGER);
    EXPECT_EQ(lit->value, "10");
}

TEST(ConstantFolding, IntegerSubtract) {
    auto expr = make_binary(BinaryOp::SUBTRACT, make_int_lit(10), make_int_lit(4));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    auto* lit = as_lit(result);
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::INTEGER);
    EXPECT_EQ(lit->value, "6");
}

TEST(ConstantFolding, IntegerMultiply) {
    auto expr = make_binary(BinaryOp::MULTIPLY, make_int_lit(5), make_int_lit(6));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    auto* lit = as_lit(result);
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->value, "30");
}

TEST(ConstantFolding, IntegerDivide) {
    auto expr = make_binary(BinaryOp::DIVIDE, make_int_lit(20), make_int_lit(4));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    auto* lit = as_lit(result);
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->value, "5");
}

TEST(ConstantFolding, IntegerDivideByZero) {
    auto expr = make_binary(BinaryOp::DIVIDE, make_int_lit(10), make_int_lit(0));
    auto result = fold_constants(*expr);
    EXPECT_EQ(result, nullptr);
}

TEST(ConstantFolding, IntegerModulo) {
    auto expr = make_binary(BinaryOp::MODULO, make_int_lit(17), make_int_lit(5));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    auto* lit = as_lit(result);
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->value, "2");
}

TEST(ConstantFolding, IntegerModuloByZero) {
    auto expr = make_binary(BinaryOp::MODULO, make_int_lit(10), make_int_lit(0));
    auto result = fold_constants(*expr);
    EXPECT_EQ(result, nullptr);
}

TEST(ConstantFolding, FloatAdd) {
    auto expr = make_binary(BinaryOp::ADD, make_float_lit(1.5), make_float_lit(2.5));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    auto* lit = as_lit(result);
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::FLOAT);
    EXPECT_DOUBLE_EQ(std::stod(lit->value), 4.0);
}

TEST(ConstantFolding, FloatDivideByZero) {
    auto expr = make_binary(BinaryOp::DIVIDE, make_float_lit(5.0), make_float_lit(0.0));
    auto result = fold_constants(*expr);
    EXPECT_EQ(result, nullptr);
}

TEST(ConstantFolding, MixedIntFloat) {
    // int + float should fold as float
    auto expr = make_binary(BinaryOp::ADD, make_int_lit(2), make_float_lit(3.5));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    auto* lit = as_lit(result);
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::FLOAT);
    EXPECT_DOUBLE_EQ(std::stod(lit->value), 5.5);
}

TEST(ConstantFolding, IntegerComparisons) {
    // 3 < 5 -> true
    auto expr1 = make_binary(BinaryOp::LESS, make_int_lit(3), make_int_lit(5));
    auto r1 = fold_constants(*expr1);
    ASSERT_NE(r1, nullptr);
    EXPECT_EQ(as_lit(r1)->value, "true");

    // 5 > 3 -> true
    auto expr2 = make_binary(BinaryOp::GREATER, make_int_lit(5), make_int_lit(3));
    auto r2 = fold_constants(*expr2);
    ASSERT_NE(r2, nullptr);
    EXPECT_EQ(as_lit(r2)->value, "true");

    // 5 == 5 -> true
    auto expr3 = make_binary(BinaryOp::EQUAL, make_int_lit(5), make_int_lit(5));
    auto r3 = fold_constants(*expr3);
    ASSERT_NE(r3, nullptr);
    EXPECT_EQ(as_lit(r3)->value, "true");

    // 5 != 5 -> false
    auto expr4 = make_binary(BinaryOp::NOT_EQUAL, make_int_lit(5), make_int_lit(5));
    auto r4 = fold_constants(*expr4);
    ASSERT_NE(r4, nullptr);
    EXPECT_EQ(as_lit(r4)->value, "false");

    // 3 <= 3 -> true
    auto expr5 = make_binary(BinaryOp::LESS_EQUAL, make_int_lit(3), make_int_lit(3));
    auto r5 = fold_constants(*expr5);
    ASSERT_NE(r5, nullptr);
    EXPECT_EQ(as_lit(r5)->value, "true");

    // 3 >= 4 -> false
    auto expr6 = make_binary(BinaryOp::GREATER_EQUAL, make_int_lit(3), make_int_lit(4));
    auto r6 = fold_constants(*expr6);
    ASSERT_NE(r6, nullptr);
    EXPECT_EQ(as_lit(r6)->value, "false");
}

TEST(ConstantFolding, StringConcat) {
    auto expr = make_binary(BinaryOp::CONCAT, make_string_lit("hello "), make_string_lit("world"));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    auto* lit = as_lit(result);
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::STRING);
    EXPECT_EQ(lit->value, "hello world");
}

TEST(ConstantFolding, NullComparison) {
    // NULL = 5 -> NULL
    auto expr = make_binary(BinaryOp::EQUAL, make_null_lit(), make_int_lit(5));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    auto* lit = as_lit(result);
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::NULL_LITERAL);
}

TEST(ConstantFolding, BooleanAndShortCircuit) {
    // false AND <anything> -> false
    auto expr1 = make_binary(BinaryOp::AND, make_bool_lit(false), make_column_ref("t", "x"));
    auto r1 = fold_constants(*expr1);
    ASSERT_NE(r1, nullptr);
    EXPECT_EQ(as_lit(r1)->value, "false");

    // <anything> AND false -> false
    auto expr2 = make_binary(BinaryOp::AND, make_column_ref("t", "x"), make_bool_lit(false));
    auto r2 = fold_constants(*expr2);
    ASSERT_NE(r2, nullptr);
    EXPECT_EQ(as_lit(r2)->value, "false");
}

TEST(ConstantFolding, BooleanAndTrueElimination) {
    // true AND col_ref -> col_ref (non-foldable operand)
    auto expr = make_binary(BinaryOp::AND, make_bool_lit(true), make_column_ref("t", "active"));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    auto* col = dynamic_cast<const ColumnRefExpr*>(result.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->table, "t");
    EXPECT_EQ(col->column, "active");

    // col_ref AND true -> col_ref
    auto expr2 = make_binary(BinaryOp::AND, make_column_ref("t", "x"), make_bool_lit(true));
    auto r2 = fold_constants(*expr2);
    ASSERT_NE(r2, nullptr);
    auto* col2 = dynamic_cast<const ColumnRefExpr*>(r2.get());
    ASSERT_NE(col2, nullptr);
    EXPECT_EQ(col2->column, "x");

    // true AND (1+2) -> 3 (foldable operand still works)
    auto expr3 = make_binary(BinaryOp::AND,
                             make_bool_lit(true),
                             make_binary(BinaryOp::ADD, make_int_lit(1), make_int_lit(2)));
    auto r3 = fold_constants(*expr3);
    ASSERT_NE(r3, nullptr);
    EXPECT_EQ(as_lit(r3)->value, "3");
}

TEST(ConstantFolding, BooleanOrShortCircuit) {
    // true OR <anything> -> true
    auto expr1 = make_binary(BinaryOp::OR, make_bool_lit(true), make_column_ref("t", "x"));
    auto r1 = fold_constants(*expr1);
    ASSERT_NE(r1, nullptr);
    EXPECT_EQ(as_lit(r1)->value, "true");
}

TEST(ConstantFolding, BooleanOrFalseElimination) {
    // false OR col_ref -> col_ref (non-foldable operand)
    auto expr = make_binary(BinaryOp::OR, make_bool_lit(false), make_column_ref("t", "active"));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    auto* col = dynamic_cast<const ColumnRefExpr*>(result.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "active");

    // col_ref OR false -> col_ref
    auto expr2 = make_binary(BinaryOp::OR, make_column_ref("t", "x"), make_bool_lit(false));
    auto r2 = fold_constants(*expr2);
    ASSERT_NE(r2, nullptr);
    auto* col2 = dynamic_cast<const ColumnRefExpr*>(r2.get());
    ASSERT_NE(col2, nullptr);
    EXPECT_EQ(col2->column, "x");

    // false OR (2+3) -> 5 (foldable operand still works)
    auto expr3 = make_binary(BinaryOp::OR,
                             make_bool_lit(false),
                             make_binary(BinaryOp::ADD, make_int_lit(2), make_int_lit(3)));
    auto r3 = fold_constants(*expr3);
    ASSERT_NE(r3, nullptr);
    EXPECT_EQ(as_lit(r3)->value, "5");
}

TEST(ConstantFolding, NotTrue) {
    auto expr = make_unary(UnaryOp::NOT, make_bool_lit(true));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(as_lit(result)->value, "false");
}

TEST(ConstantFolding, NotFalse) {
    auto expr = make_unary(UnaryOp::NOT, make_bool_lit(false));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(as_lit(result)->value, "true");
}

TEST(ConstantFolding, NegateInteger) {
    auto expr = make_unary(UnaryOp::NEGATE, make_int_lit(42));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    auto* lit = as_lit(result);
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::INTEGER);
    EXPECT_EQ(lit->value, "-42");
}

TEST(ConstantFolding, NegateFloat) {
    auto expr = make_unary(UnaryOp::NEGATE, make_float_lit(3.14));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    auto* lit = as_lit(result);
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::FLOAT);
    EXPECT_DOUBLE_EQ(std::stod(lit->value), -3.14);
}

TEST(ConstantFolding, NestedFolding) {
    // (2 + 3) * (10 - 4) -> 5 * 6 -> 30
    auto expr = make_binary(BinaryOp::MULTIPLY,
                            make_binary(BinaryOp::ADD, make_int_lit(2), make_int_lit(3)),
                            make_binary(BinaryOp::SUBTRACT, make_int_lit(10), make_int_lit(4)));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(as_lit(result)->value, "30");
}

TEST(ConstantFolding, NonConstantExprReturnsNull) {
    // Column ref cannot be folded.
    auto col = make_column_ref("t", "x");
    auto result = fold_constants(*col);
    EXPECT_EQ(result, nullptr);
}

// ============================================================================
// Predicate pushdown tests
// ============================================================================

TEST(PredicatePushdown, ExtractConjunctsSimple) {
    // a AND b AND c -> {a, b, c}
    auto a = make_column_ref("t", "a");
    auto b = make_column_ref("t", "b");
    auto c = make_column_ref("t", "c");

    auto* a_ptr = a.get();
    auto* b_ptr = b.get();
    auto* c_ptr = c.get();

    auto ab = make_binary(BinaryOp::AND, std::move(a), std::move(b));
    auto abc = make_binary(BinaryOp::AND, std::move(ab), std::move(c));

    auto conjuncts = extract_conjuncts(*abc);
    ASSERT_EQ(conjuncts.size(), 3u);
    EXPECT_EQ(conjuncts[0], a_ptr);
    EXPECT_EQ(conjuncts[1], b_ptr);
    EXPECT_EQ(conjuncts[2], c_ptr);
}

TEST(PredicatePushdown, ExtractConjunctsSingle) {
    auto col = make_column_ref("t", "x");
    auto* col_ptr = col.get();
    auto conjuncts = extract_conjuncts(*col);
    ASSERT_EQ(conjuncts.size(), 1u);
    EXPECT_EQ(conjuncts[0], col_ptr);
}

TEST(PredicatePushdown, ExtractConjunctsOrNotSplit) {
    // a OR b is NOT split — it's a single conjunct.
    auto expr = make_binary(BinaryOp::OR, make_column_ref("t", "a"), make_column_ref("t", "b"));
    auto conjuncts = extract_conjuncts(*expr);
    ASSERT_EQ(conjuncts.size(), 1u);
}

TEST(PredicatePushdown, ReferencedTablesColumn) {
    auto col = make_column_ref("orders", "amount");
    auto tables = referenced_tables(*col);
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_TRUE(tables.count("orders") > 0);
}

TEST(PredicatePushdown, ReferencedTablesBinaryExpr) {
    // orders.amount > customers.balance
    auto expr = make_binary(BinaryOp::GREATER,
                            make_column_ref("orders", "amount"),
                            make_column_ref("customers", "balance"));
    auto tables = referenced_tables(*expr);
    ASSERT_EQ(tables.size(), 2u);
    EXPECT_TRUE(tables.count("orders") > 0);
    EXPECT_TRUE(tables.count("customers") > 0);
}

TEST(PredicatePushdown, ReferencedTablesLiteral) {
    auto lit = make_int_lit(42);
    auto tables = referenced_tables(*lit);
    EXPECT_TRUE(tables.empty());
}

TEST(PredicatePushdown, IsSingleTablePredicate) {
    // orders.amount > 100 -> references only "orders"
    auto expr =
        make_binary(BinaryOp::GREATER, make_column_ref("orders", "amount"), make_int_lit(100));
    EXPECT_TRUE(is_single_table_predicate(*expr, "orders"));
    EXPECT_FALSE(is_single_table_predicate(*expr, "customers"));
}

TEST(PredicatePushdown, IsSingleTablePredicateNoTable) {
    // Literal-only predicate: is single-table for any table.
    auto expr = make_binary(BinaryOp::GREATER, make_int_lit(5), make_int_lit(3));
    EXPECT_TRUE(is_single_table_predicate(*expr, "anything"));
}

TEST(PredicatePushdown, IsJoinPredicate) {
    auto expr = make_binary(BinaryOp::EQUAL,
                            make_column_ref("orders", "customer_id"),
                            make_column_ref("customers", "id"));
    EXPECT_TRUE(is_join_predicate(*expr));
}

TEST(PredicatePushdown, IsNotJoinPredicate) {
    auto expr =
        make_binary(BinaryOp::GREATER, make_column_ref("orders", "amount"), make_int_lit(100));
    EXPECT_FALSE(is_join_predicate(*expr));
}

TEST(PredicatePushdown, ExtractJoinPredicates) {
    // Build: orders.customer_id = customers.id AND orders.amount > 100 AND customers.active = true
    auto join_pred = make_binary(BinaryOp::EQUAL,
                                 make_column_ref("orders", "customer_id"),
                                 make_column_ref("customers", "id"));
    auto table_pred1 =
        make_binary(BinaryOp::GREATER, make_column_ref("orders", "amount"), make_int_lit(100));
    auto table_pred2 =
        make_binary(BinaryOp::EQUAL, make_column_ref("customers", "active"), make_bool_lit(true));

    std::vector<const Expr*> conjuncts = {join_pred.get(), table_pred1.get(), table_pred2.get()};

    auto result = extract_join_predicates(conjuncts, "orders", "customers");
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], join_pred.get());
}

TEST(PredicatePushdown, ExtractTablePredicates) {
    auto join_pred = make_binary(BinaryOp::EQUAL,
                                 make_column_ref("orders", "customer_id"),
                                 make_column_ref("customers", "id"));
    auto table_pred1 =
        make_binary(BinaryOp::GREATER, make_column_ref("orders", "amount"), make_int_lit(100));
    auto table_pred2 =
        make_binary(BinaryOp::EQUAL, make_column_ref("customers", "active"), make_bool_lit(true));

    std::vector<const Expr*> conjuncts = {join_pred.get(), table_pred1.get(), table_pred2.get()};

    auto orders_preds = extract_table_predicates(conjuncts, "orders");
    ASSERT_EQ(orders_preds.size(), 1u);
    EXPECT_EQ(orders_preds[0], table_pred1.get());

    auto customer_preds = extract_table_predicates(conjuncts, "customers");
    ASSERT_EQ(customer_preds.size(), 1u);
    EXPECT_EQ(customer_preds[0], table_pred2.get());
}

// ============================================================================
// Projection pushdown tests
// ============================================================================

TEST(ProjectionPushdown, CollectColumnRefsSingleExpr) {
    auto expr = make_binary(BinaryOp::ADD, make_column_ref("t", "a"), make_column_ref("t", "b"));
    auto refs = collect_column_refs(*expr);
    ASSERT_EQ(refs.size(), 2u);
    EXPECT_EQ(refs[0].table_name, "t");
    EXPECT_EQ(refs[0].column_name, "a");
    EXPECT_EQ(refs[1].table_name, "t");
    EXPECT_EQ(refs[1].column_name, "b");
}

TEST(ProjectionPushdown, CollectColumnRefsFromList) {
    std::vector<ExprPtr> exprs;
    exprs.push_back(make_column_ref("t1", "x"));
    exprs.push_back(make_column_ref("t2", "y"));
    exprs.push_back(make_int_lit(42));

    auto refs = collect_column_refs(exprs);
    ASSERT_EQ(refs.size(), 2u);
    EXPECT_EQ(refs[0].table_name, "t1");
    EXPECT_EQ(refs[0].column_name, "x");
    EXPECT_EQ(refs[1].table_name, "t2");
    EXPECT_EQ(refs[1].column_name, "y");
}

TEST(ProjectionPushdown, NeededColumns) {
    std::vector<ColumnRef> refs = {
        {"orders", "id"},
        {"orders", "amount"},
        {"customers", "name"},
        {"", "status"}, // No table qualifier.
    };

    auto cols = needed_columns(refs, "orders");
    EXPECT_EQ(cols.size(), 3u); // id, amount, status (empty table matches)
    EXPECT_TRUE(cols.count("id") > 0);
    EXPECT_TRUE(cols.count("amount") > 0);
    EXPECT_TRUE(cols.count("status") > 0);

    auto cust_cols = needed_columns(refs, "customers");
    EXPECT_EQ(cust_cols.size(), 2u); // name + status (empty table matches)
    EXPECT_TRUE(cust_cols.count("name") > 0);
    EXPECT_TRUE(cust_cols.count("status") > 0);
}

// ============================================================================
// Boolean simplification tests
// ============================================================================

TEST(BooleanSimplification, DelegatesToFoldConstants) {
    // simplify_boolean is implemented as fold_constants
    auto expr = make_binary(BinaryOp::ADD, make_int_lit(1), make_int_lit(2));
    auto result = simplify_boolean(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(as_lit(result)->value, "3");
}

// ============================================================================
// Subquery decorrelation tests
// ============================================================================

TEST(SubqueryDecorrelation, IsCorrelatedSubquery) {
    // Expression references "outer" table which is in outer_tables set.
    auto expr = make_binary(
        BinaryOp::EQUAL, make_column_ref("outer", "id"), make_column_ref("inner", "ref"));
    std::unordered_set<std::string> outer_tables = {"outer"};
    EXPECT_TRUE(is_correlated_subquery(*expr, outer_tables));
}

TEST(SubqueryDecorrelation, IsNotCorrelatedSubquery) {
    auto expr = make_binary(
        BinaryOp::EQUAL, make_column_ref("inner1", "id"), make_column_ref("inner2", "ref"));
    std::unordered_set<std::string> outer_tables = {"outer"};
    EXPECT_FALSE(is_correlated_subquery(*expr, outer_tables));
}

TEST(SubqueryDecorrelation, ExistsToSemiJoin) {
    auto exists = std::make_unique<ExistsExpr>();
    exists->subquery = std::make_unique<SelectStmt>();
    auto jt = subquery_to_join_type(*exists);
    EXPECT_EQ(jt, JoinType::SEMI);
}

TEST(SubqueryDecorrelation, InToSemiJoin) {
    auto in_expr = std::make_unique<InExpr>();
    in_expr->expr = make_column_ref("t", "x");
    in_expr->negated = false;
    auto jt = subquery_to_join_type(*in_expr);
    EXPECT_EQ(jt, JoinType::SEMI);
}

TEST(SubqueryDecorrelation, NotExistsToAntiJoin) {
    auto exists = std::make_unique<ExistsExpr>();
    exists->subquery = std::make_unique<SelectStmt>();
    auto not_exists = make_unary(UnaryOp::NOT, std::move(exists));
    auto jt = subquery_to_join_type(*not_exists);
    EXPECT_EQ(jt, JoinType::ANTI);
}

TEST(SubqueryDecorrelation, NotInToAntiJoin) {
    auto in_expr = std::make_unique<InExpr>();
    in_expr->expr = make_column_ref("t", "x");
    in_expr->negated = true;
    auto jt = subquery_to_join_type(*in_expr);
    EXPECT_EQ(jt, JoinType::ANTI);
}

TEST(SubqueryDecorrelation, OtherExprToInnerJoin) {
    auto col = make_column_ref("t", "x");
    auto jt = subquery_to_join_type(*col);
    EXPECT_EQ(jt, JoinType::INNER);
}

// ============================================================================
// collect_table_refs with complex expression types
// ============================================================================

TEST(ReferencedTables, UnaryExpr) {
    auto expr = make_unary(
        UnaryOp::NOT,
        make_binary(BinaryOp::EQUAL, make_column_ref("t", "active"), make_bool_lit(true)));
    auto tables = referenced_tables(*expr);
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_TRUE(tables.count("t") > 0);
}

TEST(ReferencedTables, FunctionCallExpr) {
    auto func = std::make_unique<FunctionCallExpr>();
    func->name = "upper";
    func->args.push_back(make_column_ref("t", "name"));
    auto tables = referenced_tables(*func);
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_TRUE(tables.count("t") > 0);
}

TEST(ReferencedTables, BetweenExpr) {
    auto between = std::make_unique<BetweenExpr>();
    between->expr = make_column_ref("orders", "amount");
    between->low = make_int_lit(100);
    between->high = make_int_lit(500);
    between->negated = false;
    auto tables = referenced_tables(*between);
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_TRUE(tables.count("orders") > 0);
}

TEST(ReferencedTables, IsNullExpr) {
    auto is_null = std::make_unique<IsNullExpr>();
    is_null->expr = make_column_ref("t", "email");
    is_null->negated = false;
    auto tables = referenced_tables(*is_null);
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_TRUE(tables.count("t") > 0);
}

TEST(ReferencedTables, LikeExpr) {
    auto like = std::make_unique<LikeExpr>();
    like->expr = make_column_ref("t", "name");
    like->pattern = make_string_lit("%foo%");
    like->negated = false;
    auto tables = referenced_tables(*like);
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_TRUE(tables.count("t") > 0);
}

TEST(ReferencedTables, CastExpr) {
    auto cast = std::make_unique<CastExpr>();
    cast->expr = make_column_ref("t", "age");
    cast->target_type = TypeSpec{"FLOAT64", std::nullopt, std::nullopt, "", ""};
    auto tables = referenced_tables(*cast);
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_TRUE(tables.count("t") > 0);
}

TEST(ReferencedTables, InExpr) {
    auto in_expr = std::make_unique<InExpr>();
    in_expr->expr = make_column_ref("t", "status");
    in_expr->values.push_back(make_string_lit("active"));
    in_expr->values.push_back(make_string_lit("pending"));
    in_expr->negated = false;
    auto tables = referenced_tables(*in_expr);
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_TRUE(tables.count("t") > 0);
}

TEST(ReferencedTables, CaseExpr) {
    auto case_expr = std::make_unique<CaseExpr>();
    CaseWhen when1;
    when1.condition =
        make_binary(BinaryOp::GREATER, make_column_ref("t", "amount"), make_int_lit(100));
    when1.result = make_string_lit("high");
    case_expr->whens.push_back(std::move(when1));
    case_expr->else_expr = make_column_ref("t2", "default_label");

    auto tables = referenced_tables(*case_expr);
    ASSERT_EQ(tables.size(), 2u);
    EXPECT_TRUE(tables.count("t") > 0);
    EXPECT_TRUE(tables.count("t2") > 0);
}

// ============================================================================
// collect_column_refs with complex expression types
// ============================================================================

TEST(CollectColumnRefs, BetweenExpr) {
    auto between = std::make_unique<BetweenExpr>();
    between->expr = make_column_ref("t", "price");
    between->low = make_column_ref("t", "min_price");
    between->high = make_column_ref("t", "max_price");
    between->negated = false;
    auto refs = collect_column_refs(*between);
    ASSERT_EQ(refs.size(), 3u);
}

TEST(CollectColumnRefs, InExprValues) {
    auto in_expr = std::make_unique<InExpr>();
    in_expr->expr = make_column_ref("t", "id");
    in_expr->values.push_back(make_column_ref("t2", "ref_id"));
    in_expr->negated = false;
    auto refs = collect_column_refs(*in_expr);
    ASSERT_EQ(refs.size(), 2u);
    EXPECT_EQ(refs[0].column_name, "id");
    EXPECT_EQ(refs[1].column_name, "ref_id");
}

} // anonymous namespace
} // namespace sixseven
