#include "sixseven/txn/txn_manager.h"

#include "sixseven/common/logging.h"

#include <algorithm>
#include <limits>

namespace sixseven {

Result<Transaction*> TransactionManager::begin(IsolationLevel level) {
    std::lock_guard lock(mu_);

    auto txn = std::make_unique<Transaction>();
    txn->txn_id = next_txn_id_++;
    txn->isolation_level = level;
    txn->status = TransactionStatus::ACTIVE;
    txn->snapshot = take_snapshot_locked();

    auto* ptr = txn.get();
    transactions_[ptr->txn_id] = std::move(txn);

    SIXSEVEN_LOG_DEBUG("BEGIN txn_id={} isolation={}", ptr->txn_id, isolation_level_name(level));
    return ok(ptr);
}

Result<void> TransactionManager::commit(txn_id_t txn_id) {
    std::lock_guard lock(mu_);

    auto it = transactions_.find(txn_id);
    if (it == transactions_.end()) {
        return make_error(StatusCode::NOT_FOUND, "transaction not found");
    }

    auto& txn = *it->second;
    if (txn.status != TransactionStatus::ACTIVE) {
        return make_error(StatusCode::TXN_ABORTED, "cannot commit non-active transaction");
    }

    // Under Snapshot Isolation or Serializable, check for write-write conflicts.
    if (txn.isolation_level >= IsolationLevel::SNAPSHOT_ISOLATION) {
        auto result = check_write_conflicts(txn);
        if (!result) {
            txn.status = TransactionStatus::ABORTED;
            SIXSEVEN_LOG_WARN("ABORT txn_id={} due to write-write conflict", txn_id);
            return result;
        }
    }

    // Under Serializable (SSI), additionally check for rw-dependency cycles.
    if (txn.isolation_level == IsolationLevel::SERIALIZABLE) {
        auto result = check_serialization_conflicts(txn);
        if (!result) {
            txn.status = TransactionStatus::ABORTED;
            SIXSEVEN_LOG_WARN("ABORT txn_id={} due to serialization anomaly", txn_id);
            return result;
        }
    }

    txn.status = TransactionStatus::COMMITTED;
    SIXSEVEN_LOG_DEBUG("COMMIT txn_id={}", txn_id);
    return ok();
}

Result<void> TransactionManager::abort(txn_id_t txn_id) {
    std::lock_guard lock(mu_);

    auto it = transactions_.find(txn_id);
    if (it == transactions_.end()) {
        return make_error(StatusCode::NOT_FOUND, "transaction not found");
    }

    auto& txn = *it->second;
    if (txn.status != TransactionStatus::ACTIVE) {
        return make_error(StatusCode::INVALID_ARGUMENT, "cannot abort non-active transaction");
    }

    txn.status = TransactionStatus::ABORTED;
    SIXSEVEN_LOG_DEBUG("ABORT txn_id={}", txn_id);
    return ok();
}

Snapshot TransactionManager::take_snapshot() const {
    std::lock_guard lock(mu_);
    return take_snapshot_locked();
}

Snapshot TransactionManager::get_statement_snapshot(txn_id_t txn_id) {
    std::lock_guard lock(mu_);

    auto it = transactions_.find(txn_id);
    if (it == transactions_.end()) {
        return {};
    }

    auto& txn = *it->second;

    if (txn.isolation_level == IsolationLevel::READ_COMMITTED) {
        // Read Committed: refresh snapshot for each statement.
        txn.snapshot = take_snapshot_locked();
    }
    // SI/SSI: reuse the snapshot from BEGIN.
    return txn.snapshot;
}

TransactionStatus TransactionManager::get_status(txn_id_t txn_id) const {
    // Frozen stamps (GDB-714) mark autocommit/bootstrap writes: always
    // committed, visible to every snapshot. Never allocated as a real id.
    if (txn_id == frozen_txn_id) {
        return TransactionStatus::COMMITTED;
    }

    std::lock_guard lock(mu_);

    auto it = transactions_.find(txn_id);
    if (it != transactions_.end()) {
        return it->second->status;
    }

    // Check if this was a committed transaction that was garbage-collected.
    if (pruned_committed_.count(txn_id) > 0) {
        return TransactionStatus::COMMITTED;
    }

    // Unknown transactions are treated as aborted (conservative).
    return TransactionStatus::ABORTED;
}

Transaction* TransactionManager::get_transaction(txn_id_t txn_id) const {
    std::lock_guard lock(mu_);

    auto it = transactions_.find(txn_id);
    if (it == transactions_.end()) {
        return nullptr;
    }
    return it->second.get();
}

txn_id_t TransactionManager::next_txn_id() const {
    std::lock_guard lock(mu_);
    return next_txn_id_;
}

void TransactionManager::record_write(txn_id_t txn_id, RID rid) {
    std::lock_guard lock(mu_);

    auto it = transactions_.find(txn_id);
    if (it != transactions_.end()) {
        it->second->write_set.insert(rid);
    }
}

void TransactionManager::record_read(txn_id_t txn_id, RID rid) {
    std::lock_guard lock(mu_);

    auto it = transactions_.find(txn_id);
    if (it != transactions_.end()) {
        it->second->read_set.insert(rid);
    }
}

Result<void> TransactionManager::savepoint(txn_id_t txn_id, const std::string& name) {
    std::lock_guard lock(mu_);

    auto it = transactions_.find(txn_id);
    if (it == transactions_.end()) {
        return make_error(StatusCode::NOT_FOUND, "transaction not found");
    }

    auto& txn = *it->second;
    if (txn.status != TransactionStatus::ACTIVE) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "cannot create savepoint in non-active transaction");
    }

    Savepoint sp;
    sp.name = name;
    sp.saved_write_set = txn.write_set;
    sp.saved_read_set = txn.read_set;
    txn.savepoints.push_back(std::move(sp));

    SIXSEVEN_LOG_DEBUG("SAVEPOINT {} in txn_id={}", name, txn_id);
    return ok();
}

