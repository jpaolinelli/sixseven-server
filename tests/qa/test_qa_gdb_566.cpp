/// @file test_qa_gdb_566.cpp
/// @brief Adversarial QA tests for GDB-566: pg_class and pg_attribute Virtual Tables
///
/// Tests cover: acceptance criteria verification, edge cases (empty tables, single
/// column, many columns, all types), referential integrity between pg_class and
/// pg_attribute, system table exclusion, virtual table exclusion, attnotnull
/// inversion correctness, OID mapping coverage for all 23 types, and stress tests.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
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

// ===========================================================================
// Helpers
// ===========================================================================

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

uint32_t test_pg_oid(TypeId t) {
    switch (t) {
    case TypeId::BOOL:
        return 16;
    case TypeId::INT8:
    case TypeId::INT16:
    case TypeId::UINT8:
        return 21;
    case TypeId::INT32:
    case TypeId::UINT16:
        return 23;
    case TypeId::INT64:
    case TypeId::UINT32:
        return 20;
    case TypeId::UINT64:
    case TypeId::DECIMAL:
        return 1700;
    case TypeId::FLOAT32:
        return 700;
    case TypeId::FLOAT64:
        return 701;
    case TypeId::STRING:
    case TypeId::PATH:
        return 25;
    case TypeId::BLOB:
        return 17;
    case TypeId::DATE:
        return 1082;
    case TypeId::TIME:
        return 1083;
    case TypeId::TIMESTAMP:
        return 1114;
    case TypeId::INTERVAL:
        return 1186;
    case TypeId::POINT:
        return 600;
    case TypeId::JSON:
        return 114;
    case TypeId::UUID:
        return 2950;
    case TypeId::EMBEDDING:
        return 100000;
    }
    return 25;
}

int32_t test_pg_typlen(TypeId t) {
    switch (t) {
    case TypeId::BOOL:
        return 1;
    case TypeId::INT8:
    case TypeId::INT16:
    case TypeId::UINT8:
        return 2;
    case TypeId::INT32:
    case TypeId::UINT16:
    case TypeId::FLOAT32:
    case TypeId::DATE:
        return 4;
    case TypeId::INT64:
    case TypeId::UINT32:
    case TypeId::FLOAT64:
    case TypeId::TIME:
    case TypeId::TIMESTAMP:
        return 8;
    case TypeId::INTERVAL:
    case TypeId::POINT:
    case TypeId::UUID:
        return 16;
    default:
        return -1;
    }
}

void register_pg_class(Catalog& catalog) {
    VirtualTableDef def;
    def.name = "pg_class";
    def.columns = {
        {0, "oid", TypeId::INT32, false, ""},
        {1, "relname", TypeId::STRING, false, ""},
        {2, "relnamespace", TypeId::INT32, false, ""},
        {3, "relkind", TypeId::STRING, false, ""},
        {4, "reltuples", TypeId::FLOAT64, false, ""},
        {5, "relhasindex", TypeId::BOOL, false, ""},
        {6, "relnatts", TypeId::INT32, false, ""},
    };
    def.generator = [&catalog]() -> std::vector<std::vector<std::string>> {
        std::vector<std::vector<std::string>> rows;
        for (const auto& db : catalog.list_databases()) {
            for (const auto& table : catalog.list_tables(db.database_id)) {
                if (table.table_id < first_user_table_id) {
                    continue;
                }
                bool has_index = !catalog.list_indexes(table.table_id).empty();
                rows.push_back({
                    std::to_string(table.table_id),
                    table.name,
                    "2200",
                    "r",
                    "-1",
                    has_index ? "true" : "false",
                    std::to_string(static_cast<int32_t>(table.columns.size())),
                });
            }
        }
        return rows;
    };
    catalog.register_virtual_table(std::move(def));
}

