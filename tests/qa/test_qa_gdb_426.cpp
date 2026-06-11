/// @file test_qa_gdb_426.cpp
/// QA adversarial tests for GDB-426: Weighted Shortest Path.
///
/// Verifies:
///   AC1: Dijkstra finds weighted shortest path on known graph
///   AC2: path_cost(p) returns correct total weight
///   AC3: Negative weight produces clear error
///   AC4: Disconnected graph returns empty result
///   AC5: Unit tests written and passing
///   AC6: No regressions
///
/// Adversarial categories: diamond graphs with equal costs, zero-weight edges,
/// shared intermediate nodes, cycles, large weights, SHORTEST_K with weights,
/// ALL_SHORTEST with weights, disconnected subgraphs, self-loops, stress tests,
/// non-numeric weight properties, missing weight properties, operator reopen.

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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Parser helpers
// ============================================================================

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

// ============================================================================
// Weighted graph test fixture
// ============================================================================

/// Reusable fixture with configurable weighted graph topology.
class GDB426_WeightedSP : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb426";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        // Create 'nodes' table with id column.
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

    void create_edge_type(const std::string& name, const std::string& prop_name = "weight") {
        ColumnDef w_col{prop_name, TypeId::FLOAT64};
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

    std::unique_ptr<ColumnRefExpr> make_weight_expr(const std::string& table = "r",
                                                    const std::string& col = "weight") {
        auto expr = std::make_unique<ColumnRefExpr>();
        expr->table = table;
        expr->column = col;
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

    /// Run weighted shortest path and return result tuples.
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

    /// Run and expect open() to fail, returning the error.
    Error run_expect_error(PathSelector sel, const Expr* weight, const std::string& edge = "road") {
        BoundStatement bound;
        MatchShortestPathOperator op(*graph_,
                                     *catalog_,
                                     *storage_,
                                     default_database_id,
                                     make_config(edge),
                                     make_schema(),
                                     nullptr,
                                     bound,
                                     sel,
                                     "p",
                                     0,
                                     MatchShortestPathOperator::DEFAULT_MAX_VISITED,
                                     weight);
        auto result = op.open();
        EXPECT_FALSE(result.has_value());
        if (result.has_value()) {
            // Should not reach here; return a sentinel so the caller's
            // assertions fail loudly rather than crashing.
            return Error{StatusCode::OK, "expected open() to fail but it succeeded"};
        }
        return result.error();
    }

    /// Filter results by source and target PK (copies, does not move).
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
// AC1: Dijkstra finds weighted shortest path on known graph
// ============================================================================

/// AC1: Basic weighted shortest path — cheaper long path beats expensive short path.
TEST_F(GDB426_WeightedSP, AC1_DijkstraFindsWeightedShortest) {
    // Graph: 1--(10)-->2--(20)-->5  (cost 30)
    //        1--(5)-->3--(3)-->4--(2)-->5  (cost 10)
    for (int64_t id : {1, 2, 3, 4, 5})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 10.0);
    link(2, 5, 20.0);
    link(1, 3, 5.0);
    link(3, 4, 3.0);
    link(4, 5, 2.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ANY_SHORTEST, w.get());
    auto from_1_to_5 = filter_pair(results, 1, 5);
    ASSERT_EQ(from_1_to_5.size(), 1u);

    const auto& path = from_1_to_5[0]->values[2].as_path();
    // Should be 1→3→4→5 (cheaper), NOT 1→2→5.
    ASSERT_EQ(path.steps.size(), 4u);
    EXPECT_EQ(path.steps[0].node_pk, 1);
    EXPECT_EQ(path.steps[1].node_pk, 3);
    EXPECT_EQ(path.steps[2].node_pk, 4);
    EXPECT_EQ(path.steps[3].node_pk, 5);
    EXPECT_DOUBLE_EQ(path.total_weight, 10.0);
}

// ============================================================================
// AC2: path_cost(p) returns correct total weight
// ============================================================================

/// AC2: Verify path total_weight field matches expected cost.
TEST_F(GDB426_WeightedSP, AC2_PathCostCorrect) {
    for (int64_t id : {1, 2, 3})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 7.5);
    link(2, 3, 12.5);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ANY_SHORTEST, w.get());
    auto from_1_to_3 = filter_pair(results, 1, 3);
    ASSERT_EQ(from_1_to_3.size(), 1u);
    EXPECT_DOUBLE_EQ(from_1_to_3[0]->values[2].as_path().total_weight, 20.0);
}

