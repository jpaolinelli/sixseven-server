/// @file test_qa_gdb_139.cpp
/// @brief QA adversarial tests for GDB-139: Rule-based query rewrites.
///
/// Targets edge cases: nested boolean folding, double NOT, negate zero,
/// integer overflow, empty string concat, NULL with all comparison ops,
/// deep AND chains, 3-table predicates, empty table qualifiers, reversed
/// join table names, constant-only predicates, clone edge cases, subquery
/// decorrelation with non-exists NOT operand.

#include "sixseven/planner/rewrite_rules.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace sixseven;

// =============================================================================
// Helpers (matching existing test patterns)
// =============================================================================

static ExprPtr mk_int(int64_t v) {
    auto lit = std::make_unique<LiteralExpr>();
    lit->kind = LiteralKind::INTEGER;
    lit->value = std::to_string(v);
    return lit;
}

static ExprPtr mk_float(double v) {
    auto lit = std::make_unique<LiteralExpr>();
    lit->kind = LiteralKind::FLOAT;
    lit->value = std::to_string(v);
    return lit;
}

static ExprPtr mk_bool(bool v) {
    auto lit = std::make_unique<LiteralExpr>();
    lit->kind = LiteralKind::BOOLEAN;
    lit->value = v ? "true" : "false";
    return lit;
}

static ExprPtr mk_str(const std::string& v) {
    auto lit = std::make_unique<LiteralExpr>();
    lit->kind = LiteralKind::STRING;
    lit->value = v;
    return lit;
}

static ExprPtr mk_null() {
    auto lit = std::make_unique<LiteralExpr>();
    lit->kind = LiteralKind::NULL_LITERAL;
    lit->value = "NULL";
    return lit;
}

static ExprPtr mk_col(const std::string& table, const std::string& column) {
    auto col = std::make_unique<ColumnRefExpr>();
    col->table = table;
    col->column = column;
    return col;
}

static ExprPtr mk_bin(BinaryOp op, ExprPtr lhs, ExprPtr rhs) {
    auto bin = std::make_unique<BinaryExpr>();
    bin->op = op;
    bin->lhs = std::move(lhs);
    bin->rhs = std::move(rhs);
    return bin;
}

static ExprPtr mk_un(UnaryOp op, ExprPtr operand) {
    auto u = std::make_unique<UnaryExpr>();
    u->op = op;
    u->operand = std::move(operand);
    return u;
}

static const LiteralExpr* as_lit(const ExprPtr& e) {
    return dynamic_cast<const LiteralExpr*>(e.get());
}

// =============================================================================
// Constant folding adversarial tests
// =============================================================================

TEST(QA139_Folding, NegateZeroInt) {
    auto expr = mk_un(UnaryOp::NEGATE, mk_int(0));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(as_lit(result)->value, "0");
}

TEST(QA139_Folding, NegateZeroFloat) {
    auto expr = mk_un(UnaryOp::NEGATE, mk_float(0.0));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    // -0.0 may render as "0.000000" or "-0.000000" depending on std::to_string.
    double val = std::stod(as_lit(result)->value);
    EXPECT_DOUBLE_EQ(val, 0.0);
}

TEST(QA139_Folding, DoubleNotTrue) {
    // NOT NOT true -> true
    auto expr = mk_un(UnaryOp::NOT, mk_un(UnaryOp::NOT, mk_bool(true)));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(as_lit(result)->value, "true");
}

TEST(QA139_Folding, DoubleNotFalse) {
    // NOT NOT false -> false
    auto expr = mk_un(UnaryOp::NOT, mk_un(UnaryOp::NOT, mk_bool(false)));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(as_lit(result)->value, "false");
}

TEST(QA139_Folding, EmptyStringConcat) {
    auto expr = mk_bin(BinaryOp::CONCAT, mk_str(""), mk_str(""));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(as_lit(result)->value, "");
}

TEST(QA139_Folding, StringConcatLeftEmpty) {
    auto expr = mk_bin(BinaryOp::CONCAT, mk_str(""), mk_str("hello"));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(as_lit(result)->value, "hello");
}

TEST(QA139_Folding, NullLessComparison) {
    auto expr = mk_bin(BinaryOp::LESS, mk_int(5), mk_null());
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(as_lit(result)->kind, LiteralKind::NULL_LITERAL);
}

TEST(QA139_Folding, NullGreaterEqualComparison) {
    auto expr = mk_bin(BinaryOp::GREATER_EQUAL, mk_null(), mk_null());
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(as_lit(result)->kind, LiteralKind::NULL_LITERAL);
}

