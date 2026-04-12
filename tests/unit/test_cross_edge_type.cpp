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
#include <filesystem>
#include <string>
#include <tuple>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

/// Test fixture for cross-edge-type traversal patterns (GDB-423).
///
/// Creates:
///   - 'persons' table: Alice(1), Bob(2), Charlie(3), Diana(4), Eve(5)
///   - 'companies' table: Acme(10), Globex(20)
///   - 'knows' edge: persons -> persons
///     Chain: 1->2->3->4->5
///   - 'works_at' edge: persons -> companies
///     Alice->Acme, Bob->Acme, Charlie->Globex, Diana->Globex, Eve->Acme
class CrossEdgeTypeTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_cross_edge";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
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

        // Create 'companies' table.
        {
            TableSchema ts;
            ts.name = "companies";
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
            companies_id_ = *tid;

            auto schema = catalog_->get_table(default_database_id, "companies");
            ASSERT_TRUE(schema.has_value());
            auto sr = storage_->create_table_storage(default_database_id, companies_id_, *schema);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }

        // Insert persons.
        insert_person(1, "Alice");
        insert_person(2, "Bob");
        insert_person(3, "Charlie");
        insert_person(4, "Diana");
        insert_person(5, "Eve");

        // Insert companies.
        insert_company(10, "Acme");
        insert_company(20, "Globex");

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
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void link(const std::string& edge_type, int64_t from, int64_t to) {
        auto r = graph_->link(default_database_id, edge_type, Value(from), Value(to));
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

    void insert_company(int64_t id, const std::string& name) {
        auto ts = storage_->get_table_storage(companies_id_);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        auto schema = catalog_->get_table(default_database_id, "companies");
        ASSERT_TRUE(schema.has_value());

        std::vector<Value> vals = {Value(id), Value(name)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    /// Run a variable-length match and collect (col0, col1, ...) string tuples.
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
    table_id_t companies_id_ = 0;
};

// ============================================================================
// Variable + Fixed: (a)-[:knows]->{1,3}(b)-[:works_at]->(c)
// ============================================================================

TEST_F(CrossEdgeTypeTest, VariablePlusFixed) {
    // (a:persons)-[r1:knows]->{1,3}(b:persons)-[r2:works_at]->(c:companies)
    // RETURN a.name, b.name, c.name
    //
    // Variable-length knows {1,3} produces (source, target) pairs:
    //   1-hop: (1,2), (2,3), (3,4), (4,5)
    //   2-hop: (1,3), (2,4), (3,5)
    //   3-hop: (1,4), (2,5)
    // Then fixed-length works_at expands target to company:
    //   (1,2)->Acme, (2,3)->Globex, (3,4)->Globex, (4,5)->Acme
    //   (1,3)->Globex, (2,4)->Globex, (3,5)->Acme
    //   (1,4)->Globex, (2,5)->Acme
    // Total: 9 results.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "companies"});
    config.edges.push_back(MatchEdgeDef("r1", "knows", TraverseDirection::OUT, 1, 3));
    config.edges.push_back(MatchEdgeDef("r2", "works_at", TraverseDirection::OUT));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, companies_id_});
    OutputSchema schema(std::move(out_cols));

    auto results = run_vl_match(std::move(config), std::move(schema));

    ASSERT_EQ(results.size(), 9u);

    // Verify specific results: Alice knows Bob (1 hop), Bob works_at Acme.
    std::vector<std::tuple<std::string, std::string, std::string>> named;
    for (const auto& t : results) {
        ASSERT_GE(t.values.size(), 3u);
        named.emplace_back(
            t.values[0].as_string(), t.values[1].as_string(), t.values[2].as_string());
    }
    std::sort(named.begin(), named.end());

    // Alice->Bob->Acme (1 hop)
    EXPECT_NE(
        std::find(named.begin(),
                  named.end(),
                  std::make_tuple(std::string("Alice"), std::string("Bob"), std::string("Acme"))),
        named.end());

    // Alice->Charlie->Globex (2 hops)
    EXPECT_NE(std::find(named.begin(),
                        named.end(),
                        std::make_tuple(
                            std::string("Alice"), std::string("Charlie"), std::string("Globex"))),
              named.end());

    // Alice->Diana->Globex (3 hops)
    EXPECT_NE(std::find(named.begin(),
                        named.end(),
                        std::make_tuple(
                            std::string("Alice"), std::string("Diana"), std::string("Globex"))),
              named.end());
}

// ============================================================================
// Fixed + Variable: (a)-[:knows]->(b)-[:knows]->{1,3}(c)
// ============================================================================

TEST_F(CrossEdgeTypeTest, FixedPlusVariable) {
    // (a:persons)-[r1:knows]->(b:persons)-[r2:knows]->{1,3}(c:persons)
    // RETURN a.name, b.name, c.name
    //
    // Fixed knows gives: (1,2), (2,3), (3,4), (4,5)
    // Then variable knows {1,3} from each target:
    //   From 2: 2->3(1), 2->4(2), 2->5(3) => (1,2,3), (1,2,4), (1,2,5)
    //   From 3: 3->4(1), 3->5(2) => (2,3,4), (2,3,5)
    //   From 4: 4->5(1) => (3,4,5)
    //   From 5: nothing
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

    ASSERT_EQ(results.size(), 6u);

    std::vector<std::tuple<std::string, std::string, std::string>> named;
    for (const auto& t : results) {
        ASSERT_GE(t.values.size(), 3u);
        named.emplace_back(
            t.values[0].as_string(), t.values[1].as_string(), t.values[2].as_string());
    }
    std::sort(named.begin(), named.end());

    // Alice->Bob->Charlie (fixed+1hop)
    EXPECT_NE(std::find(named.begin(),
                        named.end(),
                        std::make_tuple(
                            std::string("Alice"), std::string("Bob"), std::string("Charlie"))),
              named.end());

    // Alice->Bob->Eve (fixed+3hops)
    EXPECT_NE(
        std::find(named.begin(),
                  named.end(),
                  std::make_tuple(std::string("Alice"), std::string("Bob"), std::string("Eve"))),
        named.end());

    // Charlie->Diana->Eve (fixed+1hop)
    EXPECT_NE(std::find(named.begin(),
                        named.end(),
                        std::make_tuple(
                            std::string("Charlie"), std::string("Diana"), std::string("Eve"))),
              named.end());
}

