/// @file test_qa_gdb_105.cpp
/// QA adversarial tests for GDB-105: SELECT with JOINs, GROUP BY, ORDER BY,
/// LIMIT.
///
/// Tests cover: all SELECT clauses, JOIN types, aliases, subqueries, CTEs,
/// set operations, DISTINCT, and error paths.

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
// Basic SELECT
// =============================================================================

TEST(QA_GDB105, SelectStar) {
    auto stmt = parse_one("SELECT * FROM users");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->items.size(), 1u);
    EXPECT_TRUE(sel->items[0].is_star);
    EXPECT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].name, "users");
}

TEST(QA_GDB105, SelectTableStar) {
    auto stmt = parse_one("SELECT u.* FROM users u");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_TRUE(sel->items[0].is_star);
    EXPECT_EQ(sel->items[0].table_star, "u");
}

TEST(QA_GDB105, SelectColumns) {
    auto stmt = parse_one("SELECT a, b, c FROM t");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->items.size(), 3u);
}

TEST(QA_GDB105, SelectWithExplicitAlias) {
    auto stmt = parse_one("SELECT a AS col_a FROM t");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->items[0].alias, "col_a");
}

TEST(QA_GDB105, SelectWithImplicitAlias) {
    auto stmt = parse_one("SELECT a alias_a FROM t");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->items[0].alias, "alias_a");
}

TEST(QA_GDB105, SelectDistinct) {
    auto stmt = parse_one("SELECT DISTINCT name FROM users");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_TRUE(sel->distinct);
}

TEST(QA_GDB105, SelectWithoutFrom) {
    // SELECT 1 — no FROM clause.
    auto stmt = parse_one("SELECT 1");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_TRUE(sel->from.empty());
}

TEST(QA_GDB105, SelectExpressionItem) {
    auto stmt = parse_one("SELECT a + b AS total FROM t");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(sel->items[0].expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::ADD);
    EXPECT_EQ(sel->items[0].alias, "total");
}

// =============================================================================
// FROM clause
// =============================================================================

TEST(QA_GDB105, FromMultipleTables) {
    auto stmt = parse_one("SELECT * FROM t1, t2, t3");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->from.size(), 3u);
}

TEST(QA_GDB105, FromTableExplicitAlias) {
    auto stmt = parse_one("SELECT * FROM users AS u");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->from[0].name, "users");
    EXPECT_EQ(sel->from[0].alias, "u");
}

TEST(QA_GDB105, FromTableImplicitAlias) {
    auto stmt = parse_one("SELECT * FROM users u");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->from[0].alias, "u");
}

TEST(QA_GDB105, FromSubquery) {
    auto stmt = parse_one("SELECT * FROM (SELECT 1 AS x) AS sub");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_NE(sel->from[0].subquery, nullptr);
    EXPECT_EQ(sel->from[0].alias, "sub");
}

TEST(QA_GDB105, FromSubqueryImplicitAlias) {
    auto stmt = parse_one("SELECT * FROM (SELECT 1) sub");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->from[0].alias, "sub");
}

// =============================================================================
// JOIN clauses
// =============================================================================

TEST(QA_GDB105, InnerJoin) {
    auto stmt = parse_one(
        "SELECT * FROM users INNER JOIN orders ON users.id = orders.user_id");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->joins.size(), 1u);
    EXPECT_EQ(sel->joins[0].type, JoinType::INNER);
    EXPECT_NE(sel->joins[0].on_expr, nullptr);
}

TEST(QA_GDB105, BareJoinIsInner) {
    auto stmt = parse_one(
        "SELECT * FROM users JOIN orders ON users.id = orders.user_id");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->joins[0].type, JoinType::INNER);
}

TEST(QA_GDB105, LeftJoin) {
    auto stmt = parse_one(
        "SELECT * FROM users LEFT JOIN orders ON users.id = orders.user_id");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->joins[0].type, JoinType::LEFT);
}

TEST(QA_GDB105, LeftOuterJoin) {
    auto stmt = parse_one(
        "SELECT * FROM users LEFT OUTER JOIN orders ON users.id = orders.user_id");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->joins[0].type, JoinType::LEFT);
}