/// AC2: path_cost for same-node path should be 0.
TEST_F(GDB426_WeightedSP, AC2_SameNodeCostZero) {
    insert_node(1);
    create_edge_type("road");

    auto w = make_weight_expr();
    auto results = run(PathSelector::ANY_SHORTEST, w.get());
    auto from_1_to_1 = filter_pair(results, 1, 1);
    ASSERT_EQ(from_1_to_1.size(), 1u);
    EXPECT_DOUBLE_EQ(from_1_to_1[0]->values[2].as_path().total_weight, 0.0);
    EXPECT_EQ(from_1_to_1[0]->values[2].as_path().length(), 0);
}

// ============================================================================
// AC3: Negative weight produces clear error
// ============================================================================

/// AC3: Negative weight on an edge should produce a clear error during open().
TEST_F(GDB426_WeightedSP, AC3_NegativeWeightError) {
    for (int64_t id : {1, 2})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, -5.0);

    auto w = make_weight_expr();
    auto err = run_expect_error(PathSelector::ANY_SHORTEST, w.get());
    // Error message should mention "non-negative".
    EXPECT_NE(err.message.find("non-negative"), std::string::npos)
        << "Error message should mention 'non-negative', got: " << err.message;
}

/// AC3: Negative weight on a later edge in the path should also error.
TEST_F(GDB426_WeightedSP, AC3_NegativeWeightOnLaterEdge) {
    for (int64_t id : {1, 2, 3})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 5.0);
    link(2, 3, -1.0); // Negative on second edge.

    auto w = make_weight_expr();
    auto err = run_expect_error(PathSelector::ANY_SHORTEST, w.get());
    EXPECT_NE(err.message.find("non-negative"), std::string::npos);
}

// ============================================================================
// AC4: Disconnected graph returns empty result
// ============================================================================

/// AC4: No path between disconnected nodes returns empty.
TEST_F(GDB426_WeightedSP, AC4_DisconnectedNoPath) {
    for (int64_t id : {1, 2, 10, 20})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(10, 20, 1.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ANY_SHORTEST, w.get());
    auto from_1_to_10 = filter_pair(results, 1, 10);
    EXPECT_TRUE(from_1_to_10.empty());
    auto from_1_to_20 = filter_pair(results, 1, 20);
    EXPECT_TRUE(from_1_to_20.empty());
}

/// AC4: Isolated node (no edges at all) returns empty for paths to it.
TEST_F(GDB426_WeightedSP, AC4_IsolatedNode) {
    for (int64_t id : {1, 2, 99})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ANY_SHORTEST, w.get());
    auto from_1_to_99 = filter_pair(results, 1, 99);
    EXPECT_TRUE(from_1_to_99.empty());
}

// ============================================================================
// Adversarial: Zero-weight edges
// ============================================================================

/// Zero-weight edges are valid. Dijkstra should still find the correct path.
TEST_F(GDB426_WeightedSP, ZeroWeightEdges) {
    for (int64_t id : {1, 2, 3, 4})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 0.0);
    link(2, 3, 0.0);
    link(1, 4, 1.0);
    link(4, 3, 0.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ANY_SHORTEST, w.get());
    auto from_1_to_3 = filter_pair(results, 1, 3);
    ASSERT_EQ(from_1_to_3.size(), 1u);
    // Path 1→2→3 has cost 0, 1→4→3 has cost 1. Should pick cost-0 path.
    EXPECT_DOUBLE_EQ(from_1_to_3[0]->values[2].as_path().total_weight, 0.0);
    EXPECT_EQ(from_1_to_3[0]->values[2].as_path().length(), 2);
}

// ============================================================================
// Adversarial: Cycles in weighted graph
// ============================================================================

