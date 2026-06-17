#pragma once

// ---------------------------------------------------------------------------
// index_encoding.h — shared little-endian byte-packing helpers for index
// persistence files (btree_persistence, hash_persistence, bm25_index).
//
// Writer: free functions that append to a std::vector<uint8_t>.
// Reader: bounds-checked cursor over a std::span<const uint8_t>; every read
//         returns a Result<T> so callers can propagate truncation/corruption
//         errors instead of reading past the end of the buffer.
// ---------------------------------------------------------------------------

#include "sixseven/common/result.h"
#include "sixseven/common/status.h"

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace sixseven {
namespace index_encoding {

// ---------------------------------------------------------------------------
// Writer helpers — append-only, little-endian
// ---------------------------------------------------------------------------

inline void write_u8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}

inline void write_u16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >> 8));
}

inline void write_u32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v >> 16));
    buf.push_back(static_cast<uint8_t>(v >> 24));
}

inline void write_u64(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
}

inline void write_double(std::vector<uint8_t>& buf, double v) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    write_u64(buf, bits);
}

// ---------------------------------------------------------------------------
// Reader — bounds-checked cursor over a byte span
//
// Every read method returns Result<T>.  On a short buffer the Result holds an
// IO_ERROR; the cursor position is not advanced on failure so the caller can
// inspect where the truncation occurred.
// ---------------------------------------------------------------------------

struct Reader {
    std::span<const uint8_t> data;
    size_t pos = 0;

    explicit Reader(std::span<const uint8_t> d) : data(d) {}

    /// Remaining bytes in the buffer.
    [[nodiscard]] size_t remaining() const { return pos < data.size() ? data.size() - pos : 0; }

    /// True iff all bytes have been consumed.
    [[nodiscard]] bool at_end() const { return pos >= data.size(); }

    [[nodiscard]] Result<uint8_t> read_u8() {
        if (pos + 1 > data.size()) {
            return make_error(StatusCode::IO_ERROR,
                              "index_encoding: truncated buffer reading u8 at offset " +
                                  std::to_string(pos));
        }
        return ok(data[pos++]);
    }

    [[nodiscard]] Result<uint16_t> read_u16() {
        if (pos + 2 > data.size()) {
            return make_error(StatusCode::IO_ERROR,
                              "index_encoding: truncated buffer reading u16 at offset " +
                                  std::to_string(pos));
        }
        uint16_t v = static_cast<uint16_t>(data[pos]) | (static_cast<uint16_t>(data[pos + 1]) << 8);
        pos += 2;
        return ok(v);
    }

    [[nodiscard]] Result<uint32_t> read_u32() {
        if (pos + 4 > data.size()) {
            return make_error(StatusCode::IO_ERROR,
                              "index_encoding: truncated buffer reading u32 at offset " +
                                  std::to_string(pos));
        }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<uint32_t>(data[pos + static_cast<size_t>(i)]) << (8 * i);
        }
        pos += 4;
        return ok(v);
    }

    [[nodiscard]] Result<uint64_t> read_u64() {
        if (pos + 8 > data.size()) {
            return make_error(StatusCode::IO_ERROR,
                              "index_encoding: truncated buffer reading u64 at offset " +
                                  std::to_string(pos));
        }
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<uint64_t>(data[pos + static_cast<size_t>(i)]) << (8 * i);
        }
        pos += 8;
        return ok(v);
    }

    [[nodiscard]] Result<double> read_double() {
        auto bits_r = read_u64();
        if (!bits_r) {
            return make_error(bits_r.error().code, bits_r.error().message);
        }
        double v = 0.0;
        std::memcpy(&v, &*bits_r, sizeof(v));
        return ok(v);
    }

    /// Read exactly `n` bytes as a std::string.
    [[nodiscard]] Result<std::string> read_bytes(size_t n) {
        if (pos + n > data.size()) {
            return make_error(StatusCode::IO_ERROR,
                              "index_encoding: truncated buffer reading " + std::to_string(n) +
                                  " bytes at offset " + std::to_string(pos));
        }
        std::string s(reinterpret_cast<const char*>(data.data() + pos), n);
        pos += n;
        return ok(std::move(s));
    }

    /// Peek at the current byte without advancing.
    [[nodiscard]] Result<uint8_t> peek_u8() const {
        if (pos >= data.size()) {
            return make_error(StatusCode::IO_ERROR,
                              "index_encoding: truncated buffer peeking u8 at offset " +
                                  std::to_string(pos));
        }
        return ok(data[pos]);
    }

    /// Return a raw pointer to the current position (for callers that
    /// need to pass a const uint8_t* / end pair to deserialize()).
    [[nodiscard]] const uint8_t* ptr() const { return data.data() + pos; }

    /// Return a raw pointer one past the last byte (the end sentinel).
    [[nodiscard]] const uint8_t* end_ptr() const { return data.data() + data.size(); }

    /// Advance by `n` bytes — fails if there are not enough bytes remaining.
    [[nodiscard]] Result<void> skip(size_t n) {
        if (pos + n > data.size()) {
            return make_error(StatusCode::IO_ERROR,
                              "index_encoding: truncated buffer skipping " + std::to_string(n) +
                                  " bytes at offset " + std::to_string(pos));
        }
        pos += n;
        return ok();
    }

    /// After deserialization via ptr()/end_ptr(), sync the cursor forward by
    /// the number of bytes consumed externally.
    void advance(size_t n) { pos += n; }
};

} // namespace index_encoding
} // namespace sixseven
