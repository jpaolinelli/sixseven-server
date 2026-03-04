#include "giodb/vector/text_normalizer.h"
#include "giodb/vector/tokenizer.h"

#include <gtest/gtest.h>

using namespace giodb;

// ===================================================================
// BertNormalizer — Lowercase
// ===================================================================

TEST(BertNormalizer, LowercaseAscii) {
    BertNormalizer norm(/*lowercase=*/true);
    EXPECT_EQ(norm.normalize("Hello World"), "hello world");
}

TEST(BertNormalizer, LowercasePreservesAlreadyLower) {
    BertNormalizer norm(/*lowercase=*/true);
    EXPECT_EQ(norm.normalize("hello"), "hello");
}

TEST(BertNormalizer, LowercaseAllCaps) {
    BertNormalizer norm(/*lowercase=*/true);
    EXPECT_EQ(norm.normalize("HELLO WORLD"), "hello world");
}

TEST(BertNormalizer, LowercaseMixedCase) {
    BertNormalizer norm(/*lowercase=*/true);
    EXPECT_EQ(norm.normalize("HeLLo WoRLd"), "hello world");
}

TEST(BertNormalizer, NoLowercase) {
    BertNormalizer norm(/*lowercase=*/false);
    EXPECT_EQ(norm.normalize("Hello World"), "Hello World");
}

// ===================================================================
// BertNormalizer — Accent / Diacritic Stripping
// ===================================================================

TEST(BertNormalizer, StripAccentsCafe) {
    // cafe with accent -> cafe (lowercase + strip accents).
    BertNormalizer norm(/*lowercase=*/true, /*strip_accents=*/true);
    EXPECT_EQ(norm.normalize("caf\xC3\xA9"), "cafe"); // cafe with e-acute (U+00E9).
}

TEST(BertNormalizer, StripAccentsNTilde) {
    BertNormalizer norm(/*lowercase=*/true, /*strip_accents=*/true);
    EXPECT_EQ(norm.normalize("espa\xC3\xB1ol"), "espanol"); // espanol with n-tilde (U+00F1).
}

TEST(BertNormalizer, StripAccentsUUmlaut) {
    BertNormalizer norm(/*lowercase=*/true, /*strip_accents=*/true);
    EXPECT_EQ(norm.normalize("\xC3\xBC"
                             "ber"),
              "uber"); // u-umlaut (U+00FC).
}

TEST(BertNormalizer, StripAccentsUppercase) {
    BertNormalizer norm(/*lowercase=*/true, /*strip_accents=*/true);
    // ECOLE with E-acute (U+00C9) -> ecole.
    EXPECT_EQ(norm.normalize("\xC3\x89"
                             "COLE"),
              "ecole");
}

TEST(BertNormalizer, StripAccentsLatinExtendedA) {
    BertNormalizer norm(/*lowercase=*/true, /*strip_accents=*/true);
    // Sljivovica with s-caron (U+0161) and c-acute (U+0107).
    EXPECT_EQ(norm.normalize("\xC5\xA1ljivo\xC4\x87"
                             "a"),
              "sljivoca"); // s-caron + c-acute.
}

TEST(BertNormalizer, NoStripAccents) {
    BertNormalizer norm(/*lowercase=*/true, /*strip_accents=*/false);
    // Accented characters remain.
    EXPECT_EQ(norm.normalize("caf\xC3\xA9"), "caf\xC3\xA9");
}

// ===================================================================
// BertNormalizer — Control Character Removal
// ===================================================================

TEST(BertNormalizer, RemovesControlCharacters) {
    BertNormalizer norm;
    // Null byte and bell character embedded — both are control chars, removed.
    std::string with_null("hello\x00world\x07test", 16);
    EXPECT_EQ(norm.normalize(with_null), "helloworldtest");
}

