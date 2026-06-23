/// @file test_qa_gdb_325.cpp
/// @brief QA adversarial tests for GDB-325: Unit Tests — Tokenizer Components.
///
/// Tests adversarial edge cases across all tokenizer components:
/// - TokenizerJsonLoader: malformed JSON, corrupt vocab, missing fields
/// - TextNormalizer: invalid UTF-8, extreme strings, combined operations
/// - PreTokenizer: boundary inputs, all-punctuation, long strings
/// - WordPieceTokenizer: boundary max_length, all-UNK, stress, truncation
/// - BPETokenizer: byte-level edge cases, merge edge cases, stress

#include "sixseven/vector/bpe_tokenizer.h"
#include "sixseven/vector/pre_tokenizer.h"
#include "sixseven/vector/text_normalizer.h"
#include "sixseven/vector/tokenizer.h"
#include "sixseven/vector/tokenizer_json_loader.h"
#include "sixseven/vector/wordpiece_tokenizer.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace sixseven;

namespace {

// =====================================================================
// Helpers
// =====================================================================

/// Return absolute path to the MiniLM tokenizer fixture.
std::string minilm_fixture() {
    return (std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures" /
            "tokenizer_minilm.json")
        .string();
}

/// Write a temporary JSON file.
class TempJsonFile {
public:
    explicit TempJsonFile(const std::string& content, const std::string& suffix = "")
        : path_(std::filesystem::temp_directory_path() / ("sixseven_qa_325_" + suffix + ".json")) {
        std::ofstream out(path_);
        out << content;
    }

    ~TempJsonFile() { std::filesystem::remove(path_); }

    [[nodiscard]] std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

/// Build a minimal WordPiece config for testing.
TokenizerConfig make_wordpiece_config() {
    TokenizerConfig config;
    config.vocab = {
        {"hello", 7592},
        {"world", 2088},
        {"the", 1996},
        {"em", 7861},
        {"##bed", 8270},
        {"##ding", 4667},
        {"!", 999},
        {",", 1010},
        {"a", 200},
        {"b", 201},
        {"##c", 202},
    };
    config.special_tokens = {.pad = 0, .unk = 100, .cls = 101, .sep = 102, .mask = 103};
    config.model_type = TokenizerModelType::WORDPIECE;
    config.normalizer = NormalizerType::BERT;
    config.pre_tokenizer = PreTokenizerType::PUNCTUATION;
    config.subword_prefix = "##";
    config.normalizer_lowercase = true;
    config.normalizer_strip_accents = false;
    return config;
}

/// Build a minimal BPE config for testing.
TokenizerConfig make_bpe_config() {
    TokenizerConfig config;
    config.vocab = {
        {"h", 100},
        {"e", 101},
        {"l", 102},
        {"o", 103},
        {"w", 104},
        {"r", 105},
        {"d", 106},
        {"a", 108},
        {"t", 109},
        {"n", 110},
        {"\xC4\xA0", 200},
        {"he", 300},
        {"hel", 301},
        {"hell", 302},
        {"hello", 303},
        {"\xC4\xA0w", 400},
        {"\xC4\xA0wo", 401},
        {"\xC4\xA0wor", 402},
        {"\xC4\xA0worl", 403},
        {"\xC4\xA0world", 404},
    };
    config.special_tokens = {.pad = 1, .unk = 3, .cls = 0, .sep = 2, .mask = 4};
    config.model_type = TokenizerModelType::BPE;
    config.normalizer = NormalizerType::NONE;
    config.pre_tokenizer = PreTokenizerType::WHITESPACE;
    config.merges = {
        "h e",
        "he l",
        "hel l",
        "hell o",
        "\xC4\xA0 w",
        "\xC4\xA0w o",
        "\xC4\xA0wo r",
        "\xC4\xA0wor l",
        "\xC4\xA0worl d",
    };
    return config;
}

// =====================================================================
// QA_GDB325_JsonLoader — Adversarial JSON loading tests
// =====================================================================

TEST(QA_GDB325_JsonLoader, ModelNotObjectReturnsError) {
    TempJsonFile file(R"({"model": "not_an_object"})", "model_str");
    auto result = load_tokenizer_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB325_JsonLoader, AddedTokensMalformedEntriesSkipped) {
    // Entries without required fields should be silently skipped.
    TempJsonFile file(R"({
        "model": {"type": "WordPiece", "vocab": {"a": 0}},
        "added_tokens": [
            {"missing_id": true},
            {"id": "not_a_number", "content": "[PAD]"},
            {"id": 5, "content": 123},
            null,
            42
        ]
    })",
                      "bad_tokens");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Default special tokens unchanged since all entries were skipped.
    EXPECT_EQ(result->special_tokens.pad, 0);
}

