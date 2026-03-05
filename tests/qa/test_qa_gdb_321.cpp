#include "sixseven/vector/pre_tokenizer.h"
#include "sixseven/vector/tokenizer.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

using namespace sixseven;

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
// GDB321 — BertPreTokenizer: Punctuation Boundary Characters
// ===================================================================

TEST(QA_GDB321_BertBoundary, FirstPunctuationChar_Exclamation) {
    // ASCII 33 is '!' — first char in punctuation range 33-47.
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("a!b"));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "a");
    EXPECT_EQ(tokens[1], "!");
    EXPECT_EQ(tokens[2], "b");
}

TEST(QA_GDB321_BertBoundary, LastPunctuationChar_Slash) {
    // ASCII 47 is '/' — last char in punctuation range 33-47.
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("a/b"));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "a");
    EXPECT_EQ(tokens[1], "/");
    EXPECT_EQ(tokens[2], "b");
}

TEST(QA_GDB321_BertBoundary, DigitsNotPunctuation) {
    // ASCII 48-57 are digits '0'-'9' — should NOT be punctuation.
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("abc0123def"));
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "abc0123def");
}

TEST(QA_GDB321_BertBoundary, ColonStartOfSecondRange) {
    // ASCII 58 is ':' — start of punctuation range 58-64.
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("a:b"));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[1], ":");
}

TEST(QA_GDB321_BertBoundary, AtSignEndOfSecondRange) {
    // ASCII 64 is '@' — end of punctuation range 58-64.
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("user@domain"));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "user");
    EXPECT_EQ(tokens[1], "@");
    EXPECT_EQ(tokens[2], "domain");
}

TEST(QA_GDB321_BertBoundary, UppercaseLettersNotPunctuation) {
    // ASCII 65-90 are uppercase letters — should NOT be punctuation.
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("ABCXYZ"));
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "ABCXYZ");
}

TEST(QA_GDB321_BertBoundary, BracketStartOfThirdRange) {
    // ASCII 91 is '[' — start of punctuation range 91-96.
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("a[b]c"));
    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0], "a");
    EXPECT_EQ(tokens[1], "[");
    EXPECT_EQ(tokens[2], "b");
    EXPECT_EQ(tokens[3], "]");
    EXPECT_EQ(tokens[4], "c");
}

TEST(QA_GDB321_BertBoundary, UnderscoreIsPunctuation) {
    // ASCII 95 is '_' — falls in punctuation range 91-96.
    // BERT treats underscore as punctuation, so this is correct.
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("hello_world"));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "hello");
    EXPECT_EQ(tokens[1], "_");
    EXPECT_EQ(tokens[2], "world");
}

TEST(QA_GDB321_BertBoundary, BacktickIsPunctuation) {
    // ASCII 96 is '`' — end of punctuation range 91-96.
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("a`b"));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[1], "`");
}

TEST(QA_GDB321_BertBoundary, LowercaseLettersNotPunctuation) {
    // ASCII 97-122 are lowercase letters — should NOT be punctuation.
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("abcxyz"));
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "abcxyz");
}

TEST(QA_GDB321_BertBoundary, BraceStartOfFourthRange) {
    // ASCII 123 is '{' — start of punctuation range 123-126.
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("a{b}c"));
    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0], "a");
    EXPECT_EQ(tokens[1], "{");
    EXPECT_EQ(tokens[2], "b");
    EXPECT_EQ(tokens[3], "}");
    EXPECT_EQ(tokens[4], "c");
}

TEST(QA_GDB321_BertBoundary, TildeEndOfFourthRange) {
    // ASCII 126 is '~' — last char in punctuation range 123-126.
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("a~b"));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[1], "~");
}

TEST(QA_GDB321_BertBoundary, DELNotPunctuation) {
    // ASCII 127 is DEL — NOT in any punctuation range.
    // It should be treated as a word character.
    BertPreTokenizer pt;
    std::string input = "a";
    input += static_cast<char>(127);
    input += "b";
    auto tokens = to_strings(pt.pre_tokenize(input));
    // DEL is not whitespace or punctuation, so it should be lumped with word chars.
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].size(), 3u);
}

TEST(QA_GDB321_BertBoundary, SpaceNotPunctuation) {
    // ASCII 32 is space — should be whitespace, NOT punctuation.
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("a b"));
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "a");
    EXPECT_EQ(tokens[1], "b");
}

// ===================================================================
// GDB321 — BertPreTokenizer: All ASCII Punctuation Characters
// ===================================================================