/// Dijkstra should handle cycles without infinite looping.
TEST_F(GDB426_WeightedSP, CycleDoesNotLoop) {
    for (int64_t id : {1, 2, 3})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(2, 3, 1.0);
    link(3, 1, 1.0); // Cycle back to start.

    auto w = make_weight_expr();
    auto results = run(PathSelector::ANY_SHORTEST, w.get());
    auto from_1_to_3 = filter_pair(results, 1, 3);
    ASSERT_EQ(from_1_to_3.size(), 1u);
    EXPECT_DOUBLE_EQ(from_1_to_3[0]->values[2].as_path().total_weight, 2.0);
}

// ============================================================================
// Adversarial: Very large weights
// ============================================================================

/// Large weights should not cause overflow or precision issues.
TEST_F(GDB426_WeightedSP, LargeWeights) {
    for (int64_t id : {1, 2, 3})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1e15);
    link(2, 3, 1e15);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ANY_SHORTEST, w.get());
    auto from_1_to_3 = filter_pair(results, 1, 3);
    ASSERT_EQ(from_1_to_3.size(), 1u);
    EXPECT_DOUBLE_EQ(from_1_to_3[0]->values[2].as_path().total_weight, 2e15);
}

// ============================================================================
// Adversarial: max_depth limit with weighted paths
// ============================================================================

/// max_hops should limit path length even if cost is low.
TEST_F(GDB426_WeightedSP, MaxDepthLimitsPath) {
    // Chain: 1→2→3→4→5, each weight 0.1.
    for (int64_t id : {1, 2, 3, 4, 5})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 0.1);
    link(2, 3, 0.1);
    link(3, 4, 0.1);
    link(4, 5, 0.1);

    auto w = make_weight_expr();
    // max_hops = 2, so can't reach node 5 from node 1 (needs 4 hops).
    auto results = run(PathSelector::ANY_SHORTEST, w.get(), "road", 0, 2);
    auto from_1_to_5 = filter_pair(results, 1, 5);
    EXPECT_TRUE(from_1_to_5.empty());

    // max_hops = 3: can reach 4 from 1 but not 5.
    auto results3 = run(PathSelector::ANY_SHORTEST, w.get(), "road", 0, 3);
    auto from_1_to_4 = filter_pair(results3, 1, 4);
    ASSERT_EQ(from_1_to_4.size(), 1u);
    auto from_1_to_5_3 = filter_pair(results3, 1, 5);
    EXPECT_TRUE(from_1_to_5_3.empty());
}

// ============================================================================
// Adversarial: Diamond graph — equal-cost paths (ALL_SHORTEST)
// ============================================================================

/// Diamond graph with equal-cost paths:
///   1--(5)-->2--(5)-->4
///   1--(5)-->3--(5)-->4
/// Both paths cost 10. ALL_SHORTEST should find both.
TEST_F(GDB426_WeightedSP, AllShortestDiamondEqualCost) {
    for (int64_t id : {1, 2, 3, 4})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 5.0);
    link(1, 3, 5.0);
    link(2, 4, 5.0);
    link(3, 4, 5.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_4 = filter_pair(results, 1, 4);

    // Both paths have cost 10. ALL_SHORTEST should return both.
    ASSERT_EQ(from_1_to_4.size(), 2u)
        << "ALL_SHORTEST should find 2 equal-cost paths in diamond graph";

    // Both should have total_weight = 10.
    for (const auto* t : from_1_to_4) {
        EXPECT_DOUBLE_EQ(t->values[2].as_path().total_weight, 10.0);
    }
}

// ============================================================================
// Adversarial: Shared intermediate node — ALL_SHORTEST misses paths
// ============================================================================

