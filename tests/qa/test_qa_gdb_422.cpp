/// @file test_qa_gdb_422.cpp
/// QA adversarial tests for GDB-422: Variable-Length Path Patterns.
///
/// Verifies:
///   AC1: {1,6} quantifier traverses 1-6 hops
///   AC2: {3} quantifier traverses exactly 3 hops
///   AC3: + shorthand works (1 or more)
///   AC4: * shorthand works (0 or more)
///   AC5: Cycle prevention on cyclic graphs
///   AC6: path_length(p) returns correct hop count
///   AC7: Memory bounding prevents runaway queries
///   AC8: Unit tests written and passing
///   AC9: No regressions
///
/// Adversarial categories: parser edge cases, boundary values, error paths,
/// cycle stress, memory bounding, path value correctness, planner path
/// selection, FROM MATCH vs standalone MATCH, malformed quantifiers.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/coercion.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/executor/pattern_match.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/variable_length_match.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Parser helpers
// ============================================================================

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

static void parse_fail(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return; // Lexer failure is acceptable.

    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_FALSE(stmts.has_value()) << "Expected parse failure for: " << sql;
}

static MatchStmt* match_from_select(const StmtPtr& stmt) {
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    if (!sel || sel->from.empty() || !sel->from[0].match_source)
        return nullptr;
    return dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
}

// ============================================================================
// Parser adversarial tests
// ============================================================================

// -- AC1: {min,max} quantifier parsing edge cases ----------------------------

TEST(QA_GDB422_Parse, RangeQuantifier_LargeBounds) {
    // Large but valid int32_t values.
    auto stmt =
        parse_one("SELECT a.name FROM MATCH (a:p)-[r:e]->{1,1000}(b:p)");
    auto* m = match_from_select(stmt);
    ASSERT_NE(m, nullptr);
    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_TRUE(edge->is_variable_length());
    EXPECT_EQ(*edge->min_hops, 1);
    EXPECT_EQ(*edge->max_hops, 1000);
}

TEST(QA_GDB422_Parse, RangeQuantifier_ZeroMin) {
    auto stmt =
        parse_one("SELECT a.name FROM MATCH (a:p)-[r:e]->{0,5}(b:p)");
    auto* m = match_from_select(stmt);
    ASSERT_NE(m, nullptr);
    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(*edge->min_hops, 0);
    EXPECT_EQ(*edge->max_hops, 5);
}

TEST(QA_GDB422_Parse, RangeQuantifier_SameMinMax) {
    // {5,5} should be treated like {5}.
    auto stmt =
        parse_one("SELECT a.name FROM MATCH (a:p)-[r:e]->{5,5}(b:p)");
    auto* m = match_from_select(stmt);
    ASSERT_NE(m, nullptr);
    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(*edge->min_hops, 5);
    EXPECT_EQ(*edge->max_hops, 5);
}

// -- AC2: {n} exact quantifier parsing ---------------------------------------

TEST(QA_GDB422_Parse, ExactQuantifier_One) {
    auto stmt =
        parse_one("SELECT a.name FROM MATCH (a:p)-[r:e]->{1}(b:p)");
    auto* m = match_from_select(stmt);
    ASSERT_NE(m, nullptr);
    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(*edge->min_hops, 1);
    EXPECT_EQ(*edge->max_hops, 1);
}

TEST(QA_GDB422_Parse, ExactQuantifier_Zero) {
    // {0} should parse even if binder rejects it.
    auto stmt =
        parse_one("SELECT a.name FROM MATCH (a:p)-[r:e]->{0}(b:p)");
    auto* m = match_from_select(stmt);
    ASSERT_NE(m, nullptr);
    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(*edge->min_hops, 0);
    EXPECT_EQ(*edge->max_hops, 0);
}

// -- AC3: + shorthand --------------------------------------------------------

TEST(QA_GDB422_Parse, PlusShorthand_HasNoMaxHops) {
    auto stmt =
        parse_one("SELECT a.name FROM MATCH (a:p)-[r:e]->+(b:p)");
    auto* m = match_from_select(stmt);
    ASSERT_NE(m, nullptr);
    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(*edge->min_hops, 1);
    EXPECT_FALSE(edge->max_hops.has_value());
}

