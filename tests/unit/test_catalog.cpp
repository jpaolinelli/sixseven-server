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

    auto id = catalog.create_table(default_database_id, make_schema("users"));
    ASSERT_TRUE(id.has_value()) << id.error().message;
    EXPECT_GE(*id, 1);
}

TEST(Catalog, CreateTableSequentialIds) {
    Catalog catalog;

    auto id1 = catalog.create_table(default_database_id, make_schema("t1"));
    auto id2 = catalog.create_table(default_database_id, make_schema("t2"));
    auto id3 = catalog.create_table(default_database_id, make_schema("t3"));
    ASSERT_TRUE(id1.has_value());
    ASSERT_TRUE(id2.has_value());
    ASSERT_TRUE(id3.has_value());

    EXPECT_EQ(*id2, *id1 + 1);
    EXPECT_EQ(*id3, *id2 + 1);
}

TEST(Catalog, CreateTableDuplicateNameFails) {
    Catalog catalog;

    auto id1 = catalog.create_table(default_database_id, make_schema("users"));
    ASSERT_TRUE(id1.has_value());

    auto id2 = catalog.create_table(default_database_id, make_schema("users"));
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

    auto id = catalog.create_table(default_database_id, schema);
    ASSERT_TRUE(id.has_value());

    auto retrieved = catalog.get_table(default_database_id, "test");
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->columns[0].ordinal, 0);
    EXPECT_EQ(retrieved->columns[1].ordinal, 1);
}

// -- Get table ----------------------------------------------------------------

TEST(Catalog, GetTableByName) {
    Catalog catalog;

    auto id = catalog.create_table(default_database_id, make_schema("users"));
    ASSERT_TRUE(id.has_value());

    auto schema = catalog.get_table(default_database_id, "users");
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

    auto id = catalog.create_table(default_database_id, make_schema("users"));
    ASSERT_TRUE(id.has_value());

    auto schema = catalog.get_table_by_id(*id);
    ASSERT_TRUE(schema.has_value());
    EXPECT_EQ(schema->name, "users");
}

TEST(Catalog, GetTableNotFoundByName) {
    Catalog catalog;
    auto result = catalog.get_table(default_database_id, "nonexistent");
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
    auto tables = catalog.list_tables(default_database_id);
    EXPECT_TRUE(tables.empty());
}

TEST(Catalog, ListTablesMultiple) {
    Catalog catalog;

    ASSERT_TRUE(catalog.create_table(default_database_id, make_schema("alpha")).has_value());
    ASSERT_TRUE(catalog.create_table(default_database_id, make_schema("beta")).has_value());
    ASSERT_TRUE(catalog.create_table(default_database_id, make_schema("gamma")).has_value());

    auto tables = catalog.list_tables(default_database_id);
    EXPECT_EQ(tables.size(), 3u);

    // Should be sorted by table_id.
    EXPECT_EQ(tables[0].name, "alpha");
    EXPECT_EQ(tables[1].name, "beta");
    EXPECT_EQ(tables[2].name, "gamma");
}

// -- Drop table ---------------------------------------------------------------

TEST(Catalog, DropTable) {
    Catalog catalog;

    auto id = catalog.create_table(default_database_id, make_schema("users"));
    ASSERT_TRUE(id.has_value());

    auto drop = catalog.drop_table(default_database_id, "users");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    auto get = catalog.get_table(default_database_id, "users");
    EXPECT_FALSE(get.has_value());
    EXPECT_EQ(get.error().code, StatusCode::NOT_FOUND);
}

TEST(Catalog, DropTableNotFound) {
    Catalog catalog;
    auto drop = catalog.drop_table(default_database_id, "nonexistent");
    EXPECT_FALSE(drop.has_value());
    EXPECT_EQ(drop.error().code, StatusCode::NOT_FOUND);
}

TEST(Catalog, DropTableRemovesFromList) {
    Catalog catalog;

    ASSERT_TRUE(catalog.create_table(default_database_id, make_schema("t1")).has_value());
    ASSERT_TRUE(catalog.create_table(default_database_id, make_schema("t2")).has_value());
    ASSERT_TRUE(catalog.create_table(default_database_id, make_schema("t3")).has_value());

    ASSERT_TRUE(catalog.drop_table(default_database_id, "t2").has_value());

    auto tables = catalog.list_tables(default_database_id);
    EXPECT_EQ(tables.size(), 2u);
    EXPECT_EQ(tables[0].name, "t1");
    EXPECT_EQ(tables[1].name, "t3");
}

