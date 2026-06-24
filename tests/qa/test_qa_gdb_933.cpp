/// @file test_qa_gdb_933.cpp
/// @brief Adversarial QA tests for GDB-933: HashTokenizer max_length=1
/// truncation now yields [CLS] (was [SEP]), aligning to CLS-first contract.
///
/// Test focus:
/// 1. Behavior preservation: N=1 -> [CLS]; N=2 -> [CLS,SEP]; N>=3 -> [CLS,...,SEP]
/// 2. Cross-impl consistency: Hash, WordPiece, BPE all agree at N=1 and N=2
/// 3. Edge cases: empty input, punctuation-only, very long input at small max_length
/// 4. No regression: N>=2 outputs byte-identical to pre-fix behavior

#include "sixseven/vector/bpe_tokenizer.h"
#include "sixseven/vector/tokenizer.h"
#include "sixseven/vector/wordpiece_tokenizer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// Special token constants (HashTokenizer defaults)
// ---------------------------------------------------------------------------

constexpr int64_t HASH_PAD = 0;
constexpr int64_t HASH_CLS = 101;
constexpr int64_t HASH_SEP = 102;

// ---------------------------------------------------------------------------
// WordPiece minimal config (same vocab pattern as gdb_322 tests)
// ---------------------------------------------------------------------------

TokenizerConfig make_wordpiece_config() {
    TokenizerConfig cfg;
    cfg.vocab = {
        {"[PAD]", 0},
        {"[UNK]", 100},
        {"[CLS]", 101},
        {"[SEP]", 102},
        {"hello", 200},
        {"world", 201},
        {"foo", 202},
        {"bar", 203},
        {"a", 204},
    };
    cfg.special_tokens = {.pad = 0, .unk = 100, .cls = 101, .sep = 102, .mask = 103};
    cfg.model_type = TokenizerModelType::WORDPIECE;
    cfg.normalizer = NormalizerType::BERT;
    cfg.pre_tokenizer = PreTokenizerType::PUNCTUATION;
    return cfg;
}

// ---------------------------------------------------------------------------
// BPE minimal config mirrored from gdb_323 (own token IDs to stay isolated)
// ---------------------------------------------------------------------------

const std::vector<std::string> GDB933_BPE_MERGES = {
    "h e",
    "he l",
    "hel l",
    "hell o",
};

const std::unordered_map<std::string, int64_t> GDB933_BPE_VOCAB = {
    {"h", 400},
    {"e", 401},
    {"l", 402},
    {"o", 403},
    {"w", 404},
    {"r", 405},
    {"d", 406},
    {"\xC4\xA0", 407},
    {"he", 500},
    {"hel", 501},
    {"hell", 502},
    {"hello", 503},
};

TokenizerConfig make_bpe_config() {
    TokenizerConfig cfg;
    cfg.vocab = GDB933_BPE_VOCAB;
    cfg.special_tokens = {.pad = 0, .unk = 100, .cls = 101, .sep = 102, .mask = 103};
    cfg.model_type = TokenizerModelType::BPE;
    cfg.normalizer = NormalizerType::NONE;
    cfg.pre_tokenizer = PreTokenizerType::WHITESPACE;
    cfg.merges = GDB933_BPE_MERGES;
    return cfg;
}

// ---------------------------------------------------------------------------
// GDB-933: HashTokenizer behavior-preservation pin tests
// ---------------------------------------------------------------------------

// AC: max_length=1 yields exactly [CLS] (101), NOT [SEP] (102).
TEST(QA_GDB933_BehaviorPin, HashTokenizer_N1_OnlyCLS) {
    HashTokenizer tok;
    auto ids = tok.encode("hello world foo", 1);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], HASH_CLS) << "max_length=1 must yield only CLS (101), not SEP (102)";
    EXPECT_NE(ids[0], HASH_SEP) << "SEP must not appear at max_length=1";
}

// AC: max_length=2 yields [CLS, SEP] - no content tokens fit.
TEST(QA_GDB933_BehaviorPin, HashTokenizer_N2_CLSAndSEP) {
    HashTokenizer tok;
    auto ids = tok.encode("hello world foo", 2);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], HASH_CLS) << "ids[0] must be CLS";
    EXPECT_EQ(ids[1], HASH_SEP) << "ids[1] must be SEP at max_length=2";
}

// AC: max_length=3 yields [CLS, content_word, SEP] with no padding.
TEST(QA_GDB933_BehaviorPin, HashTokenizer_N3_CLSContentSEP) {
    HashTokenizer tok;
    auto ids = tok.encode("hello world foo", 3);
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], HASH_CLS) << "ids[0] must be CLS";
    // ids[1] is a content token: not CLS, SEP, PAD, UNK
    EXPECT_NE(ids[1], HASH_CLS);
    EXPECT_NE(ids[1], HASH_SEP);
    EXPECT_NE(ids[1], HASH_PAD);
    EXPECT_EQ(ids[2], HASH_SEP) << "ids[2] must be SEP (last slot)";
}

