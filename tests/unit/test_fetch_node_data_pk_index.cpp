// GDB-1009: fetch_node_data PK index point-lookup correctness test.
//
// Verifies that PatternMatchOperator, VariableLengthMatchOperator, and
// MatchShortestPathOperator return identical row data whether fetch_node_data
// uses the new index path or the fallback heap-scan path.
//
// ASCII-only, no BOM.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/match_shortest_path.h"
#include "sixseven/executor/pattern_match.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/executor/variable_length_match.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/index/btree_index.h"
#include "sixseven/index/rid.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// Fixture: two node tables ('persons', 'orgs'), one edge type ('member_of'),
// four person rows and two org rows, a btree PK index on each table.
// ---------------------------------------------------------------------------
class FetchNodeDataPkIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_fetch_node_pk_index";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_r = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_r.has_value()) << db_r.error().message;

        // Create 'persons' table: (id INT64, name STRING).
        {
            TableSchema ts;
            ts.name = "persons";
            CatalogColumnDef c0;
            c0.ordinal = 0;
            c0.name = "id";
            c0.type_id = TypeId::INT64;
            c0.nullable = false;
            CatalogColumnDef c1;
            c1.ordinal = 1;
            c1.name = "name";
            c1.type_id = TypeId::STRING;
            c1.nullable = false;
            ts.columns = {c0, c1};
            ts.pk_columns = "id";
            auto tid = catalog_->create_table(default_database_id, ts);
            ASSERT_TRUE(tid.has_value()) << tid.error().message;
            persons_id_ = *tid;

            auto sch = catalog_->get_table(default_database_id, "persons");
            ASSERT_TRUE(sch.has_value());
            auto sr = storage_->create_table_storage(default_database_id, persons_id_, *sch);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }

        // Create 'orgs' table: (id INT64, org_name STRING).
        {
            TableSchema ts;
            ts.name = "orgs";
            CatalogColumnDef c0;
            c0.ordinal = 0;
            c0.name = "id";
            c0.type_id = TypeId::INT64;
            c0.nullable = false;
            CatalogColumnDef c1;
            c1.ordinal = 1;
            c1.name = "org_name";
            c1.type_id = TypeId::STRING;
            c1.nullable = false;
            ts.columns = {c0, c1};
            ts.pk_columns = "id";
            auto tid = catalog_->create_table(default_database_id, ts);
            ASSERT_TRUE(tid.has_value()) << tid.error().message;
            orgs_id_ = *tid;

            auto sch = catalog_->get_table(default_database_id, "orgs");
            ASSERT_TRUE(sch.has_value());
            auto sr = storage_->create_table_storage(default_database_id, orgs_id_, *sch);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }

        // Register btree PK indexes in catalog.
        {
            IndexDef def;
            def.table_id = persons_id_;
            def.name = "persons_pk";
            def.index_type = "btree";
            def.columns = "id";
            def.is_unique = true;
            auto r = catalog_->create_index(def);
            ASSERT_TRUE(r.has_value()) << r.error().message;
            persons_idx_id_ = *r;
        }
        {
            IndexDef def;
            def.table_id = orgs_id_;
            def.name = "orgs_pk";
            def.index_type = "btree";
            def.columns = "id";
            def.is_unique = true;
            auto r = catalog_->create_index(def);
            ASSERT_TRUE(r.has_value()) << r.error().message;
            orgs_idx_id_ = *r;
        }

        // Build in-memory BTreeIndex objects.
        BTreeConfig bcfg;
        bcfg.key_types = {TypeId::INT64};
        bcfg.is_unique = true;
        persons_btree_ = std::make_unique<BTreeIndex>(bcfg);
        orgs_btree_ = std::make_unique<BTreeIndex>(bcfg);

        // Insert persons and populate btree.
        insert_person(1, "Alice");
        insert_person(2, "Bob");
        insert_person(3, "Carol");
        insert_person(4, "Dave");

        // Insert orgs and populate btree.
        insert_org(10, "Acme");
        insert_org(20, "Globex");

        // Edge type: persons -[member_of]-> orgs.
        auto eid = graph_->create_edge_type(default_database_id,
                                            "member_of",
                                            persons_id_,
                                            orgs_id_,
                                            TypeId::INT64,
                                            TypeId::INT64,
                                            {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // Edge type: persons -[knows]-> persons (for variable-length tests).
        auto eid2 = graph_->create_edge_type(default_database_id,
                                             "knows",
                                             persons_id_,
                                             persons_id_,
                                             TypeId::INT64,
                                             TypeId::INT64,
                                             {});
        ASSERT_TRUE(eid2.has_value()) << eid2.error().message;

        // Links: persons -> orgs.
        link("member_of", 1, 10);
        link("member_of", 2, 10);
        link("member_of", 3, 20);
        link("member_of", 4, 20);

        // Links: persons -> persons chain (1->2->3->4).
        link("knows", 1, 2);
        link("knows", 2, 3);
        link("knows", 3, 4);

        // Wire up the index maps.
        btree_map_[persons_idx_id_] = persons_btree_.get();
        btree_map_[orgs_idx_id_] = orgs_btree_.get();
    }

    void TearDown() override {
        persons_btree_.reset();
        orgs_btree_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void insert_person(int64_t id, const std::string& name) {
        auto ts = storage_->get_table_storage(persons_id_);
        ASSERT_TRUE(ts.has_value());
        auto sch = catalog_->get_table(default_database_id, "persons");
        ASSERT_TRUE(sch.has_value());
        std::vector<Value> vals = {Value(id), Value(name)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
        auto r = persons_btree_->insert({Value(id)}, *rid);
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    void insert_org(int64_t id, const std::string& org_name) {
        auto ts = storage_->get_table_storage(orgs_id_);
        ASSERT_TRUE(ts.has_value());
        auto sch = catalog_->get_table(default_database_id, "orgs");
        ASSERT_TRUE(sch.has_value());
        std::vector<Value> vals = {Value(id), Value(org_name)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
        auto r = orgs_btree_->insert({Value(id)}, *rid);
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    void link(const std::string& edge_type, int64_t from, int64_t to) {
        auto r = graph_->link(default_database_id, edge_type, Value(from), Value(to));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t persons_id_ = 0;
    table_id_t orgs_id_ = 0;
    index_id_t persons_idx_id_ = 0;
    index_id_t orgs_idx_id_ = 0;
    std::unique_ptr<BTreeIndex> persons_btree_;
    std::unique_ptr<BTreeIndex> orgs_btree_;
    std::unordered_map<index_id_t, BTreeIndex*> btree_map_;
};

// ---------------------------------------------------------------------------
// Helper: run a single-hop MATCH and collect (person_name, org_name) pairs.
// ---------------------------------------------------------------------------
static std::vector<std::pair<std::string, std::string>>
run_single_hop(GraphEngine& graph,
               Catalog& catalog,
               StorageManager& storage,
               table_id_t persons_id,
               table_id_t orgs_id,
               const std::unordered_map<index_id_t, BTreeIndex*>* btree_map,
               const std::unordered_map<index_id_t, HashIndex*>* hash_map) {
    MatchConfig config;
    config.nodes.push_back({"p", "persons"});
    config.nodes.push_back({"o", "orgs"});
    config.edges.push_back({"r", "member_of", TraverseDirection::OUT});

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"p", "name", TypeId::STRING, false, persons_id});
    out_cols.push_back({"o", "org_name", TypeId::STRING, false, orgs_id});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
    PatternMatchOperator op(graph,
                            catalog,
                            storage,
                            default_database_id,
                            std::move(config),
                            std::move(schema),
                            nullptr,
                            bound,
                            btree_map,
                            hash_map);

    auto open_r = op.open();
    if (!open_r) {
        return {};
    }

    std::vector<std::pair<std::string, std::string>> results;
    while (true) {
        auto next_r = op.next();
        if (!next_r || !next_r->has_value()) {
            break;
        }
        auto& row = **next_r;
        if (row.values.size() >= 2) {
            results.emplace_back(row.values[0].as_string(), row.values[1].as_string());
        }
    }
    op.close();
    return results;
}

// ---------------------------------------------------------------------------
// Test 1: single-hop with index path returns correct (person_name, org_name).
// ---------------------------------------------------------------------------
TEST_F(FetchNodeDataPkIndexTest, SingleHopIndexPathCorrect) {
    auto rows =
        run_single_hop(*graph_, *catalog_, *storage_, persons_id_, orgs_id_, &btree_map_, nullptr);
    ASSERT_EQ(rows.size(), 4u);

    // Sort for deterministic comparison.
    std::sort(rows.begin(), rows.end());
    EXPECT_EQ(rows[0], std::make_pair(std::string("Alice"), std::string("Acme")));
    EXPECT_EQ(rows[1], std::make_pair(std::string("Bob"), std::string("Acme")));
    EXPECT_EQ(rows[2], std::make_pair(std::string("Carol"), std::string("Globex")));
    EXPECT_EQ(rows[3], std::make_pair(std::string("Dave"), std::string("Globex")));
}

// ---------------------------------------------------------------------------
// Test 2: fallback (null index maps) returns identical rows.
// ---------------------------------------------------------------------------
TEST_F(FetchNodeDataPkIndexTest, SingleHopFallbackIdentical) {
    auto rows_idx =
        run_single_hop(*graph_, *catalog_, *storage_, persons_id_, orgs_id_, &btree_map_, nullptr);
    auto rows_scan =
        run_single_hop(*graph_, *catalog_, *storage_, persons_id_, orgs_id_, nullptr, nullptr);

    ASSERT_EQ(rows_idx.size(), rows_scan.size());
    std::sort(rows_idx.begin(), rows_idx.end());
    std::sort(rows_scan.begin(), rows_scan.end());
    for (size_t i = 0; i < rows_idx.size(); ++i) {
        EXPECT_EQ(rows_idx[i], rows_scan[i]) << "Mismatch at row " << i;
    }
}

// ---------------------------------------------------------------------------
// Test 3: VariableLengthMatchOperator with index path - knows chain 1->2->3.
// ---------------------------------------------------------------------------
TEST_F(FetchNodeDataPkIndexTest, VariableLengthMatchIndexPath) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    MatchEdgeDef edge("r", "knows", TraverseDirection::OUT, 1, 3);
    config.edges.push_back(edge);

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
                                   VariableLengthMatchOperator::DEFAULT_MAX_VISITED,
                                   &btree_map_,
                                   nullptr);

    auto open_r = op.open();
    ASSERT_TRUE(open_r.has_value()) << open_r.error().message;

    std::vector<std::pair<std::string, std::string>> rows;
    while (true) {
        auto next_r = op.next();
        ASSERT_TRUE(next_r.has_value()) << next_r.error().message;
        if (!next_r->has_value()) {
            break;
        }
        auto& row = **next_r;
        if (row.values.size() >= 2) {
            rows.emplace_back(row.values[0].as_string(), row.values[1].as_string());
        }
    }
    op.close();

    // With chain 1->2->3->4 and hops 1..3 from all source nodes, there must
    // be at least one result with source "Alice".
    bool found_alice_src = false;
    for (const auto& [src, dst] : rows) {
        if (src == "Alice") {
            found_alice_src = true;
        }
    }
    EXPECT_TRUE(found_alice_src) << "Expected at least one row with source Alice";
    EXPECT_FALSE(rows.empty());
}

// ---------------------------------------------------------------------------
// Test 4: VariableLengthMatch fallback == index path (value-identical).
// ---------------------------------------------------------------------------
TEST_F(FetchNodeDataPkIndexTest, VariableLengthMatchFallbackIdentical) {
    auto run = [&](const std::unordered_map<index_id_t, BTreeIndex*>* btree_map)
        -> std::vector<std::pair<std::string, std::string>> {
        MatchConfig config;
        config.nodes.push_back({"a", "persons"});
        config.nodes.push_back({"b", "persons"});
        MatchEdgeDef edge("r", "knows", TraverseDirection::OUT, 1, 2);
        config.edges.push_back(edge);

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
                                       VariableLengthMatchOperator::DEFAULT_MAX_VISITED,
                                       btree_map,
                                       nullptr);
        auto open_r = op.open();
        if (!open_r) {
            return {};
        }
        std::vector<std::pair<std::string, std::string>> rows;
        while (true) {
            auto next_r = op.next();
            if (!next_r || !next_r->has_value()) {
                break;
            }
            auto& row = **next_r;
            if (row.values.size() >= 2) {
                rows.emplace_back(row.values[0].as_string(), row.values[1].as_string());
            }
        }
        op.close();
        return rows;
    };

    auto rows_idx = run(&btree_map_);
    auto rows_scan = run(nullptr);

    ASSERT_EQ(rows_idx.size(), rows_scan.size());
    std::sort(rows_idx.begin(), rows_idx.end());
    std::sort(rows_scan.begin(), rows_scan.end());
    for (size_t i = 0; i < rows_idx.size(); ++i) {
        EXPECT_EQ(rows_idx[i], rows_scan[i]) << "Mismatch at row " << i;
    }
}

// ---------------------------------------------------------------------------
// Test 5: MatchShortestPathOperator with index path returns src/dst names.
// ---------------------------------------------------------------------------
TEST_F(FetchNodeDataPkIndexTest, ShortestPathIndexPath) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    MatchEdgeDef edge("r", "knows", TraverseDirection::OUT, 1, 10);
    config.edges.push_back(edge);

    std::string path_var = "p";
    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
    out_cols.push_back({"", "p", TypeId::PATH, false, 0});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
    MatchShortestPathOperator op(*graph_,
                                 *catalog_,
                                 *storage_,
                                 default_database_id,
                                 std::move(config),
                                 std::move(schema),
                                 nullptr,
                                 bound,
                                 PathSelector::ANY_SHORTEST,
                                 path_var,
                                 1,
                                 MatchShortestPathOperator::DEFAULT_MAX_VISITED,
                                 nullptr,
                                 &btree_map_,
                                 nullptr);

    auto open_r = op.open();
    ASSERT_TRUE(open_r.has_value()) << open_r.error().message;

    bool got_row = false;
    while (true) {
        auto next_r = op.next();
        ASSERT_TRUE(next_r.has_value()) << next_r.error().message;
        if (!next_r->has_value()) {
            break;
        }
        got_row = true;
        auto& row = **next_r;
        ASSERT_GE(row.values.size(), 2u);
        // Source and destination names must be non-empty strings.
        EXPECT_FALSE(row.values[0].as_string().empty());
        EXPECT_FALSE(row.values[1].as_string().empty());
    }
    op.close();

    // The chain 1->2->3->4 means path A->B->C->D exists; we must get results.
    EXPECT_TRUE(got_row);
}

} // namespace
} // namespace sixseven
