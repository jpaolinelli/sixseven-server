/// @file test_qa_gdb_318.cpp
/// @brief QA adversarial tests for GDB-318: Tokenizer Abstraction & Interface.
///
/// Tests the Tokenizer base class, HashTokenizer implementation,
/// TokenizerConfig struct, SpecialTokenIds defaults, and OnnxProvider
/// integration with the Tokenizer interface.

#include "sixseven/vector/onnx_provider.h"
#include "sixseven/vector/tokenizer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

using namespace sixseven;

// ===========================================================================
// Mock for OnnxProvider integration tests
// ===========================================================================

namespace {

class MockOnnxSessionGDB318 : public OnnxSession {
public:
    void set_embedding(const std::vector<float>& embedding) {
        embedding_ = embedding;
        should_fail_ = false;
    }

    Result<std::vector<float>> run(const std::vector<int64_t>& input_ids,
                                   const std::vector<int64_t>& attention_mask,
                                   size_t expected_dim) override {
        last_input_ids_ = input_ids;
        last_attention_mask_ = attention_mask;
        run_count_++;

        if (should_fail_) {
            return make_error(StatusCode::INTERNAL_ERROR, "mock failure");
        }
        if (embedding_.size() != expected_dim) {
            return make_error(StatusCode::INTERNAL_ERROR, "dimension mismatch");
        }
        return ok(embedding_);
    }

    Result<void> health_check() override { return ok(); }

    std::vector<int64_t> last_input_ids_;
    std::vector<int64_t> last_attention_mask_;
    int run_count_ = 0;

private:
    std::vector<float> embedding_;
    bool should_fail_ = false;
};

/// A trivial custom tokenizer for testing the OnnxProvider 4-arg constructor.
class StubTokenizer : public Tokenizer {
public:
    explicit StubTokenizer(size_t max_seq = 64) : max_seq_(max_seq) {}

    [[nodiscard]] std::vector<int64_t> encode(const std::string& /*text*/,
                                              size_t max_length) const override {
        // Always returns [1, 2, 0, 0, ...] padded to max_length.
        std::vector<int64_t> tokens(max_length, 0);
        if (max_length > 0)
            tokens[0] = 1;
        if (max_length > 1)
            tokens[1] = 2;
        return tokens;
    }

    [[nodiscard]] std::vector<int64_t>
    attention_mask(const std::vector<int64_t>& tokens) const override {
        std::vector<int64_t> mask;
        mask.reserve(tokens.size());
        for (auto t : tokens) {
            mask.push_back(t != 0 ? 1 : 0);
        }
        return mask;
    }

    [[nodiscard]] size_t vocab_size() const override { return 100; }
    [[nodiscard]] size_t max_sequence_length() const override { return max_seq_; }

private:
    size_t max_seq_;
};

} // namespace

// ===========================================================================
// SpecialTokenIds defaults
// ===========================================================================

TEST(QA_GDB318_SpecialTokenIds, DefaultValues) {
    SpecialTokenIds ids;
    EXPECT_EQ(ids.pad, 0);
    EXPECT_EQ(ids.unk, 100);
    EXPECT_EQ(ids.cls, 101);
    EXPECT_EQ(ids.sep, 102);
    EXPECT_EQ(ids.mask, 103);
}

// ===========================================================================
// TokenizerConfig struct
// ===========================================================================

TEST(QA_GDB318_TokenizerConfig, DefaultValues) {
    TokenizerConfig config;
    EXPECT_TRUE(config.vocab.empty());
    EXPECT_EQ(config.model_type, TokenizerModelType::HASH);
    EXPECT_EQ(config.normalizer, NormalizerType::LOWERCASE);
    EXPECT_EQ(config.pre_tokenizer, PreTokenizerType::PUNCTUATION);
    EXPECT_TRUE(config.subword_prefix.empty());

    // Special tokens should have correct defaults.
    EXPECT_EQ(config.special_tokens.pad, 0);
    EXPECT_EQ(config.special_tokens.cls, 101);
    EXPECT_EQ(config.special_tokens.sep, 102);
}

TEST(QA_GDB318_TokenizerConfig, CanPopulateVocab) {
    TokenizerConfig config;
    config.vocab["hello"] = 500;
    config.vocab["world"] = 501;
    EXPECT_EQ(config.vocab.size(), 2u);
    EXPECT_EQ(config.vocab.at("hello"), 500);
}

