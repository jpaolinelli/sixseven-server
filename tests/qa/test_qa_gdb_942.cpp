// QA adversarial tests for GDB-942: safe_* parse helpers behavior-preservation.
//
// Focus: confirm that safe_* wrappers mirror std::sto* semantics exactly --
// the only change is throw -> Result<T>.  Any input accepted by std::sto* must
// still be accepted (same value), and any input rejected must still be rejected
// (PARSE_ERROR). Particular attention paid to:
//   - trailing non-numeric chars ("12abc") -- std::stoi ACCEPTS (returns 12)
//   - leading whitespace ("  12  ")        -- std::stoi ACCEPTS (returns 12)
//   - explicit positive sign ("+5")        -- std::stoi ACCEPTS (returns 5)
//   - hex prefix ("0x10")                 -- std::stoi base-10 stops at 'x',
//                                            so returns 0 (not 16, not an error)
//   - INT32_MAX / INT32_MAX+1 boundaries  -- exact boundary for safe_stoi
//   - UINT32_MAX / UINT32_MAX+1           -- exact boundary for safe_stou32
//   - INT64_MAX / INT64_MAX+1             -- exact boundary for safe_stoll
//   - UINT64_MAX / UINT64_MAX+1           -- exact boundary for safe_stoull/safe_stoul

#include "sixseven/common/parse_utils.h"
#include "sixseven/common/status.h"

#include <gtest/gtest.h>

#include <climits>
#include <cstdint>
#include <limits>
#include <string>

namespace sixseven {

// ============================================================================
// safe_stoi -- behavior-preservation adversarial cases
// ============================================================================

// "12abc": std::stoi("12abc") returns 12, does NOT throw.
// If safe_stoi rejects this it is a behavior regression.
TEST(QA_GDB942_SafeStoi, TrailingGarbaceAccepted) {
    auto r = safe_stoi("12abc");
    ASSERT_TRUE(r.has_value()) << "safe_stoi rejected \"12abc\" but std::stoi accepts it (returns 12)";
    EXPECT_EQ(*r, 12);
}

// "  12  ": std::stoi skips leading whitespace and stops at trailing space -> 12.
TEST(QA_GDB942_SafeStoi, LeadingAndTrailingWhitespace) {
    auto r = safe_stoi("  12  ");
    ASSERT_TRUE(r.has_value()) << "safe_stoi rejected \"  12  \" but std::stoi accepts it";
    EXPECT_EQ(*r, 12);
}

// "+5": explicit positive sign -- std::stoi accepts this.
TEST(QA_GDB942_SafeStoi, ExplicitPositiveSign) {
    auto r = safe_stoi("+5");
    ASSERT_TRUE(r.has_value()) << "safe_stoi rejected \"+5\" but std::stoi accepts it";
    EXPECT_EQ(*r, 5);
}

// "0x10": base-10 parse, std::stoi stops at 'x' after reading '0' -> returns 0.
// This must NOT be an error and must NOT return 16.
TEST(QA_GDB942_SafeStoi, HexPrefixReadsZero) {
    auto r = safe_stoi("0x10");
    ASSERT_TRUE(r.has_value()) << "safe_stoi rejected \"0x10\" but std::stoi(base 10) accepts it (returns 0)";
    EXPECT_EQ(*r, 0);
}

// INT32_MAX (2147483647) -- must succeed.
TEST(QA_GDB942_SafeStoi, Int32MaxBoundary) {
    auto r = safe_stoi("2147483647");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::numeric_limits<int>::max());
}

// INT32_MAX+1 (2147483648) -- must be PARSE_ERROR (out of range for int).
TEST(QA_GDB942_SafeStoi, Int32MaxPlusOneBoundary) {
    auto r = safe_stoi("2147483648");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// INT32_MIN (-2147483648) -- must succeed.
TEST(QA_GDB942_SafeStoi, Int32MinBoundary) {
    auto r = safe_stoi("-2147483648");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::numeric_limits<int>::min());
}

