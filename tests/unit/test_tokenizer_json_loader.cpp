#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/vector/tokenizer.h"
#include "sixseven/vector/tokenizer_json_loader.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace sixseven {
namespace {

/// Return absolute path to the test fixtures directory.
std::filesystem::path fixtures_dir() {
    return std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures";
}

/// Return absolute path to the MiniLM tokenizer fixture.
std::string minilm_fixture() {
    return (fixtures_dir() / "tokenizer_minilm.json").string();
}

/// Write a temporary JSON file and return its path.
class TempJsonFile {
public:
    explicit TempJsonFile(const std::string& content)
        : path_(std::filesystem::temp_directory_path() / "sixseven_test_tokenizer.json") {
        std::ofstream out(path_);
        out << content;
    }

    ~TempJsonFile() { std::filesystem::remove(path_); }

    [[nodiscard]] std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

// ===================================================================
// Tests with real all-MiniLM-L6-v2 tokenizer.json
// ===================================================================

TEST(TokenizerJsonLoader, LoadMiniLM_ModelType) {
    auto result = load_tokenizer_config(minilm_fixture());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_type, TokenizerModelType::WORDPIECE);
}

TEST(TokenizerJsonLoader, LoadMiniLM_VocabSize) {
    auto result = load_tokenizer_config(minilm_fixture());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->vocab.size(), 30522u);
}

TEST(TokenizerJsonLoader, LoadMiniLM_VocabContainsKnownTokens) {
    auto result = load_tokenizer_config(minilm_fixture());
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Verify well-known tokens from BERT vocabulary.
    EXPECT_EQ(result->vocab.at("[PAD]"), 0);
    EXPECT_EQ(result->vocab.at("[UNK]"), 100);
    EXPECT_EQ(result->vocab.at("[CLS]"), 101);
    EXPECT_EQ(result->vocab.at("[SEP]"), 102);
    EXPECT_EQ(result->vocab.at("[MASK]"), 103);
    EXPECT_EQ(result->vocab.at("the"), 1996);
}

TEST(TokenizerJsonLoader, LoadMiniLM_SpecialTokenIds) {
    auto result = load_tokenizer_config(minilm_fixture());
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(result->special_tokens.pad, 0);
    EXPECT_EQ(result->special_tokens.unk, 100);
    EXPECT_EQ(result->special_tokens.cls, 101);
    EXPECT_EQ(result->special_tokens.sep, 102);
    EXPECT_EQ(result->special_tokens.mask, 103);
}

TEST(TokenizerJsonLoader, LoadMiniLM_SubwordPrefix) {
    auto result = load_tokenizer_config(minilm_fixture());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->subword_prefix, "##");
}

TEST(TokenizerJsonLoader, LoadMiniLM_Normalizer) {
    auto result = load_tokenizer_config(minilm_fixture());
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(result->normalizer, NormalizerType::BERT);
    EXPECT_TRUE(result->normalizer_lowercase);
    // strip_accents is null in MiniLM → defaults to false.
    EXPECT_FALSE(result->normalizer_strip_accents);
}

TEST(TokenizerJsonLoader, LoadMiniLM_PreTokenizer) {
    auto result = load_tokenizer_config(minilm_fixture());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->pre_tokenizer, PreTokenizerType::PUNCTUATION);
}

TEST(TokenizerJsonLoader, LoadMiniLM_NoMerges) {
    auto result = load_tokenizer_config(minilm_fixture());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // WordPiece models do not use merges.
    EXPECT_TRUE(result->merges.empty());
}

// ===================================================================
// Error handling: file I/O
// ===================================================================