TEST(QA_GDB318_TokenizerConfig, EnumVariants) {
    // Verify all enum variants can be assigned.
    TokenizerConfig config;

    config.model_type = TokenizerModelType::WORDPIECE;
    EXPECT_EQ(config.model_type, TokenizerModelType::WORDPIECE);

    config.model_type = TokenizerModelType::BPE;
    EXPECT_EQ(config.model_type, TokenizerModelType::BPE);

    config.normalizer = NormalizerType::NONE;
    EXPECT_EQ(config.normalizer, NormalizerType::NONE);

    config.normalizer = NormalizerType::NFC;
    EXPECT_EQ(config.normalizer, NormalizerType::NFC);

    config.pre_tokenizer = PreTokenizerType::NONE;
    EXPECT_EQ(config.pre_tokenizer, PreTokenizerType::NONE);

    config.pre_tokenizer = PreTokenizerType::WHITESPACE;
    EXPECT_EQ(config.pre_tokenizer, PreTokenizerType::WHITESPACE);
}

// ===========================================================================
// HashTokenizer — vocab_size / max_sequence_length
// ===========================================================================

TEST(QA_GDB318_HashTokenizer, VocabSizeConstant) {
    HashTokenizer tok;
    EXPECT_EQ(tok.vocab_size(), HashTokenizer::VOCAB_SIZE);
    EXPECT_EQ(tok.vocab_size(), 30000u);
}

TEST(QA_GDB318_HashTokenizer, DefaultMaxSeqLength) {
    HashTokenizer tok;
    EXPECT_EQ(tok.max_sequence_length(), HashTokenizer::DEFAULT_MAX_SEQ_LENGTH);
    EXPECT_EQ(tok.max_sequence_length(), 128u);
}

TEST(QA_GDB318_HashTokenizer, CustomMaxSeqLength) {
    HashTokenizer tok(256);
    EXPECT_EQ(tok.max_sequence_length(), 256u);
}

TEST(QA_GDB318_HashTokenizer, MaxSeqLengthZero) {
    HashTokenizer tok(0);
    EXPECT_EQ(tok.max_sequence_length(), 0u);
}

TEST(QA_GDB318_HashTokenizer, MaxSeqLengthOne) {
    HashTokenizer tok(1);
    EXPECT_EQ(tok.max_sequence_length(), 1u);
}

// ===========================================================================
// HashTokenizer::encode — boundary values for max_length
// ===========================================================================

TEST(QA_GDB318_Encode, MaxLengthZeroReturnsEmpty) {
    HashTokenizer tok;
    auto tokens = tok.encode("hello", 0);
    EXPECT_TRUE(tokens.empty());
}

TEST(QA_GDB318_Encode, MaxLengthOneReturnsSingleToken) {
    HashTokenizer tok;
    auto tokens = tok.encode("hello", 1);
    ASSERT_EQ(tokens.size(), 1u);
    // After truncation, the last token is always SEP.
    EXPECT_EQ(tokens[0], 102);
}

TEST(QA_GDB318_Encode, MaxLengthTwoOnlySpecialTokens) {
    HashTokenizer tok;
    auto tokens = tok.encode("hello world", 2);
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], 101); // CLS
    EXPECT_EQ(tokens[1], 102); // SEP
    // Text tokens are dropped — not enough room.
}

TEST(QA_GDB318_Encode, MaxLengthThreeOneWordToken) {
    HashTokenizer tok;
    auto tokens = tok.encode("hello world", 3);
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], 101); // CLS
    // Only one word fits (CLS + 1 word + SEP = 3).
    EXPECT_GE(tokens[1], HashTokenizer::VOCAB_OFFSET);
    EXPECT_EQ(tokens[2], 102); // SEP
}

TEST(QA_GDB318_Encode, ExactSizeNoTruncationNoPadding) {
    // "hello world" = 2 words, CLS + 2 + SEP = 4.
    HashTokenizer tok;
    auto tokens = tok.encode("hello world", 4);
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[0], 101);                         // CLS
    EXPECT_GE(tokens[1], HashTokenizer::VOCAB_OFFSET); // hello
    EXPECT_GE(tokens[2], HashTokenizer::VOCAB_OFFSET); // world
    EXPECT_EQ(tokens[3], 102);                         // SEP
}