TEST(QA_GDB325_JsonLoader, NormalizerNullDoesNotCrash) {
    TempJsonFile file(R"({
        "model": {"type": "WordPiece", "vocab": {"a": 0}},
        "normalizer": null
    })",
                      "norm_null");
    auto result = load_tokenizer_config(file.path());
    // null normalizer should be treated as missing → use defaults.
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

TEST(QA_GDB325_JsonLoader, PreTokenizerNullDoesNotCrash) {
    TempJsonFile file(R"({
        "model": {"type": "WordPiece", "vocab": {"a": 0}},
        "pre_tokenizer": null
    })",
                      "pretok_null");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

TEST(QA_GDB325_JsonLoader, RealMiniLMFixtureLoadAndRoundTrip) {
    // Load the real fixture, construct a tokenizer, encode, verify non-crash.
    auto config = load_tokenizer_config(minilm_fixture());
    ASSERT_TRUE(config.has_value()) << config.error().message;

    WordPieceTokenizer tok(*config);
    auto ids = tok.encode("adversarial test input", 128);
    ASSERT_EQ(ids.size(), 128u);
    EXPECT_EQ(ids[0], 101); // CLS
    EXPECT_NE(ids[1], 0);   // At least one content token
}

// =====================================================================
// QA_GDB325_TextNormalizer — Adversarial normalization tests
// =====================================================================

TEST(QA_GDB325_TextNormalizer, InvalidUtf8SingleByte) {
    BertNormalizer norm(/*lowercase=*/true);
    // 0xFF is not a valid UTF-8 leading byte.
    std::string input("\xFF", 1);
    // Should not crash — produces replacement character or is handled gracefully.
    auto result = norm.normalize(input);
    EXPECT_FALSE(result.empty() && input.size() > 0);
}

TEST(QA_GDB325_TextNormalizer, TruncatedMultibyteSequence) {
    BertNormalizer norm;
    // Start of a 2-byte sequence (0xC3) without continuation byte.
    std::string input("hello\xC3");
    auto result = norm.normalize(input);
    // Should not crash.
    EXPECT_FALSE(result.empty());
}

TEST(QA_GDB325_TextNormalizer, TruncatedThreeByteSequence) {
    BertNormalizer norm;
    // Start of a 3-byte sequence (0xE4) with only one continuation byte.
    std::string input("test\xE4\xBD");
    auto result = norm.normalize(input);
    // Should not crash.
    EXPECT_FALSE(result.empty());
}

TEST(QA_GDB325_TextNormalizer, InvalidContinuationByte) {
    BertNormalizer norm;
    // 0xC3 followed by 0xFF (not a valid continuation byte 10xxxxxx).
    std::string input("a\xC3\xFF"
                      "b");
    auto result = norm.normalize(input);
    // Should not crash and should contain 'a' and 'b'.
    EXPECT_NE(result.find('a'), std::string::npos);
    EXPECT_NE(result.find('b'), std::string::npos);
}