TEST(TokenizerJsonLoader, NonexistentFileReturnsIOError) {
    auto result = load_tokenizer_config("/tmp/does_not_exist_12345.json");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

// ===================================================================
// Error handling: malformed JSON
// ===================================================================

TEST(TokenizerJsonLoader, MalformedJsonReturnsParseError) {
    TempJsonFile file("{this is not valid json}");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(TokenizerJsonLoader, EmptyFileReturnsParseError) {
    TempJsonFile file("");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(TokenizerJsonLoader, JsonArrayRootReturnsParseError) {
    TempJsonFile file("[]");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// ===================================================================
// Error handling: missing required fields
// ===================================================================

TEST(TokenizerJsonLoader, MissingModelObjectReturnsParseError) {
    TempJsonFile file(R"({"version": "1.0"})");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(TokenizerJsonLoader, MissingModelTypeReturnsParseError) {
    TempJsonFile file(R"({"model": {"vocab": {"a": 0}}})");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(TokenizerJsonLoader, MissingVocabReturnsParseError) {
    TempJsonFile file(R"({"model": {"type": "WordPiece"}})");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// ===================================================================
// Error handling: wrong types
// ===================================================================

TEST(TokenizerJsonLoader, ModelTypeNotStringReturnsParseError) {
    TempJsonFile file(R"({"model": {"type": 123, "vocab": {}}})");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(TokenizerJsonLoader, VocabNotObjectReturnsParseError) {
    TempJsonFile file(R"({"model": {"type": "WordPiece", "vocab": []}})");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(TokenizerJsonLoader, VocabEntryNonIntegerReturnsParseError) {
    TempJsonFile file(R"({"model": {"type": "WordPiece", "vocab": {"hello": "bad"}}})");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// ===================================================================
// Error handling: unsupported model type
// ===================================================================

TEST(TokenizerJsonLoader, UnsupportedModelTypeReturnsError) {
    TempJsonFile file(R"({"model": {"type": "FutureModel", "vocab": {"a": 0}}})");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ===================================================================
// Supported model types
// ===================================================================

TEST(TokenizerJsonLoader, BPEModelTypeAccepted) {
    TempJsonFile file(R"({"model": {"type": "BPE", "vocab": {"a": 0}}})");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_type, TokenizerModelType::BPE);
}

TEST(TokenizerJsonLoader, UnigramModelTypeAccepted) {
    TempJsonFile file(R"({"model": {"type": "Unigram", "vocab": {"a": 0}}})");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_type, TokenizerModelType::UNIGRAM);
}

// ===================================================================
// BPE merges parsing
// ===================================================================

TEST(TokenizerJsonLoader, BPEMergesParsed) {
    TempJsonFile file(R"({
        "model": {
            "type": "BPE",
            "vocab": {"a": 0, "b": 1, "ab": 2},
            "merges": ["a b", "ab c"]
        }
    })");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->merges.size(), 2u);
    EXPECT_EQ(result->merges[0], "a b");
    EXPECT_EQ(result->merges[1], "ab c");
}

TEST(TokenizerJsonLoader, NonStringMergeEntryReturnsParseError) {
    TempJsonFile file(R"({
        "model": {
            "type": "BPE",
            "vocab": {"a": 0},
            "merges": [42]
        }
    })");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// ===================================================================
// Added tokens / special token extraction
// ===================================================================

TEST(TokenizerJsonLoader, AddedTokensPopulateSpecialIds) {
    TempJsonFile file(R"({
        "model": {"type": "WordPiece", "vocab": {"[PAD]": 0}},
        "added_tokens": [
            {"id": 5, "content": "[PAD]", "special": true},
            {"id": 10, "content": "[UNK]", "special": true},
            {"id": 20, "content": "[CLS]", "special": true},
            {"id": 30, "content": "[SEP]", "special": true},
            {"id": 40, "content": "[MASK]", "special": true}
        ]
    })");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->special_tokens.pad, 5);
    EXPECT_EQ(result->special_tokens.unk, 10);
    EXPECT_EQ(result->special_tokens.cls, 20);
    EXPECT_EQ(result->special_tokens.sep, 30);
    EXPECT_EQ(result->special_tokens.mask, 40);
}

TEST(TokenizerJsonLoader, UnknownAddedTokensIgnored) {
    TempJsonFile file(R"({
        "model": {"type": "WordPiece", "vocab": {"a": 0}},
        "added_tokens": [
            {"id": 999, "content": "[CUSTOM]", "special": true}
        ]
    })");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Default special token IDs remain.
    EXPECT_EQ(result->special_tokens.pad, 0);
    EXPECT_EQ(result->special_tokens.unk, 100);
}

// ===================================================================
// Normalizer settings
// ===================================================================

TEST(TokenizerJsonLoader, NormalizerLowercaseFalse) {
    TempJsonFile file(R"({
        "model": {"type": "WordPiece", "vocab": {"a": 0}},
        "normalizer": {"type": "BertNormalizer", "lowercase": false}
    })");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::BERT);
    EXPECT_FALSE(result->normalizer_lowercase);
}

TEST(TokenizerJsonLoader, NormalizerStripAccentsTrue) {
    TempJsonFile file(R"({
        "model": {"type": "WordPiece", "vocab": {"a": 0}},
        "normalizer": {
            "type": "BertNormalizer",
            "lowercase": true,
            "strip_accents": true
        }
    })");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->normalizer_strip_accents);
}

TEST(TokenizerJsonLoader, NoNormalizerDefaultsToLowercase) {
    TempJsonFile file(R"({"model": {"type": "WordPiece", "vocab": {"a": 0}}})");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Default TokenizerConfig values.
    EXPECT_EQ(result->normalizer, NormalizerType::LOWERCASE);
    EXPECT_TRUE(result->normalizer_lowercase);
}

// ===================================================================
// Pre-tokenizer
// ===================================================================

TEST(TokenizerJsonLoader, WhitespacePreTokenizer) {
    TempJsonFile file(R"({
        "model": {"type": "WordPiece", "vocab": {"a": 0}},
        "pre_tokenizer": {"type": "Whitespace"}
    })");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->pre_tokenizer, PreTokenizerType::WHITESPACE);
}

TEST(TokenizerJsonLoader, NoPreTokenizerDefaultsToPunctuation) {
    TempJsonFile file(R"({"model": {"type": "WordPiece", "vocab": {"a": 0}}})");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Default TokenizerConfig value.
    EXPECT_EQ(result->pre_tokenizer, PreTokenizerType::PUNCTUATION);
}

// ===================================================================
// Subword prefix
// ===================================================================

TEST(TokenizerJsonLoader, SubwordPrefixExtracted) {
    TempJsonFile file(R"({
        "model": {
            "type": "WordPiece",
            "vocab": {"a": 0},
            "continuing_subword_prefix": "@@"
        }
    })");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->subword_prefix, "@@");
}

TEST(TokenizerJsonLoader, MissingSubwordPrefixDefaultsToEmpty) {
    TempJsonFile file(R"({"model": {"type": "WordPiece", "vocab": {"a": 0}}})");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->subword_prefix.empty());
}

// ===================================================================
// GDB-352: Normalizer enum when no type field
// ===================================================================

TEST(TokenizerJsonLoader, NormalizerNoTypeFieldLowercaseTrue) {
    TempJsonFile file(R"({
        "model": {"type": "WordPiece", "vocab": {"a": 0}},
        "normalizer": {"lowercase": true}
    })");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::LOWERCASE);
    EXPECT_TRUE(result->normalizer_lowercase);
}

