/// @file test_qa_gdb_565.cpp
/// @brief Adversarial QA tests for GDB-565: pg_type and pg_namespace Virtual Tables
///
/// Tests cover: acceptance criteria verification, OID consistency with pg_protocol.cpp,
/// type mapping correctness, pg_namespace content, WHERE filtering through binder,
/// cross-table referential integrity, and boundary conditions.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/executor/pg_catalog_tables.h"
#include "sixseven/executor/virtual_catalog_scan.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/planner/binder.h"

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

StmtPtr parse_sql(const std::string& sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return nullptr;
    Parser parser(std::move(*tokens));
    auto stmt = parser.parse();
    if (!stmt)
        return nullptr;
    return std::move(*stmt);
}

OutputSchema make_output_schema(const VirtualTableDef& def) {
    std::vector<OutputColumn> cols;
    for (const auto& col : def.columns) {
        cols.push_back({def.name, col.name, col.type_id, col.nullable, def.table_id});
    }
    return OutputSchema(std::move(cols));
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

struct NamespaceRow {
    int32_t oid;
    std::string nspname;
    int32_t nspowner;
};

std::vector<NamespaceRow> scan_pg_namespace(Catalog& catalog) {
    auto vt = catalog.get_virtual_table("pg_namespace");
    EXPECT_TRUE(vt.has_value());
    if (!vt.has_value())
        return {};

    OutputSchema schema(std::vector<OutputColumn>{
        {"pg_namespace", "oid", TypeId::INT32, false, vt->table_id},
        {"pg_namespace", "nspname", TypeId::STRING, false, vt->table_id},
        {"pg_namespace", "nspowner", TypeId::INT32, false, vt->table_id},
    });

    VirtualCatalogScanOperator scan(std::move(*vt), std::move(schema));
    auto open_r = scan.open();
    EXPECT_TRUE(open_r.has_value());
    if (!open_r.has_value())
        return {};

    std::vector<NamespaceRow> rows;
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
        });
    }
    scan.close();
    return rows;
}

// ===========================================================================
// AC1: pg_type returns rows for all SixSevenDB types
// ===========================================================================

TEST(QA_GDB565, AC1_PgTypeReturnsAllTypes) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);
    // GDB-949: 23 internal types collapse to 17 distinct pg oids after deduplication.
    EXPECT_EQ(rows.size(), 17u)
        << "Should have one row per distinct pg oid (deduped from 23 internal types)";

    std::unordered_set<std::string> expected_names = {
        "bool",
        "int2",
        "int4",
        "int8",
        "numeric",
        "float4",
        "float8",
        "text",
        "bytea",
        "date",
        "time",
        "timestamp",
        "interval",
        "point",
        "json",
        "uuid",
        "embedding",
    };

    std::unordered_set<std::string> found_names;
    for (const auto& r : rows) {
        found_names.insert(r.typname);
    }

    for (const auto& name : expected_names) {
        EXPECT_TRUE(found_names.count(name)) << "Missing pg_type name: " << name;
    }
}

// ===========================================================================
// AC2: OIDs match type_to_pg_oid mapping in pg_protocol.cpp
// ===========================================================================

TEST(QA_GDB565, AC2_OidsMatchPgProtocol) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);

    // Expected OIDs from pg_protocol.cpp constants.
    std::unordered_map<std::string, std::set<int32_t>> expected_oids = {
        {"bool", {16}},
        {"int2", {21}},
        {"int4", {23}},
        {"int8", {20}},
        {"float4", {700}},
        {"float8", {701}},
        {"numeric", {1700}},
        {"text", {25}},
        {"bytea", {17}},
        {"date", {1082}},
        {"time", {1083}},
        {"timestamp", {1114}},
        {"interval", {1186}},
        {"point", {600}},
        {"json", {114}},
        {"uuid", {2950}},
        {"embedding", {100000}},
    };

    for (const auto& r : rows) {
        auto it = expected_oids.find(r.typname);
        ASSERT_NE(it, expected_oids.end()) << "Unexpected typname: " << r.typname;
        EXPECT_TRUE(it->second.count(r.oid))
            << "typname=" << r.typname << " has unexpected OID " << r.oid
            << ", expected one of: " << *it->second.begin();
    }
}

// ===========================================================================
// AC3: EMBEDDING type has OID 100000
// ===========================================================================

TEST(QA_GDB565, AC3_EmbeddingHasOid100000) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);
    auto it = std::find_if(
        rows.begin(), rows.end(), [](const TypeRow& r) { return r.typname == "embedding"; });
    ASSERT_NE(it, rows.end()) << "EMBEDDING type must appear in pg_type";
    EXPECT_EQ(it->oid, 100000);
    EXPECT_EQ(it->typlen, -1) << "EMBEDDING should be variable-length";
}

