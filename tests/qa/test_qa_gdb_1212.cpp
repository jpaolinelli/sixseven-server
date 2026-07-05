// QA adversarial tests for GDB-1212: unify the graph-traversal `heterogeneous`
// flag onto TraversalConfig::heterogeneous as the single source of truth for
// EdgeTraversalOperator and EnrichedTraversalOperator (previously each took a
// redundant `bool heterogeneous` ctor param that silently shadowed
// config.heterogeneous).
//
// Mission:
//   1. NO BEHAVIOR CHANGE - end-to-end TRAVERSE queries (planner-driven, both
//      EDGES and node/FETCH modes) over heterogeneous and homogeneous edge
//      sets must return identical results to before the refactor.
//   2. TRAP CLOSED - directly constructing the operators with
//      config.heterogeneous = true (bypassing the planner) must now actually
//      take the heterogeneous code path (no visited-set seeding), so a
//      colliding-PK target is not silently dropped (the GDB-696 regression).
//   3. Mixing both operators, multi-edge-type graphs, self-loops, cycles,
//      multi-hop chains, and homogeneous-still-correct checks.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/edge_traversal.h"
#include "sixseven/executor/enriched_traversal.h"
#include "sixseven/executor/query_engine.h"
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

// ---------------------------------------------------------------------------
// End-to-end fixture: drives the full planner/executor pipeline via SQL so we
// exercise exactly the code path production traffic uses (planner sets
// config.heterogeneous, then constructs the operator with the trimmed ctor).
// ---------------------------------------------------------------------------
class QA_GDB1212_EndToEnd : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_qa_gdb1212";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// Heterogeneous edge with a colliding PK between the start (users) table and
// the target (posts) table: users(1) and posts(1) both exist. Before GDB-696
// was fixed this dropped posts(1); GDB-1212 must not reintroduce that bug via
// the refactor (planner still sets config.heterogeneous, and both operators
// now genuinely read it instead of a separate ctor bool).
TEST_F(QA_GDB1212_EndToEnd, NodeModeHeterogeneousCollidingPkNotDropped) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO posts VALUES (1, 'colliding post')");
    exec_ok("INSERT INTO posts VALUES (2, 'other post')");
    exec_ok("CREATE EDGE TYPE authored FROM users TO posts");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("LINK users(1) TO posts(2) VIA authored");

    auto qr = exec_ok("TRAVERSE authored FROM users(1)");
    ASSERT_EQ(qr.rows.size(), 2u) << "colliding-PK target posts(1) must not be dropped";
}

// Same collision, but EDGES mode (goes through EdgeTraversalOperator instead
// of EnrichedTraversalOperator).
TEST_F(QA_GDB1212_EndToEnd, EdgesModeHeterogeneousCollidingPkNotDropped) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO posts VALUES (1, 'colliding post')");
    exec_ok("INSERT INTO posts VALUES (2, 'other post')");
    exec_ok("CREATE EDGE TYPE authored FROM users TO posts");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("LINK users(1) TO posts(2) VIA authored");

    auto qr = exec_ok("TRAVERSE authored FROM users(1) FETCH EDGES");
    ASSERT_EQ(qr.rows.size(), 2u) << "colliding-PK target posts(1) must not be dropped (EDGES mode)";
}

// Homogeneous traversal: self-loop must still be suppressed (visited set IS
// seeded with the start key when heterogeneous is false). Regression check
// that the refactor didn't flip homogeneous behavior.
TEST_F(QA_GDB1212_EndToEnd, HomogeneousSelfLoopStillSuppressed) {
    exec_ok("CREATE TABLE nodes (id INT PRIMARY KEY, label VARCHAR)");
    exec_ok("INSERT INTO nodes VALUES (1, 'a')");
    exec_ok("INSERT INTO nodes VALUES (2, 'b')");
    exec_ok("CREATE EDGE TYPE links FROM nodes TO nodes");
    exec_ok("LINK nodes(1) TO nodes(1) VIA links"); // self-loop
    exec_ok("LINK nodes(1) TO nodes(2) VIA links");

    auto qr = exec_ok("TRAVERSE links FROM nodes(1)");
    // Start node itself must not reappear as a result via the self-loop.
    for (const auto& row : qr.rows) {
        EXPECT_NE(row[0].as_int32(), 1) << "start node must not be re-emitted via self-loop";
    }
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 2);
}

// Homogeneous multi-hop cycle: A->B->C->A. Must terminate (no infinite loop)
// and visit each node once, proving the visited-set seeding + cycle guard is
// intact for the homogeneous path post-refactor.
TEST_F(QA_GDB1212_EndToEnd, HomogeneousCycleTerminatesAndVisitsOnce) {
    exec_ok("CREATE TABLE nodes (id INT PRIMARY KEY, label VARCHAR)");
    exec_ok("INSERT INTO nodes VALUES (1, 'a')");
    exec_ok("INSERT INTO nodes VALUES (2, 'b')");
    exec_ok("INSERT INTO nodes VALUES (3, 'c')");
    exec_ok("CREATE EDGE TYPE links FROM nodes TO nodes");
    exec_ok("LINK nodes(1) TO nodes(2) VIA links");
    exec_ok("LINK nodes(2) TO nodes(3) VIA links");
    exec_ok("LINK nodes(3) TO nodes(1) VIA links"); // closes the cycle

    auto qr = exec_ok("TRAVERSE links FROM nodes(1) MAX DEPTH 10");
    ASSERT_EQ(qr.rows.size(), 2u) << "expected nodes 2 and 3 exactly once each";
    std::vector<int32_t> ids;
    for (const auto& row : qr.rows) ids.push_back(row[0].as_int32());
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids[0], 2);
    EXPECT_EQ(ids[1], 3);
}