TEST(QA_GDB321_BertPunct, AllASCIIPunctuationSplitIndividually) {
    BertPreTokenizer pt;
    // Every ASCII punctuation character from the four ranges.
    std::string all_punct = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
    auto tokens = pt.pre_tokenize(all_punct);
    // Each character should become its own token.
    ASSERT_EQ(tokens.size(), all_punct.size());
    for (size_t i = 0; i < tokens.size(); ++i) {
        EXPECT_EQ(tokens[i].size(), 1u) << "Token " << i << " should be a single character";
        EXPECT_EQ(tokens[i][0], all_punct[i]) << "Token " << i << " mismatch";
    }
}

// ===================================================================
// GDB321 — BertPreTokenizer: Single Character Inputs
// ===================================================================

TEST(QA_GDB321_BertSingle, SingleLetter) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("x"));
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "x");
}

TEST(QA_GDB321_BertSingle, SinglePunctuation) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("!"));
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "!");
}

TEST(QA_GDB321_BertSingle, SingleSpace) {
    BertPreTokenizer pt;
    auto tokens = pt.pre_tokenize(" ");
    EXPECT_TRUE(tokens.empty());
}

TEST(QA_GDB321_BertSingle, SingleTab) {
    BertPreTokenizer pt;
    auto tokens = pt.pre_tokenize("\t");
    EXPECT_TRUE(tokens.empty());
}

TEST(QA_GDB321_BertSingle, SingleNewline) {
    BertPreTokenizer pt;
    auto tokens = pt.pre_tokenize("\n");
    EXPECT_TRUE(tokens.empty());
}

TEST(QA_GDB321_BertSingle, SingleCarriageReturn) {
    BertPreTokenizer pt;
    auto tokens = pt.pre_tokenize("\r");
    EXPECT_TRUE(tokens.empty());
}

// ===================================================================
// GDB321 — BertPreTokenizer: Special Whitespace Characters
// ===================================================================

TEST(QA_GDB321_BertWhitespace, VerticalTabTreatedAsWordChar) {
    // \v (ASCII 11) is NOT in the is_whitespace check.
    // It will be treated as a word character.
    BertPreTokenizer pt;
    std::string input = "a\vb";
    auto tokens = pt.pre_tokenize(input);
    // \v is not whitespace or punctuation, so "a\vb" is one word.
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].size(), 3u);
}

TEST(QA_GDB321_BertWhitespace, FormFeedTreatedAsWordChar) {
    // \f (ASCII 12) is NOT in the is_whitespace check.
    // It will be treated as a word character.
    BertPreTokenizer pt;
    std::string input = "a\fb";
    auto tokens = pt.pre_tokenize(input);
    // \f is not whitespace or punctuation, so "a\fb" is one word.
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].size(), 3u);
}

TEST(QA_GDB321_BertWhitespace, CRLFSequence) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("hello\r\nworld"));
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "hello");
    EXPECT_EQ(tokens[1], "world");
}

TEST(QA_GDB321_BertWhitespace, MixedWhitespaceTypes) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("a \t\n\r b"));
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "a");
    EXPECT_EQ(tokens[1], "b");
}

// ===================================================================
// GDB321 — BertPreTokenizer: High-Byte Characters
// ===================================================================

TEST(QA_GDB321_BertHighByte, HighByteCharsTreatedAsWord) {
    // Characters >= 128 (e.g., UTF-8 continuation bytes) should be
    // treated as word characters, not punctuation or whitespace.
    BertPreTokenizer pt;
    std::string input = "caf";
    input += static_cast<char>(0xC3); // UTF-8 lead byte for 'é'
    input += static_cast<char>(0xA9); // UTF-8 continuation byte for 'é'
    auto tokens = pt.pre_tokenize(input);
    // All bytes treated as word characters — one token.
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].size(), 5u);
}

TEST(QA_GDB321_BertHighByte, HighBytePunctuationMixed) {
    // High-byte chars mixed with punctuation.
    BertPreTokenizer pt;
    std::string input;
    input += static_cast<char>(0xC3);
    input += static_cast<char>(0xA9);
    input += '!';
    input += static_cast<char>(0xC3);
    input += static_cast<char>(0xBC);
    auto tokens = pt.pre_tokenize(input);
    // [high-byte word] [!] [high-byte word]
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[1], std::string_view("!"));
}

// ===================================================================
// GDB321 — BertPreTokenizer: Null Byte Handling
// ===================================================================

