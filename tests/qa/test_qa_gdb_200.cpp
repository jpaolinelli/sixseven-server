/// @file test_qa_gdb_200.cpp
/// @brief QA adversarial tests for GDB-200: Parameter substitution in extended query Execute.

#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/server/connection.h"
#include "sixseven/server/pg_protocol.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <climits>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace sixseven;

// =============================================================================
// Helper: OID constants (mirroring src/server/pg_protocol.cpp)
// =============================================================================

namespace {

constexpr uint32_t OID_BOOL = 16;
constexpr uint32_t OID_INT2 = 21;
constexpr uint32_t OID_INT4 = 23;
constexpr uint32_t OID_INT8 = 20;
constexpr uint32_t OID_FLOAT4 = 700;
constexpr uint32_t OID_FLOAT8 = 701;
constexpr uint32_t OID_NUMERIC = 1700;
constexpr uint32_t OID_TEXT = 25;
constexpr uint32_t OID_DATE = 1082;
constexpr uint32_t OID_TIMESTAMP = 1114;
constexpr uint32_t OID_UUID = 2950;
constexpr uint32_t OID_JSON = 114;
constexpr uint32_t OID_BYTEA = 17;

} // namespace

// =============================================================================
// Boundary: Parameter number overflow ($99999999999 triggers std::stoi overflow)
// =============================================================================

TEST(QA_GDB200_Boundary, VeryLargeParamNumberDoesNotCrash) {
    // $99999999999 exceeds INT_MAX; std::stoi would throw std::out_of_range.
    // The function must return an error, not throw/crash.
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    EXPECT_NO_THROW({
        auto result = substitute_parameters("SELECT $99999999999", params, oids);
        // We don't care if it's an error or some other handling,
        // as long as it doesn't crash.
        if (result.has_value()) {
            // If it somehow succeeds, that's unexpected but not a crash.
        } else {
            EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
        }
    });
}

TEST(QA_GDB200_Boundary, ParamNumberIntMaxDoesNotCrash) {
    // A param number equal to INT_MAX.
    std::vector<std::optional<std::string>> params = {"1"};
    std::vector<uint32_t> oids = {OID_INT4};
    std::string sql = "SELECT $" + std::to_string(INT_MAX);
    EXPECT_NO_THROW({
        auto result = substitute_parameters(sql, params, oids);
        // Should be out of range since we only have 1 param.
        EXPECT_FALSE(result.has_value());
    });
}

TEST(QA_GDB200_Boundary, ParamNumberJustOverIntMaxDoesNotCrash) {
    // A param number exceeding INT_MAX by 1.
    std::vector<std::optional<std::string>> params = {"1"};
    std::vector<uint32_t> oids = {OID_INT4};
    std::string sql = "SELECT $" + std::to_string(static_cast<int64_t>(INT_MAX) + 1);
    EXPECT_NO_THROW({
        auto result = substitute_parameters(sql, params, oids);
        (void)result;
    });
}

// =============================================================================
// Boundary: Numeric literal validation edge cases
// =============================================================================