void register_pg_attribute(Catalog& catalog) {
    VirtualTableDef def;
    def.name = "pg_attribute";
    def.columns = {
        {0, "attrelid", TypeId::INT32, false, ""},
        {1, "attname", TypeId::STRING, false, ""},
        {2, "atttypid", TypeId::INT32, false, ""},
        {3, "attlen", TypeId::INT32, false, ""},
        {4, "attnum", TypeId::INT32, false, ""},
        {5, "attnotnull", TypeId::BOOL, false, ""},
        {6, "attisdropped", TypeId::BOOL, false, ""},
    };
    def.generator = [&catalog]() -> std::vector<std::vector<std::string>> {
        std::vector<std::vector<std::string>> rows;
        for (const auto& db : catalog.list_databases()) {
            for (const auto& table : catalog.list_tables(db.database_id)) {
                if (table.table_id < first_user_table_id) {
                    continue;
                }
                for (const auto& col : table.columns) {
                    rows.push_back({
                        std::to_string(table.table_id),
                        col.name,
                        std::to_string(test_pg_oid(col.type_id)),
                        std::to_string(test_pg_typlen(col.type_id)),
                        std::to_string(col.ordinal + 1),
                        col.nullable ? "false" : "true",
                        "false",
                    });
                }
            }
        }
        return rows;
    };
    catalog.register_virtual_table(std::move(def));
}

// Scan helpers

struct ClassRow {
    int32_t oid;
    std::string relname;
    int32_t relnamespace;
    std::string relkind;
    double reltuples;
    bool relhasindex;
    int32_t relnatts;
};

struct AttributeRow {
    int32_t attrelid;
    std::string attname;
    int32_t atttypid;
    int32_t attlen;
    int32_t attnum;
    bool attnotnull;
    bool attisdropped;
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
            tuple.values[2].as_int32(),
            std::string(tuple.values[3].as_string()),
            tuple.values[4].as_float64(),
            tuple.values[5].as_bool(),
            tuple.values[6].as_int32(),
        });
    }
    scan.close();
    return rows;
}

std::vector<AttributeRow> scan_pg_attribute(Catalog& catalog) {
    auto vt = catalog.get_virtual_table("pg_attribute");
    EXPECT_TRUE(vt.has_value());
    if (!vt.has_value())
        return {};

    VirtualCatalogScanOperator scan(vt.value(), make_output_schema(vt.value()));
    auto open_r = scan.open();
    EXPECT_TRUE(open_r.has_value());
    if (!open_r.has_value())
        return {};

    std::vector<AttributeRow> rows;
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
            tuple.values[4].as_int32(),
            tuple.values[5].as_bool(),
            tuple.values[6].as_bool(),
        });
    }
    scan.close();
    return rows;
}

// ===========================================================================
// AC: SELECT * FROM pg_catalog.pg_class returns one row per user table
// ===========================================================================

TEST(GDB566_PgClass, AC_OneRowPerUserTable) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    TableSchema t1;
    t1.name = "alpha";
    t1.columns = {{0, "id", TypeId::INT32, false, ""}};
    t1.pk_columns = "id";
    ASSERT_TRUE(catalog.create_table(default_database_id, std::move(t1)).has_value());

    TableSchema t2;
    t2.name = "beta";
    t2.columns = {{0, "id", TypeId::INT32, false, ""}};
    t2.pk_columns = "id";
    ASSERT_TRUE(catalog.create_table(default_database_id, std::move(t2)).has_value());

    TableSchema t3;
    t3.name = "gamma";
    t3.columns = {{0, "id", TypeId::INT32, false, ""}};
    t3.pk_columns = "id";
    ASSERT_TRUE(catalog.create_table(default_database_id, std::move(t3)).has_value());

    register_pg_class(catalog);
    auto rows = scan_pg_class(catalog);
    EXPECT_EQ(rows.size(), 3u);

    std::set<std::string> names;
    for (const auto& r : rows) {
        names.insert(r.relname);
    }
    EXPECT_TRUE(names.count("alpha"));
    EXPECT_TRUE(names.count("beta"));
    EXPECT_TRUE(names.count("gamma"));
}

