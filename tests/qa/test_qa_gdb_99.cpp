/// QA adversarial tests for GDB-99: sys_edge_types and sys_embedding_columns.
/// Tests edge type CRUD, embedding column registration, cascade deletes, validation.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace sixseven;

// =============================================================================
// Helpers
// =============================================================================
static TableSchema make_table(const std::string& name) {
    TableSchema s;
    s.table_id = 0;
    s.name = name;
    s.columns = {{0, "id", TypeId::INT32, false, ""}};
    return s;
}

// =============================================================================
// Edge Type Creation
// =============================================================================

TEST(QA_GDB99_Edge, CreateEdgeType) {
    Catalog catalog;
    auto src = catalog.create_table(default_database_id, make_table("person"));
    auto tgt = catalog.create_table(default_database_id, make_table("company"));
    ASSERT_TRUE(src.has_value());
    ASSERT_TRUE(tgt.has_value());

    EdgeTypeDef def;
    def.edge_id = 0;
    def.name = "works_at";
    def.source_table_id = *src;
    def.target_table_id = *tgt;
    def.properties = "since INT32, role STRING";

    auto r = catalog.create_edge_type(default_database_id, def);
    ASSERT_TRUE(r.has_value());

    auto e = catalog.get_edge_type(default_database_id, "works_at");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->source_table_id, *src);
    EXPECT_EQ(e->target_table_id, *tgt);
    EXPECT_EQ(e->properties, "since INT32, role STRING");
}

TEST(QA_GDB99_Edge, SelfReferenceEdge) {
    Catalog catalog;
    auto t = catalog.create_table(default_database_id, make_table("person"));
    ASSERT_TRUE(t.has_value());

    EdgeTypeDef def{0, 0, "follows", *t, *t, ""};
    auto r = catalog.create_edge_type(default_database_id, def);
    ASSERT_TRUE(r.has_value());

    auto e = catalog.get_edge_type(default_database_id, "follows");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->source_table_id, e->target_table_id);
}

TEST(QA_GDB99_Edge, DuplicateEdgeTypeName) {
    Catalog catalog;
    auto t = catalog.create_table(default_database_id, make_table("t"));
    ASSERT_TRUE(t.has_value());

    EdgeTypeDef d1{0, 0, "relates", *t, *t, ""};
    EdgeTypeDef d2{0, 0, "relates", *t, *t, ""};

    auto r1 = catalog.create_edge_type(default_database_id, d1);
    ASSERT_TRUE(r1.has_value());

    auto r2 = catalog.create_edge_type(default_database_id, d2);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, StatusCode::ALREADY_EXISTS);
}

TEST(QA_GDB99_Edge, MissingSourceTable) {
    Catalog catalog;
    auto t = catalog.create_table(default_database_id, make_table("t"));
    ASSERT_TRUE(t.has_value());

    EdgeTypeDef def{0, 0, "bad_edge", 9999, *t, ""};
    auto r = catalog.create_edge_type(default_database_id, def);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_GDB99_Edge, MissingTargetTable) {
    Catalog catalog;
    auto t = catalog.create_table(default_database_id, make_table("t"));
    ASSERT_TRUE(t.has_value());

    EdgeTypeDef def{0, 0, "bad_edge", *t, 9999, ""};
    auto r = catalog.create_edge_type(default_database_id, def);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
}

// =============================================================================
// Edge Type Drop
// =============================================================================

TEST(QA_GDB99_Edge, DropEdgeType) {
    Catalog catalog;
    auto t = catalog.create_table(default_database_id, make_table("t"));
    ASSERT_TRUE(t.has_value());

    (void)catalog.create_edge_type(default_database_id, {0, 0, "e", *t, *t, ""});

    auto dr = catalog.drop_edge_type(default_database_id, "e");
    ASSERT_TRUE(dr.has_value());

    auto e = catalog.get_edge_type(default_database_id, "e");
    ASSERT_FALSE(e.has_value());
}

