/// @file test_qa_gdb_421.cpp
/// QA adversarial tests for GDB-421: SQL-ify MATCH Syntax.
///
/// Verifies:
///   AC1: SELECT a.name, b.name FROM MATCH (a:users)-[r:knows]->(b:users) WHERE a.id = 1 works.
///   AC2: SELECT ... FROM MATCH ... ORDER BY ... LIMIT ... composability works.
///   AC3: Standalone MATCH ... RETURN still works (backward compat).
///   AC4: Unit tests written and passing.
///   AC5: No regressions in existing MATCH tests.
///
/// Adversarial categories: parser edge cases, boundary values, error paths,
/// mixed directions, deep multi-hop chains, empty patterns, malformed syntax,
/// composability stress, backward compatibility regression.

#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Helpers
// ============================================================================

/// Parse SQL and expect success, returning all statements.
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

/// Parse SQL and expect exactly one statement.
static StmtPtr parse_one(std::string_view sql) {
    auto stmts = parse_ok(sql);
    EXPECT_EQ(stmts.size(), 1u);
    if (stmts.size() != 1)
        return nullptr;
    return std::move(stmts[0]);
}

/// Parse SQL and expect a parse failure.
static void parse_fail(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens) {
        // Lexer failure is also acceptable for malformed input.
        return;
    }

    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_FALSE(stmts.has_value())
        << "Expected parse failure for: " << sql;
}

/// Downcast statement to SelectStmt or return nullptr.
static SelectStmt* as_select(const StmtPtr& stmt) {
    return dynamic_cast<SelectStmt*>(stmt.get());
}

/// Downcast statement to MatchStmt or return nullptr.
static MatchStmt* as_match(const StmtPtr& stmt) {
    return dynamic_cast<MatchStmt*>(stmt.get());
}

/// Get the MatchStmt from the first FROM source of a SelectStmt.
static MatchStmt* match_from_select(const SelectStmt* sel) {
    if (!sel || sel->from.empty() || !sel->from[0].match_source)
        return nullptr;
    return dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
}

// ============================================================================
// AC1: Basic SELECT ... FROM MATCH works
// ============================================================================

TEST(GDB421, AC1_BasicSelectFromMatch) {
    auto stmt = parse_one(
        "SELECT a.name, b.name FROM MATCH (a:users)-[r:knows]->(b:users) WHERE a.id = 1");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->items.size(), 2u);
    ASSERT_EQ(sel->from.size(), 1u);
    auto* m = match_from_select(sel);
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 2u);
    EXPECT_EQ(m->pattern[0].node.variable, "a");
    EXPECT_EQ(m->pattern[0].node.label, "users");
    EXPECT_EQ(m->pattern[1].node.variable, "b");
    EXPECT_EQ(m->pattern[1].node.label, "users");
    // WHERE should be on the outer SELECT, not on the MatchStmt.
    EXPECT_NE(sel->where_expr, nullptr);
    EXPECT_EQ(m->where_expr, nullptr);
}

TEST(GDB421, AC1_SingleColumn) {
    auto stmt = parse_one("SELECT a.id FROM MATCH (a:people)-[e:knows]->(b:people)");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->items.size(), 1u);
    ASSERT_NE(match_from_select(sel), nullptr);
}

TEST(GDB421, AC1_ManyColumns) {
    auto stmt = parse_one(
        "SELECT a.id, a.name, b.id, b.name, e.weight "
        "FROM MATCH (a:people)-[e:works_at]->(b:companies)");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->items.size(), 5u);
}

// ============================================================================
// AC2: Composability — ORDER BY, LIMIT, OFFSET
// ============================================================================

TEST(GDB421, AC2_OrderByAsc) {
    auto stmt = parse_one(
        "SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users) ORDER BY a.name ASC");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->order_by.size(), 1u);
    EXPECT_EQ(sel->order_by[0].direction, SortDirection::ASC);
}

TEST(GDB421, AC2_OrderByDesc) {
    auto stmt = parse_one(
        "SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users) ORDER BY a.name DESC");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->order_by.size(), 1u);
    EXPECT_EQ(sel->order_by[0].direction, SortDirection::DESC);
}

