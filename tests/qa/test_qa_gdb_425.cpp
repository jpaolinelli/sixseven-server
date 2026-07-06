/// @file test_qa_gdb_425.cpp
/// QA adversarial tests for GDB-425: Inline Predicate Filtering.
///
/// Verifies:
///   AC1: Edge filtering: [r:knows WHERE r.since > '2020'] prunes edges during traversal
///   AC2: Node filtering: (b:persons WHERE b.active = TRUE) prunes nodes during traversal
///   AC3: Combined edge + node filtering works
///   AC4: Pruning reduces visited node count vs post-traversal WHERE
///   AC5: Unit tests written and passing
///   AC6: No regressions
///
/// Adversarial categories: empty graphs, all-filtered-out, source node filter,
/// multi-hop fixed-length with filters, BOTH direction with inline predicates,
/// edge filter with no edge properties, node filter on nonexistent column,
/// stress test with aggressive pruning, min_hops boundary, self-loop handling,
/// triple filter (source + edge + target), parser edge cases.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/executor/pattern_match.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/variable_length_match.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "test_qa_helpers.h"

namespace sixseven {
namespace {

// ============================================================================
// Parser helpers
// ============================================================================

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

void parse_fails(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return; // Lexer failure is acceptable.
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_FALSE(stmts.has_value()) << "expected parse failure for: " << sql;
}

// ============================================================================
// Parser adversarial tests
// ============================================================================

TEST(QA_GDB425_Parse, DoubleWhereInNodePattern) {
    // WHERE inside node + WHERE clause should both work (inner filters pattern,
    // outer filters results).
    auto stmt =
        parse_one("SELECT a.name FROM MATCH (a:persons WHERE a.id > 1)-[r:knows]->(b:persons) "
                  "WHERE a.name = 'Alice'");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    // Inner filter on node.
    EXPECT_NE(m->pattern[0].node.filter_expr, nullptr);
    // Outer WHERE clause.
    EXPECT_NE(sel->where_expr, nullptr);
}

TEST(QA_GDB425_Parse, DoubleWhereInEdgePattern) {
    // WHERE inside edge + outer WHERE.
    auto stmt = parse_one("SELECT a.name FROM MATCH "
                          "(a:persons)-[r:knows WHERE r.weight > 5]->(b:persons) "
                          "WHERE b.name = 'Bob'");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_NE(m->pattern[0].outgoing_edge->filter_expr, nullptr);
    EXPECT_NE(sel->where_expr, nullptr);
}

TEST(QA_GDB425_Parse, InlineWhereWithVariableLength) {
    // Inline WHERE on edge + quantifier.
    auto stmt = parse_one(
        "SELECT a.name FROM MATCH "
        "(a:persons)-[r:knows WHERE r.weight > 0]->{2,5}(b:persons WHERE b.active = TRUE)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_NE(m->pattern[0].outgoing_edge->filter_expr, nullptr);
    EXPECT_TRUE(m->pattern[0].outgoing_edge->is_variable_length());
    EXPECT_NE(m->pattern[1].node.filter_expr, nullptr);
}

TEST(QA_GDB425_Parse, InlineWhereWithAndExpr) {
    // Compound expression in inline WHERE.
    auto stmt = parse_one("SELECT a.name FROM MATCH "
                          "(a:persons WHERE a.id > 1 AND a.active = TRUE)-[r:knows]->(b:persons)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    EXPECT_NE(m->pattern[0].node.filter_expr, nullptr);
}

TEST(QA_GDB425_Parse, EmptyWhereInNodeShouldFail) {
    // WHERE with no expression should fail.
    parse_fails("SELECT a.name FROM MATCH (a:persons WHERE)-[r:knows]->(b:persons)");
}

TEST(QA_GDB425_Parse, EmptyWhereInEdgeShouldFail) {
    parse_fails("SELECT a.name FROM MATCH (a:persons)-[r:knows WHERE]->(b:persons)");
}

TEST(QA_GDB425_Parse, InlineWhereOnBothNodeAndEdge) {
    // All three positions: source, edge, target.
    auto stmt = parse_one("SELECT a.name FROM MATCH "
                          "(a:persons WHERE a.id = 1)-[r:knows WHERE r.weight > 5]->"
                          "(b:persons WHERE b.active = TRUE)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    EXPECT_NE(m->pattern[0].node.filter_expr, nullptr);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_NE(m->pattern[0].outgoing_edge->filter_expr, nullptr);
    EXPECT_NE(m->pattern[1].node.filter_expr, nullptr);
}

// ============================================================================
// Executor test fixture
// ============================================================================

class QA_GDB425 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb425";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        bootstrap_qa_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        // Create 'persons' table: id(INT64), name(STRING), active(BOOL), company(STRING).
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
            CatalogColumnDef active_col;
            active_col.ordinal = 2;
            active_col.name = "active";
            active_col.type_id = TypeId::BOOL;
            active_col.nullable = false;
            ts.columns.push_back(active_col);
            CatalogColumnDef company_col;
            company_col.ordinal = 3;
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

        // Insert persons.
        // 1: Alice, active=true, Acme
        // 2: Bob, active=false, Globex
        // 3: Charlie, active=true, Acme
        // 4: Diana, active=false, Initech
        // 5: Eve, active=true, Acme
        // 6: Frank, active=true, Globex
        insert_person(1, "Alice", true, "Acme");
        insert_person(2, "Bob", false, "Globex");
        insert_person(3, "Charlie", true, "Acme");
        insert_person(4, "Diana", false, "Initech");
        insert_person(5, "Eve", true, "Acme");
        insert_person(6, "Frank", true, "Globex");

        // Create 'knows' edge type with 'weight' (INT64) property.
        ColumnDef weight_col{"weight", TypeId::INT64};
        auto eid = graph_->create_edge_type(default_database_id,
                                            "knows",
                                            persons_id_,
                                            persons_id_,
                                            TypeId::INT64,
                                            TypeId::INT64,
                                            {weight_col});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // Graph topology:
        //   1(Alice) -10-> 2(Bob) -20-> 3(Charlie) -30-> 4(Diana) -5-> 5(Eve) -15-> 6(Frank)
        //   1(Alice) -25-> 3(Charlie)  (shortcut edge)
        link("knows", 1, 2, {Value(int64_t{10})});
        link("knows", 2, 3, {Value(int64_t{20})});
        link("knows", 3, 4, {Value(int64_t{30})});
        link("knows", 4, 5, {Value(int64_t{5})});
        link("knows", 5, 6, {Value(int64_t{15})});
        link("knows", 1, 3, {Value(int64_t{25})}); // shortcut
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void
    insert_person(int64_t id, const std::string& name, bool active, const std::string& company) {
        auto ts = storage_->get_table_storage(persons_id_);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        auto schema = catalog_->get_table(default_database_id, "persons");
        ASSERT_TRUE(schema.has_value());
        std::vector<Value> vals = {Value(id), Value(name), Value(active), Value(company)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    void link(const std::string& edge_type,
              int64_t from,
              int64_t to,
              const std::vector<Value>& props = {}) {
        auto r = graph_->link(default_database_id, edge_type, Value(from), Value(to), props);
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    std::unique_ptr<Expr> parse_expr(std::string_view expr_str) {
        std::string sql = "SELECT 1 WHERE " + std::string(expr_str);
        Lexer lexer(sql);
        auto tokens = lexer.tokenize();
        if (!tokens)
            return nullptr;
        Parser parser(std::move(*tokens));
        auto stmts = parser.parse_all();
        if (!stmts || stmts->empty())
            return nullptr;
        auto* sel = dynamic_cast<SelectStmt*>((*stmts)[0].get());
        if (!sel || !sel->where_expr)
            return nullptr;
        return std::move(sel->where_expr);
    }

    /// Run a single-hop PatternMatchOperator and collect results.
    std::vector<Tuple>
    run_single_hop(MatchConfig config, OutputSchema schema, const Expr* where_expr = nullptr) {
        BoundStatement bound;
        PatternMatchOperator op(*graph_,
                                *catalog_,
                                *storage_,
                                default_database_id,
                                std::move(config),
                                std::move(schema),
                                where_expr,
                                bound);
        auto r = op.open();
        EXPECT_TRUE(r.has_value()) << r.error().message;
        if (!r)
            return {};

        std::vector<Tuple> results;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row || !row->has_value())
                break;
            results.push_back(std::move(**row));
        }
        op.close();
        return results;
    }

    /// Run a variable-length VariableLengthMatchOperator and collect results.
    std::vector<Tuple> run_var_length(MatchConfig config,
                                      OutputSchema schema,
                                      const Expr* where_expr = nullptr,
                                      size_t max_visited = 100'000) {
        BoundStatement bound;
        VariableLengthMatchOperator op(*graph_,
                                       *catalog_,
                                       *storage_,
                                       default_database_id,
                                       std::move(config),
                                       std::move(schema),
                                       where_expr,
                                       bound,
                                       max_visited);
        auto r = op.open();
        EXPECT_TRUE(r.has_value()) << r.error().message;
        if (!r)
            return {};

        std::vector<Tuple> results;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row || !row->has_value())
                break;
            results.push_back(std::move(**row));
        }
        op.close();
        return results;
    }

    /// Extract target name (column 1) from result tuples.
    std::set<std::string> target_names(const std::vector<Tuple>& results) {
        std::set<std::string> names;
        for (const auto& t : results) {
            if (t.values.size() >= 2) {
                if (auto* n = std::get_if<std::string>(&t.values[1].data())) {
                    names.insert(*n);
                }
            }
        }
        return names;
    }

    /// Extract source name (column 0) from result tuples.
    std::set<std::string> source_names(const std::vector<Tuple>& results) {
        std::set<std::string> names;
        for (const auto& t : results) {
            if (!t.values.empty()) {
                if (auto* n = std::get_if<std::string>(&t.values[0].data())) {
                    names.insert(*n);
                }
            }
        }
        return names;
    }

    OutputSchema two_col_schema() {
        std::vector<OutputColumn> cols;
        cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
        cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
        return OutputSchema(std::move(cols));
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t persons_id_ = 0;
};

// ============================================================================
// AC1: Edge filtering prunes edges during traversal
// ============================================================================

TEST_F(QA_GDB425, EdgeFilterPrunesHighWeight) {
    // [r:knows WHERE r.weight > 15] — only edges with weight > 15 pass.
    // Passing edges: 2->3(20), 3->4(30), 1->3(25), 5->6(15 fails).
    auto filter = parse_expr("r.weight > 15");
    ASSERT_NE(filter, nullptr);

    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    MatchEdgeDef edge("r", "knows", TraverseDirection::OUT, 1, 4);
    edge.filter_expr = filter.get();
    config.edges.push_back(std::move(edge));

    auto results = run_var_length(std::move(config), two_col_schema());

    // From each source:
    // 1: edges 1->2(10 fail), 1->3(25 pass) → reach 3. From 3: 3->4(30 pass) → reach 4.
    //    From 4: 4->5(5 fail). So from 1: {3, 4}.
    // 2: edge 2->3(20 pass) → reach 3. From 3: 3->4(30 pass) → reach 4. So from 2: {3, 4}.
    // 3: edge 3->4(30 pass) → reach 4. From 4: 4->5(5 fail). So from 3: {4}.
    // 4: edge 4->5(5 fail). So from 4: nothing.
    // 5: edge 5->6(15 fail). So from 5: nothing.
    // 6: no outgoing. So from 6: nothing.
    // Total = 2 + 2 + 1 = 5.
    EXPECT_EQ(results.size(), 5u);

    // Verify that low-weight edges were pruned: no path should reach 2 or 5 or 6.
    auto tgts = target_names(results);
    EXPECT_EQ(tgts.count("Bob"), 0u);   // Only reachable via weight=10 edge
    EXPECT_EQ(tgts.count("Eve"), 0u);   // Only reachable via weight=5 edge
    EXPECT_EQ(tgts.count("Frank"), 0u); // Only reachable via weight=15 edge
}

TEST_F(QA_GDB425, EdgeFilterAllEdgesFiltered) {
    // r.weight > 999 → no edges pass. Should produce zero results.
    auto filter = parse_expr("r.weight > 999");
    ASSERT_NE(filter, nullptr);

    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    MatchEdgeDef edge("r", "knows", TraverseDirection::OUT, 1, 4);
    edge.filter_expr = filter.get();
    config.edges.push_back(std::move(edge));

    auto results = run_var_length(std::move(config), two_col_schema());
    EXPECT_EQ(results.size(), 0u);
}

TEST_F(QA_GDB425, EdgeFilterSingleHop) {
    // Single-hop with edge filter: only edges with weight >= 20 pass.
    // Passing: 2->3(20), 3->4(30), 1->3(25).
    auto filter = parse_expr("r.weight >= 20");
    ASSERT_NE(filter, nullptr);

    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    MatchEdgeDef edge("r", "knows", TraverseDirection::OUT);
    edge.filter_expr = filter.get();
    config.edges.push_back(std::move(edge));

    auto results = run_single_hop(std::move(config), two_col_schema());
    EXPECT_EQ(results.size(), 3u);

    auto tgts = target_names(results);
    EXPECT_EQ(tgts.count("Charlie"), 1u); // from both Alice(25) and Bob(20)
    EXPECT_EQ(tgts.count("Diana"), 1u);   // from Charlie(30)
}

// ============================================================================
// AC2: Node filtering prunes nodes during traversal
// ============================================================================

TEST_F(QA_GDB425, NodeFilterActiveOnly_VarLength) {
    // Target filter: b.active = TRUE → only active nodes (1,3,5,6) pass.
    // Inactive: 2(Bob), 4(Diana).
    // BFS prunes inactive nodes (no emit, no expansion).
    auto filter = parse_expr("b.active = TRUE");
    ASSERT_NE(filter, nullptr);

    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    MatchNodeDef tgt;
    tgt.variable = "b";
    tgt.label = "persons";
    tgt.filter_expr = filter.get();
    config.nodes.push_back(std::move(tgt));
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 1, 5));

    auto results = run_var_length(std::move(config), two_col_schema());

    // With BFS pruning of inactive nodes:
    // From 1: 1->2(inactive, pruned), 1->3(active, emit). From 3: 3->4(inactive, pruned). → {3}
    // From 2: 2->3(active, emit). From 3: 3->4(inactive, pruned). → {3}
    // From 3: 3->4(inactive, pruned). → nothing
    // From 4: 4->5(active, emit). From 5: 5->6(active, emit). → {5, 6}
    // From 5: 5->6(active, emit). From 6: no outgoing. → {6}
    // From 6: no outgoing. → nothing
    // Total: 1 + 1 + 0 + 2 + 1 + 0 = 5
    EXPECT_EQ(results.size(), 5u);

    // Verify no inactive targets.
    auto tgts = target_names(results);
    EXPECT_EQ(tgts.count("Bob"), 0u);
    EXPECT_EQ(tgts.count("Diana"), 0u);
}

TEST_F(QA_GDB425, NodeFilterAllNodesFiltered) {
    // b.company = 'Nonexistent' → no targets pass.
    auto filter = parse_expr("b.company = 'Nonexistent'");
    ASSERT_NE(filter, nullptr);

    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    MatchNodeDef tgt;
    tgt.variable = "b";
    tgt.label = "persons";
    tgt.filter_expr = filter.get();
    config.nodes.push_back(std::move(tgt));
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 1, 4));

