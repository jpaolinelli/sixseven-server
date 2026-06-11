/// @file test_qa_gdb_598.cpp
/// QA adversarial tests for GDB-598: Edge types are global across databases
/// and orphan on DROP TABLE CASCADE.
///
/// Issue 1: Edge types should be database-scoped (same name in different DBs).
/// Issue 2: DROP TABLE CASCADE should clean up associated edge types.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/graph/graph_engine.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "test_qa_helpers.h"

namespace sixseven {
namespace {

// ============================================================================
// Helpers
// ============================================================================

static TableSchema make_schema(const std::string& name, TypeId pk_type = TypeId::INT64) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {
        {0, "id", pk_type, false, ""},
        {1, "name", TypeId::STRING, true, ""},
    };
    schema.pk_columns = "id";
    return schema;
}

static Value pk(int64_t v) {
    return Value(v);
}

// ============================================================================
// Fixture: Two databases, each with their own tables
// ============================================================================

class QA_GDB598 : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_ = std::make_unique<Catalog>();
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        // Bootstrap the default database (id=1) so tests that reference
        // default_database_id can create tables and edge types in it.
        bootstrap_qa_catalog(*catalog_);

        // Create database 1.
        auto db1 = catalog_->create_database("db1");
        ASSERT_TRUE(db1.has_value()) << db1.error().message;
        db1_id_ = *db1;

        // Create database 2.
        auto db2 = catalog_->create_database("db2");
        ASSERT_TRUE(db2.has_value()) << db2.error().message;
        db2_id_ = *db2;

        // Tables in db1.
        auto t1 = catalog_->create_table(db1_id_, make_schema("users"));
        ASSERT_TRUE(t1.has_value()) << t1.error().message;
        db1_users_ = *t1;

        auto t2 = catalog_->create_table(db1_id_, make_schema("posts"));
        ASSERT_TRUE(t2.has_value()) << t2.error().message;
        db1_posts_ = *t2;

        // Tables in db2.
        auto t3 = catalog_->create_table(db2_id_, make_schema("users"));
        ASSERT_TRUE(t3.has_value()) << t3.error().message;
        db2_users_ = *t3;

        auto t4 = catalog_->create_table(db2_id_, make_schema("posts"));
        ASSERT_TRUE(t4.has_value()) << t4.error().message;
        db2_posts_ = *t4;
    }

    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<GraphEngine> graph_;

    database_id_t db1_id_ = 0;
    database_id_t db2_id_ = 0;
    table_id_t db1_users_ = 0;
    table_id_t db1_posts_ = 0;
    table_id_t db2_users_ = 0;
    table_id_t db2_posts_ = 0;
};

// ============================================================================
// Issue 1: Database-scoped edge types
// ============================================================================

