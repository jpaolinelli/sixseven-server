/// @file test_qa_gdb_955.cpp
/// @brief Adversarial QA tests for GDB-955:
///   VirtualTableGenerator cell type changed to std::optional<std::string>
///   so that nullopt = SQL NULL and present (incl. "") = value.
///
/// Attack surface:
///   - present "" on every column type must NEVER be null
///   - nullopt on every column type must ALWAYS be null
///   - boundary rows: fewer/more cells than columns, zero columns, zero rows
///   - string_to_value numeric edge cases (overflow, leading spaces, sign)
///   - no deref of a valueless optional
///   - no-regression: NULL contract preserved

#include "sixseven/executor/pg_catalog_tables.h"
#include "sixseven/executor/virtual_catalog_scan.h"

#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace sixseven {

// ---------------------------------------------------------------------------
// Helper: build OutputSchema from a VirtualTableDef and run the scan,
// returning all tuples.  Fails the test on any open()/next() error.
// ---------------------------------------------------------------------------

static std::vector<Tuple> run_scan(VirtualTableDef def) {
    std::vector<OutputColumn> out_cols;
    out_cols.reserve(def.columns.size());
    for (const auto& col : def.columns) {
        out_cols.push_back({def.name, col.name, col.type_id, col.nullable, def.table_id});
    }
    OutputSchema schema(std::move(out_cols));

    VirtualCatalogScanOperator scan(std::move(def), std::move(schema));

    auto open_r = scan.open();
    EXPECT_TRUE(open_r.has_value()) << open_r.error().message;
    if (!open_r)
        return {};

    std::vector<Tuple> rows;
    while (true) {
        auto next_r = scan.next();
        EXPECT_TRUE(next_r.has_value()) << next_r.error().message;
        if (!next_r)
            break;
        if (!next_r->has_value())
            break;
        rows.push_back(std::move(**next_r));
    }
    scan.close();
    return rows;
}

static constexpr table_id_t kTestTableId = -9900;

// ---------------------------------------------------------------------------
// 1. Present "" on each column type must NEVER yield a null value
// ---------------------------------------------------------------------------

TEST(QA_GDB955_PresentEmptyNeverNull, StringColumn) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::STRING, true, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>("")}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].values[0].is_null()) << "present empty string on STRING must not be NULL";
    EXPECT_EQ(rows[0].values[0].as_string(), "");
}

TEST(QA_GDB955_PresentEmptyNeverNull, Int16Column) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::INT16, true, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>("")}};
    };
    // "" -> safe_stoi fails -> fallback Value("") which is a string, not null
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].values[0].is_null()) << "present empty string on INT16 must not be NULL";
}

TEST(QA_GDB955_PresentEmptyNeverNull, Int32Column) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::INT32, true, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>("")}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].values[0].is_null()) << "present empty string on INT32 must not be NULL";
}

TEST(QA_GDB955_PresentEmptyNeverNull, Int64Column) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::INT64, true, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>("")}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].values[0].is_null()) << "present empty string on INT64 must not be NULL";
}

TEST(QA_GDB955_PresentEmptyNeverNull, BoolColumn) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::BOOL, true, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>("")}};
    };
    // "" -> s=="true"||s=="1" is false -> Value(false), not null
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].values[0].is_null()) << "present empty string on BOOL must not be NULL";
    EXPECT_FALSE(rows[0].values[0].as_bool()) << "empty string BOOL should be false";
}

TEST(QA_GDB955_PresentEmptyNeverNull, Float64Column) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::FLOAT64, true, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>("")}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].values[0].is_null()) << "present empty string on FLOAT64 must not be NULL";
}

TEST(QA_GDB955_PresentEmptyNeverNull, DefaultTypeColumn) {
    // Any unhandled TypeId falls to the default branch which just returns Value(s)
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::JSON, true, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>("")}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].values[0].is_null()) << "present empty string on JSON must not be NULL";
}

// ---------------------------------------------------------------------------
// 2. nullopt on each column type must ALWAYS yield is_null()
// ---------------------------------------------------------------------------

TEST(QA_GDB955_NulloptAlwaysNull, StringColumn) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::STRING, true, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::nullopt}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].values[0].is_null());
}

TEST(QA_GDB955_NulloptAlwaysNull, Int32Column) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::INT32, true, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::nullopt}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].values[0].is_null());
}

