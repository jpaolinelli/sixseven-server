#include "giodb/vector/wordpiece_tokenizer.h"

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace giodb {
namespace {

/// Build a minimal vocabulary from all-MiniLM-L6-v2 containing only
/// the tokens needed for the test cases. Reference token IDs were
/// generated from Python: AutoTokenizer.from_pretrained(
///     "sentence-transformers/all-MiniLM-L6-v2").
// clang-format off
const std::unordered_map<std::string, int64_t> TEST_VOCAB = {
    {"!", 999},
    {"##12", 12521},
    {"##3", 2509},
    {"##bc", 9818},
    {"##bed", 8270},
    {"##ble", 3468},
    {"##ding", 4667},
    {"##ffa", 20961},
    {"##y", 2100},
    {"##za", 4143},
    {",", 1010},
    {"and", 1998},
    {"brown", 2829},
    {"cat", 4937},
    {"dog", 3899},
    {"em", 7861},
    {"fox", 4419},
    {"hello", 7592},
    {"here", 2182},
    {"is", 2003},
    {"jumps", 14523},
    {"lazy", 13971},
    {"mat", 13523},
    {"on", 2006},
    {"over", 2058},
    {"quick", 4248},
    {"sat", 2938},
    {"the", 1996},
    {"una", 14477},
    {"world", 2088},
    {"x", 1060},
};
// clang-format on

/// Create a TokenizerConfig matching all-MiniLM-L6-v2 settings.
TokenizerConfig make_test_config() {
    TokenizerConfig config;
    config.vocab = TEST_VOCAB;
    config.special_tokens = {.pad = 0, .unk = 100, .cls = 101, .sep = 102, .mask = 103};
    config.model_type = TokenizerModelType::WORDPIECE;
    config.normalizer = NormalizerType::BERT;
    config.pre_tokenizer = PreTokenizerType::PUNCTUATION;
    config.subword_prefix = "##";
    config.normalizer_lowercase = true;
    config.normalizer_strip_accents = false;
    return config;
}

const size_t MAX_LEN = 16;

class WordPieceTokenizerTest : public ::testing::Test {
protected:
    void SetUp() override { tokenizer_ = std::make_unique<WordPieceTokenizer>(make_test_config()); }

    std::unique_ptr<WordPieceTokenizer> tokenizer_;
};

// --- Python-validated reference tests ---

TEST_F(WordPieceTokenizerTest, HelloWorld) {
    // Python: [101, 7592, 2088, 102, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
    auto ids = tokenizer_->encode("hello world", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {101, 7592, 2088, 102, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(ids, expected);
}

TEST_F(WordPieceTokenizerTest, SubwordSplittingEmbedding) {
    // "embedding" -> ["em", "##bed", "##ding"]
    // Python: [101, 7861, 8270, 4667, 102, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
    auto ids = tokenizer_->encode("embedding", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {101, 7861, 8270, 4667, 102, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(ids, expected);
}

TEST_F(WordPieceTokenizerTest, SubwordSplittingUnaffable) {
    // "unaffable" -> ["una", "##ffa", "##ble"]
    // Python: [101, 14477, 20961, 3468, 102, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
    auto ids = tokenizer_->encode("unaffable", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {101, 14477, 20961, 3468, 102, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(ids, expected);
}

TEST_F(WordPieceTokenizerTest, TheQuickBrownFox) {
    // Python: [101, 1996, 4248, 2829, 4419, 102, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
    auto ids = tokenizer_->encode("the quick brown fox", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {
        101, 1996, 4248, 2829, 4419, 102, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(ids, expected);
}

TEST_F(WordPieceTokenizerTest, SubwordDecompositionXyzabc123) {
    // "xyzabc123" decomposes to subwords: ["x", "##y", "##za", "##bc", "##12", "##3"]
    // Python: [101, 1060, 2100, 4143, 9818, 12521, 2509, 102, 0, 0, 0, 0, 0, 0, 0, 0]
    auto ids = tokenizer_->encode("xyzabc123", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {
        101, 1060, 2100, 4143, 9818, 12521, 2509, 102, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(ids, expected);
}

TEST_F(WordPieceTokenizerTest, MixedKnownAndSubwords) {
    // "the xyzabc is here" -> ["the", "x", "##y", "##za", "##bc", "is", "here"]
    // Python: [101, 1996, 1060, 2100, 4143, 9818, 2003, 2182, 102, 0, 0, 0, 0, 0, 0, 0]
    auto ids = tokenizer_->encode("the xyzabc is here", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {
        101, 1996, 1060, 2100, 4143, 9818, 2003, 2182, 102, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(ids, expected);
}

TEST_F(WordPieceTokenizerTest, PunctuationAfterPreTokenizer) {
    // "hello, world!" -> ["hello", ",", "world", "!"]
    // Python: [101, 7592, 1010, 2088, 999, 102, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
    auto ids = tokenizer_->encode("hello, world!", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {101, 7592, 1010, 2088, 999, 102, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(ids, expected);
}

TEST_F(WordPieceTokenizerTest, EmptyInput) {
    // Python: [101, 102, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
    auto ids = tokenizer_->encode("", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {101, 102, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(ids, expected);
}

TEST_F(WordPieceTokenizerTest, MaxLengthTruncation) {
    // "the quick brown fox jumps over the lazy dog and the cat sat on the mat"
    // With max_length=10, Python truncates to first 8 content tokens + CLS + SEP:
    // [101, 1996, 4248, 2829, 4419, 14523, 2058, 1996, 13971, 102]
    auto ids = tokenizer_->encode(
        "the quick brown fox jumps over the lazy dog and the cat sat on the mat", 10);
    ASSERT_EQ(ids.size(), 10u);
    std::vector<int64_t> expected = {101, 1996, 4248, 2829, 4419, 14523, 2058, 1996, 13971, 102};
    EXPECT_EQ(ids, expected);
}

// --- Unknown word test ---

TEST_F(WordPieceTokenizerTest, TrulyUnknownWord) {
    // A word where no single character is in vocab produces UNK.
    auto ids = tokenizer_->encode("zzzzz", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    // CLS, UNK, SEP, then PAD
    EXPECT_EQ(ids[0], 101);
    EXPECT_EQ(ids[1], 100); // UNK
    EXPECT_EQ(ids[2], 102);
    for (size_t i = 3; i < MAX_LEN; ++i) {
        EXPECT_EQ(ids[i], 0);
    }
}

// --- Attention mask ---

TEST_F(WordPieceTokenizerTest, AttentionMask) {
    auto ids = tokenizer_->encode("hello world", MAX_LEN);
    auto mask = tokenizer_->attention_mask(ids);
    ASSERT_EQ(mask.size(), MAX_LEN);
    // CLS, hello, world, SEP -> 1s; rest -> 0s
    std::vector<int64_t> expected = {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(mask, expected);
}

// --- Edge cases ---

TEST_F(WordPieceTokenizerTest, MaxLengthZero) {
    auto ids = tokenizer_->encode("hello", 0);
    EXPECT_TRUE(ids.empty());
}

TEST_F(WordPieceTokenizerTest, MaxLengthOne) {
    // Only room for CLS, no room for SEP.
    auto ids = tokenizer_->encode("hello", 1);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], 101); // CLS
}

TEST_F(WordPieceTokenizerTest, MaxLengthTwo) {
    // Room for CLS + SEP only, no content.
    auto ids = tokenizer_->encode("hello", 2);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 101); // CLS
    EXPECT_EQ(ids[1], 102); // SEP
}

TEST_F(WordPieceTokenizerTest, VocabSize) {
    EXPECT_EQ(tokenizer_->vocab_size(), TEST_VOCAB.size());
}

TEST_F(WordPieceTokenizerTest, MaxSequenceLength) {
    EXPECT_EQ(tokenizer_->max_sequence_length(), 512u);
}

TEST_F(WordPieceTokenizerTest, CaseInsensitive) {
    // BERT normalizer lowercases, so "HELLO WORLD" should match "hello world".
    auto ids_lower = tokenizer_->encode("hello world", MAX_LEN);
    auto ids_upper = tokenizer_->encode("HELLO WORLD", MAX_LEN);
    EXPECT_EQ(ids_lower, ids_upper);
}

TEST_F(WordPieceTokenizerTest, ConfigurableSubwordPrefix) {
    // Use a non-standard prefix to verify it's configurable.
    auto config = make_test_config();
    config.subword_prefix = "@@";
    // Replace "##bed" with "@@bed" in the vocab.
    config.vocab.erase("##bed");
    config.vocab.erase("##ding");
    config.vocab["@@bed"] = 8270;
    config.vocab["@@ding"] = 4667;

    WordPieceTokenizer custom_tok(config);
    auto ids = custom_tok.encode("embedding", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    // Should produce same token IDs since vocab maps to same values.
    std::vector<int64_t> expected = {101, 7861, 8270, 4667, 102, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(ids, expected);
}

} // namespace
} // namespace giodb
