#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/executor/pg_catalog_tables.h"
#include "sixseven/executor/virtual_catalog_scan.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

// =========================================================================
// Helper: register pg_type with production-equivalent generator
// =========================================================================

void register_pg_type(Catalog& catalog) {
    catalog.register_virtual_table(make_pg_type());
}

void register_pg_namespace(Catalog& catalog) {
    catalog.register_virtual_table(make_pg_namespace());
}

struct TypeRow {
    int32_t oid;
    std::string typname;
    int32_t typnamespace;
    int32_t typlen;
    std::string typtype;
    int32_t typelem;
    int32_t typrelid;
    int32_t typbasetype;
};

std::vector<TypeRow> scan_pg_type(Catalog& catalog) {
    auto vt = catalog.get_virtual_table("pg_type");
    EXPECT_TRUE(vt.has_value());
    if (!vt.has_value())
        return {};

    OutputSchema schema(std::vector<OutputColumn>{
        {"pg_type", "oid", TypeId::INT32, false, vt->table_id},
        {"pg_type", "typname", TypeId::STRING, false, vt->table_id},
        {"pg_type", "typnamespace", TypeId::INT32, false, vt->table_id},
        {"pg_type", "typlen", TypeId::INT32, false, vt->table_id},
        {"pg_type", "typtype", TypeId::STRING, false, vt->table_id},
        {"pg_type", "typelem", TypeId::INT32, false, vt->table_id},
        {"pg_type", "typrelid", TypeId::INT32, false, vt->table_id},
        {"pg_type", "typbasetype", TypeId::INT32, false, vt->table_id},
    });

    VirtualCatalogScanOperator scan(std::move(*vt), std::move(schema));
    auto open_r = scan.open();
    EXPECT_TRUE(open_r.has_value());
    if (!open_r.has_value())
        return {};

    std::vector<TypeRow> rows;
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
            std::string(tuple.values[4].as_string()),
            tuple.values[5].as_int32(),
            tuple.values[6].as_int32(),
            tuple.values[7].as_int32(),
        });
    }
    scan.close();
    return rows;
}

// =========================================================================
// pg_type tests
// =========================================================================

TEST(PgType, ReturnsAllSixSevenTypes) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_type(catalog);

    auto rows = scan_pg_type(catalog);
    EXPECT_EQ(rows.size(), 23u);
}

TEST(PgType, EmbeddingTypeHasOid100000) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_type(catalog);

    auto rows = scan_pg_type(catalog);
    auto it = std::find_if(
        rows.begin(), rows.end(), [](const TypeRow& r) { return r.typname == "embedding"; });
    ASSERT_NE(it, rows.end());
    EXPECT_EQ(it->oid, 100000);
}

TEST(PgType, OidsMatchExpectedMapping) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_type(catalog);

    auto rows = scan_pg_type(catalog);

    std::unordered_map<std::string, std::vector<int32_t>> name_to_oids;
    for (const auto& r : rows) {
        name_to_oids[r.typname].push_back(r.oid);
    }

    EXPECT_EQ(name_to_oids["bool"].size(), 1u);
    EXPECT_EQ(name_to_oids["bool"][0], 16);

    EXPECT_EQ(name_to_oids["int4"].size(), 2u);
    for (auto oid : name_to_oids["int4"]) {
        EXPECT_EQ(oid, 23);
    }

    EXPECT_EQ(name_to_oids["text"].size(), 2u);
    for (auto oid : name_to_oids["text"]) {
        EXPECT_EQ(oid, 25);
    }
}

TEST(PgType, AllRowsHaveNamespace11) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_type(catalog);

    auto rows = scan_pg_type(catalog);
    for (const auto& r : rows) {
        EXPECT_EQ(r.typnamespace, 11) << "typname=" << r.typname;
    }
}

TEST(PgType, AllRowsHaveBaseTypeB) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_type(catalog);

    auto rows = scan_pg_type(catalog);
    for (const auto& r : rows) {
        EXPECT_EQ(r.typtype, "b") << "typname=" << r.typname;
        EXPECT_EQ(r.typelem, 0) << "typname=" << r.typname;
        EXPECT_EQ(r.typrelid, 0) << "typname=" << r.typname;
        EXPECT_EQ(r.typbasetype, 0) << "typname=" << r.typname;
    }
}

