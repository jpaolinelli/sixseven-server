// GDB-902 Adversarial QA: Catalog::get_index / drop_index database scoping.
//
// Adversarial cases not covered by the implementer's baseline tests:
//   1. Phantom-map-entry probe: get_index on a database_id with no indexes must
//      NOT insert an entry into index_name_to_id_ (operator[] side-effect).
//   2. Mass-isolation: N databases each holding a same-named index; dropping
//      from one database never silently removes from another.
//   3. Stale inner-map after drop: after drop_index the inner name-entry is
//      gone; a second create_index with the same name must SUCCEED (no stale
//      entry blocks it).
//   4. Cross-database ALREADY_EXISTS: creating the same-named index in two
//      different databases must SUCCEED (not ALREADY_EXISTS).
//   5. get_index returns the correct table_id even when many same-named indexes
//      exist across databases.
//   6. drop_index on a completely fresh catalog with no databases registered.
//   7. list_indexes does not leak indexes across databases.
//   8. Reverse-order drop: create in A then B, drop B first, A survives.
//   9. Non-existent database_id — no operator[] insert side-effect (probed by
//      checking list_all_indexes count is unchanged).
//  10. After drop_index, list_all_indexes decreases by exactly one.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"

#include <gtest/gtest.h>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

static TableSchema make_table(const std::string& name) {
    TableSchema s;
    s.name = name;
    s.columns = {{0, "id", TypeId::INT32, false, ""}};
    s.pk_columns = "id";
    return s;
}