TEST(QA_GDB325_TextNormalizer, LongStringPerformance) {
    BertNormalizer norm(/*lowercase=*/true,
                        /*strip_accents=*/true,
                        /*handle_chinese=*/true,
                        /*clean_text=*/true);
    // 100k character string.
    std::string input(100000, 'A');
    auto result = norm.normalize(input);
    // All 'A' should become 'a'.
    EXPECT_EQ(result.size(), 100000u);
    EXPECT_TRUE(result.find_first_not_of('a') == std::string::npos);
}

TEST(QA_GDB325_TextNormalizer, StripAccentsCombiningMarkOnly) {
    BertNormalizer norm(/*lowercase=*/true, /*strip_accents=*/true);
    // A string that is ONLY a combining acute accent (U+0301).
    std::string input("\xCC\x81");
    auto result = norm.normalize(input);
    // Combining mark should be stripped, leaving empty.
    EXPECT_TRUE(result.empty());
}

TEST(QA_GDB325_TextNormalizer, NullCharacterInMiddle) {
    BertNormalizer norm;
    // Null byte (0x00) is a control character.
    std::string input("hello\x00world", 11);
    auto result = norm.normalize(input);
    // Null should be stripped (it's a control character).
    EXPECT_EQ(result, "helloworld");
}

TEST(QA_GDB325_TextNormalizer, DeleteCharacter0x7F) {
    BertNormalizer norm;
    // 0x7F (DEL) is a control character.
    std::string input("hello\x7Fworld");
    auto result = norm.normalize(input);
    EXPECT_EQ(result, "helloworld");
}

TEST(QA_GDB325_TextNormalizer, UnicodeWhitespace_NBSP) {
    BertNormalizer norm;
    // U+00A0 (NBSP) encoded as UTF-8: C2 A0.
    std::string input("hello\xC2\xA0world");
    auto result = norm.normalize(input);
    // Should be collapsed to a single space.
    EXPECT_EQ(result, "hello world");
}

TEST(QA_GDB325_TextNormalizer, UnicodeWhitespace_EmSpace) {
    BertNormalizer norm;
    // U+2003 (em space) encoded as UTF-8: E2 80 83.
    std::string input("hello\xE2\x80\x83world");
    auto result = norm.normalize(input);
    EXPECT_EQ(result, "hello world");
}

TEST(QA_GDB325_TextNormalizer, LowercaseNormalizerInvalidUtf8) {
    LowercaseNormalizer norm;
    std::string input("\xFF\xFE"
                      "test");
    // Should not crash.
    auto result = norm.normalize(input);
    EXPECT_NE(result.find("test"), std::string::npos);
}

TEST(QA_GDB325_TextNormalizer, BertNormalizerCJKBoundaryCodepoints) {
    BertNormalizer norm(/*lowercase=*/true,
                        /*strip_accents=*/false,
                        /*handle_chinese=*/true);
    // First CJK ideograph: U+4E00 (UTF-8: E4 B8 80).
    std::string input = "a\xE4\xB8\x80"
                        "b";
    auto result = norm.normalize(input);
    // Should add spaces: "a" + " " + CJK + " " + "b"
    EXPECT_EQ(result, "a \xE4\xB8\x80 b");
}

TEST(QA_GDB325_TextNormalizer, FactoryCreateNormalizerAllTypes) {
    // Verify factory produces non-null for all NormalizerType values.
    for (auto type : {NormalizerType::NONE,
                      NormalizerType::LOWERCASE,
                      NormalizerType::BERT,
                      NormalizerType::NFC}) {
        TokenizerConfig config;
        config.normalizer = type;
        auto norm = create_normalizer(config);
        ASSERT_NE(norm, nullptr) << "null normalizer for type " << static_cast<int>(type);
        // Should not crash on empty input.
        auto result = norm->normalize("");
        EXPECT_EQ(result, "");
    }
}

// =====================================================================
// QA_GDB325_PreTokenizer — Adversarial pre-tokenization tests
// =====================================================================

TEST(QA_GDB325_PreTokenizer, SingleCharacterWord) {
    BertPreTokenizer pt;
    auto tokens = pt.pre_tokenize("a");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "a");
}