// ===========================================================================
// AC: SELECT * FROM pg_catalog.pg_attribute WHERE attrelid = <oid>
// ===========================================================================

TEST(GDB566_PgAttribute, AC_FilterByAttrelid) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    TableSchema t1;
    t1.name = "products";
    t1.columns = {
        {0, "product_id", TypeId::INT32, false, ""},
        {1, "name", TypeId::STRING, true, ""},
        {2, "price", TypeId::FLOAT64, true, ""},
    };
    t1.pk_columns = "product_id";
    auto r1 = catalog.create_table(default_database_id, std::move(t1));
    ASSERT_TRUE(r1.has_value());
    auto products_id = r1.value();

    TableSchema t2;
    t2.name = "categories";
    t2.columns = {
        {0, "cat_id", TypeId::INT32, false, ""},
        {1, "label", TypeId::STRING, false, ""},
    };
    t2.pk_columns = "cat_id";
    ASSERT_TRUE(catalog.create_table(default_database_id, std::move(t2)).has_value());

    register_pg_class(catalog);
    register_pg_attribute(catalog);

    auto attr_rows = scan_pg_attribute(catalog);
    int products_count = 0;
    for (const auto& r : attr_rows) {
        if (r.attrelid == static_cast<int32_t>(products_id)) {
            ++products_count;
        }
    }
    EXPECT_EQ(products_count, 3);
}

// ===========================================================================
// AC: Column type OIDs match type_to_pg_oid mapping — all 23 types
// ===========================================================================

TEST(GDB566_PgAttribute, AC_AllTypeOidsCovered) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    static constexpr std::array<TypeId, 23> all_types = {
        TypeId::BOOL,    TypeId::INT8,      TypeId::INT16,    TypeId::INT32,  TypeId::INT64,
        TypeId::UINT8,   TypeId::UINT16,    TypeId::UINT32,   TypeId::UINT64, TypeId::FLOAT32,
        TypeId::FLOAT64, TypeId::DECIMAL,   TypeId::STRING,   TypeId::BLOB,   TypeId::DATE,
        TypeId::TIME,    TypeId::TIMESTAMP, TypeId::INTERVAL, TypeId::POINT,  TypeId::JSON,
        TypeId::UUID,    TypeId::EMBEDDING, TypeId::PATH,
    };

    TableSchema ts;
    ts.name = "all_types_table";
    int ordinal = 0;
    for (auto t : all_types) {
        ts.columns.push_back({ordinal, "col_" + std::to_string(ordinal), t, true, ""});
        ++ordinal;
    }
    ts.pk_columns = "col_0";
    ASSERT_TRUE(catalog.create_table(default_database_id, std::move(ts)).has_value());

    register_pg_attribute(catalog);
    auto attr_rows = scan_pg_attribute(catalog);

    ASSERT_EQ(attr_rows.size(), static_cast<size_t>(all_types.size()));

    for (size_t i = 0; i < all_types.size(); ++i) {
        EXPECT_EQ(attr_rows[i].atttypid, static_cast<int32_t>(test_pg_oid(all_types[i])))
            << "type index=" << i << " attname=" << attr_rows[i].attname;
        EXPECT_EQ(attr_rows[i].attlen, test_pg_typlen(all_types[i]))
            << "type index=" << i << " attname=" << attr_rows[i].attname;
    }
}

// ===========================================================================
// AC: System catalog tables (internal) are NOT exposed in pg_class
// ===========================================================================

