// QA tests for GDB-829: Verify nodes()/edges() production handlers in expr_evaluator.
// Adversarial coverage: empty path, single-node, 2-hop, multi-hop, cycles,
// order preservation, terminal-step convention, type-error on non-PATH, NULL input.

#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

ExprPtr make_path_func(const std::string& func_name) {
    auto col = std::make_unique<ColumnRefExpr>();
    col->column = "p";
    auto call = std::make_unique<FunctionCallExpr>();
    call->name = func_name;
    call->args.push_back(std::move(col));
    return call;
}

OutputSchema make_path_schema() {
    OutputColumn col;
    col.name = "p";
    col.type_id = TypeId::PATH;
    return OutputSchema({col});
}

Tuple make_path_tuple(Path path) {
    return Tuple{{Value(std::move(path))}, std::nullopt};
}

// Make a tuple carrying a NULL value in the PATH column slot.
Tuple make_null_tuple() {
    return Tuple{{Value::make_null()}, std::nullopt};
}

// Make a tuple carrying an INT32 value in the column slot (wrong type).
Tuple make_int_tuple(int32_t v) {
    return Tuple{{Value(v)}, std::nullopt};
}

OutputSchema make_int_schema() {
    OutputColumn col;
    col.name = "p";
    col.type_id = TypeId::INT32;
    return OutputSchema({col});
}

} // namespace

// ---------------------------------------------------------------------------
// Suite: GDB829_ProductionNewTests
// These verify the 4 new tests added by the implementer are genuine — they
// would only pass if the production handler runs correctly.
// ---------------------------------------------------------------------------

// The 4 new PathFunctions tests from test_path_selectors.cpp exercising the
// production handler — verified here by duplicating the same call pattern and
// asserting identical results (ensures the handler, not just the test file, works).

TEST(QA_GDB829_ProductionNewTests, NodesThreeHopPath) {
    Path p;
    p.steps.push_back({1, 100});
    p.steps.push_back({2, 101});
    p.steps.push_back({3, -1});

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};
    auto expr = make_path_func("NODES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_json().data, "[1,2,3]");
}

TEST(QA_GDB829_ProductionNewTests, NodesSingleNodePath) {
    Path p;
    p.steps.push_back({42, -1});

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};
    auto expr = make_path_func("NODES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_json().data, "[42]");
}

TEST(QA_GDB829_ProductionNewTests, EdgesThreeHopPath) {
    Path p;
    p.steps.push_back({1, 100});
    p.steps.push_back({2, 101});
    p.steps.push_back({3, -1});

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};
    auto expr = make_path_func("EDGES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_json().data, "[100,101]");
}

TEST(QA_GDB829_ProductionNewTests, EdgesEmptySingleNode) {
    Path p;
    p.steps.push_back({100, -1});

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};
    auto expr = make_path_func("EDGES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_json().data, "[]");
}

// ---------------------------------------------------------------------------
// Suite: GDB829_EmptyPath
// A Path with zero steps — both functions must return "[]".
// ---------------------------------------------------------------------------

TEST(QA_GDB829_EmptyPath, NodesEmptyPath) {
    Path p; // no steps at all
    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};
    auto expr = make_path_func("NODES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_json().data, "[]");
}

TEST(QA_GDB829_EmptyPath, EdgesEmptyPath) {
    Path p;
    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};
    auto expr = make_path_func("EDGES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_json().data, "[]");
}

// ---------------------------------------------------------------------------
// Suite: GDB829_TerminalStepConvention
// N-hop path has N+1 nodes and N edges. Terminal step has edge_id == -1 and
// must appear in nodes() but NOT in edges().
// ---------------------------------------------------------------------------

TEST(QA_GDB829_TerminalStepConvention, TwoHopNodesCount) {
    // A --e1--> B --e2--> C  (2 edges, 3 nodes)
    Path p;
    p.steps.push_back({10, 200});
    p.steps.push_back({20, 201});
    p.steps.push_back({30, -1}); // terminal

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};

    auto nodes_expr = make_path_func("NODES");
    auto nodes_result = evaluate_expr(*nodes_expr, tuple, schema, bound);
    ASSERT_TRUE(nodes_result.has_value()) << nodes_result.error().message;
    // Must be 3 nodes
    EXPECT_EQ(nodes_result->as_json().data, "[10,20,30]");
}

