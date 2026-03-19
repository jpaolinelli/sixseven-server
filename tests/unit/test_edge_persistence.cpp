#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>

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
        "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link("follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(engine_->link("follows", pk(1), pk(3)).has_value());
    ASSERT_TRUE(engine_->link("follows", pk(2), pk(3)).has_value());

    // Simulate restart.
    engine_ = restart_engine();

    // Verify edges are still there.
    auto from_1 = engine_->get_edges_from("follows", pk(1));
    ASSERT_TRUE(from_1.has_value()) << from_1.error().message;
    EXPECT_EQ(from_1->size(), 2u);

    auto from_2 = engine_->get_edges_from("follows", pk(2));
    ASSERT_TRUE(from_2.has_value()) << from_2.error().message;
    EXPECT_EQ(from_2->size(), 1u);
    EXPECT_EQ((*from_2)[0].target_pk.as_int64(), 3);
}

TEST_F(EdgePersistenceTest, ReverseIndexRecoveredAfterRestart) {
    auto et = engine_->create_edge_type(default_database_id, 
        "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link("follows", pk(1), pk(3)).has_value());
    ASSERT_TRUE(engine_->link("follows", pk(2), pk(3)).has_value());

    engine_ = restart_engine();

    // Reverse lookup: node 3 has incoming edges from 1 and 2.
    auto to_3 = engine_->get_edges_to("follows", pk(3));
    ASSERT_TRUE(to_3.has_value()) << to_3.error().message;
    EXPECT_EQ(to_3->size(), 2u);
}

TEST_F(EdgePersistenceTest, EdgePropertiesPersisted) {
    std::vector<ColumnDef> props = {{"weight", TypeId::FLOAT64}, {"label", TypeId::STRING}};
    auto et = engine_->create_edge_type(default_database_id, 
        "rated", users_table_id_, posts_table_id_, TypeId::INT64, TypeId::INT64, props);
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link("rated", pk(1), pk(10), {Value(4.5), Value(std::string("great"))})
                    .has_value());

    engine_ = restart_engine();

    auto edges = engine_->get_edges_from("rated", pk(1));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    ASSERT_EQ(edges->size(), 1u);
    ASSERT_EQ((*edges)[0].properties.size(), 2u);
    EXPECT_DOUBLE_EQ((*edges)[0].properties[0].as_float64(), 4.5);
    EXPECT_EQ((*edges)[0].properties[1].as_string(), "great");
}

// == UNLINK persistence =======================================================

TEST_F(EdgePersistenceTest, UnlinkedEdgesNotRecovered) {
    auto et = engine_->create_edge_type(default_database_id, 
        "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link("follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(engine_->link("follows", pk(1), pk(3)).has_value());

    // Unlink one edge.
    ASSERT_TRUE(engine_->unlink("follows", pk(1), pk(2)).has_value());

    engine_ = restart_engine();

    // Only pk(3) edge should remain.
    auto edges = engine_->get_edges_from("follows", pk(1));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    ASSERT_EQ(edges->size(), 1u);
    EXPECT_EQ((*edges)[0].target_pk.as_int64(), 3);
}

TEST_F(EdgePersistenceTest, UnlinkWherePersistedCorrectly) {
    std::vector<ColumnDef> props = {{"weight", TypeId::FLOAT64}};
    auto et = engine_->create_edge_type(default_database_id, 
        "rated", users_table_id_, posts_table_id_, TypeId::INT64, TypeId::INT64, props);
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link("rated", pk(1), pk(10), {Value(2.0)}).has_value());
    ASSERT_TRUE(engine_->link("rated", pk(1), pk(10), {Value(4.5)}).has_value());
    ASSERT_TRUE(engine_->link("rated", pk(1), pk(10), {Value(5.0)}).has_value());

    // Delete edges where weight < 3.0.
    auto result = engine_->unlink_where("rated", pk(1), pk(10), [](const EdgeRow& e) {
        return e.properties[0].as_float64() < 3.0;
    });
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, 1u);

    engine_ = restart_engine();

    // Two edges should remain (4.5 and 5.0).
    auto edges = engine_->get_edges_from("rated", pk(1));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    EXPECT_EQ(edges->size(), 2u);
}

// == Multiple edge types ======================================================

