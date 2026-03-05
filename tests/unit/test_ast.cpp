#include "sixseven/parser/ast.h"

#include <gtest/gtest.h>

using namespace sixseven;

// -- Helper factories --------------------------------------------------------

static ExprPtr make_int(const std::string& v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::INTEGER;
    e->value = v;
    return e;
}

static ExprPtr make_string(const std::string& v) {
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

static ExprPtr make_null() {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::NULL_LITERAL;
    return e;
}

static ExprPtr make_bool(bool v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::BOOLEAN;
    e->value = v ? "true" : "false";
    return e;
}

// -- Expression tests --------------------------------------------------------

TEST(Ast, LiteralExprInteger) {
    auto e = make_int("42");
    auto* lit = dynamic_cast<LiteralExpr*>(e.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::INTEGER);
    EXPECT_EQ(lit->value, "42");
}

TEST(Ast, LiteralExprString) {
    auto e = make_string("hello");
    auto* lit = dynamic_cast<LiteralExpr*>(e.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::STRING);
    EXPECT_EQ(lit->value, "hello");
}

TEST(Ast, LiteralExprNull) {
    auto e = make_null();
    auto* lit = dynamic_cast<LiteralExpr*>(e.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::NULL_LITERAL);
    EXPECT_TRUE(lit->value.empty());
}

TEST(Ast, LiteralExprBoolean) {
    auto e = make_bool(true);
    auto* lit = dynamic_cast<LiteralExpr*>(e.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::BOOLEAN);
    EXPECT_EQ(lit->value, "true");
}

TEST(Ast, LiteralExprFloat) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::FLOAT;
    e->value = "3.14";
    EXPECT_EQ(e->kind, LiteralKind::FLOAT);
    EXPECT_EQ(e->value, "3.14");
}

TEST(Ast, ColumnRefUnqualified) {
    auto e = make_col("name");
    auto* col = dynamic_cast<ColumnRefExpr*>(e.get());
    ASSERT_NE(col, nullptr);
    EXPECT_TRUE(col->table.empty());
    EXPECT_EQ(col->column, "name");
}

TEST(Ast, ColumnRefQualified) {
    auto e = make_col("id", "users");
    auto* col = dynamic_cast<ColumnRefExpr*>(e.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->table, "users");
    EXPECT_EQ(col->column, "id");
}

TEST(Ast, BinaryExpr) {
    auto bin = std::make_unique<BinaryExpr>();
    bin->op = BinaryOp::ADD;
    bin->lhs = make_int("1");
    bin->rhs = make_int("2");

    EXPECT_EQ(bin->op, BinaryOp::ADD);
    auto* lhs = dynamic_cast<LiteralExpr*>(bin->lhs.get());
    auto* rhs = dynamic_cast<LiteralExpr*>(bin->rhs.get());
    ASSERT_NE(lhs, nullptr);
    ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(lhs->value, "1");
    EXPECT_EQ(rhs->value, "2");
}

TEST(Ast, DeepBinaryTree) {
    // Build: ((1 + 2) * (3 - 4))
    auto add = std::make_unique<BinaryExpr>();
    add->op = BinaryOp::ADD;
    add->lhs = make_int("1");
    add->rhs = make_int("2");

    auto sub = std::make_unique<BinaryExpr>();
    sub->op = BinaryOp::SUBTRACT;
    sub->lhs = make_int("3");
    sub->rhs = make_int("4");

    auto mul = std::make_unique<BinaryExpr>();
    mul->op = BinaryOp::MULTIPLY;
    mul->lhs = std::move(add);
    mul->rhs = std::move(sub);

    auto* top = dynamic_cast<BinaryExpr*>(mul.get());
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(top->op, BinaryOp::MULTIPLY);
    auto* left = dynamic_cast<BinaryExpr*>(top->lhs.get());
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(left->op, BinaryOp::ADD);
}

TEST(Ast, UnaryExpr) {
    auto neg = std::make_unique<UnaryExpr>();
    neg->op = UnaryOp::NEGATE;
    neg->operand = make_int("5");

    EXPECT_EQ(neg->op, UnaryOp::NEGATE);
    auto* operand = dynamic_cast<LiteralExpr*>(neg->operand.get());
    ASSERT_NE(operand, nullptr);
    EXPECT_EQ(operand->value, "5");
}