TEST(BertNormalizer, RemovesC1ControlChars) {
    BertNormalizer norm;
    // C1 control range: 0x80-0x9F (UTF-8: C2 80 to C2 9F).
    std::string input = "hello\xC2\x85world"; // U+0085 NEL.
    EXPECT_EQ(norm.normalize(input), "helloworld");
}

TEST(BertNormalizer, PreservesTabNewlineCarriageReturn) {
    BertNormalizer norm;
    // Tab, newline, carriage-return are treated as whitespace, not control.
    EXPECT_EQ(norm.normalize("hello\tworld\ntest\rend"), "hello world test end");
}

// ===================================================================
// BertNormalizer — Whitespace Collapsing
// ===================================================================

TEST(BertNormalizer, CollapsesMultipleSpaces) {
    BertNormalizer norm;
    EXPECT_EQ(norm.normalize("hello   world"), "hello world");
}

TEST(BertNormalizer, CollapsesMixedWhitespace) {
    BertNormalizer norm;
    EXPECT_EQ(norm.normalize("hello \t\n world"), "hello world");
}

TEST(BertNormalizer, TrimsLeadingTrailingWhitespace) {
    BertNormalizer norm;
    EXPECT_EQ(norm.normalize("  hello world  "), "hello world");
}

TEST(BertNormalizer, AllWhitespace) {
    BertNormalizer norm;
    EXPECT_EQ(norm.normalize("   \t\n  "), "");
}

// ===================================================================
// BertNormalizer — CJK Character Handling
// ===================================================================

TEST(BertNormalizer, AddSpacesAroundCJK) {
    BertNormalizer norm(/*lowercase=*/true,
                        /*strip_accents=*/false,
                        /*handle_chinese=*/true);
    // U+4F60 (ni) U+597D (hao) = "nihao" in Chinese.
    std::string input = "\xE4\xBD\xA0\xE5\xA5\xBD";
    EXPECT_EQ(norm.normalize(input), "\xE4\xBD\xA0 \xE5\xA5\xBD");
}

TEST(BertNormalizer, CJKMixedWithLatin) {
    BertNormalizer norm(/*lowercase=*/true,
                        /*strip_accents=*/false,
                        /*handle_chinese=*/true);
    // "hello" + U+4E16 (shi) + U+754C (jie) + "test".
    std::string input = "hello\xE4\xB8\x96\xE7\x95\x8C"
                        "test";
    EXPECT_EQ(norm.normalize(input), "hello \xE4\xB8\x96 \xE7\x95\x8C test");
}

TEST(BertNormalizer, CJKHandlingDisabled) {
    BertNormalizer norm(/*lowercase=*/true,
                        /*strip_accents=*/false,
                        /*handle_chinese=*/false);
    std::string input = "\xE4\xBD\xA0\xE5\xA5\xBD";
    // No spaces added.
    EXPECT_EQ(norm.normalize(input), "\xE4\xBD\xA0\xE5\xA5\xBD");
}

// ===================================================================
// BertNormalizer — Empty and Edge Cases
// ===================================================================

TEST(BertNormalizer, EmptyInput) {
    BertNormalizer norm;
    EXPECT_EQ(norm.normalize(""), "");
}

TEST(BertNormalizer, SingleCharacter) {
    BertNormalizer norm;
    EXPECT_EQ(norm.normalize("A"), "a");
}

TEST(BertNormalizer, OnlyControlChars) {
    BertNormalizer norm;
    std::string input("\x01\x02\x03", 3);
    EXPECT_EQ(norm.normalize(input), "");
}

TEST(BertNormalizer, UnicodePunctuation) {
    BertNormalizer norm;
    // Regular ASCII punctuation should pass through.
    EXPECT_EQ(norm.normalize("hello, world! test."), "hello, world! test.");
}

TEST(BertNormalizer, NumbersPreserved) {
    BertNormalizer norm;
    EXPECT_EQ(norm.normalize("Test 123"), "test 123");
}

