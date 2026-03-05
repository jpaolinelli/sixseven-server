#pragma once

#include "sixseven/common/result.h"
#include "sixseven/index/btree_key.h"
#include "sixseven/index/rid.h"

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <utility>

namespace sixseven {

// Forward declaration.
class BTreeIndex;

/// Iterator for B+ tree range scans. Follows sibling pointers across leaves.
///
/// @note Lifetime: the iterator holds a shared lock on the owning BTreeIndex's
/// tree_latch_, preventing concurrent writes during the scan. The iterator
/// MUST NOT outlive the BTreeIndex that created it. Destroying the iterator
/// releases the shared lock, allowing writes to proceed.
class BTreeIterator {
public:
    /// Construct an iterator starting at the given leaf/position.
    /// end_key is the exclusive upper bound (nullopt = no upper bound).
    /// tree_lock is a shared lock on the tree's latch (moved in).
    BTreeIterator(const BTreeIndex& index,
                  PageId start_leaf_id,
                  uint16_t start_pos,
                  std::optional<KeyType> end_key,
                  std::shared_lock<std::shared_mutex> tree_lock);

    /// Default constructor creates an exhausted (end) iterator with no lock.
    BTreeIterator();

    /// Advance to the next entry. Returns (key, rid) or nullopt if exhausted.
    [[nodiscard]] Result<std::optional<std::pair<KeyType, RID>>> next();

    /// Return true if the iterator has been exhausted.
    [[nodiscard]] bool is_end() const;

private:
    const BTreeIndex* index_ = nullptr;
    PageId current_leaf_id_ = 0;
    uint16_t current_pos_ = 0;
    std::optional<KeyType> end_key_;
    bool exhausted_ = true;

    /// Shared lock on the tree, held for the lifetime of the scan.
    /// Default-constructed (no mutex owned) for exhausted iterators.
    std::shared_lock<std::shared_mutex> tree_lock_;
};

} // namespace sixseven