TEST(PgType, VariableLengthTypesHaveNegativeTyplen) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_type(catalog);

    auto rows = scan_pg_type(catalog);
    std::unordered_set<std::string> variable_types = {
        "text",
        "bytea",
        "json",
        "numeric",
        "embedding",
    };

    for (const auto& r : rows) {
        if (variable_types.count(r.typname)) {
            EXPECT_EQ(r.typlen, -1) << "typname=" << r.typname;
        } else {
            EXPECT_GT(r.typlen, 0) << "typname=" << r.typname;
        }
    }
}

TEST(PgType, SpecificTypeLengths) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_type(catalog);

    auto rows = scan_pg_type(catalog);

    auto find = [&](const std::string& name) -> const TypeRow* {
        for (const auto& r : rows) {
            if (r.typname == name)
                return &r;
        }
        return nullptr;
    };

    auto* r = find("bool");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->typlen, 1);

    r = find("int4");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->typlen, 4);

    r = find("float8");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->typlen, 8);

    r = find("uuid");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->typlen, 16);
}

// =========================================================================
// pg_namespace tests
// =========================================================================

TEST(PgNamespace, ReturnsTwoNamespaces) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_namespace(catalog);

    auto vt = catalog.get_virtual_table("pg_namespace");
    ASSERT_TRUE(vt.has_value());

    OutputSchema schema(std::vector<OutputColumn>{
        {"pg_namespace", "oid", TypeId::INT32, false, vt->table_id},
        {"pg_namespace", "nspname", TypeId::STRING, false, vt->table_id},
        {"pg_namespace", "nspowner", TypeId::INT32, false, vt->table_id},
    });

    VirtualCatalogScanOperator scan(std::move(*vt), std::move(schema));
    auto open_r = scan.open();
    ASSERT_TRUE(open_r.has_value());

    std::vector<std::tuple<int32_t, std::string, int32_t>> rows;
    while (true) {
        auto next_r = scan.next();
        ASSERT_TRUE(next_r.has_value());
        if (!next_r->has_value())
            break;
        auto& tuple = next_r->value();
        rows.emplace_back(tuple.values[0].as_int32(),
                          std::string(tuple.values[1].as_string()),
                          tuple.values[2].as_int32());
    }
    scan.close();

    ASSERT_EQ(rows.size(), 2u);

    EXPECT_EQ(std::get<0>(rows[0]), 11);
    EXPECT_EQ(std::get<1>(rows[0]), "pg_catalog");
    EXPECT_EQ(std::get<2>(rows[0]), 10);

    EXPECT_EQ(std::get<0>(rows[1]), 2200);
    EXPECT_EQ(std::get<1>(rows[1]), "public");
    EXPECT_EQ(std::get<2>(rows[1]), 10);
}

TEST(PgNamespace, LookupSucceeds) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_namespace(catalog);

    auto vt = catalog.get_virtual_table("pg_namespace");
    ASSERT_TRUE(vt.has_value());
    EXPECT_EQ(vt->name, "pg_namespace");
    EXPECT_EQ(vt->columns.size(), 3u);
}

// =========================================================================
// Helper: pg_oid / pg_typlen lambdas (matching production)
// =========================================================================

// =========================================================================
// Helper: register pg_class and pg_attribute
// =========================================================================

void register_pg_class(Catalog& catalog) {
    catalog.register_virtual_table(make_pg_class(catalog));
}

void register_pg_attribute(Catalog& catalog) {
    catalog.register_virtual_table(make_pg_attribute(catalog));
}

// =========================================================================
// Helper: scan helpers for pg_class and pg_attribute
// =========================================================================

struct ClassRow {
    int32_t oid;
    std::string relname;
    int32_t relnamespace;
    std::string relkind;
    double reltuples;
    bool relhasindex;
    int32_t relnatts;
};

