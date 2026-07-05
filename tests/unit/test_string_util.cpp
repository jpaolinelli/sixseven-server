#include "sixseven/common/string_util.h"

#include <gtest/gtest.h>

using namespace sixseven;

// ---------------------------------------------------------------------------
// to_upper
// ---------------------------------------------------------------------------

TEST(StringUtil, ToUpperBasicAscii) {
    EXPECT_EQ(to_upper("hello"), "HELLO");
}

TEST(StringUtil, ToUpperAlreadyUpper) {
    EXPECT_EQ(to_upper("HELLO"), "HELLO");
}

TEST(StringUtil, ToUpperMixedCase) {
    EXPECT_EQ(to_upper("HeLLo WoRLD"), "HELLO WORLD");
}

TEST(StringUtil, ToUpperDigitsAndPunctuationUnchanged) {
    EXPECT_EQ(to_upper("abc123!@# def"), "ABC123!@# DEF");
}

TEST(StringUtil, ToUpperEmptyString) {
    EXPECT_EQ(to_upper(""), "");
}

TEST(StringUtil, ToUpperHighBitBytesNoUb) {
    // Bytes with the high bit set (>127) must not trigger UB when passed to
    // std::toupper. The canonical implementation casts to unsigned char before
    // calling std::toupper; verify it survives such input without crashing and
    // returns a string of the same length (locale-dependent behavior for these
    // bytes is not something we assert on, just that it's well-defined/safe).
    std::string input;
    for (int c = 128; c < 256; ++c) {
        input.push_back(static_cast<char>(static_cast<unsigned char>(c)));
    }
    std::string result = to_upper(input);
    EXPECT_EQ(result.size(), input.size());
}

// ---------------------------------------------------------------------------
// to_lower
// ---------------------------------------------------------------------------

TEST(StringUtil, ToLowerBasicAscii) {
    EXPECT_EQ(to_lower("HELLO"), "hello");
}

TEST(StringUtil, ToLowerAlreadyLower) {
    EXPECT_EQ(to_lower("hello"), "hello");
}

TEST(StringUtil, ToLowerMixedCase) {
    EXPECT_EQ(to_lower("HeLLo WoRLD"), "hello world");
}

TEST(StringUtil, ToLowerDigitsAndPunctuationUnchanged) {
    EXPECT_EQ(to_lower("ABC123!@# DEF"), "abc123!@# def");
}

TEST(StringUtil, ToLowerEmptyString) {
    EXPECT_EQ(to_lower(""), "");
}

TEST(StringUtil, ToLowerHighBitBytesNoUb) {
    std::string input;
    for (int c = 128; c < 256; ++c) {
        input.push_back(static_cast<char>(static_cast<unsigned char>(c)));
    }
    std::string result = to_lower(input);
    EXPECT_EQ(result.size(), input.size());
}

// ---------------------------------------------------------------------------
// starts_with_ci
// ---------------------------------------------------------------------------

TEST(StringUtil, StartsWithCiBasicMatch) {
    EXPECT_TRUE(starts_with_ci("SELECT * FROM t", "SELECT "));
}

TEST(StringUtil, StartsWithCiCaseInsensitiveMatch) {
    EXPECT_TRUE(starts_with_ci("select * from t", "SELECT "));
    EXPECT_TRUE(starts_with_ci("SeLeCt * from t", "sElEcT "));
}

TEST(StringUtil, StartsWithCiNoMatch) {
    EXPECT_FALSE(starts_with_ci("INSERT INTO t", "SELECT "));
}

TEST(StringUtil, StartsWithCiEmptyPrefixAlwaysMatches) {
    EXPECT_TRUE(starts_with_ci("anything", ""));
    EXPECT_TRUE(starts_with_ci("", ""));
}

TEST(StringUtil, StartsWithCiPrefixLongerThanStrFails) {
    EXPECT_FALSE(starts_with_ci("SEL", "SELECT "));
}

TEST(StringUtil, StartsWithCiEmptyStrWithNonEmptyPrefixFails) {
    EXPECT_FALSE(starts_with_ci("", "SELECT "));
}

TEST(StringUtil, StartsWithCiExactMatch) {
    EXPECT_TRUE(starts_with_ci("SELECT", "SELECT"));
}

TEST(StringUtil, StartsWithCiHighBitByteSafety) {
    // High-bit bytes must not trigger UB when passed through
    // static_cast<unsigned char> into std::tolower.
    std::string str;
    std::string prefix;
    for (int c = 128; c < 160; ++c) {
        str.push_back(static_cast<char>(static_cast<unsigned char>(c)));
        prefix.push_back(static_cast<char>(static_cast<unsigned char>(c)));
    }
    // Should not crash; identical bytes should compare equal regardless of
    // tolower's (locale-dependent, but well-defined for unsigned char) result.
    EXPECT_TRUE(starts_with_ci(str, prefix));
}
