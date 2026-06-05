#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace sixseven {

/// Configuration for the BM25 text analyzer.
///
/// The same configuration MUST be used at index time and query time, otherwise
/// indexed terms and query terms will not match. The config is persisted in the
/// BM25 index metadata page so a reopened index analyzes queries identically.
struct Bm25AnalyzerConfig {
    /// Lowercase ASCII letters before tokenizing.
    bool lowercase = true;

    /// Drop tokens that appear in the stop-word set.
    bool remove_stopwords = true;

    /// Apply Porter stemming (English) to each surviving token.
    bool stem = true;

    /// Minimum token length (in bytes) to keep. Tokens shorter than this are
    /// dropped before stop-word removal and stemming. 0 disables the filter.
    uint32_t min_token_length = 1;

    /// Stop-word set. When empty and remove_stopwords is true, the default
    /// English stop-word list is used (see default_english_stopwords()).
    std::unordered_set<std::string> stopwords;
};

/// Return the built-in English stop-word list used when remove_stopwords is
/// enabled and no custom set is supplied.
[[nodiscard]] const std::unordered_set<std::string>& default_english_stopwords();

/// Purpose-built lexical analyzer for BM25 full-text search.
///
/// Pipeline: lowercase -> tokenize (split on non-alphanumeric ASCII, bytes >=
/// 0x80 are kept as token characters so UTF-8 words survive) -> length filter
/// -> stop-word removal -> Porter stemming. The output is the ordered list of
/// terms used both to build posting lists and to score queries.
///
/// This is deliberately distinct from the embedding tokenizers in the vector
/// module (WordPiece/BPE produce subword units keyed to a vocabulary, which is
/// the wrong granularity for lexical relevance scoring).
class Bm25Analyzer {
public:
    Bm25Analyzer() = default;
    explicit Bm25Analyzer(Bm25AnalyzerConfig config);

    /// Analyze raw text into an ordered list of BM25 terms.
    [[nodiscard]] std::vector<std::string> analyze(const std::string& text) const;

    /// Stem a single already-lowercased ASCII token using the Porter algorithm.
    /// Exposed for testing; analyze() applies this internally when stemming is
    /// enabled.
    [[nodiscard]] static std::string porter_stem(const std::string& token);

    [[nodiscard]] const Bm25AnalyzerConfig& config() const { return config_; }

private:
    Bm25AnalyzerConfig config_;
};

} // namespace sixseven
