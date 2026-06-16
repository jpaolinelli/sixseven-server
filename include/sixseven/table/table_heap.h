#pragma once

#include "sixseven/common/result.h"
#include "sixseven/index/rid.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/page.h"
#include "sixseven/storage/wal_record.h"
#include "sixseven/txn/mvcc_tuple.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace sixseven {

/// Forward declarations.
class TableIterator;
class TransactionManager;
class WalWriter;

/// Configuration options for a TableHeap (GDB-714).
struct TableHeapOptions {
    /// When true, every tuple stored on a page is prefixed with a 24-byte
    /// MvccTupleHeader (xmin / xmax / t_ctid). The header is transparent to
    /// callers: insert/update accept user bytes and get/scan return user
    /// bytes; the header is read via get_tuple_header().
    ///
    /// StorageManager enables this for all SQL table files (the v2 file
    /// format); raw heaps constructed without options keep the legacy
    /// headerless layout (used by edge heaps and low-level tests).
    bool mvcc_headers = false;
};

/// Manages tuple storage in a heap file (unordered collection of data pages).
///
/// A heap file consists of data pages (page IDs 1+) within a single database
/// file. Tuples are inserted into the first page with available space.
/// Each tuple is identified by its RID (page_id + slot_id).
///
/// MVCC headers (GDB-714): when constructed with
/// TableHeapOptions{.mvcc_headers = true}, the on-page representation of a
/// tuple is [MvccTupleHeader (24 bytes) | user data]:
///  - insert stamps xmin (default: frozen_txn_id while DML runs without
///    transaction context) and xmax = invalid (live);
///  - update preserves the existing header and replaces the user data
///    (in-place update does not create a new version chain entry yet);
///  - delete stamps xmax into the WAL before-image and then physically
///    deletes the slot. Physical deletion is intentionally preserved so the
///    executor's observable semantics (deleted rows vanish immediately, page
///    space is reclaimable via compact) are unchanged; visibility-based
///    deletes and VACUUM-driven reclamation arrive with the transactions
///    epic (see GDB-1230).
///
/// WAL (GDB-714): after attach_wal(), insert/update/delete append
/// physiological WAL records whose payload carries full tuple images
/// including the MVCC header (see table_wal.h), so redo reconstructs
/// identical page contents. Logging is best-effort: WAL append failures are
/// logged and do not fail the heap operation.
///
/// Usage:
/// ```
///   TableHeap heap(bpm, dm, file_id, TableHeapOptions{.mvcc_headers = true});
///   auto rid = heap.insert_tuple(data).value();
///   auto tuple = heap.get_tuple(rid).value();       // user bytes
///   auto hdr = heap.get_tuple_header(rid).value();  // xmin/xmax
///   heap.delete_tuple(rid);
/// ```
class TableHeap {
public:
    /// Construct a TableHeap managing tuple storage for a single file.
    /// @param bpm Buffer pool manager (must be tied to the same file_id).
    /// @param dm  Disk manager for page count queries.
    /// @param file_id The file backing this heap.
    /// @param options Heap behavior options (MVCC headers, see above).
    TableHeap(BufferPoolManager& bpm,
              DiskManager& dm,
              FileId file_id,
              TableHeapOptions options = {});

    /// Attach a WAL writer (GDB-714). Subsequent insert/update/delete
    /// operations append INSERT/UPDATE/DELETE records carrying full tuple
    /// images for redo. Pass nullptr to detach.
    /// @param wal WAL writer (not owned; must outlive this heap or be detached).
    /// @param table_id Table identifier stamped into each record.
    void attach_wal(WalWriter* wal, uint32_t table_id);

    /// Attach a transaction manager (GDB-747). When attached, reads through
    /// this heap (get_tuple / begin scans) filter tuple versions by
    /// transaction status: versions created by a known-aborted transaction,
    /// or deleted by a transaction that is not known-aborted, are invisible.
    /// Transaction ids unknown to the manager (e.g. rows persisted by a
    /// previous process — there is no persistent commit log yet) are treated
    /// as committed so existing data stays readable across restarts.
    /// Pass nullptr to detach.
    /// @param txn_mgr Transaction manager (not owned; must outlive this heap
    ///                or be detached).
    void attach_txn_manager(const TransactionManager* txn_mgr);

    /// Whether this heap stores MVCC tuple headers.
    [[nodiscard]] bool mvcc_headers() const { return options_.mvcc_headers; }

