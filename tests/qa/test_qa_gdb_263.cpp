/// @file test_qa_gdb_263.cpp
/// QA adversarial tests for GDB-263: Parser — TRAVERSE as a FROM source.
///
/// Verifies that TRAVERSE can appear as a table-valued source in FROM clauses,
/// aliases work correctly, all TRAVERSE clauses function inside FROM, standalone
/// TRAVERSE remains backward-compatible, and parser errors are clear.

#include "giodb/parser/lexer.h"
#include "giodb/parser/parser.h"

#include <gtest/gtest.h>

using namespace giodb;

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

static void expect_parse_error(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return; // lexer error is also acceptable
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_FALSE(stmts.has_value()) << "expected parse error for: " << sql;
}

/// Helper to extract SelectStmt from a parsed statement.
static SelectStmt* as_select(const StmtPtr& stmt) {
    return dynamic_cast<SelectStmt*>(stmt.get());
}

/// Helper to extract TraverseStmt from a TableRef's traverse_source.
static TraverseStmt* as_traverse(const TableRef& ref) {
    return dynamic_cast<TraverseStmt*>(ref.traverse_source.get());
}

// =============================================================================
// AC1: SELECT * FROM TRAVERSE parses into SelectStmt with TableRef containing
//      traverse_source TraverseStmt
// =============================================================================

TEST(QA_GDB263, AC1_BasicTraverseInFrom) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);

    // TableRef must have traverse_source set, name empty, no subquery.
    ASSERT_NE(sel->from[0].traverse_source, nullptr);
    EXPECT_TRUE(sel->from[0].name.empty());
    EXPECT_EQ(sel->from[0].subquery, nullptr);

    // TraverseStmt fields must be correct.
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "follows");
    EXPECT_EQ(tr->from_table, "users");
    EXPECT_EQ(tr->direction, TraverseDirection::OUT);
}

TEST(QA_GDB263, AC1_TraverseSourceMutuallyExclusiveWithTableName) {
    // When traverse_source is set, name must be empty.
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_TRUE(sel->from[0].name.empty())
        << "name should be empty when traverse_source is set, got: " << sel->from[0].name;
    EXPECT_NE(sel->from[0].traverse_source, nullptr);
}

TEST(QA_GDB263, AC1_TraverseSourceMutuallyExclusiveWithSubquery) {
    // When traverse_source is set, subquery must be null.
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].subquery, nullptr)
        << "subquery should be null when traverse_source is set";
}

// =============================================================================
// AC2: TRAVERSE with alias — explicit AS and implicit
// =============================================================================

TEST(QA_GDB263, AC2_ExplicitAlias) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT AS t");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].alias, "t");
    ASSERT_NE(as_traverse(sel->from[0]), nullptr);
    EXPECT_EQ(as_traverse(sel->from[0])->edge_type, "follows");
}

TEST(QA_GDB263, AC2_ImplicitAlias) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT trav");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].alias, "trav");
}

TEST(QA_GDB263, AC2_NoAlias) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_TRUE(sel->from[0].alias.empty()) << "alias should be empty when not provided";
}

TEST(QA_GDB263, AC2_MultiWordAliasIsNotAllowed) {
    // Only one identifier should be consumed as alias. Second word starts a
    // new clause or fails. "foo bar" after TRAVERSE should not both be alias.
    // If the query is "SELECT * FROM TRAVERSE ... AS foo bar", the "bar" should
    // cause an error or be ignored (depending on context).
    expect_parse_error("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT AS foo bar");
}

// =============================================================================
// AC3: All existing TRAVERSE clauses work inside FROM
// =============================================================================

TEST(QA_GDB263, AC3_DirectionOut) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->direction, TraverseDirection::OUT);
}

TEST(QA_GDB263, AC3_DirectionIn) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION IN");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->direction, TraverseDirection::IN);
}