TEST(TokenizerJsonLoader, NormalizerNoTypeFieldLowercaseFalse) {
    TempJsonFile file(R"({
        "model": {"type": "WordPiece", "vocab": {"a": 0}},
        "normalizer": {"lowercase": false}
    })");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::NONE);
    EXPECT_FALSE(result->normalizer_lowercase);
}

TEST(TokenizerJsonLoader, NormalizerNoTypeFieldDefaultsToLowercase) {
    // No "type" key, no "lowercase" key — defaults to lowercase=true → LOWERCASE.
    TempJsonFile file(R"({
        "model": {"type": "WordPiece", "vocab": {"a": 0}},
        "normalizer": {}
    })");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::LOWERCASE);
    EXPECT_TRUE(result->normalizer_lowercase);
}

// ===================================================================
// GDB-776: RoBERTa/GPT-style angle-bracket special tokens
// ===================================================================

TEST(TokenizerJsonLoader, BPEAngleBracketAddedTokensPopulateSpecialIds) {
    TempJsonFile file(R"({
        "model": {"type": "BPE", "vocab": {"<s>": 0, "<pad>": 1, "</s>": 2, "<unk>": 3, "<mask>": 4}},
        "added_tokens": [
            {"id": 0, "content": "<s>",    "special": true},
            {"id": 1, "content": "<pad>",  "special": true},
            {"id": 2, "content": "</s>",   "special": true},
            {"id": 3, "content": "<unk>",  "special": true},
            {"id": 4, "content": "<mask>", "special": true}
        ]
    })");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // <s>    -> cls
    EXPECT_EQ(result->special_tokens.cls, 0);
    // <pad>  -> pad
    EXPECT_EQ(result->special_tokens.pad, 1);
    // </s>   -> sep
    EXPECT_EQ(result->special_tokens.sep, 2);
    // <unk>  -> unk
    EXPECT_EQ(result->special_tokens.unk, 3);
    // <mask> -> mask
    EXPECT_EQ(result->special_tokens.mask, 4);
}

