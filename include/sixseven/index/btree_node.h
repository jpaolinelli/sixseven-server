#pragma once

#include "sixseven/common/result.h"
#include "sixseven/index/btree_key.h"
#include "sixseven/index/rid.h"
#include "sixseven/storage/disk_manager.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace sixseven {

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

    /// Return the key at the given index.
    /// @pre index < key_count() — caller must verify bounds.
    /// Throws std::out_of_range in debug/release if out of bounds.
    [[nodiscard]] const KeyType& key_at(uint16_t index) const;

    /// Return the child page_id at the given index.
    /// @pre index < children().size() — caller must verify bounds.
    /// Throws std::out_of_range in debug/release if out of bounds.
    [[nodiscard]] PageId child_at(uint16_t index) const;

    void set_child_at(uint16_t index, PageId child_id);

    /// Find the child page_id to follow for the given search key.
    /// Uses binary search: returns children_[i] where keys_[i-1] <= key < keys_[i].
    [[nodiscard]] Result<PageId> search(const KeyType& key) const;

    /// Insert a new (key, right_child) pair at the given position.
    /// The key separates children_[pos] and the new right_child.
    /// Returns INTERNAL_ERROR if the node is already at max capacity.
    [[nodiscard]] Result<void> insert_at(uint16_t pos, const KeyType& key, PageId right_child);

    /// Remove key at index. Also removes children_[index + 1].
    void remove_at(uint16_t index);

    /// True if the node is full (key_count == max_keys).
    [[nodiscard]] bool is_full() const;

    /// True if the node is underfull (below minimum occupancy).
    /// Root nodes are never underfull. Non-root minimum = ceil(max_keys / 2).
    ///
    /// @note The minimum occupancy formula (max_keys + 1) / 2 is more aggressive
    /// than the textbook ceil(order/2) - 1 (where order = max_keys + 1).
    /// This triggers merging/redistribution earlier, keeping nodes fuller on
    /// average at the cost of slightly more structural modifications.
    [[nodiscard]] bool is_underfull(bool is_root) const;

    /// Direct access to keys/children for split operations.
    [[nodiscard]] const std::vector<KeyType>& keys() const;
    [[nodiscard]] const std::vector<PageId>& children() const;
    [[nodiscard]] std::vector<KeyType>& keys();
    [[nodiscard]] std::vector<PageId>& children();

private:
    PageId page_id_;
    PageId parent_page_id_ = invalid_page_id;
    uint16_t max_keys_;
    std::vector<KeyType> keys_;
    std::vector<PageId> children_;
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

    /// Return the key at the given index.
    /// @pre index < key_count() — caller must verify bounds.
    /// Throws std::out_of_range in debug/release if out of bounds.
    [[nodiscard]] const KeyType& key_at(uint16_t index) const;

    /// Return the RID at the given index.
    /// @pre index < key_count() — caller must verify bounds.
    /// Throws std::out_of_range in debug/release if out of bounds.
    [[nodiscard]] const RID& rid_at(uint16_t index) const;

    /// Point lookup: find the RID for an exact key match.
    /// Returns nullopt if key not found.
    [[nodiscard]] Result<std::optional<RID>> search(const KeyType& key) const;

    /// Find the position where key would be inserted (for range scan start).
    /// Returns the index of the first key >= the given key.
    [[nodiscard]] Result<uint16_t> lower_bound(const KeyType& key) const;

    /// Insert a (key, rid) pair in sorted order.
    /// If is_unique is true, returns CONSTRAINT_VIOLATION if key already exists.
    ///
    /// @note This method does NOT enforce capacity limits; it always inserts into
    /// the underlying vectors. The caller (BTreeIndex) is responsible for checking
    /// is_full() and splitting the node after the insert when over capacity.
    /// This design allows the "insert-then-split" algorithm where the node
    /// temporarily holds max_keys + 1 entries before being split.
    [[nodiscard]] Result<void> insert(const KeyType& key, const RID& rid, bool is_unique = false);

    /// Delete the entry with the given key. Returns true if found and deleted.
    [[nodiscard]] Result<bool> remove(const KeyType& key);

    /// True if the node is full (key_count == max_keys).
    [[nodiscard]] bool is_full() const;

    /// True if the node is underfull (below minimum occupancy).
    /// Root nodes are never underfull. Non-root minimum = ceil(max_keys / 2).
    ///
    /// @note See BTreeInternalNode::is_underfull for details on the threshold.
    [[nodiscard]] bool is_underfull(bool is_root) const;

    /// Direct access to keys/rids for split operations.
    [[nodiscard]] const std::vector<KeyType>& keys() const;
    [[nodiscard]] const std::vector<RID>& rids() const;
    [[nodiscard]] std::vector<KeyType>& keys();
    [[nodiscard]] std::vector<RID>& rids();

private:
    PageId page_id_;
    PageId parent_page_id_ = invalid_page_id;
    PageId next_leaf_id_ = invalid_page_id;
    PageId prev_leaf_id_ = invalid_page_id;
    uint16_t max_keys_;
    std::vector<KeyType> keys_;
    std::vector<RID> rids_;
};

} // namespace sixseven
