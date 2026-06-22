/// QA adversarial tests for GDB-879: drop_edge_type dedup refactor + null-deref fix.
///
/// Adversarial focus:
/// 1. MUTATION-CHECK: verify the null-guard regression test is non-vacuous (HIGH finding if
///    it cannot reach the guarded code).
/// 2. BEHAVIOR EQUIVALENCE: drop_edge_type and drop_edge_type_locked leave identical end-state.
/// 3. ERROR PROPAGATION: NOT_FOUND on missing edge type; catalog drop error propagates.
/// 4. Multi-edge-type: drop_edge_types_for_table drops all associated edge types.
/// 5. State completeness: after drop, edge_tables_ and edge_storage_ maps are clean.

#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

// Pull in the test catalog helper from the unit test directory.
#include "test_catalog_helpers.h"

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static TableSchema make_qa879_schema(const std::string& name, TypeId pk_type = TypeId::INT64) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {
        {0, "id", pk_type, false, ""},
        {1, "data", TypeId::STRING, true, ""},
    };
    schema.pk_columns = "id";
    return schema;
}

static Value i64(int64_t v) {
    return Value(v);
}

// ---------------------------------------------------------------------------
// Fixture: persistent engine (dm_ != nullptr)
// ---------------------------------------------------------------------------

class QA_GDB879_Persistent : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_879_persist";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);

        auto t1 = catalog_.create_table(default_database_id, make_qa879_schema("users"));
        ASSERT_TRUE(t1.has_value()) << t1.error().message;
        users_id_ = *t1;

        auto t2 = catalog_.create_table(default_database_id, make_qa879_schema("posts"));
        ASSERT_TRUE(t2.has_value()) << t2.error().message;
        posts_id_ = *t2;

        engine_ = std::make_unique<GraphEngine>(catalog_, dm_, data_dir_);
    }

    void TearDown() override {
        engine_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    std::filesystem::path data_dir_;
    DiskManager dm_;
    Catalog catalog_;
    std::unique_ptr<GraphEngine> engine_;
    table_id_t users_id_ = 0;
    table_id_t posts_id_ = 0;
};

// ---------------------------------------------------------------------------
// Fixture: non-persistent engine (dm_ == nullptr)
// ---------------------------------------------------------------------------

class QA_GDB879_NonPersistent : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);

        auto t1 = catalog_.create_table(default_database_id, make_qa879_schema("nodes_a"));
        ASSERT_TRUE(t1.has_value()) << t1.error().message;
        src_id_ = *t1;

        auto t2 = catalog_.create_table(default_database_id, make_qa879_schema("nodes_b"));
        ASSERT_TRUE(t2.has_value()) << t2.error().message;
        tgt_id_ = *t2;

        // Non-persistent: dm_ is not provided.
        engine_ = std::make_unique<GraphEngine>(catalog_);
    }

    void TearDown() override { engine_.reset(); }

    Catalog catalog_;
    std::unique_ptr<GraphEngine> engine_;
    table_id_t src_id_ = 0;
    table_id_t tgt_id_ = 0;
};

// ===========================================================================
// AC1: verify the finding was real and the fix was applied
// ===========================================================================

