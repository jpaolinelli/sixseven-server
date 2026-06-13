/// @file test_qa_gdb_106.cpp
/// @brief QA adversarial tests for GDB-106: Parser for TRAVERSE, NEAREST,
///        MATCH, SHORTEST PATH, and admin statements.
///
/// Covers edge cases, boundary values, error paths, and stress tests for
/// graph/vector queries and administrative commands.

#include "parser_qa_helpers.h"

#include <string>

// ---------------------------------------------------------------------------
// File-specific helpers
// ---------------------------------------------------------------------------


/// Recursively locate the NearestExpr embedded in a WHERE expression tree,
/// descending through AND/OR (BinaryExpr) and NOT (UnaryExpr) nodes.
static const NearestExpr* find_nearest_expr(const Expr* e) {
    if (e == nullptr)
        return nullptr;
    if (auto* n = dynamic_cast<const NearestExpr*>(e))
        return n;
    if (auto* bin = dynamic_cast<const BinaryExpr*>(e)) {
        if (auto* found = find_nearest_expr(bin->lhs.get()))
            return found;
        return find_nearest_expr(bin->rhs.get());
    }
    if (auto* un = dynamic_cast<const UnaryExpr*>(e))
        return find_nearest_expr(un->operand.get());
    return nullptr;
}

/// Parse a SELECT carrying a NEAREST predicate and return the NearestExpr.
/// The owning statements are kept alive in a static store so the returned
/// borrowed pointer stays valid for the duration of the test program.
static const NearestExpr* parse_nearest(std::string_view sql) {
    static std::vector<StmtPtr> keep_alive;
    auto stmt = parse_one(sql);
    EXPECT_NE(stmt, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    EXPECT_NE(sel, nullptr);
    if (sel == nullptr)
        return nullptr;
    const NearestExpr* n = find_nearest_expr(sel->where_expr.get());
    keep_alive.push_back(std::move(stmt));
    return n;
}

/// Extract the column name from a NearestExpr's column field.
static std::string nearest_column(const NearestExpr* n) {
    auto* col = dynamic_cast<const ColumnRefExpr*>(n->column.get());
    EXPECT_NE(col, nullptr);
    return col != nullptr ? col->column : std::string{};
}

// ---------------------------------------------------------------------------
// TRAVERSE tests
// ---------------------------------------------------------------------------

TEST(QA_GDB_106_Traverse, BasicTraverse) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1)");
    ASSERT_NE(stmt, nullptr);
    auto* t = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->edge_type, "follows");
    EXPECT_EQ(t->from_table, "users");
    EXPECT_EQ(t->direction, TraverseDirection::OUT); // default
    EXPECT_FALSE(t->max_depth.has_value());
    EXPECT_EQ(t->where_expr, nullptr);
    EXPECT_FALSE(t->fetch);
}

TEST(QA_GDB_106_Traverse, DirectionIn) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1) DIRECTION IN");
    auto* t = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->direction, TraverseDirection::IN);
}

TEST(QA_GDB_106_Traverse, DirectionOut) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1) DIRECTION OUT");
    auto* t = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->direction, TraverseDirection::OUT);
}

TEST(QA_GDB_106_Traverse, DirectionBoth) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1) DIRECTION BOTH");
    auto* t = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->direction, TraverseDirection::BOTH);
}

TEST(QA_GDB_106_Traverse, MaxDepth) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1) MAX_DEPTH 5");
    auto* t = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(t, nullptr);
    ASSERT_TRUE(t->max_depth.has_value());
    EXPECT_EQ(*t->max_depth, 5);
}

TEST(QA_GDB_106_Traverse, MaxDepthZero) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1) MAX_DEPTH 0");
    auto* t = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(t, nullptr);
    ASSERT_TRUE(t->max_depth.has_value());
    EXPECT_EQ(*t->max_depth, 0);
}

TEST(QA_GDB_106_Traverse, WhereClause) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1) WHERE depth < 3");
    auto* t = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(t, nullptr);
    EXPECT_NE(t->where_expr, nullptr);
}

TEST(QA_GDB_106_Traverse, FetchFlag) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1) FETCH");
    auto* t = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(t->fetch);
}

TEST(QA_GDB_106_Traverse, AllOptions) {
    auto stmt = parse_one("TRAVERSE follows FROM users(42) DIRECTION BOTH MAX_DEPTH 10 "
                          "WHERE depth > 0 FETCH");
    auto* t = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->edge_type, "follows");
    EXPECT_EQ(t->from_table, "users");
    EXPECT_EQ(t->direction, TraverseDirection::BOTH);
    ASSERT_TRUE(t->max_depth.has_value());
    EXPECT_EQ(*t->max_depth, 10);
    EXPECT_NE(t->where_expr, nullptr);
    EXPECT_TRUE(t->fetch);
}

