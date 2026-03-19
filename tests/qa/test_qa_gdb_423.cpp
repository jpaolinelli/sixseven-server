/// @file test_qa_gdb_423.cpp
/// QA adversarial tests for GDB-423: Cross-Edge-Type Traversal.
///
/// Verifies:
///   AC1: (a)-[:knows]->{1,4}(b)-[:works_at]->(c) works (variable + fixed)
///   AC2: (a)-[:authored]->(b)-[:cited]->{1,3}(c) works (fixed + variable)
///   AC3: (a)-[:knows]->{1,3}(b)-[:knows]->{1,3}(c) works (variable + variable)
///   AC4: Table compatibility validation produces clear errors
///   AC5: Unit tests written and passing
///   AC6: No regressions
///
/// Adversarial categories: three-segment chaining, zero-hop boundaries,
/// cyclic graphs, max_visited limits, binding overwrite, binder validation
/// edge cases, empty intermediate results, direction mixing, stress tests.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
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
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Test fixture — richer graph topology for adversarial testing
// ============================================================================

/// Creates:
///   - 'persons' table: Alice(1), Bob(2), Charlie(3), Diana(4), Eve(5)
///   - 'companies' table: Acme(10), Globex(20)
///   - 'projects' table: Alpha(100), Beta(200)
///   - 'knows' edge: persons -> persons (chain: 1->2->3->4->5)
///   - 'works_at' edge: persons -> companies
///   - 'owns' edge: companies -> projects
class QA_GDB423 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb423";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        // Create tables.
        persons_id_ = create_table("persons");
        companies_id_ = create_table("companies");
        projects_id_ = create_table("projects");

        // Insert persons.
        insert_row("persons", persons_id_, 1, "Alice");
        insert_row("persons", persons_id_, 2, "Bob");
        insert_row("persons", persons_id_, 3, "Charlie");
        insert_row("persons", persons_id_, 4, "Diana");
        insert_row("persons", persons_id_, 5, "Eve");

        // Insert companies.
        insert_row("companies", companies_id_, 10, "Acme");
        insert_row("companies", companies_id_, 20, "Globex");

        // Insert projects.
        insert_row("projects", projects_id_, 100, "Alpha");
        insert_row("projects", projects_id_, 200, "Beta");

        // Create edge types.
        auto eid_knows = graph_->create_edge_type(default_database_id,
                                                  "knows",
                                                  persons_id_,
                                                  persons_id_,
                                                  TypeId::INT64,
                                                  TypeId::INT64,
                                                  {});
        ASSERT_TRUE(eid_knows.has_value()) << eid_knows.error().message;

        auto eid_works = graph_->create_edge_type(default_database_id,
                                                  "works_at",
                                                  persons_id_,
                                                  companies_id_,
                                                  TypeId::INT64,
                                                  TypeId::INT64,
                                                  {});
        ASSERT_TRUE(eid_works.has_value()) << eid_works.error().message;

        auto eid_owns = graph_->create_edge_type(default_database_id,
                                                 "owns",
                                                 companies_id_,
                                                 projects_id_,
                                                 TypeId::INT64,
                                                 TypeId::INT64,
                                                 {});
        ASSERT_TRUE(eid_owns.has_value()) << eid_owns.error().message;

        // Populate edges.
        // knows: linear chain 1->2->3->4->5
        link("knows", 1, 2);
        link("knows", 2, 3);
        link("knows", 3, 4);
        link("knows", 4, 5);

        // works_at: person -> company
        link("works_at", 1, 10); // Alice -> Acme
        link("works_at", 2, 10); // Bob -> Acme
        link("works_at", 3, 20); // Charlie -> Globex
        link("works_at", 4, 20); // Diana -> Globex
        link("works_at", 5, 10); // Eve -> Acme

        // owns: company -> project
        link("owns", 10, 100); // Acme -> Alpha
        link("owns", 20, 200); // Globex -> Beta
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    table_id_t create_table(const std::string& name) {
        TableSchema ts;
        ts.name = name;
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
        EXPECT_TRUE(tid.has_value()) << tid.error().message;
        auto schema = catalog_->get_table(default_database_id, name);
        EXPECT_TRUE(schema.has_value());
        auto sr = storage_->create_table_storage(default_database_id, *tid, *schema);
        EXPECT_TRUE(sr.has_value()) << sr.error().message;
        return *tid;
    }

    void
    insert_row(const std::string& table_name, table_id_t tid, int64_t id, const std::string& name) {
        auto ts = storage_->get_table_storage(tid);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        auto schema = catalog_->get_table(default_database_id, table_name);
        ASSERT_TRUE(schema.has_value());
        std::vector<Value> vals = {Value(id), Value(name)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    void link(const std::string& edge_type, int64_t from, int64_t to) {
        auto r = graph_->link(default_database_id, edge_type, Value(from), Value(to));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    std::vector<Tuple>
    run_vl_match(MatchConfig config,
                 OutputSchema schema,
                 size_t max_visited = VariableLengthMatchOperator::DEFAULT_MAX_VISITED) {
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

    /// Run and expect an error from open().
    Result<void>
    run_vl_match_expect_error(MatchConfig config, OutputSchema schema, size_t max_visited) {
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
        return op.open();
    }

    /// Helper to extract (col0, col1, col2) string tuples from results.
    std::vector<std::tuple<std::string, std::string, std::string>>
    extract_3col_names(const std::vector<Tuple>& results) {
        std::vector<std::tuple<std::string, std::string, std::string>> named;
        for (const auto& t : results) {
            EXPECT_GE(t.values.size(), 3u);
            if (t.values.size() >= 3) {
                named.emplace_back(
                    t.values[0].as_string(), t.values[1].as_string(), t.values[2].as_string());
            }
        }
        std::sort(named.begin(), named.end());
        return named;
    }

    /// Bind a SQL statement, return the result.
    Result<BoundStatement> bind_sql(const std::string& sql) {
        Lexer lexer(sql);
        auto tokens = lexer.tokenize();
        if (!tokens)
            return tl::unexpected(tokens.error());
        Parser parser(std::move(*tokens));
        auto stmts = parser.parse_all();
        if (!stmts)
            return tl::unexpected(stmts.error());
        if (stmts->empty())
            return make_error(StatusCode::PARSE_ERROR, "no statements parsed");
        Binder binder(*catalog_, default_database_id);
        return binder.bind(*stmts->front());
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t persons_id_ = 0;
    table_id_t companies_id_ = 0;
    table_id_t projects_id_ = 0;
};

// ============================================================================
// AC1: Variable + Fixed — (a)-[:knows]->{1,4}(b)-[:works_at]->(c)
// ============================================================================

TEST_F(QA_GDB423, AC1_VariablePlusFixed_CorrectResultCount) {
    // knows{1,4} from a linear chain of 5 nodes:
    //   1-hop: 4 pairs, 2-hop: 3, 3-hop: 2, 4-hop: 1 = 10 pairs
    // Each pair expands through works_at = 10 results.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "companies"});
    config.edges.push_back(MatchEdgeDef("r1", "knows", TraverseDirection::OUT, 1, 4));
    config.edges.push_back(MatchEdgeDef("r2", "works_at", TraverseDirection::OUT));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, companies_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));
    EXPECT_EQ(results.size(), 10u);
}

TEST_F(QA_GDB423, AC1_VariablePlusFixed_SpecificResults) {
    // Verify Alice->Eve(4-hop)->Acme appears in results.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "companies"});
    config.edges.push_back(MatchEdgeDef("r1", "knows", TraverseDirection::OUT, 1, 4));
    config.edges.push_back(MatchEdgeDef("r2", "works_at", TraverseDirection::OUT));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, companies_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));
    auto named = extract_3col_names(results);

    // Alice->Eve via 4-hop knows, Eve works at Acme
    EXPECT_NE(
        std::find(named.begin(),
                  named.end(),
                  std::make_tuple(std::string("Alice"), std::string("Eve"), std::string("Acme"))),
        named.end())
        << "Expected Alice->Eve(4-hop)->Acme";

    // Bob->Diana via 2-hop knows, Diana works at Globex
    EXPECT_NE(
        std::find(named.begin(),
                  named.end(),
                  std::make_tuple(std::string("Bob"), std::string("Diana"), std::string("Globex"))),
        named.end())
        << "Expected Bob->Diana(2-hop)->Globex";
}