TEST_F(EdgePersistenceTest, MultipleEdgeTypesPersisted) {
    auto et1 = engine_->create_edge_type(default_database_id, 
        "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et1.has_value()) << et1.error().message;

    auto et2 = engine_->create_edge_type(default_database_id, 
        "authored", users_table_id_, posts_table_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et2.has_value()) << et2.error().message;

    ASSERT_TRUE(engine_->link("follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(engine_->link("authored", pk(1), pk(100)).has_value());

    engine_ = restart_engine();

    auto follows = engine_->get_edges_from("follows", pk(1));
    ASSERT_TRUE(follows.has_value()) << follows.error().message;
    EXPECT_EQ(follows->size(), 1u);
    EXPECT_EQ((*follows)[0].target_pk.as_int64(), 2);

    auto authored = engine_->get_edges_from("authored", pk(1));
    ASSERT_TRUE(authored.has_value()) << authored.error().message;
    EXPECT_EQ(authored->size(), 1u);
    EXPECT_EQ((*authored)[0].target_pk.as_int64(), 100);
}

// == Row ID continuity ========================================================

TEST_F(EdgePersistenceTest, NewEdgesAfterRestartGetUniqueRowIds) {
    auto et = engine_->create_edge_type(default_database_id, 
        "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    auto id1 = engine_->link("follows", pk(1), pk(2));
    ASSERT_TRUE(id1.has_value()) << id1.error().message;

    auto id2 = engine_->link("follows", pk(1), pk(3));
    ASSERT_TRUE(id2.has_value()) << id2.error().message;

    engine_ = restart_engine();

    // Insert a new edge after restart.
    auto id3 = engine_->link("follows", pk(1), pk(4));
    ASSERT_TRUE(id3.has_value()) << id3.error().message;

    // New row ID should be greater than any restored row ID.
    EXPECT_GT(*id3, *id2);

    // All three edges should be present.
    auto edges = engine_->get_edges_from("follows", pk(1));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    EXPECT_EQ(edges->size(), 3u);
}

// == Empty edge type ==========================================================

TEST_F(EdgePersistenceTest, EmptyEdgeTypeRecoveredCorrectly) {
    auto et = engine_->create_edge_type(default_database_id, 
        "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    // No edges inserted.
    engine_ = restart_engine();

    // Edge type should exist but be empty.
    auto edges = engine_->get_edges_from("follows", pk(1));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    EXPECT_TRUE(edges->empty());
}

// == Drop edge type persistence ===============================================

TEST_F(EdgePersistenceTest, DroppedEdgeTypeFileRemoved) {
    auto et = engine_->create_edge_type(default_database_id, 
        "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link("follows", pk(1), pk(2)).has_value());

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
        "self_link", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link("self_link", pk(1), pk(1)).has_value());

    engine_ = restart_engine();

    auto from = engine_->get_edges_from("self_link", pk(1));
    ASSERT_TRUE(from.has_value()) << from.error().message;
    EXPECT_EQ(from->size(), 1u);
    EXPECT_EQ((*from)[0].source_pk.as_int64(), 1);
    EXPECT_EQ((*from)[0].target_pk.as_int64(), 1);
}

// == Multiple restarts ========================================================

TEST_F(EdgePersistenceTest, DataSurvivesMultipleRestarts) {
    auto et = engine_->create_edge_type(default_database_id, 
        "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et.has_value()) << et.error().message;

    ASSERT_TRUE(engine_->link("follows", pk(1), pk(2)).has_value());

    // First restart.
    engine_ = restart_engine();

    // Add more edges after first restart.
    ASSERT_TRUE(engine_->link("follows", pk(2), pk(3)).has_value());

    // Second restart.
    engine_ = restart_engine();

    auto from_1 = engine_->get_edges_from("follows", pk(1));
    ASSERT_TRUE(from_1.has_value()) << from_1.error().message;
    EXPECT_EQ(from_1->size(), 1u);

    auto from_2 = engine_->get_edges_from("follows", pk(2));
    ASSERT_TRUE(from_2.has_value()) << from_2.error().message;
    EXPECT_EQ(from_2->size(), 1u);
}

// == Restore edge in EdgeTable ================================================

TEST(EdgeTableRestore, RestoreEdgeSetsCorrectRowId) {
    Catalog catalog;
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
    EXPECT_FALSE(dup.has_value());
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
