/// @file test_qa_gdb_693.cpp
/// @brief QA adversarial tests for GDB-693: accept TRACE as an identifier.
///
/// GDB-677 added the TRACE keyword to the lexer to support `WITH TRACE` on
/// TRAVERSE, but omitted TRACE from parser.cpp is_name_token(). That made the
/// bare word `trace` a fully reserved keyword, breaking previously-valid SQL
/// (CREATE TABLE trace, SELECT trace, etc.). GDB-693 restores `trace` as a
/// usable identifier by adding `case TokenType::TRACE:` to is_name_token().
///
/// These tests are adversarial: they verify not only that the four documented
/// statements parse, but that the resulting AST actually binds `trace` to the
/// right slot; that `trace` works in every other identifier position
/// (aliases, qualified names, JOIN/WHERE/ORDER BY/GROUP BY, DML column lists,
/// edge-type positions in LINK / CREATE EDGE TYPE / MATCH); and — most
/// importantly — that restoring the identifier did NOT break the GDB-677
/// `WITH TRACE` clause, including the maximally-confusing case where `trace`
/// is the edge type AND `WITH TRACE` is present in the same statement.

#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/parser/token.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace sixseven;

namespace {

// Parse `sql`, expecting exactly one successful statement. Returns nullptr (and
// records a gtest failure with the parser error message) otherwise.
StmtPtr parse_one(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    EXPECT_TRUE(tokens.has_value()) << "lex failed: " << sql;
    if (!tokens)
        return nullptr;
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_TRUE(stmts.has_value())
        << "parse failed for: " << sql << " -> " << (stmts ? "" : stmts.error().message);
    if (!stmts)
        return nullptr;
    EXPECT_EQ(stmts->size(), 1u) << "expected exactly one statement for: " << sql;
    if (stmts->size() != 1)
        return nullptr;
    return std::move((*stmts)[0]);
}

// Returns true if `sql` fails to lex or parse (a hard parse error).
bool is_parse_error(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return true; // lexer error counts as a parse failure
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    return !stmts.has_value();
}

const TraverseStmt* as_traverse(const StmtPtr& s) {
    return dynamic_cast<const TraverseStmt*>(s.get());
}
const SelectStmt* as_select(const StmtPtr& s) {
    return dynamic_cast<const SelectStmt*>(s.get());
}
const CreateTableStmt* as_create_table(const StmtPtr& s) {
    return dynamic_cast<const CreateTableStmt*>(s.get());
}

} // namespace

// =============================================================================
// AC 1-4: the four documented statements must parse AND bind `trace` correctly.
// =============================================================================

TEST(QA_GDB693_AcceptanceCriteria, TraceAsTableNameBindsToTableName) {
    auto stmt = parse_one("CREATE TABLE trace (id INT)");
    const auto* ct = as_create_table(stmt);
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->name, "trace");
    ASSERT_EQ(ct->columns.size(), 1u);
    EXPECT_EQ(ct->columns[0].name, "id");
}

TEST(QA_GDB693_AcceptanceCriteria, TraceAsColumnNameBindsToColumn) {
    auto stmt = parse_one("CREATE TABLE t (trace INT)");
    const auto* ct = as_create_table(stmt);
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->name, "t");
    ASSERT_EQ(ct->columns.size(), 1u);
    EXPECT_EQ(ct->columns[0].name, "trace");
    EXPECT_EQ(ct->columns[0].type.name, "INT");
}

TEST(QA_GDB693_AcceptanceCriteria, TraceAsSelectColumnBindsToColumnRef) {
    auto stmt = parse_one("SELECT trace FROM events");
    const auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    const auto* col = dynamic_cast<const ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "trace");
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].name, "events");
}

TEST(QA_GDB693_AcceptanceCriteria, TraceAsEdgeTypeBindsToEdgeType) {
    auto stmt = parse_one("TRAVERSE trace FROM users(1)");
    const auto* tr = as_traverse(stmt);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "trace");
    EXPECT_EQ(tr->from_table, "users");
    // No WITH TRACE clause present: the flag must stay false. This proves the
    // edge-type `trace` was NOT misread as a trace flag.
    EXPECT_FALSE(tr->trace);
}

// =============================================================================
// CRITICAL INTERACTION: `trace` as edge type WHILE `WITH TRACE` is present.
//
// This is the adversarial heart of the ticket. The edge type goes through
// parse_name() (which now accepts TRACE), and the trailing `WITH TRACE` is a
// distinct optional clause. Both `trace`s must be disambiguated by position.
// =============================================================================

