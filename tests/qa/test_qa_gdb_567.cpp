/// @file test_qa_gdb_567.cpp
/// @brief Adversarial QA tests for GDB-567: pg_index and pg_database Virtual Tables

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/executor/pg_catalog_tables.h"
#include "sixseven/executor/virtual_catalog_scan.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sixseven {
namespace {

void init_test_catalog(Catalog& catalog) {
    auto r = catalog.restore_database(default_database_id, "demo");
    (void)r;
}

OutputSchema make_output_schema(const VirtualTableDef& def) {
    std::vector<OutputColumn> cols;
    for (const auto& col : def.columns) {
        cols.push_back({def.name, col.name, col.type_id, col.nullable, def.table_id});
    }
    return OutputSchema(std::move(cols));
}

// =========================================================================
// Scan helpers
// =========================================================================

struct IndexRow {
    int32_t indexrelid;
    int32_t indrelid;
    int16_t indnatts;
    bool indisunique;
    bool indisprimary;
    std::string indkey;
};

std::vector<IndexRow> scan_pg_index(Catalog& catalog) {
    auto vt = catalog.get_virtual_table("pg_index");
    EXPECT_TRUE(vt.has_value());
    if (!vt.has_value())
        return {};

    VirtualCatalogScanOperator scan(vt.value(), make_output_schema(vt.value()));
    auto open_r = scan.open();
    EXPECT_TRUE(open_r.has_value());
    if (!open_r.has_value())
        return {};

    std::vector<IndexRow> rows;
    while (true) {
        auto next_r = scan.next();
        EXPECT_TRUE(next_r.has_value());
        if (!next_r.has_value())
            break;
        if (!next_r->has_value())
            break;
        auto& tuple = next_r->value();
        rows.push_back({
            tuple.values[0].as_int32(),
            tuple.values[1].as_int32(),
            tuple.values[2].as_int16(),
            tuple.values[3].as_bool(),
            tuple.values[4].as_bool(),
            std::string(tuple.values[5].as_string()),
        });
    }
    scan.close();
    return rows;
}

struct DatabaseRow {
    int32_t oid;
    std::string datname;
    int32_t datdba;
    int32_t encoding;
};

std::vector<DatabaseRow> scan_pg_database(Catalog& catalog) {
    auto vt = catalog.get_virtual_table("pg_database");
    EXPECT_TRUE(vt.has_value());
    if (!vt.has_value())
        return {};

    VirtualCatalogScanOperator scan(vt.value(), make_output_schema(vt.value()));
    auto open_r = scan.open();
    EXPECT_TRUE(open_r.has_value());
    if (!open_r.has_value())
        return {};

    std::vector<DatabaseRow> rows;
    while (true) {
        auto next_r = scan.next();
        EXPECT_TRUE(next_r.has_value());
        if (!next_r.has_value())
            break;
        if (!next_r->has_value())
            break;
        auto& tuple = next_r->value();
        rows.push_back({
            tuple.values[0].as_int32(),
            std::string(tuple.values[1].as_string()),
            tuple.values[2].as_int32(),
            tuple.values[3].as_int32(),
        });
    }
    scan.close();
    return rows;
}

struct ClassRow {
    int32_t oid;
    std::string relname;
};

std::vector<ClassRow> scan_pg_class(Catalog& catalog) {
    auto vt = catalog.get_virtual_table("pg_class");
    EXPECT_TRUE(vt.has_value());
    if (!vt.has_value())
        return {};

    VirtualCatalogScanOperator scan(vt.value(), make_output_schema(vt.value()));
    auto open_r = scan.open();
    EXPECT_TRUE(open_r.has_value());
    if (!open_r.has_value())
        return {};

    std::vector<ClassRow> rows;
    while (true) {
        auto next_r = scan.next();
        EXPECT_TRUE(next_r.has_value());
        if (!next_r.has_value())
            break;
        if (!next_r->has_value())
            break;
        auto& tuple = next_r->value();
        rows.push_back({
            tuple.values[0].as_int32(),
            std::string(tuple.values[1].as_string()),
        });
    }
    scan.close();
    return rows;
}

table_id_t create_users_table(Catalog& catalog) {
    TableSchema ts;
    ts.name = "users";
    ts.columns = {
        {0, "id", TypeId::INT32, false, ""},
        {1, "name", TypeId::STRING, true, ""},
        {2, "email", TypeId::STRING, false, ""},
    };
    ts.pk_columns = "id";
    auto r = catalog.create_table(default_database_id, std::move(ts));
    EXPECT_TRUE(r.has_value());
    return r ? *r : table_id_t{};
}

// =========================================================================
// AC: SELECT * FROM pg_catalog.pg_database returns the database row
// =========================================================================

TEST(GDB567_PgDatabase, AC_ReturnsSingleRow) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_database());

    auto rows = scan_pg_database(catalog);
    ASSERT_EQ(rows.size(), 1u);
}