// ============================================================================
// Variable + Variable: (a)-[:knows]->{1,2}(b)-[:knows]->{1,2}(c)
// ============================================================================

TEST_F(CrossEdgeTypeTest, VariablePlusVariable) {
    // (a:persons)-[r1:knows]->{1,2}(b:persons)-[r2:knows]->{1,2}(c:persons)
    // RETURN a.name, b.name, c.name
    //
    // First variable knows {1,2}:
    //   1-hop: (1,2), (2,3), (3,4), (4,5)
    //   2-hop: (1,3), (2,4), (3,5)
    // Then for each (a,b), variable knows {1,2} from b:
    //   (1,2): 2->3(1), 2->4(2) => (1,2,3), (1,2,4)
    //   (2,3): 3->4(1), 3->5(2) => (2,3,4), (2,3,5)
    //   (3,4): 4->5(1) => (3,4,5)
    //   (4,5): nothing
    //   (1,3): 3->4(1), 3->5(2) => (1,3,4), (1,3,5)
    //   (2,4): 4->5(1) => (2,4,5)
    //   (3,5): nothing
    // Total: 8 results.
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

    ASSERT_EQ(results.size(), 8u);

    std::vector<std::tuple<std::string, std::string, std::string>> named;
    for (const auto& t : results) {
        ASSERT_GE(t.values.size(), 3u);
        named.emplace_back(
            t.values[0].as_string(), t.values[1].as_string(), t.values[2].as_string());
    }

    // Spot-check: (Alice, Bob, Charlie) — both 1-hop
    EXPECT_NE(std::find(named.begin(),
                        named.end(),
                        std::make_tuple(
                            std::string("Alice"), std::string("Bob"), std::string("Charlie"))),
              named.end());

    // (Alice, Charlie, Diana) — 2-hop + 1-hop
    EXPECT_NE(std::find(named.begin(),
                        named.end(),
                        std::make_tuple(
                            std::string("Alice"), std::string("Charlie"), std::string("Diana"))),
              named.end());
}

// ============================================================================
// Table compatibility validation — error cases
// ============================================================================

TEST_F(CrossEdgeTypeTest, TableCompatibilityError) {
    // Try to chain: (a:persons)-[:works_at]->(b:companies)-[:knows]->(c:persons)
    // This should fail validation because 'knows' expects source=persons but
    // b is companies (target of works_at).
    //
    // We test via the binder's public bind() method with a parsed SQL statement.
    std::string sql = "SELECT a.name, c.name "
                      "FROM MATCH (a:persons)-[r1:works_at]->(b:companies)"
                      "-[r2:knows]->(c:persons)";

    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value()) << tokens.error().message;

    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    ASSERT_TRUE(stmts.has_value()) << stmts.error().message;
    ASSERT_EQ(stmts->size(), 1u);

    Binder binder(*catalog_, default_database_id);
    auto result = binder.bind(*stmts->front());

    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("table compatibility error"), std::string::npos);
}

// ============================================================================
// Variable+Fixed with cross edge types and content verification
// ============================================================================

TEST_F(CrossEdgeTypeTest, VariablePlusFixedCrossType) {
    // (a:persons)-[:knows]->{1,4}(b:persons)-[:works_at]->(c:companies)
    // WHERE c.name = 'Acme'
    //
    // This is the canonical query from the ticket description.
    // Without WHERE (we test the pattern itself), all persons reachable
    // via knows{1,4} from each person get expanded to their company.
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

    // 1-hop knows: (1,2), (2,3), (3,4), (4,5) — 4 pairs
    // 2-hop: (1,3), (2,4), (3,5) — 3 pairs
    // 3-hop: (1,4), (2,5) — 2 pairs
    // 4-hop: (1,5) — 1 pair
    // Total knows pairs: 10, each expanded through works_at = 10 results.
    EXPECT_EQ(results.size(), 10u);

    // Verify all results have 3 non-null columns.
    for (const auto& t : results) {
        ASSERT_EQ(t.values.size(), 3u);
        EXPECT_FALSE(t.values[0].is_null());
        EXPECT_FALSE(t.values[1].is_null());
        EXPECT_FALSE(t.values[2].is_null());
    }
}

// ============================================================================
// Empty result when no paths connect segments
// ============================================================================

TEST_F(CrossEdgeTypeTest, EmptyResultNoConnection) {
    // Create a new edge type with no edges to test empty chaining.
    auto eid = graph_->create_edge_type(
        default_database_id, "mentors", persons_id_, persons_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(eid.has_value());

    // (a:persons)-[:mentors]->{1,3}(b:persons)-[:works_at]->(c:companies)
    // mentors has no edges, so result should be empty.
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

} // namespace
} // namespace sixseven
