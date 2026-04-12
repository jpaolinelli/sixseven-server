#include "sixseven/graph/edge_table.h"

#include "sixseven/common/logging.h"
#include "sixseven/index/btree_iterator.h"

namespace sixseven {

EdgeTable::EdgeTable(EdgeTableConfig config) : config_(std::move(config)) {
    // Forward adjacency index: composite key (source_pk, edge_row_id).
    // Unique because edge_row_id is globally unique.
    BTreeConfig fwd_cfg;
    fwd_cfg.key_types = {config_.source_pk_type, TypeId::INT64};
    fwd_cfg.is_unique = true;
    forward_index_ = std::make_unique<BTreeIndex>(fwd_cfg);

    // Reverse adjacency index: composite key (target_pk, edge_row_id).
    BTreeConfig rev_cfg;
    rev_cfg.key_types = {config_.target_pk_type, TypeId::INT64};
    rev_cfg.is_unique = true;
    reverse_index_ = std::make_unique<BTreeIndex>(rev_cfg);

    // Optional unique constraint index on (source_pk, target_pk).
    if (config_.prevent_duplicates) {
        BTreeConfig uniq_cfg;
        uniq_cfg.key_types = {config_.source_pk_type, config_.target_pk_type};
        uniq_cfg.is_unique = true;
        unique_index_ = std::make_unique<BTreeIndex>(uniq_cfg);
    }
}

Result<uint64_t> EdgeTable::insert_edge(const Value& source_pk,
                                        const Value& target_pk,
                                        const std::vector<Value>& properties) {
    std::unique_lock lock(mu_);

    // Validate property count matches schema.
    if (properties.size() != config_.property_columns.size()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "expected " + std::to_string(config_.property_columns.size()) +
                              " properties, got " + std::to_string(properties.size()));
    }

    // Check duplicate constraint if enabled.
    if (unique_index_) {
        KeyType unique_key = {source_pk, target_pk};
        auto existing = unique_index_->search(unique_key);
        if (!existing.has_value()) {
            return tl::unexpected(existing.error());
        }
        if (existing->has_value()) {
            return make_error(StatusCode::CONSTRAINT_VIOLATION,
                              "duplicate edge: (" + config_.name + ") already exists");
        }
    }

    // Assign row ID with overflow guard.
    uint64_t row_id = next_row_id_++;
    if (row_id > kMaxEncodableRowId) {
        --next_row_id_;
        return make_error(StatusCode::INTERNAL_ERROR,
                          "edge row ID overflow: exceeded 48-bit RID encoding capacity");
    }
    Value row_id_val(static_cast<int64_t>(row_id));

    // Insert into forward adjacency index: (source_pk, row_id).
    KeyType fwd_key = {source_pk, row_id_val};
    auto fwd_result = forward_index_->insert(fwd_key, encode_rid(row_id));
    if (!fwd_result.has_value()) {
        return tl::unexpected(fwd_result.error());
    }

    // Insert into reverse adjacency index: (target_pk, row_id).
    KeyType rev_key = {target_pk, row_id_val};
    auto rev_result = reverse_index_->insert(rev_key, encode_rid(row_id));
    if (!rev_result.has_value()) {
        // Rollback forward index insert.
        (void)forward_index_->remove(fwd_key);
        return tl::unexpected(rev_result.error());
    }

    // Insert into unique constraint index if enabled.
    if (unique_index_) {
        KeyType unique_key = {source_pk, target_pk};
        auto uniq_result = unique_index_->insert(unique_key, encode_rid(row_id));
        if (!uniq_result.has_value()) {
            // Rollback forward and reverse index inserts.
            (void)forward_index_->remove(fwd_key);
            (void)reverse_index_->remove(rev_key);
            return tl::unexpected(uniq_result.error());
        }
    }

    // Store the edge row.
    EdgeRow row;
    row.edge_row_id = row_id;
    row.source_pk = source_pk;
    row.target_pk = target_pk;
    row.properties = properties;
    edges_.emplace(row_id, std::move(row));

    SIXSEVEN_LOG_DEBUG("inserted edge {} in edge type '{}'", row_id, config_.name);
    return ok(row_id);
}

