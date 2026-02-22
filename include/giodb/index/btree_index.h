#pragma once

#include "giodb/common/result.h"
#include "giodb/common/types.h"
#include "giodb/index/btree_key.h"
#include "giodb/index/btree_node.h"
#include "giodb/index/rid.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace giodb {

// Forward declaration.
class WalWriter;
class BTreeIterator;

/// Configuration for creating a B+ tree index.
struct BTreeConfig {
    /// Column types in the composite key (determines comparison semantics).
    std::vector<TypeId> key_types;

    /// Maximum keys per internal node (0 = use default of 128).
    uint16_t internal_max_keys = 0;

    /// Maximum keys per leaf node (0 = use default of 128).
    uint16_t leaf_max_keys = 0;

    /// Whether this index enforces uniqueness.
    bool is_unique = false;
};

/// B+ tree index: supports insert, point lookup, range scan, delete, bulk load.
class BTreeIndex {
public:
    /// Construct a B+ tree index.
    /// @param config Index configuration.
    /// @param wal Optional WAL writer for logging structural changes.
    explicit BTreeIndex(BTreeConfig config, WalWriter* wal = nullptr);

    /// Insert a (key, rid) pair into the index.
    /// Returns CONSTRAINT_VIOLATION if is_unique and key already exists.
    [[nodiscard]] Result<void> insert(const KeyType& key, const RID& rid);

    /// Point lookup: find the RID for an exact key match.
    [[nodiscard]] Result<std::optional<RID>> search(const KeyType& key) const;

    /// Range scan: returns an iterator over [begin_key, end_key).
    /// Pass nullopt for open-ended bounds.
    [[nodiscard]] Result<BTreeIterator> range_scan(
        const std::optional<KeyType>& begin_key,
        const std::optional<KeyType>& end_key) const;

    /// Delete the entry with the given key.
    /// Returns true if the key was found and deleted.
    [[nodiscard]] Result<bool> remove(const KeyType& key);

    /// Bulk load from pre-sorted entries. Tree must be empty.
    [[nodiscard]] Result<void> bulk_load(
        std::vector<std::pair<KeyType, RID>>& sorted_entries);

    /// Return the root page ID (invalid_page_id if tree is empty).
    [[nodiscard]] PageId root_page_id() const;

    /// Return the total number of entries in the index.
    [[nodiscard]] uint64_t size() const;

    /// Return whether the index is empty.
    [[nodiscard]] bool empty() const;

    /// Return the index configuration.
    [[nodiscard]] const BTreeConfig& config() const;

    // -- Node access (for iterator and internal use) --

    [[nodiscard]] BTreeLeafNode* get_leaf_node(PageId page_id);
    [[nodiscard]] const BTreeLeafNode* get_leaf_node(PageId page_id) const;
    [[nodiscard]] BTreeInternalNode* get_internal_node(PageId page_id);
    [[nodiscard]] const BTreeInternalNode* get_internal_node(PageId page_id) const;

    /// Return true if the given page_id belongs to a leaf node.
    [[nodiscard]] bool is_leaf_node(PageId page_id) const;

private:
    // -- Node creation ---
    [[nodiscard]] BTreeLeafNode* create_leaf_node();
    [[nodiscard]] BTreeInternalNode* create_internal_node();

    // -- Traversal ---
    [[nodiscard]] Result<PageId> find_leaf(const KeyType& key) const;

    /// Find the leftmost leaf (for open-begin range scans).
    [[nodiscard]] PageId find_leftmost_leaf() const;

    // -- Structural modification ---
    [[nodiscard]] Result<std::pair<KeyType, PageId>> split_leaf(BTreeLeafNode* leaf);
    [[nodiscard]] Result<std::pair<KeyType, PageId>> split_internal(BTreeInternalNode* node);
    [[nodiscard]] Result<void> insert_into_parent(
        PageId left_page_id, const KeyType& key, PageId right_page_id);
    [[nodiscard]] Result<void> create_new_root(
        PageId left_page_id, const KeyType& key, PageId right_page_id);

    // -- Delete helpers ---
    [[nodiscard]] Result<void> fix_underfull_leaf(BTreeLeafNode* leaf);
    [[nodiscard]] Result<void> fix_underfull_internal(BTreeInternalNode* node);

    // -- Internal max key getters ---
    [[nodiscard]] uint16_t effective_internal_max_keys() const;
    [[nodiscard]] uint16_t effective_leaf_max_keys() const;

    // -- State ---
    BTreeConfig config_;
    [[maybe_unused]] WalWriter* wal_;
    PageId root_page_id_ = invalid_page_id;
    uint64_t size_ = 0;
    PageId next_page_id_ = 1; // Page ID allocator (0 is invalid).

    /// Node storage.
    std::unordered_map<PageId, std::unique_ptr<BTreeLeafNode>> leaf_nodes_;
    std::unordered_map<PageId, std::unique_ptr<BTreeInternalNode>> internal_nodes_;

    /// Tree-level latch for root pointer changes (GDB-96).
    mutable std::shared_mutex tree_latch_;
};

} // namespace giodb