TEST(QA_GDB321_BertNull, NullByteInMiddle) {
    // Null byte (ASCII 0) is not whitespace and not punctuation.
    // It should be treated as a word character.
    BertPreTokenizer pt;
    std::string input = "a";
    input += '\0';
    input += "b";
    auto tokens = pt.pre_tokenize(std::string_view(input.data(), input.size()));
    // \0 is lumped with word chars, so "a\0b" is one token.
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].size(), 3u);
}

// ===================================================================
// GDB321 — BertPreTokenizer: Complex Patterns
// ===================================================================

TEST(QA_GDB321_BertComplex, EmailAddress) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("user@example.com"));
    // user @ example . com
    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0], "user");
    EXPECT_EQ(tokens[1], "@");
    EXPECT_EQ(tokens[2], "example");
    EXPECT_EQ(tokens[3], ".");
    EXPECT_EQ(tokens[4], "com");
}

TEST(QA_GDB321_BertComplex, URLPath) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("https://example.com/path"));
    // https : / / example . com / path
    ASSERT_EQ(tokens.size(), 9u);
    EXPECT_EQ(tokens[0], "https");
    EXPECT_EQ(tokens[1], ":");
    EXPECT_EQ(tokens[2], "/");
    EXPECT_EQ(tokens[3], "/");
    EXPECT_EQ(tokens[4], "example");
    EXPECT_EQ(tokens[5], ".");
    EXPECT_EQ(tokens[6], "com");
    EXPECT_EQ(tokens[7], "/");
    EXPECT_EQ(tokens[8], "path");
}

TEST(QA_GDB321_BertComplex, MultipleContractions) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("I'm can't won't"));
    // I ' m   can ' t   won ' t
    ASSERT_EQ(tokens.size(), 9u);
    EXPECT_EQ(tokens[0], "I");
    EXPECT_EQ(tokens[1], "'");
    EXPECT_EQ(tokens[2], "m");
    EXPECT_EQ(tokens[3], "can");
    EXPECT_EQ(tokens[4], "'");
    EXPECT_EQ(tokens[5], "t");
    EXPECT_EQ(tokens[6], "won");
    EXPECT_EQ(tokens[7], "'");
    EXPECT_EQ(tokens[8], "t");
}

TEST(QA_GDB321_BertComplex, QuotedString) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("\"hello world\""));
    // " hello   world "
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[0], "\"");
    EXPECT_EQ(tokens[1], "hello");
    EXPECT_EQ(tokens[2], "world");
    EXPECT_EQ(tokens[3], "\"");
}

TEST(QA_GDB321_BertComplex, PunctuationSandwich) {
    // Punctuation between two punctuation chars.
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("(!?)"));
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[0], "(");
    EXPECT_EQ(tokens[1], "!");
    EXPECT_EQ(tokens[2], "?");
    EXPECT_EQ(tokens[3], ")");
}

TEST(QA_GDB321_BertComplex, PunctuationWithWhitespace) {
    // Punctuation surrounded by whitespace — still one token each.
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize(" ! ? . "));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "!");
    EXPECT_EQ(tokens[1], "?");
    EXPECT_EQ(tokens[2], ".");
}

// ===================================================================
// GDB321 — BertPreTokenizer: Stress Test
// ===================================================================

TEST(QA_GDB321_BertStress, LargeInputManyWords) {
    BertPreTokenizer pt;
    // Build a string with 10000 words.
    std::string input;
    for (int i = 0; i < 10000; ++i) {
        if (i > 0)
            input += ' ';
        input += "word" + std::to_string(i);
    }
    auto tokens = pt.pre_tokenize(input);
    ASSERT_EQ(tokens.size(), 10000u);
    EXPECT_EQ(std::string(tokens[0]), "word0");
    EXPECT_EQ(std::string(tokens[9999]), "word9999");
}

TEST(QA_GDB321_BertStress, LargeInputManyPunctuation) {
    BertPreTokenizer pt;
    // 10000 consecutive punctuation characters.
    std::string input(10000, '.');
    auto tokens = pt.pre_tokenize(input);
    ASSERT_EQ(tokens.size(), 10000u);
    for (size_t i = 0; i < tokens.size(); ++i) {
        EXPECT_EQ(tokens[i], ".");
    }
}

TEST(QA_GDB321_BertStress, VeryLongSingleWord) {
    BertPreTokenizer pt;
    // One word with 100000 characters.
    std::string input(100000, 'x');
    auto tokens = pt.pre_tokenize(input);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].size(), 100000u);
}

// ===================================================================
// GDB321 — BertPreTokenizer: String View Integrity
// ===================================================================