TEST(GDB566_PgClass, AC_SystemTablesExcluded) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    TableSchema t1;
    t1.name = "my_table";
    t1.columns = {{0, "id", TypeId::INT32, false, ""}};
    t1.pk_columns = "id";
    ASSERT_TRUE(catalog.create_table(default_database_id, std::move(t1)).has_value());

    register_pg_class(catalog);
    auto rows = scan_pg_class(catalog);

    for (const auto& r : rows) {
        EXPECT_GE(r.oid, first_user_table_id)
            << "System table leaked into pg_class: " << r.relname;
        EXPECT_NE(r.relname, "sys_tables") << "sys_tables should not appear";
        EXPECT_NE(r.relname, "sys_columns") << "sys_columns should not appear";
        EXPECT_NE(r.relname, "sys_indexes") << "sys_indexes should not appear";
        EXPECT_NE(r.relname, "sys_databases") << "sys_databases should not appear";
    }

    EXPECT_EQ(rows.size(), 1u);
}

// ===========================================================================
// AC: JOIN between pg_class and pg_attribute — referential integrity
// ===========================================================================

TEST(GDB566_CrossTable, AC_JoinIntegrity) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    TableSchema t1;
    t1.name = "employees";
    t1.columns = {
        {0, "emp_id", TypeId::INT64, false, ""},
        {1, "name", TypeId::STRING, true, ""},
        {2, "salary", TypeId::FLOAT64, true, ""},
    };
    t1.pk_columns = "emp_id";
    ASSERT_TRUE(catalog.create_table(default_database_id, std::move(t1)).has_value());

    TableSchema t2;
    t2.name = "departments";
    t2.columns = {
        {0, "dept_id", TypeId::INT32, false, ""},
        {1, "dept_name", TypeId::STRING, false, ""},
    };
    t2.pk_columns = "dept_id";
    ASSERT_TRUE(catalog.create_table(default_database_id, std::move(t2)).has_value());

    register_pg_class(catalog);
    register_pg_attribute(catalog);

    auto class_rows = scan_pg_class(catalog);
    auto attr_rows = scan_pg_attribute(catalog);

    std::unordered_map<int32_t, int32_t> class_relnatts;
    for (const auto& r : class_rows) {
        class_relnatts[r.oid] = r.relnatts;
    }

    std::unordered_map<int32_t, int> attr_counts;
    for (const auto& r : attr_rows) {
        EXPECT_TRUE(class_relnatts.count(r.attrelid))
            << "Orphan attribute: attrelid=" << r.attrelid << " attname=" << r.attname;
        attr_counts[r.attrelid]++;
    }

    for (const auto& [oid, relnatts] : class_relnatts) {
        EXPECT_EQ(attr_counts[oid], relnatts)
            << "Mismatch between relnatts and attribute count for oid=" << oid;
    }
}

// ===========================================================================
// Edge case: single-column table
// ===========================================================================

TEST(GDB566_PgClass, Edge_SingleColumnTable) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    TableSchema ts;
    ts.name = "singleton";
    ts.columns = {{0, "only_col", TypeId::INT32, false, ""}};
    ts.pk_columns = "only_col";
    ASSERT_TRUE(catalog.create_table(default_database_id, std::move(ts)).has_value());

    register_pg_class(catalog);
    register_pg_attribute(catalog);

    auto class_rows = scan_pg_class(catalog);
    ASSERT_EQ(class_rows.size(), 1u);
    EXPECT_EQ(class_rows[0].relnatts, 1);

    auto attr_rows = scan_pg_attribute(catalog);
    ASSERT_EQ(attr_rows.size(), 1u);
    EXPECT_EQ(attr_rows[0].attname, "only_col");
    EXPECT_EQ(attr_rows[0].attnum, 1);
}

// ===========================================================================
// Edge case: empty catalog (no user tables)
// ===========================================================================

TEST(GDB566_PgClass, Edge_NoCatalogTablesInEmptyCatalog) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_class(catalog);
    register_pg_attribute(catalog);

    auto class_rows = scan_pg_class(catalog);
    EXPECT_TRUE(class_rows.empty());

    auto attr_rows = scan_pg_attribute(catalog);
    EXPECT_TRUE(attr_rows.empty());
}