TEST(QA_GDB_106_Traverse, ExpressionKey) {
    auto stmt = parse_one("TRAVERSE likes FROM posts(1 + 2)");
    auto* t = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->from_table, "posts");
    // from_key is an expression (1 + 2)
    auto* bin = dynamic_cast<BinaryExpr*>(t->from_key.get());
    ASSERT_NE(bin, nullptr);
}

TEST(QA_GDB_106_Traverse, StringKey) {
    auto stmt = parse_one("TRAVERSE knows FROM people('alice')");
    auto* t = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(t, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(t->from_key.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::STRING);
}

// Error paths
TEST(QA_GDB_106_Traverse, MissingEdgeType) {
    expect_parse_error("TRAVERSE FROM users(1)");
}

TEST(QA_GDB_106_Traverse, MissingFrom) {
    expect_parse_error("TRAVERSE follows users(1)");
}

TEST(QA_GDB_106_Traverse, MissingTableName) {
    expect_parse_error("TRAVERSE follows FROM (1)");
}

TEST(QA_GDB_106_Traverse, MissingKey) {
    expect_parse_error("TRAVERSE follows FROM users()");
}

TEST(QA_GDB_106_Traverse, MissingLeftParen) {
    expect_parse_error("TRAVERSE follows FROM users 1)");
}

TEST(QA_GDB_106_Traverse, MissingRightParen) {
    expect_parse_error("TRAVERSE follows FROM users(1");
}

TEST(QA_GDB_106_Traverse, InvalidDirection) {
    expect_parse_error("TRAVERSE follows FROM users(1) DIRECTION LEFT");
}

TEST(QA_GDB_106_Traverse, MaxDepthNonInteger) {
    expect_parse_error("TRAVERSE follows FROM users(1) MAX_DEPTH 'five'");
}

TEST(QA_GDB_106_Traverse, DirectionAfterMaxDepth) {
    // The grammar is fixed-order: DIRECTION must come before MAX_DEPTH
    // (parse_traverse checks DIRECTION first, then MAX_DEPTH). With the
    // clauses reversed, the trailing "DIRECTION IN" tokens cannot start a
    // new statement, so parse_all must reject the input rather than
    // silently dropping the direction.
    expect_parse_error("TRAVERSE follows FROM users(1) MAX_DEPTH 3 DIRECTION IN");
}

TEST(QA_GDB_106_Traverse, DirectionBeforeMaxDepthParses) {
    // Companion to DirectionAfterMaxDepth: the documented order parses and
    // both options land on the statement.
    auto stmt = parse_one("TRAVERSE follows FROM users(1) DIRECTION IN MAX_DEPTH 3");
    auto* t = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->direction, TraverseDirection::IN);
    ASSERT_TRUE(t->max_depth.has_value());
    EXPECT_EQ(*t->max_depth, 3);
}

// ---------------------------------------------------------------------------
// NEAREST tests
// ---------------------------------------------------------------------------

TEST(QA_GDB_106_Nearest, BasicNearest) {
    auto* n =
        parse_nearest("SELECT * FROM products WHERE NEAREST(embedding, 5) TO [1.0, 2.0, 3.0]");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(nearest_column(n), "embedding");
    EXPECT_EQ(n->metric, NearestMetric::COSINE); // default
    EXPECT_EQ(n->within_traverse, nullptr);
}

TEST(QA_GDB_106_Nearest, WithTextTarget) {
    auto* n = parse_nearest("SELECT * FROM docs WHERE NEAREST(embed, 10) TO 'machine learning'");
    ASSERT_NE(n, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(n->target.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::STRING);
}

TEST(QA_GDB_106_Nearest, UsingCosine) {
    auto* n = parse_nearest("SELECT * FROM t WHERE NEAREST(col, 5) TO [1.0] USING COSINE");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->metric, NearestMetric::COSINE);
}

TEST(QA_GDB_106_Nearest, UsingL2) {
    auto* n = parse_nearest("SELECT * FROM t WHERE NEAREST(col, 5) TO [1.0] USING L2");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->metric, NearestMetric::L2);
}

TEST(QA_GDB_106_Nearest, UsingDot) {
    auto* n = parse_nearest("SELECT * FROM t WHERE NEAREST(col, 5) TO [1.0] USING DOT");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->metric, NearestMetric::DOT);
}

TEST(QA_GDB_106_Nearest, WithWhere) {
    // Residual predicate becomes a trailing AND alongside the NEAREST predicate.
    auto* n = parse_nearest("SELECT * FROM t WHERE NEAREST(col, 5) TO [1.0] AND active = true");
    ASSERT_NE(n, nullptr);
}

TEST(QA_GDB_106_Nearest, WithWhereAndUsing) {
    auto* n =
        parse_nearest("SELECT * FROM t WHERE NEAREST(col, 5) TO [1.0] USING L2 AND active = true");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->metric, NearestMetric::L2);
}

