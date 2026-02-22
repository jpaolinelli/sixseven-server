#include "giodb/index/btree_node.h"

#include <algorithm>

namespace giodb {

// =============================================================================
// BTreeInternalNode
// =============================================================================

BTreeInternalNode::BTreeInternalNode(PageId page_id, uint16_t max_keys)
    : page_id_(page_id), max_keys_(max_keys) {}

PageId BTreeInternalNode::page_id() const {
    return page_id_;
}

PageId BTreeInternalNode::parent_page_id() const {
    return parent_page_id_;
}

void BTreeInternalNode::set_parent_page_id(PageId id) {
    parent_page_id_ = id;
}

uint16_t BTreeInternalNode::key_count() const {
    return static_cast<uint16_t>(keys_.size());
}

uint16_t BTreeInternalNode::max_keys() const {
    return max_keys_;
}

const KeyType& BTreeInternalNode::key_at(uint16_t index) const {
    return keys_.at(index);
}

PageId BTreeInternalNode::child_at(uint16_t index) const {
    return children_.at(index);
}

void BTreeInternalNode::set_child_at(uint16_t index, PageId child_id) {
    children_.at(index) = child_id;
}

Result<PageId> BTreeInternalNode::search(const KeyType& key) const {
    if (children_.empty()) {
        return make_error(StatusCode::INTERNAL_ERROR, "internal node has no children");
    }

    // Binary search: find the first key > search key.
    // The child to the left of that key is the one to follow.
    uint16_t lo = 0;
    uint16_t hi = key_count();

    while (lo < hi) {
        uint16_t mid = lo + (hi - lo) / 2;
        auto cmp = compare_keys(key, keys_[mid]);
        if (!cmp.has_value()) {
            return tl::unexpected(cmp.error());
        }
        if (*cmp == std::strong_ordering::less) {
            hi = mid;
        } else {
            // key >= keys_[mid], go right.
            lo = mid + 1;
        }
    }

    // lo is the index of the first key > search key.
    // The child at index lo is the correct subtree.
    return ok(children_[lo]);
}

Result<void> BTreeInternalNode::insert_at(uint16_t pos, const KeyType& key,
                                           PageId right_child) {
    if (key_count() >= max_keys_) {
        return make_error(StatusCode::INTERNAL_ERROR, "internal node is full");
    }

    keys_.insert(keys_.begin() + pos, key);
    children_.insert(children_.begin() + pos + 1, right_child);
    return ok();
}

void BTreeInternalNode::remove_at(uint16_t index) {
    keys_.erase(keys_.begin() + index);
    children_.erase(children_.begin() + index + 1);
}

bool BTreeInternalNode::is_full() const {
    return key_count() >= max_keys_;
}

bool BTreeInternalNode::is_underfull(bool is_root) const {
    if (is_root) {
        return false;
    }
    uint16_t min_keys = (max_keys_ + 1) / 2;
    return key_count() < min_keys;
}

const std::vector<KeyType>& BTreeInternalNode::keys() const {
    return keys_;
}

const std::vector<PageId>& BTreeInternalNode::children() const {
    return children_;
}

std::vector<KeyType>& BTreeInternalNode::keys() {
    return keys_;
}

std::vector<PageId>& BTreeInternalNode::children() {
    return children_;
}

std::shared_mutex& BTreeInternalNode::latch() {
    return latch_;
}

// =============================================================================
// BTreeLeafNode
// =============================================================================

BTreeLeafNode::BTreeLeafNode(PageId page_id, uint16_t max_keys)
    : page_id_(page_id), max_keys_(max_keys) {}

PageId BTreeLeafNode::page_id() const {
    return page_id_;
}

PageId BTreeLeafNode::parent_page_id() const {
    return parent_page_id_;
}

void BTreeLeafNode::set_parent_page_id(PageId id) {
    parent_page_id_ = id;
}

PageId BTreeLeafNode::next_leaf_id() const {
    return next_leaf_id_;
}

void BTreeLeafNode::set_next_leaf_id(PageId id) {
    next_leaf_id_ = id;
}

PageId BTreeLeafNode::prev_leaf_id() const {
    return prev_leaf_id_;
}

void BTreeLeafNode::set_prev_leaf_id(PageId id) {
    prev_leaf_id_ = id;
}

uint16_t BTreeLeafNode::key_count() const {
    return static_cast<uint16_t>(keys_.size());
}

uint16_t BTreeLeafNode::max_keys() const {
    return max_keys_;
}

const KeyType& BTreeLeafNode::key_at(uint16_t index) const {
    return keys_.at(index);
}

const RID& BTreeLeafNode::rid_at(uint16_t index) const {
    return rids_.at(index);
}

Result<std::optional<RID>> BTreeLeafNode::search(const KeyType& key) const {
    // Binary search for exact match.
    uint16_t lo = 0;
    uint16_t hi = key_count();

    while (lo < hi) {
        uint16_t mid = lo + (hi - lo) / 2;
        auto cmp = compare_keys(key, keys_[mid]);
        if (!cmp.has_value()) {
            return tl::unexpected(cmp.error());
        }
        if (*cmp == std::strong_ordering::equal) {
            return ok(std::optional<RID>(rids_[mid]));
        }
        if (*cmp == std::strong_ordering::less) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }

    return ok(std::optional<RID>(std::nullopt));
}

Result<uint16_t> BTreeLeafNode::lower_bound(const KeyType& key) const {
    uint16_t lo = 0;
    uint16_t hi = key_count();

    while (lo < hi) {
        uint16_t mid = lo + (hi - lo) / 2;
        auto cmp = compare_keys(keys_[mid], key);
        if (!cmp.has_value()) {
            return tl::unexpected(cmp.error());
        }
        if (*cmp == std::strong_ordering::less) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return ok(lo);
}

Result<void> BTreeLeafNode::insert(const KeyType& key, const RID& rid, bool is_unique) {
    // Find insertion position via binary search.
    auto pos_result = lower_bound(key);
    if (!pos_result.has_value()) {
        return tl::unexpected(pos_result.error());
    }
    uint16_t pos = *pos_result;

    // Check for duplicate if unique.
    if (is_unique && pos < key_count()) {
        auto cmp = compare_keys(key, keys_[pos]);
        if (!cmp.has_value()) {
            return tl::unexpected(cmp.error());
        }
        if (*cmp == std::strong_ordering::equal) {
            return make_error(StatusCode::CONSTRAINT_VIOLATION, "duplicate key in unique index");
        }
    }

    keys_.insert(keys_.begin() + pos, key);
    rids_.insert(rids_.begin() + pos, rid);
    return ok();
}

Result<bool> BTreeLeafNode::remove(const KeyType& key) {
    // Binary search for exact match.
    uint16_t lo = 0;
    uint16_t hi = key_count();

    while (lo < hi) {
        uint16_t mid = lo + (hi - lo) / 2;
        auto cmp = compare_keys(key, keys_[mid]);
        if (!cmp.has_value()) {
            return tl::unexpected(cmp.error());
        }
        if (*cmp == std::strong_ordering::equal) {
            keys_.erase(keys_.begin() + mid);
            rids_.erase(rids_.begin() + mid);
            return ok(true);
        }
        if (*cmp == std::strong_ordering::less) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }

    return ok(false);
}

bool BTreeLeafNode::is_full() const {
    return key_count() >= max_keys_;
}

bool BTreeLeafNode::is_underfull(bool is_root) const {
    if (is_root) {
        return false;
    }
    uint16_t min_keys = (max_keys_ + 1) / 2;
    return key_count() < min_keys;
}

const std::vector<KeyType>& BTreeLeafNode::keys() const {
    return keys_;
}

const std::vector<RID>& BTreeLeafNode::rids() const {
    return rids_;
}

std::vector<KeyType>& BTreeLeafNode::keys() {
    return keys_;
}

std::vector<RID>& BTreeLeafNode::rids() {
    return rids_;
}

std::shared_mutex& BTreeLeafNode::latch() {
    return latch_;
}

} // namespace giodb