TEST(Catalog, DropTableAllowsRecreateWithSameName) {
    Catalog catalog;

    auto id1 = catalog.create_table(default_database_id, make_schema("users"));
    ASSERT_TRUE(id1.has_value());

    ASSERT_TRUE(catalog.drop_table(default_database_id, "users").has_value());

    auto id2 = catalog.create_table(default_database_id, make_schema("users"));
    ASSERT_TRUE(id2.has_value());
    EXPECT_NE(*id1, *id2); // New ID should be different.
}

// -- Create index -------------------------------------------------------------

TEST(Catalog, CreateIndex) {
    Catalog catalog;

    auto tid = catalog.create_table(default_database_id, make_schema("users"));
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

    auto tid = catalog.create_table(default_database_id, make_schema("users"));
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

    auto tid = catalog.create_table(default_database_id, make_schema("users"));
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

    auto t1 = catalog.create_table(default_database_id, make_schema("t1"));
    auto t2 = catalog.create_table(default_database_id, make_schema("t2"));
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

    auto tid = catalog.create_table(default_database_id, make_schema("users"));
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

    auto tid = catalog.create_table(default_database_id, make_schema("users"));
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

    auto tid = catalog.create_table(default_database_id, make_schema("users"));
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

    ASSERT_TRUE(catalog.drop_table(default_database_id, "users").has_value());

    // Both indexes should be gone.
    EXPECT_FALSE(catalog.get_index("idx_a").has_value());
    EXPECT_FALSE(catalog.get_index("idx_b").has_value());
    EXPECT_TRUE(catalog.list_all_indexes().empty());
}

// -- Cache invalidation (DDL consistency) -------------------------------------

TEST(Catalog, CacheInvalidationOnDrop) {
    Catalog catalog;

    auto id = catalog.create_table(default_database_id, make_schema("users"));
    ASSERT_TRUE(id.has_value());

    // Cache hit.
    auto schema1 = catalog.get_table(default_database_id, "users");
    ASSERT_TRUE(schema1.has_value());

    // Drop invalidates.
    ASSERT_TRUE(catalog.drop_table(default_database_id, "users").has_value());

    // Cache miss.
    auto schema2 = catalog.get_table(default_database_id, "users");
    EXPECT_FALSE(schema2.has_value());
}

TEST(Catalog, CacheInvalidationOnRecreate) {
    Catalog catalog;

    auto id1 = catalog.create_table(default_database_id, make_schema("users"));
    ASSERT_TRUE(id1.has_value());

    ASSERT_TRUE(catalog.drop_table(default_database_id, "users").has_value());

    // Recreate with different columns.
    TableSchema new_schema;
    new_schema.name = "users";
    new_schema.columns = {{0, "user_id", TypeId::INT64, false, ""}};

    auto id2 = catalog.create_table(default_database_id, new_schema);
    ASSERT_TRUE(id2.has_value());

    auto retrieved = catalog.get_table(default_database_id, "users");
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->columns.size(), 1u);
    EXPECT_EQ(retrieved->columns[0].name, "user_id");
    EXPECT_EQ(retrieved->columns[0].type_id, TypeId::INT64);
}

// -- Edge type operations -----------------------------------------------------