// ===================================================================
// BertNormalizer — Combined Operations
// ===================================================================

TEST(BertNormalizer, LowercaseAndStripAccentsAndClean) {
    BertNormalizer norm(/*lowercase=*/true,
                        /*strip_accents=*/true,
                        /*handle_chinese=*/true,
                        /*clean_text=*/true);
    // Mixed: uppercase + accented + extra whitespace.
    EXPECT_EQ(norm.normalize("  CAF\xC3\x89   \xC3\x9C"
                             "BER  "),
              "cafe uber");
}

// ===================================================================
// LowercaseNormalizer
// ===================================================================

TEST(LowercaseNormalizer, LowercasesAscii) {
    LowercaseNormalizer norm;
    EXPECT_EQ(norm.normalize("Hello World"), "hello world");
}

TEST(LowercaseNormalizer, PreservesWhitespace) {
    LowercaseNormalizer norm;
    EXPECT_EQ(norm.normalize("Hello  World\n"), "hello  world\n");
}

TEST(LowercaseNormalizer, PreservesAccents) {
    LowercaseNormalizer norm;
    // U+00C9 (E with acute) lowercases to U+00E9 (e with acute).
    EXPECT_EQ(norm.normalize("\xC3\x89"), "\xC3\xA9");
}

TEST(LowercaseNormalizer, DoesNotStripControlChars) {
    LowercaseNormalizer norm;
    std::string input("A\x01"
                      "B",
                      3);
    std::string expected("a\x01"
                         "b",
                         3);
    EXPECT_EQ(norm.normalize(input), expected);
}

TEST(LowercaseNormalizer, EmptyInput) {
    LowercaseNormalizer norm;
    EXPECT_EQ(norm.normalize(""), "");
}

TEST(LowercaseNormalizer, LatinExtendedA) {
    LowercaseNormalizer norm;
    // U+0100 (A-macron) -> U+0101 (a-macron).
    EXPECT_EQ(norm.normalize("\xC4\x80"), "\xC4\x81");
}

// ===================================================================
// NullNormalizer
// ===================================================================

TEST(NullNormalizer, PassThrough) {
    NullNormalizer norm;
    EXPECT_EQ(norm.normalize("Hello World"), "Hello World");
}

TEST(NullNormalizer, PreservesEverything) {
    NullNormalizer norm;
    std::string input = "  HELLO\x01\t\xC3\xA9  ";
    EXPECT_EQ(norm.normalize(input), input);
}

TEST(NullNormalizer, EmptyInput) {
    NullNormalizer norm;
    EXPECT_EQ(norm.normalize(""), "");
}

// ===================================================================
// Factory: create_normalizer
// ===================================================================

TEST(CreateNormalizer, BertType) {
    TokenizerConfig config;
    config.normalizer = NormalizerType::BERT;
    config.normalizer_lowercase = true;
    config.normalizer_strip_accents = true;

    auto norm = create_normalizer(config);
    ASSERT_NE(norm, nullptr);
    // BertNormalizer should lowercase and strip accents.
    EXPECT_EQ(norm->normalize("CAF\xC3\x89"), "cafe");
}

TEST(CreateNormalizer, BertTypeNoLowercase) {
    TokenizerConfig config;
    config.normalizer = NormalizerType::BERT;
    config.normalizer_lowercase = false;
    config.normalizer_strip_accents = false;

    auto norm = create_normalizer(config);
    ASSERT_NE(norm, nullptr);
    // BertNormalizer with lowercase=false should preserve case.
    EXPECT_EQ(norm->normalize("Hello World"), "Hello World");
}

TEST(CreateNormalizer, LowercaseType) {
    TokenizerConfig config;
    config.normalizer = NormalizerType::LOWERCASE;

    auto norm = create_normalizer(config);
    ASSERT_NE(norm, nullptr);
    EXPECT_EQ(norm->normalize("Hello World"), "hello world");
    // Should NOT strip accents or collapse whitespace.
    EXPECT_EQ(norm->normalize("  caf\xC3\xA9  "), "  caf\xC3\xA9  ");
}

