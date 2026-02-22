#include "giodb/index/btree_index.h"

#include "giodb/index/btree_iterator.h"

namespace giodb {

// -- Default max keys ---------------------------------------------------------

static constexpr uint16_t default_max_keys = 128;

uint16_t BTreeIndex::effective_internal_max_keys() const {
    return config_.internal_max_keys > 0 ? config_.internal_max_keys : default_max_keys;
}

uint16_t BTreeIndex::effective_leaf_max_keys() const {
    return config_.leaf_max_keys > 0 ? config_.leaf_max_keys : default_max_keys;
}

// -- Construction -------------------------------------------------------------

BTreeIndex::BTreeIndex(BTreeConfig config, WalWriter* wal)
    : config_(std::move(config)), wal_(wal) {}

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

// -- Accessors ----------------------------------------------------------------

PageId BTreeIndex::root_page_id() const {
    return root_page_id_;
}

uint64_t BTreeIndex::size() const {
    return size_;
}

bool BTreeIndex::empty() const {
    return size_ == 0;
}

const BTreeConfig& BTreeIndex::config() const {
    return config_;
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

PageId BTreeIndex::find_leftmost_leaf() const {
    if (root_page_id_ == invalid_page_id) {
        return invalid_page_id;
    }

    PageId current = root_page_id_;

    while (!is_leaf_node(current)) {
        const auto* node = get_internal_node(current);
        if (node == nullptr) {
            return invalid_page_id;
        }
        current = node->child_at(0);
    }

    return current;
}

// -- Insert (GDB-93) ---------------------------------------------------------

Result<void> BTreeIndex::insert(const KeyType& key, const RID& rid) {
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

    // Leaf is full — need to split.
    // First, insert into the leaf (temporarily over capacity) then split.
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
        auto* child_leaf = get_leaf_node(child_id);
        if (child_leaf != nullptr) {
            child_leaf->set_parent_page_id(new_node->page_id());
        } else {
            auto* child_internal = get_internal_node(child_id);
            if (child_internal != nullptr) {
                child_internal->set_parent_page_id(new_node->page_id());
            }
        }
    }

    // Old node keeps keys before split_point and corresponding children.
    old_keys.resize(split_point);
    old_children.resize(split_point + 1);

    // Set parent for new node.
    new_node->set_parent_page_id(node->parent_page_id());

    return ok(std::make_pair(std::move(push_up_key), new_node->page_id()));
}

Result<void> BTreeIndex::insert_into_parent(PageId left_page_id, const KeyType& key,
                                             PageId right_page_id) {
    // Check if left is the root.
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

    // Find position to insert separator in parent.
    // The separator should be inserted after the child pointer that points to left_page_id.
    uint16_t pos = 0;
    for (uint16_t i = 0; i < static_cast<uint16_t>(parent->children().size()); ++i) {
        if (parent->child_at(i) == left_page_id) {
            pos = i;
            break;
        }
    }

    if (!parent->is_full()) {
        auto result = parent->insert_at(pos, key, right_page_id);
        if (!result.has_value()) {
            return tl::unexpected(result.error());
        }

        // Update right child's parent pointer.
        auto* right_leaf = get_leaf_node(right_page_id);
        if (right_leaf != nullptr) {
            right_leaf->set_parent_page_id(parent_id);
        } else {
            auto* right_internal = get_internal_node(right_page_id);
            if (right_internal != nullptr) {
                right_internal->set_parent_page_id(parent_id);
            }
        }

        return ok();
    }

    // Parent is full — insert directly into vectors (bypassing capacity check) then split.
    parent->keys().insert(parent->keys().begin() + pos, key);
    parent->children().insert(parent->children().begin() + pos + 1, right_page_id);

    // Update right child's parent pointer before split.
    auto* right_leaf = get_leaf_node(right_page_id);
    if (right_leaf != nullptr) {
        right_leaf->set_parent_page_id(parent_id);
    } else {
        auto* right_internal = get_internal_node(right_page_id);
        if (right_internal != nullptr) {
            right_internal->set_parent_page_id(parent_id);
        }
    }

    auto split_result = split_internal(parent);
    if (!split_result.has_value()) {
        return tl::unexpected(split_result.error());
    }
    auto& [push_up_key, new_node_id] = *split_result;

    return insert_into_parent(parent_id, push_up_key, new_node_id);
}

Result<void> BTreeIndex::create_new_root(PageId left_page_id, const KeyType& key,
                                          PageId right_page_id) {
    auto* new_root = create_internal_node();
    new_root->keys().push_back(key);
    new_root->children().push_back(left_page_id);
    new_root->children().push_back(right_page_id);

    // Update parent pointers of children.
    auto* left_leaf = get_leaf_node(left_page_id);
    if (left_leaf != nullptr) {
        left_leaf->set_parent_page_id(new_root->page_id());
    } else {
        auto* left_internal = get_internal_node(left_page_id);
        if (left_internal != nullptr) {
            left_internal->set_parent_page_id(new_root->page_id());
        }
    }

    auto* right_leaf = get_leaf_node(right_page_id);
    if (right_leaf != nullptr) {
        right_leaf->set_parent_page_id(new_root->page_id());
    } else {
        auto* right_internal = get_internal_node(right_page_id);
        if (right_internal != nullptr) {
            right_internal->set_parent_page_id(new_root->page_id());
        }
    }

    root_page_id_ = new_root->page_id();
    return ok();
}

// -- Search (GDB-94) ---------------------------------------------------------

Result<std::optional<RID>> BTreeIndex::search(const KeyType& key) const {
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
    if (root_page_id_ == invalid_page_id) {
        return ok(BTreeIterator()); // Empty iterator.
    }

    PageId start_leaf_id = invalid_page_id;
    uint16_t start_pos = 0;

    if (begin_key.has_value()) {
        auto leaf_id_result = find_leaf(*begin_key);
        if (!leaf_id_result.has_value()) {
            return tl::unexpected(leaf_id_result.error());
        }
        start_leaf_id = *leaf_id_result;

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
        start_leaf_id = find_leftmost_leaf();
        start_pos = 0;
        if (start_leaf_id == invalid_page_id) {
            return ok(BTreeIterator());
        }
    }

    return ok(BTreeIterator(*this, start_leaf_id, start_pos, end_key));
}

// -- Delete (GDB-95) ---------------------------------------------------------

Result<bool> BTreeIndex::remove(const KeyType& key) {
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

Result<void> BTreeIndex::fix_underfull_leaf(BTreeLeafNode* leaf) {
    PageId parent_id = leaf->parent_page_id();
    auto* parent = get_internal_node(parent_id);
    if (parent == nullptr) {
        return make_error(StatusCode::INTERNAL_ERROR, "parent not found for underfull leaf");
    }

    // Find index of this leaf in parent's children.
    uint16_t child_index = 0;
    for (uint16_t i = 0; i < static_cast<uint16_t>(parent->children().size()); ++i) {
        if (parent->child_at(i) == leaf->page_id()) {
            child_index = i;
            break;
        }
    }

    // Try redistribute from left sibling.
    if (child_index > 0) {
        auto* left_sibling = get_leaf_node(parent->child_at(child_index - 1));
        if (left_sibling != nullptr) {
            uint16_t min_keys = (leaf->max_keys() + 1) / 2;
            if (left_sibling->key_count() > min_keys) {
                // Steal last key from left sibling.
                uint16_t steal_idx = left_sibling->key_count() - 1;
                leaf->keys().insert(leaf->keys().begin(), left_sibling->key_at(steal_idx));
                leaf->rids().insert(leaf->rids().begin(), left_sibling->rid_at(steal_idx));
                left_sibling->keys().pop_back();
                left_sibling->rids().pop_back();

                // Update parent separator: parent key at (child_index - 1) = new first key of leaf.
                parent->keys()[child_index - 1] = leaf->key_at(0);
                return ok();
            }
        }
    }

    // Try redistribute from right sibling.
    if (child_index + 1 < static_cast<uint16_t>(parent->children().size())) {
        auto* right_sibling = get_leaf_node(parent->child_at(child_index + 1));
        if (right_sibling != nullptr) {
            uint16_t min_keys = (leaf->max_keys() + 1) / 2;
            if (right_sibling->key_count() > min_keys) {
                // Steal first key from right sibling.
                leaf->keys().push_back(right_sibling->key_at(0));
                leaf->rids().push_back(right_sibling->rid_at(0));
                right_sibling->keys().erase(right_sibling->keys().begin());
                right_sibling->rids().erase(right_sibling->rids().begin());

                // Update parent separator.
                parent->keys()[child_index] = right_sibling->key_at(0);
                return ok();
            }
        }
    }

    // Merge with a sibling. Prefer merging with left sibling.
    if (child_index > 0) {
        auto* left_sibling = get_leaf_node(parent->child_at(child_index - 1));
        if (left_sibling != nullptr) {
            // Merge leaf into left_sibling.
            left_sibling->keys().insert(left_sibling->keys().end(), leaf->keys().begin(),
                                        leaf->keys().end());
            left_sibling->rids().insert(left_sibling->rids().end(), leaf->rids().begin(),
                                        leaf->rids().end());

            // Update sibling pointers.
            left_sibling->set_next_leaf_id(leaf->next_leaf_id());
            if (leaf->next_leaf_id() != invalid_page_id) {
                auto* next_leaf = get_leaf_node(leaf->next_leaf_id());
                if (next_leaf != nullptr) {
                    next_leaf->set_prev_leaf_id(left_sibling->page_id());
                }
            }

            // Remove separator and child pointer from parent.
            PageId leaf_id = leaf->page_id();
            parent->remove_at(child_index - 1);
            leaf_nodes_.erase(leaf_id);

            // Check if parent needs fixing.
            if (parent->page_id() == root_page_id_ && parent->key_count() == 0) {
                // Root has no keys left, its only child becomes new root.
                PageId only_child = parent->child_at(0);
                internal_nodes_.erase(root_page_id_);
                root_page_id_ = only_child;

                // Clear parent pointer of new root.
                auto* new_root_leaf = get_leaf_node(root_page_id_);
                if (new_root_leaf != nullptr) {
                    new_root_leaf->set_parent_page_id(invalid_page_id);
                } else {
                    auto* new_root_internal = get_internal_node(root_page_id_);
                    if (new_root_internal != nullptr) {
                        new_root_internal->set_parent_page_id(invalid_page_id);
                    }
                }
                return ok();
            }

            if (parent->page_id() != root_page_id_ && parent->is_underfull(false)) {
                return fix_underfull_internal(parent);
            }

            return ok();
        }
    }

    // Merge with right sibling.
    if (child_index + 1 < static_cast<uint16_t>(parent->children().size())) {
        auto* right_sibling = get_leaf_node(parent->child_at(child_index + 1));
        if (right_sibling != nullptr) {
            // Merge right_sibling into leaf.
            leaf->keys().insert(leaf->keys().end(), right_sibling->keys().begin(),
                                right_sibling->keys().end());
            leaf->rids().insert(leaf->rids().end(), right_sibling->rids().begin(),
                                right_sibling->rids().end());

            // Update sibling pointers.
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

            if (parent->page_id() == root_page_id_ && parent->key_count() == 0) {
                PageId only_child = parent->child_at(0);
                internal_nodes_.erase(root_page_id_);
                root_page_id_ = only_child;

                auto* new_root_leaf = get_leaf_node(root_page_id_);
                if (new_root_leaf != nullptr) {
                    new_root_leaf->set_parent_page_id(invalid_page_id);
                } else {
                    auto* new_root_internal = get_internal_node(root_page_id_);
                    if (new_root_internal != nullptr) {
                        new_root_internal->set_parent_page_id(invalid_page_id);
                    }
                }
                return ok();
            }

            if (parent->page_id() != root_page_id_ && parent->is_underfull(false)) {
                return fix_underfull_internal(parent);
            }

            return ok();
        }
    }

    return ok(); // No sibling available (shouldn't happen in valid tree).
}

Result<void> BTreeIndex::fix_underfull_internal(BTreeInternalNode* node) {
    PageId parent_id = node->parent_page_id();
    auto* parent = get_internal_node(parent_id);
    if (parent == nullptr) {
        return make_error(StatusCode::INTERNAL_ERROR, "parent not found for underfull internal node");
    }

    // Find index of this node in parent's children.
    uint16_t child_index = 0;
    for (uint16_t i = 0; i < static_cast<uint16_t>(parent->children().size()); ++i) {
        if (parent->child_at(i) == node->page_id()) {
            child_index = i;
            break;
        }
    }

    // Try redistribute from left sibling.
    if (child_index > 0) {
        auto* left_sibling = get_internal_node(parent->child_at(child_index - 1));
        if (left_sibling != nullptr) {
            uint16_t min_keys = (node->max_keys() + 1) / 2;
            if (left_sibling->key_count() > min_keys) {
                // Pull separator down from parent, push last key of sibling up.
                KeyType parent_key = parent->key_at(child_index - 1);
                KeyType sibling_last_key = left_sibling->keys().back();
                PageId moved_child = left_sibling->children().back();

                node->keys().insert(node->keys().begin(), parent_key);
                node->children().insert(node->children().begin(), moved_child);

                // Update parent pointer of moved child.
                auto* moved_leaf = get_leaf_node(moved_child);
                if (moved_leaf != nullptr) {
                    moved_leaf->set_parent_page_id(node->page_id());
                } else {
                    auto* moved_internal = get_internal_node(moved_child);
                    if (moved_internal != nullptr) {
                        moved_internal->set_parent_page_id(node->page_id());
                    }
                }

                parent->keys()[child_index - 1] = sibling_last_key;
                left_sibling->keys().pop_back();
                left_sibling->children().pop_back();

                return ok();
            }
        }
    }

    // Try redistribute from right sibling.
    if (child_index + 1 < static_cast<uint16_t>(parent->children().size())) {
        auto* right_sibling = get_internal_node(parent->child_at(child_index + 1));
        if (right_sibling != nullptr) {
            uint16_t min_keys = (node->max_keys() + 1) / 2;
            if (right_sibling->key_count() > min_keys) {
                KeyType parent_key = parent->key_at(child_index);
                KeyType sibling_first_key = right_sibling->keys().front();
                PageId moved_child = right_sibling->children().front();

                node->keys().push_back(parent_key);
                node->children().push_back(moved_child);

                auto* moved_leaf = get_leaf_node(moved_child);
                if (moved_leaf != nullptr) {
                    moved_leaf->set_parent_page_id(node->page_id());
                } else {
                    auto* moved_internal = get_internal_node(moved_child);
                    if (moved_internal != nullptr) {
                        moved_internal->set_parent_page_id(node->page_id());
                    }
                }

                parent->keys()[child_index] = sibling_first_key;
                right_sibling->keys().erase(right_sibling->keys().begin());
                right_sibling->children().erase(right_sibling->children().begin());

                return ok();
            }
        }
    }

    // Merge with left sibling.
    if (child_index > 0) {
        auto* left_sibling = get_internal_node(parent->child_at(child_index - 1));
        if (left_sibling != nullptr) {
            // Pull separator down from parent.
            KeyType parent_key = parent->key_at(child_index - 1);
            left_sibling->keys().push_back(parent_key);

            // Move all keys and children from node to left sibling.
            left_sibling->keys().insert(left_sibling->keys().end(), node->keys().begin(),
                                        node->keys().end());
            left_sibling->children().insert(left_sibling->children().end(),
                                            node->children().begin(), node->children().end());

            // Update parent pointers for moved children.
            for (PageId child_id : node->children()) {
                auto* child_leaf = get_leaf_node(child_id);
                if (child_leaf != nullptr) {
                    child_leaf->set_parent_page_id(left_sibling->page_id());
                } else {
                    auto* child_internal = get_internal_node(child_id);
                    if (child_internal != nullptr) {
                        child_internal->set_parent_page_id(left_sibling->page_id());
                    }
                }
            }

            PageId node_id = node->page_id();
            parent->remove_at(child_index - 1);
            internal_nodes_.erase(node_id);

            if (parent->page_id() == root_page_id_ && parent->key_count() == 0) {
                PageId only_child = parent->child_at(0);
                internal_nodes_.erase(root_page_id_);
                root_page_id_ = only_child;

                auto* new_root_leaf = get_leaf_node(root_page_id_);
                if (new_root_leaf != nullptr) {
                    new_root_leaf->set_parent_page_id(invalid_page_id);
                } else {
                    auto* new_root_internal = get_internal_node(root_page_id_);
                    if (new_root_internal != nullptr) {
                        new_root_internal->set_parent_page_id(invalid_page_id);
                    }
                }
                return ok();
            }

            if (parent->page_id() != root_page_id_ && parent->is_underfull(false)) {
                return fix_underfull_internal(parent);
            }

            return ok();
        }
    }

    // Merge with right sibling.
    if (child_index + 1 < static_cast<uint16_t>(parent->children().size())) {
        auto* right_sibling = get_internal_node(parent->child_at(child_index + 1));
        if (right_sibling != nullptr) {
            KeyType parent_key = parent->key_at(child_index);
            node->keys().push_back(parent_key);

            node->keys().insert(node->keys().end(), right_sibling->keys().begin(),
                                right_sibling->keys().end());
            node->children().insert(node->children().end(), right_sibling->children().begin(),
                                    right_sibling->children().end());

            for (PageId child_id : right_sibling->children()) {
                auto* child_leaf = get_leaf_node(child_id);
                if (child_leaf != nullptr) {
                    child_leaf->set_parent_page_id(node->page_id());
                } else {
                    auto* child_internal = get_internal_node(child_id);
                    if (child_internal != nullptr) {
                        child_internal->set_parent_page_id(node->page_id());
                    }
                }
            }

            PageId right_id = right_sibling->page_id();
            parent->remove_at(child_index);
            internal_nodes_.erase(right_id);

            if (parent->page_id() == root_page_id_ && parent->key_count() == 0) {
                PageId only_child = parent->child_at(0);
                internal_nodes_.erase(root_page_id_);
                root_page_id_ = only_child;

                auto* new_root_leaf = get_leaf_node(root_page_id_);
                if (new_root_leaf != nullptr) {
                    new_root_leaf->set_parent_page_id(invalid_page_id);
                } else {
                    auto* new_root_internal = get_internal_node(root_page_id_);
                    if (new_root_internal != nullptr) {
                        new_root_internal->set_parent_page_id(invalid_page_id);
                    }
                }
                return ok();
            }

            if (parent->page_id() != root_page_id_ && parent->is_underfull(false)) {
                return fix_underfull_internal(parent);
            }

            return ok();
        }
    }

    return ok();
}

// -- Bulk Load (GDB-97) ------------------------------------------------------

Result<void> BTreeIndex::bulk_load(std::vector<std::pair<KeyType, RID>>& sorted_entries) {
    if (root_page_id_ != invalid_page_id) {
        return make_error(StatusCode::INVALID_ARGUMENT, "bulk load requires an empty tree");
    }

    if (sorted_entries.empty()) {
        return ok();
    }

    // Check for duplicates in unique index.
    if (config_.is_unique) {
        for (size_t i = 1; i < sorted_entries.size(); ++i) {
            auto cmp = compare_keys(sorted_entries[i - 1].first, sorted_entries[i].first);
            if (!cmp.has_value()) {
                return tl::unexpected(cmp.error());
            }
            if (*cmp == std::strong_ordering::equal) {
                return make_error(StatusCode::CONSTRAINT_VIOLATION,
                                  "duplicate key in sorted input at position " +
                                      std::to_string(i));
            }
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

        uint16_t count =
            static_cast<uint16_t>(std::min(static_cast<size_t>(fill_target),
                                           sorted_entries.size() - idx));
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
    bool children_are_leaves = true;

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
                if (children_are_leaves) {
                    auto* child_leaf = get_leaf_node(cid);
                    if (child_leaf != nullptr) {
                        child_leaf->set_parent_page_id(internal->page_id());
                    }
                } else {
                    auto* child_internal = get_internal_node(cid);
                    if (child_internal != nullptr) {
                        child_internal->set_parent_page_id(internal->page_id());
                    }
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
        children_are_leaves = false;
    }

    root_page_id_ = current_children[0];
    return ok();
}

} // namespace giodb