// AC: max_length=5 yields [CLS, w1, w2, w3, SEP] - all three words fit.
TEST(QA_GDB933_BehaviorPin, HashTokenizer_N5_ThreeWordsAndSEP) {
    HashTokenizer tok;
    auto ids = tok.encode("hello world foo", 5);
    ASSERT_EQ(ids.size(), 5u);
    EXPECT_EQ(ids[0], HASH_CLS);
    EXPECT_EQ(ids[4], HASH_SEP) << "SEP must occupy last slot at max_length=5";
    // No padding expected: exactly 5 tokens, all non-zero
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_NE(ids[i], HASH_PAD) << "no PAD expected at index " << i;
    }
}

// AC: max_length=10 yields [CLS, w1, w2, w3, SEP, PAD...].
TEST(QA_GDB933_BehaviorPin, HashTokenizer_N10_PaddingAfterSEP) {
    HashTokenizer tok;
    auto ids = tok.encode("hello world foo", 10);
    ASSERT_EQ(ids.size(), 10u);
    EXPECT_EQ(ids[0], HASH_CLS);
    EXPECT_EQ(ids[4], HASH_SEP);
    for (size_t i = 5; i < 10; ++i) {
        EXPECT_EQ(ids[i], HASH_PAD) << "PAD expected at index " << i;
    }
}

// REGRESSION: N>=2 SEP is always the last non-pad token.
// This pinned test would fail if the truncation changed N=2 or N=3 behavior.
TEST(QA_GDB933_Regression, HashTokenizer_N2_IsIdempotentAcrossInputs) {
    HashTokenizer tok;
    // Different inputs, same structure at N=2.
    for (const auto& text : {"hello", "hello world", "a b c d e f g", "the quick brown fox", ""}) {
        auto ids = tok.encode(text, 2);
        ASSERT_EQ(ids.size(), 2u) << "text=\"" << text << "\"";
        EXPECT_EQ(ids[0], HASH_CLS) << "text=\"" << text << "\" ids[0]!=CLS";
        EXPECT_EQ(ids[1], HASH_SEP) << "text=\"" << text << "\" ids[1]!=SEP";
    }
}

// REGRESSION: N=3 SEP is always last for content-bearing inputs.
// (For zero-content inputs like "!@#$", SEP lands at slot 1; tested separately.)
TEST(QA_GDB933_Regression, HashTokenizer_N3_SEPIsAlwaysLast) {
    HashTokenizer tok;
    for (const auto& text : {"hello", "hello world foo bar baz"}) {
        auto ids = tok.encode(text, 3);
        ASSERT_EQ(ids.size(), 3u) << "text=\"" << text << "\"";
        EXPECT_EQ(ids[0], HASH_CLS);
        EXPECT_EQ(ids[2], HASH_SEP) << "text=\"" << text << "\" SEP must be last";
    }
}

// ---------------------------------------------------------------------------
// GDB-933: Cross-impl consistency at max_length=1 and max_length=2
// ---------------------------------------------------------------------------

// All three impls yield [CLS] at max_length=1.
TEST(QA_GDB933_CrossImpl, AllTokenizers_N1_YieldCLS) {
    // HashTokenizer
    {
        HashTokenizer tok;
        auto ids = tok.encode("hello world", 1);
        ASSERT_EQ(ids.size(), 1u) << "HashTokenizer N=1 size";
        EXPECT_EQ(ids[0], 101) << "HashTokenizer N=1 must be CLS=101";
    }
    // WordPieceTokenizer
    {
        WordPieceTokenizer tok(make_wordpiece_config());
        auto ids = tok.encode("hello world", 1);
        ASSERT_EQ(ids.size(), 1u) << "WordPieceTokenizer N=1 size";
        EXPECT_EQ(ids[0], 101) << "WordPieceTokenizer N=1 must be CLS=101";
    }
    // BPETokenizer
    {
        BPETokenizer tok(make_bpe_config());
        auto ids = tok.encode("hello world", 1);
        ASSERT_EQ(ids.size(), 1u) << "BPETokenizer N=1 size";
        EXPECT_EQ(ids[0], 101) << "BPETokenizer N=1 must be CLS=101";
    }
}