TEST(QA_GDB263, AC3_DirectionBoth) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION BOTH");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->direction, TraverseDirection::BOTH);
}

TEST(QA_GDB263, AC3_MaxDepth) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) MAX_DEPTH 10");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    ASSERT_TRUE(tr->max_depth.has_value());
    EXPECT_EQ(*tr->max_depth, 10);
}

TEST(QA_GDB263, AC3_Where) {
    auto stmt = parse_one(
        "SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT WHERE weight > 0.5");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_NE(tr->where_expr, nullptr);
}

TEST(QA_GDB263, AC3_Fetch) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT FETCH");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_TRUE(tr->fetch);
}

TEST(QA_GDB263, AC3_AllClausesCombined) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE knows FROM users(42) DIRECTION BOTH "
                          "MAX_DEPTH 5 WHERE weight > 0.5 FETCH AS t");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].alias, "t");

    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "knows");
    EXPECT_EQ(tr->from_table, "users");
    EXPECT_EQ(tr->direction, TraverseDirection::BOTH);
    ASSERT_TRUE(tr->max_depth.has_value());
    EXPECT_EQ(*tr->max_depth, 5);
    EXPECT_NE(tr->where_expr, nullptr);
    EXPECT_TRUE(tr->fetch);
}

TEST(QA_GDB263, AC3_NoDirectionDefaultsOut) {
    // When DIRECTION is omitted, default is OUT per TraverseStmt declaration.
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1)");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->direction, TraverseDirection::OUT);
}

TEST(QA_GDB263, AC3_NoMaxDepthIsNullopt) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_FALSE(tr->max_depth.has_value());
}

TEST(QA_GDB263, AC3_FetchDefaultFalse) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_FALSE(tr->fetch);
}

// =============================================================================
// AC4: Standalone TRAVERSE — backward compatibility
// =============================================================================

TEST(QA_GDB263, AC4_StandaloneTraverseStillWorks) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1) DIRECTION OUT");
    auto* tr = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "follows");
    EXPECT_EQ(tr->from_table, "users");
    EXPECT_EQ(tr->direction, TraverseDirection::OUT);
}

TEST(QA_GDB263, AC4_StandaloneTraverseWithAllClauses) {
    auto stmt =
        parse_one("TRAVERSE knows FROM users(42) DIRECTION BOTH MAX_DEPTH 3 WHERE w > 0 FETCH");
    auto* tr = dynamic_cast<TraverseStmt*>(stmt.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "knows");
    EXPECT_EQ(tr->from_table, "users");
    EXPECT_EQ(tr->direction, TraverseDirection::BOTH);
    ASSERT_TRUE(tr->max_depth.has_value());
    EXPECT_EQ(*tr->max_depth, 3);
    EXPECT_NE(tr->where_expr, nullptr);
    EXPECT_TRUE(tr->fetch);
}

TEST(QA_GDB263, AC4_StandaloneVsWrappedProduceDifferentAST) {
    // Standalone: top-level is TraverseStmt.
    auto standalone = parse_one("TRAVERSE follows FROM users(1) DIRECTION OUT");
    EXPECT_NE(dynamic_cast<TraverseStmt*>(standalone.get()), nullptr);

    // Wrapped: top-level is SelectStmt, TraverseStmt nested in from[0].traverse_source.
    auto wrapped = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    EXPECT_NE(dynamic_cast<SelectStmt*>(wrapped.get()), nullptr);
    EXPECT_EQ(dynamic_cast<TraverseStmt*>(wrapped.get()), nullptr);
}

// =============================================================================
// AC5: Parser errors are clear when TRAVERSE syntax is malformed inside FROM
// =============================================================================

TEST(QA_GDB263, AC5_MissingEdgeType) {
    expect_parse_error("SELECT * FROM TRAVERSE FROM users(1)");
}

TEST(QA_GDB263, AC5_MissingFromKeyword) {
    expect_parse_error("SELECT * FROM TRAVERSE follows users(1)");
}

