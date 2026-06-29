/// @file test_qa_gdb_995.cpp
/// Adversarial QA for GDB-995: ShortestPathOperator::run_bidirectional_bfs
/// must enforce config_.max_visited and return INVALID_ARGUMENT on exceed.
///
/// Attack surface:
///   - max_visited=0: size_t underflow risk (guard is >, not >= max+1); seeds
///     are inserted before the inner loop so total is already 2 before the
///     first neighbor is examined -- guard fires on first neighbor insert.
///   - max_visited exactly equal to seeds (2): guard fires on first expansion.
///   - max_visited at natural visit count for a direct path: should PASS.
///   - Disconnected graph with small max_visited: must return empty, not hang.
///   - Trivial case (from==to): path is returned before BFS runs; must succeed
///     even with max_visited=0.
///   - Determinism: same result on two consecutive calls.
///   - Correct path for normal search (regression guard).

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/shortest_path.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// Shared fixture
// ---------------------------------------------------------------------------
// Graph topology:
//   1 -> 2, 1 -> 3, 1 -> 4   (star from hub=1)
//   5 is an isolated node (no edges)
// ---------------------------------------------------------------------------
class QA_GDB995 : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        TableSchema ts;
        ts.name = "qa995_nodes";
        CatalogColumnDef pk_col;
        pk_col.ordinal = 0;
        pk_col.name = "id";
        pk_col.type_id = TypeId::INT64;
        pk_col.nullable = false;
        ts.columns.push_back(pk_col);
        ts.pk_columns = "id";
        auto tid = catalog_->create_table(default_database_id, std::move(ts));
        ASSERT_TRUE(tid.has_value()) << tid.error().message;
        table_id_ = *tid;

        auto eid = graph_->create_edge_type(default_database_id,
                                            "qa995_links",
                                            table_id_,
                                            table_id_,
                                            TypeId::INT64,
                                            TypeId::INT64,
                                            {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // Star: 1->2, 1->3, 1->4
        link(1, 2);
        link(1, 3);
        link(1, 4);
        // Node 5 remains isolated (no edges inserted)
    }

    void link(int64_t from, int64_t to) {
        auto r = graph_->link(default_database_id, "qa995_links", Value(from), Value(to));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    ShortestPathConfig
    make_config(int64_t from, int64_t to, size_t max_visited, int32_t max_depth = 10) {
        ShortestPathConfig cfg;
        cfg.database_id = default_database_id;
        cfg.edge_type = "qa995_links";
        cfg.from_key = Value(from);
        cfg.to_key = Value(to);
        cfg.direction = TraverseDirection::OUT;
        cfg.max_depth = max_depth;
        cfg.max_visited = max_visited;
        cfg.heterogeneous = false;
        cfg.source_table_id = table_id_;
        cfg.target_table_id = table_id_;
        return cfg;
    }

    OutputSchema make_schema() {
        std::vector<OutputColumn> cols;
        cols.push_back({"", "node", TypeId::INT64, false, 0});
        cols.push_back({"", "hop", TypeId::INT64, false, 0});
        return OutputSchema(std::move(cols));
    }

    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t table_id_{};
};

// ---------------------------------------------------------------------------
// AC: guard fires and returns INVALID_ARGUMENT + "max_visited" in message
// (already covered by dev test; re-verify in QA suite for completeness)
// ---------------------------------------------------------------------------
TEST_F(QA_GDB995, ExceedMaxVisitedReturnsInvalidArgument) {
    auto cfg = make_config(1, 4, 1);
    ShortestPathOperator op(*graph_, std::move(cfg), make_schema());
    auto result = op.open();
    ASSERT_FALSE(result.has_value()) << "Expected INVALID_ARGUMENT when max_visited=1 exceeded";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(result.error().message.find("max_visited"), std::string::npos)
        << "Error message missing 'max_visited': " << result.error().message;
}

// ---------------------------------------------------------------------------
// Boundary: max_visited=0
// size_t is unsigned: 0 must NOT cause underflow.  The guard is
//   fwd_visited.size() + bwd_visited.size() > config_.max_visited
// Seeds (lines 91,99 in shortest_path.cpp) insert 1 node each before the
// inner loop runs, giving total=2 > 0 on the first neighbor insertion.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB995, MaxVisitedZeroErrorsCleanly) {
    auto cfg = make_config(1, 4, 0);
    ShortestPathOperator op(*graph_, std::move(cfg), make_schema());
    auto result = op.open();
    ASSERT_FALSE(result.has_value()) << "max_visited=0 should always error (seeds alone exceed it)";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    // Sanity: no crash / no UB from size_t arithmetic
}

