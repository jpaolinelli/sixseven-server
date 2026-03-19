#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/match_shortest_path.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <vector>

using namespace sixseven;

// ===========================================================================
// Parser tests for WEIGHT clause
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

bool parse_fails(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return true;
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    return !stmts.has_value();
}

} // namespace

TEST(WeightParser, WeightClauseParsed) {
    auto stmt = parse_one("MATCH p = ANY SHORTEST (a:cities)-[r:road]->{1,20}(b:cities) "
                          "WEIGHT r.distance RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::ANY_SHORTEST);
    EXPECT_EQ(m->path_variable, "p");
    ASSERT_NE(m->weight_expr, nullptr);

    auto* col = dynamic_cast<ColumnRefExpr*>(m->weight_expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->table, "r");
    EXPECT_EQ(col->column, "distance");
}

TEST(WeightParser, WeightClauseWithAllShortest) {
    auto stmt = parse_one("MATCH p = ALL SHORTEST (a:cities)-[r:road]->{1,20}(b:cities) "
                          "WEIGHT r.distance RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::ALL_SHORTEST);
    ASSERT_NE(m->weight_expr, nullptr);
}

TEST(WeightParser, WeightClauseWithShortestK) {
    auto stmt = parse_one("MATCH p = SHORTEST 3 (a:cities)-[r:road]->{1,20}(b:cities) "
                          "WEIGHT r.distance RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::SHORTEST_K);
    EXPECT_EQ(m->shortest_k, 3);
    ASSERT_NE(m->weight_expr, nullptr);
}

TEST(WeightParser, WeightWithoutPathSelectorFails) {
    // WEIGHT without a path selector should be a parse error.
    EXPECT_TRUE(parse_fails("MATCH (a:cities)-[r:road]->(b:cities) WEIGHT r.distance RETURN a, b"));
}

TEST(WeightParser, NoWeightClauseIsNull) {
    auto stmt =
        parse_one("MATCH p = ANY SHORTEST (a:cities)-[r:road]->{1,10}(b:cities) RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->weight_expr, nullptr);
}

TEST(WeightParser, SelectFromMatchWeight) {
    auto stmt = parse_one("SELECT a.name, b.name, path_cost(p) AS total_distance "
                          "FROM MATCH p = ANY SHORTEST (a:cities)-[r:road]->{1,20}(b:cities) "
                          "WEIGHT r.distance "
                          "WHERE a.name = 'NYC' AND b.name = 'LA'");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    ASSERT_NE(sel->from[0].match_source, nullptr);

    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::ANY_SHORTEST);
    ASSERT_NE(m->weight_expr, nullptr);
}

// ===========================================================================
// Executor tests for weighted shortest path (Dijkstra)
// ===========================================================================

/// Test fixture with a weighted graph.
///
/// Graph topology (edge weights in parentheses):
///   1 --(10)--> 2 --(20)--> 5
///   1 --(5)---> 3 --(3)---> 4 --(2)--> 5
///
///   Direct path 1→2→5: cost = 30
///   Cheaper path 1→3→4→5: cost = 10
class WeightedShortestPathTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_weighted_sp";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        // Create 'cities' table with id column.
        {
            TableSchema ts;
            ts.name = "cities";
            CatalogColumnDef pk_col;
            pk_col.ordinal = 0;
            pk_col.name = "id";
            pk_col.type_id = TypeId::INT64;
            pk_col.nullable = false;
            ts.columns.push_back(pk_col);
            ts.pk_columns = "id";
            auto tid = catalog_->create_table(default_database_id, std::move(ts));
            ASSERT_TRUE(tid.has_value()) << tid.error().message;
            cities_id_ = *tid;

            auto schema = catalog_->get_table(default_database_id, "cities");
            ASSERT_TRUE(schema.has_value());
            auto sr = storage_->create_table_storage(default_database_id, cities_id_, *schema);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }

        // Insert cities: 1, 2, 3, 4, 5.
        for (int64_t id : {1, 2, 3, 4, 5}) {
            insert_city(id);
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
        // Path 1→2→5 costs 30 (10+20)
        // Path 1→3→4→5 costs 10 (5+3+2)
        link(1, 2, 10.0);
        link(2, 5, 20.0);
        link(1, 3, 5.0);
        link(3, 4, 3.0);
        link(4, 5, 2.0);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void link(int64_t from, int64_t to, double distance) {
        auto r =
            graph_->link(default_database_id, "road", Value(from), Value(to), {Value(distance)});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    void insert_city(int64_t id) {
        auto ts = storage_->get_table_storage(cities_id_);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        auto schema = catalog_->get_table(default_database_id, "cities");
        ASSERT_TRUE(schema.has_value());

        std::vector<Value> vals = {Value(id)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    /// Build a weight expression: ColumnRefExpr for "r.distance".
    std::unique_ptr<ColumnRefExpr> make_weight_expr() {
        auto expr = std::make_unique<ColumnRefExpr>();
        expr->table = "r";
        expr->column = "distance";
        return expr;
    }

    /// Run a weighted shortest-path MATCH operator and collect output tuples.
    std::vector<Tuple> run_weighted_match(MatchConfig config,
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

    /// Build default config for cities→cities via road.
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

    static constexpr database_id_t default_database_id = 1;
    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t cities_id_ = 0;
};

TEST_F(WeightedShortestPathTest, DijkstraFindsWeightedShortestPath) {
    // The weighted shortest path from 1 to 5 should be 1→3→4→5 (cost 10),
    // NOT 1→2→5 (cost 30).
    auto weight = make_weight_expr();
    auto results =
        run_weighted_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST, weight.get());

    std::vector<Tuple> from_1_to_5;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 5) {
            from_1_to_5.push_back(std::move(t));
        }
    }

    ASSERT_EQ(from_1_to_5.size(), 1u);
    const auto& path = from_1_to_5[0].values[2].as_path();
    // Should be 1→3→4→5 (3 hops, cost 10).
    EXPECT_EQ(path.length(), 3);
    ASSERT_EQ(path.steps.size(), 4u);
    EXPECT_EQ(path.steps[0].node_pk, 1);
    EXPECT_EQ(path.steps[1].node_pk, 3);
    EXPECT_EQ(path.steps[2].node_pk, 4);
    EXPECT_EQ(path.steps[3].node_pk, 5);
}

TEST_F(WeightedShortestPathTest, PathCostReturnsCorrectTotal) {
    // path_cost(p) should return 10.0 for the 1→3→4→5 path.
    auto weight = make_weight_expr();
    auto results =
        run_weighted_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST, weight.get());

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
    // City 1 to city that doesn't exist in edges — should return no path.
    // Insert an isolated city.
    insert_city(99);

    auto weight = make_weight_expr();
    auto results =
        run_weighted_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST, weight.get());

    std::vector<Tuple> from_1_to_99;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 99) {
            from_1_to_99.push_back(std::move(t));
        }
    }

    EXPECT_TRUE(from_1_to_99.empty());
}