TEST(Catalog, CreateEdgeType) {
    Catalog catalog;

    auto t1 = catalog.create_table(default_database_id, make_schema("users"));
    auto t2 = catalog.create_table(default_database_id, make_schema("posts"));
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

    auto t1 = catalog.create_table(default_database_id, make_schema("a"));
    auto t2 = catalog.create_table(default_database_id, make_schema("b"));
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

    auto t1 = catalog.create_table(default_database_id, make_schema("a"));
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

    auto t1 = catalog.create_table(default_database_id, make_schema("a"));
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

    auto t1 = catalog.create_table(default_database_id, make_schema("users"));
    auto t2 = catalog.create_table(default_database_id, make_schema("posts"));
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

    auto t1 = catalog.create_table(default_database_id, make_schema("a"));
    auto t2 = catalog.create_table(default_database_id, make_schema("b"));
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

    auto t1 = catalog.create_table(default_database_id, make_schema("a"));
    auto t2 = catalog.create_table(default_database_id, make_schema("b"));
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

    auto t1 = catalog.create_table(default_database_id, make_schema("users"));
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

    auto tid = catalog.create_table(default_database_id, make_schema("documents"));
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

    auto tid = catalog.create_table(default_database_id, make_schema("documents"));
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

    auto t1 = catalog.create_table(default_database_id, make_schema("t1"));
    auto t2 = catalog.create_table(default_database_id, make_schema("t2"));
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

    auto tid = catalog.create_table(default_database_id, make_schema("t1"));
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

    auto src_id = catalog.create_table(default_database_id, make_schema("src"));
    auto tgt_id = catalog.create_table(default_database_id, make_schema("tgt"));
    ASSERT_TRUE(src_id.has_value());
    ASSERT_TRUE(tgt_id.has_value());

    EdgeTypeDef edge;
    edge.name = "follows";
    edge.source_table_id = *src_id;
    edge.target_table_id = *tgt_id;
    auto eid = catalog.create_edge_type(edge);
    ASSERT_TRUE(eid.has_value());

    // Drop the source table — edge type should be removed.
    auto drop = catalog.drop_table(default_database_id, "src");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    auto edges = catalog.list_edge_types();
    EXPECT_TRUE(edges.empty());

    auto get_edge = catalog.get_edge_type("follows");
    EXPECT_FALSE(get_edge.has_value());
    EXPECT_EQ(get_edge.error().code, StatusCode::NOT_FOUND);
}

TEST(Catalog, DropTableCascadesToEdgeTypesAsTarget) {
    Catalog catalog;

    auto src_id = catalog.create_table(default_database_id, make_schema("src"));
    auto tgt_id = catalog.create_table(default_database_id, make_schema("tgt"));
    ASSERT_TRUE(src_id.has_value());
    ASSERT_TRUE(tgt_id.has_value());

    EdgeTypeDef edge;
    edge.name = "likes";
    edge.source_table_id = *src_id;
    edge.target_table_id = *tgt_id;
    auto eid = catalog.create_edge_type(edge);
    ASSERT_TRUE(eid.has_value());

    // Drop the target table — edge type should be removed.
    auto drop = catalog.drop_table(default_database_id, "tgt");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    auto edges = catalog.list_edge_types();
    EXPECT_TRUE(edges.empty());
}

// -- drop_table cascade to embedding columns ----------------------------------

TEST(Catalog, DropTableCascadesToEmbeddingColumns) {
    Catalog catalog;

    auto tid = catalog.create_table(default_database_id, make_schema("docs"));
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
    auto drop = catalog.drop_table(default_database_id, "docs");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    auto after = catalog.list_all_embedding_columns();
    EXPECT_TRUE(after.empty());
}

// == Database operations ======================================================

// -- Default database ---------------------------------------------------------

TEST(Catalog, DefaultDatabaseExistsOnInit) {
    Catalog catalog;

    auto db = catalog.get_database("giodb");
    ASSERT_TRUE(db.has_value()) << db.error().message;
    EXPECT_EQ(db->database_id, default_database_id);
    EXPECT_EQ(db->name, "giodb");
}

TEST(Catalog, ListDatabasesIncludesDefault) {
    Catalog catalog;

    auto dbs = catalog.list_databases();
    ASSERT_EQ(dbs.size(), 1u);
    EXPECT_EQ(dbs[0].database_id, default_database_id);
    EXPECT_EQ(dbs[0].name, "giodb");
}

// -- Create database ----------------------------------------------------------

TEST(Catalog, CreateDatabaseAssignsId) {
    Catalog catalog;

    auto id = catalog.create_database("test_db");
    ASSERT_TRUE(id.has_value()) << id.error().message;
    EXPECT_GT(*id, default_database_id);
}

TEST(Catalog, CreateDatabaseSequentialIds) {
    Catalog catalog;

    auto id1 = catalog.create_database("db1");
    auto id2 = catalog.create_database("db2");
    ASSERT_TRUE(id1.has_value());
    ASSERT_TRUE(id2.has_value());
    EXPECT_EQ(*id2, *id1 + 1);
}

TEST(Catalog, CreateDatabaseDuplicateNameFails) {
    Catalog catalog;

    auto id1 = catalog.create_database("mydb");
    ASSERT_TRUE(id1.has_value());

    auto id2 = catalog.create_database("mydb");
    EXPECT_FALSE(id2.has_value());
    EXPECT_EQ(id2.error().code, StatusCode::ALREADY_EXISTS);
}