    /// Whether a transaction manager is attached (visibility filtering active).
    [[nodiscard]] bool has_txn_manager() const { return txn_mgr_ != nullptr; }

    /// Insert a tuple into the heap. Finds the first page with sufficient
    /// free space, or allocates a new page if none has room.
    /// @param data Tuple bytes (must be non-empty and fit in a single page).
    /// @param xmin Creating transaction id stamped into the MVCC header
    ///             (MVCC mode only). Defaults to frozen_txn_id (autocommit).
    /// @return The RID (page_id, slot_id) of the inserted tuple.
    [[nodiscard]] Result<RID> insert_tuple(std::span<const uint8_t> data,
                                           txn_id_t xmin = frozen_txn_id);

    /// Insert multiple tuples in batch, reducing buffer pool latch acquisitions
    /// from O(rows) to O(pages). Each page is pinned once and filled with as
    /// many tuples as fit before unpinning and moving to the next page.
    /// @param tuples Vector of tuple byte spans (empty tuples are rejected).
    /// @param xmin Creating transaction id for the MVCC headers (MVCC mode).
    /// @return A vector of RIDs, one per inserted tuple, in the same order.
    [[nodiscard]] Result<std::vector<RID>>
    insert_batch(const std::vector<std::span<const uint8_t>>& tuples,
                 txn_id_t xmin = frozen_txn_id);

    /// Read a tuple by RID. Returns a copy of the tuple data (user bytes
    /// only — the MVCC header, if present, is stripped).
    /// The page is unpinned before returning, so the data is a snapshot.
    [[nodiscard]] Result<std::vector<uint8_t>> get_tuple(RID rid);

    /// Read the MVCC header of a tuple (MVCC mode only).
    /// Returns NOT_IMPLEMENTED for heaps without MVCC headers.
    [[nodiscard]] Result<MvccTupleHeader> get_tuple_header(RID rid);

    /// Update a tuple in-place. If the new data fits in the existing slot,
    /// the update is done directly. If not, the page is compacted and the
    /// update is retried. If the tuple still doesn't fit after compaction
    /// (e.g., it exceeds available page space), the update fails.
    /// In MVCC mode the existing header (xmin / t_ctid) is preserved.
    /// The RID remains stable on success.
    [[nodiscard]] Result<void> update_tuple(RID rid, std::span<const uint8_t> data);

    /// Delete a tuple by marking its slot as deleted.
    /// @param xmax Deleting transaction id. In MVCC mode it is stamped into
    ///             the WAL before-image (the on-page slot is then physically
    ///             deleted — see class documentation). Defaults to
    ///             frozen_txn_id (autocommit).
    [[nodiscard]] Result<void> delete_tuple(RID rid, txn_id_t xmax = frozen_txn_id);

    /// Logically delete a tuple (GDB-747, MVCC mode only): stamp xmax (and
    /// optionally the version-chain pointer t_ctid) into the on-page MVCC
    /// header without removing the slot. The old version stays on the page so
    /// an aborted deleting transaction leaves the tuple visible again; VACUUM
    /// (GDB-1230) reclaims dead versions later. Logged as a WAL UPDATE record
    /// with full before/after images so redo reproduces the stamped header.
    /// Decrements the live row count.
    /// @param rid          Tuple to mark deleted.
    /// @param xmax         Deleting transaction id.
    /// @param next_version RID of the replacing version (UPDATE chains), or
    ///                     RID::invalid() for plain DELETE.
    [[nodiscard]] Result<void>
    mark_deleted(RID rid, txn_id_t xmax, RID next_version = RID::invalid());

    /// Adjust the live row count by a signed delta (GDB-747). Used by the
    /// executor to compensate counters when a transaction rolls back (its
    /// logical inserts/deletes already moved the counter).
    void adjust_row_count(int64_t delta);

    /// A single in-page tuple update request for batch processing.
    struct TupleUpdate {
        RID rid;
        std::span<const uint8_t> data;
    };

    /// Update multiple tuples in batch, grouping by page_id internally.
    /// For each page: pin once, apply all updates, unpin once. This reduces
    /// buffer pool round-trips from O(tuples) to O(pages), cutting page latch
    /// contention with concurrent INSERT operations.
    /// @return A vector of RIDs that failed (caller can retry or escalate).
    [[nodiscard]] Result<std::vector<RID>>
    update_tuples_batch(const std::vector<TupleUpdate>& updates);

