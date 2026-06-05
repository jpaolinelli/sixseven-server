#include "sixseven/index/bm25_analyzer.h"

#include <cctype>

namespace sixseven {

namespace {

/// Returns true if c is an ASCII character that should split tokens.
/// Anything that is not an ASCII alphanumeric is a separator, EXCEPT bytes with
/// the high bit set (UTF-8 continuation/lead bytes) which are kept so that
/// multi-byte words are not shredded into single bytes.
bool is_separator(unsigned char c) {
    if (c >= 0x80) {
        return false; // part of a UTF-8 multi-byte sequence: keep.
    }
    return std::isalnum(c) == 0;
}

unsigned char ascii_lower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<unsigned char>(c - 'A' + 'a');
    }
    return c;
}

// --- Porter stemmer ---------------------------------------------------------
//
// Classic Porter (1980) stemming algorithm for English. Operates on lowercase
// ASCII tokens. This is a faithful, compact implementation; it is intentionally
// conservative and deterministic so index-time and query-time stems agree.

bool is_consonant(const std::string& s, size_t i) {
    const char c = s[i];
    if (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u') {
        return false;
    }
    if (c == 'y') {
        // 'y' is a consonant unless preceded by a consonant.
        return i == 0 || !is_consonant(s, i - 1);
    }
    return true;
}

/// Measure m: the number of vowel-consonant sequences in [0, len).
int measure(const std::string& s) {
    int m = 0;
    size_t i = 0;
    const size_t len = s.size();
    // Skip leading consonants.
    while (i < len && is_consonant(s, i)) {
        ++i;
    }
    while (i < len) {
        // Skip vowels.
        while (i < len && !is_consonant(s, i)) {
            ++i;
        }
        if (i >= len) {
            break;
        }
        ++m;
        // Skip consonants.
        while (i < len && is_consonant(s, i)) {
            ++i;
        }
    }
    return m;
}

bool has_vowel(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (!is_consonant(s, i)) {
            return true;
        }
    }
    return false;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// True if the word ends with a double consonant.
bool ends_double_consonant(const std::string& s) {
    if (s.size() < 2) {
        return false;
    }
    const size_t n = s.size();
    return s[n - 1] == s[n - 2] && is_consonant(s, n - 1);
}

/// True if the word ends consonant-vowel-consonant where the final consonant
/// is not w, x, or y (the "*o" condition in Porter's paper).
bool ends_cvc(const std::string& s) {
    if (s.size() < 3) {
        return false;
    }
    const size_t n = s.size();
    if (!is_consonant(s, n - 3) || is_consonant(s, n - 2) || !is_consonant(s, n - 1)) {
        return false;
    }
    const char c = s[n - 1];
    return c != 'w' && c != 'x' && c != 'y';
}

/// Replace the suffix `from` with `to` if the stem (word minus `from`) has
/// measure satisfying min_m. Returns true if a replacement occurred.
bool replace_suffix(std::string& s, const std::string& from, const std::string& to, int min_m) {
    if (!ends_with(s, from)) {
        return false;
    }
    const std::string stem = s.substr(0, s.size() - from.size());
    if (measure(stem) > min_m) {
        s = stem + to;
        return true;
    }
    return false;
}

void porter_step1a(std::string& s) {
    if (ends_with(s, "sses")) {
        s.erase(s.size() - 2); // sses -> ss
    } else if (ends_with(s, "ies")) {
        s.erase(s.size() - 2); // ies -> i
    } else if (ends_with(s, "ss")) {
        // ss -> ss (no change)
    } else if (ends_with(s, "s")) {
        s.erase(s.size() - 1); // s -> (empty)
    }
}

void porter_step1b(std::string& s) {
    bool flag = false;
    if (ends_with(s, "eed")) {
        const std::string stem = s.substr(0, s.size() - 3);
        if (measure(stem) > 0) {
            s = stem + "ee";
        }
        return;
    }
    if (ends_with(s, "ed")) {
        const std::string stem = s.substr(0, s.size() - 2);
        if (has_vowel(stem)) {
            s = stem;
            flag = true;
        }
    } else if (ends_with(s, "ing")) {
        const std::string stem = s.substr(0, s.size() - 3);
        if (has_vowel(stem)) {
            s = stem;
            flag = true;
        }
    }
    if (flag) {
        if (ends_with(s, "at") || ends_with(s, "bl") || ends_with(s, "iz")) {
            s += "e";
        } else if (ends_double_consonant(s)) {
            const char c = s.back();
            if (c != 'l' && c != 's' && c != 'z') {
                s.erase(s.size() - 1);
            }
        } else if (measure(s) == 1 && ends_cvc(s)) {
            s += "e";
        }
    }
}

void porter_step1c(std::string& s) {
    if (ends_with(s, "y")) {
        const std::string stem = s.substr(0, s.size() - 1);
        if (has_vowel(stem)) {
            s = stem + "i";
        }
    }
}

