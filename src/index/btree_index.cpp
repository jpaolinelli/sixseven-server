#include "sixseven/index/btree_index.h"

#include "sixseven/index/btree_iterator.h"

#include <shared_mutex>

namespace sixseven {

// -- Default max keys ---------------------------------------------------------

inline constexpr uint16_t default_max_keys = 128;

uint16_t BTreeIndex::effective_internal_max_keys() const {
    return config_.internal_max_keys > 0 ? config_.internal_max_keys : default_max_keys;
}

uint16_t BTreeIndex::effective_leaf_max_keys() const {
    return config_.leaf_max_keys > 0 ? config_.leaf_max_keys : default_max_keys;
}

// -- Construction -------------------------------------------------------------

BTreeIndex::BTreeIndex(BTreeConfig config) : config_(std::move(config)) {}

// -- Node creation ------------------------------------------------------------

BTreeLeafNode* BTreeIndex::create_leaf_node() {
    PageId id = next_page_id_++;
    auto node = std::make_unique<BTreeLeafNode>(id, effective_leaf_max_keys());
    auto* ptr = node.get();
    leaf_nodes_[id] = std::move(node);
    return ptr;
}

BTreeInternalNode* BTreeIndex::create_internal_node() {
    PageId id = next_page_id_++;
    auto node = std::make_unique<BTreeInternalNode>(id, effective_internal_max_keys());
    auto* ptr = node.get();
    internal_nodes_[id] = std::move(node);
    return ptr;
}

// -- Node access --------------------------------------------------------------

BTreeLeafNode* BTreeIndex::get_leaf_node(PageId page_id) {
    auto it = leaf_nodes_.find(page_id);
    return it != leaf_nodes_.end() ? it->second.get() : nullptr;
}

const BTreeLeafNode* BTreeIndex::get_leaf_node(PageId page_id) const {
    auto it = leaf_nodes_.find(page_id);
    return it != leaf_nodes_.end() ? it->second.get() : nullptr;
}

BTreeInternalNode* BTreeIndex::get_internal_node(PageId page_id) {
    auto it = internal_nodes_.find(page_id);
    return it != internal_nodes_.end() ? it->second.get() : nullptr;
}

const BTreeInternalNode* BTreeIndex::get_internal_node(PageId page_id) const {
    auto it = internal_nodes_.find(page_id);
    return it != internal_nodes_.end() ? it->second.get() : nullptr;
}

bool BTreeIndex::is_leaf_node(PageId page_id) const {
    return leaf_nodes_.count(page_id) > 0;
}

std::shared_mutex& BTreeIndex::tree_latch() const {
    return tree_latch_;
}

// -- Accessors ----------------------------------------------------------------

PageId BTreeIndex::root_page_id() const {
    std::shared_lock lock(tree_latch_);
    return root_page_id_;
}

uint64_t BTreeIndex::size() const {
    std::shared_lock lock(tree_latch_);
    return size_;
}

bool BTreeIndex::empty() const {
    std::shared_lock lock(tree_latch_);
    return size_ == 0;
}

const BTreeConfig& BTreeIndex::config() const {
    return config_; // Immutable after construction — no lock needed.
}

// -- Shared helpers (reduce code duplication) ----------------------------------

Result<uint16_t> BTreeIndex::find_child_index(const BTreeInternalNode* parent,
                                              PageId child_id) const {
    const auto& children = parent->children();
    for (uint16_t i = 0; i < static_cast<uint16_t>(children.size()); ++i) {
        if (children[i] == child_id) {
            return ok(i);
        }
    }
    return make_error(StatusCode::INTERNAL_ERROR,
                      "child " + std::to_string(child_id) + " not found in parent " +
                          std::to_string(parent->page_id()));
}

Result<void> BTreeIndex::set_node_parent(PageId child_id, PageId parent_id) {
    if (child_id == invalid_page_id) {
        return ok(); // Silently ignore sentinel.
    }
    auto* leaf = get_leaf_node(child_id);
    if (leaf != nullptr) {
        leaf->set_parent_page_id(parent_id);
        return ok();
    }
    auto* internal = get_internal_node(child_id);
    if (internal != nullptr) {
        internal->set_parent_page_id(parent_id);
        return ok();
    }
    return make_error(StatusCode::INTERNAL_ERROR, "node not found: " + std::to_string(child_id));
}

Result<void> BTreeIndex::handle_post_merge(BTreeInternalNode* parent) {
    if (parent->page_id() == root_page_id_ && parent->key_count() == 0) {
        // Root has no keys left; its only child becomes new root.
        PageId only_child = parent->child_at(0);
        internal_nodes_.erase(root_page_id_);
        root_page_id_ = only_child;
        return set_node_parent(root_page_id_, invalid_page_id);
    }

    if (parent->page_id() != root_page_id_ && parent->is_underfull(false)) {
        return fix_underfull_internal(parent);
    }

    return ok();
}

// -- Traversal ----------------------------------------------------------------

Result<PageId> BTreeIndex::find_leaf(const KeyType& key) const {
    if (root_page_id_ == invalid_page_id) {
        return make_error(StatusCode::NOT_FOUND, "tree is empty");
    }

    PageId current = root_page_id_;

    while (!is_leaf_node(current)) {
        const auto* node = get_internal_node(current);
        if (node == nullptr) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "internal node not found: " + std::to_string(current));
        }
        auto child = node->search(key);
        if (!child.has_value()) {
            return tl::unexpected(child.error());
        }
        current = *child;
    }

    return ok(current);
}