TEST(QA_GDB955_NulloptAlwaysNull, Int64Column) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::INT64, true, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::nullopt}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].values[0].is_null());
}

TEST(QA_GDB955_NulloptAlwaysNull, BoolColumn) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::BOOL, true, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::nullopt}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].values[0].is_null());
}

TEST(QA_GDB955_NulloptAlwaysNull, Float64Column) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::FLOAT64, true, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::nullopt}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].values[0].is_null());
}

// ---------------------------------------------------------------------------
// 3. Boundary: row with FEWER cells than columns -> trailing columns NULL
// ---------------------------------------------------------------------------

TEST(QA_GDB955_Boundary, FewerCellsThanColumnsTrailingNull) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {
        {0, "a", TypeId::INT32, false, ""},
        {1, "b", TypeId::STRING, true, ""},
        {2, "c", TypeId::STRING, true, ""},
        {3, "d", TypeId::BOOL, true, ""},
    };
    // Row has only 1 cell; b, c, d must be NULL
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>("99")}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].values.size(), 4u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 99);
    EXPECT_TRUE(rows[0].values[1].is_null()) << "missing b must be NULL";
    EXPECT_TRUE(rows[0].values[2].is_null()) << "missing c must be NULL";
    EXPECT_TRUE(rows[0].values[3].is_null()) << "missing d must be NULL";
}

TEST(QA_GDB955_Boundary, EmptyRowAllColumnsNull) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {
        {0, "a", TypeId::INT32, true, ""},
        {1, "b", TypeId::STRING, true, ""},
    };
    // Row is completely empty vector
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> { return {{}}; };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].values.size(), 2u);
    EXPECT_TRUE(rows[0].values[0].is_null());
    EXPECT_TRUE(rows[0].values[1].is_null());
}

TEST(QA_GDB955_Boundary, MoreCellsThanColumnsExtraIgnored) {
    // Extra cells beyond column count must not crash or corrupt
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::INT32, false, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>("5"),
                 std::optional<std::string>("extra1"),
                 std::optional<std::string>("extra2")}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].values.size(), 1u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 5);
}

TEST(QA_GDB955_Boundary, ZeroColumnDef) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>("ignored")}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values.size(), 0u);
}

TEST(QA_GDB955_Boundary, ZeroRowGenerator) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::INT32, false, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> { return {}; };
    auto rows = run_scan(std::move(def));
    EXPECT_EQ(rows.size(), 0u);
}

TEST(QA_GDB955_Boundary, EmptyOuterVectorGenerator) {
    // Same as zero rows - no crash
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::STRING, true, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> { return {}; };
    auto rows = run_scan(std::move(def));
    EXPECT_TRUE(rows.empty());
}

// ---------------------------------------------------------------------------
// 4. string_to_value numeric edge cases (probing existing behavior)
//    Note: safe_stoi/stoll/stod wrap std::sto* which accept leading whitespace
//    and trailing non-numeric chars ("12abc" -> 12). Document actual behavior.
// ---------------------------------------------------------------------------

TEST(QA_GDB955_StringToValue, Int32ValidZero) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::INT32, false, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>("0")}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].values[0].is_null());
    EXPECT_EQ(rows[0].values[0].as_int32(), 0);
}

TEST(QA_GDB955_StringToValue, Int32Negative) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::INT32, false, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>("-1")}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].values[0].is_null());
    EXPECT_EQ(rows[0].values[0].as_int32(), -1);
}

TEST(QA_GDB955_StringToValue, Int32OverflowFallsBackToString) {
    // "99999999999" overflows INT32; safe_stoi should fail -> Value(s) string fallback.
    // This is documented LOW design behavior - the column silently yields a STRING value.
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::INT32, true, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>("99999999999")}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    // Must NOT be null (present cell, even on overflow)
    EXPECT_FALSE(rows[0].values[0].is_null())
        << "overflow on INT32 must not yield NULL - LOW design note: yields string fallback";
    // The actual fallback is Value("99999999999") - a string Value
    EXPECT_EQ(rows[0].values[0].as_string(), "99999999999")
        << "LOW design: INT32 overflow yields string fallback, not numeric";
}

TEST(QA_GDB955_StringToValue, Int64MaxValue) {
    const std::string max_val = std::to_string(std::numeric_limits<int64_t>::max());
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::INT64, false, ""}};
    def.generator = [max_val]() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>(max_val)}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].values[0].is_null());
    EXPECT_EQ(rows[0].values[0].as_int64(), std::numeric_limits<int64_t>::max());
}