TEST(Catalog, CreateDatabaseNameConflictsWithDefault) {
    Catalog catalog;

    auto id = catalog.create_database("giodb");
    EXPECT_FALSE(id.has_value());
    EXPECT_EQ(id.error().code, StatusCode::ALREADY_EXISTS);
}

// -- Get database -------------------------------------------------------------

TEST(Catalog, GetDatabaseByName) {
    Catalog catalog;

    auto id = catalog.create_database("analytics");
    ASSERT_TRUE(id.has_value());

    auto db = catalog.get_database("analytics");
    ASSERT_TRUE(db.has_value()) << db.error().message;
    EXPECT_EQ(db->database_id, *id);
    EXPECT_EQ(db->name, "analytics");
}

TEST(Catalog, GetDatabaseNotFound) {
    Catalog catalog;

    auto db = catalog.get_database("nonexistent");
    EXPECT_FALSE(db.has_value());
    EXPECT_EQ(db.error().code, StatusCode::NOT_FOUND);
}

// -- List databases -----------------------------------------------------------

TEST(Catalog, ListDatabasesSortedById) {
    Catalog catalog;

    ASSERT_TRUE(catalog.create_database("zeta").has_value());
    ASSERT_TRUE(catalog.create_database("alpha").has_value());

    auto dbs = catalog.list_databases();
    ASSERT_EQ(dbs.size(), 3u); // giodb + zeta + alpha
    EXPECT_EQ(dbs[0].name, "giodb");
    EXPECT_LT(dbs[0].database_id, dbs[1].database_id);
    EXPECT_LT(dbs[1].database_id, dbs[2].database_id);
}

// -- Drop database ------------------------------------------------------------

TEST(Catalog, DropEmptyDatabase) {
    Catalog catalog;

    auto id = catalog.create_database("temp_db");
    ASSERT_TRUE(id.has_value());

    auto drop = catalog.drop_database(*id, false);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    auto get = catalog.get_database("temp_db");
    EXPECT_FALSE(get.has_value());
    EXPECT_EQ(get.error().code, StatusCode::NOT_FOUND);
}

TEST(Catalog, DropDatabaseNotFound) {
    Catalog catalog;

    auto drop = catalog.drop_database(999, false);
    EXPECT_FALSE(drop.has_value());
    EXPECT_EQ(drop.error().code, StatusCode::NOT_FOUND);
}