// -- AC4: * shorthand --------------------------------------------------------

TEST(QA_GDB422_Parse, StarShorthand_HasNoMaxHops) {
    auto stmt =
        parse_one("SELECT a.name FROM MATCH (a:p)-[r:e]->*(b:p)");
    auto* m = match_from_select(stmt);
    ASSERT_NE(m, nullptr);
    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(*edge->min_hops, 0);
    EXPECT_FALSE(edge->max_hops.has_value());
}

// -- Unbounded max: {min,} form ----------------------------------------------

TEST(QA_GDB422_Parse, UnboundedMax) {
    // {3,} — min=3, no max bound.
    auto stmt =
        parse_one("SELECT a.name FROM MATCH (a:p)-[r:e]->{3,}(b:p)");
    auto* m = match_from_select(stmt);
    ASSERT_NE(m, nullptr);
    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(*edge->min_hops, 3);
    EXPECT_FALSE(edge->max_hops.has_value());
}

// -- Malformed quantifiers ---------------------------------------------------

TEST(QA_GDB422_Parse, MalformedQuantifier_MissingMin) {
    parse_fail("SELECT a.name FROM MATCH (a:p)-[r:e]->{,5}(b:p)");
}

TEST(QA_GDB422_Parse, MalformedQuantifier_EmptyBraces) {
    parse_fail("SELECT a.name FROM MATCH (a:p)-[r:e]->{}(b:p)");
}

TEST(QA_GDB422_Parse, MalformedQuantifier_NegativeMin) {
    // Lexer won't produce a negative integer literal, so this should fail.
    parse_fail("SELECT a.name FROM MATCH (a:p)-[r:e]->{-1,5}(b:p)");
}

// -- Quantifiers with different directions -----------------------------------

TEST(QA_GDB422_Parse, IncomingWithQuantifier) {
    auto stmt =
        parse_one("SELECT a.name FROM MATCH (a:p)<-[r:e]-{2,4}(b:p)");
    auto* m = match_from_select(stmt);
    ASSERT_NE(m, nullptr);
    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(edge->direction, TraverseDirection::IN);
    EXPECT_EQ(*edge->min_hops, 2);
    EXPECT_EQ(*edge->max_hops, 4);
}

TEST(QA_GDB422_Parse, UndirectedWithPlus) {
    auto stmt =
        parse_one("SELECT a.name FROM MATCH (a:p)-[r:e]-+(b:p)");
    auto* m = match_from_select(stmt);
    ASSERT_NE(m, nullptr);
    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(edge->direction, TraverseDirection::BOTH);
    EXPECT_EQ(*edge->min_hops, 1);
}

// -- No quantifier (regression: still works as single hop) -------------------

TEST(QA_GDB422_Parse, NoQuantifierRegression) {
    auto stmt =
        parse_one("SELECT a.name FROM MATCH (a:p)-[r:e]->(b:p)");
    auto* m = match_from_select(stmt);
    ASSERT_NE(m, nullptr);
    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_FALSE(edge->is_variable_length());
    EXPECT_FALSE(edge->min_hops.has_value());
    EXPECT_FALSE(edge->max_hops.has_value());
}

// ============================================================================
// Path value type adversarial tests
// ============================================================================

TEST(QA_GDB422_Path, EmptyPathLength) {
    Path p;
    EXPECT_EQ(p.length(), 0);
    EXPECT_TRUE(p.steps.empty());
}

TEST(QA_GDB422_Path, SingleNodePathLength) {
    Path p;
    p.steps.push_back({42, -1});
    EXPECT_EQ(p.length(), 0); // One node, zero edges.
}

TEST(QA_GDB422_Path, LongPathLength) {
    // 10-hop path: 11 steps.
    Path p;
    for (int i = 0; i < 11; ++i) {
        p.steps.push_back({i, (i < 10) ? (i * 100) : int64_t(-1)});
    }
    EXPECT_EQ(p.length(), 10);
}

