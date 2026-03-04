/// QA adversarial tests for GDB-98: System catalog tables (sys_tables, sys_columns, sys_indexes).
/// Tests catalog CRUD, schema integrity, cascade deletes, and edge cases.

#include "giodb/catalog/catalog.h"
#include "giodb/catalog/schema.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace giodb;

// =============================================================================
// Helper: make a simple TableSchema
// =============================================================================
static TableSchema make_schema(const std::string& name,
                               const std::vector<CatalogColumnDef>& cols = {},
                               const std::string& pk = "") {
    TableSchema schema;
    schema.table_id = 0; // Will be auto-assigned
    schema.name = name;
    schema.columns = cols;
    schema.pk_columns = pk;
    return schema;
}

static CatalogColumnDef make_col(int32_t ordinal,
                                 const std::string& name,
                                 TypeId type,
                                 bool nullable = true,
                                 const std::string& default_expr = "") {
    return {ordinal, name, type, nullable, default_expr};
}

// =============================================================================
// Table Creation Edge Cases
// =============================================================================

TEST(QA_GDB98_Tables, CreateTableEmptyName) {
    Catalog catalog;
    auto schema = make_schema("");
    auto result = catalog.create_table(default_database_id, schema);
    // Should either succeed (empty name is technically valid) or fail gracefully
    // The key point is it shouldn't crash or corrupt state
    if (result.has_value()) {
        auto t = catalog.get_table(default_database_id, "");
        ASSERT_TRUE(t.has_value());
    }
}

TEST(QA_GDB98_Tables, CreateTableWithSpecialCharacters) {
    Catalog catalog;
    auto schema = make_schema("table with spaces & special! chars @#$");
    auto result = catalog.create_table(default_database_id, schema);
    ASSERT_TRUE(result.has_value());

    auto t = catalog.get_table(default_database_id, "table with spaces & special! chars @#$");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->name, "table with spaces & special! chars @#$");
}

TEST(QA_GDB98_Tables, CreateTableVeryLongName) {
    Catalog catalog;
    std::string long_name(1000, 'a');
    auto schema = make_schema(long_name);
    auto result = catalog.create_table(default_database_id, schema);
    ASSERT_TRUE(result.has_value());

    auto t = catalog.get_table(default_database_id, long_name);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->name.size(), 1000u);
}

TEST(QA_GDB98_Tables, CreateTableNoColumns) {
    Catalog catalog;
    auto schema = make_schema("empty_cols");
    auto result = catalog.create_table(default_database_id, schema);
    ASSERT_TRUE(result.has_value());

    auto t = catalog.get_table(default_database_id, "empty_cols");
    ASSERT_TRUE(t.has_value());
    EXPECT_TRUE(t->columns.empty());
}

TEST(QA_GDB98_Tables, CreateTableManyColumns) {
    Catalog catalog;
    std::vector<CatalogColumnDef> cols;
    for (int i = 0; i < 100; ++i) {
        cols.push_back(make_col(i, "col_" + std::to_string(i), TypeId::INT64));
    }
    auto schema = make_schema("wide_table", cols);
    auto result = catalog.create_table(default_database_id, schema);
    ASSERT_TRUE(result.has_value());

    auto t = catalog.get_table(default_database_id, "wide_table");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->columns.size(), 100u);
}

TEST(QA_GDB98_Tables, CreateTableDuplicateColumnNames) {
    Catalog catalog;
    std::vector<CatalogColumnDef> cols;
    cols.push_back(make_col(0, "dup_col", TypeId::INT32));
    cols.push_back(make_col(1, "dup_col", TypeId::STRING));
    auto schema = make_schema("dup_cols", cols);

    // The catalog might not validate duplicate column names itself
    auto result = catalog.create_table(default_database_id, schema);
    // Whether it succeeds or fails, it shouldn't crash
    (void)result;
}