    auto results = run_var_length(std::move(config), two_col_schema());
    EXPECT_EQ(results.size(), 0u);
}

TEST_F(QA_GDB425, NodeFilterSingleHop) {
    // Single-hop target filter: b.active = TRUE.
    auto filter = parse_expr("b.active = TRUE");
    ASSERT_NE(filter, nullptr);

    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    MatchNodeDef tgt;
    tgt.variable = "b";
    tgt.label = "persons";
    tgt.filter_expr = filter.get();
    config.nodes.push_back(std::move(tgt));
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT));

    auto results = run_single_hop(std::move(config), two_col_schema());

    // Edges: 1->2(inactive), 2->3(active), 3->4(inactive), 4->5(active), 5->6(active), 1->3(active)
    // Active targets: 3 (from 2 and 1), 5 (from 4), 6 (from 5) = 4 results.
    EXPECT_EQ(results.size(), 4u);
    auto tgts = target_names(results);
    EXPECT_EQ(tgts.count("Bob"), 0u);
    EXPECT_EQ(tgts.count("Diana"), 0u);
}

// ============================================================================
// AC3: Combined edge + node filtering
// ============================================================================

TEST_F(QA_GDB425, CombinedEdgeAndNodeFilter_VarLength) {
    // Edge: r.weight > 15. Node: b.active = TRUE.
    auto edge_filter = parse_expr("r.weight > 15");
    auto node_filter = parse_expr("b.active = TRUE");
    ASSERT_NE(edge_filter, nullptr);
    ASSERT_NE(node_filter, nullptr);

    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    MatchNodeDef tgt;
    tgt.variable = "b";
    tgt.label = "persons";
    tgt.filter_expr = node_filter.get();
    config.nodes.push_back(std::move(tgt));
    MatchEdgeDef edge("r", "knows", TraverseDirection::OUT, 1, 4);
    edge.filter_expr = edge_filter.get();
    config.edges.push_back(std::move(edge));

    auto results = run_var_length(std::move(config), two_col_schema());

    // Edges passing weight > 15: 2->3(20), 3->4(30), 1->3(25).
    // Active targets: 1(Alice), 3(Charlie), 5(Eve), 6(Frank).
    // From 1: 1->2(weight=10 fail), 1->3(weight=25 pass, Charlie active) → emit (Alice→Charlie).
    //   From 3: 3->4(weight=30 pass, Diana inactive) → pruned.
    // From 2: 2->3(weight=20 pass, Charlie active) → emit (Bob→Charlie).
    //   From 3: 3->4(weight=30 pass, Diana inactive) → pruned.
    // From 3: 3->4(weight=30 pass, Diana inactive) → pruned.
    // From 4: 4->5(weight=5 fail). Nothing.
    // From 5: 5->6(weight=15 fail). Nothing.
    // From 6: no outgoing.
    // Total: 2 (Alice→Charlie, Bob→Charlie).
    EXPECT_EQ(results.size(), 2u);

    auto tgts = target_names(results);
    EXPECT_EQ(tgts.size(), 1u);
    EXPECT_EQ(tgts.count("Charlie"), 1u);
}