TEST(Ast, UnaryNot) {
    auto not_expr = std::make_unique<UnaryExpr>();
    not_expr->op = UnaryOp::NOT;
    not_expr->operand = make_bool(true);
    EXPECT_EQ(not_expr->op, UnaryOp::NOT);
}

TEST(Ast, FunctionCallExpr) {
    auto fn = std::make_unique<FunctionCallExpr>();
    fn->name = "COUNT";
    fn->distinct = true;
    fn->args.push_back(make_col("id"));

    EXPECT_EQ(fn->name, "COUNT");
    EXPECT_TRUE(fn->distinct);
    EXPECT_EQ(fn->args.size(), 1u);
}

TEST(Ast, FunctionCallMultipleArgs) {
    auto fn = std::make_unique<FunctionCallExpr>();
    fn->name = "COALESCE";
    fn->args.push_back(make_col("a"));
    fn->args.push_back(make_col("b"));
    fn->args.push_back(make_int("0"));

    EXPECT_EQ(fn->args.size(), 3u);
}

TEST(Ast, CastExpr) {
    auto cast = std::make_unique<CastExpr>();
    cast->expr = make_string("42");
    cast->target_type.name = "INT";

    auto* inner = dynamic_cast<LiteralExpr*>(cast->expr.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(cast->target_type.name, "INT");
}

TEST(Ast, CaseExprSearched) {
    auto case_expr = std::make_unique<CaseExpr>();
    // Searched CASE (no operand).
    CaseWhen w;
    w.condition = make_bool(true);
    w.result = make_string("yes");
    case_expr->whens.push_back(std::move(w));
    case_expr->else_expr = make_string("no");

    EXPECT_EQ(case_expr->operand, nullptr);
    EXPECT_EQ(case_expr->whens.size(), 1u);
    EXPECT_NE(case_expr->else_expr, nullptr);
}

TEST(Ast, CaseExprSimple) {
    auto case_expr = std::make_unique<CaseExpr>();
    case_expr->operand = make_col("status");
    CaseWhen w;
    w.condition = make_int("1");
    w.result = make_string("active");
    case_expr->whens.push_back(std::move(w));

    EXPECT_NE(case_expr->operand, nullptr);
}

TEST(Ast, InExprValues) {
    auto in_expr = std::make_unique<InExpr>();
    in_expr->expr = make_col("id");
    in_expr->values.push_back(make_int("1"));
    in_expr->values.push_back(make_int("2"));
    in_expr->values.push_back(make_int("3"));
    in_expr->negated = false;

    EXPECT_EQ(in_expr->values.size(), 3u);
    EXPECT_FALSE(in_expr->negated);
    EXPECT_EQ(in_expr->subquery, nullptr);
}

TEST(Ast, InExprNotIn) {
    auto in_expr = std::make_unique<InExpr>();
    in_expr->expr = make_col("id");
    in_expr->values.push_back(make_int("1"));
    in_expr->negated = true;

    EXPECT_TRUE(in_expr->negated);
}

TEST(Ast, BetweenExpr) {
    auto between = std::make_unique<BetweenExpr>();
    between->expr = make_col("age");
    between->low = make_int("18");
    between->high = make_int("65");
    between->negated = false;

    auto* lo = dynamic_cast<LiteralExpr*>(between->low.get());
    auto* hi = dynamic_cast<LiteralExpr*>(between->high.get());
    ASSERT_NE(lo, nullptr);
    ASSERT_NE(hi, nullptr);
    EXPECT_EQ(lo->value, "18");
    EXPECT_EQ(hi->value, "65");
}

TEST(Ast, IsNullExpr) {
    auto is_null = std::make_unique<IsNullExpr>();
    is_null->expr = make_col("email");
    is_null->negated = false;

    EXPECT_FALSE(is_null->negated);
}

TEST(Ast, IsNotNullExpr) {
    auto is_null = std::make_unique<IsNullExpr>();
    is_null->expr = make_col("email");
    is_null->negated = true;

    EXPECT_TRUE(is_null->negated);
}

TEST(Ast, LikeExpr) {
    auto like = std::make_unique<LikeExpr>();
    like->expr = make_col("name");
    like->pattern = make_string("%smith%");
    like->negated = false;

    EXPECT_FALSE(like->negated);
}

TEST(Ast, ArrayExpr) {
    auto arr = std::make_unique<ArrayExpr>();
    arr->elements.push_back(make_int("1"));
    arr->elements.push_back(make_int("2"));
    arr->elements.push_back(make_int("3"));

    EXPECT_EQ(arr->elements.size(), 3u);
}

TEST(Ast, ExprLineColumn) {
    auto e = make_int("42");
    e->line = 5;
    e->col = 10;
    EXPECT_EQ(e->line, 5u);
    EXPECT_EQ(e->col, 10u);
}

// -- Supporting type tests ---------------------------------------------------

TEST(Ast, TypeSpecBasic) {
    TypeSpec ts;
    ts.name = "INT";
    EXPECT_EQ(ts.name, "INT");
    EXPECT_FALSE(ts.param1.has_value());
    EXPECT_FALSE(ts.param2.has_value());
}

TEST(Ast, TypeSpecVarchar) {
    TypeSpec ts;
    ts.name = "VARCHAR";
    ts.param1 = 255;
    EXPECT_EQ(ts.param1.value(), 255);
}

TEST(Ast, TypeSpecDecimal) {
    TypeSpec ts;
    ts.name = "DECIMAL";
    ts.param1 = 10;
    ts.param2 = 2;
    EXPECT_EQ(ts.param1.value(), 10);
    EXPECT_EQ(ts.param2.value(), 2);
}

TEST(Ast, TypeSpecEmbedding) {
    TypeSpec ts;
    ts.name = "EMBEDDING";
    ts.param1 = 384;
    ts.source = "description";
    ts.provider = "openai";
    EXPECT_EQ(ts.param1.value(), 384);
    EXPECT_EQ(ts.source, "description");
    EXPECT_EQ(ts.provider, "openai");
}

TEST(Ast, AstColumnDefWithDefaults) {
    AstColumnDef cd;
    cd.name = "age";
    cd.type.name = "INT";
    cd.nullable = false;
    cd.default_expr = make_int("0");

    EXPECT_EQ(cd.name, "age");
    EXPECT_FALSE(cd.nullable);
    auto* def = dynamic_cast<LiteralExpr*>(cd.default_expr.get());
    ASSERT_NE(def, nullptr);
    EXPECT_EQ(def->value, "0");
}

TEST(Ast, AstColumnDefWithForeignKey) {
    AstColumnDef cd;
    cd.name = "user_id";
    cd.type.name = "INT";
    cd.fk_table = "users";
    cd.fk_column = "id";
    cd.fk_on_delete = ReferentialAction::CASCADE;

    EXPECT_EQ(cd.fk_table, "users");
    EXPECT_EQ(cd.fk_on_delete, ReferentialAction::CASCADE);
}

TEST(Ast, TableConstraintPrimaryKey) {
    TableConstraint tc;
    tc.kind = TableConstraint::Kind::PRIMARY_KEY;
    tc.columns = {"id"};

    EXPECT_EQ(tc.kind, TableConstraint::Kind::PRIMARY_KEY);
    EXPECT_EQ(tc.columns.size(), 1u);
}

TEST(Ast, TableConstraintCompositePK) {
    TableConstraint tc;
    tc.kind = TableConstraint::Kind::PRIMARY_KEY;
    tc.columns = {"user_id", "role_id"};

    EXPECT_EQ(tc.columns.size(), 2u);
}

TEST(Ast, TableRefSimple) {
    TableRef tr;
    tr.name = "users";
    tr.alias = "u";

    EXPECT_EQ(tr.name, "users");
    EXPECT_EQ(tr.alias, "u");
    EXPECT_EQ(tr.subquery, nullptr);
}

TEST(Ast, OrderByItem) {
    OrderByItem item;
    item.expr = make_col("name");
    item.direction = SortDirection::DESC;

    EXPECT_EQ(item.direction, SortDirection::DESC);
}

TEST(Ast, SelectItemStar) {
    SelectItem si;
    si.is_star = true;
    EXPECT_TRUE(si.is_star);
    EXPECT_EQ(si.expr, nullptr);
}

TEST(Ast, SelectItemTableStar) {
    SelectItem si;
    si.is_star = true;
    si.table_star = "users";
    EXPECT_EQ(si.table_star, "users");
}

TEST(Ast, SelectItemExprAlias) {
    SelectItem si;
    si.expr = make_col("name");
    si.alias = "user_name";
    EXPECT_EQ(si.alias, "user_name");
}

// -- DDL statement tests -----------------------------------------------------

TEST(Ast, CreateTableStmt) {
    auto stmt = std::make_unique<CreateTableStmt>();
    stmt->name = "users";
    stmt->if_not_exists = true;

    AstColumnDef id;
    id.name = "id";
    id.type.name = "INT";
    id.nullable = false;
    stmt->columns.push_back(std::move(id));

    AstColumnDef name;
    name.name = "name";
    name.type.name = "VARCHAR";
    name.type.param1 = 100;
    stmt->columns.push_back(std::move(name));

    TableConstraint pk;
    pk.kind = TableConstraint::Kind::PRIMARY_KEY;
    pk.columns = {"id"};
    stmt->constraints.push_back(std::move(pk));

    EXPECT_EQ(stmt->name, "users");
    EXPECT_TRUE(stmt->if_not_exists);
    EXPECT_EQ(stmt->columns.size(), 2u);
    EXPECT_EQ(stmt->constraints.size(), 1u);
}

TEST(Ast, DropTableStmt) {
    auto stmt = std::make_unique<DropTableStmt>();
    stmt->name = "old_table";
    stmt->if_exists = true;
    stmt->cascade = true;

    EXPECT_EQ(stmt->name, "old_table");
    EXPECT_TRUE(stmt->if_exists);
    EXPECT_TRUE(stmt->cascade);
}

TEST(Ast, AlterTableAddColumn) {
    auto stmt = std::make_unique<AlterTableStmt>();
    stmt->table_name = "users";
    stmt->action = AlterAction::ADD_COLUMN;
    stmt->column.name = "email";
    stmt->column.type.name = "VARCHAR";
    stmt->column.type.param1 = 255;

    EXPECT_EQ(stmt->action, AlterAction::ADD_COLUMN);
    EXPECT_EQ(stmt->column.name, "email");
}

TEST(Ast, AlterTableRenameColumn) {
    auto stmt = std::make_unique<AlterTableStmt>();
    stmt->table_name = "users";
    stmt->action = AlterAction::RENAME_COLUMN;
    stmt->column_name = "old_name";
    stmt->new_column_name = "new_name";

    EXPECT_EQ(stmt->action, AlterAction::RENAME_COLUMN);
    EXPECT_EQ(stmt->column_name, "old_name");
    EXPECT_EQ(stmt->new_column_name, "new_name");
}

TEST(Ast, CreateIndexStmt) {
    auto stmt = std::make_unique<CreateIndexStmt>();
    stmt->name = "idx_users_name";
    stmt->table_name = "users";
    stmt->columns = {"last_name", "first_name"};
    stmt->is_unique = true;
    stmt->method = "btree";

    EXPECT_EQ(stmt->name, "idx_users_name");
    EXPECT_EQ(stmt->columns.size(), 2u);
    EXPECT_TRUE(stmt->is_unique);
    EXPECT_EQ(stmt->method, "btree");
}

TEST(Ast, DropIndexStmt) {
    auto stmt = std::make_unique<DropIndexStmt>();
    stmt->name = "idx_old";
    stmt->if_exists = true;
    EXPECT_TRUE(stmt->if_exists);
}

TEST(Ast, CreateEdgeTypeStmt) {
    auto stmt = std::make_unique<CreateEdgeTypeStmt>();
    stmt->name = "follows";
    stmt->from_table = "users";
    stmt->to_table = "users";

    EdgeProperty since;
    since.name = "since";
    since.type.name = "TIMESTAMP";
    stmt->properties.push_back(std::move(since));

    EXPECT_EQ(stmt->name, "follows");
    EXPECT_EQ(stmt->from_table, "users");
    EXPECT_EQ(stmt->properties.size(), 1u);
}

TEST(Ast, DropEdgeTypeStmt) {
    auto stmt = std::make_unique<DropEdgeTypeStmt>();
    stmt->name = "follows";
    stmt->if_exists = true;
    EXPECT_TRUE(stmt->if_exists);
}

// -- DML statement tests -----------------------------------------------------

TEST(Ast, InsertStmtValues) {
    auto stmt = std::make_unique<InsertStmt>();
    stmt->table_name = "users";
    stmt->columns = {"name", "age"};

    std::vector<ExprPtr> row;
    row.push_back(make_string("Alice"));
    row.push_back(make_int("30"));
    stmt->values.push_back(std::move(row));

    EXPECT_EQ(stmt->table_name, "users");
    EXPECT_EQ(stmt->columns.size(), 2u);
    EXPECT_EQ(stmt->values.size(), 1u);
    EXPECT_EQ(stmt->values[0].size(), 2u);
    EXPECT_EQ(stmt->select, nullptr);
}

TEST(Ast, InsertStmtMultipleRows) {
    auto stmt = std::make_unique<InsertStmt>();
    stmt->table_name = "data";
    stmt->columns = {"x"};

    for (int i = 0; i < 3; ++i) {
        std::vector<ExprPtr> row;
        row.push_back(make_int(std::to_string(i)));
        stmt->values.push_back(std::move(row));
    }

    EXPECT_EQ(stmt->values.size(), 3u);
}

TEST(Ast, UpdateStmt) {
    auto stmt = std::make_unique<UpdateStmt>();
    stmt->table_name = "users";

    Assignment a;
    a.column = "name";
    a.value = make_string("Bob");
    stmt->assignments.push_back(std::move(a));

    auto eq = std::make_unique<BinaryExpr>();
    eq->op = BinaryOp::EQUAL;
    eq->lhs = make_col("id");
    eq->rhs = make_int("1");
    stmt->where_expr = std::move(eq);

    EXPECT_EQ(stmt->table_name, "users");
    EXPECT_EQ(stmt->assignments.size(), 1u);
    EXPECT_NE(stmt->where_expr, nullptr);
}

TEST(Ast, DeleteStmt) {
    auto stmt = std::make_unique<DeleteStmt>();
    stmt->table_name = "users";
    stmt->where_expr = make_bool(true);

    EXPECT_EQ(stmt->table_name, "users");
    EXPECT_NE(stmt->where_expr, nullptr);
}

TEST(Ast, LinkStmt) {
    auto stmt = std::make_unique<LinkStmt>();
    stmt->source_table = "users";
    stmt->source_key = make_int("1");
    stmt->target_table = "users";
    stmt->target_key = make_int("2");
    stmt->edge_type = "follows";

    Assignment since;
    since.column = "since";
    since.value = make_string("2024-01-01");
    stmt->properties.push_back(std::move(since));

    EXPECT_EQ(stmt->edge_type, "follows");
    EXPECT_EQ(stmt->properties.size(), 1u);
}

TEST(Ast, UnlinkStmt) {
    auto stmt = std::make_unique<UnlinkStmt>();
    stmt->source_table = "users";
    stmt->source_key = make_int("1");
    stmt->target_table = "users";
    stmt->target_key = make_int("2");
    stmt->edge_type = "follows";

    EXPECT_EQ(stmt->edge_type, "follows");
    EXPECT_EQ(stmt->where_expr, nullptr);
}

// -- Query statement tests ---------------------------------------------------

TEST(Ast, SimpleSelectStmt) {
    auto stmt = std::make_unique<SelectStmt>();
    stmt->distinct = false;

    SelectItem si;
    si.is_star = true;
    stmt->items.push_back(std::move(si));

    TableRef tr;
    tr.name = "users";
    stmt->from.push_back(std::move(tr));

    EXPECT_FALSE(stmt->distinct);
    EXPECT_EQ(stmt->items.size(), 1u);
    EXPECT_TRUE(stmt->items[0].is_star);
    EXPECT_EQ(stmt->from.size(), 1u);
}

TEST(Ast, SelectWithJoin) {
    auto stmt = std::make_unique<SelectStmt>();

    SelectItem si;
    si.is_star = true;
    stmt->items.push_back(std::move(si));

    TableRef tr;
    tr.name = "users";
    tr.alias = "u";
    stmt->from.push_back(std::move(tr));

    JoinClause join;
    join.type = JoinType::LEFT;
    join.table.name = "orders";
    join.table.alias = "o";
    auto on = std::make_unique<BinaryExpr>();
    on->op = BinaryOp::EQUAL;
    on->lhs = make_col("id", "u");
    on->rhs = make_col("user_id", "o");
    join.on_expr = std::move(on);
    stmt->joins.push_back(std::move(join));

    EXPECT_EQ(stmt->joins.size(), 1u);
    EXPECT_EQ(stmt->joins[0].type, JoinType::LEFT);
}

TEST(Ast, SelectWithGroupByHaving) {
    auto stmt = std::make_unique<SelectStmt>();

    SelectItem si;
    si.expr = make_col("status");
    stmt->items.push_back(std::move(si));

    stmt->group_by.push_back(make_col("status"));

    auto having = std::make_unique<BinaryExpr>();
    having->op = BinaryOp::GREATER;
    auto fn = std::make_unique<FunctionCallExpr>();
    fn->name = "COUNT";
    fn->args.push_back(make_col("id"));
    having->lhs = std::move(fn);
    having->rhs = make_int("5");
    stmt->having_expr = std::move(having);

    EXPECT_EQ(stmt->group_by.size(), 1u);
    EXPECT_NE(stmt->having_expr, nullptr);
}

TEST(Ast, SelectWithOrderByLimitOffset) {
    auto stmt = std::make_unique<SelectStmt>();

    SelectItem si;
    si.is_star = true;
    stmt->items.push_back(std::move(si));

    OrderByItem ob;
    ob.expr = make_col("name");
    ob.direction = SortDirection::ASC;
    stmt->order_by.push_back(std::move(ob));

    stmt->limit = make_int("10");
    stmt->offset = make_int("20");

    EXPECT_EQ(stmt->order_by.size(), 1u);
    EXPECT_EQ(stmt->order_by[0].direction, SortDirection::ASC);
    EXPECT_NE(stmt->limit, nullptr);
    EXPECT_NE(stmt->offset, nullptr);
}

TEST(Ast, SelectWithCTE) {
    auto stmt = std::make_unique<SelectStmt>();

    auto inner = std::make_unique<SelectStmt>();
    SelectItem si;
    si.is_star = true;
    inner->items.push_back(std::move(si));
    TableRef tr;
    tr.name = "users";
    inner->from.push_back(std::move(tr));

    SelectStmt::CTE cte;
    cte.name = "active_users";
    cte.query = std::move(inner);
    stmt->ctes.push_back(std::move(cte));

    EXPECT_EQ(stmt->ctes.size(), 1u);
    EXPECT_EQ(stmt->ctes[0].name, "active_users");
}

TEST(Ast, SelectWithUnion) {
    auto left = std::make_unique<SelectStmt>();
    SelectItem si1;
    si1.expr = make_col("name");
    left->items.push_back(std::move(si1));

    auto right = std::make_unique<SelectStmt>();
    SelectItem si2;
    si2.expr = make_col("name");
    right->items.push_back(std::move(si2));

    left->set_op = SelectStmt::SetOp::UNION;
    left->set_rhs = std::move(right);

    EXPECT_EQ(left->set_op, SelectStmt::SetOp::UNION);
    EXPECT_NE(left->set_rhs, nullptr);
}

TEST(Ast, TraverseStmt) {
    auto stmt = std::make_unique<TraverseStmt>();
    stmt->edge_type = "follows";
    stmt->from_table = "users";
    stmt->from_key = make_int("1");
    stmt->direction = TraverseDirection::OUT;
    stmt->max_depth = 3;
    stmt->fetch = true;

    EXPECT_EQ(stmt->edge_type, "follows");
    EXPECT_EQ(stmt->direction, TraverseDirection::OUT);
    EXPECT_EQ(stmt->max_depth.value(), 3);
    EXPECT_TRUE(stmt->fetch);
}

TEST(Ast, NearestStmt) {
    auto stmt = std::make_unique<NearestStmt>();
    stmt->k = make_int("10");
    stmt->table_name = "products";
    stmt->column_name = "embedding";
    stmt->metric = NearestMetric::COSINE;

    auto arr = std::make_unique<ArrayExpr>();
    arr->elements.push_back(make_int("1"));
    arr->elements.push_back(make_int("2"));
    stmt->target = std::move(arr);

    EXPECT_EQ(stmt->table_name, "products");
    EXPECT_EQ(stmt->metric, NearestMetric::COSINE);
    auto* target = dynamic_cast<ArrayExpr*>(stmt->target.get());
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->elements.size(), 2u);
}

