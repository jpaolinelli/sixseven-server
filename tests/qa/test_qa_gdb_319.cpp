#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/vector/tokenizer.h"
#include "sixseven/vector/tokenizer_json_loader.h"

#include <gtest/gtest.h>

#include <climits>
#include <cstdint>
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
/// Uses a unique name per instance to avoid test interference.
class TempJsonFile {
public:
    explicit TempJsonFile(const std::string& content, const std::string& suffix = "")
        : path_(std::filesystem::temp_directory_path() / ("sixseven_qa_319_" + suffix + ".json")) {
        std::ofstream out(path_);
        out << content;
    }

    ~TempJsonFile() { std::filesystem::remove(path_); }

    [[nodiscard]] std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

// ===================================================================
// AC1: load_tokenizer_config(path) → Result<TokenizerConfig>
// ===================================================================

TEST(QA_GDB319_LoadFunction, ReturnsResultOnValidInput) {
    auto result = load_tokenizer_config(minilm_fixture());
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

TEST(QA_GDB319_LoadFunction, ReturnsErrorOnInvalidPath) {
    auto result = load_tokenizer_config("/nonexistent/path/foo.json");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

// ===================================================================
// AC2: Extracts model.type (WordPiece, BPE, Unigram)
// ===================================================================

TEST(QA_GDB319_ModelType, WordPieceFromFixture) {
    auto result = load_tokenizer_config(minilm_fixture());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_type, TokenizerModelType::WORDPIECE);
}

TEST(QA_GDB319_ModelType, BPEAccepted) {
    TempJsonFile file(R"({"model":{"type":"BPE","vocab":{"a":0}}})", "bpe");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_type, TokenizerModelType::BPE);
}

TEST(QA_GDB319_ModelType, UnigramAccepted) {
    TempJsonFile file(R"({"model":{"type":"Unigram","vocab":{"a":0}}})", "unigram");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_type, TokenizerModelType::UNIGRAM);
}

TEST(QA_GDB319_ModelType, CaseSensitiveRejection) {
    // "wordpiece" is not "WordPiece" - should reject
    TempJsonFile file(R"({"model":{"type":"wordpiece","vocab":{"a":0}}})", "case");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB319_ModelType, EmptyStringTypeRejected) {
    TempJsonFile file(R"({"model":{"type":"","vocab":{"a":0}}})", "empty_type");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ===================================================================
// AC3: Extracts model.vocab as unordered_map<string, int64_t>
// ===================================================================

TEST(QA_GDB319_Vocab, FixtureVocabSize) {
    auto result = load_tokenizer_config(minilm_fixture());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->vocab.size(), 30522u);
}

TEST(QA_GDB319_Vocab, EmptyVocabObjectAccepted) {
    // Empty vocab is technically valid JSON — no entries
    TempJsonFile file(R"({"model":{"type":"WordPiece","vocab":{}}})", "empty_vocab");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->vocab.empty());
}

TEST(QA_GDB319_Vocab, SingleEntryVocab) {
    TempJsonFile file(R"({"model":{"type":"WordPiece","vocab":{"hello":42}}})", "single_vocab");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->vocab.size(), 1u);
    EXPECT_EQ(result->vocab.at("hello"), 42);
}

TEST(QA_GDB319_Vocab, NegativeVocabId) {
    TempJsonFile file(R"({"model":{"type":"WordPiece","vocab":{"tok":-1}}})", "neg_vocab");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->vocab.at("tok"), -1);
}

TEST(QA_GDB319_Vocab, LargeVocabId) {
    // Test with large int64 value
    TempJsonFile file(R"({"model":{"type":"WordPiece","vocab":{"tok":9223372036854775806}}})",
                      "large_id");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->vocab.at("tok"), 9223372036854775806LL);
}

TEST(QA_GDB319_Vocab, ZeroVocabId) {
    TempJsonFile file(R"({"model":{"type":"WordPiece","vocab":{"tok":0}}})", "zero_id");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->vocab.at("tok"), 0);
}