TEST(QA_GDB263, AC5_MissingKeyExpression) {
    expect_parse_error("SELECT * FROM TRAVERSE follows FROM users");
}

TEST(QA_GDB263, AC5_MissingClosingParen) {
    expect_parse_error("SELECT * FROM TRAVERSE follows FROM users(1");
}

TEST(QA_GDB263, AC5_MissingOpeningParen) {
    expect_parse_error("SELECT * FROM TRAVERSE follows FROM users 1)");
}

TEST(QA_GDB263, AC5_InvalidDirection) {
    expect_parse_error("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION UPWARD");
}

TEST(QA_GDB263, AC5_MaxDepthNonInteger) {
    expect_parse_error("SELECT * FROM TRAVERSE follows FROM users(1) MAX_DEPTH abc");
}

TEST(QA_GDB263, AC5_EmptyTraverseInsideFrom) {
    // TRAVERSE with nothing after it.
    expect_parse_error("SELECT * FROM TRAVERSE");
}

TEST(QA_GDB263, AC5_TraverseWithoutTableInFrom) {
    expect_parse_error("SELECT * FROM TRAVERSE follows FROM (1)");
}

// =============================================================================
// Adversarial: Alias boundary — clause keywords should NOT be consumed as alias
// =============================================================================

TEST(QA_GDB263, WhereAmbiguity_GreedilyConsumedByTraverse) {
    // BUG/AMBIGUITY: When TRAVERSE appears in FROM without FETCH or alias
    // terminating it, the WHERE clause is greedily consumed by TRAVERSE,
    // leaving the outer SELECT with no WHERE.
    //
    // In standard SQL, "SELECT * FROM <source> WHERE ..." always puts
    // WHERE on the SELECT. But TRAVERSE's grammar also accepts WHERE,
    // creating ambiguity. The parser gives WHERE to TRAVERSE.
    //
    // This test documents the current behavior (WHERE goes to TRAVERSE).
    auto stmt =
        parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT WHERE 1 = 1");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);

    // WHERE was consumed by TRAVERSE, not by SELECT.
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_NE(tr->where_expr, nullptr) << "TRAVERSE should have consumed the WHERE";
    EXPECT_EQ(sel->where_expr, nullptr)
        << "SELECT WHERE is null because TRAVERSE consumed it (ambiguity)";
}

TEST(QA_GDB263, WhereAmbiguity_WorkaroundWithFetch) {
    // Workaround: use FETCH to terminate TRAVERSE context, then outer WHERE
    // goes to SELECT.
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                          "FETCH AS t WHERE t.id = 1");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->where_expr, nullptr) << "TRAVERSE WHERE should be null (no inner WHERE)";
    EXPECT_TRUE(tr->fetch);
    EXPECT_NE(sel->where_expr, nullptr) << "Outer SELECT WHERE should be set";
}

TEST(QA_GDB263, WhereAmbiguity_WorkaroundWithAlias) {
    // Workaround: use alias to terminate TRAVERSE context.
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                          "AS t WHERE t.id = 1");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->from[0].alias, "t");
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->where_expr, nullptr) << "TRAVERSE WHERE should be null";
    EXPECT_NE(sel->where_expr, nullptr) << "Outer SELECT WHERE should be set";
}

TEST(QA_GDB263, AliasNotConsumedForJOIN) {
    // "JOIN" after TRAVERSE should start a JOIN, not be an alias.
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                          "JOIN other_table ON other_table.id = 1");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_TRUE(sel->from[0].alias.empty());
    ASSERT_EQ(sel->joins.size(), 1u);
}

TEST(QA_GDB263, AliasNotConsumedForORDER) {
    auto stmt = parse_one(
        "SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT ORDER BY 1");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_TRUE(sel->from[0].alias.empty());
    EXPECT_FALSE(sel->order_by.empty());
}