void porter_step2(std::string& s) {
    static const std::pair<const char*, const char*> kRules[] = {
        {"ational", "ate"}, {"tional", "tion"}, {"enci", "ence"},   {"anci", "ance"},
        {"izer", "ize"},    {"bli", "ble"},     {"alli", "al"},     {"entli", "ent"},
        {"eli", "e"},       {"ousli", "ous"},   {"ization", "ize"}, {"ation", "ate"},
        {"ator", "ate"},    {"alism", "al"},    {"iveness", "ive"}, {"fulness", "ful"},
        {"ousness", "ous"}, {"aliti", "al"},    {"iviti", "ive"},   {"biliti", "ble"},
        {"logi", "log"},
    };
    for (const auto& [from, to] : kRules) {
        if (replace_suffix(s, from, to, 0)) {
            return;
        }
    }
}

void porter_step3(std::string& s) {
    static const std::pair<const char*, const char*> kRules[] = {
        {"icate", "ic"},
        {"ative", ""},
        {"alize", "al"},
        {"iciti", "ic"},
        {"ical", "ic"},
        {"ful", ""},
        {"ness", ""},
    };
    for (const auto& [from, to] : kRules) {
        if (replace_suffix(s, from, to, 0)) {
            return;
        }
    }
}

void porter_step4(std::string& s) {
    static const char* const kSuffixes[] = {
        "al",
        "ance",
        "ence",
        "er",
        "ic",
        "able",
        "ible",
        "ant",
        "ement",
        "ment",
        "ent",
        "ou",
        "ism",
        "ate",
        "iti",
        "ous",
        "ive",
        "ize",
    };
    for (const char* suffix : kSuffixes) {
        if (ends_with(s, suffix)) {
            const std::string stem = s.substr(0, s.size() - std::string(suffix).size());
            if (measure(stem) > 1) {
                s = stem;
            }
            return;
        }
    }
    // Special case: "ion" only when preceded by 's' or 't'.
    if (ends_with(s, "ion")) {
        const std::string stem = s.substr(0, s.size() - 3);
        if (measure(stem) > 1 && !stem.empty() && (stem.back() == 's' || stem.back() == 't')) {
            s = stem;
        }
    }
}

void porter_step5(std::string& s) {
    // Step 5a: remove trailing 'e'.
    if (ends_with(s, "e")) {
        const std::string stem = s.substr(0, s.size() - 1);
        const int m = measure(stem);
        if (m > 1 || (m == 1 && !ends_cvc(stem))) {
            s = stem;
        }
    }
    // Step 5b: collapse double 'l' when measure > 1.
    if (s.size() >= 2 && s.back() == 'l' && ends_double_consonant(s) && measure(s) > 1) {
        s.erase(s.size() - 1);
    }
}

} // namespace

const std::unordered_set<std::string>& default_english_stopwords() {
    static const std::unordered_set<std::string> kStopwords = {
        "a",    "an",    "and",  "are",   "as",   "at",    "be",    "but",   "by",     "for",
        "if",   "in",    "into", "is",    "it",   "no",    "not",   "of",    "on",     "or",
        "such", "that",  "the",  "their", "then", "there", "these", "they",  "this",   "to",
        "was",  "will",  "with", "from",  "have", "has",   "had",   "he",    "she",    "we",
        "you",  "i",     "do",   "does",  "did",  "can",   "could", "would", "should", "were",
        "been", "being", "than", "too",   "very", "just",  "also",  "about", "over",   "under",
    };
    return kStopwords;
}

Bm25Analyzer::Bm25Analyzer(Bm25AnalyzerConfig config) : config_(std::move(config)) {}

std::string Bm25Analyzer::porter_stem(const std::string& token) {
    // Porter's algorithm is a no-op on very short words.
    if (token.size() <= 2) {
        return token;
    }
    std::string s = token;
    porter_step1a(s);
    porter_step1b(s);
    porter_step1c(s);
    porter_step2(s);
    porter_step3(s);
    porter_step4(s);
    porter_step5(s);
    return s;
}

std::vector<std::string> Bm25Analyzer::analyze(const std::string& text) const {
    std::vector<std::string> terms;
    const std::unordered_set<std::string>& stopwords =
        (config_.remove_stopwords && config_.stopwords.empty()) ? default_english_stopwords()
                                                                : config_.stopwords;

    std::string token;
    token.reserve(32);

    auto flush = [&]() {
        if (token.empty()) {
            return;
        }
        if (config_.min_token_length != 0 && token.size() < config_.min_token_length) {
            token.clear();
            return;
        }
        if (config_.remove_stopwords && stopwords.count(token) != 0) {
            token.clear();
            return;
        }
        if (config_.stem) {
            token = porter_stem(token);
            // A token can become a stop word after stemming; re-check.
            if (config_.remove_stopwords && stopwords.count(token) != 0) {
                token.clear();
                return;
            }
        }
        if (!token.empty()) {
            terms.push_back(token);
        }
        token.clear();
    };

    for (char ch : text) {
        auto c = static_cast<unsigned char>(ch);
        if (is_separator(c)) {
            flush();
            continue;
        }
        token.push_back(static_cast<char>(config_.lowercase ? ascii_lower(c) : c));
    }
    flush();

    return terms;
}

} // namespace sixseven
