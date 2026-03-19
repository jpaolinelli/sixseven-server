/// @file test_qa_gdb_559.cpp
/// QA adversarial tests for GDB-559: ALL_SHORTEST weighted Dijkstra returns
/// non-shortest paths to destination.
///
/// Verifies the fix: destination arrivals are filtered by cost, and expensive
/// paths are purged when a cheaper arrival is found.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/match_shortest_path.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <set>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Fixture
// ============================================================================

class QA_GDB559 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb559";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        // Create nodes table.
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
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void create_edge_type(const std::string& name) {
        ColumnDef w_col{"weight", TypeId::FLOAT64};
        auto eid = graph_->create_edge_type(
            default_database_id, name, nodes_id_, nodes_id_, TypeId::INT64, TypeId::INT64, {w_col});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;
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

    void link(int64_t from, int64_t to, double w, const std::string& edge = "road") {
        auto r = graph_->link(default_database_id, edge, Value(from), Value(to), {Value(w)});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    std::unique_ptr<ColumnRefExpr> make_weight_expr() {
        auto expr = std::make_unique<ColumnRefExpr>();
        expr->table = "r";
        expr->column = "weight";
        return expr;
    }

    MatchConfig make_config(const std::string& edge = "road", int32_t max_hops = 100) {
        MatchConfig config;
        config.nodes.push_back({"a", "nodes"});
        config.nodes.push_back({"b", "nodes"});
        config.edges.push_back(MatchEdgeDef("r", edge, TraverseDirection::OUT, 1, max_hops));
        return config;
    }

    OutputSchema make_schema() {
        std::vector<OutputColumn> cols;
        cols.push_back({"a", "id", TypeId::INT64, false, nodes_id_});
        cols.push_back({"b", "id", TypeId::INT64, false, nodes_id_});
        cols.push_back({"p", "path", TypeId::PATH, false, 0});
        return OutputSchema(std::move(cols));
    }

    std::vector<Tuple> run(PathSelector sel,
                           const Expr* weight,
                           const std::string& edge = "road",
                           int32_t k = 0,
                           int32_t max_hops = 100) {
        BoundStatement bound;
        MatchShortestPathOperator op(*graph_,
                                     *catalog_,
                                     *storage_,
                                     default_database_id,
                                     make_config(edge, max_hops),
                                     make_schema(),
                                     nullptr,
                                     bound,
                                     sel,
                                     "p",
                                     k,
                                     MatchShortestPathOperator::DEFAULT_MAX_VISITED,
                                     weight);
        auto open_result = op.open();
        EXPECT_TRUE(open_result.has_value()) << open_result.error().message;
        if (!open_result.has_value())
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

    static std::vector<const Tuple*>
    filter_pair(const std::vector<Tuple>& results, int64_t src, int64_t tgt) {
        std::vector<const Tuple*> filtered;
        for (const auto& t : results) {
            if (t.values.size() >= 2 && !t.values[0].is_null() && !t.values[1].is_null() &&
                t.values[0].as_int64() == src && t.values[1].as_int64() == tgt) {
                filtered.push_back(&t);
            }
        }
        return filtered;
    }

    static constexpr database_id_t default_database_id = 1;
    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t nodes_id_ = 0;
};

// ============================================================================
// AC1: ALL_SHORTEST filters non-shortest paths at destination
// ============================================================================

TEST_F(QA_GDB559, AllShortest_TicketRepro_OnlyShortestReturned) {
    // Exact reproduction from the ticket:
    //   1--(1)-->2--(1)-->4  (cost 2, shortest)
    //   1--(5)-->3--(5)-->4  (cost 10, NOT shortest)
    // Expected: 1 path (cost 2). Bug returned 2.
    for (int64_t id : {1, 2, 3, 4})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 4, 1.0);
    link(1, 3, 5.0);
    link(3, 4, 5.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_4 = filter_pair(results, 1, 4);

    ASSERT_EQ(from_1_to_4.size(), 1u)
        << "ALL_SHORTEST must return only the cheapest path, not non-shortest paths";
    EXPECT_DOUBLE_EQ(from_1_to_4[0]->values[2].as_path().total_weight, 2.0);
}

TEST_F(QA_GDB559, AllShortest_ThreeTiersOfCost_OnlyCheapestReturned) {
    // Three paths to destination with costs 3, 6, 9:
    //   1--(1)-->2--(2)-->5  (cost 3)
    //   1--(2)-->3--(4)-->5  (cost 6)
    //   1--(3)-->4--(6)-->5  (cost 9)
    for (int64_t id : {1, 2, 3, 4, 5})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 5, 2.0);
    link(1, 3, 2.0);
    link(3, 5, 4.0);
    link(1, 4, 3.0);
    link(4, 5, 6.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_5 = filter_pair(results, 1, 5);

    ASSERT_EQ(from_1_to_5.size(), 1u);
    EXPECT_DOUBLE_EQ(from_1_to_5[0]->values[2].as_path().total_weight, 3.0);
}

TEST_F(QA_GDB559, AllShortest_EqualCostPathsAllReturned) {
    // Two paths with same cost should BOTH be returned:
    //   1--(1)-->2--(2)-->4  (cost 3)
    //   1--(2)-->3--(1)-->4  (cost 3)
    for (int64_t id : {1, 2, 3, 4})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 4, 2.0);
    link(1, 3, 2.0);
    link(3, 4, 1.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_4 = filter_pair(results, 1, 4);

    ASSERT_EQ(from_1_to_4.size(), 2u) << "ALL_SHORTEST should return both equal-cost paths";
    for (const auto* t : from_1_to_4) {
        EXPECT_DOUBLE_EQ(t->values[2].as_path().total_weight, 3.0);
    }
}

TEST_F(QA_GDB559, AllShortest_EqualCostPlusExpensive_OnlyEqualCostReturned) {
    // Two equal-cost paths (cost 3) plus one expensive (cost 10):
    //   1--(1)-->2--(2)-->5  (cost 3)
    //   1--(2)-->3--(1)-->5  (cost 3)
    //   1--(4)-->4--(6)-->5  (cost 10)
    for (int64_t id : {1, 2, 3, 4, 5})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 5, 2.0);
    link(1, 3, 2.0);
    link(3, 5, 1.0);
    link(1, 4, 4.0);
    link(4, 5, 6.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_5 = filter_pair(results, 1, 5);

    ASSERT_EQ(from_1_to_5.size(), 2u)
        << "Only the two equal-cost shortest paths should be returned";
    for (const auto* t : from_1_to_5) {
        EXPECT_DOUBLE_EQ(t->values[2].as_path().total_weight, 3.0);
    }
}

// ============================================================================
// AC2: Late cheaper arrival purges expensive paths
// ============================================================================

TEST_F(QA_GDB559, AllShortest_LateCheaperArrival_PurgesExpensive) {
    // Adversarial topology: expensive path reaches destination first because
    // its intermediate node has low entry cost.
    //   1--(1)-->2--(100)-->4  (cost 101, node 2 popped early at cost 1)
    //   1--(50)-->3--(1)-->4   (cost 51, node 3 popped later at cost 50)
    for (int64_t id : {1, 2, 3, 4})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 4, 100.0);
    link(1, 3, 50.0);
    link(3, 4, 1.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_4 = filter_pair(results, 1, 4);

    ASSERT_EQ(from_1_to_4.size(), 1u)
        << "Expensive path (cost 101) should be purged when cheaper (cost 51) arrives";
    EXPECT_DOUBLE_EQ(from_1_to_4[0]->values[2].as_path().total_weight, 51.0);
    // Verify it's the correct path (through node 3, not node 2).
    const auto& path = from_1_to_4[0]->values[2].as_path();
    ASSERT_EQ(path.steps.size(), 3u);
    EXPECT_EQ(path.steps[0].node_pk, 1);
    EXPECT_EQ(path.steps[1].node_pk, 3);
    EXPECT_EQ(path.steps[2].node_pk, 4);
}

TEST_F(QA_GDB559, AllShortest_MultiplePurges_OnlyCheapestSurvives) {
    // Three arrival tiers with progressively cheaper late arrivals:
    //   1--(1)-->2--(200)-->5   (cost 201, arrives first)
    //   1--(10)-->3--(90)-->5   (cost 100, arrives second, purges 201)
    //   1--(40)-->4--(10)-->5   (cost 50, arrives third, purges 100)
    for (int64_t id : {1, 2, 3, 4, 5})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 5, 200.0);
    link(1, 3, 10.0);
    link(3, 5, 90.0);
    link(1, 4, 40.0);
    link(4, 5, 10.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_5 = filter_pair(results, 1, 5);

    ASSERT_EQ(from_1_to_5.size(), 1u)
        << "After multiple purges, only the cheapest path should remain";
    EXPECT_DOUBLE_EQ(from_1_to_5[0]->values[2].as_path().total_weight, 50.0);
}

TEST_F(QA_GDB559, AllShortest_PurgeAndThenEqualCostArrival) {
    // Expensive path arrives, then two equal cheaper paths arrive:
    //   1--(1)-->2--(200)-->5   (cost 201, arrives first, gets purged)
    //   1--(10)-->3--(20)-->5   (cost 30, arrives second)
    //   1--(10)-->4--(20)-->5   (cost 30, arrives third, same cost)
    for (int64_t id : {1, 2, 3, 4, 5})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 5, 200.0);
    link(1, 3, 10.0);
    link(3, 5, 20.0);
    link(1, 4, 10.0);
    link(4, 5, 20.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_5 = filter_pair(results, 1, 5);

    ASSERT_EQ(from_1_to_5.size(), 2u)
        << "After purge, both equal-cost cheaper paths should be returned";
    for (const auto* t : from_1_to_5) {
        EXPECT_DOUBLE_EQ(t->values[2].as_path().total_weight, 30.0);
    }
}

// ============================================================================
// Edge cases: zero weights, very large weights, single path
// ============================================================================

TEST_F(QA_GDB559, AllShortest_ZeroWeightPath_FiltersNonZero) {
    // Zero-weight path vs non-zero path:
    //   1--(0)-->2--(0)-->3  (cost 0)
    //   1--(1)-->4--(1)-->3  (cost 2)
    for (int64_t id : {1, 2, 3, 4})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 0.0);
    link(2, 3, 0.0);
    link(1, 4, 1.0);
    link(4, 3, 1.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_3 = filter_pair(results, 1, 3);

    ASSERT_EQ(from_1_to_3.size(), 1u) << "Zero-cost path should be the only shortest";
    EXPECT_DOUBLE_EQ(from_1_to_3[0]->values[2].as_path().total_weight, 0.0);
}

TEST_F(QA_GDB559, AllShortest_VeryLargeWeightDifference) {
    // Massive weight difference to test floating-point handling:
    //   1--(0.001)-->2--(0.001)-->3  (cost 0.002)
    //   1--(1e6)-->4--(1e6)-->3      (cost 2e6)
    for (int64_t id : {1, 2, 3, 4})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 0.001);
    link(2, 3, 0.001);
    link(1, 4, 1e6);
    link(4, 3, 1e6);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_3 = filter_pair(results, 1, 3);

    ASSERT_EQ(from_1_to_3.size(), 1u);
    EXPECT_NEAR(from_1_to_3[0]->values[2].as_path().total_weight, 0.002, 1e-9);
}

TEST_F(QA_GDB559, AllShortest_SinglePathToDest_Unchanged) {
    // Only one path exists — should still be returned.
    //   1--(5)-->2--(10)-->3
    for (int64_t id : {1, 2, 3})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 5.0);
    link(2, 3, 10.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_3 = filter_pair(results, 1, 3);

    ASSERT_EQ(from_1_to_3.size(), 1u);
    EXPECT_DOUBLE_EQ(from_1_to_3[0]->values[2].as_path().total_weight, 15.0);
}

// ============================================================================
// ANY_SHORTEST regression: should not be broken by the fix
// ============================================================================

TEST_F(QA_GDB559, AnyShortest_ReturnsShortestPath) {
    // ANY_SHORTEST with unequal costs: should return cheapest.
    //   1--(1)-->2--(1)-->4  (cost 2)
    //   1--(5)-->3--(5)-->4  (cost 10)
    for (int64_t id : {1, 2, 3, 4})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 4, 1.0);
    link(1, 3, 5.0);
    link(3, 4, 5.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ANY_SHORTEST, w.get());
    auto from_1_to_4 = filter_pair(results, 1, 4);

    ASSERT_EQ(from_1_to_4.size(), 1u);
    EXPECT_DOUBLE_EQ(from_1_to_4[0]->values[2].as_path().total_weight, 2.0);
}