TEST(QA_GDB693_Interaction, TraceEdgeTypeWithTraceClause) {
    // edge_type == "trace" AND trace flag == true, simultaneously.
    auto stmt = parse_one("TRAVERSE trace FROM users(1) WITH TRACE");
    const auto* tr = as_traverse(stmt);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "trace");
    EXPECT_EQ(tr->from_table, "users");
    EXPECT_TRUE(tr->trace);
    EXPECT_FALSE(tr->fetch);
}

TEST(QA_GDB693_Interaction, TraceEdgeTypeFetchWithTraceClause) {
    auto stmt = parse_one("TRAVERSE trace FROM users(1) FETCH WITH TRACE");
    const auto* tr = as_traverse(stmt);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "trace");
    EXPECT_TRUE(tr->fetch);
    EXPECT_TRUE(tr->trace);
}

TEST(QA_GDB693_Interaction, TraceEdgeTypeWithAllClausesAndWithTrace) {
    auto stmt = parse_one(
        "TRAVERSE trace FROM users(1) DIRECTION OUT MAX_DEPTH 3 MODE EDGES "
        "WHERE id > 0 FETCH WITH TRACE");
    const auto* tr = as_traverse(stmt);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "trace");
    ASSERT_TRUE(tr->max_depth.has_value());
    EXPECT_EQ(*tr->max_depth, 3);
    EXPECT_TRUE(tr->fetch);
    EXPECT_TRUE(tr->trace);
}

// `trace` edge type with NO WITH TRACE: flag stays false (regression guard for
// over-eager flag-setting on the edge-type token).
TEST(QA_GDB693_Interaction, TraceEdgeTypeNoClauseFlagFalse) {
    auto stmt = parse_one("TRAVERSE trace FROM users(1) DIRECTION IN");
    const auto* tr = as_traverse(stmt);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "trace");
    EXPECT_FALSE(tr->trace);
    EXPECT_EQ(tr->direction, TraverseDirection::IN);
}

// `trace` edge type inside a SELECT-FROM-TRAVERSE that also uses WITH TRACE.
TEST(QA_GDB693_Interaction, TraceEdgeTypeWithTraceInSelectFrom) {
    auto stmt = parse_one("SELECT * FROM TRAVERSE trace FROM users(1) WITH TRACE");
    const auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    ASSERT_NE(sel->from[0].traverse_source, nullptr);
    const auto* tr = dynamic_cast<const TraverseStmt*>(sel->from[0].traverse_source.get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "trace");
    EXPECT_TRUE(tr->trace);
}

// Case sensitivity: TRACE the edge type and TRACE the clause, all uppercase.
TEST(QA_GDB693_Interaction, UppercaseTraceEdgeTypeAndUppercaseWithTraceClause) {
    auto stmt = parse_one("TRAVERSE TRACE FROM users(1) WITH TRACE");
    const auto* tr = as_traverse(stmt);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "TRACE"); // lexeme preserved verbatim
    EXPECT_TRUE(tr->trace);
}

// Mixed case edge type, lowercase clause.
TEST(QA_GDB693_Interaction, MixedCaseTraceEdgeTypeLowercaseClause) {
    auto stmt = parse_one("TRAVERSE TrAcE FROM users(1) with trace");
    const auto* tr = as_traverse(stmt);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "TrAcE");
    EXPECT_TRUE(tr->trace);
}

// =============================================================================
// `trace` as the FROM table while WITH TRACE is also present.
// =============================================================================

TEST(QA_GDB693_Interaction, TraceAsFromTableWithTraceClause) {
    auto stmt = parse_one("TRAVERSE follows FROM trace(1) WITH TRACE");
    const auto* tr = as_traverse(stmt);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "follows");
    EXPECT_EQ(tr->from_table, "trace");
    EXPECT_TRUE(tr->trace);
}

// Maximal confusion: edge type `trace`, from table `trace`, WITH TRACE.
TEST(QA_GDB693_Interaction, TraceEdgeTraceTableWithTrace) {
    auto stmt = parse_one("TRAVERSE trace FROM trace(1) WITH TRACE");
    const auto* tr = as_traverse(stmt);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "trace");
    EXPECT_EQ(tr->from_table, "trace");
    EXPECT_TRUE(tr->trace);
}

// =============================================================================
// GDB-677 WITH TRACE error paths must STILL error (no regression to the
// feature from restoring the identifier).
// =============================================================================

