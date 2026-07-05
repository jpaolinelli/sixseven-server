// Unit tests for MatchShortestPathOperator (src/executor/match_shortest_path.cpp).
//
// Covers both the unweighted BFS path-selection codepath
// (find_shortest_paths) and the weighted Dijkstra codepath
// (find_weighted_shortest_paths), including ANY/ALL/SHORTEST-K selectors,
// plan_node_name() per selector, min-hops/same-node edge cases,
// max_visited error propagation, and negative-weight rejection.
//
// Relocated from tests/unit/test_path_selectors.cpp and
// tests/unit/test_weighted_shortest_path.cpp as part of GDB-1215 to align
// the source->test naming convention. Parser tests for the WEIGHT/SHORTEST
// clauses, Path struct field tests, and the legacy ShortestPathOperator
// backward-compat tests remain in their original files since they are not
// specific to MatchShortestPathOperator.
#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/match_shortest_path.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

// ===========================================================================
// Unweighted BFS: MatchShortestPathTest fixture
// (relocated from test_path_selectors.cpp)
// ===========================================================================

/// Test fixture with a graph and storage for shortest path testing.
///
/// Graph topology:
///   1 → 2 → 3 → 6
///   1 → 4 → 5 → 6
///   (Two paths from 1 to 6, both length 3)
class MatchShortestPathTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_shortest_path_match";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        // Create 'persons' table with id column.
        {
            TableSchema ts;
            ts.name = "persons";
            CatalogColumnDef pk_col;
            pk_col.ordinal = 0;
            pk_col.name = "id";
            pk_col.type_id = TypeId::INT64;
            pk_col.nullable = false;
            ts.columns.push_back(pk_col);
            ts.pk_columns = "id";
            auto tid = catalog_->create_table(default_database_id, std::move(ts));
            ASSERT_TRUE(tid.has_value()) << tid.error().message;
            persons_id_ = *tid;

            auto schema = catalog_->get_table(default_database_id, "persons");
            ASSERT_TRUE(schema.has_value());
            auto sr = storage_->create_table_storage(default_database_id, persons_id_, *schema);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }

        // Insert persons: 1, 2, 3, 4, 5, 6.
        for (int64_t id : {1, 2, 3, 4, 5, 6}) {
            insert_person(id);
        }

        auto eid = graph_->create_edge_type(default_database_id,
                                            "knows",
                                            persons_id_,
                                            persons_id_,
                                            TypeId::INT64,
                                            TypeId::INT64,
                                            {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // Two equal-length paths: 1→2→3→6 and 1→4→5→6.
        link(1, 2);
        link(2, 3);
        link(3, 6);
        link(1, 4);
        link(4, 5);
        link(5, 6);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void link(int64_t from, int64_t to) {
        auto r = graph_->link(default_database_id, "knows", Value(from), Value(to));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    void insert_person(int64_t id) {
        auto ts = storage_->get_table_storage(persons_id_);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        auto schema = catalog_->get_table(default_database_id, "persons");
        ASSERT_TRUE(schema.has_value());

        std::vector<Value> vals = {Value(id)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    /// Run a shortest-path MATCH operator and collect output tuples.
    std::vector<Tuple> run_shortest_match(MatchConfig config,
                                          OutputSchema schema,
                                          PathSelector selector,
                                          const std::string& path_var = "p",
                                          int32_t k = 0) {
        BoundStatement bound;
        MatchShortestPathOperator op(*graph_,
                                     *catalog_,
                                     *storage_,
                                     default_database_id,
                                     std::move(config),
                                     std::move(schema),
                                     nullptr,
                                     bound,
                                     selector,
                                     path_var,
                                     k);
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

    /// Build default config for persons→persons via knows.
    MatchConfig make_config() {
        MatchConfig config;
        config.nodes.push_back({"a", "persons"});
        config.nodes.push_back({"b", "persons"});
        config.edges.push_back(MatchEdgeDef("", "knows", TraverseDirection::OUT, 1, 10));
        return config;
    }

    /// Build output schema with source id, target id, and path.
    OutputSchema make_schema() {
        std::vector<OutputColumn> cols;
        cols.push_back({"a", "id", TypeId::INT64, false, persons_id_});
        cols.push_back({"b", "id", TypeId::INT64, false, persons_id_});
        cols.push_back({"p", "path", TypeId::PATH, false, 0});
        return OutputSchema(std::move(cols));
    }

    static constexpr database_id_t default_database_id = 1;
    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t persons_id_ = 0;
};

TEST_F(MatchShortestPathTest, AnyShortestFindsOnePath) {
    // ANY SHORTEST from 1 to 6 should return exactly one path of length 3.
    // There are two equal-length paths (1→2→3→6 and 1→4→5→6), but ANY returns just one.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);

    // Filter to only paths from 1 to 6.
    std::vector<Tuple> from_1_to_6;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 6) {
            from_1_to_6.push_back(std::move(t));
        }
    }

    ASSERT_EQ(from_1_to_6.size(), 1u);
    EXPECT_EQ(from_1_to_6[0].values[2].as_path().length(), 3);
}

TEST_F(MatchShortestPathTest, AllShortestFindsBothPaths) {
    // ALL SHORTEST from 1 to 6 should return both paths of length 3.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ALL_SHORTEST);

    std::vector<Tuple> from_1_to_6;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 6) {
            from_1_to_6.push_back(std::move(t));
        }
    }

    ASSERT_EQ(from_1_to_6.size(), 2u);
    EXPECT_EQ(from_1_to_6[0].values[2].as_path().length(), 3);
    EXPECT_EQ(from_1_to_6[1].values[2].as_path().length(), 3);
}

TEST_F(MatchShortestPathTest, ShortestKReturnsKPaths) {
    // SHORTEST 1 from 1 to 6 should return only 1 path.
    auto results =
        run_shortest_match(make_config(), make_schema(), PathSelector::SHORTEST_K, "p", 1);

    std::vector<Tuple> from_1_to_6;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 6) {
            from_1_to_6.push_back(std::move(t));
        }
    }

    ASSERT_EQ(from_1_to_6.size(), 1u);
    EXPECT_EQ(from_1_to_6[0].values[2].as_path().length(), 3);
}