TEST(QA139_Folding, NullNotEqualComparison) {
    auto expr = mk_bin(BinaryOp::NOT_EQUAL, mk_null(), mk_int(10));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(as_lit(result)->kind, LiteralKind::NULL_LITERAL);
}

TEST(QA139_Folding, AndOfTwoTrue) {
    auto expr = mk_bin(BinaryOp::AND, mk_bool(true), mk_bool(true));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(as_lit(result)->value, "true");
}

TEST(QA139_Folding, OrOfTwoFalse) {
    auto expr = mk_bin(BinaryOp::OR, mk_bool(false), mk_bool(false));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(as_lit(result)->value, "false");
}

TEST(QA139_Folding, FoldIntMultiplyByZero) {
    auto expr = mk_bin(BinaryOp::MULTIPLY, mk_int(99999), mk_int(0));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(as_lit(result)->value, "0");
}

TEST(QA139_Folding, DeeplyNestedArithmetic) {
    // ((1 + 2) + (3 + 4)) + ((5 + 6) + (7 + 8)) = 36
    auto a = mk_bin(BinaryOp::ADD, mk_int(1), mk_int(2));
    auto b = mk_bin(BinaryOp::ADD, mk_int(3), mk_int(4));
    auto c = mk_bin(BinaryOp::ADD, mk_int(5), mk_int(6));
    auto d = mk_bin(BinaryOp::ADD, mk_int(7), mk_int(8));
    auto ab = mk_bin(BinaryOp::ADD, std::move(a), std::move(b));
    auto cd = mk_bin(BinaryOp::ADD, std::move(c), std::move(d));
    auto expr = mk_bin(BinaryOp::ADD, std::move(ab), std::move(cd));

    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(as_lit(result)->value, "36");
}

TEST(QA139_Folding, PartialFoldBinaryWithOneConstant) {
    // col + 0: LHS is non-constant, so cannot fold.
    auto expr = mk_bin(BinaryOp::ADD, mk_col("t", "x"), mk_int(0));
    auto result = fold_constants(*expr);
    // Cannot fold: one operand is a column reference.
    EXPECT_EQ(result, nullptr);
}

TEST(QA139_Folding, NotOfNonBoolean) {
    // NOT of an integer literal — NOT doesn't apply to integers.
    auto expr = mk_un(UnaryOp::NOT, mk_int(42));
    auto result = fold_constants(*expr);
    EXPECT_EQ(result, nullptr);
}

TEST(QA139_Folding, NegateOfNonNumeric) {
    // NEGATE of a string — shouldn't fold.
    auto expr = mk_un(UnaryOp::NEGATE, mk_str("hello"));
    auto result = fold_constants(*expr);
    EXPECT_EQ(result, nullptr);
}

TEST(QA139_Folding, FloatSubtract) {
    auto expr = mk_bin(BinaryOp::SUBTRACT, mk_float(10.5), mk_float(3.2));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_NEAR(std::stod(as_lit(result)->value), 7.3, 0.001);
}

TEST(QA139_Folding, FloatMultiply) {
    auto expr = mk_bin(BinaryOp::MULTIPLY, mk_float(2.5), mk_float(4.0));
    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_DOUBLE_EQ(std::stod(as_lit(result)->value), 10.0);
}

// =============================================================================
// Predicate pushdown adversarial tests
// =============================================================================

TEST(QA139_Predicate, DeepAndChainExtraction) {
    // a AND b AND c AND d AND e -> 5 conjuncts.
    auto a = mk_col("t", "a");
    auto b = mk_col("t", "b");
    auto c = mk_col("t", "c");
    auto d = mk_col("t", "d");
    auto e = mk_col("t", "e");

    auto ab = mk_bin(BinaryOp::AND, std::move(a), std::move(b));
    auto abc = mk_bin(BinaryOp::AND, std::move(ab), std::move(c));
    auto abcd = mk_bin(BinaryOp::AND, std::move(abc), std::move(d));
    auto abcde = mk_bin(BinaryOp::AND, std::move(abcd), std::move(e));

    auto conjuncts = extract_conjuncts(*abcde);
    EXPECT_EQ(conjuncts.size(), 5u);
}

TEST(QA139_Predicate, ColumnRefNoTableName) {
    auto col = mk_col("", "x");
    auto tables = referenced_tables(*col);
    EXPECT_TRUE(tables.empty());
}

TEST(QA139_Predicate, ThreeTablePredicateNotJoin) {
    // a.x = b.y + c.z references 3 tables: not a single-table or 2-table join.
    auto expr = mk_bin(BinaryOp::EQUAL,
                       mk_col("a", "x"),
                       mk_bin(BinaryOp::ADD, mk_col("b", "y"), mk_col("c", "z")));
    EXPECT_FALSE(is_join_predicate(*expr)); // 3 tables.
    EXPECT_FALSE(is_single_table_predicate(*expr, "a"));
}

