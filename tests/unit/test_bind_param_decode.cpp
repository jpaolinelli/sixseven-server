/// @file test_bind_param_decode.cpp
/// @brief Unit tests for binary Bind-parameter decoding (GDB-712):
/// decode_binary_parameter() and decode_bind_parameters().

#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/server/pg_protocol.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace sixseven;

namespace {

std::string be16(uint16_t v) {
    std::string s(2, '\0');
    s[0] = static_cast<char>((v >> 8) & 0xFF);
    s[1] = static_cast<char>(v & 0xFF);
    return s;
}

std::string be32(uint32_t v) {
    std::string s(4, '\0');
    for (int i = 0; i < 4; ++i) {
        s[static_cast<size_t>(i)] = static_cast<char>((v >> (24 - 8 * i)) & 0xFF);
    }
    return s;
}

std::string be64(uint64_t v) {
    std::string s(8, '\0');
    for (int i = 0; i < 8; ++i) {
        s[static_cast<size_t>(i)] = static_cast<char>((v >> (56 - 8 * i)) & 0xFF);
    }
    return s;
}

std::string be_float4(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return be32(bits);
}

std::string be_float8(double d) {
    uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    return be64(bits);
}

} // namespace

// =============================================================================
// decode_binary_parameter: integer types
// =============================================================================

TEST(BinaryParamDecode, Int4Positive) {
    auto r = decode_binary_parameter(be32(42), 23);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(*r, "42");
}

TEST(BinaryParamDecode, Int4Negative) {
    auto r = decode_binary_parameter(be32(static_cast<uint32_t>(-12345)), 23);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(*r, "-12345");
}

TEST(BinaryParamDecode, Int4MinMax) {
    auto min = decode_binary_parameter(be32(0x80000000u), 23);
    ASSERT_TRUE(min.has_value());
    EXPECT_EQ(*min, "-2147483648");

    auto max = decode_binary_parameter(be32(0x7FFFFFFFu), 23);
    ASSERT_TRUE(max.has_value());
    EXPECT_EQ(*max, "2147483647");
}

TEST(BinaryParamDecode, Int2Values) {
    auto pos = decode_binary_parameter(be16(300), 21);
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ(*pos, "300");

    auto neg = decode_binary_parameter(be16(static_cast<uint16_t>(-2)), 21);
    ASSERT_TRUE(neg.has_value());
    EXPECT_EQ(*neg, "-2");

    auto min = decode_binary_parameter(be16(0x8000u), 21);
    ASSERT_TRUE(min.has_value());
    EXPECT_EQ(*min, "-32768");
}

TEST(BinaryParamDecode, Int8Values) {
    auto big = decode_binary_parameter(be64(5000000000ULL), 20);
    ASSERT_TRUE(big.has_value());
    EXPECT_EQ(*big, "5000000000");

    auto min = decode_binary_parameter(be64(0x8000000000000000ULL), 20);
    ASSERT_TRUE(min.has_value());
    EXPECT_EQ(*min, "-9223372036854775808");

    auto max = decode_binary_parameter(be64(0x7FFFFFFFFFFFFFFFULL), 20);
    ASSERT_TRUE(max.has_value());
    EXPECT_EQ(*max, "9223372036854775807");
}

// =============================================================================
// decode_binary_parameter: bool and float types
// =============================================================================

TEST(BinaryParamDecode, BoolTrueFalse) {
    auto t = decode_binary_parameter(std::string("\x01", 1), 16);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(*t, "t");

    auto f = decode_binary_parameter(std::string("\x00", 1), 16);
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(*f, "f");
}

TEST(BinaryParamDecode, BoolNonzeroByteIsTrue) {
    auto r = decode_binary_parameter(std::string("\x7f", 1), 16);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "t");
}

TEST(BinaryParamDecode, Float4RoundTrip) {
    auto r = decode_binary_parameter(be_float4(1.5F), 700);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(*r, "1.5");
}

TEST(BinaryParamDecode, Float8RoundTrip) {
    auto r = decode_binary_parameter(be_float8(2.5), 701);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(*r, "2.5");

    auto frac = decode_binary_parameter(be_float8(0.1), 701);
    ASSERT_TRUE(frac.has_value());
    EXPECT_EQ(*frac, "0.1"); // Shortest round-trip representation.

    auto neg = decode_binary_parameter(be_float8(-0.25), 701);
    ASSERT_TRUE(neg.has_value());
    EXPECT_EQ(*neg, "-0.25");
}

// =============================================================================
// decode_binary_parameter: text-like passthrough
// =============================================================================

TEST(BinaryParamDecode, TextPassthrough) {
    auto r = decode_binary_parameter("hello", 25);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "hello");
}

TEST(BinaryParamDecode, VarcharPassthrough) {
    auto r = decode_binary_parameter("it's", 1043);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "it's"); // Escaping happens later, at substitution time.
}

TEST(BinaryParamDecode, JsonPassthrough) {
    auto r = decode_binary_parameter(R"({"k":1})", 114);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, R"({"k":1})");
}

// =============================================================================
// decode_binary_parameter: error conditions
// =============================================================================

