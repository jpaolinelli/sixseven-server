// GDB-808 QA: Adversarial tests for catalog next-id counter safety after dead-code removal.
//
// set_next_index_id and set_next_edge_id were deleted; restore_index and
// restore_edge_type now advance the counters inline. These tests verify
// that NO id-reuse can occur under adversarial restore sequences.

#include "sixseven/catalog/catalog.h"

#include <gtest/gtest.h>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

static TableSchema make_schema(const std::string& name) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {{0, "id", TypeId::INT32, false, ""}};
    schema.pk_columns = "id";
    return schema;
}

// Fixture that sets up a catalog with the demo database and two tables.
class GDB808Test : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        auto t1 = catalog_.create_table(default_database_id, make_schema("nodes_a"));
        auto t2 = catalog_.create_table(default_database_id, make_schema("nodes_b"));
        ASSERT_TRUE(t1.has_value());
        ASSERT_TRUE(t2.has_value());
        tid_a_ = *t1;
        tid_b_ = *t2;
    }

    IndexDef make_index_def(const std::string& name, index_id_t pre_id = 0) {
        IndexDef d;
        d.index_id = pre_id;
        d.table_id = tid_a_;
        d.name = name;
        d.index_type = "btree";
        d.columns = "id";
        d.is_unique = false;
        return d;
    }

    EdgeTypeDef make_edge_def(const std::string& name, edge_id_t pre_id = 0) {
        EdgeTypeDef d;
        d.edge_id = pre_id;
        d.name = name;
        d.source_table_id = tid_a_;
        d.target_table_id = tid_b_;
        return d;
    }

    Catalog catalog_;
    table_id_t tid_a_{};
    table_id_t tid_b_{};
};

// ---------------------------------------------------------------------------
// INDEX ID COLLISION TESTS
// ---------------------------------------------------------------------------

// Restore highest id last — counter must still be max+1.
TEST_F(GDB808Test, GDB808_IndexRestoreHighestLast_NoCollision) {
    auto r1 = catalog_.restore_index(make_index_def("ix1", 5));
    auto r2 = catalog_.restore_index(make_index_def("ix2", 3));
    auto r3 = catalog_.restore_index(make_index_def("ix3", 9)); // highest last
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r3.has_value());

    auto nid = catalog_.create_index(make_index_def("ix_new"));
    ASSERT_TRUE(nid.has_value()) << nid.error().message;
    EXPECT_GT(*nid, static_cast<index_id_t>(9))
        << "Allocated index_id " << *nid << " collides with restored max id 9";
}

// Restore highest id first — counter must still be max+1.
TEST_F(GDB808Test, GDB808_IndexRestoreHighestFirst_NoCollision) {
    auto r1 = catalog_.restore_index(make_index_def("ix1", 99)); // highest first
    auto r2 = catalog_.restore_index(make_index_def("ix2", 1));
    auto r3 = catalog_.restore_index(make_index_def("ix3", 50));
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r3.has_value());

    auto nid = catalog_.create_index(make_index_def("ix_new"));
    ASSERT_TRUE(nid.has_value()) << nid.error().message;
    EXPECT_GT(*nid, static_cast<index_id_t>(99))
        << "Allocated index_id " << *nid << " collides with or precedes restored id 99";
}

// Restore with gaps (ids 1, 5, 9) — next must be >= 10.
TEST_F(GDB808Test, GDB808_IndexRestoreWithGaps_NoCollision) {
    ASSERT_TRUE(catalog_.restore_index(make_index_def("ix1", 1)).has_value());
    ASSERT_TRUE(catalog_.restore_index(make_index_def("ix5", 5)).has_value());
    ASSERT_TRUE(catalog_.restore_index(make_index_def("ix9", 9)).has_value());

    auto nid = catalog_.create_index(make_index_def("ix_new"));
    ASSERT_TRUE(nid.has_value()) << nid.error().message;
    // Must not reuse any of 1, 5, 9 — so must be >= 10.
    EXPECT_GE(*nid, static_cast<index_id_t>(10));
    EXPECT_NE(*nid, static_cast<index_id_t>(1));
    EXPECT_NE(*nid, static_cast<index_id_t>(5));
    EXPECT_NE(*nid, static_cast<index_id_t>(9));
}

