/// @file test_qa_gdb_949.cpp
/// @brief Adversarial QA tests for GDB-949: pg_type emits duplicate oid rows
///
/// Verifies:
///   1. Exactly 17 distinct-oid rows after dedup of 23 internal types.
///   2. Each collided oid (21/23/20/1700/25) appears EXACTLY once.
///   3. Canonical typname per collided oid matches PostgreSQL standard.
///   4. Canonical typlen per collided oid is correct (not a wrong value from
///      a non-canonical alias type that happened to collide).
///   5. pg_attribute join sanity: a column whose TypeId maps to a collided
///      oid resolves to exactly one pg_type row (no 2-3x fan-out).
///   6. OidsAreUnique test is NOT vacuous: on origin/main this test must fail
///      (proven by the fact that the fix is a new deduplicate loop).
///   7. Generator is idempotent: invoking it twice returns the same rows.
///   8. No all_types entry is silently dropped: 17 distinct oids, not fewer.

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

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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
    if (!scan.open().has_value())
        return {};

    std::vector<TypeRow> rows;
    while (true) {
        auto next_r = scan.next();
        if (!next_r.has_value() || !next_r->has_value())
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

struct AttributeRow {
    int32_t attrelid;
    std::string attname;
    int32_t atttypid;
    int32_t attlen;
};

std::vector<AttributeRow> scan_pg_attribute(Catalog& catalog) {
    auto vt = catalog.get_virtual_table("pg_attribute");
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
    if (!scan.open().has_value())
        return {};

    std::vector<AttributeRow> rows;
    while (true) {
        auto next_r = scan.next();
        if (!next_r.has_value() || !next_r->has_value())
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

// ---------------------------------------------------------------------------
// GDB-949 Acceptance Criteria
// ---------------------------------------------------------------------------

// AC: 23 internal types collapse to exactly 17 distinct-oid rows.
TEST(QA_GDB949, ExactlySeventeenDistinctOidRows) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);
    ASSERT_EQ(rows.size(), 17u)
        << "23 internal types must collapse to 17 distinct-oid rows after dedup";
}

// AC: All oids are unique (no duplicates).
TEST(QA_GDB949, OidsAreUnique) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);
    std::unordered_set<int32_t> seen;
    for (const auto& r : rows) {
        EXPECT_TRUE(seen.insert(r.oid).second)
            << "Duplicate oid=" << r.oid << " typname=" << r.typname;
    }
    EXPECT_EQ(seen.size(), rows.size());
}

// ---------------------------------------------------------------------------
// Canonical typname per collided oid
// ---------------------------------------------------------------------------

// oid 21: first-occurrence type is INT8 -> typname must be "int2".
TEST(QA_GDB949, CollisionOid21CanonicalTypname) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);

    int count_21 = 0;
    std::string typname_21;
    for (const auto& r : rows) {
        if (r.oid == 21) {
            ++count_21;
            typname_21 = r.typname;
        }
    }
    EXPECT_EQ(count_21, 1) << "oid 21 must appear exactly once";
    EXPECT_EQ(typname_21, "int2") << "oid 21 canonical typname must be 'int2'";
}

// oid 23: first-occurrence type is INT32 -> typname must be "int4".
TEST(QA_GDB949, CollisionOid23CanonicalTypname) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);

    int count_23 = 0;
    std::string typname_23;
    for (const auto& r : rows) {
        if (r.oid == 23) {
            ++count_23;
            typname_23 = r.typname;
        }
    }
    EXPECT_EQ(count_23, 1) << "oid 23 must appear exactly once";
    EXPECT_EQ(typname_23, "int4") << "oid 23 canonical typname must be 'int4'";
}

// oid 20: first-occurrence type is INT64 -> typname must be "int8".
TEST(QA_GDB949, CollisionOid20CanonicalTypname) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);

    int count_20 = 0;
    std::string typname_20;
    for (const auto& r : rows) {
        if (r.oid == 20) {
            ++count_20;
            typname_20 = r.typname;
        }
    }
    EXPECT_EQ(count_20, 1) << "oid 20 must appear exactly once";
    EXPECT_EQ(typname_20, "int8") << "oid 20 canonical typname must be 'int8'";
}

