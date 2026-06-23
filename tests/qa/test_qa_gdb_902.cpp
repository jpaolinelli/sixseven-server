// GDB-902 QA: Catalog::get_index and drop_index must be scoped per database.
//
// Bug: get_index(name) and drop_index(name) iterated index_name_to_id_ across
// all databases, returning/erasing the FIRST match in unordered_map iteration
// order. If databases A and B both have an index named "idx_dup", DROP INDEX
// while connected to A could silently drop B's index.
//
// Fix: signatures changed to get_index(database_id, name) and
// drop_index(database_id, name); the implementation now only looks up the name
// in the inner map for that specific database.
//
// Mutation target: the OLD unscoped loop in drop_index/get_index fails the
// cross-database isolation test.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"

#include <gtest/gtest.h>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

static TableSchema make_schema(const std::string& name) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {{0, "id", TypeId::INT32, false, ""}};
    schema.pk_columns = "id";
    return schema;
}

static IndexDef make_index_def(table_id_t tid, const std::string& name) {
    IndexDef def;
    def.table_id = tid;
    def.name = name;
    def.index_type = "btree";
    def.columns = "id";
    return def;
}

} // namespace

// Create same-named index in two databases; drop in A, verify B survives.
TEST(QA_GDB902, DropIndexScopedToDatabase) {
    Catalog catalog;

    auto db_a = catalog.create_database("db_a");
    ASSERT_TRUE(db_a.has_value()) << db_a.error().message;
    auto db_b = catalog.create_database("db_b");
    ASSERT_TRUE(db_b.has_value()) << db_b.error().message;

    auto tid_a = catalog.create_table(*db_a, make_schema("tbl_a"));
    ASSERT_TRUE(tid_a.has_value()) << tid_a.error().message;
    auto tid_b = catalog.create_table(*db_b, make_schema("tbl_b"));
    ASSERT_TRUE(tid_b.has_value()) << tid_b.error().message;

    // Same index name in both databases -- create_index allows this.
    auto iid_a = catalog.create_index(make_index_def(*tid_a, "idx_dup"));
    ASSERT_TRUE(iid_a.has_value()) << iid_a.error().message;
    auto iid_b = catalog.create_index(make_index_def(*tid_b, "idx_dup"));
    ASSERT_TRUE(iid_b.has_value()) << iid_b.error().message;

    // Both must be retrievable from their respective databases.
    auto get_a = catalog.get_index(*db_a, "idx_dup");
    ASSERT_TRUE(get_a.has_value()) << get_a.error().message;
    EXPECT_EQ(get_a->table_id, *tid_a);

    auto get_b = catalog.get_index(*db_b, "idx_dup");
    ASSERT_TRUE(get_b.has_value()) << get_b.error().message;
    EXPECT_EQ(get_b->table_id, *tid_b);

    // Drop from db_a only.
    auto drop_a = catalog.drop_index(*db_a, "idx_dup");
    ASSERT_TRUE(drop_a.has_value()) << drop_a.error().message;

    // A's index is gone.
    auto get_a2 = catalog.get_index(*db_a, "idx_dup");
    EXPECT_FALSE(get_a2.has_value()) << "A's index should be gone after drop";

    // B's index survives unchanged.
    auto get_b2 = catalog.get_index(*db_b, "idx_dup");
    ASSERT_TRUE(get_b2.has_value()) << "B's index must survive drop in A";
    EXPECT_EQ(get_b2->table_id, *tid_b);
    EXPECT_EQ(get_b2->index_id, *iid_b);
}

// get_index in database A must NOT find an index that only exists in database B.
TEST(QA_GDB902, GetIndexDoesNotCrossDatabase) {
    Catalog catalog;

    auto db_a = catalog.create_database("db_a2");
    ASSERT_TRUE(db_a.has_value());
    auto db_b = catalog.create_database("db_b2");
    ASSERT_TRUE(db_b.has_value());

    auto tid_b = catalog.create_table(*db_b, make_schema("tbl_b2"));
    ASSERT_TRUE(tid_b.has_value());

    ASSERT_TRUE(catalog.create_index(make_index_def(*tid_b, "idx_only_in_b")).has_value());

    // db_a has no such index.
    auto get_in_a = catalog.get_index(*db_a, "idx_only_in_b");
    EXPECT_FALSE(get_in_a.has_value());
    if (!get_in_a.has_value()) {
        EXPECT_EQ(get_in_a.error().code, StatusCode::NOT_FOUND);
    }

    // db_b still finds it.
    auto get_in_b = catalog.get_index(*db_b, "idx_only_in_b");
    ASSERT_TRUE(get_in_b.has_value()) << get_in_b.error().message;
}

// drop_index in database A must NOT remove an index that only exists in database B.
TEST(QA_GDB902, DropIndexDoesNotCrossDatabase) {
    Catalog catalog;

    auto db_a = catalog.create_database("db_a3");
    ASSERT_TRUE(db_a.has_value());
    auto db_b = catalog.create_database("db_b3");
    ASSERT_TRUE(db_b.has_value());

    auto tid_b = catalog.create_table(*db_b, make_schema("tbl_b3"));
    ASSERT_TRUE(tid_b.has_value());

    ASSERT_TRUE(catalog.create_index(make_index_def(*tid_b, "idx_b_only")).has_value());

    // Attempt to drop from db_a -- must fail.
    auto drop_in_a = catalog.drop_index(*db_a, "idx_b_only");
    EXPECT_FALSE(drop_in_a.has_value());
    if (!drop_in_a.has_value()) {
        EXPECT_EQ(drop_in_a.error().code, StatusCode::NOT_FOUND);
    }

    // B's index must still exist.
    auto get_b = catalog.get_index(*db_b, "idx_b_only");
    ASSERT_TRUE(get_b.has_value()) << "B's index must survive erroneous drop in A";
}

// Single-database positive regression: create+get+drop still works.
TEST(QA_GDB902, SingleDatabaseCreateGetDrop) {
    Catalog catalog;
    // init_test_catalog creates the default database (database_id == 1).
    init_test_catalog(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema("tbl_single"));
    ASSERT_TRUE(tid.has_value()) << tid.error().message;

    ASSERT_TRUE(catalog.create_index(make_index_def(*tid, "idx_single")).has_value());

    auto get = catalog.get_index(default_database_id, "idx_single");
    ASSERT_TRUE(get.has_value()) << get.error().message;
    EXPECT_EQ(get->table_id, *tid);
    EXPECT_EQ(get->name, "idx_single");

    auto drop = catalog.drop_index(default_database_id, "idx_single");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // After drop: NOT_FOUND.
    auto get2 = catalog.get_index(default_database_id, "idx_single");
    EXPECT_FALSE(get2.has_value());
    if (!get2.has_value()) {
        EXPECT_EQ(get2.error().code, StatusCode::NOT_FOUND);
    }

    // Double drop: NOT_FOUND.
    auto drop2 = catalog.drop_index(default_database_id, "idx_single");
    EXPECT_FALSE(drop2.has_value());
    if (!drop2.has_value()) {
        EXPECT_EQ(drop2.error().code, StatusCode::NOT_FOUND);
    }
}