std::vector<ClassRow> scan_pg_class(Catalog& catalog) {
    auto vt = catalog.get_virtual_table("pg_class");
    EXPECT_TRUE(vt.has_value());
    if (!vt.has_value())
        return {};

    OutputSchema schema(std::vector<OutputColumn>{
        {"pg_class", "oid", TypeId::INT32, false, vt->table_id},
        {"pg_class", "relname", TypeId::STRING, false, vt->table_id},
        {"pg_class", "relnamespace", TypeId::INT32, false, vt->table_id},
        {"pg_class", "relkind", TypeId::STRING, false, vt->table_id},
        {"pg_class", "reltuples", TypeId::FLOAT64, false, vt->table_id},
        {"pg_class", "relhasindex", TypeId::BOOL, false, vt->table_id},
        {"pg_class", "relnatts", TypeId::INT32, false, vt->table_id},
    });

    VirtualCatalogScanOperator scan(std::move(*vt), std::move(schema));
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

struct AttributeRow {
    int32_t attrelid;
    std::string attname;
    int32_t atttypid;
    int32_t attlen;
    int32_t attnum;
    bool attnotnull;
    bool attisdropped;
};

std::vector<AttributeRow> scan_pg_attribute(Catalog& catalog) {
    auto vt = catalog.get_virtual_table("pg_attribute");
    EXPECT_TRUE(vt.has_value());
    if (!vt.has_value())
        return {};

    OutputSchema schema(std::vector<OutputColumn>{
        {"pg_attribute", "attrelid", TypeId::INT32, false, vt->table_id},
        {"pg_attribute", "attname", TypeId::STRING, false, vt->table_id},
        {"pg_attribute", "atttypid", TypeId::INT32, false, vt->table_id},
        {"pg_attribute", "attlen", TypeId::INT32, false, vt->table_id},
        {"pg_attribute", "attnum", TypeId::INT32, false, vt->table_id},
        {"pg_attribute", "attnotnull", TypeId::BOOL, false, vt->table_id},
        {"pg_attribute", "attisdropped", TypeId::BOOL, false, vt->table_id},
    });

    VirtualCatalogScanOperator scan(std::move(*vt), std::move(schema));
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

// Helper: create a test catalog with user tables for pg_class/pg_attribute tests.
void create_test_tables(Catalog& catalog) {
    catalog.set_next_table_id(first_user_table_id);

    TableSchema users;
    users.name = "users";
    users.columns = {
        {0, "id", TypeId::INT32, false, ""},
        {1, "name", TypeId::STRING, true, ""},
        {2, "email", TypeId::STRING, false, ""},
    };
    users.pk_columns = "id";
    auto r1 = catalog.create_table(default_database_id, std::move(users));
    ASSERT_TRUE(r1.has_value());

    TableSchema orders;
    orders.name = "orders";
    orders.columns = {
        {0, "order_id", TypeId::INT64, false, ""},
        {1, "user_id", TypeId::INT32, false, ""},
        {2, "amount", TypeId::FLOAT64, true, ""},
        {3, "created_at", TypeId::TIMESTAMP, true, ""},
    };
    orders.pk_columns = "order_id";
    auto r2 = catalog.create_table(default_database_id, std::move(orders));
    ASSERT_TRUE(r2.has_value());
}

// =========================================================================
// pg_class tests
// =========================================================================

TEST(PgClass, ReturnsOneRowPerUserTable) {
    Catalog catalog;
    init_test_catalog(catalog);
    create_test_tables(catalog);
    register_pg_class(catalog);

    auto rows = scan_pg_class(catalog);
    EXPECT_EQ(rows.size(), 2u);
}

TEST(PgClass, ExcludesSystemTables) {
    Catalog catalog;
    init_test_catalog(catalog);
    create_test_tables(catalog);
    register_pg_class(catalog);

    auto rows = scan_pg_class(catalog);
    for (const auto& r : rows) {
        EXPECT_GE(r.oid, first_user_table_id) << "relname=" << r.relname;
    }
}

TEST(PgClass, ColumnsHaveCorrectValues) {
    Catalog catalog;
    init_test_catalog(catalog);
    create_test_tables(catalog);
    register_pg_class(catalog);

    auto rows = scan_pg_class(catalog);
    ASSERT_EQ(rows.size(), 2u);

    auto find = [&](const std::string& name) -> const ClassRow* {
        for (const auto& r : rows) {
            if (r.relname == name)
                return &r;
        }
        return nullptr;
    };

    auto* users = find("users");
    ASSERT_NE(users, nullptr);
    EXPECT_EQ(users->relnamespace, 2200);
    EXPECT_EQ(users->relkind, "r");
    EXPECT_EQ(users->reltuples, -1.0);
    EXPECT_EQ(users->relnatts, 3);

    auto* orders = find("orders");
    ASSERT_NE(orders, nullptr);
    EXPECT_EQ(orders->relnamespace, 2200);
    EXPECT_EQ(orders->relkind, "r");
    EXPECT_EQ(orders->relnatts, 4);
}

TEST(PgClass, OidsAreDerivedFromTableId) {
    Catalog catalog;
    init_test_catalog(catalog);
    create_test_tables(catalog);
    register_pg_class(catalog);

    auto rows = scan_pg_class(catalog);
    std::unordered_set<int32_t> oids;
    for (const auto& r : rows) {
        EXPECT_TRUE(oids.insert(r.oid).second) << "duplicate oid=" << r.oid;
    }
}

TEST(PgClass, EmptyCatalogReturnsNoRows) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_class(catalog);

    auto rows = scan_pg_class(catalog);
    EXPECT_TRUE(rows.empty());
}

// =========================================================================
// pg_attribute tests
// =========================================================================

TEST(PgAttribute, ReturnsOneRowPerColumnPerTable) {
    Catalog catalog;
    init_test_catalog(catalog);
    create_test_tables(catalog);
    register_pg_attribute(catalog);

    auto rows = scan_pg_attribute(catalog);
    EXPECT_EQ(rows.size(), 7u);
}

TEST(PgAttribute, AttrelidMatchesPgClassOid) {
    Catalog catalog;
    init_test_catalog(catalog);
    create_test_tables(catalog);
    register_pg_class(catalog);
    register_pg_attribute(catalog);

    auto class_rows = scan_pg_class(catalog);
    auto attr_rows = scan_pg_attribute(catalog);

    std::unordered_set<int32_t> class_oids;
    for (const auto& r : class_rows) {
        class_oids.insert(r.oid);
    }

    for (const auto& r : attr_rows) {
        EXPECT_TRUE(class_oids.count(r.attrelid))
            << "attrelid=" << r.attrelid << " attname=" << r.attname;
    }
}

TEST(PgAttribute, AtttypidUsesTypeToOidMapping) {
    Catalog catalog;
    init_test_catalog(catalog);
    create_test_tables(catalog);
    register_pg_attribute(catalog);

    auto rows = scan_pg_attribute(catalog);

    auto find = [&](const std::string& name) -> const AttributeRow* {
        for (const auto& r : rows) {
            if (r.attname == name)
                return &r;
        }
        return nullptr;
    };

    auto* id_col = find("id");
    ASSERT_NE(id_col, nullptr);
    EXPECT_EQ(id_col->atttypid, 23);

    auto* name_col = find("name");
    ASSERT_NE(name_col, nullptr);
    EXPECT_EQ(name_col->atttypid, 25);

    auto* order_id_col = find("order_id");
    ASSERT_NE(order_id_col, nullptr);
    EXPECT_EQ(order_id_col->atttypid, 20);

    auto* amount_col = find("amount");
    ASSERT_NE(amount_col, nullptr);
    EXPECT_EQ(amount_col->atttypid, 701);

    auto* created_col = find("created_at");
    ASSERT_NE(created_col, nullptr);
    EXPECT_EQ(created_col->atttypid, 1114);
}

TEST(PgAttribute, AttlenMatchesTypeLength) {
    Catalog catalog;
    init_test_catalog(catalog);
    create_test_tables(catalog);
    register_pg_attribute(catalog);

    auto rows = scan_pg_attribute(catalog);

    auto find = [&](const std::string& name) -> const AttributeRow* {
        for (const auto& r : rows) {
            if (r.attname == name)
                return &r;
        }
        return nullptr;
    };

    auto* id_col = find("id");
    ASSERT_NE(id_col, nullptr);
    EXPECT_EQ(id_col->attlen, 4);

    auto* name_col = find("name");
    ASSERT_NE(name_col, nullptr);
    EXPECT_EQ(name_col->attlen, -1);

    auto* order_id_col = find("order_id");
    ASSERT_NE(order_id_col, nullptr);
    EXPECT_EQ(order_id_col->attlen, 8);
}

TEST(PgAttribute, AttNumIsOneBased) {
    Catalog catalog;
    init_test_catalog(catalog);
    create_test_tables(catalog);
    register_pg_attribute(catalog);

    auto rows = scan_pg_attribute(catalog);

    auto find = [&](const std::string& name) -> const AttributeRow* {
        for (const auto& r : rows) {
            if (r.attname == name)
                return &r;
        }
        return nullptr;
    };

    EXPECT_EQ(find("id")->attnum, 1);
    EXPECT_EQ(find("name")->attnum, 2);
    EXPECT_EQ(find("email")->attnum, 3);

    EXPECT_EQ(find("order_id")->attnum, 1);
    EXPECT_EQ(find("user_id")->attnum, 2);
    EXPECT_EQ(find("amount")->attnum, 3);
    EXPECT_EQ(find("created_at")->attnum, 4);
}

TEST(PgAttribute, AttnotnullReflectsNullability) {
    Catalog catalog;
    init_test_catalog(catalog);
    create_test_tables(catalog);
    register_pg_attribute(catalog);

    auto rows = scan_pg_attribute(catalog);

    auto find = [&](const std::string& name) -> const AttributeRow* {
        for (const auto& r : rows) {
            if (r.attname == name)
                return &r;
        }
        return nullptr;
    };

    EXPECT_TRUE(find("id")->attnotnull);
    EXPECT_FALSE(find("name")->attnotnull);
    EXPECT_TRUE(find("email")->attnotnull);
    EXPECT_TRUE(find("order_id")->attnotnull);
    EXPECT_FALSE(find("amount")->attnotnull);
}

TEST(PgAttribute, AttisdroppedAlwaysFalse) {
    Catalog catalog;
    init_test_catalog(catalog);
    create_test_tables(catalog);
    register_pg_attribute(catalog);

    auto rows = scan_pg_attribute(catalog);
    for (const auto& r : rows) {
        EXPECT_FALSE(r.attisdropped) << "attname=" << r.attname;
    }
}

TEST(PgAttribute, FilterByAttrelid) {
    Catalog catalog;
    init_test_catalog(catalog);
    create_test_tables(catalog);
    register_pg_class(catalog);
    register_pg_attribute(catalog);

    auto class_rows = scan_pg_class(catalog);
    auto attr_rows = scan_pg_attribute(catalog);

    auto find_class = [&](const std::string& name) -> const ClassRow* {
        for (const auto& r : class_rows) {
            if (r.relname == name)
                return &r;
        }
        return nullptr;
    };

    auto* users_class = find_class("users");
    ASSERT_NE(users_class, nullptr);

    int count = 0;
    for (const auto& r : attr_rows) {
        if (r.attrelid == users_class->oid) {
            ++count;
        }
    }
    EXPECT_EQ(count, 3);
}

// =========================================================================
// Helper: register pg_index and pg_database
// =========================================================================

void register_pg_index(Catalog& catalog) {
    catalog.register_virtual_table(make_pg_index(catalog));
}

void register_pg_database(Catalog& catalog) {
    catalog.register_virtual_table(make_pg_database(catalog));
}

// =========================================================================
// Helper: scan helpers for pg_index and pg_database
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

    OutputSchema schema(std::vector<OutputColumn>{
        {"pg_index", "indexrelid", TypeId::INT32, false, vt->table_id},
        {"pg_index", "indrelid", TypeId::INT32, false, vt->table_id},
        {"pg_index", "indnatts", TypeId::INT16, false, vt->table_id},
        {"pg_index", "indisunique", TypeId::BOOL, false, vt->table_id},
        {"pg_index", "indisprimary", TypeId::BOOL, false, vt->table_id},
        {"pg_index", "indkey", TypeId::STRING, false, vt->table_id},
    });

    VirtualCatalogScanOperator scan(std::move(*vt), std::move(schema));
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

    OutputSchema schema(std::vector<OutputColumn>{
        {"pg_database", "oid", TypeId::INT32, false, vt->table_id},
        {"pg_database", "datname", TypeId::STRING, false, vt->table_id},
        {"pg_database", "datdba", TypeId::INT32, false, vt->table_id},
        {"pg_database", "encoding", TypeId::INT32, false, vt->table_id},
    });

    VirtualCatalogScanOperator scan(std::move(*vt), std::move(schema));
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

// =========================================================================
// pg_database tests
// =========================================================================

TEST(PgDatabase, ReturnsOneRowPerCatalogDatabase) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_database(catalog);

    // The Catalog ctor creates system_database_id="sixseven_system" and
    // init_test_catalog adds default_database_id="demo" -> 2 databases total.
    auto rows = scan_pg_database(catalog);
    EXPECT_EQ(rows.size(), catalog.list_databases().size());
}