TEST(QA_GDB321_BertViews, AllViewsPointIntoOriginal) {
    BertPreTokenizer pt;
    std::string text = "hello, world! don't stop...";
    auto tokens = pt.pre_tokenize(text);
    ASSERT_GT(tokens.size(), 0u);
    for (size_t i = 0; i < tokens.size(); ++i) {
        EXPECT_GE(tokens[i].data(), text.data())
            << "Token " << i << " starts before original string";
        EXPECT_LE(tokens[i].data() + tokens[i].size(), text.data() + text.size())
            << "Token " << i << " extends past original string";
        EXPECT_FALSE(tokens[i].empty()) << "Token " << i << " should not be empty";
    }
}

TEST(QA_GDB321_BertViews, NoOverlappingTokens) {
    BertPreTokenizer pt;
    std::string text = "hello, world! don't stop...";
    auto tokens = pt.pre_tokenize(text);
    for (size_t i = 1; i < tokens.size(); ++i) {
        // Each token should start at or after the previous one ends.
        EXPECT_GE(tokens[i].data(), tokens[i - 1].data() + tokens[i - 1].size())
            << "Token " << i << " overlaps with token " << (i - 1);
    }
}

// ===================================================================
// GDB321 — WhitespaceSplitPreTokenizer: Adversarial Tests
// ===================================================================

TEST(QA_GDB321_WsSplit, SingleCharWord) {
    WhitespaceSplitPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("a"));
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "a");
}

TEST(QA_GDB321_WsSplit, SinglePunctuationKept) {
    WhitespaceSplitPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("!"));
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "!");
}

TEST(QA_GDB321_WsSplit, PunctuationAttachedToWords) {
    WhitespaceSplitPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("hello!!! world???"));
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "hello!!!");
    EXPECT_EQ(tokens[1], "world???");
}

TEST(QA_GDB321_WsSplit, VerticalTabNotWhitespace) {
    WhitespaceSplitPreTokenizer pt;
    std::string input = "a\vb";
    auto tokens = pt.pre_tokenize(input);
    // \v is not treated as whitespace — "a\vb" is one token.
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].size(), 3u);
}

TEST(QA_GDB321_WsSplit, FormFeedNotWhitespace) {
    WhitespaceSplitPreTokenizer pt;
    std::string input = "a\fb";
    auto tokens = pt.pre_tokenize(input);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].size(), 3u);
}

TEST(QA_GDB321_WsSplit, CRLFSplits) {
    WhitespaceSplitPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("hello\r\nworld"));
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "hello");
    EXPECT_EQ(tokens[1], "world");
}

TEST(QA_GDB321_WsSplit, AllViewsPointIntoOriginal) {
    WhitespaceSplitPreTokenizer pt;
    std::string text = "hello, world! don't stop";
    auto tokens = pt.pre_tokenize(text);
    ASSERT_GT(tokens.size(), 0u);
    for (size_t i = 0; i < tokens.size(); ++i) {
        EXPECT_GE(tokens[i].data(), text.data());
        EXPECT_LE(tokens[i].data() + tokens[i].size(), text.data() + text.size());
        EXPECT_FALSE(tokens[i].empty());
    }
}

TEST(QA_GDB321_WsSplit, StressLargeInput) {
    WhitespaceSplitPreTokenizer pt;
    std::string input;
    for (int i = 0; i < 10000; ++i) {
        if (i > 0)
            input += ' ';
        input += "word" + std::to_string(i);
    }
    auto tokens = pt.pre_tokenize(input);
    ASSERT_EQ(tokens.size(), 10000u);
}

// ===================================================================
// GDB321 — NullPreTokenizer: Adversarial Tests
// ===================================================================

TEST(QA_GDB321_NullPT, SingleChar) {
    NullPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("x"));
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "x");
}

TEST(QA_GDB321_NullPT, WhitespaceOnlyReturnsWholeText) {
    NullPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("   "));
    // NullPreTokenizer returns non-empty text as a single span.
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "   ");
}

TEST(QA_GDB321_NullPT, ViewPointsIntoOriginal) {
    NullPreTokenizer pt;
    std::string text = "hello world";
    auto tokens = pt.pre_tokenize(text);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].data(), text.data());
    EXPECT_EQ(tokens[0].size(), text.size());
}

TEST(QA_GDB321_NullPT, StressLargeInput) {
    NullPreTokenizer pt;
    std::string input(100000, 'a');
    auto tokens = pt.pre_tokenize(input);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].size(), 100000u);
}

// ===================================================================
// GDB321 — Factory: Adversarial Tests
// ===================================================================