TEST(GDB567_PgDatabase, AC_CorrectValues) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_database());

    auto rows = scan_pg_database(catalog);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].oid, 1);
    EXPECT_EQ(rows[0].datname, "sixsevendb");
    EXPECT_EQ(rows[0].datdba, 10);
    EXPECT_EQ(rows[0].encoding, 6);
}

// =========================================================================
// AC: SELECT * FROM pg_catalog.pg_index returns rows for all indexes
// =========================================================================

TEST(GDB567_PgIndex, AC_ReturnsOneRowPerIndex) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    auto users_id = create_users_table(catalog);

    IndexDef idx1;
    idx1.table_id = users_id;
    idx1.name = "idx_email";
    idx1.index_type = "btree";
    idx1.columns = "email";
    idx1.is_unique = true;
    ASSERT_TRUE(catalog.create_index(std::move(idx1)).has_value());

    IndexDef idx2;
    idx2.table_id = users_id;
    idx2.name = "idx_name";
    idx2.index_type = "btree";
    idx2.columns = "name";
    idx2.is_unique = false;
    ASSERT_TRUE(catalog.create_index(std::move(idx2)).has_value());

    catalog.register_virtual_table(make_pg_index(catalog));
    auto rows = scan_pg_index(catalog);
    EXPECT_EQ(rows.size(), 2u);
}

// =========================================================================
// AC: Index columns correctly reference pg_class OIDs
// =========================================================================

TEST(GDB567_PgIndex, AC_IndrelidReferencesPgClassOid) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    auto users_id = create_users_table(catalog);

    IndexDef idx;
    idx.table_id = users_id;
    idx.name = "idx_email";
    idx.index_type = "btree";
    idx.columns = "email";
    idx.is_unique = true;
    ASSERT_TRUE(catalog.create_index(std::move(idx)).has_value());

    catalog.register_virtual_table(make_pg_class(catalog));
    catalog.register_virtual_table(make_pg_index(catalog));

    auto class_rows = scan_pg_class(catalog);
    auto index_rows = scan_pg_index(catalog);

    std::unordered_set<int32_t> class_oids;
    for (const auto& r : class_rows) {
        class_oids.insert(r.oid);
    }

    for (const auto& r : index_rows) {
        EXPECT_TRUE(class_oids.count(r.indrelid))
            << "indrelid=" << r.indrelid << " not in pg_class";
    }
}

// =========================================================================
// AC: indisunique and indisprimary flags
// =========================================================================

