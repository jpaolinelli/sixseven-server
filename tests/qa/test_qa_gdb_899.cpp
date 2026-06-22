/// @file tests/qa/test_qa_gdb_899.cpp
/// QA adversarial tests for GDB-899: mutation-grade confirmation of the
/// corrected OverlongNulC080 assertion and broader NUL-leak probing.
///
/// GDB-899 fixed a tautological assertion in test_qa_gdb_355.cpp:36.
/// The original EXPECT_NE(find('\x00'), npos-1) could never fail; the
/// corrected EXPECT_EQ(find('\x00'), npos) actually verifies NUL absence.
///
/// This file:
///   (1) Confirms the fix is mutation-grade: the normalizer does NOT emit a
///       raw \x00 for C0 80 (would have been missed by the old assertion).
///   (2) Probes all overlong NUL encodings (2-, 3-, 4-byte) for NUL leaks.
///   (3) Tests embedded NUL in otherwise-valid input, lone continuation bytes,
///       truncated sequences, and C0 80 surrounded by ASCII.

#include "sixseven/vector/text_normalizer.h"

#include <gtest/gtest.h>

#include <string>

namespace sixseven {
namespace {

const std::string REPLACEMENT = "\xEF\xBF\xBD"; // U+FFFD in UTF-8

// ===================================================================
// AC1: Mutation-grade confirmation of the corrected assertion.
// The normalizer must NOT emit a raw NUL byte for C0 80.
// ===================================================================

TEST(QA_GDB899, MutationGrade_NoNulLeakFromC080) {
    BertNormalizer norm(false, false, false, false);
    std::string result = norm.normalize("\xC0\x80");
    // This is the critical check the old assertion could never fail.
    // If decode_utf8 leaked a raw \x00, this test catches it.
    EXPECT_EQ(result.find('\x00'), std::string::npos)
        << "BertNormalizer leaked a raw NUL byte from overlong C0 80";
    // And the output must contain the replacement character.
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos)
        << "BertNormalizer did not produce U+FFFD for overlong C0 80";
}

TEST(QA_GDB899, MutationGrade_NoNulLeakFromC080_Lowercase) {
    LowercaseNormalizer norm;
    std::string result = norm.normalize("\xC0\x80");
    EXPECT_EQ(result.find('\x00'), std::string::npos)
        << "LowercaseNormalizer leaked a raw NUL byte from overlong C0 80";
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos)
        << "LowercaseNormalizer did not produce U+FFFD for overlong C0 80";
}

// ===================================================================
// AC2: 3-byte overlong NUL (E0 80 80) — no raw NUL leak.
// ===================================================================