// ===========================================================================
// AC4: pg_namespace returns pg_catalog and public
// ===========================================================================

TEST(QA_GDB565, AC4_PgNamespaceReturnsExpectedRows) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_namespace());

    auto rows = scan_pg_namespace(catalog);
    ASSERT_EQ(rows.size(), 2u);

    EXPECT_EQ(rows[0].oid, 11);
    EXPECT_EQ(rows[0].nspname, "pg_catalog");
    EXPECT_EQ(rows[0].nspowner, 10);

    EXPECT_EQ(rows[1].oid, 2200);
    EXPECT_EQ(rows[1].nspname, "public");
    EXPECT_EQ(rows[1].nspowner, 10);
}

// ===========================================================================
// AC5: WHERE clause filtering binds successfully on pg_type
// ===========================================================================

TEST(QA_GDB565, AC5_WhereFilteringBindsSuccessfully) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto stmt = parse_sql("SELECT oid, typname FROM pg_catalog.pg_type WHERE typname = 'int4'");
    ASSERT_NE(stmt, nullptr);

    Binder binder(catalog);
    auto result = binder.bind(*stmt);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->output_columns.size(), 2u);
}

// AC6 ("dev unit tests exist in tests/unit/test_pg_catalog_tables.cpp") was a
// SUCCEED()-only placeholder that executed no code and asserted nothing -- it
// inflated the pass count and could not detect the referenced coverage being
// removed. That criterion is a code-review/CI concern, not a runtime assertion;
// the actual pg_catalog table behavior is exercised by the AC1-AC5 and
// adversarial tests in this file (plus tests/unit/test_pg_catalog_tables.cpp).
// Removed.

// ===========================================================================
// Adversarial: pg_type column structure
// ===========================================================================

TEST(QA_GDB565, Adversarial_PgTypeAllColumnsPresent) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto vt = catalog.get_virtual_table("pg_type");
    ASSERT_TRUE(vt.has_value());
    EXPECT_EQ(vt->columns.size(), 8u);

    EXPECT_EQ(vt->columns[0].name, "oid");
    EXPECT_EQ(vt->columns[1].name, "typname");
    EXPECT_EQ(vt->columns[2].name, "typnamespace");
    EXPECT_EQ(vt->columns[3].name, "typlen");
    EXPECT_EQ(vt->columns[4].name, "typtype");
    EXPECT_EQ(vt->columns[5].name, "typelem");
    EXPECT_EQ(vt->columns[6].name, "typrelid");
    EXPECT_EQ(vt->columns[7].name, "typbasetype");

    auto raw_rows = vt->generator();
    for (size_t i = 0; i < raw_rows.size(); ++i) {
        EXPECT_EQ(raw_rows[i].size(), 8u) << "Row " << i << " should have exactly 8 columns";
    }
}

// ===========================================================================
// Adversarial: duplicate typnames have consistent OIDs
// ===========================================================================

TEST(QA_GDB565, Adversarial_PgTypeNoDuplicateOidTypnamePairs) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);

    std::unordered_map<std::string, int32_t> name_to_oid;
    for (const auto& r : rows) {
        auto it = name_to_oid.find(r.typname);
        if (it != name_to_oid.end()) {
            EXPECT_EQ(it->second, r.oid)
                << "typname=" << r.typname << " has inconsistent OIDs: " << it->second << " vs "
                << r.oid;
        } else {
            name_to_oid[r.typname] = r.oid;
        }
    }
}

// ===========================================================================
// Adversarial: typnamespace references valid pg_namespace
// ===========================================================================

TEST(QA_GDB565, Adversarial_PgTypeTypnamespaceRefersToValidNamespace) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());
    catalog.register_virtual_table(make_pg_namespace());

    auto type_rows = scan_pg_type(catalog);
    auto ns_rows = scan_pg_namespace(catalog);

    std::unordered_set<int32_t> valid_ns_oids;
    for (const auto& ns : ns_rows) {
        valid_ns_oids.insert(ns.oid);
    }

    for (const auto& r : type_rows) {
        EXPECT_TRUE(valid_ns_oids.count(r.typnamespace))
            << "typname=" << r.typname << " has typnamespace=" << r.typnamespace
            << " which does not exist in pg_namespace";
    }
}

// ===========================================================================
// Adversarial: static fields correct for all rows
// ===========================================================================

TEST(QA_GDB565, Adversarial_PgTypeStaticFieldsCorrect) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);
    ASSERT_FALSE(rows.empty());

    for (const auto& r : rows) {
        EXPECT_EQ(r.typtype, "b") << "typname=" << r.typname << " should have typtype='b'";
        EXPECT_EQ(r.typelem, 0) << "typname=" << r.typname << " should have typelem=0";
        EXPECT_EQ(r.typrelid, 0) << "typname=" << r.typname << " should have typrelid=0";
        EXPECT_EQ(r.typbasetype, 0) << "typname=" << r.typname << " should have typbasetype=0";
        EXPECT_EQ(r.typnamespace, 11) << "typname=" << r.typname << " should be in pg_catalog (11)";
    }
}

