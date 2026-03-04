/// @file test_qa_gdb_322.cpp
/// @brief QA adversarial tests for GDB-322: WordPiece Algorithm.

#include "giodb/vector/wordpiece_tokenizer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace giodb {
namespace {

// ---------------------------------------------------------------------------
// Shared vocabulary and config helpers
// ---------------------------------------------------------------------------

/// Minimal test vocabulary with carefully chosen entries for adversarial tests.
// clang-format off
const std::unordered_map<std::string, int64_t> QA_VOCAB = {
    // Single characters
    {"a",     200},
    {"b",     201},
    {"x",     202},
    {"z",     203},

    // Multi-character words (for greedy longest-match testing)
    {"ab",    300},
    {"abc",   301},
    {"abcd",  302},

    // Continuation subwords
    {"##a",   400},
    {"##b",   401},
    {"##c",   402},
    {"##d",   403},
    {"##cd",  404},
    {"##bcd", 405},

    // Real-ish words
    {"hello", 500},
    {"world", 501},
    {"the",   502},
    {"cat",   503},
    {"dog",   504},
    {"is",    505},

    // Punctuation
    {"!",     600},
    {",",     601},
    {".",     602},

    // Subword decomposition pairs
    {"em",      700},
    {"##bed",   701},
    {"##ding",  702},
    {"un",      703},
    {"##aff",   704},
    {"##able",  705},
};
// clang-format on

TokenizerConfig make_qa_config() {
    TokenizerConfig config;
    config.vocab = QA_VOCAB;
    config.special_tokens = {.pad = 0, .unk = 100, .cls = 101, .sep = 102, .mask = 103};
    config.model_type = TokenizerModelType::WORDPIECE;
    config.normalizer = NormalizerType::BERT;
    config.pre_tokenizer = PreTokenizerType::PUNCTUATION;
    config.subword_prefix = "##";
    config.normalizer_lowercase = true;
    config.normalizer_strip_accents = false;
    return config;
}

constexpr int64_t CLS = 101;
constexpr int64_t SEP = 102;
constexpr int64_t PAD = 0;
constexpr int64_t UNK = 100;

// ---------------------------------------------------------------------------
// QA_GDB322_GreedyLongestMatch — Verify longest-match-first behavior
// ---------------------------------------------------------------------------

TEST(QA_GDB322_GreedyLongestMatch, ChoosesLongestPrefixOverShorter) {
    // Vocab has "a"(200), "ab"(300), "abc"(301), "abcd"(302).
    // "abcd" should match as one token (302), not "a"+"##bcd" or "ab"+"##cd".
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("abcd", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 302); // "abcd" as a single token
    EXPECT_EQ(ids[2], SEP);
}

TEST(QA_GDB322_GreedyLongestMatch, ChoosesLongestContinuationSubword) {
    // For "abcda": first "abcd"(302), then remainder "a" -> "##a"(400).
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("abcda", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 302); // "abcd"
    EXPECT_EQ(ids[2], 400); // "##a"
    EXPECT_EQ(ids[3], SEP);
}

TEST(QA_GDB322_GreedyLongestMatch, FallsBackToShorterPrefixWhenLongerFails) {
    // "abce": "abce" not in vocab, "abc" not followed by continuation for "e".
    // "abc"(301) matches first, then "e" -> "##e" not in vocab -> UNK for whole word.
    // Actually wait: the algorithm returns UNK for the ENTIRE word if any
    // continuation fails. So "abce" -> UNK.
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("abce", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    // "abc"(301) matches, remainder "e" needs "##e" which is not in vocab -> UNK for whole word
    EXPECT_EQ(ids[1], UNK);
    EXPECT_EQ(ids[2], SEP);
}

// ---------------------------------------------------------------------------
// QA_GDB322_UNKHandling — Unknown word scenarios
// ---------------------------------------------------------------------------

TEST(QA_GDB322_UNKHandling, WordWithNoCharInVocab) {
    // 'q' is not in the QA_VOCAB at all. "qqq" -> UNK.
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("qqq", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], UNK);
    EXPECT_EQ(ids[2], SEP);
    for (size_t i = 3; i < 8; ++i) {
        EXPECT_EQ(ids[i], PAD);
    }
}

TEST(QA_GDB322_UNKHandling, MultipleUnknownWordsEachGetOwnUNK) {
    // "qqq rrr sss" -> three separate unknown words, each gets UNK.
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("qqq rrr sss", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], UNK); // "qqq"
    EXPECT_EQ(ids[2], UNK); // "rrr"
    EXPECT_EQ(ids[3], UNK); // "sss"
    EXPECT_EQ(ids[4], SEP);
}