TEST(GDB567_PgIndex, AC_IndisuniqueFlagCorrect) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    auto users_id = create_users_table(catalog);

    IndexDef unique_idx;
    unique_idx.table_id = users_id;
    unique_idx.name = "idx_email_uniq";
    unique_idx.index_type = "btree";
    unique_idx.columns = "email";
    unique_idx.is_unique = true;
    ASSERT_TRUE(catalog.create_index(std::move(unique_idx)).has_value());

    IndexDef non_unique_idx;
    non_unique_idx.table_id = users_id;
    non_unique_idx.name = "idx_name_nonuniq";
    non_unique_idx.index_type = "btree";
    non_unique_idx.columns = "name";
    non_unique_idx.is_unique = false;
    ASSERT_TRUE(catalog.create_index(std::move(non_unique_idx)).has_value());

    catalog.register_virtual_table(make_pg_index(catalog));
    auto rows = scan_pg_index(catalog);
    ASSERT_EQ(rows.size(), 2u);

    auto find_by_indkey = [&](const std::string& key) -> const IndexRow* {
        for (const auto& r : rows) {
            if (r.indkey == key)
                return &r;
        }
        return nullptr;
    };

    auto* email_idx = find_by_indkey("3");
    ASSERT_NE(email_idx, nullptr) << "Expected indkey=3 for email (ordinal 2 + 1)";
    EXPECT_TRUE(email_idx->indisunique);

    auto* name_idx = find_by_indkey("2");
    ASSERT_NE(name_idx, nullptr) << "Expected indkey=2 for name (ordinal 1 + 1)";
    EXPECT_FALSE(name_idx->indisunique);
}

TEST(GDB567_PgIndex, AC_IndisprimaryDetectsPK) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    auto users_id = create_users_table(catalog);

    IndexDef pk_idx;
    pk_idx.table_id = users_id;
    pk_idx.name = "pk_users";
    pk_idx.index_type = "btree";
    pk_idx.columns = "id";
    pk_idx.is_unique = true;
    ASSERT_TRUE(catalog.create_index(std::move(pk_idx)).has_value());

    IndexDef non_pk_idx;
    non_pk_idx.table_id = users_id;
    non_pk_idx.name = "idx_email";
    non_pk_idx.index_type = "btree";
    non_pk_idx.columns = "email";
    non_pk_idx.is_unique = true;
    ASSERT_TRUE(catalog.create_index(std::move(non_pk_idx)).has_value());

    catalog.register_virtual_table(make_pg_index(catalog));
    auto rows = scan_pg_index(catalog);
    ASSERT_EQ(rows.size(), 2u);

    auto find_by_indkey = [&](const std::string& key) -> const IndexRow* {
        for (const auto& r : rows) {
            if (r.indkey == key)
                return &r;
        }
        return nullptr;
    };

    auto* pk = find_by_indkey("1");
    ASSERT_NE(pk, nullptr);
    EXPECT_TRUE(pk->indisprimary);

    auto* non_pk = find_by_indkey("3");
    ASSERT_NE(non_pk, nullptr);
    EXPECT_FALSE(non_pk->indisprimary);
}

// =========================================================================
// AC: indnatts is correct
// =========================================================================

TEST(GDB567_PgIndex, AC_IndnattsColumn) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    auto users_id = create_users_table(catalog);

    IndexDef idx;
    idx.table_id = users_id;
    idx.name = "idx_email";
    idx.index_type = "btree";
    idx.columns = "email";
    idx.is_unique = false;
    ASSERT_TRUE(catalog.create_index(std::move(idx)).has_value());

    catalog.register_virtual_table(make_pg_index(catalog));
    auto rows = scan_pg_index(catalog);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].indnatts, 1);
}

// =========================================================================
// Edge case: multi-column index — indkey space-separated, indnatts > 1
// =========================================================================

TEST(GDB567_PgIndex, Edge_MultiColumnIndex) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    auto users_id = create_users_table(catalog);

    IndexDef idx;
    idx.table_id = users_id;
    idx.name = "idx_name_email";
    idx.index_type = "btree";
    idx.columns = "name,email";
    idx.is_unique = true;
    ASSERT_TRUE(catalog.create_index(std::move(idx)).has_value());

    catalog.register_virtual_table(make_pg_index(catalog));
    auto rows = scan_pg_index(catalog);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].indnatts, 2);
    EXPECT_EQ(rows[0].indkey, "2 3");
}

// =========================================================================
// Edge case: composite primary key detection
// =========================================================================