Result<std::vector<uint64_t>> EdgeTable::insert_edges_batch(
    const std::vector<EdgeInsertRequest>& edges) {
    if (edges.empty()) {
        return ok(std::vector<uint64_t>{});
    }

    std::unique_lock lock(mu_);

    // Track what we've inserted so we can roll back on partial failure.
    struct Inserted {
        uint64_t row_id;
        KeyType fwd_key;
        KeyType rev_key;
        bool has_unique = false;
        KeyType unique_key;
    };
    std::vector<Inserted> inserted;
    inserted.reserve(edges.size());

    auto rollback = [&]() {
        // Undo in reverse order.
        for (auto it = inserted.rbegin(); it != inserted.rend(); ++it) {
            if (it->has_unique) {
                (void)unique_index_->remove(it->unique_key);
            }
            (void)reverse_index_->remove(it->rev_key);
            (void)forward_index_->remove(it->fwd_key);
            edges_.erase(it->row_id);
        }
        // Roll back the row ID counter.
        next_row_id_ -= inserted.size();
    };

    std::vector<uint64_t> row_ids;
    row_ids.reserve(edges.size());

    for (const auto& e : edges) {
        // Validate property count.
        if (e.properties.size() != config_.property_columns.size()) {
            rollback();
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "expected " + std::to_string(config_.property_columns.size()) +
                                  " properties, got " + std::to_string(e.properties.size()));
        }

        // Duplicate check.
        if (unique_index_) {
            KeyType unique_key = {e.source_pk, e.target_pk};
            auto existing = unique_index_->search(unique_key);
            if (!existing.has_value()) {
                rollback();
                return tl::unexpected(existing.error());
            }
            if (existing->has_value()) {
                rollback();
                return make_error(StatusCode::CONSTRAINT_VIOLATION,
                                  "duplicate edge: (" + config_.name + ") already exists");
            }
        }

        // Assign row ID.
        uint64_t row_id = next_row_id_++;
        if (row_id > kMaxEncodableRowId) {
            --next_row_id_;
            rollback();
            return make_error(StatusCode::INTERNAL_ERROR,
                              "edge row ID overflow: exceeded 48-bit RID encoding capacity");
        }
        Value row_id_val(static_cast<int64_t>(row_id));

        // Forward index insert.
        KeyType fwd_key = {e.source_pk, row_id_val};
        auto fwd = forward_index_->insert(fwd_key, encode_rid(row_id));
        if (!fwd.has_value()) {
            --next_row_id_;
            rollback();
            return tl::unexpected(fwd.error());
        }

        // Reverse index insert.
        KeyType rev_key = {e.target_pk, row_id_val};
        auto rev = reverse_index_->insert(rev_key, encode_rid(row_id));
        if (!rev.has_value()) {
            (void)forward_index_->remove(fwd_key);
            --next_row_id_;
            rollback();
            return tl::unexpected(rev.error());
        }

        // Unique index insert.
        Inserted ins{row_id, fwd_key, rev_key, false, {}};
        if (unique_index_) {
            KeyType unique_key = {e.source_pk, e.target_pk};
            auto uniq = unique_index_->insert(unique_key, encode_rid(row_id));
            if (!uniq.has_value()) {
                (void)forward_index_->remove(fwd_key);
                (void)reverse_index_->remove(rev_key);
                --next_row_id_;
                rollback();
                return tl::unexpected(uniq.error());
            }
            ins.has_unique = true;
            ins.unique_key = unique_key;
        }

        // Store edge row.
        EdgeRow row;
        row.edge_row_id = row_id;
        row.source_pk = e.source_pk;
        row.target_pk = e.target_pk;
        row.properties = e.properties;
        edges_.emplace(row_id, std::move(row));

        inserted.push_back(std::move(ins));
        row_ids.push_back(row_id);
    }

    SIXSEVEN_LOG_DEBUG("batch inserted {} edges in edge type '{}'", row_ids.size(), config_.name);
    return ok(std::move(row_ids));
}

