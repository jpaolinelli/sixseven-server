/// @file test_qa_gdb_610.cpp
/// @brief Adversarial QA tests for GDB-610: Case-insensitive OutputSchema::find_column
///
/// Tests cover: case-insensitive qualified/unqualified lookup, mixed-case
/// combinations, empty strings, Unicode-like edge cases, ambiguity with case
/// folding, single-char names, long names, and stress tests.

#include "sixseven/executor/tuple.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace sixseven;

// ============================================================================
// QA_GDB610_Unqualified — case-insensitive unqualified column lookup
// ============================================================================

TEST(QA_GDB610_Unqualified, AllLowercase) {
    OutputSchema schema({{"t", "score", TypeId::FLOAT64, false, 1}});
    auto idx = schema.find_column("score");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(QA_GDB610_Unqualified, AllUppercase) {
    OutputSchema schema({{"t", "score", TypeId::FLOAT64, false, 1}});
    auto idx = schema.find_column("SCORE");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(QA_GDB610_Unqualified, MixedCase_ScOrE) {
    OutputSchema schema({{"t", "score", TypeId::FLOAT64, false, 1}});
    auto idx = schema.find_column("ScOrE");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(QA_GDB610_Unqualified, SchemaStoredUppercase) {
    OutputSchema schema({{"T", "SCORE", TypeId::FLOAT64, false, 1}});
    auto idx = schema.find_column("score");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(QA_GDB610_Unqualified, SchemaStoredMixedCase) {
    OutputSchema schema({{"Table", "NaMe", TypeId::STRING, false, 1}});
    auto idx = schema.find_column("name");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);

    idx = schema.find_column("NAME");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);

    idx = schema.find_column("NaMe");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(QA_GDB610_Unqualified, EmptyName_NotFound) {
    OutputSchema schema({{"t", "score", TypeId::FLOAT64, false, 1}});
    EXPECT_FALSE(schema.find_column("").has_value());
}

TEST(QA_GDB610_Unqualified, EmptySchemaColumn_MatchesEmptySearch) {
    OutputSchema schema({{"t", "", TypeId::FLOAT64, false, 1}});
    auto idx = schema.find_column("");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(QA_GDB610_Unqualified, SingleCharNames) {
    OutputSchema schema({
        {"t", "a", TypeId::INT32, false, 1},
        {"t", "B", TypeId::INT32, false, 1},
        {"t", "c", TypeId::INT32, false, 1},
    });
    auto idx = schema.find_column("A");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);

    idx = schema.find_column("b");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 1);

    idx = schema.find_column("C");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 2);
}

TEST(QA_GDB610_Unqualified, AmbiguousAfterCaseFolding) {
    // "id" and "ID" stored in different tables — same name after folding → ambiguous
    OutputSchema schema({
        {"users", "id", TypeId::INT32, false, 1},
        {"orders", "ID", TypeId::INT32, false, 2},
    });
    EXPECT_FALSE(schema.find_column("id").has_value());
    EXPECT_FALSE(schema.find_column("ID").has_value());
    EXPECT_FALSE(schema.find_column("Id").has_value());
}

TEST(QA_GDB610_Unqualified, NameWithDigitsAndUnderscores) {
    OutputSchema schema({{"t", "col_123_ABC", TypeId::STRING, false, 1}});
    auto idx = schema.find_column("COL_123_abc");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);

    idx = schema.find_column("Col_123_Abc");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(QA_GDB610_Unqualified, SubstringNoMatch) {
    OutputSchema schema({{"t", "score", TypeId::FLOAT64, false, 1}});
    EXPECT_FALSE(schema.find_column("scor").has_value());
    EXPECT_FALSE(schema.find_column("scores").has_value());
    EXPECT_FALSE(schema.find_column("SCOR").has_value());
}

TEST(QA_GDB610_Unqualified, EmptySchema) {
    OutputSchema schema((std::vector<OutputColumn>{}));
    EXPECT_FALSE(schema.find_column("anything").has_value());
}

// ============================================================================
// QA_GDB610_Qualified — case-insensitive qualified (table.column) lookup
// ============================================================================

TEST(QA_GDB610_Qualified, BothLowercase) {
    OutputSchema schema({{"rated", "score", TypeId::FLOAT64, false, 1}});
    auto idx = schema.find_column("rated", "score");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(QA_GDB610_Qualified, BothUppercase) {
    OutputSchema schema({{"rated", "score", TypeId::FLOAT64, false, 1}});
    auto idx = schema.find_column("RATED", "SCORE");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(QA_GDB610_Qualified, TableUpperColumnLower) {
    OutputSchema schema({{"rated", "score", TypeId::FLOAT64, false, 1}});
    auto idx = schema.find_column("RATED", "score");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(QA_GDB610_Qualified, TableLowerColumnUpper) {
    OutputSchema schema({{"rated", "score", TypeId::FLOAT64, false, 1}});
    auto idx = schema.find_column("rated", "SCORE");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(QA_GDB610_Qualified, MixedCaseBoth) {
    OutputSchema schema({{"rated", "score", TypeId::FLOAT64, false, 1}});
    auto idx = schema.find_column("RaTeD", "sCoRe");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(QA_GDB610_Qualified, SchemaStoredUppercase) {
    OutputSchema schema({{"RATED", "SCORE", TypeId::FLOAT64, false, 1}});
    auto idx = schema.find_column("rated", "score");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(QA_GDB610_Qualified, WrongTable_SameColumn) {
    OutputSchema schema({
        {"users", "name", TypeId::STRING, false, 1},
        {"orders", "name", TypeId::STRING, false, 2},
    });
    auto idx = schema.find_column("USERS", "NAME");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);

    idx = schema.find_column("ORDERS", "NAME");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 1);

    EXPECT_FALSE(schema.find_column("products", "name").has_value());
}

TEST(QA_GDB610_Qualified, TableMatchColumnMismatch) {
    OutputSchema schema({{"t", "score", TypeId::FLOAT64, false, 1}});
    EXPECT_FALSE(schema.find_column("t", "MISSING").has_value());
    EXPECT_FALSE(schema.find_column("T", "MISSING").has_value());
}

TEST(QA_GDB610_Qualified, EmptyTableName) {
    OutputSchema schema({{"", "score", TypeId::FLOAT64, false, 1}});
    auto idx = schema.find_column("", "score");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);

    auto idx2 = schema.find_column("", "SCORE");
    ASSERT_TRUE(idx2.has_value());
    EXPECT_EQ(*idx2, 0);
}

TEST(QA_GDB610_Qualified, EmptyColumnName) {
    OutputSchema schema({{"t", "", TypeId::FLOAT64, false, 1}});
    auto idx = schema.find_column("t", "");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);

    auto idx2 = schema.find_column("T", "");
    ASSERT_TRUE(idx2.has_value());
    EXPECT_EQ(*idx2, 0);
}

TEST(QA_GDB610_Qualified, BothEmpty) {
    OutputSchema schema({{"", "", TypeId::FLOAT64, false, 1}});
    auto idx = schema.find_column("", "");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(QA_GDB610_Qualified, SingleCharTableAndColumn) {
    OutputSchema schema({{"a", "b", TypeId::INT32, false, 1}});
    auto idx = schema.find_column("A", "B");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);

    idx = schema.find_column("a", "B");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(QA_GDB610_Qualified, DigitsNotAffectedByCaseFolding) {
    OutputSchema schema({{"table1", "col2", TypeId::INT32, false, 1}});
    auto idx = schema.find_column("TABLE1", "COL2");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(QA_GDB610_Qualified, UnderscoresPreserved) {
    OutputSchema schema({{"my_table", "my_col", TypeId::INT32, false, 1}});
    auto idx = schema.find_column("MY_TABLE", "MY_COL");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

// ============================================================================
// QA_GDB610_Reproduction — reproduce the exact bug scenario from the ticket
// ============================================================================

TEST(QA_GDB610_Reproduction, EdgePropertyUppercaseColumn) {
    // Simulates: SELECT rated.SCORE — binder resolves but runtime find_column must too
    OutputSchema schema({{"rated", "score", TypeId::FLOAT64, false, 1}});
    auto idx = schema.find_column("rated", "SCORE");
    ASSERT_TRUE(idx.has_value()) << "rated.SCORE should resolve (uppercase column)";
    EXPECT_EQ(*idx, 0);
}

TEST(QA_GDB610_Reproduction, EdgePropertyUppercaseTable) {
    // Simulates: SELECT RATED.score
    OutputSchema schema({{"rated", "score", TypeId::FLOAT64, false, 1}});
    auto idx = schema.find_column("RATED", "score");
    ASSERT_TRUE(idx.has_value()) << "RATED.score should resolve (uppercase table)";
    EXPECT_EQ(*idx, 0);
}

TEST(QA_GDB610_Reproduction, EdgePropertyBothUppercase) {
    // Simulates: SELECT RATED.SCORE
    OutputSchema schema({{"rated", "score", TypeId::FLOAT64, false, 1}});
    auto idx = schema.find_column("RATED", "SCORE");
    ASSERT_TRUE(idx.has_value()) << "RATED.SCORE should resolve (both uppercase)";
    EXPECT_EQ(*idx, 0);
}

// ============================================================================
// QA_GDB610_Stress — many columns, lookup performance sanity
// ============================================================================

TEST(QA_GDB610_Stress, ManyColumns_CaseInsensitiveLookup) {
    std::vector<OutputColumn> cols;
    for (int i = 0; i < 1000; ++i) {
        cols.push_back({"table_" + std::to_string(i), "col_" + std::to_string(i),
                         TypeId::INT32, false, static_cast<table_id_t>(i)});
    }
    OutputSchema schema(std::move(cols));

    // Find last column with uppercase query
    auto idx = schema.find_column("TABLE_999", "COL_999");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 999);

    // Find first column with mixed case
    idx = schema.find_column("Table_0", "Col_0");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);

    // Find middle column
    idx = schema.find_column("TABLE_500", "COL_500");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 500);

    // Not found
    EXPECT_FALSE(schema.find_column("TABLE_1000", "COL_1000").has_value());
}

TEST(QA_GDB610_Stress, ManyColumns_UnqualifiedCaseInsensitive) {
    std::vector<OutputColumn> cols;
    for (int i = 0; i < 500; ++i) {
        // Each column in a unique table so unqualified is unambiguous
        cols.push_back({"t" + std::to_string(i), "unique_col_" + std::to_string(i),
                         TypeId::INT32, false, static_cast<table_id_t>(i)});
    }
    OutputSchema schema(std::move(cols));

    auto idx = schema.find_column("UNIQUE_COL_499");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 499);

    idx = schema.find_column("Unique_Col_0");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

// ============================================================================
// QA_GDB610_IEquals — edge cases in character comparison
// ============================================================================

TEST(QA_GDB610_IEquals, NonAlphaCharsUnaffected) {
    // Digits, underscores, hyphens should be compared as-is
    OutputSchema schema({{"t", "col-123_abc!", TypeId::STRING, false, 1}});
    auto idx = schema.find_column("col-123_ABC!");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);

    // Same special chars, wrong alpha case shouldn't matter
    idx = schema.find_column("COL-123_ABC!");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(QA_GDB610_IEquals, LengthMismatchRejects) {
    OutputSchema schema({{"t", "abc", TypeId::STRING, false, 1}});
    EXPECT_FALSE(schema.find_column("ab").has_value());
    EXPECT_FALSE(schema.find_column("abcd").has_value());
    EXPECT_FALSE(schema.find_column("AB").has_value());
    EXPECT_FALSE(schema.find_column("ABCD").has_value());
}