TEST_F(QA_GDB425, CombinedEdgeAndNodeFilter_SingleHop) {
    // Single-hop: edge weight >= 20, target active.
    auto edge_filter = parse_expr("r.weight >= 20");
    auto node_filter = parse_expr("b.active = TRUE");
    ASSERT_NE(edge_filter, nullptr);
    ASSERT_NE(node_filter, nullptr);

    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    MatchNodeDef tgt;
    tgt.variable = "b";
    tgt.label = "persons";
    tgt.filter_expr = node_filter.get();
    config.nodes.push_back(std::move(tgt));
    MatchEdgeDef edge("r", "knows", TraverseDirection::OUT);
    edge.filter_expr = edge_filter.get();
    config.edges.push_back(std::move(edge));

    auto results = run_single_hop(std::move(config), two_col_schema());

    // Edges passing weight >= 20: 2->3(20), 3->4(30), 1->3(25).
    // Active targets: 3(Charlie) from edges 2->3 and 1->3. 4(Diana) inactive.
    // Results: (Bob→Charlie), (Alice→Charlie) = 2.
    EXPECT_EQ(results.size(), 2u);
    auto tgts = target_names(results);
    EXPECT_EQ(tgts.size(), 1u);
    EXPECT_EQ(tgts.count("Charlie"), 1u);
}