TEST(QA_GDB98_Tables, CreateTableInvalidDatabaseId) {
    Catalog catalog;
    auto schema = make_schema("test");
    auto result = catalog.create_table(999, schema);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_GDB98_Tables, CreateDuplicateTableName) {
    Catalog catalog;
    auto s1 = make_schema("users");
    auto s2 = make_schema("users");

    auto r1 = catalog.create_table(default_database_id, s1);
    ASSERT_TRUE(r1.has_value());

    auto r2 = catalog.create_table(default_database_id, s2);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, StatusCode::ALREADY_EXISTS);
}

TEST(QA_GDB98_Tables, SameNameDifferentDatabases) {
    Catalog catalog;
    auto db = catalog.create_database("other_db");
    ASSERT_TRUE(db.has_value());

    auto s1 = make_schema("users");
    auto s2 = make_schema("users");

    auto r1 = catalog.create_table(default_database_id, s1);
    ASSERT_TRUE(r1.has_value());

    auto r2 = catalog.create_table(*db, s2);
    ASSERT_TRUE(r2.has_value());

    // They should have different table IDs
    EXPECT_NE(*r1, *r2);
}

// =============================================================================
// Table ID Assignment
// =============================================================================

TEST(QA_GDB98_Tables, IdsAreSequential) {
    Catalog catalog;
    std::vector<table_id_t> ids;
    for (int i = 0; i < 5; ++i) {
        auto schema = make_schema("t" + std::to_string(i));
        auto r = catalog.create_table(default_database_id, schema);
        ASSERT_TRUE(r.has_value());
        ids.push_back(*r);
    }

    for (size_t i = 1; i < ids.size(); ++i) {
        EXPECT_GT(ids[i], ids[i - 1]);
    }
}

TEST(QA_GDB98_Tables, DroppedIdNotReused) {
    Catalog catalog;
    auto r1 = catalog.create_table(default_database_id, make_schema("t1"));
    ASSERT_TRUE(r1.has_value());
    table_id_t first_id = *r1;

    (void)catalog.drop_table(default_database_id, "t1");

    auto r2 = catalog.create_table(default_database_id, make_schema("t2"));
    ASSERT_TRUE(r2.has_value());
    EXPECT_GT(*r2, first_id);
}

// =============================================================================
// Table Retrieval
// =============================================================================

TEST(QA_GDB98_Tables, GetTableNotFound) {
    Catalog catalog;
    auto r = catalog.get_table(default_database_id, "nonexistent");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_GDB98_Tables, GetTableByIdNotFound) {
    Catalog catalog;
    auto r = catalog.get_table_by_id(9999);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_GDB98_Tables, GetTableByIdAfterDrop) {
    Catalog catalog;
    auto r1 = catalog.create_table(default_database_id, make_schema("t1"));
    ASSERT_TRUE(r1.has_value());
    table_id_t id = *r1;

    (void)catalog.drop_table(default_database_id, "t1");

    auto r2 = catalog.get_table_by_id(id);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, StatusCode::NOT_FOUND);
}

// =============================================================================
// Table Drop Edge Cases
// =============================================================================

TEST(QA_GDB98_Tables, DropNonexistentTable) {
    Catalog catalog;
    auto r = catalog.drop_table(default_database_id, "nonexistent");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_GDB98_Tables, DropAndRecreate) {
    Catalog catalog;
    auto r1 = catalog.create_table(default_database_id, make_schema("t"));
    ASSERT_TRUE(r1.has_value());

    auto dr = catalog.drop_table(default_database_id, "t");
    ASSERT_TRUE(dr.has_value());

    auto r2 = catalog.create_table(default_database_id, make_schema("t"));
    ASSERT_TRUE(r2.has_value());
    EXPECT_NE(*r1, *r2); // Different ID
}

TEST(QA_GDB98_Tables, DropTableTwice) {
    Catalog catalog;
    (void)catalog.create_table(default_database_id, make_schema("t"));

    auto d1 = catalog.drop_table(default_database_id, "t");
    ASSERT_TRUE(d1.has_value());

    auto d2 = catalog.drop_table(default_database_id, "t");
    ASSERT_FALSE(d2.has_value());
    EXPECT_EQ(d2.error().code, StatusCode::NOT_FOUND);
}

// =============================================================================
// Column Schema Integrity
// =============================================================================

