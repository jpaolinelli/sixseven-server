#pragma once

#include <memory>
#include <string>

namespace sixseven {

struct TokenizerConfig;

/// Abstract base class for text normalization before tokenization.
///
/// Normalizers preprocess raw text (lowercasing, accent stripping,
/// whitespace cleanup, etc.) before it is split into tokens.
class TextNormalizer { // NOLINT(cppcoreguidelines-special-member-functions)
public:
    virtual ~TextNormalizer() = default;

    /// Normalize the input text and return the result.
    [[nodiscard]] virtual std::string normalize(const std::string& text) const = 0;
};

/// BERT-style normalizer supporting multiple normalization steps.
///
/// Optionally performs: lowercase conversion, Unicode accent/diacritic
/// stripping (NFD decomposition + combining mark removal), control
/// character removal, whitespace collapsing, and CJK character spacing.
class BertNormalizer : public TextNormalizer {
public:
    /// Construct a BertNormalizer with the given options.
    ///
    /// @param lowercase          Convert text to lowercase.
    /// @param strip_accents      Remove Unicode diacritics (e.g. e with acute -> e).
    /// @param handle_chinese     Add spaces around CJK ideographs.
    /// @param clean_text         Remove control characters and collapse whitespace.
    explicit BertNormalizer(bool lowercase = true,
                            bool strip_accents = false,
                            bool handle_chinese = true,
                            bool clean_text = true);

    [[nodiscard]] std::string normalize(const std::string& text) const override;

private:
    bool lowercase_;
    bool strip_accents_;
    bool handle_chinese_;
    bool clean_text_;
};

/// Simple normalizer that only lowercases text.
class LowercaseNormalizer : public TextNormalizer {
public:
    [[nodiscard]] std::string normalize(const std::string& text) const override;
};

/// Pass-through normalizer that returns input unchanged.
class NullNormalizer : public TextNormalizer {
public:
    [[nodiscard]] std::string normalize(const std::string& text) const override;
};

/// ICU-backed Unicode normalizer supporting NFC and NFKC forms.
///
/// On Windows uses the Windows SDK ICU C API (unorm2_getNFCInstance /
/// unorm2_getNFKCInstance) with a UTF-8 <-> UTF-16 round-trip.
/// On other platforms uses the full ICU C++ API (icu::Normalizer2 singletons).
/// In both cases the normalizer instance is owned by the ICU runtime.
class IcuNormalizer : public TextNormalizer {
public:
    /// Unicode normalization form.
    enum class Form { NFC, NFKC };

    /// Construct an IcuNormalizer for the given normalization form.
    explicit IcuNormalizer(Form form);

    [[nodiscard]] std::string normalize(const std::string& text) const override;

private:
    Form form_;
};

/// Create a normalizer from a TokenizerConfig.
///
/// Routes on NormalizerType:
///   NONE      -> NullNormalizer
///   LOWERCASE -> LowercaseNormalizer
///   BERT      -> BertNormalizer (with config's lowercase/strip_accents flags)
///   NFC       -> IcuNormalizer(NFC)  — ICU canonical composition
///   NFKC      -> IcuNormalizer(NFKC) — ICU compatibility composition
[[nodiscard]] std::unique_ptr<TextNormalizer> create_normalizer(const TokenizerConfig& config);

} // namespace sixseven
