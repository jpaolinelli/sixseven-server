#pragma once

/// Shared test helpers for B+ tree unit tests.
/// Used by test_btree_node.cpp, test_btree.cpp, and test_btree_concurrency.cpp.

#include "sixseven/index/btree_index.h"
#include "sixseven/index/btree_iterator.h"
#include "sixseven/index/btree_key.h"
#include "sixseven/index/btree_node.h"
#include "sixseven/index/rid.h"

#include <string>
#include <vector>

namespace sixseven::test {

/// Create a BTreeIndex with small capacity for testing (forces splits early).
inline BTreeIndex
make_test_index(uint16_t leaf_max = 4, uint16_t internal_max = 4, bool is_unique = false) {
    BTreeConfig config;
    config.key_types = {TypeId::INT64};
    config.leaf_max_keys = leaf_max;
    config.internal_max_keys = internal_max;
    config.is_unique = is_unique;
    return BTreeIndex(std::move(config));
}

/// Create a single-column INT64 key.
inline KeyType make_key(int64_t v) {
    return {Value(v)};
}

/// Create a composite (INT64, STRING) key.
inline KeyType make_composite_key(int64_t v, const std::string& s) {
    return {Value(v), Value(s)};
}

/// Create an RID with page_id = value, slot_id = 0.
inline RID make_rid(uint32_t page_id, uint16_t slot_id = 0) {
    return {page_id, slot_id};
}

/// Helper to extract INT64 from a single-column key.
inline int64_t key_val(const KeyType& k) {
    return std::get<int64_t>(k[0].data());
}

/// Collect all (key, rid) pairs from a range scan into a vector.
/// Advances the iterator until exhausted or an error occurs.
inline Result<std::vector<std::pair<KeyType, RID>>> collect_scan(BTreeIterator& it) {
    std::vector<std::pair<KeyType, RID>> results;
    while (!it.is_end()) {
        auto entry = it.next();
        if (!entry.has_value()) {
            return tl::unexpected(entry.error());
        }
        if (!entry->has_value()) {
            break;
        }
        results.push_back(std::move(**entry));
    }
    return ok(std::move(results));
}

} // namespace sixseven::test
