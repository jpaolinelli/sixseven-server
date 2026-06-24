// tests/qa/test_qa_gdb_925.cpp
//
// GDB-925: Downstream safety tests for catalog permissiveness.
//
// The 12 strengthened QA_Catalog tests pin "catalog accepts X without
// validation". These tests verify that accepting such objects does NOT cause
// downstream crashes or corruption when the accepted objects are subsequently
// used (listed, retrieved, dropped). If any of the tests below crash or
// corrupt the catalog, that is a Critical/High finding.

#include "sixseven/catalog/catalog.h"

#include <gtest/gtest.h>

#include <string>

#include "test_qa_helpers.h"

using namespace sixseven;

// ---------------------------------------------------------------------------
// Shared helper: make a minimal two-column table schema.
// ---------------------------------------------------------------------------

static TableSchema make_schema(const std::string& name) {
    TableSchema s;
    s.name = name;
    s.columns = {
        {0, "id", TypeId::INT32, false, ""},
        {1, "val", TypeId::STRING, true, ""},
    };
    s.pk_columns = "id";
    return s;
}

// ---------------------------------------------------------------------------
// Downstream safety: negative-dimension embedding column
//
// The catalog accepts dim=-1. Verify that after acceptance the object can be
// listed and is consistent, and the catalog does not crash on subsequent ops.
// ---------------------------------------------------------------------------

