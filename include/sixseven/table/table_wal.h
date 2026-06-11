#pragma once

#include "sixseven/common/result.h"
#include "sixseven/storage/wal_record.h"
#include "sixseven/storage/wal_recovery.h"
#include "sixseven/table/table_heap.h"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace sixseven {

// -- Table WAL payload (GDB-714) -----------------------------------------------

/// Payload carried by table-heap INSERT/UPDATE/DELETE WAL records.
///
/// Binary format (native byte order):
/// ```
///   [before_len: uint32][before_image bytes]
///   [after_len:  uint32][after_image bytes]
/// ```
///
/// Images are *full on-page tuple bytes* — for MVCC heaps that is
/// [MvccTupleHeader (24 bytes) | user data] — so redo reconstructs pages
/// byte-identically, headers included.
///
/// Per record type:
///  - INSERT: before empty, after = inserted tuple image.
///  - UPDATE: before = pre-update image, after = post-update image.
///  - DELETE: before = image as of deletion with xmax stamped into the
///            header (the redo-able header mutation), after empty.
struct TableWalPayload {
    std::vector<uint8_t> before_image;
    std::vector<uint8_t> after_image;
};

/// Serialize a table WAL payload into a WalRecord data buffer.
[[nodiscard]] std::vector<uint8_t> serialize_table_wal_payload(const TableWalPayload& payload);

/// Convenience overload building the payload from spans without an
/// intermediate TableWalPayload copy.
[[nodiscard]] std::vector<uint8_t>
serialize_table_wal_payload(std::span<const uint8_t> before_image,
                            std::span<const uint8_t> after_image);

/// Deserialize a table WAL payload from a WalRecord data buffer.
/// Returns INVALID_ARGUMENT if the buffer is truncated or inconsistent.
[[nodiscard]] Result<TableWalPayload> deserialize_table_wal_payload(std::span<const uint8_t> data);

// -- Recovery handler ------------------------------------------------------------

/// Applies table-heap INSERT/UPDATE/DELETE WAL records to TableHeap storage
/// during crash recovery (GDB-714).
///
/// Register each table's heap by table_id; records for unregistered table
/// ids (e.g. index or graph subsystem records sharing the WAL stream) are
/// skipped successfully. All operations are idempotent, as required by the
/// RecoveryHandler contract.
///
/// Redo: INSERT/UPDATE write the after-image at the recorded RID (allocating
/// pages as needed); DELETE removes the slot. Undo: INSERT removes the slot;
/// UPDATE writes back the before-image; DELETE re-inserts the before-image
/// with xmax cleared (the deleting transaction did not commit).
class TableHeapRecoveryHandler : public RecoveryHandler {
public:
    /// Register the heap that owns @p table_id. The heap must outlive this
    /// handler. Re-registering a table id replaces the mapping.
    void register_table(uint32_t table_id, TableHeap* heap);

    [[nodiscard]] Result<void> redo(const WalRecord& record) override;
    [[nodiscard]] Result<void> undo(const WalRecord& record) override;

private:
    std::unordered_map<uint32_t, TableHeap*> tables_;
};

} // namespace sixseven