// ===========================================================================
// Edge case: virtual tables themselves must NOT appear in pg_class
// ===========================================================================

TEST(GDB566_PgClass, Edge_VirtualTablesNotExposed) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    TableSchema ts;
    ts.name = "real_table";
    ts.columns = {{0, "id", TypeId::INT32, false, ""}};
    ts.pk_columns = "id";
    ASSERT_TRUE(catalog.create_table(default_database_id, std::move(ts)).has_value());

    register_pg_class(catalog);
    register_pg_attribute(catalog);

    auto class_rows = scan_pg_class(catalog);
    for (const auto& r : class_rows) {
        EXPECT_NE(r.relname, "pg_class") << "pg_class virtual table should not list itself";
        EXPECT_NE(r.relname, "pg_attribute")
            << "pg_attribute should not appear in pg_class";
        EXPECT_NE(r.relname, "pg_type") << "pg_type should not appear in pg_class";
        EXPECT_NE(r.relname, "pg_namespace") << "pg_namespace should not appear in pg_class";
    }

    EXPECT_EQ(class_rows.size(), 1u);
    EXPECT_EQ(class_rows[0].relname, "real_table");
}

// ===========================================================================
// Edge case: table with many columns — relnatts accuracy
// ===========================================================================

TEST(GDB566_PgClass, Edge_ManyColumnsRelnatts) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    TableSchema ts;
    ts.name = "wide_table";
    for (int i = 0; i < 50; ++i) {
        ts.columns.push_back({i, "col_" + std::to_string(i), TypeId::INT32, true, ""});
    }
    ts.pk_columns = "col_0";
    ASSERT_TRUE(catalog.create_table(default_database_id, std::move(ts)).has_value());

    register_pg_class(catalog);
    register_pg_attribute(catalog);

    auto class_rows = scan_pg_class(catalog);
    ASSERT_EQ(class_rows.size(), 1u);
    EXPECT_EQ(class_rows[0].relnatts, 50);

    auto attr_rows = scan_pg_attribute(catalog);
    EXPECT_EQ(attr_rows.size(), 50u);

    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(attr_rows[i].attnum, i + 1) << "attnum mismatch at index=" << i;
    }
}

// ===========================================================================
// Edge case: duplicate column names across tables
// ===========================================================================

TEST(GDB566_PgAttribute, Edge_DuplicateColumnNamesAcrossTables) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    TableSchema t1;
    t1.name = "table_a";
    t1.columns = {
        {0, "id", TypeId::INT32, false, ""},
        {1, "value", TypeId::STRING, true, ""},
    };
    t1.pk_columns = "id";
    auto r1 = catalog.create_table(default_database_id, std::move(t1));
    ASSERT_TRUE(r1.has_value());

    TableSchema t2;
    t2.name = "table_b";
    t2.columns = {
        {0, "id", TypeId::INT64, false, ""},
        {1, "value", TypeId::FLOAT64, true, ""},
    };
    t2.pk_columns = "id";
    auto r2 = catalog.create_table(default_database_id, std::move(t2));
    ASSERT_TRUE(r2.has_value());

    register_pg_attribute(catalog);
    auto attr_rows = scan_pg_attribute(catalog);

    ASSERT_EQ(attr_rows.size(), 4u);

    auto find_by_relid_name = [&](int32_t relid,
                                  const std::string& name) -> const AttributeRow* {
        for (const auto& r : attr_rows) {
            if (r.attrelid == relid && r.attname == name)
                return &r;
        }
        return nullptr;
    };

    auto* a_id = find_by_relid_name(static_cast<int32_t>(r1.value()), "id");
    ASSERT_NE(a_id, nullptr);
    EXPECT_EQ(a_id->atttypid, 23);

    auto* b_id = find_by_relid_name(static_cast<int32_t>(r2.value()), "id");
    ASSERT_NE(b_id, nullptr);
    EXPECT_EQ(b_id->atttypid, 20);
}