// Restore zero indexes then allocate — should get id 1 (no collision with anything).
TEST_F(GDB808Test, GDB808_IndexRestoreZeroItems_AllocatesFromBase) {
    // No restore calls at all.
    auto nid = catalog_.create_index(make_index_def("ix_first"));
    ASSERT_TRUE(nid.has_value()) << nid.error().message;
    EXPECT_GE(*nid, static_cast<index_id_t>(1));
}

// Restore a very large id — counter must not overflow/wrap.
TEST_F(GDB808Test, GDB808_IndexRestoreVeryLargeId_NoCollision) {
    constexpr index_id_t BIG = 1'000'000;
    ASSERT_TRUE(catalog_.restore_index(make_index_def("ix_big", BIG)).has_value());

    auto nid = catalog_.create_index(make_index_def("ix_new"));
    ASSERT_TRUE(nid.has_value()) << nid.error().message;
    EXPECT_GT(*nid, BIG) << "Allocated index_id " << *nid << " would collide with restored id "
                         << BIG;
}

// Allocate one, then restore a higher id, then allocate again — both new ids
// must be unique and not equal each other or the restored id.
TEST_F(GDB808Test, GDB808_IndexAllocateRestoreAllocate_NoCollision) {
    auto id1 = catalog_.create_index(make_index_def("ix_first"));
    ASSERT_TRUE(id1.has_value());

    // Restore an index with id larger than what was just allocated.
    constexpr index_id_t RESTORED = 500;
    ASSERT_TRUE(catalog_.restore_index(make_index_def("ix_restored", RESTORED)).has_value());

    auto id2 = catalog_.create_index(make_index_def("ix_second"));
    ASSERT_TRUE(id2.has_value()) << id2.error().message;

    EXPECT_NE(*id1, *id2) << "Two create_index calls returned the same id";
    EXPECT_NE(*id2, RESTORED) << "Second allocation collided with restored id";
    EXPECT_GT(*id2, RESTORED) << "Second allocation is below restored id";
}

// Allocate many in a tight loop after restore — none should collide with restored.
TEST_F(GDB808Test, GDB808_IndexManyAllocationsAfterRestore_NoCollision) {
    constexpr index_id_t RESTORED = 42;
    ASSERT_TRUE(catalog_.restore_index(make_index_def("ix_base", RESTORED)).has_value());

    std::vector<index_id_t> allocated;
    for (int i = 0; i < 20; ++i) {
        auto nd = make_index_def("ix_loop_" + std::to_string(i));
        auto nid = catalog_.create_index(nd);
        ASSERT_TRUE(nid.has_value()) << nid.error().message;
        EXPECT_GT(*nid, RESTORED) << "Allocation " << i << " id " << *nid << " <= restored id "
                                  << RESTORED;
        for (auto prev : allocated) {
            EXPECT_NE(*nid, prev) << "Duplicate index_id " << *nid << " allocated";
        }
        allocated.push_back(*nid);
    }
}

// ---------------------------------------------------------------------------
// EDGE ID COLLISION TESTS
// ---------------------------------------------------------------------------

TEST_F(GDB808Test, GDB808_EdgeRestoreHighestLast_NoCollision) {
    ASSERT_TRUE(
        catalog_.restore_edge_type(default_database_id, make_edge_def("e1", 10)).has_value());
    ASSERT_TRUE(
        catalog_.restore_edge_type(default_database_id, make_edge_def("e2", 4)).has_value());
    ASSERT_TRUE(catalog_.restore_edge_type(default_database_id, make_edge_def("e3", 77))
                    .has_value()); // highest last

    auto nid = catalog_.create_edge_type(default_database_id, make_edge_def("e_new"));
    ASSERT_TRUE(nid.has_value()) << nid.error().message;
    EXPECT_GT(*nid, static_cast<edge_id_t>(77));
}

TEST_F(GDB808Test, GDB808_EdgeRestoreHighestFirst_NoCollision) {
    ASSERT_TRUE(catalog_.restore_edge_type(default_database_id, make_edge_def("e1", 200))
                    .has_value()); // highest first
    ASSERT_TRUE(
        catalog_.restore_edge_type(default_database_id, make_edge_def("e2", 7)).has_value());
    ASSERT_TRUE(
        catalog_.restore_edge_type(default_database_id, make_edge_def("e3", 50)).has_value());

    auto nid = catalog_.create_edge_type(default_database_id, make_edge_def("e_new"));
    ASSERT_TRUE(nid.has_value()) << nid.error().message;
    EXPECT_GT(*nid, static_cast<edge_id_t>(200));
}

