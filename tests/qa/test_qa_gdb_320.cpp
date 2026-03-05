/// @file tests/qa/test_qa_gdb_320.cpp
/// QA adversarial tests for GDB-320: Text Normalizer.

#include "sixseven/vector/text_normalizer.h"
#include "sixseven/vector/tokenizer.h"

#include <gtest/gtest.h>

#include <string>

using namespace sixseven;

// ===================================================================
// QA_BertNormalizer — Boundary & Edge Cases
// ===================================================================

TEST(QA_GDB320_BertNormalizer, EmptyStringReturnsEmpty) {
    BertNormalizer norm(true, true, true, true);
    EXPECT_EQ(norm.normalize(""), "");
}

TEST(QA_GDB320_BertNormalizer, SingleAsciiSpace) {
    BertNormalizer norm;
    EXPECT_EQ(norm.normalize(" "), "");
}

TEST(QA_GDB320_BertNormalizer, SingleTab) {
    BertNormalizer norm;
    EXPECT_EQ(norm.normalize("\t"), "");
}

TEST(QA_GDB320_BertNormalizer, SingleNewline) {
    BertNormalizer norm;
    EXPECT_EQ(norm.normalize("\n"), "");
}

TEST(QA_GDB320_BertNormalizer, SingleAsciiChar) {
    BertNormalizer norm;
    EXPECT_EQ(norm.normalize("x"), "x");
}

TEST(QA_GDB320_BertNormalizer, OnlyWhitespace) {
    BertNormalizer norm;
    EXPECT_EQ(norm.normalize("   \t\n\r   "), "");
}

TEST(QA_GDB320_BertNormalizer, LeadingWhitespaceRemoved) {
    BertNormalizer norm;
    EXPECT_EQ(norm.normalize("   hello"), "hello");
}

TEST(QA_GDB320_BertNormalizer, TrailingWhitespaceRemoved) {
    BertNormalizer norm;
    EXPECT_EQ(norm.normalize("hello   "), "hello");
}

TEST(QA_GDB320_BertNormalizer, ManyConsecutiveSpaces) {
    BertNormalizer norm;
    std::string input = "a" + std::string(100, ' ') + "b";
    EXPECT_EQ(norm.normalize(input), "a b");
}

TEST(QA_GDB320_BertNormalizer, MixedWhitespaceTypes) {
    BertNormalizer norm;
    // space, tab, newline, carriage return, no-break space (C2 A0)
    EXPECT_EQ(norm.normalize("a \t\n\r\xC2\xA0"
                             "b"),
              "a b");
}

TEST(QA_GDB320_BertNormalizer, UnicodeWhitespaceEmSpace) {
    BertNormalizer norm;
    // U+2003 EM SPACE (E2 80 83)
    EXPECT_EQ(norm.normalize("a\xE2\x80\x83"
                             "b"),
              "a b");
}

TEST(QA_GDB320_BertNormalizer, UnicodeWhitespaceIdeographic) {
    BertNormalizer norm;
    // U+3000 IDEOGRAPHIC SPACE (E3 80 80) — should collapse to regular space.
    EXPECT_EQ(norm.normalize("a\xE3\x80\x80"
                             "b"),
              "a b");
}

// ===================================================================
// QA_BertNormalizer — Control Characters
// ===================================================================

TEST(QA_GDB320_BertNormalizer, NullByteRemoved) {
    BertNormalizer norm;
    std::string input("a\x00"
                      "b",
                      3);
    EXPECT_EQ(norm.normalize(input), "ab");
}

TEST(QA_GDB320_BertNormalizer, AllControlCharsRemoved) {
    BertNormalizer norm;
    std::string input;
    input.push_back('a');
    // Add all C0 control chars except tab/newline/CR.
    for (char c = 0x01; c <= 0x1F; ++c) {
        if (c != '\t' && c != '\n' && c != '\r') {
            input.push_back(c);
        }
    }
    input.push_back('b');
    EXPECT_EQ(norm.normalize(input), "ab");
}

TEST(QA_GDB320_BertNormalizer, DeleteCharRemoved) {
    BertNormalizer norm;
    // DEL is 0x7F — a control character.
    EXPECT_EQ(norm.normalize("a\x7F"
                             "b"),
              "ab");
}