/// Two equal-cost paths through a shared intermediate node:
///   1--(1)-->2--(1)-->3--(1)-->4
///   1--(1)-->5--(1)-->3--(1)-->4
/// Both paths cost 3. ALL_SHORTEST should find both.
/// BUG HYPOTHESIS: The strict < in best_cost check may prevent exploring
/// the second path through node 3 at equal cost.
TEST_F(GDB426_WeightedSP, AllShortestSharedIntermediateEqualCost) {
    for (int64_t id : {1, 2, 3, 4, 5})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 1.0);
    link(1, 5, 1.0);
    link(2, 3, 1.0);
    link(5, 3, 1.0);
    link(3, 4, 1.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ALL_SHORTEST, w.get());
    auto from_1_to_4 = filter_pair(results, 1, 4);

    // Both paths cost 3: 1→2→3→4 and 1→5→3→4.
    // This test may expose a bug where the second path through shared node 3
    // is rejected because best_cost[3] = 2 and new_cost = 2 is not < 2.
    ASSERT_GE(from_1_to_4.size(), 2u)
        << "ALL_SHORTEST with shared intermediate node should find both equal-cost paths";
}

// ============================================================================
// Adversarial: SHORTEST_K with weights
// ============================================================================

/// SHORTEST 2 should find the two cheapest paths.
TEST_F(GDB426_WeightedSP, ShortestKFindsKPaths) {
    // Three paths from 1 to 4:
    //   1→2→4 cost 10 (3+7)
    //   1→3→4 cost 5 (2+3)
    //   1→5→4 cost 20 (15+5)
    for (int64_t id : {1, 2, 3, 4, 5})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 3.0);
    link(2, 4, 7.0);
    link(1, 3, 2.0);
    link(3, 4, 3.0);
    link(1, 5, 15.0);
    link(5, 4, 5.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::SHORTEST_K, w.get(), "road", 2);
    auto from_1_to_4 = filter_pair(results, 1, 4);

    // SHORTEST 2 should find 2 paths to node 4.
    ASSERT_GE(from_1_to_4.size(), 2u) << "SHORTEST 2 should find at least 2 paths";

    // Sort by cost to verify ordering.
    std::sort(from_1_to_4.begin(), from_1_to_4.end(), [](const Tuple* a, const Tuple* b) {
        return a->values[2].as_path().total_weight < b->values[2].as_path().total_weight;
    });

    // Cheapest should be cost 5 (1→3→4).
    EXPECT_DOUBLE_EQ(from_1_to_4[0]->values[2].as_path().total_weight, 5.0);
    // Second should be cost 10 (1→2→4).
    EXPECT_DOUBLE_EQ(from_1_to_4[1]->values[2].as_path().total_weight, 10.0);
}

/// SHORTEST K where K > available paths should return all available.
TEST_F(GDB426_WeightedSP, ShortestKMoreThanAvailable) {
    for (int64_t id : {1, 2})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 5.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::SHORTEST_K, w.get(), "road", 10);
    auto from_1_to_2 = filter_pair(results, 1, 2);
    // Only one path exists, should return 1 regardless of K=10.
    EXPECT_GE(from_1_to_2.size(), 1u);
}

// ============================================================================
// Adversarial: Non-numeric weight property
// ============================================================================

/// String weight property should produce a clear error.
TEST_F(GDB426_WeightedSP, StringWeightPropertyError) {
    for (int64_t id : {1, 2})
        insert_node(id);

    // Create edge type with string property.
    ColumnDef str_col{"label", TypeId::STRING};
    auto eid = graph_->create_edge_type(default_database_id,
                                        "labeled",
                                        nodes_id_,
                                        nodes_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        {str_col});
    ASSERT_TRUE(eid.has_value());

    auto r = graph_->link(default_database_id,
                          "labeled",
                          Value(int64_t{1}),
                          Value(int64_t{2}),
                          {Value(std::string("hello"))});
    ASSERT_TRUE(r.has_value());

    auto w = make_weight_expr("r", "label");
    auto err = run_expect_error(PathSelector::ANY_SHORTEST, w.get(), "labeled");
    EXPECT_NE(err.message.find("numeric"), std::string::npos)
        << "Error should mention numeric type requirement, got: " << err.message;
}

// ============================================================================
// Adversarial: Missing weight property name
// ============================================================================

/// Referencing a non-existent property should produce a clear error.
TEST_F(GDB426_WeightedSP, MissingWeightPropertyError) {
    for (int64_t id : {1, 2})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 5.0);

    auto w = make_weight_expr("r", "nonexistent_property");
    auto err = run_expect_error(PathSelector::ANY_SHORTEST, w.get());
    EXPECT_NE(err.message.find("nonexistent_property"), std::string::npos)
        << "Error should mention the missing property name, got: " << err.message;
}

