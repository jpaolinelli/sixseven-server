#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>

#include "test_catalog_helpers.h"

using namespace sixseven;

// -- Helpers ------------------------------------------------------------------

static TableSchema make_table_schema(const std::string& name, TypeId pk_type = TypeId::INT64) {
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

/// Test fixture that sets up disk-backed GraphEngine with two tables.
class EdgePersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_edge_persist";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);

        auto t1 = catalog_.create_table(default_database_id, make_table_schema("users"));
        ASSERT_TRUE(t1.has_value()) << t1.error().message;
        users_table_id_ = *t1;

        auto t2 = catalog_.create_table(default_database_id, make_table_schema("posts"));
        ASSERT_TRUE(t2.has_value()) << t2.error().message;
        posts_table_id_ = *t2;

        engine_ = std::make_unique<GraphEngine>(catalog_, dm_, data_dir_);
    }

    void TearDown() override {
        engine_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    /// Create a fresh GraphEngine from the same catalog and disk state.
    /// Simulates a server restart (catalog is already restored, but edges are in-memory only).
    std::unique_ptr<GraphEngine> restart_engine() {
        engine_.reset(); // Close the old engine (releases file handles).
        auto new_engine = std::make_unique<GraphEngine>(catalog_, dm_, data_dir_);
        auto load = new_engine->load_edges();
        EXPECT_TRUE(load.has_value()) << load.error().message;
        return new_engine;
    }

    std::filesystem::path data_dir_;
    DiskManager dm_;
    Catalog catalog_;
    std::unique_ptr<GraphEngine> engine_;
    table_id_t users_table_id_ = 0;
    table_id_t posts_table_id_ = 0;
};

// == Basic persistence ========================================================

