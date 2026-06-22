/// @file tests/qa/test_qa_gdb_355.cpp
/// QA adversarial tests for GDB-355: decode_utf8 accepts overlong UTF-8
/// sequences.
///
/// Bug: decode_utf8() assembled codepoints from overlong sequences without
/// checking whether the decoded value could have been encoded in fewer bytes.
///
/// Fix: After decoding, check that cp >= MIN_CP[extra] where MIN_CP is the
/// minimum codepoint for each byte count. Overlong sequences return U+FFFD.

#include "sixseven/vector/text_normalizer.h"

#include <gtest/gtest.h>

#include <string>

namespace sixseven {
namespace {

const std::string REPLACEMENT = "\xEF\xBF\xBD"; // U+FFFD in UTF-8

// Use BertNormalizer with all flags off to observe raw decode_utf8 behavior.
// NullNormalizer would return input unchanged (no decoding).
// LowercaseNormalizer would decode, but we want minimal transformation.

// ===================================================================
// 2-byte overlong encodings (codepoints < 0x80 encoded as 2 bytes)
// ===================================================================

TEST(QA_GDB355, OverlongNulC080) {
    BertNormalizer norm(false, false, false, false);
    // C0 80 → U+0000 as 2-byte (overlong). Should produce U+FFFD.
    std::string input = "\xC0\x80";
    std::string result = norm.normalize(input);
    // Should contain replacement character(s), NOT a NUL byte.
    EXPECT_EQ(result.find('\x00'), std::string::npos); // no raw NUL
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

TEST(QA_GDB355, OverlongSpaceC0A0) {
    BertNormalizer norm(false, false, false, false);
    // C0 A0 → U+0020 (space) as 2-byte (overlong). Should produce U+FFFD.
    std::string input = "\xC0\xA0";
    std::string result = norm.normalize(input);
    // Must not produce an ASCII space.
    EXPECT_EQ(result.find(' '), std::string::npos);
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

TEST(QA_GDB355, OverlongSlashC0AF) {
    BertNormalizer norm(false, false, false, false);
    // C0 AF → U+002F (/) as 2-byte (overlong). Should produce U+FFFD.
    std::string input = "\xC0\xAF";
    std::string result = norm.normalize(input);
    EXPECT_EQ(result.find('/'), std::string::npos);
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

TEST(QA_GDB355, OverlongC1BF) {
    BertNormalizer norm(false, false, false, false);
    // C1 BF → U+007F (DEL) as 2-byte (overlong). Should produce U+FFFD.
    std::string input = "\xC1\xBF";
    std::string result = norm.normalize(input);
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

// ===================================================================
// 3-byte overlong encodings (codepoints < 0x800 encoded as 3 bytes)
// ===================================================================

TEST(QA_GDB355, Overlong3ByteNulE08080) {
    BertNormalizer norm(false, false, false, false);
    // E0 80 80 → U+0000 as 3-byte (overlong). Should produce U+FFFD.
    std::string input = "\xE0\x80\x80";
    std::string result = norm.normalize(input);
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

TEST(QA_GDB355, Overlong3ByteSpaceE080A0) {
    BertNormalizer norm(false, false, false, false);
    // E0 80 A0 → U+0020 (space) as 3-byte (overlong). Should produce U+FFFD.
    std::string input = "\xE0\x80\xA0";
    std::string result = norm.normalize(input);
    EXPECT_EQ(result.find(' '), std::string::npos);
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

TEST(QA_GDB355, Overlong3ByteE09FBF) {
    BertNormalizer norm(false, false, false, false);
    // E0 9F BF → U+07FF (maximum 2-byte codepoint) as 3-byte (overlong).
    std::string input = "\xE0\x9F\xBF";
    std::string result = norm.normalize(input);
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

// ===================================================================
// 4-byte overlong encodings (codepoints < 0x10000 encoded as 4 bytes)
// ===================================================================

TEST(QA_GDB355, Overlong4ByteNulF0808080) {
    BertNormalizer norm(false, false, false, false);
    // F0 80 80 80 → U+0000 as 4-byte (overlong). Should produce U+FFFD.
    std::string input = "\xF0\x80\x80\x80";
    std::string result = norm.normalize(input);
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

TEST(QA_GDB355, Overlong4ByteF08FBFBF) {
    BertNormalizer norm(false, false, false, false);
    // F0 8F BF BF → U+FFFF (maximum 3-byte codepoint) as 4-byte (overlong).
    std::string input = "\xF0\x8F\xBF\xBF";
    std::string result = norm.normalize(input);
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

// ===================================================================
// Valid minimum encodings (should NOT be rejected)
// ===================================================================

TEST(QA_GDB355, Valid2ByteMinimumC280) {
    BertNormalizer norm(false, false, false, false);
    // C2 80 → U+0080 — the minimum valid 2-byte encoding.
    // U+0080 is a C1 control character; it should be decoded (not overlong-rejected).
    // With clean_text=false, control chars pass through.
    std::string input = "\xC2\x80";
    std::string result = norm.normalize(input);
    // Should NOT contain replacement character for overlong rejection.
    // (U+0080 as UTF-8 = C2 80 — round-trips correctly.)
    EXPECT_EQ(result, "\xC2\x80");
}

TEST(QA_GDB355, Valid3ByteMinimumE0A080) {
    BertNormalizer norm(false, false, false, false);
    // E0 A0 80 → U+0800 — the minimum valid 3-byte encoding.
    std::string input = "\xE0\xA0\x80";
    std::string result = norm.normalize(input);
    EXPECT_EQ(result, "\xE0\xA0\x80");
}

TEST(QA_GDB355, Valid4ByteMinimumF0908080) {
    BertNormalizer norm(false, false, false, false);
    // F0 90 80 80 → U+10000 — the minimum valid 4-byte encoding.
    std::string input = "\xF0\x90\x80\x80";
    std::string result = norm.normalize(input);
    EXPECT_EQ(result, "\xF0\x90\x80\x80");
}

// ===================================================================
// Overlong sequences embedded in valid text
// ===================================================================

TEST(QA_GDB355, OverlongInMiddleOfText) {
    BertNormalizer norm(false, false, false, false);
    // "abc" + C0 80 (overlong NUL) + "def"
    std::string input = "abc\xC0\x80"
                        "def";
    std::string result = norm.normalize(input);
    // The overlong bytes should produce replacement chars, not pass through.
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
    // The valid ASCII should still be there.
    EXPECT_NE(result.find("abc"), std::string::npos);
    EXPECT_NE(result.find("def"), std::string::npos);
}

TEST(QA_GDB355, MultipleOverlongSequences) {
    BertNormalizer norm(false, false, false, false);
    // Two consecutive overlong sequences.
    std::string input = "\xC0\x80\xC0\xA0";
    std::string result = norm.normalize(input);
    // Should contain multiple replacement characters.
    size_t first = result.find(REPLACEMENT);
    ASSERT_NE(first, std::string::npos);
    size_t second = result.find(REPLACEMENT, first + REPLACEMENT.size());
    EXPECT_NE(second, std::string::npos);
}

// ===================================================================
// Stress: many overlong sequences
// ===================================================================

TEST(QA_GDB355, StressManyOverlongSequences) {
    BertNormalizer norm(false, false, false, false);
    // 500 overlong sequences (C0 80 each).
    std::string input;
    for (int i = 0; i < 500; ++i) {
        input += "\xC0\x80";
    }
    std::string result = norm.normalize(input);
    // Should not crash or hang. Result should contain replacement characters.
    EXPECT_FALSE(result.empty());
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

// ===================================================================
// LowercaseNormalizer also uses decode_utf8 — verify overlong rejection
// ===================================================================

TEST(QA_GDB355, LowercaseNormalizerRejectsOverlong) {
    LowercaseNormalizer norm;
    std::string input = "a\xC0\x80"
                        "b";
    std::string result = norm.normalize(input);
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
    EXPECT_NE(result.find("a"), std::string::npos);
    EXPECT_NE(result.find("b"), std::string::npos);
}

} // namespace
} // namespace sixseven