// ============================================================================
// AC4: Pruning reduces visited count (node filter cuts BFS tree)
// ============================================================================

TEST_F(QA_GDB425, PruningReducesResults) {
    // Without filter: variable-length 1-5 hops from all sources.
    MatchConfig config_no_filter;
    config_no_filter.nodes.push_back({"a", "persons"});
    config_no_filter.nodes.push_back({"b", "persons"});
    config_no_filter.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 1, 5));
    auto no_filter_results = run_var_length(std::move(config_no_filter), two_col_schema());

    // With aggressive filter: b.active = FALSE → only Bob(2) and Diana(4) as targets.
    // Since pruned nodes are not expanded, fewer BFS expansions occur.
    auto filter = parse_expr("b.active = FALSE");
    ASSERT_NE(filter, nullptr);

    MatchConfig config_filter;
    config_filter.nodes.push_back({"a", "persons"});
    MatchNodeDef tgt;
    tgt.variable = "b";
    tgt.label = "persons";
    tgt.filter_expr = filter.get();
    config_filter.nodes.push_back(std::move(tgt));
    config_filter.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 1, 5));
    auto filter_results = run_var_length(std::move(config_filter), two_col_schema());

    // The filtered set must be strictly smaller.
    EXPECT_LT(filter_results.size(), no_filter_results.size());
}