// ===========================================================================
// Adversarial: pg_namespace owners consistent
// ===========================================================================

TEST(QA_GDB565, Adversarial_PgNamespaceOwnersAreConsistent) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_namespace());

    auto rows = scan_pg_namespace(catalog);
    for (const auto& r : rows) {
        EXPECT_EQ(r.nspowner, 10) << "nspname=" << r.nspname << " should have nspowner=10";
    }
}

// ===========================================================================
// Adversarial: pg_namespace OIDs are distinct
// ===========================================================================

TEST(QA_GDB565, Adversarial_PgNamespaceNoDuplicateOids) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_namespace());

    auto rows = scan_pg_namespace(catalog);
    std::set<int32_t> oids;
    for (const auto& r : rows) {
        EXPECT_TRUE(oids.insert(r.oid).second) << "Duplicate pg_namespace OID: " << r.oid;
    }
}

// ===========================================================================
// Adversarial: spot-check specific OID mappings
// ===========================================================================

TEST(QA_GDB565, Adversarial_PgTypeSpecificOidMapping) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);

    auto find_by_name = [&](const std::string& name) -> const TypeRow* {
        for (const auto& r : rows) {
            if (r.typname == name)
                return &r;
        }
        return nullptr;
    };

    auto* r = find_by_name("bool");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->oid, 16);

    r = find_by_name("int4");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->oid, 23);

    r = find_by_name("int8");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->oid, 20);

    r = find_by_name("text");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->oid, 25);

    r = find_by_name("date");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->oid, 1082);

    r = find_by_name("uuid");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->oid, 2950);

    r = find_by_name("bytea");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->oid, 17);

    r = find_by_name("json");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->oid, 114);

    r = find_by_name("point");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->oid, 600);
}

// ===========================================================================
// Adversarial: variable-length type consistency
// ===========================================================================

TEST(QA_GDB565, Adversarial_PgTypeVariableLengthConsistency) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

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
            EXPECT_EQ(r.typlen, -1) << "typname=" << r.typname << " should be variable-length";
        } else {
            EXPECT_GT(r.typlen, 0)
                << "typname=" << r.typname << " should have positive fixed length";
        }
    }
}

// ===========================================================================
// Adversarial: fixed-length type boundary values
// ===========================================================================

TEST(QA_GDB565, Adversarial_PgTypeFixedLengthBoundaries) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);

    auto find_by_name = [&](const std::string& name) -> const TypeRow* {
        for (const auto& r : rows) {
            if (r.typname == name)
                return &r;
        }
        return nullptr;
    };

    auto* r = find_by_name("bool");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->typlen, 1);

    r = find_by_name("int2");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->typlen, 2);

    r = find_by_name("int4");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->typlen, 4);

    r = find_by_name("int8");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->typlen, 8);

    r = find_by_name("float4");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->typlen, 4);

    r = find_by_name("float8");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->typlen, 8);

    r = find_by_name("point");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->typlen, 16);

    r = find_by_name("uuid");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->typlen, 16);

    r = find_by_name("date");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->typlen, 4);

    r = find_by_name("time");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->typlen, 8);

    r = find_by_name("timestamp");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->typlen, 8);

    r = find_by_name("interval");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->typlen, 16);
}

// ===========================================================================
// Adversarial: WHERE oid = <value> filter binds on pg_type
// ===========================================================================

TEST(QA_GDB565, Adversarial_PgTypeWhereOidFilterBinds) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto stmt = parse_sql("SELECT typname FROM pg_catalog.pg_type WHERE oid = 23");
    ASSERT_NE(stmt, nullptr);

    Binder binder(catalog);
    auto result = binder.bind(*stmt);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->output_columns.size(), 1u);
    EXPECT_EQ(result->output_columns[0].column_name, "typname");
}

// ===========================================================================
// Adversarial: JOIN pg_type with pg_namespace binds
// ===========================================================================

TEST(QA_GDB565, Adversarial_PgTypeJoinPgNamespace) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());
    catalog.register_virtual_table(make_pg_namespace());

    auto stmt = parse_sql("SELECT t.typname, n.nspname "
                          "FROM pg_catalog.pg_type t "
                          "JOIN pg_catalog.pg_namespace n ON t.typnamespace = n.oid");
    ASSERT_NE(stmt, nullptr);

    Binder binder(catalog);
    auto result = binder.bind(*stmt);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->output_columns.size(), 2u);
}