// GDB-858: Under {1,10} (min_hops=1) the acyclic 'knows' graph produces no
// cycle, so a same-node query must return ZERO paths.
TEST_F(MatchShortestPathTest, SameNodeUnderMinHopsOneReturnsNoPath) {
    // make_config() uses MatchEdgeDef("", "knows", OUT, 1, 10) → min_hops=1.
    // The knows graph (1→2→3→6, 1→4→5→6) is acyclic, so no path 1→…→1 exists.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);

    std::vector<Tuple> from_1_to_1;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 1) {
            from_1_to_1.push_back(std::move(t));
        }
    }

    EXPECT_TRUE(from_1_to_1.empty())
        << "Under {1,10} a same-node query on an acyclic graph must return no path (GDB-858)";
}

TEST_F(MatchShortestPathTest, PathContainsCorrectNodes) {
    // ANY SHORTEST from 1 to 3: shortest path is 1→2→3 (length 2).
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);

    std::vector<Tuple> from_1_to_3;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 3) {
            from_1_to_3.push_back(std::move(t));
        }
    }

    ASSERT_EQ(from_1_to_3.size(), 1u);
    const auto& path = from_1_to_3[0].values[2].as_path();
    EXPECT_EQ(path.length(), 2);
    ASSERT_EQ(path.steps.size(), 3u);
    EXPECT_EQ(path.steps[0].node_pk, 1);
    EXPECT_EQ(path.steps[1].node_pk, 2);
    EXPECT_EQ(path.steps[2].node_pk, 3);
}

