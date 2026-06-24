#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sixseven {

/// Type of tokenizer model (subword algorithm).
enum class TokenizerModelType {
    HASH,      ///< Simple hash-based tokenization (no real subwords).
    WORDPIECE, ///< WordPiece (BERT-style).
    BPE,       ///< Byte-Pair Encoding (GPT-style).
    UNIGRAM,   ///< Unigram (SentencePiece-style).
};

/// Text normalizer applied before tokenization.
enum class NormalizerType {
    NONE,      ///< No normalization.
    LOWERCASE, ///< Lowercase all characters.
    NFC,       ///< Unicode NFC normalization (ICU-backed canonical composition).
    NFKC,      ///< Unicode NFKC normalization (ICU-backed compatibility composition).
    BERT,      ///< BERT normalization (lowercase, accent strip, cleanup, CJK spacing).
};

/// Pre-tokenizer that splits text before subword tokenization.
enum class PreTokenizerType {
    NONE,        ///< No pre-tokenization (pass text as-is).
    WHITESPACE,  ///< Split on whitespace boundaries.
    PUNCTUATION, ///< Split on whitespace and punctuation.
};

/// IDs for special tokens used by the tokenizer.
struct SpecialTokenIds {
    int64_t pad = 0;    ///< Padding token.
    int64_t unk = 100;  ///< Unknown token.
    int64_t cls = 101;  ///< [CLS] sentence start.
    int64_t sep = 102;  ///< [SEP] sentence end / separator.
    int64_t mask = 103; ///< [MASK] for masked language modeling.
};

/// Configuration for constructing a tokenizer.
///
/// Holds the vocabulary mapping, special token IDs, model type,
/// normalizer type, pre-tokenizer type, and subword prefix.
struct TokenizerConfig {
    /// Vocabulary: token string → token ID.
    std::unordered_map<std::string, int64_t> vocab;

    /// Special token IDs.
    SpecialTokenIds special_tokens;

    /// Subword model type.
    TokenizerModelType model_type = TokenizerModelType::HASH;

    /// Text normalizer.
    NormalizerType normalizer = NormalizerType::LOWERCASE;

    /// Pre-tokenizer.
    PreTokenizerType pre_tokenizer = PreTokenizerType::PUNCTUATION;

    /// Prefix for continuation subword tokens (e.g., "##" for WordPiece).
    std::string subword_prefix;

    /// BPE merge rules in priority order (only used for BPE models).
    std::vector<std::string> merges;

    /// Whether the normalizer lowercases text.
    bool normalizer_lowercase = true;

    /// Whether the normalizer strips accents.
    bool normalizer_strip_accents = false;
};

/// Abstract base class for text tokenizers.
///
/// Converts raw text into integer token IDs suitable for model input.
/// Implementations handle vocabulary lookup, special token insertion
/// (CLS/SEP), padding, and truncation.
class Tokenizer {
public:
    virtual ~Tokenizer() = default;

    /// Encode text into a fixed-length sequence of token IDs.
    ///
    /// The returned vector has exactly @p max_length elements, padded
    /// with the PAD token or truncated as needed. The sequence starts
    /// with CLS and ends with SEP (before padding).
    ///
    /// @param text       Input text to tokenize.
    /// @param max_length Maximum sequence length (including special tokens).
    /// @return Token ID vector of exactly max_length elements.
    [[nodiscard]] virtual std::vector<int64_t> encode(const std::string& text,
                                                      size_t max_length) const = 0;

    /// Build an attention mask from token IDs.
    ///
    /// Returns 1 for non-padding positions and 0 for padding positions.
    ///
    /// @param tokens Token ID sequence (from encode()).
    /// @return Attention mask of the same length as tokens.
    [[nodiscard]] virtual std::vector<int64_t>
    attention_mask(const std::vector<int64_t>& tokens) const = 0;

    /// Return the vocabulary size.
    [[nodiscard]] virtual size_t vocab_size() const = 0;

    /// Return the maximum supported sequence length.
    [[nodiscard]] virtual size_t max_sequence_length() const = 0;
};

/// Hash-based tokenizer that maps words to stable integer IDs.
///
/// Splits text on whitespace/punctuation, lowercases each word, and
/// hashes it to an integer in [VOCAB_OFFSET, VOCAB_OFFSET + VOCAB_SIZE).
/// Adds CLS at start, SEP at end, pads with PAD.
///
/// This is a simple tokenizer suitable for local ONNX inference where
/// exact pretrained tokenization is not required.
class HashTokenizer : public Tokenizer {
public:
    /// Hash vocabulary size (number of distinct word buckets).
    static constexpr size_t VOCAB_SIZE = 30000;

    /// Offset added to hash values to avoid special token ID range.
    static constexpr int64_t VOCAB_OFFSET = 104;

    /// Default maximum sequence length.
    static constexpr size_t DEFAULT_MAX_SEQ_LENGTH = 128;

    /// Construct with default settings.
    explicit HashTokenizer(size_t max_seq_length = DEFAULT_MAX_SEQ_LENGTH);

    [[nodiscard]] std::vector<int64_t> encode(const std::string& text,
                                              size_t max_length) const override;

    [[nodiscard]] std::vector<int64_t>
    attention_mask(const std::vector<int64_t>& tokens) const override;

    [[nodiscard]] size_t vocab_size() const override;
    [[nodiscard]] size_t max_sequence_length() const override;

private:
    size_t max_seq_length_;
    SpecialTokenIds special_tokens_;
};

} // namespace sixseven