TEST(GDB567_PgIndex, Edge_CompositePrimaryKey) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    TableSchema ts;
    ts.name = "order_items";
    ts.columns = {
        {0, "order_id", TypeId::INT32, false, ""},
        {1, "item_id", TypeId::INT32, false, ""},
        {2, "quantity", TypeId::INT32, true, ""},
    };
    ts.pk_columns = "order_id,item_id";
    auto table_id = catalog.create_table(default_database_id, std::move(ts));
    ASSERT_TRUE(table_id.has_value());

    IndexDef pk_idx;
    pk_idx.table_id = table_id.value();
    pk_idx.name = "pk_order_items";
    pk_idx.index_type = "btree";
    pk_idx.columns = "order_id,item_id";
    pk_idx.is_unique = true;
    ASSERT_TRUE(catalog.create_index(std::move(pk_idx)).has_value());

    catalog.register_virtual_table(make_pg_index(catalog));
    auto rows = scan_pg_index(catalog);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].indisprimary);
    EXPECT_EQ(rows[0].indnatts, 2);
    EXPECT_EQ(rows[0].indkey, "1 2");
}

// =========================================================================
// Edge case: index on last column
// =========================================================================

TEST(GDB567_PgIndex, Edge_IndexOnLastColumn) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    TableSchema ts;
    ts.name = "events";
    ts.columns = {
        {0, "id", TypeId::INT32, false, ""},
        {1, "name", TypeId::STRING, true, ""},
        {2, "ts", TypeId::TIMESTAMP, true, ""},
        {3, "payload", TypeId::STRING, true, ""},
    };
    ts.pk_columns = "id";
    auto table_id = catalog.create_table(default_database_id, std::move(ts));
    ASSERT_TRUE(table_id.has_value());

    IndexDef idx;
    idx.table_id = table_id.value();
    idx.name = "idx_payload";
    idx.index_type = "btree";
    idx.columns = "payload";
    idx.is_unique = false;
    ASSERT_TRUE(catalog.create_index(std::move(idx)).has_value());

    catalog.register_virtual_table(make_pg_index(catalog));
    auto rows = scan_pg_index(catalog);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].indkey, "4");
    EXPECT_EQ(rows[0].indnatts, 1);
}

// =========================================================================
// Edge case: empty catalog returns no pg_index rows
// =========================================================================

TEST(GDB567_PgIndex, Edge_EmptyCatalog) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_index(catalog));

    auto rows = scan_pg_index(catalog);
    EXPECT_TRUE(rows.empty());
}

// =========================================================================
// Edge case: table with no indexes
// =========================================================================

TEST(GDB567_PgIndex, Edge_TableWithNoIndexes) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    create_users_table(catalog);

    catalog.register_virtual_table(make_pg_index(catalog));
    auto rows = scan_pg_index(catalog);
    EXPECT_TRUE(rows.empty());
}

// =========================================================================
// Edge case: index OID uniqueness
// =========================================================================

TEST(GDB567_PgIndex, Edge_IndexOidUniqueness) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    auto users_id = create_users_table(catalog);

    for (int i = 0; i < 10; ++i) {
        IndexDef idx;
        idx.table_id = users_id;
        idx.name = "idx_" + std::to_string(i);
        idx.index_type = "btree";
        idx.columns = (i % 3 == 0) ? "id" : (i % 3 == 1) ? "name" : "email";
        idx.is_unique = (i % 2 == 0);
        ASSERT_TRUE(catalog.create_index(std::move(idx)).has_value());
    }

    catalog.register_virtual_table(make_pg_index(catalog));
    auto rows = scan_pg_index(catalog);
    ASSERT_EQ(rows.size(), 10u);

    std::unordered_set<int32_t> oids;
    for (const auto& r : rows) {
        EXPECT_TRUE(oids.insert(r.indexrelid).second)
            << "Duplicate indexrelid=" << r.indexrelid;
    }
}

// =========================================================================
// Edge case: indexes across multiple tables
// =========================================================================

