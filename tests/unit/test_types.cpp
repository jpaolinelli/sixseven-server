#include "giodb/common/types.h"

#include <gtest/gtest.h>

using namespace giodb;

// -- type_name ----------------------------------------------------------------

TEST(TypeId, TypeNameReturnsCorrectStrings) {
    EXPECT_EQ(type_name(TypeId::INT8), "INT8");
    EXPECT_EQ(type_name(TypeId::INT16), "INT16");
    EXPECT_EQ(type_name(TypeId::INT32), "INT32");
    EXPECT_EQ(type_name(TypeId::INT64), "INT64");
    EXPECT_EQ(type_name(TypeId::UINT8), "UINT8");
    EXPECT_EQ(type_name(TypeId::UINT16), "UINT16");
    EXPECT_EQ(type_name(TypeId::UINT32), "UINT32");
    EXPECT_EQ(type_name(TypeId::UINT64), "UINT64");
    EXPECT_EQ(type_name(TypeId::FLOAT32), "FLOAT32");
    EXPECT_EQ(type_name(TypeId::FLOAT64), "FLOAT64");
    EXPECT_EQ(type_name(TypeId::DECIMAL), "DECIMAL");
    EXPECT_EQ(type_name(TypeId::BOOL), "BOOL");
    EXPECT_EQ(type_name(TypeId::STRING), "STRING");
    EXPECT_EQ(type_name(TypeId::BLOB), "BLOB");
    EXPECT_EQ(type_name(TypeId::DATE), "DATE");
    EXPECT_EQ(type_name(TypeId::TIME), "TIME");
    EXPECT_EQ(type_name(TypeId::TIMESTAMP), "TIMESTAMP");
    EXPECT_EQ(type_name(TypeId::INTERVAL), "INTERVAL");
    EXPECT_EQ(type_name(TypeId::POINT), "POINT");
    EXPECT_EQ(type_name(TypeId::JSON), "JSON");
    EXPECT_EQ(type_name(TypeId::UUID), "UUID");
    EXPECT_EQ(type_name(TypeId::EMBEDDING), "EMBEDDING");
}

// -- fixed_size ---------------------------------------------------------------

TEST(TypeId, FixedSizeReturnsCorrectSizes) {
    // 1-byte types
    EXPECT_EQ(fixed_size(TypeId::INT8), 1U);
    EXPECT_EQ(fixed_size(TypeId::UINT8), 1U);
    EXPECT_EQ(fixed_size(TypeId::BOOL), 1U);

    // 2-byte types
    EXPECT_EQ(fixed_size(TypeId::INT16), 2U);
    EXPECT_EQ(fixed_size(TypeId::UINT16), 2U);

    // 4-byte types
    EXPECT_EQ(fixed_size(TypeId::INT32), 4U);
    EXPECT_EQ(fixed_size(TypeId::UINT32), 4U);
    EXPECT_EQ(fixed_size(TypeId::FLOAT32), 4U);
    EXPECT_EQ(fixed_size(TypeId::DATE), 4U);

    // 8-byte types
    EXPECT_EQ(fixed_size(TypeId::INT64), 8U);
    EXPECT_EQ(fixed_size(TypeId::UINT64), 8U);
    EXPECT_EQ(fixed_size(TypeId::FLOAT64), 8U);
    EXPECT_EQ(fixed_size(TypeId::TIME), 8U);
    EXPECT_EQ(fixed_size(TypeId::TIMESTAMP), 8U);

    // 16-byte types
    EXPECT_EQ(fixed_size(TypeId::DECIMAL), 16U);
    EXPECT_EQ(fixed_size(TypeId::INTERVAL), 16U);
    EXPECT_EQ(fixed_size(TypeId::POINT), 16U);
    EXPECT_EQ(fixed_size(TypeId::UUID), 16U);
}

TEST(TypeId, FixedSizeReturnsNulloptForVariableLength) {
    EXPECT_FALSE(fixed_size(TypeId::STRING).has_value());
    EXPECT_FALSE(fixed_size(TypeId::BLOB).has_value());
    EXPECT_FALSE(fixed_size(TypeId::JSON).has_value());
    EXPECT_FALSE(fixed_size(TypeId::EMBEDDING).has_value());
}

// -- alignment ----------------------------------------------------------------

TEST(TypeId, AlignmentValues) {
    EXPECT_EQ(alignment(TypeId::INT8), 1U);
    EXPECT_EQ(alignment(TypeId::BOOL), 1U);
    EXPECT_EQ(alignment(TypeId::UINT8), 1U);

    EXPECT_EQ(alignment(TypeId::INT16), 2U);
    EXPECT_EQ(alignment(TypeId::UINT16), 2U);

    EXPECT_EQ(alignment(TypeId::INT32), 4U);
    EXPECT_EQ(alignment(TypeId::UINT32), 4U);
    EXPECT_EQ(alignment(TypeId::FLOAT32), 4U);
    EXPECT_EQ(alignment(TypeId::DATE), 4U);

    EXPECT_EQ(alignment(TypeId::INT64), 8U);
    EXPECT_EQ(alignment(TypeId::UINT64), 8U);
    EXPECT_EQ(alignment(TypeId::FLOAT64), 8U);
    EXPECT_EQ(alignment(TypeId::TIME), 8U);
    EXPECT_EQ(alignment(TypeId::TIMESTAMP), 8U);
    EXPECT_EQ(alignment(TypeId::DECIMAL), 8U);
    EXPECT_EQ(alignment(TypeId::INTERVAL), 8U);
    EXPECT_EQ(alignment(TypeId::POINT), 8U);

    // UUID is a byte array — 1-byte aligned
    EXPECT_EQ(alignment(TypeId::UUID), 1U);

    // Variable-length types are pointer-aligned
    EXPECT_EQ(alignment(TypeId::STRING), 8U);
    EXPECT_EQ(alignment(TypeId::BLOB), 8U);
    EXPECT_EQ(alignment(TypeId::JSON), 8U);
    EXPECT_EQ(alignment(TypeId::EMBEDDING), 8U);
}

