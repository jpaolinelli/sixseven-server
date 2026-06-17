/// @file test_qa_gdb_858.cpp
/// @brief QA regression tests for GDB-858: MatchShortestPathOperator must
///        honour the quantifier's min_hops when deciding whether to emit the
///        trivial 0-hop self-path for src==tgt.
///
/// Bug: Both the BFS (find_shortest_paths) and Dijkstra
/// (find_weighted_shortest_paths) variants unconditionally returned a 0-hop
/// self-path whenever src==tgt, ignoring min_hops entirely.  Under a pattern
/// like ->{1,20} (min_hops=1) on an acyclic graph, a query from node X to node
/// X must return NOTHING — a (x,x) pair may only match via a real cycle of >=1
/// edges.
///
/// These tests drive the production MatchShortestPathOperator directly with
/// independently-constructed expectations.  They must FAIL on the pre-fix
/// operator and PASS after the fix.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/match_shortest_path.h"
#include "sixseven/executor/pattern_match.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

// ===========================================================================
// Shared fixture
// ===========================================================================

/// Builds a small acyclic weighted graph:
///   1 --(10)--> 2 --(20)--> 5
///   1 --(5)---> 3 --(3)---> 4 --(2)--> 5
///
/// min_hops=1 (quantifier {1,20}) means 1->1 must return empty.
/// An extra edge 5->1 with weight 7.0 is added by AddCycleBack() to verify
/// that a genuine cycle IS found when one exists.
class GDB858Test : public ::testing::Test {
protected:
    static constexpr database_id_t kDbId = 1;

    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb858";
        std::error_code ec;
        std::filesystem::remove_all(data_dir_, ec);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_r = storage_->create_database_storage(kDbId);
        ASSERT_TRUE(db_r.has_value()) << db_r.error().message;

        // Node table: cities, INT64 PK 'id'.
        {
            TableSchema ts;
            ts.name = "cities";
            CatalogColumnDef pk;
            pk.ordinal = 0;
            pk.name = "id";
            pk.type_id = TypeId::INT64;
            pk.nullable = false;
            ts.columns.push_back(pk);
            ts.pk_columns = "id";
            auto tid = catalog_->create_table(kDbId, std::move(ts));
            ASSERT_TRUE(tid.has_value()) << tid.error().message;
            cities_id_ = *tid;

            auto schema = catalog_->get_table(kDbId, "cities");
            ASSERT_TRUE(schema.has_value());
            auto sr = storage_->create_table_storage(kDbId, cities_id_, *schema);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }

        // Insert nodes 1..5.
        for (int64_t id : {1, 2, 3, 4, 5}) {
            insert_city(id);
        }

        // Edge type 'road' with FLOAT64 'distance' property.
        {
            ColumnDef dist{"distance", TypeId::FLOAT64};
            auto eid = graph_->create_edge_type(
                kDbId, "road", cities_id_, cities_id_, TypeId::INT64, TypeId::INT64, {dist});
            ASSERT_TRUE(eid.has_value()) << eid.error().message;
        }

        // Acyclic topology.
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