TEST(QA_GDB263, AliasNotConsumedForLIMIT) {
    auto stmt =
        parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT LIMIT 10");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_TRUE(sel->from[0].alias.empty());
    EXPECT_NE(sel->limit, nullptr);
}

TEST(QA_GDB263, AliasNotConsumedForGROUP) {
    auto stmt = parse_one(
        "SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT GROUP BY 1");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_TRUE(sel->from[0].alias.empty());
    EXPECT_FALSE(sel->group_by.empty());
}

// =============================================================================
// Adversarial: Multiple FROM sources (comma-separated)
// =============================================================================

TEST(QA_GDB263, TraverseAsSecondFromSource) {
    // FROM table, TRAVERSE ... — TRAVERSE appears after a regular table.
    auto stmt =
        parse_one("SELECT * FROM other_table, TRAVERSE follows FROM users(1) DIRECTION OUT");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 2u);

    // First source is a regular table.
    EXPECT_EQ(sel->from[0].name, "other_table");
    EXPECT_EQ(sel->from[0].traverse_source, nullptr);

    // Second source is TRAVERSE.
    EXPECT_TRUE(sel->from[1].name.empty());
    ASSERT_NE(sel->from[1].traverse_source, nullptr);
    auto* tr = as_traverse(sel->from[1]);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "follows");
}

TEST(QA_GDB263, TraverseAsFirstWithTrailingTable) {
    // FROM TRAVERSE ..., table — TRAVERSE first, then a regular table.
    auto stmt = parse_one(
        "SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT AS t, other_table");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 2u);

    // First source is TRAVERSE with alias.
    ASSERT_NE(sel->from[0].traverse_source, nullptr);
    EXPECT_EQ(sel->from[0].alias, "t");

    // Second source is regular table.
    EXPECT_EQ(sel->from[1].name, "other_table");
}

TEST(QA_GDB263, TwoTraverseSources) {
    // FROM TRAVERSE ..., TRAVERSE ... — two TRAVERSE sources.
    auto stmt =
        parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT AS t1, "
                  "TRAVERSE knows FROM users(2) DIRECTION IN AS t2");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 2u);

    auto* tr1 = as_traverse(sel->from[0]);
    ASSERT_NE(tr1, nullptr);
    EXPECT_EQ(tr1->edge_type, "follows");
    EXPECT_EQ(sel->from[0].alias, "t1");

    auto* tr2 = as_traverse(sel->from[1]);
    ASSERT_NE(tr2, nullptr);
    EXPECT_EQ(tr2->edge_type, "knows");
    EXPECT_EQ(sel->from[1].alias, "t2");
}

// =============================================================================
// Adversarial: TRAVERSE in JOIN clause
// =============================================================================

TEST(QA_GDB263, TraverseInJoinClause) {
    // TRAVERSE can appear as the table in a JOIN since parse_table_ref is used.
    auto stmt = parse_one("SELECT * FROM users "
                          "JOIN TRAVERSE follows FROM users(1) DIRECTION OUT AS t ON t.id = 1");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].name, "users");

    ASSERT_EQ(sel->joins.size(), 1u);
    ASSERT_NE(sel->joins[0].table.traverse_source, nullptr);
    auto* tr = dynamic_cast<TraverseStmt*>(sel->joins[0].table.traverse_source.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "follows");
    EXPECT_EQ(sel->joins[0].table.alias, "t");
}

TEST(QA_GDB263, TraverseInLeftJoin) {
    auto stmt =
        parse_one("SELECT * FROM users "
                  "LEFT JOIN TRAVERSE follows FROM users(1) DIRECTION OUT AS t ON t.id = 1");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->joins.size(), 1u);
    EXPECT_EQ(sel->joins[0].type, JoinType::LEFT);
    ASSERT_NE(sel->joins[0].table.traverse_source, nullptr);
}

// =============================================================================
// Adversarial: Key expression edge cases
// =============================================================================