/// The teardown_edge_storage_locked helper is shared by drop_edge_type and
/// drop_edge_type_locked. Verify it exists structurally by exercising both
/// callers and confirming they produce identical final state.
TEST_F(QA_GDB879_Persistent, GDB879_BothCallpathsProduceIdenticalState) {
    // Create two edge types so each path gets its own victim.
    auto et1 = engine_->create_edge_type(
        default_database_id, "path_a", users_id_, users_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et1.has_value()) << et1.error().message;

    auto et2 = engine_->create_edge_type(
        default_database_id, "path_b", users_id_, posts_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et2.has_value()) << et2.error().message;

    // Insert edges into both.
    ASSERT_TRUE(engine_->link(default_database_id, "path_a", i64(1), i64(2)).has_value());
    ASSERT_TRUE(engine_->link(default_database_id, "path_b", i64(3), i64(4)).has_value());

    // Drop via drop_edge_type (error-propagating path).
    auto drop_a = engine_->drop_edge_type(default_database_id, "path_a");
    ASSERT_TRUE(drop_a.has_value()) << drop_a.error().message;

    // path_a: edge type and its edges must be gone.
    auto edges_a = engine_->get_edges_from(default_database_id, "path_a", i64(1));
    ASSERT_FALSE(edges_a.has_value());
    EXPECT_EQ(edges_a.error().code, StatusCode::NOT_FOUND);

    // path_a: second drop returns NOT_FOUND.
    auto drop_a2 = engine_->drop_edge_type(default_database_id, "path_a");
    ASSERT_FALSE(drop_a2.has_value());
    EXPECT_EQ(drop_a2.error().code, StatusCode::NOT_FOUND);

    // path_b: still alive (independent).
    auto edges_b = engine_->get_edges_from(default_database_id, "path_b", i64(3));
    ASSERT_TRUE(edges_b.has_value()) << edges_b.error().message;
    EXPECT_EQ(edges_b->size(), 1u);

    // Drop path_b via drop_edge_types_for_table (which uses drop_edge_type_locked).
    auto drop_for_table = engine_->drop_edge_types_for_table(default_database_id, posts_id_);
    ASSERT_TRUE(drop_for_table.has_value()) << drop_for_table.error().message;

    // path_b: now gone too.
    auto edges_b2 = engine_->get_edges_from(default_database_id, "path_b", i64(3));
    ASSERT_FALSE(edges_b2.has_value());
    EXPECT_EQ(edges_b2.error().code, StatusCode::NOT_FOUND);
}

// ===========================================================================
// AC2: MUTATION-CHECK — null-guard reachability analysis
// ===========================================================================

/// ADVERSARIAL: the null-deref regression test in the unit suite
/// (GDB879_NullDmDropEdgeType) operates on a non-persistent engine which
/// never populates edge_storage_. This means the null-guarded dm_ dereference
/// inside teardown_edge_storage_locked is NEVER REACHED — the entire
/// if-block is skipped because edge_storage_.find() returns end().
///
/// We document this as a HIGH finding: the null-guard fix cannot be confirmed
/// non-vacuous by the existing unit test. This QA test explicitly verifies the
/// reachability gap by asserting the observable result (no crash) while
/// acknowledging the guard is unreachable on a non-persistent engine.
TEST_F(QA_GDB879_NonPersistent, GDB879_NullGuardReachabilityCheck_NonPersistentHasNoStorage) {
    auto et = engine_->create_edge_type(
        default_database_id, "linked", src_id_, tgt_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link(default_database_id, "linked", i64(10), i64(20)).has_value());

    // list_edge_types confirms the edge type exists.
    auto types_before = engine_->list_edge_types(default_database_id);
    ASSERT_EQ(types_before.size(), 1u);
    EXPECT_EQ(types_before[0], "linked");

    // drop_edge_type should succeed (non-persistent path: skips storage block entirely,
    // only erases from edge_tables_).
    auto drop = engine_->drop_edge_type(default_database_id, "linked");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // Edge type must be gone.
    auto types_after = engine_->list_edge_types(default_database_id);
    EXPECT_TRUE(types_after.empty());

    auto get = engine_->get_edges_from(default_database_id, "linked", i64(10));
    ASSERT_FALSE(get.has_value());
    EXPECT_EQ(get.error().code, StatusCode::NOT_FOUND);

    // NOTE: the null guard at lines 319 and 331 of teardown_edge_storage_locked
    // (inside `if (sit != edge_storage_.end())`) is UNREACHABLE on a non-persistent
    // engine because create_edge_type never calls create_edge_storage when dm_==nullptr.
    // The existing GDB879_NullDmDropEdgeType test is therefore vacuous w.r.t. the fix.
    // A mutation that removes the null guard would not cause a crash on this code path.
}

// ===========================================================================
// AC3: ERROR PROPAGATION
// ===========================================================================

/// drop_edge_type on a name that never existed returns NOT_FOUND immediately.
TEST_F(QA_GDB879_Persistent, GDB879_DropNonExistentEdgeTypeReturnsNotFound) {
    auto drop = engine_->drop_edge_type(default_database_id, "does_not_exist");
    ASSERT_FALSE(drop.has_value());
    EXPECT_EQ(drop.error().code, StatusCode::NOT_FOUND);
    EXPECT_FALSE(drop.error().message.empty());
}