TEST(QA_GDB319_Vocab, FloatVocabIdRejected) {
    TempJsonFile file(R"({"model":{"type":"WordPiece","vocab":{"tok":1.5}}})", "float_id");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB319_Vocab, EmptyStringKey) {
    // Empty string is a valid JSON key
    TempJsonFile file(R"({"model":{"type":"WordPiece","vocab":{"":0}}})", "empty_key");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->vocab.at(""), 0);
}

TEST(QA_GDB319_Vocab, UnicodeTokenKeys) {
    TempJsonFile file(R"({"model":{"type":"WordPiece","vocab":{"café":0,"日本語":1,"🚀":2}}})",
                      "unicode_keys");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->vocab.size(), 3u);
    EXPECT_EQ(result->vocab.at("café"), 0);
    EXPECT_EQ(result->vocab.at("日本語"), 1);
    EXPECT_EQ(result->vocab.at("🚀"), 2);
}

TEST(QA_GDB319_Vocab, DuplicateKeysLastWins) {
    // JSON with duplicate keys — nlohmann::json keeps last value
    TempJsonFile file(R"({"model":{"type":"WordPiece","vocab":{"tok":1,"tok":2}}})", "dup_keys");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->vocab.at("tok"), 2);
}

// ===================================================================
// AC4: Extracts model.merges (for BPE) as ordered vector
// ===================================================================

TEST(QA_GDB319_Merges, BPEMergesPreserveOrder) {
    TempJsonFile file(R"({
        "model":{
            "type":"BPE",
            "vocab":{"a":0,"b":1,"c":2,"ab":3},
            "merges":["a b","b c","a b c"]
        }
    })",
                      "merges_order");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->merges.size(), 3u);
    EXPECT_EQ(result->merges[0], "a b");
    EXPECT_EQ(result->merges[1], "b c");
    EXPECT_EQ(result->merges[2], "a b c");
}

TEST(QA_GDB319_Merges, EmptyMergesArray) {
    TempJsonFile file(R"({
        "model":{"type":"BPE","vocab":{"a":0},"merges":[]}
    })",
                      "empty_merges");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->merges.empty());
}

TEST(QA_GDB319_Merges, WordPieceNoMerges) {
    auto result = load_tokenizer_config(minilm_fixture());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->merges.empty());
}

TEST(QA_GDB319_Merges, MergesIgnoredWhenNotArray) {
    // merges present but as a string — should be ignored (not error)
    TempJsonFile file(R"({
        "model":{"type":"BPE","vocab":{"a":0},"merges":"not an array"}
    })",
                      "merges_str");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->merges.empty());
}

TEST(QA_GDB319_Merges, NonStringMergeEntryFails) {
    TempJsonFile file(R"({
        "model":{"type":"BPE","vocab":{"a":0},"merges":[123]}
    })",
                      "merges_int");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB319_Merges, MixedMergeEntriesFails) {
    // First entry valid, second invalid
    TempJsonFile file(R"({
        "model":{"type":"BPE","vocab":{"a":0},"merges":["a b", 42]}
    })",
                      "merges_mixed");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// ===================================================================
// AC5: Extracts added_tokens with IDs and special flag
// ===================================================================

TEST(QA_GDB319_AddedTokens, AllFiveSpecialTokens) {
    auto result = load_tokenizer_config(minilm_fixture());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->special_tokens.pad, 0);
    EXPECT_EQ(result->special_tokens.unk, 100);
    EXPECT_EQ(result->special_tokens.cls, 101);
    EXPECT_EQ(result->special_tokens.sep, 102);
    EXPECT_EQ(result->special_tokens.mask, 103);
}

TEST(QA_GDB319_AddedTokens, NoAddedTokensSectionKeepsDefaults) {
    TempJsonFile file(R"({"model":{"type":"WordPiece","vocab":{"a":0}}})", "no_added");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->special_tokens.pad, 0);
    EXPECT_EQ(result->special_tokens.unk, 100);
    EXPECT_EQ(result->special_tokens.cls, 101);
    EXPECT_EQ(result->special_tokens.sep, 102);
    EXPECT_EQ(result->special_tokens.mask, 103);
}