TEST_F(QA_GDB559, AnyShortest_LateCheaperArrival_ReturnsCorrectCost) {
    // Adversarial for ANY_SHORTEST: expensive intermediate popped first.
    //   1--(1)-->2--(100)-->4  (cost 101, intermediate 2 popped at cost 1)
    //   1--(50)-->3--(1)-->4   (cost 51, intermediate 3 popped at cost 50)
    // ANY_SHORTEST should return cost 51 (the actual shortest), not 101.
    for (int64_t id : {1, 2, 3, 4})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 4, 100.0);
    link(1, 3, 50.0);
    link(3, 4, 1.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ANY_SHORTEST, w.get());
    auto from_1_to_4 = filter_pair(results, 1, 4);

    ASSERT_EQ(from_1_to_4.size(), 1u);
    // The actual shortest weighted path has cost 51, not 101.
    EXPECT_DOUBLE_EQ(from_1_to_4[0]->values[2].as_path().total_weight, 51.0)
        << "ANY_SHORTEST should return the cheapest path (51), not the first arrival (101)";
}

// ============================================================================
// SHORTEST_K: destination cost filtering
// ============================================================================

TEST_F(QA_GDB559, ShortestK_FiltersNonShortestAtDestination) {
    // SHORTEST_K should also not return non-shortest paths.
    //   1--(1)-->2--(1)-->4  (cost 2)
    //   1--(5)-->3--(5)-->4  (cost 10)
    for (int64_t id : {1, 2, 3, 4})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 4, 1.0);
    link(1, 3, 5.0);
    link(3, 4, 5.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::SHORTEST_K, w.get(), "road", 5);
    auto from_1_to_4 = filter_pair(results, 1, 4);

    for (const auto* t : from_1_to_4) {
        EXPECT_DOUBLE_EQ(t->values[2].as_path().total_weight, 2.0)
            << "SHORTEST_K should only return cheapest paths, not cost-10";
    }
}