Result<PageId> BTreeIndex::find_leftmost_leaf() const {
    if (root_page_id_ == invalid_page_id) {
        return make_error(StatusCode::NOT_FOUND, "tree is empty");
    }

    PageId current = root_page_id_;

    while (!is_leaf_node(current)) {
        const auto* node = get_internal_node(current);
        if (node == nullptr) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "internal node not found: " + std::to_string(current));
        }
        current = node->child_at(0);
    }

    return ok(current);
}

// -- Insert (GDB-93) ---------------------------------------------------------

Result<void> BTreeIndex::insert(const KeyType& key, const RID& rid) {
    std::unique_lock lock(tree_latch_);

    // If tree is empty, create root leaf.
    if (root_page_id_ == invalid_page_id) {
        auto* leaf = create_leaf_node();
        root_page_id_ = leaf->page_id();
        auto result = leaf->insert(key, rid, config_.is_unique);
        if (!result.has_value()) {
            return tl::unexpected(result.error());
        }
        ++size_;
        return ok();
    }

    // Find the target leaf.
    auto leaf_id_result = find_leaf(key);
    if (!leaf_id_result.has_value()) {
        return tl::unexpected(leaf_id_result.error());
    }
    PageId leaf_id = *leaf_id_result;

    auto* leaf = get_leaf_node(leaf_id);
    if (leaf == nullptr) {
        return make_error(StatusCode::INTERNAL_ERROR,
                          "leaf node not found: " + std::to_string(leaf_id));
    }

    // If leaf has space, insert directly.
    if (!leaf->is_full()) {
        auto result = leaf->insert(key, rid, config_.is_unique);
        if (!result.has_value()) {
            return tl::unexpected(result.error());
        }
        ++size_;
        return ok();
    }

    // Leaf is full — insert-then-split: the leaf temporarily holds max_keys + 1
    // entries. BTreeLeafNode::insert() intentionally does NOT enforce capacity
    // limits; the caller (here) splits the node immediately after the insert.
    auto insert_result = leaf->insert(key, rid, config_.is_unique);
    if (!insert_result.has_value()) {
        return tl::unexpected(insert_result.error());
    }
    ++size_;

    // Split the leaf.
    auto split_result = split_leaf(leaf);
    if (!split_result.has_value()) {
        return tl::unexpected(split_result.error());
    }
    auto& [separator, new_leaf_id] = *split_result;

    // Insert separator into parent.
    return insert_into_parent(leaf_id, separator, new_leaf_id);
}