TEST(PgDatabase, DefaultDatabaseRowHasCorrectValues) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_database(catalog);

    auto rows = scan_pg_database(catalog);

    // Find the default ("demo") database row.
    const DatabaseRow* demo_row = nullptr;
    for (const auto& r : rows) {
        if (r.datname == default_database_name) {
            demo_row = &r;
            break;
        }
    }
    ASSERT_NE(demo_row, nullptr) << "Expected a row for the default database";
    EXPECT_EQ(demo_row->oid, default_database_id);
    EXPECT_EQ(demo_row->datdba, 10);
    EXPECT_EQ(demo_row->encoding, 6);
}

TEST(PgDatabase, NewDatabaseAppearsInPgDatabase) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_database(catalog);

    // Record baseline count (system db + default db).
    auto baseline_count = catalog.list_databases().size();

    // Add another database and verify it appears in pg_database output.
    auto create_r = catalog.create_database("testdb_extra");
    ASSERT_TRUE(create_r.has_value()) << create_r.error().message;
    database_id_t new_id = *create_r;

    // The generator captures catalog by reference, so calling it after the new
    // database is created must return one additional row.
    auto vt = catalog.get_virtual_table("pg_database");
    ASSERT_TRUE(vt.has_value());
    auto raw_rows = vt->generator();

    EXPECT_EQ(raw_rows.size(), baseline_count + 1u);

    bool found_default = false;
    bool found_new = false;
    for (const auto& row : raw_rows) {
        if (row[0] == std::to_string(default_database_id) && row[1] == default_database_name) {
            found_default = true;
        }
        if (row[0] == std::to_string(new_id) && row[1] == "testdb_extra") {
            found_new = true;
        }
    }
    EXPECT_TRUE(found_default) << "Default database row must still be present";
    EXPECT_TRUE(found_new) << "Newly created database must appear in pg_database";
}

