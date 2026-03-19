/// @file test_qa_gdb_555.cpp
/// QA adversarial tests for GDB-555: ALL_SHORTEST with weighted paths misses
/// equal-cost paths through shared intermediate nodes.
///
/// Verifies the fix: ALL_SHORTEST with WEIGHT uses <= for equal-cost paths.

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
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Fixture
// ============================================================================

class QA_GDB555 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb555";
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
        auto eid = graph_->create_edge_type(default_database_id, 
            name, nodes_id_, nodes_id_, TypeId::INT64, TypeId::INT64, {w_col});
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
        auto r = graph_->link(edge, Value(from), Value(to), {Value(w)});
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
// Core fix: ALL_SHORTEST finds equal-cost paths through shared intermediate
// ============================================================================

TEST_F(QA_GDB555, AllShortest_SharedIntermediate_BothPathsFound) {
    // Graph:
    //   1 --(1)--> 2 --(1)--> 3 --(1)--> 4
    //   1 --(1)--> 5 --(1)--> 3 --(1)--> 4
    // Both paths 1->2->3->4 and 1->5->3->4 have cost 3.
    for (int64_t id : {1, 2, 3, 4, 5})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 3, 1.0);
    link(3, 4, 1.0);
    link(1, 5, 1.0);
    link(5, 3, 1.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_4 = filter_pair(results, 1, 4);

    ASSERT_EQ(from_1_to_4.size(), 2u)
        << "ALL_SHORTEST should find both equal-cost paths through shared node 3";

    // Both paths should have cost 3.
    for (const auto* t : from_1_to_4) {
        EXPECT_DOUBLE_EQ(t->values[2].as_path().total_weight, 3.0);
    }
}

TEST_F(QA_GDB555, AllShortest_SharedIntermediate_CorrectPaths) {
    // Same graph as above, verify actual paths.
    for (int64_t id : {1, 2, 3, 4, 5})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 3, 1.0);
    link(3, 4, 1.0);
    link(1, 5, 1.0);
    link(5, 3, 1.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_4 = filter_pair(results, 1, 4);
    ASSERT_EQ(from_1_to_4.size(), 2u);

    // Collect second node of each path (should be 2 and 5).
    std::set<int64_t> second_nodes;
    for (const auto* t : from_1_to_4) {
        const auto& path = t->values[2].as_path();
        ASSERT_GE(path.steps.size(), 2u);
        second_nodes.insert(path.steps[1].node_pk);
    }
    EXPECT_TRUE(second_nodes.count(2)) << "Path through node 2 should be found";
    EXPECT_TRUE(second_nodes.count(5)) << "Path through node 5 should be found";
}

// ============================================================================
// Regression: ANY_SHORTEST still returns single path
// ============================================================================

TEST_F(QA_GDB555, AnyShortest_StillReturnsSinglePath) {
    for (int64_t id : {1, 2, 3, 4, 5})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 3, 1.0);
    link(3, 4, 1.0);
    link(1, 5, 1.0);
    link(5, 3, 1.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ANY_SHORTEST, w.get());
    auto from_1_to_4 = filter_pair(results, 1, 4);
    EXPECT_EQ(from_1_to_4.size(), 1u) << "ANY_SHORTEST should still return exactly one path";
}

// ============================================================================
// Adversarial: multiple shared intermediate nodes
// ============================================================================

TEST_F(QA_GDB555, AllShortest_TwoSharedIntermediates) {
    // Graph:
    //   1 --(1)--> 2 --(1)--> 3 --(1)--> 4 --(1)--> 5
    //   1 --(1)--> 6 --(1)--> 3 --(1)--> 4 --(1)--> 5
    //   1 --(1)--> 2 --(1)--> 7 --(2)--> 5  (cost 4, not shortest)
    // Two shortest paths 1->2->3->4->5 and 1->6->3->4->5 (cost 4 each).
    for (int64_t id : {1, 2, 3, 4, 5, 6, 7})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 3, 1.0);
    link(3, 4, 1.0);
    link(4, 5, 1.0);
    link(1, 6, 1.0);
    link(6, 3, 1.0);
    link(2, 7, 1.0);
    link(7, 5, 2.0); // cost 4 via 1->2->7->5 (same cost but different path)

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_5 = filter_pair(results, 1, 5);

    // Should find at least 2 equal-cost paths (through 2->3->4 and 6->3->4),
    // and possibly the 1->2->7->5 path too (also cost 4).
    EXPECT_GE(from_1_to_5.size(), 2u) << "ALL_SHORTEST should find multiple equal-cost paths";

    // All returned paths should have the same cost.
    double expected_cost = from_1_to_5[0]->values[2].as_path().total_weight;
    for (const auto* t : from_1_to_5) {
        EXPECT_DOUBLE_EQ(t->values[2].as_path().total_weight, expected_cost);
    }
}

// ============================================================================
// Edge case: all edges same weight, diamond graph
// ============================================================================

TEST_F(QA_GDB555, AllShortest_Diamond_EqualWeights) {
    // Diamond: 1->(2 and 3)->4, all weights 1.0
    for (int64_t id : {1, 2, 3, 4})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(1, 3, 1.0);
    link(2, 4, 1.0);
    link(3, 4, 1.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_4 = filter_pair(results, 1, 4);

    EXPECT_EQ(from_1_to_4.size(), 2u)
        << "Diamond with equal weights: 2 shortest paths 1->2->4 and 1->3->4";
}

// ============================================================================
// Edge case: unequal weights, only one shortest path
// ============================================================================

TEST_F(QA_GDB555, AllShortest_UnequalWeights_OnlyReturnsShortestPaths) {
    // Fixed by GDB-559: ALL_SHORTEST with weighted paths now correctly filters
    // destination arrivals by cost. Only shortest paths are returned.
    //
    // 1--(1)-->2--(1)-->4 (cost 2, shortest)
    // 1--(5)-->3--(5)-->4 (cost 10, NOT shortest)
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

    EXPECT_EQ(from_1_to_4.size(), 1u)
        << "ALL_SHORTEST should only return the shortest path (cost 2)";
    EXPECT_DOUBLE_EQ(from_1_to_4[0]->values[2].as_path().total_weight, 2.0);
}

// ============================================================================
// Edge case: zero-weight edges with shared intermediate
// ============================================================================

TEST_F(QA_GDB555, AllShortest_ZeroWeightSharedNode) {
    // 1--(0)-->2--(0)-->3
    // 1--(0)-->4--(0)-->3
    for (int64_t id : {1, 2, 3, 4})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 0.0);
    link(2, 3, 0.0);
    link(1, 4, 0.0);
    link(4, 3, 0.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_3 = filter_pair(results, 1, 3);

    EXPECT_EQ(from_1_to_3.size(), 2u)
        << "ALL_SHORTEST with zero-weight edges should find both paths";
    for (const auto* t : from_1_to_3) {
        EXPECT_DOUBLE_EQ(t->values[2].as_path().total_weight, 0.0);
    }
}

} // namespace
} // namespace sixseven
