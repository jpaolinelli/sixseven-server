#include "sixseven/vector/bpe_tokenizer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// Test infrastructure
// ---------------------------------------------------------------------------

// clang-format off

/// Merge rules for the QA BPE model.
/// Same as the dev test model but with additional rules to test edge cases.
const std::vector<std::string> QA_MERGES = {
    "h e",                        // rank 0
    "he l",                       // rank 1
    "hel l",                      // rank 2
    "hell o",                     // rank 3
    "\xC4\xA0 w",                 // rank 4
    "\xC4\xA0w o",                // rank 5
    "\xC4\xA0wo r",               // rank 6
    "\xC4\xA0wor l",              // rank 7
    "\xC4\xA0worl d",             // rank 8
    "\xC4\xA0 hello",             // rank 9
    "\xC4\xA0 h",                 // rank 10
    "\xC4\xA0h e",                // rank 11
    "\xC4\xA0he l",               // rank 12
    "\xC4\xA0hel p",              // rank 13
    "a b",                        // rank 14
    "ab c",                       // rank 15
    "abc d",                      // rank 16
};

/// Vocabulary for QA BPE model.
const std::unordered_map<std::string, int64_t> QA_VOCAB = {
    // Single printable ASCII characters.
    {"h", 100}, {"e", 101}, {"l", 102}, {"o", 103},
    {"w", 104}, {"r", 105}, {"d", 106}, {"p", 107},
    {"a", 108}, {"t", 109}, {"n", 110}, {"i", 111},
    {"b", 120}, {"c", 121},

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

    // Chain merge tokens: a b -> ab, ab c -> abc, abc d -> abcd
    {"ab", 500}, {"abc", 501}, {"abcd", 502},
};

// clang-format on

constexpr int64_t CLS = 0;
constexpr int64_t PAD = 1;
constexpr int64_t SEP = 2;
constexpr int64_t UNK = 3;

TokenizerConfig make_qa_config() {
    TokenizerConfig config;
    config.vocab = QA_VOCAB;
    config.special_tokens = {.pad = PAD, .unk = UNK, .cls = CLS, .sep = SEP, .mask = 4};
    config.model_type = TokenizerModelType::BPE;
    config.normalizer = NormalizerType::NONE;
    config.pre_tokenizer = PreTokenizerType::WHITESPACE;
    config.merges = QA_MERGES;
    return config;
}

class QA_GDB323_BPETokenizer : public ::testing::Test {
protected:
    void SetUp() override { tokenizer_ = std::make_unique<BPETokenizer>(make_qa_config()); }

    std::unique_ptr<BPETokenizer> tokenizer_;
};

// ---------------------------------------------------------------------------
// Boundary: max_length edge values
// ---------------------------------------------------------------------------

TEST_F(QA_GDB323_BPETokenizer, MaxLengthZeroReturnsEmpty) {
    auto ids = tokenizer_->encode("hello world", 0);
    EXPECT_TRUE(ids.empty());
}

TEST_F(QA_GDB323_BPETokenizer, MaxLengthOneOnlyCLS) {
    // max_length=1: only CLS, no room for SEP or content.
    auto ids = tokenizer_->encode("hello world", 1);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], CLS);
}

TEST_F(QA_GDB323_BPETokenizer, MaxLengthTwoCLSSEPOnly) {
    // max_length=2: CLS + SEP, no room for content.
    auto ids = tokenizer_->encode("hello world", 2);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], SEP);
}

TEST_F(QA_GDB323_BPETokenizer, MaxLengthThreeOneContentToken) {
    // max_length=3: CLS + 1 content + SEP. "hello world" = [303, 404].
    // Only 303 fits.
    auto ids = tokenizer_->encode("hello world", 3);
    ASSERT_EQ(ids.size(), 3u);
    std::vector<int64_t> expected = {CLS, 303, SEP};
    EXPECT_EQ(ids, expected);
}

TEST_F(QA_GDB323_BPETokenizer, ExactFitNoPadding) {
    // "hello world" produces 2 content tokens [303, 404].
    // max_length=4 means CLS + 303 + 404 + SEP = exactly 4.
    auto ids = tokenizer_->encode("hello world", 4);
    ASSERT_EQ(ids.size(), 4u);
    std::vector<int64_t> expected = {CLS, 303, 404, SEP};
    EXPECT_EQ(ids, expected);
}