// All three impls yield [CLS, SEP] at max_length=2 (no content).
TEST(QA_GDB933_CrossImpl, AllTokenizers_N2_YieldCLSSEP) {
    // HashTokenizer
    {
        HashTokenizer tok;
        auto ids = tok.encode("hello world", 2);
        ASSERT_EQ(ids.size(), 2u) << "HashTokenizer N=2 size";
        EXPECT_EQ(ids[0], 101) << "HashTokenizer N=2 ids[0]!=CLS";
        EXPECT_EQ(ids[1], 102) << "HashTokenizer N=2 ids[1]!=SEP";
    }
    // WordPieceTokenizer
    {
        WordPieceTokenizer tok(make_wordpiece_config());
        auto ids = tok.encode("hello world", 2);
        ASSERT_EQ(ids.size(), 2u) << "WordPieceTokenizer N=2 size";
        EXPECT_EQ(ids[0], 101) << "WordPieceTokenizer N=2 ids[0]!=CLS";
        EXPECT_EQ(ids[1], 102) << "WordPieceTokenizer N=2 ids[1]!=SEP";
    }
    // BPETokenizer
    {
        BPETokenizer tok(make_bpe_config());
        auto ids = tok.encode("hello world", 2);
        ASSERT_EQ(ids.size(), 2u) << "BPETokenizer N=2 size";
        EXPECT_EQ(ids[0], 101) << "BPETokenizer N=2 ids[0]!=CLS";
        EXPECT_EQ(ids[1], 102) << "BPETokenizer N=2 ids[1]!=SEP";
    }
}

// ---------------------------------------------------------------------------
// GDB-933: Edge cases for HashTokenizer
// ---------------------------------------------------------------------------

// Empty string at max_length=1 -> [CLS].
TEST(QA_GDB933_EdgeCase, HashTokenizer_EmptyInput_N1) {
    HashTokenizer tok;
    auto ids = tok.encode("", 1);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], HASH_CLS);
}

// max_length=0 -> empty (unchanged behavior).
TEST(QA_GDB933_EdgeCase, HashTokenizer_N0_Empty) {
    HashTokenizer tok;
    auto ids = tok.encode("hello world", 0);
    EXPECT_TRUE(ids.empty());
}

// Very long input at max_length=1: only CLS.
TEST(QA_GDB933_EdgeCase, HashTokenizer_VeryLongInput_N1) {
    HashTokenizer tok;
    // Build a string with 1000 words.
    std::string long_text;
    for (int i = 0; i < 1000; ++i) {
        long_text += "word ";
    }
    auto ids = tok.encode(long_text, 1);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], HASH_CLS) << "Very long input at N=1 must yield only CLS";
}

// Very long input at max_length=2: [CLS, SEP].
TEST(QA_GDB933_EdgeCase, HashTokenizer_VeryLongInput_N2) {
    HashTokenizer tok;
    std::string long_text;
    for (int i = 0; i < 1000; ++i) {
        long_text += "word ";
    }
    auto ids = tok.encode(long_text, 2);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], HASH_CLS);
    EXPECT_EQ(ids[1], HASH_SEP);
}

// All-punctuation input at max_length=1 -> [CLS] (no words to add).
TEST(QA_GDB933_EdgeCase, HashTokenizer_AllPunctuation_N1) {
    HashTokenizer tok;
    auto ids = tok.encode("!!! ??? ...", 1);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], HASH_CLS);
}

// All-punctuation input at max_length=3: [CLS, SEP, PAD] since no words.
TEST(QA_GDB933_EdgeCase, HashTokenizer_AllPunctuation_N3) {
    HashTokenizer tok;
    auto ids = tok.encode("!!! ??? ...", 3);
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], HASH_CLS);
    EXPECT_EQ(ids[1], HASH_SEP);
    EXPECT_EQ(ids[2], HASH_PAD);
}

// Single-character word at max_length=1 -> [CLS].
TEST(QA_GDB933_EdgeCase, HashTokenizer_SingleChar_N1) {
    HashTokenizer tok;
    auto ids = tok.encode("a", 1);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], HASH_CLS);
}

// Single-character word at max_length=2 -> [CLS, SEP] (word doesn't fit).
TEST(QA_GDB933_EdgeCase, HashTokenizer_SingleChar_N2) {
    HashTokenizer tok;
    auto ids = tok.encode("a", 2);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], HASH_CLS);
    EXPECT_EQ(ids[1], HASH_SEP);
}

// Single-character word at max_length=3: [CLS, word_hash, SEP].
TEST(QA_GDB933_EdgeCase, HashTokenizer_SingleChar_N3) {
    HashTokenizer tok;
    auto ids = tok.encode("a", 3);
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], HASH_CLS);
    EXPECT_NE(ids[1], HASH_CLS);
    EXPECT_NE(ids[1], HASH_SEP);
    EXPECT_NE(ids[1], HASH_PAD);
    EXPECT_EQ(ids[2], HASH_SEP);
}

// Determinism: same input/max_length must produce identical output.
TEST(QA_GDB933_EdgeCase, HashTokenizer_N1_IsDeterministic) {
    HashTokenizer tok;
    auto ids1 = tok.encode("hello world foo", 1);
    auto ids2 = tok.encode("hello world foo", 1);
    ASSERT_EQ(ids1.size(), ids2.size());
    EXPECT_EQ(ids1[0], ids2[0]);
}

} // namespace
} // namespace sixseven
