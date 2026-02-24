#pragma once

#include "giodb/index/rid.h"
#include "giodb/storage/wal_record.h"

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace giodb {

/// MVCC tuple header prepended to user data on heap pages.
///
/// Layout (24 bytes, fixed size):
/// ```
///   [xmin:      uint64_t]   — creating transaction ID
///   [xmax:      uint64_t]   — deleting transaction ID (0 = not deleted)
///   [t_ctid_page: uint32_t] — version chain: next version page ID
///   [t_ctid_slot: uint16_t] — version chain: next version slot ID
///   [padding:     uint16_t] — alignment padding (reserved)
/// ```
struct MvccTupleHeader {
    txn_id_t xmin = invalid_txn_id; ///< Transaction that created this tuple.
    txn_id_t xmax = invalid_txn_id; ///< Transaction that deleted this tuple (0 = live).
    RID t_ctid = RID::invalid();    ///< Chain pointer to next version of this tuple.

    /// Check if this tuple has a successor in the version chain.
    [[nodiscard]] bool has_next_version() const { return t_ctid != RID::invalid(); }
};

/// Serialized size of the MVCC header in bytes.
/// xmin(8) + xmax(8) + page_id(4) + slot_id(2) + padding(2) = 24
inline constexpr size_t mvcc_header_size = 24;

/// Serialize an MvccTupleHeader into a byte buffer.
/// The buffer must have at least mvcc_header_size bytes.
inline void serialize_mvcc_header(const MvccTupleHeader& header, uint8_t* dest) {
    std::memcpy(dest, &header.xmin, sizeof(txn_id_t));
    dest += sizeof(txn_id_t);
    std::memcpy(dest, &header.xmax, sizeof(txn_id_t));
    dest += sizeof(txn_id_t);
    std::memcpy(dest, &header.t_ctid.page_id, sizeof(PageId));
    dest += sizeof(PageId);
    std::memcpy(dest, &header.t_ctid.slot_id, sizeof(SlotId));
    dest += sizeof(SlotId);
    // 2 bytes padding (zeroed).
    uint16_t pad = 0;
    std::memcpy(dest, &pad, sizeof(uint16_t));
}

/// Deserialize an MvccTupleHeader from a byte span.
/// The span must have at least mvcc_header_size bytes.
inline MvccTupleHeader deserialize_mvcc_header(const uint8_t* src) {
    MvccTupleHeader header;
    std::memcpy(&header.xmin, src, sizeof(txn_id_t));
    src += sizeof(txn_id_t);
    std::memcpy(&header.xmax, src, sizeof(txn_id_t));
    src += sizeof(txn_id_t);
    std::memcpy(&header.t_ctid.page_id, src, sizeof(PageId));
    src += sizeof(PageId);
    std::memcpy(&header.t_ctid.slot_id, src, sizeof(SlotId));
    return header;
}

/// Prepend an MVCC header to user tuple data.
/// Returns a new buffer: [MvccHeader | user_data].
inline std::vector<uint8_t> prepend_mvcc_header(const MvccTupleHeader& header,
                                                std::span<const uint8_t> user_data) {
    std::vector<uint8_t> result(mvcc_header_size + user_data.size());
    serialize_mvcc_header(header, result.data());
    std::memcpy(result.data() + mvcc_header_size, user_data.data(), user_data.size());
    return result;
}

/// Extract the user data portion from an MVCC-prefixed tuple.
/// Returns a span pointing into the original data, starting after the MVCC header.
inline std::span<const uint8_t> strip_mvcc_header(std::span<const uint8_t> tuple_data) {
    if (tuple_data.size() <= mvcc_header_size) {
        return {};
    }
    return tuple_data.subspan(mvcc_header_size);
}

/// Read the MVCC header from an MVCC-prefixed tuple without copying.
inline MvccTupleHeader read_mvcc_header(std::span<const uint8_t> tuple_data) {
    if (tuple_data.size() < mvcc_header_size) {
        return {};
    }
    return deserialize_mvcc_header(tuple_data.data());
}

/// Write xmax and t_ctid into an existing MVCC-prefixed tuple in-place.
/// Used for marking deletion or updating the version chain pointer.
inline void write_mvcc_xmax(uint8_t* tuple_data, txn_id_t xmax) {
    std::memcpy(tuple_data + sizeof(txn_id_t), &xmax, sizeof(txn_id_t));
}

/// Write the version chain pointer (t_ctid) into an existing MVCC-prefixed tuple.
inline void write_mvcc_ctid(uint8_t* tuple_data, RID ctid) {
    uint8_t* dest = tuple_data + sizeof(txn_id_t) + sizeof(txn_id_t);
    std::memcpy(dest, &ctid.page_id, sizeof(PageId));
    dest += sizeof(PageId);
    std::memcpy(dest, &ctid.slot_id, sizeof(SlotId));
}

} // namespace giodb
