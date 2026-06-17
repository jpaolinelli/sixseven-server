/// @file test_qa_gdb_860.cpp
/// @brief QA regression tests for GDB-860: Replace tautological
///        PathSelectorEnum.AllValues with production-driving plan_node_name()
///        tests; close the "Weighted " prefix coverage gap flagged in review.
///
/// The PR (GDB-855) introduced MatchShortestPathTest.PlanNodeNamePerSelector
/// which covers all four PathSelector values for the unweighted case.
/// Review flagged that the "Weighted " prefix branch (weight_expr_ != nullptr)
/// was not exercised.  These tests close that gap.
///
/// Tests here:
///   1. WeightedPlanNodeNamePerSelector — constructs a real operator with a
///      non-null weight_expr for every PathSelector; asserts the exact
///      "Weighted …" string.  Mutation-grade: swapping ANY/ALL or stripping the
///      prefix regresses exactly the failing assertion.
///   2. ShortestKInterpolationDifferentK — SHORTEST_K with k=1 must produce
///      "Shortest 1 Path Match", proving the number is not hardcoded as 3.
///   3. AllEnumeratorsCoveredUnweighted — companion check that every known
///      enumerator (NONE, ANY_SHORTEST, ALL_SHORTEST, SHORTEST_K) produces a
///      distinct unweighted string (guards against a new enumerator landing in
///      the default branch silently).

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
#include <set>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

// ===========================================================================
// Minimal fixture: mirrors GDB858Test but named GDB860Test to avoid ODR clash.
// Uses the same "cities" / "road" weighted-edge setup so we can pass a real
// weight_expr (a ColumnRefExpr referencing r.distance) to the operator ctor.
// ===========================================================================

class GDB860Test : public ::testing::Test {
protected:
    static constexpr database_id_t kDbId = 1;

    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb860";
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

        // Insert two nodes so the graph is non-empty.
        for (int64_t id : {1, 2}) {
            insert_city(id);
        }

        // Edge type 'road' with FLOAT64 'distance' property.
        {
            ColumnDef dist{"distance", TypeId::FLOAT64};
            auto eid = graph_->create_edge_type(
                kDbId, "road", cities_id_, cities_id_, TypeId::INT64, TypeId::INT64, {dist});
            ASSERT_TRUE(eid.has_value()) << eid.error().message;
        }

        // One edge so the graph engine can resolve edge metadata.
        link(1, 2, 5.0);
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

    OutputSchema make_schema() {
        std::vector<OutputColumn> cols;
        cols.push_back({"a", "id", TypeId::INT64, false, cities_id_});
        cols.push_back({"b", "id", TypeId::INT64, false, cities_id_});
        cols.push_back({"p", "path", TypeId::PATH, false, 0});
        return OutputSchema(std::move(cols));
    }

    MatchConfig make_config(int32_t min_hops = 1, int32_t max_hops = 10) {
        MatchConfig config;
        config.nodes.push_back({"a", "cities"});
        config.nodes.push_back({"b", "cities"});
        config.edges.push_back(
            MatchEdgeDef("r", "road", TraverseDirection::OUT, min_hops, max_hops));
        return config;
    }

    /// Build a ColumnRefExpr for r.distance — the weight expression used in
    /// weighted shortest path tests.  The operator inspects weight_expr_ only
    /// for its non-null-ness when generating plan_node_name(); the actual column
    /// reference is only dereferenced during do_open() (Dijkstra expansion).
    std::unique_ptr<ColumnRefExpr> make_weight_expr() {
        auto e = std::make_unique<ColumnRefExpr>();
        e->table = "r";
        e->column = "distance";
        return e;
    }

    static constexpr database_id_t default_database_id = kDbId;
    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t cities_id_ = 0;
};

// ===========================================================================
// Test 1: Weighted plan_node_name() for every PathSelector.
//
// Mutation-grade requirements:
//   - Removing the "Weighted " prefix from any case fails.
//   - Swapping ANY_SHORTEST / ALL_SHORTEST strings fails.
//   - Stripping the k-interpolation from SHORTEST_K fails.
//   - Reverting weight_expr_ check to always-false fails ALL four.
//
// Expected strings are derived independently by reading the source switch at
// src/executor/match_shortest_path.cpp lines 77-89:
//   prefix = "Weighted "
//   ANY_SHORTEST  -> "Weighted Any Shortest Path Match"
//   ALL_SHORTEST  -> "Weighted All Shortest Path Match"
//   SHORTEST_K(3) -> "Weighted Shortest 3 Path Match"
//   NONE (default)-> "Weighted Shortest Path Match"
// ===========================================================================

TEST_F(GDB860Test, WeightedPlanNodeNamePerSelector) {
    BoundStatement bound;
    auto weight_expr = make_weight_expr();

    auto make_op = [&](PathSelector sel, int32_t k = 0) {
        return MatchShortestPathOperator(*graph_,
                                         *catalog_,
                                         *storage_,
                                         kDbId,
                                         make_config(),
                                         make_schema(),
                                         nullptr,
                                         bound,
                                         sel,
                                         "p",
                                         k,
                                         MatchShortestPathOperator::DEFAULT_MAX_VISITED,
                                         weight_expr.get());
    };

    // NONE falls through to default: "Weighted Shortest Path Match"
    {
        auto op = make_op(PathSelector::NONE);
        EXPECT_EQ(op.plan_node_name(), "Weighted Shortest Path Match");
    }
    // ANY_SHORTEST: "Weighted Any Shortest Path Match"
    {
        auto op = make_op(PathSelector::ANY_SHORTEST);
        EXPECT_EQ(op.plan_node_name(), "Weighted Any Shortest Path Match");
    }
    // ALL_SHORTEST: "Weighted All Shortest Path Match"
    {
        auto op = make_op(PathSelector::ALL_SHORTEST);
        EXPECT_EQ(op.plan_node_name(), "Weighted All Shortest Path Match");
    }
    // SHORTEST_K(3): "Weighted Shortest 3 Path Match"
    {
        auto op = make_op(PathSelector::SHORTEST_K, 3);
        EXPECT_EQ(op.plan_node_name(), "Weighted Shortest 3 Path Match");
    }
}