TEST(QA_GDB321_Factory, DefaultConfigUsesPunctuation) {
    // Default TokenizerConfig has pre_tokenizer = PUNCTUATION.
    TokenizerConfig config;
    auto pt = create_pre_tokenizer(config);
    ASSERT_NE(pt, nullptr);
    auto tokens = to_strings(pt->pre_tokenize("a.b"));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "a");
    EXPECT_EQ(tokens[1], ".");
    EXPECT_EQ(tokens[2], "b");
}

TEST(QA_GDB321_Factory, PolymorphicUsageThroughBasePointer) {
    // Ensure all three factory results work through PreTokenizer*.
    for (auto type :
         {PreTokenizerType::NONE, PreTokenizerType::WHITESPACE, PreTokenizerType::PUNCTUATION}) {
        TokenizerConfig config;
        config.pre_tokenizer = type;
        std::unique_ptr<PreTokenizer> pt = create_pre_tokenizer(config);
        ASSERT_NE(pt, nullptr);
        // All should handle empty input without crash.
        auto tokens = pt->pre_tokenize("");
        EXPECT_TRUE(tokens.empty());
    }
}

TEST(QA_GDB321_Factory, EachTypeProducesDifferentBehavior) {
    std::string input = "hello, world!";

    TokenizerConfig config_none;
    config_none.pre_tokenizer = PreTokenizerType::NONE;
    auto pt_none = create_pre_tokenizer(config_none);
    auto tokens_none = pt_none->pre_tokenize(input);

    TokenizerConfig config_ws;
    config_ws.pre_tokenizer = PreTokenizerType::WHITESPACE;
    auto pt_ws = create_pre_tokenizer(config_ws);
    auto tokens_ws = pt_ws->pre_tokenize(input);

    TokenizerConfig config_punct;
    config_punct.pre_tokenizer = PreTokenizerType::PUNCTUATION;
    auto pt_punct = create_pre_tokenizer(config_punct);
    auto tokens_punct = pt_punct->pre_tokenize(input);

    // NONE: 1 token (whole text).
    EXPECT_EQ(tokens_none.size(), 1u);
    // WHITESPACE: 2 tokens ("hello," and "world!").
    EXPECT_EQ(tokens_ws.size(), 2u);
    // PUNCTUATION: 4 tokens ("hello", ",", "world", "!").
    EXPECT_EQ(tokens_punct.size(), 4u);
}

// ===================================================================
// GDB321 — AC Verification: Contractions
// ===================================================================

TEST(QA_GDB321_AC, ContractionDont) {
    // AC explicitly requires: don't → ["don", "'", "t"]
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("don't"));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "don");
    EXPECT_EQ(tokens[1], "'");
    EXPECT_EQ(tokens[2], "t");
}

TEST(QA_GDB321_AC, EmptyInputBert) {
    BertPreTokenizer pt;
    auto tokens = pt.pre_tokenize("");
    EXPECT_TRUE(tokens.empty());
}

TEST(QA_GDB321_AC, EmptyInputWhitespace) {
    WhitespaceSplitPreTokenizer pt;
    auto tokens = pt.pre_tokenize("");
    EXPECT_TRUE(tokens.empty());
}

TEST(QA_GDB321_AC, LeadingTrailingWhitespace) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("  hello  "));
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "hello");
}

TEST(QA_GDB321_AC, MultipleSpaces) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("a    b    c"));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "a");
    EXPECT_EQ(tokens[1], "b");
    EXPECT_EQ(tokens[2], "c");
}

TEST(QA_GDB321_AC, PunctuationHeavyText) {
    BertPreTokenizer pt;
    auto tokens = to_strings(pt.pre_tokenize("wow!!! really??? yes..."));
    // wow ! ! ! really ? ? ? yes . . .  = 12 tokens
    ASSERT_EQ(tokens.size(), 12u);
    EXPECT_EQ(tokens[0], "wow");
    EXPECT_EQ(tokens[1], "!");
    EXPECT_EQ(tokens[2], "!");
    EXPECT_EQ(tokens[3], "!");
    EXPECT_EQ(tokens[4], "really");
    EXPECT_EQ(tokens[5], "?");
    EXPECT_EQ(tokens[6], "?");
    EXPECT_EQ(tokens[7], "?");
    EXPECT_EQ(tokens[8], "yes");
    EXPECT_EQ(tokens[9], ".");
    EXPECT_EQ(tokens[10], ".");
    EXPECT_EQ(tokens[11], ".");
}