Result<void> TransactionManager::rollback_to_savepoint(txn_id_t txn_id, const std::string& name) {
    std::lock_guard lock(mu_);

    auto it = transactions_.find(txn_id);
    if (it == transactions_.end()) {
        return make_error(StatusCode::NOT_FOUND, "transaction not found");
    }

    auto& txn = *it->second;
    if (txn.status != TransactionStatus::ACTIVE) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "cannot rollback to savepoint in non-active transaction");
    }

    // Find the most recent savepoint with the given name (search from back).
    auto sp_it = std::find_if(txn.savepoints.rbegin(),
                              txn.savepoints.rend(),
                              [&name](const Savepoint& sp) { return sp.name == name; });
    if (sp_it == txn.savepoints.rend()) {
        return make_error(StatusCode::NOT_FOUND, "savepoint not found: " + name);
    }

    // Restore write and read sets to the savepoint state.
    txn.write_set = sp_it->saved_write_set;
    txn.read_set = sp_it->saved_read_set;

    // Remove all savepoints created after this one (keep the target savepoint).
    // sp_it.base() points to the element after sp_it in forward order.
    txn.savepoints.erase(sp_it.base(), txn.savepoints.end());

    SIXSEVEN_LOG_DEBUG("ROLLBACK TO SAVEPOINT {} in txn_id={}", name, txn_id);
    return ok();
}

Result<void> TransactionManager::release_savepoint(txn_id_t txn_id, const std::string& name) {
    std::lock_guard lock(mu_);

    auto it = transactions_.find(txn_id);
    if (it == transactions_.end()) {
        return make_error(StatusCode::NOT_FOUND, "transaction not found");
    }

    auto& txn = *it->second;
    if (txn.status != TransactionStatus::ACTIVE) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "cannot release savepoint in non-active transaction");
    }

    // Find the most recent savepoint with the given name.
    auto sp_it = std::find_if(txn.savepoints.rbegin(),
                              txn.savepoints.rend(),
                              [&name](const Savepoint& sp) { return sp.name == name; });
    if (sp_it == txn.savepoints.rend()) {
        return make_error(StatusCode::NOT_FOUND, "savepoint not found: " + name);
    }

    // Remove this savepoint and all established after it.
    // std::prev(sp_it.base()) converts reverse iterator to forward iterator pointing at the
    // element.
    txn.savepoints.erase(std::prev(sp_it.base()), txn.savepoints.end());

    SIXSEVEN_LOG_DEBUG("RELEASE SAVEPOINT {} in txn_id={}", name, txn_id);
    return ok();
}

txn_id_t TransactionManager::xmin_horizon() const {
    std::lock_guard lock(mu_);
    return xmin_horizon_locked();
}

txn_id_t TransactionManager::xmin_horizon_locked() const {
    txn_id_t horizon = std::numeric_limits<txn_id_t>::max();
    for (const auto& [id, txn] : transactions_) {
        if (txn->status == TransactionStatus::ACTIVE) {
            if (txn->snapshot.xmin < horizon && txn->snapshot.xmin != 0) {
                horizon = txn->snapshot.xmin;
            }
            // Also consider the transaction's own ID as a lower bound.
            if (txn->txn_id < horizon) {
                horizon = txn->txn_id;
            }
        }
    }
    // If no active transactions, horizon is the next txn_id (everything is safe).
    if (horizon == std::numeric_limits<txn_id_t>::max()) {
        horizon = next_txn_id_;
    }
    return horizon;
}

