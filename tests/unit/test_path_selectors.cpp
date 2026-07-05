#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/executor/shortest_path.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/planner/binder.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

// ===========================================================================
// Parser tests for path selectors
//
// NOTE (GDB-1215): The MatchShortestPathOperator-specific executor tests
// that used to live in this file (MatchShortestPathTest fixture and
// PlanNodeNamePerSelector) have moved to
// tests/unit/test_match_shortest_path.cpp to align the source->test naming
// convention with src/executor/match_shortest_path.cpp. This file retains
// parser tests, Path-function tests, PathSelector AST tests, and the
// legacy ShortestPathOperator backward-compat tests, none of which are
// specific to MatchShortestPathOperator.
// ===========================================================================

namespace {

std::vector<StmtPtr> parse_ok(std::string_view sql) {
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

StmtPtr parse_one(std::string_view sql) {
    auto stmts = parse_ok(sql);
    EXPECT_EQ(stmts.size(), 1u);
    if (stmts.size() != 1)
        return nullptr;
    return std::move(stmts[0]);
}

} // namespace

TEST(PathSelectorParser, AnyShortestParsed) {
    auto stmt =
        parse_one("MATCH p = ANY SHORTEST (a:persons)-[:knows]->{1,10}(b:persons) RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::ANY_SHORTEST);
    EXPECT_EQ(m->path_variable, "p");
    EXPECT_EQ(m->shortest_k, 0);
    ASSERT_EQ(m->pattern.size(), 2u);
    EXPECT_EQ(m->pattern[0].node.variable, "a");
    EXPECT_EQ(m->pattern[0].node.label, "persons");
}

TEST(PathSelectorParser, AllShortestParsed) {
    auto stmt =
        parse_one("MATCH p = ALL SHORTEST (a:persons)-[:knows]->{1,10}(b:persons) RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::ALL_SHORTEST);
    EXPECT_EQ(m->path_variable, "p");
    EXPECT_EQ(m->shortest_k, 0);
}

TEST(PathSelectorParser, ShortestKParsed) {
    auto stmt =
        parse_one("MATCH p = SHORTEST 3 (a:persons)-[:knows]->{1,10}(b:persons) RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::SHORTEST_K);
    EXPECT_EQ(m->path_variable, "p");
    EXPECT_EQ(m->shortest_k, 3);
}

TEST(PathSelectorParser, NoPathSelector) {
    auto stmt = parse_one("MATCH (a:persons)-[:knows]->(b:persons) RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::NONE);
    EXPECT_TRUE(m->path_variable.empty());
}

TEST(PathSelectorParser, SelectFromMatchAnyShortest) {
    auto stmt = parse_one("SELECT a.name, b.name "
                          "FROM MATCH p = ANY SHORTEST (a:persons)-[:knows]->{1,10}(b:persons) "
                          "WHERE a.id = 1");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    ASSERT_NE(sel->from[0].match_source, nullptr);

    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::ANY_SHORTEST);
    EXPECT_EQ(m->path_variable, "p");
}

// ===========================================================================
// Path function tests
// ===========================================================================

TEST(PathFunctions, PathLengthReturnsHopCount) {
    Path p;
    p.steps.push_back({1, 100});
    p.steps.push_back({2, 101});
    p.steps.push_back({3, -1});
    EXPECT_EQ(p.length(), 2);
}

TEST(PathFunctions, PathLengthEmptyPath) {
    Path p;
    EXPECT_EQ(p.length(), 0);
}

TEST(PathFunctions, PathLengthSingleNode) {
    Path p;
    p.steps.push_back({1, -1});
    EXPECT_EQ(p.length(), 0);
}

// Helper: build a FunctionCallExpr(name, col_ref("p")) for evaluating
// SQL builtin functions that take a PATH column argument.
namespace {
ExprPtr make_path_func(const std::string& func_name) {
    auto col = std::make_unique<ColumnRefExpr>();
    col->column = "p";
    auto call = std::make_unique<FunctionCallExpr>();
    call->name = func_name;
    call->args.push_back(std::move(col));
    return call;
}

// Build a one-column OutputSchema for a PATH column named "p".
OutputSchema make_path_schema() {
    OutputColumn col;
    col.name = "p";
    col.type_id = TypeId::PATH;
    return OutputSchema({col});
}

// Build a tuple carrying the given Path as a single PATH-typed value.
Tuple make_path_tuple(Path path) {
    return Tuple{{Value(std::move(path))}, std::nullopt};
}
} // namespace

// nodes(path) must call the production expr_evaluator handler and return a
// JSON array of every node PK in path order (including the terminal node).
TEST(PathFunctions, NodesReturnsCorrectNodeList) {
    // 3-node, 2-hop path: 1 --100--> 2 --101--> 3
    Path p;
    p.steps.push_back({1, 100});
    p.steps.push_back({2, 101});
    p.steps.push_back({3, -1});

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};

    auto expr = make_path_func("NODES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Production nodes() returns a JSON array of all node PKs (including terminal).
    EXPECT_EQ(result->as_json().data, "[1,2,3]");
}

// nodes() on a single-node path (no edges) must return a one-element array.
TEST(PathFunctions, NodesReturnsSingleNodePath) {
    Path p;
    p.steps.push_back({42, -1});

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};

    auto expr = make_path_func("NODES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_json().data, "[42]");
}

// edges(path) must call the production expr_evaluator handler and return a
// JSON array of only the edge IDs (steps where edge_id >= 0), in order.
TEST(PathFunctions, EdgesReturnsCorrectEdgeList) {
    // 3-node, 2-hop path: 1 --100--> 2 --101--> 3
    Path p;
    p.steps.push_back({1, 100});
    p.steps.push_back({2, 101});
    p.steps.push_back({3, -1});

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};

    auto expr = make_path_func("EDGES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Production edges() returns only steps with edge_id >= 0.
    EXPECT_EQ(result->as_json().data, "[100,101]");
}

// edges() on a single-node path must return an empty JSON array (no edges).
TEST(PathFunctions, EdgesEmptyForSingleNodePath) {
    Path p;
    p.steps.push_back({42, -1});

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};

    auto expr = make_path_func("EDGES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_json().data, "[]");
}

TEST(PathFunctions, PathValueInVariant) {
    Path p;
    p.steps.push_back({1, 100});
    p.steps.push_back({2, -1});

    Value v(std::move(p));
    EXPECT_EQ(v.type_id(), TypeId::PATH);
    EXPECT_EQ(v.as_path().length(), 1);
    EXPECT_EQ(v.as_path().steps.size(), 2u);
}

// ===========================================================================
// PathSelector enum and AST tests
// ===========================================================================

TEST(PathSelectorEnum, DefaultIsNone) {
    MatchStmt stmt;
    EXPECT_EQ(stmt.path_selector, PathSelector::NONE);
    EXPECT_TRUE(stmt.path_variable.empty());
    EXPECT_EQ(stmt.shortest_k, 0);
}

// ===========================================================================
// Backward compatibility tests — SHORTEST PATH ... VIA still works
// ===========================================================================

TEST(BackwardCompat, ShortestPathViaStillParses) {
    auto stmt = parse_one("SHORTEST PATH FROM persons(1) TO persons(99) VIA knows");
    auto* sp = dynamic_cast<ShortestPathStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->from_table, "persons");
    EXPECT_EQ(sp->to_table, "persons");
    EXPECT_EQ(sp->edge_type, "knows");
}

TEST(BackwardCompat, ShortestPathViaWithDirection) {
    auto stmt = parse_one("SHORTEST PATH FROM persons(1) TO persons(99) VIA knows DIRECTION IN");
    auto* sp = dynamic_cast<ShortestPathStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->direction, TraverseDirection::IN);
}

TEST(BackwardCompat, ShortestPathViaWithMaxDepth) {
    auto stmt = parse_one("SHORTEST PATH FROM persons(1) TO persons(99) VIA knows MAX_DEPTH 5");
    auto* sp = dynamic_cast<ShortestPathStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    ASSERT_TRUE(sp->max_depth.has_value());
    EXPECT_EQ(*sp->max_depth, 5);
}

/// Verify the old ShortestPathOperator still works with the existing test graph.
class ShortestPathBackwardCompat : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        TableSchema ts;
        ts.name = "nodes";
        CatalogColumnDef pk_col;
        pk_col.ordinal = 0;
        pk_col.name = "id";
        pk_col.type_id = TypeId::INT64;
        pk_col.nullable = false;
        ts.columns.push_back(pk_col);
        ts.pk_columns = "id";
        auto tid = catalog_->create_table(1, std::move(ts));
        ASSERT_TRUE(tid.has_value()) << tid.error().message;

        auto eid = graph_->create_edge_type(
            default_database_id, "knows", *tid, *tid, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        auto r1 = graph_->link(default_database_id, "knows", Value(int64_t{1}), Value(int64_t{2}));
        ASSERT_TRUE(r1.has_value());
        auto r2 = graph_->link(default_database_id, "knows", Value(int64_t{2}), Value(int64_t{3}));
        ASSERT_TRUE(r2.has_value());
    }

    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<GraphEngine> graph_;
};

TEST_F(ShortestPathBackwardCompat, OldOperatorStillWorks) {
    ShortestPathConfig config;
    config.edge_type = "knows";
    config.from_key = Value(int64_t{1});
    config.to_key = Value(int64_t{3});
    config.direction = TraverseDirection::OUT;

    std::vector<OutputColumn> cols;
    cols.push_back({"", "node", TypeId::INT64, false, 0});
    cols.push_back({"", "hop", TypeId::INT64, false, 0});
    OutputSchema schema(std::move(cols));

    ShortestPathOperator op(*graph_, std::move(config), std::move(schema));
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<int64_t> path;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        path.push_back(row->value().values[0].as_int64());
    }
    op.close();

    ASSERT_EQ(path.size(), 3u);
    EXPECT_EQ(path[0], 1);
    EXPECT_EQ(path[1], 2);
    EXPECT_EQ(path[2], 3);
}