TEST(QA_GDB98_Columns, ColumnOrdinalPreserved) {
    Catalog catalog;
    std::vector<CatalogColumnDef> cols;
    cols.push_back(make_col(0, "id", TypeId::INT32, false));
    cols.push_back(make_col(1, "name", TypeId::STRING));
    cols.push_back(make_col(2, "active", TypeId::BOOL, true, "true"));

    auto r = catalog.create_table(default_database_id, make_schema("t", cols));
    ASSERT_TRUE(r.has_value());

    auto t = catalog.get_table(default_database_id, "t");
    ASSERT_TRUE(t.has_value());
    ASSERT_EQ(t->columns.size(), 3u);

    EXPECT_EQ(t->columns[0].ordinal, 0);
    EXPECT_EQ(t->columns[0].name, "id");
    EXPECT_EQ(t->columns[0].type_id, TypeId::INT32);
    EXPECT_FALSE(t->columns[0].nullable);

    EXPECT_EQ(t->columns[1].ordinal, 1);
    EXPECT_EQ(t->columns[1].name, "name");
    EXPECT_EQ(t->columns[1].type_id, TypeId::STRING);
    EXPECT_TRUE(t->columns[1].nullable);

    EXPECT_EQ(t->columns[2].ordinal, 2);
    EXPECT_EQ(t->columns[2].name, "active");
    EXPECT_EQ(t->columns[2].default_expr, "true");
}

TEST(QA_GDB98_Columns, AllTypeIds) {
    Catalog catalog;
    // Test all standard column types work
    TypeId types[] = {
        TypeId::INT8,
        TypeId::INT16,
        TypeId::INT32,
        TypeId::INT64,
        TypeId::UINT8,
        TypeId::UINT16,
        TypeId::UINT32,
        TypeId::UINT64,
        TypeId::FLOAT32,
        TypeId::FLOAT64,
        TypeId::BOOL,
        TypeId::STRING,
        TypeId::BLOB,
        TypeId::DATE,
        TypeId::TIME,
        TypeId::TIMESTAMP,
    };

    std::vector<CatalogColumnDef> cols;
    for (int i = 0; i < 16; ++i) {
        cols.push_back(make_col(i, "col_" + std::to_string(i), types[i]));
    }

    auto r = catalog.create_table(default_database_id, make_schema("all_types", cols));
    ASSERT_TRUE(r.has_value());

    auto t = catalog.get_table(default_database_id, "all_types");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->columns.size(), 16u);

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(t->columns[i].type_id, types[i]) << "Type mismatch at column " << i;
    }
}

// =============================================================================
// Index Operations
// =============================================================================

TEST(QA_GDB98_Indexes, CreateAndRetrieve) {
    Catalog catalog;
    auto tid = catalog.create_table(default_database_id,
                                    make_schema("users", {make_col(0, "id", TypeId::INT32)}));
    ASSERT_TRUE(tid.has_value());

    IndexDef idef;
    idef.index_id = 0; // Will be auto-assigned
    idef.table_id = *tid;
    idef.name = "idx_users_id";
    idef.index_type = "btree";
    idef.columns = "id";
    idef.is_unique = true;

    auto r = catalog.create_index(idef);
    ASSERT_TRUE(r.has_value());

    auto idx = catalog.get_index("idx_users_id");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(idx->table_id, *tid);
    EXPECT_EQ(idx->index_type, "btree");
    EXPECT_TRUE(idx->is_unique);
}

TEST(QA_GDB98_Indexes, DuplicateIndexName) {
    Catalog catalog;
    auto tid = catalog.create_table(default_database_id, make_schema("t"));
    ASSERT_TRUE(tid.has_value());

    IndexDef def1{0, *tid, "idx", "btree", "col1", false};
    IndexDef def2{0, *tid, "idx", "hash", "col2", true};

    auto r1 = catalog.create_index(def1);
    ASSERT_TRUE(r1.has_value());

    auto r2 = catalog.create_index(def2);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, StatusCode::ALREADY_EXISTS);
}