TEST(QA_GDB319_AddedTokens, EmptyAddedTokensArray) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "added_tokens":[]
    })",
                      "empty_added");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Defaults preserved
    EXPECT_EQ(result->special_tokens.pad, 0);
    EXPECT_EQ(result->special_tokens.unk, 100);
}

TEST(QA_GDB319_AddedTokens, MalformedEntrySkipped) {
    // Entry missing "content" field — should be skipped
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "added_tokens":[
            {"id":5},
            {"id":10,"content":"[UNK]","special":true}
        ]
    })",
                      "malformed_entry");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->special_tokens.unk, 10);
}

TEST(QA_GDB319_AddedTokens, NonObjectEntrySkipped) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "added_tokens":["not_an_object", 42]
    })",
                      "non_obj_entry");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

TEST(QA_GDB319_AddedTokens, NegativeSpecialTokenId) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "added_tokens":[
            {"id":-1,"content":"[PAD]","special":true}
        ]
    })",
                      "neg_special");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->special_tokens.pad, -1);
}

TEST(QA_GDB319_AddedTokens, DuplicateSpecialTokenLastWins) {
    // Two entries for [PAD] — second should overwrite
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "added_tokens":[
            {"id":5,"content":"[PAD]","special":true},
            {"id":99,"content":"[PAD]","special":true}
        ]
    })",
                      "dup_special");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->special_tokens.pad, 99);
}

TEST(QA_GDB319_AddedTokens, UnknownTokensIgnored) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "added_tokens":[
            {"id":999,"content":"[CUSTOM_TOKEN]","special":true}
        ]
    })",
                      "unknown_token");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Defaults unchanged
    EXPECT_EQ(result->special_tokens.pad, 0);
    EXPECT_EQ(result->special_tokens.unk, 100);
}

TEST(QA_GDB319_AddedTokens, SpecialFlagNotRequired) {
    // Entry without "special" field but with valid id/content
    // The code doesn't check the "special" flag — matches by content name
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "added_tokens":[
            {"id":42,"content":"[CLS]"}
        ]
    })",
                      "no_special_flag");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->special_tokens.cls, 42);
}

TEST(QA_GDB319_AddedTokens, StringIdSkipped) {
    // id is a string, not integer — should be skipped
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "added_tokens":[
            {"id":"not_int","content":"[PAD]","special":true}
        ]
    })",
                      "str_id");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Default preserved since entry was skipped
    EXPECT_EQ(result->special_tokens.pad, 0);
}

// ===================================================================
// AC6: Extracts normalizer.type and settings
// ===================================================================

TEST(QA_GDB319_Normalizer, BertNormalizerWithLowercase) {
    auto result = load_tokenizer_config(minilm_fixture());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::BERT);
    EXPECT_TRUE(result->normalizer_lowercase);
}

TEST(QA_GDB319_Normalizer, BertNormalizerWithoutLowercase) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"type":"BertNormalizer","lowercase":false}
    })",
                      "no_lower");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::BERT);
    EXPECT_FALSE(result->normalizer_lowercase);
}

TEST(QA_GDB319_Normalizer, NFCNormalizerType) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"type":"NFC"}
    })",
                      "nfc");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::NFC);
}

TEST(QA_GDB319_Normalizer, NFKCNormalizerType) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"type":"NFKC"}
    })",
                      "nfkc");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::NFC);
}

TEST(QA_GDB319_Normalizer, UnknownNormalizerTypeDefaultsNone) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"type":"SomeFutureThing","lowercase":false}
    })",
                      "unknown_norm");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::NONE);
}

TEST(QA_GDB319_Normalizer, NullNormalizerSectionKeepsDefaults) {
    // "normalizer": null — is_object() returns false, so section skipped
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":null
    })",
                      "null_norm");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::LOWERCASE);
    EXPECT_TRUE(result->normalizer_lowercase);
}

