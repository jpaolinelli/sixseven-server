/// QA adversarial tests for GDB-102: AST node types for all statement categories.
/// Tests visitor dispatch, default values, deep nesting, and edge cases.
/// GDB-756: vacuous assign-then-assert tests converted to parser round-trips.

#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

using namespace sixseven;

// =============================================================================
// Parser round-trip helpers (GDB-756)
// =============================================================================

static StmtPtr parse_one_102(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens) {
        ADD_FAILURE() << "Lex error: " << tokens.error().message;
        return nullptr;
    }
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    if (!stmts) {
        ADD_FAILURE() << "Parse error: " << stmts.error().message;
        return nullptr;
    }
    if (stmts->size() != 1u) {
        ADD_FAILURE() << "Expected 1 statement, got " << stmts->size();
        return nullptr;
    }
    return std::move((*stmts)[0]);
}

// =============================================================================
// Helper factories
// =============================================================================

static ExprPtr make_int(const std::string& v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::INTEGER;
    e->value = v;
    return e;
}

static ExprPtr make_str(const std::string& v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::STRING;
    e->value = v;
    return e;
}

static ExprPtr make_col(const std::string& col, const std::string& table = "") {
    auto e = std::make_unique<ColumnRefExpr>();
    e->table = table;
    e->column = col;
    return e;
}

// =============================================================================
// Counting visitor — verifies double-dispatch for ALL 47 node types
// =============================================================================

namespace {

class CountingVisitor : public AstVisitor {
public:
    std::string last_type;
    int total = 0;

    // -- Expressions --
    void visit(const LiteralExpr&) override {
        last_type = "LiteralExpr";
        ++total;
    }
    void visit(const ParamRefExpr&) override {
        last_type = "ParamRefExpr";
        ++total;
    }
    void visit(const ColumnRefExpr&) override {
        last_type = "ColumnRefExpr";
        ++total;
    }
    void visit(const BinaryExpr&) override {
        last_type = "BinaryExpr";
        ++total;
    }
    void visit(const UnaryExpr&) override {
        last_type = "UnaryExpr";
        ++total;
    }
    void visit(const FunctionCallExpr&) override {
        last_type = "FunctionCallExpr";
        ++total;
    }
    void visit(const CastExpr&) override {
        last_type = "CastExpr";
        ++total;
    }
    void visit(const CaseExpr&) override {
        last_type = "CaseExpr";
        ++total;
    }
    void visit(const InExpr&) override {
        last_type = "InExpr";
        ++total;
    }
    void visit(const BetweenExpr&) override {
        last_type = "BetweenExpr";
        ++total;
    }
    void visit(const IsNullExpr&) override {
        last_type = "IsNullExpr";
        ++total;
    }
    void visit(const LikeExpr&) override {
        last_type = "LikeExpr";
        ++total;
    }
    void visit(const MatchExpr&) override {
        last_type = "MatchExpr";
        ++total;
    }
    void visit(const NearestExpr&) override {
        last_type = "NearestExpr";
        ++total;
    }
    void visit(const ExistsExpr&) override {
        last_type = "ExistsExpr";
        ++total;
    }
    void visit(const SubqueryExpr&) override {
        last_type = "SubqueryExpr";
        ++total;
    }
    void visit(const ArrayExpr&) override {
        last_type = "ArrayExpr";
        ++total;
    }
    void visit(const WindowFunctionExpr&) override {
        last_type = "WindowFunctionExpr";
        ++total;
    }

    // -- DDL --
    void visit(const CreateTableStmt&) override {
        last_type = "CreateTableStmt";
        ++total;
    }
    void visit(const DropTableStmt&) override {
        last_type = "DropTableStmt";
        ++total;
    }
    void visit(const AlterTableStmt&) override {
        last_type = "AlterTableStmt";
        ++total;
    }
    void visit(const CreateIndexStmt&) override {
        last_type = "CreateIndexStmt";
        ++total;
    }
    void visit(const DropIndexStmt&) override {
        last_type = "DropIndexStmt";
        ++total;
    }
    void visit(const CreateEdgeTypeStmt&) override {
        last_type = "CreateEdgeTypeStmt";
        ++total;
    }
    void visit(const DropEdgeTypeStmt&) override {
        last_type = "DropEdgeTypeStmt";
        ++total;
    }
    void visit(const CreateDatabaseStmt&) override {
        last_type = "CreateDatabaseStmt";
        ++total;
    }
    void visit(const DropDatabaseStmt&) override {
        last_type = "DropDatabaseStmt";
        ++total;
    }
    void visit(const CreateUserStmt&) override {
        last_type = "CreateUserStmt";
        ++total;
    }
    void visit(const DropUserStmt&) override {
        last_type = "DropUserStmt";
        ++total;
    }
    void visit(const AlterUserStmt&) override {
        last_type = "AlterUserStmt";
        ++total;
    }

    // -- DML --
    void visit(const InsertStmt&) override {
        last_type = "InsertStmt";
        ++total;
    }
    void visit(const UpdateStmt&) override {
        last_type = "UpdateStmt";
        ++total;
    }
    void visit(const DeleteStmt&) override {
        last_type = "DeleteStmt";
        ++total;
    }
    void visit(const LinkStmt&) override {
        last_type = "LinkStmt";
        ++total;
    }
    void visit(const BulkLinkStmt&) override {
        last_type = "BulkLinkStmt";
        ++total;
    }
    void visit(const UnlinkStmt&) override {
        last_type = "UnlinkStmt";
        ++total;
    }

    // -- Query --
    void visit(const SelectStmt&) override {
        last_type = "SelectStmt";
        ++total;
    }
    void visit(const TraverseStmt&) override {
        last_type = "TraverseStmt";
        ++total;
    }
    void visit(const MatchStmt&) override {
        last_type = "MatchStmt";
        ++total;
    }
    void visit(const ShortestPathStmt&) override {
        last_type = "ShortestPathStmt";
        ++total;
    }

    // -- TCL --
    void visit(const BeginStmt&) override {
        last_type = "BeginStmt";
        ++total;
    }
    void visit(const CommitStmt&) override {
        last_type = "CommitStmt";
        ++total;
    }
    void visit(const RollbackStmt&) override {
        last_type = "RollbackStmt";
        ++total;
    }
    void visit(const SavepointStmt&) override {
        last_type = "SavepointStmt";
        ++total;
    }
    void visit(const ReleaseSavepointStmt&) override {
        last_type = "ReleaseSavepointStmt";
        ++total;
    }

    // -- Admin --
    void visit(const SetStmt&) override {
        last_type = "SetStmt";
        ++total;
    }
    void visit(const ShowStmt&) override {
        last_type = "ShowStmt";
        ++total;
    }
    void visit(const ExplainStmt&) override {
        last_type = "ExplainStmt";
        ++total;
    }
    void visit(const DescribeStmt&) override {
        last_type = "DescribeStmt";
        ++total;
    }
    void visit(const BackfillStmt&) override {
        last_type = "BackfillStmt";
        ++total;
    }
    void visit(const ReembedStmt&) override {
        last_type = "ReembedStmt";
        ++total;
    }
    void visit(const ReindexStmt&) override {
        last_type = "ReindexStmt";
        ++total;
    }
    void visit(const VacuumStmt&) override {
        last_type = "VacuumStmt";
        ++total;
    }
    void visit(const AnalyzeStmt&) override {
        last_type = "AnalyzeStmt";
        ++total;
    }
};

} // anonymous namespace