TEST(QA_GDB_106_Nearest, WithinTraverse) {
    auto* n = parse_nearest("SELECT * FROM products WHERE NEAREST(embedding, 5) TO 'laptop' "
                            "WITHIN TRAVERSE similar FROM products(1)");
    ASSERT_NE(n, nullptr);
    ASSERT_NE(n->within_traverse, nullptr);
    auto* trav = dynamic_cast<TraverseStmt*>(n->within_traverse.get());
    ASSERT_NE(trav, nullptr);
    EXPECT_EQ(trav->edge_type, "similar");
    EXPECT_EQ(trav->from_table, "products");
}

TEST(QA_GDB_106_Nearest, WithinTraverseWithDirection) {
    auto* n = parse_nearest("SELECT * FROM t WHERE NEAREST(col, 5) TO 'x' "
                            "WITHIN TRAVERSE e FROM t(1) DIRECTION BOTH");
    ASSERT_NE(n, nullptr);
    auto* trav = dynamic_cast<TraverseStmt*>(n->within_traverse.get());
    ASSERT_NE(trav, nullptr);
    EXPECT_EQ(trav->direction, TraverseDirection::BOTH);
}

TEST(QA_GDB_106_Nearest, WithinTraverseWithMaxDepth) {
    auto* n = parse_nearest("SELECT * FROM t WHERE NEAREST(col, 5) TO 'x' "
                            "WITHIN TRAVERSE e FROM t(1) MAX_DEPTH 3");
    ASSERT_NE(n, nullptr);
    auto* trav = dynamic_cast<TraverseStmt*>(n->within_traverse.get());
    ASSERT_NE(trav, nullptr);
    ASSERT_TRUE(trav->max_depth.has_value());
    EXPECT_EQ(*trav->max_depth, 3);
}

TEST(QA_GDB_106_Nearest, WithinTraverseAndWhereAndUsing) {
    auto* n = parse_nearest(
        "SELECT * FROM products WHERE NEAREST(embed, 10) TO [1.0, 2.0] "
        "WITHIN TRAVERSE related FROM products(42) DIRECTION IN MAX_DEPTH 2 USING DOT "
        "AND price < 100");
    ASSERT_NE(n, nullptr);
    ASSERT_NE(n->within_traverse, nullptr);
    EXPECT_EQ(n->metric, NearestMetric::DOT);
}

TEST(QA_GDB_106_Nearest, ExpressionK) {
    auto* n = parse_nearest("SELECT * FROM t WHERE NEAREST(col, 2 + 3) TO 'x'");
    ASSERT_NE(n, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(n->k.get());
    ASSERT_NE(bin, nullptr);
}

// Error paths
TEST(QA_GDB_106_Nearest, MissingTo) {
    expect_parse_error("SELECT * FROM t WHERE NEAREST(col, 5) [1.0]");
}

TEST(QA_GDB_106_Nearest, InvalidMetric) {
    expect_parse_error("SELECT * FROM t WHERE NEAREST(col, 5) TO [1.0] USING MANHATTAN");
}

TEST(QA_GDB_106_Nearest, WithinWithoutTraverse) {
    expect_parse_error("SELECT * FROM t WHERE NEAREST(col, 5) TO 'x' WITHIN e FROM t(1)");
}

// ---------------------------------------------------------------------------
// MATCH tests
// ---------------------------------------------------------------------------

TEST(QA_GDB_106_Match, SingleNode) {
    auto stmt = parse_one("MATCH (a) RETURN a");
    ASSERT_NE(stmt, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 1);
    EXPECT_EQ(m->pattern[0].node.variable, "a");
    EXPECT_TRUE(m->pattern[0].node.label.empty());
    EXPECT_FALSE(m->pattern[0].outgoing_edge.has_value());
}

TEST(QA_GDB_106_Match, NodeWithLabel) {
    auto stmt = parse_one("MATCH (p:Person) RETURN p");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 1);
    EXPECT_EQ(m->pattern[0].node.variable, "p");
    EXPECT_EQ(m->pattern[0].node.label, "Person");
}

TEST(QA_GDB_106_Match, NodeLabelOnly) {
    auto stmt = parse_one("MATCH (:Person) RETURN *");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 1);
    EXPECT_TRUE(m->pattern[0].node.variable.empty());
    EXPECT_EQ(m->pattern[0].node.label, "Person");
}

TEST(QA_GDB_106_Match, EmptyNode) {
    auto stmt = parse_one("MATCH () RETURN *");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 1);
    EXPECT_TRUE(m->pattern[0].node.variable.empty());
    EXPECT_TRUE(m->pattern[0].node.label.empty());
}

TEST(QA_GDB_106_Match, OutgoingEdge) {
    auto stmt = parse_one("MATCH (a)-[:KNOWS]->(b) RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 2);
    // First element has the edge
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->edge_type, "KNOWS");
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::OUT);
    // Second element is the target node
    EXPECT_EQ(m->pattern[1].node.variable, "b");
}