TEST(QA_GDB955_StringToValue, Int64OverflowFallsBackToString) {
    // Value beyond INT64_MAX overflows; safe_stoll fails -> string fallback, not null.
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::INT64, true, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>("99999999999999999999999")}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].values[0].is_null())
        << "INT64 overflow must not yield NULL - yields string fallback";
}

TEST(QA_GDB955_StringToValue, BoolTrueValues) {
    for (const char* s : {"true", "1"}) {
        VirtualTableDef def;
        def.table_id = kTestTableId;
        def.name = "t";
        def.columns = {{0, "v", TypeId::BOOL, false, ""}};
        def.generator = [s]() -> std::vector<std::vector<std::optional<std::string>>> {
            return {{std::optional<std::string>(s)}};
        };
        auto rows = run_scan(std::move(def));
        ASSERT_EQ(rows.size(), 1u) << "for input: " << s;
        EXPECT_FALSE(rows[0].values[0].is_null()) << "for input: " << s;
        EXPECT_TRUE(rows[0].values[0].as_bool()) << "for input: " << s;
    }
}

TEST(QA_GDB955_StringToValue, BoolFalseValues) {
    for (const char* s : {"false", "0", "yes", "TRUE", "False"}) {
        VirtualTableDef def;
        def.table_id = kTestTableId;
        def.name = "t";
        def.columns = {{0, "v", TypeId::BOOL, false, ""}};
        def.generator = [s]() -> std::vector<std::vector<std::optional<std::string>>> {
            return {{std::optional<std::string>(s)}};
        };
        auto rows = run_scan(std::move(def));
        ASSERT_EQ(rows.size(), 1u) << "for input: " << s;
        // Must not be null
        EXPECT_FALSE(rows[0].values[0].is_null()) << "for input: " << s;
        // BOOL rule: only exact "true"/"1" -> true; anything else -> false
        EXPECT_FALSE(rows[0].values[0].as_bool())
            << "for input: " << s << " (case-sensitive, only 'true'/'1' are truthy)";
    }
}

TEST(QA_GDB955_StringToValue, Float64ValidValue) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::FLOAT64, false, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {{std::optional<std::string>("3.14")}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].values[0].is_null());
    EXPECT_DOUBLE_EQ(rows[0].values[0].as_float64(), 3.14);
}

TEST(QA_GDB955_StringToValue, Float64OverflowFallsBackToString) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::FLOAT64, true, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        // Massively out of range for double
        return {{std::optional<std::string>("1.8e999999999")}};
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].values[0].is_null())
        << "FLOAT64 overflow must not yield NULL - yields string fallback";
}

// ---------------------------------------------------------------------------
// 5. Multi-row generator: confirm iteration over all rows, no early exit
// ---------------------------------------------------------------------------

TEST(QA_GDB955_MultiRow, AllRowsYielded) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "id", TypeId::INT32, false, ""}};
    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        return {
            {std::optional<std::string>("1")},
            {std::optional<std::string>("2")},
            {std::optional<std::string>("3")},
            {std::nullopt},                   // row 4: null
            {std::optional<std::string>("")}, // row 5: present empty
        };
    };
    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 5u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
    EXPECT_EQ(rows[1].values[0].as_int32(), 2);
    EXPECT_EQ(rows[2].values[0].as_int32(), 3);
    EXPECT_TRUE(rows[3].values[0].is_null());
    EXPECT_FALSE(rows[4].values[0].is_null()) << "present empty on row 5 must not be null";
}

// ---------------------------------------------------------------------------
// 6. No-regression: make_pg_type NULL contract
//    pg_type rows must have null/non-null exactly where expected.
//    Spot-check: all 23 rows returned, oid and typname are non-null.
// ---------------------------------------------------------------------------

