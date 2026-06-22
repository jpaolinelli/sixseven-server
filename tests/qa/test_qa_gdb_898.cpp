// QA adversarial tests for GDB-898: make_pg_database reads real catalog.
//
// Attack surface:
//   (1) Mutation-grade: tests FAIL if make_pg_database reverts to canned row.
//   (2) Multi-database correctness: CREATE/DROP, oid==database_id, ordering.
//   (3) Cross-consistency with pg_class: databases in pg_class appear in
//       pg_database.
//   (4) datdba=10 / encoding=6 are intentional constants.
//   (5) Generator-after-catalog-change: capture-by-ref picks up mutations.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/executor/pg_catalog_tables.h"
#include "sixseven/executor/virtual_catalog_scan.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct DbRow {
    int32_t oid;
    std::string datname;
    int32_t datdba;
    int32_t encoding;
};

std::vector<DbRow> scan_pg_database(Catalog& catalog) {
    auto vt = catalog.get_virtual_table("pg_database");
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
    if (!open_r.has_value())
        return {};

    std::vector<DbRow> rows;
    while (true) {
        auto next_r = scan.next();
        if (!next_r.has_value() || !next_r->has_value())
            break;
        auto& t = next_r->value();
        rows.push_back({t.values[0].as_int32(), std::string(t.values[1].as_string()),
                        t.values[2].as_int32(), t.values[3].as_int32()});
    }
    scan.close();
    return rows;
}

void register_pg_database(Catalog& catalog) {
    catalog.register_virtual_table(make_pg_database(catalog));
}

void register_pg_class(Catalog& catalog) {
    catalog.register_virtual_table(make_pg_class(catalog));
}

// ---------------------------------------------------------------------------
// (1) MUTATION-GRADE: Verify tests catch the old canned-row behaviour.
//     If make_pg_database reverted to returning {{"1","sixsevendb","10","6"}}
//     unconditionally, the following tests would all fail.
// ---------------------------------------------------------------------------

// GDB898_MutationGrade: row count tracks the catalog, not a hard-coded "1"
TEST(QA_GDB898_PgDatabase, RowCountEqualsCatalogDatabaseCount) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_database(catalog);

    // At baseline the catalog has system db + default db = 2 entries.
    auto catalog_count = catalog.list_databases().size();
    auto rows = scan_pg_database(catalog);
    EXPECT_EQ(rows.size(), catalog_count)
        << "pg_database row count must equal catalog.list_databases().size()";
    // Crucially: if canned row was returned we'd see 1, not 2.
    EXPECT_GE(rows.size(), 2u)
        << "Expected at least the system database + the default database";
}

// GDB898_MutationGrade: row OID must match the real database_id, not always 1.
TEST(QA_GDB898_PgDatabase, OidEqualsRealDatabaseId) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_database(catalog);

    auto rows = scan_pg_database(catalog);
    // Find the default ("demo") database.
    const DbRow* demo = nullptr;
    for (const auto& r : rows) {
        if (r.datname == default_database_name) {
            demo = &r;
            break;
        }
    }
    ASSERT_NE(demo, nullptr) << "Default database row not found";
    EXPECT_EQ(demo->oid, static_cast<int32_t>(default_database_id))
        << "OID must equal default_database_id, not a hardcoded 1";
}

// GDB898_MutationGrade: name must be the real name, not the hardcoded "sixsevendb".
TEST(QA_GDB898_PgDatabase, DatabaseNameIsRealNotHardcoded) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_database(catalog);

    auto rows = scan_pg_database(catalog);
    // None of the real databases should be named "sixsevendb" since no such DB
    // exists in a freshly-initialised catalog.  The hardcoded canned row returned
    // exactly that name.
    bool found_hardcoded = false;
    for (const auto& r : rows) {
        if (r.datname == "sixsevendb") {
            found_hardcoded = true;
        }
    }
    EXPECT_FALSE(found_hardcoded)
        << "Found \"sixsevendb\" — suggests the old hardcoded canned row is still active";
}

// ---------------------------------------------------------------------------
// (2a) Multi-database correctness: CREATE DATABASE appears in pg_database
// ---------------------------------------------------------------------------

TEST(QA_GDB898_PgDatabase, CreateDatabaseAppearsImmediately) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_database(catalog);

    auto before_count = scan_pg_database(catalog).size();

    auto r = catalog.create_database("qa_newdb");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    database_id_t new_id = *r;

    // Generator captures catalog by ref — must see the new DB.
    auto after = scan_pg_database(catalog);
    EXPECT_EQ(after.size(), before_count + 1u);

    bool found = false;
    for (const auto& row : after) {
        if (row.oid == static_cast<int32_t>(new_id) && row.datname == "qa_newdb") {
            found = true;
            EXPECT_EQ(row.datdba, 10);
            EXPECT_EQ(row.encoding, 6);
        }
    }
    EXPECT_TRUE(found) << "Newly created database must appear in pg_database";
}