TEST(QA_GDB263, KeyExpressionIsArithmetic) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1 + 2) DIRECTION OUT");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_NE(tr->from_key, nullptr);
}

TEST(QA_GDB263, KeyExpressionIsString) {
    auto stmt =
        parse_one("SELECT * FROM TRAVERSE follows FROM users('abc-uuid') DIRECTION OUT");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_NE(tr->from_key, nullptr);
}

TEST(QA_GDB263, KeyExpressionIsNegative) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(-1) DIRECTION OUT");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_NE(tr->from_key, nullptr);
}

TEST(QA_GDB263, KeyExpressionIsZero) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(0) DIRECTION OUT");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_NE(tr->from_key, nullptr);
}

// =============================================================================
// Adversarial: MAX_DEPTH edge cases
// =============================================================================

TEST(QA_GDB263, MaxDepthZero) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) MAX_DEPTH 0");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    ASSERT_TRUE(tr->max_depth.has_value());
    EXPECT_EQ(*tr->max_depth, 0);
}

TEST(QA_GDB263, MaxDepthOne) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) MAX_DEPTH 1");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    ASSERT_TRUE(tr->max_depth.has_value());
    EXPECT_EQ(*tr->max_depth, 1);
}

TEST(QA_GDB263, MaxDepthLargeValue) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) MAX_DEPTH 999999");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    ASSERT_TRUE(tr->max_depth.has_value());
    EXPECT_EQ(*tr->max_depth, 999999);
}

TEST(QA_GDB263, MaxDepthNegativeValue) {
    // safe_stoi should parse -1 from lexer. The lexer sees '-' and '1' as
    // separate tokens, so "MAX_DEPTH -1" likely fails since it expects
    // INTEGER_LITERAL and gets MINUS.
    expect_parse_error("SELECT * FROM TRAVERSE follows FROM users(1) MAX_DEPTH -1");
}

TEST(QA_GDB263, MaxDepthFloatNotAllowed) {
    expect_parse_error("SELECT * FROM TRAVERSE follows FROM users(1) MAX_DEPTH 3.5");
}

// =============================================================================
// Adversarial: WHERE clause inside TRAVERSE vs outer SELECT WHERE
// =============================================================================

TEST(QA_GDB263, TraverseWhereDoesNotConsumeOuterWhere) {
    // This is tricky: TRAVERSE has its own WHERE, and the SELECT can also
    // have WHERE. We need to verify both parse correctly.
    // TRAVERSE's WHERE should consume until FETCH or end of TRAVERSE context.
    // The outer WHERE then belongs to the SELECT.
    //
    // With explicit alias: ... FETCH AS t WHERE t.x = 1
    auto stmt =
        parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                  "WHERE depth < 3 FETCH AS t WHERE t.id = 1");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);

    // The inner TRAVERSE WHERE should be set.
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_NE(tr->where_expr, nullptr);
    EXPECT_TRUE(tr->fetch);

    // The outer SELECT WHERE should also be set.
    EXPECT_NE(sel->where_expr, nullptr);
}

// =============================================================================
// Adversarial: Interaction with SELECT column list
// =============================================================================

TEST(QA_GDB263, SelectSpecificColumnsFromTraverse) {
    auto stmt =
        parse_one("SELECT a, b, c FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->items.size(), 3u);
    ASSERT_NE(as_traverse(sel->from[0]), nullptr);
}

TEST(QA_GDB263, SelectWithAliasedColumnsFromTraverse) {
    auto stmt = parse_one("SELECT t.id, t.name FROM TRAVERSE follows FROM users(1) "
                          "DIRECTION OUT AS t");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->items.size(), 2u);
    EXPECT_EQ(sel->from[0].alias, "t");
}

// =============================================================================
// Adversarial: Multi-statement parsing
// =============================================================================

