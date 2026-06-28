#include "sixseven/cli/result_formatter.h"

#include <gtest/gtest.h>

using namespace sixseven::cli;

// ---------------------------------------------------------------------------
// Multi-column, multi-row table
// ---------------------------------------------------------------------------

TEST(CliResultFormatter, MultiColMultiRow) {
    std::vector<std::string> cols = {"id", "name"};
    std::vector<std::vector<std::optional<std::string>>> rows = {
        {"1", "alice"},
        {"2", "bob"},
    };
    auto out = format_result_table(cols, rows, "SELECT 2");

    // Header contains column names.
    EXPECT_NE(out.find("id"), std::string::npos);
    EXPECT_NE(out.find("name"), std::string::npos);
    // Separator line.
    EXPECT_NE(out.find("---"), std::string::npos);
    // Row data.
    EXPECT_NE(out.find("alice"), std::string::npos);
    EXPECT_NE(out.find("bob"), std::string::npos);
    // Row count.
    EXPECT_NE(out.find("(2 rows)"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Empty result (0 rows)
// ---------------------------------------------------------------------------

TEST(CliResultFormatter, ZeroRows) {
    std::vector<std::string> cols = {"id", "name"};
    std::vector<std::vector<std::optional<std::string>>> rows;
    auto out = format_result_table(cols, rows, "SELECT 0");

    EXPECT_NE(out.find("id"), std::string::npos);
    EXPECT_NE(out.find("(0 rows)"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Single row ("1 row" not "1 rows")
// ---------------------------------------------------------------------------

TEST(CliResultFormatter, SingleRowCount) {
    std::vector<std::string> cols = {"x"};
    std::vector<std::vector<std::optional<std::string>>> rows = {{"42"}};
    auto out = format_result_table(cols, rows, "SELECT 1");

    EXPECT_NE(out.find("(1 row)"), std::string::npos);
    EXPECT_EQ(out.find("(1 rows)"), std::string::npos); // Must NOT say "rows"
}

// ---------------------------------------------------------------------------
// NULL cell displayed as "NULL"
// ---------------------------------------------------------------------------

TEST(CliResultFormatter, NullCellDisplayed) {
    std::vector<std::string> cols = {"a", "b"};
    std::vector<std::vector<std::optional<std::string>>> rows = {
        {std::string("hello"), std::nullopt},
    };
    auto out = format_result_table(cols, rows, "SELECT 1");

    EXPECT_NE(out.find("hello"), std::string::npos);
    EXPECT_NE(out.find("NULL"), std::string::npos);
}

// ---------------------------------------------------------------------------
// No columns (command-only result like INSERT)
// ---------------------------------------------------------------------------

TEST(CliResultFormatter, NoColumnsCommandTag) {
    std::vector<std::string> cols;
    std::vector<std::vector<std::optional<std::string>>> rows;
    auto out = format_result_table(cols, rows, "INSERT 0 1");

    EXPECT_NE(out.find("INSERT 0 1"), std::string::npos);
    // No row count line when there are no columns.
    EXPECT_EQ(out.find("("), std::string::npos);
}

// ---------------------------------------------------------------------------
// Single column
// ---------------------------------------------------------------------------

TEST(CliResultFormatter, SingleColumn) {
    std::vector<std::string> cols = {"val"};
    std::vector<std::vector<std::optional<std::string>>> rows = {
        {"100"},
        {"200"},
        {"300"},
    };
    auto out = format_result_table(cols, rows, "SELECT 3");

    EXPECT_NE(out.find("val"), std::string::npos);
    EXPECT_NE(out.find("100"), std::string::npos);
    EXPECT_NE(out.find("200"), std::string::npos);
    EXPECT_NE(out.find("300"), std::string::npos);
    EXPECT_NE(out.find("(3 rows)"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Column width padded to header width
// ---------------------------------------------------------------------------

TEST(CliResultFormatter, ColumnWidthAtLeastHeaderWidth) {
    // Column name wider than values.
    std::vector<std::string> cols = {"very_long_column_name"};
    std::vector<std::vector<std::optional<std::string>>> rows = {{"x"}};
    auto out = format_result_table(cols, rows, "SELECT 1");

    // Header name must appear fully.
    EXPECT_NE(out.find("very_long_column_name"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Value wider than column name
// ---------------------------------------------------------------------------

TEST(CliResultFormatter, ValueWiderThanHeader) {
    std::vector<std::string> cols = {"x"};
    std::vector<std::vector<std::optional<std::string>>> rows = {
        {"this_is_a_very_long_value"},
    };
    auto out = format_result_table(cols, rows, "SELECT 1");
    EXPECT_NE(out.find("this_is_a_very_long_value"), std::string::npos);
}