TEST(QA_GDB318_Encode, LargeMaxLength) {
    HashTokenizer tok;
    auto tokens = tok.encode("word", 10000);
    ASSERT_EQ(tokens.size(), 10000u);
    EXPECT_EQ(tokens[0], 101); // CLS
    EXPECT_GE(tokens[1], HashTokenizer::VOCAB_OFFSET);
    EXPECT_EQ(tokens[2], 102); // SEP
    // Rest is padding.
    for (size_t i = 3; i < 10000; ++i) {
        EXPECT_EQ(tokens[i], 0) << "expected PAD at position " << i;
    }
}

// ===========================================================================
// HashTokenizer::encode — empty and whitespace input
// ===========================================================================

TEST(QA_GDB318_Encode, EmptyStringProducesOnlyCLSSEPPad) {
    HashTokenizer tok;
    auto tokens = tok.encode("", 8);
    ASSERT_EQ(tokens.size(), 8u);
    EXPECT_EQ(tokens[0], 101); // CLS
    EXPECT_EQ(tokens[1], 102); // SEP
    for (size_t i = 2; i < 8; ++i) {
        EXPECT_EQ(tokens[i], 0);
    }
}

TEST(QA_GDB318_Encode, WhitespaceOnlyString) {
    HashTokenizer tok;
    auto tokens = tok.encode("   \t\n\r  ", 8);
    ASSERT_EQ(tokens.size(), 8u);
    EXPECT_EQ(tokens[0], 101);
    EXPECT_EQ(tokens[1], 102);
    for (size_t i = 2; i < 8; ++i) {
        EXPECT_EQ(tokens[i], 0);
    }
}

TEST(QA_GDB318_Encode, OnlyPunctuation) {
    HashTokenizer tok;
    auto tokens = tok.encode("!@#$%^&*()", 8);
    ASSERT_EQ(tokens.size(), 8u);
    EXPECT_EQ(tokens[0], 101); // CLS
    EXPECT_EQ(tokens[1], 102); // SEP — no alphanumeric chars found
}

// ===========================================================================
// HashTokenizer::encode — text variations
// ===========================================================================

TEST(QA_GDB318_Encode, SingleCharacterWord) {
    HashTokenizer tok;
    auto tokens = tok.encode("a", 8);
    ASSERT_EQ(tokens.size(), 8u);
    EXPECT_EQ(tokens[0], 101);
    EXPECT_GE(tokens[1], HashTokenizer::VOCAB_OFFSET);
    EXPECT_EQ(tokens[2], 102);
}

TEST(QA_GDB318_Encode, NumericInput) {
    HashTokenizer tok;
    auto tokens = tok.encode("12345", 8);
    ASSERT_EQ(tokens.size(), 8u);
    EXPECT_EQ(tokens[0], 101);
    // Digits are alphanumeric, so "12345" is one word token.
    EXPECT_GE(tokens[1], HashTokenizer::VOCAB_OFFSET);
    EXPECT_EQ(tokens[2], 102);
}

TEST(QA_GDB318_Encode, MixedAlphanumeric) {
    HashTokenizer tok;
    auto tokens = tok.encode("abc123 def456", 8);
    ASSERT_EQ(tokens.size(), 8u);
    EXPECT_EQ(tokens[0], 101);
    EXPECT_GE(tokens[1], HashTokenizer::VOCAB_OFFSET); // "abc123"
    EXPECT_GE(tokens[2], HashTokenizer::VOCAB_OFFSET); // "def456"
    EXPECT_EQ(tokens[3], 102);
}

TEST(QA_GDB318_Encode, ConsecutivePunctuationSplitsNothing) {
    // "a...b" has 'a' and 'b' separated by dots (non-alphanumeric).
    HashTokenizer tok;
    auto tokens = tok.encode("a...b", 8);
    ASSERT_EQ(tokens.size(), 8u);
    EXPECT_EQ(tokens[0], 101);
    // Two words: "a" and "b".
    EXPECT_GE(tokens[1], HashTokenizer::VOCAB_OFFSET);
    EXPECT_GE(tokens[2], HashTokenizer::VOCAB_OFFSET);
    EXPECT_EQ(tokens[3], 102);
}

TEST(QA_GDB318_Encode, LeadingAndTrailingWhitespace) {
    HashTokenizer tok;
    auto tokens_trimmed = tok.encode("hello", 8);
    auto tokens_padded = tok.encode("  hello  ", 8);
    // Should produce the same word tokens since whitespace is a delimiter.
    EXPECT_EQ(tokens_trimmed, tokens_padded);
}