TEST(QA_GDB263, TraverseInFromFollowedBySemicolon) {
    auto stmts = parse_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT;");
    ASSERT_EQ(stmts.size(), 1u);
    auto* sel = dynamic_cast<SelectStmt*>(stmts[0].get());
    ASSERT_NE(sel, nullptr);
    ASSERT_NE(sel->from[0].traverse_source, nullptr);
}

TEST(QA_GDB263, TraverseInFromFollowedByAnotherStatement) {
    auto stmts = parse_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT; "
                          "SELECT 1");
    ASSERT_EQ(stmts.size(), 2u);
    auto* sel1 = dynamic_cast<SelectStmt*>(stmts[0].get());
    ASSERT_NE(sel1, nullptr);
    ASSERT_NE(sel1->from[0].traverse_source, nullptr);

    auto* sel2 = dynamic_cast<SelectStmt*>(stmts[1].get());
    ASSERT_NE(sel2, nullptr);
}

TEST(QA_GDB263, StandaloneTraverseFollowedBySelectWithTraverseInFrom) {
    auto stmts = parse_ok("TRAVERSE follows FROM users(1) DIRECTION OUT; "
                          "SELECT * FROM TRAVERSE knows FROM users(2) DIRECTION IN");
    ASSERT_EQ(stmts.size(), 2u);

    auto* tr = dynamic_cast<TraverseStmt*>(stmts[0].get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "follows");

    auto* sel = dynamic_cast<SelectStmt*>(stmts[1].get());
    ASSERT_NE(sel, nullptr);
    auto* tr2 = dynamic_cast<TraverseStmt*>(sel->from[0].traverse_source.get());
    ASSERT_NE(tr2, nullptr);
    EXPECT_EQ(tr2->edge_type, "knows");
}

// =============================================================================
// Adversarial: Interaction with UNION/INTERSECT/EXCEPT
// =============================================================================

TEST(QA_GDB263, TraverseInFromWithUnion) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                          "UNION SELECT * FROM other_table");
    // Should parse as a compound statement or two-part union.
    // The exact AST depends on how UNION is modeled. Just verify it parses.
    ASSERT_NE(stmt, nullptr);
}

// =============================================================================
// Adversarial: Edge type with unusual but valid names
// =============================================================================

TEST(QA_GDB263, EdgeTypeWithUnderscore) {
    auto stmt =
        parse_one("SELECT * FROM TRAVERSE has_parent FROM nodes(1) DIRECTION OUT");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "has_parent");
}

TEST(QA_GDB263, FromTableWithUnderscore) {
    auto stmt =
        parse_one("SELECT * FROM TRAVERSE follows FROM user_profiles(1) DIRECTION OUT");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->from_table, "user_profiles");
}

// =============================================================================
// Adversarial: TRAVERSE without DIRECTION inside FROM (should use default)
// =============================================================================

TEST(QA_GDB263, MinimalTraverseInFrom) {
    // Minimal valid TRAVERSE: just edge + FROM table(key).
    auto stmt = parse_one("SELECT * FROM TRAVERSE follows FROM users(1)");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    auto* tr = as_traverse(sel->from[0]);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "follows");
    EXPECT_EQ(tr->from_table, "users");
    EXPECT_EQ(tr->direction, TraverseDirection::OUT); // default
    EXPECT_FALSE(tr->max_depth.has_value());
    EXPECT_EQ(tr->where_expr, nullptr);
    EXPECT_FALSE(tr->fetch);
}

// =============================================================================
// Adversarial: Implicit alias boundary — comma should NOT be consumed as alias
// =============================================================================

TEST(QA_GDB263, ImplicitAliasStopsAtComma) {
    // "FROM TRAVERSE ... DIRECTION OUT, other" — the comma separates FROM
    // sources, should not be swallowed into alias detection.
    auto stmt = parse_one(
        "SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT, other_table");
    auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 2u);
    EXPECT_TRUE(sel->from[0].alias.empty());
    EXPECT_EQ(sel->from[1].name, "other_table");
}