TEST(QA_GDB99_Edge, DropNonexistentEdgeType) {
    Catalog catalog;
    auto r = catalog.drop_edge_type(default_database_id, "nonexistent");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_GDB99_Edge, CascadeDropSourceTable) {
    Catalog catalog;
    auto src = catalog.create_table(default_database_id, make_table("src"));
    auto tgt = catalog.create_table(default_database_id, make_table("tgt"));
    ASSERT_TRUE(src.has_value());
    ASSERT_TRUE(tgt.has_value());

    (void)catalog.create_edge_type(default_database_id, {0, 0, "link", *src, *tgt, ""});

    // Drop source table
    auto dr = catalog.drop_table(default_database_id, "src");
    ASSERT_TRUE(dr.has_value());

    // Edge type should be cascaded away
    auto e = catalog.get_edge_type(default_database_id, "link");
    ASSERT_FALSE(e.has_value());
}

TEST(QA_GDB99_Edge, CascadeDropTargetTable) {
    Catalog catalog;
    auto src = catalog.create_table(default_database_id, make_table("src"));
    auto tgt = catalog.create_table(default_database_id, make_table("tgt"));
    ASSERT_TRUE(src.has_value());
    ASSERT_TRUE(tgt.has_value());

    (void)catalog.create_edge_type(default_database_id, {0, 0, "link", *src, *tgt, ""});

    // Drop target table
    auto dr = catalog.drop_table(default_database_id, "tgt");
    ASSERT_TRUE(dr.has_value());

    auto e = catalog.get_edge_type(default_database_id, "link");
    ASSERT_FALSE(e.has_value());
}

TEST(QA_GDB99_Edge, CascadeSelfRefOnDrop) {
    Catalog catalog;
    auto t = catalog.create_table(default_database_id, make_table("t"));
    ASSERT_TRUE(t.has_value());

    (void)catalog.create_edge_type(default_database_id, {0, 0, "self_edge", *t, *t, ""});

    auto dr = catalog.drop_table(default_database_id, "t");
    ASSERT_TRUE(dr.has_value());

    auto e = catalog.get_edge_type(default_database_id, "self_edge");
    ASSERT_FALSE(e.has_value());
}

// =============================================================================
// Edge Type List
// =============================================================================