// ===========================================================================
// Adversarial: SELECT * binds correctly for both tables
// ===========================================================================

TEST(QA_GDB565, Adversarial_SelectStarBindsPgType) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto stmt = parse_sql("SELECT * FROM pg_catalog.pg_type");
    ASSERT_NE(stmt, nullptr);

    Binder binder(catalog);
    auto result = binder.bind(*stmt);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->output_columns.size(), 8u);
}

TEST(QA_GDB565, Adversarial_SelectStarBindsPgNamespace) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_namespace());

    auto stmt = parse_sql("SELECT * FROM pg_catalog.pg_namespace");
    ASSERT_NE(stmt, nullptr);

    Binder binder(catalog);
    auto result = binder.bind(*stmt);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->output_columns.size(), 3u);
}

// ===========================================================================
// Adversarial: pg_type has no NULL values
// ===========================================================================

TEST(QA_GDB565, Adversarial_PgTypeNoNullValues) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto vt = catalog.get_virtual_table("pg_type");
    ASSERT_TRUE(vt.has_value());

    auto raw_rows = vt->generator();
    for (size_t i = 0; i < raw_rows.size(); ++i) {
        for (size_t j = 0; j < raw_rows[i].size(); ++j) {
            EXPECT_FALSE(!raw_rows[i][j].has_value())
                << "Row " << i << " col " << j << " should not be empty (would become NULL)";
        }
    }
}

// ===========================================================================
// Adversarial: all OIDs are positive
// ===========================================================================

TEST(QA_GDB565, Adversarial_AllOidsPositive) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());
    catalog.register_virtual_table(make_pg_namespace());

    auto type_rows = scan_pg_type(catalog);
    for (const auto& r : type_rows) {
        EXPECT_GT(r.oid, 0) << "pg_type OID should be positive, typname=" << r.typname;
    }

    auto ns_rows = scan_pg_namespace(catalog);
    for (const auto& r : ns_rows) {
        EXPECT_GT(r.oid, 0) << "pg_namespace OID should be positive, nspname=" << r.nspname;
    }
}

// ===========================================================================
// Adversarial: pg_type with alias binds
// ===========================================================================

TEST(QA_GDB565, Adversarial_PgTypeWithAlias) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto stmt = parse_sql("SELECT t.oid, t.typname FROM pg_catalog.pg_type AS t");
    ASSERT_NE(stmt, nullptr);

    Binder binder(catalog);
    auto result = binder.bind(*stmt);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->output_columns.size(), 2u);
}

// ===========================================================================
// Adversarial: invalid column reference on pg_type
// ===========================================================================

TEST(QA_GDB565, Adversarial_InvalidColumnOnPgType) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto stmt = parse_sql("SELECT nonexistent_col FROM pg_catalog.pg_type");
    ASSERT_NE(stmt, nullptr);

    Binder binder(catalog);
    auto result = binder.bind(*stmt);
    ASSERT_FALSE(result.has_value()) << "Should fail for nonexistent column on pg_type";
}

// ===========================================================================
// Adversarial: pg_namespace lookup succeeds
// ===========================================================================

TEST(QA_GDB565, Adversarial_PgNamespaceLookupSucceeds) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_namespace());

    auto vt = catalog.get_virtual_table("pg_namespace");
    ASSERT_TRUE(vt.has_value());
    EXPECT_EQ(vt->name, "pg_namespace");
    EXPECT_EQ(vt->columns.size(), 3u);
    EXPECT_LT(vt->table_id, 0) << "Virtual table should have negative ID";
}

// ===========================================================================
// Adversarial: pg_type scan iterator lifecycle
// ===========================================================================

TEST(QA_GDB565, Adversarial_PgTypeScanIteratorLifecycle) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto vt = catalog.get_virtual_table("pg_type");
    ASSERT_TRUE(vt.has_value());

    auto schema = make_output_schema(*vt);
    VirtualCatalogScanOperator scan(std::move(*vt), std::move(schema));

    // open -> drain -> close -> reopen -> drain -> close
    ASSERT_TRUE(scan.open().has_value());
    int count1 = 0;
    while (true) {
        auto r = scan.next();
        ASSERT_TRUE(r.has_value());
        if (!r->has_value())
            break;
        ++count1;
    }
    scan.close();

    ASSERT_TRUE(scan.open().has_value());
    int count2 = 0;
    while (true) {
        auto r = scan.next();
        ASSERT_TRUE(r.has_value());
        if (!r->has_value())
            break;
        ++count2;
    }
    scan.close();

    EXPECT_EQ(count1, count2) << "Re-scan should produce same number of rows";
    EXPECT_EQ(count1, 17); // GDB-949: 17 distinct pg oids after deduplication
}

} // namespace
} // namespace sixseven
