#include "giodb/vector/bpe_tokenizer.h"

#include <limits>
#include <string>

namespace giodb {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string BPETokenizer::codepoint_to_utf8(uint32_t cp) {
    std::string result;
    if (cp < 0x80) {
        result.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return result;
}

std::vector<std::string> BPETokenizer::split_utf8(const std::string& s) {
    std::vector<std::string> chars;
    size_t i = 0;
    while (i < s.size()) {
        size_t len = 1;
        auto byte = static_cast<unsigned char>(s[i]);
        if (byte >= 0xF0) {
            len = 4;
        } else if (byte >= 0xE0) {
            len = 3;
        } else if (byte >= 0xC0) {
            len = 2;
        }
        if (i + len > s.size()) {
            len = 1; // Invalid UTF-8, take single byte.
        }
        chars.push_back(s.substr(i, len));
        i += len;
    }
    return chars;
}

std::unordered_map<uint8_t, std::string> BPETokenizer::build_byte_to_unicode() {
    // GPT-2 byte-to-unicode mapping:
    // "Printable" bytes map to the same Unicode codepoint.
    // Remaining bytes map to codepoints starting at U+0100 (256).
    std::vector<bool> is_direct(256, false);

    // Range 33-126: ASCII printable (! through ~).
    for (int b = 33; b <= 126; ++b) {
        is_direct[static_cast<size_t>(b)] = true;
    }
    // Range 161-172: Latin-1 supplement.
    for (int b = 161; b <= 172; ++b) {
        is_direct[static_cast<size_t>(b)] = true;
    }
    // Range 174-255: Latin-1 supplement.
    for (int b = 174; b <= 255; ++b) {
        is_direct[static_cast<size_t>(b)] = true;
    }

    std::unordered_map<uint8_t, std::string> mapping;
    uint32_t next_cp = 256;

    for (int b = 0; b < 256; ++b) {
        auto byte_val = static_cast<uint8_t>(b);
        if (is_direct[static_cast<size_t>(b)]) {
            mapping[byte_val] = codepoint_to_utf8(static_cast<uint32_t>(b));
        } else {
            mapping[byte_val] = codepoint_to_utf8(next_cp++);
        }
    }

    return mapping;
}

std::string BPETokenizer::bytes_to_unicode(const std::string& input) const {
    std::string result;
    for (unsigned char byte : input) {
        auto it = byte_to_unicode_.find(byte);
        if (it != byte_to_unicode_.end()) {
            result += it->second;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// BPETokenizer
// ---------------------------------------------------------------------------

BPETokenizer::BPETokenizer(const TokenizerConfig& config)
    : config_(config), normalizer_(create_normalizer(config)),
      pre_tokenizer_(create_pre_tokenizer(config)), byte_to_unicode_(build_byte_to_unicode()) {
    // Index merge rules by their string representation for O(1) lookup.
    for (size_t i = 0; i < config_.merges.size(); ++i) {
        merge_ranks_[config_.merges[i]] = i;
    }
}

std::vector<int64_t> BPETokenizer::tokenize_word(const std::string& word) const {
    if (word.empty()) {
        return {};
    }

    // Split the byte-encoded word into individual UTF-8 characters.
    auto symbols = split_utf8(word);
    if (symbols.empty()) {
        return {};
    }

    // Iteratively merge the highest-priority adjacent pair.
    while (symbols.size() > 1) {
        // Find the pair with the lowest rank (highest priority).
        size_t best_rank = std::numeric_limits<size_t>::max();
        std::string best_left;
        std::string best_right;

        for (size_t i = 0; i + 1 < symbols.size(); ++i) {
            auto key = symbols[i] + " " + symbols[i + 1];
            auto it = merge_ranks_.find(key);
            if (it != merge_ranks_.end() && it->second < best_rank) {
                best_rank = it->second;
                best_left = symbols[i];
                best_right = symbols[i + 1];
            }
        }

        if (best_rank == std::numeric_limits<size_t>::max()) {
            break; // No more applicable merges.
        }

        // Merge all occurrences of the best pair.
        std::vector<std::string> merged;
        size_t i = 0;
        while (i < symbols.size()) {
            if (i + 1 < symbols.size() && symbols[i] == best_left && symbols[i + 1] == best_right) {
                merged.push_back(best_left + best_right);
                i += 2;
            } else {
                merged.push_back(symbols[i]);
                i += 1;
            }
        }
        symbols = std::move(merged);
    }

    // Look up each resulting symbol in the vocabulary.
    std::vector<int64_t> ids;
    ids.reserve(symbols.size());
    for (const auto& sym : symbols) {
        auto it = config_.vocab.find(sym);
        if (it != config_.vocab.end()) {
            ids.push_back(it->second);
        } else {
            ids.push_back(config_.special_tokens.unk);
        }
    }
    return ids;
}

std::vector<int64_t> BPETokenizer::encode(const std::string& text, size_t max_length) const {
    if (max_length == 0) {
        return {};
    }

    // Step 1: Normalize.
    auto normalized = normalizer_->normalize(text);

    // Step 2: Pre-tokenize into words.
    auto words = pre_tokenizer_->pre_tokenize(normalized);

    // Step 3: Byte-encode and BPE-tokenize each word.
    // GPT-2/RoBERTa convention: prepend space to non-first words.
    // The space byte (0x20) becomes U+0120 through byte-to-unicode mapping.
    std::vector<int64_t> content_tokens;
    for (size_t i = 0; i < words.size(); ++i) {
        std::string raw;
        if (i > 0) {
            raw = " " + std::string(words[i]);
        } else {
            raw = std::string(words[i]);
        }
        auto encoded = bytes_to_unicode(raw);
        auto word_ids = tokenize_word(encoded);
        content_tokens.insert(content_tokens.end(), word_ids.begin(), word_ids.end());
    }

    // Step 4: Add CLS/SEP and handle truncation.
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

std::vector<int64_t> BPETokenizer::attention_mask(const std::vector<int64_t>& tokens) const {
    std::vector<int64_t> mask;
    mask.reserve(tokens.size());
    for (int64_t token : tokens) {
        mask.push_back(token != config_.special_tokens.pad ? 1 : 0);
    }
    return mask;
}

size_t BPETokenizer::vocab_size() const {
    return config_.vocab.size();
}

size_t BPETokenizer::max_sequence_length() const {
    // GPT-2 supports up to 1024 tokens, but the actual limit is
    // determined by the caller via the max_length parameter.
    return 1024;
}

} // namespace giodb