TEST(GDB421, AC2_MultipleOrderByColumns) {
    auto stmt = parse_one(
        "SELECT a.name, b.name FROM MATCH (a:users)-[e:follows]->(b:users) "
        "ORDER BY a.name ASC, b.name DESC");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->order_by.size(), 2u);
    EXPECT_EQ(sel->order_by[0].direction, SortDirection::ASC);
    EXPECT_EQ(sel->order_by[1].direction, SortDirection::DESC);
}

TEST(GDB421, AC2_LimitOnly) {
    auto stmt = parse_one(
        "SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users) LIMIT 1");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_NE(sel->limit, nullptr);
    EXPECT_EQ(sel->offset, nullptr);
}

TEST(GDB421, AC2_LimitZero) {
    auto stmt = parse_one(
        "SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users) LIMIT 0");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_NE(sel->limit, nullptr);
}

TEST(GDB421, AC2_LimitWithOffset) {
    auto stmt = parse_one(
        "SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users) LIMIT 10 OFFSET 20");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_NE(sel->limit, nullptr);
    EXPECT_NE(sel->offset, nullptr);
}

TEST(GDB421, AC2_WhereOrderByLimitOffset) {
    auto stmt = parse_one(
        "SELECT a.name, b.name "
        "FROM MATCH (a:users)-[e:follows]->(b:users) "
        "WHERE a.active = true "
        "ORDER BY a.name "
        "LIMIT 100 OFFSET 50");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_NE(sel->where_expr, nullptr);
    ASSERT_EQ(sel->order_by.size(), 1u);
    EXPECT_NE(sel->limit, nullptr);
    EXPECT_NE(sel->offset, nullptr);
}

TEST(GDB421, AC2_DistinctWithOrderByAndLimit) {
    auto stmt = parse_one(
        "SELECT DISTINCT a.name "
        "FROM MATCH (a:users)-[e:follows]->(b:users) "
        "ORDER BY a.name LIMIT 5");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_TRUE(sel->distinct);
    ASSERT_EQ(sel->order_by.size(), 1u);
    EXPECT_NE(sel->limit, nullptr);
}

// ============================================================================
// AC3: Backward compatibility — MATCH ... RETURN still works
// ============================================================================

TEST(GDB421, AC3_StandaloneMatchReturn) {
    auto stmt = parse_one(
        "MATCH (a:users)-[e:follows]->(b:users) RETURN a.name, b.name");
    auto* m = as_match(stmt);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->pattern.size(), 2u);
    EXPECT_EQ(m->return_items.size(), 2u);
}

TEST(GDB421, AC3_StandaloneMatchReturnWithWhere) {
    auto stmt = parse_one(
        "MATCH (a:users)-[e:follows]->(b:users) WHERE a.id = 1 RETURN b.name");
    auto* m = as_match(stmt);
    ASSERT_NE(m, nullptr);
    EXPECT_NE(m->where_expr, nullptr);
    EXPECT_EQ(m->return_items.size(), 1u);
}

TEST(GDB421, AC3_StandaloneMatchMultiHop) {
    auto stmt = parse_one(
        "MATCH (a:people)-[r1:knows]->(b:people)-[r2:works_at]->(c:companies) "
        "RETURN a.name, b.name, c.name");
    auto* m = as_match(stmt);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->pattern.size(), 3u);
    EXPECT_EQ(m->return_items.size(), 3u);
}

TEST(GDB421, AC3_StandaloneMatchIncoming) {
    auto stmt = parse_one(
        "MATCH (a:users)<-[e:follows]-(b:users) RETURN a.name");
    auto* m = as_match(stmt);
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::IN);
}

TEST(GDB421, AC3_StandaloneMatchUndirected) {
    auto stmt = parse_one(
        "MATCH (a:users)-[e:knows]-(b:users) RETURN a.name");
    auto* m = as_match(stmt);
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::BOTH);
}

// ============================================================================
// Edge cases: direction combinations
// ============================================================================