Result<std::pair<KeyType, PageId>> BTreeIndex::split_leaf(BTreeLeafNode* leaf) {
    auto* new_leaf = create_leaf_node();
    uint16_t total = leaf->key_count();
    uint16_t split_point = total / 2;

    // Move upper half to new leaf.
    auto& old_keys = leaf->keys();
    auto& old_rids = leaf->rids();

    new_leaf->keys().assign(old_keys.begin() + split_point, old_keys.end());
    new_leaf->rids().assign(old_rids.begin() + split_point, old_rids.end());

    old_keys.resize(split_point);
    old_rids.resize(split_point);

    // Link sibling pointers.
    PageId old_next = leaf->next_leaf_id();
    new_leaf->set_next_leaf_id(old_next);
    new_leaf->set_prev_leaf_id(leaf->page_id());
    leaf->set_next_leaf_id(new_leaf->page_id());

    if (old_next != invalid_page_id) {
        auto* old_next_leaf = get_leaf_node(old_next);
        if (old_next_leaf != nullptr) {
            old_next_leaf->set_prev_leaf_id(new_leaf->page_id());
        }
    }

    // Set parent for new leaf.
    new_leaf->set_parent_page_id(leaf->parent_page_id());

    // Separator = first key of new leaf (copy up).
    KeyType separator = new_leaf->key_at(0);

    return ok(std::make_pair(std::move(separator), new_leaf->page_id()));
}

Result<std::pair<KeyType, PageId>> BTreeIndex::split_internal(BTreeInternalNode* node) {
    auto* new_node = create_internal_node();
    uint16_t total = node->key_count();
    uint16_t split_point = total / 2;

    // Middle key is pushed up (not copied into either node).
    KeyType push_up_key = node->key_at(split_point);

    auto& old_keys = node->keys();
    auto& old_children = node->children();

    // New node gets keys after split_point+1 and corresponding children.
    new_node->keys().assign(old_keys.begin() + split_point + 1, old_keys.end());
    new_node->children().assign(old_children.begin() + split_point + 1, old_children.end());

    // Update parent pointers for children moved to new node.
    for (PageId child_id : new_node->children()) {
        auto result = set_node_parent(child_id, new_node->page_id());
        if (!result.has_value()) {
            return tl::unexpected(result.error());
        }
    }

    // Old node keeps keys before split_point and corresponding children.
    old_keys.resize(split_point);
    old_children.resize(split_point + 1);

    // Set parent for new node.
    new_node->set_parent_page_id(node->parent_page_id());

    return ok(std::make_pair(std::move(push_up_key), new_node->page_id()));
}

Result<void>
BTreeIndex::insert_into_parent(PageId left_page_id, const KeyType& key, PageId right_page_id) {
    // Get parent page ID from the left node.
    PageId parent_id = invalid_page_id;
    auto* left_leaf = get_leaf_node(left_page_id);
    if (left_leaf != nullptr) {
        parent_id = left_leaf->parent_page_id();
    } else {
        auto* left_internal = get_internal_node(left_page_id);
        if (left_internal != nullptr) {
            parent_id = left_internal->parent_page_id();
        }
    }

    if (parent_id == invalid_page_id) {
        // Left is the root — create new root.
        return create_new_root(left_page_id, key, right_page_id);
    }

    auto* parent = get_internal_node(parent_id);
    if (parent == nullptr) {
        return make_error(StatusCode::INTERNAL_ERROR,
                          "parent node not found: " + std::to_string(parent_id));
    }

    // Find position to insert separator (verified child-index search).
    auto pos_result = find_child_index(parent, left_page_id);
    if (!pos_result.has_value()) {
        return tl::unexpected(pos_result.error());
    }
    uint16_t pos = *pos_result;

    if (!parent->is_full()) {
        auto result = parent->insert_at(pos, key, right_page_id);
        if (!result.has_value()) {
            return tl::unexpected(result.error());
        }
        auto set_result = set_node_parent(right_page_id, parent_id);
        if (!set_result.has_value()) {
            return tl::unexpected(set_result.error());
        }
        return ok();
    }

    // Parent is full — insert directly into vectors (bypassing capacity check)
    // then split. This is the "insert-then-split" pattern: the internal node
    // temporarily holds max_keys + 1 entries, mirroring the leaf approach.
    parent->keys().insert(parent->keys().begin() + pos, key);
    parent->children().insert(parent->children().begin() + pos + 1, right_page_id);

    auto set_result = set_node_parent(right_page_id, parent_id);
    if (!set_result.has_value()) {
        return tl::unexpected(set_result.error());
    }

    auto split_result = split_internal(parent);
    if (!split_result.has_value()) {
        return tl::unexpected(split_result.error());
    }
    auto& [push_up_key, new_node_id] = *split_result;

    return insert_into_parent(parent_id, push_up_key, new_node_id);
}