TEST(QA_GDB325_PreTokenizer, SinglePunctuationCharacter) {
    BertPreTokenizer pt;
    auto tokens = pt.pre_tokenize("!");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "!");
}

TEST(QA_GDB325_PreTokenizer, LongWordNoPunctuation) {
    BertPreTokenizer pt;
    std::string long_word(10000, 'x');
    auto tokens = pt.pre_tokenize(long_word);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].size(), 10000u);
}

TEST(QA_GDB325_PreTokenizer, AlternatingPunctuationAndLetters) {
    BertPreTokenizer pt;
    // "a.b.c" -> ["a", ".", "b", ".", "c"]
    auto tokens = pt.pre_tokenize("a.b.c");
    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0], "a");
    EXPECT_EQ(tokens[1], ".");
    EXPECT_EQ(tokens[2], "b");
    EXPECT_EQ(tokens[3], ".");
    EXPECT_EQ(tokens[4], "c");
}

TEST(QA_GDB325_PreTokenizer, WhitespaceOnlyCarriageReturn) {
    BertPreTokenizer pt;
    auto tokens = pt.pre_tokenize("\r\r\r");
    EXPECT_TRUE(tokens.empty());
}

TEST(QA_GDB325_PreTokenizer, MixedUnicodeAndAscii) {
    BertPreTokenizer pt;
    // UTF-8 characters are not ASCII, so they're treated as word characters.
    std::string input = "\xC3\xA9"
                        "hello"; // e-acute + "hello"
    auto tokens = pt.pre_tokenize(input);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], input); // All kept as one token.
}

TEST(QA_GDB325_PreTokenizer, FactoryCreatePreTokenizerAllTypes) {
    for (auto type :
         {PreTokenizerType::NONE, PreTokenizerType::WHITESPACE, PreTokenizerType::PUNCTUATION}) {
        TokenizerConfig config;
        config.pre_tokenizer = type;
        auto pt = create_pre_tokenizer(config);
        ASSERT_NE(pt, nullptr) << "null pre_tokenizer for type " << static_cast<int>(type);
        auto tokens = pt->pre_tokenize("");
        EXPECT_TRUE(tokens.empty());
    }
}

TEST(QA_GDB325_PreTokenizer, WhitespaceSplitLongInput) {
    WhitespaceSplitPreTokenizer pt;
    // 1000 words of "word" separated by spaces.
    std::string input;
    for (int i = 0; i < 1000; ++i) {
        if (i > 0)
            input += " ";
        input += "word";
    }
    auto tokens = pt.pre_tokenize(input);
    EXPECT_EQ(tokens.size(), 1000u);
}

TEST(QA_GDB325_PreTokenizer, NullPreTokenizerSingleSpan) {
    NullPreTokenizer pt;
    std::string input = "hello world foo bar";
    auto tokens = pt.pre_tokenize(input);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], input);
    // Verify the view points into the original string.
    EXPECT_EQ(tokens[0].data(), input.data());
}

// =====================================================================
// QA_GDB325_WordPiece — Adversarial WordPiece tokenizer tests
// =====================================================================

class QA_GDB325_WordPieceTest : public ::testing::Test {
protected:
    void SetUp() override {
        tokenizer_ = std::make_unique<WordPieceTokenizer>(make_wordpiece_config());
    }
    std::unique_ptr<WordPieceTokenizer> tokenizer_;
};

TEST_F(QA_GDB325_WordPieceTest, AllUnknownWordsMultiple) {
    // Multiple unknown words should each produce UNK.
    auto ids = tokenizer_->encode("zzz yyy xxx", 16);
    ASSERT_EQ(ids.size(), 16u);
    EXPECT_EQ(ids[0], 101); // CLS
    EXPECT_EQ(ids[1], 100); // UNK for "zzz"
    EXPECT_EQ(ids[2], 100); // UNK for "yyy"
    EXPECT_EQ(ids[3], 100); // UNK for "xxx"
    EXPECT_EQ(ids[4], 102); // SEP
}