// =============================================================================
// Visitor dispatch: every expression type
// =============================================================================

TEST(QA_GDB102, VisitorDispatch_LiteralExpr) {
    CountingVisitor v;
    LiteralExpr e;
    e.kind = LiteralKind::INTEGER;
    e.value = "42";
    e.accept(v);
    EXPECT_EQ(v.last_type, "LiteralExpr");
    EXPECT_EQ(v.total, 1);
}

TEST(QA_GDB102, VisitorDispatch_ColumnRefExpr) {
    CountingVisitor v;
    ColumnRefExpr e;
    e.column = "id";
    e.accept(v);
    EXPECT_EQ(v.last_type, "ColumnRefExpr");
}

TEST(QA_GDB102, VisitorDispatch_BinaryExpr) {
    CountingVisitor v;
    BinaryExpr e;
    e.op = BinaryOp::ADD;
    e.lhs = make_int("1");
    e.rhs = make_int("2");
    e.accept(v);
    EXPECT_EQ(v.last_type, "BinaryExpr");
}

TEST(QA_GDB102, VisitorDispatch_UnaryExpr) {
    CountingVisitor v;
    UnaryExpr e;
    e.op = UnaryOp::NEGATE;
    e.operand = make_int("1");
    e.accept(v);
    EXPECT_EQ(v.last_type, "UnaryExpr");
}

TEST(QA_GDB102, VisitorDispatch_FunctionCallExpr) {
    CountingVisitor v;
    FunctionCallExpr e;
    e.name = "SUM";
    e.accept(v);
    EXPECT_EQ(v.last_type, "FunctionCallExpr");
}

TEST(QA_GDB102, VisitorDispatch_CastExpr) {
    CountingVisitor v;
    CastExpr e;
    e.expr = make_int("1");
    e.target_type.name = "VARCHAR";
    e.accept(v);
    EXPECT_EQ(v.last_type, "CastExpr");
}

TEST(QA_GDB102, VisitorDispatch_CaseExpr) {
    CountingVisitor v;
    CaseExpr e;
    e.accept(v);
    EXPECT_EQ(v.last_type, "CaseExpr");
}

TEST(QA_GDB102, VisitorDispatch_InExpr) {
    CountingVisitor v;
    InExpr e;
    e.expr = make_col("id");
    e.accept(v);
    EXPECT_EQ(v.last_type, "InExpr");
}

TEST(QA_GDB102, VisitorDispatch_BetweenExpr) {
    CountingVisitor v;
    BetweenExpr e;
    e.expr = make_col("age");
    e.low = make_int("0");
    e.high = make_int("100");
    e.accept(v);
    EXPECT_EQ(v.last_type, "BetweenExpr");
}

TEST(QA_GDB102, VisitorDispatch_IsNullExpr) {
    CountingVisitor v;
    IsNullExpr e;
    e.expr = make_col("x");
    e.accept(v);
    EXPECT_EQ(v.last_type, "IsNullExpr");
}

TEST(QA_GDB102, VisitorDispatch_LikeExpr) {
    CountingVisitor v;
    LikeExpr e;
    e.expr = make_col("name");
    e.pattern = make_str("%test%");
    e.accept(v);
    EXPECT_EQ(v.last_type, "LikeExpr");
}

TEST(QA_GDB102, VisitorDispatch_ExistsExpr) {
    CountingVisitor v;
    ExistsExpr e;
    e.subquery = std::make_unique<SelectStmt>();
    e.accept(v);
    EXPECT_EQ(v.last_type, "ExistsExpr");
}

TEST(QA_GDB102, VisitorDispatch_SubqueryExpr) {
    CountingVisitor v;
    SubqueryExpr e;
    e.subquery = std::make_unique<SelectStmt>();
    e.accept(v);
    EXPECT_EQ(v.last_type, "SubqueryExpr");
}

TEST(QA_GDB102, VisitorDispatch_ArrayExpr) {
    CountingVisitor v;
    ArrayExpr e;
    e.elements.push_back(make_int("1"));
    e.accept(v);
    EXPECT_EQ(v.last_type, "ArrayExpr");
}

// =============================================================================
// Visitor dispatch: every statement type
// =============================================================================

TEST(QA_GDB102, VisitorDispatch_AllStatementTypes) {
    CountingVisitor v;

    // DDL (12 types)
    CreateTableStmt s1;
    s1.accept(v);
    EXPECT_EQ(v.last_type, "CreateTableStmt");
    DropTableStmt s2;
    s2.accept(v);
    EXPECT_EQ(v.last_type, "DropTableStmt");
    AlterTableStmt s3;
    s3.accept(v);
    EXPECT_EQ(v.last_type, "AlterTableStmt");
    CreateIndexStmt s4;
    s4.accept(v);
    EXPECT_EQ(v.last_type, "CreateIndexStmt");
    DropIndexStmt s5;
    s5.accept(v);
    EXPECT_EQ(v.last_type, "DropIndexStmt");
    CreateEdgeTypeStmt s6;
    s6.accept(v);
    EXPECT_EQ(v.last_type, "CreateEdgeTypeStmt");
    DropEdgeTypeStmt s7;
    s7.accept(v);
    EXPECT_EQ(v.last_type, "DropEdgeTypeStmt");
    CreateDatabaseStmt s8;
    s8.accept(v);
    EXPECT_EQ(v.last_type, "CreateDatabaseStmt");
    DropDatabaseStmt s9;
    s9.accept(v);
    EXPECT_EQ(v.last_type, "DropDatabaseStmt");
    CreateUserStmt s10;
    s10.accept(v);
    EXPECT_EQ(v.last_type, "CreateUserStmt");
    DropUserStmt s11;
    s11.accept(v);
    EXPECT_EQ(v.last_type, "DropUserStmt");
    AlterUserStmt s12;
    s12.accept(v);
    EXPECT_EQ(v.last_type, "AlterUserStmt");

    // DML (5 types)
    InsertStmt s13;
    s13.accept(v);
    EXPECT_EQ(v.last_type, "InsertStmt");
    UpdateStmt s14;
    s14.accept(v);
    EXPECT_EQ(v.last_type, "UpdateStmt");
    DeleteStmt s15;
    s15.accept(v);
    EXPECT_EQ(v.last_type, "DeleteStmt");
    LinkStmt s16;
    s16.accept(v);
    EXPECT_EQ(v.last_type, "LinkStmt");
    UnlinkStmt s17;
    s17.accept(v);
    EXPECT_EQ(v.last_type, "UnlinkStmt");

    // Query (4 types)
    SelectStmt s18;
    s18.accept(v);
    EXPECT_EQ(v.last_type, "SelectStmt");
    TraverseStmt s19;
    s19.accept(v);
    EXPECT_EQ(v.last_type, "TraverseStmt");
    MatchStmt s21;
    s21.accept(v);
    EXPECT_EQ(v.last_type, "MatchStmt");
    ShortestPathStmt s22;
    s22.accept(v);
    EXPECT_EQ(v.last_type, "ShortestPathStmt");

    // TCL (4 types)
    BeginStmt s23;
    s23.accept(v);
    EXPECT_EQ(v.last_type, "BeginStmt");
    CommitStmt s24;
    s24.accept(v);
    EXPECT_EQ(v.last_type, "CommitStmt");
    RollbackStmt s25;
    s25.accept(v);
    EXPECT_EQ(v.last_type, "RollbackStmt");
    SavepointStmt s26;
    s26.accept(v);
    EXPECT_EQ(v.last_type, "SavepointStmt");

    // Admin (7 types)
    SetStmt s27;
    s27.accept(v);
    EXPECT_EQ(v.last_type, "SetStmt");
    ShowStmt s28;
    s28.accept(v);
    EXPECT_EQ(v.last_type, "ShowStmt");
    ExplainStmt s29;
    s29.accept(v);
    EXPECT_EQ(v.last_type, "ExplainStmt");
    DescribeStmt s30;
    s30.accept(v);
    EXPECT_EQ(v.last_type, "DescribeStmt");
    ReembedStmt s31;
    s31.accept(v);
    EXPECT_EQ(v.last_type, "ReembedStmt");
    VacuumStmt s32;
    s32.accept(v);
    EXPECT_EQ(v.last_type, "VacuumStmt");
    AnalyzeStmt s33;
    s33.accept(v);
    EXPECT_EQ(v.last_type, "AnalyzeStmt");

    // 12 DDL + 5 DML + 4 Query + 4 TCL + 7 Admin = 32 statements total
    EXPECT_EQ(v.total, 32);
}