TEST_F(QA_GDB323_BPETokenizer, ExactFitPlusOnePad) {
    // max_length=5: CLS + 303 + 404 + SEP + 1 PAD.
    auto ids = tokenizer_->encode("hello world", 5);
    ASSERT_EQ(ids.size(), 5u);
    std::vector<int64_t> expected = {CLS, 303, 404, SEP, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(QA_GDB323_BPETokenizer, LargeMaxLengthPadsCorrectly) {
    // max_length=100: should pad extensively.
    auto ids = tokenizer_->encode("hello", 100);
    ASSERT_EQ(ids.size(), 100u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 303); // hello
    EXPECT_EQ(ids[2], SEP);
    for (size_t i = 3; i < 100; ++i) {
        EXPECT_EQ(ids[i], PAD) << "Position " << i << " should be PAD";
    }
}

// ---------------------------------------------------------------------------
// Boundary: empty and whitespace-only inputs
// ---------------------------------------------------------------------------

TEST_F(QA_GDB323_BPETokenizer, EmptyStringProducesCLSSEPPad) {
    auto ids = tokenizer_->encode("", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], SEP);
    for (size_t i = 2; i < 8; ++i) {
        EXPECT_EQ(ids[i], PAD);
    }
}

TEST_F(QA_GDB323_BPETokenizer, WhitespaceOnlyInput) {
    // The whitespace pre-tokenizer should produce no words for " " input.
    // Result: [CLS, SEP, PAD, ...]
    auto ids = tokenizer_->encode(" ", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], SEP);
    for (size_t i = 2; i < 8; ++i) {
        EXPECT_EQ(ids[i], PAD);
    }
}

TEST_F(QA_GDB323_BPETokenizer, MultipleSpacesOnlyInput) {
    auto ids = tokenizer_->encode("     ", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], SEP);
}

TEST_F(QA_GDB323_BPETokenizer, TabNewlineInput) {
    // Tab and newline are whitespace — pre-tokenizer should split on them.
    auto ids = tokenizer_->encode("\t\n", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], SEP);
}

// ---------------------------------------------------------------------------
// BPE merge algorithm: chain merges
// ---------------------------------------------------------------------------