TEST_F(QA_GDB325_WordPieceTest, MixedKnownAndUnknown) {
    auto ids = tokenizer_->encode("hello zzz world", 16);
    ASSERT_EQ(ids.size(), 16u);
    EXPECT_EQ(ids[0], 101);  // CLS
    EXPECT_EQ(ids[1], 7592); // hello
    EXPECT_EQ(ids[2], 100);  // UNK for "zzz"
    EXPECT_EQ(ids[3], 2088); // world
    EXPECT_EQ(ids[4], 102);  // SEP
}

TEST_F(QA_GDB325_WordPieceTest, VeryLargeMaxLength) {
    auto ids = tokenizer_->encode("hello", 10000);
    ASSERT_EQ(ids.size(), 10000u);
    EXPECT_EQ(ids[0], 101);  // CLS
    EXPECT_EQ(ids[1], 7592); // hello
    EXPECT_EQ(ids[2], 102);  // SEP
    // Rest should be PAD.
    for (size_t i = 3; i < 10000; ++i) {
        EXPECT_EQ(ids[i], 0) << "index " << i;
    }
}

TEST_F(QA_GDB325_WordPieceTest, OnlyWhitespaceInput) {
    auto ids = tokenizer_->encode("   \t\n  ", 8);
    ASSERT_EQ(ids.size(), 8u);
    // BertNormalizer collapses whitespace → empty after trim → no content tokens.
    EXPECT_EQ(ids[0], 101); // CLS
    EXPECT_EQ(ids[1], 102); // SEP
    for (size_t i = 2; i < 8; ++i) {
        EXPECT_EQ(ids[i], 0);
    }
}

TEST_F(QA_GDB325_WordPieceTest, OnlyPunctuationInput) {
    auto ids = tokenizer_->encode("!!!", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], 101); // CLS
    EXPECT_EQ(ids[1], 999); // !
    EXPECT_EQ(ids[2], 999); // !
    EXPECT_EQ(ids[3], 999); // !
    EXPECT_EQ(ids[4], 102); // SEP
}

TEST_F(QA_GDB325_WordPieceTest, TruncationExactBoundary) {
    // "hello, world" produces: hello, ",", world = 3 content tokens.
    // With max_length=5 (CLS + 3 content + SEP), should fit exactly.
    auto ids = tokenizer_->encode("hello, world", 5);
    ASSERT_EQ(ids.size(), 5u);
    EXPECT_EQ(ids[0], 101);  // CLS
    EXPECT_EQ(ids[1], 7592); // hello
    EXPECT_EQ(ids[2], 1010); // ,
    EXPECT_EQ(ids[3], 2088); // world
    EXPECT_EQ(ids[4], 102);  // SEP
}

TEST_F(QA_GDB325_WordPieceTest, TruncationOneShort) {
    // Same input, max_length=4 should truncate one content token.
    auto ids = tokenizer_->encode("hello, world", 4);
    ASSERT_EQ(ids.size(), 4u);
    EXPECT_EQ(ids[0], 101);  // CLS
    EXPECT_EQ(ids[1], 7592); // hello
    EXPECT_EQ(ids[2], 1010); // ,
    EXPECT_EQ(ids[3], 102);  // SEP
}

TEST_F(QA_GDB325_WordPieceTest, AttentionMaskAllPadding) {
    // CLS + SEP + padding.
    auto ids = tokenizer_->encode("", 8);
    auto mask = tokenizer_->attention_mask(ids);
    ASSERT_EQ(mask.size(), 8u);
    EXPECT_EQ(mask[0], 1); // CLS
    EXPECT_EQ(mask[1], 1); // SEP
    for (size_t i = 2; i < 8; ++i) {
        EXPECT_EQ(mask[i], 0);
    }
}

