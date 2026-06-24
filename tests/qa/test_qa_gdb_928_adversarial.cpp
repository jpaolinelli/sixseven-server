// GDB-928 adversarial QA tests: IcuNormalizer edge cases.
//
// All Unicode codepoints are embedded via \xNN escapes only (no literal
// multibyte characters) so the file is pure ASCII.
//
// Adversarial categories:
//   1. Malformed UTF-8 (lone continuation, truncated multibyte, overlong,
//      surrogate-in-UTF-8) -- no crash/UB; deterministic output.
//   2. Embedded NUL -- length-correct handling, not C-string truncation.
//   3. Large input of combining characters -- no quadratic blowup / crash.
//   4. Idempotence: normalize(normalize(x)) == normalize(x).
//   5. Hangul jamo composition under NFC.

#include "sixseven/vector/text_normalizer.h"
#include "sixseven/vector/tokenizer.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

namespace sixseven {
namespace {

// ===========================================================================
// 1. Malformed UTF-8 -- must not crash; pinned deterministic output.
// ===========================================================================

// Lone continuation byte \x80.  ICU replaces invalid sequences with U+FFFD
// (\xEF\xBF\xBD) or returns input unchanged -- either is acceptable, but the
// result must be non-empty and must not crash.
TEST(GDB928_Adversarial_Malformed, LoneContinuationByte_NFC) {
    IcuNormalizer norm(IcuNormalizer::Form::NFC);
    const std::string input = "\x80";
    std::string result;
    ASSERT_NO_FATAL_FAILURE(result = norm.normalize(input));
    // Must return *something* -- not an empty string and not a crash.
    // Pin the actual behaviour: ICU substitutes U+FFFD.
    // Acceptable outcomes: original byte OR U+FFFD replacement.
    const std::string fffd = "\xEF\xBF\xBD";
    EXPECT_TRUE(result == input || result == fffd)
        << "Unexpected output for lone continuation byte: len=" << result.size();
}

TEST(GDB928_Adversarial_Malformed, LoneContinuationByte_NFKC) {
    IcuNormalizer norm(IcuNormalizer::Form::NFKC);
    const std::string input = "\x80";
    std::string result;
    ASSERT_NO_FATAL_FAILURE(result = norm.normalize(input));
    const std::string fffd = "\xEF\xBF\xBD";
    EXPECT_TRUE(result == input || result == fffd)
        << "Unexpected output for lone continuation byte: len=" << result.size();
}

// Truncated two-byte sequence \xC3 with no continuation.
TEST(GDB928_Adversarial_Malformed, TruncatedTwoByte_NFC) {
    IcuNormalizer norm(IcuNormalizer::Form::NFC);
    const std::string input = "\xC3";
    std::string result;
    ASSERT_NO_FATAL_FAILURE(result = norm.normalize(input));
    const std::string fffd = "\xEF\xBF\xBD";
    EXPECT_TRUE(result == input || result == fffd)
        << "Unexpected output for truncated two-byte sequence: len=" << result.size();
}

TEST(GDB928_Adversarial_Malformed, TruncatedTwoByte_NFKC) {
    IcuNormalizer norm(IcuNormalizer::Form::NFKC);
    const std::string input = "\xC3";
    std::string result;
    ASSERT_NO_FATAL_FAILURE(result = norm.normalize(input));
    const std::string fffd = "\xEF\xBF\xBD";
    EXPECT_TRUE(result == input || result == fffd)
        << "Unexpected output for truncated two-byte sequence: len=" << result.size();
}

// Overlong encoding: \xC0\x80 is an overlong NUL (RFC 3629 forbids it).
TEST(GDB928_Adversarial_Malformed, OverlongNul_NFC) {
    IcuNormalizer norm(IcuNormalizer::Form::NFC);
    const std::string input = "\xC0\x80";
    std::string result;
    ASSERT_NO_FATAL_FAILURE(result = norm.normalize(input));
    // Must not crash; we don't mandate a specific replacement.
    EXPECT_FALSE(result.empty() && input.size() > 0)
        << "Overlong NUL: normalizer returned empty for non-empty input";
    // Pin: should not produce the raw NUL byte 0x00 inside the output
    // (overlong NUL should never decode to an embedded 0x00).
    for (char c : result) {
        EXPECT_NE(static_cast<unsigned char>(c), 0u)
            << "Overlong NUL was decoded into a real NUL byte -- security risk";
    }
}

// UTF-16 surrogate U+D800 encoded as UTF-8: \xED\xA0\x80.
// This is invalid UTF-8 (RFC 3629 Section 3).  Must not crash.
TEST(GDB928_Adversarial_Malformed, Utf16SurrogateInUtf8_NFC) {
    IcuNormalizer norm(IcuNormalizer::Form::NFC);
    const std::string input = "\xED\xA0\x80";
    std::string result;
    ASSERT_NO_FATAL_FAILURE(result = norm.normalize(input));
    // Must return a string (crash = fail, hang = timeout = fail).
    (void)result;
}

TEST(GDB928_Adversarial_Malformed, Utf16SurrogateInUtf8_NFKC) {
    IcuNormalizer norm(IcuNormalizer::Form::NFKC);
    const std::string input = "\xED\xA0\x80";
    std::string result;
    ASSERT_NO_FATAL_FAILURE(result = norm.normalize(input));
    (void)result;
}

// Mixed: valid UTF-8, then lone continuation, then valid ASCII.
TEST(GDB928_Adversarial_Malformed, MixedValidAndInvalid_NFC) {
    IcuNormalizer norm(IcuNormalizer::Form::NFC);
    // "a" + lone \x80 + "b"
    const std::string input = "a\x80"
                              "b";
    std::string result;
    ASSERT_NO_FATAL_FAILURE(result = norm.normalize(input));
    // Result must contain "a" and "b" somewhere.
    EXPECT_TRUE(result.find('a') != std::string::npos);
    EXPECT_TRUE(result.find('b') != std::string::npos);
}

// ===========================================================================
// 2. Embedded NUL -- must not truncate at the NUL.
// ===========================================================================

TEST(GDB928_Adversarial_EmbeddedNul, LengthCorrect_NFC) {
    IcuNormalizer norm(IcuNormalizer::Form::NFC);
    // "a\0b" -- 3 bytes.
    const std::string input("a\0b", 3);
    ASSERT_EQ(input.size(), 3u);
    std::string result;
    ASSERT_NO_FATAL_FAILURE(result = norm.normalize(input));
    // ICU fromUTF8 uses StringPiece which is length-aware.
    // The NUL should be preserved (U+0000 is a valid codepoint in NFC).
    EXPECT_EQ(result.size(), 3u) << "Embedded NUL caused length truncation: got " << result.size()
                                 << " bytes";
    EXPECT_EQ(result[0], 'a');
    EXPECT_EQ(result[1], '\0');
    EXPECT_EQ(result[2], 'b');
}

TEST(GDB928_Adversarial_EmbeddedNul, LengthCorrect_NFKC) {
    IcuNormalizer norm(IcuNormalizer::Form::NFKC);
    const std::string input("a\0b", 3);
    ASSERT_EQ(input.size(), 3u);
    std::string result;
    ASSERT_NO_FATAL_FAILURE(result = norm.normalize(input));
    EXPECT_EQ(result.size(), 3u) << "Embedded NUL caused length truncation: got " << result.size()
                                 << " bytes";
    EXPECT_EQ(result[0], 'a');
    EXPECT_EQ(result[1], '\0');
    EXPECT_EQ(result[2], 'b');
}

// ===========================================================================
// 3. Large input -- combining character stress, no crash / no O(n^2) hang.
// ===========================================================================

// 50 000 repetitions of "e\xCC\x81" (e + combining acute).
// Each pair composes to "\xC3\xA9" under NFC -- 100 000 input bytes -> 50 000 output bytes.
TEST(GDB928_Adversarial_Large, RepeatedCombiningPairs_NFC) {
    IcuNormalizer norm(IcuNormalizer::Form::NFC);
    const std::string unit = "e\xCC\x81"; // 3 bytes
    std::string input;
    input.reserve(unit.size() * 50000);
    for (int i = 0; i < 50000; ++i) {
        input += unit;
    }
    std::string result;
    ASSERT_NO_FATAL_FAILURE(result = norm.normalize(input));
    // Each pair -> 2-byte composed form.
    EXPECT_EQ(result.size(), 50000u * 2u) << "Large NFC: unexpected output size " << result.size();
    // Spot-check: every 2-byte group is the composed e-acute "\xC3\xA9".
    for (size_t i = 0; i < result.size(); i += 2) {
        ASSERT_EQ(static_cast<unsigned char>(result[i]), 0xC3u);
        ASSERT_EQ(static_cast<unsigned char>(result[i + 1]), 0xA9u);
    }
}

// 50 000 repetitions of fi-ligature (U+FB01) -- NFKC maps each to "fi" (2 bytes).
TEST(GDB928_Adversarial_Large, RepeatedFiLigature_NFKC) {
    IcuNormalizer norm(IcuNormalizer::Form::NFKC);
    const std::string unit = "\xEF\xAC\x81"; // 3 bytes
    std::string input;
    input.reserve(unit.size() * 50000);
    for (int i = 0; i < 50000; ++i) {
        input += unit;
    }
    std::string result;
    ASSERT_NO_FATAL_FAILURE(result = norm.normalize(input));
    // Each fi-ligature -> "fi" (2 ASCII bytes).
    EXPECT_EQ(result.size(), 50000u * 2u) << "Large NFKC: unexpected output size " << result.size();
    for (size_t i = 0; i < result.size(); i += 2) {
        EXPECT_EQ(result[i], 'f');
        EXPECT_EQ(result[i + 1], 'i');
    }
}

// ===========================================================================
// 4. Idempotence: normalize(normalize(x)) == normalize(x)
// ===========================================================================

TEST(GDB928_Adversarial_Idempotent, CombiningMarks_NFC) {
    IcuNormalizer norm(IcuNormalizer::Form::NFC);
    const std::string inputs[] = {
        "e\xCC\x81",    // e + combining acute
        "\xC3\xA9",     // pre-composed e-acute
        "A\xCC\x8A",    // A + combining ring
        "caf\xC3\xA9",  // "cafe" with pre-composed accent
        "\xEF\xAC\x81", // fi-ligature (NFC preserves)
        "hello world",
        "",
    };
    for (const auto& input : inputs) {
        const std::string once = norm.normalize(input);
        const std::string twice = norm.normalize(once);
        EXPECT_EQ(once, twice) << "NFC not idempotent for input of length " << input.size();
    }
}

TEST(GDB928_Adversarial_Idempotent, CompatChars_NFKC) {
    IcuNormalizer norm(IcuNormalizer::Form::NFKC);
    const std::string inputs[] = {
        "\xEF\xAC\x81", // fi-ligature
        "\xEF\xBC\x91", // fullwidth "1"
        "e\xCC\x81",    // e + combining acute
        "\xC3\xA9",     // pre-composed e-acute
        "hello world",
        "",
    };
    for (const auto& input : inputs) {
        const std::string once = norm.normalize(input);
        const std::string twice = norm.normalize(once);
        EXPECT_EQ(once, twice) << "NFKC not idempotent for input of length " << input.size();
    }
}

// ===========================================================================
// 5. Hangul jamo composition under NFC.
//    U+1100 (choseong G) + U+1161 (jungseong A) -> U+AC00 (ga syllable)
//    UTF-8: \xE1\x84\x80 \xE1\x85\xA1  ->  \xEA\xB0\x80
// ===========================================================================

TEST(GDB928_Adversarial_Hangul, JamoComposesToSyllable_NFC) {
    IcuNormalizer norm(IcuNormalizer::Form::NFC);
    // Choseong G (U+1100) + Jungseong A (U+1161)
    const std::string jamo = "\xE1\x84\x80\xE1\x85\xA1";
    // Expected composed syllable: U+AC00 "\xEA\xB0\x80"
    const std::string syllable = "\xEA\xB0\x80";
    EXPECT_EQ(norm.normalize(jamo), syllable) << "Hangul jamo not composed to syllable under NFC";
}

TEST(GDB928_Adversarial_Hangul, JamoComposesToSyllable_NFKC) {
    IcuNormalizer norm(IcuNormalizer::Form::NFKC);
    const std::string jamo = "\xE1\x84\x80\xE1\x85\xA1";
    const std::string syllable = "\xEA\xB0\x80";
    EXPECT_EQ(norm.normalize(jamo), syllable) << "Hangul jamo not composed to syllable under NFKC";
}

// Precomposed syllable is idempotent under NFC.
TEST(GDB928_Adversarial_Hangul, PrecomposedSyllableIdempotent_NFC) {
    IcuNormalizer norm(IcuNormalizer::Form::NFC);
    const std::string syllable = "\xEA\xB0\x80";
    EXPECT_EQ(norm.normalize(syllable), syllable);
}

// ===========================================================================
// 6. Regression: NONE / LOWERCASE / BERT paths unaffected.
// ===========================================================================

TEST(GDB928_Adversarial_Regression, NonePathUnchanged) {
    TokenizerConfig cfg;
    cfg.normalizer = NormalizerType::NONE;
    auto norm = create_normalizer(cfg);
    ASSERT_NE(norm, nullptr);
    const std::string fi = "\xEF\xAC\x81";
    EXPECT_EQ(norm->normalize(fi), fi);
    EXPECT_EQ(norm->normalize("hello"), "hello");
}

TEST(GDB928_Adversarial_Regression, LowercasePathUnchanged) {
    TokenizerConfig cfg;
    cfg.normalizer = NormalizerType::LOWERCASE;
    auto norm = create_normalizer(cfg);
    ASSERT_NE(norm, nullptr);
    EXPECT_EQ(norm->normalize("Hello World"), "hello world");
    // fi-ligature preserved (no compat decomp).
    const std::string fi = "\xEF\xAC\x81";
    EXPECT_EQ(norm->normalize(fi), fi);
}

TEST(GDB928_Adversarial_Regression, BertPathUnchanged) {
    TokenizerConfig cfg;
    cfg.normalizer = NormalizerType::BERT;
    cfg.normalizer_lowercase = true;
    cfg.normalizer_strip_accents = false;
    auto norm = create_normalizer(cfg);
    ASSERT_NE(norm, nullptr);
    EXPECT_EQ(norm->normalize("Hello World"), "hello world");
}

} // namespace
} // namespace sixseven
