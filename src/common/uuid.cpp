#include "sixseven/common/uuid.h"

namespace sixseven {

namespace {

/// Convert a hex character to its 4-bit value. Returns -1 on invalid input.
int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

} // namespace

Result<Uuid> parse_uuid(const std::string& s) {
    // Expected format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (36 chars)
    // Hyphen positions: 8, 13, 18, 23
    if (s.size() != 36) {
        return make_error(StatusCode::TYPE_ERROR,
                          "invalid UUID format: expected 36 characters, got " +
                              std::to_string(s.size()));
    }

    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') {
        return make_error(StatusCode::TYPE_ERROR,
                          "invalid UUID format: hyphens expected at positions 8, 13, 18, 23");
    }

    Uuid uuid{};
    size_t byte_idx = 0;

    for (size_t i = 0; i < 36; ++i) {
        // Skip hyphen positions.
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            continue;
        }

        int hi = hex_digit(s[i]);
        if (hi < 0) {
            return make_error(StatusCode::TYPE_ERROR,
                              "invalid UUID format: non-hex character at position " +
                                  std::to_string(i));
        }

        ++i; // advance to low nibble
        int lo = hex_digit(s[i]);
        if (lo < 0) {
            return make_error(StatusCode::TYPE_ERROR,
                              "invalid UUID format: non-hex character at position " +
                                  std::to_string(i));
        }

        uuid[byte_idx++] = static_cast<uint8_t>((hi << 4) | lo);
    }

    return ok(uuid);
}

std::string to_hex(const uint8_t* data, size_t len) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        const uint8_t byte = data[i];
        out += digits[(byte >> 4) & 0xF];
        out += digits[byte & 0xF];
    }
    return out;
}

std::string format_uuid(const Uuid& uuid) {
    std::string out;
    out.reserve(36);
    for (size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out += '-';
        }
        out += to_hex(&uuid[i], 1);
    }
    return out;
}

} // namespace sixseven