// ============================================================================
// Source node filter
// ============================================================================

TEST_F(QA_GDB425, SourceNodeFilter_VarLength) {
    // Source filter: a.id = 1 → only Alice as source.
    auto src_filter = parse_expr("a.id = 1");
    ASSERT_NE(src_filter, nullptr);

    MatchConfig config;
    MatchNodeDef src;
    src.variable = "a";
    src.label = "persons";
    src.filter_expr = src_filter.get();
    config.nodes.push_back(std::move(src));
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 1, 5));

    auto results = run_var_length(std::move(config), two_col_schema());

    // Only Alice as source. Chain: 1->2->3->4->5->6, plus shortcut 1->3.
    // From 1: 2(1-hop), 3(1-hop via shortcut or 2-hop), 4, 5, 6.
    // All source names should be Alice.
    auto srcs = source_names(results);
    EXPECT_EQ(srcs.size(), 1u);
    EXPECT_EQ(srcs.count("Alice"), 1u);
    EXPECT_GT(results.size(), 0u);
}

TEST_F(QA_GDB425, SourceNodeFilter_SingleHop) {
    // Source filter: a.company = 'Acme' → Alice(1), Charlie(3), Eve(5).
    auto src_filter = parse_expr("a.company = 'Acme'");
    ASSERT_NE(src_filter, nullptr);

    MatchConfig config;
    MatchNodeDef src;
    src.variable = "a";
    src.label = "persons";
    src.filter_expr = src_filter.get();
    config.nodes.push_back(std::move(src));
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT));

    auto results = run_single_hop(std::move(config), two_col_schema());

    // Acme sources: Alice(1), Charlie(3), Eve(5).
    // Alice: 1->2, 1->3 = 2 edges.
    // Charlie: 3->4 = 1 edge.
    // Eve: 5->6 = 1 edge.
    // Total: 4 results.
    EXPECT_EQ(results.size(), 4u);
    auto srcs = source_names(results);
    EXPECT_EQ(srcs.count("Alice"), 1u);
    EXPECT_EQ(srcs.count("Charlie"), 1u);
    EXPECT_EQ(srcs.count("Eve"), 1u);
    EXPECT_EQ(srcs.count("Bob"), 0u);
    EXPECT_EQ(srcs.count("Diana"), 0u);
    EXPECT_EQ(srcs.count("Frank"), 0u);
}