// -- is_numeric ---------------------------------------------------------------

TEST(TypeId, IsNumeric) {
    // All integer and floating types are numeric
    EXPECT_TRUE(is_numeric(TypeId::INT8));
    EXPECT_TRUE(is_numeric(TypeId::INT16));
    EXPECT_TRUE(is_numeric(TypeId::INT32));
    EXPECT_TRUE(is_numeric(TypeId::INT64));
    EXPECT_TRUE(is_numeric(TypeId::UINT8));
    EXPECT_TRUE(is_numeric(TypeId::UINT16));
    EXPECT_TRUE(is_numeric(TypeId::UINT32));
    EXPECT_TRUE(is_numeric(TypeId::UINT64));
    EXPECT_TRUE(is_numeric(TypeId::FLOAT32));
    EXPECT_TRUE(is_numeric(TypeId::FLOAT64));
    EXPECT_TRUE(is_numeric(TypeId::DECIMAL));

    // Non-numeric types
    EXPECT_FALSE(is_numeric(TypeId::BOOL));
    EXPECT_FALSE(is_numeric(TypeId::STRING));
    EXPECT_FALSE(is_numeric(TypeId::BLOB));
    EXPECT_FALSE(is_numeric(TypeId::DATE));
    EXPECT_FALSE(is_numeric(TypeId::TIME));
    EXPECT_FALSE(is_numeric(TypeId::TIMESTAMP));
    EXPECT_FALSE(is_numeric(TypeId::INTERVAL));
    EXPECT_FALSE(is_numeric(TypeId::POINT));
    EXPECT_FALSE(is_numeric(TypeId::JSON));
    EXPECT_FALSE(is_numeric(TypeId::UUID));
    EXPECT_FALSE(is_numeric(TypeId::EMBEDDING));
}

// -- is_integer ---------------------------------------------------------------

TEST(TypeId, IsInteger) {
    EXPECT_TRUE(is_integer(TypeId::INT8));
    EXPECT_TRUE(is_integer(TypeId::INT16));
    EXPECT_TRUE(is_integer(TypeId::INT32));
    EXPECT_TRUE(is_integer(TypeId::INT64));
    EXPECT_TRUE(is_integer(TypeId::UINT8));
    EXPECT_TRUE(is_integer(TypeId::UINT16));
    EXPECT_TRUE(is_integer(TypeId::UINT32));
    EXPECT_TRUE(is_integer(TypeId::UINT64));

    EXPECT_FALSE(is_integer(TypeId::FLOAT32));
    EXPECT_FALSE(is_integer(TypeId::FLOAT64));
    EXPECT_FALSE(is_integer(TypeId::DECIMAL));
    EXPECT_FALSE(is_integer(TypeId::BOOL));
    EXPECT_FALSE(is_integer(TypeId::STRING));
}

// -- is_floating --------------------------------------------------------------

TEST(TypeId, IsFloating) {
    EXPECT_TRUE(is_floating(TypeId::FLOAT32));
    EXPECT_TRUE(is_floating(TypeId::FLOAT64));

    EXPECT_FALSE(is_floating(TypeId::INT32));
    EXPECT_FALSE(is_floating(TypeId::DECIMAL));
    EXPECT_FALSE(is_floating(TypeId::STRING));
}

// -- is_comparable ------------------------------------------------------------

TEST(TypeId, IsComparable) {
    // Most types are comparable
    EXPECT_TRUE(is_comparable(TypeId::INT32));
    EXPECT_TRUE(is_comparable(TypeId::FLOAT64));
    EXPECT_TRUE(is_comparable(TypeId::STRING));
    EXPECT_TRUE(is_comparable(TypeId::BOOL));
    EXPECT_TRUE(is_comparable(TypeId::DATE));
    EXPECT_TRUE(is_comparable(TypeId::TIME));
    EXPECT_TRUE(is_comparable(TypeId::TIMESTAMP));
    EXPECT_TRUE(is_comparable(TypeId::INTERVAL));
    EXPECT_TRUE(is_comparable(TypeId::POINT));
    EXPECT_TRUE(is_comparable(TypeId::JSON));
    EXPECT_TRUE(is_comparable(TypeId::UUID));
    EXPECT_TRUE(is_comparable(TypeId::DECIMAL));

    // BLOB and EMBEDDING are not comparable
    EXPECT_FALSE(is_comparable(TypeId::BLOB));
    EXPECT_FALSE(is_comparable(TypeId::EMBEDDING));
}

// -- is_variable_length -------------------------------------------------------

TEST(TypeId, IsVariableLength) {
    EXPECT_TRUE(is_variable_length(TypeId::STRING));
    EXPECT_TRUE(is_variable_length(TypeId::BLOB));
    EXPECT_TRUE(is_variable_length(TypeId::JSON));
    EXPECT_TRUE(is_variable_length(TypeId::EMBEDDING));

    // All fixed types
    EXPECT_FALSE(is_variable_length(TypeId::INT8));
    EXPECT_FALSE(is_variable_length(TypeId::INT32));
    EXPECT_FALSE(is_variable_length(TypeId::FLOAT64));
    EXPECT_FALSE(is_variable_length(TypeId::BOOL));
    EXPECT_FALSE(is_variable_length(TypeId::DATE));
    EXPECT_FALSE(is_variable_length(TypeId::UUID));
    EXPECT_FALSE(is_variable_length(TypeId::DECIMAL));
    EXPECT_FALSE(is_variable_length(TypeId::POINT));
}