// =============================================================================
// Visitor dispatch via polymorphic base pointer
// =============================================================================

TEST(QA_GDB102, VisitorViaExprBasePtr) {
    CountingVisitor v;

    // Create various expression types and dispatch through Expr*
    std::vector<ExprPtr> exprs;
    exprs.push_back(std::make_unique<LiteralExpr>());
    exprs.push_back(std::make_unique<ColumnRefExpr>());
    auto bin = std::make_unique<BinaryExpr>();
    bin->lhs = make_int("1");
    bin->rhs = make_int("2");
    exprs.push_back(std::move(bin));
    auto unary = std::make_unique<UnaryExpr>();
    unary->operand = make_int("1");
    exprs.push_back(std::move(unary));
    exprs.push_back(std::make_unique<FunctionCallExpr>());
    auto cast = std::make_unique<CastExpr>();
    cast->expr = make_int("1");
    exprs.push_back(std::move(cast));
    exprs.push_back(std::make_unique<CaseExpr>());
    auto in = std::make_unique<InExpr>();
    in->expr = make_col("x");
    exprs.push_back(std::move(in));
    auto between = std::make_unique<BetweenExpr>();
    between->expr = make_col("x");
    between->low = make_int("0");
    between->high = make_int("1");
    exprs.push_back(std::move(between));
    auto isnull = std::make_unique<IsNullExpr>();
    isnull->expr = make_col("x");
    exprs.push_back(std::move(isnull));
    auto like = std::make_unique<LikeExpr>();
    like->expr = make_col("x");
    like->pattern = make_str("%");
    exprs.push_back(std::move(like));
    auto exists = std::make_unique<ExistsExpr>();
    exists->subquery = std::make_unique<SelectStmt>();
    exprs.push_back(std::move(exists));
    auto sq = std::make_unique<SubqueryExpr>();
    sq->subquery = std::make_unique<SelectStmt>();
    exprs.push_back(std::move(sq));
    exprs.push_back(std::make_unique<ArrayExpr>());

    for (auto& e : exprs) {
        e->accept(v);
    }

    EXPECT_EQ(v.total, 14); // All 14 expression types
}

TEST(QA_GDB102, VisitorViaStmtBasePtr) {
    CountingVisitor v;

    // Create all 32 statement types and dispatch through Stmt*
    std::vector<StmtPtr> stmts;
    stmts.push_back(std::make_unique<CreateTableStmt>());
    stmts.push_back(std::make_unique<DropTableStmt>());
    stmts.push_back(std::make_unique<AlterTableStmt>());
    stmts.push_back(std::make_unique<CreateIndexStmt>());
    stmts.push_back(std::make_unique<DropIndexStmt>());
    stmts.push_back(std::make_unique<CreateEdgeTypeStmt>());
    stmts.push_back(std::make_unique<DropEdgeTypeStmt>());
    stmts.push_back(std::make_unique<CreateDatabaseStmt>());
    stmts.push_back(std::make_unique<DropDatabaseStmt>());
    stmts.push_back(std::make_unique<CreateUserStmt>());
    stmts.push_back(std::make_unique<DropUserStmt>());
    stmts.push_back(std::make_unique<AlterUserStmt>());
    stmts.push_back(std::make_unique<InsertStmt>());
    stmts.push_back(std::make_unique<UpdateStmt>());
    stmts.push_back(std::make_unique<DeleteStmt>());
    stmts.push_back(std::make_unique<LinkStmt>());
    stmts.push_back(std::make_unique<UnlinkStmt>());
    stmts.push_back(std::make_unique<SelectStmt>());
    stmts.push_back(std::make_unique<TraverseStmt>());
    stmts.push_back(std::make_unique<MatchStmt>());
    stmts.push_back(std::make_unique<ShortestPathStmt>());
    stmts.push_back(std::make_unique<BeginStmt>());
    stmts.push_back(std::make_unique<CommitStmt>());
    stmts.push_back(std::make_unique<RollbackStmt>());
    stmts.push_back(std::make_unique<SavepointStmt>());
    stmts.push_back(std::make_unique<SetStmt>());
    stmts.push_back(std::make_unique<ShowStmt>());
    stmts.push_back(std::make_unique<ExplainStmt>());
    stmts.push_back(std::make_unique<DescribeStmt>());
    stmts.push_back(std::make_unique<ReembedStmt>());
    stmts.push_back(std::make_unique<VacuumStmt>());
    stmts.push_back(std::make_unique<AnalyzeStmt>());

    for (auto& s : stmts) {
        s->accept(v);
    }

    EXPECT_EQ(v.total, 32);
}

// =============================================================================
// Default values
// =============================================================================

TEST(QA_GDB102, ExprDefaultLineCol) {
    LiteralExpr e;
    EXPECT_EQ(e.line, 0u);
    EXPECT_EQ(e.col, 0u);
}

TEST(QA_GDB102, AstColumnDefDefaults) {
    AstColumnDef cd;
    EXPECT_TRUE(cd.name.empty());
    EXPECT_TRUE(cd.nullable); // default is true
    EXPECT_FALSE(cd.is_unique);
    EXPECT_EQ(cd.default_expr, nullptr);
    EXPECT_EQ(cd.check_expr, nullptr);
    EXPECT_TRUE(cd.fk_table.empty());
    EXPECT_TRUE(cd.fk_column.empty());
    EXPECT_EQ(cd.fk_on_delete, ReferentialAction::RESTRICT);
}

TEST(QA_GDB102, TableConstraintDefaults) {
    TableConstraint tc;
    EXPECT_TRUE(tc.name.empty());
    EXPECT_TRUE(tc.columns.empty());
    EXPECT_EQ(tc.check_expr, nullptr);
    EXPECT_TRUE(tc.fk_table.empty());
    EXPECT_TRUE(tc.fk_columns.empty());
    EXPECT_EQ(tc.on_delete, ReferentialAction::RESTRICT);
}