TEST(Ast, MatchStmt) {
    auto stmt = std::make_unique<MatchStmt>();

    PathElement pe1;
    pe1.node.variable = "a";
    pe1.node.label = "users";
    pe1.outgoing_edge = EdgePatternDef{};
    pe1.outgoing_edge->variable = "r";
    pe1.outgoing_edge->edge_type = "knows";
    pe1.outgoing_edge->direction = TraverseDirection::OUT;
    stmt->pattern.push_back(std::move(pe1));

    PathElement pe2;
    pe2.node.variable = "b";
    pe2.node.label = "users";
    stmt->pattern.push_back(std::move(pe2));

    SelectItem ret;
    ret.expr = make_col("name", "a");
    stmt->return_items.push_back(std::move(ret));

    EXPECT_EQ(stmt->pattern.size(), 2u);
    EXPECT_EQ(stmt->pattern[0].node.label, "users");
    EXPECT_TRUE(stmt->pattern[0].outgoing_edge.has_value());
    EXPECT_FALSE(stmt->pattern[1].outgoing_edge.has_value());
    EXPECT_EQ(stmt->return_items.size(), 1u);
}

TEST(Ast, ShortestPathStmt) {
    auto stmt = std::make_unique<ShortestPathStmt>();
    stmt->from_table = "users";
    stmt->from_key = make_int("1");
    stmt->to_table = "users";
    stmt->to_key = make_int("100");
    stmt->edge_type = "knows";
    stmt->direction = TraverseDirection::BOTH;
    stmt->max_depth = 6;

    EXPECT_EQ(stmt->edge_type, "knows");
    EXPECT_EQ(stmt->direction, TraverseDirection::BOTH);
    EXPECT_EQ(stmt->max_depth.value(), 6);
}