TEST(BinaryParamDecode, WrongLengthRejected) {
    EXPECT_FALSE(decode_binary_parameter(std::string("\x00\x00\x2a", 3), 23).has_value());
    EXPECT_FALSE(decode_binary_parameter(std::string(), 23).has_value());
    EXPECT_FALSE(decode_binary_parameter(be32(1), 21).has_value());  // 4 bytes as int2.
    EXPECT_FALSE(decode_binary_parameter(be32(1), 20).has_value());  // 4 bytes as int8.
    EXPECT_FALSE(decode_binary_parameter(be64(1), 700).has_value()); // 8 bytes as float4.
    EXPECT_FALSE(decode_binary_parameter(be32(1), 701).has_value()); // 4 bytes as float8.
    EXPECT_FALSE(decode_binary_parameter("tt", 16).has_value());     // 2 bytes as bool.

    auto r = decode_binary_parameter(std::string("\x00\x00\x2a", 3), 23);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(BinaryParamDecode, UnsupportedOidsRejected) {
    // Types whose binary wire format we cannot decode must be rejected with
    // an explicit error — never spliced into SQL as raw bytes.
    for (uint32_t oid : {1700u /*numeric*/,
                         1114u /*timestamp*/,
                         1082u /*date*/,
                         17u /*bytea*/,
                         2950u /*uuid*/,
                         0u /*unspecified*/}) {
        auto r = decode_binary_parameter(be64(1), oid);
        ASSERT_FALSE(r.has_value()) << "OID " << oid << " unexpectedly decoded";
        EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    }
}

// =============================================================================
// decode_bind_parameters: format-code semantics
// =============================================================================

TEST(BindParamsDecode, EmptyFormatCodesMeansAllText) {
    std::vector<std::optional<std::string>> params = {std::string("42"), std::string("abc")};
    auto r = decode_bind_parameters(params, {}, {23, 25});
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->size(), 2u);
    EXPECT_EQ((*r)[0], "42");
    EXPECT_EQ((*r)[1], "abc");
}

TEST(BindParamsDecode, SingleTextFormatCodePassthrough) {
    std::vector<std::optional<std::string>> params = {std::string("42")};
    auto r = decode_bind_parameters(params, {0}, {23});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)[0], "42");
}

TEST(BindParamsDecode, SingleBinaryFormatCodeAppliesToAll) {
    std::vector<std::optional<std::string>> params = {be32(7), be32(9)};
    auto r = decode_bind_parameters(params, {1}, {23, 23});
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->size(), 2u);
    EXPECT_EQ((*r)[0], "7");
    EXPECT_EQ((*r)[1], "9");
}

TEST(BindParamsDecode, PerParameterFormatCodes) {
    std::vector<std::optional<std::string>> params = {be32(42), std::string("alice")};
    auto r = decode_bind_parameters(params, {1, 0}, {23, 25});
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ((*r)[0], "42");
    EXPECT_EQ((*r)[1], "alice");
}

TEST(BindParamsDecode, NullParamPassesThroughUnchanged) {
    std::vector<std::optional<std::string>> params = {std::nullopt, be32(5)};
    auto r = decode_bind_parameters(params, {1}, {25, 23});
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE((*r)[0].has_value());
    EXPECT_EQ((*r)[1], "5");
}

TEST(BindParamsDecode, FormatCodeCountMismatchRejected) {
    std::vector<std::optional<std::string>> params = {std::string("42")};
    auto r = decode_bind_parameters(params, {1, 1}, {23});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(BindParamsDecode, InvalidFormatCodeValueRejected) {
    std::vector<std::optional<std::string>> params = {std::string("42")};
    auto r = decode_bind_parameters(params, {2}, {23});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(BindParamsDecode, MissingOidWithBinaryFormatRejected) {
    // Fewer OIDs than parameters: the missing OID defaults to 0 (unspecified),
    // which cannot be binary-decoded.
    std::vector<std::optional<std::string>> params = {be32(1)};
    auto r = decode_bind_parameters(params, {1}, {});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(BindParamsDecode, DecodeErrorIdentifiesParameterNumber) {
    std::vector<std::optional<std::string>> params = {std::string("ok"), be32(1)};
    auto r = decode_bind_parameters(params, {0, 1}, {25, 1700 /*numeric: unsupported*/});
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("$2"), std::string::npos) << r.error().message;
}

TEST(BindParamsDecode, EmptyParamsNoFormats) {
    auto r = decode_bind_parameters({}, {}, {});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->empty());
}

// =============================================================================
// Integration: decoded binary params flow through substitute_parameters
// =============================================================================

TEST(BindParamsDecode, DecodedBinaryIntSubstitutesIntoSql) {
    std::vector<std::optional<std::string>> params = {be32(42)};
    std::vector<uint32_t> oids = {23};
    auto decoded = decode_bind_parameters(params, {1}, oids);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;

    auto sql = substitute_parameters("SELECT * FROM t WHERE id = $1", *decoded, oids);
    ASSERT_TRUE(sql.has_value()) << sql.error().message;
    EXPECT_EQ(*sql, "SELECT * FROM t WHERE id = 42");
}

TEST(BindParamsDecode, RawBinaryBytesWithoutDecodeFailSubstitution) {
    // Documents the original H18 failure mode: raw big-endian int4 bytes
    // passed straight to substitution are rejected as an invalid numeric
    // literal — which is exactly what every binary parameter hit before the
    // decode step existed.
    std::vector<std::optional<std::string>> params = {be32(42)};
    std::vector<uint32_t> oids = {23};
    auto sql = substitute_parameters("SELECT * FROM t WHERE id = $1", params, oids);
    ASSERT_FALSE(sql.has_value());
    EXPECT_EQ(sql.error().code, StatusCode::INVALID_ARGUMENT);
}