// `WITH foo` is still an error: only TRACE may follow the modifier WITH.
TEST(QA_GDB693_NoRegression, WithNonTraceKeywordStillErrors) {
    EXPECT_TRUE(is_parse_error("TRAVERSE follows FROM users(1) WITH foo"));
}

// `WITH` with nothing after it is still an error.
TEST(QA_GDB693_NoRegression, WithNothingStillErrors) {
    EXPECT_TRUE(is_parse_error("TRAVERSE follows FROM users(1) WITH"));
}

// Bare trailing TRACE (no WITH) is still an error: it is a dangling token, not
// silently consumed as a second edge name or flag.
TEST(QA_GDB693_NoRegression, BareTrailingTraceStillErrors) {
    EXPECT_TRUE(is_parse_error("TRAVERSE follows FROM users(1) TRACE"));
}

// Doubled WITH TRACE WITH TRACE is still an error (second clause dangles).
TEST(QA_GDB693_NoRegression, DoubleWithTraceStillErrors) {
    EXPECT_TRUE(is_parse_error("TRAVERSE follows FROM users(1) WITH TRACE WITH TRACE"));
}

// Plain WITH TRACE on a non-`trace` edge type still sets the flag.
TEST(QA_GDB693_NoRegression, PlainWithTraceStillSetsFlag) {
    auto stmt = parse_one("TRAVERSE follows FROM users(1) WITH TRACE");
    const auto* tr = as_traverse(stmt);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "follows");
    EXPECT_TRUE(tr->trace);
}

// A statement-leading `WITH TRACE AS (...)` is a CTE context. TRACE is now a
// valid name token, so this must parse as a CTE named `trace` (previously it
// would have been rejected). This pins the new, correct behavior.
TEST(QA_GDB693_NoRegression, LeadingWithTraceAsCteNowParses) {
    auto stmt = parse_one("WITH trace AS (SELECT 1) SELECT * FROM trace");
    const auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->ctes.size(), 1u);
    EXPECT_EQ(sel->ctes[0].name, "trace");
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].name, "trace");
}

// =============================================================================
// `trace` in other identifier contexts: aliases.
// =============================================================================

TEST(QA_GDB693_OtherContexts, TraceAsExplicitColumnAlias) {
    auto stmt = parse_one("SELECT id AS trace FROM events");
    const auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    EXPECT_EQ(sel->items[0].alias, "trace");
}

TEST(QA_GDB693_OtherContexts, TraceAsImplicitColumnAlias) {
    auto stmt = parse_one("SELECT id trace FROM events");
    const auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    EXPECT_EQ(sel->items[0].alias, "trace");
}

TEST(QA_GDB693_OtherContexts, TraceAsExplicitTableAlias) {
    auto stmt = parse_one("SELECT t.id FROM events AS trace");
    const auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].name, "events");
    EXPECT_EQ(sel->from[0].alias, "trace");
}

TEST(QA_GDB693_OtherContexts, TraceAsImplicitTableAlias) {
    auto stmt = parse_one("SELECT id FROM events trace");
    const auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].name, "events");
    EXPECT_EQ(sel->from[0].alias, "trace");
}

// Column named trace, aliased to trace: `SELECT trace AS trace`.
TEST(QA_GDB693_OtherContexts, TraceColumnAliasedToTrace) {
    auto stmt = parse_one("SELECT trace AS trace FROM events");
    const auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    const auto* col = dynamic_cast<const ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "trace");
    EXPECT_EQ(sel->items[0].alias, "trace");
}

// =============================================================================
// `trace` in qualified names.
// =============================================================================

TEST(QA_GDB693_OtherContexts, TraceAsQualifiedColumn) {
    auto stmt = parse_one("SELECT t.trace FROM events t");
    const auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    const auto* col = dynamic_cast<const ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->table, "t");
    EXPECT_EQ(col->column, "trace");
}

// Both sides are `trace`: table qualifier `trace`, column `trace`.
TEST(QA_GDB693_OtherContexts, TraceQualifierAndTraceColumn) {
    auto stmt = parse_one("SELECT trace.trace FROM events trace");
    const auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    const auto* col = dynamic_cast<const ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->table, "trace");
    EXPECT_EQ(col->column, "trace");
}

TEST(QA_GDB693_OtherContexts, TraceDotStarSelectsTableStar) {
    auto stmt = parse_one("SELECT trace.* FROM events trace");
    const auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    EXPECT_TRUE(sel->items[0].is_star);
    EXPECT_EQ(sel->items[0].table_star, "trace");
}

// =============================================================================
// `trace` in JOIN / WHERE / ORDER BY / GROUP BY.
// =============================================================================

