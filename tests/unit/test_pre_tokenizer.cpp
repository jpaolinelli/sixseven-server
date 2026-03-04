#include "giodb/vector/pre_tokenizer.h"
#include "giodb/vector/tokenizer.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

using namespace giodb;

// Helper to convert vector<string_view> to vector<string> for easier assertions.
static std::vector<std::string> to_strings(const std::vector<std::string_view>& views) {
    std::vector<std::string> result;
    result.reserve(views.size());
    for (auto sv : views) {
        result.emplace_back(sv);
    }
    return result;
}

// ===================================================================
// BertPreTokenizer — Basic Word Splitting
// ===================================================================

TEST(BertPreTokenizer, SimpleWords) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("hello world"));
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "hello");
    EXPECT_EQ(tokens[1], "world");
}

TEST(BertPreTokenizer, SingleWord) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("hello"));
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "hello");
}

TEST(BertPreTokenizer, ThreeWords) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("the quick fox"));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "the");
    EXPECT_EQ(tokens[1], "quick");
    EXPECT_EQ(tokens[2], "fox");
}

// ===================================================================
// BertPreTokenizer — Whitespace Handling
// ===================================================================

TEST(BertPreTokenizer, MultipleSpaces) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("hello   world"));
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "hello");
    EXPECT_EQ(tokens[1], "world");
}

TEST(BertPreTokenizer, LeadingWhitespace) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("  hello world"));
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "hello");
    EXPECT_EQ(tokens[1], "world");
}

TEST(BertPreTokenizer, TrailingWhitespace) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("hello world  "));
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "hello");
    EXPECT_EQ(tokens[1], "world");
}

TEST(BertPreTokenizer, LeadingAndTrailingWhitespace) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("  hello world  "));
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "hello");
    EXPECT_EQ(tokens[1], "world");
}

TEST(BertPreTokenizer, TabsAndNewlines) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("hello\tworld\ntest"));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "hello");
    EXPECT_EQ(tokens[1], "world");
    EXPECT_EQ(tokens[2], "test");
}

TEST(BertPreTokenizer, OnlyWhitespace) {
    BertPreTokenizer pt;
    auto tokens = pt.pre_tokenize("   \t\n  ");
    EXPECT_TRUE(tokens.empty());
}

// ===================================================================
// BertPreTokenizer — Punctuation Splitting
// ===================================================================

TEST(BertPreTokenizer, TrailingPunctuation) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("hello!"));
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "hello");
    EXPECT_EQ(tokens[1], "!");
}

TEST(BertPreTokenizer, LeadingPunctuation) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("(hello)"));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "(");
    EXPECT_EQ(tokens[1], "hello");
    EXPECT_EQ(tokens[2], ")");
}

TEST(BertPreTokenizer, Contraction) {
    BertPreTokenizer pt;
    // don't -> ["don", "'", "t"]
    auto tokens = to_strings(pt.pre_tokenize("don't"));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "don");
    EXPECT_EQ(tokens[1], "'");
    EXPECT_EQ(tokens[2], "t");
}

TEST(BertPreTokenizer, ContractionInSentence) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("I don't know"));
    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0], "I");
    EXPECT_EQ(tokens[1], "don");
    EXPECT_EQ(tokens[2], "'");
    EXPECT_EQ(tokens[3], "t");
    EXPECT_EQ(tokens[4], "know");
}

TEST(BertPreTokenizer, ConsecutivePunctuation) {
    BertPreTokenizer pt;
    // "a...b" -> ["a", ".", ".", ".", "b"]
    auto tokens = to_strings(pt.pre_tokenize("a...b"));
    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0], "a");
    EXPECT_EQ(tokens[1], ".");
    EXPECT_EQ(tokens[2], ".");
    EXPECT_EQ(tokens[3], ".");
    EXPECT_EQ(tokens[4], "b");
}