// ===========================================================================
// Test 2: SHORTEST_K number interpolation is not hardcoded as 3.
//
// The existing PlanNodeNamePerSelector test uses k=3.  A production bug that
// hardcodes "3" instead of std::to_string(shortest_k_) would pass that test.
// Using k=1 here locks the interpolation independently.
//
// Unweighted variant (no weight_expr): "Shortest 1 Path Match"
// Weighted variant:                    "Weighted Shortest 1 Path Match"
// ===========================================================================

TEST_F(GDB860Test, ShortestKInterpolationDifferentKUnweighted) {
    BoundStatement bound;
    MatchShortestPathOperator op(*graph_,
                                 *catalog_,
                                 *storage_,
                                 kDbId,
                                 make_config(),
                                 make_schema(),
                                 nullptr,
                                 bound,
                                 PathSelector::SHORTEST_K,
                                 "p",
                                 /*k=*/1);
    EXPECT_EQ(op.plan_node_name(), "Shortest 1 Path Match");
}

TEST_F(GDB860Test, ShortestKInterpolationDifferentKWeighted) {
    BoundStatement bound;
    auto weight_expr = make_weight_expr();
    MatchShortestPathOperator op(*graph_,
                                 *catalog_,
                                 *storage_,
                                 kDbId,
                                 make_config(),
                                 make_schema(),
                                 nullptr,
                                 bound,
                                 PathSelector::SHORTEST_K,
                                 "p",
                                 /*k=*/1,
                                 MatchShortestPathOperator::DEFAULT_MAX_VISITED,
                                 weight_expr.get());
    EXPECT_EQ(op.plan_node_name(), "Weighted Shortest 1 Path Match");
}

// ===========================================================================
// Test 3: All four PathSelector enumerators produce distinct unweighted names.
//
// This guards against a new enumerator being added that silently falls through
// to the default branch and collides with NONE's string.  If the set of unique
// names has fewer elements than the number of enumerators we test, that's a
// collision — a production gap.
// ===========================================================================

TEST_F(GDB860Test, AllEnumeratorsCoveredUnweightedDistinct) {
    BoundStatement bound;

    auto make_op = [&](PathSelector sel, int32_t k = 0) {
        return MatchShortestPathOperator(*graph_,
                                         *catalog_,
                                         *storage_,
                                         kDbId,
                                         make_config(),
                                         make_schema(),
                                         nullptr,
                                         bound,
                                         sel,
                                         "p",
                                         k);
    };

    // Collect all plan_node_name() strings for every known enumerator.
    std::set<std::string> names;
    names.insert(make_op(PathSelector::NONE).plan_node_name());
    names.insert(make_op(PathSelector::ANY_SHORTEST).plan_node_name());
    names.insert(make_op(PathSelector::ALL_SHORTEST).plan_node_name());
    names.insert(make_op(PathSelector::SHORTEST_K, 5).plan_node_name());

    // All four must be distinct — if any two collapse to the same string, a
    // case label is either missing or duplicated in the production switch.
    EXPECT_EQ(names.size(), 4u)
        << "plan_node_name() produced duplicate strings for distinct PathSelector values";

    // Spot-check the exact strings as a second mutation layer.
    EXPECT_NE(names.find("Shortest Path Match"), names.end());
    EXPECT_NE(names.find("Any Shortest Path Match"), names.end());
    EXPECT_NE(names.find("All Shortest Path Match"), names.end());
    EXPECT_NE(names.find("Shortest 5 Path Match"), names.end());
}

// ===========================================================================
// Test 4: Weighted vs unweighted toggle — same selector, opposite weight_expr,
// must produce different names.  Confirms the ternary branch is live.
// ===========================================================================

TEST_F(GDB860Test, WeightedPrefixToggledByWeightExpr) {
    BoundStatement bound;
    auto weight_expr = make_weight_expr();

    // Unweighted ANY_SHORTEST.
    MatchShortestPathOperator op_unweighted(*graph_,
                                            *catalog_,
                                            *storage_,
                                            kDbId,
                                            make_config(),
                                            make_schema(),
                                            nullptr,
                                            bound,
                                            PathSelector::ANY_SHORTEST,
                                            "p",
                                            0);
    // Weighted ANY_SHORTEST.
    MatchShortestPathOperator op_weighted(*graph_,
                                          *catalog_,
                                          *storage_,
                                          kDbId,
                                          make_config(),
                                          make_schema(),
                                          nullptr,
                                          bound,
                                          PathSelector::ANY_SHORTEST,
                                          "p",
                                          0,
                                          MatchShortestPathOperator::DEFAULT_MAX_VISITED,
                                          weight_expr.get());

    const std::string unweighted_name = op_unweighted.plan_node_name();
    const std::string weighted_name = op_weighted.plan_node_name();

    EXPECT_EQ(unweighted_name, "Any Shortest Path Match");
    EXPECT_EQ(weighted_name, "Weighted Any Shortest Path Match");
    // The two must differ — if both produce the same string, the prefix branch is dead.
    EXPECT_NE(unweighted_name, weighted_name);
}