TEST(QA_GDB322_UNKHandling, MixedKnownAndUnknownWords) {
    // "hello qqq world" -> hello(500), UNK, world(501)
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("hello qqq world", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 500); // "hello"
    EXPECT_EQ(ids[2], UNK); // "qqq"
    EXPECT_EQ(ids[3], 501); // "world"
    EXPECT_EQ(ids[4], SEP);
}

TEST(QA_GDB322_UNKHandling, FirstCharMatchButContinuationFails) {
    // "af": "a"(200) matches, but "##f" not in vocab -> UNK for whole word.
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("af", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], UNK);
    EXPECT_EQ(ids[2], SEP);
}

// ---------------------------------------------------------------------------
// QA_GDB322_SubwordDecomposition — Multi-subword words
// ---------------------------------------------------------------------------

TEST(QA_GDB322_SubwordDecomposition, TwoSubwords) {
    // "embedding" -> "em"(700) + "##bed"(701) + "##ding"(702)
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("embedding", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 700); // "em"
    EXPECT_EQ(ids[2], 701); // "##bed"
    EXPECT_EQ(ids[3], 702); // "##ding"
    EXPECT_EQ(ids[4], SEP);
}

TEST(QA_GDB322_SubwordDecomposition, ThreeSubwords) {
    // "unaffable" -> "un"(703) + "##aff"(704) + "##able"(705)
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("unaffable", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 703); // "un"
    EXPECT_EQ(ids[2], 704); // "##aff"
    EXPECT_EQ(ids[3], 705); // "##able"
    EXPECT_EQ(ids[4], SEP);
}

TEST(QA_GDB322_SubwordDecomposition, SingleCharSubwords) {
    // "abcda" -> "abcd"(302) + "##a"(400)
    // Then try "aba" -> "ab"(300) + "##a"(400)
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("aba", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 300); // "ab"
    EXPECT_EQ(ids[2], 400); // "##a"
    EXPECT_EQ(ids[3], SEP);
}

// ---------------------------------------------------------------------------
// QA_GDB322_CLSSEPPadding — Special token insertion and padding
// ---------------------------------------------------------------------------

TEST(QA_GDB322_CLSSEPPadding, EmptyInputProducesCLSSEPPad) {
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("", 6);
    ASSERT_EQ(ids.size(), 6u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], SEP);
    for (size_t i = 2; i < 6; ++i) {
        EXPECT_EQ(ids[i], PAD);
    }
}

TEST(QA_GDB322_CLSSEPPadding, WhitespaceOnlyInputProducesCLSSEPPad) {
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("   ", 6);
    ASSERT_EQ(ids.size(), 6u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], SEP);
    for (size_t i = 2; i < 6; ++i) {
        EXPECT_EQ(ids[i], PAD);
    }
}

TEST(QA_GDB322_CLSSEPPadding, ContentFillsExactlyNoTruncationNoPadding) {
    // 3 content tokens + CLS + SEP = 5. Use max_length=5.
    // "hello the cat" -> hello(500), the(502), cat(503)
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("hello the cat", 5);
    ASSERT_EQ(ids.size(), 5u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 500); // hello
    EXPECT_EQ(ids[2], 502); // the
    EXPECT_EQ(ids[3], 503); // cat
    EXPECT_EQ(ids[4], SEP);
}

TEST(QA_GDB322_CLSSEPPadding, PaddingFillsRemainingSlots) {
    // "hello" = 1 content token. max_length=8 -> CLS, hello, SEP, PAD x5.
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("hello", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 500);
    EXPECT_EQ(ids[2], SEP);
    for (size_t i = 3; i < 8; ++i) {
        EXPECT_EQ(ids[i], PAD);
    }
}

// ---------------------------------------------------------------------------
// QA_GDB322_Truncation — Truncation behavior
// ---------------------------------------------------------------------------

TEST(QA_GDB322_Truncation, TruncatesExcessContentTokens) {
    // "hello world the cat dog is" -> 6 content tokens.
    // With max_length=5: CLS + 3 content + SEP.
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("hello world the cat dog is", 5);
    ASSERT_EQ(ids.size(), 5u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 500); // hello
    EXPECT_EQ(ids[2], 501); // world
    EXPECT_EQ(ids[3], 502); // the
    EXPECT_EQ(ids[4], SEP); // SEP at end
}