// Each PathSelector must map to a distinct plan_node_name() string.  This
// test constructs a real MatchShortestPathOperator for each selector and
// asserts the exact string produced by the production switch — a mutation
// that swaps two cases (e.g. ANY_SHORTEST <-> ALL_SHORTEST) will fail here.
TEST_F(MatchShortestPathTest, PlanNodeNamePerSelector) {
    BoundStatement bound;

    auto make_op = [&](PathSelector sel, int32_t k = 0) {
        return MatchShortestPathOperator(*graph_,
                                         *catalog_,
                                         *storage_,
                                         default_database_id,
                                         make_config(),
                                         make_schema(),
                                         nullptr,
                                         bound,
                                         sel,
                                         "p",
                                         k);
    };

    {
        auto op = make_op(PathSelector::NONE);
        EXPECT_EQ(op.plan_node_name(), "Shortest Path Match");
    }
    {
        auto op = make_op(PathSelector::ANY_SHORTEST);
        EXPECT_EQ(op.plan_node_name(), "Any Shortest Path Match");
    }
    {
        auto op = make_op(PathSelector::ALL_SHORTEST);
        EXPECT_EQ(op.plan_node_name(), "All Shortest Path Match");
    }
    {
        auto op = make_op(PathSelector::SHORTEST_K, 3);
        EXPECT_EQ(op.plan_node_name(), "Shortest 3 Path Match");
    }
}

// ===========================================================================
// Weighted Dijkstra: shared base fixture for weighted graph executor tests
// (relocated from test_weighted_shortest_path.cpp)
// ===========================================================================

