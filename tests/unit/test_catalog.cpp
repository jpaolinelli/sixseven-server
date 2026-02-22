#include "giodb/catalog/catalog.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

using namespace giodb;

// -- Helper: build a simple TableSchema ---------------------------------------

static TableSchema make_schema(const std::string& name) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {
        {0, "id", TypeId::INT32, false, ""},
        {1, "name", TypeId::STRING, true, ""},
        {2, "active", TypeId::BOOL, true, "true"},
    };
    schema.pk_columns = "id";
    return schema;
}

// -- Create table -------------------------------------------------------------

TEST(Catalog, CreateTableAssignsId) {
    Catalog catalog;

    auto id = catalog.create_table(make_schema("users"));
    ASSERT_TRUE(id.has_value()) << id.error().message;
    EXPECT_GE(*id, 1);
}

TEST(Catalog, CreateTableSequentialIds) {
    Catalog catalog;

    auto id1 = catalog.create_table(make_schema("t1"));
    auto id2 = catalog.create_table(make_schema("t2"));
    auto id3 = catalog.create_table(make_schema("t3"));
    ASSERT_TRUE(id1.has_value());
    ASSERT_TRUE(id2.has_value());
    ASSERT_TRUE(id3.has_value());

    EXPECT_EQ(*id2, *id1 + 1);
    EXPECT_EQ(*id3, *id2 + 1);
}

TEST(Catalog, CreateTableDuplicateNameFails) {
    Catalog catalog;

    auto id1 = catalog.create_table(make_schema("users"));
    ASSERT_TRUE(id1.has_value());

    auto id2 = catalog.create_table(make_schema("users"));
    EXPECT_FALSE(id2.has_value());
    EXPECT_EQ(id2.error().code, StatusCode::ALREADY_EXISTS);
}

TEST(Catalog, CreateTableAssignsOrdinals) {
    Catalog catalog;

    TableSchema schema;
    schema.name = "test";
    schema.columns = {
        {-1, "a", TypeId::INT32, false, ""},
        {-1, "b", TypeId::STRING, true, ""},
    };

    auto id = catalog.create_table(schema);
    ASSERT_TRUE(id.has_value());

    auto retrieved = catalog.get_table("test");
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->columns[0].ordinal, 0);
    EXPECT_EQ(retrieved->columns[1].ordinal, 1);
}

// -- Get table ----------------------------------------------------------------

TEST(Catalog, GetTableByName) {
    Catalog catalog;

    auto id = catalog.create_table(make_schema("users"));
    ASSERT_TRUE(id.has_value());

    auto schema = catalog.get_table("users");
    ASSERT_TRUE(schema.has_value()) << schema.error().message;
    EXPECT_EQ(schema->name, "users");
    EXPECT_EQ(schema->table_id, *id);
    EXPECT_EQ(schema->columns.size(), 3u);
    EXPECT_EQ(schema->columns[0].name, "id");
    EXPECT_EQ(schema->columns[0].type_id, TypeId::INT32);
    EXPECT_FALSE(schema->columns[0].nullable);
    EXPECT_EQ(schema->columns[1].name, "name");
    EXPECT_TRUE(schema->columns[1].nullable);
    EXPECT_EQ(schema->pk_columns, "id");
}

TEST(Catalog, GetTableByIdWorks) {
    Catalog catalog;

    auto id = catalog.create_table(make_schema("users"));
    ASSERT_TRUE(id.has_value());

    auto schema = catalog.get_table_by_id(*id);
    ASSERT_TRUE(schema.has_value());
    EXPECT_EQ(schema->name, "users");
}