TEST(QA_GDB320_BertNormalizer, C1ControlRangeRemoved) {
    BertNormalizer norm;
    // U+0080 to U+009F are C1 control characters.
    // U+0085 NEL (C2 85) should be removed.
    // U+0090 DCS (C2 90) should be removed.
    std::string input = "a\xC2\x85"
                        "b\xC2\x90"
                        "c";
    EXPECT_EQ(norm.normalize(input), "abc");
}

TEST(QA_GDB320_BertNormalizer, OnlyControlCharsYieldsEmpty) {
    BertNormalizer norm;
    std::string input("\x01\x02\x03\x7F", 4);
    EXPECT_EQ(norm.normalize(input), "");
}

// ===================================================================
// QA_BertNormalizer — Accent Stripping
// ===================================================================

TEST(QA_GDB320_BertNormalizer, StripAllLatin1UpperAccented) {
    BertNormalizer norm(true, true, true, true);
    // A with grave through O with stroke, skipping multiplication sign and
    // non-letter positions.
    EXPECT_EQ(norm.normalize("\xC3\x80"), "a"); // U+00C0 À → a
    EXPECT_EQ(norm.normalize("\xC3\x81"), "a"); // U+00C1 Á → a
    EXPECT_EQ(norm.normalize("\xC3\x82"), "a"); // U+00C2 Â → a
    EXPECT_EQ(norm.normalize("\xC3\x83"), "a"); // U+00C3 Ã → a
    EXPECT_EQ(norm.normalize("\xC3\x84"), "a"); // U+00C4 Ä → a
    EXPECT_EQ(norm.normalize("\xC3\x85"), "a"); // U+00C5 Å → a
    EXPECT_EQ(norm.normalize("\xC3\x87"), "c"); // U+00C7 Ç → c
    EXPECT_EQ(norm.normalize("\xC3\x88"), "e"); // U+00C8 È → e
    EXPECT_EQ(norm.normalize("\xC3\x89"), "e"); // U+00C9 É → e
    EXPECT_EQ(norm.normalize("\xC3\x8A"), "e"); // U+00CA Ê → e
    EXPECT_EQ(norm.normalize("\xC3\x8B"), "e"); // U+00CB Ë → e
    EXPECT_EQ(norm.normalize("\xC3\x91"), "n"); // U+00D1 Ñ → n
    EXPECT_EQ(norm.normalize("\xC3\x9C"), "u"); // U+00DC Ü → u
}

TEST(QA_GDB320_BertNormalizer, StripAllLatin1LowerAccented) {
    BertNormalizer norm(true, true, true, true);
    EXPECT_EQ(norm.normalize("\xC3\xA0"), "a"); // U+00E0 à → a
    EXPECT_EQ(norm.normalize("\xC3\xA9"), "e"); // U+00E9 é → e
    EXPECT_EQ(norm.normalize("\xC3\xB1"), "n"); // U+00F1 ñ → n
    EXPECT_EQ(norm.normalize("\xC3\xB6"), "o"); // U+00F6 ö → o
    EXPECT_EQ(norm.normalize("\xC3\xBC"), "u"); // U+00FC ü → u
    EXPECT_EQ(norm.normalize("\xC3\xBF"), "y"); // U+00FF ÿ → y
}

TEST(QA_GDB320_BertNormalizer, StripAccentsWithoutLowercase) {
    BertNormalizer norm(false, true, false, false);
    // Strip accents but don't lowercase: É → E (not e).
    EXPECT_EQ(norm.normalize("\xC3\x89"), "E"); // U+00C9 É → E
    EXPECT_EQ(norm.normalize("\xC3\xA9"), "e"); // U+00E9 é → e
}

TEST(QA_GDB320_BertNormalizer, StripAccentsPrecomposedCombiningMark) {
    BertNormalizer norm(false, true, false, false);
    // Input: 'e' + U+0301 (combining acute accent).
    // The combining mark should be stripped, leaving just 'e'.
    EXPECT_EQ(norm.normalize("e\xCC\x81"), "e");
}

TEST(QA_GDB320_BertNormalizer, StripAccentsOnlyCombiningMarks) {
    BertNormalizer norm(false, true, false, false);
    // Input: only combining marks (U+0300, U+0301, U+0302).
    EXPECT_EQ(norm.normalize("\xCC\x80\xCC\x81\xCC\x82"), "");
}

TEST(QA_GDB320_BertNormalizer, AccentsNoStripPreservesAccents) {
    BertNormalizer norm(true, false, false, false);
    // U+00E9 should be lowered but not stripped.
    EXPECT_EQ(norm.normalize("\xC3\xA9"), "\xC3\xA9");
}

