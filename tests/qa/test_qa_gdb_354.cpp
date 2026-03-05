/// @file tests/qa/test_qa_gdb_354.cpp
/// QA adversarial tests for GDB-354: BertNormalizer trims leading/trailing
/// spaces even when clean_text=false.
///
/// Bug: `BertNormalizer::normalize()` unconditionally trimmed leading and
/// trailing spaces, regardless of the `clean_text_` flag.
///
/// Fix: Trimming is now guarded by `if (clean_text_)`.

#include "sixseven/vector/text_normalizer.h"

#include <gtest/gtest.h>

#include <string>

namespace sixseven {
namespace {

// ===================================================================
// Core fix: clean_text=false → no trimming
// ===================================================================

TEST(QA_GDB354, CleanTextFalseNoTrimming) {
    BertNormalizer norm(false, false, false, false);
    EXPECT_EQ(norm.normalize("  hello  "), "  hello  ");
}

TEST(QA_GDB354, CleanTextFalseLeadingSpacesPreserved) {
    BertNormalizer norm(false, false, false, false);
    EXPECT_EQ(norm.normalize("   hello"), "   hello");
}

TEST(QA_GDB354, CleanTextFalseTrailingSpacesPreserved) {
    BertNormalizer norm(false, false, false, false);
    EXPECT_EQ(norm.normalize("hello   "), "hello   ");
}

TEST(QA_GDB354, CleanTextFalseOnlySpaces) {
    BertNormalizer norm(false, false, false, false);
    EXPECT_EQ(norm.normalize("     "), "     ");
}

TEST(QA_GDB354, CleanTextFalseSingleSpace) {
    BertNormalizer norm(false, false, false, false);
    EXPECT_EQ(norm.normalize(" "), " ");
}

TEST(QA_GDB354, CleanTextFalseTabsPreserved) {
    BertNormalizer norm(false, false, false, false);
    EXPECT_EQ(norm.normalize("\thello\t"), "\thello\t");
}

TEST(QA_GDB354, CleanTextFalseNewlinesPreserved) {
    BertNormalizer norm(false, false, false, false);
    EXPECT_EQ(norm.normalize("\nhello\n"), "\nhello\n");
}

TEST(QA_GDB354, CleanTextFalseMixedWhitespace) {
    BertNormalizer norm(false, false, false, false);
    // All whitespace types preserved when clean_text=false.
    EXPECT_EQ(norm.normalize(" \t\n\r hello \t\n\r "), " \t\n\r hello \t\n\r ");
}

// ===================================================================
// Verify: clean_text=true still trims correctly
// ===================================================================

TEST(QA_GDB354, CleanTextTrueTrimsLeadingAndTrailing) {
    BertNormalizer norm(false, false, false, true);
    EXPECT_EQ(norm.normalize("  hello  "), "hello");
}

TEST(QA_GDB354, CleanTextTrueTrimsAllWhitespace) {
    BertNormalizer norm(false, false, false, true);
    EXPECT_EQ(norm.normalize("   "), "");
}

TEST(QA_GDB354, CleanTextTrueCollapsesInternalSpaces) {
    BertNormalizer norm(false, false, false, true);
    EXPECT_EQ(norm.normalize("  a     b  "), "a b");
}

// ===================================================================
// Combinations: clean_text=false with other flags enabled
// ===================================================================

TEST(QA_GDB354, LowercaseEnabledCleanTextFalse) {
    BertNormalizer norm(true, false, false, false);
    // Lowercase should work, but spaces should not be trimmed.
    EXPECT_EQ(norm.normalize("  HELLO  "), "  hello  ");
}

TEST(QA_GDB354, StripAccentsEnabledCleanTextFalse) {
    BertNormalizer norm(false, true, false, false);
    // Accents stripped, but spaces preserved.
    EXPECT_EQ(norm.normalize("  caf\xC3\xA9  "), "  cafe  ");
}

TEST(QA_GDB354, CJKEnabledCleanTextFalse) {
    BertNormalizer norm(false, false, true, false);
    // CJK spacing should be added, but no trimming.
    // U+4E2D (中)
    std::string input = "  \xE4\xB8\xAD  ";
    std::string result = norm.normalize(input);
    // CJK adds spaces around the character, but leading/trailing preserved.
    EXPECT_EQ(result.front(), ' ');
    EXPECT_EQ(result.back(), ' ');
}

TEST(QA_GDB354, AllFlagsExceptCleanTextTrue) {
    BertNormalizer norm(true, true, true, false);
    // Lowercase, strip accents, CJK handling all enabled, but no trimming.
    EXPECT_EQ(norm.normalize("  CAF\xC3\x89  "), "  cafe  ");
}

// ===================================================================
// Edge: empty string and single character
// ===================================================================

TEST(QA_GDB354, CleanTextFalseEmptyString) {
    BertNormalizer norm(false, false, false, false);
    EXPECT_EQ(norm.normalize(""), "");
}

TEST(QA_GDB354, CleanTextFalseSingleChar) {
    BertNormalizer norm(false, false, false, false);
    EXPECT_EQ(norm.normalize("x"), "x");
}

// ===================================================================
// Stress: many leading/trailing spaces
// ===================================================================

TEST(QA_GDB354, CleanTextFalse100LeadingSpaces) {
    BertNormalizer norm(false, false, false, false);
    std::string input = std::string(100, ' ') + "hello";
    EXPECT_EQ(norm.normalize(input), input);
}

TEST(QA_GDB354, CleanTextFalse100TrailingSpaces) {
    BertNormalizer norm(false, false, false, false);
    std::string input = "hello" + std::string(100, ' ');
    EXPECT_EQ(norm.normalize(input), input);
}

TEST(QA_GDB354, CleanTextFalse100BothSides) {
    BertNormalizer norm(false, false, false, false);
    std::string input = std::string(100, ' ') + "hello" + std::string(100, ' ');
    EXPECT_EQ(norm.normalize(input), input);
}

} // namespace
} // namespace sixseven