TEST(QA_GDB102, JoinClauseDefaults) {
    JoinClause jc;
    EXPECT_EQ(jc.type, JoinType::INNER);
    EXPECT_TRUE(jc.table.name.empty());
    EXPECT_EQ(jc.on_expr, nullptr);
}

TEST(QA_GDB102, OrderByItemDefaults) {
    OrderByItem obi;
    EXPECT_EQ(obi.direction, SortDirection::ASC);
    EXPECT_EQ(obi.expr, nullptr);
}

TEST(QA_GDB102, SelectItemDefaults) {
    SelectItem si;
    EXPECT_EQ(si.expr, nullptr);
    EXPECT_TRUE(si.alias.empty());
    EXPECT_FALSE(si.is_star);
    EXPECT_TRUE(si.table_star.empty());
}

TEST(QA_GDB102, EdgePatternDefDefaults) {
    EdgePatternDef ep;
    EXPECT_TRUE(ep.variable.empty());
    EXPECT_TRUE(ep.edge_type.empty());
    EXPECT_EQ(ep.direction, TraverseDirection::OUT);
}

TEST(QA_GDB102, SelectStmtDefaults) {
    SelectStmt s;
    EXPECT_FALSE(s.distinct);
    EXPECT_TRUE(s.items.empty());
    EXPECT_TRUE(s.from.empty());
    EXPECT_TRUE(s.joins.empty());
    EXPECT_EQ(s.where_expr, nullptr);
    EXPECT_TRUE(s.group_by.empty());
    EXPECT_EQ(s.having_expr, nullptr);
    EXPECT_TRUE(s.order_by.empty());
    EXPECT_EQ(s.limit, nullptr);
    EXPECT_EQ(s.offset, nullptr);
    EXPECT_EQ(s.set_op, SelectStmt::SetOp::NONE);
    EXPECT_EQ(s.set_rhs, nullptr);
    EXPECT_TRUE(s.ctes.empty());
}

TEST(QA_GDB102, TraverseStmtDefaults) {
    TraverseStmt s;
    EXPECT_EQ(s.direction, TraverseDirection::OUT);
    EXPECT_FALSE(s.max_depth.has_value());
    EXPECT_EQ(s.where_expr, nullptr);
    EXPECT_FALSE(s.fetch);
}

TEST(QA_GDB102, NearestExprDefaults) {
    NearestExpr e;
    EXPECT_EQ(e.metric, NearestMetric::COSINE);
    EXPECT_EQ(e.within_traverse, nullptr);
}

TEST(QA_GDB102, ExplainStmtDefaults) {
    ExplainStmt s;
    EXPECT_FALSE(s.analyze);
    EXPECT_EQ(s.format, ExplainFormat::TEXT);
    EXPECT_EQ(s.statement, nullptr);
}

TEST(QA_GDB102, InExprDefaults) {
    InExpr e;
    EXPECT_FALSE(e.negated);
    EXPECT_TRUE(e.values.empty());
    EXPECT_EQ(e.subquery, nullptr);
}

TEST(QA_GDB102, BetweenExprDefaults) {
    BetweenExpr e;
    EXPECT_FALSE(e.negated);
}

TEST(QA_GDB102, IsNullExprDefaults) {
    IsNullExpr e;
    EXPECT_FALSE(e.negated);
}

TEST(QA_GDB102, LikeExprDefaults) {
    LikeExpr e;
    EXPECT_FALSE(e.negated);
}

TEST(QA_GDB102, FunctionCallExprDefaults) {
    FunctionCallExpr e;
    EXPECT_FALSE(e.distinct);
    EXPECT_TRUE(e.args.empty());
}

TEST(QA_GDB102, CreateTableStmtDefaults) {
    CreateTableStmt s;
    EXPECT_FALSE(s.if_not_exists);
    EXPECT_TRUE(s.columns.empty());
    EXPECT_TRUE(s.constraints.empty());
}

TEST(QA_GDB102, DropTableStmtDefaults) {
    DropTableStmt s;
    EXPECT_FALSE(s.if_exists);
    EXPECT_FALSE(s.cascade);
}

TEST(QA_GDB102, CreateIndexStmtDefaults) {
    CreateIndexStmt s;
    EXPECT_FALSE(s.is_unique);
    EXPECT_FALSE(s.if_not_exists);
    EXPECT_TRUE(s.method.empty());
}

TEST(QA_GDB102, DropIndexStmtDefaults) {
    DropIndexStmt s;
    EXPECT_FALSE(s.if_exists);
}

TEST(QA_GDB102, CreateDatabaseStmtDefaults) {
    CreateDatabaseStmt s;
    EXPECT_FALSE(s.if_not_exists);
}

TEST(QA_GDB102, DropDatabaseStmtDefaults) {
    DropDatabaseStmt s;
    EXPECT_FALSE(s.if_exists);
    EXPECT_FALSE(s.cascade);
}

TEST(QA_GDB102, DropEdgeTypeStmtDefaults) {
    DropEdgeTypeStmt s;
    EXPECT_FALSE(s.if_exists);
}

TEST(QA_GDB102, DropUserStmtDefaults) {
    DropUserStmt s;
    EXPECT_FALSE(s.if_exists);
}

TEST(QA_GDB102, RollbackStmtDefaults) {
    RollbackStmt s;
    EXPECT_TRUE(s.savepoint.empty());
}

// =============================================================================
// Enum coverage
// =============================================================================

TEST(QA_GDB102, BinaryOpAllValues) {
    // Verify all 14 binary ops are distinct
    std::vector<BinaryOp> ops = {
        BinaryOp::ADD,
        BinaryOp::SUBTRACT,
        BinaryOp::MULTIPLY,
        BinaryOp::DIVIDE,
        BinaryOp::MODULO,
        BinaryOp::EQUAL,
        BinaryOp::NOT_EQUAL,
        BinaryOp::LESS,
        BinaryOp::GREATER,
        BinaryOp::LESS_EQUAL,
        BinaryOp::GREATER_EQUAL,
        BinaryOp::AND,
        BinaryOp::OR,
        BinaryOp::CONCAT,
    };

    // Each should be unique
    for (size_t i = 0; i < ops.size(); ++i) {
        for (size_t j = i + 1; j < ops.size(); ++j) {
            EXPECT_NE(ops[i], ops[j]) << "ops[" << i << "] == ops[" << j << "]";
        }
    }
}

TEST(QA_GDB102, JoinTypeAllValues) {
    std::vector<JoinType> types = {
        JoinType::INNER,
        JoinType::LEFT,
        JoinType::RIGHT,
        JoinType::FULL,
        JoinType::CROSS,
        JoinType::SEMI,
        JoinType::ANTI,
    };
    for (size_t i = 0; i < types.size(); ++i) {
        for (size_t j = i + 1; j < types.size(); ++j) {
            EXPECT_NE(types[i], types[j]);
        }
    }
}

