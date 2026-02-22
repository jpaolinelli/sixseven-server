#pragma once

#include "giodb/common/result.h"
#include "giodb/index/btree_key.h"
#include "giodb/index/rid.h"
#include "giodb/storage/disk_manager.h"

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <vector>

namespace giodb {

/// Invalid/sentinel page ID for B+ tree pointers.
inline constexpr PageId invalid_page_id = 0;

// -- Internal Node -----------------------------------------------------------

/// B+ tree internal node: stores sorted keys and child page_id pointers.
/// For N keys, there are N+1 children.
/// keys_[i] is the separator between children_[i] and children_[i+1].
class BTreeInternalNode {
public:
    /// @param page_id Logical page ID for this node.
    /// @param max_keys Maximum number of keys this node can hold.
    BTreeInternalNode(PageId page_id, uint16_t max_keys);

    // -- Accessors ---

    [[nodiscard]] PageId page_id() const;
    [[nodiscard]] PageId parent_page_id() const;
    void set_parent_page_id(PageId id);
    [[nodiscard]] uint16_t key_count() const;
    [[nodiscard]] uint16_t max_keys() const;
    [[nodiscard]] const KeyType& key_at(uint16_t index) const;
    [[nodiscard]] PageId child_at(uint16_t index) const;
    void set_child_at(uint16_t index, PageId child_id);

    /// Find the child page_id to follow for the given search key.
    /// Uses binary search: returns children_[i] where keys_[i-1] <= key < keys_[i].
    [[nodiscard]] Result<PageId> search(const KeyType& key) const;

    /// Insert a new (key, right_child) pair at the given position.
    /// The key separates children_[pos] and the new right_child.
    [[nodiscard]] Result<void> insert_at(uint16_t pos, const KeyType& key, PageId right_child);

    /// Remove key at index. Also removes children_[index + 1].
    void remove_at(uint16_t index);

    /// True if the node is full (key_count == max_keys).
    [[nodiscard]] bool is_full() const;

    /// True if the node is underfull (below minimum occupancy).
    /// Root nodes are never underfull. Non-root minimum = ceil(max_keys / 2).
    [[nodiscard]] bool is_underfull(bool is_root) const;

    /// Direct access to keys/children for split operations.
    [[nodiscard]] const std::vector<KeyType>& keys() const;
    [[nodiscard]] const std::vector<PageId>& children() const;
    [[nodiscard]] std::vector<KeyType>& keys();
    [[nodiscard]] std::vector<PageId>& children();

    /// Latch for concurrency (GDB-96).
    [[nodiscard]] std::shared_mutex& latch();

private:
    PageId page_id_;
    PageId parent_page_id_ = invalid_page_id;
    uint16_t max_keys_;
    std::vector<KeyType> keys_;
    std::vector<PageId> children_;
    mutable std::shared_mutex latch_;
};

// -- Leaf Node ---------------------------------------------------------------

/// B+ tree leaf node: stores sorted (key, RID) pairs plus sibling pointers.
class BTreeLeafNode {
public:
    /// @param page_id Logical page ID for this node.
    /// @param max_keys Maximum number of keys this node can hold.
    BTreeLeafNode(PageId page_id, uint16_t max_keys);

    // -- Accessors ---

    [[nodiscard]] PageId page_id() const;
    [[nodiscard]] PageId parent_page_id() const;
    void set_parent_page_id(PageId id);
    [[nodiscard]] PageId next_leaf_id() const;
    void set_next_leaf_id(PageId id);
    [[nodiscard]] PageId prev_leaf_id() const;
    void set_prev_leaf_id(PageId id);
    [[nodiscard]] uint16_t key_count() const;
    [[nodiscard]] uint16_t max_keys() const;
    [[nodiscard]] const KeyType& key_at(uint16_t index) const;
    [[nodiscard]] const RID& rid_at(uint16_t index) const;

    /// Point lookup: find the RID for an exact key match.
    /// Returns nullopt if key not found.
    [[nodiscard]] Result<std::optional<RID>> search(const KeyType& key) const;

    /// Find the position where key would be inserted (for range scan start).
    /// Returns the index of the first key >= the given key.
    [[nodiscard]] Result<uint16_t> lower_bound(const KeyType& key) const;

    /// Insert a (key, rid) pair in sorted order.
    /// If is_unique is true, returns CONSTRAINT_VIOLATION if key already exists.
    [[nodiscard]] Result<void> insert(const KeyType& key, const RID& rid, bool is_unique = false);

    /// Delete the entry with the given key. Returns true if found and deleted.
    [[nodiscard]] Result<bool> remove(const KeyType& key);

    /// True if the node is full (key_count == max_keys).
    [[nodiscard]] bool is_full() const;

    /// True if the node is underfull (below minimum occupancy).
    /// Root nodes are never underfull. Non-root minimum = ceil(max_keys / 2).
    [[nodiscard]] bool is_underfull(bool is_root) const;

    /// Direct access to keys/rids for split operations.
    [[nodiscard]] const std::vector<KeyType>& keys() const;
    [[nodiscard]] const std::vector<RID>& rids() const;
    [[nodiscard]] std::vector<KeyType>& keys();
    [[nodiscard]] std::vector<RID>& rids();

    /// Latch for concurrency (GDB-96).
    [[nodiscard]] std::shared_mutex& latch();

private:
    PageId page_id_;
    PageId parent_page_id_ = invalid_page_id;
    PageId next_leaf_id_ = invalid_page_id;
    PageId prev_leaf_id_ = invalid_page_id;
    uint16_t max_keys_;
    std::vector<KeyType> keys_;
    std::vector<RID> rids_;
    mutable std::shared_mutex latch_;
};

} // namespace giodb