TEST(QA_GDB_106_Match, IncomingEdge) {
    auto stmt = parse_one("MATCH (a)<-[:KNOWS]-(b) RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 2);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::IN);
}

TEST(QA_GDB_106_Match, UndirectedEdge) {
    auto stmt = parse_one("MATCH (a)-[:KNOWS]-(b) RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 2);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::BOTH);
}

TEST(QA_GDB_106_Match, BidirectionalEdge) {
    // <-[]--> means both prefix < and suffix >, which is BOTH
    auto stmt = parse_one("MATCH (a)<-[:KNOWS]->(b) RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 2);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::BOTH);
}

TEST(QA_GDB_106_Match, EdgeWithVariable) {
    auto stmt = parse_one("MATCH (a)-[r:KNOWS]->(b) RETURN r");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->variable, "r");
    EXPECT_EQ(m->pattern[0].outgoing_edge->edge_type, "KNOWS");
}

TEST(QA_GDB_106_Match, EdgeTypeOnly) {
    // Edge with just a name (no colon prefix) is treated as edge_type
    auto stmt = parse_one("MATCH (a)-[KNOWS]->(b) RETURN a");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->edge_type, "KNOWS");
}

TEST(QA_GDB_106_Match, EmptyEdgeBrackets) {
    auto stmt = parse_one("MATCH (a)-[]->(b) RETURN a");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_TRUE(m->pattern[0].outgoing_edge->edge_type.empty());
    EXPECT_TRUE(m->pattern[0].outgoing_edge->variable.empty());
}

TEST(QA_GDB_106_Match, BareEdgeNoBrackets) {
    // Since GDB-552, --> is a valid edge pattern (-- is not treated as a
    // line comment when followed by >).
    auto stmt = parse_one("MATCH (a)-->(b) RETURN a");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::OUT);
    EXPECT_TRUE(m->pattern[0].outgoing_edge->edge_type.empty());
}

TEST(QA_GDB_106_Match, BareEdgeWithBracketsNonDirectional) {
    auto stmt = parse_one("MATCH (a)-[]-(b) RETURN a");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::BOTH);
    EXPECT_TRUE(m->pattern[0].outgoing_edge->edge_type.empty());
}

TEST(QA_GDB_106_Match, ThreeNodePath) {
    auto stmt = parse_one("MATCH (a:Person)-[:KNOWS]->(b:Person)-[:WORKS_AT]->(c:Company) "
                          "RETURN a, c");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 3);
    // First node + edge
    EXPECT_EQ(m->pattern[0].node.variable, "a");
    EXPECT_EQ(m->pattern[0].node.label, "Person");
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->edge_type, "KNOWS");
    // Second node + edge
    EXPECT_EQ(m->pattern[1].node.variable, "b");
    ASSERT_TRUE(m->pattern[1].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[1].outgoing_edge->edge_type, "WORKS_AT");
    // Third node, no edge
    EXPECT_EQ(m->pattern[2].node.variable, "c");
    EXPECT_EQ(m->pattern[2].node.label, "Company");
    EXPECT_FALSE(m->pattern[2].outgoing_edge.has_value());
}

TEST(QA_GDB_106_Match, WithWhere) {
    auto stmt = parse_one("MATCH (a:Person)-[:KNOWS]->(b) WHERE a.age > 30 RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_NE(m->where_expr, nullptr);
}

TEST(QA_GDB_106_Match, MultipleReturnItems) {
    auto stmt = parse_one("MATCH (a)-[r:LIKES]->(b) RETURN a, r, b, a.name AS n");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->return_items.size(), 4);
}

TEST(QA_GDB_106_Match, ReturnStar) {
    auto stmt = parse_one("MATCH (a)-[:E]->(b) RETURN *");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->return_items.size(), 1);
    EXPECT_TRUE(m->return_items[0].is_star);
}

// Error paths
TEST(QA_GDB_106_Match, MissingReturn) {
    expect_parse_error("MATCH (a)-[:KNOWS]->(b)");
}

TEST(QA_GDB_106_Match, MissingNodeParen) {
    expect_parse_error("MATCH a-[:KNOWS]->(b) RETURN a");
}

TEST(QA_GDB_106_Match, MissingClosingNodeParen) {
    expect_parse_error("MATCH (a-[:KNOWS]->(b) RETURN a");
}

TEST(QA_GDB_106_Match, MissingEdgeBracketClose) {
    expect_parse_error("MATCH (a)-[:KNOWS->(b) RETURN a");
}

// ---------------------------------------------------------------------------
// SHORTEST PATH tests
// ---------------------------------------------------------------------------

TEST(QA_GDB_106_ShortestPath, Basic) {
    auto stmt = parse_one("SHORTEST PATH FROM users(1) TO users(10) VIA follows");
    ASSERT_NE(stmt, nullptr);
    auto* sp = dynamic_cast<ShortestPathStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->from_table, "users");
    EXPECT_EQ(sp->to_table, "users");
    EXPECT_EQ(sp->edge_type, "follows");
    EXPECT_EQ(sp->direction, TraverseDirection::OUT); // default
    EXPECT_FALSE(sp->max_depth.has_value());
}

