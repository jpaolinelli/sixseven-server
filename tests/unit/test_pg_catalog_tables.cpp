#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
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
    static constexpr std::array<TypeId, 23> all_types = {
        TypeId::BOOL,      TypeId::INT8,      TypeId::INT16,     TypeId::INT32,
        TypeId::INT64,     TypeId::UINT8,     TypeId::UINT16,    TypeId::UINT32,
        TypeId::UINT64,    TypeId::FLOAT32,   TypeId::FLOAT64,   TypeId::DECIMAL,
        TypeId::STRING,    TypeId::BLOB,      TypeId::DATE,      TypeId::TIME,
        TypeId::TIMESTAMP, TypeId::INTERVAL,  TypeId::POINT,     TypeId::JSON,
        TypeId::UUID,      TypeId::EMBEDDING, TypeId::PATH,
    };

    auto pg_typname = [](TypeId t) -> std::string {
        switch (t) {
        case TypeId::BOOL:      return "bool";
        case TypeId::INT8:      return "int2";
        case TypeId::INT16:     return "int2";
        case TypeId::INT32:     return "int4";
        case TypeId::INT64:     return "int8";
        case TypeId::UINT8:     return "int2";
        case TypeId::UINT16:    return "int4";
        case TypeId::UINT32:    return "int8";
        case TypeId::UINT64:    return "numeric";
        case TypeId::FLOAT32:   return "float4";
        case TypeId::FLOAT64:   return "float8";
        case TypeId::DECIMAL:   return "numeric";
        case TypeId::STRING:    return "text";
        case TypeId::BLOB:      return "bytea";
        case TypeId::DATE:      return "date";
        case TypeId::TIME:      return "time";
        case TypeId::TIMESTAMP: return "timestamp";
        case TypeId::INTERVAL:  return "interval";
        case TypeId::POINT:     return "point";
        case TypeId::JSON:      return "json";
        case TypeId::UUID:      return "uuid";
        case TypeId::EMBEDDING: return "embedding";
        case TypeId::PATH:      return "text";
        }
        return "text";
    };

    auto pg_typlen = [](TypeId t) -> int32_t {
        switch (t) {
        case TypeId::BOOL:      return 1;
        case TypeId::INT8:      return 2;
        case TypeId::INT16:     return 2;
        case TypeId::INT32:     return 4;
        case TypeId::INT64:     return 8;
        case TypeId::UINT8:     return 2;
        case TypeId::UINT16:    return 4;
        case TypeId::UINT32:    return 8;
        case TypeId::UINT64:    return -1;
        case TypeId::FLOAT32:   return 4;
        case TypeId::FLOAT64:   return 8;
        case TypeId::DECIMAL:   return -1;
        case TypeId::STRING:    return -1;
        case TypeId::BLOB:      return -1;
        case TypeId::DATE:      return 4;
        case TypeId::TIME:      return 8;
        case TypeId::TIMESTAMP: return 8;
        case TypeId::INTERVAL:  return 16;
        case TypeId::POINT:     return 16;
        case TypeId::JSON:      return -1;
        case TypeId::UUID:      return 16;
        case TypeId::EMBEDDING: return -1;
        case TypeId::PATH:      return -1;
        }
        return -1;
    };

    auto pg_oid = [](TypeId t) -> uint32_t {
        switch (t) {
        case TypeId::BOOL:      return 16;
        case TypeId::INT8:      return 21;
        case TypeId::INT16:     return 21;
        case TypeId::INT32:     return 23;
        case TypeId::INT64:     return 20;
        case TypeId::UINT8:     return 21;
        case TypeId::UINT16:    return 23;
        case TypeId::UINT32:    return 20;
        case TypeId::UINT64:    return 1700;
        case TypeId::FLOAT32:   return 700;
        case TypeId::FLOAT64:   return 701;
        case TypeId::DECIMAL:   return 1700;
        case TypeId::STRING:    return 25;
        case TypeId::BLOB:      return 17;
        case TypeId::DATE:      return 1082;
        case TypeId::TIME:      return 1083;
        case TypeId::TIMESTAMP: return 1114;
        case TypeId::INTERVAL:  return 1186;
        case TypeId::POINT:     return 600;
        case TypeId::JSON:      return 114;
        case TypeId::UUID:      return 2950;
        case TypeId::EMBEDDING: return 100000;
        case TypeId::PATH:      return 25;
        }
        return 25;
    };

    VirtualTableDef def;
    def.name = "pg_type";
    def.columns = {
        {0, "oid", TypeId::INT32, false, ""},
        {1, "typname", TypeId::STRING, false, ""},
        {2, "typnamespace", TypeId::INT32, false, ""},
        {3, "typlen", TypeId::INT32, false, ""},
        {4, "typtype", TypeId::STRING, false, ""},
        {5, "typelem", TypeId::INT32, false, ""},
        {6, "typrelid", TypeId::INT32, false, ""},
        {7, "typbasetype", TypeId::INT32, false, ""},
    };
    def.generator = [pg_typname, pg_typlen, pg_oid]() -> std::vector<std::vector<std::string>> {
        std::vector<std::vector<std::string>> rows;
        rows.reserve(all_types.size());
        for (auto t : all_types) {
            rows.push_back({
                std::to_string(pg_oid(t)),
                pg_typname(t),
                "11",
                std::to_string(pg_typlen(t)),
                "b",
                "0",
                "0",
                "0",
            });
        }
        return rows;
    };
    catalog.register_virtual_table(std::move(def));
}

void register_pg_namespace(Catalog& catalog) {
    VirtualTableDef def;
    def.name = "pg_namespace";
    def.columns = {
        {0, "oid", TypeId::INT32, false, ""},
        {1, "nspname", TypeId::STRING, false, ""},
        {2, "nspowner", TypeId::INT32, false, ""},
    };
    def.generator = []() -> std::vector<std::vector<std::string>> {
        return {
            {"11", "pg_catalog", "10"},
            {"2200", "public", "10"},
        };
    };
    catalog.register_virtual_table(std::move(def));
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
    if (!vt.has_value()) return {};

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
    if (!open_r.has_value()) return {};

    std::vector<TypeRow> rows;
    while (true) {
        auto next_r = scan.next();
        EXPECT_TRUE(next_r.has_value());
        if (!next_r.has_value()) break;
        if (!next_r->has_value()) break;
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
    auto it = std::find_if(rows.begin(), rows.end(),
                           [](const TypeRow& r) { return r.typname == "embedding"; });
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
        "text", "bytea", "json", "numeric", "embedding",
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
            if (r.typname == name) return &r;
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
        if (!next_r->has_value()) break;
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

} // namespace
} // namespace sixseven