// (2b) Multiple databases have distinct OIDs and names.
TEST(QA_GDB898_PgDatabase, MultipleCreatedDatabasesHaveDistinctOidsAndNames) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_database(catalog);

    auto r1 = catalog.create_database("db_alpha");
    ASSERT_TRUE(r1.has_value());
    auto r2 = catalog.create_database("db_beta");
    ASSERT_TRUE(r2.has_value());
    auto r3 = catalog.create_database("db_gamma");
    ASSERT_TRUE(r3.has_value());

    auto rows = scan_pg_database(catalog);

    std::unordered_set<int32_t> oids;
    std::unordered_set<std::string> names;
    for (const auto& row : rows) {
        EXPECT_TRUE(oids.insert(row.oid).second) << "Duplicate OID " << row.oid;
        EXPECT_TRUE(names.insert(row.datname).second) << "Duplicate name " << row.datname;
    }

    // All three new databases should be present.
    EXPECT_NE(names.find("db_alpha"), names.end());
    EXPECT_NE(names.find("db_beta"), names.end());
    EXPECT_NE(names.find("db_gamma"), names.end());
}

// (2c) OID in pg_database exactly matches database_id returned by create_database.
TEST(QA_GDB898_PgDatabase, OidExactlyMatchesDatabaseId) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_database(catalog);

    auto r = catalog.create_database("qa_oid_check");
    ASSERT_TRUE(r.has_value());
    database_id_t expected_id = *r;

    auto rows = scan_pg_database(catalog);
    const DbRow* found = nullptr;
    for (const auto& row : rows) {
        if (row.datname == "qa_oid_check") {
            found = &row;
            break;
        }
    }
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->oid, static_cast<int32_t>(expected_id));
}

// (2d) DROP DATABASE removes the row from pg_database.
TEST(QA_GDB898_PgDatabase, DroppedDatabaseDisappearsFromPgDatabase) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_database(catalog);

    auto r = catalog.create_database("qa_dropit");
    ASSERT_TRUE(r.has_value());
    database_id_t drop_id = *r;

    // Confirm it is present.
    {
        auto rows = scan_pg_database(catalog);
        bool found = false;
        for (const auto& row : rows) {
            if (row.oid == static_cast<int32_t>(drop_id))
                found = true;
        }
        ASSERT_TRUE(found) << "Database must be visible before drop";
    }

    auto drop_r = catalog.drop_database(drop_id, /*cascade=*/true);
    ASSERT_TRUE(drop_r.has_value()) << drop_r.error().message;

    // After drop it must be gone.
    {
        auto rows = scan_pg_database(catalog);
        for (const auto& row : rows) {
            EXPECT_NE(row.oid, static_cast<int32_t>(drop_id))
                << "Dropped database OID must not appear in pg_database";
        }
    }
}

// (2e) Ordering: pg_database rows are in ascending database_id order
//      (matching list_databases() sort contract).
TEST(QA_GDB898_PgDatabase, RowsOrderedByDatabaseId) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_database(catalog);

    (void)catalog.create_database("qa_c");
    (void)catalog.create_database("qa_a");
    (void)catalog.create_database("qa_b");

    auto rows = scan_pg_database(catalog);
    for (size_t i = 1; i < rows.size(); ++i) {
        EXPECT_LE(rows[i - 1].oid, rows[i].oid)
            << "pg_database rows must be ordered by ascending OID (database_id)";
    }
}

// (2f) System database "sixseven_system" appears in pg_database — this is
//      the adjudicated-correct PG-compatible behaviour (real PostgreSQL lists
//      system DBs too), so we assert the presence, not absence.
TEST(QA_GDB898_PgDatabase, SystemDatabaseAppearsInPgDatabase) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_database(catalog);

    auto rows = scan_pg_database(catalog);
    bool found_system = false;
    for (const auto& r : rows) {
        if (r.oid == static_cast<int32_t>(system_database_id)) {
            found_system = true;
            EXPECT_EQ(r.datdba, 10) << "System db datdba must be 10";
            EXPECT_EQ(r.encoding, 6) << "System db encoding must be 6";
        }
    }
    EXPECT_TRUE(found_system) << "sixseven_system must appear in pg_database";
}

// (2g) Edge-case: long database name.
TEST(QA_GDB898_PgDatabase, LongDatabaseNameRoundTrips) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_database(catalog);

    std::string long_name(128, 'x');
    auto r = catalog.create_database(long_name);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    database_id_t new_id = *r;

    auto rows = scan_pg_database(catalog);
    bool found = false;
    for (const auto& row : rows) {
        if (row.oid == static_cast<int32_t>(new_id)) {
            EXPECT_EQ(row.datname, long_name);
            found = true;
        }
    }
    EXPECT_TRUE(found) << "Long-name database must appear in pg_database";
}

// ---------------------------------------------------------------------------
// (3) Cross-consistency with pg_class: every database whose tables appear
//     in pg_class must also appear in pg_database.
// ---------------------------------------------------------------------------

