#include "giodb/index/btree_iterator.h"

#include "giodb/index/btree_index.h"
#include "giodb/index/btree_node.h"

namespace giodb {

BTreeIterator::BTreeIterator(const BTreeIndex& index, PageId start_leaf_id,
                             uint16_t start_pos, std::optional<KeyType> end_key,
                             std::shared_lock<std::shared_mutex> tree_lock)
    : index_(&index),
      current_leaf_id_(start_leaf_id),
      current_pos_(start_pos),
      end_key_(std::move(end_key)),
      exhausted_(false),
      tree_lock_(std::move(tree_lock)) {}

BTreeIterator::BTreeIterator() : exhausted_(true) {}

Result<std::optional<std::pair<KeyType, RID>>> BTreeIterator::next() {
    if (exhausted_) {
        return ok(std::optional<std::pair<KeyType, RID>>(std::nullopt));
    }

    // Get current leaf.
    const auto* leaf = index_->get_leaf_node(current_leaf_id_);
    if (leaf == nullptr) {
        exhausted_ = true;
        return ok(std::optional<std::pair<KeyType, RID>>(std::nullopt));
    }

    // If past end of current leaf, advance to next.
    while (current_pos_ >= leaf->key_count()) {
        PageId next_id = leaf->next_leaf_id();
        if (next_id == invalid_page_id) {
            exhausted_ = true;
            return ok(std::optional<std::pair<KeyType, RID>>(std::nullopt));
        }
        current_leaf_id_ = next_id;
        current_pos_ = 0;
        leaf = index_->get_leaf_node(current_leaf_id_);
        if (leaf == nullptr) {
            exhausted_ = true;
            return ok(std::optional<std::pair<KeyType, RID>>(std::nullopt));
        }
    }

    // Check end bound.
    if (end_key_.has_value()) {
        auto cmp = compare_keys(leaf->key_at(current_pos_), *end_key_);
        if (!cmp.has_value()) {
            return tl::unexpected(cmp.error());
        }
        if (*cmp != std::strong_ordering::less) {
            // Current key >= end_key, stop.
            exhausted_ = true;
            return ok(std::optional<std::pair<KeyType, RID>>(std::nullopt));
        }
    }

    auto result = std::make_pair(leaf->key_at(current_pos_), leaf->rid_at(current_pos_));
    ++current_pos_;
    return ok(std::optional<std::pair<KeyType, RID>>(std::move(result)));
}

bool BTreeIterator::is_end() const {
    return exhausted_;
}

} // namespace giodb