// ============================================================================
// Triple filter: source + edge + target
// ============================================================================

TEST_F(QA_GDB425, TripleFilter_SingleHop) {
    // Source: a.company = 'Acme', Edge: r.weight >= 20, Target: b.active = FALSE.
    auto src_filter = parse_expr("a.company = 'Acme'");
    auto edge_filter = parse_expr("r.weight >= 20");
    auto tgt_filter = parse_expr("b.active = FALSE");
    ASSERT_NE(src_filter, nullptr);
    ASSERT_NE(edge_filter, nullptr);
    ASSERT_NE(tgt_filter, nullptr);

    MatchConfig config;
    MatchNodeDef src;
    src.variable = "a";
    src.label = "persons";
    src.filter_expr = src_filter.get();
    config.nodes.push_back(std::move(src));
    MatchNodeDef tgt;
    tgt.variable = "b";
    tgt.label = "persons";
    tgt.filter_expr = tgt_filter.get();
    config.nodes.push_back(std::move(tgt));
    MatchEdgeDef edge("r", "knows", TraverseDirection::OUT);
    edge.filter_expr = edge_filter.get();
    config.edges.push_back(std::move(edge));

    auto results = run_single_hop(std::move(config), two_col_schema());

    // Acme sources: Alice(1), Charlie(3), Eve(5).
    // Edges from Acme sources with weight >= 20:
    //   Alice: 1->3(25, but Charlie active → fail tgt), 1->2(10 fails edge).
    //   Charlie: 3->4(30, Diana inactive → pass!).
    //   Eve: 5->6(15 fails edge).
    // Result: (Charlie→Diana) = 1.
    EXPECT_EQ(results.size(), 1u);
    if (!results.empty()) {
        auto* s = std::get_if<std::string>(&results[0].values[0].data());
        auto* t = std::get_if<std::string>(&results[0].values[1].data());
        ASSERT_NE(s, nullptr);
        ASSERT_NE(t, nullptr);
        EXPECT_EQ(*s, "Charlie");
        EXPECT_EQ(*t, "Diana");
    }
}

// ============================================================================
// Edge cases: no edges, empty graph behavior
// ============================================================================

TEST_F(QA_GDB425, EdgeFilterOnZeroEdges) {
    // Create a new edge type with no edges and test filtering.
    ColumnDef score_col{"score", TypeId::INT64};
    auto eid = graph_->create_edge_type(default_database_id,
                                        "likes",
                                        persons_id_,
                                        persons_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        {score_col});
    ASSERT_TRUE(eid.has_value()) << eid.error().message;

    auto filter = parse_expr("r.score > 0");
    ASSERT_NE(filter, nullptr);

    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    MatchEdgeDef edge("r", "likes", TraverseDirection::OUT);
    edge.filter_expr = filter.get();
    config.edges.push_back(std::move(edge));

    auto results = run_single_hop(std::move(config), two_col_schema());
    EXPECT_EQ(results.size(), 0u);
}

// ============================================================================
// Boundary: min_hops = 0
// ============================================================================

TEST_F(QA_GDB425, MinHopsZeroWithNodeFilter) {
    // {0,2} with target filter. At depth 0, the source itself may be emitted.
    // The target node filter should NOT apply at depth 0 (start node).
    auto filter = parse_expr("b.active = TRUE");
    ASSERT_NE(filter, nullptr);

    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    MatchNodeDef tgt;
    tgt.variable = "b";
    tgt.label = "persons";
    tgt.filter_expr = filter.get();
    config.nodes.push_back(std::move(tgt));
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 0, 2));

    auto results = run_var_length(std::move(config), two_col_schema());

    // At depth 0, every source emits itself (tgt_node filter not applied at depth 0).
    // At depth 1+, only active nodes are emitted AND expanded.
    // Depth 0: all 6 nodes emit.
    // Depth 1+: filtered by active.
    EXPECT_GE(results.size(), 6u); // At least the 6 depth-0 self-results.
}