TEST_F(WeightedShortestPathTest, SameNodeReturnsZeroCostPath) {
    auto weight = make_weight_expr();
    auto results =
        run_weighted_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST, weight.get());

    std::vector<Tuple> from_1_to_1;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 1) {
            from_1_to_1.push_back(std::move(t));
        }
    }

    ASSERT_EQ(from_1_to_1.size(), 1u);
    const auto& path = from_1_to_1[0].values[2].as_path();
    EXPECT_EQ(path.length(), 0);
    EXPECT_DOUBLE_EQ(path.total_weight, 0.0);
}

// ===========================================================================
// GDB-559: ALL_SHORTEST filters non-shortest paths at destination
// ===========================================================================

TEST_F(WeightedShortestPathTest, AllShortestOnlyReturnsCheapestPaths) {
    // Graph has two paths from 1 to 5:
    //   1→2→5 costs 30
    //   1→3→4→5 costs 10
    // ALL_SHORTEST should only return the cost-10 path.
    auto weight = make_weight_expr();
    auto results =
        run_weighted_match(make_config(), make_schema(), PathSelector::ALL_SHORTEST, weight.get());

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
    // Same graph: SHORTEST 5 from 1 to 5 should not include cost-30 path
    // if cost-10 is the true shortest.
    auto weight = make_weight_expr();
    auto results = run_weighted_match(
        make_config(), make_schema(), PathSelector::SHORTEST_K, weight.get(), "p", 5);

    std::vector<Tuple> from_1_to_5;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 5) {
            from_1_to_5.push_back(std::move(t));
        }
    }

    // All returned paths to node 5 should have cost 10 (the minimum).
    for (const auto& t : from_1_to_5) {
        EXPECT_DOUBLE_EQ(t.values[2].as_path().total_weight, 10.0);
    }
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
class LateCheeperArrivalTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_late_cheaper";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        {
            TableSchema ts;
            ts.name = "nodes";
            CatalogColumnDef pk_col;
            pk_col.ordinal = 0;
            pk_col.name = "id";
            pk_col.type_id = TypeId::INT64;
            pk_col.nullable = false;
            ts.columns.push_back(pk_col);
            ts.pk_columns = "id";
            auto tid = catalog_->create_table(default_database_id, std::move(ts));
            ASSERT_TRUE(tid.has_value()) << tid.error().message;
            nodes_id_ = *tid;

            auto schema = catalog_->get_table(default_database_id, "nodes");
            ASSERT_TRUE(schema.has_value());
            auto sr = storage_->create_table_storage(default_database_id, nodes_id_, *schema);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }

        for (int64_t id : {1, 2, 3, 4}) {
            insert_node(id);
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
        link(1, 2, 1.0);
        link(2, 4, 100.0); // total: 101

        // Cheaper path reaches destination later (node 3 popped at cost 50).
        link(1, 3, 50.0);
        link(3, 4, 1.0); // total: 51
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void link(int64_t from, int64_t to, double distance) {
        auto r =
            graph_->link(default_database_id, "road", Value(from), Value(to), {Value(distance)});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    void insert_node(int64_t id) {
        auto ts = storage_->get_table_storage(nodes_id_);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        auto schema = catalog_->get_table(default_database_id, "nodes");
        ASSERT_TRUE(schema.has_value());
        std::vector<Value> vals = {Value(id)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    std::unique_ptr<ColumnRefExpr> make_weight_expr() {
        auto expr = std::make_unique<ColumnRefExpr>();
        expr->table = "r";
        expr->column = "distance";
        return expr;
    }

    std::vector<Tuple> run_match(PathSelector selector, int32_t k = 0) {
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
                                     "p",
                                     k,
                                     MatchShortestPathOperator::DEFAULT_MAX_VISITED,
                                     weight.get());
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
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t nodes_id_ = 0;
};

TEST_F(LateCheeperArrivalTest, AllShortestPurgesExpensivePathOnCheaperArrival) {
    // The expensive path (cost 101 via node 2) reaches node 4 first,
    // then the cheaper path (cost 51 via node 3) arrives later.
    // Only the cost-51 path should be returned.
    auto results = run_match(PathSelector::ALL_SHORTEST);

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

TEST_F(LateCheeperArrivalTest, AnyShortestReturnsCheapestNotFirstArrival) {
    // GDB-560: ANY_SHORTEST must return the cheapest path, not the first
    // to arrive. The cost-101 path via node 2 arrives first but is not shortest.
    auto results = run_match(PathSelector::ANY_SHORTEST);

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

TEST_F(LateCheeperArrivalTest, ShortestKDoesNotEarlyReturnWithExpensivePaths) {
    // GDB-561: SHORTEST 1 must not early-return with the first (expensive)
    // arrival before discovering cheaper paths.
    auto results = run_match(PathSelector::SHORTEST_K, /*k=*/1);

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

class NegativeWeightTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_neg_weight";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        {
            TableSchema ts;
            ts.name = "nodes";
            CatalogColumnDef pk_col;
            pk_col.ordinal = 0;
            pk_col.name = "id";
            pk_col.type_id = TypeId::INT64;
            pk_col.nullable = false;
            ts.columns.push_back(pk_col);
            ts.pk_columns = "id";
            auto tid = catalog_->create_table(default_database_id, std::move(ts));
            ASSERT_TRUE(tid.has_value()) << tid.error().message;
            nodes_id_ = *tid;

            auto schema = catalog_->get_table(default_database_id, "nodes");
            ASSERT_TRUE(schema.has_value());
            auto sr = storage_->create_table_storage(default_database_id, nodes_id_, *schema);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }

        for (int64_t id : {1, 2}) {
            auto ts = storage_->get_table_storage(nodes_id_);
            ASSERT_TRUE(ts.has_value());
            auto schema = catalog_->get_table(default_database_id, "nodes");
            ASSERT_TRUE(schema.has_value());
            std::vector<Value> vals = {Value(id)};
            auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
            ASSERT_TRUE(data.has_value());
            auto rid = (*ts)->heap->insert_tuple(*data);
            ASSERT_TRUE(rid.has_value());
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

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    static constexpr database_id_t default_database_id = 1;
    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t nodes_id_ = 0;
};

TEST_F(NegativeWeightTest, NegativeWeightProducesError) {
    auto weight_expr = std::make_unique<ColumnRefExpr>();
    weight_expr->table = "r";
    weight_expr->column = "cost";

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
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("non-negative"), std::string::npos);
}

// ===========================================================================
// Path struct total_weight field tests
// ===========================================================================

TEST(PathWeight, DefaultTotalWeightIsZero) {
    Path p;
    EXPECT_DOUBLE_EQ(p.total_weight, 0.0);
}

TEST(PathWeight, TotalWeightPreserved) {
    Path p;
    p.steps.push_back({1, 100});
    p.steps.push_back({2, -1});
    p.total_weight = 42.5;

    Value v(std::move(p));
    EXPECT_EQ(v.type_id(), TypeId::PATH);
    EXPECT_DOUBLE_EQ(v.as_path().total_weight, 42.5);
}

// ===========================================================================
// WEIGHT keyword is a valid identifier
// ===========================================================================

TEST(WeightParser, WeightAsColumnName) {
    auto stmt = parse_one("SELECT weight FROM cities");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->items.size(), 1u);
}