    // -- WAL recovery primitives (GDB-714) -------------------------------------
    //
    // Raw-image operations used by TableHeapRecoveryHandler during redo/undo.
    // They take full on-page tuple images (MVCC header included for MVCC
    // heaps), are idempotent, never log WAL themselves, and keep the
    // persistent row count consistent.

    /// Write a raw tuple image at an exact RID, allocating pages and
    /// extending the slot directory as needed. Overwrites a live slot,
    /// revives a deleted one. Idempotent.
    [[nodiscard]] Result<void> restore_raw_tuple(RID rid, std::span<const uint8_t> raw_image);

    /// Delete the slot at an exact RID if it is live. Succeeds (no-op) if the
    /// slot/page is already deleted or absent. Idempotent.
    [[nodiscard]] Result<void> delete_raw_tuple(RID rid);

    /// Return a sequential-scan iterator starting from the first tuple.
    /// Fails if the file page count cannot be read.
    [[nodiscard]] Result<TableIterator> begin();

    /// Return the number of data pages in the heap file.
    /// Data pages start at page ID 1 (page 0 is the file header).
    [[nodiscard]] Result<uint32_t> page_count() const;

    /// Return the live row count (O(1) via atomic counter).
    [[nodiscard]] uint64_t row_count() const;

private:
    BufferPoolManager& bpm_;
    DiskManager& dm_;
    FileId file_id_;
    TableHeapOptions options_;

    /// Optional WAL writer for logging tuple mutations (not owned).
    WalWriter* wal_ = nullptr;
    /// Optional transaction manager for visibility filtering (not owned).
    const TransactionManager* txn_mgr_ = nullptr;
    /// Table id stamped into WAL records when wal_ is attached.
    uint32_t wal_table_id_ = 0;

    /// Hint: last page we successfully inserted into.
    /// Speeds up sequential inserts by avoiding full scans.
    /// Accessed by concurrent insert() callers; atomic with relaxed ordering
    /// is sufficient because this is only a placement hint — a stale or torn
    /// read produces a wrong-but-validated page id, not corruption.
    std::atomic<PageId> last_insert_page_{0};

    /// Live row count, incremented on insert, decremented on delete.
    /// TODO(GDB-616): When txn integration is done, rollback must compensate
    /// the counter (decrement on aborted insert, increment on aborted delete).
    std::atomic<uint64_t> row_count_{0};

    /// Read row count from header page (page 0) on table open.
    void load_row_count_from_header();

    /// Write row count to header page (page 0) for persistence.
    void persist_row_count();

    /// Build the on-page image for user data: prepends an MvccTupleHeader in
    /// MVCC mode, otherwise returns a plain copy of the user bytes.
    [[nodiscard]] std::vector<uint8_t> make_on_page_image(std::span<const uint8_t> user_data,
                                                          txn_id_t xmin) const;

    /// Best-effort WAL append for a tuple mutation (no-op when detached).
    void log_wal(WalRecordType type,
                 RID rid,
                 txn_id_t txn_id,
                 std::span<const uint8_t> before_image,
                 std::span<const uint8_t> after_image);
};

/// Sequential scan iterator over all live tuples in a TableHeap.
///
/// Usage:
/// ```
///   auto it = heap.begin().value();
///   while (auto result = it.next()) {
///       auto& [rid, data] = *result;
///       // process tuple...
///   }
/// ```
class TableIterator {
public:
    TableIterator(BufferPoolManager& bpm,
                  PageId start_page,
                  uint32_t total_pages,
                  bool strip_mvcc_headers = false,
                  const TransactionManager* txn_mgr = nullptr);

    /// Advance to the next live tuple.
    /// @return A pair of (RID, tuple data copy), or nullopt if scan is exhausted.
    ///         For MVCC heaps the data is the user payload (header stripped).
    [[nodiscard]] std::optional<std::pair<RID, std::vector<uint8_t>>> next();

    /// Skip @p count live tuples without deserializing or copying their data.
    /// Much faster than calling next() and discarding the result, because it
    /// only checks the slot directory (4 bytes per slot) instead of copying
    /// full tuple payloads.
    void skip(size_t count);

private:
    BufferPoolManager& bpm_;
    PageId current_page_;
    SlotId current_slot_ = 0;
    uint32_t total_pages_;
    bool exhausted_ = false;
    /// Strip the 24-byte MvccTupleHeader from returned tuples (GDB-714).
    bool strip_mvcc_headers_ = false;
    /// Transaction manager for visibility filtering (GDB-747, may be null).
    const TransactionManager* txn_mgr_ = nullptr;
};

} // namespace sixseven