TEST(QA_GDB99_Edge, ListEdgeTypes) {
    Catalog catalog;
    auto t1 = catalog.create_table(default_database_id, make_table("t1"));
    auto t2 = catalog.create_table(default_database_id, make_table("t2"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    (void)catalog.create_edge_type(default_database_id, {0, 0, "e1", *t1, *t2, ""});
    (void)catalog.create_edge_type(default_database_id, {0, 0, "e2", *t2, *t1, ""});
    (void)catalog.create_edge_type(default_database_id, {0, 0, "e3", *t1, *t1, ""});

    auto edges = catalog.list_edge_types(default_database_id);
    EXPECT_EQ(edges.size(), 3u);

    // Should be sorted by edge_id
    for (size_t i = 1; i < edges.size(); ++i) {
        EXPECT_LT(edges[i - 1].edge_id, edges[i].edge_id);
    }
}

TEST(QA_GDB99_Edge, ListEdgeTypesEmpty) {
    Catalog catalog;
    auto edges = catalog.list_edge_types(default_database_id);
    EXPECT_TRUE(edges.empty());
}

// =============================================================================
// Edge Type Schemas
// =============================================================================

TEST(QA_GDB99_SysSchema, SysEdgeTypesSchema) {
    auto schema = sys_edge_types_schema();
    EXPECT_EQ(schema.name, "sys_edge_types");

    bool has_edge_id = false, has_name = false;
    bool has_src = false, has_tgt = false;
    for (const auto& col : schema.columns) {
        if (col.name == "edge_id")
            has_edge_id = true;
        if (col.name == "name")
            has_name = true;
        if (col.name == "source_table_id")
            has_src = true;
        if (col.name == "target_table_id")
            has_tgt = true;
    }
    EXPECT_TRUE(has_edge_id);
    EXPECT_TRUE(has_name);
    EXPECT_TRUE(has_src);
    EXPECT_TRUE(has_tgt);
}

TEST(QA_GDB99_SysSchema, SysEmbeddingColumnsSchema) {
    auto schema = sys_embedding_columns_schema();
    EXPECT_EQ(schema.name, "sys_embedding_columns");

    bool has_table_id = false, has_column_id = false;
    bool has_dim = false, has_provider = false;
    for (const auto& col : schema.columns) {
        if (col.name == "table_id")
            has_table_id = true;
        if (col.name == "column_id")
            has_column_id = true;
        if (col.name == "dimension")
            has_dim = true;
        if (col.name == "provider")
            has_provider = true;
    }
    EXPECT_TRUE(has_table_id);
    EXPECT_TRUE(has_column_id);
    EXPECT_TRUE(has_dim);
    EXPECT_TRUE(has_provider);
}

// =============================================================================
// Embedding Column Registration
// =============================================================================

TEST(QA_GDB99_Embedding, RegisterColumn) {
    Catalog catalog;
    auto t = catalog.create_table(default_database_id, make_table("docs"));
    ASSERT_TRUE(t.has_value());

    EmbeddingColumnDef def;
    def.table_id = *t;
    def.column_id = 1;
    def.dimension = 384;
    def.source_expr = "title || ' ' || body";
    def.provider = "ollama/all-minilm";

    auto r = catalog.register_embedding_column(def);
    ASSERT_TRUE(r.has_value());

    auto cols = catalog.list_embedding_columns(*t);
    ASSERT_EQ(cols.size(), 1u);
    EXPECT_EQ(cols[0].dimension, 384);
    EXPECT_EQ(cols[0].provider, "ollama/all-minilm");
    EXPECT_EQ(cols[0].source_expr, "title || ' ' || body");
}

TEST(QA_GDB99_Embedding, RegisterDuplicate) {
    Catalog catalog;
    auto t = catalog.create_table(default_database_id, make_table("docs"));
    ASSERT_TRUE(t.has_value());

    EmbeddingColumnDef def{*t, 1, 384, "text", "provider"};

    auto r1 = catalog.register_embedding_column(def);
    ASSERT_TRUE(r1.has_value());

    auto r2 = catalog.register_embedding_column(def);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, StatusCode::ALREADY_EXISTS);
}

TEST(QA_GDB99_Embedding, RegisterOnNonexistentTable) {
    Catalog catalog;
    EmbeddingColumnDef def{9999, 0, 128, "text", "provider"};
    auto r = catalog.register_embedding_column(def);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_GDB99_Embedding, CascadeDropTable) {
    Catalog catalog;
    auto t = catalog.create_table(default_database_id, make_table("docs"));
    ASSERT_TRUE(t.has_value());

    (void)catalog.register_embedding_column({*t, 0, 384, "text", "p1"});
    (void)catalog.register_embedding_column({*t, 1, 768, "title", "p2"});

    auto dr = catalog.drop_table(default_database_id, "docs");
    ASSERT_TRUE(dr.has_value());

    auto cols = catalog.list_embedding_columns(*t);
    EXPECT_TRUE(cols.empty());

    auto all = catalog.list_all_embedding_columns();
    EXPECT_TRUE(all.empty());
}

TEST(QA_GDB99_Embedding, MultipleColumnsOneTable) {
    Catalog catalog;
    auto t = catalog.create_table(default_database_id, make_table("docs"));
    ASSERT_TRUE(t.has_value());

    (void)catalog.register_embedding_column({*t, 0, 384, "text", "p1"});
    (void)catalog.register_embedding_column({*t, 1, 768, "title", "p2"});
    (void)catalog.register_embedding_column({*t, 2, 128, "tags", "p3"});

    auto cols = catalog.list_embedding_columns(*t);
    EXPECT_EQ(cols.size(), 3u);
}

TEST(QA_GDB99_Embedding, ListAllAcrossTables) {
    Catalog catalog;
    auto t1 = catalog.create_table(default_database_id, make_table("t1"));
    auto t2 = catalog.create_table(default_database_id, make_table("t2"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    (void)catalog.register_embedding_column({*t1, 0, 384, "text", "p"});
    (void)catalog.register_embedding_column({*t2, 0, 768, "text", "p"});

    auto all = catalog.list_all_embedding_columns();
    EXPECT_EQ(all.size(), 2u);
}

// =============================================================================
// Embedding Provider Operations
// =============================================================================

TEST(QA_GDB99_Provider, RegisterAndGet) {
    Catalog catalog;

    EmbeddingProviderConfig cfg;
    cfg.name = "ollama/all-minilm";
    cfg.type = "ollama";
    cfg.base_url = "http://localhost:11434";
    cfg.model = "all-minilm";
    cfg.api_key = "";
    cfg.dimension = 384;

    auto r = catalog.register_embedding_provider(cfg);
    ASSERT_TRUE(r.has_value());

    auto p = catalog.get_embedding_provider("ollama/all-minilm");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->type, "ollama");
    EXPECT_EQ(p->dimension, 384);
}

TEST(QA_GDB99_Provider, DuplicateProvider) {
    Catalog catalog;
    EmbeddingProviderConfig cfg{"p1", "ollama", "", "", "", 384};

    auto r1 = catalog.register_embedding_provider(cfg);
    ASSERT_TRUE(r1.has_value());

    auto r2 = catalog.register_embedding_provider(cfg);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, StatusCode::ALREADY_EXISTS);
}

TEST(QA_GDB99_Provider, RemoveProvider) {
    Catalog catalog;
    (void)catalog.register_embedding_provider({"p1", "ollama", "", "", "", 384});

    auto dr = catalog.remove_embedding_provider("p1");
    ASSERT_TRUE(dr.has_value());

    auto p = catalog.get_embedding_provider("p1");
    ASSERT_FALSE(p.has_value());
}

TEST(QA_GDB99_Provider, RemoveNonexistent) {
    Catalog catalog;
    auto r = catalog.remove_embedding_provider("nonexistent");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_GDB99_Provider, ListProviders) {
    Catalog catalog;
    (void)catalog.register_embedding_provider({"z_provider", "ollama", "", "", "", 384});
    (void)catalog.register_embedding_provider({"a_provider", "openai", "", "", "", 768});
    (void)catalog.register_embedding_provider({"m_provider", "onnx", "", "", "", 128});

    auto providers = catalog.list_embedding_providers();
    EXPECT_EQ(providers.size(), 3u);

    // Should be sorted by name
    for (size_t i = 1; i < providers.size(); ++i) {
        EXPECT_LT(providers[i - 1].name, providers[i].name);
    }
}

// =============================================================================
// Stress
// =============================================================================

TEST(QA_GDB99_Stress, ManyEdgeTypesAndEmbeddings) {
    Catalog catalog;

    // Create 10 tables
    std::vector<table_id_t> table_ids;
    for (int i = 0; i < 10; ++i) {
        auto t = catalog.create_table(default_database_id, make_table("t" + std::to_string(i)));
        ASSERT_TRUE(t.has_value());
        table_ids.push_back(*t);
    }

    // Create edge types between all adjacent pairs
    for (int i = 0; i < 9; ++i) {
        EdgeTypeDef def{0, 0, "e_" + std::to_string(i), table_ids[i], table_ids[i + 1], ""};
        auto r = catalog.create_edge_type(default_database_id, def);
        ASSERT_TRUE(r.has_value());
    }

    EXPECT_EQ(catalog.list_edge_types(default_database_id).size(), 9u);

    // Register embedding column on each table
    for (int i = 0; i < 10; ++i) {
        auto r = catalog.register_embedding_column({table_ids[i], 0, 384, "text", "provider"});
        ASSERT_TRUE(r.has_value());
    }

    EXPECT_EQ(catalog.list_all_embedding_columns().size(), 10u);

    // Drop middle table — should cascade its edge types and embedding
    auto dr = catalog.drop_table(default_database_id, "t5");
    ASSERT_TRUE(dr.has_value());

    // Edge types referencing t5 should be gone (e_4 links t4→t5, e_5 links t5→t6)
    EXPECT_FALSE(catalog.get_edge_type(default_database_id, "e_4").has_value());
    EXPECT_FALSE(catalog.get_edge_type(default_database_id, "e_5").has_value());

    // Others should remain
    EXPECT_TRUE(catalog.get_edge_type(default_database_id, "e_0").has_value());
    EXPECT_TRUE(catalog.get_edge_type(default_database_id, "e_8").has_value());

    auto remaining_edges = catalog.list_edge_types(default_database_id);
    EXPECT_EQ(remaining_edges.size(), 7u);

    auto remaining_emb = catalog.list_all_embedding_columns();
    EXPECT_EQ(remaining_emb.size(), 9u);
}