TEST(QA_GDB829_TerminalStepConvention, TwoHopEdgesCount) {
    Path p;
    p.steps.push_back({10, 200});
    p.steps.push_back({20, 201});
    p.steps.push_back({30, -1});

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};

    auto edges_expr = make_path_func("EDGES");
    auto edges_result = evaluate_expr(*edges_expr, tuple, schema, bound);
    ASSERT_TRUE(edges_result.has_value()) << edges_result.error().message;
    // Must be exactly 2 edges, terminal excluded
    EXPECT_EQ(edges_result->as_json().data, "[200,201]");
}

TEST(QA_GDB829_TerminalStepConvention, TerminalNodeIncludedInNodes) {
    // Verify terminal node PK appears in nodes() output.
    Path p;
    p.steps.push_back({5, 99});
    p.steps.push_back({999, -1}); // terminal node PK=999

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};

    auto expr = make_path_func("NODES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_json().data, "[5,999]");
}

TEST(QA_GDB829_TerminalStepConvention, TerminalEdgeExcludedFromEdges) {
    // Ensure edge_id == -1 on terminal step is NOT in edges().
    Path p;
    p.steps.push_back({1, 50});
    p.steps.push_back({2, -1}); // terminal — -1 must not appear

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};

    auto expr = make_path_func("EDGES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_json().data, "[50]");
}

// ---------------------------------------------------------------------------
// Suite: GDB829_OrderPreservation
// nodes() and edges() must return values in path-traversal order, not sorted.
// ---------------------------------------------------------------------------

TEST(QA_GDB829_OrderPreservation, NodesInTraversalOrder) {
    // Descending PKs to confirm no sorting occurs
    Path p;
    p.steps.push_back({300, 10});
    p.steps.push_back({200, 20});
    p.steps.push_back({100, -1});

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};

    auto expr = make_path_func("NODES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_json().data, "[300,200,100]");
}

TEST(QA_GDB829_OrderPreservation, EdgesInTraversalOrder) {
    // Descending edge IDs to confirm no sorting occurs
    Path p;
    p.steps.push_back({1, 500});
    p.steps.push_back({2, 300});
    p.steps.push_back({3, 100});
    p.steps.push_back({4, -1});

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};

    auto expr = make_path_func("EDGES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_json().data, "[500,300,100]");
}

// ---------------------------------------------------------------------------
// Suite: GDB829_CyclicPath
// A cycle (A -> B -> A) — node PKs repeat; edges repeat too.
// nodes() must list ALL occurrences including repeats; edges() must list all edges.
// ---------------------------------------------------------------------------

TEST(QA_GDB829_CyclicPath, NodesIncludesRepeats) {
    // A --e1--> B --e2--> A  (cycle)
    Path p;
    p.steps.push_back({10, 77});
    p.steps.push_back({20, 88});
    p.steps.push_back({10, -1}); // A again as terminal

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};

    auto expr = make_path_func("NODES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Both occurrences of node 10 must be present
    EXPECT_EQ(result->as_json().data, "[10,20,10]");
}

TEST(QA_GDB829_CyclicPath, EdgesIncludesBothEdges) {
    Path p;
    p.steps.push_back({10, 77});
    p.steps.push_back({20, 88});
    p.steps.push_back({10, -1});

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};

    auto expr = make_path_func("EDGES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_json().data, "[77,88]");
}