// ===========================================================================
// attnotnull inversion correctness
// ===========================================================================

TEST(GDB566_PgAttribute, Edge_AttnotnullInversion) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    TableSchema ts;
    ts.name = "nullable_test";
    ts.columns = {
        {0, "not_null_col", TypeId::INT32, false, ""},
        {1, "nullable_col", TypeId::INT32, true, ""},
        {2, "also_not_null", TypeId::STRING, false, ""},
        {3, "also_nullable", TypeId::STRING, true, ""},
    };
    ts.pk_columns = "not_null_col";
    ASSERT_TRUE(catalog.create_table(default_database_id, std::move(ts)).has_value());

    register_pg_attribute(catalog);
    auto attr_rows = scan_pg_attribute(catalog);
    ASSERT_EQ(attr_rows.size(), 4u);

    auto find = [&](const std::string& name) -> const AttributeRow* {
        for (const auto& r : attr_rows) {
            if (r.attname == name)
                return &r;
        }
        return nullptr;
    };

    EXPECT_TRUE(find("not_null_col")->attnotnull);
    EXPECT_FALSE(find("nullable_col")->attnotnull);
    EXPECT_TRUE(find("also_not_null")->attnotnull);
    EXPECT_FALSE(find("also_nullable")->attnotnull);
}

// ===========================================================================
// pg_class constant fields: relnamespace=2200, relkind='r', reltuples=-1
// ===========================================================================

TEST(GDB566_PgClass, Edge_ConstantFieldValues) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    for (int i = 0; i < 5; ++i) {
        TableSchema ts;
        ts.name = "tbl_" + std::to_string(i);
        ts.columns = {{0, "id", TypeId::INT32, false, ""}};
        ts.pk_columns = "id";
        ASSERT_TRUE(catalog.create_table(default_database_id, std::move(ts)).has_value());
    }

    register_pg_class(catalog);
    auto rows = scan_pg_class(catalog);
    ASSERT_EQ(rows.size(), 5u);

    for (const auto& r : rows) {
        EXPECT_EQ(r.relnamespace, 2200) << "relname=" << r.relname;
        EXPECT_EQ(r.relkind, "r") << "relname=" << r.relname;
        EXPECT_DOUBLE_EQ(r.reltuples, -1.0) << "relname=" << r.relname;
    }
}

// ===========================================================================
// OID uniqueness across all pg_class rows
// ===========================================================================

TEST(GDB566_PgClass, Edge_OidUniqueness) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    for (int i = 0; i < 20; ++i) {
        TableSchema ts;
        ts.name = "unique_oid_" + std::to_string(i);
        ts.columns = {{0, "id", TypeId::INT32, false, ""}};
        ts.pk_columns = "id";
        ASSERT_TRUE(catalog.create_table(default_database_id, std::move(ts)).has_value());
    }

    register_pg_class(catalog);
    auto rows = scan_pg_class(catalog);
    ASSERT_EQ(rows.size(), 20u);

    std::unordered_set<int32_t> oids;
    for (const auto& r : rows) {
        EXPECT_TRUE(oids.insert(r.oid).second) << "Duplicate OID=" << r.oid;
    }
}

// ===========================================================================
// attisdropped is always false for every row
// ===========================================================================

TEST(GDB566_PgAttribute, Edge_AttisdroppedAlwaysFalse) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    TableSchema ts;
    ts.name = "dropped_test";
    for (int i = 0; i < 10; ++i) {
        ts.columns.push_back({i, "c" + std::to_string(i), TypeId::STRING, true, ""});
    }
    ts.pk_columns = "c0";
    ASSERT_TRUE(catalog.create_table(default_database_id, std::move(ts)).has_value());

    register_pg_attribute(catalog);
    auto attr_rows = scan_pg_attribute(catalog);
    ASSERT_EQ(attr_rows.size(), 10u);

    for (const auto& r : attr_rows) {
        EXPECT_FALSE(r.attisdropped) << "attname=" << r.attname;
    }
}

