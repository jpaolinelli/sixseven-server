#pragma once

#include "giodb/vector/pre_tokenizer.h"
#include "giodb/vector/text_normalizer.h"
#include "giodb/vector/tokenizer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace giodb {

/// Byte-Pair Encoding tokenizer (GPT-2/RoBERTa style).
///
/// Implements the BPE subword algorithm used by GPT-2, RoBERTa, and
/// multilingual embedding models like nomic-embed-text. Supports
/// byte-level BPE with the GPT-2 byte-to-unicode mapping and the
/// space-prefix convention (space byte maps to U+0120).
///
/// Full pipeline: normalize -> pre-tokenize -> byte-encode ->
/// BPE merge -> vocab lookup -> add CLS/SEP -> pad/truncate.
class BPETokenizer : public Tokenizer {
public:
    /// Construct from a TokenizerConfig.
    ///
    /// Reads vocabulary, special token IDs, merge rules, normalizer type,
    /// and pre-tokenizer type from the config.
    explicit BPETokenizer(const TokenizerConfig& config);

    [[nodiscard]] std::vector<int64_t> encode(const std::string& text,
                                              size_t max_length) const override;

    [[nodiscard]] std::vector<int64_t>
    attention_mask(const std::vector<int64_t>& tokens) const override;

    [[nodiscard]] size_t vocab_size() const override;
    [[nodiscard]] size_t max_sequence_length() const override;

private:
    /// Apply BPE merges to a single word (already byte-encoded to unicode).
    ///
    /// Returns token IDs for the subword decomposition. Unknown subwords
    /// that are not in the vocabulary map to the UNK token ID.
    [[nodiscard]] std::vector<int64_t> tokenize_word(const std::string& word) const;

    /// Build the GPT-2 byte-to-unicode mapping table.
    ///
    /// Maps each byte value (0-255) to a UTF-8 string representing the
    /// corresponding Unicode character. Printable ASCII and Latin-1
    /// supplement characters map to themselves; remaining bytes map to
    /// codepoints starting at U+0100.
    [[nodiscard]] static std::unordered_map<uint8_t, std::string> build_byte_to_unicode();

    /// Convert raw bytes to BPE's unicode representation using the
    /// byte-to-unicode mapping.
    [[nodiscard]] std::string bytes_to_unicode(const std::string& input) const;

    /// Split a UTF-8 string into individual Unicode codepoints, each
    /// returned as a separate UTF-8 string.
    [[nodiscard]] static std::vector<std::string> split_utf8(const std::string& s);

    /// Encode a Unicode codepoint as a UTF-8 string.
    [[nodiscard]] static std::string codepoint_to_utf8(uint32_t cp);

    TokenizerConfig config_;
    std::unique_ptr<TextNormalizer> normalizer_;
    std::unique_ptr<PreTokenizer> pre_tokenizer_;

    /// Merge rules indexed by "left right" string key. Value is the rank
    /// (lower = higher priority).
    std::unordered_map<std::string, size_t> merge_ranks_;

    /// Byte-to-unicode mapping (each byte value -> UTF-8 encoded character).
    std::unordered_map<uint8_t, std::string> byte_to_unicode_;
};

} // namespace giodb