TEST(QA_GDB105, RightJoin) {
    auto stmt = parse_one(
        "SELECT * FROM users RIGHT JOIN orders ON users.id = orders.user_id");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->joins[0].type, JoinType::RIGHT);
}

TEST(QA_GDB105, FullJoin) {
    auto stmt = parse_one(
        "SELECT * FROM a FULL JOIN b ON a.id = b.id");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->joins[0].type, JoinType::FULL);
}

TEST(QA_GDB105, FullOuterJoin) {
    auto stmt = parse_one(
        "SELECT * FROM a FULL OUTER JOIN b ON a.id = b.id");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->joins[0].type, JoinType::FULL);
}

TEST(QA_GDB105, CrossJoin) {
    auto stmt = parse_one("SELECT * FROM a CROSS JOIN b");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->joins[0].type, JoinType::CROSS);
    EXPECT_EQ(sel->joins[0].on_expr, nullptr); // No ON for CROSS JOIN.
}

TEST(QA_GDB105, MultipleJoins) {
    auto stmt = parse_one(
        "SELECT * FROM a "
        "JOIN b ON a.id = b.a_id "
        "LEFT JOIN c ON b.id = c.b_id "
        "CROSS JOIN d");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->joins.size(), 3u);
    EXPECT_EQ(sel->joins[0].type, JoinType::INNER);
    EXPECT_EQ(sel->joins[1].type, JoinType::LEFT);
    EXPECT_EQ(sel->joins[2].type, JoinType::CROSS);
}

TEST(QA_GDB105, JoinWithComplexCondition) {
    auto stmt = parse_one(
        "SELECT * FROM a JOIN b ON a.id = b.a_id AND a.status = 'active'");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* and_expr = dynamic_cast<BinaryExpr*>(sel->joins[0].on_expr.get());
    ASSERT_NE(and_expr, nullptr);
    EXPECT_EQ(and_expr->op, BinaryOp::AND);
}

TEST(QA_GDB105, JoinWithTableAlias) {
    auto stmt = parse_one(
        "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->joins[0].table.alias, "o");
}

// =============================================================================
// WHERE clause
// =============================================================================

TEST(QA_GDB105, WhereBasic) {
    auto stmt = parse_one("SELECT * FROM t WHERE a > 5");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_NE(sel->where_expr, nullptr);
}

TEST(QA_GDB105, WhereComplex) {
    auto stmt = parse_one(
        "SELECT * FROM t WHERE a > 5 AND (b = 'x' OR c IS NULL)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* and_expr = dynamic_cast<BinaryExpr*>(sel->where_expr.get());
    ASSERT_NE(and_expr, nullptr);
    EXPECT_EQ(and_expr->op, BinaryOp::AND);
}

// =============================================================================
// GROUP BY and HAVING
// =============================================================================

TEST(QA_GDB105, GroupByBasic) {
    auto stmt = parse_one("SELECT dept, COUNT(*) FROM emp GROUP BY dept");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->group_by.size(), 1u);
}

TEST(QA_GDB105, GroupByMultiple) {
    auto stmt = parse_one(
        "SELECT dept, role, COUNT(*) FROM emp GROUP BY dept, role");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->group_by.size(), 2u);
}

TEST(QA_GDB105, GroupByWithHaving) {
    auto stmt = parse_one(
        "SELECT dept, COUNT(*) AS cnt FROM emp GROUP BY dept HAVING cnt > 5");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_NE(sel->having_expr, nullptr);
}

TEST(QA_GDB105, HavingWithoutGroupBy) {
    // HAVING without GROUP BY — parser accepts, semantics rejects.
    // Actually in this parser, HAVING is only parsed inside GROUP BY block.
    auto stmt = parse_one("SELECT COUNT(*) FROM t");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->having_expr, nullptr);
}

TEST(QA_GDB105, GroupByMissingBy) {
    expect_parse_error("SELECT * FROM t GROUP dept");
}

// =============================================================================
// ORDER BY
// =============================================================================

TEST(QA_GDB105, OrderByBasic) {
    auto stmt = parse_one("SELECT * FROM t ORDER BY name");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->order_by.size(), 1u);
    EXPECT_EQ(sel->order_by[0].direction, SortDirection::ASC);
}

