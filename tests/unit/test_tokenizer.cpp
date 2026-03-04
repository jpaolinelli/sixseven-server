/// @file test_tokenizer.cpp
/// @brief Unit tests for Tokenizer base class and TokenizerConfig (GDB-328).

#include "giodb/vector/tokenizer.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace giodb;

namespace {

/// Minimal concrete tokenizer for testing the interface.
class StubTokenizer : public Tokenizer {
public:
    explicit StubTokenizer(size_t vocab = 30000, size_t max_seq = 128)
        : vocab_(vocab), max_seq_(max_seq) {}

    [[nodiscard]] std::vector<int64_t> encode(const std::string& /*text*/,
                                              size_t max_length) const override {
        // Return CLS + SEP + padding.
        std::vector<int64_t> tokens;
        tokens.push_back(101); // CLS
        tokens.push_back(102); // SEP
        while (tokens.size() < max_length) {
            tokens.push_back(0);
        }
        return tokens;
    }

    [[nodiscard]] std::vector<int64_t>
    attention_mask(const std::vector<int64_t>& tokens) const override {
        std::vector<int64_t> mask;
        mask.reserve(tokens.size());
        for (int64_t t : tokens) {
            mask.push_back(t != 0 ? 1 : 0);
        }
        return mask;
    }

    [[nodiscard]] size_t vocab_size() const override { return vocab_; }
    [[nodiscard]] size_t max_sequence_length() const override { return max_seq_; }

private:
    size_t vocab_;
    size_t max_seq_;
};

} // namespace

// ---------------------------------------------------------------------------
// TokenizerConfig defaults
// ---------------------------------------------------------------------------

TEST(TokenizerConfig, DefaultSpecialTokenIds) {
    SpecialTokenIds ids;
    EXPECT_EQ(ids.pad, 0);
    EXPECT_EQ(ids.unk, 100);
    EXPECT_EQ(ids.cls, 101);
    EXPECT_EQ(ids.sep, 102);
    EXPECT_EQ(ids.mask, 103);
}

TEST(TokenizerConfig, DefaultValues) {
    TokenizerConfig config;
    EXPECT_TRUE(config.vocab.empty());
    EXPECT_EQ(config.model_type, TokenizerModelType::HASH);
    EXPECT_EQ(config.normalizer, NormalizerType::LOWERCASE);
    EXPECT_EQ(config.pre_tokenizer, PreTokenizerType::PUNCTUATION);
    EXPECT_TRUE(config.subword_prefix.empty());
}

TEST(TokenizerConfig, CustomValues) {
    TokenizerConfig config;
    config.vocab = {{"hello", 1}, {"world", 2}};
    config.special_tokens.cls = 200;
    config.model_type = TokenizerModelType::WORDPIECE;
    config.normalizer = NormalizerType::NFC;
    config.pre_tokenizer = PreTokenizerType::WHITESPACE;
    config.subword_prefix = "##";

    EXPECT_EQ(config.vocab.size(), 2u);
    EXPECT_EQ(config.special_tokens.cls, 200);
    EXPECT_EQ(config.model_type, TokenizerModelType::WORDPIECE);
    EXPECT_EQ(config.normalizer, NormalizerType::NFC);
    EXPECT_EQ(config.pre_tokenizer, PreTokenizerType::WHITESPACE);
    EXPECT_EQ(config.subword_prefix, "##");
}

// ---------------------------------------------------------------------------
// Tokenizer interface via StubTokenizer
// ---------------------------------------------------------------------------

TEST(Tokenizer, EncodeReturnsCorrectLength) {
    StubTokenizer tok;
    auto tokens = tok.encode("hello", 10);
    EXPECT_EQ(tokens.size(), 10u);
}

TEST(Tokenizer, EncodeStartsWithCls) {
    StubTokenizer tok;
    auto tokens = tok.encode("hello", 10);
    EXPECT_EQ(tokens[0], 101);
}

TEST(Tokenizer, AttentionMaskMatchesTokens) {
    StubTokenizer tok;
    auto tokens = tok.encode("hello", 8);
    auto mask = tok.attention_mask(tokens);

    ASSERT_EQ(mask.size(), tokens.size());
    // CLS and SEP are non-zero → 1.
    EXPECT_EQ(mask[0], 1);
    EXPECT_EQ(mask[1], 1);
    // Padding → 0.
    for (size_t i = 2; i < mask.size(); ++i) {
        EXPECT_EQ(mask[i], 0);
    }
}

TEST(Tokenizer, VocabSizeAccessor) {
    StubTokenizer tok(50000, 256);
    EXPECT_EQ(tok.vocab_size(), 50000u);
}

TEST(Tokenizer, MaxSequenceLengthAccessor) {
    StubTokenizer tok(30000, 512);
    EXPECT_EQ(tok.max_sequence_length(), 512u);
}

TEST(Tokenizer, PolymorphicUsage) {
    // Can be used through base class pointer.
    std::unique_ptr<Tokenizer> tok = std::make_unique<StubTokenizer>(1000, 64);
    auto tokens = tok->encode("test", 5);
    EXPECT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tok->vocab_size(), 1000u);
    EXPECT_EQ(tok->max_sequence_length(), 64u);
}
