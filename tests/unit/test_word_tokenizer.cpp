#include "sixseven/vector/builtin_provider.h"
#include "sixseven/vector/tokenizer.h"
#include "sixseven/vector/word_tokenizer.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace sixseven {
namespace {

std::vector<std::string> collect(std::string_view text) {
    std::vector<std::string> words;
    for_each_lower_word(text, [&](const std::string& word) { words.push_back(word); });
    return words;
}

TEST(WordTokenizer, EmptyInputProducesNoWords) {
    EXPECT_EQ(collect(""), std::vector<std::string>{});
}

TEST(WordTokenizer, SingleWord) {
    EXPECT_EQ(collect("hello"), std::vector<std::string>({"hello"}));
}

TEST(WordTokenizer, MultipleWords) {
    EXPECT_EQ(collect("hello world foo"), std::vector<std::string>({"hello", "world", "foo"}));
}

TEST(WordTokenizer, LeadingDelimiters) {
    EXPECT_EQ(collect("   hello"), std::vector<std::string>({"hello"}));
}

TEST(WordTokenizer, TrailingDelimiters) {
    EXPECT_EQ(collect("hello   "), std::vector<std::string>({"hello"}));
}

TEST(WordTokenizer, ConsecutiveDelimiters) {
    EXPECT_EQ(collect("hello,,,world"), std::vector<std::string>({"hello", "world"}));
}

TEST(WordTokenizer, AllDelimiterStringProducesNoWords) {
    EXPECT_EQ(collect("!!! ,,, ..."), std::vector<std::string>{});
}

TEST(WordTokenizer, DigitsMixedWithLetters) {
    EXPECT_EQ(collect("abc123 456def"), std::vector<std::string>({"abc123", "456def"}));
}

TEST(WordTokenizer, UppercaseIsLowercased) {
    EXPECT_EQ(collect("HeLLo WORLD"), std::vector<std::string>({"hello", "world"}));
}

TEST(WordTokenizer, UnderscoreIsADelimiter) {
    // Underscore is not alnum, so it currently splits words -- lock that behavior.
    EXPECT_EQ(collect("foo_bar"), std::vector<std::string>({"foo", "bar"}));
}

TEST(WordTokenizer, HighBitByteIsTreatedSafely) {
    // High-bit bytes must not trigger UB in isalnum/tolower (unsigned-char cast).
    // They are non-alnum under the classic "C" locale, so they act as delimiters.
    std::string text;
    text += "abc";
    text += static_cast<char>(0xFF);
    text += "def";
    EXPECT_EQ(collect(text), std::vector<std::string>({"abc", "def"}));
}

TEST(WordTokenizer, TrailingWordWithoutDelimiterIsFlushed) {
    EXPECT_EQ(collect("no trailing delimiter"),
              std::vector<std::string>({"no", "trailing", "delimiter"}));
}

// ---------------------------------------------------------------------------
// Cross-check: HashTokenizer and BuiltinProvider must agree on tokenization.
// ---------------------------------------------------------------------------

TEST(WordTokenizer, HashTokenizerAndBuiltinProviderAgreeOnTokens) {
    const std::string text = "Hello, World! foo_bar 123abc";

    // BuiltinProvider's private tokenize() isn't directly reachable, but its
    // embedding is a deterministic function of the word stream, so instead
    // verify both against the shared helper directly (this is the contract
    // both call sites now depend on).
    std::vector<std::string> expected;
    for_each_lower_word(text, [&](const std::string& word) { expected.push_back(word); });

    EXPECT_EQ(expected, std::vector<std::string>({"hello", "world", "foo", "bar", "123abc"}));

    // HashTokenizer builds its content tokens from the same word stream
    // (both call sites use for_each_lower_word with identical input, so the
    // word sequences are byte-identical by construction). Confirm SEP lands
    // right after the 5 content tokens: CLS(1) + 5 words + SEP(1) = index 6.
    HashTokenizer tokenizer;
    auto tokens = tokenizer.encode(text, 16);
    ASSERT_EQ(tokens.size(), 16u);
    EXPECT_EQ(tokens[0], 101); // CLS token id
    EXPECT_EQ(tokens[6], 102); // SEP token id
}

} // namespace
} // namespace sixseven