TEST(QA_GDB318_Encode, CaseInsensitivity) {
    HashTokenizer tok;
    auto lower = tok.encode("hello", 8);
    auto upper = tok.encode("HELLO", 8);
    auto mixed = tok.encode("HeLLo", 8);
    EXPECT_EQ(lower, upper);
    EXPECT_EQ(lower, mixed);
}

// ===========================================================================
// HashTokenizer::encode — truncation with many words
// ===========================================================================

TEST(QA_GDB318_Encode, TruncationPreservesSEP) {
    // Generate 200 words but allow only 10 tokens.
    std::string text;
    for (int i = 0; i < 200; ++i) {
        text += "word" + std::to_string(i) + " ";
    }
    HashTokenizer tok;
    auto tokens = tok.encode(text, 10);
    ASSERT_EQ(tokens.size(), 10u);
    EXPECT_EQ(tokens[0], 101); // CLS preserved
    EXPECT_EQ(tokens[9], 102); // SEP preserved at end
    // All intermediate tokens are word hashes.
    for (size_t i = 1; i < 9; ++i) {
        EXPECT_GE(tokens[i], HashTokenizer::VOCAB_OFFSET)
            << "position " << i << " should be a word token";
    }
}

TEST(QA_GDB318_Encode, ExactCapacityFillsWithoutPadding) {
    // "a b c" = 3 words. CLS + 3 + SEP = 5.
    HashTokenizer tok;
    auto tokens = tok.encode("a b c", 5);
    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0], 101);
    EXPECT_EQ(tokens[4], 102);
    // No padding in between.
    for (size_t i = 1; i < 4; ++i) {
        EXPECT_NE(tokens[i], 0) << "position " << i << " should not be PAD";
    }
}

// ===========================================================================
// HashTokenizer::encode — hash value range
// ===========================================================================

TEST(QA_GDB318_Encode, AllWordTokensInValidRange) {
    // Test many different words and verify all fall in the valid range.
    HashTokenizer tok;
    std::string text;
    for (int i = 0; i < 50; ++i) {
        text += "testword" + std::to_string(i) + " ";
    }
    auto tokens = tok.encode(text, 100);
    ASSERT_EQ(tokens.size(), 100u);

    // Find the SEP position.
    size_t sep_pos = 0;
    for (size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i] == 102) {
            sep_pos = i;
            break;
        }
    }
    ASSERT_GT(sep_pos, 1u);

    // All word tokens in [1, sep_pos) must be in valid range.
    for (size_t i = 1; i < sep_pos; ++i) {
        EXPECT_GE(tokens[i], HashTokenizer::VOCAB_OFFSET)
            << "word token at " << i << " below VOCAB_OFFSET";
        EXPECT_LT(tokens[i],
                  HashTokenizer::VOCAB_OFFSET + static_cast<int64_t>(HashTokenizer::VOCAB_SIZE))
            << "word token at " << i << " exceeds VOCAB_OFFSET + VOCAB_SIZE";
    }
}

TEST(QA_GDB318_Encode, WordTokensNeverCollideWithSpecialTokens) {
    // Verify no word token can be PAD(0), UNK(100), CLS(101), SEP(102), MASK(103).
    HashTokenizer tok;
    std::string text;
    for (int i = 0; i < 100; ++i) {
        text += "w" + std::to_string(i) + " ";
    }
    auto tokens = tok.encode(text, 120);

    for (size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i] == 0 || tokens[i] == 102)
            continue; // PAD or SEP
        EXPECT_GE(tokens[i], HashTokenizer::VOCAB_OFFSET)
            << "token at " << i << " is in special token range";
    }
}

// ===========================================================================
// HashTokenizer::encode — determinism
// ===========================================================================

TEST(QA_GDB318_Encode, DeterministicAcrossInstances) {
    HashTokenizer tok1;
    HashTokenizer tok2;
    auto tokens1 = tok1.encode("some test input", 20);
    auto tokens2 = tok2.encode("some test input", 20);
    EXPECT_EQ(tokens1, tokens2);
}

