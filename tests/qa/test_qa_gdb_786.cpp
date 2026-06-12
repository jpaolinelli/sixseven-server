// GDB-786: drop_database must remove orphaned and dangling edge type definitions.
//
// Leak (1): edge type owned by the dropped DB whose source/target tables live in
//           a foreign database — survives the cascade table drop but must not
//           survive drop_database.
// Leak (2): edge type owned by a surviving DB whose source or target table lived
//           in the dropped DB — reference becomes dangling after the drop.

#include "sixseven/catalog/catalog.h"

#include <gtest/gtest.h>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static TableSchema make_schema(const std::string& name) {
    TableSchema s;
    s.name = name;
    s.columns = {{0, "id", TypeId::INT32, false, ""}};
    s.pk_columns = "id";
    return s;
}

static EdgeTypeDef make_edge_def(const std::string& name, table_id_t src, table_id_t tgt) {
    EdgeTypeDef e;
    e.name = name;
    e.source_table_id = src;
    e.target_table_id = tgt;
    return e;
}

// ---------------------------------------------------------------------------
// GDB786: edge in dropped DB referencing only foreign tables is removed
// ---------------------------------------------------------------------------

TEST(GDB786, OwnedEdgeCrossDbReferenceRemovedOnDropDatabase) {
    Catalog cat;

    // db_a will be dropped; db_b is the "foreign" database.
    auto db_a = cat.create_database("db_a");
    ASSERT_TRUE(db_a.has_value()) << db_a.error().message;
    auto db_b = cat.create_database("db_b");
    ASSERT_TRUE(db_b.has_value()) << db_b.error().message;

    // Tables in db_b (foreign).
    auto tb_src = cat.create_table(*db_b, make_schema("nodes_src"));
    ASSERT_TRUE(tb_src.has_value());
    auto tb_tgt = cat.create_table(*db_b, make_schema("nodes_tgt"));
    ASSERT_TRUE(tb_tgt.has_value());

    // Edge type owned by db_a but referencing tables in db_b.
    auto eid = cat.create_edge_type(*db_a, make_edge_def("cross_edge", *tb_src, *tb_tgt));
    ASSERT_TRUE(eid.has_value()) << eid.error().message;

    // Sanity: visible before drop.
    {
        auto all = cat.list_all_edge_types();
        ASSERT_EQ(all.size(), 1u);
        EXPECT_EQ(all[0].name, "cross_edge");
    }

    // Drop db_a (no cascade needed — it has no tables).
    auto drop = cat.drop_database(*db_a, /*cascade=*/false);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // Edge must be gone from the global list.
    EXPECT_TRUE(cat.list_all_edge_types().empty());

    // Name lookup in db_a must fail.
    EXPECT_FALSE(cat.get_edge_type(*db_a, "cross_edge").has_value());

    // db_b's tables must still exist.
    EXPECT_TRUE(cat.get_table_by_id(*tb_src).has_value());
    EXPECT_TRUE(cat.get_table_by_id(*tb_tgt).has_value());
}

// ---------------------------------------------------------------------------
// GDB786: edge in a surviving DB referencing dropped DB's tables is removed
// ---------------------------------------------------------------------------

TEST(GDB786, DanglingEdgeInSurvivingDbRemovedOnDropDatabase) {
    Catalog cat;

    auto db_a = cat.create_database("db_a");
    ASSERT_TRUE(db_a.has_value());
    auto db_b = cat.create_database("db_b");
    ASSERT_TRUE(db_b.has_value());

    // Tables in db_a (will be dropped with cascade).
    auto ta_src = cat.create_table(*db_a, make_schema("a_src"));
    ASSERT_TRUE(ta_src.has_value());
    auto ta_tgt = cat.create_table(*db_a, make_schema("a_tgt"));
    ASSERT_TRUE(ta_tgt.has_value());

    // Edge owned by db_b but pointing at db_a tables.
    auto eid = cat.create_edge_type(*db_b, make_edge_def("dangling_edge", *ta_src, *ta_tgt));
    ASSERT_TRUE(eid.has_value()) << eid.error().message;

    // Sanity: visible before drop.
    ASSERT_EQ(cat.list_all_edge_types().size(), 1u);

    // Drop db_a with cascade.
    auto drop = cat.drop_database(*db_a, /*cascade=*/true);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // Dangling edge must be gone from the global list.
    EXPECT_TRUE(cat.list_all_edge_types().empty());

    // Name lookup via db_b must fail.
    EXPECT_FALSE(cat.get_edge_type(*db_b, "dangling_edge").has_value());

    // db_b itself must still exist.
    EXPECT_TRUE(cat.get_database("db_b").has_value());
}