TEST_F(QA_GDB559, ShortestK_LateCheaperArrival_PurgesExpensive) {
    // SHORTEST_K with late cheaper arrival.
    //   1--(1)-->2--(100)-->4  (cost 101)
    //   1--(50)-->3--(1)-->4   (cost 51)
    for (int64_t id : {1, 2, 3, 4})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 4, 100.0);
    link(1, 3, 50.0);
    link(3, 4, 1.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::SHORTEST_K, w.get(), "road", 5);
    auto from_1_to_4 = filter_pair(results, 1, 4);

    // The cost-101 path should be purged when cost-51 arrives.
    for (const auto* t : from_1_to_4) {
        EXPECT_DOUBLE_EQ(t->values[2].as_path().total_weight, 51.0)
            << "SHORTEST_K should purge expensive path when cheaper arrival found";
    }
}

TEST_F(QA_GDB559, ShortestK_EarlyReturn_MissesCheaperPath) {
    // Adversarial: K=2, two expensive paths arrive at dest before cheaper path.
    //   1--(1)-->2--(100)-->5  (cost 101, node 2 popped at cost 1)
    //   1--(1)-->3--(100)-->5  (cost 101, node 3 popped at cost 1)
    //   1--(50)-->4--(1)-->5   (cost 51, node 4 popped at cost 50)
    //
    // SHORTEST_K with K=2 might collect 2 cost-101 paths and return early,
    // missing the cheaper cost-51 path entirely.
    for (int64_t id : {1, 2, 3, 4, 5})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 5, 100.0);
    link(1, 3, 1.0);
    link(3, 5, 100.0);
    link(1, 4, 50.0);
    link(4, 5, 1.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::SHORTEST_K, w.get(), "road", 2);
    auto from_1_to_5 = filter_pair(results, 1, 5);

    // At minimum, the cheapest path (cost 51) must be in the results.
    bool has_cheapest = false;
    for (const auto* t : from_1_to_5) {
        if (std::abs(t->values[2].as_path().total_weight - 51.0) < 1e-9) {
            has_cheapest = true;
        }
    }
    EXPECT_TRUE(has_cheapest)
        << "SHORTEST_K must not early-return with K expensive paths when cheaper exists";

    // No path should have cost greater than 101 (the worst path to dest).
    for (const auto* t : from_1_to_5) {
        EXPECT_LE(t->values[2].as_path().total_weight, 101.0 + 1e-9);
    }
}