TEST(GDB567_PgIndex, Edge_IndexesAcrossMultipleTables) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    auto users_id = create_users_table(catalog);

    TableSchema orders;
    orders.name = "orders";
    orders.columns = {
        {0, "order_id", TypeId::INT64, false, ""},
        {1, "user_id", TypeId::INT32, false, ""},
        {2, "amount", TypeId::FLOAT64, true, ""},
    };
    orders.pk_columns = "order_id";
    auto orders_id = catalog.create_table(default_database_id, std::move(orders));
    ASSERT_TRUE(orders_id.has_value());

    IndexDef idx1;
    idx1.table_id = users_id;
    idx1.name = "idx_users_email";
    idx1.index_type = "btree";
    idx1.columns = "email";
    idx1.is_unique = true;
    ASSERT_TRUE(catalog.create_index(std::move(idx1)).has_value());

    IndexDef idx2;
    idx2.table_id = orders_id.value();
    idx2.name = "idx_orders_user_id";
    idx2.index_type = "btree";
    idx2.columns = "user_id";
    idx2.is_unique = false;
    ASSERT_TRUE(catalog.create_index(std::move(idx2)).has_value());

    catalog.register_virtual_table(make_pg_index(catalog));
    auto rows = scan_pg_index(catalog);
    ASSERT_EQ(rows.size(), 2u);

    std::unordered_set<int32_t> indrelids;
    for (const auto& r : rows) {
        indrelids.insert(r.indrelid);
    }
    EXPECT_EQ(indrelids.size(), 2u);
    EXPECT_TRUE(indrelids.count(static_cast<int32_t>(users_id)));
    EXPECT_TRUE(indrelids.count(static_cast<int32_t>(orders_id.value())));
}

// =========================================================================
// Edge case: indisprimary false when index columns differ from pk_columns
// even if overlapping
// =========================================================================

TEST(GDB567_PgIndex, Edge_PartialPKOverlapNotPrimary) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    TableSchema ts;
    ts.name = "composite_pk";
    ts.columns = {
        {0, "a", TypeId::INT32, false, ""},
        {1, "b", TypeId::INT32, false, ""},
        {2, "c", TypeId::INT32, true, ""},
    };
    ts.pk_columns = "a,b";
    auto table_id = catalog.create_table(default_database_id, std::move(ts));
    ASSERT_TRUE(table_id.has_value());

    IndexDef idx;
    idx.table_id = table_id.value();
    idx.name = "idx_a_only";
    idx.index_type = "btree";
    idx.columns = "a";
    idx.is_unique = true;
    ASSERT_TRUE(catalog.create_index(std::move(idx)).has_value());

    catalog.register_virtual_table(make_pg_index(catalog));
    auto rows = scan_pg_index(catalog);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].indisprimary);
}

// =========================================================================
// Edge case: indisprimary false when column order differs from pk_columns
// =========================================================================

TEST(GDB567_PgIndex, Edge_ReversedPKColumnsNotPrimary) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    TableSchema ts;
    ts.name = "ordered_pk";
    ts.columns = {
        {0, "x", TypeId::INT32, false, ""},
        {1, "y", TypeId::INT32, false, ""},
    };
    ts.pk_columns = "x,y";
    auto table_id = catalog.create_table(default_database_id, std::move(ts));
    ASSERT_TRUE(table_id.has_value());

    IndexDef idx;
    idx.table_id = table_id.value();
    idx.name = "idx_y_x";
    idx.index_type = "btree";
    idx.columns = "y,x";
    idx.is_unique = true;
    ASSERT_TRUE(catalog.create_index(std::move(idx)).has_value());

    catalog.register_virtual_table(make_pg_index(catalog));
    auto rows = scan_pg_index(catalog);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].indisprimary)
        << "Reversed column order should not match pk_columns";
}

// =========================================================================
// Edge case: pg_database is static — independent of catalog state
// =========================================================================

TEST(GDB567_PgDatabase, Edge_StaticRegardlessOfCatalogState) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    create_users_table(catalog);
    catalog.register_virtual_table(make_pg_database());

    auto rows = scan_pg_database(catalog);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].oid, 1);
    EXPECT_EQ(rows[0].datname, "sixsevendb");
}

// =========================================================================
// Edge case: pg_database repeated scans
// =========================================================================

TEST(GDB567_PgDatabase, Edge_RepeatedScansConsistent) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_database());

    for (int i = 0; i < 5; ++i) {
        auto rows = scan_pg_database(catalog);
        ASSERT_EQ(rows.size(), 1u) << "iteration=" << i;
        EXPECT_EQ(rows[0].datname, "sixsevendb") << "iteration=" << i;
    }
}