TEST(QA_GDB322_Truncation, TruncationKeepsCLSAtStartSEPAtEnd) {
    // Verify CLS is always first and SEP is always last in truncated output.
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("hello world the cat dog is", 4);
    ASSERT_EQ(ids.size(), 4u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids.back(), SEP);
}

TEST(QA_GDB322_Truncation, TruncationCutsSubwordTokens) {
    // "embedding" = ["em"(700), "##bed"(701), "##ding"(702)] = 3 content tokens.
    // With max_length=4: CLS + 2 content + SEP -> "em", "##bed" kept, "##ding" truncated.
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("embedding", 4);
    ASSERT_EQ(ids.size(), 4u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 700); // "em"
    EXPECT_EQ(ids[2], 701); // "##bed"
    EXPECT_EQ(ids[3], SEP);
    // "##ding" was truncated
}

TEST(QA_GDB322_Truncation, MaxLengthThreeOneContentToken) {
    // max_length=3: CLS + 1 content + SEP.
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("hello world the cat", 3);
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 500); // hello (only first content token fits)
    EXPECT_EQ(ids[2], SEP);
}

TEST(QA_GDB322_Truncation, MaxLengthZeroReturnsEmpty) {
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("hello", 0);
    EXPECT_TRUE(ids.empty());
}

TEST(QA_GDB322_Truncation, MaxLengthOneOnlyCLS) {
    // max_length=1: Only room for CLS, no SEP.
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("hello", 1);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], CLS);
}

TEST(QA_GDB322_Truncation, MaxLengthTwoCLSAndSEPOnly) {
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("hello world the cat", 2);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], SEP);
}

// ---------------------------------------------------------------------------
// QA_GDB322_AttentionMask — Attention mask correctness
// ---------------------------------------------------------------------------

TEST(QA_GDB322_AttentionMask, MaskOnesForContentZerosForPadding) {
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("hello", 6);
    auto mask = tok.attention_mask(ids);
    ASSERT_EQ(mask.size(), 6u);
    // CLS(1), hello(1), SEP(1), PAD(0), PAD(0), PAD(0)
    std::vector<int64_t> expected = {1, 1, 1, 0, 0, 0};
    EXPECT_EQ(mask, expected);
}

TEST(QA_GDB322_AttentionMask, FullSequenceNoZeros) {
    // All positions are non-padding when content fills max_length.
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    // "hello world the" -> 3 content + CLS + SEP = 5
    auto ids = tok.encode("hello world the", 5);
    auto mask = tok.attention_mask(ids);
    ASSERT_EQ(mask.size(), 5u);
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(mask[i], 1) << "position " << i;
    }
}

TEST(QA_GDB322_AttentionMask, EmptyInputOnlyCLSSEPAreOnes) {
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("", 6);
    auto mask = tok.attention_mask(ids);
    ASSERT_EQ(mask.size(), 6u);
    EXPECT_EQ(mask[0], 1); // CLS
    EXPECT_EQ(mask[1], 1); // SEP
    for (size_t i = 2; i < 6; ++i) {
        EXPECT_EQ(mask[i], 0);
    }
}

TEST(QA_GDB322_AttentionMask, EmptyTokenVectorReturnsEmptyMask) {
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto mask = tok.attention_mask({});
    EXPECT_TRUE(mask.empty());
}

TEST(QA_GDB322_AttentionMask, UNKTokenIsMarkedAsOne) {
    // UNK tokens should have attention mask = 1 (they are real content, not padding).
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("qqq", 6);
    auto mask = tok.attention_mask(ids);
    ASSERT_EQ(mask.size(), 6u);
    EXPECT_EQ(mask[0], 1); // CLS
    EXPECT_EQ(mask[1], 1); // UNK (should be 1, not 0)
    EXPECT_EQ(mask[2], 1); // SEP
    for (size_t i = 3; i < 6; ++i) {
        EXPECT_EQ(mask[i], 0);
    }
}

// ---------------------------------------------------------------------------
// QA_GDB322_ContinuationPrefix — Configurable subword prefix
// ---------------------------------------------------------------------------

