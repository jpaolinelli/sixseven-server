#pragma once

#include "giodb/common/result.h"
#include "giodb/index/btree_key.h"
#include "giodb/index/rid.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace giodb {

// Forward declaration.
class BTreeIndex;

/// Iterator for B+ tree range scans. Follows sibling pointers across leaves.
class BTreeIterator {
public:
    /// Construct an iterator starting at the given leaf/position.
    /// end_key is the exclusive upper bound (nullopt = no upper bound).
    BTreeIterator(const BTreeIndex& index,
                  PageId start_leaf_id,
                  uint16_t start_pos,
                  std::optional<KeyType> end_key);

    /// Default constructor creates an exhausted (end) iterator.
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
};

} // namespace giodb
