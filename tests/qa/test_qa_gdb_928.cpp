// GDB-928: Real ICU-backed NFC/NFKC normalization.
//
// All Unicode codepoints are embedded via \xNN escapes only (no literal
// multibyte characters) so the file is pure ASCII.

#include "sixseven/vector/text_normalizer.h"
#include "sixseven/vector/tokenizer.h"
#include "sixseven/vector/tokenizer_json_loader.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// Helper: write a temp tokenizer JSON and return its path.
// ---------------------------------------------------------------------------
class TempJsonFile {
public:
    explicit TempJsonFile(const std::string& json, const std::string& stem) {
        path_ = (std::filesystem::temp_directory_path() / ("gdb928_" + stem + ".json")).string();
        std::ofstream f(path_);
        f << json;
    }
    ~TempJsonFile() { std::filesystem::remove(path_); }
    [[nodiscard]] const std::string& path() const { return path_; }

private:
    std::string path_;
};

// ===========================================================================
// IcuNormalizer: NFC canonical composition
// ===========================================================================

// e + combining acute (U+0301) -> precomposed e-acute (U+00E9).
// Decomposed: "e\xCC\x81"   Composed: "\xC3\xA9"
TEST(GDB928_NFC, ComposesEWithCombiningAcute) {
    IcuNormalizer norm(IcuNormalizer::Form::NFC);
    EXPECT_EQ(norm.normalize("e\xCC\x81"), "\xC3\xA9");
}

// A + combining ring above (U+030A) -> A-ring (U+00C5).
// Decomposed: "A\xCC\x8A"   Composed: "\xC3\x85"
TEST(GDB928_NFC, ComposesAWithCombiningRing) {
    IcuNormalizer norm(IcuNormalizer::Form::NFC);
    EXPECT_EQ(norm.normalize("A\xCC\x8A"), "\xC3\x85");
}

// Already-composed form is idempotent.
TEST(GDB928_NFC, IdempotentOnPrecomposed) {
    IcuNormalizer norm(IcuNormalizer::Form::NFC);
    EXPECT_EQ(norm.normalize("\xC3\xA9"), "\xC3\xA9");
}

// ASCII input passes through unchanged.
TEST(GDB928_NFC, AsciiPassThrough) {
    IcuNormalizer norm(IcuNormalizer::Form::NFC);
    EXPECT_EQ(norm.normalize("hello world"), "hello world");
}

// Empty string -> empty string.
TEST(GDB928_NFC, EmptyString) {
    IcuNormalizer norm(IcuNormalizer::Form::NFC);
    EXPECT_EQ(norm.normalize(""), "");
}

// NFC does NOT decompose the fi-ligature (U+FB01 = "\xEF\xAC\x81").
// fi-ligature is a compatibility character; NFC preserves it.
TEST(GDB928_NFC, FiLigaturePreserved) {
    IcuNormalizer norm(IcuNormalizer::Form::NFC);
    EXPECT_EQ(norm.normalize("\xEF\xAC\x81"), "\xEF\xAC\x81");
}

// ===========================================================================
// IcuNormalizer: NFKC compatibility composition
// ===========================================================================

// fi-ligature (U+FB01 "\xEF\xAC\x81") -> "fi" under NFKC.
TEST(GDB928_NFKC, FiLigatureDecomposed) {
    IcuNormalizer norm(IcuNormalizer::Form::NFKC);
    EXPECT_EQ(norm.normalize("\xEF\xAC\x81"), "fi");
}

// Fullwidth digit 1 (U+FF11 "\xEF\xBC\x91") -> ASCII "1" under NFKC.
TEST(GDB928_NFKC, FullwidthDigitNormalized) {
    IcuNormalizer norm(IcuNormalizer::Form::NFKC);
    EXPECT_EQ(norm.normalize("\xEF\xBC\x91"), "1");
}

// NFKC also composes: e + combining acute -> U+00E9.
TEST(GDB928_NFKC, ComposesEWithCombiningAcute) {
    IcuNormalizer norm(IcuNormalizer::Form::NFKC);
    EXPECT_EQ(norm.normalize("e\xCC\x81"), "\xC3\xA9");
}

// ASCII input passes through unchanged.
TEST(GDB928_NFKC, AsciiPassThrough) {
    IcuNormalizer norm(IcuNormalizer::Form::NFKC);
    EXPECT_EQ(norm.normalize("hello world"), "hello world");
}