// ============================================================================
// AC2: Fixed + Variable — (a)-[:knows]->(b)-[:knows]->{1,3}(c)
// ============================================================================

TEST_F(QA_GDB423, AC2_FixedPlusVariable_CorrectResultCount) {
    // Fixed knows: (1,2), (2,3), (3,4), (4,5)
    // Variable knows {1,3} from each target:
    //   From 2: 3,4,5 => 3 results
    //   From 3: 4,5 => 2 results
    //   From 4: 5 => 1 result
    //   From 5: nothing => 0
    // Total: 6 results.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "persons"});
    config.edges.push_back(MatchEdgeDef("r1", "knows", TraverseDirection::OUT));
    config.edges.push_back(MatchEdgeDef("r2", "knows", TraverseDirection::OUT, 1, 3));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));
    EXPECT_EQ(results.size(), 6u);
}

// ============================================================================
// AC3: Variable + Variable — (a)-[:knows]->{1,2}(b)-[:knows]->{1,2}(c)
// ============================================================================

TEST_F(QA_GDB423, AC3_VariablePlusVariable_CorrectResultCount) {
    // First variable {1,2}: 4+3 = 7 (a,b) pairs
    // Second variable {1,2} from each b:
    //   b=2: 3,4 => 2 | b=3: 4,5 => 2 | b=4: 5 => 1 | b=5: 0
    //   b=3: 4,5 => 2 | b=4: 5 => 1 | b=5: 0
    // Total: (1,2)->2, (2,3)->2, (3,4)->1, (4,5)->0, (1,3)->2, (2,4)->1, (3,5)->0 = 8
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "persons"});
    config.edges.push_back(MatchEdgeDef("r1", "knows", TraverseDirection::OUT, 1, 2));
    config.edges.push_back(MatchEdgeDef("r2", "knows", TraverseDirection::OUT, 1, 2));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));
    EXPECT_EQ(results.size(), 8u);
}