// One below INT32_MIN -- must be PARSE_ERROR.
TEST(QA_GDB942_SafeStoi, Int32MinMinusOneBoundary) {
    auto r = safe_stoi("-2147483649");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// ============================================================================
// safe_stoll -- behavior-preservation adversarial cases
// ============================================================================

TEST(QA_GDB942_SafeStoll, TrailingGarbaceAccepted) {
    auto r = safe_stoll("99abc");
    ASSERT_TRUE(r.has_value()) << "safe_stoll rejected \"99abc\" but std::stoll accepts it";
    EXPECT_EQ(*r, int64_t{99});
}

TEST(QA_GDB942_SafeStoll, LeadingWhitespace) {
    auto r = safe_stoll("   -42");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, int64_t{-42});
}

TEST(QA_GDB942_SafeStoll, ExplicitPositiveSign) {
    auto r = safe_stoll("+100");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, int64_t{100});
}

// INT64_MAX (9223372036854775807) -- must succeed.
TEST(QA_GDB942_SafeStoll, Int64MaxBoundary) {
    auto r = safe_stoll("9223372036854775807");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::numeric_limits<int64_t>::max());
}

// INT64_MAX+1 -- must be PARSE_ERROR.
TEST(QA_GDB942_SafeStoll, Int64MaxPlusOneBoundary) {
    auto r = safe_stoll("9223372036854775808");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// INT64_MIN (-9223372036854775808) -- must succeed.
TEST(QA_GDB942_SafeStoll, Int64MinBoundary) {
    auto r = safe_stoll("-9223372036854775808");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::numeric_limits<int64_t>::min());
}

// ============================================================================
// safe_stou32 -- behavior-preservation adversarial cases
// ============================================================================

// "0x10": stoull base-10 reads 0, stops at 'x' -> returns 0 (within uint32 range).
TEST(QA_GDB942_SafeStou32, HexPrefixReadsZero) {
    auto r = safe_stou32("0x10");
    ASSERT_TRUE(r.has_value()) << "safe_stou32 rejected \"0x10\" but stoull base-10 accepts it (returns 0)";
    EXPECT_EQ(*r, uint32_t{0});
}

// UINT32_MAX (4294967295) exact -- must succeed.
TEST(QA_GDB942_SafeStou32, Uint32MaxExact) {
    auto r = safe_stou32("4294967295");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::numeric_limits<uint32_t>::max());
}

// UINT32_MAX+1 (4294967296) -- must be PARSE_ERROR.
TEST(QA_GDB942_SafeStou32, Uint32MaxPlusOne) {
    auto r = safe_stou32("4294967296");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// Negative literal: std::stoull throws std::invalid_argument for "-1" on some
// platforms (mingw/msvc can differ). Confirm it returns PARSE_ERROR rather than
// silently wrapping to a huge uint value.
TEST(QA_GDB942_SafeStou32, NegativeLiteralRejected) {
    auto r = safe_stou32("-1");
    // Must not silently wrap to UINT32_MAX or UINT64_MAX.
    // Either it's an error OR (on platforms where stoull accepts negatives) it
    // produces a non-zero, non-UINT32_MAX value -- but the safe wrapper's range
    // check should catch the wrapped value > UINT32_MAX.
    // Either way the value 4294967295 would be misleading for input "-1", so
    // if has_value(), confirm it is NOT UINT32_MAX (wrapping).
    if (r.has_value()) {
        // On MSVC, stoull("-1") throws invalid_argument so this branch may not execute.
        // On some platforms it wraps to ULLONG_MAX which exceeds UINT32_MAX -> range error.
        // If we reach here the implementation silently passed "-1" -> flag this.
        FAIL() << "safe_stou32(\"-1\") returned " << *r
               << " -- negative input should be rejected or wrapped and caught by range check";
    } else {
        EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
    }
}

// Trailing garbage accepted (behavior mirror).
TEST(QA_GDB942_SafeStou32, TrailingGarbageAccepted) {
    auto r = safe_stou32("100xyz");
    ASSERT_TRUE(r.has_value()) << "safe_stou32 rejected \"100xyz\" but stoull accepts it";
    EXPECT_EQ(*r, uint32_t{100});
}

// ============================================================================
// safe_stoull -- boundary adversarial cases
// ============================================================================

// UINT64_MAX exact -- must succeed.
TEST(QA_GDB942_SafeStoull, Uint64MaxExact) {
    auto r = safe_stoull("18446744073709551615");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::numeric_limits<uint64_t>::max());
}