TEST_F(QA_GDB325_WordPieceTest, AttentionMaskNoContent) {
    auto ids = tokenizer_->encode("hello", 0);
    auto mask = tokenizer_->attention_mask(ids);
    EXPECT_TRUE(mask.empty());
}

TEST_F(QA_GDB325_WordPieceTest, SubwordSplitABC) {
    // "abc": try "abc" (not in vocab), "ab" (not), "a" (200) → found.
    // Remainder "bc": try "##bc" (not in vocab), "##b" (not) → no match → UNK.
    // WordPiece: if any subword fails, entire word becomes UNK.
    auto ids = tokenizer_->encode("abc", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], 101); // CLS
    EXPECT_EQ(ids[1], 100); // UNK (no "##bc" or "##b" in vocab)
    EXPECT_EQ(ids[2], 102); // SEP
}

TEST_F(QA_GDB325_WordPieceTest, LongWordManySubwords) {
    // "embedding" with our test vocab: "em"(7861) + "##bed"(8270) + "##ding"(4667)
    auto ids = tokenizer_->encode("embedding", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], 101);  // CLS
    EXPECT_EQ(ids[1], 7861); // em
    EXPECT_EQ(ids[2], 8270); // ##bed
    EXPECT_EQ(ids[3], 4667); // ##ding
    EXPECT_EQ(ids[4], 102);  // SEP
}

// =====================================================================
// QA_GDB325_WordPiece_E2E — End-to-end with real fixture
// =====================================================================

class QA_GDB325_WordPieceE2E : public ::testing::Test {
protected:
    void SetUp() override {
        auto config = load_tokenizer_config(minilm_fixture());
        ASSERT_TRUE(config.has_value()) << config.error().message;
        tokenizer_ = std::make_unique<WordPieceTokenizer>(*config);
    }
    std::unique_ptr<WordPieceTokenizer> tokenizer_;
};

TEST_F(QA_GDB325_WordPieceE2E, NumbersTokenized) {
    // Python: "123" -> [101, 13464, 102, ...]
    auto ids = tokenizer_->encode("123", 128);
    ASSERT_GE(ids.size(), 3u);
    EXPECT_EQ(ids[0], 101); // CLS
    // "123" is a full word in the BERT vocab.
    EXPECT_NE(ids[1], 100); // Should NOT be UNK
    EXPECT_NE(ids[1], 0);   // Should NOT be PAD
}

TEST_F(QA_GDB325_WordPieceE2E, SpecialCharsProduceTokens) {
    auto ids = tokenizer_->encode("[CLS] test [SEP]", 128);
    ASSERT_GE(ids.size(), 2u);
    // The actual [CLS] and [SEP] in the text are treated as regular text
    // (split by pre-tokenizer), not as special tokens.
    EXPECT_EQ(ids[0], 101); // Real CLS added by encoder
}

TEST_F(QA_GDB325_WordPieceE2E, VeryLongSentence) {
    // Generate a sentence with 200 words.
    std::string sentence;
    for (int i = 0; i < 200; ++i) {
        if (i > 0)
            sentence += " ";
        sentence += "hello";
    }
    auto ids = tokenizer_->encode(sentence, 128);
    ASSERT_EQ(ids.size(), 128u);
    // Should be truncated: CLS + 126 content + SEP.
    EXPECT_EQ(ids[0], 101);   // CLS
    EXPECT_EQ(ids[127], 102); // SEP at end due to truncation
}

TEST_F(QA_GDB325_WordPieceE2E, UnicodeInputAccents) {
    // "café" with BertNormalizer (lowercase, no strip_accents).
    auto ids = tokenizer_->encode("caf\xC3\xA9", 128);
    ASSERT_GE(ids.size(), 3u);
    EXPECT_EQ(ids[0], 101); // CLS
    // Should produce valid tokens (not all UNK).
}

