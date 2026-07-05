#include "sixseven/vector/builtin_provider.h"
#include "sixseven/vector/tokenizer.h"
#include "sixseven/vector/word_tokenizer.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace sixseven {
namespace {

// Reimplementation of the pre-refactor loop logic (behavior spec), used as an
// independent oracle to check for_each_lower_word doesn't silently diverge.
std::vector<std::string> reference_words(const std::string& text) {
    std::vector<std::string> words;
    std::string current;
    for (unsigned char c : text) {
        if (std::isalnum(c) != 0) {
            current += static_cast<char>(std::tolower(c));
        } else {
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }
        }
    }
    if (!current.empty()) {
        words.push_back(current);
    }
    return words;
}

std::vector<std::string> collect_words(const std::string& text) {
    std::vector<std::string> out;
    for_each_lower_word(text, [&](const std::string& w) { out.push_back(w); });
    return out;
}

class QAGdb1223CrossPath : public ::testing::TestWithParam<std::string> {};

TEST_P(QAGdb1223CrossPath, HelperMatchesReferenceSpec) {
    // BuiltinProvider::tokenize is private, so we can't call it directly from
    // QA. Both HashTokenizer::encode and BuiltinProvider::tokenize delegate
    // to for_each_lower_word, so verifying the shared helper against an
    // independent reference implementation covers both call sites' word
    // boundary logic (they are provably identical since they share the code).
    const std::string& text = GetParam();
    auto helper_words = collect_words(text);
    auto ref_words = reference_words(text);

    EXPECT_EQ(helper_words, ref_words) << "helper diverges from reference spec for: " << text;
}

INSTANTIATE_TEST_SUITE_P(
    Inputs, QAGdb1223CrossPath,
    ::testing::Values(
        std::string("Hello World"),
        std::string("MixedCASE123 words_with_underscores"),
        std::string("digits123andletters456"),
        std::string("punctuation!!!everywhere???,,,..."),
        std::string("   leading and trailing spaces   "),
        std::string("consecutive---delimiters,,,,here"),
        std::string(""),
        std::string("!!!###$$$"),
        std::string("café naïve"), // high-bit UTF-8 bytes
        std::string("a"),
        std::string("a_b_c 123_456"),
        std::string("The Quick Brown Fox Jumps Over The Lazy Dog 123!")));

TEST(QAGdb1223, EmptyStringProducesNoWords) {
    EXPECT_TRUE(collect_words("").empty());
}

TEST(QAGdb1223, AllDelimitersProducesNoWords) {
    std::string all_delims = "!@#$%^&*()   ,,,...---___+++===";
    // Note: underscore is NOT alnum, so "___" counts as delimiters too.
    EXPECT_TRUE(collect_words(all_delims).empty());
}

TEST(QAGdb1223, HighBitBytesDoNotCrashAndAreTreatedAsDelimiters) {
    // Bytes 0x80-0xFF cast to unsigned char must not create UB in isalnum/tolower.
    std::string raw;
    for (int b = 0x80; b <= 0xFF; ++b) {
        raw.push_back(static_cast<char>(b));
    }
    raw += "word";
    for (int b = 0x80; b <= 0xFF; ++b) {
        raw.push_back(static_cast<char>(b));
    }

    std::vector<std::string> words;
    ASSERT_NO_THROW(words = collect_words(raw));
    ASSERT_EQ(words.size(), 1u);
    EXPECT_EQ(words[0], "word");

    // Exercise the BuiltinProvider path (via public embed()) with the same
    // high-bit-byte input to confirm no crash end-to-end.
    BuiltinProvider provider(8);
    auto embed_result = provider.embed(raw);
    ASSERT_TRUE(embed_result.has_value());
    EXPECT_EQ(embed_result->size(), 8u);
}

TEST(QAGdb1223, VeryLongWordDoesNotCrash) {
    std::string long_word(100000, 'a');
    auto words = collect_words(long_word);
    ASSERT_EQ(words.size(), 1u);
    EXPECT_EQ(words[0].size(), 100000u);

    BuiltinProvider provider(8);
    auto embed_result = provider.embed(long_word);
    ASSERT_TRUE(embed_result.has_value());
    EXPECT_EQ(embed_result->size(), 8u);
}

TEST(QAGdb1223, StringOfOnlyDelimitersIsEmptyAndProviderReturnsError) {
    // BuiltinProvider::embed should fail cleanly on text with no words.
    BuiltinProvider provider(16);
    auto result = provider.embed("!!!   ,,,   ---");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QAGdb1223, HashTokenizerEncodeMatchesSharedHelperWordBoundaries) {
    // Confirm the CLS/SEP wrapped token count reflects the same word count
    // that for_each_lower_word (the shared helper both HashTokenizer::encode
    // and BuiltinProvider::tokenize call) would find, for representative text.
    HashTokenizer tok(32);
    std::string text = "The Quick, Brown-Fox! jumps_over 123 lazy_dogs.";
    auto tokens = tok.encode(text, 32);
    auto words = collect_words(text);
    auto mask = tok.attention_mask(tokens);

    // tokens = [CLS] + up to (max_length-2) words + [SEP] + padding.
    // Count non-pad tokens (mask == 1) between CLS (index 0) and SEP
    // (the last non-pad token), i.e. the word tokens.
    size_t non_pad_count = 0;
    for (int64_t m : mask) {
        non_pad_count += static_cast<size_t>(m);
    }
    // non_pad_count includes CLS + words + SEP.
    ASSERT_GE(non_pad_count, 2u);
    size_t word_token_count = non_pad_count - 2;
    EXPECT_EQ(word_token_count, words.size());
}

TEST(QAGdb1223, EmbeddingStableForKnownText) {
    // Regression: known text must produce a stable, deterministic embedding
    // (tokenization behavior is unchanged, so vector must be unchanged too).
    BuiltinProvider provider(8);
    auto result1 = provider.embed("the quick brown fox");
    auto result2 = provider.embed("the quick brown fox");
    ASSERT_TRUE(result1.has_value());
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(*result1, *result2);

    // Case/punctuation-insensitivity: differently-cased/punctuated text with
    // the same underlying words must embed identically.
    auto result3 = provider.embed("The Quick, Brown Fox!");
    ASSERT_TRUE(result3.has_value());
    EXPECT_EQ(*result1, *result3);
}

TEST(QAGdb1223, UnderscoreIsNotPartOfWordButDigitsAre) {
    // std::isalnum does NOT include '_'. Verify this boundary is preserved.
    auto words = collect_words("foo_bar123");
    ASSERT_EQ(words.size(), 2u);
    EXPECT_EQ(words[0], "foo");
    EXPECT_EQ(words[1], "bar123");
}

} // namespace
} // namespace sixseven
