#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace sixseven {

/// Write a value to a byte buffer at the given offset using memcpy (safe,
/// unaligned-friendly, native byte order). Advances offset by sizeof(T).
///
/// This is the shared byte-order contract for on-disk WAL records
/// (src/storage/wal_record.cpp) and on-wire replication messages
/// (src/server/replication_message.cpp). Both formats rely on native-endian
/// memcpy (not an explicit little-endian encoding), so this helper must not
/// be changed to normalize endianness without a coordinated format migration
/// for both persistence layers.
template <typename T>
void write_native(std::vector<uint8_t>& buf, size_t& offset, T value) {
    std::memcpy(buf.data() + offset, &value, sizeof(T));
    offset += sizeof(T);
}

/// Read a value from a byte span at the given offset using memcpy (native
/// byte order). Advances offset by sizeof(T). Caller must ensure
/// offset + sizeof(T) <= buf.size().
///
/// See write_native() above: this is shared between the WAL and replication
/// wire formats and must remain byte-for-byte compatible with both.
template <typename T>
T read_native(std::span<const uint8_t> buf, size_t& offset) {
    T value{};
    std::memcpy(&value, buf.data() + offset, sizeof(T));
    offset += sizeof(T);
    return value;
}

} // namespace sixseven