// ---------------------------------------------------------------------------
// GDB786: unrelated edge types in surviving databases are untouched
// ---------------------------------------------------------------------------

TEST(GDB786, UnrelatedEdgeTypeSurvives) {
    Catalog cat;

    auto db_a = cat.create_database("db_a");
    ASSERT_TRUE(db_a.has_value());
    auto db_b = cat.create_database("db_b");
    ASSERT_TRUE(db_b.has_value());

    // Tables in db_b only.
    auto tb1 = cat.create_table(*db_b, make_schema("b1"));
    ASSERT_TRUE(tb1.has_value());
    auto tb2 = cat.create_table(*db_b, make_schema("b2"));
    ASSERT_TRUE(tb2.has_value());

    // Edge owned by db_b referencing db_b tables — completely unrelated to db_a.
    auto eid = cat.create_edge_type(*db_b, make_edge_def("safe_edge", *tb1, *tb2));
    ASSERT_TRUE(eid.has_value());

    // Drop db_a (empty, no cascade needed).
    auto drop = cat.drop_database(*db_a, /*cascade=*/false);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // safe_edge must still be present.
    auto all = cat.list_all_edge_types();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].name, "safe_edge");

    auto by_name = cat.get_edge_type(*db_b, "safe_edge");
    ASSERT_TRUE(by_name.has_value()) << by_name.error().message;
    EXPECT_EQ(by_name->name, "safe_edge");
}

// ---------------------------------------------------------------------------
// GDB786: primary map and name map stay consistent after mixed drops
// ---------------------------------------------------------------------------

TEST(GDB786, MapConsistencyAfterDrop) {
    Catalog cat;

    auto db_a = cat.create_database("db_a");
    ASSERT_TRUE(db_a.has_value());
    auto db_b = cat.create_database("db_b");
    ASSERT_TRUE(db_b.has_value());

    // db_a tables.
    auto ta = cat.create_table(*db_a, make_schema("ta"));
    ASSERT_TRUE(ta.has_value());

    // db_b tables.
    auto tb = cat.create_table(*db_b, make_schema("tb"));
    ASSERT_TRUE(tb.has_value());

    // Edge in db_a referencing both its own table and db_b table.
    auto e1 = cat.create_edge_type(*db_a, make_edge_def("mixed_edge", *ta, *tb));
    ASSERT_TRUE(e1.has_value());

    // Edge in db_b referencing db_b tables only (unrelated).
    auto e2 = cat.create_edge_type(*db_b, make_edge_def("local_edge", *tb, *tb));
    ASSERT_TRUE(e2.has_value());

    ASSERT_EQ(cat.list_all_edge_types().size(), 2u);

    // Drop db_a with cascade.
    auto drop = cat.drop_database(*db_a, /*cascade=*/true);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // Only local_edge must survive.
    auto all = cat.list_all_edge_types();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].name, "local_edge");

    // mixed_edge must not be reachable via db_a name map.
    EXPECT_FALSE(cat.get_edge_type(*db_a, "mixed_edge").has_value());

    // local_edge must be reachable via db_b name map.
    EXPECT_TRUE(cat.get_edge_type(*db_b, "local_edge").has_value());
}