TEST(GDB421, EdgeCase_MixedDirectionsMultiHop) {
    // outgoing then incoming: a->b<-c
    auto stmt = parse_one(
        "SELECT a.name, c.name "
        "FROM MATCH (a:people)-[r1:knows]->(b:people)<-[r2:knows]-(c:people)");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* m = match_from_select(sel);
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 3u);
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::OUT);
    EXPECT_EQ(m->pattern[1].outgoing_edge->direction, TraverseDirection::IN);
}

TEST(GDB421, EdgeCase_AllDirectionsInChain) {
    // out, in, both in one chain
    auto stmt = parse_one(
        "SELECT a.name "
        "FROM MATCH (a:p)-[e1:x]->(b:p)<-[e2:y]-(c:p)-[e3:z]-(d:p)");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* m = match_from_select(sel);
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 4u);
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::OUT);
    EXPECT_EQ(m->pattern[1].outgoing_edge->direction, TraverseDirection::IN);
    EXPECT_EQ(m->pattern[2].outgoing_edge->direction, TraverseDirection::BOTH);
    EXPECT_FALSE(m->pattern[3].outgoing_edge.has_value());
}

// ============================================================================
// Edge cases: node/edge variable and label combinations
// ============================================================================

TEST(GDB421, EdgeCase_NodeVariableOnly) {
    // (a) with no label
    auto stmt = parse_one(
        "SELECT * FROM MATCH (a)-[e:follows]->(b)");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* m = match_from_select(sel);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->pattern[0].node.variable, "a");
    EXPECT_TRUE(m->pattern[0].node.label.empty());
    EXPECT_EQ(m->pattern[1].node.variable, "b");
    EXPECT_TRUE(m->pattern[1].node.label.empty());
}

TEST(GDB421, EdgeCase_LabelOnlyNoVariable) {
    // (:label) — no variable
    auto stmt = parse_one(
        "SELECT * FROM MATCH (:users)-[:follows]->(:users)");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* m = match_from_select(sel);
    ASSERT_NE(m, nullptr);
    EXPECT_TRUE(m->pattern[0].node.variable.empty());
    EXPECT_EQ(m->pattern[0].node.label, "users");
    EXPECT_TRUE(m->pattern[0].outgoing_edge->variable.empty());
    EXPECT_EQ(m->pattern[0].outgoing_edge->edge_type, "follows");
}

TEST(GDB421, EdgeCase_EdgeVariableWithoutType) {
    // Edge with just a name, no type: -[r]-
    auto stmt = parse_one(
        "SELECT * FROM MATCH (a:users)-[r]->(b:users)");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* m = match_from_select(sel);
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    // When no colon, the name is treated as edge_type (not variable).
    EXPECT_EQ(m->pattern[0].outgoing_edge->edge_type, "r");
}

TEST(GDB421, EdgeCase_EmptyBrackets) {
    // Edge with empty brackets: -[]-
    auto stmt = parse_one(
        "SELECT * FROM MATCH (a:users)-[]->(b:users)");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* m = match_from_select(sel);
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_TRUE(m->pattern[0].outgoing_edge->variable.empty());
    EXPECT_TRUE(m->pattern[0].outgoing_edge->edge_type.empty());
}

TEST(GDB421, EdgeCase_EmptyNodeParens) {
    // Completely empty node: ()
    auto stmt = parse_one(
        "SELECT * FROM MATCH ()-[e:follows]->()");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* m = match_from_select(sel);
    ASSERT_NE(m, nullptr);
    EXPECT_TRUE(m->pattern[0].node.variable.empty());
    EXPECT_TRUE(m->pattern[0].node.label.empty());
    EXPECT_TRUE(m->pattern[1].node.variable.empty());
    EXPECT_TRUE(m->pattern[1].node.label.empty());
}

// ============================================================================
// Edge cases: SELECT * from MATCH
// ============================================================================

TEST(GDB421, EdgeCase_SelectStarFromMatch) {
    auto stmt = parse_one(
        "SELECT * FROM MATCH (a:users)-[e:follows]->(b:users)");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    EXPECT_TRUE(sel->items[0].is_star);
    ASSERT_NE(match_from_select(sel), nullptr);
}

// ============================================================================
// Edge cases: long multi-hop chains
// ============================================================================