// ============================================================================
// Complex topology: multi-hop with mixed weights
// ============================================================================

TEST_F(QA_GDB559, AllShortest_LongChain_OnlyCheapestReturned) {
    // Longer multi-hop paths:
    //   1→2→3→4→5   costs 1+1+1+1 = 4
    //   1→6→5        costs 10+10 = 20
    for (int64_t id : {1, 2, 3, 4, 5, 6})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 3, 1.0);
    link(3, 4, 1.0);
    link(4, 5, 1.0);
    link(1, 6, 10.0);
    link(6, 5, 10.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_5 = filter_pair(results, 1, 5);

    ASSERT_EQ(from_1_to_5.size(), 1u);
    EXPECT_DOUBLE_EQ(from_1_to_5[0]->values[2].as_path().total_weight, 4.0);
    EXPECT_EQ(from_1_to_5[0]->values[2].as_path().steps.size(), 5u)
        << "Should be the 4-hop path 1→2→3→4→5";
}

TEST_F(QA_GDB559, AllShortest_DiamondWithBackEdge_NoNonShortest) {
    // Diamond plus a back edge that could create a longer path:
    //   1--(1)-->2--(1)-->4  (cost 2)
    //   1--(1)-->3--(1)-->4  (cost 2)
    //   2--(1)-->3--(1)-->4  (cost 3 via 1→2→3→4, should NOT be returned)
    for (int64_t id : {1, 2, 3, 4})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 4, 1.0);
    link(1, 3, 1.0);
    link(3, 4, 1.0);
    link(2, 3, 1.0); // cross-edge

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_4 = filter_pair(results, 1, 4);

    // Should return only cost-2 paths, not the cost-3 path 1→2→3→4.
    for (const auto* t : from_1_to_4) {
        EXPECT_DOUBLE_EQ(t->values[2].as_path().total_weight, 2.0)
            << "Should not include longer 3-hop path through cross-edge";
    }
    EXPECT_GE(from_1_to_4.size(), 2u)
        << "Both cost-2 paths (through 2 and through 3) should be returned";
}

