/// @file tests/qa/test_qa_gdb_353.cpp
/// QA adversarial tests for GDB-353: to_lower() wrong for Latin Extended-A
/// range 0x0139-0x017E.
///
/// Bug: The even/odd heuristic for lowercasing was only correct for 0x0100-0x0137.
/// For 0x0139-0x0148 and 0x0179-0x017E, the pattern reverses (odd = uppercase).
/// Also U+0138 (ĸ) and U+0178 (Ÿ) had incorrect handling.
///
/// Fix: Split into five sub-ranges with correct even/odd parity, plus special
/// cases for U+0138 and U+0178.

#include "sixseven/vector/text_normalizer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace sixseven {
namespace {

/// Encode a codepoint as UTF-8.
std::string utf8(uint32_t cp) {
    std::string result;
    if (cp < 0x80) {
        result.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return result;
}

// ===================================================================
// Range 1: 0x0100-0x0137 (even = uppercase, odd = lowercase)
// Spot-checks that the original correct range still works.
// ===================================================================

TEST(QA_GDB353_Range1, AMacronUpperToLower) {
    LowercaseNormalizer norm;
    // U+0100 (Ā) → U+0101 (ā)
    EXPECT_EQ(norm.normalize(utf8(0x0100)), utf8(0x0101));
}

TEST(QA_GDB353_Range1, AMacronLowerUnchanged) {
    LowercaseNormalizer norm;
    // U+0101 (ā) → stays ā
    EXPECT_EQ(norm.normalize(utf8(0x0101)), utf8(0x0101));
}

TEST(QA_GDB353_Range1, EndOfRange0x0136UpperToLower) {
    LowercaseNormalizer norm;
    // U+0136 (Ķ) → U+0137 (ķ)
    EXPECT_EQ(norm.normalize(utf8(0x0136)), utf8(0x0137));
}

TEST(QA_GDB353_Range1, EndOfRange0x0137LowerUnchanged) {
    LowercaseNormalizer norm;
    // U+0137 (ķ) → stays ķ
    EXPECT_EQ(norm.normalize(utf8(0x0137)), utf8(0x0137));
}

// ===================================================================
// U+0138: ĸ (Latin Small Letter Kra) — standalone lowercase, no uppercase
// ===================================================================

TEST(QA_GDB353_Kra, KraStaysUnchanged) {
    LowercaseNormalizer norm;
    // U+0138 (ĸ) must NOT be changed. Previously: ĸ → Ĺ (bug).
    EXPECT_EQ(norm.normalize(utf8(0x0138)), utf8(0x0138));
}

TEST(QA_GDB353_Kra, KraThroughBertNormalizer) {
    BertNormalizer norm(true, false, false, false);
    EXPECT_EQ(norm.normalize(utf8(0x0138)), utf8(0x0138));
}

// ===================================================================
// Range 2: 0x0139-0x0148 (odd = uppercase, even = lowercase)
// This was the primary broken range.
// ===================================================================

TEST(QA_GDB353_Range2, LacuteUpperToLower) {
    LowercaseNormalizer norm;
    // U+0139 (Ĺ) → U+013A (ĺ)
    EXPECT_EQ(norm.normalize(utf8(0x0139)), utf8(0x013A));
}

TEST(QA_GDB353_Range2, LacuteLowerUnchanged) {
    LowercaseNormalizer norm;
    // U+013A (ĺ) → stays ĺ
    EXPECT_EQ(norm.normalize(utf8(0x013A)), utf8(0x013A));
}

TEST(QA_GDB353_Range2, LcedillaUpperToLower) {
    LowercaseNormalizer norm;
    // U+013B (Ļ) → U+013C (ļ)
    EXPECT_EQ(norm.normalize(utf8(0x013B)), utf8(0x013C));
}

TEST(QA_GDB353_Range2, LcaronUpperToLower) {
    LowercaseNormalizer norm;
    // U+013D (Ľ) → U+013E (ľ)
    EXPECT_EQ(norm.normalize(utf8(0x013D)), utf8(0x013E));
}

TEST(QA_GDB353_Range2, LstrokeUpperToLower) {
    LowercaseNormalizer norm;
    // U+0141 (Ł) → U+0142 (ł)
    EXPECT_EQ(norm.normalize(utf8(0x0141)), utf8(0x0142));
}

TEST(QA_GDB353_Range2, LstrokeLowerUnchanged) {
    LowercaseNormalizer norm;
    // U+0142 (ł) → stays ł. Previously: ł → Ń (bug).
    EXPECT_EQ(norm.normalize(utf8(0x0142)), utf8(0x0142));
}

TEST(QA_GDB353_Range2, NacuteUpperToLower) {
    LowercaseNormalizer norm;
    // U+0143 (Ń) → U+0144 (ń)
    EXPECT_EQ(norm.normalize(utf8(0x0143)), utf8(0x0144));
}

TEST(QA_GDB353_Range2, NacuteLowerUnchanged) {
    LowercaseNormalizer norm;
    // U+0144 (ń) → stays ń
    EXPECT_EQ(norm.normalize(utf8(0x0144)), utf8(0x0144));
}

TEST(QA_GDB353_Range2, NcedillaUpperToLower) {
    LowercaseNormalizer norm;
    // U+0145 (Ņ) → U+0146 (ņ)
    EXPECT_EQ(norm.normalize(utf8(0x0145)), utf8(0x0146));
}

TEST(QA_GDB353_Range2, NcaronUpperToLower) {
    LowercaseNormalizer norm;
    // U+0147 (Ň) → U+0148 (ň)
    EXPECT_EQ(norm.normalize(utf8(0x0147)), utf8(0x0148));
}

TEST(QA_GDB353_Range2, NcaronLowerUnchanged) {
    LowercaseNormalizer norm;
    // U+0148 (ň) → stays ň
    EXPECT_EQ(norm.normalize(utf8(0x0148)), utf8(0x0148));
}

// ===================================================================
// U+0149: ŉ (deprecated, standalone) — should pass through unchanged
// ===================================================================

TEST(QA_GDB353_Gap, NapostropheUnchanged) {
    LowercaseNormalizer norm;
    // U+0149 (ŉ) — not in any lowercasing range. Should pass through.
    EXPECT_EQ(norm.normalize(utf8(0x0149)), utf8(0x0149));
}

// ===================================================================
// Range 3: 0x014A-0x0177 (even = uppercase, odd = lowercase)
// ===================================================================

TEST(QA_GDB353_Range3, EngUpperToLower) {
    LowercaseNormalizer norm;
    // U+014A (Ŋ) → U+014B (ŋ)
    EXPECT_EQ(norm.normalize(utf8(0x014A)), utf8(0x014B));
}

TEST(QA_GDB353_Range3, EngLowerUnchanged) {
    LowercaseNormalizer norm;
    // U+014B (ŋ) → stays ŋ
    EXPECT_EQ(norm.normalize(utf8(0x014B)), utf8(0x014B));
}

TEST(QA_GDB353_Range3, ScaronUpperToLower) {
    LowercaseNormalizer norm;
    // U+0160 (Š) → U+0161 (š)
    EXPECT_EQ(norm.normalize(utf8(0x0160)), utf8(0x0161));
}

TEST(QA_GDB353_Range3, EndOfRange0x0176UpperToLower) {
    LowercaseNormalizer norm;
    // U+0176 (Ŷ) → U+0177 (ŷ)
    EXPECT_EQ(norm.normalize(utf8(0x0176)), utf8(0x0177));
}

TEST(QA_GDB353_Range3, EndOfRange0x0177LowerUnchanged) {
    LowercaseNormalizer norm;
    // U+0177 (ŷ) → stays ŷ
    EXPECT_EQ(norm.normalize(utf8(0x0177)), utf8(0x0177));
}

// ===================================================================
// U+0178: Ÿ — special case, lowercase is U+00FF (ÿ)
// ===================================================================

TEST(QA_GDB353_Ydiaeresis, YdiaeresisUpperToLower) {
    LowercaseNormalizer norm;
    // U+0178 (Ÿ) → U+00FF (ÿ)
    EXPECT_EQ(norm.normalize(utf8(0x0178)), utf8(0x00FF));
}

TEST(QA_GDB353_Ydiaeresis, YdiaeresisLowerUnchanged) {
    LowercaseNormalizer norm;
    // U+00FF (ÿ) → stays ÿ
    EXPECT_EQ(norm.normalize(utf8(0x00FF)), utf8(0x00FF));
}

TEST(QA_GDB353_Ydiaeresis, YdiaeresisThroughBertNormalizer) {
    BertNormalizer norm(true, false, false, false);
    EXPECT_EQ(norm.normalize(utf8(0x0178)), utf8(0x00FF));
}

// ===================================================================
// Range 4: 0x0179-0x017E (odd = uppercase, even = lowercase)
// ===================================================================

TEST(QA_GDB353_Range4, ZacuteUpperToLower) {
    LowercaseNormalizer norm;
    // U+0179 (Ź) → U+017A (ź)
    EXPECT_EQ(norm.normalize(utf8(0x0179)), utf8(0x017A));
}

TEST(QA_GDB353_Range4, ZacuteLowerUnchanged) {
    LowercaseNormalizer norm;
    // U+017A (ź) → stays ź
    EXPECT_EQ(norm.normalize(utf8(0x017A)), utf8(0x017A));
}

TEST(QA_GDB353_Range4, ZdotUpperToLower) {
    LowercaseNormalizer norm;
    // U+017B (Ż) → U+017C (ż)
    EXPECT_EQ(norm.normalize(utf8(0x017B)), utf8(0x017C));
}

TEST(QA_GDB353_Range4, ZdotLowerUnchanged) {
    LowercaseNormalizer norm;
    // U+017C (ż) → stays ż
    EXPECT_EQ(norm.normalize(utf8(0x017C)), utf8(0x017C));
}

TEST(QA_GDB353_Range4, ZcaronUpperToLower) {
    LowercaseNormalizer norm;
    // U+017D (Ž) → U+017E (ž) — the primary example from the bug report.
    EXPECT_EQ(norm.normalize(utf8(0x017D)), utf8(0x017E));
}

TEST(QA_GDB353_Range4, ZcaronLowerUnchanged) {
    LowercaseNormalizer norm;
    // U+017E (ž) → stays ž
    EXPECT_EQ(norm.normalize(utf8(0x017E)), utf8(0x017E));
}

// ===================================================================
// Integration: full text with affected characters
// ===================================================================

TEST(QA_GDB353_Integration, PolishTextLowercased) {
    LowercaseNormalizer norm;
    // "ŁÓDŹ" = Ł(0x0141) Ó(0x00D3) D(0x44) Ź(0x0179)
    std::string input = utf8(0x0141) + utf8(0x00D3) + "D" + utf8(0x0179);
    // Expected: ł(0x0142) ó(0x00F3) d(0x64) ź(0x017A)
    std::string expected = utf8(0x0142) + utf8(0x00F3) + "d" + utf8(0x017A);
    EXPECT_EQ(norm.normalize(input), expected);
}

TEST(QA_GDB353_Integration, CzechTextLowercased) {
    BertNormalizer norm(true, false, false, false);
    // "ŽŇĚŘ" = Ž(017D) Ň(0147) Ě(011A) Ř(0158)
    std::string input = utf8(0x017D) + utf8(0x0147) + utf8(0x011A) + utf8(0x0158);
    std::string expected = utf8(0x017E) + utf8(0x0148) + utf8(0x011B) + utf8(0x0159);
    EXPECT_EQ(norm.normalize(input), expected);
}

TEST(QA_GDB353_Integration, SlovakTextLowercased) {
    LowercaseNormalizer norm;
    // "ĹĽŇ" = Ĺ(0139) Ľ(013D) Ň(0147)
    std::string input = utf8(0x0139) + utf8(0x013D) + utf8(0x0147);
    std::string expected = utf8(0x013A) + utf8(0x013E) + utf8(0x0148);
    EXPECT_EQ(norm.normalize(input), expected);
}

TEST(QA_GDB353_Integration, MixedLatinAndExtended) {
    BertNormalizer norm(true, false, false, true);
    // "Hello Ž World" → "hello ž world"
    std::string input = "Hello " + utf8(0x017D) + " World";
    std::string expected = "hello " + utf8(0x017E) + " world";
    EXPECT_EQ(norm.normalize(input), expected);
}

// ===================================================================
// Exhaustive: every uppercase in the fixed ranges lowers correctly
// ===================================================================

TEST(QA_GDB353_Exhaustive, Range0139to0148AllUppercaseLowered) {
    LowercaseNormalizer norm;
    // In 0x0139-0x0148, odd codepoints are uppercase.
    for (uint32_t cp = 0x0139; cp <= 0x0148; cp += 2) {
        EXPECT_EQ(norm.normalize(utf8(cp)), utf8(cp + 1))
            << "Failed for U+" << std::hex << cp;
    }
}

TEST(QA_GDB353_Exhaustive, Range0139to0148AllLowercaseUnchanged) {
    LowercaseNormalizer norm;
    // In 0x0139-0x0148, even codepoints are lowercase.
    for (uint32_t cp = 0x013A; cp <= 0x0148; cp += 2) {
        EXPECT_EQ(norm.normalize(utf8(cp)), utf8(cp))
            << "Failed for U+" << std::hex << cp;
    }
}

TEST(QA_GDB353_Exhaustive, Range0179to017EAllUppercaseLowered) {
    LowercaseNormalizer norm;
    // In 0x0179-0x017E, odd codepoints are uppercase.
    for (uint32_t cp = 0x0179; cp <= 0x017E; cp += 2) {
        EXPECT_EQ(norm.normalize(utf8(cp)), utf8(cp + 1))
            << "Failed for U+" << std::hex << cp;
    }
}

TEST(QA_GDB353_Exhaustive, Range0179to017EAllLowercaseUnchanged) {
    LowercaseNormalizer norm;
    // In 0x0179-0x017E, even codepoints are lowercase.
    for (uint32_t cp = 0x017A; cp <= 0x017E; cp += 2) {
        EXPECT_EQ(norm.normalize(utf8(cp)), utf8(cp))
            << "Failed for U+" << std::hex << cp;
    }
}

// ===================================================================
// Boundary: characters just outside the Latin Extended-A range
// ===================================================================

TEST(QA_GDB353_Boundary, JustAbove017EUnchanged) {
    LowercaseNormalizer norm;
    // U+017F (ſ — Latin Small Letter Long S) — not in any lowercasing range.
    EXPECT_EQ(norm.normalize(utf8(0x017F)), utf8(0x017F));
}

TEST(QA_GDB353_Boundary, MultiplicationSignNotLowered) {
    LowercaseNormalizer norm;
    // U+00D7 × — excluded from Latin-1 Supplement lowercasing.
    EXPECT_EQ(norm.normalize(utf8(0x00D7)), utf8(0x00D7));
}

} // namespace
} // namespace sixseven