TEST(QA_GDB693_OtherContexts, TraceAsJoinedTableAndOnColumn) {
    auto stmt = parse_one("SELECT * FROM a JOIN trace ON a.id = trace.id");
    const auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->joins.size(), 1u);
    EXPECT_EQ(sel->joins[0].table.name, "trace");
    ASSERT_NE(sel->joins[0].on_expr, nullptr);
}

TEST(QA_GDB693_OtherContexts, TraceInWhereClause) {
    auto stmt = parse_one("SELECT * FROM events WHERE trace = 1");
    const auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_NE(sel->where_expr, nullptr);
    const auto* bin = dynamic_cast<const BinaryExpr*>(sel->where_expr.get());
    ASSERT_NE(bin, nullptr);
    const auto* lhs = dynamic_cast<const ColumnRefExpr*>(bin->lhs.get());
    ASSERT_NE(lhs, nullptr);
    EXPECT_EQ(lhs->column, "trace");
}

TEST(QA_GDB693_OtherContexts, TraceInOrderBy) {
    auto stmt = parse_one("SELECT id FROM events ORDER BY trace DESC");
    const auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->order_by.size(), 1u);
    const auto* col = dynamic_cast<const ColumnRefExpr*>(sel->order_by[0].expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "trace");
    EXPECT_EQ(sel->order_by[0].direction, SortDirection::DESC);
}

TEST(QA_GDB693_OtherContexts, TraceInGroupBy) {
    auto stmt = parse_one("SELECT trace, COUNT(*) FROM events GROUP BY trace");
    const auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->group_by.size(), 1u);
    const auto* col = dynamic_cast<const ColumnRefExpr*>(sel->group_by[0].get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "trace");
}

// =============================================================================
// `trace` in DML column lists.
// =============================================================================

TEST(QA_GDB693_OtherContexts, TraceInInsertColumnList) {
    auto stmt = parse_one("INSERT INTO events (trace, id) VALUES (1, 2)");
    const auto* ins = dynamic_cast<const InsertStmt*>(stmt.get());
    ASSERT_NE(ins, nullptr);
    ASSERT_EQ(ins->columns.size(), 2u);
    EXPECT_EQ(ins->columns[0], "trace");
    EXPECT_EQ(ins->columns[1], "id");
}

TEST(QA_GDB693_OtherContexts, TraceAsInsertTargetTable) {
    auto stmt = parse_one("INSERT INTO trace (id) VALUES (1)");
    const auto* ins = dynamic_cast<const InsertStmt*>(stmt.get());
    ASSERT_NE(ins, nullptr);
    EXPECT_EQ(ins->table_name, "trace");
    ASSERT_EQ(ins->columns.size(), 1u);
    EXPECT_EQ(ins->columns[0], "id");
}

TEST(QA_GDB693_OtherContexts, TraceInUpdateSetColumn) {
    auto stmt = parse_one("UPDATE events SET trace = 5 WHERE id = 1");
    const auto* upd = dynamic_cast<const UpdateStmt*>(stmt.get());
    ASSERT_NE(upd, nullptr);
    ASSERT_EQ(upd->assignments.size(), 1u);
    EXPECT_EQ(upd->assignments[0].column, "trace");
}

TEST(QA_GDB693_OtherContexts, TraceAsUpdateTargetTable) {
    auto stmt = parse_one("UPDATE trace SET id = 5");
    const auto* upd = dynamic_cast<const UpdateStmt*>(stmt.get());
    ASSERT_NE(upd, nullptr);
    EXPECT_EQ(upd->table_name, "trace");
}

TEST(QA_GDB693_OtherContexts, TraceAsDeleteTargetTable) {
    auto stmt = parse_one("DELETE FROM trace WHERE id = 1");
    const auto* del = dynamic_cast<const DeleteStmt*>(stmt.get());
    ASSERT_NE(del, nullptr);
    EXPECT_EQ(del->table_name, "trace");
}

// =============================================================================
// `trace` as an edge-type identifier in non-TRAVERSE graph statements.
// =============================================================================

TEST(QA_GDB693_OtherContexts, TraceAsCreateEdgeTypeName) {
    auto stmt = parse_one("CREATE EDGE TYPE trace FROM a TO b");
    const auto* ce = dynamic_cast<const CreateEdgeTypeStmt*>(stmt.get());
    ASSERT_NE(ce, nullptr);
    EXPECT_EQ(ce->name, "trace");
    EXPECT_EQ(ce->from_table, "a");
    EXPECT_EQ(ce->to_table, "b");
}