// Empty string -> empty string.
TEST(GDB928_NFKC, EmptyString) {
    IcuNormalizer norm(IcuNormalizer::Form::NFKC);
    EXPECT_EQ(norm.normalize(""), "");
}

// ===========================================================================
// NFC vs NFKC distinction on compatibility characters
// ===========================================================================

// fi-ligature under NFC stays fi-ligature; under NFKC becomes "fi".
// This proves the two normalizers are distinct.
TEST(GDB928_Distinction, FiLigatureDistinguishesNFCFromNFKC) {
    IcuNormalizer nfc(IcuNormalizer::Form::NFC);
    IcuNormalizer nfkc(IcuNormalizer::Form::NFKC);
    const std::string fi_ligature = "\xEF\xAC\x81";
    EXPECT_EQ(nfc.normalize(fi_ligature), fi_ligature); // NFC preserves
    EXPECT_EQ(nfkc.normalize(fi_ligature), "fi");       // NFKC decomposes
}

// ===========================================================================
// Factory: create_normalizer routes NFC -> IcuNormalizer(NFC)
//          and NFKC -> IcuNormalizer(NFKC)
// ===========================================================================

TEST(GDB928_Factory, NfcEnum_ComposesViaCuNormalizer) {
    TokenizerConfig config;
    config.normalizer = NormalizerType::NFC;
    auto norm = create_normalizer(config);
    ASSERT_NE(norm, nullptr);
    // Must compose: not a pass-through.
    EXPECT_EQ(norm->normalize("e\xCC\x81"), "\xC3\xA9");
}

TEST(GDB928_Factory, NfkcEnum_CompatDecomposesViaCuNormalizer) {
    TokenizerConfig config;
    config.normalizer = NormalizerType::NFKC;
    auto norm = create_normalizer(config);
    ASSERT_NE(norm, nullptr);
    EXPECT_EQ(norm->normalize("\xEF\xAC\x81"), "fi");
}

TEST(GDB928_Factory, NfcDoesNotDecomposeFiLigature) {
    TokenizerConfig config;
    config.normalizer = NormalizerType::NFC;
    auto norm = create_normalizer(config);
    ASSERT_NE(norm, nullptr);
    EXPECT_EQ(norm->normalize("\xEF\xAC\x81"), "\xEF\xAC\x81");
}

// ===========================================================================
// Loader: parse_normalizer_type maps "NFC" -> NFC, "NFKC" -> NFKC (distinct)
// ===========================================================================

TEST(GDB928_Loader, NfcStringMapsToNfcEnum) {
    TempJsonFile file(
        R"({"model":{"type":"WordPiece","vocab":{"a":0}},"normalizer":{"type":"NFC"}})", "nfc");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::NFC);
}

TEST(GDB928_Loader, NfkcStringMapsToNfkcEnum) {
    TempJsonFile file(
        R"({"model":{"type":"WordPiece","vocab":{"a":0}},"normalizer":{"type":"NFKC"}})", "nfkc");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::NFKC);
}

// NFC loader path produces a composing normalizer.
TEST(GDB928_Loader, NfcLoaderProducesComposingNormalizer) {
    TempJsonFile file(
        R"({"model":{"type":"WordPiece","vocab":{"a":0}},"normalizer":{"type":"NFC"}})",
        "nfc_comp");
    auto cfg = load_tokenizer_config(file.path());
    ASSERT_TRUE(cfg.has_value()) << cfg.error().message;
    auto norm = create_normalizer(*cfg);
    ASSERT_NE(norm, nullptr);
    EXPECT_EQ(norm->normalize("e\xCC\x81"), "\xC3\xA9");
}

// NFKC loader path produces a compatibility normalizer distinct from NFC.
TEST(GDB928_Loader, NfkcLoaderProducesCompatNormalizer) {
    TempJsonFile file(
        R"({"model":{"type":"WordPiece","vocab":{"a":0}},"normalizer":{"type":"NFKC"}})",
        "nfkc_comp");
    auto cfg = load_tokenizer_config(file.path());
    ASSERT_TRUE(cfg.has_value()) << cfg.error().message;
    auto norm = create_normalizer(*cfg);
    ASSERT_NE(norm, nullptr);
    EXPECT_EQ(norm->normalize("\xEF\xAC\x81"), "fi");
}

} // namespace
} // namespace sixseven