/// Base fixture that sets up Catalog/StorageManager/GraphEngine backed by a
/// temporary directory.  Derived fixtures call create_node_table(), insert_node(),
/// link(), and make_weight_expr() to build the topology they need.
class WeightedGraphTestBase : public ::testing::Test {
protected:
    void SetUp() override {
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    /// Create a node table with a single INT64 'id' primary key column.
    /// Returns the assigned table_id.
    table_id_t create_node_table(const std::string& name) {
        TableSchema ts;
        ts.name = name;
        CatalogColumnDef pk_col;
        pk_col.ordinal = 0;
        pk_col.name = "id";
        pk_col.type_id = TypeId::INT64;
        pk_col.nullable = false;
        ts.columns.push_back(pk_col);
        ts.pk_columns = "id";
        auto tid = catalog_->create_table(default_database_id, std::move(ts));
        EXPECT_TRUE(tid.has_value()) << tid.error().message;

        auto schema = catalog_->get_table(default_database_id, name);
        EXPECT_TRUE(schema.has_value());
        auto sr = storage_->create_table_storage(default_database_id, *tid, *schema);
        EXPECT_TRUE(sr.has_value()) << sr.error().message;
        return *tid;
    }

    /// Insert a single node row with the given id into the table identified by table_id.
    void insert_node(table_id_t table_id, int64_t id) {
        auto ts = storage_->get_table_storage(table_id);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        auto schema = catalog_->get_table_by_id(table_id);
        ASSERT_TRUE(schema.has_value());
        std::vector<Value> vals = {Value(id)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    /// Insert an edge between two nodes.
    void link(const std::string& edge_type, int64_t from, int64_t to, double weight) {
        auto r =
            graph_->link(default_database_id, edge_type, Value(from), Value(to), {Value(weight)});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    /// Build a weight expression: ColumnRefExpr for "<alias>.<column>".
    std::unique_ptr<ColumnRefExpr> make_weight_expr(const std::string& alias = "r",
                                                    const std::string& column = "distance") {
        auto expr = std::make_unique<ColumnRefExpr>();
        expr->table = alias;
        expr->column = column;
        return expr;
    }

    /// Run a weighted shortest-path MATCH operator and collect output tuples.
    std::vector<Tuple> run_match(MatchConfig config,
                                 OutputSchema schema,
                                 PathSelector selector,
                                 const Expr* weight_expr,
                                 const std::string& path_var = "p",
                                 int32_t k = 0) {
        BoundStatement bound;
        MatchShortestPathOperator op(*graph_,
                                     *catalog_,
                                     *storage_,
                                     default_database_id,
                                     std::move(config),
                                     std::move(schema),
                                     nullptr,
                                     bound,
                                     selector,
                                     path_var,
                                     k,
                                     MatchShortestPathOperator::DEFAULT_MAX_VISITED,
                                     weight_expr);
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

    static constexpr database_id_t default_database_id = 1;
    DiskManager dm_;
    std::filesystem::path data_dir_{std::filesystem::temp_directory_path() /
                                    "sixseven_test_weighted_graph_base"};
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
};

// ===========================================================================
// Executor tests for weighted shortest path (Dijkstra)
// ===========================================================================

/// Test fixture with a weighted graph.
///
/// Graph topology (edge weights in parentheses):
///   1 --(10)--> 2 --(20)--> 5
///   1 --(5)---> 3 --(3)---> 4 --(2)--> 5
///
///   Direct path 1->2->5: cost = 30
///   Cheaper path 1->3->4->5: cost = 10
class WeightedShortestPathTest : public WeightedGraphTestBase {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_weighted_sp";
        WeightedGraphTestBase::SetUp();

        cities_id_ = create_node_table("cities");

        // Insert cities: 1, 2, 3, 4, 5.
        for (int64_t id : {1, 2, 3, 4, 5}) {
            insert_node(cities_id_, id);
        }

        // Create 'road' edge type with a 'distance' property.
        ColumnDef dist_col{"distance", TypeId::FLOAT64};
        auto eid = graph_->create_edge_type(default_database_id,
                                            "road",
                                            cities_id_,
                                            cities_id_,
                                            TypeId::INT64,
                                            TypeId::INT64,
                                            {dist_col});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // Build weighted graph:
        // Path 1->2->5 costs 30 (10+20)
        // Path 1->3->4->5 costs 10 (5+3+2)
        link("road", 1, 2, 10.0);
        link("road", 2, 5, 20.0);
        link("road", 1, 3, 5.0);
        link("road", 3, 4, 3.0);
        link("road", 4, 5, 2.0);
    }

    /// Build default config for cities->cities via road.
    MatchConfig make_config() {
        MatchConfig config;
        config.nodes.push_back({"a", "cities"});
        config.nodes.push_back({"b", "cities"});
        config.edges.push_back(MatchEdgeDef("r", "road", TraverseDirection::OUT, 1, 20));
        return config;
    }

    /// Build output schema with source id, target id, and path.
    OutputSchema make_schema() {
        std::vector<OutputColumn> cols;
        cols.push_back({"a", "id", TypeId::INT64, false, cities_id_});
        cols.push_back({"b", "id", TypeId::INT64, false, cities_id_});
        cols.push_back({"p", "path", TypeId::PATH, false, 0});
        return OutputSchema(std::move(cols));
    }

    table_id_t cities_id_ = 0;
};

TEST_F(WeightedShortestPathTest, DijkstraFindsWeightedShortestPath) {
    // The weighted shortest path from 1 to 5 should be 1->3->4->5 (cost 10),
    // NOT 1->2->5 (cost 30).
    auto weight = make_weight_expr();
    auto results =
        run_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST, weight.get());

    std::vector<Tuple> from_1_to_5;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 5) {
            from_1_to_5.push_back(std::move(t));
        }
    }

    ASSERT_EQ(from_1_to_5.size(), 1u);
    const auto& path = from_1_to_5[0].values[2].as_path();
    // Should be 1->3->4->5 (3 hops, cost 10).
    EXPECT_EQ(path.length(), 3);
    ASSERT_EQ(path.steps.size(), 4u);
    EXPECT_EQ(path.steps[0].node_pk, 1);
    EXPECT_EQ(path.steps[1].node_pk, 3);
    EXPECT_EQ(path.steps[2].node_pk, 4);
    EXPECT_EQ(path.steps[3].node_pk, 5);
}

TEST_F(WeightedShortestPathTest, PathCostReturnsCorrectTotal) {
    // path_cost(p) should return 10.0 for the 1->3->4->5 path.
    auto weight = make_weight_expr();
    auto results =
        run_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST, weight.get());