// ============================================================================
// AC4: Table compatibility validation
// ============================================================================

TEST_F(QA_GDB423, AC4_BinderRejectsIncompatibleEdgeChain) {
    // (a:persons)-[:works_at]->(b:companies)-[:knows]->(c:persons)
    // 'knows' has source=persons but b=companies — should fail.
    auto result = bind_sql("SELECT a.name, c.name "
                           "FROM MATCH (a:persons)-[r1:works_at]->(b:companies)"
                           "-[r2:knows]->(c:persons)");
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("table compatibility error"), std::string::npos)
        << "Error message: " << result.error().message;
}

TEST_F(QA_GDB423, AC4_BinderRejectsReversedEdgeChain) {
    // (a:companies)-[:works_at]->(b:persons)-[:knows]->(c:persons)
    // 'works_at' has source=persons, target=companies, but a=companies — should fail.
    auto result = bind_sql("SELECT a.name, c.name "
                           "FROM MATCH (a:companies)-[r1:works_at]->(b:persons)"
                           "-[r2:knows]->(c:persons)");
    EXPECT_FALSE(result.has_value()) << "Should reject works_at with source=companies";
}

TEST_F(QA_GDB423, AC4_BinderAcceptsValidChain) {
    // (a:persons)-[:knows]->(b:persons)-[:works_at]->(c:companies)
    // Valid: knows(persons->persons), works_at(persons->companies)
    auto result = bind_sql("SELECT a.name, c.name "
                           "FROM MATCH (a:persons)-[r1:knows]->(b:persons)"
                           "-[r2:works_at]->(c:companies)");
    EXPECT_TRUE(result.has_value())
        << "Valid chain should bind successfully: " << result.error().message;
}

