/// QA adversarial tests for GDB-836: Duplicate-column DDL validation.
///
/// Probes the duplicate-column rejection added to Catalog::create_table().
/// Focus areas:
///   1. Catalog cleanliness after rejection (no half-registration / orphan table).
///   2. Duplicate of first vs last column; all-identical columns.
///   3. Rejected table name is immediately reusable (no orphan entry in name map).
///   4. Error message quality: identifies the duplicate column.
///   5. Correct StatusCode (ALREADY_EXISTS).
///   6. Positive guard: distinct names are not over-rejected.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "test_qa_helpers.h"

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helpers (mirror helpers from test_qa_gdb_98.cpp to keep this file standalone)
// ---------------------------------------------------------------------------

static TableSchema make_schema_836(const std::string& name,
                                   const std::vector<CatalogColumnDef>& cols = {}) {
    TableSchema schema;
    schema.table_id = 0;
    schema.name = name;
    schema.columns = cols;
    schema.pk_columns = "";
    return schema;
}

static CatalogColumnDef
make_col_836(int32_t ordinal, const std::string& name, TypeId type, bool nullable = true) {
    return {ordinal, name, type, nullable, ""};
}

// ---------------------------------------------------------------------------
// Suite QA_GDB836_DupColValidation
// ---------------------------------------------------------------------------

/// AC1 (regression): duplicate column names must be rejected with ALREADY_EXISTS
/// and the error message must name the duplicate column.
TEST(QA_GDB836_DupColValidation, RejectionBasicContract) {
    Catalog catalog;
    bootstrap_qa_catalog(catalog);

    std::vector<CatalogColumnDef> cols;
    cols.push_back(make_col_836(0, "dup_col", TypeId::INT32));
    cols.push_back(make_col_836(1, "dup_col", TypeId::STRING));
    auto result = catalog.create_table(default_database_id, make_schema_836("t_dup", cols));

    ASSERT_FALSE(result.has_value()) << "Duplicate column CREATE TABLE must be rejected";
    EXPECT_EQ(result.error().code, StatusCode::ALREADY_EXISTS);
    EXPECT_NE(result.error().message.find("dup_col"), std::string::npos)
        << "Error must name the duplicate column; got: " << result.error().message;
}

/// AC3 (no orphan): a rejected CREATE TABLE must NOT register the table name.
/// The same name must be reusable immediately after rejection.
TEST(QA_GDB836_DupColValidation, RejectedTableNameIsReusableAfterwards) {
    Catalog catalog;
    bootstrap_qa_catalog(catalog);

    // First attempt: duplicate columns — should be rejected.
    std::vector<CatalogColumnDef> bad_cols;
    bad_cols.push_back(make_col_836(0, "x", TypeId::INT32));
    bad_cols.push_back(make_col_836(1, "x", TypeId::INT64));
    auto bad_result =
        catalog.create_table(default_database_id, make_schema_836("my_table", bad_cols));
    ASSERT_FALSE(bad_result.has_value()) << "Duplicate column schema must be rejected";

    // Second attempt with the SAME table name but valid (distinct) columns.
    std::vector<CatalogColumnDef> good_cols;
    good_cols.push_back(make_col_836(0, "x", TypeId::INT32));
    good_cols.push_back(make_col_836(1, "y", TypeId::INT64));
    auto good_result =
        catalog.create_table(default_database_id, make_schema_836("my_table", good_cols));

    ASSERT_TRUE(good_result.has_value())
        << "Table name must be available after rejected CREATE; error: "
        << (good_result.has_value() ? "" : good_result.error().message);
}

/// AC3 (no orphan table id): get_table_by_id must not find any table id
/// after a rejected CREATE (catalog internal map not polluted).
TEST(QA_GDB836_DupColValidation, RejectedCreateLeavesNoDanglingTableId) {
    Catalog catalog;
    bootstrap_qa_catalog(catalog);

    // Record how many tables exist before the bad CREATE.
    auto before = catalog.list_tables(default_database_id);

    std::vector<CatalogColumnDef> cols;
    cols.push_back(make_col_836(0, "col", TypeId::INT32));
    cols.push_back(make_col_836(1, "col", TypeId::STRING));
    auto bad = catalog.create_table(default_database_id, make_schema_836("orphan_test", cols));
    ASSERT_FALSE(bad.has_value());

    // Table count must be unchanged.
    auto after = catalog.list_tables(default_database_id);
    EXPECT_EQ(before.size(), after.size())
        << "A rejected CREATE TABLE must not increase the table count";

    // "orphan_test" must not be discoverable by name.
    auto by_name = catalog.get_table(default_database_id, "orphan_test");
    EXPECT_FALSE(by_name.has_value()) << "Rejected table must not appear in get_table lookup";
}

/// Duplicate of the FIRST and LAST column (wide schema, dup at position 0 and N-1).
TEST(QA_GDB836_DupColValidation, DuplicateFirstAndLastColumn) {
    Catalog catalog;
    bootstrap_qa_catalog(catalog);

    std::vector<CatalogColumnDef> cols;
    cols.push_back(make_col_836(0, "anchor", TypeId::INT32)); // first
    cols.push_back(make_col_836(1, "mid1", TypeId::STRING));
    cols.push_back(make_col_836(2, "mid2", TypeId::FLOAT64));
    cols.push_back(make_col_836(3, "mid3", TypeId::BOOL));
    cols.push_back(make_col_836(4, "anchor", TypeId::INT64)); // last = dup of first

    auto result =
        catalog.create_table(default_database_id, make_schema_836("first_last_dup", cols));
    ASSERT_FALSE(result.has_value()) << "Duplicate of first and last column must be rejected";
    EXPECT_EQ(result.error().code, StatusCode::ALREADY_EXISTS);
}