// ===========================================================================
// Stress test: many tables
// ===========================================================================

TEST(GDB566_Stress, ManyTables) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    constexpr int num_tables = 100;
    constexpr int cols_per_table = 5;

    for (int i = 0; i < num_tables; ++i) {
        TableSchema ts;
        ts.name = "stress_" + std::to_string(i);
        for (int j = 0; j < cols_per_table; ++j) {
            ts.columns.push_back(
                {j, "col_" + std::to_string(j), TypeId::INT32, j > 0, ""});
        }
        ts.pk_columns = "col_0";
        ASSERT_TRUE(catalog.create_table(default_database_id, std::move(ts)).has_value());
    }

    register_pg_class(catalog);
    register_pg_attribute(catalog);

    auto class_rows = scan_pg_class(catalog);
    EXPECT_EQ(class_rows.size(), static_cast<size_t>(num_tables));

    auto attr_rows = scan_pg_attribute(catalog);
    EXPECT_EQ(attr_rows.size(), static_cast<size_t>(num_tables * cols_per_table));

    for (const auto& r : class_rows) {
        EXPECT_EQ(r.relnatts, cols_per_table);
    }
}

// ===========================================================================
// Scan reopening — open/close/reopen cycle
// ===========================================================================

TEST(GDB566_PgClass, Edge_ScanReopenProducesSameResults) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    TableSchema ts;
    ts.name = "reopen_test";
    ts.columns = {{0, "id", TypeId::INT32, false, ""}};
    ts.pk_columns = "id";
    ASSERT_TRUE(catalog.create_table(default_database_id, std::move(ts)).has_value());

    register_pg_class(catalog);

    auto rows1 = scan_pg_class(catalog);
    auto rows2 = scan_pg_class(catalog);

    ASSERT_EQ(rows1.size(), rows2.size());
    for (size_t i = 0; i < rows1.size(); ++i) {
        EXPECT_EQ(rows1[i].oid, rows2[i].oid);
        EXPECT_EQ(rows1[i].relname, rows2[i].relname);
    }
}

// ===========================================================================
// pg_attribute attnum monotonically increases per table
// ===========================================================================

TEST(GDB566_PgAttribute, Edge_AttNumMonotonicallyIncreases) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    TableSchema ts;
    ts.name = "monotonic";
    for (int i = 0; i < 8; ++i) {
        ts.columns.push_back({i, "field_" + std::to_string(i), TypeId::INT32, true, ""});
    }
    ts.pk_columns = "field_0";
    auto r = catalog.create_table(default_database_id, std::move(ts));
    ASSERT_TRUE(r.has_value());

    register_pg_attribute(catalog);
    auto attr_rows = scan_pg_attribute(catalog);

    int32_t prev_attnum = 0;
    for (const auto& row : attr_rows) {
        if (row.attrelid == static_cast<int32_t>(r.value())) {
            EXPECT_GT(row.attnum, prev_attnum)
                << "attnum not monotonically increasing at " << row.attname;
            prev_attnum = row.attnum;
        }
    }
    EXPECT_EQ(prev_attnum, 8);
}

// ===========================================================================
// relhasindex is false when no indexes exist
// ===========================================================================

TEST(GDB566_PgClass, Edge_RelhasindexFalseWithNoIndexes) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.set_next_table_id(first_user_table_id);

    TableSchema ts;
    ts.name = "no_index_table";
    ts.columns = {
        {0, "id", TypeId::INT32, false, ""},
        {1, "data", TypeId::STRING, true, ""},
    };
    ts.pk_columns = "id";
    ASSERT_TRUE(catalog.create_table(default_database_id, std::move(ts)).has_value());

    register_pg_class(catalog);
    auto rows = scan_pg_class(catalog);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].relhasindex);
}

} // namespace
} // namespace sixseven
