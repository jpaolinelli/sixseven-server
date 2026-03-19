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
#include <string>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Parser tests for inline WHERE predicates
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

TEST(InlinePredicateParse, NodeFilterExpr) {
    auto stmt = parse_one(
        "SELECT a.name FROM MATCH (a:persons)-[r:knows]->(b:persons WHERE b.active = TRUE)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 2u);

    // Source node has no inline filter.
    EXPECT_EQ(m->pattern[0].node.filter_expr, nullptr);

    // Target node has an inline filter.
    EXPECT_NE(m->pattern[1].node.filter_expr, nullptr);
}

TEST(InlinePredicateParse, EdgeFilterExpr) {
    auto stmt = parse_one("SELECT a.name FROM MATCH "
                          "(a:persons)-[r:knows WHERE r.since > '2020-01-01']->(b:persons)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 2u);

    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(edge->edge_type, "knows");
    EXPECT_NE(edge->filter_expr, nullptr);
}

TEST(InlinePredicateParse, CombinedNodeAndEdgeFilter) {
    auto stmt = parse_one("SELECT a.name, b.name FROM MATCH "
                          "(a:persons)-[r:knows WHERE r.since > '2020']->{1,6}"
                          "(b:persons WHERE b.active = TRUE)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);

    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_NE(edge->filter_expr, nullptr);
    EXPECT_TRUE(edge->is_variable_length());

    EXPECT_NE(m->pattern[1].node.filter_expr, nullptr);
}

TEST(InlinePredicateParse, NoFilterStillWorks) {
    auto stmt = parse_one("SELECT a.name FROM MATCH (a:persons)-[r:knows]->(b:persons)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);

    EXPECT_EQ(m->pattern[0].node.filter_expr, nullptr);
    EXPECT_EQ(m->pattern[1].node.filter_expr, nullptr);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->filter_expr, nullptr);
}

TEST(InlinePredicateParse, SourceNodeFilter) {
    auto stmt =
        parse_one("SELECT a.name FROM MATCH (a:persons WHERE a.id = 1)-[r:knows]->(b:persons)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);

    EXPECT_NE(m->pattern[0].node.filter_expr, nullptr);
    EXPECT_EQ(m->pattern[0].node.variable, "a");
    EXPECT_EQ(m->pattern[0].node.label, "persons");
}

// ============================================================================
// Executor tests for inline predicate filtering
// ============================================================================

class InlinePredicateTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_inline_pred";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        // Create 'persons' table with id, name, active, company.
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

        // Insert persons:
        // 1: Alice, active=true, Acme
        // 2: Bob, active=false, Globex
        // 3: Charlie, active=true, Acme
        // 4: Diana, active=false, Initech
        // 5: Eve, active=true, Acme
        insert_person(1, "Alice", true, "Acme");
        insert_person(2, "Bob", false, "Globex");
        insert_person(3, "Charlie", true, "Acme");
        insert_person(4, "Diana", false, "Initech");
        insert_person(5, "Eve", true, "Acme");

        // Create 'knows' edge type with a 'weight' property.
        ColumnDef weight_col{"weight", TypeId::INT64};
        auto eid = graph_->create_edge_type(default_database_id,
                                            "knows",
                                            persons_id_,
                                            persons_id_,
                                            TypeId::INT64,
                                            TypeId::INT64,
                                            {weight_col});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // Create linear chain: 1->2->3->4->5 with weights.
        link("knows", 1, 2, {Value(int64_t{10})});
        link("knows", 2, 3, {Value(int64_t{20})});
        link("knows", 3, 4, {Value(int64_t{30})});
        link("knows", 4, 5, {Value(int64_t{5})});
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

    /// Build a node filter expression from a comparison (e.g., b.active = TRUE).
    /// Uses the parser to create proper AST nodes.
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

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t persons_id_ = 0;
};

// -- Node filtering: target node predicate ------------------------------------

TEST_F(InlinePredicateTest, NodeFilterActiveOnly) {
    // (a:persons)-[r:knows]->{1,4}(b:persons WHERE b.active = TRUE)
    // Only active persons (1:Alice, 3:Charlie, 5:Eve) should appear as targets.
    // Chain: 1->2->3->4->5

    auto filter_expr = parse_expr("b.active = TRUE");
    ASSERT_NE(filter_expr, nullptr);

    BoundStatement bound;
    MatchConfig config;
    MatchNodeDef src_node;
    src_node.variable = "a";
    src_node.label = "persons";
    config.nodes.push_back(std::move(src_node));

    MatchNodeDef tgt_node;
    tgt_node.variable = "b";
    tgt_node.label = "persons";
    tgt_node.filter_expr = filter_expr.get();
    config.nodes.push_back(std::move(tgt_node));

    MatchEdgeDef edge_def("r", "knows", TraverseDirection::OUT, 1, 4);
    config.edges.push_back(std::move(edge_def));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    VariableLengthMatchOperator op(*graph_,
                                   *catalog_,
                                   *storage_,
                                   default_database_id,
                                   std::move(config),
                                   std::move(schema),
                                   nullptr,
                                   bound);
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<Tuple> results;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value())
            break;
        results.push_back(std::move(**row));
    }
    op.close();

    // With node pruning (inactive nodes pruned during BFS), results should
    // only contain active target nodes. Since BFS prunes inactive nodes,
    // paths cannot go through inactive nodes either.
    // Active: 1(Alice), 3(Charlie), 5(Eve). Inactive: 2(Bob), 4(Diana).
    // With pruning, from node 1: can't reach 2 (inactive), so can't reach 3,4,5 either.
    // From node 2: can reach 3 (active, 1-hop). From 3: can't reach 4 (inactive).
    // From node 3: can't reach 4 (inactive). From node 4: can reach 5 (active, 1-hop).
    // BFS prunes: from 1 → 2 is inactive → pruned → no further expansion from 1.
    // from 2 → 3 is active → emit. 3→4 inactive → pruned.
    // from 3 → 4 inactive → pruned.
    // from 4 → 5 active → emit.
    // from 5 → no neighbors.
    // Results: (Bob→Charlie), (Diana→Eve) = 2 results.
    EXPECT_EQ(results.size(), 2u);

    // Verify target names are active persons.
    for (const auto& t : results) {
        ASSERT_GE(t.values.size(), 2u);
        auto* name = std::get_if<std::string>(&t.values[1].data());
        ASSERT_NE(name, nullptr);
        EXPECT_TRUE(*name == "Charlie" || *name == "Eve") << "unexpected target: " << *name;
    }
}

// -- Edge filtering: edge property predicate ----------------------------------

TEST_F(InlinePredicateTest, EdgeFilterByWeight) {
    // (a:persons)-[r:knows WHERE r.weight > 15]->{1,4}(b:persons)
    // Edge weights: 1->2(10), 2->3(20), 3->4(30), 4->5(5)
    // Only edges with weight > 15 pass: 2->3(20), 3->4(30)

    auto filter_expr = parse_expr("r.weight > 15");
    ASSERT_NE(filter_expr, nullptr);

    BoundStatement bound;
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});

    MatchEdgeDef edge_def("r", "knows", TraverseDirection::OUT, 1, 4);
    edge_def.filter_expr = filter_expr.get();
    config.edges.push_back(std::move(edge_def));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    VariableLengthMatchOperator op(*graph_,
                                   *catalog_,
                                   *storage_,
                                   default_database_id,
                                   std::move(config),
                                   std::move(schema),
                                   nullptr,
                                   bound);
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<Tuple> results;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value())
            break;
        results.push_back(std::move(**row));
    }
    op.close();

    // With edge predicate (weight > 15), only edges 2->3 and 3->4 pass.
    // BFS: from 1 → edge to 2 (weight=10, fails) → no expansion from 1.
    // from 2 → edge to 3 (weight=20, passes) → 1-hop result (Bob→Charlie).
    //   from 3 → edge to 4 (weight=30, passes) → 2-hop result (Bob→Diana).
    //     from 4 → edge to 5 (weight=5, fails) → no expansion.
    // from 3 → edge to 4 (weight=30, passes) → 1-hop result (Charlie→Diana).
    //   from 4 → edge to 5 (weight=5, fails) → no expansion.
    // from 4 → edge to 5 (weight=5, fails) → no expansion from 4.
    // from 5 → no neighbors.
    // Results: (Bob→Charlie), (Bob→Diana), (Charlie→Diana) = 3 results.
    EXPECT_EQ(results.size(), 3u);
}

