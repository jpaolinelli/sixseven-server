#include "giodb/txn/deadlock_detector.h"

#include <algorithm>

namespace giodb {

void DeadlockDetector::add_edge(txn_id_t waiter, txn_id_t holder) {
    if (waiter == holder) {
        return;
    }
    adj_[waiter].insert(holder);
}

void DeadlockDetector::remove_waiter(txn_id_t waiter) {
    adj_.erase(waiter);
}

void DeadlockDetector::remove_transaction(txn_id_t txn_id) {
    // Remove as waiter.
    adj_.erase(txn_id);

    // Remove as holder from all other transactions' wait sets.
    for (auto& [waiter, holders] : adj_) {
        holders.erase(txn_id);
    }

    // Clean up empty entries.
    for (auto it = adj_.begin(); it != adj_.end();) {
        if (it->second.empty()) {
            it = adj_.erase(it);
        } else {
            ++it;
        }
    }
}

txn_id_t DeadlockDetector::detect_cycle(txn_id_t start) const {
    std::unordered_set<txn_id_t> visited;
    std::vector<txn_id_t> cycle_members;

    if (dfs(start, start, visited, cycle_members)) {
        // Find the youngest transaction (highest txn_id) in the cycle.
        return *std::max_element(cycle_members.begin(), cycle_members.end());
    }

    return invalid_txn_id;
}

bool DeadlockDetector::empty() const {
    return adj_.empty();
}

bool DeadlockDetector::dfs(txn_id_t current,
                           txn_id_t target,
                           std::unordered_set<txn_id_t>& visited,
                           std::vector<txn_id_t>& cycle_members) const {
    auto it = adj_.find(current);
    if (it == adj_.end()) {
        return false;
    }

    for (txn_id_t neighbor : it->second) {
        if (neighbor == target && !visited.empty()) {
            // Found a cycle back to the start.
            cycle_members.push_back(current);
            cycle_members.push_back(target);
            return true;
        }

        if (visited.count(neighbor) > 0) {
            continue;
        }

        visited.insert(neighbor);
        if (dfs(neighbor, target, visited, cycle_members)) {
            cycle_members.push_back(current);
            return true;
        }
    }

    return false;
}

} // namespace giodb