// -- TCL statement tests -----------------------------------------------------

TEST(Ast, BeginStmt) {
    auto stmt = std::make_unique<BeginStmt>();
    auto* s = dynamic_cast<Stmt*>(stmt.get());
    EXPECT_NE(s, nullptr);
}

TEST(Ast, CommitStmt) {
    auto stmt = std::make_unique<CommitStmt>();
    auto* s = dynamic_cast<Stmt*>(stmt.get());
    EXPECT_NE(s, nullptr);
}

TEST(Ast, RollbackStmtPlain) {
    auto stmt = std::make_unique<RollbackStmt>();
    EXPECT_TRUE(stmt->savepoint.empty());
}

TEST(Ast, RollbackToSavepoint) {
    auto stmt = std::make_unique<RollbackStmt>();
    stmt->savepoint = "sp1";
    EXPECT_EQ(stmt->savepoint, "sp1");
}

TEST(Ast, SavepointStmt) {
    auto stmt = std::make_unique<SavepointStmt>();
    stmt->name = "before_update";
    EXPECT_EQ(stmt->name, "before_update");
}

// -- Admin statement tests ---------------------------------------------------

TEST(Ast, SetStmt) {
    auto stmt = std::make_unique<SetStmt>();
    stmt->parameter = "max_connections";
    stmt->value = make_int("100");

    EXPECT_EQ(stmt->parameter, "max_connections");
}