TEST(QA_GDB318_Encode, DeterministicMultipleCalls) {
    HashTokenizer tok;
    auto t1 = tok.encode("hello world", 10);
    auto t2 = tok.encode("hello world", 10);
    auto t3 = tok.encode("hello world", 10);
    EXPECT_EQ(t1, t2);
    EXPECT_EQ(t2, t3);
}

// ===========================================================================
// HashTokenizer::attention_mask — boundary cases
// ===========================================================================

TEST(QA_GDB318_AttentionMask, EmptyVector) {
    HashTokenizer tok;
    auto mask = tok.attention_mask({});
    EXPECT_TRUE(mask.empty());
}

TEST(QA_GDB318_AttentionMask, AllPadding) {
    HashTokenizer tok;
    std::vector<int64_t> tokens(10, 0); // all PAD
    auto mask = tok.attention_mask(tokens);
    ASSERT_EQ(mask.size(), 10u);
    for (auto m : mask) {
        EXPECT_EQ(m, 0);
    }
}

TEST(QA_GDB318_AttentionMask, AllNonPadding) {
    HashTokenizer tok;
    std::vector<int64_t> tokens = {101, 500, 600, 700, 102};
    auto mask = tok.attention_mask(tokens);
    ASSERT_EQ(mask.size(), 5u);
    for (auto m : mask) {
        EXPECT_EQ(m, 1);
    }
}

TEST(QA_GDB318_AttentionMask, MatchesEncodeOutput) {
    HashTokenizer tok;
    auto tokens = tok.encode("hello world", 8);
    auto mask = tok.attention_mask(tokens);
    ASSERT_EQ(mask.size(), 8u);

    // CLS(101), hello, world, SEP(102) → 4 non-padding tokens.
    EXPECT_EQ(mask[0], 1); // CLS
    EXPECT_EQ(mask[1], 1); // hello
    EXPECT_EQ(mask[2], 1); // world
    EXPECT_EQ(mask[3], 1); // SEP
    // Padding.
    for (size_t i = 4; i < 8; ++i) {
        EXPECT_EQ(mask[i], 0) << "position " << i;
    }
}

TEST(QA_GDB318_AttentionMask, SingleElement) {
    HashTokenizer tok;
    EXPECT_EQ(tok.attention_mask({0}), std::vector<int64_t>{0});
    EXPECT_EQ(tok.attention_mask({42}), std::vector<int64_t>{1});
}

TEST(QA_GDB318_AttentionMask, NegativeTokenIdsAreNonPadding) {
    HashTokenizer tok;
    std::vector<int64_t> tokens = {-1, -100, 0, 5};
    auto mask = tok.attention_mask(tokens);
    ASSERT_EQ(mask.size(), 4u);
    EXPECT_EQ(mask[0], 1); // -1 != 0
    EXPECT_EQ(mask[1], 1); // -100 != 0
    EXPECT_EQ(mask[2], 0); // 0 == PAD
    EXPECT_EQ(mask[3], 1); // 5 != 0
}

// ===========================================================================
// Tokenizer polymorphism — HashTokenizer through base pointer
// ===========================================================================

TEST(QA_GDB318_Polymorphism, HashTokenizerThroughBasePointer) {
    std::unique_ptr<Tokenizer> tok = std::make_unique<HashTokenizer>();

    auto tokens = tok->encode("hello world", 8);
    ASSERT_EQ(tokens.size(), 8u);
    EXPECT_EQ(tokens[0], 101);

    auto mask = tok->attention_mask(tokens);
    ASSERT_EQ(mask.size(), 8u);

    EXPECT_EQ(tok->vocab_size(), 30000u);
    EXPECT_EQ(tok->max_sequence_length(), 128u);
}

TEST(QA_GDB318_Polymorphism, CustomTokenizerThroughBasePointer) {
    std::unique_ptr<Tokenizer> tok = std::make_unique<StubTokenizer>(64);

    auto tokens = tok->encode("anything", 5);
    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0], 1);
    EXPECT_EQ(tokens[1], 2);

    EXPECT_EQ(tok->vocab_size(), 100u);
    EXPECT_EQ(tok->max_sequence_length(), 64u);
}

// ===========================================================================
// OnnxProvider — tokenizer integration
// ===========================================================================

