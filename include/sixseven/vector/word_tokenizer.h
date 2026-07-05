#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace sixseven {

/// Split @p text into lowercase alphanumeric words and invoke @p callback
/// once per word, in order.
///
/// A "word" is a maximal run of characters for which `std::isalnum`
/// (evaluated on the unsigned-char-cast byte) is true. Any other byte is
/// a delimiter and ends the current word (delimiters are never included
/// in a word and never themselves emitted). Each emitted word has its
/// characters lowercased via `std::tolower` (also unsigned-char-cast).
/// A trailing word at the end of the input (i.e. one not followed by a
/// delimiter) is still flushed. Empty words (e.g. from leading, trailing,
/// or consecutive delimiters, or an all-delimiter/empty input) are never
/// emitted.
///
/// This defines the shared word-boundary rule used by the hash tokenizer
/// and the built-in embedding provider — both must agree on what counts
/// as a "word" so their token streams remain stable.
///
/// @param text     Input text (arbitrary bytes, including high-bit bytes).
/// @param callback Invoked with `const std::string&` for each word found.
template <typename Callback>
void for_each_lower_word(std::string_view text, Callback&& callback) {
    std::string current_word;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            current_word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            if (!current_word.empty()) {
                callback(current_word);
                current_word.clear();
            }
        }
    }
    if (!current_word.empty()) {
        callback(current_word);
    }
}

} // namespace sixseven
