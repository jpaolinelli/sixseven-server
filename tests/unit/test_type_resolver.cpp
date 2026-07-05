#include "sixseven/common/status.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/type_resolver.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace sixseven;

// =============================================================================
// resolve_type_spec
// =============================================================================

static TypeSpec make_spec(const std::string& name) {
    TypeSpec spec;
    spec.name = name;
    return spec;
}

TEST(TypeResolver, ResolveInt) {
    EXPECT_EQ(*resolve_type_spec(make_spec("INT")), TypeId::INT32);
}

TEST(TypeResolver, ResolveInteger) {
    EXPECT_EQ(*resolve_type_spec(make_spec("INTEGER")), TypeId::INT32);
}

TEST(TypeResolver, ResolveInt32) {
    EXPECT_EQ(*resolve_type_spec(make_spec("INT32")), TypeId::INT32);
}

TEST(TypeResolver, ResolveVarchar) {
    EXPECT_EQ(*resolve_type_spec(make_spec("VARCHAR")), TypeId::STRING);
}

TEST(TypeResolver, ResolveText) {
    EXPECT_EQ(*resolve_type_spec(make_spec("TEXT")), TypeId::STRING);
}

TEST(TypeResolver, ResolveString) {
    EXPECT_EQ(*resolve_type_spec(make_spec("STRING")), TypeId::STRING);
}

TEST(TypeResolver, ResolveBool) {
    EXPECT_EQ(*resolve_type_spec(make_spec("BOOL")), TypeId::BOOL);
}

TEST(TypeResolver, ResolveBoolean) {
    EXPECT_EQ(*resolve_type_spec(make_spec("BOOLEAN")), TypeId::BOOL);
}

TEST(TypeResolver, ResolveJsonb) {
    EXPECT_EQ(*resolve_type_spec(make_spec("JSONB")), TypeId::JSON);
}

TEST(TypeResolver, ResolveBytea) {
    EXPECT_EQ(*resolve_type_spec(make_spec("BYTEA")), TypeId::BLOB);
}

TEST(TypeResolver, ResolveNumeric) {
    EXPECT_EQ(*resolve_type_spec(make_spec("NUMERIC")), TypeId::DECIMAL);
}

TEST(TypeResolver, ResolveCaseInsensitive) {
    EXPECT_EQ(*resolve_type_spec(make_spec("int")), TypeId::INT32);
    EXPECT_EQ(*resolve_type_spec(make_spec("Text")), TypeId::STRING);
    EXPECT_EQ(*resolve_type_spec(make_spec("bOOL")), TypeId::BOOL);
}

// GDB-1220: exhaustive name -> TypeId table locking the SQL DDL path's
// accepted domain (canonical names layered with SQL aliases) after
// type_name_map() was rebuilt on top of parse_type_id's canonical set. Every
// canonical name from common/types.h::type_name must still resolve, and every
// pre-existing SQL alias must still resolve to the same TypeId as before.
TEST(TypeResolver, ResolveAllCanonicalAndAliasNames) {
    static const std::vector<std::pair<std::string, TypeId>> cases = {
        // Canonical names (must match parse_type_id's domain exactly).
        {"INT8", TypeId::INT8},
        {"INT16", TypeId::INT16},
        {"INT32", TypeId::INT32},
        {"INT64", TypeId::INT64},
        {"UINT8", TypeId::UINT8},
        {"UINT16", TypeId::UINT16},
        {"UINT32", TypeId::UINT32},
        {"UINT64", TypeId::UINT64},
        {"FLOAT32", TypeId::FLOAT32},
        {"FLOAT64", TypeId::FLOAT64},
        {"DECIMAL", TypeId::DECIMAL},
        {"BOOL", TypeId::BOOL},
        {"STRING", TypeId::STRING},
        {"BLOB", TypeId::BLOB},
        {"DATE", TypeId::DATE},
        {"TIME", TypeId::TIME},
        {"TIMESTAMP", TypeId::TIMESTAMP},
        {"INTERVAL", TypeId::INTERVAL},
        {"POINT", TypeId::POINT},
        {"JSON", TypeId::JSON},
        {"UUID", TypeId::UUID},
        {"EMBEDDING", TypeId::EMBEDDING},
        {"PATH", TypeId::PATH},
        // SQL-only aliases (not accepted by parse_type_id).
        {"TINYINT", TypeId::INT8},
        {"SMALLINT", TypeId::INT16},
        {"INT", TypeId::INT32},
        {"INTEGER", TypeId::INT32},
        {"BIGINT", TypeId::INT64},
        {"FLOAT", TypeId::FLOAT32},
        {"REAL", TypeId::FLOAT32},
        {"DOUBLE", TypeId::FLOAT64},
        {"DOUBLE PRECISION", TypeId::FLOAT64},
        {"NUMERIC", TypeId::DECIMAL},
        {"BOOLEAN", TypeId::BOOL},
        {"TEXT", TypeId::STRING},
        {"VARCHAR", TypeId::STRING},
        {"CHAR", TypeId::STRING},
        {"CHARACTER VARYING", TypeId::STRING},
        {"BYTEA", TypeId::BLOB},
        {"JSONB", TypeId::JSON},
    };

    for (const auto& [name, expected] : cases) {
        auto result = resolve_type_spec(make_spec(name));
        ASSERT_TRUE(result.has_value()) << "name: " << name;
        EXPECT_EQ(*result, expected) << "name: " << name;
    }
}