// =========================================================================
// pg_index tests
// =========================================================================

void create_test_indexes(Catalog& catalog, table_id_t users_table_id) {
    IndexDef idx;
    idx.table_id = users_table_id;
    idx.name = "idx_users_email";
    idx.index_type = "btree";
    idx.columns = "email";
    idx.is_unique = true;
    auto r = catalog.create_index(std::move(idx));
    ASSERT_TRUE(r.has_value());

    IndexDef idx2;
    idx2.table_id = users_table_id;
    idx2.name = "idx_users_name";
    idx2.index_type = "btree";
    idx2.columns = "name";
    idx2.is_unique = false;
    auto r2 = catalog.create_index(std::move(idx2));
    ASSERT_TRUE(r2.has_value());
}

TEST(PgIndex, ReturnsOneRowPerIndex) {
    Catalog catalog;
    init_test_catalog(catalog);
    create_test_tables(catalog);

    auto tables = catalog.list_tables(default_database_id);
    auto users_it =
        std::find_if(tables.begin(), tables.end(), [](const auto& t) { return t.name == "users"; });
    ASSERT_NE(users_it, tables.end());

    create_test_indexes(catalog, users_it->table_id);
    register_pg_index(catalog);

    auto rows = scan_pg_index(catalog);
    EXPECT_EQ(rows.size(), 2u);
}

