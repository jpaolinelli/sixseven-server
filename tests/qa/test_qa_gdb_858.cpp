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

/// GDB-1273 regression: a legitimate (src!=tgt) BFS path whose length is
/// EXACTLY max_hops must be RETURNED.  The shortest 1->4 path (1->3->4) is
/// 2 hops; under {1,2} it sits exactly at the quantifier ceiling.  The
/// pre-fix BFS used a strict `length < max_depth` bound (max_depth==max_hops)
/// and wrongly DROPPED it, returning zero rows.  The inclusive `<=` bound,
/// consistent with Dijkstra and VariableLengthMatchOperator, must keep it.
TEST_F(GDB858Test, BFSPathExactlyAtMaxHopsReturned) {
    auto rows = run_op(make_config(1, 2), PathSelector::ANY_SHORTEST, nullptr);
    auto paths = filter(rows, 1, 4);

    ASSERT_EQ(paths.size(), 1u)
        << "GDB-1273: BFS path of length exactly max_hops (2) must be returned";
    EXPECT_EQ(paths[0].values[2].as_path().length(), 2);
}

/// Consistency companion: the Dijkstra variant (already inclusive) must also
/// return the same length==max_hops path, so both operators agree.
TEST_F(GDB858Test, WeightedPathExactlyAtMaxHopsReturned) {
    auto weight = make_weight_expr();
    auto rows = run_op(make_config(1, 2), PathSelector::ANY_SHORTEST, weight.get());
    auto paths = filter(rows, 1, 4);

    ASSERT_EQ(paths.size(), 1u)
        << "GDB-1273: Dijkstra path of length exactly max_hops (2) must be returned";
    EXPECT_EQ(paths[0].values[2].as_path().length(), 2);
}

// ===========================================================================
// GDB-858 adversarial: direct self-loop edge (1-hop cycle x->x)
// ===========================================================================

/// A direct self-loop edge x->x satisfies min_hops=1 with exactly 1 hop.
/// The operator MUST return that path — it is a real cycle, not the suppressed
/// 0-hop trivial path.
TEST_F(GDB858Test, WeightedDirectSelfLoopReturnedUnderMinHopsOne) {
    // Add a direct self-loop: 1 --(weight 3.0)--> 1
    link(1, 1, 3.0);

    auto weight = make_weight_expr();
    auto rows = run_op(make_config(1, 20), PathSelector::ANY_SHORTEST, weight.get());
    auto self = filter(rows, 1, 1);

    ASSERT_EQ(self.size(), 1u)
        << "GDB-858 adversarial: a direct self-loop (1 hop) must be returned under min_hops=1";
    const auto& path = self[0].values[2].as_path();
    EXPECT_EQ(path.length(), 1) << "self-loop path must be exactly 1 hop";
    EXPECT_DOUBLE_EQ(path.total_weight, 3.0) << "self-loop weight must match edge weight";
}

TEST_F(GDB858Test, BFSDirectSelfLoopReturnedUnderMinHopsOne) {
    link(1, 1, 3.0);

    auto rows = run_op(make_config(1, 20), PathSelector::ANY_SHORTEST, nullptr);
    auto self = filter(rows, 1, 1);

    ASSERT_EQ(self.size(), 1u)
        << "GDB-858 adversarial: BFS — direct self-loop (1 hop) must be returned under min_hops=1";
    EXPECT_EQ(self[0].values[2].as_path().length(), 1);
}

/// Under min_hops=2, a direct self-loop (1 hop) must NOT be returned because
/// it has fewer hops than the quantifier minimum.
TEST_F(GDB858Test, WeightedDirectSelfLoopSuppressedUnderMinHopsTwo) {
    link(1, 1, 3.0);

    auto weight = make_weight_expr();
    auto rows = run_op(make_config(2, 20), PathSelector::ANY_SHORTEST, weight.get());
    auto self = filter(rows, 1, 1);

    EXPECT_TRUE(self.empty())
        << "GDB-858 adversarial: 1-hop self-loop must NOT be returned when min_hops=2";
}

TEST_F(GDB858Test, BFSDirectSelfLoopSuppressedUnderMinHopsTwo) {
    link(1, 1, 3.0);

    auto rows = run_op(make_config(2, 20), PathSelector::ANY_SHORTEST, nullptr);
    auto self = filter(rows, 1, 1);

    EXPECT_TRUE(self.empty())
        << "GDB-858 adversarial: BFS — 1-hop self-loop must NOT be returned when min_hops=2";
}

// ===========================================================================
// GDB-858 adversarial: multiple cycles of different lengths/weights back to src
// ===========================================================================