TEST(TypeResolver, ResolveUnknownTypeReturnsError) {
    auto result = resolve_type_spec(make_spec("FOOBAR"));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// =============================================================================
// common_type
// =============================================================================

TEST(TypeResolver, CommonTypeSame) {
    auto result = common_type(TypeId::INT32, TypeId::INT32);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, TypeId::INT32);
}

TEST(TypeResolver, CommonTypeInt32ToInt64Widens) {
    // INT32 can coerce to INT64, so common type should be INT64.
    auto result = common_type(TypeId::INT32, TypeId::INT64);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, TypeId::INT64);
}

TEST(TypeResolver, CommonTypeInt64ToInt32Widens) {
    auto result = common_type(TypeId::INT64, TypeId::INT32);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, TypeId::INT64);
}

TEST(TypeResolver, CommonTypeNoCommon) {
    auto result = common_type(TypeId::STRING, TypeId::INT32);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// =============================================================================
// is_aggregate_function
// =============================================================================

TEST(TypeResolver, IsAggregateFunctionCount) {
    EXPECT_TRUE(is_aggregate_function("COUNT"));
    EXPECT_TRUE(is_aggregate_function("count"));
    EXPECT_TRUE(is_aggregate_function("Count"));
}

TEST(TypeResolver, IsAggregateFunctionRecognised) {
    EXPECT_TRUE(is_aggregate_function("SUM"));
    EXPECT_TRUE(is_aggregate_function("AVG"));
    EXPECT_TRUE(is_aggregate_function("MIN"));
    EXPECT_TRUE(is_aggregate_function("MAX"));
    EXPECT_TRUE(is_aggregate_function("STRING_AGG"));
}

TEST(TypeResolver, IsAggregateFunctionScalarReturnsFalse) {
    EXPECT_FALSE(is_aggregate_function("LOWER"));
    EXPECT_FALSE(is_aggregate_function("ABS"));
    EXPECT_FALSE(is_aggregate_function("UNKNOWN_FN"));
}

// =============================================================================
// aggregate_return_type
// =============================================================================

TEST(TypeResolver, AggregateReturnTypeCount) {
    auto result = aggregate_return_type("COUNT", TypeId::INT32);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, TypeId::INT64);
}

TEST(TypeResolver, AggregateReturnTypeSumInt) {
    auto result = aggregate_return_type("SUM", TypeId::INT32);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, TypeId::INT64);
}

TEST(TypeResolver, AggregateReturnTypeSumFloat) {
    auto result = aggregate_return_type("SUM", TypeId::FLOAT64);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, TypeId::FLOAT64);
}

TEST(TypeResolver, AggregateReturnTypeSumNonNumericError) {
    auto result = aggregate_return_type("SUM", TypeId::STRING);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(TypeResolver, AggregateReturnTypeAvgNumeric) {
    auto result = aggregate_return_type("AVG", TypeId::INT64);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, TypeId::FLOAT64);
}

