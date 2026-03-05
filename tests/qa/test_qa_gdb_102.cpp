/// QA adversarial tests for GDB-102: AST node types for all statement categories.
/// Tests visitor dispatch, default values, deep nesting, and edge cases.

#include "sixseven/parser/ast.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace sixseven;

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
    void visit(const NearestStmt&) override {
        last_type = "NearestStmt";
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
    void visit(const ReembedStmt&) override {
        last_type = "ReembedStmt";
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

    // Query (5 types)
    SelectStmt s18;
    s18.accept(v);
    EXPECT_EQ(v.last_type, "SelectStmt");
    TraverseStmt s19;
    s19.accept(v);
    EXPECT_EQ(v.last_type, "TraverseStmt");
    NearestStmt s20;
    s20.accept(v);
    EXPECT_EQ(v.last_type, "NearestStmt");
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

    // 12 DDL + 5 DML + 5 Query + 4 TCL + 7 Admin = 33 statements total
    EXPECT_EQ(v.total, 33);
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

    // Create all 33 statement types and dispatch through Stmt*
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
    stmts.push_back(std::make_unique<NearestStmt>());
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

    EXPECT_EQ(v.total, 33);
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

TEST(QA_GDB102, NearestStmtDefaults) {
    NearestStmt s;
    EXPECT_EQ(s.metric, NearestMetric::COSINE);
    EXPECT_EQ(s.within_traverse, nullptr);
    EXPECT_EQ(s.where_expr, nullptr);
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
    // Build a maximal SELECT: WITH cte AS (...) SELECT DISTINCT items
    //   FROM t1 JOIN t2 ON ... WHERE ... GROUP BY ... HAVING ...
    //   ORDER BY ... LIMIT 10 OFFSET 5 UNION ALL (SELECT ...)
    auto stmt = std::make_unique<SelectStmt>();
    stmt->distinct = true;

    // CTE
    SelectStmt::CTE cte;
    cte.name = "base";
    cte.query = std::make_unique<SelectStmt>();
    stmt->ctes.push_back(std::move(cte));

    // Items
    SelectItem si;
    si.expr = make_col("a");
    si.alias = "col_a";
    stmt->items.push_back(std::move(si));

    // FROM
    TableRef tr;
    tr.name = "t1";
    tr.alias = "x";
    stmt->from.push_back(std::move(tr));

    // JOIN
    JoinClause jc;
    jc.type = JoinType::LEFT;
    jc.table.name = "t2";
    auto on_expr = std::make_unique<BinaryExpr>();
    on_expr->op = BinaryOp::EQUAL;
    on_expr->lhs = make_col("id", "x");
    on_expr->rhs = make_col("fk", "t2");
    jc.on_expr = std::move(on_expr);
    stmt->joins.push_back(std::move(jc));

    // WHERE
    stmt->where_expr = make_int("1"); // WHERE 1 (always true)

    // GROUP BY
    stmt->group_by.push_back(make_col("a"));

    // HAVING
    stmt->having_expr = make_int("1");

    // ORDER BY
    OrderByItem obi;
    obi.expr = make_col("a");
    obi.direction = SortDirection::DESC;
    stmt->order_by.push_back(std::move(obi));

    // LIMIT / OFFSET
    stmt->limit = make_int("10");
    stmt->offset = make_int("5");

    // UNION ALL
    stmt->set_op = SelectStmt::SetOp::UNION_ALL;
    stmt->set_rhs = std::make_unique<SelectStmt>();

    // Verify everything through visitor
    CountingVisitor v;
    stmt->accept(v);
    EXPECT_EQ(v.last_type, "SelectStmt");

    // Verify fields
    EXPECT_TRUE(stmt->distinct);
    EXPECT_EQ(stmt->ctes.size(), 1u);
    EXPECT_EQ(stmt->ctes[0].name, "base");
    EXPECT_EQ(stmt->items.size(), 1u);
    EXPECT_EQ(stmt->items[0].alias, "col_a");
    EXPECT_EQ(stmt->from.size(), 1u);
    EXPECT_EQ(stmt->from[0].alias, "x");
    EXPECT_EQ(stmt->joins.size(), 1u);
    EXPECT_EQ(stmt->joins[0].type, JoinType::LEFT);
    EXPECT_NE(stmt->where_expr, nullptr);
    EXPECT_EQ(stmt->group_by.size(), 1u);
    EXPECT_NE(stmt->having_expr, nullptr);
    EXPECT_EQ(stmt->order_by.size(), 1u);
    EXPECT_EQ(stmt->order_by[0].direction, SortDirection::DESC);
    EXPECT_NE(stmt->limit, nullptr);
    EXPECT_NE(stmt->offset, nullptr);
    EXPECT_EQ(stmt->set_op, SelectStmt::SetOp::UNION_ALL);
    EXPECT_NE(stmt->set_rhs, nullptr);
}

TEST(QA_GDB102, MultipleCTEs) {
    auto stmt = std::make_unique<SelectStmt>();

    for (int i = 0; i < 5; ++i) {
        SelectStmt::CTE cte;
        cte.name = "cte_" + std::to_string(i);
        cte.query = std::make_unique<SelectStmt>();
        stmt->ctes.push_back(std::move(cte));
    }

    EXPECT_EQ(stmt->ctes.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(stmt->ctes[i].name, "cte_" + std::to_string(i));
        EXPECT_NE(stmt->ctes[i].query, nullptr);
    }
}

TEST(QA_GDB102, ChainedSetOperations) {
    // SELECT ... UNION SELECT ... EXCEPT SELECT ...
    auto s1 = std::make_unique<SelectStmt>();
    auto s2 = std::make_unique<SelectStmt>();
    auto s3 = std::make_unique<SelectStmt>();

    s2->set_op = SelectStmt::SetOp::EXCEPT;
    s2->set_rhs = std::move(s3);

    s1->set_op = SelectStmt::SetOp::UNION;
    s1->set_rhs = std::move(s2);

    EXPECT_EQ(s1->set_op, SelectStmt::SetOp::UNION);
    auto* rhs = dynamic_cast<SelectStmt*>(s1->set_rhs.get());
    ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(rhs->set_op, SelectStmt::SetOp::EXCEPT);
    EXPECT_NE(rhs->set_rhs, nullptr);
}

TEST(QA_GDB102, CreateTableAllConstraintTypes) {
    auto stmt = std::make_unique<CreateTableStmt>();
    stmt->name = "full_table";
    stmt->if_not_exists = true;

    // Columns with all constraint variants
    AstColumnDef col1;
    col1.name = "id";
    col1.type.name = "INT";
    col1.nullable = false;
    col1.is_unique = true;
    stmt->columns.push_back(std::move(col1));

    AstColumnDef col2;
    col2.name = "data";
    col2.type.name = "VARCHAR";
    col2.type.param1 = 255;
    col2.default_expr = make_str("default");
    col2.check_expr = make_int("1"); // dummy check
    stmt->columns.push_back(std::move(col2));

    AstColumnDef col3;
    col3.name = "ref_id";
    col3.type.name = "INT";
    col3.fk_table = "other";
    col3.fk_column = "id";
    col3.fk_on_delete = ReferentialAction::CASCADE;
    stmt->columns.push_back(std::move(col3));

    // Table-level constraints
    TableConstraint pk;
    pk.kind = TableConstraint::Kind::PRIMARY_KEY;
    pk.columns = {"id"};
    stmt->constraints.push_back(std::move(pk));

    TableConstraint uq;
    uq.kind = TableConstraint::Kind::UNIQUE;
    uq.name = "uq_data";
    uq.columns = {"data"};
    stmt->constraints.push_back(std::move(uq));

    TableConstraint chk;
    chk.kind = TableConstraint::Kind::CHECK;
    chk.name = "chk_data";
    chk.check_expr = make_int("1");
    stmt->constraints.push_back(std::move(chk));

    TableConstraint fk;
    fk.kind = TableConstraint::Kind::FOREIGN_KEY;
    fk.name = "fk_ref";
    fk.columns = {"ref_id"};
    fk.fk_table = "other";
    fk.fk_columns = {"id"};
    fk.on_delete = ReferentialAction::CASCADE;
    stmt->constraints.push_back(std::move(fk));

    EXPECT_EQ(stmt->columns.size(), 3u);
    EXPECT_EQ(stmt->constraints.size(), 4u);
    EXPECT_EQ(stmt->constraints[3].fk_columns.size(), 1u);
    EXPECT_EQ(stmt->constraints[3].fk_columns[0], "id");
}

TEST(QA_GDB102, InsertWithSelectSubquery) {
    // INSERT INTO t (col) SELECT id FROM other
    auto stmt = std::make_unique<InsertStmt>();
    stmt->table_name = "target";
    stmt->columns = {"col"};

    auto sel = std::make_unique<SelectStmt>();
    SelectItem si;
    si.expr = make_col("id");
    sel->items.push_back(std::move(si));
    TableRef tr;
    tr.name = "source";
    sel->from.push_back(std::move(tr));

    stmt->select = std::move(sel);

    EXPECT_TRUE(stmt->values.empty());
    EXPECT_NE(stmt->select, nullptr);
}

TEST(QA_GDB102, UpdateWithMultipleAssignmentsAndReturning) {
    auto stmt = std::make_unique<UpdateStmt>();
    stmt->table_name = "t";

    for (int i = 0; i < 5; ++i) {
        Assignment a;
        a.column = "col_" + std::to_string(i);
        a.value = make_int(std::to_string(i));
        stmt->assignments.push_back(std::move(a));
    }

    stmt->where_expr = make_int("1");

    SelectItem ret;
    ret.is_star = true;
    stmt->returning.push_back(std::move(ret));

    EXPECT_EQ(stmt->assignments.size(), 5u);
    EXPECT_EQ(stmt->returning.size(), 1u);
}

TEST(QA_GDB102, CaseExprMultipleWhens) {
    auto c = std::make_unique<CaseExpr>();
    c->operand = make_col("status");

    for (int i = 0; i < 10; ++i) {
        CaseWhen w;
        w.condition = make_int(std::to_string(i));
        w.result = make_str("label_" + std::to_string(i));
        c->whens.push_back(std::move(w));
    }
    c->else_expr = make_str("unknown");

    EXPECT_EQ(c->whens.size(), 10u);
    EXPECT_NE(c->else_expr, nullptr);
    EXPECT_NE(c->operand, nullptr);
}

TEST(QA_GDB102, MatchStmtComplexPattern) {
    auto stmt = std::make_unique<MatchStmt>();

    // (a:User)-[r1:FOLLOWS]->(b:User)-[r2:LIKES]->(c:Post)
    PathElement pe1;
    pe1.node.variable = "a";
    pe1.node.label = "User";
    pe1.outgoing_edge = EdgePatternDef{"r1", "FOLLOWS", TraverseDirection::OUT};
    stmt->pattern.push_back(std::move(pe1));

    PathElement pe2;
    pe2.node.variable = "b";
    pe2.node.label = "User";
    pe2.outgoing_edge = EdgePatternDef{"r2", "LIKES", TraverseDirection::OUT};
    stmt->pattern.push_back(std::move(pe2));

    PathElement pe3;
    pe3.node.variable = "c";
    pe3.node.label = "Post";
    // No outgoing edge on last node
    stmt->pattern.push_back(std::move(pe3));

    // WHERE a.age > 18
    auto where = std::make_unique<BinaryExpr>();
    where->op = BinaryOp::GREATER;
    where->lhs = make_col("age", "a");
    where->rhs = make_int("18");
    stmt->where_expr = std::move(where);

    // RETURN a.name, c.title
    SelectItem r1;
    r1.expr = make_col("name", "a");
    stmt->return_items.push_back(std::move(r1));
    SelectItem r2;
    r2.expr = make_col("title", "c");
    stmt->return_items.push_back(std::move(r2));

    EXPECT_EQ(stmt->pattern.size(), 3u);
    EXPECT_TRUE(stmt->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(stmt->pattern[0].outgoing_edge->edge_type, "FOLLOWS");
    EXPECT_TRUE(stmt->pattern[1].outgoing_edge.has_value());
    EXPECT_EQ(stmt->pattern[1].outgoing_edge->edge_type, "LIKES");
    EXPECT_FALSE(stmt->pattern[2].outgoing_edge.has_value());
    EXPECT_NE(stmt->where_expr, nullptr);
    EXPECT_EQ(stmt->return_items.size(), 2u);
}

TEST(QA_GDB102, NearestWithinTraverse) {
    // NEAREST 5 FROM products.embedding TO [1,2,3] WITHIN TRAVERSE ...
    auto stmt = std::make_unique<NearestStmt>();
    stmt->k = make_int("5");
    stmt->table_name = "products";
    stmt->column_name = "embedding";
    stmt->metric = NearestMetric::L2;

    auto arr = std::make_unique<ArrayExpr>();
    arr->elements.push_back(make_int("1"));
    arr->elements.push_back(make_int("2"));
    arr->elements.push_back(make_int("3"));
    stmt->target = std::move(arr);

    auto traverse = std::make_unique<TraverseStmt>();
    traverse->edge_type = "belongs_to";
    traverse->from_table = "categories";
    traverse->from_key = make_int("42");
    traverse->direction = TraverseDirection::IN;
    traverse->max_depth = 2;
    stmt->within_traverse = std::move(traverse);

    stmt->where_expr = make_int("1");

    EXPECT_EQ(stmt->metric, NearestMetric::L2);
    EXPECT_NE(stmt->within_traverse, nullptr);
    EXPECT_NE(stmt->where_expr, nullptr);

    // The inner traverse should be correctly accessible
    auto* inner = dynamic_cast<TraverseStmt*>(stmt->within_traverse.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->direction, TraverseDirection::IN);
    EXPECT_EQ(inner->max_depth.value(), 2);
}

TEST(QA_GDB102, ExplainAnalyzeJsonFormat) {
    auto explain = std::make_unique<ExplainStmt>();
    explain->analyze = true;
    explain->format = ExplainFormat::JSON;

    auto inner = std::make_unique<SelectStmt>();
    SelectItem si;
    si.is_star = true;
    inner->items.push_back(std::move(si));
    explain->statement = std::move(inner);

    EXPECT_TRUE(explain->analyze);
    EXPECT_EQ(explain->format, ExplainFormat::JSON);
    EXPECT_NE(explain->statement, nullptr);
}

TEST(QA_GDB102, MultipleJoinsOnSelect) {
    auto stmt = std::make_unique<SelectStmt>();

    TableRef tr;
    tr.name = "a";
    stmt->from.push_back(std::move(tr));

    // Add 5 joins of different types
    JoinType types[] = {
        JoinType::INNER,
        JoinType::LEFT,
        JoinType::RIGHT,
        JoinType::FULL,
        JoinType::CROSS,
    };

    for (int i = 0; i < 5; ++i) {
        JoinClause jc;
        jc.type = types[i];
        jc.table.name = "t_" + std::to_string(i);
        if (types[i] != JoinType::CROSS) {
            auto on = std::make_unique<BinaryExpr>();
            on->op = BinaryOp::EQUAL;
            on->lhs = make_col("id", "a");
            on->rhs = make_col("fk", "t_" + std::to_string(i));
            jc.on_expr = std::move(on);
        }
        stmt->joins.push_back(std::move(jc));
    }

    EXPECT_EQ(stmt->joins.size(), 5u);
    EXPECT_EQ(stmt->joins[0].type, JoinType::INNER);
    EXPECT_EQ(stmt->joins[4].type, JoinType::CROSS);
    EXPECT_EQ(stmt->joins[4].on_expr, nullptr); // CROSS JOIN has no ON
}

TEST(QA_GDB102, FromSubquery) {
    // SELECT * FROM (SELECT id FROM users) AS sub
    auto stmt = std::make_unique<SelectStmt>();
    SelectItem si;
    si.is_star = true;
    stmt->items.push_back(std::move(si));

    auto sub = std::make_unique<SelectStmt>();
    SelectItem inner_si;
    inner_si.expr = make_col("id");
    sub->items.push_back(std::move(inner_si));
    TableRef inner_tr;
    inner_tr.name = "users";
    sub->from.push_back(std::move(inner_tr));

    TableRef from;
    from.alias = "sub";
    from.subquery = std::move(sub);
    stmt->from.push_back(std::move(from));

    EXPECT_TRUE(stmt->from[0].name.empty());
    EXPECT_EQ(stmt->from[0].alias, "sub");
    EXPECT_NE(stmt->from[0].subquery, nullptr);
}

TEST(QA_GDB102, InExprSubqueryVsValues) {
    // With values
    auto in1 = std::make_unique<InExpr>();
    in1->expr = make_col("id");
    in1->values.push_back(make_int("1"));
    in1->values.push_back(make_int("2"));
    EXPECT_FALSE(in1->values.empty());
    EXPECT_EQ(in1->subquery, nullptr);

    // With subquery
    auto in2 = std::make_unique<InExpr>();
    in2->expr = make_col("id");
    in2->subquery = std::make_unique<SelectStmt>();
    EXPECT_TRUE(in2->values.empty());
    EXPECT_NE(in2->subquery, nullptr);
}

TEST(QA_GDB102, EdgePropertyTypes) {
    CreateEdgeTypeStmt stmt;
    stmt.name = "weighted_follows";
    stmt.from_table = "users";
    stmt.to_table = "users";

    EdgeProperty p1;
    p1.name = "weight";
    p1.type.name = "FLOAT64";
    stmt.properties.push_back(std::move(p1));

    EdgeProperty p2;
    p2.name = "created_at";
    p2.type.name = "TIMESTAMP";
    stmt.properties.push_back(std::move(p2));

    EdgeProperty p3;
    p3.name = "metadata";
    p3.type.name = "JSON";
    stmt.properties.push_back(std::move(p3));

    EXPECT_EQ(stmt.properties.size(), 3u);
    EXPECT_EQ(stmt.properties[0].type.name, "FLOAT64");
    EXPECT_EQ(stmt.properties[1].type.name, "TIMESTAMP");
    EXPECT_EQ(stmt.properties[2].type.name, "JSON");
}

TEST(QA_GDB102, TypeSpecEmbeddingFullParams) {
    TypeSpec ts;
    ts.name = "EMBEDDING";
    ts.param1 = 768;
    ts.source = "description";
    ts.provider = "openai";

    EXPECT_EQ(ts.param1.value(), 768);
    EXPECT_EQ(ts.source, "description");
    EXPECT_EQ(ts.provider, "openai");
    EXPECT_FALSE(ts.param2.has_value()); // EMBEDDING doesn't use param2
}

TEST(QA_GDB102, ShortestPathWithAllFields) {
    ShortestPathStmt stmt;
    stmt.from_table = "cities";
    stmt.from_key = make_int("1");
    stmt.to_table = "cities";
    stmt.to_key = make_int("100");
    stmt.edge_type = "road";
    stmt.direction = TraverseDirection::BOTH;
    stmt.max_depth = 10;

    EXPECT_EQ(stmt.from_table, "cities");
    EXPECT_EQ(stmt.to_table, "cities");
    EXPECT_EQ(stmt.edge_type, "road");
    EXPECT_EQ(stmt.direction, TraverseDirection::BOTH);
    EXPECT_EQ(stmt.max_depth.value(), 10);
}

TEST(QA_GDB102, TraverseAllDirections) {
    for (auto dir : {TraverseDirection::IN, TraverseDirection::OUT, TraverseDirection::BOTH}) {
        TraverseStmt stmt;
        stmt.direction = dir;
        EXPECT_EQ(stmt.direction, dir);
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

// =============================================================================
// Move semantics for unique_ptr ownership
// =============================================================================

TEST(QA_GDB102, MoveExprOwnership) {
    auto lit = std::make_unique<LiteralExpr>();
    lit->kind = LiteralKind::INTEGER;
    lit->value = "99";

    ExprPtr base = std::move(lit);
    EXPECT_EQ(lit, nullptr); // NOLINT: testing moved-from state

    auto* restored = dynamic_cast<LiteralExpr*>(base.get());
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->value, "99");
}

TEST(QA_GDB102, MoveStmtIntoExplain) {
    auto sel = std::make_unique<SelectStmt>();
    SelectItem si;
    si.is_star = true;
    sel->items.push_back(std::move(si));

    auto explain = std::make_unique<ExplainStmt>();
    explain->statement = std::move(sel);

    EXPECT_EQ(sel, nullptr); // NOLINT: testing moved-from state
    auto* inner = dynamic_cast<SelectStmt*>(explain->statement.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->items.size(), 1u);
}

TEST(QA_GDB102, AlterTableDropColumn) {
    AlterTableStmt stmt;
    stmt.table_name = "users";
    stmt.action = AlterAction::DROP_COLUMN;
    stmt.column_name = "old_col";

    EXPECT_EQ(stmt.action, AlterAction::DROP_COLUMN);
    EXPECT_EQ(stmt.column_name, "old_col");
}

TEST(QA_GDB102, LinkWithMultipleProperties) {
    LinkStmt stmt;
    stmt.source_table = "users";
    stmt.source_key = make_int("1");
    stmt.target_table = "posts";
    stmt.target_key = make_int("42");
    stmt.edge_type = "authored";

    for (int i = 0; i < 3; ++i) {
        Assignment a;
        a.column = "prop_" + std::to_string(i);
        a.value = make_str("val_" + std::to_string(i));
        stmt.properties.push_back(std::move(a));
    }

    EXPECT_EQ(stmt.properties.size(), 3u);
    EXPECT_EQ(stmt.properties[0].column, "prop_0");
}

TEST(QA_GDB102, UnlinkWithWhere) {
    UnlinkStmt stmt;
    stmt.source_table = "users";
    stmt.source_key = make_int("1");
    stmt.target_table = "users";
    stmt.target_key = make_int("2");
    stmt.edge_type = "follows";
    stmt.where_expr = make_int("1");

    EXPECT_NE(stmt.where_expr, nullptr);
}

TEST(QA_GDB102, CreateUserAndAlterUser) {
    CreateUserStmt create;
    create.username = "admin";
    create.password = "secret123";

    AlterUserStmt alter;
    alter.username = "admin";
    alter.password = "newpass456";

    EXPECT_EQ(create.username, alter.username);
    EXPECT_NE(create.password, alter.password);
}

TEST(QA_GDB102, InsertMultipleRowsMultipleColumns) {
    InsertStmt stmt;
    stmt.table_name = "data";
    stmt.columns = {"a", "b", "c"};

    for (int row = 0; row < 100; ++row) {
        std::vector<ExprPtr> vals;
        for (int col = 0; col < 3; ++col) {
            vals.push_back(make_int(std::to_string(row * 3 + col)));
        }
        stmt.values.push_back(std::move(vals));
    }

    EXPECT_EQ(stmt.values.size(), 100u);
    EXPECT_EQ(stmt.values[0].size(), 3u);
    EXPECT_EQ(stmt.values[99].size(), 3u);
}

TEST(QA_GDB102, ArrayExprEmpty) {
    ArrayExpr arr;
    EXPECT_TRUE(arr.elements.empty());

    CountingVisitor v;
    arr.accept(v);
    EXPECT_EQ(v.last_type, "ArrayExpr");
}

TEST(QA_GDB102, ArrayExprLargeVector) {
    ArrayExpr arr;
    for (int i = 0; i < 384; ++i) {
        arr.elements.push_back(make_int(std::to_string(i)));
    }
    EXPECT_EQ(arr.elements.size(), 384u); // typical embedding dimension
}

TEST(QA_GDB102, DeleteWithReturningMultipleItems) {
    DeleteStmt stmt;
    stmt.table_name = "users";
    stmt.where_expr = make_int("1");

    SelectItem r1;
    r1.expr = make_col("id");
    stmt.returning.push_back(std::move(r1));

    SelectItem r2;
    r2.expr = make_col("name");
    r2.alias = "deleted_name";
    stmt.returning.push_back(std::move(r2));

    EXPECT_EQ(stmt.returning.size(), 2u);
    EXPECT_EQ(stmt.returning[1].alias, "deleted_name");
}

TEST(QA_GDB102, SelectMultipleFromTables) {
    SelectStmt stmt;
    for (int i = 0; i < 5; ++i) {
        TableRef tr;
        tr.name = "t" + std::to_string(i);
        tr.alias = "a" + std::to_string(i);
        stmt.from.push_back(std::move(tr));
    }
    EXPECT_EQ(stmt.from.size(), 5u);
}

TEST(QA_GDB102, SourceLocationOnExpr) {
    LiteralExpr e;
    e.line = 42;
    e.col = 10;
    EXPECT_EQ(e.line, 42u);
    EXPECT_EQ(e.col, 10u);

    // Set on a subexpression
    auto bin = std::make_unique<BinaryExpr>();
    bin->line = 100;
    bin->col = 200;
    bin->lhs = make_int("1");
    bin->rhs = make_int("2");

    EXPECT_EQ(bin->line, 100u);
    EXPECT_EQ(bin->col, 200u);
}