/// Graph: 1->2->1 (2 hops, weight 30) and 1->3->4->5->1 (4 hops, weight 15).
/// Dijkstra must pick the cheaper cycle (weight 15, via 3->4->5->1), NOT the
/// shorter-hop one (weight 30) when selector is ANY_SHORTEST (by weight).
TEST_F(GDB858Test, WeightedMultipleCyclesPicksCheapest) {
    // Back-edge 2->1 (weight 20): cycle 1->2->1 = cost 10+20 = 30 in 2 hops
    link(2, 1, 20.0);
    // Back-edge 5->1 (weight 5): cycle 1->3->4->5->1 = cost 5+3+2+5 = 15 in 4 hops
    link(5, 1, 5.0);

    auto weight = make_weight_expr();
    auto rows = run_op(make_config(1, 20), PathSelector::ANY_SHORTEST, weight.get());
    auto self = filter(rows, 1, 1);

    ASSERT_EQ(self.size(), 1u) << "GDB-858 adversarial: must return exactly one shortest cycle";
    const auto& path = self[0].values[2].as_path();
    EXPECT_DOUBLE_EQ(path.total_weight, 15.0)
        << "GDB-858 adversarial: Dijkstra must return cheaper cycle (15), not shorter-hop one (30)";
}

/// BFS picks fewest-hop cycle (2 hops via 1->2->1), ignoring weights.
TEST_F(GDB858Test, BFSMultipleCyclesPicksFewestHops) {
    link(2, 1, 20.0); // creates 2-hop cycle 1->2->1
    link(5, 1, 5.0);  // creates 4-hop cycle 1->3->4->5->1

    auto rows = run_op(make_config(1, 20), PathSelector::ANY_SHORTEST, nullptr);
    auto self = filter(rows, 1, 1);

    ASSERT_EQ(self.size(), 1u) << "GDB-858 adversarial: BFS must return the fewest-hop cycle";
    const auto& path = self[0].values[2].as_path();
    EXPECT_EQ(path.length(), 2) << "GDB-858 adversarial: BFS cycle path must be 2 hops (1->2->1)";
}

// ===========================================================================
// GDB-858 adversarial: max_hops interaction — cycle longer than max_hops
// ===========================================================================

/// No self-cycle may be returned when every cycle is longer than max_hops.
/// With the 5->1 back-edge the graph has two cycles back to 1: 1->2->5->1
/// (3 hops) and 1->3->4->5->1 (4 hops).  Under max_hops=2 BOTH exceed the
/// ceiling, so no self-path is valid.  (max_hops=3 would NOT test this intent:
/// the 3-hop cycle 1->2->5->1 is then legitimately in-bounds and must be
/// returned — exactly the inclusive-bound behavior fixed in GDB-1273.)
TEST_F(GDB858Test, WeightedCycleLongerThanMaxHopsNotReturned) {
    link(5, 1, 5.0);

    auto weight = make_weight_expr();
    auto rows = run_op(make_config(1, 2), PathSelector::ANY_SHORTEST, weight.get());
    auto self = filter(rows, 1, 1);

    EXPECT_TRUE(self.empty())
        << "GDB-858 adversarial: every cycle (>=3 hops) exceeds max_hops=2 → none returned";
}

TEST_F(GDB858Test, BFSCycleLongerThanMaxHopsNotReturned) {
    link(5, 1, 5.0);

    auto rows = run_op(make_config(1, 2), PathSelector::ANY_SHORTEST, nullptr);
    auto self = filter(rows, 1, 1);

    EXPECT_TRUE(self.empty())
        << "GDB-858 adversarial: BFS — every cycle (>=3 hops) exceeds max_hops=2 → none returned";
}

/// Inclusive-bound counterpart (GDB-1273): under max_hops=3 the 3-hop cycle
/// 1->2->5->1 IS valid and BFS (fewest hops) must return it.  This would FAIL
/// on the pre-fix exclusive `length < max_hops` bound, which dropped it.
TEST_F(GDB858Test, BFSCycleExactlyAtMaxHopsReturned) {
    link(5, 1, 5.0);

    auto rows = run_op(make_config(1, 3), PathSelector::ANY_SHORTEST, nullptr);
    auto self = filter(rows, 1, 1);

    ASSERT_EQ(self.size(), 1u)
        << "GDB-1273: the 3-hop cycle 1->2->5->1 is in-bounds under max_hops=3";
    EXPECT_EQ(self[0].values[2].as_path().length(), 3);
}

// ===========================================================================
// GDB-858 adversarial: min_hops exactly equals cycle length (boundary)
// ===========================================================================