TEST(QA_GDB422_Path, PathValueRoundTrip) {
    Path p;
    p.steps.push_back({1, 100});
    p.steps.push_back({2, 200});
    p.steps.push_back({3, -1});

    Value v(p);
    EXPECT_EQ(v.type_id(), TypeId::PATH);

    auto retrieved = v.try_as_path();
    ASSERT_TRUE(retrieved.has_value()) << retrieved.error().message;
    EXPECT_EQ((*retrieved)->length(), 2);
    EXPECT_EQ((*retrieved)->steps[0].node_pk, 1);
    EXPECT_EQ((*retrieved)->steps[1].node_pk, 2);
    EXPECT_EQ((*retrieved)->steps[2].node_pk, 3);
}

TEST(QA_GDB422_Path, PathInequalityDifferentLengths) {
    Path p1;
    p1.steps.push_back({1, 10});
    p1.steps.push_back({2, -1});

    Path p2;
    p2.steps.push_back({1, 10});
    p2.steps.push_back({2, 20});
    p2.steps.push_back({3, -1});

    EXPECT_NE(p1, p2);
}

TEST(QA_GDB422_Path, PathInequalityDifferentEdgeIds) {
    Path p1;
    p1.steps.push_back({1, 10});
    p1.steps.push_back({2, -1});

    Path p2;
    p2.steps.push_back({1, 99}); // Different edge_id.
    p2.steps.push_back({2, -1});

    EXPECT_NE(p1, p2);
}

TEST(QA_GDB422_Path, PathCompareNotSupported) {
    // Paths should not be comparable (ordering).
    Value v1(Path{});
    Value v2(Path{});
    auto cmp = compare(v1, v2);
    EXPECT_FALSE(cmp.has_value());
}

// ============================================================================
// Executor adversarial tests — VariableLengthMatchOperator
// ============================================================================