TEST(QA_GDB319_Normalizer, StripAccentsTrue) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"type":"BertNormalizer","lowercase":true,"strip_accents":true}
    })",
                      "strip_true");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->normalizer_strip_accents);
}

TEST(QA_GDB319_Normalizer, StripAccentsNullDefaultsFalse) {
    // strip_accents: null — is_boolean() returns false, stays default
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"type":"BertNormalizer","lowercase":true,"strip_accents":null}
    })",
                      "strip_null");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->normalizer_strip_accents);
}

TEST(QA_GDB319_Normalizer, NormalizerObjectNoTypeField) {
    // normalizer section exists but has no "type" — enum inferred from lowercase flag.
    // Fixed by GDB-352: previously stayed at default LOWERCASE even when lowercase=false.
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"lowercase":false}
    })",
                      "norm_no_type");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->normalizer_lowercase);
    // Now correctly inferred as NONE when lowercase is false.
    EXPECT_EQ(result->normalizer, NormalizerType::NONE);
}

// ===================================================================
// AC7: Extracts pre_tokenizer.type
// ===================================================================

TEST(QA_GDB319_PreTokenizer, BertPreTokenizerFromFixture) {
    auto result = load_tokenizer_config(minilm_fixture());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->pre_tokenizer, PreTokenizerType::PUNCTUATION);
}

TEST(QA_GDB319_PreTokenizer, WhitespaceType) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "pre_tokenizer":{"type":"Whitespace"}
    })",
                      "ws_pretok");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->pre_tokenizer, PreTokenizerType::WHITESPACE);
}

TEST(QA_GDB319_PreTokenizer, WhitespaceSplitType) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "pre_tokenizer":{"type":"WhitespaceSplit"}
    })",
                      "ws_split");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->pre_tokenizer, PreTokenizerType::WHITESPACE);
}

TEST(QA_GDB319_PreTokenizer, PunctuationType) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "pre_tokenizer":{"type":"Punctuation"}
    })",
                      "punct_pretok");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->pre_tokenizer, PreTokenizerType::PUNCTUATION);
}

TEST(QA_GDB319_PreTokenizer, UnknownTypeDefaultsNone) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "pre_tokenizer":{"type":"Metaspace"}
    })",
                      "unknown_pretok");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->pre_tokenizer, PreTokenizerType::NONE);
}

TEST(QA_GDB319_PreTokenizer, NullPreTokenizerKeepsDefault) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "pre_tokenizer":null
    })",
                      "null_pretok");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->pre_tokenizer, PreTokenizerType::PUNCTUATION);
}

TEST(QA_GDB319_PreTokenizer, PreTokenizerNoTypeField) {
    // Object exists but has no "type" key
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "pre_tokenizer":{"other_field":"value"}
    })",
                      "pretok_no_type");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Should keep default since no type was parsed
    EXPECT_EQ(result->pre_tokenizer, PreTokenizerType::PUNCTUATION);
}

// ===================================================================
// AC8: Extracts model.continuing_subword_prefix
// ===================================================================

TEST(QA_GDB319_SubwordPrefix, HashHashFromFixture) {
    auto result = load_tokenizer_config(minilm_fixture());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->subword_prefix, "##");
}

TEST(QA_GDB319_SubwordPrefix, CustomPrefix) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0},"continuing_subword_prefix":"@@"}
    })",
                      "custom_prefix");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->subword_prefix, "@@");
}

TEST(QA_GDB319_SubwordPrefix, EmptyPrefix) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0},"continuing_subword_prefix":""}
    })",
                      "empty_prefix");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->subword_prefix.empty());
}

TEST(QA_GDB319_SubwordPrefix, MissingPrefixDefaultsEmpty) {
    TempJsonFile file(R"({"model":{"type":"BPE","vocab":{"a":0}}})", "no_prefix");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->subword_prefix.empty());
}