TEST(QA_GDB102, ShowTargetAllValues) {
    std::vector<ShowTarget> targets = {
        ShowTarget::DATABASES,
        ShowTarget::TABLES,
        ShowTarget::COLUMNS,
        ShowTarget::EDGE_TYPES,
        ShowTarget::INDEXES,
        ShowTarget::EMBEDDINGS,
        ShowTarget::PROVIDERS,
        ShowTarget::REPLICATION_SLOTS,
        ShowTarget::REPLICATION_STATUS,
        ShowTarget::STANDBY_STATUS,
        ShowTarget::PARAMETER,
        ShowTarget::ALL,
    };
    for (size_t i = 0; i < targets.size(); ++i) {
        for (size_t j = i + 1; j < targets.size(); ++j) {
            EXPECT_NE(targets[i], targets[j]);
        }
    }
}

TEST(QA_GDB102, LiteralKindAllValues) {
    std::vector<LiteralKind> kinds = {
        LiteralKind::INTEGER,
        LiteralKind::FLOAT,
        LiteralKind::STRING,
        LiteralKind::BOOLEAN,
        LiteralKind::NULL_LITERAL,
    };
    for (size_t i = 0; i < kinds.size(); ++i) {
        for (size_t j = i + 1; j < kinds.size(); ++j) {
            EXPECT_NE(kinds[i], kinds[j]);
        }
    }
}

TEST(QA_GDB102, SelectSetOpAllValues) {
    std::vector<SelectStmt::SetOp> ops = {
        SelectStmt::SetOp::NONE,
        SelectStmt::SetOp::UNION,
        SelectStmt::SetOp::UNION_ALL,
        SelectStmt::SetOp::INTERSECT,
        SelectStmt::SetOp::EXCEPT,
    };
    for (size_t i = 0; i < ops.size(); ++i) {
        for (size_t j = i + 1; j < ops.size(); ++j) {
            EXPECT_NE(ops[i], ops[j]);
        }
    }
}

TEST(QA_GDB102, ExplainFormatValues) {
    EXPECT_NE(ExplainFormat::TEXT, ExplainFormat::JSON);
}

TEST(QA_GDB102, TableConstraintKindAllValues) {
    std::vector<TableConstraint::Kind> kinds = {
        TableConstraint::Kind::PRIMARY_KEY,
        TableConstraint::Kind::UNIQUE,
        TableConstraint::Kind::CHECK,
        TableConstraint::Kind::FOREIGN_KEY,
    };
    for (size_t i = 0; i < kinds.size(); ++i) {
        for (size_t j = i + 1; j < kinds.size(); ++j) {
            EXPECT_NE(kinds[i], kinds[j]);
        }
    }
}

TEST(QA_GDB102, NearestMetricAllValues) {
    std::vector<NearestMetric> metrics = {
        NearestMetric::COSINE,
        NearestMetric::L2,
        NearestMetric::DOT,
    };
    for (size_t i = 0; i < metrics.size(); ++i) {
        for (size_t j = i + 1; j < metrics.size(); ++j) {
            EXPECT_NE(metrics[i], metrics[j]);
        }
    }
}

// =============================================================================
// Deep nesting and complex compositions
// =============================================================================

TEST(QA_GDB102, DeeplyNestedBinaryExprTree) {
    // Build a deep left-recursive tree: (((1+2)+3)+4)+5)
    ExprPtr tree = make_int("1");
    for (int i = 2; i <= 100; ++i) {
        auto bin = std::make_unique<BinaryExpr>();
        bin->op = BinaryOp::ADD;
        bin->lhs = std::move(tree);
        bin->rhs = make_int(std::to_string(i));
        tree = std::move(bin);
    }

    // The root should dispatch as BinaryExpr
    CountingVisitor v;
    tree->accept(v);
    EXPECT_EQ(v.last_type, "BinaryExpr");

    // Walk the left spine to verify depth
    int depth = 0;
    const Expr* cur = tree.get();
    while (auto* bin = dynamic_cast<const BinaryExpr*>(cur)) {
        ++depth;
        cur = bin->lhs.get();
    }
    EXPECT_EQ(depth, 99); // 100 leaves => 99 binary nodes
}

TEST(QA_GDB102, SelectWithAllClauses) {
    // Round-trip: WITH base AS (SELECT 1) SELECT DISTINCT a AS col_a FROM t1 x
    //   LEFT JOIN t2 ON x.id = t2.fk WHERE 1 GROUP BY a HAVING 1
    //   ORDER BY a DESC LIMIT 10 OFFSET 5 UNION ALL SELECT 1
    auto stmt_ptr = parse_one_102("WITH base AS (SELECT 1) "
                                  "SELECT DISTINCT a AS col_a FROM t1 x "
                                  "LEFT JOIN t2 ON x.id = t2.fk WHERE 1 GROUP BY a HAVING 1 "
                                  "ORDER BY a DESC LIMIT 10 OFFSET 5 UNION ALL SELECT 1");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(stmt_ptr.get());
    ASSERT_NE(sel, nullptr);

    EXPECT_TRUE(sel->distinct);
    ASSERT_EQ(sel->ctes.size(), 1u);
    EXPECT_EQ(sel->ctes[0].name, "base");
    ASSERT_EQ(sel->items.size(), 1u);
    EXPECT_EQ(sel->items[0].alias, "col_a");
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].alias, "x");
    ASSERT_EQ(sel->joins.size(), 1u);
    EXPECT_EQ(sel->joins[0].type, JoinType::LEFT);
    EXPECT_NE(sel->where_expr, nullptr);
    EXPECT_EQ(sel->group_by.size(), 1u);
    EXPECT_NE(sel->having_expr, nullptr);
    ASSERT_EQ(sel->order_by.size(), 1u);
    EXPECT_EQ(sel->order_by[0].direction, SortDirection::DESC);
    EXPECT_NE(sel->limit, nullptr);
    EXPECT_NE(sel->offset, nullptr);
    EXPECT_EQ(sel->set_op, SelectStmt::SetOp::UNION_ALL);
    EXPECT_NE(sel->set_rhs, nullptr);
}

TEST(QA_GDB102, MultipleCTEs) {
    // Round-trip: WITH cte_0 AS (..), cte_1 AS (..), ... SELECT * FROM cte_0
    auto stmt_ptr =
        parse_one_102("WITH cte_0 AS (SELECT 1), cte_1 AS (SELECT 2), cte_2 AS (SELECT 3), "
                      "cte_3 AS (SELECT 4), cte_4 AS (SELECT 5) SELECT * FROM cte_0");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(stmt_ptr.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->ctes.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(sel->ctes[static_cast<size_t>(i)].name, "cte_" + std::to_string(i));
        EXPECT_NE(sel->ctes[static_cast<size_t>(i)].query, nullptr);
    }
}

TEST(QA_GDB102, ChainedSetOperations) {
    // Round-trip: SELECT 1 UNION SELECT 2 EXCEPT SELECT 3
    auto stmt_ptr = parse_one_102("SELECT 1 UNION SELECT 2 EXCEPT SELECT 3");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* s1 = dynamic_cast<SelectStmt*>(stmt_ptr.get());
    ASSERT_NE(s1, nullptr);
    EXPECT_EQ(s1->set_op, SelectStmt::SetOp::UNION);
    auto* rhs = dynamic_cast<SelectStmt*>(s1->set_rhs.get());
    ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(rhs->set_op, SelectStmt::SetOp::EXCEPT);
    EXPECT_NE(rhs->set_rhs, nullptr);
}