// =========================================================================
// Edge case: pg_database column count
// =========================================================================

TEST(GDB567_PgDatabase, Edge_HasFourColumns) {
    auto def = make_pg_database();
    EXPECT_EQ(def.columns.size(), 4u);
    EXPECT_EQ(def.columns[0].name, "oid");
    EXPECT_EQ(def.columns[1].name, "datname");
    EXPECT_EQ(def.columns[2].name, "datdba");
    EXPECT_EQ(def.columns[3].name, "encoding");
}

// =========================================================================
// Edge case: pg_index column schema
// =========================================================================

TEST(GDB567_PgIndex, Edge_HasCorrectColumnSchema) {
    Catalog catalog;
    init_test_catalog(catalog);
    auto def = make_pg_index(catalog);
    ASSERT_EQ(def.columns.size(), 6u);
    EXPECT_EQ(def.columns[0].name, "indexrelid");
    EXPECT_EQ(def.columns[0].type_id, TypeId::INT32);
    EXPECT_EQ(def.columns[1].name, "indrelid");
    EXPECT_EQ(def.columns[1].type_id, TypeId::INT32);
    EXPECT_EQ(def.columns[2].name, "indnatts");
    EXPECT_EQ(def.columns[2].type_id, TypeId::INT16);
    EXPECT_EQ(def.columns[3].name, "indisunique");
    EXPECT_EQ(def.columns[3].type_id, TypeId::BOOL);
    EXPECT_EQ(def.columns[4].name, "indisprimary");
    EXPECT_EQ(def.columns[4].type_id, TypeId::BOOL);
    EXPECT_EQ(def.columns[5].name, "indkey");
    EXPECT_EQ(def.columns[5].type_id, TypeId::STRING);
}

// =========================================================================
// Edge case: index on single-column table
// =========================================================================

TEST(GDB567_PgIndex, Edge_SingleColumnTable) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    TableSchema ts;
    ts.name = "ids_only";
    ts.columns = {{0, "id", TypeId::INT32, false, ""}};
    ts.pk_columns = "id";
    auto table_id = catalog.create_table(default_database_id, std::move(ts));
    ASSERT_TRUE(table_id.has_value());

    IndexDef idx;
    idx.table_id = table_id.value();
    idx.name = "pk_ids_only";
    idx.index_type = "btree";
    idx.columns = "id";
    idx.is_unique = true;
    ASSERT_TRUE(catalog.create_index(std::move(idx)).has_value());

    catalog.register_virtual_table(make_pg_index(catalog));
    auto rows = scan_pg_index(catalog);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].indkey, "1");
    EXPECT_TRUE(rows[0].indisprimary);
    EXPECT_EQ(rows[0].indnatts, 1);
}

// =========================================================================
// Edge case: pg_index scan reopening
// =========================================================================

TEST(GDB567_PgIndex, Edge_ScanReopenConsistent) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    auto users_id = create_users_table(catalog);

    IndexDef idx;
    idx.table_id = users_id;
    idx.name = "idx_email";
    idx.index_type = "btree";
    idx.columns = "email";
    idx.is_unique = true;
    ASSERT_TRUE(catalog.create_index(std::move(idx)).has_value());

    catalog.register_virtual_table(make_pg_index(catalog));

    auto rows1 = scan_pg_index(catalog);
    auto rows2 = scan_pg_index(catalog);

    ASSERT_EQ(rows1.size(), rows2.size());
    for (size_t i = 0; i < rows1.size(); ++i) {
        EXPECT_EQ(rows1[i].indexrelid, rows2[i].indexrelid);
        EXPECT_EQ(rows1[i].indrelid, rows2[i].indrelid);
        EXPECT_EQ(rows1[i].indkey, rows2[i].indkey);
    }
}

// =========================================================================
// Stress: many indexes across many tables
// =========================================================================