// ============================================================================
// Stress: many paths to destination, only a few shortest
// ============================================================================

TEST_F(QA_GDB559, AllShortest_ManyPaths_OnlyCheapestReturned) {
    // 10 paths to destination with varying costs. Only the 2 cheapest (cost 5)
    // should be returned.
    // Nodes: 1 (source), 2-11 (intermediates), 12 (destination)
    for (int64_t id = 1; id <= 12; ++id)
        insert_node(id);
    create_edge_type("road");

    // Two cheapest paths (cost 5):
    link(1, 2, 2.0);
    link(2, 12, 3.0);
    link(1, 3, 3.0);
    link(3, 12, 2.0);

    // Eight more expensive paths (costs 10-80):
    for (int64_t i = 4; i <= 11; ++i) {
        double w1 = static_cast<double>(i);
        double w2 = static_cast<double>(i) * (i - 3);
        link(1, i, w1);
        link(i, 12, w2);
    }

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_12 = filter_pair(results, 1, 12);

    ASSERT_EQ(from_1_to_12.size(), 2u)
        << "Only the two cost-5 paths should be returned out of 10 total";
    for (const auto* t : from_1_to_12) {
        EXPECT_DOUBLE_EQ(t->values[2].as_path().total_weight, 5.0);
    }
}

// ============================================================================
// Max-depth interaction: depth limit should not interfere with cost filter
// ============================================================================

TEST_F(QA_GDB559, AllShortest_MaxDepthDoesNotReturnExpensive) {
    // Cheap long path vs expensive short path, max_hops covers both:
    //   1--(1)-->2--(1)-->3--(1)-->4  (cost 3, 3 hops)
    //   1--(100)-->4                  (cost 100, 1 hop)
    for (int64_t id : {1, 2, 3, 4})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 3, 1.0);
    link(3, 4, 1.0);
    link(1, 4, 100.0);

    auto w = make_weight_expr();
    // max_hops = 5, covers both paths.
    auto results = run(PathSelector::ALL_SHORTEST, w.get(), "road", 0, 5);
    auto from_1_to_4 = filter_pair(results, 1, 4);

    ASSERT_EQ(from_1_to_4.size(), 1u);
    EXPECT_DOUBLE_EQ(from_1_to_4[0]->values[2].as_path().total_weight, 3.0)
        << "Cost-3 multi-hop path is cheaper than cost-100 single-hop path";
}

// ============================================================================
// path_cost correctness after purge
// ============================================================================

TEST_F(QA_GDB559, AllShortest_PathCostCorrectAfterPurge) {
    // After purge, the surviving path's total_weight must be accurate.
    //   1--(1)-->2--(1000)-->3  (cost 1001, arrives first, gets purged)
    //   1--(500)-->4--(1)-->3   (cost 501, arrives later)
    for (int64_t id : {1, 2, 3, 4})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 3, 1000.0);
    link(1, 4, 500.0);
    link(4, 3, 1.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_3 = filter_pair(results, 1, 3);

    ASSERT_EQ(from_1_to_3.size(), 1u);
    const auto& path = from_1_to_3[0]->values[2].as_path();
    EXPECT_DOUBLE_EQ(path.total_weight, 501.0);

    // Verify path step count matches expected hops.
    EXPECT_EQ(path.steps.size(), 3u);
    // Verify total_weight is consistent with the actual path structure.
    EXPECT_EQ(path.steps[0].node_pk, 1);
    EXPECT_EQ(path.steps[1].node_pk, 4);
    EXPECT_EQ(path.steps[2].node_pk, 3);
}

} // namespace
} // namespace sixseven