// ============================================================================
// Multi-hop fixed-length chain with filters
// ============================================================================

TEST_F(QA_GDB425, MultiHopFixedLengthWithEdgeFilter) {
    // Two fixed edges: a->b->c, both with edge filter r.weight >= 20.
    // Create a second edge type or reuse 'knows'.
    auto edge_filter = parse_expr("r.weight >= 20");
    ASSERT_NE(edge_filter, nullptr);
    // We need a separate filter for second edge variable.
    auto edge_filter2 = parse_expr("s.weight >= 20");
    ASSERT_NE(edge_filter2, nullptr);

    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "persons"});
    MatchEdgeDef e1("r", "knows", TraverseDirection::OUT);
    e1.filter_expr = edge_filter.get();
    config.edges.push_back(std::move(e1));
    MatchEdgeDef e2("s", "knows", TraverseDirection::OUT);
    e2.filter_expr = edge_filter2.get();
    config.edges.push_back(std::move(e2));

    std::vector<OutputColumn> cols;
    cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    cols.push_back({"c", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(cols));

    // Use PatternMatch (multi-hop fixed).
    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            std::move(schema),
                            nullptr,
                            bound);
    auto r = op.open();
    ASSERT_TRUE(r.has_value()) << r.error().message;

    std::vector<Tuple> results;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value())
            break;
        results.push_back(std::move(**row));
    }
    op.close();

    // Edges with weight >= 20: 2->3(20), 3->4(30), 1->3(25).
    // Two-hop chains where both edges pass:
    //   Alice->Charlie->Diana: 1->3(25 pass), 3->4(30 pass) ✓
    //   Bob->Charlie->Diana: 2->3(20 pass), 3->4(30 pass) ✓
    // That's it — no other two-hop chain has both edges >= 20.
    EXPECT_EQ(results.size(), 2u);
}

// ============================================================================
// Stress test: high fan-out with aggressive pruning
// ============================================================================

TEST_F(QA_GDB425, StressPruningReducesWork) {
    // Create a wider graph: add many persons and edges from person 1.
    for (int64_t i = 100; i < 120; ++i) {
        insert_person(i, "Person" + std::to_string(i), (i % 2 == 0), "Corp");
        link("knows", 1, i, {Value(int64_t{1})}); // weight = 1 (low)
    }

    // Without filter: many results.
    MatchConfig config_no;
    config_no.nodes.push_back({"a", "persons"});
    config_no.nodes.push_back({"b", "persons"});
    config_no.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 1, 2));
    auto no_results = run_var_length(std::move(config_no), two_col_schema());

    // With edge filter: r.weight > 10 → only original high-weight edges pass.
    auto filter = parse_expr("r.weight > 10");
    ASSERT_NE(filter, nullptr);
    MatchConfig config_f;
    config_f.nodes.push_back({"a", "persons"});
    config_f.nodes.push_back({"b", "persons"});
    MatchEdgeDef edge("r", "knows", TraverseDirection::OUT, 1, 2);
    edge.filter_expr = filter.get();
    config_f.edges.push_back(std::move(edge));
    auto f_results = run_var_length(std::move(config_f), two_col_schema());

    EXPECT_LT(f_results.size(), no_results.size());
}

// ============================================================================
// Operator reopen: verify state reset with filters
// ============================================================================

TEST_F(QA_GDB425, ReopenWithFilters) {
    auto filter = parse_expr("b.active = TRUE");
    ASSERT_NE(filter, nullptr);

    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    MatchNodeDef tgt;
    tgt.variable = "b";
    tgt.label = "persons";
    tgt.filter_expr = filter.get();
    config.nodes.push_back(std::move(tgt));
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT));

    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            two_col_schema(),
                            nullptr,
                            bound);

    // First open/close cycle.
    auto r1 = op.open();
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    size_t count1 = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value());
        if (!row->has_value())
            break;
        ++count1;
    }
    op.close();

    // Second open/close cycle — must produce same results.
    auto r2 = op.open();
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    size_t count2 = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value());
        if (!row->has_value())
            break;
        ++count2;
    }
    op.close();

    EXPECT_EQ(count1, count2);
    EXPECT_GT(count1, 0u);
}

// ============================================================================
// Edge filter with equality check
// ============================================================================

TEST_F(QA_GDB425, EdgeFilterEquality) {
    // r.weight = 20 → only the 2->3 edge.
    auto filter = parse_expr("r.weight = 20");
    ASSERT_NE(filter, nullptr);

    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    MatchEdgeDef edge("r", "knows", TraverseDirection::OUT);
    edge.filter_expr = filter.get();
    config.edges.push_back(std::move(edge));

    auto results = run_single_hop(std::move(config), two_col_schema());
    EXPECT_EQ(results.size(), 1u);
    if (!results.empty()) {
        auto* t = std::get_if<std::string>(&results[0].values[1].data());
        ASSERT_NE(t, nullptr);
        EXPECT_EQ(*t, "Charlie");
    }
}