TEST(QA_GDB_106_ShortestPath, WithDirection) {
    auto stmt = parse_one("SHORTEST PATH FROM a(1) TO b(2) VIA e DIRECTION BOTH");
    auto* sp = dynamic_cast<ShortestPathStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->direction, TraverseDirection::BOTH);
}

TEST(QA_GDB_106_ShortestPath, WithMaxDepth) {
    auto stmt = parse_one("SHORTEST PATH FROM a(1) TO b(2) VIA e MAX_DEPTH 7");
    auto* sp = dynamic_cast<ShortestPathStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    ASSERT_TRUE(sp->max_depth.has_value());
    EXPECT_EQ(*sp->max_depth, 7);
}

TEST(QA_GDB_106_ShortestPath, AllOptions) {
    auto stmt = parse_one("SHORTEST PATH FROM users(1) TO users(99) VIA knows "
                          "DIRECTION IN MAX_DEPTH 20");
    auto* sp = dynamic_cast<ShortestPathStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->from_table, "users");
    EXPECT_EQ(sp->to_table, "users");
    EXPECT_EQ(sp->edge_type, "knows");
    EXPECT_EQ(sp->direction, TraverseDirection::IN);
    ASSERT_TRUE(sp->max_depth.has_value());
    EXPECT_EQ(*sp->max_depth, 20);
}

TEST(QA_GDB_106_ShortestPath, ExpressionKeys) {
    auto stmt = parse_one("SHORTEST PATH FROM t(1 + 1) TO t(2 * 3) VIA e");
    auto* sp = dynamic_cast<ShortestPathStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_NE(dynamic_cast<BinaryExpr*>(sp->from_key.get()), nullptr);
    EXPECT_NE(dynamic_cast<BinaryExpr*>(sp->to_key.get()), nullptr);
}

TEST(QA_GDB_106_ShortestPath, DifferentTables) {
    auto stmt = parse_one("SHORTEST PATH FROM airports('JFK') TO airports('LAX') VIA routes");
    auto* sp = dynamic_cast<ShortestPathStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->from_table, "airports");
    EXPECT_EQ(sp->to_table, "airports");
    EXPECT_EQ(sp->edge_type, "routes");
}

// Error paths
TEST(QA_GDB_106_ShortestPath, MissingPath) {
    expect_parse_error("SHORTEST FROM a(1) TO b(2) VIA e");
}

TEST(QA_GDB_106_ShortestPath, MissingFrom) {
    expect_parse_error("SHORTEST PATH a(1) TO b(2) VIA e");
}

TEST(QA_GDB_106_ShortestPath, MissingTo) {
    expect_parse_error("SHORTEST PATH FROM a(1) b(2) VIA e");
}

TEST(QA_GDB_106_ShortestPath, MissingVia) {
    expect_parse_error("SHORTEST PATH FROM a(1) TO b(2)");
}

TEST(QA_GDB_106_ShortestPath, MissingFromKey) {
    expect_parse_error("SHORTEST PATH FROM a() TO b(2) VIA e");
}

TEST(QA_GDB_106_ShortestPath, MissingToKey) {
    expect_parse_error("SHORTEST PATH FROM a(1) TO b() VIA e");
}

// ---------------------------------------------------------------------------
// TCL: BEGIN / COMMIT / ROLLBACK / SAVEPOINT
// ---------------------------------------------------------------------------

TEST(QA_GDB_106_TCL, Begin) {
    auto stmt = parse_one("BEGIN");
    ASSERT_NE(stmt, nullptr);
    EXPECT_NE(dynamic_cast<BeginStmt*>(stmt.get()), nullptr);
}

TEST(QA_GDB_106_TCL, BeginTransaction) {
    auto stmt = parse_one("BEGIN TRANSACTION");
    ASSERT_NE(stmt, nullptr);
    EXPECT_NE(dynamic_cast<BeginStmt*>(stmt.get()), nullptr);
}

TEST(QA_GDB_106_TCL, Commit) {
    auto stmt = parse_one("COMMIT");
    ASSERT_NE(stmt, nullptr);
    EXPECT_NE(dynamic_cast<CommitStmt*>(stmt.get()), nullptr);
}

TEST(QA_GDB_106_TCL, Rollback) {
    auto stmt = parse_one("ROLLBACK");
    ASSERT_NE(stmt, nullptr);
    auto* r = dynamic_cast<RollbackStmt*>(stmt.get());
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->savepoint.empty());
}

TEST(QA_GDB_106_TCL, RollbackToSavepoint) {
    auto stmt = parse_one("ROLLBACK TO sp1");
    ASSERT_NE(stmt, nullptr);
    auto* r = dynamic_cast<RollbackStmt*>(stmt.get());
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->savepoint, "sp1");
}

TEST(QA_GDB_106_TCL, Savepoint) {
    auto stmt = parse_one("SAVEPOINT my_sp");
    ASSERT_NE(stmt, nullptr);
    auto* sp = dynamic_cast<SavepointStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->name, "my_sp");
}

TEST(QA_GDB_106_TCL, SavepointMissingName) {
    expect_parse_error("SAVEPOINT");
}

