// GDB-808: Regression tests for catalog next-id counter advancement during restore.
//
// set_next_index_id and set_next_edge_id were deleted as dead code because
// restore_index and restore_edge_type already advance next_index_id_ and
// next_edge_id_ inline.  These tests assert that the restore path correctly
// bumps the counters so that subsequent create_index / create_edge_type calls
// never reuse a restored ID.

#include "sixseven/catalog/catalog.h"

#include <gtest/gtest.h>

#include "test_catalog_helpers.h"

using namespace sixseven;

// Helper: build a minimal TableSchema.
static TableSchema make_schema(const std::string& name) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {{0, "id", TypeId::INT32, false, ""}};
    schema.pk_columns = "id";
    return schema;
}

// ----------------------------------------------------------------------------
// restore_index advances next_index_id_
// ----------------------------------------------------------------------------

TEST(CatalogGDB808, RestoreIndexAdvancesCounter) {
    Catalog catalog;
    init_test_catalog(catalog);

    // Create a table to attach indexes to.
    auto tbl = catalog.create_table(default_database_id, make_schema("t1"));
    ASSERT_TRUE(tbl.has_value()) << tbl.error().message;
    table_id_t tid = *tbl;

    // Restore an index with a high pre-assigned ID.
    IndexDef def;
    def.index_id = 50;
    def.table_id = tid;
    def.name = "idx_restored";
    def.index_type = "btree";
    def.columns = "id";
    def.is_unique = false;
    auto r = catalog.restore_index(def);
    ASSERT_TRUE(r.has_value()) << r.error().message;

    // The next freshly created index must receive ID > 50 (not 1 or any value
    // that would collide with the restored index).
    IndexDef new_def;
    new_def.table_id = tid;
    new_def.name = "idx_new";
    new_def.index_type = "btree";
    new_def.columns = "id";
    new_def.is_unique = false;
    auto new_id = catalog.create_index(new_def);
    ASSERT_TRUE(new_id.has_value()) << new_id.error().message;
    EXPECT_GT(*new_id, static_cast<index_id_t>(50))
        << "create_index should not reuse an ID <= the restored index_id";
}

TEST(CatalogGDB808, MultipleRestoreIndexAdvancesCounterToMax) {
    Catalog catalog;
    init_test_catalog(catalog);

    auto tbl = catalog.create_table(default_database_id, make_schema("t2"));
    ASSERT_TRUE(tbl.has_value());
    table_id_t tid = *tbl;

    // Restore three indexes out of order.
    for (auto [iid, iname] : std::initializer_list<std::pair<index_id_t, const char*>>{
             {10, "ix_a"}, {30, "ix_b"}, {20, "ix_c"}}) {
        IndexDef d;
        d.index_id = iid;
        d.table_id = tid;
        d.name = iname;
        d.index_type = "btree";
        d.columns = "id";
        auto rv = catalog.restore_index(d);
        ASSERT_TRUE(rv.has_value()) << rv.error().message;
    }

    // Next allocated ID must be > 30 (the max restored).
    IndexDef nd;
    nd.table_id = tid;
    nd.name = "ix_new";
    nd.index_type = "btree";
    nd.columns = "id";
    auto nid = catalog.create_index(nd);
    ASSERT_TRUE(nid.has_value()) << nid.error().message;
    EXPECT_GT(*nid, static_cast<index_id_t>(30));
}

// ----------------------------------------------------------------------------
// restore_edge_type advances next_edge_id_
// ----------------------------------------------------------------------------

TEST(CatalogGDB808, RestoreEdgeTypeAdvancesCounter) {
    Catalog catalog;
    init_test_catalog(catalog);

    auto t1 = catalog.create_table(default_database_id, make_schema("nodes1"));
    auto t2 = catalog.create_table(default_database_id, make_schema("nodes2"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    EdgeTypeDef def;
    def.edge_id = 75;
    def.name = "edge_restored";
    def.source_table_id = *t1;
    def.target_table_id = *t2;
    auto r = catalog.restore_edge_type(default_database_id, def);
    ASSERT_TRUE(r.has_value()) << r.error().message;

    // Next create_edge_type should not reuse any ID <= 75.
    EdgeTypeDef nd;
    nd.name = "edge_new";
    nd.source_table_id = *t1;
    nd.target_table_id = *t2;
    auto nid = catalog.create_edge_type(default_database_id, nd);
    ASSERT_TRUE(nid.has_value()) << nid.error().message;
    EXPECT_GT(*nid, static_cast<edge_id_t>(75));
}

TEST(CatalogGDB808, MultipleRestoreEdgeTypeAdvancesCounterToMax) {
    Catalog catalog;
    init_test_catalog(catalog);

    auto t1 = catalog.create_table(default_database_id, make_schema("src"));
    auto t2 = catalog.create_table(default_database_id, make_schema("dst"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    for (auto [eid, ename] : std::initializer_list<std::pair<edge_id_t, const char*>>{
             {5, "e_a"}, {100, "e_b"}, {42, "e_c"}}) {
        EdgeTypeDef d;
        d.edge_id = eid;
        d.name = ename;
        d.source_table_id = *t1;
        d.target_table_id = *t2;
        auto rv = catalog.restore_edge_type(default_database_id, d);
        ASSERT_TRUE(rv.has_value()) << rv.error().message;
    }

    EdgeTypeDef nd;
    nd.name = "e_new";
    nd.source_table_id = *t1;
    nd.target_table_id = *t2;
    auto nid = catalog.create_edge_type(default_database_id, nd);
    ASSERT_TRUE(nid.has_value()) << nid.error().message;
    EXPECT_GT(*nid, static_cast<edge_id_t>(100));
}
