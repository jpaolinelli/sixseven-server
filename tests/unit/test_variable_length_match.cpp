#include "sixseven/catalog/catalog.h"
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
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Parser tests for quantifier syntax
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

// -- Quantifier parsing: {min,max} -------------------------------------------

TEST(VarLenParse, RangeQuantifier) {
    auto stmt =
        parse_one("SELECT a.name, b.name FROM MATCH (a:persons)-[r:knows]->{1,6}(b:persons)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 2u);

    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(edge->edge_type, "knows");
    EXPECT_EQ(edge->direction, TraverseDirection::OUT);
    EXPECT_TRUE(edge->is_variable_length());
    ASSERT_TRUE(edge->min_hops.has_value());
    EXPECT_EQ(*edge->min_hops, 1);
    ASSERT_TRUE(edge->max_hops.has_value());
    EXPECT_EQ(*edge->max_hops, 6);
}

// -- Quantifier parsing: {n} exact hops --------------------------------------

TEST(VarLenParse, ExactQuantifier) {
    auto stmt = parse_one("SELECT a.name FROM MATCH (a:persons)-[r:knows]->{3}(b:persons)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);

    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_TRUE(edge->is_variable_length());
    ASSERT_TRUE(edge->min_hops.has_value());
    EXPECT_EQ(*edge->min_hops, 3);
    ASSERT_TRUE(edge->max_hops.has_value());
    EXPECT_EQ(*edge->max_hops, 3);
}

// -- Quantifier parsing: + shorthand -----------------------------------------

TEST(VarLenParse, PlusShorthand) {
    auto stmt = parse_one("SELECT a.name FROM MATCH (a:persons)-[r:knows]->+(b:persons)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);

    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_TRUE(edge->is_variable_length());
    ASSERT_TRUE(edge->min_hops.has_value());
    EXPECT_EQ(*edge->min_hops, 1);
    EXPECT_FALSE(edge->max_hops.has_value());
}

// -- Quantifier parsing: * shorthand -----------------------------------------

TEST(VarLenParse, StarShorthand) {
    auto stmt = parse_one("SELECT a.name FROM MATCH (a:persons)-[r:knows]->*(b:persons)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);

    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_TRUE(edge->is_variable_length());
    ASSERT_TRUE(edge->min_hops.has_value());
    EXPECT_EQ(*edge->min_hops, 0);
    EXPECT_FALSE(edge->max_hops.has_value());
}

// -- Quantifier parsing: {0,10} zero-start range -----------------------------

TEST(VarLenParse, ZeroStartRange) {
    auto stmt = parse_one("SELECT a.name FROM MATCH (a:persons)-[r:knows]->{0,10}(b:persons)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);

    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_TRUE(edge->is_variable_length());
    EXPECT_EQ(*edge->min_hops, 0);
    EXPECT_EQ(*edge->max_hops, 10);
}

// -- Quantifier parsing: incoming direction ----------------------------------

TEST(VarLenParse, IncomingDirection) {
    auto stmt = parse_one("SELECT a.name FROM MATCH (a:persons)<-[r:knows]-{1,3}(b:persons)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);

    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(edge->direction, TraverseDirection::IN);
    EXPECT_TRUE(edge->is_variable_length());
    EXPECT_EQ(*edge->min_hops, 1);
    EXPECT_EQ(*edge->max_hops, 3);
}

// -- Quantifier parsing: undirected -----------------------------------------

TEST(VarLenParse, UndirectedQuantifier) {
    auto stmt = parse_one("SELECT a.name FROM MATCH (a:persons)-[r:knows]-{1,4}(b:persons)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);

    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(edge->direction, TraverseDirection::BOTH);
    EXPECT_TRUE(edge->is_variable_length());
    EXPECT_EQ(*edge->min_hops, 1);
    EXPECT_EQ(*edge->max_hops, 4);
}

// -- Non-quantified edge still parses as fixed single hop --------------------

TEST(VarLenParse, NoQuantifierStillWorks) {
    auto stmt = parse_one("SELECT a.name FROM MATCH (a:persons)-[r:knows]->(b:persons)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);

    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_FALSE(edge->is_variable_length());
    EXPECT_FALSE(edge->min_hops.has_value());
    EXPECT_FALSE(edge->max_hops.has_value());
}

// ============================================================================
// Path value type tests
// ============================================================================

TEST(PathValueTest, PathLength) {
    Path p;
    EXPECT_EQ(p.length(), 0);

    p.steps.push_back({1, 100});
    p.steps.push_back({2, 101});
    p.steps.push_back({3, -1});
    EXPECT_EQ(p.length(), 2);
}

TEST(PathValueTest, PathInValue) {
    Path p;
    p.steps.push_back({1, 100});
    p.steps.push_back({2, -1});

    Value v(p);
    EXPECT_FALSE(v.is_null());
    EXPECT_EQ(v.type_id(), TypeId::PATH);

    const auto& retrieved = v.as_path();
    EXPECT_EQ(retrieved.length(), 1);
    EXPECT_EQ(retrieved.steps.size(), 2u);
    EXPECT_EQ(retrieved.steps[0].node_pk, 1);
    EXPECT_EQ(retrieved.steps[1].node_pk, 2);
}

TEST(PathValueTest, EmptyPath) {
    Path p;
    Value v(p);
    EXPECT_EQ(v.type_id(), TypeId::PATH);
    EXPECT_EQ(v.as_path().length(), 0);
}

TEST(PathValueTest, PathEquality) {
    Path p1;
    p1.steps.push_back({1, 10});
    p1.steps.push_back({2, -1});

    Path p2;
    p2.steps.push_back({1, 10});
    p2.steps.push_back({2, -1});

    EXPECT_EQ(p1, p2);

    Path p3;
    p3.steps.push_back({1, 10});
    p3.steps.push_back({3, -1});

    EXPECT_NE(p1, p3);
}

// ============================================================================
// Executor tests — VariableLengthMatchOperator
// ============================================================================

/// Test fixture for variable-length MATCH executor tests.
///
/// Creates a linear graph: A -> B -> C -> D -> E
/// and a cyclic graph: A -> B -> C -> A (via 'cycle' edge type).
class VarLenMatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_var_len_match";
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
            CatalogColumnDef company_col;
            company_col.ordinal = 2;
            company_col.name = "company";
            company_col.type_id = TypeId::STRING;
            company_col.nullable = true;
            ts.columns.push_back(company_col);
            ts.pk_columns = "id";
            auto tid = catalog_->create_table(default_database_id, std::move(ts));
            ASSERT_TRUE(tid.has_value()) << tid.error().message;
            persons_id_ = *tid;

            auto schema = catalog_->get_table(default_database_id, "persons");
            ASSERT_TRUE(schema.has_value());
            auto sr = storage_->create_table_storage(default_database_id, persons_id_, *schema);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }

        // Insert persons: A(1), B(2), C(3), D(4), E(5).
        insert_person(1, "Alice", "Acme");
        insert_person(2, "Bob", "Globex");
        insert_person(3, "Charlie", "Acme");
        insert_person(4, "Diana", "Initech");
        insert_person(5, "Eve", "Acme");

        // Create 'knows' edge type: persons -> persons.
        auto eid = graph_->create_edge_type(default_database_id, 
            "knows", persons_id_, persons_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // Create linear chain: 1->2->3->4->5
        link("knows", 1, 2);
        link("knows", 2, 3);
        link("knows", 3, 4);
        link("knows", 4, 5);

        // Create 'cycle' edge type.
        auto cid = graph_->create_edge_type(default_database_id, 
            "cycle", persons_id_, persons_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(cid.has_value()) << cid.error().message;

        // Create a cycle: 1->2->3->1
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

    void insert_person(int64_t id, const std::string& name, const std::string& company) {
        auto ts = storage_->get_table_storage(persons_id_);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        auto schema = catalog_->get_table(default_database_id, "persons");
        ASSERT_TRUE(schema.has_value());

        std::vector<Value> vals = {Value(id), Value(name), Value(company)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    /// Run a variable-length match and collect output tuples.
    std::vector<Tuple> run_vl_match(MatchConfig config, OutputSchema schema) {
        BoundStatement bound;
        VariableLengthMatchOperator op(*graph_,
                                       *catalog_,
                                       *storage_,
                                       default_database_id,
                                       std::move(config),
                                       std::move(schema),
                                       nullptr,
                                       bound);
        auto open_result = op.open();
        EXPECT_TRUE(open_result.has_value()) << open_result.error().message;

        std::vector<Tuple> results;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row->has_value())
                break;
            results.push_back(std::move(**row));
        }
        op.close();
        return results;
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t persons_id_ = 0;
};

// -- BFS at various depths ---------------------------------------------------

TEST_F(VarLenMatchTest, ExactOneHop) {
    // (a:persons)-[r:knows]->{1}(b:persons) — exactly 1 hop.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 1, 1));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));

    // Linear chain 1->2->3->4->5 has 4 edges = 4 single-hop paths.
    EXPECT_EQ(results.size(), 4u);
}

TEST_F(VarLenMatchTest, ExactTwoHops) {
    // (a:persons)-[r:knows]->{2}(b:persons) — exactly 2 hops.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 2, 2));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));

    // 2-hop paths in 1->2->3->4->5: 1->3, 2->4, 3->5 = 3 paths.
    EXPECT_EQ(results.size(), 3u);
}

TEST_F(VarLenMatchTest, RangeOneToThree) {
    // (a:persons)-[r:knows]->{1,3}(b:persons)
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 1, 3));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));

    // 1-hop: 4 paths, 2-hop: 3 paths, 3-hop: 2 paths = 9 total.
    EXPECT_EQ(results.size(), 9u);
}

TEST_F(VarLenMatchTest, RangeOneToSix) {
    // (a:persons)-[r:knows]->{1,6}(b:persons) — the LinkedIn query pattern.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 1, 6));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));

    // Chain of 5 nodes: 1-hop(4) + 2-hop(3) + 3-hop(2) + 4-hop(1) = 10.
    EXPECT_EQ(results.size(), 10u);
}

// -- Cycle handling on cyclic graphs -----------------------------------------

TEST_F(VarLenMatchTest, CycleHandling) {
    // On cycle edge: 1->2->3->1 (cycle).
    // (a:persons)-[r:cycle]->{1,10}(b:persons) — should NOT infinite loop.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "cycle", TraverseDirection::OUT, 1, 10));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));

    // From node 1: 1->2 (depth 1), 1->2->3 (depth 2) = 2 paths
    // From node 2: 2->3 (depth 1), 2->3->1 (depth 2) = 2 paths
    // From node 3: 3->1 (depth 1), 3->1->2 (depth 2) = 2 paths
    // Nodes 4 and 5 have no cycle edges, so 0 paths.
    // Total: 6 paths.
    EXPECT_EQ(results.size(), 6u);
}

// -- Memory bounding prevents runaway queries --------------------------------

TEST_F(VarLenMatchTest, MemoryBounding) {
    // Use a very small max_visited to trigger the limit.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 1, 100));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
    VariableLengthMatchOperator op(*graph_,
                                   *catalog_,
                                   *storage_,
                                   default_database_id,
                                   std::move(config),
                                   std::move(schema),
                                   nullptr,
                                   bound,
                                   3); // max_visited = 3, very tight

    auto open_result = op.open();
    // Should fail because we'll hit the limit before exploring all paths.
    EXPECT_FALSE(open_result.has_value());
    op.close();
}

// -- path_length() function --------------------------------------------------

TEST_F(VarLenMatchTest, PathLengthFunction) {
    // Verify path_length returns correct hop count for various paths.
    Path p0;
    EXPECT_EQ(p0.length(), 0);

    Path p1;
    p1.steps.push_back({1, 100});
    p1.steps.push_back({2, -1});
    EXPECT_EQ(p1.length(), 1);

    Path p3;
    p3.steps.push_back({1, 100});
    p3.steps.push_back({2, 101});
    p3.steps.push_back({3, 102});
    p3.steps.push_back({4, -1});
    EXPECT_EQ(p3.length(), 3);
}

// -- Zero-hop start (star quantifier) ----------------------------------------

TEST_F(VarLenMatchTest, ZeroHopStar) {
    // (a:persons)-[r:knows]->{0,2}(b:persons)
    // Zero-hop means a == b.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 0, 2));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));

    // 0-hop: 5 (each node matches itself)
    // 1-hop: 4 (chain edges)
    // 2-hop: 3 (chain 2-hop)
    // Total: 12
    EXPECT_EQ(results.size(), 12u);
}

// -- Output correctness: verify source/target names --------------------------

TEST_F(VarLenMatchTest, OutputCorrectness) {
    // Verify that the source and target names are correct for 2-hop.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 2, 2));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));

    // 2-hop paths: Alice->Charlie, Bob->Diana, Charlie->Eve
    ASSERT_EQ(results.size(), 3u);

    std::unordered_set<std::string> pairs;
    for (const auto& t : results) {
        ASSERT_GE(t.values.size(), 2u);
        auto src = t.values[0].as_string();
        auto tgt = t.values[1].as_string();
        pairs.insert(src + "->" + tgt);
    }

    EXPECT_TRUE(pairs.count("Alice->Charlie") > 0);
    EXPECT_TRUE(pairs.count("Bob->Diana") > 0);
    EXPECT_TRUE(pairs.count("Charlie->Eve") > 0);
}

// -- Empty results when no paths exist at depth ------------------------------

TEST_F(VarLenMatchTest, NoPathsAtDepth) {
    // (a:persons)-[r:knows]->{5,10}(b:persons)
    // Chain has only 4 edges, so no 5+ hop paths exist.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 5, 10));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));

    EXPECT_EQ(results.size(), 0u);
}

// -- path_length() SQL function evaluation ------------------------------------

TEST(PathLengthSqlFunction, ReturnsCorrectLength) {
    // Build a tuple with a Path value and a schema to reference it.
    Path path;
    path.steps.push_back({1, 10});
    path.steps.push_back({2, 20});
    path.steps.push_back({3, -1});

    Tuple tuple;
    tuple.values.push_back(Value(path));

    std::vector<OutputColumn> cols;
    cols.push_back({"", "p", TypeId::PATH, true, 0});
    OutputSchema schema(std::move(cols));

    // Build: path_length(p)
    auto col_ref = std::make_unique<ColumnRefExpr>();
    col_ref->column = "p";

    FunctionCallExpr fn_expr;
    fn_expr.name = "path_length";
    fn_expr.args.push_back(std::move(col_ref));

    BoundStatement bound;
    auto result = evaluate_expr(fn_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->is_null());
    EXPECT_EQ(result->as_int64(), 2);
}

TEST(PathLengthSqlFunction, NullArgReturnsNull) {
    Tuple tuple;
    tuple.values.push_back(Value::make_null());

    std::vector<OutputColumn> cols;
    cols.push_back({"", "p", TypeId::PATH, true, 0});
    OutputSchema schema(std::move(cols));

    auto col_ref = std::make_unique<ColumnRefExpr>();
    col_ref->column = "p";

    FunctionCallExpr fn_expr;
    fn_expr.name = "path_length";
    fn_expr.args.push_back(std::move(col_ref));

    BoundStatement bound;
    auto result = evaluate_expr(fn_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->is_null());
}

TEST(PathLengthSqlFunction, WrongArgCountFails) {
    FunctionCallExpr fn_expr;
    fn_expr.name = "path_length";
    // No args — should fail.

    Tuple empty_tuple;
    std::vector<OutputColumn> no_cols;
    OutputSchema empty_schema(std::move(no_cols));
    BoundStatement bound;

    auto result = evaluate_expr(fn_expr, empty_tuple, empty_schema, bound);
    EXPECT_FALSE(result.has_value());
}

} // namespace
} // namespace sixseven