TEST_F(GDB808Test, GDB808_EdgeRestoreWithGaps_NoCollision) {
    ASSERT_TRUE(
        catalog_.restore_edge_type(default_database_id, make_edge_def("e1", 1)).has_value());
    ASSERT_TRUE(
        catalog_.restore_edge_type(default_database_id, make_edge_def("e5", 5)).has_value());
    ASSERT_TRUE(
        catalog_.restore_edge_type(default_database_id, make_edge_def("e9", 9)).has_value());

    auto nid = catalog_.create_edge_type(default_database_id, make_edge_def("e_new"));
    ASSERT_TRUE(nid.has_value()) << nid.error().message;
    EXPECT_GE(*nid, static_cast<edge_id_t>(10));
    EXPECT_NE(*nid, static_cast<edge_id_t>(1));
    EXPECT_NE(*nid, static_cast<edge_id_t>(5));
    EXPECT_NE(*nid, static_cast<edge_id_t>(9));
}

TEST_F(GDB808Test, GDB808_EdgeRestoreZeroItems_AllocatesFromBase) {
    auto nid = catalog_.create_edge_type(default_database_id, make_edge_def("e_first"));
    ASSERT_TRUE(nid.has_value()) << nid.error().message;
    EXPECT_GE(*nid, static_cast<edge_id_t>(1));
}

TEST_F(GDB808Test, GDB808_EdgeRestoreVeryLargeId_NoCollision) {
    constexpr edge_id_t BIG = 2'000'000;
    ASSERT_TRUE(
        catalog_.restore_edge_type(default_database_id, make_edge_def("e_big", BIG)).has_value());

    auto nid = catalog_.create_edge_type(default_database_id, make_edge_def("e_new"));
    ASSERT_TRUE(nid.has_value()) << nid.error().message;
    EXPECT_GT(*nid, BIG);
}

TEST_F(GDB808Test, GDB808_EdgeAllocateRestoreAllocate_NoCollision) {
    auto id1 = catalog_.create_edge_type(default_database_id, make_edge_def("e_first"));
    ASSERT_TRUE(id1.has_value());

    constexpr edge_id_t RESTORED = 999;
    ASSERT_TRUE(
        catalog_.restore_edge_type(default_database_id, make_edge_def("e_restored", RESTORED))
            .has_value());

    auto id2 = catalog_.create_edge_type(default_database_id, make_edge_def("e_second"));
    ASSERT_TRUE(id2.has_value()) << id2.error().message;

    EXPECT_NE(*id1, *id2);
    EXPECT_NE(*id2, RESTORED);
    EXPECT_GT(*id2, RESTORED);
}

TEST_F(GDB808Test, GDB808_EdgeManyAllocationsAfterRestore_NoCollision) {
    constexpr edge_id_t RESTORED = 88;
    ASSERT_TRUE(catalog_.restore_edge_type(default_database_id, make_edge_def("e_base", RESTORED))
                    .has_value());

    std::vector<edge_id_t> allocated;
    for (int i = 0; i < 20; ++i) {
        auto nd = make_edge_def("e_loop_" + std::to_string(i));
        auto nid = catalog_.create_edge_type(default_database_id, nd);
        ASSERT_TRUE(nid.has_value()) << nid.error().message;
        EXPECT_GT(*nid, RESTORED);
        for (auto prev : allocated) {
            EXPECT_NE(*nid, prev) << "Duplicate edge_id " << *nid;
        }
        allocated.push_back(*nid);
    }
}

// ---------------------------------------------------------------------------
// CROSS: both index and edge restore in same catalog — no cross-contamination
// (counters are separate fields so this is a sanity guard)
// ---------------------------------------------------------------------------

TEST_F(GDB808Test, GDB808_IndexAndEdgeCountersIndependent) {
    // Restore index at id 500, edge at id 3.
    ASSERT_TRUE(catalog_.restore_index(make_index_def("ix_high", 500)).has_value());
    ASSERT_TRUE(
        catalog_.restore_edge_type(default_database_id, make_edge_def("e_low", 3)).has_value());

    auto new_edge = catalog_.create_edge_type(default_database_id, make_edge_def("e_new"));
    ASSERT_TRUE(new_edge.has_value()) << new_edge.error().message;
    // Edge counter should have advanced past 3, not past 500.
    EXPECT_GT(*new_edge, static_cast<edge_id_t>(3));
    EXPECT_LT(*new_edge, static_cast<edge_id_t>(500))
        << "Edge counter appears contaminated by index counter";
}

