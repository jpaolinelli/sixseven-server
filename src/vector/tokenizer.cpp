#include "giodb/vector/tokenizer.h"

#include <cctype>
#include <functional>
#include <string>

namespace giodb {

// ---------------------------------------------------------------------------
// HashTokenizer
// ---------------------------------------------------------------------------

HashTokenizer::HashTokenizer(size_t max_seq_length) : max_seq_length_(max_seq_length) {}

std::vector<int64_t> HashTokenizer::encode(const std::string& text, size_t max_length) const {
    // Simple hash-based tokenizer.
    // 1. Split into lowercase words on whitespace/punctuation.
    // 2. Hash each word to a stable integer in [VOCAB_OFFSET, VOCAB_OFFSET + VOCAB_SIZE).
    //    (Avoids special token IDs: PAD=0, UNK=100, CLS=101, SEP=102, MASK=103.)
    // 3. Add CLS at start, SEP at end, pad with 0.

    if (max_length == 0) {
        return {};
    }

    std::vector<int64_t> tokens;
    tokens.push_back(special_tokens_.cls);

    std::string current_word;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            current_word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            if (!current_word.empty()) {
                if (tokens.size() < max_length - 1) {
                    auto hash = std::hash<std::string>{}(current_word);
                    tokens.push_back(static_cast<int64_t>(hash % VOCAB_SIZE) + VOCAB_OFFSET);
                }
                current_word.clear();
            }
        }
    }
    // Flush the last word.
    if (!current_word.empty() && tokens.size() < max_length - 1) {
        auto hash = std::hash<std::string>{}(current_word);
        tokens.push_back(static_cast<int64_t>(hash % VOCAB_SIZE) + VOCAB_OFFSET);
    }

    tokens.push_back(special_tokens_.sep);

    // Pad to max_length.
    while (tokens.size() < max_length) {
        tokens.push_back(special_tokens_.pad);
    }

    // Truncate if somehow exceeded (shouldn't happen with the checks above).
    if (tokens.size() > max_length) {
        tokens.resize(max_length);
        tokens.back() = special_tokens_.sep;
    }

    return tokens;
}

std::vector<int64_t> HashTokenizer::attention_mask(const std::vector<int64_t>& tokens) const {
    std::vector<int64_t> mask;
    mask.reserve(tokens.size());
    for (int64_t token : tokens) {
        mask.push_back(token != special_tokens_.pad ? 1 : 0);
    }
    return mask;
}

size_t HashTokenizer::vocab_size() const {
    return VOCAB_SIZE;
}

size_t HashTokenizer::max_sequence_length() const {
    return max_seq_length_;
}

} // namespace giodb