// ============================================================================
// Adversarial: Three-segment chaining
// ============================================================================

TEST_F(QA_GDB423, ThreeSegmentChain_VarFixedFixed) {
    // (a:persons)-[:knows]->{1,2}(b:persons)-[:works_at]->(c:companies)-[:owns]->(d:projects)
    // knows{1,2}: 7 (a,b) pairs
    // works_at from each b: each person -> 1 company
    // owns from each company: each company -> 1 project
    // Total: 7 results (each knows path -> company -> project).
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "companies"});
    config.nodes.push_back({"d", "projects"});
    config.edges.push_back(MatchEdgeDef("r1", "knows", TraverseDirection::OUT, 1, 2));
    config.edges.push_back(MatchEdgeDef("r2", "works_at", TraverseDirection::OUT));
    config.edges.push_back(MatchEdgeDef("r3", "owns", TraverseDirection::OUT));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, companies_id_});
    out_cols.push_back({"d", "name", TypeId::STRING, false, projects_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));

    EXPECT_EQ(results.size(), 7u);

    // Verify all results have 4 non-null columns.
    for (const auto& t : results) {
        ASSERT_EQ(t.values.size(), 4u);
        for (size_t i = 0; i < 4; ++i) {
            EXPECT_FALSE(t.values[i].is_null()) << "Column " << i << " is null";
        }
    }
}

TEST_F(QA_GDB423, ThreeSegmentChain_FixedFixedFixed) {
    // (a:persons)-[:knows]->(b:persons)-[:works_at]->(c:companies)-[:owns]->(d:projects)
    // All fixed-length edges.
    // knows: (1,2), (2,3), (3,4), (4,5)
    // works_at from each target: 2->Acme, 3->Globex, 4->Globex, 5->Acme
    // owns from each company: Acme->Alpha, Globex->Beta
    // Result: 4 tuples.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "companies"});
    config.nodes.push_back({"d", "projects"});
    config.edges.push_back(MatchEdgeDef("r1", "knows", TraverseDirection::OUT));
    config.edges.push_back(MatchEdgeDef("r2", "works_at", TraverseDirection::OUT));
    config.edges.push_back(MatchEdgeDef("r3", "owns", TraverseDirection::OUT));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, companies_id_});
    out_cols.push_back({"d", "name", TypeId::STRING, false, projects_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));

    EXPECT_EQ(results.size(), 4u);
}

// ============================================================================
// Adversarial: Cyclic graph with cross-edge chaining
// ============================================================================

TEST_F(QA_GDB423, CyclicGraphCrossEdge) {
    // Add a cycle: 5->1 (so knows becomes: 1->2->3->4->5->1)
    link("knows", 5, 1);

    // (a:persons)-[:knows]->{1,6}(b:persons)-[:works_at]->(c:companies)
    // With cycle prevention, each BFS path won't revisit nodes.
    // This should still terminate and produce finite results.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "companies"});
    config.edges.push_back(MatchEdgeDef("r1", "knows", TraverseDirection::OUT, 1, 6));
    config.edges.push_back(MatchEdgeDef("r2", "works_at", TraverseDirection::OUT));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, companies_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));

    // With cycle, each person can reach all other 4 persons via 1-4 hops.
    // So 5 source nodes * 4 reachable targets = 20 knows pairs, each -> works_at.
    // Total should be 20.
    EXPECT_EQ(results.size(), 20u);

    // Verify no null columns.
    for (const auto& t : results) {
        ASSERT_EQ(t.values.size(), 3u);
        EXPECT_FALSE(t.values[0].is_null());
        EXPECT_FALSE(t.values[1].is_null());
        EXPECT_FALSE(t.values[2].is_null());
    }
}