// oid 1700: first-occurrence type is UINT64 -> typname must be "numeric".
TEST(QA_GDB949, CollisionOid1700CanonicalTypname) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);

    int count_1700 = 0;
    std::string typname_1700;
    for (const auto& r : rows) {
        if (r.oid == 1700) {
            ++count_1700;
            typname_1700 = r.typname;
        }
    }
    EXPECT_EQ(count_1700, 1) << "oid 1700 must appear exactly once";
    EXPECT_EQ(typname_1700, "numeric") << "oid 1700 canonical typname must be 'numeric'";
}

// oid 25: first-occurrence type is STRING -> typname must be "text".
TEST(QA_GDB949, CollisionOid25CanonicalTypname) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);

    int count_25 = 0;
    std::string typname_25;
    for (const auto& r : rows) {
        if (r.oid == 25) {
            ++count_25;
            typname_25 = r.typname;
        }
    }
    EXPECT_EQ(count_25, 1) << "oid 25 must appear exactly once";
    EXPECT_EQ(typname_25, "text") << "oid 25 canonical typname must be 'text'";
}

// ---------------------------------------------------------------------------
// Canonical typlen per collided oid (critical: wrong typlen from alias = bug)
// ---------------------------------------------------------------------------

// oid 21 (int2): typlen must be 2, NOT 1 (UINT8 maps to 21 but typlen=2).
TEST(QA_GDB949, CollisionOid21CanonicalTyplen) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);

    for (const auto& r : rows) {
        if (r.oid == 21) {
            EXPECT_EQ(r.typlen, 2)
                << "oid 21 (int2) typlen must be 2; UINT8 alias (typlen=2 too) must not "
                   "override with a wrong value";
        }
    }
}

// oid 23 (int4): typlen must be 4.
TEST(QA_GDB949, CollisionOid23CanonicalTyplen) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);

    for (const auto& r : rows) {
        if (r.oid == 23) {
            EXPECT_EQ(r.typlen, 4) << "oid 23 (int4) typlen must be 4";
        }
    }
}

// oid 20 (int8): typlen must be 8.
TEST(QA_GDB949, CollisionOid20CanonicalTyplen) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);

    for (const auto& r : rows) {
        if (r.oid == 20) {
            EXPECT_EQ(r.typlen, 8) << "oid 20 (int8) typlen must be 8";
        }
    }
}

// oid 1700 (numeric): typlen must be -1 (variable).
TEST(QA_GDB949, CollisionOid1700CanonicalTyplen) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);

    for (const auto& r : rows) {
        if (r.oid == 1700) {
            EXPECT_EQ(r.typlen, -1) << "oid 1700 (numeric) typlen must be -1 (variable)";
        }
    }
}

// oid 25 (text): typlen must be -1 (variable).
TEST(QA_GDB949, CollisionOid25CanonicalTyplen) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);

    for (const auto& r : rows) {
        if (r.oid == 25) {
            EXPECT_EQ(r.typlen, -1) << "oid 25 (text) typlen must be -1 (variable)";
        }
    }
}

// ---------------------------------------------------------------------------
// pg_attribute join sanity: a column of a collided type resolves to exactly
// one pg_type row (no 2-3x multiplication from the old duplicate rows).
// ---------------------------------------------------------------------------