// ===================================================================
// QA_BertNormalizer — CJK Character Handling
// ===================================================================

TEST(QA_GDB320_BertNormalizer, SingleCJKCharacter) {
    BertNormalizer norm;
    // U+4E2D (中) — should be surrounded by spaces, then trimmed.
    EXPECT_EQ(norm.normalize("\xE4\xB8\xAD"), "\xE4\xB8\xAD");
}

TEST(QA_GDB320_BertNormalizer, MultipleCJKCharacters) {
    BertNormalizer norm;
    // 中文 (U+4E2D U+6587)
    std::string input = "\xE4\xB8\xAD\xE6\x96\x87";
    EXPECT_EQ(norm.normalize(input), "\xE4\xB8\xAD \xE6\x96\x87");
}

TEST(QA_GDB320_BertNormalizer, CJKPrefixedByLatin) {
    BertNormalizer norm;
    // "hello中" — space inserted between Latin and CJK.
    EXPECT_EQ(norm.normalize("hello\xE4\xB8\xAD"), "hello \xE4\xB8\xAD");
}

TEST(QA_GDB320_BertNormalizer, CJKFollowedByLatin) {
    BertNormalizer norm;
    // "中hello" — space inserted between CJK and Latin.
    EXPECT_EQ(norm.normalize("\xE4\xB8\xAD"
                             "hello"),
              "\xE4\xB8\xAD hello");
}

TEST(QA_GDB320_BertNormalizer, CJKSurroundedBySpaces) {
    BertNormalizer norm;
    // " 中 " — spaces already present; should collapse correctly.
    EXPECT_EQ(norm.normalize(" \xE4\xB8\xAD "), "\xE4\xB8\xAD");
}

TEST(QA_GDB320_BertNormalizer, CJKExtensionBRange) {
    BertNormalizer norm;
    // U+20000 (CJK Extension B — 4-byte UTF-8: F0 A0 80 80).
    std::string input = "a\xF0\xA0\x80\x80"
                        "b";
    EXPECT_EQ(norm.normalize(input), "a \xF0\xA0\x80\x80 b");
}

TEST(QA_GDB320_BertNormalizer, CJKCompatibilityIdeographs) {
    BertNormalizer norm;
    // U+F900 (CJK Compatibility Ideographs range start).
    std::string input = "a\xEF\xA4\x80"
                        "b";
    EXPECT_EQ(norm.normalize(input), "a \xEF\xA4\x80 b");
}

TEST(QA_GDB320_BertNormalizer, CJKHandlingDisabledNoSpaces) {
    BertNormalizer norm(true, false, false, true);
    // CJK chars without spacing.
    std::string input = "\xE4\xB8\xAD\xE6\x96\x87";
    EXPECT_EQ(norm.normalize(input), input);
}

// ===================================================================
// QA_BertNormalizer — Invalid UTF-8
// ===================================================================

TEST(QA_GDB320_BertNormalizer, LoneContinuationByte) {
    BertNormalizer norm(false, false, false, false);
    // 0x80 is a lone continuation byte — should produce U+FFFD.
    std::string input("a\x80"
                      "b",
                      3);
    // U+FFFD in UTF-8 is EF BF BD.
    std::string expected = "a\xEF\xBF\xBD"
                           "b";
    EXPECT_EQ(norm.normalize(input), expected);
}

TEST(QA_GDB320_BertNormalizer, TruncatedTwoByteSequence) {
    BertNormalizer norm(false, false, false, false);
    // 0xC3 alone at end of string (expects continuation byte).
    std::string input = "a\xC3";
    std::string expected = "a\xEF\xBF\xBD";
    EXPECT_EQ(norm.normalize(input), expected);
}

TEST(QA_GDB320_BertNormalizer, TruncatedThreeByteSequence) {
    BertNormalizer norm(false, false, false, false);
    // 0xE4 0xB8 at end (expects one more continuation byte).
    std::string input = "a\xE4\xB8";
    // The decoder sees the leading byte, checks if pos+extra >= len.
    // pos=1, extra=2, len=3. 1+2 >= 3 is true, so returns FFFD + advance 1.
    // Then the next byte 0xB8 is a continuation byte (0x80-0xBF), processed
    // as a lone continuation byte → FFFD.
    std::string expected = "a\xEF\xBF\xBD\xEF\xBF\xBD";
    EXPECT_EQ(norm.normalize(input), expected);
}