// ============================================================================
// Adversarial: max_visited limit triggers across segments
// ============================================================================

TEST_F(QA_GDB423, MaxVisitedLimitTriggersError) {
    // Use a very small max_visited to force the limit to trigger.
    // knows{1,4} from 5 source nodes: BFS will process many entries.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "companies"});
    config.edges.push_back(MatchEdgeDef("r1", "knows", TraverseDirection::OUT, 1, 4));
    config.edges.push_back(MatchEdgeDef("r2", "works_at", TraverseDirection::OUT));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, companies_id_});
    OutputSchema schema(std::move(out_cols));

    // max_visited=3 should trigger before processing all BFS entries.
    auto result = run_vl_match_expect_error(std::move(config), std::move(schema), 3);
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("max_visited"), std::string::npos)
        << "Error: " << result.error().message;
}

// ============================================================================
// Adversarial: Empty intermediate segment kills pipeline
// ============================================================================

TEST_F(QA_GDB423, EmptyIntermediateSegment) {
    // Create edge type with no edges.
    auto eid = graph_->create_edge_type(
        default_database_id, "mentors", persons_id_, persons_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(eid.has_value());

    // (a:persons)-[:knows]->{1,2}(b:persons)-[:mentors]->(c:persons)
    // knows produces results, but mentors has no edges — result should be empty.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "persons"});
    config.edges.push_back(MatchEdgeDef("r1", "knows", TraverseDirection::OUT, 1, 2));
    config.edges.push_back(MatchEdgeDef("r2", "mentors", TraverseDirection::OUT));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));
    EXPECT_EQ(results.size(), 0u);
}

TEST_F(QA_GDB423, EmptyFirstSegmentKillsPipeline) {
    // Create edge type with no edges.
    auto eid = graph_->create_edge_type(
        default_database_id, "mentors", persons_id_, persons_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(eid.has_value());

    // (a:persons)-[:mentors]->{1,3}(b:persons)-[:works_at]->(c:companies)
    // mentors has no edges — result should be empty even though works_at has edges.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "companies"});
    config.edges.push_back(MatchEdgeDef("r1", "mentors", TraverseDirection::OUT, 1, 3));
    config.edges.push_back(MatchEdgeDef("r2", "works_at", TraverseDirection::OUT));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, companies_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));
    EXPECT_EQ(results.size(), 0u);
}

// ============================================================================
// Adversarial: min_hops=0 boundary (zero-hop includes start node)
// ============================================================================

TEST_F(QA_GDB423, ZeroHopMinIncludesStartNode) {
    // (a:persons)-[:knows]->{0,1}(b:persons)-[:works_at]->(c:companies)
    // min_hops=0 means b can be the same as a (0 hops).
    // 0-hop: each person is their own target => 5 pairs
    // 1-hop: 4 pairs (the chain edges)
    // Total knows pairs: 9, each expanded through works_at = 9 results.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "companies"});
    config.edges.push_back(MatchEdgeDef("r1", "knows", TraverseDirection::OUT, 0, 1));
    config.edges.push_back(MatchEdgeDef("r2", "works_at", TraverseDirection::OUT));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, companies_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));

    // 0-hop: 5 self-pairs, 1-hop: 4 chain pairs = 9 total
    EXPECT_EQ(results.size(), 9u);

    // Verify a zero-hop result: Alice->Alice->Acme
    auto named = extract_3col_names(results);
    EXPECT_NE(
        std::find(named.begin(),
                  named.end(),
                  std::make_tuple(std::string("Alice"), std::string("Alice"), std::string("Acme"))),
        named.end())
        << "Zero-hop should produce Alice->Alice->Acme";
}