TEST(QA_GDB898_PgDatabase, DatabasesInPgClassAlsoAppearInPgDatabase) {
    Catalog catalog;
    init_test_catalog(catalog);

    // Create a second user database and put a table in it.
    auto r = catalog.create_database("qa_second_db");
    ASSERT_TRUE(r.has_value());
    database_id_t second_db = *r;

    catalog.set_next_table_id(first_user_table_id);
    TableSchema ts;
    ts.name = "things";
    ts.columns = {{0, "id", TypeId::INT32, false, ""}};
    ts.pk_columns = "id";
    auto tr = catalog.create_table(second_db, std::move(ts));
    ASSERT_TRUE(tr.has_value());

    register_pg_database(catalog);
    register_pg_class(catalog);

    // Collect all database_ids implied by pg_class entries (via pg_database
    // OID set cross-check).  Since pg_class does not store database_id
    // directly, we verify the inverse: every OID in pg_database covers at
    // least the databases implied by list_databases().
    auto db_rows = scan_pg_database(catalog);
    std::unordered_set<int32_t> db_oids;
    for (const auto& r2 : db_rows) {
        db_oids.insert(r2.oid);
    }

    // Every database known to the catalog must appear in pg_database.
    for (const auto& db : catalog.list_databases()) {
        EXPECT_NE(db_oids.find(static_cast<int32_t>(db.database_id)), db_oids.end())
            << "Database id=" << db.database_id << " name=" << db.name
            << " is in the catalog but missing from pg_database";
    }
}

// ---------------------------------------------------------------------------
// (4) datdba and encoding are intentional constants across all databases.
// ---------------------------------------------------------------------------

TEST(QA_GDB898_PgDatabase, DatdbaIsAlways10) {
    Catalog catalog;
    init_test_catalog(catalog);
    (void)catalog.create_database("qa_check1");
    (void)catalog.create_database("qa_check2");
    register_pg_database(catalog);

    auto rows = scan_pg_database(catalog);
    for (const auto& r : rows) {
        EXPECT_EQ(r.datdba, 10) << "datdba must be the constant 10 for datname=" << r.datname;
    }
}

TEST(QA_GDB898_PgDatabase, EncodingIsAlways6) {
    Catalog catalog;
    init_test_catalog(catalog);
    (void)catalog.create_database("qa_enc1");
    register_pg_database(catalog);

    auto rows = scan_pg_database(catalog);
    for (const auto& r : rows) {
        EXPECT_EQ(r.encoding, 6) << "encoding must be the constant 6 for datname=" << r.datname;
    }
}

// (4b) Make sure make_pg_namespace, make_pg_type, and make_pg_class are
//      unaffected — they should still not take a Catalog& just because
//      make_pg_database now does.
TEST(QA_GDB898_PgDatabase, OtherGeneratorsUnaffectedByChange) {
    Catalog catalog;
    init_test_catalog(catalog);

    // These must compile and produce non-empty results without error.
    auto ns_def = make_pg_namespace();
    EXPECT_EQ(ns_def.name, "pg_namespace");
    auto ns_rows = ns_def.generator();
    EXPECT_EQ(ns_rows.size(), 2u);

    auto type_def = make_pg_type();
    EXPECT_EQ(type_def.name, "pg_type");
    auto type_rows = type_def.generator();
    EXPECT_EQ(type_rows.size(), 23u);
}

// ---------------------------------------------------------------------------
// (5) Lifetime / capture-by-ref: generator invoked after catalog mutations
//     reflects the latest state (same guard pattern as make_pg_class).
// ---------------------------------------------------------------------------

TEST(QA_GDB898_PgDatabase, GeneratorReflectsLatestCatalogState) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_database(catalog);

    // Snapshot baseline raw row count from the generator.
    auto vt = catalog.get_virtual_table("pg_database");
    ASSERT_TRUE(vt.has_value());

    auto rows_before = vt->generator();
    size_t count_before = rows_before.size();

    // Add a database AFTER register — the already-registered generator must
    // pick it up on next invocation (capture by ref).
    auto r = catalog.create_database("qa_late_add");
    ASSERT_TRUE(r.has_value());

    auto rows_after = vt->generator();
    EXPECT_EQ(rows_after.size(), count_before + 1u)
        << "Generator must see databases added after registration";

    // Also verify removing a database is reflected.
    database_id_t added_id = *r;
    auto drop_r = catalog.drop_database(added_id, /*cascade=*/true);
    ASSERT_TRUE(drop_r.has_value());

    auto rows_after_drop = vt->generator();
    EXPECT_EQ(rows_after_drop.size(), count_before)
        << "Generator must reflect dropped database";
}

// ---------------------------------------------------------------------------
// register_pg_catalog_tables wires make_pg_database(catalog) — smoke test.
// ---------------------------------------------------------------------------

TEST(QA_GDB898_PgDatabase, RegisterAllTablesIncludesPgDatabase) {
    Catalog catalog;
    init_test_catalog(catalog);
    register_pg_catalog_tables(catalog);

    auto vt = catalog.get_virtual_table("pg_database");
    ASSERT_TRUE(vt.has_value()) << "pg_database must be registered";

    auto rows = vt->generator();
    EXPECT_GE(rows.size(), 1u) << "pg_database must have at least one row";

    // Each row must have exactly 4 columns.
    for (const auto& row : rows) {
        EXPECT_EQ(row.size(), 4u) << "pg_database row must have 4 columns";
    }
}

} // namespace
} // namespace sixseven
