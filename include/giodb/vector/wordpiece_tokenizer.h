#pragma once

#include "giodb/vector/pre_tokenizer.h"
#include "giodb/vector/text_normalizer.h"
#include "giodb/vector/tokenizer.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace giodb {

/// WordPiece subword tokenizer (BERT-style).
///
/// Implements the greedy longest-match-first algorithm used by BERT,
/// MiniLM, BGE, ELECTRA, and DistilBERT. For each word from the
/// pre-tokenizer, finds the longest prefix in the vocabulary, emits
/// that token, and continues with the remainder prefixed by the
/// continuation prefix (typically "##"). If no match is found at any
/// point, emits UNK for the entire word.
///
/// Full pipeline: normalize -> pre-tokenize -> WordPiece encode ->
/// add CLS/SEP -> pad/truncate.
class WordPieceTokenizer : public Tokenizer {
public:
    /// Construct from a TokenizerConfig.
    ///
    /// Reads vocabulary, special token IDs, continuation prefix,
    /// normalizer type, and pre-tokenizer type from the config.
    explicit WordPieceTokenizer(const TokenizerConfig& config);

    [[nodiscard]] std::vector<int64_t> encode(const std::string& text,
                                              size_t max_length) const override;

    [[nodiscard]] std::vector<int64_t>
    attention_mask(const std::vector<int64_t>& tokens) const override;

    [[nodiscard]] size_t vocab_size() const override;
    [[nodiscard]] size_t max_sequence_length() const override;

private:
    /// Apply WordPiece algorithm to a single word.
    ///
    /// Returns token IDs for the subword decomposition, or a vector
    /// containing only the UNK token ID if the word cannot be tokenized.
    [[nodiscard]] std::vector<int64_t> tokenize_word(std::string_view word) const;

    TokenizerConfig config_;
    std::unique_ptr<TextNormalizer> normalizer_;
    std::unique_ptr<PreTokenizer> pre_tokenizer_;
};

} // namespace giodb
