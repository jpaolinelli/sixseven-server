#include "giodb/txn/mvcc.h"

namespace giodb {

bool is_visible(const MvccTupleHeader& header,
                const Snapshot& snapshot,
                const TransactionManager& txn_mgr,
                txn_id_t viewer_txn_id) {
    // Rule 0: A transaction always sees its own uncommitted changes.
    if (viewer_txn_id != invalid_txn_id && header.xmin == viewer_txn_id) {
        // We created this tuple. It's visible unless we also deleted it.
        if (header.xmax == invalid_txn_id) {
            return true; // Not deleted.
        }
        if (header.xmax == viewer_txn_id) {
            return false; // We deleted it ourselves.
        }
        // Deleted by someone else — still visible to us since xmax != our txn.
        // (The other deleter hasn't committed relative to us.)
        return true;
    }

    // Rule 1: Check xmin (the creating transaction).
    // The tuple is invisible if xmin is not committed, or was taken after
    // our snapshot, or was in-progress when we took the snapshot.
    auto xmin_status = txn_mgr.get_status(header.xmin);

    if (xmin_status == TransactionStatus::ABORTED) {
        return false; // Creating transaction aborted — tuple never existed.
    }

    if (xmin_status == TransactionStatus::ACTIVE) {
        return false; // Creating transaction still in progress.
    }

    // xmin is committed. Check if it's within our snapshot window.
    if (header.xmin >= snapshot.xmax) {
        return false; // Created after our snapshot — invisible.
    }

    if (snapshot.is_active(header.xmin)) {
        return false; // Was in-progress when we took the snapshot — invisible.
    }

    // Rule 2: Check xmax (the deleting transaction).
    // If xmax is not set, the tuple is live and visible.
    if (header.xmax == invalid_txn_id) {
        return true;
    }

    auto xmax_status = txn_mgr.get_status(header.xmax);

    if (xmax_status == TransactionStatus::ABORTED) {
        return true; // Deleter aborted — deletion didn't happen.
    }

    if (xmax_status == TransactionStatus::ACTIVE) {
        return true; // Deleter still in progress — not yet deleted.
    }

    // xmax is committed. Check if the deletion is within our snapshot.
    if (header.xmax >= snapshot.xmax) {
        return true; // Deleted after our snapshot — still visible to us.
    }

    if (snapshot.is_active(header.xmax)) {
        return true; // Deleter was in-progress at snapshot time — still visible.
    }

    // xmax is committed and within our snapshot — tuple has been deleted.
    return false;
}

bool is_dead(const MvccTupleHeader& header,
             txn_id_t xmin_horizon,
             const TransactionManager& txn_mgr) {
    // A tuple version is dead if it cannot be seen by any active transaction.

    // Case 1: xmin aborted — tuple was never visible.
    auto xmin_status = txn_mgr.get_status(header.xmin);
    if (xmin_status == TransactionStatus::ABORTED) {
        return true;
    }

    // Case 2: xmin still active — not dead yet (creator may commit).
    if (xmin_status == TransactionStatus::ACTIVE) {
        return false;
    }

    // xmin is committed.

    // Case 3: xmax not set — tuple is live, not dead.
    if (header.xmax == invalid_txn_id) {
        return false;
    }

    // Case 4: xmax aborted — tuple is still live.
    auto xmax_status = txn_mgr.get_status(header.xmax);
    if (xmax_status == TransactionStatus::ABORTED) {
        return false;
    }

    // Case 5: xmax active — deletion not yet finalized.
    if (xmax_status == TransactionStatus::ACTIVE) {
        return false;
    }

    // Case 6: xmax committed and older than xmin_horizon.
    // No active transaction can see this version.
    return header.xmax < xmin_horizon;
}

} // namespace giodb
