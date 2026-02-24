#pragma once

#include "giodb/txn/mvcc_tuple.h"
#include "giodb/txn/transaction.h"
#include "giodb/txn/txn_manager.h"

namespace giodb {

/// Check if a tuple is visible to a given snapshot.
///
/// A tuple version is visible when:
///   1. xmin is committed AND xmin < snapshot.xmax AND xmin not in snapshot.active_txn_ids
///   2. AND (xmax is invalid OR xmax is aborted OR xmax >= snapshot.xmax
///           OR xmax in snapshot.active_txn_ids)
///
/// Special case: a transaction can always see its own uncommitted changes.
///
/// @param header     The MVCC header of the tuple.
/// @param snapshot   The snapshot to check against.
/// @param txn_mgr   The transaction manager (for status lookups).
/// @param viewer_txn_id  The transaction performing the visibility check.
/// @return true if the tuple is visible.
[[nodiscard]] bool is_visible(const MvccTupleHeader& header,
                              const Snapshot& snapshot,
                              const TransactionManager& txn_mgr,
                              txn_id_t viewer_txn_id = invalid_txn_id);

/// Check if a tuple version is dead (invisible to ALL possible snapshots).
/// A dead tuple is safe for vacuum if its xmax is committed and older than
/// the xmin_horizon.
///
/// @param header      The MVCC header of the tuple.
/// @param xmin_horizon The oldest active snapshot's xmin.
/// @param txn_mgr    The transaction manager (for status lookups).
/// @return true if the tuple version is dead and can be reclaimed.
[[nodiscard]] bool
is_dead(const MvccTupleHeader& header, txn_id_t xmin_horizon, const TransactionManager& txn_mgr);

} // namespace giodb