TEST(TypeResolver, AggregateReturnTypeAvgNonNumericError) {
    auto result = aggregate_return_type("AVG", TypeId::BOOL);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(TypeResolver, AggregateReturnTypeMinMaxPassthrough) {
    auto min_result = aggregate_return_type("MIN", TypeId::INT32);
    ASSERT_TRUE(min_result.has_value()) << min_result.error().message;
    EXPECT_EQ(*min_result, TypeId::INT32);

    auto max_result = aggregate_return_type("MAX", TypeId::STRING);
    ASSERT_TRUE(max_result.has_value()) << max_result.error().message;
    EXPECT_EQ(*max_result, TypeId::STRING);
}

TEST(TypeResolver, AggregateReturnTypeMinMaxNonComparableError) {
    // BLOB is not comparable, so MIN(blob_col) should fail.
    auto result = aggregate_return_type("MIN", TypeId::BLOB);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(TypeResolver, AggregateReturnTypeStringAgg) {
    auto result = aggregate_return_type("STRING_AGG", TypeId::STRING);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, TypeId::STRING);
}

TEST(TypeResolver, AggregateReturnTypeUnknown) {
    auto result = aggregate_return_type("FOOAGG", TypeId::INT32);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// =============================================================================
// function_return_type
// =============================================================================

TEST(TypeResolver, FunctionReturnTypeStringFns) {
    std::vector<TypeId> args = {TypeId::STRING};
    EXPECT_EQ(*function_return_type("LOWER", args), TypeId::STRING);
    EXPECT_EQ(*function_return_type("UPPER", args), TypeId::STRING);
    EXPECT_EQ(*function_return_type("TRIM", args), TypeId::STRING);
    EXPECT_EQ(*function_return_type("REVERSE", args), TypeId::STRING);
}

TEST(TypeResolver, FunctionReturnTypeLengthInt64) {
    std::vector<TypeId> args = {TypeId::STRING};
    EXPECT_EQ(*function_return_type("LENGTH", args), TypeId::INT64);
    EXPECT_EQ(*function_return_type("CHAR_LENGTH", args), TypeId::INT64);
}

TEST(TypeResolver, FunctionReturnTypeAbsPassthrough) {
    std::vector<TypeId> args = {TypeId::FLOAT64};
    auto result = function_return_type("ABS", args);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, TypeId::FLOAT64);
}

TEST(TypeResolver, FunctionReturnTypeAbsNonNumericError) {
    std::vector<TypeId> args = {TypeId::STRING};
    auto result = function_return_type("ABS", args);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(TypeResolver, FunctionReturnTypeCeilFloat64) {
    std::vector<TypeId> args = {TypeId::FLOAT64};
    EXPECT_EQ(*function_return_type("CEIL", args), TypeId::FLOAT64);
    EXPECT_EQ(*function_return_type("FLOOR", args), TypeId::FLOAT64);
    EXPECT_EQ(*function_return_type("ROUND", args), TypeId::FLOAT64);
}

TEST(TypeResolver, FunctionReturnTypeNowTimestamp) {
    EXPECT_EQ(*function_return_type("NOW", {}), TypeId::TIMESTAMP);
    EXPECT_EQ(*function_return_type("CURRENT_TIMESTAMP", {}), TypeId::TIMESTAMP);
}

TEST(TypeResolver, FunctionReturnTypeCoalesceFirstArg) {
    std::vector<TypeId> args = {TypeId::INT32, TypeId::INT32};
    EXPECT_EQ(*function_return_type("COALESCE", args), TypeId::INT32);
}

TEST(TypeResolver, FunctionReturnTypeCoalesceZeroArgsError) {
    auto result = function_return_type("COALESCE", {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(TypeResolver, FunctionReturnTypeJsonFns) {
    EXPECT_EQ(*function_return_type("JSON_EXTRACT", {}), TypeId::JSON);
    EXPECT_EQ(*function_return_type("JSON_ARRAY", {}), TypeId::JSON);
    EXPECT_EQ(*function_return_type("JSON_OBJECT", {}), TypeId::JSON);
}

TEST(TypeResolver, FunctionReturnTypeUuidFns) {
    EXPECT_EQ(*function_return_type("GEN_UUID", {}), TypeId::UUID);
    EXPECT_EQ(*function_return_type("GEN_RANDOM_UUID", {}), TypeId::UUID);
    EXPECT_EQ(*function_return_type("UUID_GENERATE_V4", {}), TypeId::UUID);
}

TEST(TypeResolver, FunctionReturnTypeToCasts) {
    EXPECT_EQ(*function_return_type("TO_CHAR", {}), TypeId::STRING);
    EXPECT_EQ(*function_return_type("TO_NUMBER", {}), TypeId::FLOAT64);
    EXPECT_EQ(*function_return_type("TO_DATE", {}), TypeId::DATE);
    EXPECT_EQ(*function_return_type("TO_TIMESTAMP", {}), TypeId::TIMESTAMP);
}

TEST(TypeResolver, FunctionReturnTypePgReplicationFns) {
    EXPECT_EQ(*function_return_type("PG_CURRENT_WAL_LSN", {}), TypeId::INT64);
    EXPECT_EQ(*function_return_type("PG_IS_IN_RECOVERY", {}), TypeId::BOOL);
    EXPECT_EQ(*function_return_type("PG_LAST_WAL_REPLAY_LSN", {}), TypeId::INT64);
}

TEST(TypeResolver, FunctionReturnTypeUnknownError) {
    auto result = function_return_type("DOES_NOT_EXIST", {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}