TEST(PgIndex, IndrelidReferencesPgClassOid) {
    Catalog catalog;
    init_test_catalog(catalog);
    create_test_tables(catalog);

    auto tables = catalog.list_tables(default_database_id);
    auto users_it =
        std::find_if(tables.begin(), tables.end(), [](const auto& t) { return t.name == "users"; });
    ASSERT_NE(users_it, tables.end());

    create_test_indexes(catalog, users_it->table_id);
    register_pg_class(catalog);
    register_pg_index(catalog);

    auto class_rows = scan_pg_class(catalog);
    auto index_rows = scan_pg_index(catalog);

    std::unordered_set<int32_t> class_oids;
    for (const auto& r : class_rows) {
        class_oids.insert(r.oid);
    }

    for (const auto& r : index_rows) {
        EXPECT_TRUE(class_oids.count(r.indrelid))
            << "indrelid=" << r.indrelid << " not found in pg_class";
    }
}

TEST(PgIndex, IndisuniqueFlagIsCorrect) {
    Catalog catalog;
    init_test_catalog(catalog);
    create_test_tables(catalog);

    auto tables = catalog.list_tables(default_database_id);
    auto users_it =
        std::find_if(tables.begin(), tables.end(), [](const auto& t) { return t.name == "users"; });
    ASSERT_NE(users_it, tables.end());

    create_test_indexes(catalog, users_it->table_id);
    register_pg_index(catalog);

    auto rows = scan_pg_index(catalog);
    ASSERT_EQ(rows.size(), 2u);

    auto find = [&](const std::string& key) -> const IndexRow* {
        for (const auto& r : rows) {
            if (r.indkey == key)
                return &r;
        }
        return nullptr;
    };

    auto* email_idx = find("3");
    ASSERT_NE(email_idx, nullptr);
    EXPECT_TRUE(email_idx->indisunique);

    auto* name_idx = find("2");
    ASSERT_NE(name_idx, nullptr);
    EXPECT_FALSE(name_idx->indisunique);
}