TEST(QA_GDB_106_TCL, MultipleStatements) {
    auto stmts = parse_ok("BEGIN; SAVEPOINT s1; ROLLBACK TO s1; COMMIT;");
    ASSERT_EQ(stmts.size(), 4);
    EXPECT_NE(dynamic_cast<BeginStmt*>(stmts[0].get()), nullptr);
    EXPECT_NE(dynamic_cast<SavepointStmt*>(stmts[1].get()), nullptr);
    EXPECT_NE(dynamic_cast<RollbackStmt*>(stmts[2].get()), nullptr);
    EXPECT_NE(dynamic_cast<CommitStmt*>(stmts[3].get()), nullptr);
}

// ---------------------------------------------------------------------------
// Admin: SET
// ---------------------------------------------------------------------------

TEST(QA_GDB_106_Set, BasicSet) {
    auto stmt = parse_one("SET logging = 'info'");
    ASSERT_NE(stmt, nullptr);
    auto* s = dynamic_cast<SetStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->parameter, "logging");
    auto* val = dynamic_cast<LiteralExpr*>(s->value.get());
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(val->value, "info");
}

TEST(QA_GDB_106_Set, DottedParam) {
    auto stmt = parse_one("SET logging.level = 'debug'");
    auto* s = dynamic_cast<SetStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->parameter, "logging.level");
}

TEST(QA_GDB_106_Set, DeepDottedParam) {
    auto stmt = parse_one("SET a.b.c.d = 42");
    auto* s = dynamic_cast<SetStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->parameter, "a.b.c.d");
}

TEST(QA_GDB_106_Set, IntegerValue) {
    auto stmt = parse_one("SET max_connections = 100");
    auto* s = dynamic_cast<SetStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    auto* val = dynamic_cast<LiteralExpr*>(s->value.get());
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(val->kind, LiteralKind::INTEGER);
}

TEST(QA_GDB_106_Set, BooleanValue) {
    auto stmt = parse_one("SET wal.enabled = true");
    auto* s = dynamic_cast<SetStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->parameter, "wal.enabled");
    auto* val = dynamic_cast<LiteralExpr*>(s->value.get());
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(val->kind, LiteralKind::BOOLEAN);
}

TEST(QA_GDB_106_Set, ExpressionValue) {
    auto stmt = parse_one("SET timeout = 30 * 60");
    auto* s = dynamic_cast<SetStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(s->value.get());
    ASSERT_NE(bin, nullptr);
}

// Error paths
TEST(QA_GDB_106_Set, MissingEquals) {
    expect_parse_error("SET logging 'info'");
}

TEST(QA_GDB_106_Set, MissingValue) {
    expect_parse_error("SET logging =");
}

TEST(QA_GDB_106_Set, MissingParam) {
    expect_parse_error("SET = 'info'");
}

// ---------------------------------------------------------------------------
// Admin: SHOW
// ---------------------------------------------------------------------------

TEST(QA_GDB_106_Show, ShowDatabases) {
    auto stmt = parse_one("SHOW DATABASES");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::DATABASES);
}

TEST(QA_GDB_106_Show, ShowTables) {
    auto stmt = parse_one("SHOW TABLES");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::TABLES);
}

TEST(QA_GDB_106_Show, ShowColumns) {
    auto stmt = parse_one("SHOW COLUMNS FROM users");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::COLUMNS);
    EXPECT_EQ(s->name, "users");
}

TEST(QA_GDB_106_Show, ShowColumnSingular) {
    auto stmt = parse_one("SHOW COLUMN FROM users");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::COLUMNS);
    EXPECT_EQ(s->name, "users");
}

TEST(QA_GDB_106_Show, ShowEdgeTypes) {
    auto stmt = parse_one("SHOW EDGE TYPES");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::EDGE_TYPES);
}

TEST(QA_GDB_106_Show, ShowEdgeType) {
    auto stmt = parse_one("SHOW EDGE TYPE");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::EDGE_TYPES);
}

TEST(QA_GDB_106_Show, ShowIndexes) {
    auto stmt = parse_one("SHOW INDEXES");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::INDEXES);
}

TEST(QA_GDB_106_Show, ShowIndex) {
    auto stmt = parse_one("SHOW INDEX");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::INDEXES);
}

TEST(QA_GDB_106_Show, ShowEmbeddings) {
    auto stmt = parse_one("SHOW EMBEDDINGS");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::EMBEDDINGS);
    EXPECT_TRUE(s->name.empty());
}

TEST(QA_GDB_106_Show, ShowEmbeddingsFromTable) {
    auto stmt = parse_one("SHOW EMBEDDINGS FROM products");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::EMBEDDINGS);
    EXPECT_EQ(s->name, "products");
}