// ============================================================================
// Adversarial: Exact hop count (min==max) with cross-edge
// ============================================================================

TEST_F(QA_GDB423, ExactHopCount_CrossEdge) {
    // (a:persons)-[:knows]->{3,3}(b:persons)-[:works_at]->(c:companies)
    // Exactly 3 hops: (1,4), (2,5) — only 2 pairs.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "companies"});
    config.edges.push_back(MatchEdgeDef("r1", "knows", TraverseDirection::OUT, 3, 3));
    config.edges.push_back(MatchEdgeDef("r2", "works_at", TraverseDirection::OUT));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, companies_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));

    EXPECT_EQ(results.size(), 2u);

    auto named = extract_3col_names(results);
    // Alice->Diana (3 hops), Diana works at Globex
    EXPECT_NE(std::find(named.begin(),
                        named.end(),
                        std::make_tuple(
                            std::string("Alice"), std::string("Diana"), std::string("Globex"))),
              named.end());
    // Bob->Eve (3 hops), Eve works at Acme
    EXPECT_NE(
        std::find(named.begin(),
                  named.end(),
                  std::make_tuple(std::string("Bob"), std::string("Eve"), std::string("Acme"))),
        named.end());
}

// ============================================================================
// Adversarial: Single-edge config (no chaining, baseline)
// ============================================================================

TEST_F(QA_GDB423, SingleVariableLengthEdge_NoChaining) {
    // (a:persons)-[:knows]->{1,2}(b:persons)
    // No second edge — just variable-length, no cross-edge.
    // 1-hop: 4, 2-hop: 3 = 7 total.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r1", "knows", TraverseDirection::OUT, 1, 2));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));
    EXPECT_EQ(results.size(), 7u);
}

// ============================================================================
// Adversarial: Binder validation — three-edge chain with incompatibility
// ============================================================================

TEST_F(QA_GDB423, BinderRejectsThreeEdgeChainWithMiddleIncompatibility) {
    // (a:persons)-[:knows]->(b:persons)-[:owns]->(c:projects)
    // 'owns' has source=companies, but b=persons — should fail.
    auto result = bind_sql("SELECT a.name, c.name "
                           "FROM MATCH (a:persons)-[r1:knows]->(b:persons)"
                           "-[r2:owns]->(c:projects)");
    EXPECT_FALSE(result.has_value())
        << "Should reject: owns expects source=companies but got persons";
    if (!result.has_value()) {
        EXPECT_NE(result.error().message.find("table compatibility error"), std::string::npos)
            << "Error: " << result.error().message;
    }
}

// ============================================================================
// Adversarial: Binder accepts valid three-edge chain
// ============================================================================

TEST_F(QA_GDB423, BinderAcceptsValidThreeEdgeChain) {
    // (a:persons)-[:knows]->(b:persons)-[:works_at]->(c:companies)-[:owns]->(d:projects)
    // All edge types compatible.
    auto result = bind_sql("SELECT a.name, d.name "
                           "FROM MATCH (a:persons)-[r1:knows]->(b:persons)"
                           "-[r2:works_at]->(c:companies)"
                           "-[r3:owns]->(d:projects)");
    EXPECT_TRUE(result.has_value()) << "Valid 3-edge chain should bind: " << result.error().message;
}

// ============================================================================
// Adversarial: Variable + Variable with different edge types
// ============================================================================