TEST(QA_GDB925, NegativeDimEmbeddingColumnDownstreamSafe) {
    Catalog catalog;
    bootstrap_qa_catalog(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema("docs"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.table_id = *tid;
    def.column_id = 0;
    def.dimension = -1;
    def.source_expr = "val";
    def.provider = "test";

    auto reg = catalog.register_embedding_column(def);
    ASSERT_TRUE(reg.has_value()) << reg.error().message;

    // list_embedding_columns must not crash and must return the registered entry.
    auto embs = catalog.list_embedding_columns(*tid);
    ASSERT_EQ(embs.size(), 1u);
    EXPECT_EQ(embs[0].dimension, -1);
    EXPECT_EQ(embs[0].table_id, *tid);

    // list_all_embedding_columns must not crash.
    auto all = catalog.list_all_embedding_columns();
    EXPECT_FALSE(all.empty());

    // Drop the table that owns the embedding column - must not crash.
    // (system database protection doesn't apply; this is in the default db.)
    auto drop = catalog.drop_table(default_database_id, "docs");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // After drop, embedding columns for that table must be gone.
    auto embs_after = catalog.list_embedding_columns(*tid);
    EXPECT_TRUE(embs_after.empty());
}

// ---------------------------------------------------------------------------
// Downstream safety: zero-dimension embedding column
// ---------------------------------------------------------------------------

TEST(QA_GDB925, ZeroDimEmbeddingColumnDownstreamSafe) {
    Catalog catalog;
    bootstrap_qa_catalog(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema("vecs"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.table_id = *tid;
    def.column_id = 0;
    def.dimension = 0;
    def.source_expr = "val";
    def.provider = "test";

    auto reg = catalog.register_embedding_column(def);
    ASSERT_TRUE(reg.has_value()) << reg.error().message;

    auto embs = catalog.list_embedding_columns(*tid);
    ASSERT_EQ(embs.size(), 1u);
    EXPECT_EQ(embs[0].dimension, 0);

    // Registering a second embedding on the same column with different dim
    // must not corrupt the first entry.
    EmbeddingColumnDef def2;
    def2.table_id = *tid;
    def2.column_id = 1;
    def2.dimension = 128;
    def2.source_expr = "val";
    def2.provider = "test";
    auto reg2 = catalog.register_embedding_column(def2);
    ASSERT_TRUE(reg2.has_value()) << reg2.error().message;

    auto embs2 = catalog.list_embedding_columns(*tid);
    ASSERT_EQ(embs2.size(), 2u);
    // First entry must still have dim=0, not dim=128.
    EXPECT_EQ(embs2[0].dimension, 0);
    EXPECT_EQ(embs2[1].dimension, 128);
}

// ---------------------------------------------------------------------------
// Downstream safety: index on nonexistent column - list and drop must be safe
// ---------------------------------------------------------------------------

TEST(QA_GDB925, IndexOnNonexistentColumnDownstreamSafe) {
    Catalog catalog;
    bootstrap_qa_catalog(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema("users"));
    ASSERT_TRUE(tid.has_value());

    IndexDef def;
    def.table_id = *tid;
    def.name = "idx_bogus_925";
    def.index_type = "btree";
    def.columns = "nonexistent_column";

    auto cr = catalog.create_index(def);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    // get_index must return the accepted definition without crashing.
    auto get = catalog.get_index(default_database_id, "idx_bogus_925");
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ(get->columns, "nonexistent_column");

    // list_indexes must include the bogus index.
    auto idxs = catalog.list_indexes(*tid);
    ASSERT_FALSE(idxs.empty());
    bool found = false;
    for (const auto& idx : idxs) {
        if (idx.name == "idx_bogus_925") {
            found = true;
        }
    }
    EXPECT_TRUE(found);

    // drop_index must succeed for the bogus index.
    auto drop = catalog.drop_index(default_database_id, "idx_bogus_925");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // After drop, index must be gone.
    auto get_after = catalog.get_index(default_database_id, "idx_bogus_925");
    EXPECT_FALSE(get_after.has_value());
}

// ---------------------------------------------------------------------------
// Downstream safety: empty-name objects - list/get/drop must not crash
// ---------------------------------------------------------------------------

TEST(QA_GDB925, EmptyNameTableListGetDropSafe) {
    Catalog catalog;
    bootstrap_qa_catalog(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema(""));
    ASSERT_TRUE(tid.has_value()) << tid.error().message;

    // get_table by empty name must succeed.
    auto get = catalog.get_table(default_database_id, "");
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ(get->name, "");

    // list_tables must include the empty-named table.
    auto tables = catalog.list_tables(default_database_id);
    bool found = false;
    for (const auto& t : tables) {
        if (t.name.empty()) {
            found = true;
        }
    }
    EXPECT_TRUE(found);

    // drop_table by empty name must succeed.
    auto drop = catalog.drop_table(default_database_id, "");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // After drop, get must return NOT_FOUND (not crash).
    auto get_after = catalog.get_table(default_database_id, "");
    EXPECT_FALSE(get_after.has_value());
}

// ---------------------------------------------------------------------------
// Downstream safety: empty-name index - list/get/drop must not crash
// ---------------------------------------------------------------------------

TEST(QA_GDB925, EmptyNameIndexListGetDropSafe) {
    Catalog catalog;
    bootstrap_qa_catalog(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema("t_for_idx"));
    ASSERT_TRUE(tid.has_value());

    IndexDef def;
    def.table_id = *tid;
    def.name = "";
    def.index_type = "btree";
    def.columns = "id";

    auto cr = catalog.create_index(def);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    auto get = catalog.get_index(default_database_id, "");
    ASSERT_TRUE(get.has_value());

    // drop_index by empty name must succeed.
    auto drop = catalog.drop_index(default_database_id, "");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    auto get_after = catalog.get_index(default_database_id, "");
    EXPECT_FALSE(get_after.has_value());
}

// ---------------------------------------------------------------------------
// Downstream safety: out-of-range column_id embedding - catalog remains
// consistent after the bogus registration.
// ---------------------------------------------------------------------------

TEST(QA_GDB925, OutOfRangeColumnIdEmbeddingCatalogConsistency) {
    Catalog catalog;
    bootstrap_qa_catalog(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema("emb_tbl"));
    ASSERT_TRUE(tid.has_value());

    // Register embedding with column_id=999 (table only has cols 0 and 1).
    EmbeddingColumnDef def;
    def.table_id = *tid;
    def.column_id = 999;
    def.dimension = 64;
    def.source_expr = "val";
    def.provider = "test";

    auto reg = catalog.register_embedding_column(def);
    ASSERT_TRUE(reg.has_value()) << reg.error().message;

    // Catalog must still allow a valid embedding registration afterwards.
    EmbeddingColumnDef def2;
    def2.table_id = *tid;
    def2.column_id = 0;
    def2.dimension = 128;
    def2.source_expr = "val";
    def2.provider = "test";

    auto reg2 = catalog.register_embedding_column(def2);
    ASSERT_TRUE(reg2.has_value()) << "Catalog corrupted by bogus column_id: "
                                  << reg2.error().message;

    auto embs = catalog.list_embedding_columns(*tid);
    ASSERT_EQ(embs.size(), 2u);

    // get_table_by_id must still work (catalog state remains intact).
    auto tbl = catalog.get_table_by_id(*tid);
    ASSERT_TRUE(tbl.has_value());
    EXPECT_EQ(tbl->columns.size(), 2u);
}
