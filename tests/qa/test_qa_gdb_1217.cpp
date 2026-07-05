/// @file test_qa_gdb_1217.cpp
/// QA adversarial tests for GDB-1217: behavior-preserving dedup of graph
/// Value-extraction helpers (value_to_int64/double/string) into
/// include/sixseven/graph/value_extract.{h,cpp}.
///
/// Verifies:
///   AC: FIRST verify against current source (real duplication confirmed).
///   AC: Fix with regression test -- shared helpers used by all 11/3/2
///       call sites, each call site's TYPE_ERROR message text UNCHANGED.
///
/// Adversarial focus (per QA brief):
///   1. Each algorithm's TYPE_ERROR for non-integer node keys carries THAT
///      algorithm's own label, not a generic or swapped one -- spot-checked
///      end-to-end through algorithm _execute() entry points (the same
///      surface the executor calls via the algorithm registry).
///   2. NULL node key -> INVALID_ARGUMENT path unchanged across callers.
///   3. uint64 > INT64_MAX at a node-key call site: silent wraparound
///      preserved, no crash, matches main's pre-dedup behavior.
///   4. No coverage lost vs. the removed inline copies (context-label
///      variation for value_to_double / value_to_string call sites too).

#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/betweenness_centrality.h"
#include "sixseven/graph/closeness_centrality.h"
#include "sixseven/graph/degree_centrality.h"
#include "sixseven/graph/eigenvector_centrality.h"
#include "sixseven/graph/harmonic_centrality.h"
#include "sixseven/graph/pagerank.h"
#include "sixseven/graph/triangle_count.h"
#include "sixseven/graph/value_extract.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>

#include "graph_qa_fixture.h"

namespace sixseven {
namespace {

using graph_qa::GraphQaFixtureBase;

// ============================================================================
// Fixture: builds a "nodes" table + STRING-keyed edge type so that node
// PKs stored in edges are non-integer, forcing value_to_int64 to hit the
// TYPE_ERROR branch inside each algorithm's _execute().
// ============================================================================

class Gdb1217StringKeyedGraph : public GraphQaFixtureBase {
protected:
    void SetUp() override {
        // NOTE: GraphQaFixtureBase::SetUp() calls catalog_.create_table(default_database_id, ...)
        // but the Catalog only auto-registers system_database_id in its constructor, not
        // default_database_id (see src/catalog/catalog.cpp Catalog::Catalog()). This is a
        // pre-existing gap in the shared fixture (confirmed present on main, predates
        // GDB-1217; last touched by GDB-1158) -- not something introduced by this ticket.
        // Register the database explicitly here so our tests are self-contained.
        // restore_database() lets us pin the id to default_database_id (1), which is
        // what GraphQaFixtureBase::SetUp() / AlgorithmContext hard-code.
        auto db = catalog_.restore_database(default_database_id, "gdb1217_test_db");
        ASSERT_TRUE(db.has_value()) << db.error().message;
        GraphQaFixtureBase::SetUp();
        auto et = engine_.create_edge_type(default_database_id,
                                           "knows_str",
                                           table_id_,
                                           table_id_,
                                           TypeId::STRING,
                                           TypeId::STRING,
                                           {});
        ASSERT_TRUE(et.has_value()) << et.error().message;

        auto link = engine_.link(
            default_database_id, "knows_str", Value(std::string("alice")), Value(std::string("bob")));
        ASSERT_TRUE(link.has_value()) << link.error().message;
    }