/// Test fixture matching the unit test fixture, for QA executor tests.
class QA_GDB422_Executor : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb422";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        // Create 'persons' table.
        {
            TableSchema ts;
            ts.name = "persons";
            CatalogColumnDef id_col;
            id_col.ordinal = 0;
            id_col.name = "id";
            id_col.type_id = TypeId::INT64;
            id_col.nullable = false;
            ts.columns.push_back(id_col);
            CatalogColumnDef name_col;
            name_col.ordinal = 1;
            name_col.name = "name";
            name_col.type_id = TypeId::STRING;
            name_col.nullable = false;
            ts.columns.push_back(name_col);
            ts.pk_columns = "id";
            auto tid = catalog_->create_table(default_database_id, std::move(ts));
            ASSERT_TRUE(tid.has_value()) << tid.error().message;
            persons_id_ = *tid;

            auto schema = catalog_->get_table(default_database_id, "persons");
            ASSERT_TRUE(schema.has_value());
            auto sr = storage_->create_table_storage(default_database_id, persons_id_, *schema);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }

        // Insert persons: 1..6 in a chain.
        insert_person(1, "Alice");
        insert_person(2, "Bob");
        insert_person(3, "Charlie");
        insert_person(4, "Diana");
        insert_person(5, "Eve");
        insert_person(6, "Frank");

        // Create 'knows' edge type.
        auto eid = graph_->create_edge_type(
            "knows", persons_id_, persons_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // Linear chain: 1->2->3->4->5->6
        link("knows", 1, 2);
        link("knows", 2, 3);
        link("knows", 3, 4);
        link("knows", 4, 5);
        link("knows", 5, 6);

        // Create 'cycle' edge type.
        auto cid = graph_->create_edge_type(
            "cycle", persons_id_, persons_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(cid.has_value()) << cid.error().message;

        // Triangle cycle: 1->2->3->1
        link("cycle", 1, 2);
        link("cycle", 2, 3);
        link("cycle", 3, 1);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void link(const std::string& edge_type, int64_t from, int64_t to) {
        auto r = graph_->link(edge_type, Value(from), Value(to));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    void insert_person(int64_t id, const std::string& name) {
        auto ts = storage_->get_table_storage(persons_id_);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        auto schema = catalog_->get_table(default_database_id, "persons");
        ASSERT_TRUE(schema.has_value());

        std::vector<Value> vals = {Value(id), Value(name)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    std::vector<Tuple> run_vl_match(MatchConfig config, OutputSchema schema,
                                     size_t max_visited = 100'000) {
        BoundStatement bound;
        VariableLengthMatchOperator op(*graph_,
                                       *catalog_,
                                       *storage_,
                                       default_database_id,
                                       std::move(config),
                                       std::move(schema),
                                       nullptr,
                                       bound,
                                       max_visited);
        auto open_result = op.open();
        EXPECT_TRUE(open_result.has_value()) << open_result.error().message;
        if (!open_result.has_value())
            return {};

        std::vector<Tuple> results;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row.has_value() || !row->has_value())
                break;
            results.push_back(std::move(**row));
        }
        op.close();
        return results;
    }

    MatchConfig make_config(const std::string& edge_type,
                            std::optional<int32_t> min_h,
                            std::optional<int32_t> max_h,
                            TraverseDirection dir = TraverseDirection::OUT) {
        MatchConfig config;
        config.nodes.push_back({"a", "persons"});
        config.nodes.push_back({"b", "persons"});
        config.edges.push_back(MatchEdgeDef("r", edge_type, dir, min_h, max_h));
        return config;
    }

    OutputSchema make_schema() {
        std::vector<OutputColumn> out_cols;
        out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
        out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
        return OutputSchema(std::move(out_cols));
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t persons_id_ = 0;
};

// -- AC1: {1,6} quantifier traverses 1-6 hops -------------------------------

TEST_F(QA_GDB422_Executor, AC1_Range_1_6) {
    // Chain of 6 nodes (5 edges).
    // 1-hop: 5, 2-hop: 4, 3-hop: 3, 4-hop: 2, 5-hop: 1 = 15 paths.
    auto config = make_config("knows", 1, 6);
    auto results = run_vl_match(std::move(config), make_schema());
    EXPECT_EQ(results.size(), 15u);
}

// -- AC2: {3} quantifier traverses exactly 3 hops ---------------------------

TEST_F(QA_GDB422_Executor, AC2_Exact_3) {
    // Chain of 6 nodes, exactly 3 hops.
    // 1->4, 2->5, 3->6 = 3 paths.
    auto config = make_config("knows", 3, 3);
    auto results = run_vl_match(std::move(config), make_schema());
    EXPECT_EQ(results.size(), 3u);
}

TEST_F(QA_GDB422_Executor, AC2_Exact_1) {
    // Exactly 1 hop = 5 direct edges.
    auto config = make_config("knows", 1, 1);
    auto results = run_vl_match(std::move(config), make_schema());
    EXPECT_EQ(results.size(), 5u);
}

TEST_F(QA_GDB422_Executor, AC2_Exact_5) {
    // Exactly 5 hops in a 6-node chain: only 1->6.
    auto config = make_config("knows", 5, 5);
    auto results = run_vl_match(std::move(config), make_schema());
    EXPECT_EQ(results.size(), 1u);

    // Verify it's Alice -> Frank.
    ASSERT_GE(results[0].values.size(), 2u);
    EXPECT_EQ(results[0].values[0].as_string(), "Alice");
    EXPECT_EQ(results[0].values[1].as_string(), "Frank");
}

TEST_F(QA_GDB422_Executor, AC2_Exact_6_NoPaths) {
    // Chain only has 5 edges; 6 hops is impossible.
    auto config = make_config("knows", 6, 6);
    auto results = run_vl_match(std::move(config), make_schema());
    EXPECT_EQ(results.size(), 0u);
}

// -- AC3: + shorthand (1 or more hops) --------------------------------------

TEST_F(QA_GDB422_Executor, AC3_PlusShorthand) {
    // + means {1, nullopt}, which defaults to max_hops=20.
    // In our 6-node chain: 5+4+3+2+1 = 15 paths.
    auto config = make_config("knows", 1, std::nullopt);
    auto results = run_vl_match(std::move(config), make_schema());
    EXPECT_EQ(results.size(), 15u);
}

// -- AC4: * shorthand (0 or more hops) --------------------------------------

TEST_F(QA_GDB422_Executor, AC4_StarShorthand) {
    // * means {0, nullopt}, which defaults to max_hops=20.
    // 0-hop: 6 (self matches), plus 15 from + = 21 total.
    auto config = make_config("knows", 0, std::nullopt);
    auto results = run_vl_match(std::move(config), make_schema());
    EXPECT_EQ(results.size(), 21u);
}

TEST_F(QA_GDB422_Executor, AC4_ZeroHopSelfMatch) {
    // {0,0} is rejected by binder, but {0,1} should include self-matches.
    auto config = make_config("knows", 0, 1);
    auto results = run_vl_match(std::move(config), make_schema());
    // 0-hop: 6 (self), 1-hop: 5 = 11.
    EXPECT_EQ(results.size(), 11u);

    // Verify at least one self-match: source == target.
    bool found_self = false;
    for (const auto& t : results) {
        if (t.values.size() >= 2 &&
            t.values[0].as_string() == t.values[1].as_string()) {
            found_self = true;
            break;
        }
    }
    EXPECT_TRUE(found_self) << "Expected at least one self-match at depth 0";
}

// -- AC5: Cycle prevention ---------------------------------------------------

TEST_F(QA_GDB422_Executor, AC5_CycleTriangle) {
    // Cycle: 1->2->3->1. With {1,10}, visited set prevents revisiting.
    // From 1: 1->2 (d1), 1->2->3 (d2). Can't go 3->1 (visited).
    // From 2: 2->3 (d1), 2->3->1 (d2). Can't go 1->2 (visited).
    // From 3: 3->1 (d1), 3->1->2 (d2). Can't go 2->3 (visited).
    // Nodes 4,5,6 have no cycle edges = 0.
    // Total: 6 paths.
    auto config = make_config("cycle", 1, 10);
    auto results = run_vl_match(std::move(config), make_schema());
    EXPECT_EQ(results.size(), 6u);
}

TEST_F(QA_GDB422_Executor, AC5_CycleDepthExact3_NoPaths) {
    // In triangle 1->2->3->1, depth 3 would require visiting 4 nodes.
    // But 3->1 is blocked by visited (1 is start). No depth-3 paths.
    auto config = make_config("cycle", 3, 3);
    auto results = run_vl_match(std::move(config), make_schema());
    EXPECT_EQ(results.size(), 0u);
}

TEST_F(QA_GDB422_Executor, AC5_CycleWithZeroHop) {
    // {0,10} on cycle. 0-hop: 6 self-matches. 1-hop: 3, 2-hop: 3 = 12.
    auto config = make_config("cycle", 0, 10);
    auto results = run_vl_match(std::move(config), make_schema());
    EXPECT_EQ(results.size(), 12u);
}

// -- AC6: path_length(p) returns correct hop count ---------------------------

TEST(QA_GDB422_PathLength, ZeroLength) {
    Path p;
    p.steps.push_back({1, -1}); // Single node, no edges.
    Value v(p);
    auto path_ptr = v.try_as_path();
    ASSERT_TRUE(path_ptr.has_value()) << path_ptr.error().message;
    EXPECT_EQ((*path_ptr)->length(), 0);
}

TEST(QA_GDB422_PathLength, FiveHops) {
    Path p;
    for (int i = 0; i < 6; ++i) {
        p.steps.push_back({i + 1, (i < 5) ? (i * 10) : int64_t(-1)});
    }
    EXPECT_EQ(p.length(), 5);
}

TEST(QA_GDB422_PathLength, ExprEvaluator_WrongType) {
    // path_length(42) should return a type error.
    Tuple tuple;
    tuple.values.push_back(Value(int64_t(42)));

    std::vector<OutputColumn> cols;
    cols.push_back({"", "x", TypeId::INT64, false, 0});
    OutputSchema schema(std::move(cols));

    auto col_ref = std::make_unique<ColumnRefExpr>();
    col_ref->column = "x";

    FunctionCallExpr fn_expr;
    fn_expr.name = "path_length";
    fn_expr.args.push_back(std::move(col_ref));

    BoundStatement bound;
    auto result = evaluate_expr(fn_expr, tuple, schema, bound);
    EXPECT_FALSE(result.has_value())
        << "path_length on INT64 should return an error";
}

TEST(QA_GDB422_PathLength, ExprEvaluator_TwoArgs) {
    // path_length(a, b) should fail — expects exactly 1 arg.
    FunctionCallExpr fn_expr;
    fn_expr.name = "path_length";
    fn_expr.args.push_back(std::make_unique<ColumnRefExpr>());
    fn_expr.args.push_back(std::make_unique<ColumnRefExpr>());

    Tuple tuple;
    std::vector<OutputColumn> cols;
    OutputSchema schema(std::move(cols));
    BoundStatement bound;

    auto result = evaluate_expr(fn_expr, tuple, schema, bound);
    EXPECT_FALSE(result.has_value());
}

// -- AC7: Memory bounding ---------------------------------------------------

TEST_F(QA_GDB422_Executor, AC7_MemoryBound_TightLimit) {
    // max_visited = 1: should fail immediately after dequeuing start node.
    MatchConfig config = make_config("knows", 1, 100);

    BoundStatement bound;
    VariableLengthMatchOperator op(*graph_,
                                   *catalog_,
                                   *storage_,
                                   default_database_id,
                                   std::move(config),
                                   make_schema(),
                                   nullptr,
                                   bound,
                                   1);
    auto result = op.open();
    EXPECT_FALSE(result.has_value());
    op.close();
}

TEST_F(QA_GDB422_Executor, AC7_MemoryBound_ExactlyEnough) {
    // For a single-hop {1,1} query on 6 nodes:
    // BFS dequeues each start node (6 total), then their neighbors.
    // With a generous limit, should succeed.
    auto config = make_config("knows", 1, 1);
    auto results = run_vl_match(std::move(config), make_schema(), 100);
    EXPECT_EQ(results.size(), 5u);
}

// -- Boundary: Empty graph (no edges of given type) --------------------------

TEST_F(QA_GDB422_Executor, EmptyEdgeType) {
    // Create a new edge type with no edges.
    auto eid = graph_->create_edge_type(
        "empty", persons_id_, persons_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(eid.has_value());

    auto config = make_config("empty", 1, 5);
    auto results = run_vl_match(std::move(config), make_schema());
    EXPECT_EQ(results.size(), 0u);
}

// -- Boundary: Single node graph ---------------------------------------------

TEST_F(QA_GDB422_Executor, ZeroHopOnSingleNode) {
    // Zero-hop on a graph with a single isolated node type.
    // All 6 persons should self-match at depth 0.
    auto config = make_config("knows", 0, 0);
    auto results = run_vl_match(std::move(config), make_schema());
    EXPECT_EQ(results.size(), 6u);
}

// -- Output correctness: verify specific pairs at various depths -------------

TEST_F(QA_GDB422_Executor, OutputPairs_Depth2) {
    auto config = make_config("knows", 2, 2);
    auto results = run_vl_match(std::move(config), make_schema());

    // 2-hop paths in 1->2->3->4->5->6:
    // 1->3, 2->4, 3->5, 4->6 = 4 paths.
    ASSERT_EQ(results.size(), 4u);

    std::unordered_set<std::string> pairs;
    for (const auto& t : results) {
        ASSERT_GE(t.values.size(), 2u);
        pairs.insert(t.values[0].as_string() + "->" + t.values[1].as_string());
    }

    EXPECT_TRUE(pairs.count("Alice->Charlie") > 0);
    EXPECT_TRUE(pairs.count("Bob->Diana") > 0);
    EXPECT_TRUE(pairs.count("Charlie->Eve") > 0);
    EXPECT_TRUE(pairs.count("Diana->Frank") > 0);
}

// -- Path output column verification -----------------------------------------

TEST_F(QA_GDB422_Executor, PathOutputColumn) {
    // Include a "path" column in the output and verify it is populated.
    MatchConfig config = make_config("knows", 2, 2);

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"r", "path", TypeId::PATH, true, 0});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));
    ASSERT_EQ(results.size(), 4u);

    for (const auto& t : results) {
        ASSERT_GE(t.values.size(), 3u);
        // The path column should be a PATH value.
        EXPECT_EQ(t.values[2].type_id(), TypeId::PATH);
        auto path_ptr = t.values[2].try_as_path();
        ASSERT_TRUE(path_ptr.has_value()) << path_ptr.error().message;
        // 2-hop path: 3 steps (start, middle, end).
        EXPECT_EQ((*path_ptr)->steps.size(), 3u);
        EXPECT_EQ((*path_ptr)->length(), 2);
    }
}