/// All columns have the same name (pathological: N identical names).
TEST(QA_GDB836_DupColValidation, AllColumnsIdenticalNames) {
    Catalog catalog;
    bootstrap_qa_catalog(catalog);

    std::vector<CatalogColumnDef> cols;
    for (int i = 0; i < 5; ++i) {
        cols.push_back(make_col_836(i, "same", TypeId::INT32));
    }

    auto result = catalog.create_table(default_database_id, make_schema_836("all_same", cols));
    ASSERT_FALSE(result.has_value()) << "Five columns all named 'same' must be rejected";
    EXPECT_EQ(result.error().code, StatusCode::ALREADY_EXISTS);
}

/// Case-insensitive: mixed-case variants of the same identifier.
/// "Col", "COL", "col" should all collide.
TEST(QA_GDB836_DupColValidation, CaseInsensitiveMixedVariants) {
    Catalog catalog;
    bootstrap_qa_catalog(catalog);

    std::vector<CatalogColumnDef> cols;
    cols.push_back(make_col_836(0, "Col", TypeId::INT32));
    cols.push_back(make_col_836(1, "COL", TypeId::STRING));

    auto result =
        catalog.create_table(default_database_id, make_schema_836("mixed_case_dup", cols));
    ASSERT_FALSE(result.has_value())
        << "'Col' and 'COL' are the same identifier and must be rejected";
    EXPECT_EQ(result.error().code, StatusCode::ALREADY_EXISTS);
}

/// Positive guard: two columns that are genuinely distinct (differ in more than case)
/// must not be rejected.
TEST(QA_GDB836_DupColValidation, GenuinelyDistinctColumnNamesAccepted) {
    Catalog catalog;
    bootstrap_qa_catalog(catalog);

    std::vector<CatalogColumnDef> cols;
    cols.push_back(make_col_836(0, "col_a", TypeId::INT32));
    cols.push_back(make_col_836(1, "col_b", TypeId::STRING));
    cols.push_back(make_col_836(2, "col_c", TypeId::FLOAT64));
    cols.push_back(make_col_836(3, "col_d", TypeId::BOOL));

    auto result = catalog.create_table(default_database_id, make_schema_836("valid_table", cols));
    ASSERT_TRUE(result.has_value()) << "Distinct column names must be accepted; error: "
                                    << (result.has_value() ? "" : result.error().message);

    // Table should be findable.
    auto t = catalog.get_table(default_database_id, "valid_table");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->columns.size(), 4u);
}

/// Error message quality: must identify which column is the duplicate,
/// and must include context (table name or column name).
TEST(QA_GDB836_DupColValidation, ErrorMessageNamesTheDuplicateColumn) {
    Catalog catalog;
    bootstrap_qa_catalog(catalog);

    std::vector<CatalogColumnDef> cols;
    cols.push_back(make_col_836(0, "alpha", TypeId::INT32));
    cols.push_back(make_col_836(1, "beta", TypeId::STRING));
    cols.push_back(make_col_836(2, "alpha", TypeId::FLOAT64)); // dup

    auto result = catalog.create_table(default_database_id, make_schema_836("err_msg_table", cols));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::ALREADY_EXISTS);

    // The message should name the duplicate column "alpha".
    const auto& msg = result.error().message;
    EXPECT_NE(msg.find("alpha"), std::string::npos)
        << "Error message must name the duplicate column 'alpha'; got: " << msg;
    EXPECT_FALSE(msg.empty()) << "Error message must not be empty";
}

/// Single-column table: can never have a duplicate; must succeed.
TEST(QA_GDB836_DupColValidation, SingleColumnNoDuplicate) {
    Catalog catalog;
    bootstrap_qa_catalog(catalog);

    std::vector<CatalogColumnDef> cols;
    cols.push_back(make_col_836(0, "id", TypeId::INT32));

    auto result = catalog.create_table(default_database_id, make_schema_836("single_col", cols));
    ASSERT_TRUE(result.has_value()) << "Single-column table must be accepted";
}

/// Zero-column table: no columns => no possible duplicate; must succeed
/// (or at least not fail due to the dup-detection loop).
TEST(QA_GDB836_DupColValidation, ZeroColumnNoDuplicate) {
    Catalog catalog;
    bootstrap_qa_catalog(catalog);

    // No columns at all.
    auto result = catalog.create_table(default_database_id, make_schema_836("zero_col"));
    // The duplicate check must not crash or spuriously reject a zero-column schema.
    // We do not prescribe success vs. failure (no-column tables may be illegal),
    // but the error code must NOT be ALREADY_EXISTS.
    if (!result.has_value()) {
        EXPECT_NE(result.error().code, StatusCode::ALREADY_EXISTS)
            << "Zero-column rejection must not be mistakenly attributed to duplicate columns";
    }
}

/// Stress: many columns where only the very last two are duplicates.
/// The O(n^2) scan must still catch it.
TEST(QA_GDB836_DupColValidation, DuplicateDeepInLargeColumnList) {
    Catalog catalog;
    bootstrap_qa_catalog(catalog);

    std::vector<CatalogColumnDef> cols;
    // 98 unique columns.
    for (int i = 0; i < 98; ++i) {
        cols.push_back(make_col_836(i, "col_" + std::to_string(i), TypeId::INT32));
    }
    // Two more with the same name as the very first one.
    cols.push_back(make_col_836(98, "col_0", TypeId::STRING)); // dup of index 0

    auto result = catalog.create_table(default_database_id, make_schema_836("deep_dup", cols));
    ASSERT_FALSE(result.has_value())
        << "Duplicate at column index 98 (matching index 0) must be caught";
    EXPECT_EQ(result.error().code, StatusCode::ALREADY_EXISTS);
}