TEST(Ast, ShowTablesStmt) {
    auto stmt = std::make_unique<ShowStmt>();
    stmt->target = ShowTarget::TABLES;
    EXPECT_EQ(stmt->target, ShowTarget::TABLES);
}

TEST(Ast, ShowColumnsStmt) {
    auto stmt = std::make_unique<ShowStmt>();
    stmt->target = ShowTarget::COLUMNS;
    stmt->name = "users";
    EXPECT_EQ(stmt->name, "users");
}

TEST(Ast, ExplainStmt) {
    auto inner = std::make_unique<SelectStmt>();
    SelectItem si;
    si.is_star = true;
    inner->items.push_back(std::move(si));

    auto stmt = std::make_unique<ExplainStmt>();
    stmt->statement = std::move(inner);
    stmt->analyze = true;

    EXPECT_TRUE(stmt->analyze);
    EXPECT_NE(stmt->statement, nullptr);
}

TEST(Ast, DescribeStmt) {
    auto stmt = std::make_unique<DescribeStmt>();
    stmt->table_name = "users";
    EXPECT_EQ(stmt->table_name, "users");
}

TEST(Ast, ReembedStmt) {
    auto stmt = std::make_unique<ReembedStmt>();
    stmt->table_name = "products";
    EXPECT_EQ(stmt->table_name, "products");
}