Result<void>
BTreeIndex::create_new_root(PageId left_page_id, const KeyType& key, PageId right_page_id) {
    auto* new_root = create_internal_node();
    new_root->keys().push_back(key);
    new_root->children().push_back(left_page_id);
    new_root->children().push_back(right_page_id);

    auto left_result = set_node_parent(left_page_id, new_root->page_id());
    if (!left_result.has_value()) {
        return tl::unexpected(left_result.error());
    }
    auto right_result = set_node_parent(right_page_id, new_root->page_id());
    if (!right_result.has_value()) {
        return tl::unexpected(right_result.error());
    }

    root_page_id_ = new_root->page_id();
    return ok();
}

// -- Search (GDB-94) ---------------------------------------------------------

Result<std::optional<RID>> BTreeIndex::search(const KeyType& key) const {
    std::shared_lock lock(tree_latch_);

    if (root_page_id_ == invalid_page_id) {
        return ok(std::optional<RID>(std::nullopt));
    }

    auto leaf_id_result = find_leaf(key);
    if (!leaf_id_result.has_value()) {
        return tl::unexpected(leaf_id_result.error());
    }

    const auto* leaf = get_leaf_node(*leaf_id_result);
    if (leaf == nullptr) {
        return make_error(StatusCode::INTERNAL_ERROR,
                          "leaf node not found: " + std::to_string(*leaf_id_result));
    }

    return leaf->search(key);
}

Result<BTreeIterator> BTreeIndex::range_scan(const std::optional<KeyType>& begin_key,
                                             const std::optional<KeyType>& end_key) const {
    // Acquire a shared lock that will be transferred to the iterator.
    std::shared_lock lock(tree_latch_);

    if (root_page_id_ == invalid_page_id) {
        return ok(BTreeIterator()); // Empty iterator (lock released).
    }

    PageId start_leaf_id = invalid_page_id;
    uint16_t start_pos = 0;

    if (begin_key.has_value()) {
        auto leaf_id_result = find_leaf(*begin_key);
        if (!leaf_id_result.has_value()) {
            return tl::unexpected(leaf_id_result.error());
        }
        start_leaf_id = *leaf_id_result;

        // find_leaf() routes key >= separator to the right child, so with
        // duplicate keys spanning multiple leaves it may land on the last
        // leaf containing begin_key instead of the first.  Walk backward
        // through prev_leaf_id pointers to find the true leftmost leaf.
        while (true) {
            const auto* cur = get_leaf_node(start_leaf_id);
            if (cur == nullptr) {
                return make_error(StatusCode::INTERNAL_ERROR, "leaf not found");
            }
            PageId prev_id = cur->prev_leaf_id();
            if (prev_id == invalid_page_id) {
                break;
            }
            const auto* prev = get_leaf_node(prev_id);
            if (prev == nullptr || prev->key_count() == 0) {
                break;
            }
            auto cmp = compare_keys(prev->key_at(prev->key_count() - 1), *begin_key);
            if (!cmp.has_value()) {
                return tl::unexpected(cmp.error());
            }
            if (*cmp == std::strong_ordering::less) {
                break; // Previous leaf's last key < begin_key, stop.
            }
            start_leaf_id = prev_id;
        }

        const auto* leaf = get_leaf_node(start_leaf_id);
        if (leaf == nullptr) {
            return make_error(StatusCode::INTERNAL_ERROR, "leaf not found");
        }
        auto pos_result = leaf->lower_bound(*begin_key);
        if (!pos_result.has_value()) {
            return tl::unexpected(pos_result.error());
        }
        start_pos = *pos_result;

        // If position is past end of this leaf, advance to next.
        if (start_pos >= leaf->key_count()) {
            start_leaf_id = leaf->next_leaf_id();
            start_pos = 0;
            if (start_leaf_id == invalid_page_id) {
                return ok(BTreeIterator()); // No results.
            }
        }
    } else {
        auto leftmost = find_leftmost_leaf();
        if (!leftmost.has_value()) {
            return tl::unexpected(leftmost.error());
        }
        start_leaf_id = *leftmost;
        start_pos = 0;
    }

    // Move the shared lock into the iterator so it stays alive for the scan.
    return ok(BTreeIterator(*this, start_leaf_id, start_pos, end_key, std::move(lock)));
}