TEST(QA_GDB320_BertNormalizer, InvalidLeadingByte0xFF) {
    BertNormalizer norm(false, false, false, false);
    // 0xFF is never valid in UTF-8.
    std::string input("a\xFF"
                      "b",
                      3);
    std::string expected = "a\xEF\xBF\xBD"
                           "b";
    EXPECT_EQ(norm.normalize(input), expected);
}

TEST(QA_GDB320_BertNormalizer, InvalidLeadingByte0xFE) {
    BertNormalizer norm(false, false, false, false);
    std::string input("a\xFE"
                      "b",
                      3);
    std::string expected = "a\xEF\xBF\xBD"
                           "b";
    EXPECT_EQ(norm.normalize(input), expected);
}

// ===================================================================
// QA_BertNormalizer — All Flags Disabled
// ===================================================================

TEST(QA_GDB320_BertNormalizer, AllFlagsDisabledPassThrough) {
    BertNormalizer norm(false, false, false, false);
    // With all options disabled, input should pass through unchanged
    // (except trimming always applies).
    std::string input = "HELLO World\t\n";
    EXPECT_EQ(norm.normalize(input), input);
}

TEST(QA_GDB320_BertNormalizer, AllFlagsDisabledControlCharsPreserved) {
    BertNormalizer norm(false, false, false, false);
    std::string input("A\x01"
                      "B",
                      3);
    // Control chars NOT removed since clean_text=false.
    EXPECT_EQ(norm.normalize(input), input);
}

// ===================================================================
// QA_BertNormalizer — Combined Operations
// ===================================================================

TEST(QA_GDB320_BertNormalizer, AllFeaturesEnabledComplex) {
    BertNormalizer norm(true, true, true, true);
    // Input: uppercase + accented + CJK + control chars + mixed whitespace.
    // "  CAFÉ\x01 \t 中文 Test\n  "
    std::string input = "  CAF\xC3\x89\x01 \t \xE4\xB8\xAD\xE6\x96\x87 Test\n  ";
    // Expected: lowercase + accents stripped + control removed + whitespace
    // collapsed + CJK spaced + trimmed.
    EXPECT_EQ(norm.normalize(input), "cafe \xE4\xB8\xAD \xE6\x96\x87 test");
}

TEST(QA_GDB320_BertNormalizer, StripAccentsAndCJKTogether) {
    BertNormalizer norm(true, true, true, true);
    // "Ñ中" — accent stripped, CJK spaced.
    std::string input = "\xC3\x91\xE4\xB8\xAD";
    EXPECT_EQ(norm.normalize(input), "n \xE4\xB8\xAD");
}

// ===================================================================
// QA_BertNormalizer — Stress / Scale
// ===================================================================

TEST(QA_GDB320_BertNormalizer, VeryLongAsciiString) {
    BertNormalizer norm;
    // 10000 chars of "hello " repeated.
    std::string input;
    for (int i = 0; i < 1000; ++i) {
        input += "hello world ";
    }
    auto result = norm.normalize(input);
    // Should not crash and should produce a trimmed, collapsed result.
    EXPECT_FALSE(result.empty());
    // No double spaces in result.
    EXPECT_EQ(result.find("  "), std::string::npos);
}

TEST(QA_GDB320_BertNormalizer, VeryLongUnicodeString) {
    BertNormalizer norm(true, true, true, true);
    // 1000 accented characters: é repeated.
    std::string input;
    for (int i = 0; i < 1000; ++i) {
        input += "\xC3\xA9"; // U+00E9 é
    }
    auto result = norm.normalize(input);
    EXPECT_EQ(result, std::string(1000, 'e'));
}

TEST(QA_GDB320_BertNormalizer, ManyCJKCharacters) {
    BertNormalizer norm;
    // 500 CJK characters (U+4E00 repeated).
    std::string input;
    for (int i = 0; i < 500; ++i) {
        input += "\xE4\xB8\x80"; // U+4E00
    }
    auto result = norm.normalize(input);
    EXPECT_FALSE(result.empty());
    // Each CJK char should be space-separated.
    // 500 chars with spaces between = 500 * (3 bytes) + 499 spaces.
    size_t space_count = 0;
    for (char c : result) {
        if (c == ' ') {
            ++space_count;
        }
    }
    EXPECT_EQ(space_count, 499u);
}

TEST(QA_GDB320_BertNormalizer, AllControlCharsLongSequence) {
    BertNormalizer norm;
    // 500 control chars.
    std::string input(500, '\x01');
    EXPECT_EQ(norm.normalize(input), "");
}