TEST(QA_GDB98_Indexes, IndexOnNonexistentTable) {
    Catalog catalog;
    IndexDef def{0, 9999, "idx", "btree", "col1", false};
    auto r = catalog.create_index(def);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_GDB98_Indexes, CascadeDropWithTable) {
    Catalog catalog;
    auto tid = catalog.create_table(default_database_id, make_schema("t"));
    ASSERT_TRUE(tid.has_value());

    IndexDef def1{0, *tid, "idx1", "btree", "col1", false};
    IndexDef def2{0, *tid, "idx2", "hash", "col2", true};
    (void)catalog.create_index(def1);
    (void)catalog.create_index(def2);

    // Drop table cascades to indexes
    auto dr = catalog.drop_table(default_database_id, "t");
    ASSERT_TRUE(dr.has_value());

    auto i1 = catalog.get_index("idx1");
    ASSERT_FALSE(i1.has_value());

    auto i2 = catalog.get_index("idx2");
    ASSERT_FALSE(i2.has_value());

    auto all = catalog.list_indexes(*tid);
    EXPECT_TRUE(all.empty());
}

TEST(QA_GDB98_Indexes, DropNonexistentIndex) {
    Catalog catalog;
    auto r = catalog.drop_index("nonexistent");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_GDB98_Indexes, ListAllIndexes) {
    Catalog catalog;
    auto t1 = catalog.create_table(default_database_id, make_schema("t1"));
    auto t2 = catalog.create_table(default_database_id, make_schema("t2"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    (void)catalog.create_index({0, *t1, "idx_a", "btree", "c1", false});
    (void)catalog.create_index({0, *t2, "idx_b", "hash", "c2", true});
    (void)catalog.create_index({0, *t1, "idx_c", "btree", "c3", false});

    auto all = catalog.list_all_indexes();
    EXPECT_EQ(all.size(), 3u);

    auto t1_indexes = catalog.list_indexes(*t1);
    EXPECT_EQ(t1_indexes.size(), 2u);

    auto t2_indexes = catalog.list_indexes(*t2);
    EXPECT_EQ(t2_indexes.size(), 1u);
}

// =============================================================================
// System Table Schema Definitions
// =============================================================================

TEST(QA_GDB98_SysSchema, SysTablesSchema) {
    auto schema = sys_tables_schema();
    EXPECT_EQ(schema.name, "sys_tables");
    EXPECT_GE(schema.columns.size(), 3u); // At minimum: table_id, name, pk_columns
    // Verify key columns exist
    bool has_table_id = false, has_name = false;
    for (const auto& col : schema.columns) {
        if (col.name == "table_id")
            has_table_id = true;
        if (col.name == "name")
            has_name = true;
    }
    EXPECT_TRUE(has_table_id);
    EXPECT_TRUE(has_name);
}

TEST(QA_GDB98_SysSchema, SysColumnsSchema) {
    auto schema = sys_columns_schema();
    EXPECT_EQ(schema.name, "sys_columns");
    bool has_table_id = false, has_ordinal = false, has_name = false, has_type_id = false;
    for (const auto& col : schema.columns) {
        if (col.name == "table_id")
            has_table_id = true;
        if (col.name == "ordinal")
            has_ordinal = true;
        if (col.name == "name")
            has_name = true;
        if (col.name == "type_id")
            has_type_id = true;
    }
    EXPECT_TRUE(has_table_id);
    EXPECT_TRUE(has_ordinal);
    EXPECT_TRUE(has_name);
    EXPECT_TRUE(has_type_id);
}

TEST(QA_GDB98_SysSchema, SysIndexesSchema) {
    auto schema = sys_indexes_schema();
    EXPECT_EQ(schema.name, "sys_indexes");
    bool has_index_id = false, has_table_id = false, has_is_unique = false;
    for (const auto& col : schema.columns) {
        if (col.name == "index_id")
            has_index_id = true;
        if (col.name == "table_id")
            has_table_id = true;
        if (col.name == "is_unique")
            has_is_unique = true;
    }
    EXPECT_TRUE(has_index_id);
    EXPECT_TRUE(has_table_id);
    EXPECT_TRUE(has_is_unique);
}

// =============================================================================
// Restore (Persistence) Operations
// =============================================================================

TEST(QA_GDB98_Restore, RestoreTablePreservesId) {
    Catalog catalog;

    TableSchema schema;
    schema.table_id = 42; // Pre-assigned
    schema.name = "restored_table";
    schema.columns = {make_col(0, "x", TypeId::INT32)};

    auto r = catalog.restore_table(default_database_id, schema);
    ASSERT_TRUE(r.has_value());

    auto t = catalog.get_table(default_database_id, "restored_table");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->table_id, 42);
}

TEST(QA_GDB98_Restore, RestoreIndexPreservesId) {
    Catalog catalog;
    auto tid = catalog.create_table(default_database_id, make_schema("t"));
    ASSERT_TRUE(tid.has_value());

    IndexDef def;
    def.index_id = 100; // Pre-assigned
    def.table_id = *tid;
    def.name = "restored_idx";
    def.index_type = "btree";
    def.columns = "col1";
    def.is_unique = false;

    auto r = catalog.restore_index(def);
    ASSERT_TRUE(r.has_value());

    auto idx = catalog.get_index("restored_idx");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(idx->index_id, 100);
}

TEST(QA_GDB98_Restore, SetNextIdAdvancesSequence) {
    Catalog catalog;
    catalog.set_next_table_id(100);

    auto tid = catalog.create_table(default_database_id, make_schema("t"));
    ASSERT_TRUE(tid.has_value());
    EXPECT_GE(*tid, 100);
}

// =============================================================================
// List Operations
// =============================================================================

TEST(QA_GDB98_List, ListTablesEmpty) {
    Catalog catalog;
    auto tables = catalog.list_tables(default_database_id);
    EXPECT_TRUE(tables.empty());
}

TEST(QA_GDB98_List, ListTablesSorted) {
    Catalog catalog;
    // Create in reverse order
    for (int i = 5; i >= 1; --i) {
        (void)catalog.create_table(default_database_id, make_schema("t" + std::to_string(i)));
    }

    auto tables = catalog.list_tables(default_database_id);
    EXPECT_EQ(tables.size(), 5u);

    // Should be sorted by table_id
    for (size_t i = 1; i < tables.size(); ++i) {
        EXPECT_LT(tables[i - 1].table_id, tables[i].table_id);
    }
}

// =============================================================================
// Stress Test
// =============================================================================

TEST(QA_GDB98_Stress, ManyTablesAndIndexes) {
    Catalog catalog;

    // Create 50 tables with 3 columns each
    for (int i = 0; i < 50; ++i) {
        std::vector<CatalogColumnDef> cols;
        cols.push_back(make_col(0, "id", TypeId::INT32, false));
        cols.push_back(make_col(1, "name", TypeId::STRING));
        cols.push_back(make_col(2, "value", TypeId::FLOAT64));

        auto schema = make_schema("table_" + std::to_string(i), cols, "id");
        auto tid = catalog.create_table(default_database_id, schema);
        ASSERT_TRUE(tid.has_value());

        // Create 2 indexes per table
        IndexDef idx1{0, *tid, "idx_" + std::to_string(i) + "_pk", "btree", "id", true};
        IndexDef idx2{0, *tid, "idx_" + std::to_string(i) + "_name", "btree", "name", false};
        ASSERT_TRUE(catalog.create_index(idx1).has_value());
        ASSERT_TRUE(catalog.create_index(idx2).has_value());
    }

    auto tables = catalog.list_tables(default_database_id);
    EXPECT_EQ(tables.size(), 50u);

    auto all_indexes = catalog.list_all_indexes();
    EXPECT_EQ(all_indexes.size(), 100u);

    // Drop half the tables and verify cascade
    for (int i = 0; i < 25; ++i) {
        auto dr = catalog.drop_table(default_database_id, "table_" + std::to_string(i));
        ASSERT_TRUE(dr.has_value());
    }

    tables = catalog.list_tables(default_database_id);
    EXPECT_EQ(tables.size(), 25u);

    all_indexes = catalog.list_all_indexes();
    EXPECT_EQ(all_indexes.size(), 50u); // Half should be cascaded away
}
