#include "sixseven/common/types.h"

#include <gtest/gtest.h>

using namespace sixseven;

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

// -- parse_type_id --------------------------------------------------------------
// GDB-1220: parse_type_id is the canonical-name parser used directly by the
// graph edge-property deserializer (src/graph/graph_engine.cpp). Unlike the
// SQL DDL type resolver (planner/type_resolver.cpp), it must accept ONLY the
// canonical type names below and must NOT accept SQL aliases (INT, SMALLINT,
// BOOLEAN, VARCHAR, etc.). These tests lock that domain so a future
// consolidation of the two type-name maps cannot silently let SQL aliases
// leak into graph edge-property persistence.

TEST(ParseTypeId, AcceptsEveryCanonicalName) {
    EXPECT_EQ(parse_type_id("INT8"), TypeId::INT8);
    EXPECT_EQ(parse_type_id("INT16"), TypeId::INT16);
    EXPECT_EQ(parse_type_id("INT32"), TypeId::INT32);
    EXPECT_EQ(parse_type_id("INT64"), TypeId::INT64);
    EXPECT_EQ(parse_type_id("UINT8"), TypeId::UINT8);
    EXPECT_EQ(parse_type_id("UINT16"), TypeId::UINT16);
    EXPECT_EQ(parse_type_id("UINT32"), TypeId::UINT32);
    EXPECT_EQ(parse_type_id("UINT64"), TypeId::UINT64);
    EXPECT_EQ(parse_type_id("FLOAT32"), TypeId::FLOAT32);
    EXPECT_EQ(parse_type_id("FLOAT64"), TypeId::FLOAT64);
    EXPECT_EQ(parse_type_id("DECIMAL"), TypeId::DECIMAL);
    EXPECT_EQ(parse_type_id("BOOL"), TypeId::BOOL);
    EXPECT_EQ(parse_type_id("STRING"), TypeId::STRING);
    EXPECT_EQ(parse_type_id("BLOB"), TypeId::BLOB);
    EXPECT_EQ(parse_type_id("DATE"), TypeId::DATE);
    EXPECT_EQ(parse_type_id("TIME"), TypeId::TIME);
    EXPECT_EQ(parse_type_id("TIMESTAMP"), TypeId::TIMESTAMP);
    EXPECT_EQ(parse_type_id("INTERVAL"), TypeId::INTERVAL);
    EXPECT_EQ(parse_type_id("POINT"), TypeId::POINT);
    EXPECT_EQ(parse_type_id("JSON"), TypeId::JSON);
    EXPECT_EQ(parse_type_id("UUID"), TypeId::UUID);
    EXPECT_EQ(parse_type_id("EMBEDDING"), TypeId::EMBEDDING);
    EXPECT_EQ(parse_type_id("PATH"), TypeId::PATH);
}

TEST(ParseTypeId, IsCaseInsensitiveForCanonicalNames) {
    EXPECT_EQ(parse_type_id("int32"), TypeId::INT32);
    EXPECT_EQ(parse_type_id("String"), TypeId::STRING);
    EXPECT_EQ(parse_type_id("bOoL"), TypeId::BOOL);
}

TEST(ParseTypeId, RejectsSqlAliases) {
    // These are valid in the SQL DDL type resolver (type_name_map) but must
    // NOT be accepted by the canonical graph-facing parser.
    EXPECT_FALSE(parse_type_id("INT").has_value());
    EXPECT_FALSE(parse_type_id("INTEGER").has_value());
    EXPECT_FALSE(parse_type_id("TINYINT").has_value());
    EXPECT_FALSE(parse_type_id("SMALLINT").has_value());
    EXPECT_FALSE(parse_type_id("BIGINT").has_value());
    EXPECT_FALSE(parse_type_id("FLOAT").has_value());
    EXPECT_FALSE(parse_type_id("REAL").has_value());
    EXPECT_FALSE(parse_type_id("DOUBLE").has_value());
    EXPECT_FALSE(parse_type_id("DOUBLE PRECISION").has_value());
    EXPECT_FALSE(parse_type_id("NUMERIC").has_value());
    EXPECT_FALSE(parse_type_id("BOOLEAN").has_value());
    EXPECT_FALSE(parse_type_id("TEXT").has_value());
    EXPECT_FALSE(parse_type_id("VARCHAR").has_value());
    EXPECT_FALSE(parse_type_id("CHAR").has_value());
    EXPECT_FALSE(parse_type_id("CHARACTER VARYING").has_value());
    EXPECT_FALSE(parse_type_id("BYTEA").has_value());
    EXPECT_FALSE(parse_type_id("JSONB").has_value());
}

TEST(ParseTypeId, RejectsUnknownName) {
    EXPECT_FALSE(parse_type_id("NOT_A_TYPE").has_value());
    EXPECT_FALSE(parse_type_id("").has_value());
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