// Multi-hop heterogeneous chain is capped to depth 1 by design (planner logic
// unrelated to this refactor, but exercised here to ensure the refactor didn't
// disturb the cap or the direction handling).
TEST_F(QA_GDB1212_EndToEnd, HeterogeneousMultiHopCappedAtDepthOne) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO posts VALUES (10, 'p10')");
    exec_ok("CREATE EDGE TYPE authored FROM users TO posts");
    exec_ok("LINK users(1) TO posts(10) VIA authored");

    auto qr = exec_ok("TRAVERSE authored FROM users(1) MAX DEPTH 5");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 10);
}

// IN direction on a heterogeneous edge with a colliding PK on the reverse
// walk (posts(1) -> users(1)).
TEST_F(QA_GDB1212_EndToEnd, HeterogeneousInDirectionCollidingPkNotDropped) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO users VALUES (2, 'bob')");
    exec_ok("INSERT INTO posts VALUES (1, 'p1')");
    exec_ok("CREATE EDGE TYPE authored FROM users TO posts");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("LINK users(2) TO posts(1) VIA authored");

    auto qr = exec_ok("TRAVERSE authored FROM posts(1) DIRECTION IN");
    ASSERT_EQ(qr.rows.size(), 2u) << "both authors of posts(1) must be returned";
}

// Mixing: run a heterogeneous EDGES-mode traversal and a heterogeneous
// node-mode traversal back to back against the same graph engine/catalog, to
// confirm no shared/static state leaks between EdgeTraversalOperator and
// EnrichedTraversalOperator instances (each owns its own config_ copy now
// that the redundant member is gone).
TEST_F(QA_GDB1212_EndToEnd, MixingEdgeAndEnrichedOperatorsNoCrossContamination) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO posts VALUES (1, 'colliding post')");
    exec_ok("INSERT INTO posts VALUES (2, 'other post')");
    exec_ok("CREATE EDGE TYPE authored FROM users TO posts");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("LINK users(1) TO posts(2) VIA authored");

    auto edges_qr = exec_ok("TRAVERSE authored FROM users(1) FETCH EDGES");
    ASSERT_EQ(edges_qr.rows.size(), 2u);

    auto nodes_qr = exec_ok("TRAVERSE authored FROM users(1)");
    ASSERT_EQ(nodes_qr.rows.size(), 2u);

    // Run node-mode again to confirm repeatability (no residual state from the
    // prior EdgeTraversalOperator affecting a fresh EnrichedTraversalOperator).
    auto nodes_qr2 = exec_ok("TRAVERSE authored FROM users(1)");
    ASSERT_EQ(nodes_qr2.rows.size(), 2u);
}

// ---------------------------------------------------------------------------
// Direct-construction fixture: bypasses the planner entirely to hammer the
// trap directly, the same way the two new dev unit tests do, but with
// additional adversarial angles (self-loop with heterogeneous=true, BOTH
// direction, stress with many colliding pairs).
// ---------------------------------------------------------------------------
class QA_GDB1212_DirectConstruction : public ::testing::Test {
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
    }

    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t src_table_id_ = 0;
    table_id_t dst_table_id_ = 0;
};

// Self-loop analogue for the heterogeneous case: src(1) -> dst(1) where
// dst(1) shares the PK value with the start node. With
// config.heterogeneous = true, this must NOT be treated as a suppressed
// self-loop (it's a different table/entity, purely a PK-value collision).
TEST_F(QA_GDB1212_DirectConstruction, HeterogeneousPkCollisionIsNotTreatedAsSelfLoop) {
    auto e1 = graph_->link(default_database_id, "crosses", Value(int64_t{1}), Value(int64_t{1}));
    ASSERT_TRUE(e1.has_value()) << e1.error().message;

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
    ASSERT_TRUE(op.open().has_value());

    int count = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value())
            break;
        ++count;
        EXPECT_EQ(row->value().values[1].as_int64(), 1);
    }
    op.close();
    EXPECT_EQ(count, 1) << "PK-colliding heterogeneous target must be emitted, not suppressed";
}

