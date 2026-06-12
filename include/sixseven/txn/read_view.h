#pragma once

#include "sixseven/txn/transaction.h"

namespace sixseven {

/// Per-statement MVCC read view (GDB-777): the snapshot a statement reads
/// under plus the transaction id performing the read. Installed by the
/// executor around statement execution and consulted by TableHeap reads
/// (get_tuple / TableIterator) to filter tuple versions via is_visible().
///
/// When no read view is installed (recovery, vacuum, direct heap usage,
/// internal bootstrap), heap reads fall back to the status-based filtering
/// introduced by GDB-747 — behavior for non-transactional paths is unchanged.
struct MvccReadView {
    /// Snapshot taken at statement start (read committed) or transaction
    /// begin (snapshot isolation / serializable).
    Snapshot snapshot;
    /// Transaction performing the read; invalid_txn_id for autocommit reads
    /// outside any transaction.
    txn_id_t viewer_txn_id = invalid_txn_id;
};

/// Thread-local accessor for the current statement's read view.
/// @return The installed read view, or nullptr when none is active.
[[nodiscard]] const MvccReadView* current_mvcc_read_view();

/// RAII guard installing a thread-local MVCC read view for the duration of a
/// statement. Saves and restores any previously installed view, so nested
/// (internal) statement executions compose correctly.
class MvccReadViewGuard {
public:
    explicit MvccReadViewGuard(MvccReadView view);
    ~MvccReadViewGuard();

    MvccReadViewGuard(const MvccReadViewGuard&) = delete;
    MvccReadViewGuard& operator=(const MvccReadViewGuard&) = delete;
    MvccReadViewGuard(MvccReadViewGuard&&) = delete;
    MvccReadViewGuard& operator=(MvccReadViewGuard&&) = delete;

private:
    MvccReadView view_;
    const MvccReadView* previous_;
};

} // namespace sixseven