TEST(Catalog, GetTableNotFoundByName) {
    Catalog catalog;
    auto result = catalog.get_table("nonexistent");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(Catalog, GetTableNotFoundById) {
    Catalog catalog;
    auto result = catalog.get_table_by_id(999);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// -- List tables --------------------------------------------------------------

TEST(Catalog, ListTablesEmpty) {
    Catalog catalog;
    auto tables = catalog.list_tables();
    EXPECT_TRUE(tables.empty());
}

TEST(Catalog, ListTablesMultiple) {
    Catalog catalog;

    ASSERT_TRUE(catalog.create_table(make_schema("alpha")).has_value());
    ASSERT_TRUE(catalog.create_table(make_schema("beta")).has_value());
    ASSERT_TRUE(catalog.create_table(make_schema("gamma")).has_value());

    auto tables = catalog.list_tables();
    EXPECT_EQ(tables.size(), 3u);

    // Should be sorted by table_id.
    EXPECT_EQ(tables[0].name, "alpha");
    EXPECT_EQ(tables[1].name, "beta");
    EXPECT_EQ(tables[2].name, "gamma");
}

// -- Drop table ---------------------------------------------------------------

TEST(Catalog, DropTable) {
    Catalog catalog;

    auto id = catalog.create_table(make_schema("users"));
    ASSERT_TRUE(id.has_value());

    auto drop = catalog.drop_table("users");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    auto get = catalog.get_table("users");
    EXPECT_FALSE(get.has_value());
    EXPECT_EQ(get.error().code, StatusCode::NOT_FOUND);
}

TEST(Catalog, DropTableNotFound) {
    Catalog catalog;
    auto drop = catalog.drop_table("nonexistent");
    EXPECT_FALSE(drop.has_value());
    EXPECT_EQ(drop.error().code, StatusCode::NOT_FOUND);
}

TEST(Catalog, DropTableRemovesFromList) {
    Catalog catalog;

    ASSERT_TRUE(catalog.create_table(make_schema("t1")).has_value());
    ASSERT_TRUE(catalog.create_table(make_schema("t2")).has_value());
    ASSERT_TRUE(catalog.create_table(make_schema("t3")).has_value());

    ASSERT_TRUE(catalog.drop_table("t2").has_value());

    auto tables = catalog.list_tables();
    EXPECT_EQ(tables.size(), 2u);
    EXPECT_EQ(tables[0].name, "t1");
    EXPECT_EQ(tables[1].name, "t3");
}

TEST(Catalog, DropTableAllowsRecreateWithSameName) {
    Catalog catalog;

    auto id1 = catalog.create_table(make_schema("users"));
    ASSERT_TRUE(id1.has_value());

    ASSERT_TRUE(catalog.drop_table("users").has_value());

    auto id2 = catalog.create_table(make_schema("users"));
    ASSERT_TRUE(id2.has_value());
    EXPECT_NE(*id1, *id2); // New ID should be different.
}

// -- Create index -------------------------------------------------------------

TEST(Catalog, CreateIndex) {
    Catalog catalog;

    auto tid = catalog.create_table(make_schema("users"));
    ASSERT_TRUE(tid.has_value());

    IndexDef def;
    def.table_id = *tid;
    def.name = "idx_users_name";
    def.index_type = "btree";
    def.columns = "name";
    def.is_unique = false;

    auto idx_id = catalog.create_index(def);
    ASSERT_TRUE(idx_id.has_value()) << idx_id.error().message;
    EXPECT_GE(*idx_id, 1);
}

TEST(Catalog, CreateIndexDuplicateNameFails) {
    Catalog catalog;

    auto tid = catalog.create_table(make_schema("users"));
    ASSERT_TRUE(tid.has_value());

    IndexDef def;
    def.table_id = *tid;
    def.name = "idx_users_name";
    def.index_type = "btree";
    def.columns = "name";

    ASSERT_TRUE(catalog.create_index(def).has_value());

    auto dup = catalog.create_index(def);
    EXPECT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, StatusCode::ALREADY_EXISTS);
}

TEST(Catalog, CreateIndexNonexistentTableFails) {
    Catalog catalog;

    IndexDef def;
    def.table_id = 999;
    def.name = "idx_missing";
    def.index_type = "btree";
    def.columns = "id";

    auto result = catalog.create_index(def);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// -- Get index ----------------------------------------------------------------

TEST(Catalog, GetIndex) {
    Catalog catalog;

    auto tid = catalog.create_table(make_schema("users"));
    ASSERT_TRUE(tid.has_value());

    IndexDef def;
    def.table_id = *tid;
    def.name = "idx_users_name";
    def.index_type = "btree";
    def.columns = "name";
    def.is_unique = true;

    auto idx_id = catalog.create_index(def);
    ASSERT_TRUE(idx_id.has_value());

    auto retrieved = catalog.get_index("idx_users_name");
    ASSERT_TRUE(retrieved.has_value()) << retrieved.error().message;
    EXPECT_EQ(retrieved->name, "idx_users_name");
    EXPECT_EQ(retrieved->table_id, *tid);
    EXPECT_EQ(retrieved->index_type, "btree");
    EXPECT_EQ(retrieved->columns, "name");
    EXPECT_TRUE(retrieved->is_unique);
    EXPECT_EQ(retrieved->index_id, *idx_id);
}

TEST(Catalog, GetIndexNotFound) {
    Catalog catalog;
    auto result = catalog.get_index("nonexistent");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// -- List indexes -------------------------------------------------------------

TEST(Catalog, ListIndexesForTable) {
    Catalog catalog;

    auto t1 = catalog.create_table(make_schema("t1"));
    auto t2 = catalog.create_table(make_schema("t2"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    IndexDef d1;
    d1.table_id = *t1;
    d1.name = "idx_t1_a";
    d1.index_type = "btree";
    d1.columns = "id";
    ASSERT_TRUE(catalog.create_index(d1).has_value());

    IndexDef d2;
    d2.table_id = *t1;
    d2.name = "idx_t1_b";
    d2.index_type = "btree";
    d2.columns = "name";
    ASSERT_TRUE(catalog.create_index(d2).has_value());

    IndexDef d3;
    d3.table_id = *t2;
    d3.name = "idx_t2_a";
    d3.index_type = "btree";
    d3.columns = "id";
    ASSERT_TRUE(catalog.create_index(d3).has_value());

    auto t1_indexes = catalog.list_indexes(*t1);
    EXPECT_EQ(t1_indexes.size(), 2u);

    auto t2_indexes = catalog.list_indexes(*t2);
    EXPECT_EQ(t2_indexes.size(), 1u);
}

TEST(Catalog, ListAllIndexes) {
    Catalog catalog;

    auto tid = catalog.create_table(make_schema("users"));
    ASSERT_TRUE(tid.has_value());

    for (int i = 0; i < 3; ++i) {
        IndexDef def;
        def.table_id = *tid;
        def.name = "idx_" + std::to_string(i);
        def.index_type = "btree";
        def.columns = "id";
        ASSERT_TRUE(catalog.create_index(def).has_value());
    }

    auto all = catalog.list_all_indexes();
    EXPECT_EQ(all.size(), 3u);
}

// -- Drop index ---------------------------------------------------------------

TEST(Catalog, DropIndex) {
    Catalog catalog;

    auto tid = catalog.create_table(make_schema("users"));
    ASSERT_TRUE(tid.has_value());

    IndexDef def;
    def.table_id = *tid;
    def.name = "idx_users_name";
    def.index_type = "btree";
    def.columns = "name";
    ASSERT_TRUE(catalog.create_index(def).has_value());

    auto drop = catalog.drop_index("idx_users_name");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    auto get = catalog.get_index("idx_users_name");
    EXPECT_FALSE(get.has_value());
}

TEST(Catalog, DropIndexNotFound) {
    Catalog catalog;
    auto drop = catalog.drop_index("nonexistent");
    EXPECT_FALSE(drop.has_value());
    EXPECT_EQ(drop.error().code, StatusCode::NOT_FOUND);
}

// -- Drop table cascades to indexes -------------------------------------------

TEST(Catalog, DropTableCascadesToIndexes) {
    Catalog catalog;

    auto tid = catalog.create_table(make_schema("users"));
    ASSERT_TRUE(tid.has_value());

    IndexDef d1;
    d1.table_id = *tid;
    d1.name = "idx_a";
    d1.index_type = "btree";
    d1.columns = "id";
    ASSERT_TRUE(catalog.create_index(d1).has_value());

    IndexDef d2;
    d2.table_id = *tid;
    d2.name = "idx_b";
    d2.index_type = "btree";
    d2.columns = "name";
    ASSERT_TRUE(catalog.create_index(d2).has_value());

    ASSERT_TRUE(catalog.drop_table("users").has_value());

    // Both indexes should be gone.
    EXPECT_FALSE(catalog.get_index("idx_a").has_value());
    EXPECT_FALSE(catalog.get_index("idx_b").has_value());
    EXPECT_TRUE(catalog.list_all_indexes().empty());
}

// -- Cache invalidation (DDL consistency) -------------------------------------

TEST(Catalog, CacheInvalidationOnDrop) {
    Catalog catalog;

    auto id = catalog.create_table(make_schema("users"));
    ASSERT_TRUE(id.has_value());

    // Cache hit.
    auto schema1 = catalog.get_table("users");
    ASSERT_TRUE(schema1.has_value());

    // Drop invalidates.
    ASSERT_TRUE(catalog.drop_table("users").has_value());

    // Cache miss.
    auto schema2 = catalog.get_table("users");
    EXPECT_FALSE(schema2.has_value());
}

TEST(Catalog, CacheInvalidationOnRecreate) {
    Catalog catalog;

    auto id1 = catalog.create_table(make_schema("users"));
    ASSERT_TRUE(id1.has_value());

    ASSERT_TRUE(catalog.drop_table("users").has_value());

    // Recreate with different columns.
    TableSchema new_schema;
    new_schema.name = "users";
    new_schema.columns = {{0, "user_id", TypeId::INT64, false, ""}};

    auto id2 = catalog.create_table(new_schema);
    ASSERT_TRUE(id2.has_value());

    auto retrieved = catalog.get_table("users");
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->columns.size(), 1u);
    EXPECT_EQ(retrieved->columns[0].name, "user_id");
    EXPECT_EQ(retrieved->columns[0].type_id, TypeId::INT64);
}

// -- Edge type operations -----------------------------------------------------

TEST(Catalog, CreateEdgeType) {
    Catalog catalog;

    auto t1 = catalog.create_table(make_schema("users"));
    auto t2 = catalog.create_table(make_schema("posts"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    EdgeTypeDef def;
    def.name = "authored";
    def.source_table_id = *t1;
    def.target_table_id = *t2;
    def.properties = "created_at";

    auto eid = catalog.create_edge_type(def);
    ASSERT_TRUE(eid.has_value()) << eid.error().message;
    EXPECT_GE(*eid, 1);
}

TEST(Catalog, CreateEdgeTypeDuplicateNameFails) {
    Catalog catalog;

    auto t1 = catalog.create_table(make_schema("a"));
    auto t2 = catalog.create_table(make_schema("b"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    EdgeTypeDef def;
    def.name = "follows";
    def.source_table_id = *t1;
    def.target_table_id = *t2;
    ASSERT_TRUE(catalog.create_edge_type(def).has_value());

    auto dup = catalog.create_edge_type(def);
    EXPECT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, StatusCode::ALREADY_EXISTS);
}

TEST(Catalog, CreateEdgeTypeSourceTableNotFoundFails) {
    Catalog catalog;

    auto t1 = catalog.create_table(make_schema("a"));
    ASSERT_TRUE(t1.has_value());

    EdgeTypeDef def;
    def.name = "broken_edge";
    def.source_table_id = 999;
    def.target_table_id = *t1;

    auto result = catalog.create_edge_type(def);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(Catalog, CreateEdgeTypeTargetTableNotFoundFails) {
    Catalog catalog;

    auto t1 = catalog.create_table(make_schema("a"));
    ASSERT_TRUE(t1.has_value());

    EdgeTypeDef def;
    def.name = "broken_edge";
    def.source_table_id = *t1;
    def.target_table_id = 999;

    auto result = catalog.create_edge_type(def);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(Catalog, GetEdgeType) {
    Catalog catalog;

    auto t1 = catalog.create_table(make_schema("users"));
    auto t2 = catalog.create_table(make_schema("posts"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    EdgeTypeDef def;
    def.name = "authored";
    def.source_table_id = *t1;
    def.target_table_id = *t2;
    def.properties = "weight";

    auto eid = catalog.create_edge_type(def);
    ASSERT_TRUE(eid.has_value());

    auto retrieved = catalog.get_edge_type("authored");
    ASSERT_TRUE(retrieved.has_value()) << retrieved.error().message;
    EXPECT_EQ(retrieved->name, "authored");
    EXPECT_EQ(retrieved->source_table_id, *t1);
    EXPECT_EQ(retrieved->target_table_id, *t2);
    EXPECT_EQ(retrieved->properties, "weight");
    EXPECT_EQ(retrieved->edge_id, *eid);
}

TEST(Catalog, GetEdgeTypeNotFound) {
    Catalog catalog;
    auto result = catalog.get_edge_type("nonexistent");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(Catalog, DropEdgeType) {
    Catalog catalog;

    auto t1 = catalog.create_table(make_schema("a"));
    auto t2 = catalog.create_table(make_schema("b"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    EdgeTypeDef def;
    def.name = "follows";
    def.source_table_id = *t1;
    def.target_table_id = *t2;
    ASSERT_TRUE(catalog.create_edge_type(def).has_value());

    auto drop = catalog.drop_edge_type("follows");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    EXPECT_FALSE(catalog.get_edge_type("follows").has_value());
}

TEST(Catalog, DropEdgeTypeNotFound) {
    Catalog catalog;
    auto drop = catalog.drop_edge_type("nonexistent");
    EXPECT_FALSE(drop.has_value());
    EXPECT_EQ(drop.error().code, StatusCode::NOT_FOUND);
}

TEST(Catalog, ListEdgeTypes) {
    Catalog catalog;

    auto t1 = catalog.create_table(make_schema("a"));
    auto t2 = catalog.create_table(make_schema("b"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    EdgeTypeDef d1;
    d1.name = "follows";
    d1.source_table_id = *t1;
    d1.target_table_id = *t2;
    ASSERT_TRUE(catalog.create_edge_type(d1).has_value());

    EdgeTypeDef d2;
    d2.name = "likes";
    d2.source_table_id = *t1;
    d2.target_table_id = *t2;
    ASSERT_TRUE(catalog.create_edge_type(d2).has_value());

    auto edges = catalog.list_edge_types();
    EXPECT_EQ(edges.size(), 2u);
    EXPECT_EQ(edges[0].name, "follows");
    EXPECT_EQ(edges[1].name, "likes");
}

TEST(Catalog, ListEdgeTypesEmpty) {
    Catalog catalog;
    auto edges = catalog.list_edge_types();
    EXPECT_TRUE(edges.empty());
}

TEST(Catalog, CreateEdgeTypeSelfReference) {
    Catalog catalog;

    auto t1 = catalog.create_table(make_schema("users"));
    ASSERT_TRUE(t1.has_value());

    EdgeTypeDef def;
    def.name = "follows";
    def.source_table_id = *t1;
    def.target_table_id = *t1; // Self-reference is valid.

    auto eid = catalog.create_edge_type(def);
    ASSERT_TRUE(eid.has_value()) << eid.error().message;
}

// -- Embedding column operations ----------------------------------------------

TEST(Catalog, RegisterEmbeddingColumn) {
    Catalog catalog;

    auto tid = catalog.create_table(make_schema("documents"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.table_id = *tid;
    def.column_id = 1;
    def.dimension = 384;
    def.source_expr = "name";
    def.provider = "openai";

    auto result = catalog.register_embedding_column(def);
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

TEST(Catalog, RegisterEmbeddingColumnDuplicateFails) {
    Catalog catalog;

    auto tid = catalog.create_table(make_schema("documents"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.table_id = *tid;
    def.column_id = 1;
    def.dimension = 384;
    def.source_expr = "name";
    def.provider = "openai";

    ASSERT_TRUE(catalog.register_embedding_column(def).has_value());

    auto dup = catalog.register_embedding_column(def);
    EXPECT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, StatusCode::ALREADY_EXISTS);
}

TEST(Catalog, RegisterEmbeddingColumnNonexistentTableFails) {
    Catalog catalog;

    EmbeddingColumnDef def;
    def.table_id = 999;
    def.column_id = 0;
    def.dimension = 128;

    auto result = catalog.register_embedding_column(def);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(Catalog, ListEmbeddingColumnsForTable) {
    Catalog catalog;

    auto t1 = catalog.create_table(make_schema("t1"));
    auto t2 = catalog.create_table(make_schema("t2"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    EmbeddingColumnDef d1;
    d1.table_id = *t1;
    d1.column_id = 0;
    d1.dimension = 128;
    d1.source_expr = "name";
    d1.provider = "openai";
    ASSERT_TRUE(catalog.register_embedding_column(d1).has_value());

    EmbeddingColumnDef d2;
    d2.table_id = *t1;
    d2.column_id = 1;
    d2.dimension = 256;
    d2.source_expr = "name,active";
    d2.provider = "cohere";
    ASSERT_TRUE(catalog.register_embedding_column(d2).has_value());

    EmbeddingColumnDef d3;
    d3.table_id = *t2;
    d3.column_id = 0;
    d3.dimension = 512;
    d3.source_expr = "id";
    d3.provider = "local";
    ASSERT_TRUE(catalog.register_embedding_column(d3).has_value());

    auto t1_embs = catalog.list_embedding_columns(*t1);
    EXPECT_EQ(t1_embs.size(), 2u);

    auto t2_embs = catalog.list_embedding_columns(*t2);
    EXPECT_EQ(t2_embs.size(), 1u);
    EXPECT_EQ(t2_embs[0].dimension, 512);
    EXPECT_EQ(t2_embs[0].provider, "local");
}

TEST(Catalog, ListAllEmbeddingColumns) {
    Catalog catalog;

    auto tid = catalog.create_table(make_schema("t1"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef d1;
    d1.table_id = *tid;
    d1.column_id = 0;
    d1.dimension = 128;
    ASSERT_TRUE(catalog.register_embedding_column(d1).has_value());

    EmbeddingColumnDef d2;
    d2.table_id = *tid;
    d2.column_id = 1;
    d2.dimension = 256;
    ASSERT_TRUE(catalog.register_embedding_column(d2).has_value());

    auto all = catalog.list_all_embedding_columns();
    EXPECT_EQ(all.size(), 2u);
}

TEST(Catalog, ListEmbeddingColumnsEmpty) {
    Catalog catalog;
    auto result = catalog.list_all_embedding_columns();
    EXPECT_TRUE(result.empty());
}

// -- drop_table cascade to edge types -----------------------------------------

TEST(Catalog, DropTableCascadesToEdgeTypes) {
    Catalog catalog;

    auto src_id = catalog.create_table(make_schema("src"));
    auto tgt_id = catalog.create_table(make_schema("tgt"));
    ASSERT_TRUE(src_id.has_value());
    ASSERT_TRUE(tgt_id.has_value());

    EdgeTypeDef edge;
    edge.name = "follows";
    edge.source_table_id = *src_id;
    edge.target_table_id = *tgt_id;
    auto eid = catalog.create_edge_type(edge);
    ASSERT_TRUE(eid.has_value());

    // Drop the source table — edge type should be removed.
    auto drop = catalog.drop_table("src");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    auto edges = catalog.list_edge_types();
    EXPECT_TRUE(edges.empty());

    auto get_edge = catalog.get_edge_type("follows");
    EXPECT_FALSE(get_edge.has_value());
    EXPECT_EQ(get_edge.error().code, StatusCode::NOT_FOUND);
}

TEST(Catalog, DropTableCascadesToEdgeTypesAsTarget) {
    Catalog catalog;

    auto src_id = catalog.create_table(make_schema("src"));
    auto tgt_id = catalog.create_table(make_schema("tgt"));
    ASSERT_TRUE(src_id.has_value());
    ASSERT_TRUE(tgt_id.has_value());

    EdgeTypeDef edge;
    edge.name = "likes";
    edge.source_table_id = *src_id;
    edge.target_table_id = *tgt_id;
    auto eid = catalog.create_edge_type(edge);
    ASSERT_TRUE(eid.has_value());

    // Drop the target table — edge type should be removed.
    auto drop = catalog.drop_table("tgt");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    auto edges = catalog.list_edge_types();
    EXPECT_TRUE(edges.empty());
}

// -- drop_table cascade to embedding columns ----------------------------------

TEST(Catalog, DropTableCascadesToEmbeddingColumns) {
    Catalog catalog;

    auto tid = catalog.create_table(make_schema("docs"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef emb;
    emb.table_id = *tid;
    emb.column_id = 1;
    emb.dimension = 128;
    emb.source_expr = "name";
    emb.provider = "test";
    auto reg = catalog.register_embedding_column(emb);
    ASSERT_TRUE(reg.has_value()) << reg.error().message;

    // Verify it exists.
    auto before = catalog.list_embedding_columns(*tid);
    EXPECT_EQ(before.size(), 1u);

    // Drop the table — embedding columns should be removed.
    auto drop = catalog.drop_table("docs");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    auto after = catalog.list_all_embedding_columns();
    EXPECT_TRUE(after.empty());
}