TEST(QA_GDB322_ContinuationPrefix, CustomPrefixUsedForContinuations) {
    auto config = make_qa_config();
    config.subword_prefix = "@@";
    // Remove ## entries and add @@ equivalents
    config.vocab.erase("##a");
    config.vocab.erase("##b");
    config.vocab["@@a"] = 400;
    config.vocab["@@b"] = 401;

    WordPieceTokenizer tok(config);
    // "aba" -> "ab"(300) + "@@a"(400)
    auto ids = tok.encode("aba", 6);
    ASSERT_EQ(ids.size(), 6u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 300); // "ab"
    EXPECT_EQ(ids[2], 400); // "@@a"
    EXPECT_EQ(ids[3], SEP);
}

TEST(QA_GDB322_ContinuationPrefix, EmptyPrefixStillWorks) {
    // With empty prefix, continuation subwords are just the raw characters.
    auto config = make_qa_config();
    config.subword_prefix = "";
    // With empty prefix, "aba" -> "ab"(300) + "a"(200)
    // Because continuation lookup uses "" + "a" = "a", which maps to 200.
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("aba", 6);
    ASSERT_EQ(ids.size(), 6u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 300); // "ab"
    EXPECT_EQ(ids[2], 200); // "a" (no prefix)
    EXPECT_EQ(ids[3], SEP);
}

// ---------------------------------------------------------------------------
// QA_GDB322_Punctuation — Punctuation handling with pre-tokenizer
// ---------------------------------------------------------------------------

TEST(QA_GDB322_Punctuation, PunctuationIsSeparateToken) {
    // "hello!" -> pre-tokenizer splits to ["hello", "!"]
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("hello!", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 500); // hello
    EXPECT_EQ(ids[2], 600); // !
    EXPECT_EQ(ids[3], SEP);
}

TEST(QA_GDB322_Punctuation, MultiplePunctuationMarks) {
    // "hello!!" -> ["hello", "!", "!"]
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("hello!!", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 500); // hello
    EXPECT_EQ(ids[2], 600); // !
    EXPECT_EQ(ids[3], 600); // !
    EXPECT_EQ(ids[4], SEP);
}

TEST(QA_GDB322_Punctuation, PunctuationOnlyInput) {
    // "!!!" -> ["!", "!", "!"]
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("!!!", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 600); // !
    EXPECT_EQ(ids[2], 600); // !
    EXPECT_EQ(ids[3], 600); // !
    EXPECT_EQ(ids[4], SEP);
}

// ---------------------------------------------------------------------------
// QA_GDB322_CaseNormalization — Case-insensitive encoding
// ---------------------------------------------------------------------------

TEST(QA_GDB322_CaseNormalization, UppercaseMatchesLowercase) {
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids_lower = tok.encode("hello world", 8);
    auto ids_upper = tok.encode("HELLO WORLD", 8);
    EXPECT_EQ(ids_lower, ids_upper);
}

TEST(QA_GDB322_CaseNormalization, MixedCaseMatchesLowercase) {
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids_lower = tok.encode("hello world", 8);
    auto ids_mixed = tok.encode("HeLLo WoRLD", 8);
    EXPECT_EQ(ids_lower, ids_mixed);
}

// ---------------------------------------------------------------------------
// QA_GDB322_VocabAndInterface — Tokenizer interface methods
// ---------------------------------------------------------------------------

TEST(QA_GDB322_VocabAndInterface, VocabSizeMatchesConfig) {
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    EXPECT_EQ(tok.vocab_size(), QA_VOCAB.size());
}

TEST(QA_GDB322_VocabAndInterface, MaxSequenceLengthReturns512) {
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    EXPECT_EQ(tok.max_sequence_length(), 512u);
}

TEST(QA_GDB322_VocabAndInterface, ImplementsTokenizerInterface) {
    auto config = make_qa_config();
    // Verify it can be used through the base class pointer.
    std::unique_ptr<Tokenizer> tok = std::make_unique<WordPieceTokenizer>(config);
    auto ids = tok->encode("hello", 4);
    ASSERT_EQ(ids.size(), 4u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 500);
    EXPECT_EQ(ids[2], SEP);
}

TEST(QA_GDB322_VocabAndInterface, EmptyVocabEverythingIsUNK) {
    auto config = make_qa_config();
    config.vocab.clear();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("hello world", 8);
    ASSERT_EQ(ids.size(), 8u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], UNK); // "hello" not in empty vocab
    EXPECT_EQ(ids[2], UNK); // "world" not in empty vocab
    EXPECT_EQ(ids[3], SEP);
}

// ---------------------------------------------------------------------------
// QA_GDB322_OutputSize — Output vector size guarantees
// ---------------------------------------------------------------------------