/// drop_edge_type on a wrong database_id returns NOT_FOUND.
TEST_F(QA_GDB879_Persistent, GDB879_DropWrongDatabaseIdReturnsNotFound) {
    auto et = engine_->create_edge_type(
        default_database_id, "knows", users_id_, users_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    database_id_t wrong_db = default_database_id + 999;
    auto drop = engine_->drop_edge_type(wrong_db, "knows");
    ASSERT_FALSE(drop.has_value());
    EXPECT_EQ(drop.error().code, StatusCode::NOT_FOUND);

    // Original edge type in the correct database must be untouched.
    auto still_there = engine_->get_edges_from(default_database_id, "knows", i64(1));
    ASSERT_TRUE(still_there.has_value()) << still_there.error().message;
}

/// drop_edge_type with an empty name returns NOT_FOUND (no crash).
TEST_F(QA_GDB879_Persistent, GDB879_DropEmptyNameReturnsNotFound) {
    auto drop = engine_->drop_edge_type(default_database_id, "");
    ASSERT_FALSE(drop.has_value());
    EXPECT_EQ(drop.error().code, StatusCode::NOT_FOUND);
}

// ===========================================================================
// AC4: BEHAVIOR EQUIVALENCE — persistent engine end-state
// ===========================================================================

/// After drop_edge_type on a persistent engine:
/// - heap file is removed
/// - fwd/rev/uniq index files are removed (if they existed)
/// - edge table is absent from list_edge_types
/// - get_all_edges returns NOT_FOUND
/// - second drop returns NOT_FOUND
TEST_F(QA_GDB879_Persistent, GDB879_DropFullEndState_HeapAndIndexFilesGone) {
    auto et = engine_->create_edge_type(default_database_id,
                                        "belongs",
                                        users_id_,
                                        posts_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        {},
                                        /*prevent_duplicates=*/false);
    ASSERT_TRUE(et.has_value()) << et.error().message;
    edge_id_t eid = *et;

    ASSERT_TRUE(engine_->link(default_database_id, "belongs", i64(1), i64(10)).has_value());
    ASSERT_TRUE(engine_->link(default_database_id, "belongs", i64(2), i64(20)).has_value());

    // Flush indexes so those files exist on disk too.
    ASSERT_TRUE(engine_->flush_edge_indexes().has_value());

    auto base = data_dir_ / "databases" / std::to_string(default_database_id) / "edges";
    auto heap_path = base / ("edge_" + std::to_string(eid) + ".db");
    auto fwd_path = base / ("edge_" + std::to_string(eid) + "_fwd.db");
    auto rev_path = base / ("edge_" + std::to_string(eid) + "_rev.db");

    ASSERT_TRUE(std::filesystem::exists(heap_path)) << "heap file must exist before drop";
    ASSERT_TRUE(std::filesystem::exists(fwd_path)) << "fwd index must exist before drop";
    ASSERT_TRUE(std::filesystem::exists(rev_path)) << "rev index must exist before drop";

    // Drop.
    auto drop = engine_->drop_edge_type(default_database_id, "belongs");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // All storage files gone.
    EXPECT_FALSE(std::filesystem::exists(heap_path)) << "heap file must be removed after drop";
    EXPECT_FALSE(std::filesystem::exists(fwd_path)) << "fwd index must be removed after drop";
    EXPECT_FALSE(std::filesystem::exists(rev_path)) << "rev index must be removed after drop";

    // Edge type not in list.
    auto types = engine_->list_edge_types(default_database_id);
    for (const auto& t : types) {
        EXPECT_NE(t, "belongs") << "dropped edge type must not appear in list";
    }

    // Queries return NOT_FOUND.
    auto all = engine_->get_all_edges(default_database_id, "belongs");
    ASSERT_FALSE(all.has_value());
    EXPECT_EQ(all.error().code, StatusCode::NOT_FOUND);

    // Second drop returns NOT_FOUND.
    auto drop2 = engine_->drop_edge_type(default_database_id, "belongs");
    ASSERT_FALSE(drop2.has_value());
    EXPECT_EQ(drop2.error().code, StatusCode::NOT_FOUND);
}

/// After drop_edge_type, a new edge type with the same name can be created.
TEST_F(QA_GDB879_Persistent, GDB879_DropThenRecreateSucceeds) {
    auto et1 = engine_->create_edge_type(
        default_database_id, "temp", users_id_, users_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et1.has_value()) << et1.error().message;

    ASSERT_TRUE(engine_->link(default_database_id, "temp", i64(1), i64(2)).has_value());

    auto drop = engine_->drop_edge_type(default_database_id, "temp");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // Recreate with same name.
    auto et2 = engine_->create_edge_type(
        default_database_id, "temp", users_id_, posts_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et2.has_value())
        << "should be able to create a new edge type with a previously dropped name: "
        << et2.error().message;

    // The new edge type should work correctly.
    ASSERT_TRUE(engine_->link(default_database_id, "temp", i64(5), i64(50)).has_value());
    auto edges = engine_->get_edges_from(default_database_id, "temp", i64(5));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    EXPECT_EQ(edges->size(), 1u);
    EXPECT_EQ((*edges)[0].target_pk.as_int64(), 50);
}

// ===========================================================================
// AC5: Multi-edge-type — drop_edge_types_for_table
// ===========================================================================

/// drop_edge_types_for_table removes all edge types referencing the table
/// (both as source and target), leaves other edge types intact.
TEST_F(QA_GDB879_Persistent, GDB879_DropEdgeTypesForTable_MultiEdgeCleanup) {
    // Three edge types: two touch posts_id_, one does not.
    auto et_a = engine_->create_edge_type(
        default_database_id, "authored", users_id_, posts_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et_a.has_value()) << et_a.error().message;

    auto et_b = engine_->create_edge_type(
        default_database_id, "liked", posts_id_, users_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et_b.has_value()) << et_b.error().message;

    auto et_c = engine_->create_edge_type(
        default_database_id, "follows", users_id_, users_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et_c.has_value()) << et_c.error().message;

    ASSERT_TRUE(engine_->link(default_database_id, "authored", i64(1), i64(10)).has_value());
    ASSERT_TRUE(engine_->link(default_database_id, "liked", i64(10), i64(1)).has_value());
    ASSERT_TRUE(engine_->link(default_database_id, "follows", i64(1), i64(2)).has_value());

    // Drop all edge types that reference posts_id_.
    auto drop_for_table = engine_->drop_edge_types_for_table(default_database_id, posts_id_);
    ASSERT_TRUE(drop_for_table.has_value()) << drop_for_table.error().message;

    // "authored" and "liked" must be gone.
    auto authored = engine_->get_edges_from(default_database_id, "authored", i64(1));
    ASSERT_FALSE(authored.has_value());
    EXPECT_EQ(authored.error().code, StatusCode::NOT_FOUND);

    auto liked = engine_->get_edges_from(default_database_id, "liked", i64(10));
    ASSERT_FALSE(liked.has_value());
    EXPECT_EQ(liked.error().code, StatusCode::NOT_FOUND);

    // "follows" must still be alive.
    auto follows = engine_->get_edges_from(default_database_id, "follows", i64(1));
    ASSERT_TRUE(follows.has_value()) << follows.error().message;
    EXPECT_EQ(follows->size(), 1u);

    // Storage files for "authored" and "liked" must be removed.
    auto base = data_dir_ / "databases" / std::to_string(default_database_id) / "edges";
    EXPECT_FALSE(std::filesystem::exists(base / ("edge_" + std::to_string(*et_a) + ".db")));
    EXPECT_FALSE(std::filesystem::exists(base / ("edge_" + std::to_string(*et_b) + ".db")));
    // "follows" storage file must still exist.
    EXPECT_TRUE(std::filesystem::exists(base / ("edge_" + std::to_string(*et_c) + ".db")));
}

/// drop_edge_types_for_table on a table with no associated edge types is a no-op (no crash).
TEST_F(QA_GDB879_Persistent, GDB879_DropEdgeTypesForTable_NoEdgesIsNoop) {
    // Create an edge type between users only.
    auto et = engine_->create_edge_type(
        default_database_id, "follows", users_id_, users_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    // Drop for posts_id_ — no edge types reference posts.
    auto drop = engine_->drop_edge_types_for_table(default_database_id, posts_id_);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // "follows" still alive.
    auto follows = engine_->get_edges_from(default_database_id, "follows", i64(1));
    ASSERT_TRUE(follows.has_value()) << follows.error().message;
}

// ===========================================================================
// AC6: Unique-constraint edge type drop cleans up uniq index file
// ===========================================================================

TEST_F(QA_GDB879_Persistent, GDB879_DropUniqueEdgeTypeRemovesUniqIndexFile) {
    auto et = engine_->create_edge_type(default_database_id,
                                        "unique_edge",
                                        users_id_,
                                        users_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        {},
                                        /*prevent_duplicates=*/true);
    ASSERT_TRUE(et.has_value()) << et.error().message;
    edge_id_t eid = *et;

    ASSERT_TRUE(engine_->link(default_database_id, "unique_edge", i64(1), i64(2)).has_value());

    // Flush indexes so uniq file exists.
    ASSERT_TRUE(engine_->flush_edge_indexes().has_value());

    auto base = data_dir_ / "databases" / std::to_string(default_database_id) / "edges";
    auto uniq_path = base / ("edge_" + std::to_string(eid) + "_uniq.db");
    ASSERT_TRUE(std::filesystem::exists(uniq_path)) << "uniq index file must exist before drop";

    auto drop = engine_->drop_edge_type(default_database_id, "unique_edge");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    EXPECT_FALSE(std::filesystem::exists(uniq_path))
        << "uniq index file must be removed after drop";
}

// ===========================================================================
// AC7: Idempotency — drop_edge_types_for_table called twice is safe
// ===========================================================================

TEST_F(QA_GDB879_Persistent, GDB879_DropEdgeTypesForTable_TwiceIsIdempotent) {
    auto et = engine_->create_edge_type(
        default_database_id, "authored", users_id_, posts_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link(default_database_id, "authored", i64(1), i64(10)).has_value());

    // First drop.
    auto drop1 = engine_->drop_edge_types_for_table(default_database_id, posts_id_);
    ASSERT_TRUE(drop1.has_value()) << drop1.error().message;

    // Second drop — no edge types left referencing posts_id_, should be a silent no-op.
    auto drop2 = engine_->drop_edge_types_for_table(default_database_id, posts_id_);
    ASSERT_TRUE(drop2.has_value()) << drop2.error().message;
}

// ===========================================================================
// AC8: Non-persistent engine — drop_edge_types_for_table also works
// ===========================================================================

TEST_F(QA_GDB879_NonPersistent, GDB879_DropEdgeTypesForTable_NonPersistentNoCrash) {
    auto et_a = engine_->create_edge_type(
        default_database_id, "connected", src_id_, tgt_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et_a.has_value()) << et_a.error().message;

    auto et_b = engine_->create_edge_type(
        default_database_id, "associated", tgt_id_, src_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et_b.has_value()) << et_b.error().message;

    ASSERT_TRUE(engine_->link(default_database_id, "connected", i64(1), i64(2)).has_value());

    // Drop via locked path (drop_edge_types_for_table calls drop_edge_type_locked).
    auto drop = engine_->drop_edge_types_for_table(default_database_id, tgt_id_);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // Both edge types reference tgt_id_; both should be gone.
    auto types = engine_->list_edge_types(default_database_id);
    EXPECT_TRUE(types.empty()) << "all edge types referencing tgt_id_ must be dropped";
}

// ===========================================================================
// AC9: Stress — drop many edge types in sequence, no state corruption
// ===========================================================================

TEST_F(QA_GDB879_Persistent, GDB879_DropManyEdgeTypesSequential) {
    constexpr int kCount = 20;

    std::vector<edge_id_t> eids;
    for (int i = 0; i < kCount; ++i) {
        auto et = engine_->create_edge_type(default_database_id,
                                            "stress_" + std::to_string(i),
                                            users_id_,
                                            users_id_,
                                            TypeId::INT64,
                                            TypeId::INT64,
                                            {});
        ASSERT_TRUE(et.has_value()) << et.error().message;
        eids.push_back(*et);

        ASSERT_TRUE(
            engine_->link(default_database_id, "stress_" + std::to_string(i), i64(i), i64(i + 1))
                .has_value());
    }

    // Drop all.
    for (int i = 0; i < kCount; ++i) {
        auto drop = engine_->drop_edge_type(default_database_id, "stress_" + std::to_string(i));
        ASSERT_TRUE(drop.has_value()) << "drop stress_" << i << " failed: " << drop.error().message;
    }

    // No edge types should remain.
    auto types = engine_->list_edge_types(default_database_id);
    EXPECT_TRUE(types.empty());

    // All storage files must be gone.
    auto base = data_dir_ / "databases" / std::to_string(default_database_id) / "edges";
    for (edge_id_t eid : eids) {
        EXPECT_FALSE(std::filesystem::exists(base / ("edge_" + std::to_string(eid) + ".db")));
    }
}
