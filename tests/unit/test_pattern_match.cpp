#include "giodb/catalog/catalog.h"
#include "giodb/common/result.h"
#include "giodb/common/value.h"
#include "giodb/executor/pattern_match.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/graph/graph_engine.h"
#include "giodb/planner/binder.h"
#include "giodb/storage/disk_manager.h"
#include "giodb/table/tuple.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace giodb {
namespace {

/// Test fixture for MATCH pattern matching tests.
///
/// Creates tables 'people' and 'companies', an edge type 'works_at',
/// and populates data for testing single-hop and multi-hop patterns.
class PatternMatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "giodb_test_pattern_match";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        // Create 'people' table.
        {
            TableSchema ts;
            ts.name = "people";
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
            people_id_ = *tid;

            auto schema = catalog_->get_table(default_database_id, "people");
            ASSERT_TRUE(schema.has_value());
            auto sr = storage_->create_table_storage(default_database_id, people_id_, *schema);
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

        // Insert people.
        insert_person(1, "Alice");
        insert_person(2, "Bob");
        insert_person(3, "Charlie");

        // Insert companies.
        insert_company(10, "Acme");
        insert_company(20, "Globex");

        // Create edge type: people -[works_at]-> companies.
        auto eid = graph_->create_edge_type(
            "works_at", people_id_, companies_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // Create edge type: people -[knows]-> people.
        auto eid2 = graph_->create_edge_type(
            "knows", people_id_, people_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid2.has_value()) << eid2.error().message;

        // Link people to companies.
        link("works_at", 1, 10);
        link("works_at", 2, 10);
        link("works_at", 3, 20);

        // Link people who know each other.
        link("knows", 1, 2);
        link("knows", 2, 3);
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
        auto ts = storage_->get_table_storage(people_id_);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        auto schema = catalog_->get_table(default_database_id, "people");
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

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t people_id_ = 0;
    table_id_t companies_id_ = 0;
};

TEST_F(PatternMatchTest, SingleHopPattern) {
    // MATCH (p:people)-[r:works_at]->(c:companies) RETURN p.name, c.name
    MatchConfig config;
    config.nodes.push_back({"p", "people"});
    config.nodes.push_back({"c", "companies"});
    config.edges.push_back({"r", "works_at", TraverseDirection::OUT});

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"p", "name", TypeId::STRING, false, people_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, companies_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
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

    std::vector<std::pair<std::string, std::string>> results;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        auto& vals = row->value().values;
        ASSERT_EQ(vals.size(), 2);
        results.emplace_back(vals[0].as_string(), vals[1].as_string());
    }
    op.close();

    // Alice→Acme, Bob→Acme, Charlie→Globex
    ASSERT_EQ(results.size(), 3);

    std::sort(results.begin(), results.end());
    EXPECT_EQ(results[0].first, "Alice");
    EXPECT_EQ(results[0].second, "Acme");
    EXPECT_EQ(results[1].first, "Bob");
    EXPECT_EQ(results[1].second, "Acme");
    EXPECT_EQ(results[2].first, "Charlie");
    EXPECT_EQ(results[2].second, "Globex");
}

TEST_F(PatternMatchTest, MultiHopPattern) {
    // MATCH (a:people)-[r1:knows]->(b:people)-[r2:works_at]->(c:companies)
    // RETURN a.name, b.name, c.name
    MatchConfig config;
    config.nodes.push_back({"a", "people"});
    config.nodes.push_back({"b", "people"});
    config.nodes.push_back({"c", "companies"});
    config.edges.push_back({"r1", "knows", TraverseDirection::OUT});
    config.edges.push_back({"r2", "works_at", TraverseDirection::OUT});

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, people_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, people_id_});
    out_cols.push_back({"c", "name", TypeId::STRING, false, companies_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
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

    std::vector<std::tuple<std::string, std::string, std::string>> results;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        auto& vals = row->value().values;
        ASSERT_EQ(vals.size(), 3);
        results.emplace_back(vals[0].as_string(), vals[1].as_string(), vals[2].as_string());
    }
    op.close();

    // Alice→Bob→Acme, Bob→Charlie→Globex
    ASSERT_EQ(results.size(), 2);

    std::sort(results.begin(), results.end());
    EXPECT_EQ(std::get<0>(results[0]), "Alice");
    EXPECT_EQ(std::get<1>(results[0]), "Bob");
    EXPECT_EQ(std::get<2>(results[0]), "Acme");
    EXPECT_EQ(std::get<0>(results[1]), "Bob");
    EXPECT_EQ(std::get<1>(results[1]), "Charlie");
    EXPECT_EQ(std::get<2>(results[1]), "Globex");
}

TEST_F(PatternMatchTest, EmptyEdgeTable) {
    // Create a new edge type with no edges.
    auto eid = graph_->create_edge_type(
        "empty_edge", people_id_, companies_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(eid.has_value());

    MatchConfig config;
    config.nodes.push_back({"p", "people"});
    config.nodes.push_back({"c", "companies"});
    config.edges.push_back({"r", "empty_edge", TraverseDirection::OUT});

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"p", "name", TypeId::STRING, false, people_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            std::move(schema),
                            nullptr,
                            bound);

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value());

    auto row = op.next();
    ASSERT_TRUE(row.has_value());
    EXPECT_FALSE(row->has_value());

    op.close();
}

TEST_F(PatternMatchTest, NoEdgesInPattern) {
    MatchConfig config;
    config.nodes.push_back({"p", "people"});
    // No edges.

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"p", "name", TypeId::STRING, false, people_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            std::move(schema),
                            nullptr,
                            bound);

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value());

    auto row = op.next();
    ASSERT_TRUE(row.has_value());
    // No edges means empty result.
    EXPECT_FALSE(row->has_value());

    op.close();
}

TEST_F(PatternMatchTest, BothDirectionSingleHop) {
    // MATCH (p:people)-[r:knows BOTH]->(q:people) RETURN p.name, q.name
    // With BOTH direction, Alice knows Bob (forward) AND Bob knows Alice (reverse).
    MatchConfig config;
    config.nodes.push_back({"p", "people"});
    config.nodes.push_back({"q", "people"});
    config.edges.push_back({"r", "knows", TraverseDirection::BOTH});

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"p", "name", TypeId::STRING, false, people_id_});
    out_cols.push_back({"q", "name", TypeId::STRING, false, people_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
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

    std::vector<std::pair<std::string, std::string>> results;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        auto& vals = row->value().values;
        ASSERT_EQ(vals.size(), 2);
        results.emplace_back(vals[0].as_string(), vals[1].as_string());
    }
    op.close();

    // Edges: Alice→Bob, Bob→Charlie (both forward and reverse).
    // Alice: forward→Bob, reverse from nobody (nobody knows Alice via 'knows')
    // Bob: forward→Charlie, reverse←Alice
    // Charlie: forward→nobody, reverse←Bob
    // So BOTH gives: (Alice,Bob), (Bob,Alice), (Bob,Charlie), (Charlie,Bob)
    ASSERT_EQ(results.size(), 4);

    std::sort(results.begin(), results.end());
    EXPECT_EQ(results[0].first, "Alice");
    EXPECT_EQ(results[0].second, "Bob");
    EXPECT_EQ(results[1].first, "Bob");
    EXPECT_EQ(results[1].second, "Alice");
    EXPECT_EQ(results[2].first, "Bob");
    EXPECT_EQ(results[2].second, "Charlie");
    EXPECT_EQ(results[3].first, "Charlie");
    EXPECT_EQ(results[3].second, "Bob");
}

} // namespace
} // namespace giodb