    void insert_city(int64_t id) {
        auto ts = storage_->get_table_storage(cities_id_);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        auto schema = catalog_->get_table_by_id(cities_id_);
        ASSERT_TRUE(schema.has_value());
        std::vector<Value> vals = {Value(id)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    void link(int64_t from, int64_t to, double weight) {
        auto r = graph_->link(kDbId, "road", Value(from), Value(to), {Value(weight)});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    /// Build an OutputSchema for (a.id, b.id, p.path).
    OutputSchema make_schema() {
        std::vector<OutputColumn> cols;
        cols.push_back({"a", "id", TypeId::INT64, false, cities_id_});
        cols.push_back({"b", "id", TypeId::INT64, false, cities_id_});
        cols.push_back({"p", "path", TypeId::PATH, false, 0});
        return OutputSchema(std::move(cols));
    }

    /// Build a MatchConfig with the given min/max hops.
    MatchConfig make_config(int32_t min_hops, int32_t max_hops) {
        MatchConfig config;
        config.nodes.push_back({"a", "cities"});
        config.nodes.push_back({"b", "cities"});
        config.edges.push_back(
            MatchEdgeDef("r", "road", TraverseDirection::OUT, min_hops, max_hops));
        return config;
    }

    /// Build a ColumnRefExpr for r.distance.
    std::unique_ptr<ColumnRefExpr> make_weight_expr() {
        auto e = std::make_unique<ColumnRefExpr>();
        e->table = "r";
        e->column = "distance";
        return e;
    }

    /// Run the operator and return all result tuples.
    std::vector<Tuple> run_op(MatchConfig config, PathSelector selector, const Expr* weight_expr) {
        BoundStatement bound;
        MatchShortestPathOperator op(*graph_,
                                     *catalog_,
                                     *storage_,
                                     kDbId,
                                     std::move(config),
                                     make_schema(),
                                     nullptr,
                                     bound,
                                     selector,
                                     "p",
                                     0,
                                     MatchShortestPathOperator::DEFAULT_MAX_VISITED,
                                     weight_expr);
        auto r = op.open();
        EXPECT_TRUE(r.has_value()) << r.error().message;

        std::vector<Tuple> rows;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row->has_value())
                break;
            rows.push_back(std::move(**row));
        }
        op.close();
        return rows;
    }

    /// Filter rows to those where src==from_id and tgt==to_id.
    static std::vector<Tuple> filter(std::vector<Tuple>& rows, int64_t from_id, int64_t to_id) {
        std::vector<Tuple> out;
        for (auto& t : rows) {
            if (t.values[0].as_int64() == from_id && t.values[1].as_int64() == to_id) {
                out.push_back(t);
            }
        }
        return out;
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t cities_id_ = 0;
};

// ===========================================================================
// GDB-858 regression: min_hops=1, acyclic graph, weighted (Dijkstra)
// ===========================================================================

/// Under {1,20} the Dijkstra variant must NOT emit a 0-hop 1->1 self-path on
/// an acyclic graph.  This test would FAIL on the pre-fix operator (which
/// returned exactly one 0-weight result) and PASS after the fix.
TEST_F(GDB858Test, WeightedSameNodeMinHopsOneNoResult) {
    auto weight = make_weight_expr();
    auto rows = run_op(make_config(1, 20), PathSelector::ANY_SHORTEST, weight.get());
    auto self = filter(rows, 1, 1);

    EXPECT_TRUE(self.empty())
        << "GDB-858: Dijkstra variant must not emit a 0-hop self-path when min_hops=1 "
           "and no real cycle exists";
}

/// All five nodes must produce empty self-results under {1,20} on this acyclic
/// graph (not just node 1).
TEST_F(GDB858Test, WeightedAllNodesNoSelfPathUnderMinHopsOne) {
    auto weight = make_weight_expr();
    auto rows = run_op(make_config(1, 20), PathSelector::ANY_SHORTEST, weight.get());

    for (int64_t id : {1, 2, 3, 4, 5}) {
        auto self = filter(rows, id, id);
        EXPECT_TRUE(self.empty())
            << "GDB-858: node " << id
            << " must not produce a self-path under {1,20} on an acyclic graph";
    }
}

// ===========================================================================
// GDB-858 regression: min_hops=1, acyclic graph, unweighted (BFS)
// ===========================================================================

/// The BFS variant must also honour min_hops=1 (no weight expression passed).
TEST_F(GDB858Test, BFSSameNodeMinHopsOneNoResult) {
    auto rows = run_op(make_config(1, 20), PathSelector::ANY_SHORTEST, nullptr);
    auto self = filter(rows, 1, 1);

    EXPECT_TRUE(self.empty())
        << "GDB-858: BFS variant must not emit a 0-hop self-path when min_hops=1 "
           "and no real cycle exists";
}

// ===========================================================================
// GDB-858 complementary: min_hops=0 still emits the 0-hop self-path
// ===========================================================================

/// Under {0,20} (min_hops=0) the trivial self-path IS valid and must be
/// returned.  This verifies the fix is narrow (does not suppress the path when
/// the quantifier legitimately includes 0 hops).
TEST_F(GDB858Test, WeightedSameNodeMinHopsZeroReturnsSelfPath) {
    auto weight = make_weight_expr();
    auto rows = run_op(make_config(0, 20), PathSelector::ANY_SHORTEST, weight.get());
    auto self = filter(rows, 1, 1);

    ASSERT_EQ(self.size(), 1u) << "GDB-858: under {0,20} the 0-hop self-path must be emitted";
    const auto& path = self[0].values[2].as_path();
    EXPECT_EQ(path.length(), 0);
    EXPECT_DOUBLE_EQ(path.total_weight, 0.0);
}

TEST_F(GDB858Test, BFSSameNodeMinHopsZeroReturnsSelfPath) {
    auto rows = run_op(make_config(0, 20), PathSelector::ANY_SHORTEST, nullptr);
    auto self = filter(rows, 1, 1);

    ASSERT_EQ(self.size(), 1u) << "GDB-858: BFS under {0,20} must emit the 0-hop self-path";
    EXPECT_EQ(self[0].values[2].as_path().length(), 0);
}

// ===========================================================================
// GDB-858 complementary: genuine cycle is still found under {1,20}
// ===========================================================================

/// Add edge 5->1 so that a real cycle 1->3->4->5->1 (cost 15) exists.
/// Under {1,20} the operator must now RETURN that cycle path.
/// This proves the fix doesn't suppress legitimate cycle-based self-results.
TEST_F(GDB858Test, WeightedRealCycleFoundUnderMinHopsOne) {
    // Add back-edge 5->1 (weight 5.0), forming cycle 1->3->4->5->1 (cost 15).
    link(5, 1, 5.0);

    auto weight = make_weight_expr();
    auto rows = run_op(make_config(1, 20), PathSelector::ANY_SHORTEST, weight.get());
    auto self = filter(rows, 1, 1);

    ASSERT_EQ(self.size(), 1u) << "GDB-858: a genuine cycle must be returned even under min_hops=1";
    const auto& path = self[0].values[2].as_path();
    EXPECT_GE(path.length(), 1) << "GDB-858: cycle path must have at least 1 hop";
    EXPECT_GT(path.total_weight, 0.0) << "GDB-858: cycle path must have positive weight";
}

TEST_F(GDB858Test, BFSRealCycleFoundUnderMinHopsOne) {
    link(5, 1, 5.0);

    auto rows = run_op(make_config(1, 20), PathSelector::ANY_SHORTEST, nullptr);
    auto self = filter(rows, 1, 1);

    ASSERT_EQ(self.size(), 1u) << "GDB-858: BFS must find the real cycle under min_hops=1";
    EXPECT_GE(self[0].values[2].as_path().length(), 1);
}

// ===========================================================================
// GDB-858 non-regression: normal (src!=tgt) paths are unaffected
// ===========================================================================

/// The fix must not disturb normal path finding between distinct nodes.
/// 1->5 shortest weighted path is via 1->3->4->5 (cost 10).
TEST_F(GDB858Test, WeightedNormalPathUnaffected) {
    auto weight = make_weight_expr();
    auto rows = run_op(make_config(1, 20), PathSelector::ANY_SHORTEST, weight.get());
    auto paths = filter(rows, 1, 5);

    ASSERT_EQ(paths.size(), 1u);
    const auto& path = paths[0].values[2].as_path();
    EXPECT_EQ(path.length(), 3);
    EXPECT_DOUBLE_EQ(path.total_weight, 10.0);
}