static IndexDef make_index(table_id_t tid, const std::string& name) {
    IndexDef d;
    d.table_id = tid;
    d.name = name;
    d.index_type = "btree";
    d.columns = "id";
    return d;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Phantom-map-entry probe
//    get_index on a db_id that has NO entries must return NOT_FOUND and must
//    NOT silently insert a key into index_name_to_id_ (which would make a
//    subsequent create_index with a different db_id use the wrong inner map).
// ---------------------------------------------------------------------------
TEST(QA_GDB902_Adversarial, GetIndexOnUnknownDbDoesNotInsertPhantomEntry) {
    Catalog catalog;
    init_test_catalog(catalog);

    // db_id 999 has never been registered.
    database_id_t phantom_db = 999;

    auto r = catalog.get_index(phantom_db, "idx_ghost");
    EXPECT_FALSE(r.has_value());
    if (!r.has_value()) {
        EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
    }

    // Now create a real index in the default database. If operator[] had
    // inserted a phantom entry, the name lookup table may have been corrupted.
    auto tid = catalog.create_table(default_database_id, make_table("tbl_phantom_probe"));
    ASSERT_TRUE(tid.has_value()) << tid.error().message;

    auto iid = catalog.create_index(make_index(*tid, "idx_phantom_probe"));
    ASSERT_TRUE(iid.has_value()) << iid.error().message;

    // Must be retrievable from the correct db.
    auto get_ok = catalog.get_index(default_database_id, "idx_phantom_probe");
    ASSERT_TRUE(get_ok.has_value()) << get_ok.error().message;
    EXPECT_EQ(get_ok->table_id, *tid);

    // Must NOT be found under the phantom db.
    auto get_phantom = catalog.get_index(phantom_db, "idx_phantom_probe");
    EXPECT_FALSE(get_phantom.has_value());
}

// ---------------------------------------------------------------------------
// 2. Mass-isolation: 5 databases each with "idx_shared"
//    Dropping from db[2] must leave the other 4 intact.
// ---------------------------------------------------------------------------
TEST(QA_GDB902_Adversarial, MassIsolationDropOneOfFive) {
    Catalog catalog;

    const int N = 5;
    std::vector<database_id_t> db_ids;
    std::vector<table_id_t> tbl_ids;
    std::vector<index_id_t> idx_ids;

    for (int i = 0; i < N; ++i) {
        auto db = catalog.create_database("mass_db_" + std::to_string(i));
        ASSERT_TRUE(db.has_value()) << db.error().message;
        db_ids.push_back(*db);

        auto tid = catalog.create_table(*db, make_table("tbl_mass_" + std::to_string(i)));
        ASSERT_TRUE(tid.has_value()) << tid.error().message;
        tbl_ids.push_back(*tid);

        auto iid = catalog.create_index(make_index(*tid, "idx_shared"));
        ASSERT_TRUE(iid.has_value()) << iid.error().message;
        idx_ids.push_back(*iid);
    }

    // Drop from db[2] only.
    auto drop = catalog.drop_index(db_ids[2], "idx_shared");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // db[2] is gone.
    auto gone = catalog.get_index(db_ids[2], "idx_shared");
    EXPECT_FALSE(gone.has_value()) << "db[2] index should be gone";

    // All other databases still have their index with the correct table_id.
    for (int i = 0; i < N; ++i) {
        if (i == 2) {
            continue;
        }
        auto r = catalog.get_index(db_ids[i], "idx_shared");
        ASSERT_TRUE(r.has_value())
            << "db[" << i << "] idx_shared must survive drop in db[2]: " << r.error().message;
        EXPECT_EQ(r->table_id, tbl_ids[i])
            << "db[" << i << "] index points to wrong table after cross-db drop";
        EXPECT_EQ(r->index_id, idx_ids[i])
            << "db[" << i << "] index_id changed after cross-db drop";
    }
}

// ---------------------------------------------------------------------------
// 3. Stale inner-map after drop: re-creating same name in same db must succeed.
// ---------------------------------------------------------------------------
TEST(QA_GDB902_Adversarial, RecreateIndexSameNameSameDbAfterDrop) {
    Catalog catalog;
    init_test_catalog(catalog);

    auto tid = catalog.create_table(default_database_id, make_table("tbl_recreate"));
    ASSERT_TRUE(tid.has_value()) << tid.error().message;

    // First create.
    auto iid1 = catalog.create_index(make_index(*tid, "idx_recreate"));
    ASSERT_TRUE(iid1.has_value()) << iid1.error().message;

    // Drop it.
    ASSERT_TRUE(catalog.drop_index(default_database_id, "idx_recreate").has_value());

    // Re-create — must not fail with ALREADY_EXISTS (stale inner entry bug).
    auto iid2 = catalog.create_index(make_index(*tid, "idx_recreate"));
    ASSERT_TRUE(iid2.has_value()) << "Re-create after drop must succeed: " << iid2.error().message;

    // New index has a different index_id.
    EXPECT_NE(*iid1, *iid2) << "Recreated index must get a new index_id";

    // Retrievable with new id.
    auto get = catalog.get_index(default_database_id, "idx_recreate");
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ(get->index_id, *iid2);
}

// ---------------------------------------------------------------------------
// 4. Cross-database ALREADY_EXISTS: same name in two dbs must be allowed.
// ---------------------------------------------------------------------------
TEST(QA_GDB902_Adversarial, SameIndexNameTwoDatabasesIsAllowed) {
    Catalog catalog;

    auto db_x = catalog.create_database("db_x_ae");
    ASSERT_TRUE(db_x.has_value());
    auto db_y = catalog.create_database("db_y_ae");
    ASSERT_TRUE(db_y.has_value());

    auto tx = catalog.create_table(*db_x, make_table("tbl_x_ae"));
    ASSERT_TRUE(tx.has_value());
    auto ty = catalog.create_table(*db_y, make_table("tbl_y_ae"));
    ASSERT_TRUE(ty.has_value());

    auto ix = catalog.create_index(make_index(*tx, "idx_ae"));
    ASSERT_TRUE(ix.has_value()) << "First create must succeed: " << ix.error().message;

    // Same name, different db — must NOT return ALREADY_EXISTS.
    auto iy = catalog.create_index(make_index(*ty, "idx_ae"));
    ASSERT_TRUE(iy.has_value())
        << "create_index with same name in different db must succeed: " << iy.error().message;

    EXPECT_NE(*ix, *iy) << "Two indexes in different dbs must have distinct index_ids";
}

// ---------------------------------------------------------------------------
// 5. get_index returns correct table_id when same name exists in many dbs.
// ---------------------------------------------------------------------------
TEST(QA_GDB902_Adversarial, GetIndexReturnsCorrectDefinitionPerDb) {
    Catalog catalog;

    auto db1 = catalog.create_database("db_correct_1");
    auto db2 = catalog.create_database("db_correct_2");
    auto db3 = catalog.create_database("db_correct_3");
    ASSERT_TRUE(db1.has_value());
    ASSERT_TRUE(db2.has_value());
    ASSERT_TRUE(db3.has_value());

    auto t1 = catalog.create_table(*db1, make_table("tbl_c1"));
    auto t2 = catalog.create_table(*db2, make_table("tbl_c2"));
    auto t3 = catalog.create_table(*db3, make_table("tbl_c3"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());
    ASSERT_TRUE(t3.has_value());

    ASSERT_TRUE(catalog.create_index(make_index(*t1, "idx_correct")).has_value());
    ASSERT_TRUE(catalog.create_index(make_index(*t2, "idx_correct")).has_value());
    ASSERT_TRUE(catalog.create_index(make_index(*t3, "idx_correct")).has_value());

    // Each db returns its OWN table_id — not another db's.
    auto r1 = catalog.get_index(*db1, "idx_correct");
    auto r2 = catalog.get_index(*db2, "idx_correct");
    auto r3 = catalog.get_index(*db3, "idx_correct");

    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    ASSERT_TRUE(r3.has_value()) << r3.error().message;

    EXPECT_EQ(r1->table_id, *t1) << "db1 get_index returned wrong table_id";
    EXPECT_EQ(r2->table_id, *t2) << "db2 get_index returned wrong table_id";
    EXPECT_EQ(r3->table_id, *t3) << "db3 get_index returned wrong table_id";

    // All three index_ids must be distinct.
    EXPECT_NE(r1->index_id, r2->index_id);
    EXPECT_NE(r2->index_id, r3->index_id);
    EXPECT_NE(r1->index_id, r3->index_id);
}

// ---------------------------------------------------------------------------
// 6. drop_index on completely fresh catalog (no databases, no indexes).
// ---------------------------------------------------------------------------
TEST(QA_GDB902_Adversarial, DropIndexOnFreshCatalogReturnsNotFound) {
    Catalog catalog;
    // No init_test_catalog — completely empty (only system db).
    auto r = catalog.drop_index(42, "idx_does_not_exist");
    EXPECT_FALSE(r.has_value());
    if (!r.has_value()) {
        EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
    }
}

// ---------------------------------------------------------------------------
// 7. list_indexes does not cross databases.
//    Table in db_a has idx_list; table in db_b has no indexes.
//    list_indexes(tid_b) must return empty.
// ---------------------------------------------------------------------------
TEST(QA_GDB902_Adversarial, ListIndexesDoesNotCrossDatabase) {
    Catalog catalog;

    auto dba = catalog.create_database("db_list_a");
    auto dbb = catalog.create_database("db_list_b");
    ASSERT_TRUE(dba.has_value());
    ASSERT_TRUE(dbb.has_value());

    auto ta = catalog.create_table(*dba, make_table("tbl_list_a"));
    auto tb = catalog.create_table(*dbb, make_table("tbl_list_b"));
    ASSERT_TRUE(ta.has_value());
    ASSERT_TRUE(tb.has_value());

    ASSERT_TRUE(catalog.create_index(make_index(*ta, "idx_list")).has_value());

    // tb has no indexes — list_indexes must return empty.
    auto indexes_b = catalog.list_indexes(*tb);
    EXPECT_TRUE(indexes_b.empty())
        << "list_indexes for tb must be empty (index is on ta in a different db)";

    // ta has one index.
    auto indexes_a = catalog.list_indexes(*ta);
    ASSERT_EQ(indexes_a.size(), 1u);
    EXPECT_EQ(indexes_a[0].name, "idx_list");
}

// ---------------------------------------------------------------------------
// 8. Reverse-order drop: create A then B, drop B first, A survives.
// ---------------------------------------------------------------------------
TEST(QA_GDB902_Adversarial, ReverseOrderDropPreservesFirstCreated) {
    Catalog catalog;

    auto da = catalog.create_database("db_rev_a");
    auto db = catalog.create_database("db_rev_b");
    ASSERT_TRUE(da.has_value());
    ASSERT_TRUE(db.has_value());

    auto ta = catalog.create_table(*da, make_table("tbl_rev_a"));
    auto tb = catalog.create_table(*db, make_table("tbl_rev_b"));
    ASSERT_TRUE(ta.has_value());
    ASSERT_TRUE(tb.has_value());

    auto ia = catalog.create_index(make_index(*ta, "idx_rev"));
    auto ib = catalog.create_index(make_index(*tb, "idx_rev"));
    ASSERT_TRUE(ia.has_value()) << ia.error().message;
    ASSERT_TRUE(ib.has_value()) << ib.error().message;

    // Drop B first.
    ASSERT_TRUE(catalog.drop_index(*db, "idx_rev").has_value());

    // B is gone.
    EXPECT_FALSE(catalog.get_index(*db, "idx_rev").has_value());

    // A still intact.
    auto ra = catalog.get_index(*da, "idx_rev");
    ASSERT_TRUE(ra.has_value()) << "A's index must survive drop of B's same-named index";
    EXPECT_EQ(ra->table_id, *ta);
    EXPECT_EQ(ra->index_id, *ia);

    // Now drop A.
    ASSERT_TRUE(catalog.drop_index(*da, "idx_rev").has_value());
    EXPECT_FALSE(catalog.get_index(*da, "idx_rev").has_value());
}

// ---------------------------------------------------------------------------
// 9. Non-existent db_id probe for drop_index: no side effects on
//    list_all_indexes count.
// ---------------------------------------------------------------------------
TEST(QA_GDB902_Adversarial, DropIndexNonExistentDbDoesNotCorruptGlobalList) {
    Catalog catalog;
    init_test_catalog(catalog);

    auto tid = catalog.create_table(default_database_id, make_table("tbl_global_list"));
    ASSERT_TRUE(tid.has_value());
    ASSERT_TRUE(catalog.create_index(make_index(*tid, "idx_global_list")).has_value());

    auto before = catalog.list_all_indexes();

    // Attempt drop with a non-existent db_id — should fail cleanly.
    auto r = catalog.drop_index(9999, "idx_global_list");
    EXPECT_FALSE(r.has_value());
    if (!r.has_value()) {
        EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
    }

    // Global list unchanged.
    auto after = catalog.list_all_indexes();
    EXPECT_EQ(before.size(), after.size())
        << "list_all_indexes count changed after failed cross-db drop attempt";
}

// ---------------------------------------------------------------------------
// 10. After drop_index, list_all_indexes decreases by exactly one.
// ---------------------------------------------------------------------------
TEST(QA_GDB902_Adversarial, DropIndexDecreasesGlobalListByOne) {
    Catalog catalog;
    init_test_catalog(catalog);

    auto ta = catalog.create_table(default_database_id, make_table("tbl_count_a"));
    auto tb = catalog.create_table(default_database_id, make_table("tbl_count_b"));
    ASSERT_TRUE(ta.has_value());
    ASSERT_TRUE(tb.has_value());

    ASSERT_TRUE(catalog.create_index(make_index(*ta, "idx_count_a")).has_value());
    ASSERT_TRUE(catalog.create_index(make_index(*tb, "idx_count_b")).has_value());

    auto before = catalog.list_all_indexes().size();

    ASSERT_TRUE(catalog.drop_index(default_database_id, "idx_count_a").has_value());

    auto after = catalog.list_all_indexes().size();
    EXPECT_EQ(after + 1, before) << "list_all_indexes must decrease by exactly 1 after drop";

    // Remaining index still correct.
    auto r = catalog.get_index(default_database_id, "idx_count_b");
    ASSERT_TRUE(r.has_value()) << "Non-dropped index must still be findable";
    EXPECT_EQ(r->table_id, *tb);
}