TEST(QA139_Predicate, JoinPredicateReversedTableOrder) {
    // customers.id = orders.customer_id
    auto pred = mk_bin(BinaryOp::EQUAL, mk_col("customers", "id"), mk_col("orders", "customer_id"));
    std::vector<const Expr*> conjuncts = {pred.get()};

    // Should still be found when queried with (orders, customers).
    auto result = extract_join_predicates(conjuncts, "orders", "customers");
    EXPECT_EQ(result.size(), 1u);
}

TEST(QA139_Predicate, ConstantOnlyPredicateMatches) {
    // 5 > 3: no table references — should be single-table for any table.
    auto pred = mk_bin(BinaryOp::GREATER, mk_int(5), mk_int(3));
    std::vector<const Expr*> conjuncts = {pred.get()};

    auto result = extract_table_predicates(conjuncts, "any_table");
    EXPECT_EQ(result.size(), 1u);
}

TEST(QA139_Predicate, EmptyConjunctsList) {
    std::vector<const Expr*> conjuncts;

    auto join_preds = extract_join_predicates(conjuncts, "a", "b");
    EXPECT_TRUE(join_preds.empty());

    auto table_preds = extract_table_predicates(conjuncts, "a");
    EXPECT_TRUE(table_preds.empty());
}

TEST(QA139_Predicate, SingleTablePredicateWithExtraTables) {
    // a.x = 5 is single-table for "a" but not for "b".
    auto pred = mk_bin(BinaryOp::EQUAL, mk_col("a", "x"), mk_int(5));
    EXPECT_TRUE(is_single_table_predicate(*pred, "a"));
    EXPECT_FALSE(is_single_table_predicate(*pred, "b"));
}

TEST(QA139_Predicate, LiteralOnlyIsSingleTable) {
    auto lit = mk_int(42);
    EXPECT_TRUE(is_single_table_predicate(*lit, "anything"));
    EXPECT_FALSE(is_join_predicate(*lit));
}

// =============================================================================
// Projection pushdown adversarial tests
// =============================================================================

TEST(QA139_Projection, EmptyExprList) {
    std::vector<ExprPtr> exprs;
    auto refs = collect_column_refs(exprs);
    EXPECT_TRUE(refs.empty());
}

TEST(QA139_Projection, LiteralOnlyNoRefs) {
    auto refs = collect_column_refs(*mk_int(42));
    EXPECT_TRUE(refs.empty());
}

TEST(QA139_Projection, NeededColumnsEmptyRefs) {
    std::vector<ColumnRef> refs;
    auto cols = needed_columns(refs, "t");
    EXPECT_TRUE(cols.empty());
}

TEST(QA139_Projection, NeededColumnsDeduplication) {
    // Same column referenced twice — should deduplicate in the set.
    std::vector<ColumnRef> refs = {{"t", "x"}, {"t", "x"}, {"t", "y"}};
    auto cols = needed_columns(refs, "t");
    EXPECT_EQ(cols.size(), 2u);
    EXPECT_TRUE(cols.count("x") > 0);
    EXPECT_TRUE(cols.count("y") > 0);
}

TEST(QA139_Projection, NeededColumnsNoMatch) {
    std::vector<ColumnRef> refs = {{"a", "x"}, {"b", "y"}};
    auto cols = needed_columns(refs, "c");
    EXPECT_TRUE(cols.empty());
}

TEST(QA139_Projection, CollectRefsFromUnary) {
    auto expr = mk_un(UnaryOp::NOT, mk_col("t", "active"));
    auto refs = collect_column_refs(*expr);
    ASSERT_EQ(refs.size(), 1u);
    EXPECT_EQ(refs[0].column_name, "active");
}

TEST(QA139_Projection, CollectRefsFromFunction) {
    auto func = std::make_unique<FunctionCallExpr>();
    func->name = "lower";
    func->args.push_back(mk_col("t", "name"));
    func->args.push_back(mk_col("t2", "locale"));

    auto refs = collect_column_refs(*func);
    ASSERT_EQ(refs.size(), 2u);
}

TEST(QA139_Projection, CollectRefsFromIsNull) {
    auto is_null = std::make_unique<IsNullExpr>();
    is_null->expr = mk_col("t", "email");
    is_null->negated = false;
    auto refs = collect_column_refs(*is_null);
    ASSERT_EQ(refs.size(), 1u);
    EXPECT_EQ(refs[0].column_name, "email");
}

