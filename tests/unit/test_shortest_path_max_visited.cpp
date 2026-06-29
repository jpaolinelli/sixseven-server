#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
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

// Fixture that builds a small star graph:
//   hub (1) -> spoke (2), hub (1) -> spoke (3), hub (1) -> spoke (4)
// Bidirectional BFS seeded at node 1 (fwd) and node 4 (bwd) will insert
// at least 2 nodes (node 1 and node 4) before finding the path, so
// max_visited=1 triggers the guard.
class ShortestPathMaxVisitedTest : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

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
        table_id_ = *tid;

        auto eid = graph_->create_edge_type(
            default_database_id, "links", table_id_, table_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // Build star: 1->2, 1->3, 1->4
        link(1, 2);
        link(1, 3);
        link(1, 4);
    }

    void link(int64_t from, int64_t to) {
        auto r = graph_->link(default_database_id, "links", Value(from), Value(to));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    ShortestPathConfig make_config(int64_t from, int64_t to, size_t max_visited) {
        ShortestPathConfig cfg;
        cfg.database_id = default_database_id;
        cfg.edge_type = "links";
        cfg.from_key = Value(from);
        cfg.to_key = Value(to);
        cfg.direction = TraverseDirection::OUT;
        cfg.max_depth = 10;
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

// Negative test: max_visited=1 is too small for a 2-node path (1->4).
// Bidirectional BFS seeds node 1 into fwd_visited and node 4 into bwd_visited
// (total=2 > 1) on the very first expansion, so the guard fires before a path
// can be returned.
//
// Mutation check: WITHOUT the guard, op.open() succeeds and returns a valid
// path (the negative assertion fails). WITH the guard, op.open() returns an
// error with INVALID_ARGUMENT and message containing "max_visited" (passes).
TEST_F(ShortestPathMaxVisitedTest, ExceedMaxVisitedErrors) {
    auto cfg = make_config(1, 4, 1);
    ShortestPathOperator op(*graph_, std::move(cfg), make_schema());

    auto result = op.open();
    ASSERT_FALSE(result.has_value())
        << "Expected error when max_visited exceeded, but op.open() succeeded";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(result.error().message.find("max_visited"), std::string::npos)
        << "Error message: " << result.error().message;
}

// Positive test: with a generous max_visited the operator finds the path 1->4
// and returns it correctly. This confirms the guard does not fire on normal
// searches.
TEST_F(ShortestPathMaxVisitedTest, NormalSearchSucceedsWithLargeMaxVisited) {
    auto cfg = make_config(1, 4, 100000);
    ShortestPathOperator op(*graph_, std::move(cfg), make_schema());

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<std::pair<int64_t, int64_t>> path;
    while (true) {
        auto next_result = op.next();
        ASSERT_TRUE(next_result.has_value()) << next_result.error().message;
        if (!next_result->has_value()) {
            break;
        }
        const auto& tup = **next_result;
        ASSERT_EQ(tup.values.size(), 2u);
        path.emplace_back(tup.values[0].as_int64(), tup.values[1].as_int64());
    }
    op.close();

    // Path 1->4 is a single hop: [(1, 0), (4, 1)]
    ASSERT_EQ(path.size(), 2u);
    EXPECT_EQ(path[0].first, 1);
    EXPECT_EQ(path[0].second, 0);
    EXPECT_EQ(path[1].first, 4);
    EXPECT_EQ(path[1].second, 1);
}

} // namespace
} // namespace sixseven