TEST(QA_GDB102, CreateTableAllConstraintTypes) {
    // Round-trip: CREATE TABLE IF NOT EXISTS with inline column constraints and
    // table-level PRIMARY KEY, UNIQUE, CHECK, FOREIGN KEY constraints.
    auto stmt_ptr = parse_one_102(
        "CREATE TABLE IF NOT EXISTS full_table ("
        "  id INT NOT NULL UNIQUE, "
        "  data VARCHAR(255) DEFAULT 'default' CHECK (1), "
        "  ref_id INT REFERENCES other(id) ON DELETE CASCADE, "
        "  PRIMARY KEY (id), "
        "  CONSTRAINT uq_data UNIQUE (data), "
        "  CONSTRAINT chk_data CHECK (1), "
        "  CONSTRAINT fk_ref FOREIGN KEY (ref_id) REFERENCES other(id) ON DELETE CASCADE"
        ")");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt_ptr.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_TRUE(ct->if_not_exists);
    EXPECT_EQ(ct->name, "full_table");
    EXPECT_EQ(ct->columns.size(), 3u);
    EXPECT_EQ(ct->constraints.size(), 4u);
    // Find the FOREIGN KEY constraint and verify fk_columns
    bool found_fk = false;
    for (const auto& c : ct->constraints) {
        if (c.kind == TableConstraint::Kind::FOREIGN_KEY) {
            EXPECT_EQ(c.fk_columns.size(), 1u);
            EXPECT_EQ(c.fk_columns[0], "id");
            EXPECT_EQ(c.on_delete, ReferentialAction::CASCADE);
            found_fk = true;
        }
    }
    EXPECT_TRUE(found_fk);
}

TEST(QA_GDB102, InsertWithSelectSubquery) {
    // Round-trip: INSERT INTO target (col) SELECT id FROM source
    auto stmt_ptr = parse_one_102("INSERT INTO target (col) SELECT id FROM source");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* ins = dynamic_cast<InsertStmt*>(stmt_ptr.get());
    ASSERT_NE(ins, nullptr);
    EXPECT_EQ(ins->table_name, "target");
    ASSERT_EQ(ins->columns.size(), 1u);
    EXPECT_EQ(ins->columns[0], "col");
    EXPECT_TRUE(ins->values.empty());
    ASSERT_NE(ins->select, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(ins->select.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].name, "source");
}

TEST(QA_GDB102, UpdateWithMultipleAssignmentsAndReturning) {
    // Round-trip: UPDATE t SET col_0=0, col_1=1, col_2=2, col_3=3, col_4=4
    //   WHERE 1 RETURNING *
    auto stmt_ptr =
        parse_one_102("UPDATE t SET col_0 = 0, col_1 = 1, col_2 = 2, col_3 = 3, col_4 = 4 "
                      "WHERE 1 RETURNING *");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* upd = dynamic_cast<UpdateStmt*>(stmt_ptr.get());
    ASSERT_NE(upd, nullptr);
    EXPECT_EQ(upd->table_name, "t");
    EXPECT_EQ(upd->assignments.size(), 5u);
    EXPECT_NE(upd->where_expr, nullptr);
    ASSERT_EQ(upd->returning.size(), 1u);
    EXPECT_TRUE(upd->returning[0].is_star);
}

TEST(QA_GDB102, CaseExprMultipleWhens) {
    // Round-trip: CASE status WHEN 0 THEN 'label_0' ... WHEN 9 THEN 'label_9' ELSE 'unknown' END
    auto stmt_ptr =
        parse_one_102("SELECT CASE status "
                      "WHEN 0 THEN 'label_0' WHEN 1 THEN 'label_1' WHEN 2 THEN 'label_2' "
                      "WHEN 3 THEN 'label_3' WHEN 4 THEN 'label_4' WHEN 5 THEN 'label_5' "
                      "WHEN 6 THEN 'label_6' WHEN 7 THEN 'label_7' WHEN 8 THEN 'label_8' "
                      "WHEN 9 THEN 'label_9' ELSE 'unknown' END FROM t");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(stmt_ptr.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* c = dynamic_cast<CaseExpr*>(sel->items[0].expr.get());
    ASSERT_NE(c, nullptr);
    EXPECT_NE(c->operand, nullptr);
    EXPECT_EQ(c->whens.size(), 10u);
    EXPECT_NE(c->else_expr, nullptr);
}

TEST(QA_GDB102, MatchStmtComplexPattern) {
    // Round-trip: (a:User)-[r1:FOLLOWS]->(b:User)-[r2:LIKES]->(c:Post)
    //   WHERE a.age > 18 RETURN a.name, c.title
    auto stmt_ptr = parse_one_102("MATCH (a:User)-[r1:FOLLOWS]->(b:User)-[r2:LIKES]->(c:Post) "
                                  "WHERE a.age > 18 RETURN a.name, c.title");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(stmt_ptr.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 3u);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->edge_type, "FOLLOWS");
    ASSERT_TRUE(m->pattern[1].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[1].outgoing_edge->edge_type, "LIKES");
    EXPECT_FALSE(m->pattern[2].outgoing_edge.has_value());
    EXPECT_NE(m->where_expr, nullptr);
    EXPECT_EQ(m->return_items.size(), 2u);
}

TEST(QA_GDB102, NearestWithinTraverse) {
    // Round-trip: NEAREST ... WITHIN TRAVERSE ... USING L2
    // Parser order: TO target [WITHIN TRAVERSE ...] [USING metric]
    auto stmt_ptr =
        parse_one_102("SELECT * FROM products "
                      "WHERE NEAREST(embedding, 5) TO [1, 2, 3] "
                      "WITHIN TRAVERSE belongs_to FROM categories(42) DIRECTION IN MAX_DEPTH 2 "
                      "USING L2");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(stmt_ptr.get());
    ASSERT_NE(sel, nullptr);
    // Locate the NearestExpr in the WHERE predicate
    auto* n = dynamic_cast<NearestExpr*>(sel->where_expr.get());
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->metric, NearestMetric::L2);
    ASSERT_NE(n->within_traverse, nullptr);
    auto* inner = dynamic_cast<TraverseStmt*>(n->within_traverse.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->direction, TraverseDirection::IN);
    ASSERT_TRUE(inner->max_depth.has_value());
    EXPECT_EQ(inner->max_depth.value(), 2);
}

TEST(QA_GDB102, ExplainAnalyzeJsonFormat) {
    // Round-trip: EXPLAIN ANALYZE FORMAT JSON SELECT * FROM t
    auto stmt_ptr = parse_one_102("EXPLAIN ANALYZE FORMAT JSON SELECT * FROM t");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* explain = dynamic_cast<ExplainStmt*>(stmt_ptr.get());
    ASSERT_NE(explain, nullptr);
    EXPECT_TRUE(explain->analyze);
    EXPECT_EQ(explain->format, ExplainFormat::JSON);
    ASSERT_NE(explain->statement, nullptr);
    EXPECT_NE(dynamic_cast<SelectStmt*>(explain->statement.get()), nullptr);
}