TEST(GDB567_Stress, ManyIndexesManyTables) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    constexpr int num_tables = 50;
    std::vector<table_id_t> table_ids;

    for (int i = 0; i < num_tables; ++i) {
        TableSchema ts;
        ts.name = "tbl_" + std::to_string(i);
        ts.columns = {
            {0, "id", TypeId::INT32, false, ""},
            {1, "data", TypeId::STRING, true, ""},
        };
        ts.pk_columns = "id";
        auto r = catalog.create_table(default_database_id, std::move(ts));
        ASSERT_TRUE(r.has_value());
        table_ids.push_back(r.value());
    }

    int total_indexes = 0;
    for (int i = 0; i < num_tables; ++i) {
        IndexDef idx;
        idx.table_id = table_ids[i];
        idx.name = "idx_" + std::to_string(i) + "_data";
        idx.index_type = "btree";
        idx.columns = "data";
        idx.is_unique = false;
        ASSERT_TRUE(catalog.create_index(std::move(idx)).has_value());
        ++total_indexes;

        if (i % 3 == 0) {
            IndexDef idx2;
            idx2.table_id = table_ids[i];
            idx2.name = "idx_" + std::to_string(i) + "_id";
            idx2.index_type = "btree";
            idx2.columns = "id";
            idx2.is_unique = true;
            ASSERT_TRUE(catalog.create_index(std::move(idx2)).has_value());
            ++total_indexes;
        }
    }

    catalog.register_virtual_table(make_pg_index(catalog));
    auto rows = scan_pg_index(catalog);
    EXPECT_EQ(rows.size(), static_cast<size_t>(total_indexes));

    std::unordered_set<int32_t> oids;
    for (const auto& r : rows) {
        EXPECT_TRUE(oids.insert(r.indexrelid).second);
    }
}

// =========================================================================
// Cross-table: pg_index + pg_class referential integrity
// =========================================================================

TEST(GDB567_CrossTable, PgIndexPgClassIntegrity) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    for (int i = 0; i < 5; ++i) {
        TableSchema ts;
        ts.name = "ref_tbl_" + std::to_string(i);
        ts.columns = {
            {0, "id", TypeId::INT32, false, ""},
            {1, "val", TypeId::STRING, true, ""},
        };
        ts.pk_columns = "id";
        auto r = catalog.create_table(default_database_id, std::move(ts));
        ASSERT_TRUE(r.has_value());

        IndexDef idx;
        idx.table_id = r.value();
        idx.name = "idx_ref_" + std::to_string(i);
        idx.index_type = "btree";
        idx.columns = "val";
        idx.is_unique = false;
        ASSERT_TRUE(catalog.create_index(std::move(idx)).has_value());
    }

    catalog.register_virtual_table(make_pg_class(catalog));
    catalog.register_virtual_table(make_pg_index(catalog));

    auto class_rows = scan_pg_class(catalog);
    auto index_rows = scan_pg_index(catalog);

    ASSERT_EQ(class_rows.size(), 5u);
    ASSERT_EQ(index_rows.size(), 5u);

    std::unordered_set<int32_t> class_oids;
    for (const auto& r : class_rows) {
        class_oids.insert(r.oid);
    }

    for (const auto& r : index_rows) {
        EXPECT_TRUE(class_oids.count(r.indrelid))
            << "indexrelid=" << r.indexrelid << " indrelid=" << r.indrelid
            << " not found in pg_class";
    }
}

// =========================================================================
// Edge case: register_pg_catalog_tables registers all six tables
// =========================================================================

TEST(GDB567_Registration, RegisterAllPgCatalogTables) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_catalog_tables(catalog);

    EXPECT_TRUE(catalog.get_virtual_table("pg_database").has_value());
    EXPECT_TRUE(catalog.get_virtual_table("pg_namespace").has_value());
    EXPECT_TRUE(catalog.get_virtual_table("pg_type").has_value());
    EXPECT_TRUE(catalog.get_virtual_table("pg_class").has_value());
    EXPECT_TRUE(catalog.get_virtual_table("pg_attribute").has_value());
    EXPECT_TRUE(catalog.get_virtual_table("pg_index").has_value());

    auto all = catalog.list_virtual_tables();
    EXPECT_GE(all.size(), 6u);
}

} // namespace
} // namespace sixseven
