#include "sixseven/executor/tuple.h"

#include <gtest/gtest.h>

using namespace sixseven;

// -- find_column (unqualified) ------------------------------------------------

TEST(OutputSchema, FindColumnUnqualified_ExactCase) {
    OutputSchema schema({
        {"users", "name", TypeId::STRING, false, 1},
        {"users", "age", TypeId::INT32, false, 1},
    });
    auto idx = schema.find_column("name");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(OutputSchema, FindColumnUnqualified_CaseInsensitive) {
    OutputSchema schema({
        {"users", "name", TypeId::STRING, false, 1},
        {"users", "age", TypeId::INT32, false, 1},
    });
    auto idx = schema.find_column("NAME");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);

    idx = schema.find_column("Age");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 1);
}

TEST(OutputSchema, FindColumnUnqualified_NotFound) {
    OutputSchema schema({
        {"users", "name", TypeId::STRING, false, 1},
    });
    EXPECT_FALSE(schema.find_column("missing").has_value());
}

TEST(OutputSchema, FindColumnUnqualified_Ambiguous) {
    OutputSchema schema({
        {"users", "id", TypeId::INT32, false, 1},
        {"orders", "id", TypeId::INT32, false, 2},
    });
    EXPECT_FALSE(schema.find_column("id").has_value());
    EXPECT_FALSE(schema.find_column("ID").has_value());
}

// -- find_column (qualified) --------------------------------------------------

TEST(OutputSchema, FindColumnQualified_ExactCase) {
    OutputSchema schema({
        {"users", "name", TypeId::STRING, false, 1},
        {"orders", "name", TypeId::STRING, false, 2},
    });
    auto idx = schema.find_column("users", "name");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);

    idx = schema.find_column("orders", "name");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 1);
}

TEST(OutputSchema, FindColumnQualified_CaseInsensitiveColumn) {
    OutputSchema schema({
        {"rated", "score", TypeId::FLOAT64, false, 1},
    });
    auto idx = schema.find_column("rated", "SCORE");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(OutputSchema, FindColumnQualified_CaseInsensitiveTable) {
    OutputSchema schema({
        {"rated", "score", TypeId::FLOAT64, false, 1},
    });
    auto idx = schema.find_column("RATED", "score");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(OutputSchema, FindColumnQualified_CaseInsensitiveBoth) {
    OutputSchema schema({
        {"rated", "score", TypeId::FLOAT64, false, 1},
    });
    auto idx = schema.find_column("RATED", "SCORE");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0);
}

TEST(OutputSchema, FindColumnQualified_NotFound) {
    OutputSchema schema({
        {"users", "name", TypeId::STRING, false, 1},
    });
    EXPECT_FALSE(schema.find_column("users", "missing").has_value());
    // When the table doesn't match but the column name is unique,
    // find_column falls back to column-name-only match (supports
    // alias.property for edge properties qualified by edge type name).
    EXPECT_TRUE(schema.find_column("missing", "name").has_value());
}