/// AC: Same edge type name can be created in two different databases.
TEST_F(QA_GDB598, SameNameDifferentDatabases) {
    auto e1 = graph_->create_edge_type(
        db1_id_, "follows", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(e1.has_value()) << e1.error().message;

    auto e2 = graph_->create_edge_type(
        db2_id_, "follows", db2_users_, db2_users_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(e2.has_value()) << e2.error().message;

    // Both should have distinct edge IDs.
    EXPECT_NE(*e1, *e2);
}

/// AC: Duplicate within same database still fails.
TEST_F(QA_GDB598, DuplicateInSameDatabaseFails) {
    auto e1 = graph_->create_edge_type(
        db1_id_, "follows", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(e1.has_value()) << e1.error().message;

    auto e2 = graph_->create_edge_type(
        db1_id_, "follows", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {});
    EXPECT_FALSE(e2.has_value());
    EXPECT_EQ(e2.error().code, StatusCode::ALREADY_EXISTS);
}

/// Edges in db1 are isolated from db2.
TEST_F(QA_GDB598, LinkIsolationBetweenDatabases) {
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db1_id_, "follows", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db2_id_, "follows", db2_users_, db2_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    // Link in db1.
    auto link1 = graph_->link(db1_id_, "follows", pk(1), pk(2));
    ASSERT_TRUE(link1.has_value()) << link1.error().message;

    // Link in db2.
    auto link2 = graph_->link(db2_id_, "follows", pk(1), pk(2));
    ASSERT_TRUE(link2.has_value()) << link2.error().message;

    // db1 edges should only show db1's edges.
    auto edges1 = graph_->get_all_edges(db1_id_, "follows");
    ASSERT_TRUE(edges1.has_value()) << edges1.error().message;
    EXPECT_EQ(edges1->size(), 1u);

    // db2 edges should only show db2's edges.
    auto edges2 = graph_->get_all_edges(db2_id_, "follows");
    ASSERT_TRUE(edges2.has_value()) << edges2.error().message;
    EXPECT_EQ(edges2->size(), 1u);
}

/// list_edge_types returns only the current database's types.
TEST_F(QA_GDB598, ListEdgeTypesIsScoped) {
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db1_id_, "follows", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db1_id_, "authored", db1_users_, db1_posts_, TypeId::INT64, TypeId::INT64, {})
            .has_value());
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db2_id_, "follows", db2_users_, db2_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    auto db1_types = graph_->list_edge_types(db1_id_);
    EXPECT_EQ(db1_types.size(), 2u);

    auto db2_types = graph_->list_edge_types(db2_id_);
    EXPECT_EQ(db2_types.size(), 1u);
    EXPECT_EQ(db2_types[0], "follows");
}

/// Drop edge type in one DB doesn't affect same-named type in another.
TEST_F(QA_GDB598, DropEdgeTypeIsolation) {
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db1_id_, "follows", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db2_id_, "follows", db2_users_, db2_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    // Drop from db1.
    auto drop = graph_->drop_edge_type(db1_id_, "follows");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // db2's "follows" is still alive.
    auto link = graph_->link(db2_id_, "follows", pk(1), pk(2));
    ASSERT_TRUE(link.has_value()) << link.error().message;

    // db1's "follows" is gone.
    auto link_gone = graph_->link(db1_id_, "follows", pk(1), pk(2));
    EXPECT_FALSE(link_gone.has_value());
    EXPECT_EQ(link_gone.error().code, StatusCode::NOT_FOUND);
}

/// Cross-database get_edges_from isolation.
TEST_F(QA_GDB598, GetEdgesFromIsolation) {
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db1_id_, "follows", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db2_id_, "follows", db2_users_, db2_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    ASSERT_TRUE(graph_->link(db1_id_, "follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(graph_->link(db1_id_, "follows", pk(1), pk(3)).has_value());
    ASSERT_TRUE(graph_->link(db2_id_, "follows", pk(1), pk(99)).has_value());

    auto db1_edges = graph_->get_edges_from(db1_id_, "follows", pk(1));
    ASSERT_TRUE(db1_edges.has_value()) << db1_edges.error().message;
    EXPECT_EQ(db1_edges->size(), 2u);

    auto db2_edges = graph_->get_edges_from(db2_id_, "follows", pk(1));
    ASSERT_TRUE(db2_edges.has_value()) << db2_edges.error().message;
    EXPECT_EQ(db2_edges->size(), 1u);
}

/// Edge type not found in wrong database.
TEST_F(QA_GDB598, EdgeTypeNotFoundInWrongDatabase) {
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db1_id_, "follows", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    // Attempt to use in db2 (doesn't exist there).
    auto link = graph_->link(db2_id_, "follows", pk(1), pk(2));
    EXPECT_FALSE(link.has_value());
    EXPECT_EQ(link.error().code, StatusCode::NOT_FOUND);

    auto edges = graph_->get_edges_from(db2_id_, "follows", pk(1));
    EXPECT_FALSE(edges.has_value());
    EXPECT_EQ(edges.error().code, StatusCode::NOT_FOUND);

    auto unlink = graph_->unlink(db2_id_, "follows", pk(1), pk(2));
    EXPECT_FALSE(unlink.has_value());
    EXPECT_EQ(unlink.error().code, StatusCode::NOT_FOUND);
}

// ============================================================================
// Issue 2: DROP TABLE CASCADE cleans up edge types
// ============================================================================

/// AC: drop_edge_types_for_table removes edge types where the table is source.
TEST_F(QA_GDB598, DropEdgeTypesForSourceTable) {
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db1_id_, "authored", db1_users_, db1_posts_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    // Drop table = db1_users_ (source).
    auto drop = graph_->drop_edge_types_for_table(db1_id_, db1_users_);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // Edge type should be gone.
    auto types = graph_->list_edge_types(db1_id_);
    EXPECT_TRUE(types.empty());
}

/// AC: drop_edge_types_for_table removes edge types where the table is target.
TEST_F(QA_GDB598, DropEdgeTypesForTargetTable) {
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db1_id_, "authored", db1_users_, db1_posts_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    // Drop table = db1_posts_ (target).
    auto drop = graph_->drop_edge_types_for_table(db1_id_, db1_posts_);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    auto types = graph_->list_edge_types(db1_id_);
    EXPECT_TRUE(types.empty());
}

/// AC: After CASCADE cleanup, can re-create same-named edge type.
TEST_F(QA_GDB598, RecreateAfterCascadeCleanup) {
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db1_id_, "follows", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    // Simulate DROP TABLE CASCADE.
    auto drop = graph_->drop_edge_types_for_table(db1_id_, db1_users_);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // Re-create with same name — must succeed (no orphaned state).
    auto recreate = graph_->create_edge_type(
        db1_id_, "follows", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(recreate.has_value()) << recreate.error().message;

    // Should be usable.
    auto link = graph_->link(db1_id_, "follows", pk(1), pk(2));
    ASSERT_TRUE(link.has_value()) << link.error().message;
}

/// CASCADE for a table in db1 does not affect edge types in db2.
TEST_F(QA_GDB598, CascadeDoesNotAffectOtherDatabases) {
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db1_id_, "follows", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db2_id_, "follows", db2_users_, db2_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    // CASCADE in db1.
    auto drop = graph_->drop_edge_types_for_table(db1_id_, db1_users_);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // db1's "follows" is gone.
    EXPECT_TRUE(graph_->list_edge_types(db1_id_).empty());

    // db2's "follows" survives.
    auto db2_types = graph_->list_edge_types(db2_id_);
    EXPECT_EQ(db2_types.size(), 1u);
    EXPECT_EQ(db2_types[0], "follows");

    auto link = graph_->link(db2_id_, "follows", pk(1), pk(2));
    ASSERT_TRUE(link.has_value()) << link.error().message;
}

/// drop_edge_types_for_table with a table that has no edge types is a no-op.
TEST_F(QA_GDB598, CascadeNoopForTableWithoutEdges) {
    auto drop = graph_->drop_edge_types_for_table(db1_id_, db1_users_);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;
}

/// Multiple edge types referencing same table — all cleaned up on CASCADE.
TEST_F(QA_GDB598, CascadeCleansUpMultipleEdgeTypes) {
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db1_id_, "follows", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db1_id_, "authored", db1_users_, db1_posts_, TypeId::INT64, TypeId::INT64, {})
            .has_value());
    ASSERT_TRUE(graph_
                    ->create_edge_type(
                        db1_id_, "likes", db1_users_, db1_posts_, TypeId::INT64, TypeId::INT64, {})
                    .has_value());

    auto types_before = graph_->list_edge_types(db1_id_);
    EXPECT_EQ(types_before.size(), 3u);

    // Drop db1_users_ — "follows" (source), "authored" (source), "likes" (source).
    auto drop = graph_->drop_edge_types_for_table(db1_id_, db1_users_);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    auto types_after = graph_->list_edge_types(db1_id_);
    EXPECT_TRUE(types_after.empty());
}

/// CASCADE preserves edge types that don't reference the dropped table.
TEST_F(QA_GDB598, CascadePreservesUnrelatedEdgeTypes) {
    // "follows" links users->users, "post_tags" links posts->posts.
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db1_id_, "follows", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db1_id_, "post_tags", db1_posts_, db1_posts_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    // Drop db1_users_ — only "follows" should be removed.
    auto drop = graph_->drop_edge_types_for_table(db1_id_, db1_users_);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    auto types = graph_->list_edge_types(db1_id_);
    EXPECT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0], "post_tags");
}