TEST(GDB421, EdgeCase_FourHopChain) {
    auto stmt = parse_one(
        "SELECT a.name "
        "FROM MATCH (a:p)-[e1:x]->(b:p)-[e2:y]->(c:p)-[e3:z]->(d:p)-[e4:w]->(e:p)");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* m = match_from_select(sel);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->pattern.size(), 5u);
    // First 4 nodes have outgoing edges, last does not.
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_TRUE(m->pattern[i].outgoing_edge.has_value())
            << "Expected edge at index " << i;
    }
    EXPECT_FALSE(m->pattern[4].outgoing_edge.has_value());
}

// ============================================================================
// Edge cases: single node (no edges)
// ============================================================================

TEST(GDB421, EdgeCase_SingleNodeNoEdge) {
    // Just a single node with no edge: SELECT * FROM MATCH (a:users)
    auto stmt = parse_one("SELECT a.name FROM MATCH (a:users)");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* m = match_from_select(sel);
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 1u);
    EXPECT_EQ(m->pattern[0].node.variable, "a");
    EXPECT_EQ(m->pattern[0].node.label, "users");
    EXPECT_FALSE(m->pattern[0].outgoing_edge.has_value());
}

// ============================================================================
// Error paths: malformed syntax
// ============================================================================

TEST(GDB421, ErrorPath_MissingNodeParens) {
    // Missing opening parenthesis for node.
    parse_fail("SELECT a.name FROM MATCH a:users-[e:follows]->(b:users)");
}

TEST(GDB421, ErrorPath_MissingClosingNodeParen) {
    parse_fail("SELECT a.name FROM MATCH (a:users-[e:follows]->(b:users)");
}

TEST(GDB421, ErrorPath_MissingEdgeBracketClose) {
    parse_fail("SELECT a.name FROM MATCH (a:users)-[e:follows->(b:users)");
}

TEST(GDB421, ErrorPath_SelectFromMatchMissingPattern) {
    // FROM MATCH with no pattern at all.
    parse_fail("SELECT a.name FROM MATCH");
}

TEST(GDB421, ErrorPath_StandaloneMatchMissingReturn) {
    // Standalone MATCH without RETURN.
    parse_fail("MATCH (a:users)-[e:follows]->(b:users)");
}

TEST(GDB421, EdgeCase_DoubleDashBeforeArrowIsNotComment) {
    // GDB-552 fix: `--` followed by `>`, `[`, or `(` is no longer consumed
    // as a line comment. The bracket-less `-->` shorthand is not valid
    // syntax, so this should produce a parse error instead of silently
    // truncating the pattern.
    parse_fail("SELECT a.name FROM MATCH (a:users)-->[e:follows]->(b:users)");
}

// ============================================================================
// WHERE clause placement verification
// ============================================================================

TEST(GDB421, WhereClause_OnOuterSelectNotMatch) {
    // In SELECT...FROM MATCH, WHERE belongs to outer SELECT.
    auto stmt = parse_one(
        "SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users) WHERE a.id > 5");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    // WHERE is on the SELECT.
    EXPECT_NE(sel->where_expr, nullptr);
    // MatchStmt should have no where_expr.
    auto* m = match_from_select(sel);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->where_expr, nullptr);
}

TEST(GDB421, WhereClause_StandaloneMatchHasWhere) {
    // In standalone MATCH...WHERE...RETURN, WHERE is on the MatchStmt.
    auto stmt = parse_one(
        "MATCH (a:users)-[e:follows]->(b:users) WHERE a.id > 5 RETURN a.name");
    auto* m = as_match(stmt);
    ASSERT_NE(m, nullptr);
    EXPECT_NE(m->where_expr, nullptr);
}

TEST(GDB421, WhereClause_ComplexPredicate) {
    auto stmt = parse_one(
        "SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users) "
        "WHERE a.age >= 18 AND b.active = true AND a.name <> 'test'");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_NE(sel->where_expr, nullptr);
}

// ============================================================================
// Regression: both old and new syntax in same session
// ============================================================================