TEST(CreateNormalizer, NoneType) {
    TokenizerConfig config;
    config.normalizer = NormalizerType::NONE;

    auto norm = create_normalizer(config);
    ASSERT_NE(norm, nullptr);
    EXPECT_EQ(norm->normalize("Hello World"), "Hello World");
}

TEST(CreateNormalizer, NfcFallsBackToNull) {
    TokenizerConfig config;
    config.normalizer = NormalizerType::NFC;

    auto norm = create_normalizer(config);
    ASSERT_NE(norm, nullptr);
    // NFC not implemented — pass-through.
    EXPECT_EQ(norm->normalize("Hello World"), "Hello World");
}

// ===================================================================
// GDB-353: to_lower() Latin Extended-A range 0x0139-0x017E
// ===================================================================

TEST(LowercaseNormalizer, LatinExtendedA_OddUpperRange0x0139) {
    LowercaseNormalizer norm;
    // U+0139 (Ĺ, odd=uppercase) -> U+013A (ĺ).
    EXPECT_EQ(norm.normalize("\xC4\xB9"), "\xC4\xBA");
}

TEST(LowercaseNormalizer, LatinExtendedA_EvenLowerRange0x013A) {
    LowercaseNormalizer norm;
    // U+013A (ĺ, even=lowercase) stays unchanged.
    EXPECT_EQ(norm.normalize("\xC4\xBA"), "\xC4\xBA");
}

TEST(LowercaseNormalizer, LatinExtendedA_PolishL) {
    LowercaseNormalizer norm;
    // U+0141 (Ł, odd=uppercase) -> U+0142 (ł).
    EXPECT_EQ(norm.normalize("\xC5\x81"), "\xC5\x82");
    // U+0142 (ł, even=lowercase) stays unchanged.
    EXPECT_EQ(norm.normalize("\xC5\x82"), "\xC5\x82");
}

TEST(LowercaseNormalizer, LatinExtendedA_CzechZCaron) {
    LowercaseNormalizer norm;
    // U+017D (Ž, odd=uppercase) -> U+017E (ž).
    EXPECT_EQ(norm.normalize("\xC5\xBD"), "\xC5\xBE");
    // U+017E (ž, even=lowercase) stays unchanged.
    EXPECT_EQ(norm.normalize("\xC5\xBE"), "\xC5\xBE");
}

TEST(LowercaseNormalizer, LatinExtendedA_Kra) {
    LowercaseNormalizer norm;
    // U+0138 (ĸ, standalone lowercase) stays unchanged.
    EXPECT_EQ(norm.normalize("\xC4\xB8"), "\xC4\xB8");
}

TEST(LowercaseNormalizer, LatinExtendedA_YDiaeresis) {
    LowercaseNormalizer norm;
    // U+0178 (Ÿ) -> U+00FF (ÿ).
    EXPECT_EQ(norm.normalize("\xC5\xB8"), "\xC3\xBF");
}

TEST(LowercaseNormalizer, LatinExtendedA_ZAcute) {
    LowercaseNormalizer norm;
    // U+0179 (Ź, odd=uppercase) -> U+017A (ź).
    EXPECT_EQ(norm.normalize("\xC5\xB9"), "\xC5\xBA");
}

TEST(LowercaseNormalizer, LatinExtendedA_NWithCaron) {
    LowercaseNormalizer norm;
    // U+0147 (Ň, odd=uppercase) -> U+0148 (ň).
    EXPECT_EQ(norm.normalize("\xC5\x87"), "\xC5\x88");
}