TEST(QA139_Projection, CollectRefsFromLike) {
    auto like = std::make_unique<LikeExpr>();
    like->expr = mk_col("t", "name");
    like->pattern = mk_str("%foo%");
    like->negated = false;
    auto refs = collect_column_refs(*like);
    ASSERT_EQ(refs.size(), 1u);
    EXPECT_EQ(refs[0].column_name, "name");
}

TEST(QA139_Projection, CollectRefsFromCast) {
    auto cast = std::make_unique<CastExpr>();
    cast->expr = mk_col("t", "age");
    cast->target_type = TypeSpec{"INT32", std::nullopt, std::nullopt, "", ""};
    auto refs = collect_column_refs(*cast);
    ASSERT_EQ(refs.size(), 1u);
    EXPECT_EQ(refs[0].column_name, "age");
}

// =============================================================================
// Subquery decorrelation adversarial tests
// =============================================================================

TEST(QA139_Subquery, NotOfNonExistsIsInner) {
    // NOT of a column ref — not EXISTS, not IN.
    auto expr = mk_un(UnaryOp::NOT, mk_col("t", "active"));
    auto jt = subquery_to_join_type(*expr);
    EXPECT_EQ(jt, JoinType::INNER);
}

TEST(QA139_Subquery, CorrelatedWithMultipleOuterTables) {
    auto expr = mk_bin(BinaryOp::EQUAL, mk_col("outer1", "id"), mk_col("inner", "ref"));
    std::unordered_set<std::string> outer_tables = {"outer1", "outer2"};
    EXPECT_TRUE(is_correlated_subquery(*expr, outer_tables));
}

TEST(QA139_Subquery, NotCorrelatedEmptyOuterTables) {
    auto expr = mk_col("t", "x");
    std::unordered_set<std::string> outer_tables;
    EXPECT_FALSE(is_correlated_subquery(*expr, outer_tables));
}

TEST(QA139_Subquery, LiteralNotCorrelated) {
    auto expr = mk_int(42);
    std::unordered_set<std::string> outer_tables = {"outer"};
    EXPECT_FALSE(is_correlated_subquery(*expr, outer_tables));
}

// =============================================================================
// Boolean simplification edge cases
// =============================================================================

TEST(QA139_Boolean, SimplifyBooleanDelegatesToFold) {
    auto expr = mk_bin(BinaryOp::AND, mk_bool(true), mk_bool(false));
    auto result = simplify_boolean(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(as_lit(result)->value, "false");
}

TEST(QA139_Boolean, OrChainWithTrueShortCircuits) {
    // false OR false OR true OR col -> true
    auto inner = mk_bin(BinaryOp::OR, mk_bool(false), mk_bool(false));
    auto mid = mk_bin(BinaryOp::OR, std::move(inner), mk_bool(true));
    auto expr = mk_bin(BinaryOp::OR, std::move(mid), mk_col("t", "x"));

    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(as_lit(result)->value, "true");
}

TEST(QA139_Boolean, AndChainWithFalseShortCircuits) {
    // true AND true AND false AND col -> false
    auto inner = mk_bin(BinaryOp::AND, mk_bool(true), mk_bool(true));
    auto mid = mk_bin(BinaryOp::AND, std::move(inner), mk_bool(false));
    auto expr = mk_bin(BinaryOp::AND, std::move(mid), mk_col("t", "x"));

    auto result = fold_constants(*expr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(as_lit(result)->value, "false");
}

// =============================================================================
// referenced_tables with complex nested expressions
// =============================================================================

TEST(QA139_ReferencedTables, NestedBinaryExpression) {
    // (a.x + b.y) > (c.z * d.w) — references 4 tables.
    auto expr = mk_bin(BinaryOp::GREATER,
                       mk_bin(BinaryOp::ADD, mk_col("a", "x"), mk_col("b", "y")),
                       mk_bin(BinaryOp::MULTIPLY, mk_col("c", "z"), mk_col("d", "w")));
    auto tables = referenced_tables(*expr);
    EXPECT_EQ(tables.size(), 4u);
    EXPECT_TRUE(tables.count("a") > 0);
    EXPECT_TRUE(tables.count("b") > 0);
    EXPECT_TRUE(tables.count("c") > 0);
    EXPECT_TRUE(tables.count("d") > 0);
}

TEST(QA139_ReferencedTables, DuplicateTableRefsCounted) {
    // a.x = a.y — references 1 table, not 2.
    auto expr = mk_bin(BinaryOp::EQUAL, mk_col("a", "x"), mk_col("a", "y"));
    auto tables = referenced_tables(*expr);
    EXPECT_EQ(tables.size(), 1u);
    EXPECT_TRUE(tables.count("a") > 0);
}