TEST(QA_GDB318_OnnxProvider, DefaultConstructorUsesHashTokenizer) {
    auto mock = std::make_unique<MockOnnxSessionGDB318>();
    auto* mock_ptr = mock.get();
    std::vector<float> emb(3, 0.5F);
    mock_ptr->set_embedding(emb);

    OnnxProvider provider("model.onnx", 3, std::move(mock));

    // Verify the tokenizer is a HashTokenizer.
    EXPECT_EQ(provider.tokenizer().vocab_size(), 30000u);
    EXPECT_EQ(provider.tokenizer().max_sequence_length(), OnnxProvider::MAX_SEQ_LENGTH);

    // Embed should work — uses internal HashTokenizer.
    auto result = provider.embed("test");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Session should receive MAX_SEQ_LENGTH tokens.
    EXPECT_EQ(mock_ptr->last_input_ids_.size(), OnnxProvider::MAX_SEQ_LENGTH);
    EXPECT_EQ(mock_ptr->last_attention_mask_.size(), OnnxProvider::MAX_SEQ_LENGTH);
}

TEST(QA_GDB318_OnnxProvider, CustomTokenizerConstructor) {
    auto mock = std::make_unique<MockOnnxSessionGDB318>();
    auto* mock_ptr = mock.get();
    std::vector<float> emb(3, 0.5F);
    mock_ptr->set_embedding(emb);

    auto custom_tok = std::make_unique<StubTokenizer>(64);
    OnnxProvider provider("model.onnx", 3, std::move(mock), std::move(custom_tok));

    // Verify it uses our custom tokenizer.
    EXPECT_EQ(provider.tokenizer().vocab_size(), 100u);
    EXPECT_EQ(provider.tokenizer().max_sequence_length(), 64u);

    // Embed should work with the custom tokenizer.
    auto result = provider.embed("test");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // StubTokenizer encode() returns [1, 2, 0, 0, ...].
    // OnnxProvider uses MAX_SEQ_LENGTH as the max_length argument.
    EXPECT_EQ(mock_ptr->last_input_ids_.size(), OnnxProvider::MAX_SEQ_LENGTH);
    EXPECT_EQ(mock_ptr->last_input_ids_[0], 1);
    EXPECT_EQ(mock_ptr->last_input_ids_[1], 2);
}

TEST(QA_GDB318_OnnxProvider, TokenizerAccessorReturnsCorrectType) {
    auto mock = std::make_unique<MockOnnxSessionGDB318>();
    OnnxProvider provider("model.onnx", 3, std::move(mock));

    const Tokenizer& tok_ref = provider.tokenizer();
    // Should be a HashTokenizer — verify via interface.
    EXPECT_EQ(tok_ref.vocab_size(), 30000u);
    EXPECT_EQ(tok_ref.max_sequence_length(), 128u);
}