TEST(QA_GDB322_OutputSize, OutputAlwaysExactlyMaxLength) {
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);

    // Test various max_length values.
    for (size_t ml : {1u, 2u, 3u, 5u, 8u, 16u, 32u, 64u, 128u}) {
        auto ids = tok.encode("hello world the cat dog is", ml);
        EXPECT_EQ(ids.size(), ml) << "max_length=" << ml;
    }
}

TEST(QA_GDB322_OutputSize, EmptyInputOutputAlwaysExactlyMaxLength) {
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);

    for (size_t ml : {1u, 2u, 3u, 5u, 16u}) {
        auto ids = tok.encode("", ml);
        EXPECT_EQ(ids.size(), ml) << "max_length=" << ml;
    }
}

// ---------------------------------------------------------------------------
// QA_GDB322_Stress — Stress and scale tests
// ---------------------------------------------------------------------------

TEST(QA_GDB322_Stress, ManyWordsWithTruncation) {
    // Generate a long input with many words.
    std::string input;
    for (int i = 0; i < 100; ++i) {
        if (i > 0) input += ' ';
        input += "hello";
    }
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode(input, 16);
    ASSERT_EQ(ids.size(), 16u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[15], SEP); // SEP at end after truncation
    // All content should be "hello" tokens
    for (size_t i = 1; i < 15; ++i) {
        EXPECT_EQ(ids[i], 500) << "position " << i;
    }
}

TEST(QA_GDB322_Stress, ManySubwordWords) {
    // "embedding" repeated many times -> lots of subword tokens.
    std::string input;
    for (int i = 0; i < 50; ++i) {
        if (i > 0) input += ' ';
        input += "embedding";
    }
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode(input, 32);
    ASSERT_EQ(ids.size(), 32u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[31], SEP);
}

TEST(QA_GDB322_Stress, LargeMaxLengthWithSmallInput) {
    // Small input but large max_length = lots of padding.
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("hello", 512);
    ASSERT_EQ(ids.size(), 512u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 500);
    EXPECT_EQ(ids[2], SEP);
    for (size_t i = 3; i < 512; ++i) {
        EXPECT_EQ(ids[i], PAD) << "position " << i;
    }
}

TEST(QA_GDB322_Stress, RepeatedEncodeCallsConsistent) {
    // Calling encode multiple times with the same input should produce identical results.
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids1 = tok.encode("hello world", 8);
    auto ids2 = tok.encode("hello world", 8);
    auto ids3 = tok.encode("hello world", 8);
    EXPECT_EQ(ids1, ids2);
    EXPECT_EQ(ids2, ids3);
}

// ---------------------------------------------------------------------------
// QA_GDB322_MultipleSpaces — Whitespace edge cases
// ---------------------------------------------------------------------------

TEST(QA_GDB322_MultipleSpaces, MultipleSpacesBetweenWords) {
    // "hello   world" -> normalizer collapses whitespace -> same as "hello world"
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids_multi = tok.encode("hello   world", 8);
    auto ids_single = tok.encode("hello world", 8);
    EXPECT_EQ(ids_multi, ids_single);
}

TEST(QA_GDB322_MultipleSpaces, LeadingAndTrailingWhitespace) {
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids_trimmed = tok.encode("hello", 8);
    auto ids_padded = tok.encode("  hello  ", 8);
    EXPECT_EQ(ids_trimmed, ids_padded);
}

TEST(QA_GDB322_MultipleSpaces, TabsAndNewlines) {
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids_spaces = tok.encode("hello world", 8);
    auto ids_tabs = tok.encode("hello\tworld", 8);
    EXPECT_EQ(ids_spaces, ids_tabs);
}

// ---------------------------------------------------------------------------
// QA_GDB322_SingleChar — Single character words
// ---------------------------------------------------------------------------

TEST(QA_GDB322_SingleChar, SingleKnownCharacter) {
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("a", 6);
    ASSERT_EQ(ids.size(), 6u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], 200); // "a"
    EXPECT_EQ(ids[2], SEP);
}

TEST(QA_GDB322_SingleChar, SingleUnknownCharacter) {
    auto config = make_qa_config();
    WordPieceTokenizer tok(config);
    auto ids = tok.encode("q", 6);
    ASSERT_EQ(ids.size(), 6u);
    EXPECT_EQ(ids[0], CLS);
    EXPECT_EQ(ids[1], UNK);
    EXPECT_EQ(ids[2], SEP);
}

} // namespace
} // namespace giodb
