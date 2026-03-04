#include "giodb/vector/wordpiece_tokenizer.h"

#include <string>

namespace giodb {

// ---------------------------------------------------------------------------
// WordPieceTokenizer
// ---------------------------------------------------------------------------

WordPieceTokenizer::WordPieceTokenizer(const TokenizerConfig& config)
    : config_(config), normalizer_(create_normalizer(config)),
      pre_tokenizer_(create_pre_tokenizer(config)) {}

std::vector<int64_t> WordPieceTokenizer::tokenize_word(std::string_view word) const {
    // Greedy longest-match-first algorithm.
    // For the first subword, look up the raw text in vocab.
    // For subsequent subwords, prepend the continuation prefix (e.g. "##").
    // If at any point no match is found, return UNK for the entire word.

    std::vector<int64_t> ids;
    size_t start = 0;
    bool is_first = true;

    while (start < word.size()) {
        bool found = false;
        // Try longest prefix first, shrink until we find a match.
        for (size_t end = word.size(); end > start; --end) {
            std::string substr;
            if (is_first) {
                substr = std::string(word.substr(start, end - start));
            } else {
                substr = config_.subword_prefix + std::string(word.substr(start, end - start));
            }

            auto it = config_.vocab.find(substr);
            if (it != config_.vocab.end()) {
                ids.push_back(it->second);
                start = end;
                is_first = false;
                found = true;
                break;
            }
        }

        if (!found) {
            // No subword match found — entire word is unknown.
            return {config_.special_tokens.unk};
        }
    }

    return ids;
}

std::vector<int64_t> WordPieceTokenizer::encode(const std::string& text, size_t max_length) const {
    if (max_length == 0) {
        return {};
    }

    // Step 1: Normalize.
    auto normalized = normalizer_->normalize(text);

    // Step 2: Pre-tokenize into words.
    auto words = pre_tokenizer_->pre_tokenize(normalized);

    // Step 3: WordPiece encode each word.
    std::vector<int64_t> content_tokens;
    for (auto word : words) {
        auto word_ids = tokenize_word(word);
        content_tokens.insert(content_tokens.end(), word_ids.begin(), word_ids.end());
    }

    // Step 4: Add CLS/SEP and handle truncation.
    // Reserve 2 slots for CLS and SEP.
    std::vector<int64_t> tokens;
    tokens.reserve(max_length);
    tokens.push_back(config_.special_tokens.cls);

    const size_t content_budget = max_length >= 2 ? max_length - 2 : 0;
    const size_t content_count =
        content_tokens.size() < content_budget ? content_tokens.size() : content_budget;
    tokens.insert(tokens.end(),
                  content_tokens.begin(),
                  content_tokens.begin() + static_cast<ptrdiff_t>(content_count));

    if (max_length >= 2) {
        tokens.push_back(config_.special_tokens.sep);
    }

    // Step 5: Pad to max_length.
    while (tokens.size() < max_length) {
        tokens.push_back(config_.special_tokens.pad);
    }

    return tokens;
}

std::vector<int64_t> WordPieceTokenizer::attention_mask(const std::vector<int64_t>& tokens) const {
    std::vector<int64_t> mask;
    mask.reserve(tokens.size());
    for (int64_t token : tokens) {
        mask.push_back(token != config_.special_tokens.pad ? 1 : 0);
    }
    return mask;
}

size_t WordPieceTokenizer::vocab_size() const {
    return config_.vocab.size();
}

size_t WordPieceTokenizer::max_sequence_length() const {
    // WordPiece models typically support up to 512 tokens,
    // but the actual limit is determined by the caller via max_length parameter.
    return 512;
}

} // namespace giodb