    std::vector<Tuple> from_1_to_5;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 5) {
            from_1_to_5.push_back(std::move(t));
        }
    }

    ASSERT_EQ(from_1_to_5.size(), 1u);
    const auto& path = from_1_to_5[0].values[2].as_path();
    EXPECT_DOUBLE_EQ(path.total_weight, 10.0);
}

TEST_F(WeightedShortestPathTest, DisconnectedGraphReturnsEmpty) {
    // City 1 to city that doesn't exist in edges -- should return no path.
    // Insert an isolated city.
    insert_node(cities_id_, 99);

    auto weight = make_weight_expr();
    auto results =
        run_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST, weight.get());

    std::vector<Tuple> from_1_to_99;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 99) {
            from_1_to_99.push_back(std::move(t));
        }
    }

    EXPECT_TRUE(from_1_to_99.empty());
}

// GDB-858: Under {1,20} (min_hops=1) a same-node query must return NO results
// because the acyclic graph has no real cycle from 1 back to 1.  The old
// "SameNodeReturnsZeroCostPath" test enshrined the bug; corrected semantics here.
TEST_F(WeightedShortestPathTest, SameNodeUnderMinHopsOneReturnsNoPath) {
    // make_config() uses MatchEdgeDef("r", "road", OUT, 1, 20) → min_hops=1.
    // The road graph (1->2->5, 1->3->4->5) has no cycle back to 1.
    auto weight = make_weight_expr();
    auto results =
        run_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST, weight.get());

    std::vector<Tuple> from_1_to_1;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 1) {
            from_1_to_1.push_back(std::move(t));
        }
    }

    // With min_hops=1 and no cycle in the graph, 1->1 should produce ZERO paths.
    EXPECT_TRUE(from_1_to_1.empty())
        << "Under {1,20} a same-node query on an acyclic graph must return no path (GDB-858)";
}

// GDB-858: Under {0,20} (min_hops=0) the trivial 0-hop self-path IS correct.
TEST_F(WeightedShortestPathTest, SameNodeUnderMinHopsZeroReturnsSelfPath) {
    // Build a config with min_hops=0 (i.e. {0,20}).
    MatchConfig config;
    config.nodes.push_back({"a", "cities"});
    config.nodes.push_back({"b", "cities"});
    config.edges.push_back(MatchEdgeDef("r", "road", TraverseDirection::OUT, 0, 20));

    auto weight = make_weight_expr();
    auto results = run_match(config, make_schema(), PathSelector::ANY_SHORTEST, weight.get());

    std::vector<Tuple> from_1_to_1;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 1) {
            from_1_to_1.push_back(std::move(t));
        }
    }

    // With min_hops=0, the 0-hop self-path is a valid match.
    ASSERT_EQ(from_1_to_1.size(), 1u)
        << "Under {0,20} a same-node query must return the 0-hop self-path (GDB-858)";
    const auto& path = from_1_to_1[0].values[2].as_path();
    EXPECT_EQ(path.length(), 0);
    EXPECT_DOUBLE_EQ(path.total_weight, 0.0);
}

// ===========================================================================
// GDB-559: ALL_SHORTEST filters non-shortest paths at destination
// ===========================================================================