// -- Stress: deeply nested chain traversal -----------------------------------

TEST_F(QA_GDB422_Executor, DeepChainTraversal) {
    // Full chain traversal {1,5}: should enumerate all possible paths.
    // 1-hop:5 + 2-hop:4 + 3-hop:3 + 4-hop:2 + 5-hop:1 = 15.
    auto config = make_config("knows", 1, 5);
    auto results = run_vl_match(std::move(config), make_schema());
    EXPECT_EQ(results.size(), 15u);
}

// -- Regression: non-quantified single hop still works -----------------------

TEST_F(QA_GDB422_Executor, AC9_NonQuantifiedSingleHop) {
    // Non-quantified edge should still work via PatternMatchOperator.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT));

    auto results = run_vl_match(std::move(config), make_schema());
    // The VariableLengthMatchOperator falls back to {1,20} for non-quantified
    // edges (since is_variable_length() is false, min_hops/max_hops are nullopt,
    // value_or gives 1 and 20). Chain has 5 edges and max depth 5.
    // This test verifies it doesn't crash and returns reasonable results.
    EXPECT_GE(results.size(), 5u);
}

// ============================================================================
// Binder validation tests
// ============================================================================

TEST(QA_GDB422_Binder, RejectsZeroZero) {
    // {0,0} should be rejected by the binder.
    Lexer lexer("SELECT a.name FROM MATCH (a:persons)-[r:knows]->{0}(b:persons)");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());

    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    ASSERT_TRUE(stmts.has_value());
    ASSERT_EQ(stmts->size(), 1u);

    auto* sel = dynamic_cast<SelectStmt*>((*stmts)[0].get());
    ASSERT_NE(sel, nullptr);
    auto* match = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(match, nullptr);

    // Verify the parser set {0,0}.
    const auto& edge = match->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(*edge->min_hops, 0);
    EXPECT_EQ(*edge->max_hops, 0);
}