TEST(BertPreTokenizer, PunctuationHeavy) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("wow!!! really???"));
    ASSERT_EQ(tokens.size(), 8u);
    EXPECT_EQ(tokens[0], "wow");
    EXPECT_EQ(tokens[1], "!");
    EXPECT_EQ(tokens[2], "!");
    EXPECT_EQ(tokens[3], "!");
    EXPECT_EQ(tokens[4], "really");
    EXPECT_EQ(tokens[5], "?");
    EXPECT_EQ(tokens[6], "?");
    EXPECT_EQ(tokens[7], "?");
}

TEST(BertPreTokenizer, OnlyPunctuation) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("..."));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], ".");
    EXPECT_EQ(tokens[1], ".");
    EXPECT_EQ(tokens[2], ".");
}

TEST(BertPreTokenizer, CommaAndPeriod) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("hello, world."));
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[0], "hello");
    EXPECT_EQ(tokens[1], ",");
    EXPECT_EQ(tokens[2], "world");
    EXPECT_EQ(tokens[3], ".");
}

TEST(BertPreTokenizer, MixedPunctuationAndWhitespace) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("hello , world ! test"));
    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0], "hello");
    EXPECT_EQ(tokens[1], ",");
    EXPECT_EQ(tokens[2], "world");
    EXPECT_EQ(tokens[3], "!");
    EXPECT_EQ(tokens[4], "test");
}

TEST(BertPreTokenizer, HyphenatedWord) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("well-known"));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "well");
    EXPECT_EQ(tokens[1], "-");
    EXPECT_EQ(tokens[2], "known");
}

TEST(BertPreTokenizer, NumbersWithPunctuation) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("price: $100.50"));
    ASSERT_EQ(tokens.size(), 6u);
    EXPECT_EQ(tokens[0], "price");
    EXPECT_EQ(tokens[1], ":");
    EXPECT_EQ(tokens[2], "$");
    EXPECT_EQ(tokens[3], "100");
    EXPECT_EQ(tokens[4], ".");
    EXPECT_EQ(tokens[5], "50");
}

// ===================================================================
// BertPreTokenizer — Empty Input
// ===================================================================

TEST(BertPreTokenizer, EmptyInput) {
    BertPreTokenizer pt;
    auto tokens = pt.pre_tokenize("");
    EXPECT_TRUE(tokens.empty());
}

// ===================================================================
// BertPreTokenizer — String View Validity
// ===================================================================

TEST(BertPreTokenizer, ViewsPointIntoOriginalString) {
    BertPreTokenizer pt;
    std::string text = "hello world";
    auto tokens = pt.pre_tokenize(text);
    ASSERT_EQ(tokens.size(), 2u);
    // Views should point into the original string.
    EXPECT_GE(tokens[0].data(), text.data());
    EXPECT_LE(tokens[0].data() + tokens[0].size(), text.data() + text.size());
    EXPECT_GE(tokens[1].data(), text.data());
    EXPECT_LE(tokens[1].data() + tokens[1].size(), text.data() + text.size());
}

// ===================================================================
// WhitespaceSplitPreTokenizer — Basic Splitting
// ===================================================================

TEST(WhitespaceSplitPreTokenizer, SimpleWords) {
    WhitespaceSplitPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("hello world"));
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "hello");
    EXPECT_EQ(tokens[1], "world");
}

TEST(WhitespaceSplitPreTokenizer, SingleWord) {
    WhitespaceSplitPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("hello"));
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "hello");
}

TEST(WhitespaceSplitPreTokenizer, MultipleSpaces) {
    WhitespaceSplitPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("hello   world"));
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "hello");
    EXPECT_EQ(tokens[1], "world");
}

TEST(WhitespaceSplitPreTokenizer, LeadingTrailingWhitespace) {
    WhitespaceSplitPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("  hello world  "));
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "hello");
    EXPECT_EQ(tokens[1], "world");
}

TEST(WhitespaceSplitPreTokenizer, TabsAndNewlines) {
    WhitespaceSplitPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("hello\tworld\ntest"));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "hello");
    EXPECT_EQ(tokens[1], "world");
    EXPECT_EQ(tokens[2], "test");
}