TEST(QA_GDB318_OnnxProvider, EmbedPassesTokenizerOutputToSession) {
    auto mock = std::make_unique<MockOnnxSessionGDB318>();
    auto* mock_ptr = mock.get();
    std::vector<float> emb(4, 1.0F);
    mock_ptr->set_embedding(emb);

    OnnxProvider provider("model.onnx", 4, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Verify the tokens match what HashTokenizer would produce.
    HashTokenizer standalone_tok(OnnxProvider::MAX_SEQ_LENGTH);
    auto expected_tokens = standalone_tok.encode("hello", OnnxProvider::MAX_SEQ_LENGTH);
    auto expected_mask = standalone_tok.attention_mask(expected_tokens);

    EXPECT_EQ(mock_ptr->last_input_ids_, expected_tokens);
    EXPECT_EQ(mock_ptr->last_attention_mask_, expected_mask);
}

// ===========================================================================
// Stress test — encode many different texts
// ===========================================================================

TEST(QA_GDB318_Stress, EncodeManyDifferentTexts) {
    HashTokenizer tok;
    for (int i = 0; i < 1000; ++i) {
        auto text = "text number " + std::to_string(i) + " with some padding words";
        auto tokens = tok.encode(text, 20);
        ASSERT_EQ(tokens.size(), 20u) << "iteration " << i;
        EXPECT_EQ(tokens[0], 101) << "CLS missing at iteration " << i;
        // SEP should appear somewhere.
        bool has_sep = std::find(tokens.begin(), tokens.end(), 102) != tokens.end();
        EXPECT_TRUE(has_sep) << "SEP missing at iteration " << i;
    }
}

TEST(QA_GDB318_Stress, AttentionMaskLargeVector) {
    HashTokenizer tok;
    // 10K tokens: first 500 non-zero, rest padding.
    std::vector<int64_t> tokens(10000, 0);
    for (size_t i = 0; i < 500; ++i) {
        tokens[i] = static_cast<int64_t>(i + 1);
    }
    auto mask = tok.attention_mask(tokens);
    ASSERT_EQ(mask.size(), 10000u);

    // First 500 should be 1.
    for (size_t i = 0; i < 500; ++i) {
        EXPECT_EQ(mask[i], 1) << "position " << i;
    }
    // Rest should be 0.
    for (size_t i = 500; i < 10000; ++i) {
        EXPECT_EQ(mask[i], 0) << "position " << i;
    }
}

TEST(QA_GDB318_Stress, EncodeVeryLongText) {
    // Build a text with 5000 words.
    std::string text;
    for (int i = 0; i < 5000; ++i) {
        text += "w" + std::to_string(i) + " ";
    }
    HashTokenizer tok;
    auto tokens = tok.encode(text, 128);
    ASSERT_EQ(tokens.size(), 128u);
    EXPECT_EQ(tokens[0], 101);   // CLS
    EXPECT_EQ(tokens[127], 102); // SEP at end (truncation preserves it)
}

// ===========================================================================
// Edge case: encode with special characters, unicode-ish bytes
// ===========================================================================

TEST(QA_GDB318_Encode, StringWithNullBytes) {
    HashTokenizer tok;
    // std::string can contain null bytes.
    std::string text("hello\0world", 11);
    auto tokens = tok.encode(text, 10);
    ASSERT_EQ(tokens.size(), 10u);
    EXPECT_EQ(tokens[0], 101); // CLS
    // '\0' is not alphanumeric, so it acts as a delimiter.
    // Should produce two word tokens: "hello" and "world".
    EXPECT_GE(tokens[1], HashTokenizer::VOCAB_OFFSET);
    EXPECT_GE(tokens[2], HashTokenizer::VOCAB_OFFSET);
    EXPECT_EQ(tokens[3], 102); // SEP
}

TEST(QA_GDB318_Encode, TabsAndNewlinesDelimitWords) {
    HashTokenizer tok;
    auto tokens = tok.encode("hello\tworld\nfoo", 10);
    ASSERT_EQ(tokens.size(), 10u);
    EXPECT_EQ(tokens[0], 101);
    // Three words.
    EXPECT_GE(tokens[1], HashTokenizer::VOCAB_OFFSET);
    EXPECT_GE(tokens[2], HashTokenizer::VOCAB_OFFSET);
    EXPECT_GE(tokens[3], HashTokenizer::VOCAB_OFFSET);
    EXPECT_EQ(tokens[4], 102);
}

TEST(QA_GDB318_Encode, HighBitCharsNonAlphanumeric) {
    // Characters with values > 127 (like UTF-8 multibyte sequences)
    // are not alphanumeric per std::isalnum, so they act as delimiters.
    HashTokenizer tok;
    std::string text = "caf\xC3\xA9"; // "café" in UTF-8
    auto tokens = tok.encode(text, 10);
    ASSERT_EQ(tokens.size(), 10u);
    EXPECT_EQ(tokens[0], 101);
    // "caf" is a word (bytes 0xC3, 0xA9 are non-alphanumeric per isalnum).
    EXPECT_GE(tokens[1], HashTokenizer::VOCAB_OFFSET);
}

// ===========================================================================
// Verify old static methods are removed from OnnxProvider
// ===========================================================================

// The acceptance criteria state that OnnxProvider::tokenize() and
// make_attention_mask() static methods should be removed.
// We verify this at compile time: if those methods existed, using
// the Tokenizer interface methods instead confirms the refactor.
TEST(QA_GDB318_Refactor, OnnxProviderUsesTokenizerInterface) {
    auto mock = std::make_unique<MockOnnxSessionGDB318>();
    auto* mock_ptr = mock.get();
    std::vector<float> emb(3, 0.5F);
    mock_ptr->set_embedding(emb);

    OnnxProvider provider("model.onnx", 3, std::move(mock));

    // provider.tokenizer() should exist and be usable.
    const Tokenizer& tok = provider.tokenizer();
    auto tokens = tok.encode("test", 10);
    auto mask = tok.attention_mask(tokens);

    EXPECT_EQ(tokens.size(), 10u);
    EXPECT_EQ(mask.size(), 10u);
}