TEST(QA_GDB829_CyclicPath, LongerCycleA_B_C_A) {
    // A --e1--> B --e2--> C --e3--> A  (3-hop cycle)
    Path p;
    p.steps.push_back({1, 11});
    p.steps.push_back({2, 22});
    p.steps.push_back({3, 33});
    p.steps.push_back({1, -1}); // A again

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};

    auto nodes_expr = make_path_func("NODES");
    auto nodes_result = evaluate_expr(*nodes_expr, tuple, schema, bound);
    ASSERT_TRUE(nodes_result.has_value()) << nodes_result.error().message;
    EXPECT_EQ(nodes_result->as_json().data, "[1,2,3,1]");

    // Re-build tuple (moved)
    Path p2;
    p2.steps.push_back({1, 11});
    p2.steps.push_back({2, 22});
    p2.steps.push_back({3, 33});
    p2.steps.push_back({1, -1});
    auto tuple2 = make_path_tuple(std::move(p2));

    auto edges_expr = make_path_func("EDGES");
    auto edges_result = evaluate_expr(*edges_expr, tuple2, schema, bound);
    ASSERT_TRUE(edges_result.has_value()) << edges_result.error().message;
    EXPECT_EQ(edges_result->as_json().data, "[11,22,33]");
}

// ---------------------------------------------------------------------------
// Suite: GDB829_LongerPaths
// Multi-hop paths (4+ hops) to stress count correctness.
// ---------------------------------------------------------------------------

TEST(QA_GDB829_LongerPaths, FourHopNodes) {
    Path p;
    p.steps.push_back({1, 10});
    p.steps.push_back({2, 20});
    p.steps.push_back({3, 30});
    p.steps.push_back({4, 40});
    p.steps.push_back({5, -1});

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};

    auto expr = make_path_func("NODES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_json().data, "[1,2,3,4,5]");
}

TEST(QA_GDB829_LongerPaths, FourHopEdges) {
    Path p;
    p.steps.push_back({1, 10});
    p.steps.push_back({2, 20});
    p.steps.push_back({3, 30});
    p.steps.push_back({4, 40});
    p.steps.push_back({5, -1});

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};

    auto expr = make_path_func("EDGES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_json().data, "[10,20,30,40]");
}

// ---------------------------------------------------------------------------
// Suite: GDB829_NullInput
// NULL path argument must return NULL (not crash).
// ---------------------------------------------------------------------------

TEST(QA_GDB829_NullInput, NodesOnNullReturnsNull) {
    auto schema = make_path_schema();
    auto tuple = make_null_tuple();
    BoundStatement bound{};

    auto expr = make_path_func("NODES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->is_null());
}

TEST(QA_GDB829_NullInput, EdgesOnNullReturnsNull) {
    auto schema = make_path_schema();
    auto tuple = make_null_tuple();
    BoundStatement bound{};

    auto expr = make_path_func("EDGES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->is_null());
}

// ---------------------------------------------------------------------------
// Suite: GDB829_TypeError
// Non-PATH argument must return a TYPE_ERROR, not crash.
// ---------------------------------------------------------------------------

TEST(QA_GDB829_TypeError, NodesOnIntTypeError) {
    auto schema = make_int_schema();
    auto tuple = make_int_tuple(42);
    BoundStatement bound{};

    auto expr = make_path_func("NODES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    // Must be an error (not a crash, not success with garbage)
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB829_TypeError, EdgesOnIntTypeError) {
    auto schema = make_int_schema();
    auto tuple = make_int_tuple(42);
    BoundStatement bound{};

    auto expr = make_path_func("EDGES");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// ---------------------------------------------------------------------------
// Suite: GDB829_RelationshipsAlias
// relationships() is an alias for edges() — same semantics.
// ---------------------------------------------------------------------------

TEST(QA_GDB829_RelationshipsAlias, RelationshipsSameAsEdges) {
    Path p;
    p.steps.push_back({1, 55});
    p.steps.push_back({2, 66});
    p.steps.push_back({3, -1});

    auto schema = make_path_schema();
    auto tuple = make_path_tuple(std::move(p));
    BoundStatement bound{};

    auto expr = make_path_func("RELATIONSHIPS");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_json().data, "[55,66]");
}

TEST(QA_GDB829_RelationshipsAlias, RelationshipsNullReturnsNull) {
    auto schema = make_path_schema();
    auto tuple = make_null_tuple();
    BoundStatement bound{};

    auto expr = make_path_func("RELATIONSHIPS");
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->is_null());
}