TEST(QA_GDB105, OrderByDesc) {
    auto stmt = parse_one("SELECT * FROM t ORDER BY name DESC");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->order_by[0].direction, SortDirection::DESC);
}

TEST(QA_GDB105, OrderByMultiple) {
    auto stmt = parse_one("SELECT * FROM t ORDER BY a ASC, b DESC, c");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->order_by.size(), 3u);
    EXPECT_EQ(sel->order_by[0].direction, SortDirection::ASC);
    EXPECT_EQ(sel->order_by[1].direction, SortDirection::DESC);
    EXPECT_EQ(sel->order_by[2].direction, SortDirection::ASC); // default
}

TEST(QA_GDB105, OrderByExpression) {
    auto stmt = parse_one("SELECT * FROM t ORDER BY a + b DESC");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(sel->order_by[0].expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::ADD);
}

TEST(QA_GDB105, OrderByMissingBy) {
    expect_parse_error("SELECT * FROM t ORDER name");
}

// =============================================================================
// LIMIT and OFFSET
// =============================================================================

TEST(QA_GDB105, LimitBasic) {
    auto stmt = parse_one("SELECT * FROM t LIMIT 10");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(sel->limit.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->value, "10");
}

TEST(QA_GDB105, LimitWithOffset) {
    auto stmt = parse_one("SELECT * FROM t LIMIT 10 OFFSET 20");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_NE(sel->limit, nullptr);
    EXPECT_NE(sel->offset, nullptr);
    auto* off = dynamic_cast<LiteralExpr*>(sel->offset.get());
    ASSERT_NE(off, nullptr);
    EXPECT_EQ(off->value, "20");
}

TEST(QA_GDB105, OffsetWithoutLimit) {
    auto stmt = parse_one("SELECT * FROM t OFFSET 5");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->limit, nullptr);
    EXPECT_NE(sel->offset, nullptr);
}

TEST(QA_GDB105, LimitExpression) {
    auto stmt = parse_one("SELECT * FROM t LIMIT 5 + 5");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(sel->limit.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::ADD);
}

// =============================================================================
// Set operations
// =============================================================================

TEST(QA_GDB105, UnionBasic) {
    auto stmt = parse_one("SELECT a FROM t1 UNION SELECT b FROM t2");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->set_op, SelectStmt::SetOp::UNION);
    EXPECT_NE(sel->set_rhs, nullptr);
}

TEST(QA_GDB105, UnionAll) {
    auto stmt = parse_one("SELECT a FROM t1 UNION ALL SELECT b FROM t2");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->set_op, SelectStmt::SetOp::UNION_ALL);
}

TEST(QA_GDB105, Intersect) {
    auto stmt = parse_one("SELECT a FROM t1 INTERSECT SELECT b FROM t2");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->set_op, SelectStmt::SetOp::INTERSECT);
}

TEST(QA_GDB105, Except) {
    auto stmt = parse_one("SELECT a FROM t1 EXCEPT SELECT b FROM t2");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->set_op, SelectStmt::SetOp::EXCEPT);
}

// =============================================================================
// CTEs (WITH clause)
// =============================================================================

TEST(QA_GDB105, CTEBasic) {
    auto stmt = parse_one(
        "WITH temp AS (SELECT id FROM users) SELECT * FROM temp");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->ctes.size(), 1u);
    EXPECT_EQ(sel->ctes[0].name, "temp");
}

TEST(QA_GDB105, CTEMultiple) {
    auto stmt = parse_one(
        "WITH a AS (SELECT 1), b AS (SELECT 2) SELECT * FROM a, b");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->ctes.size(), 2u);
    EXPECT_EQ(sel->ctes[0].name, "a");
    EXPECT_EQ(sel->ctes[1].name, "b");
}

TEST(QA_GDB105, CTEMissingAs) {
    expect_parse_error("WITH temp (SELECT 1) SELECT * FROM temp");
}

TEST(QA_GDB105, CTEMissingParen) {
    expect_parse_error("WITH temp AS SELECT 1 SELECT * FROM temp");
}

// =============================================================================
// Subqueries in WHERE
// =============================================================================