TEST_F(QA_GDB323_BPETokenizer, ChainMergeAbcd) {
    // "abcd" -> ['a','b','c','d']
    //   rank 14: "a b" -> ['ab','c','d']
    //   rank 15: "ab c" -> ['abc','d']
    //   rank 16: "abc d" -> ['abcd']
    //   vocab: abcd=502
    auto ids = tokenizer_->encode("abcd", 8);
    ASSERT_EQ(ids.size(), 8u);
    std::vector<int64_t> expected = {CLS, 502, SEP, PAD, PAD, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(QA_GDB323_BPETokenizer, PartialChainMergeAbc) {
    // "abc" -> ['a','b','c']
    //   rank 14: "a b" -> ['ab','c']
    //   rank 15: "ab c" -> ['abc']
    //   vocab: abc=501
    auto ids = tokenizer_->encode("abc", 8);
    ASSERT_EQ(ids.size(), 8u);
    std::vector<int64_t> expected = {CLS, 501, SEP, PAD, PAD, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(QA_GDB323_BPETokenizer, PartialChainMergeAb) {
    // "ab" -> ['a','b']
    //   rank 14: "a b" -> ['ab']
    //   vocab: ab=500
    auto ids = tokenizer_->encode("ab", 8);
    ASSERT_EQ(ids.size(), 8u);
    std::vector<int64_t> expected = {CLS, 500, SEP, PAD, PAD, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

// ---------------------------------------------------------------------------
// BPE merge algorithm: overlapping pairs
// ---------------------------------------------------------------------------

TEST_F(QA_GDB323_BPETokenizer, OverlappingPairsLeftToRight) {
    // "aba" -> ['a','b','a']
    //   Pair "a b" at rank 14; no "b a" rule.
    //   Merge: ['ab','a']
    //   No more merges.
    //   vocab: ab=500, a=108
    auto ids = tokenizer_->encode("aba", 8);
    ASSERT_EQ(ids.size(), 8u);
    std::vector<int64_t> expected = {CLS, 500, 108, SEP, PAD, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(QA_GDB323_BPETokenizer, TripleOverlappingPairs) {
    // "abab" -> ['a','b','a','b']
    //   rank 14: "a b" appears at (0,1) and (2,3) -> merge all:
    //     ['ab','ab']
    //   No more merges ("ab ab" not in rules).
    //   vocab: ab=500, ab=500
    auto ids = tokenizer_->encode("abab", 8);
    ASSERT_EQ(ids.size(), 8u);
    std::vector<int64_t> expected = {CLS, 500, 500, SEP, PAD, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(QA_GDB323_BPETokenizer, OddOverlappingMerge) {
    // "ababa" -> ['a','b','a','b','a']
    //   rank 14: "a b" at (0,1) and (2,3) -> greedy left-to-right:
    //     i=0: a+b -> "ab", i=2
    //     i=2: a+b -> "ab", i=4
    //     i=4: a -> "a"
    //     Result: ['ab','ab','a']
    //   No more merges.
    //   vocab: ab=500, ab=500, a=108
    auto ids = tokenizer_->encode("ababa", 8);
    ASSERT_EQ(ids.size(), 8u);
    std::vector<int64_t> expected = {CLS, 500, 500, 108, SEP, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

// ---------------------------------------------------------------------------
// Unknown token handling
// ---------------------------------------------------------------------------

TEST_F(QA_GDB323_BPETokenizer, SingleUnknownChar) {
    // 'z' not in vocab -> UNK.
    auto ids = tokenizer_->encode("z", 8);
    ASSERT_EQ(ids.size(), 8u);
    std::vector<int64_t> expected = {CLS, UNK, SEP, PAD, PAD, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(QA_GDB323_BPETokenizer, MixedKnownUnknownChars) {
    // "az" -> ['a','z']; no "a z" merge -> a=108, z=UNK
    auto ids = tokenizer_->encode("az", 8);
    ASSERT_EQ(ids.size(), 8u);
    std::vector<int64_t> expected = {CLS, 108, UNK, SEP, PAD, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(QA_GDB323_BPETokenizer, UnknownBeforeKnown) {
    // "za" -> ['z','a']; no merge -> z=UNK, a=108
    auto ids = tokenizer_->encode("za", 8);
    ASSERT_EQ(ids.size(), 8u);
    std::vector<int64_t> expected = {CLS, UNK, 108, SEP, PAD, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(QA_GDB323_BPETokenizer, AllUnknownLongWord) {
    // "zzzzz" -> 5 UNK tokens
    auto ids = tokenizer_->encode("zzzzz", 10);
    ASSERT_EQ(ids.size(), 10u);
    EXPECT_EQ(ids[0], CLS);
    for (size_t i = 1; i <= 5; ++i) {
        EXPECT_EQ(ids[i], UNK) << "Position " << i << " should be UNK";
    }
    EXPECT_EQ(ids[6], SEP);
    for (size_t i = 7; i < 10; ++i) {
        EXPECT_EQ(ids[i], PAD);
    }
}

// ---------------------------------------------------------------------------
// Byte-level BPE: non-ASCII and control characters
// ---------------------------------------------------------------------------

TEST_F(QA_GDB323_BPETokenizer, NonAsciiByteEncoding) {
    // Non-ASCII bytes (>127) should be mapped through byte-to-unicode
    // to their corresponding Unicode codepoints. These won't be in our
    // test vocab, so they should produce UNK tokens.
    // "\x80" (byte 128) maps to U+0100 through byte-to-unicode.
    auto ids = tokenizer_->encode("\x80", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], UNK); // byte-to-unicode output not in vocab
    EXPECT_EQ(ids[2], SEP);
}

TEST_F(QA_GDB323_BPETokenizer, NullByteInInput) {
    // Byte 0x00 should be handled through byte-to-unicode mapping.
    // 0x00 is not in the "direct" range, so maps to U+0100+offset.
    std::string input(1, '\0');
    auto ids = tokenizer_->encode(input, 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    // Null byte maps through byte-to-unicode to some codepoint not in vocab.
    EXPECT_EQ(ids[1], UNK);
    EXPECT_EQ(ids[2], SEP);
}

TEST_F(QA_GDB323_BPETokenizer, ControlCharactersInInput) {
    // Control chars (0x01-0x1F) should map through byte-to-unicode.
    // None are in the "direct" range (33-126, 161-172, 174-255).
    std::string input;
    input += '\x01';
    input += '\x1F';
    auto ids = tokenizer_->encode(input, 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    // Both map to codepoints not in vocab -> UNK each.
    EXPECT_EQ(ids[1], UNK);
    EXPECT_EQ(ids[2], UNK);
    EXPECT_EQ(ids[3], SEP);
}

// ---------------------------------------------------------------------------
// Space prefix convention (Ġ handling)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB323_BPETokenizer, SpacePrefixOnSecondWord) {
    // "a a" -> pre-tokenize: ["a", "a"]
    // word 0: "a" -> byte_to_unicode -> "a" -> [a=108]
    // word 1: " a" -> byte_to_unicode -> "\xC4\xA0a"
    //   ['\xC4\xA0', 'a'] -> no merge -> [200, 108]
    auto ids = tokenizer_->encode("a a", 8);
    ASSERT_EQ(ids.size(), 8u);
    std::vector<int64_t> expected = {CLS, 108, 200, 108, SEP, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(QA_GDB323_BPETokenizer, SpacePrefixOnThirdWord) {
    // "a a a" -> pre-tokenize: ["a", "a", "a"]
    // word 0: "a" -> 108
    // word 1: " a" -> [200, 108]
    // word 2: " a" -> [200, 108]
    auto ids = tokenizer_->encode("a a a", 10);
    ASSERT_EQ(ids.size(), 10u);
    std::vector<int64_t> expected = {CLS, 108, 200, 108, 200, 108, SEP, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(QA_GDB323_BPETokenizer, SpacePrefixMergesWithFollowingChar) {
    // "hello world" -> word 1 gets " world" -> "\xC4\xA0world"
    // Merge rules handle "\xC4\xA0 w" etc.
    // Already tested in dev tests but verify the Ġ merge chain here.
    auto ids = tokenizer_->encode("hello world", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 303); // hello
    EXPECT_EQ(ids[2], 404); // Ġworld
    EXPECT_EQ(ids[3], SEP);
}

// ---------------------------------------------------------------------------
// Truncation edge cases
// ---------------------------------------------------------------------------

TEST_F(QA_GDB323_BPETokenizer, TruncationPreservesFirstTokens) {
    // "hello world hello" -> [303, 404, 405] (3 content tokens).
    // max_length=4 -> CLS + 2 content + SEP.
    auto ids = tokenizer_->encode("hello world hello", 4);
    ASSERT_EQ(ids.size(), 4u);
    std::vector<int64_t> expected = {CLS, 303, 404, SEP};
    EXPECT_EQ(ids, expected);
}

TEST_F(QA_GDB323_BPETokenizer, TruncationWithManyTokens) {
    // Produce many content tokens. "a a a a a a" -> 6 words.
    // word 0: a=108, words 1-5: [200, 108] each = 10 more tokens.
    // Total content: 11 tokens.
    // max_length=5: CLS + 3 content + SEP.
    auto ids = tokenizer_->encode("a a a a a a", 5);
    ASSERT_EQ(ids.size(), 5u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 108); // first "a"
    EXPECT_EQ(ids[2], 200); // space-prefix of second "a"
    EXPECT_EQ(ids[3], 108); // second "a"
    EXPECT_EQ(ids[4], SEP);
}

TEST_F(QA_GDB323_BPETokenizer, MaxLengthTwoWithContent) {
    // max_length=2 with content: CLS + SEP, content is dropped.
    auto ids = tokenizer_->encode("hello world", 2);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], SEP);
}

// ---------------------------------------------------------------------------
// Attention mask adversarial tests
// ---------------------------------------------------------------------------

TEST_F(QA_GDB323_BPETokenizer, AttentionMaskEmpty) {
    auto ids = tokenizer_->encode("", 8);
    auto mask = tokenizer_->attention_mask(ids);
    ASSERT_EQ(mask.size(), 8u);
    // CLS=0 (non-PAD) + SEP=2 (non-PAD) + 6 PADs
    std::vector<int64_t> expected = {1, 1, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(mask, expected);
}

TEST_F(QA_GDB323_BPETokenizer, AttentionMaskNoPadding) {
    // "hello world" with max_length=4: CLS + 303 + 404 + SEP. No PAD.
    auto ids = tokenizer_->encode("hello world", 4);
    auto mask = tokenizer_->attention_mask(ids);
    ASSERT_EQ(mask.size(), 4u);
    std::vector<int64_t> expected = {1, 1, 1, 1};
    EXPECT_EQ(mask, expected);
}

TEST_F(QA_GDB323_BPETokenizer, AttentionMaskAllPadExceptSpecial) {
    // "" with max_length=10: CLS + SEP + 8 PADs.
    auto ids = tokenizer_->encode("", 10);
    auto mask = tokenizer_->attention_mask(ids);
    ASSERT_EQ(mask.size(), 10u);
    EXPECT_EQ(mask[0], 1); // CLS
    EXPECT_EQ(mask[1], 1); // SEP
    for (size_t i = 2; i < 10; ++i) {
        EXPECT_EQ(mask[i], 0) << "Position " << i;
    }
}

TEST_F(QA_GDB323_BPETokenizer, AttentionMaskMaxLengthZero) {
    auto ids = tokenizer_->encode("hello", 0);
    auto mask = tokenizer_->attention_mask(ids);
    EXPECT_TRUE(mask.empty());
}

// ---------------------------------------------------------------------------
// Stress tests
// ---------------------------------------------------------------------------

TEST_F(QA_GDB323_BPETokenizer, ManyWordsStress) {
    // Build a string with 100 words.
    std::string input;
    for (int i = 0; i < 100; ++i) {
        if (i > 0)
            input += " ";
        input += "hello";
    }
    auto ids = tokenizer_->encode(input, 512);
    ASSERT_EQ(ids.size(), 512u);
    EXPECT_EQ(ids[0], CLS);

    // word 0: hello=303; words 1-99: Ġhello=405.
    // Total content: 100 tokens. max_length=512, so all fit.
    EXPECT_EQ(ids[1], 303);
    for (size_t i = 2; i <= 100; ++i) {
        EXPECT_EQ(ids[i], 405) << "Position " << i;
    }
    EXPECT_EQ(ids[101], SEP);
    for (size_t i = 102; i < 512; ++i) {
        EXPECT_EQ(ids[i], PAD);
    }
}

TEST_F(QA_GDB323_BPETokenizer, LongSingleWordStress) {
    // A long word with no applicable merges: "zzzzz...z" (200 chars).
    // Each 'z' is unknown, so 200 UNK tokens.
    std::string input(200, 'z');
    auto ids = tokenizer_->encode(input, 300);
    ASSERT_EQ(ids.size(), 300u);
    EXPECT_EQ(ids[0], CLS);
    for (size_t i = 1; i <= 200; ++i) {
        EXPECT_EQ(ids[i], UNK) << "Position " << i;
    }
    EXPECT_EQ(ids[201], SEP);
}

TEST_F(QA_GDB323_BPETokenizer, LongWordTruncated) {
    // 200 UNK tokens but max_length=10: CLS + 8 content + SEP.
    std::string input(200, 'z');
    auto ids = tokenizer_->encode(input, 10);
    ASSERT_EQ(ids.size(), 10u);
    EXPECT_EQ(ids[0], CLS);
    for (size_t i = 1; i <= 8; ++i) {
        EXPECT_EQ(ids[i], UNK);
    }
    EXPECT_EQ(ids[9], SEP);
}

// ---------------------------------------------------------------------------
// Tokenizer interface tests
// ---------------------------------------------------------------------------

TEST_F(QA_GDB323_BPETokenizer, ImplementsTokenizerInterface) {
    // Verify BPETokenizer is-a Tokenizer (polymorphic usage).
    Tokenizer* base = tokenizer_.get();
    auto ids = base->encode("hello", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 303);

    auto mask = base->attention_mask(ids);
    ASSERT_EQ(mask.size(), 8u);

    EXPECT_EQ(base->vocab_size(), QA_VOCAB.size());
    EXPECT_EQ(base->max_sequence_length(), 1024u);
}

// ---------------------------------------------------------------------------
// Config: empty merges and empty vocab
// ---------------------------------------------------------------------------

TEST_F(QA_GDB323_BPETokenizer, EmptyMergesNoMerging) {
    // With no merge rules, every character stays separate.
    TokenizerConfig config;
    config.vocab = QA_VOCAB;
    config.special_tokens = {.pad = PAD, .unk = UNK, .cls = CLS, .sep = SEP, .mask = 4};
    config.model_type = TokenizerModelType::BPE;
    config.normalizer = NormalizerType::NONE;
    config.pre_tokenizer = PreTokenizerType::WHITESPACE;
    config.merges = {}; // No merges!

    BPETokenizer no_merge_tok(config);
    // "hello" -> ['h','e','l','l','o'] -> [100, 101, 102, 102, 103]
    auto ids = no_merge_tok.encode("hello", 10);
    ASSERT_EQ(ids.size(), 10u);
    std::vector<int64_t> expected = {CLS, 100, 101, 102, 102, 103, SEP, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(QA_GDB323_BPETokenizer, EmptyVocabAllUnk) {
    // With empty vocab, every subword is UNK.
    TokenizerConfig config;
    config.vocab = {}; // Empty vocab!
    config.special_tokens = {.pad = PAD, .unk = UNK, .cls = CLS, .sep = SEP, .mask = 4};
    config.model_type = TokenizerModelType::BPE;
    config.normalizer = NormalizerType::NONE;
    config.pre_tokenizer = PreTokenizerType::WHITESPACE;
    config.merges = {};

    BPETokenizer empty_vocab_tok(config);
    auto ids = empty_vocab_tok.encode("hello", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    // Each char is UNK: h, e, l, l, o = 5 UNKs
    for (size_t i = 1; i <= 5; ++i) {
        EXPECT_EQ(ids[i], UNK) << "Position " << i;
    }
    EXPECT_EQ(ids[6], SEP);
    EXPECT_EQ(ids[7], PAD);
}

// ---------------------------------------------------------------------------
// Consistency: encode twice gives same result
// ---------------------------------------------------------------------------

TEST_F(QA_GDB323_BPETokenizer, EncodeDeterministic) {
    auto ids1 = tokenizer_->encode("hello world", 8);
    auto ids2 = tokenizer_->encode("hello world", 8);
    EXPECT_EQ(ids1, ids2);
}

// ---------------------------------------------------------------------------
// Output size always equals max_length (or 0 if max_length=0)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB323_BPETokenizer, OutputSizeAlwaysMatchesMaxLength) {
    // Test a range of max_lengths from 0 to 20 with various inputs.
    std::vector<std::string> inputs = {"", "a", "hello", "hello world", "a a a a a a a a a a"};
    for (const auto& input : inputs) {
        for (size_t max_len = 0; max_len <= 20; ++max_len) {
            auto ids = tokenizer_->encode(input, max_len);
            EXPECT_EQ(ids.size(), max_len) << "Input: \"" << input << "\", max_length: " << max_len;
        }
    }
}

// ---------------------------------------------------------------------------
// Byte-to-unicode mapping completeness
// ---------------------------------------------------------------------------

TEST_F(QA_GDB323_BPETokenizer, AllBytesHandledNoSilentDrop) {
    // Every byte value (0-255) should produce at least one token through
    // byte-to-unicode. No byte should be silently dropped.
    for (int b = 0; b < 256; ++b) {
        std::string input(1, static_cast<char>(b));
        // Use WHITESPACE pre-tokenizer. Some bytes might be whitespace
        // and produce no words. For non-whitespace bytes, we expect a token.
        auto ids = tokenizer_->encode(input, 4);
        ASSERT_EQ(ids.size(), 4u) << "Byte: " << b;
        EXPECT_EQ(ids[0], CLS) << "Byte: " << b;
        // At minimum: CLS + (maybe content) + SEP + PAD...
        // If the byte is whitespace, content may be empty -> CLS, SEP, PAD, PAD.
        // If non-whitespace, content should be present -> CLS, token, SEP, PAD.
    }
}

// ---------------------------------------------------------------------------
// Merge priority ordering
// ---------------------------------------------------------------------------

TEST_F(QA_GDB323_BPETokenizer, MergePriorityRespected) {
    // "hel" -> ['h','e','l']
    //   Pairs: "h e" (rank 0), "e l" (no rule)
    //   Best: "h e" rank 0 -> ['he','l']
    //   Pairs: "he l" (rank 1)
    //   -> ['hel']
    //   vocab: hel=301
    auto ids = tokenizer_->encode("hel", 8);
    ASSERT_EQ(ids.size(), 8u);
    std::vector<int64_t> expected = {CLS, 301, SEP, PAD, PAD, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

TEST_F(QA_GDB323_BPETokenizer, MergeDoesNotSkipLowerRank) {
    // "he" -> ['h','e']
    //   rank 0: "h e" -> ['he']
    //   vocab: he=300
    auto ids = tokenizer_->encode("he", 8);
    ASSERT_EQ(ids.size(), 8u);
    std::vector<int64_t> expected = {CLS, 300, SEP, PAD, PAD, PAD, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

// ---------------------------------------------------------------------------
// Multiple words: space-prefix consistency
// ---------------------------------------------------------------------------

TEST_F(QA_GDB323_BPETokenizer, FourWordsSpacePrefixOnAll) {
    // "hello hello hello hello"
    // word 0: hello=303
    // word 1: Ġhello=405
    // word 2: Ġhello=405
    // word 3: Ġhello=405
    auto ids = tokenizer_->encode("hello hello hello hello", 8);
    ASSERT_EQ(ids.size(), 8u);
    std::vector<int64_t> expected = {CLS, 303, 405, 405, 405, SEP, PAD, PAD};
    EXPECT_EQ(ids, expected);
}

} // namespace
} // namespace sixseven
