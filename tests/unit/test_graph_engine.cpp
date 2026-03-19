#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/graph_engine.h"

#include <gtest/gtest.h>

using namespace sixseven;

// -- Helpers ------------------------------------------------------------------

static TableSchema make_table_schema(const std::string& name) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {
        {0, "id", TypeId::INT64, false, ""},
        {1, "name", TypeId::STRING, true, ""},
    };
    schema.pk_columns = "id";
    return schema;
}

static Value pk(int64_t v) {
    return Value(v);
}

/// Test fixture that sets up a Catalog with two tables and a GraphEngine.
class GraphEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto t1 = catalog_.create_table(default_database_id, make_table_schema("users"));
        ASSERT_TRUE(t1.has_value()) << t1.error().message;
        users_table_id_ = *t1;

        auto t2 = catalog_.create_table(default_database_id, make_table_schema("posts"));
        ASSERT_TRUE(t2.has_value()) << t2.error().message;
        posts_table_id_ = *t2;
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t users_table_id_ = 0;
    table_id_t posts_table_id_ = 0;
};

// == Create / Drop edge type ==================================================

TEST_F(GraphEngineTest, CreateEdgeType) {
    auto result = engine_.create_edge_type(default_database_id, 
        "authored", users_table_id_, posts_table_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_GE(*result, 1);
}

TEST_F(GraphEngineTest, CreateEdgeTypeDuplicateFails) {
    ASSERT_TRUE(
        engine_
            .create_edge_type(default_database_id, 
                "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    auto dup = engine_.create_edge_type(default_database_id, 
        "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {});
    EXPECT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, StatusCode::ALREADY_EXISTS);
}

TEST_F(GraphEngineTest, DropEdgeType) {
    ASSERT_TRUE(
        engine_
            .create_edge_type(default_database_id, 
                "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    auto drop = engine_.drop_edge_type(default_database_id, "follows");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // Edge type no longer available.
    auto link = engine_.link("follows", pk(1), pk(2));
    EXPECT_FALSE(link.has_value());
    EXPECT_EQ(link.error().code, StatusCode::NOT_FOUND);
}

TEST_F(GraphEngineTest, DropEdgeTypeNotFoundFails) {
    auto drop = engine_.drop_edge_type(default_database_id, "nonexistent");
    EXPECT_FALSE(drop.has_value());
    EXPECT_EQ(drop.error().code, StatusCode::NOT_FOUND);
}

TEST_F(GraphEngineTest, ListEdgeTypes) {
    ASSERT_TRUE(
        engine_
            .create_edge_type(default_database_id, 
                "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {})
            .has_value());
    ASSERT_TRUE(
        engine_
            .create_edge_type(default_database_id, 
                "authored", users_table_id_, posts_table_id_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    auto types = engine_.list_edge_types();
    EXPECT_EQ(types.size(), 2u);
}

// == LINK operation ===========================================================

TEST_F(GraphEngineTest, LinkCreatesEdge) {
    ASSERT_TRUE(
        engine_
            .create_edge_type(default_database_id, 
                "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    auto result = engine_.link("follows", pk(1), pk(2));
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_GE(*result, 1u);
}

TEST_F(GraphEngineTest, LinkWithProperties) {
    std::vector<ColumnDef> props = {{"weight", TypeId::FLOAT64}};
    ASSERT_TRUE(
        engine_
            .create_edge_type(default_database_id, 
                "rated", users_table_id_, posts_table_id_, TypeId::INT64, TypeId::INT64, props)
            .has_value());

    auto result = engine_.link("rated", pk(1), pk(10), {Value(4.5)});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto edges = engine_.get_edges_from("rated", pk(1));
    ASSERT_TRUE(edges.has_value());
    ASSERT_EQ(edges->size(), 1u);
    ASSERT_EQ((*edges)[0].properties.size(), 1u);
    EXPECT_DOUBLE_EQ((*edges)[0].properties[0].as_float64(), 4.5);
}

TEST_F(GraphEngineTest, LinkToNonexistentEdgeTypeFails) {
    auto result = engine_.link("nonexistent", pk(1), pk(2));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(GraphEngineTest, LinkDuplicatePreventionBlocks) {
    ASSERT_TRUE(
        engine_
            .create_edge_type(default_database_id, 
                "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {}, true)
            .has_value());

    ASSERT_TRUE(engine_.link("follows", pk(1), pk(2)).has_value());

    auto dup = engine_.link("follows", pk(1), pk(2));
    EXPECT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST_F(GraphEngineTest, LinkUpdatesForwardAndReverseIndexes) {
    ASSERT_TRUE(
        engine_
            .create_edge_type(default_database_id, 
                "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    ASSERT_TRUE(engine_.link("follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(engine_.link("follows", pk(1), pk(3)).has_value());
    ASSERT_TRUE(engine_.link("follows", pk(4), pk(2)).has_value());

    // Forward: user 1 follows users 2 and 3.
    auto from_1 = engine_.get_edges_from("follows", pk(1));
    ASSERT_TRUE(from_1.has_value());
    EXPECT_EQ(from_1->size(), 2u);

    // Reverse: user 2 is followed by users 1 and 4.
    auto to_2 = engine_.get_edges_to("follows", pk(2));
    ASSERT_TRUE(to_2.has_value());
    EXPECT_EQ(to_2->size(), 2u);
}

// == UNLINK operation =========================================================

TEST_F(GraphEngineTest, UnlinkRemovesEdge) {
    ASSERT_TRUE(
        engine_
            .create_edge_type(default_database_id, 
                "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    ASSERT_TRUE(engine_.link("follows", pk(1), pk(2)).has_value());

    auto result = engine_.unlink("follows", pk(1), pk(2));
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto edges = engine_.get_edges_from("follows", pk(1));
    ASSERT_TRUE(edges.has_value());
    EXPECT_TRUE(edges->empty());
}

TEST_F(GraphEngineTest, UnlinkUpdatesIndexes) {
    ASSERT_TRUE(
        engine_
            .create_edge_type(default_database_id, 
                "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    ASSERT_TRUE(engine_.link("follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(engine_.link("follows", pk(1), pk(3)).has_value());

    ASSERT_TRUE(engine_.unlink("follows", pk(1), pk(2)).has_value());

    // Forward: only edge to 3 remains.
    auto from_1 = engine_.get_edges_from("follows", pk(1));
    ASSERT_TRUE(from_1.has_value());
    ASSERT_EQ(from_1->size(), 1u);
    EXPECT_EQ((*from_1)[0].target_pk.as_int64(), 3);

    // Reverse: user 2 has no incoming edges.
    auto to_2 = engine_.get_edges_to("follows", pk(2));
    ASSERT_TRUE(to_2.has_value());
    EXPECT_TRUE(to_2->empty());
}

TEST_F(GraphEngineTest, UnlinkNonexistentEdgeFails) {
    ASSERT_TRUE(
        engine_
            .create_edge_type(default_database_id, 
                "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    auto result = engine_.unlink("follows", pk(1), pk(2));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(GraphEngineTest, UnlinkNonexistentEdgeTypeFails) {
    auto result = engine_.unlink("nonexistent", pk(1), pk(2));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(GraphEngineTest, UnlinkAllowsRelinkWithDuplicatePrevention) {
    ASSERT_TRUE(
        engine_
            .create_edge_type(default_database_id, 
                "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {}, true)
            .has_value());

    ASSERT_TRUE(engine_.link("follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(engine_.unlink("follows", pk(1), pk(2)).has_value());

    // Should be able to re-link after unlinking.
    auto relink = engine_.link("follows", pk(1), pk(2));
    ASSERT_TRUE(relink.has_value()) << relink.error().message;
}

// == UNLINK with WHERE ========================================================

TEST_F(GraphEngineTest, UnlinkWhereDeletesMatchingEdges) {
    std::vector<ColumnDef> props = {{"weight", TypeId::FLOAT64}};
    ASSERT_TRUE(
        engine_
            .create_edge_type(default_database_id, 
                "rated", users_table_id_, posts_table_id_, TypeId::INT64, TypeId::INT64, props)
            .has_value());

    ASSERT_TRUE(engine_.link("rated", pk(1), pk(10), {Value(4.5)}).has_value());
    ASSERT_TRUE(engine_.link("rated", pk(1), pk(10), {Value(2.0)}).has_value());
    ASSERT_TRUE(engine_.link("rated", pk(1), pk(10), {Value(5.0)}).has_value());

    // Delete edges where weight < 3.0.
    auto result = engine_.unlink_where("rated", pk(1), pk(10), [](const EdgeRow& e) {
        return e.properties[0].as_float64() < 3.0;
    });
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, 1u); // Only the 2.0 edge.

    auto remaining = engine_.get_edges_from("rated", pk(1));
    ASSERT_TRUE(remaining.has_value());
    EXPECT_EQ(remaining->size(), 2u);
}

TEST_F(GraphEngineTest, UnlinkWhereDeletesAllMatching) {
    ASSERT_TRUE(
        engine_
            .create_edge_type(default_database_id, 
                "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    ASSERT_TRUE(engine_.link("follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(engine_.link("follows", pk(1), pk(2)).has_value()); // Duplicate allowed.

    auto result =
        engine_.unlink_where("follows", pk(1), pk(2), [](const EdgeRow&) { return true; });
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, 2u);

    auto remaining = engine_.get_edges_from("follows", pk(1));
    ASSERT_TRUE(remaining.has_value());
    EXPECT_TRUE(remaining->empty());
}

TEST_F(GraphEngineTest, UnlinkWhereNoMatches) {
    ASSERT_TRUE(
        engine_
            .create_edge_type(default_database_id, 
                "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    ASSERT_TRUE(engine_.link("follows", pk(1), pk(2)).has_value());

    auto result =
        engine_.unlink_where("follows", pk(1), pk(2), [](const EdgeRow&) { return false; });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0u);

    // Edge should still exist.
    auto edges = engine_.get_edges_from("follows", pk(1));
    ASSERT_TRUE(edges.has_value());
    EXPECT_EQ(edges->size(), 1u);
}

TEST_F(GraphEngineTest, UnlinkWhereNonexistentEdgeTypeFails) {
    auto result =
        engine_.unlink_where("nonexistent", pk(1), pk(2), [](const EdgeRow&) { return true; });
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// == Edge lookups =============================================================

TEST_F(GraphEngineTest, GetEdgesFromNonexistentTypeFails) {
    auto result = engine_.get_edges_from("nonexistent", pk(1));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(GraphEngineTest, GetEdgesToNonexistentTypeFails) {
    auto result = engine_.get_edges_to("nonexistent", pk(1));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(GraphEngineTest, GetEdgeTableReturnsPointer) {
    ASSERT_TRUE(
        engine_
            .create_edge_type(default_database_id, 
                "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    auto table = engine_.get_edge_table("follows");
    ASSERT_TRUE(table.has_value()) << table.error().message;
    EXPECT_NE(*table, nullptr);
    EXPECT_EQ((*table)->config().name, "follows");
}

// == Self-referencing edge type ===============================================

TEST_F(GraphEngineTest, SelfReferenceLinkUnlink) {
    ASSERT_TRUE(
        engine_
            .create_edge_type(default_database_id, 
                "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    ASSERT_TRUE(engine_.link("follows", pk(1), pk(1)).has_value());

    auto from = engine_.get_edges_from("follows", pk(1));
    ASSERT_TRUE(from.has_value());
    EXPECT_EQ(from->size(), 1u);

    ASSERT_TRUE(engine_.unlink("follows", pk(1), pk(1)).has_value());

    from = engine_.get_edges_from("follows", pk(1));
    ASSERT_TRUE(from.has_value());
    EXPECT_TRUE(from->empty());
}

// == Multiple edge types ======================================================

TEST_F(GraphEngineTest, MultipleEdgeTypesAreIndependent) {
    ASSERT_TRUE(
        engine_
            .create_edge_type(default_database_id, 
                "follows", users_table_id_, users_table_id_, TypeId::INT64, TypeId::INT64, {})
            .has_value());
    ASSERT_TRUE(
        engine_
            .create_edge_type(default_database_id, 
                "authored", users_table_id_, posts_table_id_, TypeId::INT64, TypeId::INT64, {})
            .has_value());

    ASSERT_TRUE(engine_.link("follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(engine_.link("authored", pk(1), pk(100)).has_value());

    auto follows = engine_.get_edges_from("follows", pk(1));
    ASSERT_TRUE(follows.has_value());
    EXPECT_EQ(follows->size(), 1u);
    EXPECT_EQ((*follows)[0].target_pk.as_int64(), 2);

    auto authored = engine_.get_edges_from("authored", pk(1));
    ASSERT_TRUE(authored.has_value());
    EXPECT_EQ(authored->size(), 1u);
    EXPECT_EQ((*authored)[0].target_pk.as_int64(), 100);
}