// ---------------------------------------------------------------------------
// Boundary: max_visited exactly equals seed count (2).
// Both fwd_visited and bwd_visited seed 1 node each (total=2).
// The first neighbor inserted makes total=3 > 2 -> error.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB995, MaxVisitedEqualToSeedCountErrors) {
    auto cfg = make_config(1, 4, 2);
    ShortestPathOperator op(*graph_, std::move(cfg), make_schema());
    auto result = op.open();
    ASSERT_FALSE(result.has_value())
        << "max_visited=2 (equal to seed count) should error on first expansion";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Boundary: max_visited generous enough for 1->4 to succeed.
// The guard fires when fwd+bwd > max_visited after inserting each neighbor.
// The star graph: node1 has neighbors {2,3,4}. BFS expands node1's full
// level before finding the meeting at node4. With 3 neighbors inserted into
// fwd_visited (node2, node3, node4) plus 1 seed each side (node1, node4),
// the minimum safe max_visited is empirically >=5 (fwd=4 after node4 insert
// + bwd=1 = 5; 5>5 false). Use max_visited=5 to confirm the strict boundary.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB995, MaxVisitedAtNaturalCountSucceeds) {
    auto cfg = make_config(1, 4, 5);
    ShortestPathOperator op(*graph_, std::move(cfg), make_schema());
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value())
        << "max_visited=5 should succeed for 1->4 (strict > semantics): "
        << open_result.error().message;

    std::vector<std::pair<int64_t, int64_t>> path;
    while (true) {
        auto next_result = op.next();
        ASSERT_TRUE(next_result.has_value());
        if (!next_result->has_value())
            break;
        const auto& tup = **next_result;
        ASSERT_EQ(tup.values.size(), 2u);
        path.emplace_back(tup.values[0].as_int64(), tup.values[1].as_int64());
    }
    op.close();

    ASSERT_EQ(path.size(), 2u) << "Expected 2-hop path [(1,0),(4,1)]";
    EXPECT_EQ(path[0].first, 1);
    EXPECT_EQ(path[0].second, 0);
    EXPECT_EQ(path[1].first, 4);
    EXPECT_EQ(path[1].second, 1);
}

// ---------------------------------------------------------------------------
// Disconnected graph: node 1 -> {2,3,4}; node 5 isolated.
// With small max_visited=1, BFS expands and errors before exhausting the
// search. Confirm: no path exists between 1 and 5, but also no hang/UB.
// The guard must trigger (error) before "no path" can be concluded.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB995, DisconnectedGraphSmallMaxVisitedErrors) {
    // max_visited=1: seeds give total=2 > 1 on first expansion -> error
    auto cfg = make_config(1, 5, 1);
    ShortestPathOperator op(*graph_, std::move(cfg), make_schema());
    auto result = op.open();
    ASSERT_FALSE(result.has_value())
        << "Disconnected graph with max_visited=1 should error, not hang";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Disconnected graph with generous max_visited: must return empty result set
// (no path), not error.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB995, DisconnectedGraphGenerousMaxVisitedReturnsEmpty) {
    auto cfg = make_config(1, 5, 100000);
    ShortestPathOperator op(*graph_, std::move(cfg), make_schema());
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value())
        << "Disconnected graph with large max_visited should succeed (no path): "
        << open_result.error().message;

    int row_count = 0;
    while (true) {
        auto next_result = op.next();
        ASSERT_TRUE(next_result.has_value());
        if (!next_result->has_value())
            break;
        ++row_count;
    }
    op.close();

    EXPECT_EQ(row_count, 0) << "No path from 1 to 5 should yield 0 rows";
}

