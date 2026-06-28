/// @file test_virtual_catalog_null_contract.cpp
/// @brief Unit tests for GDB-955: empty-string vs NULL contract in
/// VirtualCatalogScanOperator.
///
/// Verifies that:
///   - nullopt cells surface as SQL NULL (Value::is_null())
///   - present "" cells on a STRING column surface as Value("") -- NOT null
///   - present "42" cells on INT32 surface as Value(42)
///   - present non-numeric strings on an INT32 column fall back (safe_stoi
///     failure) to Value(s) -- the actual fallback behaviour documented
///   - short rows (fewer cells than columns) yield NULL for trailing columns

#include "sixseven/executor/virtual_catalog_scan.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace sixseven {

// ---------------------------------------------------------------------------
// Helper: build a VirtualTableDef + matching OutputSchema and run a scan.
// ---------------------------------------------------------------------------

static std::vector<Tuple> run_scan(VirtualTableDef def) {
    std::vector<OutputColumn> out_cols;
    for (const auto& col : def.columns) {
        out_cols.push_back({def.name, col.name, col.type_id, col.nullable, def.table_id});
    }
    OutputSchema schema(std::move(out_cols));

    VirtualCatalogScanOperator scan(std::move(def), std::move(schema));

    auto open_r = scan.open();
    if (!open_r) {
        ADD_FAILURE() << "open() failed: " << open_r.error().message;
        return {};
    }

    std::vector<Tuple> rows;
    while (true) {
        auto next_r = scan.next();
        if (!next_r) {
            ADD_FAILURE() << "next() failed: " << next_r.error().message;
            break;
        }
        if (!next_r->has_value()) {
            break;
        }
        rows.push_back(std::move(**next_r));
    }
    scan.close();
    return rows;
}

// ---------------------------------------------------------------------------
// AC1: nullopt cell -> SQL NULL
// ---------------------------------------------------------------------------

TEST(VirtualCatalogNullContract, NulloptCellIsNull) {
    VirtualTableDef def;
    def.table_id = -9001;
    def.name = "test_null";
    def.columns = {{0, "val", TypeId::STRING, true, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::nullopt}};
    };

    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].values.size(), 1u);
    EXPECT_TRUE(rows[0].values[0].is_null());
}

// ---------------------------------------------------------------------------
// AC2: present "" on STRING column -> Value("") (not null)
// ---------------------------------------------------------------------------

TEST(VirtualCatalogNullContract, PresentEmptyStringIsEmptyStringNotNull) {
    VirtualTableDef def;
    def.table_id = -9002;
    def.name = "test_empty_str";
    def.columns = {{0, "val", TypeId::STRING, true, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>("")}};
    };

    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].values.size(), 1u);
    const Value& v = rows[0].values[0];
    EXPECT_FALSE(v.is_null()) << "empty string should NOT be null under new contract";
    EXPECT_EQ(v.as_string(), "");
}

// ---------------------------------------------------------------------------
// AC3: present "42" on INT32 column -> Value(42)
// ---------------------------------------------------------------------------

TEST(VirtualCatalogNullContract, PresentNumericStringOnInt32IsInt) {
    VirtualTableDef def;
    def.table_id = -9003;
    def.name = "test_int";
    def.columns = {{0, "id", TypeId::INT32, false, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>("42")}};
    };

    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].values.size(), 1u);
    const Value& v = rows[0].values[0];
    EXPECT_FALSE(v.is_null());
    EXPECT_EQ(v.as_int32(), 42);
}

// ---------------------------------------------------------------------------
// AC4: present non-numeric string on INT32 -> safe_stoi fallback (Value(s))
// ---------------------------------------------------------------------------

TEST(VirtualCatalogNullContract, NonNumericStringOnInt32FallsBackToString) {
    VirtualTableDef def;
    def.table_id = -9004;
    def.name = "test_bad_int";
    def.columns = {{0, "id", TypeId::INT32, false, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>("not_a_number")}};
    };

    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].values.size(), 1u);
    const Value& v = rows[0].values[0];
    // safe_stoi fails -> Value("not_a_number"), not null
    EXPECT_FALSE(v.is_null());
    EXPECT_EQ(v.as_string(), "not_a_number");
}

// ---------------------------------------------------------------------------
// AC5: short row (fewer cells than columns) -> trailing columns are NULL
// ---------------------------------------------------------------------------

TEST(VirtualCatalogNullContract, ShortRowTrailingColumnsAreNull) {
    VirtualTableDef def;
    def.table_id = -9005;
    def.name = "test_short_row";
    def.columns = {
        {0, "a", TypeId::INT32, false, ""},
        {1, "b", TypeId::STRING, true, ""},
        {2, "c", TypeId::STRING, true, ""},
    };
    // Row has only one cell; columns b and c should be NULL.
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>("7")}};
    };

    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].values.size(), 3u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 7);
    EXPECT_TRUE(rows[0].values[1].is_null()) << "missing cell b should be NULL";
    EXPECT_TRUE(rows[0].values[2].is_null()) << "missing cell c should be NULL";
}

// ---------------------------------------------------------------------------
// AC6: mixed row -- null, empty, and real values together
// ---------------------------------------------------------------------------

TEST(VirtualCatalogNullContract, MixedNullEmptyAndRealCells) {
    VirtualTableDef def;
    def.table_id = -9006;
    def.name = "test_mixed";
    def.columns = {
        {0, "a", TypeId::STRING, true, ""},  // nullopt -> NULL
        {1, "b", TypeId::STRING, true, ""},  // "" -> ""
        {2, "c", TypeId::STRING, false, ""}, // "hello" -> "hello"
    };
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {
            {std::nullopt, std::optional<std::string>(""), std::optional<std::string>("hello")}};
    };

    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].values.size(), 3u);
    EXPECT_TRUE(rows[0].values[0].is_null());
    EXPECT_FALSE(rows[0].values[1].is_null());
    EXPECT_EQ(rows[0].values[1].as_string(), "");
    EXPECT_FALSE(rows[0].values[2].is_null());
    EXPECT_EQ(rows[0].values[2].as_string(), "hello");
}

} // namespace sixseven