// ============================================================================
// Edge cases and adversarial tests
// ============================================================================

/// Unlink in a specific database doesn't cross boundaries.
TEST_F(QA_GDB598, UnlinkIsolation) {
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db1_id_, "follows", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db2_id_, "follows", db2_users_, db2_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    ASSERT_TRUE(graph_->link(db1_id_, "follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(graph_->link(db2_id_, "follows", pk(1), pk(2)).has_value());

    // Unlink in db1.
    auto unlink = graph_->unlink(db1_id_, "follows", pk(1), pk(2));
    ASSERT_TRUE(unlink.has_value()) << unlink.error().message;

    // db1 edge is gone.
    auto db1_edges = graph_->get_all_edges(db1_id_, "follows");
    ASSERT_TRUE(db1_edges.has_value());
    EXPECT_EQ(db1_edges->size(), 0u);

    // db2 edge survives.
    auto db2_edges = graph_->get_all_edges(db2_id_, "follows");
    ASSERT_TRUE(db2_edges.has_value());
    EXPECT_EQ(db2_edges->size(), 1u);
}

/// get_edge_table returns correct table per database.
TEST_F(QA_GDB598, GetEdgeTableIsolation) {
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db1_id_, "follows", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db2_id_, "follows", db2_users_, db2_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    auto et1 = graph_->get_edge_table(db1_id_, "follows");
    ASSERT_TRUE(et1.has_value()) << et1.error().message;

    auto et2 = graph_->get_edge_table(db2_id_, "follows");
    ASSERT_TRUE(et2.has_value()) << et2.error().message;

    // They should be different EdgeTable instances.
    EXPECT_NE(*et1, *et2);
}

/// get_edges_to is scoped per database.
TEST_F(QA_GDB598, GetEdgesToIsolation) {
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db1_id_, "follows", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db2_id_, "follows", db2_users_, db2_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    ASSERT_TRUE(graph_->link(db1_id_, "follows", pk(10), pk(1)).has_value());
    ASSERT_TRUE(graph_->link(db1_id_, "follows", pk(20), pk(1)).has_value());
    ASSERT_TRUE(graph_->link(db2_id_, "follows", pk(30), pk(1)).has_value());

    auto db1_in = graph_->get_edges_to(db1_id_, "follows", pk(1));
    ASSERT_TRUE(db1_in.has_value());
    EXPECT_EQ(db1_in->size(), 2u);

    auto db2_in = graph_->get_edges_to(db2_id_, "follows", pk(1));
    ASSERT_TRUE(db2_in.has_value());
    EXPECT_EQ(db2_in->size(), 1u);
}

/// unlink_where is scoped per database.
TEST_F(QA_GDB598, UnlinkWhereIsolation) {
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db1_id_, "follows", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());
    ASSERT_TRUE(
        graph_
            ->create_edge_type(
                db2_id_, "follows", db2_users_, db2_users_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    ASSERT_TRUE(graph_->link(db1_id_, "follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(graph_->link(db2_id_, "follows", pk(1), pk(2)).has_value());

    auto count =
        graph_->unlink_where(db1_id_, "follows", pk(1), pk(2), [](const EdgeRow&) { return true; });
    ASSERT_TRUE(count.has_value()) << count.error().message;
    EXPECT_EQ(*count, 1u);

    // db2 still has its edge.
    auto db2_edges = graph_->get_all_edges(db2_id_, "follows");
    ASSERT_TRUE(db2_edges.has_value());
    EXPECT_EQ(db2_edges->size(), 1u);
}

/// Rapid create/drop/recreate cycle across databases.
TEST_F(QA_GDB598, CreateDropRecreateStress) {
    for (int i = 0; i < 50; ++i) {
        auto e1 = graph_->create_edge_type(
            db1_id_, "stress", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(e1.has_value()) << "iteration " << i << ": " << e1.error().message;

        auto e2 = graph_->create_edge_type(
            db2_id_, "stress", db2_users_, db2_users_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(e2.has_value()) << "iteration " << i << ": " << e2.error().message;

        ASSERT_TRUE(graph_->link(db1_id_, "stress", pk(i), pk(i + 1)).has_value());
        ASSERT_TRUE(graph_->link(db2_id_, "stress", pk(i), pk(i + 1)).has_value());

        ASSERT_TRUE(graph_->drop_edge_type(db1_id_, "stress").has_value());
        ASSERT_TRUE(graph_->drop_edge_type(db2_id_, "stress").has_value());
    }

    EXPECT_TRUE(graph_->list_edge_types(db1_id_).empty());
    EXPECT_TRUE(graph_->list_edge_types(db2_id_).empty());
}

/// Cascade + recreate cycle — the orphan scenario from the bug report.
TEST_F(QA_GDB598, CascadeRecreateNeverOrphans) {
    for (int i = 0; i < 20; ++i) {
        auto create = graph_->create_edge_type(
            db1_id_, "follows", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(create.has_value()) << "iteration " << i << ": " << create.error().message;

        ASSERT_TRUE(graph_->link(db1_id_, "follows", pk(i), pk(i + 1)).has_value());

        // Simulate DROP TABLE CASCADE.
        auto drop = graph_->drop_edge_types_for_table(db1_id_, db1_users_);
        ASSERT_TRUE(drop.has_value()) << "iteration " << i << ": " << drop.error().message;

        // Must be able to recreate immediately.
        EXPECT_TRUE(graph_->list_edge_types(db1_id_).empty())
            << "iteration " << i << ": edge type not cleaned up";
    }
}

/// Many edge types per database, CASCADE for one table is precise.
TEST_F(QA_GDB598, CascadePrecision) {
    // Create 5 edge types: 3 reference db1_users_, 2 don't.
    ASSERT_TRUE(graph_
                    ->create_edge_type(
                        db1_id_, "e1", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {})
                    .has_value());
    ASSERT_TRUE(graph_
                    ->create_edge_type(
                        db1_id_, "e2", db1_users_, db1_posts_, TypeId::INT64, TypeId::INT64, {})
                    .has_value());
    ASSERT_TRUE(graph_
                    ->create_edge_type(
                        db1_id_, "e3", db1_posts_, db1_users_, TypeId::INT64, TypeId::INT64, {})
                    .has_value());
    ASSERT_TRUE(graph_
                    ->create_edge_type(
                        db1_id_, "e4", db1_posts_, db1_posts_, TypeId::INT64, TypeId::INT64, {})
                    .has_value());
    // e5 in db2 — shouldn't be touched.
    ASSERT_TRUE(graph_
                    ->create_edge_type(
                        db2_id_, "e5", db2_users_, db2_posts_, TypeId::INT64, TypeId::INT64, {})
                    .has_value());

    auto drop = graph_->drop_edge_types_for_table(db1_id_, db1_users_);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // e1, e2, e3 removed (all reference db1_users_). e4 survives.
    auto db1_types = graph_->list_edge_types(db1_id_);
    ASSERT_EQ(db1_types.size(), 1u);
    EXPECT_EQ(db1_types[0], "e4");

    // e5 in db2 survives.
    auto db2_types = graph_->list_edge_types(db2_id_);
    ASSERT_EQ(db2_types.size(), 1u);
    EXPECT_EQ(db2_types[0], "e5");
}

/// Drop non-existent edge type in a database that has no edge types.
TEST_F(QA_GDB598, DropNonExistentEdgeType) {
    auto drop = graph_->drop_edge_type(db1_id_, "nonexistent");
    EXPECT_FALSE(drop.has_value());
    EXPECT_EQ(drop.error().code, StatusCode::NOT_FOUND);
}

/// Edge type with properties works correctly in database-scoped context.
TEST_F(QA_GDB598, ScopedEdgeTypeWithProperties) {
    ColumnDef weight_col{"weight", TypeId::FLOAT64};

    ASSERT_TRUE(graph_
                    ->create_edge_type(db1_id_,
                                       "follows",
                                       db1_users_,
                                       db1_users_,
                                       TypeId::INT64,
                                       TypeId::INT64,
                                       {weight_col})
                    .has_value());
    ASSERT_TRUE(graph_
                    ->create_edge_type(db2_id_,
                                       "follows",
                                       db2_users_,
                                       db2_users_,
                                       TypeId::INT64,
                                       TypeId::INT64,
                                       {weight_col})
                    .has_value());

    auto link1 = graph_->link(db1_id_, "follows", pk(1), pk(2), {Value(1.5)});
    ASSERT_TRUE(link1.has_value()) << link1.error().message;

    auto link2 = graph_->link(db2_id_, "follows", pk(1), pk(2), {Value(9.9)});
    ASSERT_TRUE(link2.has_value()) << link2.error().message;

    // Verify properties are isolated.
    auto edges1 = graph_->get_all_edges(db1_id_, "follows");
    ASSERT_TRUE(edges1.has_value());
    ASSERT_EQ(edges1->size(), 1u);
    ASSERT_EQ(edges1->at(0).properties.size(), 1u);
    EXPECT_DOUBLE_EQ(edges1->at(0).properties[0].as_float64(), 1.5);

    auto edges2 = graph_->get_all_edges(db2_id_, "follows");
    ASSERT_TRUE(edges2.has_value());
    ASSERT_EQ(edges2->size(), 1u);
    ASSERT_EQ(edges2->at(0).properties.size(), 1u);
    EXPECT_DOUBLE_EQ(edges2->at(0).properties[0].as_float64(), 9.9);
}

/// Edge type name that looks like compound key format "123:name" is handled correctly.
TEST_F(QA_GDB598, EdgeTypeNameContainingColon) {
    // The compound key format is "database_id:name". What if the name itself contains ':'?
    auto e1 = graph_->create_edge_type(
        db1_id_, "ns:follows", db1_users_, db1_users_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(e1.has_value()) << e1.error().message;

    auto e2 = graph_->create_edge_type(
        db2_id_, "ns:follows", db2_users_, db2_users_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(e2.has_value()) << e2.error().message;

    ASSERT_TRUE(graph_->link(db1_id_, "ns:follows", pk(1), pk(2)).has_value());

    auto edges = graph_->get_all_edges(db1_id_, "ns:follows");
    ASSERT_TRUE(edges.has_value());
    EXPECT_EQ(edges->size(), 1u);

    auto types = graph_->list_edge_types(db1_id_);
    EXPECT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0], "ns:follows");
}

/// Verify that the default_database_id also works correctly in the scoped model.
TEST_F(QA_GDB598, DefaultDatabaseIdWorks) {
    auto t = catalog_->create_table(default_database_id, make_schema("nodes"));
    ASSERT_TRUE(t.has_value()) << t.error().message;

    auto e = graph_->create_edge_type(
        default_database_id, "follows", *t, *t, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(e.has_value()) << e.error().message;

    auto link = graph_->link(default_database_id, "follows", pk(1), pk(2));
    ASSERT_TRUE(link.has_value()) << link.error().message;

    auto edges = graph_->get_all_edges(default_database_id, "follows");
    ASSERT_TRUE(edges.has_value());
    EXPECT_EQ(edges->size(), 1u);
}

} // namespace
} // namespace sixseven