// -- Delete (GDB-95) ---------------------------------------------------------

Result<bool> BTreeIndex::remove(const KeyType& key) {
    std::unique_lock lock(tree_latch_);

    if (root_page_id_ == invalid_page_id) {
        return ok(false);
    }

    auto leaf_id_result = find_leaf(key);
    if (!leaf_id_result.has_value()) {
        return tl::unexpected(leaf_id_result.error());
    }

    auto* leaf = get_leaf_node(*leaf_id_result);
    if (leaf == nullptr) {
        return make_error(StatusCode::INTERNAL_ERROR, "leaf not found");
    }

    auto remove_result = leaf->remove(key);
    if (!remove_result.has_value()) {
        return tl::unexpected(remove_result.error());
    }
    if (!*remove_result) {
        return ok(false); // Key not found.
    }

    --size_;

    // If root leaf is now empty, clear the tree.
    if (leaf->page_id() == root_page_id_ && leaf->key_count() == 0) {
        leaf_nodes_.erase(root_page_id_);
        root_page_id_ = invalid_page_id;
        return ok(true);
    }

    // Check for underflow.
    if (leaf->page_id() != root_page_id_ && leaf->is_underfull(false)) {
        auto fix = fix_underfull_leaf(leaf);
        if (!fix.has_value()) {
            return tl::unexpected(fix.error());
        }
    }

    return ok(true);
}

// -- RID-qualified delete (GDB-833) ------------------------------------------

Result<bool> BTreeIndex::remove(const KeyType& key, const RID& rid) {
    std::unique_lock lock(tree_latch_);

    if (root_page_id_ == invalid_page_id) {
        return ok(false);
    }

    // Navigate to the leaf that would contain this key.
    auto leaf_id_result = find_leaf(key);
    if (!leaf_id_result.has_value()) {
        return tl::unexpected(leaf_id_result.error());
    }

    // Walk forward through sibling leaves as long as the key matches.
    // Duplicates may span page boundaries, so we scan right until the key
    // changes or we find the target (key, rid) pair.
    PageId current_id = *leaf_id_result;
    while (current_id != invalid_page_id) {
        auto* leaf = get_leaf_node(current_id);
        if (leaf == nullptr) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "leaf page not found during rid-qualified remove");
        }

        // Try to remove the exact (key, rid) pair from this leaf.
        auto remove_result = leaf->remove(key, rid);
        if (!remove_result.has_value()) {
            return tl::unexpected(remove_result.error());
        }
        if (*remove_result) {
            --size_;

            // Handle empty root leaf.
            if (leaf->page_id() == root_page_id_ && leaf->key_count() == 0) {
                leaf_nodes_.erase(root_page_id_);
                root_page_id_ = invalid_page_id;
                return ok(true);
            }

            // Fix underflow.
            if (leaf->page_id() != root_page_id_ && leaf->is_underfull(false)) {
                auto fix = fix_underfull_leaf(leaf);
                if (!fix.has_value()) {
                    return tl::unexpected(fix.error());
                }
            }

            return ok(true);
        }

        // Check if the first key on the next leaf still matches; if not, stop.
        PageId next_id = leaf->next_leaf_id();
        if (next_id == invalid_page_id) {
            break;
        }
        auto* next_leaf = get_leaf_node(next_id);
        if (next_leaf == nullptr || next_leaf->key_count() == 0) {
            break;
        }
        auto first_cmp = compare_keys(key, next_leaf->key_at(0));
        if (!first_cmp.has_value()) {
            return tl::unexpected(first_cmp.error());
        }
        if (*first_cmp != std::strong_ordering::equal) {
            break; // Next leaf starts a different key; (key, rid) not found.
        }

        current_id = next_id;
    }

    return ok(false); // (key, rid) pair not found.
}