void TransactionManager::gc_completed_transactions() {
    std::lock_guard lock(mu_);
    gc_completed_transactions_locked();
}

void TransactionManager::gc_completed_transactions_locked() {
    txn_id_t horizon = xmin_horizon_locked();

    for (auto it = transactions_.begin(); it != transactions_.end();) {
        auto& txn = *it->second;
        if (txn.status != TransactionStatus::ACTIVE && txn.txn_id < horizon) {
            if (txn.status == TransactionStatus::COMMITTED) {
                pruned_committed_.insert(txn.txn_id);
            }
            it = transactions_.erase(it);
        } else {
            ++it;
        }
    }
}

void TransactionManager::set_default_isolation_level(IsolationLevel level) {
    std::lock_guard lock(mu_);
    default_isolation_level_ = level;
}

IsolationLevel TransactionManager::default_isolation_level() const {
    std::lock_guard lock(mu_);
    return default_isolation_level_;
}

Snapshot TransactionManager::take_snapshot_locked() const {
    Snapshot snap;
    snap.xmax = next_txn_id_;

    txn_id_t min_active = snap.xmax;
    for (const auto& [id, txn] : transactions_) {
        if (txn->status == TransactionStatus::ACTIVE) {
            snap.active_txn_ids.push_back(id);
            if (id < min_active) {
                min_active = id;
            }
        }
    }
    snap.xmin = min_active;
    return snap;
}

Result<void> TransactionManager::check_write_conflicts(const Transaction& txn) const {
    // Check if any RID in our write set was also written by a concurrent
    // transaction that committed after our snapshot was taken.
    for (const auto& [id, other] : transactions_) {
        if (id == txn.txn_id) {
            continue;
        }
        if (other->status != TransactionStatus::COMMITTED) {
            continue;
        }
        // "Concurrent" means the other txn committed after our snapshot.
        // It would be in our active list if it was active when we started,
        // or its ID >= our snapshot.xmax if it started after us.
        bool was_concurrent = txn.snapshot.is_active(id) || id >= txn.snapshot.xmax;
        if (!was_concurrent) {
            continue;
        }

        // Check for overlapping writes.
        for (const auto& rid : txn.write_set) {
            if (other->write_set.count(rid) > 0) {
                return make_error(StatusCode::TXN_CONFLICT,
                                  "write-write conflict on page=" + std::to_string(rid.page_id) +
                                      " slot=" + std::to_string(rid.slot_id));
            }
        }
    }
    return ok();
}

Result<void> TransactionManager::check_serialization_conflicts(const Transaction& txn) const {
    // SSI: Detect dangerous structures in rw-dependency graph.
    //
    // A "dangerous structure" exists when there's a cycle of rw-dependencies
    // among concurrent transactions. The simplified check:
    //
    // For the committing transaction T:
    //   1. Find concurrent committed transactions that wrote to RIDs that T read
    //      (T has an rw-dependency ON those transactions: T -rw-> other)
    //   2. Find concurrent committed transactions that read RIDs that T wrote
    //      (Those transactions have an rw-dependency ON T: other -rw-> T)
    //   3. If both sets are non-empty, there's a potential dangerous structure.
    //
    // This is a conservative approximation — it may produce false positives
    // but never misses an actual serialization anomaly.

    bool has_rw_out = false; // T reads something written by a concurrent txn.
    bool has_rw_in = false;  // A concurrent txn reads something written by T.

    for (const auto& [id, other] : transactions_) {
        if (id == txn.txn_id) {
            continue;
        }
        if (other->status != TransactionStatus::COMMITTED) {
            continue;
        }
        bool was_concurrent = txn.snapshot.is_active(id) || id >= txn.snapshot.xmax;
        if (!was_concurrent) {
            continue;
        }

        // Check rw-out: Did T read something that 'other' wrote?
        if (!has_rw_out) {
            for (const auto& rid : txn.read_set) {
                if (other->write_set.count(rid) > 0) {
                    has_rw_out = true;
                    break;
                }
            }
        }

        // Check rw-in: Did 'other' read something that T wrote?
        if (!has_rw_in) {
            for (const auto& rid : txn.write_set) {
                if (other->read_set.count(rid) > 0) {
                    has_rw_in = true;
                    break;
                }
            }
        }

        if (has_rw_out && has_rw_in) {
            return make_error(StatusCode::TXN_CONFLICT,
                              "serialization anomaly detected (rw-dependency cycle)");
        }
    }

    return ok();
}

} // namespace sixseven
