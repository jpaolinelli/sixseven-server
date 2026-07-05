// GDB-1212: Regression test proving EdgeTraversalOperator and
// EnrichedTraversalOperator read heterogeneous-ness from
// TraversalConfig::heterogeneous (the single source of truth) rather than a
// separate, now-removed constructor parameter / member.
//
// Before the fix, these two operators took a redundant `bool heterogeneous`
// constructor argument and stored it in a private `heterogeneous_` member,
// silently ignoring `config.heterogeneous`. A caller who set
// `config.heterogeneous = true` but relied on the operator's own default
// (false) construction would reintroduce the GDB-696 visited-set-seeding bug:
// a heterogeneous target node whose PK collides with the start node's PK
// would be incorrectly dropped as "already visited".
//
// These tests construct the operators directly (bypassing the planner) with
// config.heterogeneous = true and confirm the colliding-PK target is emitted,
// proving config_.heterogeneous now drives the visited-set-seeding decision.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/edge_traversal.h"
#include "sixseven/executor/enriched_traversal.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/traversal.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

// Fixture: two tables ("src" and "dst") linked by a heterogeneous edge type,
// with a target row whose PK (1) collides with the start node's PK (1).
//
//   src: (id INT64 PK) — row 1
//   dst: (id INT64 PK) — rows 1 ("Collider"), 2 ("Other")
//   edge type "crosses" FROM src TO dst — heterogeneous
//   edges: crosses 1->1, crosses 1->2
class EdgeEnrichedHeterogeneousConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        TableSchema src_ts;
        src_ts.name = "src";
        CatalogColumnDef src_pk;
        src_pk.ordinal = 0;
        src_pk.name = "id";
        src_pk.type_id = TypeId::INT64;
        src_pk.nullable = false;
        src_ts.columns.push_back(src_pk);
        src_ts.pk_columns = "id";
        auto src_tid = catalog_->create_table(default_database_id, std::move(src_ts));
        ASSERT_TRUE(src_tid.has_value()) << src_tid.error().message;
        src_table_id_ = *src_tid;

        TableSchema dst_ts;
        dst_ts.name = "dst";
        CatalogColumnDef dst_pk;
        dst_pk.ordinal = 0;
        dst_pk.name = "id";
        dst_pk.type_id = TypeId::INT64;
        dst_pk.nullable = false;
        dst_ts.columns.push_back(dst_pk);
        dst_ts.pk_columns = "id";
        auto dst_tid = catalog_->create_table(default_database_id, std::move(dst_ts));
        ASSERT_TRUE(dst_tid.has_value()) << dst_tid.error().message;
        dst_table_id_ = *dst_tid;

        auto eid = graph_->create_edge_type(default_database_id,
                                            "crosses",
                                            src_table_id_,
                                            dst_table_id_,
                                            TypeId::INT64,
                                            TypeId::INT64,
                                            {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // Colliding target: dst(1), same PK value as src(1) (the start node).
        auto e1 =
            graph_->link(default_database_id, "crosses", Value(int64_t{1}), Value(int64_t{1}));
        ASSERT_TRUE(e1.has_value()) << e1.error().message;
        auto e2 =
            graph_->link(default_database_id, "crosses", Value(int64_t{1}), Value(int64_t{2}));
        ASSERT_TRUE(e2.has_value()) << e2.error().message;

        data_dir_ =
            std::filesystem::temp_directory_path() / "sixseven_test_edge_enriched_hetero_config";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
    }

    void TearDown() override { std::filesystem::remove_all(data_dir_); }

    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t src_table_id_ = 0;
    table_id_t dst_table_id_ = 0;
    std::filesystem::path data_dir_;
};

// EdgeTraversalOperator: config.heterogeneous = true must be honored, so the
// colliding dst(1) target (PK == start PK) is NOT dropped as pre-visited.
TEST_F(EdgeEnrichedHeterogeneousConfigTest, EdgeTraversalHonorsConfigHeterogeneousTrue) {
    TraversalConfig config;
    config.edge_type = "crosses";
    config.start_key = Value(int64_t{1});
    config.direction = TraverseDirection::OUT;
    config.max_depth = 1;
    config.heterogeneous = true;

    std::vector<OutputColumn> cols;
    cols.push_back({"", "__from", TypeId::INT64, false, 0});
    cols.push_back({"", "__to", TypeId::INT64, false, 0});
    cols.push_back({"", "__depth", TypeId::INT64, false, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    EdgeTraversalOperator op(*graph_, std::move(config), std::move(schema), nullptr, bound);
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<int64_t> targets;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value())
            break;
        targets.push_back(row->value().values[1].as_int64());
    }
    op.close();

    std::sort(targets.begin(), targets.end());
    ASSERT_EQ(targets.size(), 2u) << "colliding target dst(1) must not be dropped";
    EXPECT_EQ(targets[0], 1);
    EXPECT_EQ(targets[1], 2);
}

// EnrichedTraversalOperator: same trap, but through the node-centric path.
TEST_F(EdgeEnrichedHeterogeneousConfigTest, EnrichedTraversalHonorsConfigHeterogeneousTrue) {
    DiskManager dm;
    StorageManager storage(dm, data_dir_);

    auto db_storage = storage.create_database_storage(default_database_id);
    ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

    // Materialize the dst table storage and rows.
    auto dst_schema = catalog_->get_table(default_database_id, "dst");
    ASSERT_TRUE(dst_schema.has_value()) << dst_schema.error().message;

    auto create_result =
        storage.create_table_storage(default_database_id, dst_table_id_, *dst_schema);
    ASSERT_TRUE(create_result.has_value()) << create_result.error().message;

    auto dst_storage_result = storage.get_table_storage(dst_table_id_);
    ASSERT_TRUE(dst_storage_result.has_value()) << dst_storage_result.error().message;
    auto* dst_storage = *dst_storage_result;

    for (int64_t pk : {1, 2}) {
        std::vector<Value> row_values{Value(pk)};
        auto serialized = TupleSerializer::serialize(row_values, dst_storage->storage_schema);
        ASSERT_TRUE(serialized.has_value()) << serialized.error().message;
        auto insert_result = dst_storage->heap->insert_tuple(*serialized);
        ASSERT_TRUE(insert_result.has_value()) << insert_result.error().message;
    }

    TraversalConfig config;
    config.edge_type = "crosses";
    config.start_key = Value(int64_t{1});
    config.direction = TraverseDirection::OUT;
    config.max_depth = 1;
    config.heterogeneous = true;

    std::vector<OutputColumn> cols;
    cols.push_back({"", "id", TypeId::INT64, false, 0});
    cols.push_back({"", "__node", TypeId::INT64, false, 0});
    cols.push_back({"", "__depth", TypeId::INT64, false, 0});
    cols.push_back({"", "__source", TypeId::INT64, true, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    EnrichedTraversalOperator op(*graph_,
                                 std::move(config),
                                 std::move(schema),
                                 nullptr,
                                 bound,
                                 *dst_storage->heap,
                                 dst_storage->storage_schema,
                                 0,
                                 1);
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<int64_t> nodes;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value())
            break;
        nodes.push_back(row->value().values[1].as_int64());
    }
    op.close();

    std::sort(nodes.begin(), nodes.end());
    ASSERT_EQ(nodes.size(), 2u) << "colliding target dst(1) must not be dropped";
    EXPECT_EQ(nodes[0], 1);
    EXPECT_EQ(nodes[1], 2);
}

} // namespace
} // namespace sixseven