TEST(QA_GDB693_OtherContexts, TraceAsLinkEdgeType) {
    auto stmt = parse_one("LINK a(1) TO b(2) VIA trace");
    const auto* link = dynamic_cast<const LinkStmt*>(stmt.get());
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->edge_type, "trace");
    EXPECT_EQ(link->source_table, "a");
    EXPECT_EQ(link->target_table, "b");
}

// `trace` as the relationship type inside a MATCH pattern.
TEST(QA_GDB693_OtherContexts, TraceAsMatchEdgeType) {
    auto stmt = parse_one("MATCH (a)-[:trace]->(b) RETURN a");
    const auto* m = dynamic_cast<const MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    // Find the edge type in the path pattern.
    bool found_trace_edge = false;
    for (const auto& el : m->pattern) {
        if (el.outgoing_edge && el.outgoing_edge->edge_type == "trace") {
            found_trace_edge = true;
        }
    }
    EXPECT_TRUE(found_trace_edge);
}

// =============================================================================
// Confusable / boundary cases.
// =============================================================================

// `trace_id` must remain a single IDENTIFIER, never the keyword — confirms the
// fix did not widen the keyword's lexical footprint.
TEST(QA_GDB693_Boundary, TraceIdRemainsSingleIdentifier) {
    auto stmt = parse_one("SELECT trace_id FROM events");
    const auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    const auto* col = dynamic_cast<const ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "trace_id");
}

// Quoted string literal 'trace' is a value, not an identifier — unaffected.
TEST(QA_GDB693_Boundary, TraceStringLiteralIsValueNotIdentifier) {
    auto stmt = parse_one("SELECT id FROM events WHERE name = 'trace'");
    const auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_NE(sel->where_expr, nullptr);
    const auto* bin = dynamic_cast<const BinaryExpr*>(sel->where_expr.get());
    ASSERT_NE(bin, nullptr);
    const auto* rhs = dynamic_cast<const LiteralExpr*>(bin->rhs.get());
    ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(rhs->value, "trace");
}

// `trace` as both a column AND the table it lives in, in one SELECT.
TEST(QA_GDB693_Boundary, TraceColumnFromTraceTable) {
    auto stmt = parse_one("SELECT trace FROM trace");
    const auto* sel = as_select(stmt);
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    const auto* col = dynamic_cast<const ColumnRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "trace");
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].name, "trace");
}

// Many `trace` identifiers in one wide statement — stress the parser's handling
// of the now-unreserved word at scale.
TEST(QA_GDB693_Boundary, ManyTraceColumnsInWideCreateTable) {
    std::string sql = "CREATE TABLE trace (";
    const int kCols = 64;
    for (int i = 0; i < kCols; ++i) {
        if (i > 0)
            sql += ", ";
        sql += "trace" + std::to_string(i) + " INT";
    }
    sql += ")";
    auto stmt = parse_one(sql);
    const auto* ct = as_create_table(stmt);
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->name, "trace");
    ASSERT_EQ(ct->columns.size(), static_cast<size_t>(kCols));
    EXPECT_EQ(ct->columns[0].name, "trace0");
    EXPECT_EQ(ct->columns[kCols - 1].name, "trace63");
}

// Multi-statement batch mixing `trace`-as-identifier with WITH TRACE clauses;
// the per-statement state must not leak.
TEST(QA_GDB693_Boundary, MultiStatementMixedTraceUsage) {
    Lexer lexer(
        "CREATE TABLE trace (id INT); "
        "TRAVERSE trace FROM trace(1) WITH TRACE; "
        "SELECT trace FROM trace");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    ASSERT_TRUE(stmts.has_value()) << stmts.error().message;
    ASSERT_EQ(stmts->size(), 3u);
    EXPECT_NE(dynamic_cast<const CreateTableStmt*>((*stmts)[0].get()), nullptr);
    const auto* tr = dynamic_cast<const TraverseStmt*>((*stmts)[1].get());
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->edge_type, "trace");
    EXPECT_TRUE(tr->trace);
    EXPECT_NE(dynamic_cast<const SelectStmt*>((*stmts)[2].get()), nullptr);
}

// =============================================================================
// Lexer invariant: TRACE still lexes as the TRACE keyword token (the fix is
// purely in the parser, not the lexer — the token type must not have changed).
// =============================================================================

TEST(QA_GDB693_Lexer, TraceStillLexesAsTraceKeyword) {
    Lexer lexer("trace");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_FALSE(tokens->empty());
    EXPECT_EQ((*tokens)[0].type, TokenType::TRACE);
}