TEST(TokenizerJsonLoader, BPEAngleBracketTokensNotDefaultBERTValues) {
    // Regression: a vocab with only angle-bracket names must NOT keep BERT
    // defaults (pad=0, unk=100, cls=101, sep=102).
    TempJsonFile file(R"({
        "model": {"type": "BPE", "vocab": {"<s>": 0, "<pad>": 1, "</s>": 2, "<unk>": 3}},
        "added_tokens": [
            {"id": 0, "content": "<s>",   "special": true},
            {"id": 1, "content": "<pad>", "special": true},
            {"id": 2, "content": "</s>",  "special": true},
            {"id": 3, "content": "<unk>", "special": true}
        ]
    })");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_NE(result->special_tokens.cls, 101);
    EXPECT_NE(result->special_tokens.sep, 102);
    EXPECT_NE(result->special_tokens.pad, 0); // pad=1 now, not the BERT default 0
    EXPECT_EQ(result->special_tokens.pad, 1);
}

TEST(TokenizerJsonLoader, BPEAngleBracketVocabFallbackNoAddedTokens) {
    // Fallback: no added_tokens section, but vocab contains RoBERTa names.
    TempJsonFile file(R"({
        "model": {
            "type": "BPE",
            "vocab": {"<s>": 10, "<pad>": 11, "</s>": 12, "<unk>": 13, "<mask>": 14, "hello": 20}
        }
    })");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->special_tokens.cls, 10);
    EXPECT_EQ(result->special_tokens.pad, 11);
    EXPECT_EQ(result->special_tokens.sep, 12);
    EXPECT_EQ(result->special_tokens.unk, 13);
    EXPECT_EQ(result->special_tokens.mask, 14);
}

TEST(TokenizerJsonLoader, BERTVocabFallbackNoAddedTokens) {
    // Fallback: no added_tokens section, vocab contains BERT bracket names.
    TempJsonFile file(R"({
        "model": {
            "type": "WordPiece",
            "vocab": {"[PAD]": 0, "[UNK]": 100, "[CLS]": 101, "[SEP]": 102, "[MASK]": 103, "hello": 200}
        }
    })");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->special_tokens.pad, 0);
    EXPECT_EQ(result->special_tokens.unk, 100);
    EXPECT_EQ(result->special_tokens.cls, 101);
    EXPECT_EQ(result->special_tokens.sep, 102);
    EXPECT_EQ(result->special_tokens.mask, 103);
}

// ===================================================================
// GDB-776: merges pair-array format
// ===================================================================

TEST(TokenizerJsonLoader, MergesPairArrayFormatAccepted) {
    TempJsonFile file(R"({
        "model": {
            "type": "BPE",
            "vocab": {"a": 0, "b": 1, "ab": 2, "c": 3},
            "merges": [["a", "b"], ["ab", "c"]]
        }
    })");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->merges.size(), 2u);
    // Pair arrays are normalised to "a b" string form internally.
    EXPECT_EQ(result->merges[0], "a b");
    EXPECT_EQ(result->merges[1], "ab c");
}

TEST(TokenizerJsonLoader, MergesMixedStringAndPairArrayAccepted) {
    TempJsonFile file(R"({
        "model": {
            "type": "BPE",
            "vocab": {"a": 0, "b": 1, "ab": 2, "c": 3},
            "merges": ["a b", ["ab", "c"]]
        }
    })");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->merges.size(), 2u);
    EXPECT_EQ(result->merges[0], "a b");
    EXPECT_EQ(result->merges[1], "ab c");
}

TEST(TokenizerJsonLoader, MergesInvalidEntryStillErrors) {
    // An integer entry is still invalid even after the pair-array fix.
    TempJsonFile file(R"({
        "model": {
            "type": "BPE",
            "vocab": {"a": 0},
            "merges": [42]
        }
    })");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(TokenizerJsonLoader, MergesSingleElementArrayErrors) {
    TempJsonFile file(R"({
        "model": {
            "type": "BPE",
            "vocab": {"a": 0},
            "merges": [["a"]]
        }
    })");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

} // namespace
} // namespace sixseven