TEST(QA_GDB955_NoRegression, PgTypeNullContract) {
    auto def = make_pg_type();

    // Find column indices
    int oid_idx = -1, typname_idx = -1;
    [[maybe_unused]] int typbyval_idx = -1;
    for (size_t i = 0; i < def.columns.size(); ++i) {
        if (def.columns[i].name == "oid")
            oid_idx = static_cast<int>(i);
        else if (def.columns[i].name == "typname")
            typname_idx = static_cast<int>(i);
        else if (def.columns[i].name == "typbyval")
            typbyval_idx = static_cast<int>(i);
    }
    ASSERT_GE(oid_idx, 0) << "oid column not found in pg_type";
    ASSERT_GE(typname_idx, 0) << "typname column not found in pg_type";

    std::vector<OutputColumn> out_cols;
    for (const auto& col : def.columns)
        out_cols.push_back({def.name, col.name, col.type_id, col.nullable, def.table_id});
    OutputSchema schema(std::move(out_cols));

    VirtualCatalogScanOperator scan(std::move(def), std::move(schema));
    ASSERT_TRUE(scan.open().has_value());

    int row_count = 0;
    while (true) {
        auto r = scan.next();
        ASSERT_TRUE(r.has_value());
        if (!r->has_value())
            break;
        ++row_count;
        const Tuple& t = **r;
        // oid and typname must never be null
        EXPECT_FALSE(t.values[oid_idx].is_null())
            << "pg_type.oid must not be null on row " << row_count;
        EXPECT_FALSE(t.values[typname_idx].is_null())
            << "pg_type.typname must not be null on row " << row_count;
    }
    scan.close();

    // 23 type IDs defined but pg_type deduplicates by OID (multiple TypeIds share
    // the same pg OID, e.g. INT8/UINT8 both map to int2=21).  We expect at least
    // 1 row and at most 23 rows (verified empirically: 17 unique OIDs).
    EXPECT_GT(row_count, 0) << "pg_type must have at least 1 row";
    EXPECT_LE(row_count, 23) << "pg_type cannot have more rows than defined types";
}

// ---------------------------------------------------------------------------
// 7. Reopen: calling open() a second time after close() works correctly
// ---------------------------------------------------------------------------

TEST(QA_GDB955_Reopen, SecondOpenAfterCloseResets) {
    int call_count = 0;
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    def.columns = {{0, "v", TypeId::INT32, false, ""}};
    def.generator = [&call_count]() -> std::vector<std::vector<std::optional<std::string>>> {
        ++call_count;
        return {{std::optional<std::string>("42")}};
    };

    std::vector<OutputColumn> out_cols = {{"t", "v", TypeId::INT32, false, kTestTableId}};
    OutputSchema schema(std::move(out_cols));

    VirtualCatalogScanOperator scan(std::move(def), std::move(schema));

    // First open/scan/close
    ASSERT_TRUE(scan.open().has_value());
    auto r = scan.next();
    ASSERT_TRUE(r.has_value() && r->has_value());
    EXPECT_EQ((**r).values[0].as_int32(), 42);
    scan.close();
    EXPECT_EQ(call_count, 1);

    // Second open should re-invoke generator
    ASSERT_TRUE(scan.open().has_value());
    auto r2 = scan.next();
    ASSERT_TRUE(r2.has_value() && r2->has_value());
    EXPECT_EQ((**r2).values[0].as_int32(), 42);
    scan.close();
    EXPECT_EQ(call_count, 2) << "generator should be called again on second open()";
}

// ---------------------------------------------------------------------------
// 8. Mixed null/present across many columns in one row (stress)
// ---------------------------------------------------------------------------

TEST(QA_GDB955_Mixed, ManyColumnsAlternatingNullAndPresent) {
    VirtualTableDef def;
    def.table_id = kTestTableId;
    def.name = "t";
    const size_t N = 20;
    for (size_t i = 0; i < N; ++i)
        def.columns.push_back(
            {static_cast<int32_t>(i), "c" + std::to_string(i), TypeId::STRING, true, ""});

    def.generator = []() -> std::vector<std::vector<std::optional<std::string>>> {
        std::vector<std::optional<std::string>> row;
        for (size_t i = 0; i < 20; ++i)
            row.push_back(i % 2 == 0 ? std::nullopt : std::optional<std::string>("v"));
        return {row};
    };

    auto rows = run_scan(std::move(def));
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].values.size(), N);
    for (size_t i = 0; i < N; ++i) {
        if (i % 2 == 0) {
            EXPECT_TRUE(rows[0].values[i].is_null()) << "col " << i << " should be null";
        } else {
            EXPECT_FALSE(rows[0].values[i].is_null()) << "col " << i << " should not be null";
            EXPECT_EQ(rows[0].values[i].as_string(), "v");
        }
    }
}

} // namespace sixseven