TEST_F(WeightedShortestPathTest, AllShortestOnlyReturnsCheapestPaths) {
    // Graph has two paths from 1 to 5:
    //   1->2->5 costs 30
    //   1->3->4->5 costs 10
    // ALL_SHORTEST should only return the cost-10 path.
    auto weight = make_weight_expr();
    auto results =
        run_match(make_config(), make_schema(), PathSelector::ALL_SHORTEST, weight.get());

    std::vector<Tuple> from_1_to_5;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 5) {
            from_1_to_5.push_back(std::move(t));
        }
    }

    ASSERT_EQ(from_1_to_5.size(), 1u)
        << "ALL_SHORTEST should only return paths with the minimum cost";
    const auto& path = from_1_to_5[0].values[2].as_path();
    EXPECT_DOUBLE_EQ(path.total_weight, 10.0);
    ASSERT_EQ(path.steps.size(), 4u);
    EXPECT_EQ(path.steps[0].node_pk, 1);
    EXPECT_EQ(path.steps[1].node_pk, 3);
    EXPECT_EQ(path.steps[2].node_pk, 4);
    EXPECT_EQ(path.steps[3].node_pk, 5);
}

TEST_F(WeightedShortestPathTest, ShortestKOnlyReturnsCheapestPaths) {
    // Graph has 2 paths from 1->5: cost 10 (via 3->4) and cost 30 (via 2).
    // SHORTEST 5 should return both, sorted by cost.
    auto weight = make_weight_expr();
    auto results =
        run_match(make_config(), make_schema(), PathSelector::SHORTEST_K, weight.get(), "p", 5);

    std::vector<Tuple> from_1_to_5;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 5) {
            from_1_to_5.push_back(std::move(t));
        }
    }

    // SHORTEST K returns up to K cheapest paths (at different costs).
    ASSERT_EQ(from_1_to_5.size(), 2u);
    std::sort(from_1_to_5.begin(), from_1_to_5.end(), [](const Tuple& a, const Tuple& b) {
        return a.values[2].as_path().total_weight < b.values[2].as_path().total_weight;
    });
    EXPECT_DOUBLE_EQ(from_1_to_5[0].values[2].as_path().total_weight, 10.0);
    EXPECT_DOUBLE_EQ(from_1_to_5[1].values[2].as_path().total_weight, 30.0);
}

// ===========================================================================
// GDB-559: Late cheaper arrival purges previously-added expensive paths
// ===========================================================================

/// Fixture where expensive path reaches destination before cheap path.
///
/// Graph topology:
///   1 --(1)--> 2 --(100)--> 4   (cost 101, expensive but 2 is popped early)
///   1 --(50)--> 3 --(1)--> 4    (cost 51, cheaper but 3 is popped later)
///
/// Dijkstra pops node 2 (entry cost 1) before node 3 (entry cost 50),
/// so the cost-101 path reaches node 4 first.
class LateCheaperArrivalTest : public WeightedGraphTestBase {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_late_cheaper";
        WeightedGraphTestBase::SetUp();

        nodes_id_ = create_node_table("nodes");

        for (int64_t id : {1, 2, 3, 4}) {
            insert_node(nodes_id_, id);
        }

        ColumnDef dist_col{"distance", TypeId::FLOAT64};
        auto eid = graph_->create_edge_type(default_database_id,
                                            "road",
                                            nodes_id_,
                                            nodes_id_,
                                            TypeId::INT64,
                                            TypeId::INT64,
                                            {dist_col});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // Expensive path reaches destination first (node 2 popped at cost 1).
        link("road", 1, 2, 1.0);
        link("road", 2, 4, 100.0); // total: 101