Result<void> EdgeTable::restore_edge(uint64_t edge_row_id,
                                     const Value& source_pk,
                                     const Value& target_pk,
                                     const std::vector<Value>& properties) {
    std::unique_lock lock(mu_);

    if (edge_row_id > kMaxEncodableRowId) {
        return make_error(StatusCode::INTERNAL_ERROR,
                          "edge row ID overflow: exceeded 48-bit RID encoding capacity");
    }

    if (edges_.count(edge_row_id) > 0) {
        return make_error(StatusCode::ALREADY_EXISTS,
                          "duplicate edge row ID during restore: " + std::to_string(edge_row_id));
    }

    Value row_id_val(static_cast<int64_t>(edge_row_id));

    // Insert into forward adjacency index.
    KeyType fwd_key = {source_pk, row_id_val};
    auto fwd_result = forward_index_->insert(fwd_key, encode_rid(edge_row_id));
    if (!fwd_result.has_value()) {
        return tl::unexpected(fwd_result.error());
    }

    // Insert into reverse adjacency index.
    KeyType rev_key = {target_pk, row_id_val};
    auto rev_result = reverse_index_->insert(rev_key, encode_rid(edge_row_id));
    if (!rev_result.has_value()) {
        (void)forward_index_->remove(fwd_key);
        return tl::unexpected(rev_result.error());
    }

    // Insert into unique constraint index if enabled.
    if (unique_index_) {
        KeyType unique_key = {source_pk, target_pk};
        auto uniq_result = unique_index_->insert(unique_key, encode_rid(edge_row_id));
        if (!uniq_result.has_value()) {
            (void)forward_index_->remove(fwd_key);
            (void)reverse_index_->remove(rev_key);
            return tl::unexpected(uniq_result.error());
        }
    }

    // Store the edge row.
    EdgeRow row;
    row.edge_row_id = edge_row_id;
    row.source_pk = source_pk;
    row.target_pk = target_pk;
    row.properties = properties;
    edges_.emplace(edge_row_id, std::move(row));

    // Advance next_row_id_ past any restored IDs.
    if (edge_row_id >= next_row_id_) {
        next_row_id_ = edge_row_id + 1;
    }

    return ok();
}

Result<void> EdgeTable::delete_edge(uint64_t edge_row_id) {
    std::unique_lock lock(mu_);

    auto it = edges_.find(edge_row_id);
    if (it == edges_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "edge row " + std::to_string(edge_row_id) + " not found");
    }

    const EdgeRow& row = it->second;
    Value row_id_val(static_cast<int64_t>(edge_row_id));

    // Remove from forward adjacency index: exact composite key (source_pk, row_id).
    KeyType fwd_key = {row.source_pk, row_id_val};
    auto fwd_result = forward_index_->remove(fwd_key);
    if (!fwd_result.has_value()) {
        return tl::unexpected(fwd_result.error());
    }

    // Remove from reverse adjacency index: exact composite key (target_pk, row_id).
    KeyType rev_key = {row.target_pk, row_id_val};
    auto rev_result = reverse_index_->remove(rev_key);
    if (!rev_result.has_value()) {
        // Rollback forward index removal.
        (void)forward_index_->insert(fwd_key, encode_rid(edge_row_id));
        return tl::unexpected(rev_result.error());
    }

    // Remove from unique constraint index if enabled.
    if (unique_index_) {
        KeyType unique_key = {row.source_pk, row.target_pk};
        auto uniq_result = unique_index_->remove(unique_key);
        if (!uniq_result.has_value()) {
            // Rollback forward and reverse index removals.
            (void)forward_index_->insert(fwd_key, encode_rid(edge_row_id));
            (void)reverse_index_->insert(rev_key, encode_rid(edge_row_id));
            return tl::unexpected(uniq_result.error());
        }
    }

    // Remove from storage.
    edges_.erase(it);

    SIXSEVEN_LOG_DEBUG("deleted edge {} from edge type '{}'", edge_row_id, config_.name);
    return ok();
}

Result<std::vector<EdgeRow>> EdgeTable::get_edges_from(const Value& source_pk) const {
    std::shared_lock lock(mu_);

    auto row_ids = lookup_adjacency(*forward_index_, source_pk);
    if (!row_ids.has_value()) {
        return tl::unexpected(row_ids.error());
    }

    std::vector<EdgeRow> result;
    result.reserve(row_ids->size());
    for (uint64_t id : *row_ids) {
        auto it = edges_.find(id);
        if (it != edges_.end()) {
            result.push_back(it->second);
        }
    }
    return ok(std::move(result));
}

Result<std::vector<EdgeRow>> EdgeTable::get_edges_to(const Value& target_pk) const {
    std::shared_lock lock(mu_);

    auto row_ids = lookup_adjacency(*reverse_index_, target_pk);
    if (!row_ids.has_value()) {
        return tl::unexpected(row_ids.error());
    }

    std::vector<EdgeRow> result;
    result.reserve(row_ids->size());
    for (uint64_t id : *row_ids) {
        auto it = edges_.find(id);
        if (it != edges_.end()) {
            result.push_back(it->second);
        }
    }
    return ok(std::move(result));
}