// ===================================================================
// WhitespaceSplitPreTokenizer — Punctuation Preserved
// ===================================================================

TEST(WhitespaceSplitPreTokenizer, KeepsPunctuationAttached) {
    WhitespaceSplitPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("hello!"));
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "hello!");
}

TEST(WhitespaceSplitPreTokenizer, ContractionKeptWhole) {
    WhitespaceSplitPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("don't"));
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "don't");
}

TEST(WhitespaceSplitPreTokenizer, PunctuationHeavy) {
    WhitespaceSplitPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("hello, world! test..."));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "hello,");
    EXPECT_EQ(tokens[1], "world!");
    EXPECT_EQ(tokens[2], "test...");
}

// ===================================================================
// WhitespaceSplitPreTokenizer — Empty / Edge Cases
// ===================================================================

TEST(WhitespaceSplitPreTokenizer, EmptyInput) {
    WhitespaceSplitPreTokenizer pt;
    auto tokens = pt.pre_tokenize("");
    EXPECT_TRUE(tokens.empty());
}

TEST(WhitespaceSplitPreTokenizer, OnlyWhitespace) {
    WhitespaceSplitPreTokenizer pt;
    auto tokens = pt.pre_tokenize("   \t\n  ");
    EXPECT_TRUE(tokens.empty());
}

// ===================================================================
// NullPreTokenizer
// ===================================================================

TEST(NullPreTokenizer, ReturnsWholeText) {
    NullPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("hello world"));
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "hello world");
}

TEST(NullPreTokenizer, EmptyInput) {
    NullPreTokenizer pt;
    auto tokens = pt.pre_tokenize("");
    EXPECT_TRUE(tokens.empty());
}

TEST(NullPreTokenizer, PreservesPunctuation) {
    NullPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("don't stop!"));
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "don't stop!");
}

// ===================================================================
// Factory: create_pre_tokenizer
// ===================================================================

TEST(CreatePreTokenizer, PunctuationType) {
    TokenizerConfig config;
    config.pre_tokenizer = PreTokenizerType::PUNCTUATION;

    auto pt = create_pre_tokenizer(config);
    ASSERT_NE(pt, nullptr);
    // Should split on punctuation (BertPreTokenizer behavior).
    auto tokens = to_strings(pt->pre_tokenize("don't"));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "don");
    EXPECT_EQ(tokens[1], "'");
    EXPECT_EQ(tokens[2], "t");
}

TEST(CreatePreTokenizer, WhitespaceType) {
    TokenizerConfig config;
    config.pre_tokenizer = PreTokenizerType::WHITESPACE;

    auto pt = create_pre_tokenizer(config);
    ASSERT_NE(pt, nullptr);
    // Should keep punctuation attached (WhitespaceSplitPreTokenizer behavior).
    auto tokens = to_strings(pt->pre_tokenize("don't"));
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "don't");
}

TEST(CreatePreTokenizer, NoneType) {
    TokenizerConfig config;
    config.pre_tokenizer = PreTokenizerType::NONE;

    auto pt = create_pre_tokenizer(config);
    ASSERT_NE(pt, nullptr);
    // Should return entire text as one span.
    auto tokens = to_strings(pt->pre_tokenize("hello world"));
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "hello world");
}

TEST(CreatePreTokenizer, Polymorphism) {
    TokenizerConfig config;
    config.pre_tokenizer = PreTokenizerType::PUNCTUATION;

    std::unique_ptr<PreTokenizer> pt = create_pre_tokenizer(config);
    ASSERT_NE(pt, nullptr);
    auto tokens = to_strings(pt->pre_tokenize("hello, world!"));
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[0], "hello");
    EXPECT_EQ(tokens[1], ",");
    EXPECT_EQ(tokens[2], "world");
    EXPECT_EQ(tokens[3], "!");
}