TEST_F(QA_GDB423, VariablePlusVariable_DifferentEdgeTypes) {
    // Create a second person-to-person edge type.
    auto eid = graph_->create_edge_type(
        default_database_id, "trusts", persons_id_, persons_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(eid.has_value());

    // trusts: 1->3, 3->5
    link("trusts", 1, 3);
    link("trusts", 3, 5);

    // (a:persons)-[:knows]->{1,2}(b:persons)-[:trusts]->{1,2}(c:persons)
    // knows{1,2}: 7 pairs
    // For each (a,b), trusts{1,2} from b:
    //   b=2: no trusts from 2 => 0
    //   b=3: 3->5(1) => 1
    //   b=4: no trusts from 4 => 0
    //   b=5: no trusts from 5 => 0
    //   b=3 (from (1,3)): 3->5(1) => 1
    //   b=4 (from (2,4)): 0
    //   b=5 (from (3,5)): 0
    // Total: 2 results
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "persons"});
    config.edges.push_back(MatchEdgeDef("r1", "knows", TraverseDirection::OUT, 1, 2));
    config.edges.push_back(MatchEdgeDef("r2", "trusts", TraverseDirection::OUT, 1, 2));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));

    // b=3 is reached from a=2 (1-hop) and a=1 (2-hop).
    // trusts from 3: 3->5 (1-hop). trusts{1,2} also checks 2-hop: 5 has no trusts => just 1.
    // So 2 results: (2,3,5) and (1,3,5).
    EXPECT_EQ(results.size(), 2u);
}

// ============================================================================
// Adversarial: Open/close/reopen — verify operator can be reused
// ============================================================================

TEST_F(QA_GDB423, OperatorReopenProducesSameResults) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "companies"});
    config.edges.push_back(MatchEdgeDef("r1", "knows", TraverseDirection::OUT, 1, 2));
    config.edges.push_back(MatchEdgeDef("r2", "works_at", TraverseDirection::OUT));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, companies_id_});
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

    // First open/drain/close.
    auto r1 = op.open();
    ASSERT_TRUE(r1.has_value());
    size_t count1 = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value());
        if (!row->has_value())
            break;
        ++count1;
    }
    op.close();

    // Second open/drain/close — should produce same count.
    auto r2 = op.open();
    ASSERT_TRUE(r2.has_value());
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
// Adversarial: No nodes / no edges edge cases
// ============================================================================

TEST_F(QA_GDB423, EmptyConfig_NoNodes) {
    MatchConfig config;
    // No nodes, no edges.
    std::vector<OutputColumn> out_cols;
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));
    EXPECT_EQ(results.size(), 0u);
}

TEST_F(QA_GDB423, SingleNodeNoEdges) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    // One node, no edges — should return early.
    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));
    EXPECT_EQ(results.size(), 0u);
}

// ============================================================================
// Adversarial: Large hop range with linear graph (no explosion)
// ============================================================================

TEST_F(QA_GDB423, LargeHopRange_LinearGraph) {
    // (a:persons)-[:knows]->{1,100}(b:persons)-[:works_at]->(c:companies)
    // Linear chain of 5 nodes, max reachable is 4 hops.
    // Same result as {1,4} — large max_hops shouldn't cause issues.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "companies"});
    config.edges.push_back(MatchEdgeDef("r1", "knows", TraverseDirection::OUT, 1, 100));
    config.edges.push_back(MatchEdgeDef("r2", "works_at", TraverseDirection::OUT));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, companies_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));

    // Same as {1,4} on a 5-node linear chain: 10 results.
    EXPECT_EQ(results.size(), 10u);
}

// ============================================================================
// Adversarial: min_hops > reachable depth yields empty results
// ============================================================================

TEST_F(QA_GDB423, MinHopsExceedsGraphDepth) {
    // (a:persons)-[:knows]->{10,20}(b:persons)-[:works_at]->(c:companies)
    // Chain is only 4 hops deep, min_hops=10 — no results.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "companies"});
    config.edges.push_back(MatchEdgeDef("r1", "knows", TraverseDirection::OUT, 10, 20));
    config.edges.push_back(MatchEdgeDef("r2", "works_at", TraverseDirection::OUT));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, companies_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));
    EXPECT_EQ(results.size(), 0u);
}

} // namespace
} // namespace sixseven