// ---------------------------------------------------------------------------
// Trivial case (from == to): path is returned before BFS runs.
// max_visited=0 must NOT cause an error here since the guard is inside the
// BFS inner loop which is never reached on the trivial path.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB995, TrivialPathFromEqualsToIgnoresMaxVisited) {
    auto cfg = make_config(1, 1, 0); // max_visited=0; guard inside BFS loop
    ShortestPathOperator op(*graph_, std::move(cfg), make_schema());
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value())
        << "from==to trivial path must succeed regardless of max_visited=0: "
        << open_result.error().message;

    // Should emit exactly one row: (1, 0)
    auto next_result = op.next();
    ASSERT_TRUE(next_result.has_value());
    ASSERT_TRUE(next_result->has_value()) << "Expected one tuple for trivial path";
    const auto& tup = **next_result;
    ASSERT_EQ(tup.values.size(), 2u);
    EXPECT_EQ(tup.values[0].as_int64(), 1);
    EXPECT_EQ(tup.values[1].as_int64(), 0);

    // No more rows
    auto end_result = op.next();
    ASSERT_TRUE(end_result.has_value());
    EXPECT_FALSE(end_result->has_value()) << "Expected only 1 row for trivial path";

    op.close();
}

// ---------------------------------------------------------------------------
// Determinism: run the same query twice; both produce identical errors.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB995, DeterministicErrorOnRepeatedCall) {
    for (int run = 0; run < 2; ++run) {
        auto cfg = make_config(1, 4, 1);
        ShortestPathOperator op(*graph_, std::move(cfg), make_schema());
        auto result = op.open();
        ASSERT_FALSE(result.has_value()) << "Run " << run << ": expected error";
        EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT)
            << "Run " << run << ": wrong status code";
        EXPECT_NE(result.error().message.find("max_visited"), std::string::npos)
            << "Run " << run << ": message missing 'max_visited'";
    }
}

// ---------------------------------------------------------------------------
// Determinism: run the same successful query twice; both produce identical
// paths.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB995, DeterministicSuccessOnRepeatedCall) {
    for (int run = 0; run < 2; ++run) {
        auto cfg = make_config(1, 4, 100000);
        ShortestPathOperator op(*graph_, std::move(cfg), make_schema());

        auto open_result = op.open();
        ASSERT_TRUE(open_result.has_value())
            << "Run " << run << ": " << open_result.error().message;

        std::vector<std::pair<int64_t, int64_t>> path;
        while (true) {
            auto next_result = op.next();
            ASSERT_TRUE(next_result.has_value());
            if (!next_result->has_value())
                break;
            const auto& tup = **next_result;
            path.emplace_back(tup.values[0].as_int64(), tup.values[1].as_int64());
        }
        op.close();

        ASSERT_EQ(path.size(), 2u) << "Run " << run;
        EXPECT_EQ(path[0].first, 1) << "Run " << run;
        EXPECT_EQ(path[1].first, 4) << "Run " << run;
    }
}

// ---------------------------------------------------------------------------
// Error message contains the configured limit value (not a hardcoded string).
// This confirms the impl uses std::to_string(config_.max_visited).
// Use max_visited=2 (known to error: seeds give total=2, first expansion
// makes total=3 > 2 -> error with "2" in message).
// ---------------------------------------------------------------------------
TEST_F(QA_GDB995, ErrorMessageContainsLimitValue) {
    auto cfg = make_config(1, 4, 2);
    ShortestPathOperator op(*graph_, std::move(cfg), make_schema());
    auto result = op.open();
    ASSERT_FALSE(result.has_value())
        << "max_visited=2 should error (seeds=2, first expansion->3>2)";
    EXPECT_NE(result.error().message.find("2"), std::string::npos)
        << "Error message should contain the configured limit (2): " << result.error().message;
}

// ---------------------------------------------------------------------------
// Normal path correctness is not disrupted by the guard.
// A generous max_visited still yields the correct shortest path.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB995, NormalPathNotAffectedByGuard) {
    auto cfg = make_config(1, 4, 100000);
    ShortestPathOperator op(*graph_, std::move(cfg), make_schema());

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<std::pair<int64_t, int64_t>> path;
    while (true) {
        auto next_result = op.next();
        ASSERT_TRUE(next_result.has_value());
        if (!next_result->has_value())
            break;
        const auto& tup = **next_result;
        ASSERT_EQ(tup.values.size(), 2u);
        path.emplace_back(tup.values[0].as_int64(), tup.values[1].as_int64());
    }
    op.close();

    ASSERT_EQ(path.size(), 2u);
    EXPECT_EQ(path[0].first, 1);
    EXPECT_EQ(path[0].second, 0);
    EXPECT_EQ(path[1].first, 4);
    EXPECT_EQ(path[1].second, 1);
}

} // namespace
} // namespace sixseven