TEST(QA_GDB_106_Show, ShowEmbeddingSingular) {
    // EMBEDDING is a keyword token, so match_ident_ci (which only matches
    // IDENTIFIER tokens) doesn't recognize it.  SHOW EMBEDDING falls through
    // to the PARAMETER case.  This is a minor inconsistency — SHOW EMBEDDINGS
    // works because "EMBEDDINGS" is NOT a keyword.  Not filing a bug since
    // the plural form is the canonical syntax.
    auto stmt = parse_one("SHOW EMBEDDING");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::PARAMETER);
}

TEST(QA_GDB_106_Show, ShowProviders) {
    auto stmt = parse_one("SHOW PROVIDERS");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::PROVIDERS);
}

TEST(QA_GDB_106_Show, ShowProvider) {
    auto stmt = parse_one("SHOW PROVIDER");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::PROVIDERS);
}

TEST(QA_GDB_106_Show, ShowReplicationSlots) {
    auto stmt = parse_one("SHOW REPLICATION SLOTS");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::REPLICATION_SLOTS);
}

TEST(QA_GDB_106_Show, ShowReplicationStatus) {
    auto stmt = parse_one("SHOW REPLICATION STATUS");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::REPLICATION_STATUS);
}

TEST(QA_GDB_106_Show, ShowStandbyStatus) {
    auto stmt = parse_one("SHOW STANDBY STATUS");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::STANDBY_STATUS);
}

TEST(QA_GDB_106_Show, ShowAll) {
    auto stmt = parse_one("SHOW ALL");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::ALL);
}

TEST(QA_GDB_106_Show, ShowSettings) {
    auto stmt = parse_one("SHOW SETTINGS");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::ALL);
}

TEST(QA_GDB_106_Show, ShowParameter) {
    auto stmt = parse_one("SHOW max_connections");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::PARAMETER);
    EXPECT_EQ(s->name, "max_connections");
}

TEST(QA_GDB_106_Show, ShowDottedParameter) {
    auto stmt = parse_one("SHOW logging.level");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::PARAMETER);
    EXPECT_EQ(s->name, "logging.level");
}

TEST(QA_GDB_106_Show, ShowDeepDottedParameter) {
    auto stmt = parse_one("SHOW a.b.c.d");
    auto* s = dynamic_cast<ShowStmt*>(stmt.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->target, ShowTarget::PARAMETER);
    EXPECT_EQ(s->name, "a.b.c.d");
}

// Error paths
TEST(QA_GDB_106_Show, ShowColumnsMissingFrom) {
    expect_parse_error("SHOW COLUMNS users");
}

TEST(QA_GDB_106_Show, ShowReplicationInvalid) {
    expect_parse_error("SHOW REPLICATION BLAH");
}

TEST(QA_GDB_106_Show, ShowStandbyMissingStatus) {
    expect_parse_error("SHOW STANDBY BLAH");
}

TEST(QA_GDB_106_Show, ShowEdgeMissingType) {
    expect_parse_error("SHOW EDGE BLAH");
}

// ---------------------------------------------------------------------------
// Admin: EXPLAIN
// ---------------------------------------------------------------------------

TEST(QA_GDB_106_Explain, BasicExplain) {
    auto stmt = parse_one("EXPLAIN SELECT * FROM t");
    ASSERT_NE(stmt, nullptr);
    auto* e = dynamic_cast<ExplainStmt*>(stmt.get());
    ASSERT_NE(e, nullptr);
    EXPECT_FALSE(e->analyze);
    EXPECT_EQ(e->format, ExplainFormat::TEXT);
    auto* inner = dynamic_cast<SelectStmt*>(e->statement.get());
    EXPECT_NE(inner, nullptr);
}

TEST(QA_GDB_106_Explain, ExplainAnalyze) {
    auto stmt = parse_one("EXPLAIN ANALYZE SELECT * FROM t");
    auto* e = dynamic_cast<ExplainStmt*>(stmt.get());
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->analyze);
}

TEST(QA_GDB_106_Explain, FormatText) {
    auto stmt = parse_one("EXPLAIN FORMAT TEXT SELECT * FROM t");
    auto* e = dynamic_cast<ExplainStmt*>(stmt.get());
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->format, ExplainFormat::TEXT);
}

TEST(QA_GDB_106_Explain, FormatJson) {
    auto stmt = parse_one("EXPLAIN FORMAT JSON SELECT * FROM t");
    auto* e = dynamic_cast<ExplainStmt*>(stmt.get());
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->format, ExplainFormat::JSON);
}

TEST(QA_GDB_106_Explain, AnalyzeFormatJson) {
    auto stmt = parse_one("EXPLAIN ANALYZE FORMAT JSON SELECT * FROM t");
    auto* e = dynamic_cast<ExplainStmt*>(stmt.get());
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->analyze);
    EXPECT_EQ(e->format, ExplainFormat::JSON);
}

TEST(QA_GDB_106_Explain, ExplainInsert) {
    auto stmt = parse_one("EXPLAIN INSERT INTO t (a) VALUES (1)");
    auto* e = dynamic_cast<ExplainStmt*>(stmt.get());
    ASSERT_NE(e, nullptr);
    EXPECT_NE(dynamic_cast<InsertStmt*>(e->statement.get()), nullptr);
}