TEST_F(QA_GDB325_WordPieceE2E, PunctuationHeavySentence) {
    // Each punctuation becomes its own token.
    auto ids = tokenizer_->encode("wow!!! really???", 128);
    ASSERT_GE(ids.size(), 10u);
    EXPECT_EQ(ids[0], 101); // CLS
    // "wow" + "!" + "!" + "!" + "really" + "?" + "?" + "?" + SEP
}

TEST_F(QA_GDB325_WordPieceE2E, MaxLength128FullPadding) {
    auto ids = tokenizer_->encode("hi", 128);
    ASSERT_EQ(ids.size(), 128u);
    EXPECT_EQ(ids[0], 101); // CLS
    // "hi" should be a single token.
    EXPECT_EQ(ids[ids.size() - 1], 0); // Last is PAD
}

TEST_F(QA_GDB325_WordPieceE2E, ConsistentReEncoding) {
    // Encoding the same text twice should produce identical results.
    auto ids1 = tokenizer_->encode("the quick brown fox", 128);
    auto ids2 = tokenizer_->encode("the quick brown fox", 128);
    EXPECT_EQ(ids1, ids2);
}

// =====================================================================
// QA_GDB325_BPE — Adversarial BPE tokenizer tests
// =====================================================================

class QA_GDB325_BPETest : public ::testing::Test {
protected:
    void SetUp() override { tokenizer_ = std::make_unique<BPETokenizer>(make_bpe_config()); }
    std::unique_ptr<BPETokenizer> tokenizer_;
};

TEST_F(QA_GDB325_BPETest, NonAsciiByteInput) {
    // Byte 0xFF maps through byte-to-unicode to a high codepoint.
    // That codepoint won't be in our test vocab → UNK.
    std::string input("\xFF", 1);
    auto ids = tokenizer_->encode(input, 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], 0); // CLS
    EXPECT_EQ(ids[1], 3); // UNK for the unknown byte mapping
    EXPECT_EQ(ids[2], 2); // SEP
}

TEST_F(QA_GDB325_BPETest, ManyWordsStress) {
    // 100 words: "hello" repeated.
    std::string input;
    for (int i = 0; i < 100; ++i) {
        if (i > 0)
            input += " ";
        input += "hello";
    }
    auto ids = tokenizer_->encode(input, 256);
    ASSERT_EQ(ids.size(), 256u);
    EXPECT_EQ(ids[0], 0);   // CLS
    EXPECT_EQ(ids[1], 303); // First "hello"
    // Verify it doesn't crash and returns correct size.
}

TEST_F(QA_GDB325_BPETest, TruncationCutsContent) {
    // "hello world" = 2 content tokens (303, 404).
    // max_length=3: CLS + 1 content + SEP.
    auto ids = tokenizer_->encode("hello world", 3);
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], 0);   // CLS
    EXPECT_EQ(ids[1], 303); // hello
    EXPECT_EQ(ids[2], 2);   // SEP
}

TEST_F(QA_GDB325_BPETest, LargeMaxLengthPadding) {
    auto ids = tokenizer_->encode("hello", 5000);
    ASSERT_EQ(ids.size(), 5000u);
    EXPECT_EQ(ids[0], 0);   // CLS
    EXPECT_EQ(ids[1], 303); // hello
    EXPECT_EQ(ids[2], 2);   // SEP
    for (size_t i = 3; i < 5000; ++i) {
        EXPECT_EQ(ids[i], 1) << "index " << i; // PAD
    }
}

TEST_F(QA_GDB325_BPETest, AttentionMaskReflectsPadToken) {
    auto ids = tokenizer_->encode("hello", 8);
    auto mask = tokenizer_->attention_mask(ids);
    ASSERT_EQ(mask.size(), 8u);
    // CLS(0) ≠ PAD(1) → 1, hello(303) ≠ PAD → 1, SEP(2) ≠ PAD → 1
    EXPECT_EQ(mask[0], 1);
    EXPECT_EQ(mask[1], 1);
    EXPECT_EQ(mask[2], 1);
    // PAD(1) == PAD(1) → 0
    for (size_t i = 3; i < 8; ++i) {
        EXPECT_EQ(mask[i], 0);
    }
}