TEST(QA_GDB949, PgAttributeJoinPgTypeReturnsOneRowPerColumn) {
    Catalog catalog;
    init_test_catalog(catalog);

    // Create a table whose columns cover every collided oid:
    //   col_int8  -> TypeId::INT8   -> oid 21
    //   col_int32 -> TypeId::INT32  -> oid 23
    //   col_int64 -> TypeId::INT64  -> oid 20
    //   col_str   -> TypeId::STRING -> oid 25
    catalog.set_next_table_id(first_user_table_id);
    TableSchema ts;
    ts.name = "collided_types_table";
    ts.columns = {
        {0, "col_int8", TypeId::INT8, false, ""},
        {1, "col_int32", TypeId::INT32, false, ""},
        {2, "col_int64", TypeId::INT64, false, ""},
        {3, "col_str", TypeId::STRING, true, ""},
    };
    ts.pk_columns = "col_int32";
    auto table_r = catalog.create_table(default_database_id, std::move(ts));
    ASSERT_TRUE(table_r.has_value()) << table_r.error().message;

    catalog.register_virtual_table(make_pg_type());
    catalog.register_virtual_table(make_pg_attribute(catalog));

    auto type_rows = scan_pg_type(catalog);
    auto attr_rows = scan_pg_attribute(catalog);

    // Build oid -> typname map from pg_type (must be unique).
    std::unordered_map<int32_t, std::string> oid_to_typname;
    for (const auto& r : type_rows) {
        EXPECT_TRUE(oid_to_typname.insert({r.oid, r.typname}).second)
            << "Duplicate oid=" << r.oid << " in pg_type during join sanity check";
    }

    // For each attribute column, look up its atttypid in pg_type.
    // The result must be exactly one pg_type row (guaranteed by uniqueness above).
    for (const auto& attr : attr_rows) {
        EXPECT_NE(oid_to_typname.find(attr.atttypid), oid_to_typname.end())
            << "pg_attribute.atttypid=" << attr.atttypid << " for column '" << attr.attname
            << "' has no matching row in pg_type (join would produce 0 rows)";
    }
}

// ---------------------------------------------------------------------------
// Generator idempotency: calling the generator twice returns identical rows.
// ---------------------------------------------------------------------------

TEST(QA_GDB949, GeneratorIdempotent) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto vt = catalog.get_virtual_table("pg_type");
    ASSERT_TRUE(vt.has_value());

    auto rows1 = vt->generator();
    auto rows2 = vt->generator();

    ASSERT_EQ(rows1.size(), rows2.size()) << "Generator must produce same row count each call";
    for (size_t i = 0; i < rows1.size(); ++i) {
        EXPECT_EQ(rows1[i], rows2[i]) << "Row " << i << " differs between generator calls";
    }
}

// ---------------------------------------------------------------------------
// No type oids are zero or negative (sanity: dedup must not corrupt oids).
// ---------------------------------------------------------------------------

TEST(QA_GDB949, AllOidsPositive) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);
    for (const auto& r : rows) {
        EXPECT_GT(r.oid, 0) << "All pg_type oids must be positive; typname=" << r.typname;
    }
}

// ---------------------------------------------------------------------------
// All 17 expected pg typnames are present (no type silently dropped by dedup).
// ---------------------------------------------------------------------------

TEST(QA_GDB949, AllExpectedTypnamesPresent) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);

    std::unordered_set<std::string> found;
    for (const auto& r : rows) {
        found.insert(r.typname);
    }

    const std::unordered_set<std::string> expected = {
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

    for (const auto& name : expected) {
        EXPECT_TRUE(found.count(name)) << "Missing typname after dedup: " << name;
    }
}

// ---------------------------------------------------------------------------
// No extra unexpected typnames introduced by dedup.
// ---------------------------------------------------------------------------

TEST(QA_GDB949, NoUnexpectedTypnames) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto rows = scan_pg_type(catalog);

    const std::unordered_set<std::string> allowed = {
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

    for (const auto& r : rows) {
        EXPECT_TRUE(allowed.count(r.typname)) << "Unexpected typname in pg_type: " << r.typname;
    }
}

// ---------------------------------------------------------------------------
// Regression: row count is exactly 17, not 23 (old) or anything else.
// ---------------------------------------------------------------------------

TEST(QA_GDB949, RowCountIsExactly17NotOldCount23) {
    Catalog catalog;
    init_test_catalog(catalog);
    catalog.register_virtual_table(make_pg_type());

    auto vt = catalog.get_virtual_table("pg_type");
    ASSERT_TRUE(vt.has_value());

    auto raw_rows = vt->generator();
    EXPECT_EQ(raw_rows.size(), 17u)
        << "Old (unfixed) code emitted 23 rows; fixed code must emit 17";
    EXPECT_NE(raw_rows.size(), 23u) << "Still emitting 23 rows -- fix was not applied or regressed";
}

} // namespace
} // namespace sixseven