        // Cheaper path reaches destination later (node 3 popped at cost 50).
        link("road", 1, 3, 50.0);
        link("road", 3, 4, 1.0); // total: 51
    }

    std::vector<Tuple> run_match_nodes(PathSelector selector, int32_t k = 0) {
        MatchConfig config;
        config.nodes.push_back({"a", "nodes"});
        config.nodes.push_back({"b", "nodes"});
        config.edges.push_back(MatchEdgeDef("r", "road", TraverseDirection::OUT, 1, 20));

        std::vector<OutputColumn> cols;
        cols.push_back({"a", "id", TypeId::INT64, false, nodes_id_});
        cols.push_back({"b", "id", TypeId::INT64, false, nodes_id_});
        cols.push_back({"p", "path", TypeId::PATH, false, 0});
        OutputSchema schema(std::move(cols));

        auto weight = make_weight_expr();
        return run_match(std::move(config), std::move(schema), selector, weight.get(), "p", k);
    }

    table_id_t nodes_id_ = 0;
};

TEST_F(LateCheaperArrivalTest, AllShortestPurgesExpensivePathOnCheaperArrival) {
    // The expensive path (cost 101 via node 2) reaches node 4 first,
    // then the cheaper path (cost 51 via node 3) arrives later.
    // Only the cost-51 path should be returned.
    auto results = run_match_nodes(PathSelector::ALL_SHORTEST);

    std::vector<Tuple> from_1_to_4;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 4) {
            from_1_to_4.push_back(std::move(t));
        }
    }

    ASSERT_EQ(from_1_to_4.size(), 1u)
        << "Should purge the cost-101 path when the cost-51 path arrives";
    const auto& path = from_1_to_4[0].values[2].as_path();
    EXPECT_DOUBLE_EQ(path.total_weight, 51.0);
    ASSERT_EQ(path.steps.size(), 3u);
    EXPECT_EQ(path.steps[0].node_pk, 1);
    EXPECT_EQ(path.steps[1].node_pk, 3);
    EXPECT_EQ(path.steps[2].node_pk, 4);
}

TEST_F(LateCheaperArrivalTest, AnyShortestReturnsCheapestNotFirstArrival) {
    // GDB-560: ANY_SHORTEST must return the cheapest path, not the first
    // to arrive. The cost-101 path via node 2 arrives first but is not shortest.
    auto results = run_match_nodes(PathSelector::ANY_SHORTEST);

    std::vector<Tuple> from_1_to_4;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 4) {
            from_1_to_4.push_back(std::move(t));
        }
    }

    ASSERT_EQ(from_1_to_4.size(), 1u) << "ANY_SHORTEST should return exactly one path";
    const auto& path = from_1_to_4[0].values[2].as_path();
    EXPECT_DOUBLE_EQ(path.total_weight, 51.0)
        << "ANY_SHORTEST should return the cheapest path (51), not the first arrival (101)";
}

TEST_F(LateCheaperArrivalTest, ShortestKDoesNotEarlyReturnWithExpensivePaths) {
    // GDB-561: SHORTEST 1 must not early-return with the first (expensive)
    // arrival before discovering cheaper paths.
    auto results = run_match_nodes(PathSelector::SHORTEST_K, /*k=*/1);

    std::vector<Tuple> from_1_to_4;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 4) {
            from_1_to_4.push_back(std::move(t));
        }
    }

    ASSERT_EQ(from_1_to_4.size(), 1u) << "SHORTEST 1 should return exactly one path";
    const auto& path = from_1_to_4[0].values[2].as_path();
    EXPECT_DOUBLE_EQ(path.total_weight, 51.0)
        << "SHORTEST 1 should return the cheapest path (51), not the first arrival (101)";
}

// ===========================================================================
// Negative weight rejection
// ===========================================================================

class NegativeWeightTest : public WeightedGraphTestBase {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_neg_weight";
        WeightedGraphTestBase::SetUp();

        nodes_id_ = create_node_table("nodes");

        for (int64_t id : {1, 2}) {
            insert_node(nodes_id_, id);
        }

        ColumnDef weight_col{"cost", TypeId::FLOAT64};
        auto eid = graph_->create_edge_type(default_database_id,
                                            "edge",
                                            nodes_id_,
                                            nodes_id_,
                                            TypeId::INT64,
                                            TypeId::INT64,
                                            {weight_col});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // Link with negative weight.
        auto r = graph_->link(
            default_database_id, "edge", Value(int64_t{1}), Value(int64_t{2}), {Value(-5.0)});
        ASSERT_TRUE(r.has_value());
    }

    table_id_t nodes_id_ = 0;
};