// ===================================================================
// QA_BertNormalizer — to_lower edge case (U+0138 bug)
// ===================================================================

TEST(QA_GDB320_BertNormalizer, ToLowerKraCharacter) {
    // U+0138 is ĸ (Latin Small Letter Kra) — already lowercase.
    // to_lower should NOT change it. But the implementation treats
    // all even codepoints in 0x0100-0x017E as uppercase.
    // This test documents the expected correct behavior.
    BertNormalizer norm(true, false, false, false);
    // U+0138 in UTF-8: C4 B8
    std::string input = "\xC4\xB8";
    // Correct behavior: ĸ stays as ĸ (already lowercase).
    // Bug: converts to U+0139 (Ĺ) = C4 B9.
    EXPECT_EQ(norm.normalize(input), "\xC4\xB8");
}

// ===================================================================
// QA_LowercaseNormalizer — Boundary & Edge Cases
// ===================================================================

TEST(QA_GDB320_LowercaseNormalizer, EmptyString) {
    LowercaseNormalizer norm;
    EXPECT_EQ(norm.normalize(""), "");
}

TEST(QA_GDB320_LowercaseNormalizer, AllUppercase) {
    LowercaseNormalizer norm;
    EXPECT_EQ(norm.normalize("ABCDEFGHIJKLMNOPQRSTUVWXYZ"), "abcdefghijklmnopqrstuvwxyz");
}

TEST(QA_GDB320_LowercaseNormalizer, AllLowercase) {
    LowercaseNormalizer norm;
    EXPECT_EQ(norm.normalize("abcdefghijklmnopqrstuvwxyz"), "abcdefghijklmnopqrstuvwxyz");
}

TEST(QA_GDB320_LowercaseNormalizer, NumbersAndSymbols) {
    LowercaseNormalizer norm;
    EXPECT_EQ(norm.normalize("123!@#$%^&*()"), "123!@#$%^&*()");
}

TEST(QA_GDB320_LowercaseNormalizer, WhitespacePreserved) {
    LowercaseNormalizer norm;
    EXPECT_EQ(norm.normalize("  A  B  "), "  a  b  ");
}

TEST(QA_GDB320_LowercaseNormalizer, ControlCharsPreserved) {
    LowercaseNormalizer norm;
    std::string input("A\x01\x02"
                      "B",
                      4);
    std::string expected("a\x01\x02"
                         "b",
                         4);
    EXPECT_EQ(norm.normalize(input), expected);
}

TEST(QA_GDB320_LowercaseNormalizer, LatinExtendedUpperToLower) {
    LowercaseNormalizer norm;
    // U+0160 (Š) → U+0161 (š)
    EXPECT_EQ(norm.normalize("\xC5\xA0"), "\xC5\xA1");
    // U+017D (Ž) → U+017E (ž)
    EXPECT_EQ(norm.normalize("\xC5\xBD"), "\xC5\xBE");
}

TEST(QA_GDB320_LowercaseNormalizer, MultiplicationSignNotLowered) {
    LowercaseNormalizer norm;
    // U+00D7 × (multiplication sign) should NOT be lowercased.
    EXPECT_EQ(norm.normalize("\xC3\x97"), "\xC3\x97");
}

TEST(QA_GDB320_LowercaseNormalizer, KraCharacter) {
    LowercaseNormalizer norm;
    // U+0138 (ĸ) should remain unchanged.
    // Bug: incorrectly converts to U+0139.
    EXPECT_EQ(norm.normalize("\xC4\xB8"), "\xC4\xB8");
}

TEST(QA_GDB320_LowercaseNormalizer, VeryLongString) {
    LowercaseNormalizer norm;
    std::string input(10000, 'A');
    std::string expected(10000, 'a');
    EXPECT_EQ(norm.normalize(input), expected);
}

// ===================================================================
// QA_NullNormalizer — Boundary & Edge Cases
// ===================================================================

TEST(QA_GDB320_NullNormalizer, EmptyString) {
    NullNormalizer norm;
    EXPECT_EQ(norm.normalize(""), "");
}

TEST(QA_GDB320_NullNormalizer, PreservesEverythingIncludingWhitespace) {
    NullNormalizer norm;
    EXPECT_EQ(norm.normalize("  HELLO  \t\n"), "  HELLO  \t\n");
}

TEST(QA_GDB320_NullNormalizer, PreservesControlChars) {
    NullNormalizer norm;
    std::string input("A\x01\x00"
                      "B",
                      4);
    EXPECT_EQ(norm.normalize(input), input);
}