TEST_F(QA_GDB325_BPETest, SingleSpaceInput) {
    // A single space character. Pre-tokenized as whitespace → no words → no content.
    auto ids = tokenizer_->encode(" ", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], 0); // CLS
    EXPECT_EQ(ids[1], 2); // SEP
}

TEST_F(QA_GDB325_BPETest, VocabSizeAndMaxSeqLength) {
    EXPECT_EQ(tokenizer_->vocab_size(), make_bpe_config().vocab.size());
    EXPECT_EQ(tokenizer_->max_sequence_length(), 1024u);
}

TEST_F(QA_GDB325_BPETest, ConsistentReEncoding) {
    auto ids1 = tokenizer_->encode("hello world", 32);
    auto ids2 = tokenizer_->encode("hello world", 32);
    EXPECT_EQ(ids1, ids2);
}

// =====================================================================
// QA_GDB325_HashTokenizer — Adversarial HashTokenizer tests
// =====================================================================

TEST(QA_GDB325_HashTokenizer, EmptyInput) {
    HashTokenizer tok;
    auto ids = tok.encode("", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], 101); // CLS
    EXPECT_EQ(ids[1], 102); // SEP
    for (size_t i = 2; i < 8; ++i) {
        EXPECT_EQ(ids[i], 0); // PAD
    }
}

TEST(QA_GDB325_HashTokenizer, MaxLengthZero) {
    HashTokenizer tok;
    auto ids = tok.encode("hello", 0);
    EXPECT_TRUE(ids.empty());
}

TEST(QA_GDB325_HashTokenizer, MaxLengthOne) {
    // Only CLS fits, but the code pushes CLS first then SEP.
    // With max_length=1, after CLS there's no room for content or SEP.
    // The truncation path kicks in: resize to 1, set last to SEP.
    HashTokenizer tok;
    auto ids = tok.encode("hello", 1);
    ASSERT_EQ(ids.size(), 1u);
    // After the truncation code: tokens resized to 1, tokens.back() = SEP.
    EXPECT_EQ(ids[0], 102); // SEP replaces CLS via truncation
}

TEST(QA_GDB325_HashTokenizer, LongInput) {
    HashTokenizer tok;
    // 10000 'a's = one continuous word → CLS + 1 hash + SEP + padding.
    std::string input(10000, 'a');
    auto ids = tok.encode(input, 128);
    ASSERT_EQ(ids.size(), 128u);
    EXPECT_EQ(ids[0], 101); // CLS
    EXPECT_NE(ids[1], 0);   // Hashed word token
    EXPECT_EQ(ids[2], 102); // SEP
    for (size_t i = 3; i < 128; ++i) {
        EXPECT_EQ(ids[i], 0); // PAD
    }
}

TEST(QA_GDB325_HashTokenizer, ConsistentHashing) {
    HashTokenizer tok;
    auto ids1 = tok.encode("hello world", 8);
    auto ids2 = tok.encode("hello world", 8);
    EXPECT_EQ(ids1, ids2);
}

TEST(QA_GDB325_HashTokenizer, DifferentWordsProduceDifferentIds) {
    HashTokenizer tok;
    auto ids_hello = tok.encode("hello", 4);
    auto ids_world = tok.encode("world", 4);
    // Content token (index 1) should differ.
    EXPECT_NE(ids_hello[1], ids_world[1]);
}

TEST(QA_GDB325_HashTokenizer, AttentionMask) {
    HashTokenizer tok;
    auto ids = tok.encode("hello", 8);
    auto mask = tok.attention_mask(ids);
    ASSERT_EQ(mask.size(), 8u);
    // CLS and content and SEP → 1.
    EXPECT_EQ(mask[0], 1);
    EXPECT_EQ(mask[1], 1);
    EXPECT_EQ(mask[2], 1); // SEP
}

} // namespace