TEST(QA_GDB102, MultipleJoinsOnSelect) {
    // Round-trip: SELECT with INNER, LEFT, RIGHT, FULL, CROSS joins
    auto stmt_ptr = parse_one_102("SELECT * FROM a "
                                  "INNER JOIN t_0 ON a.id = t_0.fk "
                                  "LEFT JOIN t_1 ON a.id = t_1.fk "
                                  "RIGHT JOIN t_2 ON a.id = t_2.fk "
                                  "FULL JOIN t_3 ON a.id = t_3.fk "
                                  "CROSS JOIN t_4");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(stmt_ptr.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->joins.size(), 5u);
    EXPECT_EQ(sel->joins[0].type, JoinType::INNER);
    EXPECT_EQ(sel->joins[1].type, JoinType::LEFT);
    EXPECT_EQ(sel->joins[2].type, JoinType::RIGHT);
    EXPECT_EQ(sel->joins[3].type, JoinType::FULL);
    EXPECT_EQ(sel->joins[4].type, JoinType::CROSS);
    EXPECT_EQ(sel->joins[4].on_expr, nullptr); // CROSS JOIN has no ON
}

TEST(QA_GDB102, FromSubquery) {
    // Round-trip: SELECT * FROM (SELECT id FROM users) AS sub
    auto stmt_ptr = parse_one_102("SELECT * FROM (SELECT id FROM users) AS sub");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(stmt_ptr.get());
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

TEST(QA_GDB102, InExprSubqueryVsValues) {
    // Round-trip values form: SELECT * FROM t WHERE id IN (1, 2)
    {
        auto stmt_ptr = parse_one_102("SELECT * FROM t WHERE id IN (1, 2)");
        ASSERT_NE(stmt_ptr, nullptr);
        auto* sel = dynamic_cast<SelectStmt*>(stmt_ptr.get());
        ASSERT_NE(sel, nullptr);
        auto* in_expr = dynamic_cast<InExpr*>(sel->where_expr.get());
        ASSERT_NE(in_expr, nullptr);
        EXPECT_EQ(in_expr->values.size(), 2u);
        EXPECT_EQ(in_expr->subquery, nullptr);
    }
    // Round-trip subquery form: SELECT * FROM t WHERE id IN (SELECT id FROM u)
    {
        auto stmt_ptr = parse_one_102("SELECT * FROM t WHERE id IN (SELECT id FROM u)");
        ASSERT_NE(stmt_ptr, nullptr);
        auto* sel = dynamic_cast<SelectStmt*>(stmt_ptr.get());
        ASSERT_NE(sel, nullptr);
        auto* in_expr = dynamic_cast<InExpr*>(sel->where_expr.get());
        ASSERT_NE(in_expr, nullptr);
        EXPECT_TRUE(in_expr->values.empty());
        EXPECT_NE(in_expr->subquery, nullptr);
    }
}

TEST(QA_GDB102, EdgePropertyTypes) {
    // Round-trip: CREATE EDGE TYPE name (props...) FROM table TO table
    // Use parser-recognized type keywords: DOUBLE, TIMESTAMP, JSON
    auto stmt_ptr = parse_one_102("CREATE EDGE TYPE weighted_follows "
                                  "(weight DOUBLE, created_at TIMESTAMP, metadata JSON) "
                                  "FROM users TO users");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* et = dynamic_cast<CreateEdgeTypeStmt*>(stmt_ptr.get());
    ASSERT_NE(et, nullptr);
    EXPECT_EQ(et->name, "weighted_follows");
    EXPECT_EQ(et->from_table, "users");
    EXPECT_EQ(et->to_table, "users");
    ASSERT_EQ(et->properties.size(), 3u);
    EXPECT_EQ(et->properties[0].type.name, "DOUBLE");
    EXPECT_EQ(et->properties[1].type.name, "TIMESTAMP");
    EXPECT_EQ(et->properties[2].type.name, "JSON");
}

TEST(QA_GDB102, TypeSpecEmbeddingFullParams) {
    // Round-trip: CREATE TABLE with EMBEDDING(dim, source_col, 'provider') column
    // Positional syntax requires source as identifier, provider as string literal
    auto stmt_ptr =
        parse_one_102("CREATE TABLE docs (id INT, vec EMBEDDING(768, description, 'openai'))");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* ct = dynamic_cast<CreateTableStmt*>(stmt_ptr.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->columns.size(), 2u);
    const auto& ts = ct->columns[1].type;
    EXPECT_EQ(ts.name, "EMBEDDING");
    ASSERT_TRUE(ts.param1.has_value());
    EXPECT_EQ(ts.param1.value(), 768);
    EXPECT_EQ(ts.source, "description");
    EXPECT_EQ(ts.provider, "openai");
    EXPECT_FALSE(ts.param2.has_value());
}

TEST(QA_GDB102, ShortestPathWithAllFields) {
    // Round-trip: SHORTEST PATH FROM cities(1) TO cities(100) VIA road
    //   DIRECTION BOTH MAX_DEPTH 10
    auto stmt_ptr = parse_one_102("SHORTEST PATH FROM cities(1) TO cities(100) VIA road "
                                  "DIRECTION BOTH MAX_DEPTH 10");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* sp = dynamic_cast<ShortestPathStmt*>(stmt_ptr.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->from_table, "cities");
    EXPECT_EQ(sp->to_table, "cities");
    EXPECT_EQ(sp->edge_type, "road");
    EXPECT_EQ(sp->direction, TraverseDirection::BOTH);
    ASSERT_TRUE(sp->max_depth.has_value());
    EXPECT_EQ(sp->max_depth.value(), 10);
}

TEST(QA_GDB102, TraverseAllDirections) {
    // Round-trip: parse TRAVERSE with each of IN, OUT, BOTH direction keywords
    struct Case {
        const char* sql;
        TraverseDirection expected;
    };
    const Case cases[] = {
        {"TRAVERSE follows FROM users(1) DIRECTION IN", TraverseDirection::IN},
        {"TRAVERSE follows FROM users(1) DIRECTION OUT", TraverseDirection::OUT},
        {"TRAVERSE follows FROM users(1) DIRECTION BOTH", TraverseDirection::BOTH},
    };
    for (const auto& tc : cases) {
        auto stmt_ptr = parse_one_102(tc.sql);
        ASSERT_NE(stmt_ptr, nullptr) << tc.sql;
        auto* t = dynamic_cast<TraverseStmt*>(stmt_ptr.get());
        ASSERT_NE(t, nullptr) << tc.sql;
        EXPECT_EQ(t->direction, tc.expected) << tc.sql;
    }
}

TEST(QA_GDB102, ShowTargetAllVariants) {
    // Construct ShowStmt for each target type
    ShowTarget targets[] = {
        ShowTarget::DATABASES,
        ShowTarget::TABLES,
        ShowTarget::COLUMNS,
        ShowTarget::EDGE_TYPES,
        ShowTarget::INDEXES,
        ShowTarget::EMBEDDINGS,
        ShowTarget::PROVIDERS,
        ShowTarget::REPLICATION_SLOTS,
        ShowTarget::REPLICATION_STATUS,
        ShowTarget::STANDBY_STATUS,
        ShowTarget::PARAMETER,
        ShowTarget::ALL,
    };

    for (auto t : targets) {
        ShowStmt stmt;
        stmt.target = t;
        if (t == ShowTarget::COLUMNS || t == ShowTarget::PARAMETER) {
            stmt.name = "some_name";
        }

        CountingVisitor v;
        stmt.accept(v);
        EXPECT_EQ(v.last_type, "ShowStmt");
    }
}

TEST(QA_GDB102, AlterTableDropColumn) {
    // Round-trip: ALTER TABLE users DROP COLUMN old_col
    auto stmt_ptr = parse_one_102("ALTER TABLE users DROP COLUMN old_col");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* alt = dynamic_cast<AlterTableStmt*>(stmt_ptr.get());
    ASSERT_NE(alt, nullptr);
    EXPECT_EQ(alt->table_name, "users");
    EXPECT_EQ(alt->action, AlterAction::DROP_COLUMN);
    EXPECT_EQ(alt->column_name, "old_col");
}

TEST(QA_GDB102, LinkWithMultipleProperties) {
    // Round-trip: LINK users(1) TO posts(42) VIA authored (prop_0='val_0', ...)
    auto stmt_ptr = parse_one_102("LINK users(1) TO posts(42) VIA authored "
                                  "(prop_0 = 'val_0', prop_1 = 'val_1', prop_2 = 'val_2')");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* lnk = dynamic_cast<LinkStmt*>(stmt_ptr.get());
    ASSERT_NE(lnk, nullptr);
    EXPECT_EQ(lnk->source_table, "users");
    EXPECT_EQ(lnk->target_table, "posts");
    EXPECT_EQ(lnk->edge_type, "authored");
    ASSERT_EQ(lnk->properties.size(), 3u);
    EXPECT_EQ(lnk->properties[0].column, "prop_0");
}

TEST(QA_GDB102, UnlinkWithWhere) {
    // Round-trip: UNLINK users(1) FROM users(2) VIA follows WHERE 1
    auto stmt_ptr = parse_one_102("UNLINK users(1) FROM users(2) VIA follows WHERE 1");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* ul = dynamic_cast<UnlinkStmt*>(stmt_ptr.get());
    ASSERT_NE(ul, nullptr);
    EXPECT_EQ(ul->source_table, "users");
    EXPECT_EQ(ul->target_table, "users");
    EXPECT_EQ(ul->edge_type, "follows");
    EXPECT_NE(ul->where_expr, nullptr);
}

TEST(QA_GDB102, CreateUserAndAlterUser) {
    // Round-trip: CREATE USER and ALTER USER both set username/password
    {
        auto stmt_ptr = parse_one_102("CREATE USER admin WITH PASSWORD 'secret123'");
        ASSERT_NE(stmt_ptr, nullptr);
        auto* cu = dynamic_cast<CreateUserStmt*>(stmt_ptr.get());
        ASSERT_NE(cu, nullptr);
        EXPECT_EQ(cu->username, "admin");
        EXPECT_EQ(cu->password, "secret123");
    }
    {
        auto stmt_ptr = parse_one_102("ALTER USER admin WITH PASSWORD 'newpass456'");
        ASSERT_NE(stmt_ptr, nullptr);
        auto* au = dynamic_cast<AlterUserStmt*>(stmt_ptr.get());
        ASSERT_NE(au, nullptr);
        EXPECT_EQ(au->username, "admin");
        EXPECT_EQ(au->password, "newpass456");
        // Passwords are different — the ALTER changed the credential
        EXPECT_NE(au->password, "secret123");
    }
}

TEST(QA_GDB102, InsertMultipleRowsMultipleColumns) {
    // Round-trip: INSERT with multiple rows and multiple columns
    auto stmt_ptr = parse_one_102("INSERT INTO data (a, b, c) VALUES "
                                  "(0, 1, 2), (3, 4, 5), (6, 7, 8), (9, 10, 11), (12, 13, 14)");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* ins = dynamic_cast<InsertStmt*>(stmt_ptr.get());
    ASSERT_NE(ins, nullptr);
    EXPECT_EQ(ins->table_name, "data");
    ASSERT_EQ(ins->columns.size(), 3u);
    EXPECT_EQ(ins->columns[0], "a");
    EXPECT_EQ(ins->columns[1], "b");
    EXPECT_EQ(ins->columns[2], "c");
    EXPECT_EQ(ins->values.size(), 5u);
    EXPECT_EQ(ins->values[0].size(), 3u);
    EXPECT_EQ(ins->values[4].size(), 3u);
}

TEST(QA_GDB102, ArrayExprEmpty) {
    ArrayExpr arr;
    EXPECT_TRUE(arr.elements.empty());

    CountingVisitor v;
    arr.accept(v);
    EXPECT_EQ(v.last_type, "ArrayExpr");
}

// ArrayExprLargeVector deleted (GDB-756): only tested std::vector::push_back
// with no parser or semantic content. The ArrayExprEmpty test above covers
// the visitor dispatch path; NEAREST/EMBEDDING tests exercise real array parsing.

TEST(QA_GDB102, DeleteWithReturningMultipleItems) {
    // Round-trip: DELETE FROM users WHERE 1 RETURNING id, name AS deleted_name
    auto stmt_ptr = parse_one_102("DELETE FROM users WHERE 1 RETURNING id, name AS deleted_name");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* del = dynamic_cast<DeleteStmt*>(stmt_ptr.get());
    ASSERT_NE(del, nullptr);
    EXPECT_EQ(del->table_name, "users");
    EXPECT_NE(del->where_expr, nullptr);
    ASSERT_EQ(del->returning.size(), 2u);
    EXPECT_EQ(del->returning[1].alias, "deleted_name");
}

TEST(QA_GDB102, SelectMultipleFromTables) {
    // Round-trip: SELECT with comma-separated FROM tables (implicit cross join)
    auto stmt_ptr = parse_one_102("SELECT * FROM t0 a0, t1 a1, t2 a2, t3 a3, t4 a4");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(stmt_ptr.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(sel->from[static_cast<size_t>(i)].name, "t" + std::to_string(i));
        EXPECT_EQ(sel->from[static_cast<size_t>(i)].alias, "a" + std::to_string(i));
    }
}

TEST(QA_GDB102, SourceLocationOnExpr) {
    // Round-trip: the parser propagates source location from tokens into AST nodes.
    // For a single-line query the first literal should have line=1, col>=1.
    auto stmt_ptr = parse_one_102("SELECT 42 FROM t WHERE a + b > 0");
    ASSERT_NE(stmt_ptr, nullptr);
    auto* sel = dynamic_cast<SelectStmt*>(stmt_ptr.get());
    ASSERT_NE(sel, nullptr);
    // The SELECT item literal '42' should carry a parser-set location.
    ASSERT_EQ(sel->items.size(), 1u);
    auto* lit = dynamic_cast<LiteralExpr*>(sel->items[0].expr.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->line, 1u); // first line
    EXPECT_GE(lit->col, 1u);  // some non-zero column
    // The WHERE BinaryExpr should also carry a location.
    ASSERT_NE(sel->where_expr, nullptr);
    EXPECT_EQ(sel->where_expr->line, 1u);
    EXPECT_GE(sel->where_expr->col, 1u);
}
