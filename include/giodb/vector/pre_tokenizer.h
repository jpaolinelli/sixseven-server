#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace giodb {

struct TokenizerConfig;

/// Abstract base class for pre-tokenization (word splitting).
///
/// Pre-tokenizers split normalized text into word-level spans before
/// subword tokenization (WordPiece/BPE). The input text is assumed to
/// be already normalized.
class PreTokenizer { // NOLINT(cppcoreguidelines-special-member-functions)
public:
    virtual ~PreTokenizer() = default;

    /// Split text into word-level spans.
    ///
    /// Returns a vector of string_views into the input text. The caller
    /// must keep @p text alive for the lifetime of the returned views.
    ///
    /// @param text Normalized input text.
    /// @return Word spans (never contains empty views).
    [[nodiscard]] virtual std::vector<std::string_view>
    pre_tokenize(std::string_view text) const = 0;
};

/// BERT-style pre-tokenizer that splits on whitespace and punctuation.
///
/// Each punctuation character becomes its own token. Consecutive
/// punctuation characters produce separate tokens. For example:
///   "don't"  -> ["don", "'", "t"]
///   "hello!" -> ["hello", "!"]
///   "a...b"  -> ["a", ".", ".", ".", "b"]
class BertPreTokenizer : public PreTokenizer {
public:
    [[nodiscard]] std::vector<std::string_view> pre_tokenize(std::string_view text) const override;
};

/// Simple pre-tokenizer that splits only on whitespace boundaries.
///
/// Punctuation is kept attached to the surrounding word. For example:
///   "don't"  -> ["don't"]
///   "hello!" -> ["hello!"]
class WhitespaceSplitPreTokenizer : public PreTokenizer {
public:
    [[nodiscard]] std::vector<std::string_view> pre_tokenize(std::string_view text) const override;
};

/// Pass-through pre-tokenizer that returns the entire text as one span.
class NullPreTokenizer : public PreTokenizer {
public:
    [[nodiscard]] std::vector<std::string_view> pre_tokenize(std::string_view text) const override;
};

/// Create a pre-tokenizer from a TokenizerConfig.
///
/// Routes on PreTokenizerType:
///   NONE        -> NullPreTokenizer
///   WHITESPACE  -> WhitespaceSplitPreTokenizer
///   PUNCTUATION -> BertPreTokenizer
[[nodiscard]] std::unique_ptr<PreTokenizer> create_pre_tokenizer(const TokenizerConfig& config);

} // namespace giodb