// ============================================================================
// Adversarial: Self-loop with weight
// ============================================================================

/// Self-loop should not cause infinite loop in Dijkstra.
TEST_F(GDB426_WeightedSP, SelfLoopHandled) {
    for (int64_t id : {1, 2})
        insert_node(id);
    create_edge_type("road");
    link(1, 1, 1.0); // Self-loop.
    link(1, 2, 5.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ANY_SHORTEST, w.get());
    auto from_1_to_2 = filter_pair(results, 1, 2);
    ASSERT_EQ(from_1_to_2.size(), 1u);
    EXPECT_DOUBLE_EQ(from_1_to_2[0]->values[2].as_path().total_weight, 5.0);
}

// ============================================================================
// Adversarial: Integer weight property (not just FLOAT64)
// ============================================================================

/// Weights stored as INT64 should work correctly.
TEST_F(GDB426_WeightedSP, IntegerWeightProperty) {
    for (int64_t id : {1, 2, 3})
        insert_node(id);

    ColumnDef int_col{"dist", TypeId::INT64};
    auto eid = graph_->create_edge_type(default_database_id,
                                        "iroad",
                                        nodes_id_,
                                        nodes_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        {int_col});
    ASSERT_TRUE(eid.has_value());

    auto r1 = graph_->link(
        default_database_id, "iroad", Value(int64_t{1}), Value(int64_t{2}), {Value(int64_t{10})});
    ASSERT_TRUE(r1.has_value());
    auto r2 = graph_->link(
        default_database_id, "iroad", Value(int64_t{2}), Value(int64_t{3}), {Value(int64_t{20})});
    ASSERT_TRUE(r2.has_value());

    auto w = make_weight_expr("r", "dist");
    auto results = run(PathSelector::ANY_SHORTEST, w.get(), "iroad");
    auto from_1_to_3 = filter_pair(results, 1, 3);
    ASSERT_EQ(from_1_to_3.size(), 1u);
    EXPECT_DOUBLE_EQ(from_1_to_3[0]->values[2].as_path().total_weight, 30.0);
}

// ============================================================================
// Adversarial: Operator reopen
// ============================================================================

/// Reopening the operator should produce the same results.
TEST_F(GDB426_WeightedSP, OperatorReopen) {
    for (int64_t id : {1, 2})
        insert_node(id);
    create_edge_type("road");
    link(1, 2, 3.0);

    auto w = make_weight_expr();
    BoundStatement bound;
    MatchShortestPathOperator op(*graph_,
                                 *catalog_,
                                 *storage_,
                                 default_database_id,
                                 make_config(),
                                 make_schema(),
                                 nullptr,
                                 bound,
                                 PathSelector::ANY_SHORTEST,
                                 "p",
                                 0,
                                 MatchShortestPathOperator::DEFAULT_MAX_VISITED,
                                 w.get());

    // First run.
    auto r1 = op.open();
    ASSERT_TRUE(r1.has_value());
    size_t count1 = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value());
        if (!row->has_value())
            break;
        ++count1;
    }
    op.close();

    // Second run.
    auto r2 = op.open();
    ASSERT_TRUE(r2.has_value());
    size_t count2 = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value());
        if (!row->has_value())
            break;
        ++count2;
    }
    op.close();

    EXPECT_EQ(count1, count2);
    EXPECT_GT(count1, 0u);
}

// ============================================================================
// Adversarial: NULL weight value
// ============================================================================

/// NULL weight value should produce a clear error.
TEST_F(GDB426_WeightedSP, NullWeightError) {
    for (int64_t id : {1, 2})
        insert_node(id);
    create_edge_type("road");

    // Link with NULL weight.
    auto r =
        graph_->link(default_database_id, "road", Value(int64_t{1}), Value(int64_t{2}), {Value()});
    ASSERT_TRUE(r.has_value());

    auto w = make_weight_expr();
    auto err = run_expect_error(PathSelector::ANY_SHORTEST, w.get());
    EXPECT_NE(err.message.find("NULL"), std::string::npos)
        << "Error should mention NULL, got: " << err.message;
}