Result<void> BTreeIndex::fix_underfull_leaf(BTreeLeafNode* leaf) {
    PageId parent_id = leaf->parent_page_id();
    auto* parent = get_internal_node(parent_id);
    if (parent == nullptr) {
        return make_error(StatusCode::INTERNAL_ERROR, "parent not found for underfull leaf");
    }

    auto ci_result = find_child_index(parent, leaf->page_id());
    if (!ci_result.has_value()) {
        return tl::unexpected(ci_result.error());
    }
    uint16_t child_index = *ci_result;

    uint16_t min_keys = (leaf->max_keys() + 1) / 2;

    // Try redistribute from left sibling.
    if (child_index > 0) {
        auto* left_sibling = get_leaf_node(parent->child_at(child_index - 1));
        if (left_sibling != nullptr && left_sibling->key_count() > min_keys) {
            uint16_t steal_idx = left_sibling->key_count() - 1;
            leaf->keys().insert(leaf->keys().begin(), left_sibling->key_at(steal_idx));
            leaf->rids().insert(leaf->rids().begin(), left_sibling->rid_at(steal_idx));
            left_sibling->keys().pop_back();
            left_sibling->rids().pop_back();

            parent->keys()[child_index - 1] = leaf->key_at(0);
            return ok();
        }
    }

    // Try redistribute from right sibling.
    if (child_index + 1 < static_cast<uint16_t>(parent->children().size())) {
        auto* right_sibling = get_leaf_node(parent->child_at(child_index + 1));
        if (right_sibling != nullptr && right_sibling->key_count() > min_keys) {
            leaf->keys().push_back(right_sibling->key_at(0));
            leaf->rids().push_back(right_sibling->rid_at(0));
            right_sibling->keys().erase(right_sibling->keys().begin());
            right_sibling->rids().erase(right_sibling->rids().begin());

            parent->keys()[child_index] = right_sibling->key_at(0);
            return ok();
        }
    }

    // Merge with a sibling. Prefer merging with left sibling.
    if (child_index > 0) {
        auto* left_sibling = get_leaf_node(parent->child_at(child_index - 1));
        if (left_sibling != nullptr) {
            left_sibling->keys().insert(
                left_sibling->keys().end(), leaf->keys().begin(), leaf->keys().end());
            left_sibling->rids().insert(
                left_sibling->rids().end(), leaf->rids().begin(), leaf->rids().end());

            left_sibling->set_next_leaf_id(leaf->next_leaf_id());
            if (leaf->next_leaf_id() != invalid_page_id) {
                auto* next_leaf = get_leaf_node(leaf->next_leaf_id());
                if (next_leaf != nullptr) {
                    next_leaf->set_prev_leaf_id(left_sibling->page_id());
                }
            }

            PageId leaf_id = leaf->page_id();
            parent->remove_at(child_index - 1);
            leaf_nodes_.erase(leaf_id);

            return handle_post_merge(parent);
        }
    }

    // Merge with right sibling.
    if (child_index + 1 < static_cast<uint16_t>(parent->children().size())) {
        auto* right_sibling = get_leaf_node(parent->child_at(child_index + 1));
        if (right_sibling != nullptr) {
            leaf->keys().insert(
                leaf->keys().end(), right_sibling->keys().begin(), right_sibling->keys().end());
            leaf->rids().insert(
                leaf->rids().end(), right_sibling->rids().begin(), right_sibling->rids().end());

            leaf->set_next_leaf_id(right_sibling->next_leaf_id());
            if (right_sibling->next_leaf_id() != invalid_page_id) {
                auto* next_leaf = get_leaf_node(right_sibling->next_leaf_id());
                if (next_leaf != nullptr) {
                    next_leaf->set_prev_leaf_id(leaf->page_id());
                }
            }

            PageId right_id = right_sibling->page_id();
            parent->remove_at(child_index);
            leaf_nodes_.erase(right_id);

            return handle_post_merge(parent);
        }
    }

    return ok(); // No sibling available (shouldn't happen in valid tree).
}