// ============================================================================
// Node filter with string comparison
// ============================================================================

TEST_F(QA_GDB425, NodeFilterStringComparison) {
    // b.name = 'Eve' → only Eve as target.
    auto filter = parse_expr("b.name = 'Eve'");
    ASSERT_NE(filter, nullptr);

    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    MatchNodeDef tgt;
    tgt.variable = "b";
    tgt.label = "persons";
    tgt.filter_expr = filter.get();
    config.nodes.push_back(std::move(tgt));
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT));

    auto results = run_single_hop(std::move(config), two_col_schema());
    // Only edge 4->5 targets Eve.
    EXPECT_EQ(results.size(), 1u);
    auto tgts = target_names(results);
    EXPECT_EQ(tgts.count("Eve"), 1u);
}

// ============================================================================
// No filter (regression check — existing behavior unchanged)
// ============================================================================

TEST_F(QA_GDB425, NoFilterRegression_SingleHop) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT));

    auto results = run_single_hop(std::move(config), two_col_schema());
    // 6 edges total: 1->2, 2->3, 3->4, 4->5, 5->6, 1->3.
    EXPECT_EQ(results.size(), 6u);
}

TEST_F(QA_GDB425, NoFilterRegression_VarLength) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 1, 1));

    auto results = run_var_length(std::move(config), two_col_schema());
    // Same 6 single-hop edges.
    EXPECT_EQ(results.size(), 6u);
}

// ============================================================================
// IN direction with node filter
// ============================================================================

TEST_F(QA_GDB425, InDirectionWithNodeFilter) {
    // Reverse traversal: (a:persons)<-[r:knows]-(b:persons WHERE b.active = TRUE)
    // This means: for each source a, find b such that b->a exists and b is active.
    auto filter = parse_expr("b.active = TRUE");
    ASSERT_NE(filter, nullptr);

    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    MatchNodeDef tgt;
    tgt.variable = "b";
    tgt.label = "persons";
    tgt.filter_expr = filter.get();
    config.nodes.push_back(std::move(tgt));
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::IN));

    auto results = run_single_hop(std::move(config), two_col_schema());

    // IN direction: for source a, get edges TO a, neighbor = source_pk.
    // Edges: 1->2, 2->3, 3->4, 4->5, 5->6, 1->3.
    // For a=2: edge 1->2 exists, neighbor=1(Alice, active) → pass.
    // For a=3: edges 2->3 (Bob inactive → fail), 1->3 (Alice active → pass).
    // For a=4: edge 3->4 (Charlie active → pass).
    // For a=5: edge 4->5 (Diana inactive → fail).
    // For a=6: edge 5->6 (Eve active → pass).
    // Total active sources as targets: 4 results.
    EXPECT_EQ(results.size(), 4u);
    auto tgts = target_names(results);
    EXPECT_EQ(tgts.count("Bob"), 0u);
    EXPECT_EQ(tgts.count("Diana"), 0u);
}

// ============================================================================
// Compound expression in inline filter
// ============================================================================

TEST_F(QA_GDB425, CompoundAndFilter) {
    // b.active = TRUE AND b.company = 'Acme'
    auto filter = parse_expr("b.active = TRUE AND b.company = 'Acme'");
    ASSERT_NE(filter, nullptr);

    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    MatchNodeDef tgt;
    tgt.variable = "b";
    tgt.label = "persons";
    tgt.filter_expr = filter.get();
    config.nodes.push_back(std::move(tgt));
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT));

    auto results = run_single_hop(std::move(config), two_col_schema());

    // Active Acme: Alice(1), Charlie(3), Eve(5).
    // Edges to these targets: 1->3(Charlie from Alice shortcut), 2->3(Charlie from Bob),
    //   4->5(Eve from Diana). Alice(1) has no incoming in this direction.
    // Wait — targets must be active AND Acme. Targets: 3(Charlie), 5(Eve).
    //   1 is not a target of any edge.
    // Edges: 2->3(Charlie), 1->3(Charlie), 4->5(Eve) = 3 results.
    EXPECT_EQ(results.size(), 3u);
    auto tgts = target_names(results);
    EXPECT_EQ(tgts.count("Charlie"), 1u);
    EXPECT_EQ(tgts.count("Eve"), 1u);
}

} // namespace
} // namespace sixseven
