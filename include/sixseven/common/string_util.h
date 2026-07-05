#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace sixseven {

/// Uppercase a string (ASCII, locale-independent) and return it.
inline std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}

/// Lowercase a string (ASCII, locale-independent) and return it.
inline std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

} // namespace sixseven