// UINT64_MAX+1 -- must be PARSE_ERROR.
TEST(QA_GDB942_SafeStoull, Uint64MaxPlusOne) {
    auto r = safe_stoull("18446744073709551616");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// Trailing garbage -- accepted (mirror std::stoull).
TEST(QA_GDB942_SafeStoull, TrailingGarbageAccepted) {
    auto r = safe_stoull("7abc");
    ASSERT_TRUE(r.has_value()) << "safe_stoull rejected \"7abc\" but std::stoull accepts it";
    EXPECT_EQ(*r, uint64_t{7});
}

// ============================================================================
// safe_stoul -- boundary adversarial cases
// ============================================================================

TEST(QA_GDB942_SafeStoul, TrailingGarbageAccepted) {
    auto r = safe_stoul("55xyz");
    ASSERT_TRUE(r.has_value()) << "safe_stoul rejected \"55xyz\" but std::stoul accepts it";
    EXPECT_EQ(*r, 55UL);
}

TEST(QA_GDB942_SafeStoul, LeadingWhitespace) {
    auto r = safe_stoul("  99");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 99UL);
}

// ============================================================================
// safe_stod / safe_stof -- behavior-preservation adversarial cases
// ============================================================================

TEST(QA_GDB942_SafeStod, TrailingGarbageAccepted) {
    auto r = safe_stod("1.5xyz");
    ASSERT_TRUE(r.has_value()) << "safe_stod rejected \"1.5xyz\" but std::stod accepts it";
    EXPECT_DOUBLE_EQ(*r, 1.5);
}

TEST(QA_GDB942_SafeStod, LeadingWhitespace) {
    auto r = safe_stod("  2.0");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 2.0);
}

TEST(QA_GDB942_SafeStod, ExplicitPositiveSign) {
    auto r = safe_stod("+3.14");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 3.14);
}

TEST(QA_GDB942_SafeStof, TrailingGarbageAccepted) {
    auto r = safe_stof("0.5abc");
    ASSERT_TRUE(r.has_value()) << "safe_stof rejected \"0.5abc\" but std::stof accepts it";
    EXPECT_FLOAT_EQ(*r, 0.5f);
}

TEST(QA_GDB942_SafeStof, ExplicitPositiveSign) {
    auto r = safe_stof("+1.0");
    ASSERT_TRUE(r.has_value());
    EXPECT_FLOAT_EQ(*r, 1.0f);
}

// ============================================================================
// Error-path: confirm PARSE_ERROR fires through a prod-like path.
// safe_stoi with a clearly out-of-range value must return an error, not crash.
// ============================================================================

TEST(QA_GDB942_ErrorPaths, LargeOverflowNoCrash) {
    // 2^63 -- far outside int range, should get PARSE_ERROR cleanly.
    auto r = safe_stoi("9223372036854775808");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB942_ErrorPaths, NullByteInMiddle) {
    // A string_view containing a null byte mid-stream. std::stoi converts to
    // std::string which truncates at the null -- "12\0abc" -> "12" -> 12.
    std::string s;
    s.push_back('1');
    s.push_back('2');
    s.push_back('\0');
    s.push_back('a');
    // The std::string ctor from string_view preserves all bytes; std::stoi may
    // parse "12" and stop at \0 (implementation defined). Do not crash; return
    // a value or PARSE_ERROR.
    auto r = safe_stoi(std::string_view(s.data(), s.size()));
    // No assertion on value -- just must not crash/throw.
    (void)r;
}

TEST(QA_GDB942_ErrorPaths, WhitespaceOnly) {
    // Whitespace-only string: std::stoi sees no valid digit after skipping spaces
    // -> std::invalid_argument -> PARSE_ERROR.
    auto r = safe_stoi("   ");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB942_ErrorPaths, SinglePlusSign) {
    auto r = safe_stoi("+");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB942_ErrorPaths, SingleMinusSign) {
    auto r = safe_stoll("-");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

} // namespace sixseven
