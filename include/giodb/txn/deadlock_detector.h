#pragma once

#include "giodb/storage/wal_record.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace giodb {

/// Wait-for graph based deadlock detector.
///
/// Builds a directed graph where an edge (A -> B) means "transaction A is
/// waiting for transaction B to release a lock". Detects cycles using DFS.
///
/// When a cycle is found, the youngest transaction (highest txn_id) in the
/// cycle is chosen as the victim to abort.
///
/// This class is NOT thread-safe — callers must synchronize externally.
class DeadlockDetector {
public:
    /// Add a wait-for edge: waiter is blocked by holder.
    void add_edge(txn_id_t waiter, txn_id_t holder);

    /// Remove all edges where the given transaction is a waiter.
    /// Called when a transaction's lock request is granted or the transaction aborts.
    void remove_waiter(txn_id_t waiter);

    /// Remove all edges involving the given transaction (as waiter or holder).
    /// Called when a transaction completes (commit/abort) and releases all locks.
    void remove_transaction(txn_id_t txn_id);

    /// Check if adding the given waiter creates a deadlock cycle.
    /// Returns the txn_id of the youngest transaction in the cycle (victim),
    /// or invalid_txn_id if no cycle exists.
    [[nodiscard]] txn_id_t detect_cycle(txn_id_t start) const;

    /// Check if the graph has any edges (for testing).
    [[nodiscard]] bool empty() const;

private:
    /// Adjacency list: waiter -> set of holders it's waiting for.
    std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>> adj_;

    /// DFS helper for cycle detection. Returns true if a cycle is found.
    /// On cycle detection, populates `cycle_members` with all txn_ids in the cycle.
    [[nodiscard]] bool dfs(txn_id_t current,
                           txn_id_t target,
                           std::unordered_set<txn_id_t>& visited,
                           std::vector<txn_id_t>& cycle_members) const;
};

} // namespace giodb