/// Cycle 1->3->4->5->1 is 4 hops. Under {4, 20} (min_hops == cycle length),
/// the path MUST be returned (boundary inclusive).
TEST_F(GDB858Test, WeightedCycleExactlyAtMinHopsReturned) {
    link(5, 1, 5.0);

    auto weight = make_weight_expr();
    auto rows = run_op(make_config(4, 20), PathSelector::ANY_SHORTEST, weight.get());
    auto self = filter(rows, 1, 1);

    ASSERT_EQ(self.size(), 1u)
        << "GDB-858 adversarial: 4-hop cycle must be returned when min_hops==4 (exact match)";
    EXPECT_EQ(self[0].values[2].as_path().length(), 4);
}

/// Under {5, 20} (min_hops > cycle length 4), the cycle must NOT be returned.
TEST_F(GDB858Test, WeightedCycleBelowMinHopsNotReturned) {
    link(5, 1, 5.0);

    auto weight = make_weight_expr();
    auto rows = run_op(make_config(5, 20), PathSelector::ANY_SHORTEST, weight.get());
    auto self = filter(rows, 1, 1);

    EXPECT_TRUE(self.empty())
        << "GDB-858 adversarial: 4-hop cycle must NOT be returned when min_hops=5";
}

// ===========================================================================
// GDB-858 adversarial: direction filter — cycle only via non-matching direction
// ===========================================================================

/// The back-edge 5->1 is OUT from 5 to 1.  If we traverse with direction=IN
/// from src=1, we would traverse the edge backwards (1<-5, which means looking
/// at edges that TARGET 1 — so 5->1 IS reachable IN from 1).
/// Test direction=OUT from 1: there is no forward cycle 1->...->1 without
/// the back-edge existing as OUT from some node.
/// Here we verify: with direction=IN, the cycle IS found (5->1 means node 1
/// has an IN edge from 5, so from 1 going IN we reach 5, then from 5 going IN
/// we reach 4, etc. — i.e., we traverse the graph in reverse).
TEST_F(GDB858Test, WeightedDirectionINFindsReverseReachableCycle) {
    // Graph (OUT): 1->2, 2->5, 1->3, 3->4, 4->5 + back-edge 5->1
    link(5, 1, 5.0);

    auto weight = make_weight_expr();

    // Direction=IN: traversal follows edges in reverse.
    // From 1 going IN: we see edges that point TO 1, i.e. 5->1.
    // So we reach node 5, then from 5 going IN we see 2->5 and 4->5, reaching
    // nodes 2 and 4, etc.  A cycle back to 1 via IN traversal would require
    // an edge pointing TO 5 from nodes reachable from 1 in reverse.
    MatchConfig cfg;
    cfg.nodes.push_back({"a", "cities"});
    cfg.nodes.push_back({"b", "cities"});
    cfg.edges.push_back(MatchEdgeDef("r", "road", TraverseDirection::IN, 1, 20));

    auto rows = run_op(std::move(cfg), PathSelector::ANY_SHORTEST, weight.get());
    // The key assertion: if a cycle exists via IN traversal from node 1 back to
    // node 1, it must have at least 1 hop. We just confirm no 0-hop fake path.
    auto self = filter(rows, 1, 1);
    for (const auto& t : self) {
        EXPECT_GE(t.values[2].as_path().length(), 1)
            << "GDB-858 adversarial: no 0-hop path must appear even with direction=IN";
    }
}

// ===========================================================================
// GDB-858 adversarial: degenerate cases
// ===========================================================================

/// Single node, no edges. Under {1,20}, no self-path expected (no real edges).
/// Under {0,20}, the trivial self-path must still be returned.
TEST_F(GDB858Test, WeightedSingleNodeNoEdgesMinHopsOne) {
    // Insert an isolated node 6 (no outgoing or incoming edges).
    insert_city(6);

    auto weight = make_weight_expr();
    auto rows = run_op(make_config(1, 20), PathSelector::ANY_SHORTEST, weight.get());
    auto self = filter(rows, 6, 6);

    EXPECT_TRUE(self.empty())
        << "GDB-858 adversarial: isolated node must yield no self-path under {1,20}";
}

TEST_F(GDB858Test, WeightedSingleNodeNoEdgesMinHopsZero) {
    insert_city(6);

    auto weight = make_weight_expr();
    auto rows = run_op(make_config(0, 20), PathSelector::ANY_SHORTEST, weight.get());
    auto self = filter(rows, 6, 6);

    ASSERT_EQ(self.size(), 1u)
        << "GDB-858 adversarial: isolated node must yield 0-hop self-path under {0,20}";
    EXPECT_EQ(self[0].values[2].as_path().length(), 0);
}