Result<void> BTreeIndex::fix_underfull_internal(BTreeInternalNode* node) {
    PageId parent_id = node->parent_page_id();
    auto* parent = get_internal_node(parent_id);
    if (parent == nullptr) {
        return make_error(StatusCode::INTERNAL_ERROR,
                          "parent not found for underfull internal node");
    }

    auto ci_result = find_child_index(parent, node->page_id());
    if (!ci_result.has_value()) {
        return tl::unexpected(ci_result.error());
    }
    uint16_t child_index = *ci_result;

    uint16_t min_keys = (node->max_keys() + 1) / 2;

    // Try redistribute from left sibling.
    if (child_index > 0) {
        auto* left_sibling = get_internal_node(parent->child_at(child_index - 1));
        if (left_sibling != nullptr && left_sibling->key_count() > min_keys) {
            KeyType parent_key = parent->key_at(child_index - 1);
            KeyType sibling_last_key = left_sibling->keys().back();
            PageId moved_child = left_sibling->children().back();

            node->keys().insert(node->keys().begin(), parent_key);
            node->children().insert(node->children().begin(), moved_child);

            auto result = set_node_parent(moved_child, node->page_id());
            if (!result.has_value()) {
                return tl::unexpected(result.error());
            }

            parent->keys()[child_index - 1] = sibling_last_key;
            left_sibling->keys().pop_back();
            left_sibling->children().pop_back();

            return ok();
        }
    }

    // Try redistribute from right sibling.
    if (child_index + 1 < static_cast<uint16_t>(parent->children().size())) {
        auto* right_sibling = get_internal_node(parent->child_at(child_index + 1));
        if (right_sibling != nullptr && right_sibling->key_count() > min_keys) {
            KeyType parent_key = parent->key_at(child_index);
            KeyType sibling_first_key = right_sibling->keys().front();
            PageId moved_child = right_sibling->children().front();

            node->keys().push_back(parent_key);
            node->children().push_back(moved_child);

            auto result = set_node_parent(moved_child, node->page_id());
            if (!result.has_value()) {
                return tl::unexpected(result.error());
            }

            parent->keys()[child_index] = sibling_first_key;
            right_sibling->keys().erase(right_sibling->keys().begin());
            right_sibling->children().erase(right_sibling->children().begin());

            return ok();
        }
    }

    // Merge with left sibling.
    if (child_index > 0) {
        auto* left_sibling = get_internal_node(parent->child_at(child_index - 1));
        if (left_sibling != nullptr) {
            KeyType parent_key = parent->key_at(child_index - 1);
            left_sibling->keys().push_back(parent_key);

            left_sibling->keys().insert(
                left_sibling->keys().end(), node->keys().begin(), node->keys().end());
            left_sibling->children().insert(
                left_sibling->children().end(), node->children().begin(), node->children().end());

            for (PageId child_id : node->children()) {
                auto result = set_node_parent(child_id, left_sibling->page_id());
                if (!result.has_value()) {
                    return tl::unexpected(result.error());
                }
            }

            PageId node_id = node->page_id();
            parent->remove_at(child_index - 1);
            internal_nodes_.erase(node_id);

            return handle_post_merge(parent);
        }
    }

    // Merge with right sibling.
    if (child_index + 1 < static_cast<uint16_t>(parent->children().size())) {
        auto* right_sibling = get_internal_node(parent->child_at(child_index + 1));
        if (right_sibling != nullptr) {
            KeyType parent_key = parent->key_at(child_index);
            node->keys().push_back(parent_key);

            node->keys().insert(
                node->keys().end(), right_sibling->keys().begin(), right_sibling->keys().end());
            node->children().insert(node->children().end(),
                                    right_sibling->children().begin(),
                                    right_sibling->children().end());

            for (PageId child_id : right_sibling->children()) {
                auto result = set_node_parent(child_id, node->page_id());
                if (!result.has_value()) {
                    return tl::unexpected(result.error());
                }
            }

            PageId right_id = right_sibling->page_id();
            parent->remove_at(child_index);
            internal_nodes_.erase(right_id);

            return handle_post_merge(parent);
        }
    }

    return ok();
}

// -- Bulk Load (GDB-97) ------------------------------------------------------