// Explicit contrast: same source edge (crosses 1->1, 1->2), but
// config.heterogeneous = false (mislabeled, homogeneous default), and a second
// non-colliding target (dst(2)) added to distinguish "BFS actually explored
// the target" from "target happens to equal the start key". With
// heterogeneous=false, the BFS visited-set is seeded with start_key=1, so the
// neighbor pk=1 is treated as already-visited and never enqueued/explored --
// only the non-colliding neighbor (2) is discovered via BFS. This proves
// config.heterogeneous is authoritative for the visited-set-seeding decision
// (the actual mechanism GDB-696/GDB-1212 care about), independent of the
// coincidental edge-emission quirk when start_key == target_pk numerically.
TEST_F(QA_GDB1212_DirectConstruction, MisconfiguredHomogeneousFlagSkipsCollidingNeighborInBfs) {
    auto e1 = graph_->link(default_database_id, "crosses", Value(int64_t{1}), Value(int64_t{1}));
    ASSERT_TRUE(e1.has_value()) << e1.error().message;
    auto e2 = graph_->link(default_database_id, "crosses", Value(int64_t{1}), Value(int64_t{2}));
    ASSERT_TRUE(e2.has_value()) << e2.error().message;

    TraversalConfig config;
    config.edge_type = "crosses";
    config.start_key = Value(int64_t{1});
    config.direction = TraverseDirection::OUT;
    config.max_depth = 2; // depth 2 requires the colliding node to have been enqueued at depth 1
    config.heterogeneous = false; // deliberately wrong for this cross-table edge

    std::vector<OutputColumn> cols;
    cols.push_back({"", "__from", TypeId::INT64, false, 0});
    cols.push_back({"", "__to", TypeId::INT64, false, 0});
    cols.push_back({"", "__depth", TypeId::INT64, false, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    EdgeTraversalOperator op(*graph_, std::move(config), std::move(schema), nullptr, bound);
    ASSERT_TRUE(op.open().has_value());

    std::vector<int64_t> targets;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value())
            break;
        targets.push_back(row->value().values[1].as_int64());
    }
    op.close();

    // Both direct edges (1->1, 1->2) are still emitted by the edge-scan phase
    // (it re-derives depths independently of BFS enqueueing), but the colliding
    // node's own onward edges are never explored because BFS treated pk=1 as
    // pre-visited. This is the observable proof that config.heterogeneous=false
    // seeds the visited set exactly as documented.
    std::sort(targets.begin(), targets.end());
    ASSERT_EQ(targets.size(), 2u);
    EXPECT_EQ(targets[0], 1);
    EXPECT_EQ(targets[1], 2);
}

// BOTH direction with heterogeneous=true and multiple colliding pairs (stress
// variant of the collision trap, direct construction, EnrichedTraversalOperator).
TEST_F(QA_GDB1212_DirectConstruction, EnrichedBothDirectionManyCollidingPairsAllEmitted) {
    for (int64_t i = 1; i <= 20; ++i) {
        auto e = graph_->link(default_database_id, "crosses", Value(int64_t{1}), Value(i));
        ASSERT_TRUE(e.has_value()) << e.error().message;
    }

    auto data_dir = std::filesystem::temp_directory_path() / "sixseven_test_qa_gdb1212_direct";
    std::filesystem::remove_all(data_dir);
    std::filesystem::create_directories(data_dir);

    std::vector<int64_t> nodes;
    {
        DiskManager dm;
        StorageManager storage(dm, data_dir);

        auto db_storage = storage.create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        auto dst_schema = catalog_->get_table(default_database_id, "dst");
        ASSERT_TRUE(dst_schema.has_value()) << dst_schema.error().message;
        auto create_result =
            storage.create_table_storage(default_database_id, dst_table_id_, *dst_schema);
        ASSERT_TRUE(create_result.has_value()) << create_result.error().message;

        auto dst_storage_result = storage.get_table_storage(dst_table_id_);
        ASSERT_TRUE(dst_storage_result.has_value()) << dst_storage_result.error().message;
        auto* dst_storage = *dst_storage_result;

        for (int64_t pk = 1; pk <= 20; ++pk) {
            std::vector<Value> row_values{Value(pk)};
            auto serialized = TupleSerializer::serialize(row_values, dst_storage->storage_schema);
            ASSERT_TRUE(serialized.has_value()) << serialized.error().message;
            auto insert_result = dst_storage->heap->insert_tuple(*serialized);
            ASSERT_TRUE(insert_result.has_value()) << insert_result.error().message;
        }

        TraversalConfig config;
        config.edge_type = "crosses";
        config.start_key = Value(int64_t{1});
        config.direction = TraverseDirection::BOTH;
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
        ASSERT_TRUE(op.open().has_value());

        while (true) {
            auto row = op.next();
            ASSERT_TRUE(row.has_value()) << row.error().message;
            if (!row->has_value())
                break;
            nodes.push_back(row->value().values[1].as_int64());
        }
        op.close();
    }
    std::filesystem::remove_all(data_dir);

    std::sort(nodes.begin(), nodes.end());
    ASSERT_EQ(nodes.size(), 20u)
        << "all 20 colliding-eligible targets (including dst(1)) must be emitted";
    for (int64_t i = 1; i <= 20; ++i) {
        EXPECT_EQ(nodes[static_cast<size_t>(i - 1)], i);
    }
}

} // namespace
} // namespace sixseven