TEST(QA_GDB319_SubwordPrefix, NullPrefixIgnored) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0},"continuing_subword_prefix":null}
    })",
                      "null_prefix");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->subword_prefix.empty());
}

// ===================================================================
// AC9: Error handling — malformed JSON, missing fields, unsupported types
// ===================================================================

TEST(QA_GDB319_Errors, MalformedJSON) {
    TempJsonFile file("{not valid json at all!", "malformed");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    // Error message should not be empty
    EXPECT_FALSE(result.error().message.empty());
}

TEST(QA_GDB319_Errors, EmptyFile) {
    TempJsonFile file("", "empty_file");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB319_Errors, JsonArrayRoot) {
    TempJsonFile file("[1,2,3]", "array_root");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB319_Errors, JsonNullRoot) {
    TempJsonFile file("null", "null_root");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB319_Errors, JsonStringRoot) {
    TempJsonFile file(R"("just a string")", "str_root");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB319_Errors, JsonNumberRoot) {
    TempJsonFile file("42", "num_root");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB319_Errors, MissingModelObject) {
    TempJsonFile file(R"({"version":"1.0"})", "no_model");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB319_Errors, ModelIsArray) {
    TempJsonFile file(R"({"model":[1,2]})", "model_array");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB319_Errors, ModelIsNull) {
    TempJsonFile file(R"({"model":null})", "model_null");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB319_Errors, MissingModelType) {
    TempJsonFile file(R"({"model":{"vocab":{"a":0}}})", "no_type");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB319_Errors, ModelTypeNotString) {
    TempJsonFile file(R"({"model":{"type":123,"vocab":{"a":0}}})", "type_int");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB319_Errors, MissingVocab) {
    TempJsonFile file(R"({"model":{"type":"WordPiece"}})", "no_vocab");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB319_Errors, VocabIsArray) {
    TempJsonFile file(R"({"model":{"type":"WordPiece","vocab":["a","b"]}})", "vocab_array");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB319_Errors, VocabIsNull) {
    TempJsonFile file(R"({"model":{"type":"WordPiece","vocab":null}})", "vocab_null");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB319_Errors, UnsupportedModelType) {
    TempJsonFile file(R"({"model":{"type":"Char","vocab":{"a":0}}})", "bad_type");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB319_Errors, NonexistentFile) {
    auto result = load_tokenizer_config("/tmp/nonexistent_file_gdb319.json");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST(QA_GDB319_Errors, DirectoryPath) {
    auto result = load_tokenizer_config("/tmp");
    ASSERT_FALSE(result.has_value());
    // Either IO_ERROR or PARSE_ERROR is acceptable
    EXPECT_TRUE(result.error().code == StatusCode::IO_ERROR ||
                result.error().code == StatusCode::PARSE_ERROR);
}

TEST(QA_GDB319_Errors, EmptyPath) {
    auto result = load_tokenizer_config("");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST(QA_GDB319_Errors, TruncatedJson) {
    TempJsonFile file(R"({"model":{"type":"WordPiece","vocab":{"a":0)", "trunc");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// ===================================================================
// AC10: Integration with real fixture
// ===================================================================

TEST(QA_GDB319_Integration, FullMiniLMRoundTrip) {
    auto result = load_tokenizer_config(minilm_fixture());
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto& config = *result;

    // Model type
    EXPECT_EQ(config.model_type, TokenizerModelType::WORDPIECE);

    // Vocab
    EXPECT_EQ(config.vocab.size(), 30522u);
    EXPECT_EQ(config.vocab.at("[PAD]"), 0);
    EXPECT_EQ(config.vocab.at("[UNK]"), 100);
    EXPECT_EQ(config.vocab.at("[CLS]"), 101);
    EXPECT_EQ(config.vocab.at("[SEP]"), 102);
    EXPECT_EQ(config.vocab.at("[MASK]"), 103);
    EXPECT_EQ(config.vocab.at("the"), 1996);

    // Special tokens
    EXPECT_EQ(config.special_tokens.pad, 0);
    EXPECT_EQ(config.special_tokens.unk, 100);
    EXPECT_EQ(config.special_tokens.cls, 101);
    EXPECT_EQ(config.special_tokens.sep, 102);
    EXPECT_EQ(config.special_tokens.mask, 103);

    // Normalizer
    EXPECT_EQ(config.normalizer, NormalizerType::BERT);
    EXPECT_TRUE(config.normalizer_lowercase);
    EXPECT_FALSE(config.normalizer_strip_accents);

    // Pre-tokenizer
    EXPECT_EQ(config.pre_tokenizer, PreTokenizerType::PUNCTUATION);

    // Subword prefix
    EXPECT_EQ(config.subword_prefix, "##");

    // No merges for WordPiece
    EXPECT_TRUE(config.merges.empty());
}

// ===================================================================
// Edge cases: added_tokens with null fields
// ===================================================================

TEST(QA_GDB319_AddedTokens, NullIdFieldSkipped) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "added_tokens":[
            {"id":null,"content":"[PAD]","special":true}
        ]
    })",
                      "null_id");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Entry skipped, default preserved
    EXPECT_EQ(result->special_tokens.pad, 0);
}