Result<void> BTreeIndex::bulk_load(std::vector<std::pair<KeyType, RID>>& sorted_entries) {
    std::unique_lock lock(tree_latch_);

    if (root_page_id_ != invalid_page_id) {
        return make_error(StatusCode::INVALID_ARGUMENT, "bulk load requires an empty tree");
    }

    if (sorted_entries.empty()) {
        return ok();
    }

    // Validate sort order (and uniqueness if configured) in a single pass.
    for (size_t i = 1; i < sorted_entries.size(); ++i) {
        auto cmp = compare_keys(sorted_entries[i - 1].first, sorted_entries[i].first);
        if (!cmp.has_value()) {
            return tl::unexpected(cmp.error());
        }
        if (*cmp == std::strong_ordering::greater) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "bulk load input not sorted at position " + std::to_string(i));
        }
        if (config_.is_unique && *cmp == std::strong_ordering::equal) {
            return make_error(StatusCode::CONSTRAINT_VIOLATION,
                              "duplicate key in sorted input at position " + std::to_string(i));
        }
    }

    uint16_t leaf_max = effective_leaf_max_keys();
    uint16_t fill_target = leaf_max * 2 / 3; // ~67% fill factor.
    if (fill_target == 0) {
        fill_target = 1;
    }

    // Build leaves.
    std::vector<PageId> leaf_ids;
    std::vector<KeyType> separators;
    BTreeLeafNode* prev_leaf = nullptr;

    size_t idx = 0;
    while (idx < sorted_entries.size()) {
        auto* leaf = create_leaf_node();
        leaf_ids.push_back(leaf->page_id());

        uint16_t count = static_cast<uint16_t>(
            std::min(static_cast<size_t>(fill_target), sorted_entries.size() - idx));
        // If this is the last leaf and remaining entries fit, take them all.
        if (sorted_entries.size() - idx <= static_cast<size_t>(leaf_max)) {
            count = static_cast<uint16_t>(sorted_entries.size() - idx);
        }

        for (uint16_t i = 0; i < count; ++i) {
            leaf->keys().push_back(std::move(sorted_entries[idx].first));
            leaf->rids().push_back(sorted_entries[idx].second);
            ++idx;
        }

        // Link sibling pointers.
        if (prev_leaf != nullptr) {
            prev_leaf->set_next_leaf_id(leaf->page_id());
            leaf->set_prev_leaf_id(prev_leaf->page_id());
            separators.push_back(leaf->key_at(0));
        }
        prev_leaf = leaf;
    }

    size_ = sorted_entries.size();

    // If only one leaf, it becomes the root.
    if (leaf_ids.size() == 1) {
        root_page_id_ = leaf_ids[0];
        return ok();
    }

    // Build internal levels bottom-up.
    std::vector<PageId> current_children = leaf_ids;
    std::vector<KeyType> current_separators = std::move(separators);

    while (current_children.size() > 1) {
        uint16_t internal_max = effective_internal_max_keys();
        std::vector<PageId> next_children;
        std::vector<KeyType> next_separators;

        size_t sep_idx = 0;
        size_t child_idx = 0;

        while (child_idx < current_children.size()) {
            auto* internal = create_internal_node();
            next_children.push_back(internal->page_id());

            // First child.
            internal->children().push_back(current_children[child_idx]);
            ++child_idx;

            // Add keys and children up to capacity.
            uint16_t keys_added = 0;
            while (sep_idx < current_separators.size() && keys_added < internal_max &&
                   child_idx < current_children.size()) {
                internal->keys().push_back(std::move(current_separators[sep_idx]));
                internal->children().push_back(current_children[child_idx]);
                ++sep_idx;
                ++child_idx;
                ++keys_added;
            }

            // Set parent pointers for children.
            for (PageId cid : internal->children()) {
                auto result = set_node_parent(cid, internal->page_id());
                if (!result.has_value()) {
                    return tl::unexpected(result.error());
                }
            }

            // Separator for the next level.
            if (sep_idx < current_separators.size() && child_idx < current_children.size()) {
                next_separators.push_back(std::move(current_separators[sep_idx]));
                ++sep_idx;
            }
        }

        current_children = std::move(next_children);
        current_separators = std::move(next_separators);
    }

    root_page_id_ = current_children[0];
    return ok();
}

} // namespace sixseven