// ============================================================================
// Parser adversarial tests
// ============================================================================

/// WEIGHT clause without path selector should fail.
TEST(GDB426_Parser, WeightWithoutSelectorFails) {
    EXPECT_TRUE(parse_fails("MATCH (a:cities)-[r:road]->(b:cities) WEIGHT r.distance RETURN a, b"));
}

/// WEIGHT with a complex expression (not column ref) — should parse but
/// executor requires column ref.
TEST(GDB426_Parser, WeightComplexExprParsesOk) {
    auto stmt =
        parse_one("MATCH p = ANY SHORTEST (a:c)-[r:e]->{1,10}(b:c) WEIGHT r.x + r.y RETURN a");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    // weight_expr should be a BinaryExpr, not a ColumnRefExpr.
    ASSERT_NE(m->weight_expr, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(m->weight_expr.get());
    EXPECT_NE(bin, nullptr);
}

/// WEIGHT in SELECT ... FROM MATCH syntax.
TEST(GDB426_Parser, WeightInSelectFromMatch) {
    auto stmt = parse_one("SELECT path_cost(p) FROM MATCH p = ANY SHORTEST (a:n)-[r:e]->{1,5}(b:n) "
                          "WEIGHT r.w WHERE a.id = 1");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    ASSERT_NE(m->weight_expr, nullptr);
}

/// WEIGHT as a column name should still be valid.
TEST(GDB426_Parser, WeightAsIdentifier) {
    auto stmt = parse_one("SELECT weight FROM t");
    ASSERT_NE(stmt, nullptr);
}

/// Double WEIGHT clause should fail.
TEST(GDB426_Parser, DoubleWeightClauseFails) {
    // Second WEIGHT should cause parse error (WEIGHT is consumed, next token
    // is unexpected).
    EXPECT_TRUE(parse_fails("MATCH p = ANY SHORTEST (a:c)-[r:e]->{1,5}(b:c) "
                            "WEIGHT r.x WEIGHT r.y RETURN a"));
}

// ============================================================================
// Adversarial: Stress — linear chain
// ============================================================================

/// Stress test: weighted shortest path on a 50-node linear chain.
TEST_F(GDB426_WeightedSP, StressLinearChain50) {
    constexpr int N = 50;
    for (int64_t id = 1; id <= N; ++id)
        insert_node(id);
    create_edge_type("road");
    for (int64_t i = 1; i < N; ++i)
        link(i, i + 1, 1.0);

    auto w = make_weight_expr();
    auto results = run(PathSelector::ANY_SHORTEST, w.get());
    auto from_1_to_50 = filter_pair(results, 1, N);
    ASSERT_EQ(from_1_to_50.size(), 1u);
    EXPECT_DOUBLE_EQ(from_1_to_50[0]->values[2].as_path().total_weight, static_cast<double>(N - 1));
    EXPECT_EQ(from_1_to_50[0]->values[2].as_path().length(), N - 1);
}

// ============================================================================
// Path struct tests
// ============================================================================

/// Default Path total_weight should be 0.
TEST(GDB426_PathStruct, DefaultTotalWeightZero) {
    Path p;
    EXPECT_DOUBLE_EQ(p.total_weight, 0.0);
}

/// total_weight survives Value round-trip.
TEST(GDB426_PathStruct, TotalWeightPreservedInValue) {
    Path p;
    p.steps.push_back({1, 10});
    p.steps.push_back({2, -1});
    p.total_weight = 99.9;

    Value v(std::move(p));
    EXPECT_EQ(v.type_id(), TypeId::PATH);
    EXPECT_DOUBLE_EQ(v.as_path().total_weight, 99.9);
}

/// Path equality includes total_weight.
TEST(GDB426_PathStruct, EqualityIncludesTotalWeight) {
    Path p1;
    p1.steps.push_back({1, -1});
    p1.total_weight = 5.0;

    Path p2;
    p2.steps.push_back({1, -1});
    p2.total_weight = 10.0;

    EXPECT_NE(p1, p2) << "Paths with different total_weight should not be equal";
}

} // namespace
} // namespace sixseven