// -- Combined edge + node filtering -------------------------------------------

TEST_F(InlinePredicateTest, CombinedEdgeAndNodeFilter) {
    // (a:persons)-[r:knows WHERE r.weight > 15]->{1,4}(b:persons WHERE b.active = TRUE)
    // Edge filter: only edges 2->3(20), 3->4(30) pass.
    // Node filter: only active nodes (1, 3, 5) pass.

    auto edge_filter = parse_expr("r.weight > 15");
    auto node_filter = parse_expr("b.active = TRUE");
    ASSERT_NE(edge_filter, nullptr);
    ASSERT_NE(node_filter, nullptr);

    BoundStatement bound;
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});

    MatchNodeDef tgt_node;
    tgt_node.variable = "b";
    tgt_node.label = "persons";
    tgt_node.filter_expr = node_filter.get();
    config.nodes.push_back(std::move(tgt_node));

    MatchEdgeDef edge_def("r", "knows", TraverseDirection::OUT, 1, 4);
    edge_def.filter_expr = edge_filter.get();
    config.edges.push_back(std::move(edge_def));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    VariableLengthMatchOperator op(*graph_,
                                   *catalog_,
                                   *storage_,
                                   default_database_id,
                                   std::move(config),
                                   std::move(schema),
                                   nullptr,
                                   bound);
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<Tuple> results;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value())
            break;
        results.push_back(std::move(**row));
    }
    op.close();

    // Edge filter passes: 2->3(20), 3->4(30).
    // Node filter passes: active = true → 1(Alice), 3(Charlie), 5(Eve).
    // BFS from each source:
    // from 1: edge to 2 (weight=10 fails) → pruned.
    // from 2: edge to 3 (weight=20 passes). Node 3 is active → emit (Bob→Charlie).
    //   from 3: edge to 4 (weight=30 passes). Node 4 is inactive → pruned.
    // from 3: edge to 4 (weight=30 passes). Node 4 is inactive → pruned.
    // from 4: edge to 5 (weight=5 fails) → pruned.
    // from 5: no neighbors.
    // Result: (Bob→Charlie) = 1 result.
    EXPECT_EQ(results.size(), 1u);

    if (!results.empty()) {
        ASSERT_GE(results[0].values.size(), 2u);
        auto* src_name = std::get_if<std::string>(&results[0].values[0].data());
        auto* tgt_name = std::get_if<std::string>(&results[0].values[1].data());
        ASSERT_NE(src_name, nullptr);
        ASSERT_NE(tgt_name, nullptr);
        EXPECT_EQ(*src_name, "Bob");
        EXPECT_EQ(*tgt_name, "Charlie");
    }
}