TEST(BertNormalizer, LowercaseLatinExtendedA_Reversed) {
    BertNormalizer norm(/*lowercase=*/true);
    // U+017D (Ž) -> U+017E (ž) via BertNormalizer.
    EXPECT_EQ(norm.normalize("\xC5\xBD"), "\xC5\xBE");
    // U+0141 (Ł) -> U+0142 (ł).
    EXPECT_EQ(norm.normalize("\xC5\x81"), "\xC5\x82");
}

// ===================================================================
// GDB-354: BertNormalizer trimming when clean_text=false
// ===================================================================

TEST(BertNormalizer, CleanTextFalsePreservesLeadingTrailingSpaces) {
    BertNormalizer norm(/*lowercase=*/false,
                        /*strip_accents=*/false,
                        /*handle_chinese=*/false,
                        /*clean_text=*/false);
    EXPECT_EQ(norm.normalize("  hello  "), "  hello  ");
}

TEST(BertNormalizer, CleanTextFalsePreservesAllSpaces) {
    BertNormalizer norm(/*lowercase=*/false,
                        /*strip_accents=*/false,
                        /*handle_chinese=*/false,
                        /*clean_text=*/false);
    EXPECT_EQ(norm.normalize("  hello   world  "), "  hello   world  ");
}

TEST(BertNormalizer, AllFlagsFalseIsPassthrough) {
    BertNormalizer norm(/*lowercase=*/false,
                        /*strip_accents=*/false,
                        /*handle_chinese=*/false,
                        /*clean_text=*/false);
    std::string input = "  Hello  World  ";
    EXPECT_EQ(norm.normalize(input), input);
}

// ===================================================================
// GDB-355: decode_utf8 overlong sequence rejection
// ===================================================================

TEST(BertNormalizer, OverlongTwoByteSequence) {
    // Overlong 2-byte encoding of U+0020 (space): C0 A0.
    // Should be rejected as U+FFFD, not decoded as space.
    BertNormalizer norm(/*lowercase=*/false,
                        /*strip_accents=*/false,
                        /*handle_chinese=*/false,
                        /*clean_text=*/false);
    std::string input("\xC0\xA0", 2);
    std::string result = norm.normalize(input);
    // U+FFFD is EF BF BD in UTF-8. The second byte (A0) is a valid continuation
    // byte that gets treated as an invalid leading byte, producing a second U+FFFD.
    EXPECT_NE(result, " ");
    EXPECT_TRUE(result.find("\xEF\xBF\xBD") != std::string::npos);
}

TEST(BertNormalizer, OverlongThreeByteSlash) {
    // Overlong 3-byte encoding of '/' (U+002F): E0 80 AF.
    // Should be rejected, not decoded as '/'.
    BertNormalizer norm(/*lowercase=*/false,
                        /*strip_accents=*/false,
                        /*handle_chinese=*/false,
                        /*clean_text=*/false);
    std::string input("\xE0\x80\xAF", 3);
    std::string result = norm.normalize(input);
    EXPECT_NE(result, "/");
    EXPECT_TRUE(result.find("\xEF\xBF\xBD") != std::string::npos);
}

TEST(BertNormalizer, OverlongFourByteNull) {
    // Overlong 4-byte encoding of U+0000: F0 80 80 80.
    // Should be rejected as U+FFFD.
    BertNormalizer norm(/*lowercase=*/false,
                        /*strip_accents=*/false,
                        /*handle_chinese=*/false,
                        /*clean_text=*/false);
    std::string input("\xF0\x80\x80\x80", 4);
    std::string result = norm.normalize(input);
    EXPECT_TRUE(result.find("\xEF\xBF\xBD") != std::string::npos);
}

TEST(CreateNormalizer, Polymorphism) {
    TokenizerConfig config;
    config.normalizer = NormalizerType::BERT;
    config.normalizer_lowercase = true;
    config.normalizer_strip_accents = false;

    std::unique_ptr<TextNormalizer> norm = create_normalizer(config);
    ASSERT_NE(norm, nullptr);
    EXPECT_EQ(norm->normalize("HELLO"), "hello");
}
