#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/shortest_path.h"
#include "sixseven/graph/graph_engine.h"

#include <gtest/gtest.h>

#include <optional>
#include <vector>

namespace sixseven {
namespace {

/// Test fixture for shortest path tests.
///
/// Graph topology:
///   1 → 2 → 3 → 6
///   1 → 4 → 5 → 6
///   (Two paths from 1 to 6, both length 3)
class ShortestPathTest : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_ = std::make_unique<Catalog>();
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

        auto eid = graph_->create_edge_type(
            default_database_id, "knows", *tid, *tid, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        link(1, 2);
        link(2, 3);
        link(3, 6);
        link(1, 4);
        link(4, 5);
        link(5, 6);
    }

    void link(int64_t from, int64_t to) {
        auto r = graph_->link(default_database_id, "knows", Value(from), Value(to));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    std::vector<int64_t> find_path(int64_t from,
                                   int64_t to,
                                   TraverseDirection dir = TraverseDirection::OUT,
                                   int32_t max_depth = 100) {
        ShortestPathConfig config;
        config.edge_type = "knows";
        config.from_key = Value(from);
        config.to_key = Value(to);
        config.direction = dir;
        config.max_depth = max_depth;

        std::vector<OutputColumn> cols;
        cols.push_back({"", "node", TypeId::INT64, false, 0});
        cols.push_back({"", "hop", TypeId::INT64, false, 0});
        OutputSchema schema(std::move(cols));

        ShortestPathOperator op(*graph_, std::move(config), std::move(schema));

        auto open_result = op.open();
        EXPECT_TRUE(open_result.has_value()) << open_result.error().message;

        std::vector<int64_t> path;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row->has_value()) {
                break;
            }
            path.push_back(row->value().values[0].as_int64());
        }
        op.close();
        return path;
    }

    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<GraphEngine> graph_;
};

TEST_F(ShortestPathTest, DirectConnection) {
    auto path = find_path(1, 2);
    ASSERT_EQ(path.size(), 2);
    EXPECT_EQ(path[0], 1);
    EXPECT_EQ(path[1], 2);
}

TEST_F(ShortestPathTest, MultiHopPath) {
    auto path = find_path(1, 6);
    // Shortest path from 1 to 6 is length 3 (either 1→2→3→6 or 1→4→5→6).
    ASSERT_EQ(path.size(), 4);
    EXPECT_EQ(path[0], 1);
    EXPECT_EQ(path[3], 6);
}

TEST_F(ShortestPathTest, SameNode) {
    auto path = find_path(1, 1);
    // Source equals target.
    ASSERT_EQ(path.size(), 1);
    EXPECT_EQ(path[0], 1);
}

TEST_F(ShortestPathTest, NoPathExists) {
    auto path = find_path(6, 1);
    // No path from 6 to 1 (all edges are forward-only).
    EXPECT_TRUE(path.empty());
}

TEST_F(ShortestPathTest, NoPathWithMaxDepth) {
    auto path = find_path(1, 6, TraverseDirection::OUT, 2);
    // Path requires 3 hops but max_depth is 2.
    EXPECT_TRUE(path.empty());
}

TEST_F(ShortestPathTest, ReverseDirection) {
    auto path = find_path(6, 1, TraverseDirection::IN);
    // Following IN direction from 6: 6←3←2←1 or 6←5←4←1
    ASSERT_EQ(path.size(), 4);
    EXPECT_EQ(path[0], 6);
    EXPECT_EQ(path[3], 1);
}

TEST_F(ShortestPathTest, BothDirection) {
    // Add reverse edge: 6 → 7
    link(6, 7);
    // With BOTH direction, should find path from 7 to 1.
    auto path = find_path(7, 1, TraverseDirection::BOTH);
    ASSERT_FALSE(path.empty());
    EXPECT_EQ(path[0], 7);
    EXPECT_EQ(path.back(), 1);
}

TEST_F(ShortestPathTest, NonexistentNodes) {
    auto path = find_path(100, 200);
    EXPECT_TRUE(path.empty());
}

} // namespace
} // namespace sixseven
