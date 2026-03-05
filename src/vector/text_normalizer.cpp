#include "sixseven/vector/text_normalizer.h"

#include "sixseven/vector/tokenizer.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

namespace sixseven {

namespace {

/// Decode one UTF-8 codepoint from the given position.
/// Returns the codepoint and advances @p pos past the consumed bytes.
/// On invalid sequences, returns U+FFFD and advances by one byte.
uint32_t decode_utf8(const std::string& s, size_t& pos) {
    const auto len = s.size();
    if (pos >= len) {
        return 0;
    }

    auto byte = static_cast<uint8_t>(s[pos]);

    // ASCII (0xxxxxxx).
    if (byte < 0x80) {
        ++pos;
        return byte;
    }

    uint32_t cp = 0;
    size_t extra = 0;

    if ((byte & 0xE0) == 0xC0) {
        // 2-byte (110xxxxx).
        cp = byte & 0x1F;
        extra = 1;
    } else if ((byte & 0xF0) == 0xE0) {
        // 3-byte (1110xxxx).
        cp = byte & 0x0F;
        extra = 2;
    } else if ((byte & 0xF8) == 0xF0) {
        // 4-byte (11110xxx).
        cp = byte & 0x07;
        extra = 3;
    } else {
        // Invalid leading byte.
        ++pos;
        return 0xFFFD;
    }

    if (pos + extra >= len) {
        // Not enough continuation bytes.
        ++pos;
        return 0xFFFD;
    }

    for (size_t i = 1; i <= extra; ++i) {
        auto cont = static_cast<uint8_t>(s[pos + i]);
        if ((cont & 0xC0) != 0x80) {
            ++pos;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cont & 0x3F);
    }

    // Reject overlong encodings (RFC 3629 Section 3).
    // Each byte count has a minimum codepoint value; anything below is overlong.
    static constexpr uint32_t MIN_CP[] = {0, 0x80, 0x800, 0x10000};
    if (cp < MIN_CP[extra]) {
        ++pos;
        return 0xFFFD;
    }

    pos += 1 + extra;
    return cp;
}

/// Encode a Unicode codepoint as UTF-8 and append to @p out.
void encode_utf8(uint32_t cp, std::string& out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

/// Return true if the codepoint is a Unicode combining mark (Mn category).
/// Covers: Combining Diacritical Marks (0300-036F),
///         Combining Diacritical Marks Extended (1AB0-1AFF),
///         Combining Diacritical Marks Supplement (1DC0-1DFF),
///         Combining Diacritical Marks for Symbols (20D0-20FF),
///         Combining Half Marks (FE20-FE2F).
bool is_combining_mark(uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
           (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
           (cp >= 0xFE20 && cp <= 0xFE2F);
}

/// Return true if the codepoint is a Unicode control character.
/// Matches C0 (0000-001F except tab/newline/carriage-return),
/// delete (007F), and C1 (0080-009F).
bool is_control_char(uint32_t cp) {
    if (cp == '\t' || cp == '\n' || cp == '\r') {
        return false; // Whitespace, not control for our purposes.
    }
    return (cp <= 0x001F) || (cp >= 0x007F && cp <= 0x009F);
}

/// Return true if the codepoint is whitespace.
bool is_whitespace(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x00A0 || cp == 0x2000 ||
           cp == 0x2001 || cp == 0x2002 || cp == 0x2003 || cp == 0x2004 || cp == 0x2005 ||
           cp == 0x2006 || cp == 0x2007 || cp == 0x2008 || cp == 0x2009 || cp == 0x200A ||
           cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

/// Return true if the codepoint is a CJK ideograph.
bool is_cjk_character(uint32_t cp) {
    // CJK Unified Ideographs and common extension blocks.
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x20000 && cp <= 0x2A6DF) || (cp >= 0x2A700 && cp <= 0x2B73F) ||
           (cp >= 0x2B740 && cp <= 0x2B81F) || (cp >= 0x2B820 && cp <= 0x2CEAF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x2F800 && cp <= 0x2FA1F);
}

/// NFD-decompose a single codepoint: if the codepoint is a precomposed
/// Latin character with a diacritic, return the base character.
/// For characters outside the handled range, returns the codepoint unchanged.
///
/// Covers Latin-1 Supplement (U+00C0-U+00FF) and Latin Extended-A (U+0100-U+017E).
uint32_t decompose_base(uint32_t cp) {
    // Latin-1 Supplement: uppercase accented (U+00C0-U+00D6).
    // NOLINTBEGIN(readability-magic-numbers)
    switch (cp) {
    // Uppercase accented Latin-1.
    case 0x00C0:
    case 0x00C1:
    case 0x00C2:
    case 0x00C3:
    case 0x00C4:
    case 0x00C5:
        return 'A';
    case 0x00C7:
        return 'C';
    case 0x00C8:
    case 0x00C9:
    case 0x00CA:
    case 0x00CB:
        return 'E';
    case 0x00CC:
    case 0x00CD:
    case 0x00CE:
    case 0x00CF:
        return 'I';
    case 0x00D0:
        return 'D'; // Eth -> D.
    case 0x00D1:
        return 'N';
    case 0x00D2:
    case 0x00D3:
    case 0x00D4:
    case 0x00D5:
    case 0x00D6:
    case 0x00D8: // O with stroke.
        return 'O';
    case 0x00D9:
    case 0x00DA:
    case 0x00DB:
    case 0x00DC:
        return 'U';
    case 0x00DD:
        return 'Y';

    // Lowercase accented Latin-1.
    case 0x00E0:
    case 0x00E1:
    case 0x00E2:
    case 0x00E3:
    case 0x00E4:
    case 0x00E5:
        return 'a';
    case 0x00E7:
        return 'c';
    case 0x00E8:
    case 0x00E9:
    case 0x00EA:
    case 0x00EB:
        return 'e';
    case 0x00EC:
    case 0x00ED:
    case 0x00EE:
    case 0x00EF:
        return 'i';
    case 0x00F0:
        return 'd'; // eth -> d.
    case 0x00F1:
        return 'n';
    case 0x00F2:
    case 0x00F3:
    case 0x00F4:
    case 0x00F5:
    case 0x00F6:
    case 0x00F8: // o with stroke.
        return 'o';
    case 0x00F9:
    case 0x00FA:
    case 0x00FB:
    case 0x00FC:
        return 'u';
    case 0x00FD:
    case 0x00FF:
        return 'y';

    // Latin Extended-A.
    case 0x0100:
    case 0x0102:
    case 0x0104:
        return 'A';
    case 0x0101:
    case 0x0103:
    case 0x0105:
        return 'a';
    case 0x0106:
    case 0x0108:
    case 0x010A:
    case 0x010C:
        return 'C';
    case 0x0107:
    case 0x0109:
    case 0x010B:
    case 0x010D:
        return 'c';
    case 0x010E:
    case 0x0110:
        return 'D';
    case 0x010F:
    case 0x0111:
        return 'd';
    case 0x0112:
    case 0x0114:
    case 0x0116:
    case 0x0118:
    case 0x011A:
        return 'E';
    case 0x0113:
    case 0x0115:
    case 0x0117:
    case 0x0119:
    case 0x011B:
        return 'e';
    case 0x011C:
    case 0x011E:
    case 0x0120:
    case 0x0122:
        return 'G';
    case 0x011D:
    case 0x011F:
    case 0x0121:
    case 0x0123:
        return 'g';
    case 0x0124:
    case 0x0126:
        return 'H';
    case 0x0125:
    case 0x0127:
        return 'h';
    case 0x0128:
    case 0x012A:
    case 0x012C:
    case 0x012E:
    case 0x0130:
        return 'I';
    case 0x0129:
    case 0x012B:
    case 0x012D:
    case 0x012F:
    case 0x0131:
        return 'i';
    case 0x0134:
        return 'J';
    case 0x0135:
        return 'j';
    case 0x0136:
        return 'K';
    case 0x0137:
        return 'k';
    case 0x0139:
    case 0x013B:
    case 0x013D:
    case 0x013F:
    case 0x0141:
        return 'L';
    case 0x013A:
    case 0x013C:
    case 0x013E:
    case 0x0140:
    case 0x0142:
        return 'l';
    case 0x0143:
    case 0x0145:
    case 0x0147:
        return 'N';
    case 0x0144:
    case 0x0146:
    case 0x0148:
        return 'n';
    case 0x014C:
    case 0x014E:
    case 0x0150:
        return 'O';
    case 0x014D:
    case 0x014F:
    case 0x0151:
        return 'o';
    case 0x0154:
    case 0x0156:
    case 0x0158:
        return 'R';
    case 0x0155:
    case 0x0157:
    case 0x0159:
        return 'r';
    case 0x015A:
    case 0x015C:
    case 0x015E:
    case 0x0160:
        return 'S';
    case 0x015B:
    case 0x015D:
    case 0x015F:
    case 0x0161:
        return 's';
    case 0x0162:
    case 0x0164:
    case 0x0166:
        return 'T';
    case 0x0163:
    case 0x0165:
    case 0x0167:
        return 't';
    case 0x0168:
    case 0x016A:
    case 0x016C:
    case 0x016E:
    case 0x0170:
    case 0x0172:
        return 'U';
    case 0x0169:
    case 0x016B:
    case 0x016D:
    case 0x016F:
    case 0x0171:
    case 0x0173:
        return 'u';
    case 0x0174:
        return 'W';
    case 0x0175:
        return 'w';
    case 0x0176:
    case 0x0178:
        return 'Y';
    case 0x0177:
        return 'y';
    case 0x0179:
    case 0x017B:
    case 0x017D:
        return 'Z';
    case 0x017A:
    case 0x017C:
    case 0x017E:
        return 'z';

    default:
        return cp; // No decomposition available.
    }
    // NOLINTEND(readability-magic-numbers)
}

/// Lowercase a Unicode codepoint.
/// Handles ASCII, Latin-1 Supplement (U+00C0-U+00DE), and Latin Extended-A (U+0100-U+017E).
uint32_t to_lower(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') {
        return cp + 32;
    }
    // Latin-1 Supplement uppercase (except U+00D7 multiplication sign).
    if (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7) {
        return cp + 32;
    }
    // Latin Extended-A: three sub-ranges with different even/odd patterns.
    // 0x0100-0x0137: even = uppercase, odd = lowercase.
    if (cp >= 0x0100 && cp <= 0x0137 && (cp % 2 == 0)) {
        return cp + 1;
    }
    // 0x0138 (ĸ): standalone lowercase character, no uppercase form.
    // 0x0139-0x0148: odd = uppercase, even = lowercase.
    if (cp >= 0x0139 && cp <= 0x0148 && (cp % 2 == 1)) {
        return cp + 1;
    }
    // 0x014A-0x0177: even = uppercase, odd = lowercase.
    if (cp >= 0x014A && cp <= 0x0177 && (cp % 2 == 0)) {
        return cp + 1;
    }
    // 0x0178 (Ÿ): uppercase, lowercase is U+00FF (ÿ).
    if (cp == 0x0178) {
        return 0x00FF;
    }
    // 0x0179-0x017E: odd = uppercase, even = lowercase.
    if (cp >= 0x0179 && cp <= 0x017E && (cp % 2 == 1)) {
        return cp + 1;
    }
    return cp;
}

} // namespace

// --- BertNormalizer ---

BertNormalizer::BertNormalizer(bool lowercase,
                               bool strip_accents,
                               bool handle_chinese,
                               bool clean_text)
    : lowercase_(lowercase), strip_accents_(strip_accents), handle_chinese_(handle_chinese),
      clean_text_(clean_text) {}

std::string BertNormalizer::normalize(const std::string& text) const {
    std::string result;
    result.reserve(text.size());

    size_t pos = 0;
    bool prev_was_space = false;

    while (pos < text.size()) {
        uint32_t cp = decode_utf8(text, pos);

        // Strip control characters.
        if (clean_text_ && is_control_char(cp)) {
            continue;
        }

        // Strip accents: decompose precomposed characters and skip combining marks.
        if (strip_accents_) {
            if (is_combining_mark(cp)) {
                continue;
            }
            cp = decompose_base(cp);
        }

        // Lowercase.
        if (lowercase_) {
            cp = to_lower(cp);
        }

        // CJK handling: add spaces around CJK ideographs.
        if (handle_chinese_ && is_cjk_character(cp)) {
            // Collapse any trailing space before CJK.
            if (!result.empty() && result.back() != ' ') {
                result.push_back(' ');
            }
            prev_was_space = false;
            encode_utf8(cp, result);
            result.push_back(' ');
            prev_was_space = true;
            continue;
        }

        // Whitespace collapsing.
        if (clean_text_ && is_whitespace(cp)) {
            if (!prev_was_space) {
                result.push_back(' ');
                prev_was_space = true;
            }
            continue;
        }

        prev_was_space = false;
        encode_utf8(cp, result);
    }

    // Trim leading and trailing whitespace (only when clean_text is enabled).
    if (clean_text_) {
        size_t start = 0;
        while (start < result.size() && result[start] == ' ') {
            ++start;
        }
        size_t end = result.size();
        while (end > start && result[end - 1] == ' ') { // NOLINT(bugprone-infinite-loop)
            --end;
        }
        return result.substr(start, end - start);
    }

    return result;
}

// --- LowercaseNormalizer ---

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::string LowercaseNormalizer::normalize(const std::string& text) const {
    std::string result;
    result.reserve(text.size());

    size_t pos = 0;
    while (pos < text.size()) {
        uint32_t cp = decode_utf8(text, pos);
        encode_utf8(to_lower(cp), result);
    }

    return result;
}

// --- NullNormalizer ---

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::string NullNormalizer::normalize(const std::string& text) const {
    return text;
}

// --- Factory ---

std::unique_ptr<TextNormalizer> create_normalizer(const TokenizerConfig& config) {
    // NOLINTBEGIN(bugprone-branch-clone)
    switch (config.normalizer) {
    case NormalizerType::BERT:
        return std::make_unique<BertNormalizer>(config.normalizer_lowercase,
                                                config.normalizer_strip_accents);
    case NormalizerType::LOWERCASE:
        return std::make_unique<LowercaseNormalizer>();
    case NormalizerType::NONE:
    case NormalizerType::NFC:
        return std::make_unique<NullNormalizer>();
    }
    // NOLINTEND(bugprone-branch-clone)
    return std::make_unique<NullNormalizer>(); // Unreachable, satisfies compiler.
}

} // namespace sixseven