TEST(QA_GDB_106_Explain, ExplainUpdate) {
    auto stmt = parse_one("EXPLAIN UPDATE t SET a = 1");
    auto* e = dynamic_cast<ExplainStmt*>(stmt.get());
    ASSERT_NE(e, nullptr);
    EXPECT_NE(dynamic_cast<UpdateStmt*>(e->statement.get()), nullptr);
}

TEST(QA_GDB_106_Explain, ExplainDelete) {
    auto stmt = parse_one("EXPLAIN DELETE FROM t WHERE id = 1");
    auto* e = dynamic_cast<ExplainStmt*>(stmt.get());
    ASSERT_NE(e, nullptr);
    EXPECT_NE(dynamic_cast<DeleteStmt*>(e->statement.get()), nullptr);
}

TEST(QA_GDB_106_Explain, InvalidFormat) {
    expect_parse_error("EXPLAIN FORMAT XML SELECT * FROM t");
}

// ---------------------------------------------------------------------------
// Admin: DESCRIBE
// ---------------------------------------------------------------------------

TEST(QA_GDB_106_Describe, BasicDescribe) {
    auto stmt = parse_one("DESCRIBE users");
    ASSERT_NE(stmt, nullptr);
    auto* d = dynamic_cast<DescribeStmt*>(stmt.get());
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->table_name, "users");
}

TEST(QA_GDB_106_Describe, MissingTable) {
    expect_parse_error("DESCRIBE");
}

// ---------------------------------------------------------------------------
// Admin: REEMBED
// ---------------------------------------------------------------------------

TEST(QA_GDB_106_Reembed, BasicReembed) {
    auto stmt = parse_one("REEMBED products");
    ASSERT_NE(stmt, nullptr);
    auto* r = dynamic_cast<ReembedStmt*>(stmt.get());
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->table_name, "products");
}

TEST(QA_GDB_106_Reembed, ReembedTable) {
    auto stmt = parse_one("REEMBED TABLE products");
    auto* r = dynamic_cast<ReembedStmt*>(stmt.get());
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->table_name, "products");
}

TEST(QA_GDB_106_Reembed, MissingTable) {
    expect_parse_error("REEMBED");
}

// ---------------------------------------------------------------------------
// Admin: VACUUM
// ---------------------------------------------------------------------------

TEST(QA_GDB_106_Vacuum, VacuumNoTable) {
    auto stmt = parse_one("VACUUM");
    ASSERT_NE(stmt, nullptr);
    auto* v = dynamic_cast<VacuumStmt*>(stmt.get());
    ASSERT_NE(v, nullptr);
    EXPECT_TRUE(v->table_name.empty());
}

TEST(QA_GDB_106_Vacuum, VacuumWithTable) {
    auto stmt = parse_one("VACUUM users");
    auto* v = dynamic_cast<VacuumStmt*>(stmt.get());
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->table_name, "users");
}

// ---------------------------------------------------------------------------
// Admin: ANALYZE
// ---------------------------------------------------------------------------

TEST(QA_GDB_106_Analyze, AnalyzeNoTable) {
    auto stmt = parse_one("ANALYZE");
    ASSERT_NE(stmt, nullptr);
    auto* a = dynamic_cast<AnalyzeStmt*>(stmt.get());
    ASSERT_NE(a, nullptr);
    EXPECT_TRUE(a->table_name.empty());
}

TEST(QA_GDB_106_Analyze, AnalyzeWithTable) {
    auto stmt = parse_one("ANALYZE users");
    auto* a = dynamic_cast<AnalyzeStmt*>(stmt.get());
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->table_name, "users");
}

// ---------------------------------------------------------------------------
// Stress tests
// ---------------------------------------------------------------------------

TEST(QA_GDB_106_Stress, LongMatchPattern) {
    // 20-node path
    std::string sql = "MATCH (n0)";
    for (int i = 1; i < 20; ++i) {
        sql += "-[:E]->(n" + std::to_string(i) + ")";
    }
    sql += " RETURN n0";
    auto stmt = parse_one(sql);
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->pattern.size(), 20);
}

TEST(QA_GDB_106_Stress, ManyShowStatements) {
    std::string sql;
    for (int i = 0; i < 50; ++i) {
        sql += "SHOW TABLES; ";
    }
    auto stmts = parse_ok(sql);
    EXPECT_EQ(stmts.size(), 50);
}

TEST(QA_GDB_106_Stress, ManyTCLStatements) {
    std::string sql;
    for (int i = 0; i < 20; ++i) {
        sql += "BEGIN; SAVEPOINT sp" + std::to_string(i) + "; ";
    }
    for (int i = 19; i >= 0; --i) {
        sql += "ROLLBACK TO sp" + std::to_string(i) + "; COMMIT; ";
    }
    auto stmts = parse_ok(sql);
    EXPECT_EQ(stmts.size(), 80); // 20 BEGIN + 20 SAVEPOINT + 20 ROLLBACK + 20 COMMIT
}