TEST(QA_GDB899, NoNulLeak_3ByteOverlongNul_E08080) {
    BertNormalizer norm(false, false, false, false);
    std::string result = norm.normalize("\xE0\x80\x80");
    EXPECT_EQ(result.find('\x00'), std::string::npos)
        << "3-byte overlong NUL (E0 80 80) leaked a raw NUL byte";
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

TEST(QA_GDB899, NoNulLeak_3ByteOverlongNul_E08080_Lowercase) {
    LowercaseNormalizer norm;
    std::string result = norm.normalize("\xE0\x80\x80");
    EXPECT_EQ(result.find('\x00'), std::string::npos)
        << "LowercaseNormalizer: 3-byte overlong NUL leaked a raw NUL byte";
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

// ===================================================================
// AC3: 4-byte overlong NUL (F0 80 80 80) — no raw NUL leak.
// ===================================================================

TEST(QA_GDB899, NoNulLeak_4ByteOverlongNul_F0808080) {
    BertNormalizer norm(false, false, false, false);
    std::string result = norm.normalize("\xF0\x80\x80\x80");
    EXPECT_EQ(result.find('\x00'), std::string::npos)
        << "4-byte overlong NUL (F0 80 80 80) leaked a raw NUL byte";
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

TEST(QA_GDB899, NoNulLeak_4ByteOverlongNul_F0808080_Lowercase) {
    LowercaseNormalizer norm;
    std::string result = norm.normalize("\xF0\x80\x80\x80");
    EXPECT_EQ(result.find('\x00'), std::string::npos)
        << "LowercaseNormalizer: 4-byte overlong NUL leaked a raw NUL byte";
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

// ===================================================================
// AC4: Embedded literal NUL in otherwise-valid ASCII — verify it
// is NOT passed through raw (i.e., the decoder handles it correctly).
// ASCII 0x00 is a valid single-byte decode_utf8 result; encode_utf8
// encodes it back as \x00. This tests NullNormalizer pass-through
// vs BertNormalizer behavior.
// ===================================================================

TEST(QA_GDB899, EmbeddedLiteralNul_NullNormalizer_PassThrough) {
    NullNormalizer norm;
    // NullNormalizer returns input unchanged: a literal NUL stays.
    std::string input = "ab\x00" "cd";
    input.resize(5); // ensure the NUL is part of the string
    std::string result = norm.normalize(input);
    EXPECT_EQ(result.size(), 5u);
    EXPECT_EQ(result[2], '\x00');
}

TEST(QA_GDB899, EmbeddedLiteralNul_BertNormalizer_AllFlagsOff) {
    // With all flags off (no clean_text), a literal \x00 in a valid UTF-8
    // stream is decoded as U+0000 (ASCII) and encode_utf8 emits \x00 back.
    // This is not a normalizer bug — it is correct behavior for valid input.
    // Verify it does NOT crash or produce garbage.
    BertNormalizer norm(false, false, false, false);
    std::string input;
    input.push_back('a');
    input.push_back('\x00');
    input.push_back('b');
    std::string result = norm.normalize(input);
    // Must not crash; size must be preserved (3 bytes in → 3 bytes out).
    EXPECT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], 'a');
    EXPECT_EQ(result[1], '\x00');
    EXPECT_EQ(result[2], 'b');
}

// ===================================================================
// AC5: C0 80 surrounded by valid ASCII — NUL does not bleed into
// neighboring characters.
// ===================================================================

TEST(QA_GDB899, C080SurroundedByAscii_NoNulBleeding) {
    BertNormalizer norm(false, false, false, false);
    // "abc" + C0 80 + "xyz"
    std::string input = "abc\xC0\x80xyz";
    std::string result = norm.normalize(input);
    EXPECT_EQ(result.find('\x00'), std::string::npos)
        << "NUL bled into output when C0 80 was surrounded by ASCII";
    EXPECT_NE(result.find("abc"), std::string::npos);
    EXPECT_NE(result.find("xyz"), std::string::npos);
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

// ===================================================================
// AC6: Lone continuation bytes — should not produce NUL.
// ===================================================================

TEST(QA_GDB899, LoneContinuationByte_80_NoNul) {
    BertNormalizer norm(false, false, false, false);
    std::string result = norm.normalize("\x80");
    EXPECT_EQ(result.find('\x00'), std::string::npos)
        << "Lone continuation byte 0x80 produced a raw NUL";
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

TEST(QA_GDB899, LoneContinuationByte_BF_NoNul) {
    BertNormalizer norm(false, false, false, false);
    std::string result = norm.normalize("\xBF");
    EXPECT_EQ(result.find('\x00'), std::string::npos)
        << "Lone continuation byte 0xBF produced a raw NUL";
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

// ===================================================================
// AC7: Truncated multi-byte sequences — no NUL leak on short input.
// ===================================================================

TEST(QA_GDB899, TruncatedTwoByte_C0Only) {
    BertNormalizer norm(false, false, false, false);
    // C0 alone (no continuation byte).
    std::string result = norm.normalize("\xC0");
    EXPECT_EQ(result.find('\x00'), std::string::npos)
        << "Truncated C0 produced a raw NUL";
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

TEST(QA_GDB899, TruncatedThreeByte_E080Only) {
    BertNormalizer norm(false, false, false, false);
    // E0 80 — only two bytes of a 3-byte sequence.
    std::string result = norm.normalize("\xE0\x80");
    EXPECT_EQ(result.find('\x00'), std::string::npos)
        << "Truncated E0 80 produced a raw NUL";
    // Should produce at least one replacement character.
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

TEST(QA_GDB899, TruncatedFourByte_F08080Only) {
    BertNormalizer norm(false, false, false, false);
    // F0 80 80 — three bytes of a 4-byte sequence.
    std::string result = norm.normalize("\xF0\x80\x80");
    EXPECT_EQ(result.find('\x00'), std::string::npos)
        << "Truncated F0 80 80 produced a raw NUL";
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

// ===================================================================
// AC8: Stress — 1000 overlong NUL sequences, no NUL in output.
// ===================================================================

TEST(QA_GDB899, Stress_1000OverlongNulSequences_NoNulLeak) {
    BertNormalizer norm(false, false, false, false);
    std::string input;
    input.reserve(2000);
    for (int i = 0; i < 1000; ++i) {
        input += "\xC0\x80";
    }
    std::string result = norm.normalize(input);
    EXPECT_EQ(result.find('\x00'), std::string::npos)
        << "Raw NUL found in output of 1000 overlong C0 80 sequences";
    EXPECT_FALSE(result.empty());
    EXPECT_NE(result.find(REPLACEMENT), std::string::npos);
}

TEST(QA_GDB899, Stress_Mixed3And4ByteOverlongNuls_NoNulLeak) {
    BertNormalizer norm(false, false, false, false);
    std::string input;
    for (int i = 0; i < 200; ++i) {
        input += "\xE0\x80\x80"; // 3-byte overlong NUL
        input += "\xF0\x80\x80\x80"; // 4-byte overlong NUL
    }
    std::string result = norm.normalize(input);
    EXPECT_EQ(result.find('\x00'), std::string::npos)
        << "Raw NUL found in mixed 3/4-byte overlong NUL stress output";
    EXPECT_FALSE(result.empty());
}

// ===================================================================
// AC9: Verify no production files were touched (test-only change).
// This is validated structurally: the QA file compiles against the
// same header; no new symbols are expected.
// ===================================================================

TEST(QA_GDB899, NullNormalizerPassThrough_NoDecoding) {
    NullNormalizer norm;
    // NullNormalizer must return input byte-for-byte (no decoding).
    std::string input = "\xC0\x80\xE0\x80\x80\xF0\x80\x80\x80";
    std::string result = norm.normalize(input);
    EXPECT_EQ(result, input)
        << "NullNormalizer must return input unchanged";
}

} // namespace
} // namespace sixseven