// -- Fixed-length (single-hop) with node filter -------------------------------

TEST_F(InlinePredicateTest, FixedHopNodeFilter) {
    // (a:persons)-[r:knows]->(b:persons WHERE b.active = TRUE)
    // Single-hop, only active targets.
    // Edges: 1->2, 2->3, 3->4, 4->5.
    // Active targets: 3(Charlie), 5(Eve).
    // Results: (Bob→Charlie), (Diana→Eve) = 2 results.

    auto filter_expr = parse_expr("b.active = TRUE");
    ASSERT_NE(filter_expr, nullptr);

    BoundStatement bound;
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});

    MatchNodeDef tgt_node;
    tgt_node.variable = "b";
    tgt_node.label = "persons";
    tgt_node.filter_expr = filter_expr.get();
    config.nodes.push_back(std::move(tgt_node));

    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            std::move(schema),
                            nullptr,
                            bound);
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<Tuple> results;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value())
            break;
        results.push_back(std::move(**row));
    }
    op.close();

    EXPECT_EQ(results.size(), 2u);

    for (const auto& t : results) {
        ASSERT_GE(t.values.size(), 2u);
        auto* name = std::get_if<std::string>(&t.values[1].data());
        ASSERT_NE(name, nullptr);
        EXPECT_TRUE(*name == "Charlie" || *name == "Eve") << "unexpected target: " << *name;
    }
}

// -- No filter still returns all results --------------------------------------

TEST_F(InlinePredicateTest, NoFilterReturnsAll) {
    // (a:persons)-[r:knows]->{1,1}(b:persons) — no inline predicates.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 1, 1));

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
                                   bound);
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<Tuple> results;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value())
            break;
        results.push_back(std::move(**row));
    }
    op.close();

    // 4 edges in chain = 4 single-hop results.
    EXPECT_EQ(results.size(), 4u);
}

} // namespace
} // namespace sixseven