TEST(QA_GDB320_NullNormalizer, PreservesInvalidUTF8) {
    NullNormalizer norm;
    std::string input("a\xFF\xFE"
                      "b",
                      4);
    EXPECT_EQ(norm.normalize(input), input);
}

TEST(QA_GDB320_NullNormalizer, VeryLongString) {
    NullNormalizer norm;
    std::string input(10000, 'X');
    EXPECT_EQ(norm.normalize(input), input);
}

// ===================================================================
// QA_CreateNormalizer — Factory Edge Cases
// ===================================================================

TEST(QA_GDB320_Factory, BertWithAllDefaults) {
    TokenizerConfig config;
    config.normalizer = NormalizerType::BERT;
    // Defaults: lowercase=true, strip_accents=false.
    auto norm = create_normalizer(config);
    ASSERT_NE(norm, nullptr);
    EXPECT_EQ(norm->normalize("HELLO"), "hello");
    // Accents preserved since strip_accents=false.
    EXPECT_EQ(norm->normalize("caf\xC3\xA9"), "caf\xC3\xA9");
}

TEST(QA_GDB320_Factory, BertStripAccentsEnabled) {
    TokenizerConfig config;
    config.normalizer = NormalizerType::BERT;
    config.normalizer_lowercase = true;
    config.normalizer_strip_accents = true;
    auto norm = create_normalizer(config);
    ASSERT_NE(norm, nullptr);
    EXPECT_EQ(norm->normalize("CAF\xC3\x89"), "cafe");
}

TEST(QA_GDB320_Factory, BertNoLowercaseNoStripAccents) {
    TokenizerConfig config;
    config.normalizer = NormalizerType::BERT;
    config.normalizer_lowercase = false;
    config.normalizer_strip_accents = false;
    auto norm = create_normalizer(config);
    ASSERT_NE(norm, nullptr);
    // CJK still handled by default (handle_chinese=true in BertNormalizer).
    EXPECT_EQ(norm->normalize("Hello \xE4\xB8\xAD World"), "Hello \xE4\xB8\xAD World");
}

TEST(QA_GDB320_Factory, LowercasePreservesWhitespace) {
    TokenizerConfig config;
    config.normalizer = NormalizerType::LOWERCASE;
    auto norm = create_normalizer(config);
    ASSERT_NE(norm, nullptr);
    // LowercaseNormalizer should NOT collapse whitespace.
    EXPECT_EQ(norm->normalize("  A  B  "), "  a  b  ");
}

TEST(QA_GDB320_Factory, NoneReturnsNullNormalizer) {
    TokenizerConfig config;
    config.normalizer = NormalizerType::NONE;
    auto norm = create_normalizer(config);
    ASSERT_NE(norm, nullptr);
    // Pass-through: everything unchanged.
    std::string input = "  HELLO\x01\t\xC3\xA9  ";
    EXPECT_EQ(norm->normalize(input), input);
}

TEST(QA_GDB320_Factory, NfcReturnsNullNormalizer) {
    TokenizerConfig config;
    config.normalizer = NormalizerType::NFC;
    auto norm = create_normalizer(config);
    ASSERT_NE(norm, nullptr);
    std::string input = "Hello World";
    EXPECT_EQ(norm->normalize(input), input);
}

TEST(QA_GDB320_Factory, PolymorphicUsageThroughBasePointer) {
    TokenizerConfig config;
    config.normalizer = NormalizerType::BERT;
    config.normalizer_lowercase = true;
    config.normalizer_strip_accents = true;
    std::unique_ptr<TextNormalizer> base = create_normalizer(config);
    ASSERT_NE(base, nullptr);
    EXPECT_EQ(base->normalize("CAF\xC3\x89"), "cafe");
}

// ===================================================================
// QA_BertNormalizer — Trimming with clean_text=false
// ===================================================================

TEST(QA_GDB320_BertNormalizer, CleanTextFalseTrimmingBehavior) {
    BertNormalizer norm(false, false, false, false);
    // With clean_text=false, whitespace collapsing is disabled.
    // The trimming at the end still runs and trims leading/trailing spaces.
    // This test documents the actual behavior.
    std::string result = norm.normalize("  hello  ");
    // Trimming always applies — this is the current behavior.
    // If this is considered a bug, it should return "  hello  ".
    EXPECT_EQ(result, "  hello  ");
}