TEST_F(NegativeWeightTest, NegativeWeightProducesError) {
    auto weight_expr = make_weight_expr("r", "cost");

    MatchConfig config;
    config.nodes.push_back({"a", "nodes"});
    config.nodes.push_back({"b", "nodes"});
    config.edges.push_back(MatchEdgeDef("r", "edge", TraverseDirection::OUT, 1, 10));

    std::vector<OutputColumn> cols;
    cols.push_back({"a", "id", TypeId::INT64, false, nodes_id_});
    cols.push_back({"b", "id", TypeId::INT64, false, nodes_id_});
    cols.push_back({"p", "path", TypeId::PATH, false, 0});
    OutputSchema schema(std::move(cols));

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
                                 "p",
                                 0,
                                 MatchShortestPathOperator::DEFAULT_MAX_VISITED,
                                 weight_expr.get());
    auto result = op.open();
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("non-negative"), std::string::npos);
}

// ===========================================================================
// GDB-1214: max_visited exceeded -> explicit error (unified across graph ops)
// ===========================================================================

// Unweighted BFS path (find_shortest_paths, match_shortest_path.cpp) must
// return an explicit INVALID_ARGUMENT error -- not a silent partial-paths
// return -- when max_visited is exceeded.
TEST_F(WeightedShortestPathTest, UnweightedBfsExceedingMaxVisitedReturnsError) {
    MatchConfig config = make_config();

    BoundStatement bound;
    MatchShortestPathOperator op(*graph_,
                                 *catalog_,
                                 *storage_,
                                 default_database_id,
                                 std::move(config),
                                 make_schema(),
                                 nullptr,
                                 bound,
                                 PathSelector::ANY_SHORTEST,
                                 "p",
                                 0,
                                 /*max_visited=*/1,
                                 /*weight_expr=*/nullptr);
    auto result = op.open();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(result.error().message.find("exceeded max_visited limit (1)"), std::string::npos)
        << result.error().message;
}

// Weighted Dijkstra path (find_weighted_shortest_paths) must also return the
// same explicit error when max_visited is exceeded.
TEST_F(WeightedShortestPathTest, DijkstraExceedingMaxVisitedReturnsError) {
    auto weight = make_weight_expr();
    MatchConfig config = make_config();

    BoundStatement bound;
    MatchShortestPathOperator op(*graph_,
                                 *catalog_,
                                 *storage_,
                                 default_database_id,
                                 std::move(config),
                                 make_schema(),
                                 nullptr,
                                 bound,
                                 PathSelector::ANY_SHORTEST,
                                 "p",
                                 0,
                                 /*max_visited=*/1,
                                 weight.get());
    auto result = op.open();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(result.error().message.find("exceeded max_visited limit (1)"), std::string::npos)
        << result.error().message;
}

// Regression guard: a generous max_visited budget must not affect
// correctness for the Dijkstra path -- same assertion as
// DijkstraFindsWeightedShortestPath, but explicitly re-checked here in the
// context of the GDB-1214 unification to confirm no under-limit regression.
TEST_F(WeightedShortestPathTest, DijkstraUnderMaxVisitedLimitReturnsCompleteResults) {
    auto weight = make_weight_expr();
    auto results =
        run_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST, weight.get());

    std::vector<Tuple> from_1_to_5;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 5) {
            from_1_to_5.push_back(std::move(t));
        }
    }

    ASSERT_EQ(from_1_to_5.size(), 1u);
    const auto& path = from_1_to_5[0].values[2].as_path();
    EXPECT_EQ(path.length(), 3);
    EXPECT_DOUBLE_EQ(path.total_weight, 10.0);
}