TEST(QA_GDB319_AddedTokens, NullContentFieldSkipped) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "added_tokens":[
            {"id":5,"content":null,"special":true}
        ]
    })",
                      "null_content");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

TEST(QA_GDB319_AddedTokens, AddedTokensNotArray) {
    // added_tokens as an object instead of array — should be ignored
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "added_tokens":{"key":"value"}
    })",
                      "added_obj");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

// ===================================================================
// Edge cases: normalizer with missing/null lowercase field
// ===================================================================

TEST(QA_GDB319_Normalizer, LowercaseFieldMissingDefaultsTrue) {
    // normalizer object with type but no lowercase field
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"type":"BertNormalizer"}
    })",
                      "norm_no_lc");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // lowercase defaults to true inside the parsing code
    EXPECT_TRUE(result->normalizer_lowercase);
    EXPECT_EQ(result->normalizer, NormalizerType::BERT);
}

TEST(QA_GDB319_Normalizer, LowercaseFieldNull) {
    TempJsonFile file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"type":"BertNormalizer","lowercase":null}
    })",
                      "norm_lc_null");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // null is not boolean, so defaults to true
    EXPECT_TRUE(result->normalizer_lowercase);
}

// ===================================================================
// Stress test: large synthetic vocab
// ===================================================================

TEST(QA_GDB319_Stress, LargeVocab) {
    // Build a JSON string with 10000 vocab entries
    std::string vocab_entries;
    for (int i = 0; i < 10000; ++i) {
        if (i > 0)
            vocab_entries += ",";
        vocab_entries += "\"token_" + std::to_string(i) + "\":" + std::to_string(i);
    }
    std::string json = R"({"model":{"type":"WordPiece","vocab":{)" + vocab_entries + "}}}";

    TempJsonFile file(json, "stress_vocab");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->vocab.size(), 10000u);
    EXPECT_EQ(result->vocab.at("token_0"), 0);
    EXPECT_EQ(result->vocab.at("token_9999"), 9999);
}

TEST(QA_GDB319_Stress, LargeMergesList) {
    // Build a BPE config with 5000 merges
    std::string merges;
    for (int i = 0; i < 5000; ++i) {
        if (i > 0)
            merges += ",";
        merges += "\"a" + std::to_string(i) + " b" + std::to_string(i) + "\"";
    }
    std::string json = R"({"model":{"type":"BPE","vocab":{"a":0},"merges":[)" + merges + "]}}";

    TempJsonFile file(json, "stress_merges");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->merges.size(), 5000u);
    EXPECT_EQ(result->merges[0], "a0 b0");
    EXPECT_EQ(result->merges[4999], "a4999 b4999");
}

} // namespace
} // namespace sixseven
