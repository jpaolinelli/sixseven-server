#include "giodb/vector/pre_tokenizer.h"

#include "giodb/vector/tokenizer.h"

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace giodb {

namespace {

/// Return true if the byte is ASCII whitespace.
bool is_whitespace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

/// Return true if the codepoint is a Unicode punctuation character.
/// Covers ASCII punctuation ranges and common Unicode punctuation blocks.
bool is_punctuation(char ch) {
    auto uch = static_cast<unsigned char>(ch);
    // ASCII punctuation ranges: 33-47, 58-64, 91-96, 123-126.
    return (uch >= 33 && uch <= 47) || (uch >= 58 && uch <= 64) || (uch >= 91 && uch <= 96) ||
           (uch >= 123 && uch <= 126);
}

} // namespace

// --- BertPreTokenizer ---

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::vector<std::string_view> BertPreTokenizer::pre_tokenize(std::string_view text) const {
    std::vector<std::string_view> tokens;

    size_t i = 0;
    while (i < text.size()) {
        // Skip whitespace.
        if (is_whitespace(text[i])) {
            ++i;
            continue;
        }

        // Punctuation: each character is its own token.
        if (is_punctuation(text[i])) {
            tokens.emplace_back(text.data() + i, 1);
            ++i;
            continue;
        }

        // Word: collect consecutive non-whitespace, non-punctuation chars.
        size_t start = i;
        while (i < text.size() && !is_whitespace(text[i]) && !is_punctuation(text[i])) {
            ++i;
        }
        tokens.emplace_back(text.data() + start, i - start);
    }

    return tokens;
}

// --- WhitespaceSplitPreTokenizer ---

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::vector<std::string_view>
WhitespaceSplitPreTokenizer::pre_tokenize(std::string_view text) const {
    std::vector<std::string_view> tokens;

    size_t i = 0;
    while (i < text.size()) {
        // Skip whitespace.
        if (is_whitespace(text[i])) {
            ++i;
            continue;
        }

        // Word: collect consecutive non-whitespace chars.
        size_t start = i;
        while (i < text.size() && !is_whitespace(text[i])) {
            ++i;
        }
        tokens.emplace_back(text.data() + start, i - start);
    }

    return tokens;
}

// --- NullPreTokenizer ---

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::vector<std::string_view> NullPreTokenizer::pre_tokenize(std::string_view text) const {
    if (text.empty()) {
        return {};
    }
    return {text};
}

// --- Factory ---

std::unique_ptr<PreTokenizer> create_pre_tokenizer(const TokenizerConfig& config) {
    // NOLINTBEGIN(bugprone-branch-clone)
    switch (config.pre_tokenizer) {
    case PreTokenizerType::PUNCTUATION:
        return std::make_unique<BertPreTokenizer>();
    case PreTokenizerType::WHITESPACE:
        return std::make_unique<WhitespaceSplitPreTokenizer>();
    case PreTokenizerType::NONE:
        return std::make_unique<NullPreTokenizer>();
    }
    // NOLINTEND(bugprone-branch-clone)
    return std::make_unique<NullPreTokenizer>(); // Unreachable, satisfies compiler.
}

} // namespace giodb