TEST(GDB421, Regression_BothSyntaxesParseSeparately) {
    // Old syntax.
    auto stmt1 = parse_one(
        "MATCH (a:users)-[e:follows]->(b:users) RETURN a.name");
    auto* m1 = as_match(stmt1);
    ASSERT_NE(m1, nullptr);

    // New syntax.
    auto stmt2 = parse_one(
        "SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users)");
    auto* s2 = as_select(stmt2);
    ASSERT_NE(s2, nullptr);
    ASSERT_NE(match_from_select(s2), nullptr);

    // Both produce valid pattern structures.
    EXPECT_EQ(m1->pattern.size(), s2->from[0].match_source ? 2u : 0u);
}

// ============================================================================
// Boundary: case sensitivity
// ============================================================================

TEST(GDB421, CaseSensitivity_KeywordsUpperCase) {
    auto stmt = parse_one(
        "SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users)");
    ASSERT_NE(as_select(stmt), nullptr);
}

TEST(GDB421, CaseSensitivity_KeywordsLowerCase) {
    auto stmt = parse_one(
        "select a.name from match (a:users)-[e:follows]->(b:users)");
    ASSERT_NE(as_select(stmt), nullptr);
}

TEST(GDB421, CaseSensitivity_KeywordsMixedCase) {
    auto stmt = parse_one(
        "Select a.name From Match (a:users)-[e:follows]->(b:users)");
    ASSERT_NE(as_select(stmt), nullptr);
}

// ============================================================================
// Boundary: whitespace variations
// ============================================================================

TEST(GDB421, Whitespace_MinimalSpacing) {
    auto stmt = parse_one(
        "SELECT a.name FROM MATCH(a:users)-[e:follows]->(b:users)");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_NE(match_from_select(sel), nullptr);
}

TEST(GDB421, Whitespace_ExtraSpaces) {
    auto stmt = parse_one(
        "SELECT   a.name   FROM   MATCH   (a:users)  -  [e:follows]  ->  (b:users)");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_NE(match_from_select(sel), nullptr);
}

// ============================================================================
// Boundary: edge with both < and > (bidirectional arrow <-[]->)
// ============================================================================

TEST(GDB421, Direction_BothArrow) {
    // <-[]-> should be BOTH direction.
    auto stmt = parse_one(
        "SELECT a.name FROM MATCH (a:users)<-[e:knows]->(b:users)");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* m = match_from_select(sel);
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::BOTH);
}

// ============================================================================
// Composability: SELECT with expressions, not just columns
// ============================================================================

TEST(GDB421, Composability_ExpressionInSelect) {
    auto stmt = parse_one(
        "SELECT a.age + 1 FROM MATCH (a:users)-[e:follows]->(b:users)");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->items.size(), 1u);
}

TEST(GDB421, Composability_AliasedColumn) {
    auto stmt = parse_one(
        "SELECT a.name AS user_name FROM MATCH (a:users)-[e:follows]->(b:users)");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->items.size(), 1u);
    EXPECT_EQ(sel->items[0].alias, "user_name");
}

TEST(GDB421, Composability_CountAggregate) {
    auto stmt = parse_one(
        "SELECT a.name, COUNT(*) FROM MATCH (a:users)-[e:follows]->(b:users) "
        "GROUP BY a.name");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->items.size(), 2u);
    EXPECT_EQ(sel->group_by.size(), 1u);
}

TEST(GDB421, Composability_Having) {
    auto stmt = parse_one(
        "SELECT a.name, COUNT(*) AS cnt "
        "FROM MATCH (a:users)-[e:follows]->(b:users) "
        "GROUP BY a.name "
        "HAVING COUNT(*) > 3");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_NE(sel->having_expr, nullptr);
}

// ============================================================================
// Stress: pattern with many nodes
// ============================================================================

TEST(GDB421, Stress_LongChain) {
    // Build a 10-hop chain dynamically.
    std::string sql = "SELECT n0.name FROM MATCH (n0:t)";
    for (int i = 0; i < 10; ++i) {
        sql += "-[e" + std::to_string(i) + ":rel]->(n" + std::to_string(i + 1) + ":t)";
    }
    auto stmt = parse_one(sql);
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* m = match_from_select(sel);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->pattern.size(), 11u); // 11 nodes
}

} // namespace
} // namespace sixseven