Result<EdgeRow> EdgeTable::get_edge(uint64_t edge_row_id) const {
    std::shared_lock lock(mu_);

    auto it = edges_.find(edge_row_id);
    if (it == edges_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "edge row " + std::to_string(edge_row_id) + " not found");
    }
    return ok(it->second);
}

Result<std::optional<EdgeRow>> EdgeTable::find_edge(const Value& source_pk,
                                                    const Value& target_pk) const {
    std::shared_lock lock(mu_);

    // If we have a unique index, use it for O(log n) lookup.
    if (unique_index_) {
        KeyType unique_key = {source_pk, target_pk};
        auto search = unique_index_->search(unique_key);
        if (!search.has_value()) {
            return tl::unexpected(search.error());
        }
        if (!search->has_value()) {
            return ok(std::optional<EdgeRow>(std::nullopt));
        }
        uint64_t row_id = decode_rid(**search);
        auto it = edges_.find(row_id);
        if (it == edges_.end()) {
            return ok(std::optional<EdgeRow>(std::nullopt));
        }
        return ok(std::optional<EdgeRow>(it->second));
    }

    // Fallback: scan forward index entries for source_pk and filter by target_pk.
    auto row_ids = lookup_adjacency(*forward_index_, source_pk);
    if (!row_ids.has_value()) {
        return tl::unexpected(row_ids.error());
    }

    for (uint64_t id : *row_ids) {
        auto it = edges_.find(id);
        if (it != edges_.end()) {
            KeyType lhs = {it->second.target_pk};
            KeyType rhs = {target_pk};
            auto cmp = compare_keys(lhs, rhs);
            if (cmp.has_value() && *cmp == std::strong_ordering::equal) {
                return ok(std::optional<EdgeRow>(it->second));
            }
        }
    }
    return ok(std::optional<EdgeRow>(std::nullopt));
}

uint64_t EdgeTable::size() const {
    std::shared_lock lock(mu_);
    return edges_.size();
}

bool EdgeTable::empty() const {
    std::shared_lock lock(mu_);
    return edges_.empty();
}

std::vector<EdgeRow> EdgeTable::get_all_edges() const {
    std::shared_lock lock(mu_);
    std::vector<EdgeRow> result;
    result.reserve(edges_.size());
    for (const auto& [id, row] : edges_) {
        result.push_back(row);
    }
    return result;
}

const EdgeTableConfig& EdgeTable::config() const {
    return config_;
}

Result<std::vector<uint64_t>> EdgeTable::lookup_adjacency(const BTreeIndex& index,
                                                          const Value& key) const {
    // Composite key: (pk_value, min_row_id) as the scan start.
    // We scan until the first component of the key changes.
    KeyType begin_key = {key, Value(static_cast<int64_t>(0))};

    auto iter_result = index.range_scan(begin_key, std::nullopt);
    if (!iter_result.has_value()) {
        return tl::unexpected(iter_result.error());
    }

    std::vector<uint64_t> row_ids;
    KeyType match_key = {key};
    auto& iter = *iter_result;

    while (true) {
        auto entry = iter.next();
        if (!entry.has_value()) {
            // Error during iteration.
            return tl::unexpected(entry.error());
        }
        if (!entry->has_value()) {
            // Iterator exhausted.
            break;
        }
        auto& [entry_key, rid] = **entry;

        // Compare only the first component (the pk value).
        if (entry_key.empty()) {
            break;
        }
        KeyType first_component = {entry_key[0]};
        auto cmp = compare_keys(first_component, match_key);
        if (!cmp.has_value() || *cmp != std::strong_ordering::equal) {
            break;
        }

        row_ids.push_back(decode_rid(rid));
    }

    return ok(std::move(row_ids));
}

RID EdgeTable::encode_rid(uint64_t edge_row_id) {
    // RID packs into 48 bits: 32-bit PageId + 16-bit SlotId.
    static_assert(sizeof(PageId) == 4 && sizeof(SlotId) == 2,
                  "RID packing assumes 32-bit PageId and 16-bit SlotId (48 bits total)");
    return RID{static_cast<PageId>(edge_row_id & 0xFFFFFFFF),
               static_cast<SlotId>((edge_row_id >> 32) & 0xFFFF)};
}

uint64_t EdgeTable::decode_rid(const RID& rid) {
    return static_cast<uint64_t>(rid.page_id) | (static_cast<uint64_t>(rid.slot_id) << 32);
}

} // namespace sixseven
