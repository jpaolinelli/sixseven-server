#include "sixseven/index/bm25_analyzer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace sixseven;

namespace {

std::vector<std::string> analyze(const std::string& text, Bm25AnalyzerConfig cfg = {}) {
    return Bm25Analyzer(std::move(cfg)).analyze(text);
}

} // namespace

// ===================================================================
// Tokenization
// ===================================================================

TEST(Bm25Analyzer, SplitsOnWhitespace) {
    Bm25AnalyzerConfig cfg;
    cfg.remove_stopwords = false;
    cfg.stem = false;
    EXPECT_EQ(analyze("hello world foo", cfg), (std::vector<std::string>{"hello", "world", "foo"}));
}

TEST(Bm25Analyzer, SplitsOnPunctuation) {
    Bm25AnalyzerConfig cfg;
    cfg.remove_stopwords = false;
    cfg.stem = false;
    EXPECT_EQ(analyze("machine-learning, models!", cfg),
              (std::vector<std::string>{"machine", "learning", "models"}));
}

TEST(Bm25Analyzer, Lowercases) {
    Bm25AnalyzerConfig cfg;
    cfg.remove_stopwords = false;
    cfg.stem = false;
    EXPECT_EQ(analyze("Hello WORLD", cfg), (std::vector<std::string>{"hello", "world"}));
}

TEST(Bm25Analyzer, LowercaseDisabled) {
    Bm25AnalyzerConfig cfg;
    cfg.lowercase = false;
    cfg.remove_stopwords = false;
    cfg.stem = false;
    EXPECT_EQ(analyze("Hello WORLD", cfg), (std::vector<std::string>{"Hello", "WORLD"}));
}

TEST(Bm25Analyzer, KeepsDigits) {
    Bm25AnalyzerConfig cfg;
    cfg.remove_stopwords = false;
    cfg.stem = false;
    EXPECT_EQ(analyze("covid 19 sars2", cfg), (std::vector<std::string>{"covid", "19", "sars2"}));
}

TEST(Bm25Analyzer, EmptyAndSeparatorOnly) {
    EXPECT_TRUE(analyze("").empty());
    EXPECT_TRUE(analyze("   ,.-!  ").empty());
}

TEST(Bm25Analyzer, KeepsUtf8MultibyteWords) {
    // "café" (é = 0xC3 0xA9) must stay a single token, not be shredded.
    Bm25AnalyzerConfig cfg;
    cfg.remove_stopwords = false;
    cfg.stem = false;
    auto terms = analyze("caf\xC3\xA9 bar", cfg);
    ASSERT_EQ(terms.size(), 2u);
    EXPECT_EQ(terms[1], "bar");
    EXPECT_EQ(terms[0], "caf\xC3\xA9");
}

// ===================================================================
// Stop words
// ===================================================================

TEST(Bm25Analyzer, RemovesDefaultStopwords) {
    Bm25AnalyzerConfig cfg;
    cfg.stem = false;
    // "the", "a", "of" are stop words; "quick"/"brown"/"fox" survive.
    EXPECT_EQ(analyze("the quick brown fox", cfg),
              (std::vector<std::string>{"quick", "brown", "fox"}));
}

TEST(Bm25Analyzer, CustomStopwords) {
    Bm25AnalyzerConfig cfg;
    cfg.stem = false;
    cfg.stopwords = {"foo"};
    EXPECT_EQ(analyze("foo bar baz", cfg), (std::vector<std::string>{"bar", "baz"}));
    // "the" is no longer a stop word because a custom set was provided.
    EXPECT_EQ(analyze("the foo", cfg), (std::vector<std::string>{"the"}));
}

TEST(Bm25Analyzer, MinTokenLength) {
    Bm25AnalyzerConfig cfg;
    cfg.remove_stopwords = false;
    cfg.stem = false;
    cfg.min_token_length = 3;
    EXPECT_EQ(analyze("a an the abcd", cfg), (std::vector<std::string>{"the", "abcd"}));
}

// ===================================================================
// Porter stemming
// ===================================================================

TEST(Bm25Analyzer, PorterStemBasic) {
    EXPECT_EQ(Bm25Analyzer::porter_stem("running"), "run");
    EXPECT_EQ(Bm25Analyzer::porter_stem("runs"), "run");
    EXPECT_EQ(Bm25Analyzer::porter_stem("ponies"), "poni");
    EXPECT_EQ(Bm25Analyzer::porter_stem("caresses"), "caress");
    EXPECT_EQ(Bm25Analyzer::porter_stem("cats"), "cat");
}

TEST(Bm25Analyzer, PorterStemClassicExamples) {
    // Examples from Porter's paper.
    EXPECT_EQ(Bm25Analyzer::porter_stem("relational"), "relat");
    EXPECT_EQ(Bm25Analyzer::porter_stem("conditional"), "condit");
    EXPECT_EQ(Bm25Analyzer::porter_stem("rational"), "ration");
    EXPECT_EQ(Bm25Analyzer::porter_stem("happy"), "happi");
}

TEST(Bm25Analyzer, PorterStemShortWordsUnchanged) {
    EXPECT_EQ(Bm25Analyzer::porter_stem("a"), "a");
    EXPECT_EQ(Bm25Analyzer::porter_stem("be"), "be");
}

TEST(Bm25Analyzer, IndexAndQueryAgreeAfterStemming) {
    // The whole point of stemming: "learning" and "learn" map to the same term
    // so a query matches a document regardless of inflection.
    Bm25AnalyzerConfig cfg; // defaults: lowercase + stopwords + stem.
    auto doc = analyze("Machine learning models are learned", cfg);
    auto query = analyze("learn", cfg);
    ASSERT_EQ(query.size(), 1u);
    EXPECT_NE(std::find(doc.begin(), doc.end(), query[0]), doc.end());
}

TEST(Bm25Analyzer, FullPipelineDefault) {
    // "The cats are running quickly" with default config.
    auto terms = analyze("The cats are running quickly");
    // "the"/"are" dropped as stop words; remaining stemmed.
    EXPECT_EQ(terms, (std::vector<std::string>{"cat", "run", "quickli"}));
}