TEST(QA_GDB200_Boundary, NumericOidEmptyString) {
    std::vector<std::optional<std::string>> params = {""};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_FALSE(result.has_value()) << "Empty string should not pass as numeric";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB200_Boundary, NumericOidBareSign) {
    // Just a minus sign with no digits.
    std::vector<std::optional<std::string>> params = {"-"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_FALSE(result.has_value()) << "Bare minus sign should not pass as numeric";
}

TEST(QA_GDB200_Boundary, NumericOidBarePlus) {
    std::vector<std::optional<std::string>> params = {"+"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_FALSE(result.has_value()) << "Bare plus sign should not pass as numeric";
}

TEST(QA_GDB200_Boundary, NumericOidBareDot) {
    std::vector<std::optional<std::string>> params = {"."};
    std::vector<uint32_t> oids = {OID_FLOAT8};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_FALSE(result.has_value()) << "Bare dot should not pass as numeric";
}

TEST(QA_GDB200_Boundary, NumericOidLeadingDot) {
    // ".5" is a valid numeric literal.
    std::vector<std::optional<std::string>> params = {".5"};
    std::vector<uint32_t> oids = {OID_FLOAT8};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT .5");
}

TEST(QA_GDB200_Boundary, NumericOidTrailingDot) {
    // "5." is a valid numeric literal.
    std::vector<std::optional<std::string>> params = {"5."};
    std::vector<uint32_t> oids = {OID_FLOAT8};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT 5.");
}

TEST(QA_GDB200_Boundary, NumericOidScientificNegativeExponent) {
    std::vector<std::optional<std::string>> params = {"1.5e-10"};
    std::vector<uint32_t> oids = {OID_FLOAT8};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT 1.5e-10");
}

TEST(QA_GDB200_Boundary, NumericOidTruncatedExponent) {
    // "1.5e" — exponent with no digits → invalid.
    std::vector<std::optional<std::string>> params = {"1.5e"};
    std::vector<uint32_t> oids = {OID_FLOAT8};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_FALSE(result.has_value()) << "Truncated exponent should fail";
}

TEST(QA_GDB200_Boundary, NumericOidExponentSignOnly) {
    // "1e+" — exponent sign with no digits → invalid.
    std::vector<std::optional<std::string>> params = {"1e+"};
    std::vector<uint32_t> oids = {OID_FLOAT8};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_FALSE(result.has_value()) << "Exponent sign with no digits should fail";
}

TEST(QA_GDB200_Boundary, NumericOidNaN) {
    // NaN is a valid float value in IEEE 754 but NOT a numeric literal.
    std::vector<std::optional<std::string>> params = {"NaN"};
    std::vector<uint32_t> oids = {OID_FLOAT8};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_FALSE(result.has_value()) << "NaN should be rejected as numeric literal";
}

TEST(QA_GDB200_Boundary, NumericOidInfinity) {
    std::vector<std::optional<std::string>> params = {"Infinity"};
    std::vector<uint32_t> oids = {OID_FLOAT8};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_FALSE(result.has_value()) << "Infinity should be rejected as numeric literal";
}

TEST(QA_GDB200_Boundary, NumericOidNegativeInfinity) {
    std::vector<std::optional<std::string>> params = {"-Infinity"};
    std::vector<uint32_t> oids = {OID_FLOAT8};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_FALSE(result.has_value()) << "-Infinity should be rejected as numeric literal";
}

TEST(QA_GDB200_Boundary, NumericOidZero) {
    std::vector<std::optional<std::string>> params = {"0"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT 0");
}

TEST(QA_GDB200_Boundary, NumericOidNegativeZero) {
    std::vector<std::optional<std::string>> params = {"-0"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT -0");
}

TEST(QA_GDB200_Boundary, NumericOidLeadingZeros) {
    std::vector<std::optional<std::string>> params = {"007"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT 007");
}

TEST(QA_GDB200_Boundary, NumericOidWhitespace) {
    // A numeric value with leading/trailing whitespace should be rejected.
    std::vector<std::optional<std::string>> params = {" 42 "};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_FALSE(result.has_value()) << "Whitespace in numeric literal should be rejected";
}

TEST(QA_GDB200_Boundary, NumericOidWithTrailingText) {
    // "42abc" — numeric followed by non-numeric chars.
    std::vector<std::optional<std::string>> params = {"42abc"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_FALSE(result.has_value()) << "Trailing text after number should be rejected";
}

TEST(QA_GDB200_Boundary, AllNumericOidTypes) {
    // Verify all numeric OIDs accept valid numbers.
    uint32_t numeric_oids[] = {OID_INT2, OID_INT4, OID_INT8, OID_FLOAT4, OID_FLOAT8, OID_NUMERIC};
    for (auto oid : numeric_oids) {
        std::vector<std::optional<std::string>> params = {"123"};
        std::vector<uint32_t> oids = {oid};
        auto result = substitute_parameters("SELECT $1", params, oids);
        ASSERT_TRUE(result.has_value())
            << "OID " << oid << " should accept '123': " << result.error().message;
        EXPECT_EQ(*result, "SELECT 123") << "OID " << oid;
    }
}

// =============================================================================
// SQL injection adversarial tests
// =============================================================================

TEST(QA_GDB200_Injection, SingleQuoteInjectionInText) {
    std::vector<std::optional<std::string>> params = {"'; DROP TABLE users; --"};
    std::vector<uint32_t> oids = {OID_TEXT};
    auto result = substitute_parameters("SELECT * FROM t WHERE name = $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Quotes must be doubled.
    EXPECT_EQ(*result, "SELECT * FROM t WHERE name = '''; DROP TABLE users; --'");
}

TEST(QA_GDB200_Injection, NumericInjectionAttempt) {
    // Trying to inject via a numeric field.
    std::vector<std::optional<std::string>> params = {"1; DROP TABLE users"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT * FROM t WHERE id = $1", params, oids);
    ASSERT_FALSE(result.has_value()) << "Injection via numeric must be rejected";
}

TEST(QA_GDB200_Injection, NumericInjectionWithComment) {
    std::vector<std::optional<std::string>> params = {"1 --"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT * FROM t WHERE id = $1", params, oids);
    ASSERT_FALSE(result.has_value()) << "Injection via numeric with comment must be rejected";
}

TEST(QA_GDB200_Injection, BackslashEscapeInText) {
    // Backslash should NOT escape the closing quote; standard SQL uses '' for escaping.
    std::vector<std::optional<std::string>> params = {"test\\'"};
    std::vector<uint32_t> oids = {OID_TEXT};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // The single quote must be doubled, backslash passed through.
    EXPECT_EQ(*result, "SELECT 'test\\'''");
}

TEST(QA_GDB200_Injection, MultipleSingleQuotesInText) {
    std::vector<std::optional<std::string>> params = {"a''b'''c"};
    std::vector<uint32_t> oids = {OID_TEXT};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT 'a''''b''''''c'");
}

TEST(QA_GDB200_Injection, NullByteInText) {
    // Null bytes in string values — should be passed through without truncation.
    std::string val_with_null = std::string("hello\0world", 11);
    std::vector<std::optional<std::string>> params = {val_with_null};
    std::vector<uint32_t> oids = {OID_TEXT};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // The result should contain the null byte in the quoted string.
    std::string expected = std::string("SELECT 'hello\0world'", 20);
    EXPECT_EQ(*result, expected);
}

TEST(QA_GDB200_Injection, UnspecifiedOidDefaultsToQuoted) {
    // OID 0 means unspecified — must be treated as text (quoted), not numeric.
    std::vector<std::optional<std::string>> params = {"1; DROP TABLE users"};
    std::vector<uint32_t> oids = {0};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Must be quoted since OID 0 is not numeric.
    EXPECT_EQ(*result, "SELECT '1; DROP TABLE users'");
}

// =============================================================================
// Boolean parameter edge cases
// =============================================================================

TEST(QA_GDB200_Boolean, AllTrueVariants) {
    std::string true_values[] = {"t", "true", "TRUE", "1"};
    for (const auto& val : true_values) {
        std::vector<std::optional<std::string>> params = {val};
        std::vector<uint32_t> oids = {OID_BOOL};
        auto result = substitute_parameters("SELECT $1", params, oids);
        ASSERT_TRUE(result.has_value()) << "Value '" << val << "': " << result.error().message;
        EXPECT_EQ(*result, "SELECT TRUE") << "Value: " << val;
    }
}

TEST(QA_GDB200_Boolean, AllFalseVariants) {
    std::string false_values[] = {"f", "false", "FALSE", "0"};
    for (const auto& val : false_values) {
        std::vector<std::optional<std::string>> params = {val};
        std::vector<uint32_t> oids = {OID_BOOL};
        auto result = substitute_parameters("SELECT $1", params, oids);
        ASSERT_TRUE(result.has_value()) << "Value '" << val << "': " << result.error().message;
        EXPECT_EQ(*result, "SELECT FALSE") << "Value: " << val;
    }
}

TEST(QA_GDB200_Boolean, InvalidBoolValues) {
    std::string invalid[] = {"maybe", "yes", "no", "2", "-1", "T", "F", "True", "False", ""};
    for (const auto& val : invalid) {
        std::vector<std::optional<std::string>> params = {val};
        std::vector<uint32_t> oids = {OID_BOOL};
        auto result = substitute_parameters("SELECT $1", params, oids);
        ASSERT_FALSE(result.has_value()) << "Value '" << val << "' should be rejected as bool";
        EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    }
}

TEST(QA_GDB200_Boolean, NullBool) {
    std::vector<std::optional<std::string>> params = {std::nullopt};
    std::vector<uint32_t> oids = {OID_BOOL};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT NULL");
}

// =============================================================================
// Null handling
// =============================================================================

TEST(QA_GDB200_Null, AllNullParams) {
    std::vector<std::optional<std::string>> params = {std::nullopt, std::nullopt, std::nullopt};
    std::vector<uint32_t> oids = {OID_INT4, OID_TEXT, OID_BOOL};
    auto result =
        substitute_parameters("INSERT INTO t (a, b, c) VALUES ($1, $2, $3)", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "INSERT INTO t (a, b, c) VALUES (NULL, NULL, NULL)");
}

TEST(QA_GDB200_Null, NullWithVariousOids) {
    // NULL should produce "NULL" regardless of OID type.
    uint32_t oids_to_test[] = {OID_INT4, OID_TEXT, OID_BOOL, OID_FLOAT8, OID_DATE, OID_UUID, 0};
    for (auto oid : oids_to_test) {
        std::vector<std::optional<std::string>> params = {std::nullopt};
        std::vector<uint32_t> oids = {oid};
        auto result = substitute_parameters("SELECT $1", params, oids);
        ASSERT_TRUE(result.has_value()) << "OID " << oid << ": " << result.error().message;
        EXPECT_EQ(*result, "SELECT NULL") << "OID " << oid;
    }
}

// =============================================================================
// Dollar sign edge cases in SQL
// =============================================================================

TEST(QA_GDB200_Dollar, BareTrailingDollar) {
    // SQL ending with $ should not crash.
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT $");
}

TEST(QA_GDB200_Dollar, DollarFollowedByNonDigit) {
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $abc, $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT $abc, 42");
}

TEST(QA_GDB200_Dollar, DoubleDollarQuoting) {
    // PostgreSQL $$ dollar quoting — should not be parsed as param.
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $$literal$$, $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // $$ is not followed by a digit, so it's left as-is, but $1 is substituted.
    // Note: The inner $l is not a digit so it's left alone.
    // Actually $$literal$$ → $ + $ is not digit, left as-is.
    // The $1 should still be substituted.
    EXPECT_TRUE(result->find("42") != std::string::npos);
}

TEST(QA_GDB200_Dollar, DollarInsideSingleQuote) {
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT '$1' || $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT '$1' || 42");
}

TEST(QA_GDB200_Dollar, DollarInsideDoubleQuote) {
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT \"$1\", $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT \"$1\", 42");
}

TEST(QA_GDB200_Dollar, ParamInMixedQuoteContext) {
    // Single quote opened, double quote inside (not special), then close.
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result =
        substitute_parameters("SELECT 'he said \"$1\"' AS col, $1 AS val", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // $1 inside single quotes is NOT substituted; $1 outside IS.
    EXPECT_EQ(*result, "SELECT 'he said \"$1\"' AS col, 42 AS val");
}

// =============================================================================
// Parameter range and indexing
// =============================================================================

TEST(QA_GDB200_Range, ParamZero) {
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $0", params, oids);
    ASSERT_FALSE(result.has_value()) << "$0 must be rejected (1-indexed)";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB200_Range, ParamExceedsCount) {
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT $1, $2", params, oids);
    ASSERT_FALSE(result.has_value()) << "$2 with 1 param must be rejected";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB200_Range, ParamReusedMultipleTimes) {
    std::vector<std::optional<std::string>> params = {"hello"};
    std::vector<uint32_t> oids = {OID_TEXT};
    auto result = substitute_parameters("SELECT $1, $1, $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT 'hello', 'hello', 'hello'");
}

TEST(QA_GDB200_Range, ParamsOutOfOrder) {
    std::vector<std::optional<std::string>> params = {"first", "second"};
    std::vector<uint32_t> oids = {OID_TEXT, OID_TEXT};
    auto result = substitute_parameters("SELECT $2, $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT 'second', 'first'");
}

TEST(QA_GDB200_Range, MoreOidsThanParams) {
    // More OIDs than params shouldn't matter — only params count.
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4, OID_TEXT, OID_BOOL};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT 42");
}

TEST(QA_GDB200_Range, FewerOidsThanParams) {
    // Fewer OIDs than params — missing OIDs default to 0 (text).
    std::vector<std::optional<std::string>> params = {"42", "hello"};
    std::vector<uint32_t> oids = {OID_INT4}; // Only 1 OID for 2 params.
    auto result = substitute_parameters("SELECT $1, $2", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // $1 uses INT4 OID → unquoted; $2 has no OID → defaults to text (quoted).
    EXPECT_EQ(*result, "SELECT 42, 'hello'");
}

TEST(QA_GDB200_Range, EmptyOidsVector) {
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // No OIDs → default to text (quoted).
    EXPECT_EQ(*result, "SELECT '42'");
}

// =============================================================================
// Non-standard type OIDs
// =============================================================================

TEST(QA_GDB200_TypeOid, DateIsQuoted) {
    std::vector<std::optional<std::string>> params = {"2024-01-15"};
    std::vector<uint32_t> oids = {OID_DATE};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT '2024-01-15'");
}

TEST(QA_GDB200_TypeOid, TimestampIsQuoted) {
    std::vector<std::optional<std::string>> params = {"2024-01-15 10:30:00"};
    std::vector<uint32_t> oids = {OID_TIMESTAMP};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT '2024-01-15 10:30:00'");
}

TEST(QA_GDB200_TypeOid, UuidIsQuoted) {
    std::vector<std::optional<std::string>> params = {"550e8400-e29b-41d4-a716-446655440000"};
    std::vector<uint32_t> oids = {OID_UUID};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT '550e8400-e29b-41d4-a716-446655440000'");
}

TEST(QA_GDB200_TypeOid, JsonIsQuoted) {
    std::vector<std::optional<std::string>> params = {R"({"key": "value"})"};
    std::vector<uint32_t> oids = {OID_JSON};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, R"(SELECT '{"key": "value"}')");
}

TEST(QA_GDB200_TypeOid, ByteaIsQuoted) {
    std::vector<std::optional<std::string>> params = {"\\xDEADBEEF"};
    std::vector<uint32_t> oids = {OID_BYTEA};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT '\\xDEADBEEF'");
}

TEST(QA_GDB200_TypeOid, UnknownOidIsQuoted) {
    // An OID we don't recognize should default to quoted (safe).
    std::vector<std::optional<std::string>> params = {"arbitrary"};
    std::vector<uint32_t> oids = {99999};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT 'arbitrary'");
}

// =============================================================================
// Empty and edge-case SQL
// =============================================================================

TEST(QA_GDB200_EdgeCase, EmptySqlNoParams) {
    auto result = substitute_parameters("", {}, {});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "");
}

TEST(QA_GDB200_EdgeCase, EmptySqlWithParams) {
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "");
}

TEST(QA_GDB200_EdgeCase, SqlWithNoPlaceholders) {
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT 1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT 1");
}

TEST(QA_GDB200_EdgeCase, VeryLongSql) {
    // SQL with many placeholders and large values.
    std::string sql;
    std::vector<std::optional<std::string>> params;
    std::vector<uint32_t> oids;

    sql = "SELECT ";
    for (int i = 1; i <= 100; ++i) {
        if (i > 1) {
            sql += ", ";
        }
        sql += "$" + std::to_string(i);
        params.emplace_back(std::to_string(i));
        oids.push_back(OID_INT4);
    }

    auto result = substitute_parameters(sql, params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Spot-check: first and last values.
    EXPECT_TRUE(result->find("SELECT 1,") != std::string::npos || result->find("SELECT 1, ") != std::string::npos);
    EXPECT_TRUE(result->find("100") != std::string::npos);
}

TEST(QA_GDB200_EdgeCase, VeryLongStringParam) {
    std::string long_str(10000, 'x');
    std::vector<std::optional<std::string>> params = {long_str};
    std::vector<uint32_t> oids = {OID_TEXT};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT '" + long_str + "'");
}

TEST(QA_GDB200_EdgeCase, StringParamWithOnlySingleQuotes) {
    std::string quotes(10, '\'');
    std::vector<std::optional<std::string>> params = {quotes};
    std::vector<uint32_t> oids = {OID_TEXT};
    auto result = substitute_parameters("SELECT $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // 10 single quotes → each doubled = 20 quotes inside outer quotes.
    std::string expected_inner(20, '\'');
    EXPECT_EQ(*result, "SELECT '" + expected_inner + "'");
}

// =============================================================================
// Quote tracking correctness in SQL
// =============================================================================

TEST(QA_GDB200_QuoteTracking, UnterminatedSingleQuote) {
    // If the SQL has an unterminated single quote, $1 inside won't be substituted.
    // This is malformed SQL but shouldn't crash.
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT 'unterminated $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // $1 is inside the unterminated string, so NOT substituted.
    EXPECT_EQ(*result, "SELECT 'unterminated $1");
}

TEST(QA_GDB200_QuoteTracking, UnterminatedDoubleQuote) {
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT \"unterminated $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT \"unterminated $1");
}

TEST(QA_GDB200_QuoteTracking, EscapedQuoteInSqlLiteral) {
    // SQL: SELECT * FROM t WHERE name = 'it''s' AND id = $1
    // The '' is an escaped single quote; $1 should still be substituted.
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result =
        substitute_parameters("SELECT * FROM t WHERE name = 'it''s' AND id = $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT * FROM t WHERE name = 'it''s' AND id = 42");
}

TEST(QA_GDB200_QuoteTracking, AdjacentQuotedStrings) {
    // Two adjacent quoted strings: 'a''b' is 'a' then 'b' or a quote?
    // In standard SQL, 'a''b' = string "a'b".
    // The quote tracker should handle this correctly.
    std::vector<std::optional<std::string>> params = {"42"};
    std::vector<uint32_t> oids = {OID_INT4};
    auto result = substitute_parameters("SELECT 'hello' || 'world', $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "SELECT 'hello' || 'world', 42");
}

// =============================================================================
// Acceptance Criteria verification: parameterized SELECT, INSERT, UPDATE, DELETE
// =============================================================================

TEST(QA_GDB200_AC, SelectParameterized) {
    std::vector<std::optional<std::string>> params = {"active", "21"};
    std::vector<uint32_t> oids = {OID_TEXT, OID_INT4};
    auto result = substitute_parameters(
        "SELECT name, email FROM users WHERE status = $1 AND age >= $2", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result,
              "SELECT name, email FROM users WHERE status = 'active' AND age >= 21");
}

TEST(QA_GDB200_AC, InsertParameterized) {
    std::vector<std::optional<std::string>> params = {"alice", "30", "t"};
    std::vector<uint32_t> oids = {OID_TEXT, OID_INT4, OID_BOOL};
    auto result = substitute_parameters(
        "INSERT INTO users (name, age, active) VALUES ($1, $2, $3)", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "INSERT INTO users (name, age, active) VALUES ('alice', 30, TRUE)");
}

TEST(QA_GDB200_AC, UpdateParameterized) {
    std::vector<std::optional<std::string>> params = {"new_email@test.com", "5"};
    std::vector<uint32_t> oids = {OID_TEXT, OID_INT4};
    auto result =
        substitute_parameters("UPDATE users SET email = $1 WHERE id = $2", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "UPDATE users SET email = 'new_email@test.com' WHERE id = 5");
}

TEST(QA_GDB200_AC, DeleteParameterized) {
    std::vector<std::optional<std::string>> params = {"2023-01-01"};
    std::vector<uint32_t> oids = {OID_DATE};
    auto result =
        substitute_parameters("DELETE FROM logs WHERE created_at < $1", params, oids);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "DELETE FROM logs WHERE created_at < '2023-01-01'");
}

// =============================================================================
// Wire protocol integration — adversarial tests
// =============================================================================

namespace {

int create_socketpair_qa(int& client_fd) {
    int fds[2];
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    client_fd = fds[0];
    return fds[1];
}

void write_to_fd_qa(int fd, const std::vector<uint8_t>& data) {
    size_t written = 0;
    while (written < data.size()) {
        auto n = ::write(fd, data.data() + written, data.size() - written);
        ASSERT_GT(n, 0);
        written += static_cast<size_t>(n);
    }
}

std::vector<uint8_t> read_from_fd_qa(int fd) {
    std::vector<uint8_t> buf(8192);
    auto n = ::read(fd, buf.data(), buf.size());
    if (n <= 0) {
        return {};
    }
    buf.resize(static_cast<size_t>(n));
    return buf;
}

std::vector<uint8_t>
build_startup_msg_qa(const std::vector<std::pair<std::string, std::string>>& params) {
    std::vector<uint8_t> body;
    int32_t version = 196608;
    body.push_back(static_cast<uint8_t>((version >> 24) & 0xFF));
    body.push_back(static_cast<uint8_t>((version >> 16) & 0xFF));
    body.push_back(static_cast<uint8_t>((version >> 8) & 0xFF));
    body.push_back(static_cast<uint8_t>(version & 0xFF));
    for (const auto& [key, val] : params) {
        body.insert(body.end(), key.begin(), key.end());
        body.push_back(0);
        body.insert(body.end(), val.begin(), val.end());
        body.push_back(0);
    }
    body.push_back(0);
    int32_t total_len = static_cast<int32_t>(4 + body.size());
    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>((total_len >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((total_len >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((total_len >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(total_len & 0xFF));
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

std::vector<uint8_t> build_parse_msg_qa(std::string_view stmt_name,
                                        std::string_view sql,
                                        const std::vector<uint32_t>& param_oids = {}) {
    std::vector<uint8_t> body;
    body.insert(body.end(), stmt_name.begin(), stmt_name.end());
    body.push_back(0);
    body.insert(body.end(), sql.begin(), sql.end());
    body.push_back(0);
    auto num = static_cast<uint16_t>(param_oids.size());
    body.push_back(static_cast<uint8_t>((num >> 8) & 0xFF));
    body.push_back(static_cast<uint8_t>(num & 0xFF));
    for (uint32_t oid : param_oids) {
        body.push_back(static_cast<uint8_t>((oid >> 24) & 0xFF));
        body.push_back(static_cast<uint8_t>((oid >> 16) & 0xFF));
        body.push_back(static_cast<uint8_t>((oid >> 8) & 0xFF));
        body.push_back(static_cast<uint8_t>(oid & 0xFF));
    }
    std::vector<uint8_t> msg;
    msg.push_back('P');
    uint32_t body_len = static_cast<uint32_t>(4 + body.size());
    msg.push_back(static_cast<uint8_t>((body_len >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(body_len & 0xFF));
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

std::vector<uint8_t>
build_bind_msg_qa(std::string_view portal_name,
                  std::string_view stmt_name,
                  const std::vector<std::optional<std::string>>& param_values = {}) {
    std::vector<uint8_t> body;
    body.insert(body.end(), portal_name.begin(), portal_name.end());
    body.push_back(0);
    body.insert(body.end(), stmt_name.begin(), stmt_name.end());
    body.push_back(0);
    body.push_back(0);
    body.push_back(0);
    auto num = static_cast<uint16_t>(param_values.size());
    body.push_back(static_cast<uint8_t>((num >> 8) & 0xFF));
    body.push_back(static_cast<uint8_t>(num & 0xFF));
    for (const auto& val : param_values) {
        if (!val.has_value()) {
            body.push_back(0xFF);
            body.push_back(0xFF);
            body.push_back(0xFF);
            body.push_back(0xFF);
        } else {
            auto len = static_cast<uint32_t>(val->size());
            body.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
            body.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
            body.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            body.push_back(static_cast<uint8_t>(len & 0xFF));
            body.insert(body.end(), val->begin(), val->end());
        }
    }
    body.push_back(0);
    body.push_back(0);

    std::vector<uint8_t> msg;
    msg.push_back('B');
    uint32_t body_len = static_cast<uint32_t>(4 + body.size());
    msg.push_back(static_cast<uint8_t>((body_len >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(body_len & 0xFF));
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

std::vector<uint8_t> build_execute_msg_qa(std::string_view portal_name,
                                          int32_t max_rows = 0) {
    std::vector<uint8_t> msg;
    msg.push_back('E');
    uint32_t body_len = static_cast<uint32_t>(4 + portal_name.size() + 1 + 4);
    msg.push_back(static_cast<uint8_t>((body_len >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(body_len & 0xFF));
    msg.insert(msg.end(), portal_name.begin(), portal_name.end());
    msg.push_back(0);
    auto mr = static_cast<uint32_t>(max_rows);
    msg.push_back(static_cast<uint8_t>((mr >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((mr >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((mr >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(mr & 0xFF));
    return msg;
}

std::vector<uint8_t> build_sync_msg_qa() {
    return {'S', 0, 0, 0, 4};
}

void do_startup_qa(int client_fd, Connection& conn, PgProtocolHandler& handler) {
    auto startup = build_startup_msg_qa({{"user", "test"}});
    write_to_fd_qa(client_fd, startup);
    (void)conn.read_from_socket();
    (void)handler.process(conn);
    (void)conn.write_to_socket();
    (void)read_from_fd_qa(client_fd);
}

} // namespace

TEST(QA_GDB200_Wire, InjectionViaWireProtocolTextParam) {
    int client_fd = -1;
    int server_fd = create_socketpair_qa(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(200);

    std::string received_sql;
    handler.set_query_executor([&](const std::string& sql) -> Result<QueryResult> {
        received_sql = sql;
        QueryResult qr;
        qr.column_names = {"name"};
        qr.column_types = {TypeId::STRING};
        qr.rows = {};
        return ok(std::move(qr));
    });

    do_startup_qa(client_fd, conn, handler);

    // Send a SQL injection attempt as a text parameter.
    std::vector<uint8_t> batch;
    auto parse = build_parse_msg_qa("", "SELECT * FROM users WHERE name = $1", {OID_TEXT});
    auto bind =
        build_bind_msg_qa("", "", {std::optional<std::string>("'; DROP TABLE users; --")});
    auto execute = build_execute_msg_qa("");
    auto sync = build_sync_msg_qa();

    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), execute.begin(), execute.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd_qa(client_fd, batch);
    (void)conn.read_from_socket();
    auto result = handler.process(conn);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // The SQL injection must be escaped, not injected raw.
    // "DROP TABLE" text appears inside a properly escaped string literal — that's safe.
    // The key check is that the single quote at the injection boundary is doubled.
    EXPECT_EQ(received_sql,
              "SELECT * FROM users WHERE name = '''; DROP TABLE users; --'");
    EXPECT_NE(received_sql.find("'''"), std::string::npos);

    conn.close();
    ::close(client_fd);
}

TEST(QA_GDB200_Wire, AllNullParamsViaWire) {
    int client_fd = -1;
    int server_fd = create_socketpair_qa(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(201);

    std::string received_sql;
    handler.set_query_executor([&](const std::string& sql) -> Result<QueryResult> {
        received_sql = sql;
        QueryResult qr;
        qr.affected_rows = 1;
        qr.message = "INSERT";
        return ok(std::move(qr));
    });

    do_startup_qa(client_fd, conn, handler);

    std::vector<uint8_t> batch;
    auto parse =
        build_parse_msg_qa("", "INSERT INTO t (a, b) VALUES ($1, $2)", {OID_INT4, OID_TEXT});
    auto bind = build_bind_msg_qa("", "", {std::nullopt, std::nullopt});
    auto execute = build_execute_msg_qa("");
    auto sync = build_sync_msg_qa();

    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), execute.begin(), execute.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd_qa(client_fd, batch);
    (void)conn.read_from_socket();
    auto result = handler.process(conn);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(received_sql, "INSERT INTO t (a, b) VALUES (NULL, NULL)");

    conn.close();
    ::close(client_fd);
}

TEST(QA_GDB200_Wire, MixedTypeParamsViaWire) {
    int client_fd = -1;
    int server_fd = create_socketpair_qa(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(202);

    std::string received_sql;
    handler.set_query_executor([&](const std::string& sql) -> Result<QueryResult> {
        received_sql = sql;
        QueryResult qr;
        qr.affected_rows = 1;
        qr.message = "INSERT";
        return ok(std::move(qr));
    });

    do_startup_qa(client_fd, conn, handler);

    std::vector<uint8_t> batch;
    auto parse = build_parse_msg_qa(
        "", "INSERT INTO t (id, name, active, score) VALUES ($1, $2, $3, $4)",
        {OID_INT4, OID_TEXT, OID_BOOL, OID_FLOAT8});
    auto bind = build_bind_msg_qa(
        "", "",
        {std::optional<std::string>("42"), std::optional<std::string>("hello"),
         std::optional<std::string>("t"), std::optional<std::string>("3.14")});
    auto execute = build_execute_msg_qa("");
    auto sync = build_sync_msg_qa();

    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), execute.begin(), execute.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd_qa(client_fd, batch);
    (void)conn.read_from_socket();
    auto result = handler.process(conn);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(received_sql,
              "INSERT INTO t (id, name, active, score) VALUES (42, 'hello', TRUE, 3.14)");

    conn.close();
    ::close(client_fd);
}

TEST(QA_GDB200_Wire, InvalidNumericParamViaWireReturnsError) {
    int client_fd = -1;
    int server_fd = create_socketpair_qa(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(203);

    bool executor_called = false;
    handler.set_query_executor([&](const std::string& /*sql*/) -> Result<QueryResult> {
        executor_called = true;
        QueryResult qr;
        return ok(std::move(qr));
    });

    do_startup_qa(client_fd, conn, handler);

    std::vector<uint8_t> batch;
    auto parse = build_parse_msg_qa("", "SELECT $1", {OID_INT4});
    // Send a non-numeric value for an INT4 parameter.
    auto bind = build_bind_msg_qa("", "", {std::optional<std::string>("not_a_number")});
    auto execute = build_execute_msg_qa("");
    auto sync = build_sync_msg_qa();

    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), execute.begin(), execute.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd_qa(client_fd, batch);
    (void)conn.read_from_socket();
    (void)handler.process(conn);

    // The query executor should NOT be called because substitution failed.
    EXPECT_FALSE(executor_called)
        << "Executor should not be called when parameter substitution fails";

    conn.close();
    ::close(client_fd);
}

TEST(QA_GDB200_Wire, NoParamsNoSubstitution) {
    int client_fd = -1;
    int server_fd = create_socketpair_qa(client_fd);

    Connection conn(server_fd);
    PgProtocolHandler handler(204);

    std::string received_sql;
    handler.set_query_executor([&](const std::string& sql) -> Result<QueryResult> {
        received_sql = sql;
        QueryResult qr;
        qr.column_names = {"x"};
        qr.column_types = {TypeId::INT32};
        qr.rows = {{Value(static_cast<int32_t>(1))}};
        return ok(std::move(qr));
    });

    do_startup_qa(client_fd, conn, handler);

    std::vector<uint8_t> batch;
    auto parse = build_parse_msg_qa("", "SELECT 1");
    auto bind = build_bind_msg_qa("", "");
    auto execute = build_execute_msg_qa("");
    auto sync = build_sync_msg_qa();

    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), execute.begin(), execute.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd_qa(client_fd, batch);
    (void)conn.read_from_socket();
    auto result = handler.process(conn);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(received_sql, "SELECT 1");

    conn.close();
    ::close(client_fd);
}