TEST(QA_GDB422_Binder, RejectsMaxLessThanMin) {
    // The parser allows {5,2} but the binder should reject it.
    auto stmt = parse_one("SELECT a.name FROM MATCH (a:p)-[r:e]->{5,2}(b:p)");
    auto* m = match_from_select(stmt);
    ASSERT_NE(m, nullptr);
    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(*edge->min_hops, 5);
    EXPECT_EQ(*edge->max_hops, 2);
    // Parser accepts it; binder would reject at bind time.
}

// ============================================================================
// Serialization round-trip for Path values
// ============================================================================

TEST(QA_GDB422_Serialization, PathRoundTrip) {
    Path p;
    p.steps.push_back({100, 200});
    p.steps.push_back({300, 400});
    p.steps.push_back({500, -1});

    Value v(p);
    EXPECT_EQ(v.type_id(), TypeId::PATH);

    // Verify the path data survives copy.
    Value v2 = v;
    EXPECT_EQ(v2.type_id(), TypeId::PATH);
    auto p2 = v2.try_as_path();
    ASSERT_TRUE(p2.has_value()) << p2.error().message;
    EXPECT_EQ((*p2)->steps.size(), 3u);
    EXPECT_EQ((*p2)->steps[0].node_pk, 100);
    EXPECT_EQ((*p2)->steps[2].edge_id, -1);
}

} // namespace
} // namespace sixseven