// ---------------------------------------------------------------------------
// TABLE ID / set_next_table_id — must be unaffected by the deletion
// ---------------------------------------------------------------------------

TEST_F(GDB808Test, GDB808_SetNextTableIdStillWorks) {
    // set_next_table_id is used by system_bootstrap. Verify it still compiles
    // and produces the expected next table id.
    catalog_.set_next_table_id(1000);
    EXPECT_EQ(catalog_.next_table_id(), static_cast<table_id_t>(1000));

    // A subsequent create_table must get an id >= 1000.
    auto ts = make_schema("t_after_set");
    auto tid = catalog_.create_table(default_database_id, ts);
    ASSERT_TRUE(tid.has_value()) << tid.error().message;
    EXPECT_GE(*tid, static_cast<table_id_t>(1000));
}

TEST_F(GDB808Test, GDB808_SetNextTableIdDoesNotAffectIndexOrEdge) {
    // Bump table counter to a huge value; index/edge counters must be unaffected.
    catalog_.set_next_table_id(100'000);

    auto new_idx = catalog_.create_index(make_index_def("ix_check"));
    ASSERT_TRUE(new_idx.has_value()) << new_idx.error().message;
    // index counter starts at 1 and should not have jumped to 100000.
    EXPECT_LT(*new_idx, static_cast<index_id_t>(100'000))
        << "Index counter was contaminated by set_next_table_id";

    auto new_edge = catalog_.create_edge_type(default_database_id, make_edge_def("e_check"));
    ASSERT_TRUE(new_edge.has_value()) << new_edge.error().message;
    EXPECT_LT(*new_edge, static_cast<edge_id_t>(100'000))
        << "Edge counter was contaminated by set_next_table_id";
}

// ---------------------------------------------------------------------------
// DUPLICATE RESTORE — same id twice must be rejected (no silent corruption)
// ---------------------------------------------------------------------------

TEST_F(GDB808Test, GDB808_DuplicateRestoreIndexRejected) {
    ASSERT_TRUE(catalog_.restore_index(make_index_def("ix_dup", 7)).has_value());
    // Restoring a different name but same id — if the impl only checks by name
    // this could silently insert, overwriting the counter or map.
    // The restore should fail on the name conflict if name is the same.
    auto r2 = catalog_.restore_index(make_index_def("ix_dup", 7));
    ASSERT_FALSE(r2.has_value()) << "Duplicate restore_index should fail";
    EXPECT_EQ(r2.error().code, StatusCode::ALREADY_EXISTS);
}

TEST_F(GDB808Test, GDB808_DuplicateRestoreEdgeRejected) {
    ASSERT_TRUE(
        catalog_.restore_edge_type(default_database_id, make_edge_def("e_dup", 13)).has_value());
    auto r2 = catalog_.restore_edge_type(default_database_id, make_edge_def("e_dup", 13));
    ASSERT_FALSE(r2.has_value()) << "Duplicate restore_edge_type should fail";
    EXPECT_EQ(r2.error().code, StatusCode::ALREADY_EXISTS);
}

// ---------------------------------------------------------------------------
// DROP + RESTORE: ensure drop doesn't corrupt counter so that restore can
// still advance it correctly.
// ---------------------------------------------------------------------------

TEST_F(GDB808Test, GDB808_DropThenRestoreHigherId_NoCollision) {
    // Create index with id=1, drop it, then restore one with id=50.
    // The new allocation after restore must be > 50.
    auto id1 = catalog_.create_index(make_index_def("ix_drop_me"));
    ASSERT_TRUE(id1.has_value());
    ASSERT_TRUE(catalog_.drop_index(default_database_id, "ix_drop_me").has_value());

    ASSERT_TRUE(catalog_.restore_index(make_index_def("ix_restored", 50)).has_value());

    auto nid = catalog_.create_index(make_index_def("ix_after_drop_restore"));
    ASSERT_TRUE(nid.has_value()) << nid.error().message;
    EXPECT_GT(*nid, static_cast<index_id_t>(50));
}

} // namespace
