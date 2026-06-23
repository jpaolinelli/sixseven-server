// QA regression tests for GDB-920: Pin CycleGraph betweenness to exact value 1.0
//
// Acceptance criteria:
// AC1: EXPECT_NEAR(scores[1], 1.0, 1e-10) is present and asserts correctly.
// AC2: EXPECT_NEAR(scores[2], 1.0, 1e-10) is present and asserts correctly.
// AC3: EXPECT_NEAR(scores[3], 1.0, 1e-10) is present and asserts correctly.
// AC4: All-zero degenerate output would be caught (mutation grade).
// AC5: Sibling tests (LinearChain, DiamondGraph, etc.) remain unbroken.
//
// These tests call the same betweenness_centrality_execute function that the
// unit test exercises, and pin the CycleGraph invariants independently.

#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/betweenness_centrality.h"
#include "sixseven/graph/graph_engine.h"

#include <gtest/gtest.h>

#include <unordered_map>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

static TableSchema make_node_schema() {
    TableSchema schema;
    schema.name = "nodes";
    schema.columns = {{0, "id", TypeId::INT64, false, ""}};
    schema.pk_columns = "id";
    return schema;
}

static Value pk(int64_t v) {
    return Value(v);
}

std::unordered_map<int64_t, double> centrality_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, double> m;
    for (const auto& row : rows) {
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto score = std::get<double>(row.values[1].data());
        m[node_id] = score;
    }
    return m;
}

} // namespace

class QA_GDB920 : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        auto t = catalog_.create_table(default_database_id, make_node_schema());
        ASSERT_TRUE(t.has_value()) << t.error().message;
        table_id_ = *t;
    }

    void build_edges(const std::string& edge_type,
                     const std::vector<std::pair<int64_t, int64_t>>& edges) {
        auto et = engine_.create_edge_type(
            default_database_id, edge_type, table_id_, table_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(et.has_value()) << et.error().message;
        for (auto [src, tgt] : edges) {
            auto link = engine_.link(default_database_id, edge_type, pk(src), pk(tgt));
            ASSERT_TRUE(link.has_value()) << link.error().message;
        }
    }

    Result<std::vector<AlgorithmRow>> run_unnormalized(const std::string& et) {
        AlgorithmContext ctx{engine_, default_database_id, et, {{"normalized", Value(false)}}};
        return betweenness_centrality_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// AC1/AC2/AC3: each node in a directed 3-cycle has unnormalized betweenness == 1.0
TEST_F(QA_GDB920, CycleGraph_EachNodeScoresOne) {
    build_edges("c3", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run_unnormalized("c3");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = centrality_map(*result);
    ASSERT_EQ(scores.size(), 3u);

    EXPECT_NEAR(scores[1], 1.0, 1e-10);
    EXPECT_NEAR(scores[2], 1.0, 1e-10);
    EXPECT_NEAR(scores[3], 1.0, 1e-10);
}

// AC4 mutation grade: scores must be > 0.5 so all-zero would fail
TEST_F(QA_GDB920, CycleGraph_ScoresAboveHalf) {
    build_edges("c3b", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run_unnormalized("c3b");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = centrality_map(*result);
    ASSERT_EQ(scores.size(), 3u);

    for (int64_t n : {1, 2, 3}) {
        EXPECT_GT(scores[n], 0.5) << "node " << n
                                  << " score must exceed 0.5; all-zero regression would fail here";
    }
}

// AC5: symmetry -- all three nodes have equal scores (cycle symmetry)
TEST_F(QA_GDB920, CycleGraph_ScoresAreEqual) {
    build_edges("c3c", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run_unnormalized("c3c");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = centrality_map(*result);
    ASSERT_EQ(scores.size(), 3u);

    EXPECT_NEAR(scores[1], scores[2], 1e-10);
    EXPECT_NEAR(scores[2], scores[3], 1e-10);
}

// AC5 sibling: DiamondGraph scores 0.5 each (regression guard for sibling test)
TEST_F(QA_GDB920, DiamondGraph_SiblingUnbroken) {
    build_edges("diamond", {{1, 2}, {1, 3}, {2, 4}, {3, 4}});

    auto result = run_unnormalized("diamond");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = centrality_map(*result);
    ASSERT_EQ(scores.size(), 4u);

    EXPECT_DOUBLE_EQ(scores[1], 0.0);
    EXPECT_NEAR(scores[2], 0.5, 1e-10);
    EXPECT_NEAR(scores[3], 0.5, 1e-10);
    EXPECT_DOUBLE_EQ(scores[4], 0.0);
}

// Boundary: 4-cycle 1->2->3->4->1.
// For each node, count ordered pairs (s,t) where it is an intermediary:
//   node 1: (4,2), (4,3), (3,2)         = 3
//   node 2: (1,3), (1,4), (4,3)         = 3
//   node 3: (2,4), (2,1), (1,4)         = 3
//   node 4: (3,1), (3,2), (2,1)         = 3
// All nodes are symmetric in the 4-cycle -> unnormalized betweenness = 3.0 each.
TEST_F(QA_GDB920, FourCycle_ScoresSymmetric) {
    build_edges("c4", {{1, 2}, {2, 3}, {3, 4}, {4, 1}});

    auto result = run_unnormalized("c4");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = centrality_map(*result);
    ASSERT_EQ(scores.size(), 4u);

    // All scores must be equal (cyclic symmetry).
    EXPECT_NEAR(scores[1], scores[2], 1e-10);
    EXPECT_NEAR(scores[2], scores[3], 1e-10);
    EXPECT_NEAR(scores[3], scores[4], 1e-10);

    // And equal to the analytically derived value of 3.0.
    for (int64_t n : {1, 2, 3, 4}) {
        EXPECT_NEAR(scores[n], 3.0, 1e-10)
            << "node " << n << " in directed 4-cycle should have unnormalized betweenness == 3.0";
    }
}