TEST(Catalog, CannotDropDefaultDatabase) {
    Catalog catalog;

    auto drop = catalog.drop_database(default_database_id, false);
    EXPECT_FALSE(drop.has_value());
    EXPECT_EQ(drop.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST(Catalog, CannotDropDefaultDatabaseWithCascade) {
    Catalog catalog;

    auto drop = catalog.drop_database(default_database_id, true);
    EXPECT_FALSE(drop.has_value());
    EXPECT_EQ(drop.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST(Catalog, DropDatabaseWithTablesFailsWithoutCascade) {
    Catalog catalog;

    auto db_id = catalog.create_database("has_tables");
    ASSERT_TRUE(db_id.has_value());

    ASSERT_TRUE(catalog.create_table(*db_id, make_schema("t1")).has_value());

    auto drop = catalog.drop_database(*db_id, false);
    EXPECT_FALSE(drop.has_value());
    EXPECT_EQ(drop.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST(Catalog, DropDatabaseCascadeRemovesTables) {
    Catalog catalog;

    auto db_id = catalog.create_database("cascade_db");
    ASSERT_TRUE(db_id.has_value());

    auto t1 = catalog.create_table(*db_id, make_schema("t1"));
    auto t2 = catalog.create_table(*db_id, make_schema("t2"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    auto drop = catalog.drop_database(*db_id, true);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // Database is gone.
    EXPECT_FALSE(catalog.get_database("cascade_db").has_value());

    // Tables are gone (by ID lookup).
    EXPECT_FALSE(catalog.get_table_by_id(*t1).has_value());
    EXPECT_FALSE(catalog.get_table_by_id(*t2).has_value());
}

TEST(Catalog, DropDatabaseCascadeRemovesIndexes) {
    Catalog catalog;

    auto db_id = catalog.create_database("idx_db");
    ASSERT_TRUE(db_id.has_value());

    auto tid = catalog.create_table(*db_id, make_schema("t1"));
    ASSERT_TRUE(tid.has_value());

    IndexDef idx;
    idx.table_id = *tid;
    idx.name = "idx_in_dropped_db";
    idx.index_type = "btree";
    idx.columns = "id";
    ASSERT_TRUE(catalog.create_index(idx).has_value());

    auto drop = catalog.drop_database(*db_id, true);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    EXPECT_FALSE(catalog.get_index("idx_in_dropped_db").has_value());
}

TEST(Catalog, DropDatabaseRemovesFromList) {
    Catalog catalog;

    auto id = catalog.create_database("to_remove");
    ASSERT_TRUE(id.has_value());

    ASSERT_TRUE(catalog.drop_database(*id, false).has_value());

    auto dbs = catalog.list_databases();
    ASSERT_EQ(dbs.size(), 1u); // Only giodb remains.
    EXPECT_EQ(dbs[0].name, "giodb");
}

// -- Table scoping per database -----------------------------------------------

TEST(Catalog, SameTableNameInDifferentDatabases) {
    Catalog catalog;

    auto db1 = catalog.create_database("db1");
    auto db2 = catalog.create_database("db2");
    ASSERT_TRUE(db1.has_value());
    ASSERT_TRUE(db2.has_value());

    auto t1 = catalog.create_table(*db1, make_schema("users"));
    auto t2 = catalog.create_table(*db2, make_schema("users"));
    ASSERT_TRUE(t1.has_value()) << t1.error().message;
    ASSERT_TRUE(t2.has_value()) << t2.error().message;

    // Different table IDs.
    EXPECT_NE(*t1, *t2);

    // Each database sees its own table.
    auto s1 = catalog.get_table(*db1, "users");
    auto s2 = catalog.get_table(*db2, "users");
    ASSERT_TRUE(s1.has_value());
    ASSERT_TRUE(s2.has_value());
    EXPECT_EQ(s1->table_id, *t1);
    EXPECT_EQ(s2->table_id, *t2);
}

TEST(Catalog, ListTablesOnlyShowsDatabaseTables) {
    Catalog catalog;

    auto db1 = catalog.create_database("db1");
    auto db2 = catalog.create_database("db2");
    ASSERT_TRUE(db1.has_value());
    ASSERT_TRUE(db2.has_value());

    ASSERT_TRUE(catalog.create_table(*db1, make_schema("a")).has_value());
    ASSERT_TRUE(catalog.create_table(*db1, make_schema("b")).has_value());
    ASSERT_TRUE(catalog.create_table(*db2, make_schema("c")).has_value());

    auto db1_tables = catalog.list_tables(*db1);
    EXPECT_EQ(db1_tables.size(), 2u);

    auto db2_tables = catalog.list_tables(*db2);
    EXPECT_EQ(db2_tables.size(), 1u);
    EXPECT_EQ(db2_tables[0].name, "c");
}

TEST(Catalog, DropTableInOneDatabaseDoesNotAffectOther) {
    Catalog catalog;

    auto db1 = catalog.create_database("db1");
    auto db2 = catalog.create_database("db2");
    ASSERT_TRUE(db1.has_value());
    ASSERT_TRUE(db2.has_value());

    ASSERT_TRUE(catalog.create_table(*db1, make_schema("users")).has_value());
    ASSERT_TRUE(catalog.create_table(*db2, make_schema("users")).has_value());

    // Drop from db1.
    ASSERT_TRUE(catalog.drop_table(*db1, "users").has_value());

    // db2 still has it.
    auto s = catalog.get_table(*db2, "users");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->name, "users");
}

TEST(Catalog, CreateTableInNonexistentDatabaseFails) {
    Catalog catalog;

    auto result = catalog.create_table(999, make_schema("orphan"));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// -- sys_databases schema -----------------------------------------------------

TEST(Catalog, SysDatabasesSchemaDefinition) {
    auto schema = sys_databases_schema();
    EXPECT_EQ(schema.name, "sys_databases");
    ASSERT_EQ(schema.columns.size(), 2u);
    EXPECT_EQ(schema.columns[0].name, "database_id");
    EXPECT_EQ(schema.columns[0].type_id, TypeId::INT32);
    EXPECT_FALSE(schema.columns[0].nullable);
    EXPECT_EQ(schema.columns[1].name, "name");
    EXPECT_EQ(schema.columns[1].type_id, TypeId::STRING);
    EXPECT_FALSE(schema.columns[1].nullable);
    EXPECT_EQ(schema.pk_columns, "database_id");
}