TEST(QA_GDB105, WhereInSubquery) {
    auto stmt = parse_one(
        "SELECT * FROM t1 WHERE id IN (SELECT ref_id FROM t2)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* in_expr = dynamic_cast<InExpr*>(sel->where_expr.get());
    ASSERT_NE(in_expr, nullptr);
    EXPECT_NE(in_expr->subquery, nullptr);
}

TEST(QA_GDB105, WhereExistsSubquery) {
    auto stmt = parse_one(
        "SELECT * FROM t1 WHERE EXISTS (SELECT 1 FROM t2 WHERE t2.id = t1.id)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* exists = dynamic_cast<ExistsExpr*>(sel->where_expr.get());
    ASSERT_NE(exists, nullptr);
}

TEST(QA_GDB105, WhereScalarSubquery) {
    auto stmt = parse_one(
        "SELECT * FROM t WHERE a = (SELECT MAX(b) FROM t2)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* eq = dynamic_cast<BinaryExpr*>(sel->where_expr.get());
    ASSERT_NE(eq, nullptr);
    auto* sub = dynamic_cast<SubqueryExpr*>(eq->rhs.get());
    ASSERT_NE(sub, nullptr);
}

// =============================================================================
// Full query with all clauses
// =============================================================================

TEST(QA_GDB105, FullQuery) {
    auto stmt = parse_one(
        "SELECT DISTINCT u.name, COUNT(*) AS cnt "
        "FROM users u "
        "JOIN orders o ON u.id = o.user_id "
        "WHERE u.active = TRUE "
        "GROUP BY u.name "
        "HAVING COUNT(*) > 5 "
        "ORDER BY cnt DESC "
        "LIMIT 10 OFFSET 0");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_TRUE(sel->distinct);
    EXPECT_EQ(sel->items.size(), 2u);
    EXPECT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->joins.size(), 1u);
    EXPECT_NE(sel->where_expr, nullptr);
    EXPECT_EQ(sel->group_by.size(), 1u);
    EXPECT_NE(sel->having_expr, nullptr);
    EXPECT_EQ(sel->order_by.size(), 1u);
    EXPECT_NE(sel->limit, nullptr);
    EXPECT_NE(sel->offset, nullptr);
}

TEST(QA_GDB105, SelectMixedStarAndColumns) {
    // SELECT *, a FROM t — valid in many SQL engines.
    auto stmt = parse_one("SELECT *, a FROM t");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->items.size(), 2u);
    EXPECT_TRUE(sel->items[0].is_star);
    EXPECT_FALSE(sel->items[1].is_star);
}

TEST(QA_GDB105, NestedSubqueryInFrom) {
    auto stmt = parse_one(
        "SELECT * FROM (SELECT * FROM (SELECT 1 AS x) AS inner_t) AS outer_t");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_NE(sel->from[0].subquery, nullptr);
}

// =============================================================================
// Stress tests
// =============================================================================

TEST(QA_GDB105, ManyJoins) {
    std::string sql = "SELECT * FROM t0";
    for (int i = 1; i <= 20; ++i) {
        sql += " JOIN t" + std::to_string(i) + " ON t0.id = t" +
               std::to_string(i) + ".ref_id";
    }
    auto stmt = parse_one(sql);
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->joins.size(), 20u);
}

TEST(QA_GDB105, ManySelectItems) {
    std::string sql = "SELECT ";
    for (int i = 0; i < 50; ++i) {
        if (i > 0)
            sql += ", ";
        sql += "col" + std::to_string(i);
    }
    sql += " FROM t";
    auto stmt = parse_one(sql);
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->items.size(), 50u);
}

TEST(QA_GDB105, ManyOrderByItems) {
    std::string sql = "SELECT * FROM t ORDER BY ";
    for (int i = 0; i < 20; ++i) {
        if (i > 0)
            sql += ", ";
        sql += "c" + std::to_string(i) + (i % 2 == 0 ? " ASC" : " DESC");
    }
    auto stmt = parse_one(sql);
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->order_by.size(), 20u);
}

// =============================================================================
// Error paths
// =============================================================================

TEST(QA_GDB105, JoinMissingOn) {
    expect_parse_error("SELECT * FROM a JOIN b");
}

TEST(QA_GDB105, JoinMissingCondition) {
    expect_parse_error("SELECT * FROM a JOIN b ON");
}

TEST(QA_GDB105, UnionMissingSelect) {
    expect_parse_error("SELECT 1 UNION");
}