TEST(Ast, VacuumStmt) {
    auto stmt = std::make_unique<VacuumStmt>();
    stmt->table_name = "users";
    EXPECT_EQ(stmt->table_name, "users");
}

TEST(Ast, VacuumStmtAll) {
    auto stmt = std::make_unique<VacuumStmt>();
    EXPECT_TRUE(stmt->table_name.empty());
}

TEST(Ast, AnalyzeStmt) {
    auto stmt = std::make_unique<AnalyzeStmt>();
    stmt->table_name = "orders";
    EXPECT_EQ(stmt->table_name, "orders");
}

// -- Move semantics test -----------------------------------------------------

TEST(Ast, ExprMoveSemantics) {
    auto e = make_int("42");
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

// -- Subquery tests ----------------------------------------------------------

TEST(Ast, InExprWithSubquery) {
    auto sub = std::make_unique<SelectStmt>();
    SelectItem si;
    si.expr = make_col("id");
    sub->items.push_back(std::move(si));
    TableRef tr;
    tr.name = "admins";
    sub->from.push_back(std::move(tr));

    auto in_expr = std::make_unique<InExpr>();
    in_expr->expr = make_col("user_id");
    in_expr->subquery = std::move(sub);

    EXPECT_NE(in_expr->subquery, nullptr);
    EXPECT_TRUE(in_expr->values.empty());
}

TEST(Ast, ExistsExpr) {
    auto sub = std::make_unique<SelectStmt>();
    SelectItem si;
    si.expr = make_int("1");
    sub->items.push_back(std::move(si));

    auto exists = std::make_unique<ExistsExpr>();
    exists->subquery = std::move(sub);

    EXPECT_NE(exists->subquery, nullptr);
}

TEST(Ast, SubqueryExpr) {
    auto sub = std::make_unique<SelectStmt>();
    SelectItem si;
    si.expr = make_col("count");
    sub->items.push_back(std::move(si));

    auto sq = std::make_unique<SubqueryExpr>();
    sq->subquery = std::move(sub);

    EXPECT_NE(sq->subquery, nullptr);
}

TEST(Ast, TableRefSubquery) {
    auto sub = std::make_unique<SelectStmt>();
    SelectItem si;
    si.is_star = true;
    sub->items.push_back(std::move(si));
    TableRef inner_tr;
    inner_tr.name = "users";
    sub->from.push_back(std::move(inner_tr));

    TableRef tr;
    tr.alias = "sub";
    tr.subquery = std::move(sub);

    EXPECT_TRUE(tr.name.empty());
    EXPECT_EQ(tr.alias, "sub");
    EXPECT_NE(tr.subquery, nullptr);
}

// -- Returning clause test ---------------------------------------------------

TEST(Ast, InsertWithReturning) {
    auto stmt = std::make_unique<InsertStmt>();
    stmt->table_name = "users";
    stmt->columns = {"name"};

    std::vector<ExprPtr> row;
    row.push_back(make_string("Alice"));
    stmt->values.push_back(std::move(row));

    SelectItem ret;
    ret.expr = make_col("id");
    stmt->returning.push_back(std::move(ret));

    EXPECT_EQ(stmt->returning.size(), 1u);
}

TEST(Ast, DeleteWithReturning) {
    auto stmt = std::make_unique<DeleteStmt>();
    stmt->table_name = "users";
    stmt->where_expr = make_bool(true);

    SelectItem ret;
    ret.is_star = true;
    stmt->returning.push_back(std::move(ret));

    EXPECT_EQ(stmt->returning.size(), 1u);
}