TEST(PgIndex, IndkeyContainsColumnPositions) {
    Catalog catalog;
    init_test_catalog(catalog);
    create_test_tables(catalog);

    auto tables = catalog.list_tables(default_database_id);
    auto users_it =
        std::find_if(tables.begin(), tables.end(), [](const auto& t) { return t.name == "users"; });
    ASSERT_NE(users_it, tables.end());

    create_test_indexes(catalog, users_it->table_id);
    register_pg_index(catalog);

    auto rows = scan_pg_index(catalog);
    for (const auto& r : rows) {
        EXPECT_FALSE(r.indkey.empty());
        EXPECT_EQ(r.indnatts, 1);
    }
}

TEST(PgIndex, IndisprimaryDetectsPrimaryKey) {
    Catalog catalog;
    init_test_catalog(catalog);
    create_test_tables(catalog);

    auto tables = catalog.list_tables(default_database_id);
    auto users_it =
        std::find_if(tables.begin(), tables.end(), [](const auto& t) { return t.name == "users"; });
    ASSERT_NE(users_it, tables.end());

    IndexDef pk_idx;
    pk_idx.table_id = users_it->table_id;
    pk_idx.name = "pk_users";
    pk_idx.index_type = "btree";
    pk_idx.columns = "id";
    pk_idx.is_unique = true;
    auto r = catalog.create_index(std::move(pk_idx));
    ASSERT_TRUE(r.has_value());

    IndexDef non_pk_idx;
    non_pk_idx.table_id = users_it->table_id;
    non_pk_idx.name = "idx_users_email";
    non_pk_idx.index_type = "btree";
    non_pk_idx.columns = "email";
    non_pk_idx.is_unique = true;
    auto r2 = catalog.create_index(std::move(non_pk_idx));
    ASSERT_TRUE(r2.has_value());

    register_pg_index(catalog);

    auto rows = scan_pg_index(catalog);
    ASSERT_EQ(rows.size(), 2u);

    auto find_by_key = [&](const std::string& key) -> const IndexRow* {
        for (const auto& r : rows) {
            if (r.indkey == key)
                return &r;
        }
        return nullptr;
    };

    auto* pk = find_by_key("1");
    ASSERT_NE(pk, nullptr);
    EXPECT_TRUE(pk->indisprimary);

    auto* non_pk = find_by_key("3");
    ASSERT_NE(non_pk, nullptr);
    EXPECT_FALSE(non_pk->indisprimary);
}

TEST(PgIndex, EmptyCatalogReturnsNoRows) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_index(catalog);

    auto rows = scan_pg_index(catalog);
    EXPECT_TRUE(rows.empty());
}

} // namespace
} // namespace sixseven
