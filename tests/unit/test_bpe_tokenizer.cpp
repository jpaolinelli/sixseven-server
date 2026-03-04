#include "giodb/vector/bpe_tokenizer.h"

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace giodb {
namespace {

// ---------------------------------------------------------------------------
// Test vocabulary and merge rules.
//
// Uses a small synthetic vocabulary with hand-traced merge sequences.
// The byte-to-unicode mapping converts printable ASCII to itself and
// the space byte (0x20) to U+0120 (two-byte UTF-8: \xC4\xA0).
//
// Merge rules are applied in priority order (rank 0 = highest priority).
// The algorithm at each step finds the adjacent pair with the lowest rank
// across the entire symbol list and merges ALL occurrences of that pair.
//
// Reference traces are documented inline for each test case.
// ---------------------------------------------------------------------------

// clang-format off

/// Merge rules for the test BPE model.
///
/// Each rule is "left right" — merge symbol `left` with symbol `right`.
/// Rules are listed in priority order (index = rank).
const std::vector<std::string> TEST_MERGES = {
    "h e",                        // rank 0
    "he l",                       // rank 1
    "hel l",                      // rank 2
    "hell o",                     // rank 3
    "\xC4\xA0 w",                 // rank 4  (merge space-byte-unicode + 'w')
    "\xC4\xA0w o",                // rank 5
    "\xC4\xA0wo r",               // rank 6
    "\xC4\xA0wor l",              // rank 7
    "\xC4\xA0worl d",             // rank 8
    "\xC4\xA0 hello",             // rank 9  (merge space-byte-unicode + 'hello')
    "\xC4\xA0 h",                 // rank 10
    "\xC4\xA0h e",                // rank 11
    "\xC4\xA0he l",               // rank 12
    "\xC4\xA0hel p",              // rank 13
};

/// Vocabulary: BPE symbol string -> token ID.
const std::unordered_map<std::string, int64_t> TEST_VOCAB = {
    // Single printable ASCII characters.
    {"h", 100}, {"e", 101}, {"l", 102}, {"o", 103},
    {"w", 104}, {"r", 105}, {"d", 106}, {"p", 107},
    {"a", 108}, {"t", 109}, {"n", 110}, {"i", 111},

    // Space byte mapped through byte-to-unicode -> U+0120.
    {"\xC4\xA0", 200},

    // Merged tokens from "hello" sequence.
    {"he", 300}, {"hel", 301}, {"hell", 302}, {"hello", 303},

    // Merged tokens for " world" (space-prefixed).
    {"\xC4\xA0w", 400},
    {"\xC4\xA0wo", 401},
    {"\xC4\xA0wor", 402},
    {"\xC4\xA0worl", 403},
    {"\xC4\xA0world", 404},

    // Merged token for " hello" (space-prefixed).
    {"\xC4\xA0hello", 405},

    // Merged tokens for " hel" / " help" sequences.
    {"\xC4\xA0h", 410},
    {"\xC4\xA0he", 411},
    {"\xC4\xA0hel", 412},
    {"\xC4\xA0help", 413},
};

// clang-format on

/// Special token IDs for the test model (GPT-2/RoBERTa style).
constexpr int64_t CLS = 0;
constexpr int64_t PAD = 1;
constexpr int64_t SEP = 2;
constexpr int64_t UNK = 3;

const size_t MAX_LEN = 8;

/// Create a TokenizerConfig for the test BPE model.
TokenizerConfig make_test_config() {
    TokenizerConfig config;
    config.vocab = TEST_VOCAB;
    config.special_tokens = {.pad = PAD, .unk = UNK, .cls = CLS, .sep = SEP, .mask = 4};
    config.model_type = TokenizerModelType::BPE;
    config.normalizer = NormalizerType::NONE;
    config.pre_tokenizer = PreTokenizerType::WHITESPACE;
    config.merges = TEST_MERGES;
    return config;
}

class BPETokenizerTest : public ::testing::Test {
protected:
    void SetUp() override { tokenizer_ = std::make_unique<BPETokenizer>(make_test_config()); }

    std::unique_ptr<BPETokenizer> tokenizer_;
};

// ---------------------------------------------------------------------------
// Core BPE merge algorithm tests
// ---------------------------------------------------------------------------

TEST_F(BPETokenizerTest, SingleWordFullMerge) {
    // "hello" -> bytes_to_unicode -> "hello"
    // ['h','e','l','l','o']
    //   rank 0: "h e" -> ['he','l','l','o']
    //   rank 1: "he l" -> ['hel','l','o']
    //   rank 2: "hel l" -> ['hell','o']
    //   rank 3: "hell o" -> ['hello']
    // vocab: hello=303
    // Result: [CLS=0, 303, SEP=2, PAD...]
    auto ids = tokenizer_->encode("hello", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {CLS, 303, SEP, PAD, PAD, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(BPETokenizerTest, TwoWordsWithSpacePrefix) {
    // "hello world":
    //   pre-tokenize: ["hello", "world"]
    //   word 0: "hello" -> 303 (see SingleWordFullMerge)
    //   word 1: " world" -> bytes_to_unicode -> "\xC4\xA0world"
    //     ['\xC4\xA0','w','o','r','l','d']
    //     rank 4: "\xC4\xA0 w" -> ['\xC4\xA0w','o','r','l','d']
    //     rank 5: "\xC4\xA0w o" -> ['\xC4\xA0wo','r','l','d']
    //     rank 6: "\xC4\xA0wo r" -> ['\xC4\xA0wor','l','d']
    //     rank 7: "\xC4\xA0wor l" -> ['\xC4\xA0worl','d']
    //     rank 8: "\xC4\xA0worl d" -> ['\xC4\xA0world']
    //     vocab: \xC4\xA0world=404
    // Result: [CLS=0, 303, 404, SEP=2, PAD...]
    auto ids = tokenizer_->encode("hello world", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {CLS, 303, 404, SEP, PAD, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(BPETokenizerTest, SpacePrefixMergeWithSameWord) {
    // "hello hello":
    //   word 0: "hello" -> 303
    //   word 1: " hello" -> bytes_to_unicode -> "\xC4\xA0hello"
    //     ['\xC4\xA0','h','e','l','l','o']
    //     rank 0: "h e" -> ['\xC4\xA0','he','l','l','o']
    //     rank 1: "he l" -> ['\xC4\xA0','hel','l','o']
    //     rank 2: "hel l" -> ['\xC4\xA0','hell','o']
    //     rank 3: "hell o" -> ['\xC4\xA0','hello']
    //     rank 9: "\xC4\xA0 hello" -> ['\xC4\xA0hello']
    //     vocab: \xC4\xA0hello=405
    // Result: [CLS=0, 303, 405, SEP=2, PAD...]
    auto ids = tokenizer_->encode("hello hello", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {CLS, 303, 405, SEP, PAD, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(BPETokenizerTest, PartialMerge) {
    // "help":
    //   ['h','e','l','p']
    //   rank 0: "h e" -> ['he','l','p']
    //   rank 1: "he l" -> ['hel','p']
    //   no more merges ("hel p" not in merge rules)
    //   vocab: hel=301, p=107
    // Result: [CLS=0, 301, 107, SEP=2, PAD...]
    auto ids = tokenizer_->encode("help", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {CLS, 301, 107, SEP, PAD, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(BPETokenizerTest, SpacePrefixPartialMerge) {
    // "hello help":
    //   word 0: "hello" -> 303
    //   word 1: " help" -> bytes_to_unicode -> "\xC4\xA0help"
    //     ['\xC4\xA0','h','e','l','p']
    //     rank 0: "h e" -> ['\xC4\xA0','he','l','p']
    //     rank 1: "he l" -> ['\xC4\xA0','hel','p']
    //     rank 10: "\xC4\xA0 h" would need 'h' but now it's 'hel'
    //       Actually pairs are: (\xC4\xA0, hel), (hel, p)
    //       Neither matches any merge rule -> done
    //     Wait: let me retrace. After "he l" merge at rank 1:
    //       ['\xC4\xA0','hel','p']
    //       Pairs: ("\xC4\xA0 hel"), ("hel p")
    //       "\xC4\xA0 hel" is not in merges, "hel p" is not in merges -> done
    //     vocab: \xC4\xA0=200, hel=301, p=107
    //
    //   Hmm, but there's rank 10 "\xC4\xA0 h" and rank 11 "\xC4\xA0h e", etc.
    //   Let me retrace more carefully:
    //     ['\xC4\xA0','h','e','l','p']
    //     Pairs: ("\xC4\xA0 h")r10, ("h e")r0, ("e l")-, ("l p")-
    //     Best: "h e" rank 0 -> ['\xC4\xA0','he','l','p']
    //     Pairs: ("\xC4\xA0 he")-, ("he l")r1, ("l p")-
    //     Best: "he l" rank 1 -> ['\xC4\xA0','hel','p']
    //     Pairs: ("\xC4\xA0 hel")-, ("hel p")-
    //     No match -> done
    //     vocab: \xC4\xA0=200, hel=301, p=107
    //   Result: [CLS=0, 303, 200, 301, 107, SEP=2, PAD, PAD]
    auto ids = tokenizer_->encode("hello help", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {CLS, 303, 200, 301, 107, SEP, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(BPETokenizerTest, NoMergesApply) {
    // "at" -> ['a','t']
    //   No merge rule for "a t" -> stays as ['a','t']
    //   vocab: a=108, t=109
    // Result: [CLS=0, 108, 109, SEP=2, PAD...]
    auto ids = tokenizer_->encode("at", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {CLS, 108, 109, SEP, PAD, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(BPETokenizerTest, SingleCharacter) {
    // "a" -> ['a'] -> single symbol, no merges possible
    // vocab: a=108
    auto ids = tokenizer_->encode("a", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {CLS, 108, SEP, PAD, PAD, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

// ---------------------------------------------------------------------------
// Byte-level encoding tests
// ---------------------------------------------------------------------------

TEST_F(BPETokenizerTest, ByteLevelSpaceEncoding) {
    // Verify that the space byte (0x20) maps to U+0120 through
    // byte-to-unicode, producing the GPT-2 space-prefix convention.
    // "a a" -> pre-tokenize: ["a", "a"]
    //   word 0: "a" -> byte-encode -> "a" -> vocab: a=108
    //   word 1: " a" -> byte-encode -> "\xC4\xA0a"
    //     ['\xC4\xA0','a'] -> no merges -> vocab: \xC4\xA0=200, a=108
    // Result: [CLS=0, 108, 200, 108, SEP=2, PAD...]
    auto ids = tokenizer_->encode("a a", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {CLS, 108, 200, 108, SEP, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(BPETokenizerTest, UnknownCharacterProducesUnk) {
    // 'z' is not in the test vocabulary.
    // "z" -> byte-encode -> "z" -> ['z'] -> vocab miss -> UNK
    auto ids = tokenizer_->encode("z", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {CLS, UNK, SEP, PAD, PAD, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(BPETokenizerTest, AllUnknownCharacters) {
    // "zzz" -> ['z','z','z'] -> no merges -> each is UNK
    // Unlike WordPiece (which returns single UNK per word), BPE returns
    // UNK for each individual unknown subword.
    auto ids = tokenizer_->encode("zzz", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {CLS, UNK, UNK, UNK, SEP, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

// ---------------------------------------------------------------------------
// Special token and padding tests
// ---------------------------------------------------------------------------

TEST_F(BPETokenizerTest, EmptyInput) {
    auto ids = tokenizer_->encode("", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {CLS, SEP, PAD, PAD, PAD, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(BPETokenizerTest, MaxLengthZero) {
    auto ids = tokenizer_->encode("hello", 0);
    EXPECT_TRUE(ids.empty());
}

TEST_F(BPETokenizerTest, MaxLengthOne) {
    // Only room for CLS.
    auto ids = tokenizer_->encode("hello", 1);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], CLS);
}

TEST_F(BPETokenizerTest, MaxLengthTwo) {
    // Room for CLS + SEP only, no content.
    auto ids = tokenizer_->encode("hello", 2);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], SEP);
}

TEST_F(BPETokenizerTest, Truncation) {
    // "hello world" produces 2 content tokens (303, 404).
    // With max_length=3, only 1 content token fits: CLS + 1 content + SEP.
    auto ids = tokenizer_->encode("hello world", 3);
    ASSERT_EQ(ids.size(), 3u);
    std::vector<int64_t> expected = {CLS, 303, SEP};
    EXPECT_EQ(ids, expected);
}

// ---------------------------------------------------------------------------
// Attention mask tests
// ---------------------------------------------------------------------------

TEST_F(BPETokenizerTest, AttentionMask) {
    auto ids = tokenizer_->encode("hello", MAX_LEN);
    auto mask = tokenizer_->attention_mask(ids);
    ASSERT_EQ(mask.size(), MAX_LEN);
    // CLS, hello, SEP -> 1s; rest -> 0s
    std::vector<int64_t> expected = {1, 1, 1, 0, 0, 0, 0, 0};
    EXPECT_EQ(mask, expected);
}

TEST_F(BPETokenizerTest, AttentionMaskTwoWords) {
    auto ids = tokenizer_->encode("hello world", MAX_LEN);
    auto mask = tokenizer_->attention_mask(ids);
    ASSERT_EQ(mask.size(), MAX_LEN);
    // CLS, hello, world, SEP -> 1s; rest -> 0s
    std::vector<int64_t> expected = {1, 1, 1, 1, 0, 0, 0, 0};
    EXPECT_EQ(mask, expected);
}

// ---------------------------------------------------------------------------
// Interface tests
// ---------------------------------------------------------------------------

TEST_F(BPETokenizerTest, VocabSize) {
    EXPECT_EQ(tokenizer_->vocab_size(), TEST_VOCAB.size());
}

TEST_F(BPETokenizerTest, MaxSequenceLength) {
    EXPECT_EQ(tokenizer_->max_sequence_length(), 1024u);
}

// ---------------------------------------------------------------------------
// Multiple merge occurrences in one word
// ---------------------------------------------------------------------------

TEST_F(BPETokenizerTest, DuplicatePairsMergeSimultaneously) {
    // "ll" (two 'l' chars) should not merge because "l l" is not in our
    // merge rules. Each 'l' stays as a separate token.
    auto ids = tokenizer_->encode("ll", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {CLS, 102, 102, SEP, PAD, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(BPETokenizerTest, ThreeWords) {
    // "hello world hello":
    //   word 0: "hello" -> 303
    //   word 1: " world" -> 404
    //   word 2: " hello" -> 405
    // Result: [CLS, 303, 404, 405, SEP, PAD, PAD, PAD]
    auto ids = tokenizer_->encode("hello world hello", MAX_LEN);
    ASSERT_EQ(ids.size(), MAX_LEN);
    std::vector<int64_t> expected = {CLS, 303, 404, 405, SEP, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

} // namespace
} // namespace giodb