TEST_F(GDB858Test, BFSSingleNodeNoEdgesMinHopsOne) {
    insert_city(6);

    auto rows = run_op(make_config(1, 20), PathSelector::ANY_SHORTEST, nullptr);
    auto self = filter(rows, 6, 6);

    EXPECT_TRUE(self.empty())
        << "GDB-858 adversarial: BFS — isolated node must yield no self-path under {1,20}";
}

/// Disconnected pair: node 6 has no path to node 7. Must return empty for
/// that pair, and must not crash or corrupt state for subsequent pairs.
TEST_F(GDB858Test, WeightedDisconnectedPairReturnsEmpty) {
    insert_city(6);
    insert_city(7);
    // No edge between 6 and 7.

    auto weight = make_weight_expr();
    auto rows = run_op(make_config(1, 20), PathSelector::ANY_SHORTEST, weight.get());

    auto paths_6_7 = filter(rows, 6, 7);
    EXPECT_TRUE(paths_6_7.empty()) << "GDB-858 adversarial: disconnected pair must return no paths";

    auto paths_7_6 = filter(rows, 7, 6);
    EXPECT_TRUE(paths_7_6.empty())
        << "GDB-858 adversarial: disconnected pair (reverse) must return no paths";
}

// ===========================================================================
// GDB-858 adversarial: dense cyclic graph with multiple cycles through a node
// ===========================================================================

/// Build a denser graph where node 1 participates in multiple cycles:
///   - 2-hop cycle: 1->2->1 (weight 30)
///   - 3-hop cycle: 1->3->4->1' — no, let's use: 1->2->3->1 via new edges
/// Specifically: add edges 2->3 (weight 1) and 3->1 (weight 1).
/// Now cycles from 1: 1->2->1 (2 hops, w=30) and 1->2->3->1 (3 hops, w=12)
/// and 1->3->1 via direct link if we also add 3->1.
/// We only add 2->1 and 3->1 back edges.
TEST_F(GDB858Test, WeightedDenseCyclicGraphCorrectShortestCycle) {
    // Add 2->1 (weight 20): cycle 1->2->1 = 2 hops, weight 30
    link(2, 1, 20.0);
    // Add 3->1 (weight 1): cycle 1->3->1 = 2 hops, weight 6 (5+1)
    link(3, 1, 1.0);

    auto weight = make_weight_expr();
    auto rows = run_op(make_config(1, 20), PathSelector::ANY_SHORTEST, weight.get());
    auto self = filter(rows, 1, 1);

    ASSERT_EQ(self.size(), 1u) << "GDB-858 adversarial: dense graph — must return one result";
    const auto& path = self[0].values[2].as_path();
    // Cheapest cycle: 1->3->1 = 5+1 = 6 (vs 1->2->1 = 30)
    EXPECT_DOUBLE_EQ(path.total_weight, 6.0)
        << "GDB-858 adversarial: Dijkstra must pick cheapest cycle (6.0) in dense graph";
    EXPECT_EQ(path.length(), 2) << "GDB-858 adversarial: cheapest cycle has 2 hops";
}

/// Same dense graph but with BFS: both 2-hop cycles are equally short.
/// BFS must return one of them (ANY_SHORTEST returns any one).
TEST_F(GDB858Test, BFSDenseCyclicGraphReturnsTwoHopCycle) {
    link(2, 1, 20.0);
    link(3, 1, 1.0);

    auto rows = run_op(make_config(1, 20), PathSelector::ANY_SHORTEST, nullptr);
    auto self = filter(rows, 1, 1);

    // Both 1->2->1 and 1->3->1 are 2-hop cycles. BFS finds one of them.
    ASSERT_EQ(self.size(), 1u) << "GDB-858 adversarial: BFS dense graph — must return one result";
    EXPECT_EQ(self[0].values[2].as_path().length(), 2)
        << "GDB-858 adversarial: BFS must return a 2-hop cycle";
}

/// Non-self targets must remain unaffected in a dense cyclic graph.
/// 1->5 via Dijkstra: shortest is 1->3->4->5 = 10; adding back-edges 2->1
/// and 3->1 must not change this path.
TEST_F(GDB858Test, WeightedDenseCyclicGraphNonSelfTargetUnaffected) {
    link(2, 1, 20.0);
    link(3, 1, 1.0);

    auto weight = make_weight_expr();
    auto rows = run_op(make_config(1, 20), PathSelector::ANY_SHORTEST, weight.get());
    auto paths_1_5 = filter(rows, 1, 5);

    ASSERT_EQ(paths_1_5.size(), 1u)
        << "GDB-858 adversarial: non-self paths must not be affected by back-edges";
    // 1->3->4->5 = 5+3+2 = 10 (cheaper than 1->2->5 = 10+20=30)
    EXPECT_DOUBLE_EQ(paths_1_5[0].values[2].as_path().total_weight, 10.0);
}