    AlgorithmContext ctx() {
        return AlgorithmContext{engine_, default_database_id, "knows_str", {}};
    }
};

// ----------------------------------------------------------------------
// 1. Per-algorithm TYPE_ERROR label correctness (end-to-end via _execute)
// ----------------------------------------------------------------------

TEST_F(Gdb1217StringKeyedGraph, DegreeCentralityUsesOwnLabel) {
    auto result = degree_centrality_execute(ctx());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    EXPECT_EQ(result.error().message, "degree centrality requires integer node keys");
}

TEST_F(Gdb1217StringKeyedGraph, BetweennessCentralityUsesOwnLabel) {
    auto result = betweenness_centrality_execute(ctx());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    EXPECT_EQ(result.error().message, "betweenness centrality requires integer node keys");
}

TEST_F(Gdb1217StringKeyedGraph, ClosenessCentralityUsesOwnLabel) {
    auto result = closeness_centrality_execute(ctx());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    EXPECT_EQ(result.error().message, "closeness centrality requires integer node keys");
}

TEST_F(Gdb1217StringKeyedGraph, HarmonicCentralityUsesOwnLabel) {
    auto result = harmonic_centrality_execute(ctx());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    EXPECT_EQ(result.error().message, "harmonic centrality requires integer node keys");
}

TEST_F(Gdb1217StringKeyedGraph, EigenvectorCentralityUsesOwnLabel) {
    auto result = eigenvector_centrality_execute(ctx());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    EXPECT_EQ(result.error().message, "eigenvector centrality requires integer node keys");
}

TEST_F(Gdb1217StringKeyedGraph, PagerankUsesOwnLabel) {
    auto result = pagerank_execute(ctx());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    EXPECT_EQ(result.error().message, "pagerank requires integer node keys");
}

TEST_F(Gdb1217StringKeyedGraph, TriangleCountUsesOwnLabel) {
    auto result = triangle_count_execute(ctx());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    EXPECT_EQ(result.error().message, "triangle count requires integer node keys");
}

// Cross-check: labels must NOT be swapped/generic across algorithms.
TEST_F(Gdb1217StringKeyedGraph, LabelsAreNotCrossContaminated) {
    auto degree_result = degree_centrality_execute(ctx());
    auto pagerank_result = pagerank_execute(ctx());
    ASSERT_FALSE(degree_result.has_value());
    ASSERT_FALSE(pagerank_result.has_value());
    EXPECT_NE(degree_result.error().message, pagerank_result.error().message);
    EXPECT_NE(degree_result.error().message, "generic error");
    EXPECT_NE(pagerank_result.error().message, "generic error");
}

// ----------------------------------------------------------------------
// 2. NULL node key -> INVALID_ARGUMENT unchanged across callers
// ----------------------------------------------------------------------

TEST(Gdb1217NullNodeKey, DirectCallReturnsInvalidArgumentNotTypeError) {
    auto result = value_to_int64(Value(), "any centrality");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(result.error().message, "NULL node key in edge");
}

TEST(Gdb1217NullNodeKey, MessageDoesNotEmbedContextForNullPath) {
    // The NULL branch message is context-independent (matches all 11
    // pre-dedup copies) -- verify context is NOT accidentally spliced in.
    auto result = value_to_int64(Value(), "pagerank");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "NULL node key in edge");
    EXPECT_EQ(result.error().message.find("pagerank"), std::string::npos);
}

TEST(Gdb1217NullNodeKey, DoubleNullPathIndependentOfContext) {
    auto result = value_to_double(Value(), "damping parameter");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(result.error().message, "NULL parameter value");
}

TEST(Gdb1217NullNodeKey, StringNullPathIndependentOfContext) {
    auto result = value_to_string(Value(), "direction parameter");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(result.error().message, "NULL parameter value");
}

// ----------------------------------------------------------------------
// 3. uint64 > INT64_MAX silent wraparound preserved (no crash, no change)
// ----------------------------------------------------------------------

TEST(Gdb1217Uint64Wraparound, ExactlyIntMaxPlusOneWrapsToIntMin) {
    const uint64_t v = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1;
    auto result = value_to_int64(Value(v), "pagerank");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::numeric_limits<int64_t>::min());
}

TEST(Gdb1217Uint64Wraparound, Uint64MaxWrapsToNegativeOne) {
    const uint64_t v = std::numeric_limits<uint64_t>::max();
    auto result = value_to_int64(Value(v), "triangle count");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, -1);
}

TEST(Gdb1217Uint64Wraparound, ExactlyIntMaxDoesNotWrap) {
    const uint64_t v = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    auto result = value_to_int64(Value(v), "betweenness centrality");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::numeric_limits<int64_t>::max());
}

TEST(Gdb1217Uint64Wraparound, NoCrashAndSucceedsAsBeforeAcrossContexts) {
    // Same huge value, different call-site contexts: identical numeric
    // outcome, only bearing on message text if it were an error (it is not).
    const uint64_t v = std::numeric_limits<uint64_t>::max() - 5;
    auto r1 = value_to_int64(Value(v), "degree centrality");
    auto r2 = value_to_int64(Value(v), "connected_components");
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r1, *r2);
}

// ----------------------------------------------------------------------
// 4. Coverage parity: double/string extraction context labels
// ----------------------------------------------------------------------

TEST(Gdb1217CoverageParity, DoubleContextLabelsVaryByCallSite) {
    auto damping = value_to_double(Value(std::string("x")), "damping parameter");
    auto tolerance = value_to_double(Value(std::string("x")), "tolerance parameter");
    ASSERT_FALSE(damping.has_value());
    ASSERT_FALSE(tolerance.has_value());
    EXPECT_EQ(damping.error().message, "expected numeric value for damping parameter");
    EXPECT_EQ(tolerance.error().message, "expected numeric value for tolerance parameter");
    EXPECT_NE(damping.error().message, tolerance.error().message);
}

TEST(Gdb1217CoverageParity, StringContextLabelsVaryByCallSite) {
    auto direction = value_to_string(Value(static_cast<int64_t>(1)), "direction parameter");
    auto variant = value_to_string(Value(static_cast<int64_t>(1)), "variant parameter");
    ASSERT_FALSE(direction.has_value());
    ASSERT_FALSE(variant.has_value());
    EXPECT_EQ(direction.error().message, "expected string value for direction parameter");
    EXPECT_EQ(variant.error().message, "expected string value for variant parameter");
    EXPECT_NE(direction.error().message, variant.error().message);
}

TEST(Gdb1217CoverageParity, EmptyContextStringDoesNotCrash) {
    auto result = value_to_int64(Value(std::string("x")), "");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, " requires integer node keys");
}

TEST(Gdb1217CoverageParity, BoolValueRejectedByInt64WithCorrectContext) {
    auto result = value_to_int64(Value(true), "louvain");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    EXPECT_EQ(result.error().message, "louvain requires integer node keys");
}

TEST(Gdb1217CoverageParity, BoolValueAcceptedByDoubleAsArithmetic) {
    // is_arithmetic_v<bool> is true in C++, so value_to_double must accept it
    // (matches original inline behavior) -- guards against a refactor
    // accidentally special-casing bool out.
    auto result = value_to_double(Value(true), "ctx");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(*result, 1.0);
}

} // namespace
} // namespace sixseven