TEST_F(EdgePersistenceTest, EdgesRecoveredAfterRestart) {
    // Create edge type and edges.
    auto et = engine_->create_edge_type(default_database_id,
                                        "follows",
                                        users_table_id_,
                                        users_table_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link(default_database_id, "follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(engine_->link(default_database_id, "follows", pk(1), pk(3)).has_value());
    ASSERT_TRUE(engine_->link(default_database_id, "follows", pk(2), pk(3)).has_value());

    // Simulate restart.
    engine_ = restart_engine();

    // Verify edges are still there.
    auto from_1 = engine_->get_edges_from(default_database_id, "follows", pk(1));
    ASSERT_TRUE(from_1.has_value()) << from_1.error().message;
    EXPECT_EQ(from_1->size(), 2u);

    auto from_2 = engine_->get_edges_from(default_database_id, "follows", pk(2));
    ASSERT_TRUE(from_2.has_value()) << from_2.error().message;
    EXPECT_EQ(from_2->size(), 1u);
    EXPECT_EQ((*from_2)[0].target_pk.as_int64(), 3);
}

TEST_F(EdgePersistenceTest, ReverseIndexRecoveredAfterRestart) {
    auto et = engine_->create_edge_type(default_database_id,
                                        "follows",
                                        users_table_id_,
                                        users_table_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link(default_database_id, "follows", pk(1), pk(3)).has_value());
    ASSERT_TRUE(engine_->link(default_database_id, "follows", pk(2), pk(3)).has_value());

    engine_ = restart_engine();

    // Reverse lookup: node 3 has incoming edges from 1 and 2.
    auto to_3 = engine_->get_edges_to(default_database_id, "follows", pk(3));
    ASSERT_TRUE(to_3.has_value()) << to_3.error().message;
    EXPECT_EQ(to_3->size(), 2u);
}

TEST_F(EdgePersistenceTest, EdgePropertiesPersisted) {
    std::vector<ColumnDef> props = {{"weight", TypeId::FLOAT64}, {"label", TypeId::STRING}};
    auto et = engine_->create_edge_type(default_database_id,
                                        "rated",
                                        users_table_id_,
                                        posts_table_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        props);
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_
                    ->link(default_database_id,
                           "rated",
                           pk(1),
                           pk(10),
                           {Value(4.5), Value(std::string("great"))})
                    .has_value());

    engine_ = restart_engine();

    auto edges = engine_->get_edges_from(default_database_id, "rated", pk(1));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    ASSERT_EQ(edges->size(), 1u);
    ASSERT_EQ((*edges)[0].properties.size(), 2u);
    EXPECT_DOUBLE_EQ((*edges)[0].properties[0].as_float64(), 4.5);
    EXPECT_EQ((*edges)[0].properties[1].as_string(), "great");
}

// == UNLINK persistence =======================================================

TEST_F(EdgePersistenceTest, UnlinkedEdgesNotRecovered) {
    auto et = engine_->create_edge_type(default_database_id,
                                        "follows",
                                        users_table_id_,
                                        users_table_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link(default_database_id, "follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(engine_->link(default_database_id, "follows", pk(1), pk(3)).has_value());

    // Unlink one edge.
    ASSERT_TRUE(engine_->unlink(default_database_id, "follows", pk(1), pk(2)).has_value());

    engine_ = restart_engine();

    // Only pk(3) edge should remain.
    auto edges = engine_->get_edges_from(default_database_id, "follows", pk(1));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    ASSERT_EQ(edges->size(), 1u);
    EXPECT_EQ((*edges)[0].target_pk.as_int64(), 3);
}

TEST_F(EdgePersistenceTest, UnlinkWherePersistedCorrectly) {
    std::vector<ColumnDef> props = {{"weight", TypeId::FLOAT64}};
    auto et = engine_->create_edge_type(default_database_id,
                                        "rated",
                                        users_table_id_,
                                        posts_table_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        props);
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(
        engine_->link(default_database_id, "rated", pk(1), pk(10), {Value(2.0)}).has_value());
    ASSERT_TRUE(
        engine_->link(default_database_id, "rated", pk(1), pk(10), {Value(4.5)}).has_value());
    ASSERT_TRUE(
        engine_->link(default_database_id, "rated", pk(1), pk(10), {Value(5.0)}).has_value());

    // Delete edges where weight < 3.0.
    auto result =
        engine_->unlink_where(default_database_id, "rated", pk(1), pk(10), [](const EdgeRow& e) {
            return e.properties[0].as_float64() < 3.0;
        });
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, 1u);

    engine_ = restart_engine();

    // Two edges should remain (4.5 and 5.0).
    auto edges = engine_->get_edges_from(default_database_id, "rated", pk(1));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    EXPECT_EQ(edges->size(), 2u);
}

// == Multiple edge types ======================================================

TEST_F(EdgePersistenceTest, MultipleEdgeTypesPersisted) {
    auto et1 = engine_->create_edge_type(default_database_id,
                                         "follows",
                                         users_table_id_,
                                         users_table_id_,
                                         TypeId::INT64,
                                         TypeId::INT64,
                                         {});
    ASSERT_TRUE(et1.has_value()) << et1.error().message;

    auto et2 = engine_->create_edge_type(default_database_id,
                                         "authored",
                                         users_table_id_,
                                         posts_table_id_,
                                         TypeId::INT64,
                                         TypeId::INT64,
                                         {});
    ASSERT_TRUE(et2.has_value()) << et2.error().message;

    ASSERT_TRUE(engine_->link(default_database_id, "follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(engine_->link(default_database_id, "authored", pk(1), pk(100)).has_value());

    engine_ = restart_engine();

    auto follows = engine_->get_edges_from(default_database_id, "follows", pk(1));
    ASSERT_TRUE(follows.has_value()) << follows.error().message;
    EXPECT_EQ(follows->size(), 1u);
    EXPECT_EQ((*follows)[0].target_pk.as_int64(), 2);

    auto authored = engine_->get_edges_from(default_database_id, "authored", pk(1));
    ASSERT_TRUE(authored.has_value()) << authored.error().message;
    EXPECT_EQ(authored->size(), 1u);
    EXPECT_EQ((*authored)[0].target_pk.as_int64(), 100);
}

// == Row ID continuity ========================================================

TEST_F(EdgePersistenceTest, NewEdgesAfterRestartGetUniqueRowIds) {
    auto et = engine_->create_edge_type(default_database_id,
                                        "follows",
                                        users_table_id_,
                                        users_table_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    auto id1 = engine_->link(default_database_id, "follows", pk(1), pk(2));
    ASSERT_TRUE(id1.has_value()) << id1.error().message;

    auto id2 = engine_->link(default_database_id, "follows", pk(1), pk(3));
    ASSERT_TRUE(id2.has_value()) << id2.error().message;

    engine_ = restart_engine();

    // Insert a new edge after restart.
    auto id3 = engine_->link(default_database_id, "follows", pk(1), pk(4));
    ASSERT_TRUE(id3.has_value()) << id3.error().message;

    // New row ID should be greater than any restored row ID.
    EXPECT_GT(*id3, *id2);

    // All three edges should be present.
    auto edges = engine_->get_edges_from(default_database_id, "follows", pk(1));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    EXPECT_EQ(edges->size(), 3u);
}

// == Empty edge type ==========================================================

TEST_F(EdgePersistenceTest, EmptyEdgeTypeRecoveredCorrectly) {
    auto et = engine_->create_edge_type(default_database_id,
                                        "follows",
                                        users_table_id_,
                                        users_table_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    // No edges inserted.
    engine_ = restart_engine();

    // Edge type should exist but be empty.
    auto edges = engine_->get_edges_from(default_database_id, "follows", pk(1));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    EXPECT_TRUE(edges->empty());
}

// == Drop edge type persistence ===============================================

TEST_F(EdgePersistenceTest, DroppedEdgeTypeFileRemoved) {
    auto et = engine_->create_edge_type(default_database_id,
                                        "follows",
                                        users_table_id_,
                                        users_table_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link(default_database_id, "follows", pk(1), pk(2)).has_value());

    // Check that edge file exists.
    auto edge_path = data_dir_ / "databases" / std::to_string(default_database_id) / "edges" /
                     ("edge_" + std::to_string(*et) + ".db");
    EXPECT_TRUE(std::filesystem::exists(edge_path));

    // Drop the edge type.
    ASSERT_TRUE(engine_->drop_edge_type(default_database_id, "follows").has_value());

    // File should be removed.
    EXPECT_FALSE(std::filesystem::exists(edge_path));
}

// == Self-referencing persistence =============================================

TEST_F(EdgePersistenceTest, SelfReferenceEdgesPersisted) {
    auto et = engine_->create_edge_type(default_database_id,
                                        "self_link",
                                        users_table_id_,
                                        users_table_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link(default_database_id, "self_link", pk(1), pk(1)).has_value());

    engine_ = restart_engine();

    auto from = engine_->get_edges_from(default_database_id, "self_link", pk(1));
    ASSERT_TRUE(from.has_value()) << from.error().message;
    EXPECT_EQ(from->size(), 1u);
    EXPECT_EQ((*from)[0].source_pk.as_int64(), 1);
    EXPECT_EQ((*from)[0].target_pk.as_int64(), 1);
}

// == Multiple restarts ========================================================

TEST_F(EdgePersistenceTest, DataSurvivesMultipleRestarts) {
    auto et = engine_->create_edge_type(default_database_id,
                                        "follows",
                                        users_table_id_,
                                        users_table_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link(default_database_id, "follows", pk(1), pk(2)).has_value());

    // First restart.
    engine_ = restart_engine();

    // Add more edges after first restart.
    ASSERT_TRUE(engine_->link(default_database_id, "follows", pk(2), pk(3)).has_value());

    // Second restart.
    engine_ = restart_engine();

    auto from_1 = engine_->get_edges_from(default_database_id, "follows", pk(1));
    ASSERT_TRUE(from_1.has_value()) << from_1.error().message;
    EXPECT_EQ(from_1->size(), 1u);

    auto from_2 = engine_->get_edges_from(default_database_id, "follows", pk(2));
    ASSERT_TRUE(from_2.has_value()) << from_2.error().message;
    EXPECT_EQ(from_2->size(), 1u);
}

// == Restore edge in EdgeTable ================================================

TEST(EdgeTableRestore, RestoreEdgeSetsCorrectRowId) {
    Catalog catalog;
    init_test_catalog(catalog);
    auto t1 = catalog.create_table(default_database_id, make_table_schema("users"));
    ASSERT_TRUE(t1.has_value());

    EdgeTableConfig config;
    config.edge_id = 1;
    config.name = "test";
    config.source_table_id = *t1;
    config.target_table_id = *t1;
    config.source_pk_type = TypeId::INT64;
    config.target_pk_type = TypeId::INT64;

    EdgeTable table(config);

    // Restore edges with specific IDs.
    ASSERT_TRUE(table.restore_edge(5, pk(1), pk(2), {}).has_value());
    ASSERT_TRUE(table.restore_edge(10, pk(2), pk(3), {}).has_value());

    EXPECT_EQ(table.size(), 2u);

    // New insert should get ID > 10.
    auto new_id = table.insert_edge(pk(3), pk(4), {});
    ASSERT_TRUE(new_id.has_value()) << new_id.error().message;
    EXPECT_GT(*new_id, 10u);
}

TEST(EdgeTableRestore, RestoreDuplicateRowIdFails) {
    Catalog catalog;
    init_test_catalog(catalog);
    auto t1 = catalog.create_table(default_database_id, make_table_schema("users"));
    ASSERT_TRUE(t1.has_value());

    EdgeTableConfig config;
    config.edge_id = 1;
    config.name = "test";
    config.source_table_id = *t1;
    config.target_table_id = *t1;
    config.source_pk_type = TypeId::INT64;
    config.target_pk_type = TypeId::INT64;

    EdgeTable table(config);

    ASSERT_TRUE(table.restore_edge(1, pk(1), pk(2), {}).has_value());

    auto dup = table.restore_edge(1, pk(3), pk(4), {});
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, StatusCode::ALREADY_EXISTS);
}

// == parse_type_id ============================================================

TEST(ParseTypeId, ParsesAllKnownTypes) {
    EXPECT_EQ(parse_type_id("INT32"), TypeId::INT32);
    EXPECT_EQ(parse_type_id("STRING"), TypeId::STRING);
    EXPECT_EQ(parse_type_id("FLOAT64"), TypeId::FLOAT64);
    EXPECT_EQ(parse_type_id("BOOL"), TypeId::BOOL);
    EXPECT_EQ(parse_type_id("INT64"), TypeId::INT64);
}

TEST(ParseTypeId, CaseInsensitive) {
    EXPECT_EQ(parse_type_id("int32"), TypeId::INT32);
    EXPECT_EQ(parse_type_id("String"), TypeId::STRING);
    EXPECT_EQ(parse_type_id("float64"), TypeId::FLOAT64);
}

TEST(ParseTypeId, UnknownReturnsNullopt) {
    EXPECT_FALSE(parse_type_id("UNKNOWN").has_value());
    EXPECT_FALSE(parse_type_id("").has_value());
}

// == Edge index persistence ===================================================

TEST_F(EdgePersistenceTest, EdgeIndexPersistAndLoad) {
    // Create edge type and insert edges.
    auto et = engine_->create_edge_type(default_database_id,
                                        "follows",
                                        users_table_id_,
                                        users_table_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link(default_database_id, "follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(engine_->link(default_database_id, "follows", pk(1), pk(3)).has_value());
    ASSERT_TRUE(engine_->link(default_database_id, "follows", pk(2), pk(3)).has_value());
    ASSERT_TRUE(engine_->link(default_database_id, "follows", pk(3), pk(1)).has_value());

    // Flush edge indexes to disk (simulates clean shutdown).
    auto flush = engine_->flush_edge_indexes();
    ASSERT_TRUE(flush.has_value()) << flush.error().message;

    // Verify index files exist.
    auto fwd_path = data_dir_ / "databases" / std::to_string(default_database_id) / "edges" /
                    ("edge_" + std::to_string(*et) + "_fwd.db");
    auto rev_path = data_dir_ / "databases" / std::to_string(default_database_id) / "edges" /
                    ("edge_" + std::to_string(*et) + "_rev.db");
    EXPECT_TRUE(std::filesystem::exists(fwd_path));
    EXPECT_TRUE(std::filesystem::exists(rev_path));

    // Restart — should use fast path (persisted indexes).
    engine_ = restart_engine();

    // Verify all forward edges.
    auto from_1 = engine_->get_edges_from(default_database_id, "follows", pk(1));
    ASSERT_TRUE(from_1.has_value()) << from_1.error().message;
    EXPECT_EQ(from_1->size(), 2u);

    auto from_2 = engine_->get_edges_from(default_database_id, "follows", pk(2));
    ASSERT_TRUE(from_2.has_value()) << from_2.error().message;
    EXPECT_EQ(from_2->size(), 1u);

    auto from_3 = engine_->get_edges_from(default_database_id, "follows", pk(3));
    ASSERT_TRUE(from_3.has_value()) << from_3.error().message;
    EXPECT_EQ(from_3->size(), 1u);

    // Verify reverse edges.
    auto to_1 = engine_->get_edges_to(default_database_id, "follows", pk(1));
    ASSERT_TRUE(to_1.has_value()) << to_1.error().message;
    EXPECT_EQ(to_1->size(), 1u);

    auto to_3 = engine_->get_edges_to(default_database_id, "follows", pk(3));
    ASSERT_TRUE(to_3.has_value()) << to_3.error().message;
    EXPECT_EQ(to_3->size(), 2u);

    // New edges should still work after loading persisted indexes.
    auto new_link = engine_->link(default_database_id, "follows", pk(4), pk(5));
    ASSERT_TRUE(new_link.has_value()) << new_link.error().message;

    auto from_4 = engine_->get_edges_from(default_database_id, "follows", pk(4));
    ASSERT_TRUE(from_4.has_value()) << from_4.error().message;
    EXPECT_EQ(from_4->size(), 1u);
}

TEST_F(EdgePersistenceTest, EdgeIndexStalenessDetection) {
    // Create edge type and insert edges.
    auto et = engine_->create_edge_type(default_database_id,
                                        "follows",
                                        users_table_id_,
                                        users_table_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link(default_database_id, "follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(engine_->link(default_database_id, "follows", pk(1), pk(3)).has_value());

    // Flush indexes (count=2 stored in heap header).
    auto flush = engine_->flush_edge_indexes();
    ASSERT_TRUE(flush.has_value()) << flush.error().message;

    // Add more edges WITHOUT flushing indexes (simulates crash before clean shutdown).
    ASSERT_TRUE(engine_->link(default_database_id, "follows", pk(2), pk(3)).has_value());

    // Restart — should detect staleness (index has 2 entries, heap has 3 rows)
    // and fall back to full rebuild.
    engine_ = restart_engine();

    // Despite staleness, all 3 edges should be available (rebuilt from heap).
    auto all = engine_->get_all_edges(default_database_id, "follows");
    ASSERT_TRUE(all.has_value()) << all.error().message;
    EXPECT_EQ(all->size(), 3u);

    // Forward index should work correctly after rebuild.
    auto from_1 = engine_->get_edges_from(default_database_id, "follows", pk(1));
    ASSERT_TRUE(from_1.has_value()) << from_1.error().message;
    EXPECT_EQ(from_1->size(), 2u);

    auto from_2 = engine_->get_edges_from(default_database_id, "follows", pk(2));
    ASSERT_TRUE(from_2.has_value()) << from_2.error().message;
    EXPECT_EQ(from_2->size(), 1u);
}

TEST_F(EdgePersistenceTest, DropEdgeTypeRemovesIndexFiles) {
    auto et = engine_->create_edge_type(default_database_id,
                                        "follows",
                                        users_table_id_,
                                        users_table_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link(default_database_id, "follows", pk(1), pk(2)).has_value());

    // Flush indexes.
    auto flush = engine_->flush_edge_indexes();
    ASSERT_TRUE(flush.has_value()) << flush.error().message;

    auto base = data_dir_ / "databases" / std::to_string(default_database_id) / "edges";
    auto heap_path = base / ("edge_" + std::to_string(*et) + ".db");
    auto fwd_path = base / ("edge_" + std::to_string(*et) + "_fwd.db");
    auto rev_path = base / ("edge_" + std::to_string(*et) + "_rev.db");

    EXPECT_TRUE(std::filesystem::exists(heap_path));
    EXPECT_TRUE(std::filesystem::exists(fwd_path));
    EXPECT_TRUE(std::filesystem::exists(rev_path));

    // Drop edge type.
    ASSERT_TRUE(engine_->drop_edge_type(default_database_id, "follows").has_value());

    // All files should be removed.
    EXPECT_FALSE(std::filesystem::exists(heap_path));
    EXPECT_FALSE(std::filesystem::exists(fwd_path));
    EXPECT_FALSE(std::filesystem::exists(rev_path));
}

TEST_F(EdgePersistenceTest, EdgeIndexWithProperties) {
    // Create edge type with properties.
    std::vector<ColumnDef> props = {{"weight", TypeId::FLOAT64}, {"label", TypeId::STRING}};
    auto et = engine_->create_edge_type(default_database_id,
                                        "rated",
                                        users_table_id_,
                                        posts_table_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        props);
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link(default_database_id, "rated", pk(1), pk(100),
                              {Value(4.5), Value(std::string("good"))}).has_value());
    ASSERT_TRUE(engine_->link(default_database_id, "rated", pk(2), pk(100),
                              {Value(3.0), Value(std::string("ok"))}).has_value());

    // Flush indexes.
    auto flush = engine_->flush_edge_indexes();
    ASSERT_TRUE(flush.has_value()) << flush.error().message;

    // Restart with persisted indexes.
    engine_ = restart_engine();

    // Verify edges and properties survived.
    auto from_1 = engine_->get_edges_from(default_database_id, "rated", pk(1));
    ASSERT_TRUE(from_1.has_value()) << from_1.error().message;
    ASSERT_EQ(from_1->size(), 1u);
    EXPECT_DOUBLE_EQ((*from_1)[0].properties[0].as_float64(), 4.5);
    EXPECT_EQ((*from_1)[0].properties[1].as_string(), "good");

    auto to_100 = engine_->get_edges_to(default_database_id, "rated", pk(100));
    ASSERT_TRUE(to_100.has_value()) << to_100.error().message;
    EXPECT_EQ(to_100->size(), 2u);
}

// == GDB-879: drop_edge_type dedup + null-guard regression ====================

// (a/b) drop_edge_type on a persistent engine removes edge type, returns ok().
//       Second drop returns NOT_FOUND.
TEST_F(EdgePersistenceTest, GDB879_DropEdgeTypePersistentOkThenNotFound) {
    auto et = engine_->create_edge_type(default_database_id,
                                        "knows",
                                        users_table_id_,
                                        users_table_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link(default_database_id, "knows", pk(1), pk(2)).has_value());

    // First drop: should succeed.
    auto drop1 = engine_->drop_edge_type(default_database_id, "knows");
    ASSERT_TRUE(drop1.has_value()) << drop1.error().message;

    // Edge type should be gone.
    auto after = engine_->get_edges_from(default_database_id, "knows", pk(1));
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code, StatusCode::NOT_FOUND);

    // Second drop: should return NOT_FOUND.
    auto drop2 = engine_->drop_edge_type(default_database_id, "knows");
    ASSERT_FALSE(drop2.has_value());
    EXPECT_EQ(drop2.error().code, StatusCode::NOT_FOUND);
}

// (b) drop_edge_type removes the storage file from disk.
TEST_F(EdgePersistenceTest, GDB879_DropEdgeTypeRemovesStorageFile) {
    auto et = engine_->create_edge_type(default_database_id,
                                        "owns",
                                        users_table_id_,
                                        posts_table_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        {});
    ASSERT_TRUE(et.has_value()) << et.error().message;
    edge_id_t eid = *et;

    ASSERT_TRUE(engine_->link(default_database_id, "owns", pk(10), pk(20)).has_value());

    // Compute the expected file path (mirrors GraphEngine::edge_file_path).
    auto expected_path = data_dir_ / "databases" /
                         std::to_string(default_database_id) / "edges" /
                         ("edge_" + std::to_string(eid) + ".db");
    ASSERT_TRUE(std::filesystem::exists(expected_path))
        << "storage file should exist after create";

    auto drop = engine_->drop_edge_type(default_database_id, "owns");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    EXPECT_FALSE(std::filesystem::exists(expected_path))
        << "storage file should be removed after drop";
}

// (c) drop_edge_type on a non-persistent GraphEngine (dm_==nullptr) removes
// the edge type from edge_tables_ and the catalog, then returns NOT_FOUND on a
// second drop.
//
// On a non-persistent engine, create_edge_type does NOT populate edge_storage_
// (storage creation is gated on has_persistence()), so teardown_edge_storage_locked
// always finds sit==edge_storage_.end() and skips the storage block entirely.
// What this test actually exercises is the real no-persistence drop path:
// edge_tables_ erase + catalog drop, with the in-memory edge table gone after.
//
// The dm_!=nullptr guard inside teardown_edge_storage_locked is defensive
// consistency with drop_edge_type_locked and the destructor; it is not
// reachable through the public API on a dm_==nullptr engine (see comment in
// graph_engine.cpp).
TEST(GDB879_NullDmDropEdgeType, DropEdgeTypeOnNonPersistentEngineDropsTypeWithoutStorage) {
    Catalog catalog;
    // Use the non-persistent constructor (dm_ stays nullptr).
    GraphEngine engine(catalog);

    // Bootstrap catalog so create_edge_type can register the edge type.
    init_test_catalog(catalog);
    database_id_t db_id = default_database_id;

    // Register dummy tables directly in the catalog.
    TableSchema src_schema;
    src_schema.name = "nodes_a";
    src_schema.columns = {{0, "id", TypeId::INT64, false, ""}};
    src_schema.pk_columns = "id";
    auto src = catalog.create_table(db_id, src_schema);
    ASSERT_TRUE(src.has_value()) << src.error().message;

    TableSchema tgt_schema;
    tgt_schema.name = "nodes_b";
    tgt_schema.columns = {{0, "id", TypeId::INT64, false, ""}};
    tgt_schema.pk_columns = "id";
    auto tgt = catalog.create_table(db_id, tgt_schema);
    ASSERT_TRUE(tgt.has_value()) << tgt.error().message;

    auto et = engine.create_edge_type(db_id,
                                      "connected",
                                      *src,
                                      *tgt,
                                      TypeId::INT64,
                                      TypeId::INT64,
                                      {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    // Insert edges so edge_tables_ holds a live, non-empty EdgeTable.
    // (edge_storage_ stays empty — not populated without persistence.)
    ASSERT_TRUE(engine.link(db_id, "connected", Value(int64_t{1}), Value(int64_t{2})).has_value());
    ASSERT_TRUE(engine.link(db_id, "connected", Value(int64_t{1}), Value(int64_t{3})).has_value());

    // Verify the edge type is reachable before drop.
    auto before = engine.get_edges_from(db_id, "connected", Value(int64_t{1}));
    ASSERT_TRUE(before.has_value()) << before.error().message;
    EXPECT_EQ(before->size(), 2u);

    // drop_edge_type exercises the no-persistence teardown path:
    // edge_tables_.erase() + catalog_.drop_edge_type(). No storage block runs.
    auto drop1 = engine.drop_edge_type(db_id, "connected");
    ASSERT_TRUE(drop1.has_value()) << drop1.error().message;

    // Edge type and all in-memory data must be gone from edge_tables_.
    auto after = engine.get_edges_from(db_id, "connected", Value(int64_t{1}));
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code, StatusCode::NOT_FOUND);

    // Second drop returns NOT_FOUND (catalog entry also removed).
    auto drop2 = engine.drop_edge_type(db_id, "connected");
    ASSERT_FALSE(drop2.has_value());
    EXPECT_EQ(drop2.error().code, StatusCode::NOT_FOUND);
}
