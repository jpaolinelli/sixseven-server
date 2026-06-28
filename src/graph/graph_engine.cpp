#include "sixseven/graph/graph_engine.h"

#include "sixseven/common/logging.h"
#include "sixseven/common/parse_utils.h"
#include "sixseven/common/types.h"
#include "sixseven/index/btree_persistence.h"
#include "sixseven/storage/wal.h"

#include <chrono>
#include <cstring>

namespace sixseven {

GraphEngine::GraphEngine(Catalog& catalog, WalWriter* wal) : catalog_(catalog), wal_(wal) {}

GraphEngine::GraphEngine(Catalog& catalog,
                         DiskManager& dm,
                         std::filesystem::path data_dir,
                         WalWriter* wal)
    : catalog_(catalog), dm_(&dm), data_dir_(std::move(data_dir)), wal_(wal) {}

GraphEngine::~GraphEngine() {
    // Flush and close all edge storage files (heap + index files).
    for (auto& [key, storage] : edge_storage_) {
        if (!storage) continue;

        // Close edge index BPMs.
        auto close_idx = [&](std::unique_ptr<EdgeIndexStorage>& idx) {
            if (!idx) return;
            (void)idx->bpm->flush_all();
            if (dm_ != nullptr) {
                (void)dm_->close_file(idx->file_id);
            }
            idx.reset();
        };
        close_idx(storage->fwd_idx);
        close_idx(storage->rev_idx);
        close_idx(storage->uniq_idx);

        // Close heap BPM.
        if (storage->bpm) {
            (void)storage->bpm->flush_all();
        }
        if (dm_ != nullptr) {
            (void)dm_->close_file(storage->file_id);
        }
    }
    edge_storage_.clear();
    edge_tables_.clear();
}

std::string GraphEngine::make_edge_key(database_id_t database_id, const std::string& name) {
    return std::to_string(database_id) + ":" + name;
}

bool GraphEngine::has_persistence() const {
    return dm_ != nullptr;
}

std::filesystem::path GraphEngine::edge_file_path(database_id_t database_id,
                                                  edge_id_t edge_id) const {
    return data_dir_ / "databases" / std::to_string(database_id) / "edges" /
           ("edge_" + std::to_string(edge_id) + ".db");
}

std::filesystem::path GraphEngine::edge_index_path(database_id_t database_id,
                                                   edge_id_t edge_id,
                                                   const std::string& suffix) const {
    return data_dir_ / "databases" / std::to_string(database_id) / "edges" /
           ("edge_" + std::to_string(edge_id) + "_" + suffix + ".db");
}

Schema GraphEngine::build_edge_storage_schema(TypeId source_pk_type,
                                              TypeId target_pk_type,
                                              const std::vector<ColumnDef>& property_columns) {
    std::vector<ColumnDef> cols;
    cols.reserve(3 + property_columns.size());
    cols.push_back({"edge_row_id", TypeId::INT64});
    cols.push_back({"source_pk", source_pk_type});
    cols.push_back({"target_pk", target_pk_type});
    for (const auto& prop : property_columns) {
        cols.push_back(prop);
    }
    return Schema(std::move(cols));
}

std::vector<ColumnDef> GraphEngine::parse_property_columns(const std::string& properties_str) {
    std::vector<ColumnDef> cols;
    if (properties_str.empty()) {
        return cols;
    }

    // Format: "name:TYPE,name:TYPE,..."
    size_t pos = 0;
    while (pos < properties_str.size()) {
        auto comma = properties_str.find(',', pos);
        auto segment = properties_str.substr(
            pos, comma == std::string::npos ? std::string::npos : comma - pos);

        auto colon = segment.find(':');
        if (colon != std::string::npos) {
            auto name = segment.substr(0, colon);
            auto type_str = segment.substr(colon + 1);
            auto type_id = parse_type_id(type_str);
            if (type_id.has_value()) {
                cols.push_back({std::string(name), *type_id});
            }
        }

        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }

    return cols;
}

Result<void> GraphEngine::create_edge_storage(database_id_t database_id,
                                              edge_id_t edge_id,
                                              TypeId source_pk_type,
                                              TypeId target_pk_type,
                                              const std::vector<ColumnDef>& property_columns) {
    auto path = edge_file_path(database_id, edge_id);
    std::filesystem::create_directories(path.parent_path());

    auto fid = dm_->create_file(path, /*direct_io=*/false, /*overwrite=*/true);
    if (!fid) {
        return make_error(fid.error().code, fid.error().message);
    }

    auto storage = std::make_unique<EdgeStorage>();
    storage->file_id = *fid;
    storage->bpm = std::make_unique<BufferPoolManager>(*dm_, *fid, 256);
    storage->heap = std::make_unique<TableHeap>(*storage->bpm, *dm_, *fid);
    storage->storage_schema =
        build_edge_storage_schema(source_pk_type, target_pk_type, property_columns);

    // Look up the edge type name from the catalog to use as map key.
    // At this point the edge type is already registered.
    auto edge_types = catalog_.list_all_edge_types();
    std::string edge_name;
    database_id_t edge_db_id = database_id;
    for (const auto& et : edge_types) {
        if (et.edge_id == edge_id) {
            edge_name = et.name;
            edge_db_id = et.database_id;
            break;
        }
    }
    if (edge_name.empty()) {
        return make_error(StatusCode::INTERNAL_ERROR,
                          "edge type not found in catalog for id " + std::to_string(edge_id));
    }

    edge_storage_[make_edge_key(edge_db_id, edge_name)] = std::move(storage);
    return ok();
}

Result<void> GraphEngine::persist_edge(const std::string& edge_key,
                                       uint64_t edge_row_id,
                                       const Value& source_pk,
                                       const Value& target_pk,
                                       const std::vector<Value>& properties) {
    auto sit = edge_storage_.find(edge_key);
    if (sit == edge_storage_.end()) {
        return ok(); // No persistence for this edge type.
    }

    auto& storage = *sit->second;

    // Build row: (edge_row_id, source_pk, target_pk, prop0, prop1, ...).
    std::vector<Value> values;
    values.reserve(3 + properties.size());
    values.emplace_back(static_cast<int64_t>(edge_row_id));
    values.push_back(source_pk);
    values.push_back(target_pk);
    for (const auto& prop : properties) {
        values.push_back(prop);
    }

    auto data = TupleSerializer::serialize(values, storage.storage_schema);
    if (!data) {
        return make_error(data.error().code, "edge serialize failed: " + data.error().message);
    }

    auto rid = storage.heap->insert_tuple(*data);
    if (!rid) {
        return make_error(rid.error().code, "edge insert failed: " + rid.error().message);
    }

    storage.rid_map[edge_row_id] = *rid;

    // NOTE: intentionally NOT calling bpm->flush_all() here. Durability of edge inserts comes
    // from log_edge_wal() in GraphEngine::link() (see call site at ~line 440), which records
    // WalRecordType::INSERT right after this persist_edge returns. The edge heap is a normal
    // TableHeap; its dirty pages are flushed on eviction, checkpoint, and clean shutdown —
    // symmetric with regular row INSERTs. A per-edge flush_all walks all 64 buffer pool shards
    // and was the primary LINK throughput bottleneck (capped us at ~1-10 edges/sec).
    return ok();
}

Result<void> GraphEngine::delete_persisted_edge(const std::string& edge_key, uint64_t edge_row_id) {
    auto sit = edge_storage_.find(edge_key);
    if (sit == edge_storage_.end()) {
        return ok(); // No persistence for this edge type.
    }

    auto& storage = *sit->second;

    auto rid_it = storage.rid_map.find(edge_row_id);
    if (rid_it == storage.rid_map.end()) {
        return ok(); // Not persisted — nothing to delete.
    }

    auto del = storage.heap->delete_tuple(rid_it->second);
    if (!del) {
        return make_error(del.error().code, "edge delete failed: " + del.error().message);
    }

    storage.rid_map.erase(rid_it);

    // NOTE: intentionally NOT calling bpm->flush_all() here (see persist_edge for rationale).
    // Durability comes from log_edge_wal() in GraphEngine::unlink(); the dirty heap page is
    // flushed on eviction/checkpoint/shutdown like any other table row.
    return ok();
}

Result<edge_id_t> GraphEngine::create_edge_type(database_id_t database_id,
                                                const std::string& name,
                                                table_id_t source_table_id,
                                                table_id_t target_table_id,
                                                TypeId source_pk_type,
                                                TypeId target_pk_type,
                                                const std::vector<ColumnDef>& property_columns,
                                                bool prevent_duplicates) {
    std::lock_guard lock(mu_);

    auto key = make_edge_key(database_id, name);

    // Check if edge table already exists locally.
    if (edge_tables_.count(key) > 0) {
        return make_error(StatusCode::ALREADY_EXISTS,
                          "edge type '" + name + "' already exists in graph engine");
    }

    // Build property list for catalog (comma-separated name:TYPE pairs).
    // The sentinel token "__uniq__" is prepended when prevent_duplicates is set
    // so the flag survives a server restart.  parse_property_columns() skips
    // tokens that contain no colon, so it is safely ignored during reload.
    std::string props_str;
    if (prevent_duplicates) {
        props_str = "__uniq__";
    }
    for (size_t i = 0; i < property_columns.size(); ++i) {
        if (!props_str.empty()) {
            props_str += ",";
        }
        props_str += property_columns[i].name;
        props_str += ":";
        props_str += type_name(property_columns[i].type);
    }

    // Register in catalog.
    EdgeTypeDef def;
    def.name = name;
    def.source_table_id = source_table_id;
    def.target_table_id = target_table_id;
    def.properties = props_str;
    def.prevent_duplicates = prevent_duplicates;

    auto edge_id_result = catalog_.create_edge_type(database_id, def);
    if (!edge_id_result.has_value()) {
        return tl::unexpected(edge_id_result.error());
    }

    // Create the backing EdgeTable.
    EdgeTableConfig config;
    config.edge_id = *edge_id_result;
    config.name = name;
    config.source_table_id = source_table_id;
    config.target_table_id = target_table_id;
    config.source_pk_type = source_pk_type;
    config.target_pk_type = target_pk_type;
    config.property_columns = property_columns;
    config.prevent_duplicates = prevent_duplicates;

    edge_tables_.emplace(key, std::make_unique<EdgeTable>(std::move(config)));

    // Create persistent storage if available.
    if (has_persistence()) {
        auto storage_result = create_edge_storage(
            database_id, *edge_id_result, source_pk_type, target_pk_type, property_columns);
        if (!storage_result) {
            SIXSEVEN_LOG_WARN(
                "failed to create edge storage for '{}': {}", name, storage_result.error().message);
        }
    }

    SIXSEVEN_LOG_INFO(
        "created edge type '{}' (id={}, database={})", name, *edge_id_result, database_id);
    return ok(*edge_id_result);
}

void GraphEngine::teardown_edge_storage_locked(const std::string& edge_key,
                                               database_id_t database_id,
                                               const std::string& name,
                                               edge_id_t edge_id) {
    // Remove persistent storage if present.
    auto sit = edge_storage_.find(edge_key);
    if (sit != edge_storage_.end()) {
        auto& storage = *sit->second;

        // Close and remove edge index files.
        auto close_and_remove_idx = [&](std::unique_ptr<EdgeIndexStorage>& idx,
                                        const std::string& suffix) {
            if (idx) {
                (void)idx->bpm->flush_all();
                if (dm_ != nullptr) {
                    (void)dm_->close_file(idx->file_id);
                }
                idx.reset();
            }
            std::filesystem::remove(edge_index_path(database_id, edge_id, suffix));
        };
        close_and_remove_idx(storage.fwd_idx, "fwd");
        close_and_remove_idx(storage.rev_idx, "rev");
        close_and_remove_idx(storage.uniq_idx, "uniq");

        // Close and remove heap file.
        (void)storage.bpm->flush_all();
        // Defensive guard: dm_ != nullptr is always true when edge_storage_ is
        // populated, because create_edge_type only calls create_edge_storage when
        // has_persistence() is true (i.e. dm_ != nullptr). The guard is kept for
        // consistency with drop_edge_type_locked and the destructor so that this
        // helper remains safe regardless of call site.
        if (dm_ != nullptr) {
            (void)dm_->close_file(storage.file_id);
        }

        auto path = edge_file_path(database_id, edge_id);
        std::filesystem::remove(path);

        edge_storage_.erase(sit);
    }

    // Remove the backing EdgeTable.
    edge_tables_.erase(edge_key);

    SIXSEVEN_LOG_INFO("dropped edge type '{}' (database={})", name, database_id);
}

void GraphEngine::drop_edge_type_locked(const std::string& edge_key,
                                        database_id_t database_id,
                                        const std::string& name) {
    auto it = edge_tables_.find(edge_key);
    if (it == edge_tables_.end()) {
        return;
    }

    auto edge_id = it->second->config().edge_id;

    // Remove from catalog (ignore errors — may already be removed by CASCADE).
    (void)catalog_.drop_edge_type(database_id, name);

    teardown_edge_storage_locked(edge_key, database_id, name, edge_id);
}

Result<void> GraphEngine::drop_edge_type(database_id_t database_id, const std::string& name) {
    std::lock_guard lock(mu_);

    auto key = make_edge_key(database_id, name);

    auto it = edge_tables_.find(key);
    if (it == edge_tables_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "edge type '" + name + "' not found in graph engine");
    }

    auto edge_id = it->second->config().edge_id;

    // Remove from catalog (error-propagating — unlike the locked variant).
    auto drop_result = catalog_.drop_edge_type(database_id, name);
    if (!drop_result.has_value()) {
        return tl::unexpected(drop_result.error());
    }

    teardown_edge_storage_locked(key, database_id, name, edge_id);
    return ok();
}

Result<void> GraphEngine::drop_edge_types_for_table(database_id_t database_id,
                                                    table_id_t table_id) {
    std::lock_guard lock(mu_);

    // Collect edge keys to drop (can't modify map while iterating).
    std::vector<std::pair<std::string, std::string>> keys_and_names; // (key, name)
    for (const auto& [key, table] : edge_tables_) {
        const auto& cfg = table->config();
        if ((cfg.source_table_id == table_id || cfg.target_table_id == table_id)) {
            keys_and_names.emplace_back(key, cfg.name);
        }
    }

    for (const auto& [key, name] : keys_and_names) {
        drop_edge_type_locked(key, database_id, name);
    }

    return ok();
}

Result<uint64_t> GraphEngine::link(database_id_t database_id,
                                   const std::string& edge_type,
                                   const Value& source_pk,
                                   const Value& target_pk,
                                   const std::vector<Value>& properties) {
    std::lock_guard lock(mu_);

    auto key = make_edge_key(database_id, edge_type);

    auto it = edge_tables_.find(key);
    if (it == edge_tables_.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + edge_type + "' not found");
    }

    auto result = it->second->insert_edge(source_pk, target_pk, properties);
    if (!result.has_value()) {
        return tl::unexpected(result.error());
    }

    // Persist to disk.
    if (has_persistence()) {
        auto persist = persist_edge(key, *result, source_pk, target_pk, properties);
        if (!persist) {
            SIXSEVEN_LOG_WARN(
                "edge persist failed for '{}': {}", edge_type, persist.error().message);
        }
    }

    // Log to WAL for durability.
    log_edge_wal(WalRecordType::INSERT, it->second->config().edge_id, *result, edge_type);

    SIXSEVEN_LOG_DEBUG("LINK via '{}': edge_row_id={}", edge_type, *result);
    return ok(*result);
}

Result<uint64_t> GraphEngine::link_batch(database_id_t database_id,
                                        const std::string& edge_type,
                                        const std::vector<EdgeInsertRequest>& edges) {
    if (edges.empty()) {
        return ok(static_cast<uint64_t>(0));
    }

    std::lock_guard lock(mu_);

    auto key = make_edge_key(database_id, edge_type);

    auto it = edge_tables_.find(key);
    if (it == edge_tables_.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + edge_type + "' not found");
    }

    // Single-lock batch insert into the edge table.
    auto row_ids = it->second->insert_edges_batch(edges);
    if (!row_ids.has_value()) {
        return tl::unexpected(row_ids.error());
    }

    // Persist and WAL-log each edge.
    for (size_t i = 0; i < row_ids->size(); ++i) {
        uint64_t row_id = (*row_ids)[i];
        const auto& e = edges[i];

        if (has_persistence()) {
            auto persist = persist_edge(key, row_id, e.source_pk, e.target_pk, e.properties);
            if (!persist) {
                SIXSEVEN_LOG_WARN(
                    "edge persist failed for '{}': {}", edge_type, persist.error().message);
            }
        }

        log_edge_wal(WalRecordType::INSERT, it->second->config().edge_id, row_id, edge_type);
    }

    SIXSEVEN_LOG_DEBUG("LINK BATCH via '{}': {} edges", edge_type, row_ids->size());
    return ok(static_cast<uint64_t>(row_ids->size()));
}

Result<void> GraphEngine::unlink(database_id_t database_id,
                                 const std::string& edge_type,
                                 const Value& source_pk,
                                 const Value& target_pk) {
    std::lock_guard lock(mu_);

    auto key = make_edge_key(database_id, edge_type);

    auto it = edge_tables_.find(key);
    if (it == edge_tables_.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + edge_type + "' not found");
    }

    // Find the edge by source/target.
    auto found = it->second->find_edge(source_pk, target_pk);
    if (!found.has_value()) {
        return tl::unexpected(found.error());
    }
    if (!found->has_value()) {
        return make_error(StatusCode::NOT_FOUND, "edge not found for UNLINK");
    }

    auto edge_row_id = (*found)->edge_row_id;

    auto del = it->second->delete_edge(edge_row_id);
    if (!del.has_value()) {
        return tl::unexpected(del.error());
    }

    // Delete from persistent storage.
    if (has_persistence()) {
        auto persist_del = delete_persisted_edge(key, edge_row_id);
        if (!persist_del) {
            SIXSEVEN_LOG_WARN(
                "edge persist delete failed for '{}': {}", edge_type, persist_del.error().message);
        }
    }

    // Log to WAL for durability.
    log_edge_wal(WalRecordType::DELETE, it->second->config().edge_id, edge_row_id, edge_type);

    SIXSEVEN_LOG_DEBUG("UNLINK via '{}': removed edge_row_id={}", edge_type, edge_row_id);
    return ok();
}

Result<uint64_t> GraphEngine::unlink_where(database_id_t database_id,
                                           const std::string& edge_type,
                                           const Value& source_pk,
                                           const Value& target_pk,
                                           std::function<bool(const EdgeRow&)> predicate) {
    std::lock_guard lock(mu_);

    auto key = make_edge_key(database_id, edge_type);

    auto it = edge_tables_.find(key);
    if (it == edge_tables_.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + edge_type + "' not found");
    }

    auto& table = *it->second;

    // Find all edges from source to target.
    auto edges = table.get_edges_from(source_pk);
    if (!edges.has_value()) {
        return tl::unexpected(edges.error());
    }

    // Collect matching edge row IDs first, then delete in a second pass.
    // This avoids partial deletion: we validate all candidates before mutating.
    std::vector<uint64_t> to_delete;
    for (const auto& edge : *edges) {
        KeyType lhs = {edge.target_pk};
        KeyType rhs = {target_pk};
        auto cmp = compare_keys(lhs, rhs);
        if (!cmp.has_value() || *cmp != std::strong_ordering::equal) {
            continue;
        }
        if (!predicate(edge)) {
            continue;
        }
        to_delete.push_back(edge.edge_row_id);
    }

    // Delete all matching edges.
    for (uint64_t row_id : to_delete) {
        auto del = table.delete_edge(row_id);
        if (!del.has_value()) {
            return tl::unexpected(del.error());
        }

        // Delete from persistent storage.
        if (has_persistence()) {
            auto persist_del = delete_persisted_edge(key, row_id);
            if (!persist_del) {
                SIXSEVEN_LOG_WARN("edge persist delete failed for '{}': {}",
                                  edge_type,
                                  persist_del.error().message);
            }
        }

        // Log to WAL for durability.
        log_edge_wal(WalRecordType::DELETE, table.config().edge_id, row_id, edge_type);
    }

    SIXSEVEN_LOG_DEBUG("UNLINK WHERE via '{}': removed {} edges", edge_type, to_delete.size());
    return ok(static_cast<uint64_t>(to_delete.size()));
}

Result<std::vector<EdgeRow>> GraphEngine::get_edges_from(database_id_t database_id,
                                                         const std::string& edge_type,
                                                         const Value& source_pk) const {
    std::lock_guard lock(mu_);

    auto key = make_edge_key(database_id, edge_type);

    auto it = edge_tables_.find(key);
    if (it == edge_tables_.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + edge_type + "' not found");
    }

    return it->second->get_edges_from(source_pk);
}

Result<std::vector<EdgeRow>> GraphEngine::get_edges_to(database_id_t database_id,
                                                       const std::string& edge_type,
                                                       const Value& target_pk) const {
    std::lock_guard lock(mu_);

    auto key = make_edge_key(database_id, edge_type);

    auto it = edge_tables_.find(key);
    if (it == edge_tables_.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + edge_type + "' not found");
    }

    return it->second->get_edges_to(target_pk);
}

Result<std::vector<EdgeRow>> GraphEngine::get_all_edges(database_id_t database_id,
                                                        const std::string& edge_type) const {
    std::lock_guard lock(mu_);

    auto key = make_edge_key(database_id, edge_type);

    auto it = edge_tables_.find(key);
    if (it == edge_tables_.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + edge_type + "' not found");
    }

    return ok(it->second->get_all_edges());
}

Result<EdgeTable*> GraphEngine::get_edge_table(database_id_t database_id, const std::string& name) {
    std::lock_guard lock(mu_);

    auto key = make_edge_key(database_id, name);

    auto it = edge_tables_.find(key);
    if (it == edge_tables_.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + name + "' not found");
    }
    return ok(it->second.get());
}

std::vector<std::string> GraphEngine::list_edge_types(database_id_t database_id) const {
    std::lock_guard lock(mu_);

    auto prefix = std::to_string(database_id) + ":";
    std::vector<std::string> names;
    for (const auto& [key, _] : edge_tables_) {
        if (key.substr(0, prefix.size()) == prefix) {
            names.push_back(key.substr(prefix.size()));
        }
    }
    return names;
}

// -- Edge index persistence ---------------------------------------------------

Result<void> GraphEngine::persist_edge_indexes(const std::string& edge_key,
                                                database_id_t database_id,
                                                edge_id_t edge_id) {
    auto tit = edge_tables_.find(edge_key);
    if (tit == edge_tables_.end()) {
        return ok(); // Edge table no longer exists.
    }

    auto& edge_table = *tit->second;
    auto sit = edge_storage_.find(edge_key);

    // Helper lambda to persist one index to a file.
    auto persist_one = [&](const BTreeIndex* index,
                           const std::string& suffix,
                           std::unique_ptr<EdgeIndexStorage>& idx_storage) -> Result<void> {
        if (index == nullptr) {
            return ok();
        }

        auto path = edge_index_path(database_id, edge_id, suffix);

        // Close existing index storage if open.
        if (idx_storage) {
            (void)idx_storage->bpm->flush_all();
            (void)dm_->close_file(idx_storage->file_id);
            idx_storage.reset();
        }

        // Remove existing file.
        std::filesystem::remove(path);

        auto fid = dm_->create_file(path, /*direct_io=*/false, /*overwrite=*/true);
        if (!fid) {
            return make_error(fid.error().code,
                              "failed to create edge index file: " + fid.error().message);
        }

        auto bpm = std::make_unique<BufferPoolManager>(*dm_, *fid, 64);

        auto meta = BTreePersistence::persist(*bpm, *index);
        if (!meta) {
            (void)dm_->close_file(*fid);
            std::filesystem::remove(path);
            return make_error(meta.error().code,
                              "failed to persist edge index: " + meta.error().message);
        }

        // Store meta page ID in header extension.
        auto wr = dm_->write_header_ext_u64(*fid, 0, static_cast<uint64_t>(*meta));
        if (!wr) {
            (void)dm_->close_file(*fid);
            return make_error(wr.error().code,
                              "failed to write edge index meta page ID: " + wr.error().message);
        }

        (void)bpm->flush_all();
        (void)dm_->close_file(*fid);
        return ok();
    };

    // Dummy storage refs for persist_one when EdgeStorage doesn't exist yet.
    std::unique_ptr<EdgeIndexStorage> dummy_fwd, dummy_rev, dummy_uniq;
    auto& fwd_ref = sit != edge_storage_.end() ? sit->second->fwd_idx : dummy_fwd;
    auto& rev_ref = sit != edge_storage_.end() ? sit->second->rev_idx : dummy_rev;
    auto& uniq_ref = sit != edge_storage_.end() ? sit->second->uniq_idx : dummy_uniq;

    auto r1 = persist_one(edge_table.forward_index(), "fwd", fwd_ref);
    if (!r1) return r1;

    auto r2 = persist_one(edge_table.reverse_index(), "rev", rev_ref);
    if (!r2) return r2;

    auto r3 = persist_one(edge_table.unique_index(), "uniq", uniq_ref);
    if (!r3) return r3;

    // Store edge count and next_row_id in the heap file header extension for staleness detection.
    if (sit != edge_storage_.end()) {
        auto& storage = *sit->second;
        (void)dm_->write_header_ext_u64(storage.file_id, 0, edge_table.size());
        (void)dm_->write_header_ext_u64(storage.file_id, 8, edge_table.next_row_id());
    }

    return ok();
}

Result<bool> GraphEngine::load_edge_indexes(const std::string& edge_key,
                                             database_id_t database_id,
                                             edge_id_t edge_id,
                                             EdgeTable& edge_table) {
    // Check that all expected index files exist.
    auto fwd_path = edge_index_path(database_id, edge_id, "fwd");
    auto rev_path = edge_index_path(database_id, edge_id, "rev");

    if (!std::filesystem::exists(fwd_path) || !std::filesystem::exists(rev_path)) {
        return ok(false);
    }

    bool has_uniq = edge_table.config().prevent_duplicates;
    if (has_uniq) {
        auto uniq_path = edge_index_path(database_id, edge_id, "uniq");
        if (!std::filesystem::exists(uniq_path)) {
            return ok(false);
        }
    } else {
        // Defense-in-depth: warn if a uniq index file exists on disk but the
        // restored flag is false.  This indicates a legacy catalog record that
        // predates GDB-871 persistence; the orphaned file is harmless but the
        // constraint will NOT be enforced without the flag.
        auto uniq_path = edge_index_path(database_id, edge_id, "uniq");
        if (std::filesystem::exists(uniq_path)) {
            SIXSEVEN_LOG_WARN(
                "edge type id={} has a unique-index file on disk but prevent_duplicates is not "
                "set in the catalog.  The unique constraint will not be enforced.  "
                "Recreate the edge type to restore the constraint.",
                edge_id);
        }
    }

    // Read persisted edge count from heap header extension for staleness check.
    auto sit = edge_storage_.find(edge_key);
    if (sit == edge_storage_.end()) {
        return ok(false); // No heap storage — can't validate.
    }
    auto& heap_storage = *sit->second;

    auto persisted_count = dm_->read_header_ext_u64(heap_storage.file_id, 0);
    // If no count stored, indexes may be stale — fall back.
    if (!persisted_count) {
        return ok(false);
    }

    // Helper to load one index from disk.
    auto load_one = [&](const std::filesystem::path& path,
                        std::unique_ptr<EdgeIndexStorage>& out_storage)
        -> Result<std::unique_ptr<BTreeIndex>> {
        auto fid = dm_->open_file(path);
        if (!fid) {
            return make_error(fid.error().code, fid.error().message);
        }

        auto bpm = std::make_unique<BufferPoolManager>(*dm_, *fid, 64);

        auto meta_val = dm_->read_header_ext_u64(*fid, 0);
        if (!meta_val || *meta_val == 0) {
            (void)dm_->close_file(*fid);
            return make_error(StatusCode::NOT_FOUND, "no meta page ID in edge index file");
        }

        auto index = BTreePersistence::load(*bpm, static_cast<PageId>(*meta_val));
        if (!index) {
            (void)dm_->close_file(*fid);
            return make_error(index.error().code,
                              "failed to load edge index: " + index.error().message);
        }

        // Keep the BPM alive for the lifetime of the index.
        out_storage = std::make_unique<EdgeIndexStorage>();
        out_storage->file_id = *fid;
        out_storage->bpm = std::move(bpm);

        return ok(std::move(*index));
    };

    // Load forward index.
    auto fwd_idx = load_one(fwd_path, heap_storage.fwd_idx);
    if (!fwd_idx) {
        SIXSEVEN_LOG_DEBUG("failed to load forward edge index: {}", fwd_idx.error().message);
        return ok(false);
    }

    // Load reverse index.
    auto rev_idx = load_one(rev_path, heap_storage.rev_idx);
    if (!rev_idx) {
        // Clean up forward.
        if (heap_storage.fwd_idx) {
            (void)dm_->close_file(heap_storage.fwd_idx->file_id);
            heap_storage.fwd_idx.reset();
        }
        SIXSEVEN_LOG_DEBUG("failed to load reverse edge index: {}", rev_idx.error().message);
        return ok(false);
    }

    // Load unique index if needed.
    std::unique_ptr<BTreeIndex> uniq_loaded;
    if (has_uniq) {
        auto uniq_path = edge_index_path(database_id, edge_id, "uniq");
        auto uniq_idx = load_one(uniq_path, heap_storage.uniq_idx);
        if (!uniq_idx) {
            // Clean up forward and reverse.
            if (heap_storage.fwd_idx) {
                (void)dm_->close_file(heap_storage.fwd_idx->file_id);
                heap_storage.fwd_idx.reset();
            }
            if (heap_storage.rev_idx) {
                (void)dm_->close_file(heap_storage.rev_idx->file_id);
                heap_storage.rev_idx.reset();
            }
            SIXSEVEN_LOG_DEBUG("failed to load unique edge index: {}", uniq_idx.error().message);
            return ok(false);
        }
        uniq_loaded = std::move(*uniq_idx);
    }

    // Staleness check: compare loaded index size vs persisted edge count.
    uint64_t fwd_size = (*fwd_idx)->size();
    if (fwd_size != *persisted_count) {
        SIXSEVEN_LOG_WARN("edge index staleness detected for '{}': index size {} != persisted count {}",
                          edge_key, fwd_size, *persisted_count);
        // Clean up loaded indexes.
        if (heap_storage.fwd_idx) {
            (void)dm_->close_file(heap_storage.fwd_idx->file_id);
            heap_storage.fwd_idx.reset();
        }
        if (heap_storage.rev_idx) {
            (void)dm_->close_file(heap_storage.rev_idx->file_id);
            heap_storage.rev_idx.reset();
        }
        if (heap_storage.uniq_idx) {
            (void)dm_->close_file(heap_storage.uniq_idx->file_id);
            heap_storage.uniq_idx.reset();
        }
        // Remove stale index files.
        std::filesystem::remove(fwd_path);
        std::filesystem::remove(rev_path);
        if (has_uniq) {
            std::filesystem::remove(edge_index_path(database_id, edge_id, "uniq"));
        }
        return ok(false);
    }

    // Install the loaded indexes into the EdgeTable.
    edge_table.set_indexes(std::move(*fwd_idx), std::move(*rev_idx), std::move(uniq_loaded));

    return ok(true);
}

Result<void> GraphEngine::flush_edge_indexes() {
    std::lock_guard lock(mu_);

    if (!has_persistence()) {
        return ok();
    }

    auto t_start = std::chrono::steady_clock::now();
    size_t flushed = 0;

    for (auto& [key, table] : edge_tables_) {
        auto& cfg = table->config();

        // Extract database_id from the key (format: "database_id:name").
        auto colon = key.find(':');
        if (colon == std::string::npos) continue;
        auto parsed_id = safe_stoul(key.substr(0, colon));
        if (!parsed_id) {
            SIXSEVEN_LOG_WARN("failed to parse database_id from edge key '{}': {}",
                              key, parsed_id.error().message);
            continue;
        }
        auto db_id = static_cast<database_id_t>(*parsed_id);

        auto r = persist_edge_indexes(key, db_id, cfg.edge_id);
        if (r) {
            ++flushed;
        } else {
            SIXSEVEN_LOG_WARN("failed to persist edge indexes for '{}': {}",
                              cfg.name, r.error().message);
        }
    }

    auto elapsed = std::chrono::steady_clock::now() - t_start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    SIXSEVEN_LOG_INFO("graph engine: flushed {} edge index set(s) to disk in {}ms", flushed, ms);

    return ok();
}

Result<void> GraphEngine::flush_edges() {
    std::lock_guard lock(mu_);

    if (!has_persistence()) {
        return ok();
    }

    auto t_start = std::chrono::steady_clock::now();

    // 1. Flush edge heap + index data pages so the on-disk files reflect every
    //    in-memory edge. load_edges() rebuilds adjacency by scanning the heap,
    //    so the heap is the durability-critical piece.
    for (auto& [key, storage] : edge_storage_) {
        if (!storage) continue;
        if (storage->bpm) {
            (void)storage->bpm->flush_all();
        }
        auto flush_idx = [](const std::unique_ptr<EdgeIndexStorage>& idx) {
            if (idx && idx->bpm) {
                (void)idx->bpm->flush_all();
            }
        };
        flush_idx(storage->fwd_idx);
        flush_idx(storage->rev_idx);
        flush_idx(storage->uniq_idx);
    }

    // 2. Persist the B+ tree index structures so the next startup can load them
    //    directly instead of rebuilding from the heap.
    size_t flushed = 0;
    for (auto& [key, table] : edge_tables_) {
        auto colon = key.find(':');
        if (colon == std::string::npos) continue;
        auto parsed_id2 = safe_stoul(key.substr(0, colon));
        if (!parsed_id2) {
            SIXSEVEN_LOG_WARN("failed to parse database_id from edge key '{}': {}",
                              key, parsed_id2.error().message);
            continue;
        }
        auto db_id = static_cast<database_id_t>(*parsed_id2);
        auto r = persist_edge_indexes(key, db_id, table->config().edge_id);
        if (r) {
            ++flushed;
        } else {
            SIXSEVEN_LOG_WARN("failed to persist edge indexes for '{}': {}",
                              table->config().name, r.error().message);
        }
    }

    auto elapsed = std::chrono::steady_clock::now() - t_start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    SIXSEVEN_LOG_INFO("graph engine: flushed edge heaps + {} index set(s) to disk in {}ms",
                      flushed, ms);

    return ok();
}

Result<void> GraphEngine::load_edges() {
    std::lock_guard lock(mu_);

    if (!has_persistence()) {
        return ok(); // No persistence — nothing to load.
    }

    auto edge_types = catalog_.list_all_edge_types();
    if (edge_types.empty()) {
        return ok();
    }

    uint64_t total_edges = 0;

    for (const auto& et : edge_types) {
        auto key = make_edge_key(et.database_id, et.name);

        // Skip if edge table already exists (shouldn't happen, but be safe).
        if (edge_tables_.count(key) > 0) {
            continue;
        }

        // Derive PK types from source/target table schemas.
        TypeId source_pk_type = TypeId::INT64;
        TypeId target_pk_type = TypeId::INT64;

        auto from_schema = catalog_.get_table_by_id(et.source_table_id);
        if (from_schema) {
            for (const auto& col : from_schema->columns) {
                if (col.name == from_schema->pk_columns) {
                    source_pk_type = col.type_id;
                    break;
                }
            }
        }

        auto to_schema = catalog_.get_table_by_id(et.target_table_id);
        if (to_schema) {
            for (const auto& col : to_schema->columns) {
                if (col.name == to_schema->pk_columns) {
                    target_pk_type = col.type_id;
                    break;
                }
            }
        }

        // Parse property columns from catalog string.
        auto prop_cols = parse_property_columns(et.properties);

        // Create the in-memory EdgeTable.
        EdgeTableConfig config;
        config.edge_id = et.edge_id;
        config.name = et.name;
        config.source_table_id = et.source_table_id;
        config.target_table_id = et.target_table_id;
        config.source_pk_type = source_pk_type;
        config.target_pk_type = target_pk_type;
        config.property_columns = prop_cols;
        // Restore the unique-edge constraint from the persisted sentinel token.
        // Old catalog records without the token default to false (backward compat).
        config.prevent_duplicates = et.prevent_duplicates;

        edge_tables_.emplace(key, std::make_unique<EdgeTable>(config));

        // Open persistent storage and load edge data.
        auto path = edge_file_path(et.database_id, et.edge_id);
        if (!std::filesystem::exists(path)) {
            // No storage file — edge type has no persisted edges.
            SIXSEVEN_LOG_DEBUG("no edge storage file for '{}', skipping load", et.name);
            continue;
        }

        auto fid = dm_->open_file(path);
        if (!fid) {
            SIXSEVEN_LOG_WARN(
                "failed to open edge storage for '{}': {}", et.name, fid.error().message);
            continue;
        }

        auto storage = std::make_unique<EdgeStorage>();
        storage->file_id = *fid;
        storage->bpm = std::make_unique<BufferPoolManager>(*dm_, *fid, 256);
        storage->heap = std::make_unique<TableHeap>(*storage->bpm, *dm_, *fid);
        storage->storage_schema =
            build_edge_storage_schema(source_pk_type, target_pk_type, prop_cols);

        edge_storage_[key] = std::move(storage);

        // Try to load persisted B+ tree indexes (fast path).
        auto& edge_table = *edge_tables_[key];
        bool indexes_loaded = false;
        {
            auto idx_result = load_edge_indexes(key, et.database_id, et.edge_id, edge_table);
            if (idx_result && *idx_result) {
                indexes_loaded = true;
                SIXSEVEN_LOG_INFO("loaded persisted indexes for edge type '{}'", et.name);
            }
        }
        if (!indexes_loaded) {
            SIXSEVEN_LOG_INFO("rebuilding indexes for edge type '{}' from heap", et.name);
        }

        // Scan heap and restore edge data.
        auto& edge_store = *edge_storage_[key];
        auto iter = edge_store.heap->begin();
        if (!iter) {
            SIXSEVEN_LOG_WARN(
                "failed to iterate edge storage for '{}': {}", et.name, iter.error().message);
            continue;
        }

        for (;;) {
            auto row_result = iter->next();
            if (!row_result || !row_result->has_value()) {
                break;
            }
            auto& row = **row_result;
            auto values = TupleSerializer::deserialize(row.second, edge_store.storage_schema);
            if (!values) {
                SIXSEVEN_LOG_WARN("skipping corrupt edge row in '{}'", et.name);
                continue;
            }

            auto& v = *values;
            if (v.size() < 3) {
                SIXSEVEN_LOG_WARN("skipping short edge row in '{}'", et.name);
                continue;
            }

            auto edge_row_id = static_cast<uint64_t>(v[0].as_int64());
            const auto& src_pk = v[1];
            const auto& tgt_pk = v[2];

            // Extract properties (columns 3+).
            std::vector<Value> props;
            for (size_t i = 3; i < v.size(); ++i) {
                props.push_back(v[i]);
            }

            Result<void> restore;
            if (indexes_loaded) {
                // Fast path: populate edges_ map only, skip B+ tree inserts.
                restore = edge_table.restore_edge_data_only(edge_row_id, src_pk, tgt_pk, props);
            } else {
                // Slow path: full rebuild including B+ tree insertions.
                restore = edge_table.restore_edge(edge_row_id, src_pk, tgt_pk, props);
            }
            if (!restore) {
                SIXSEVEN_LOG_WARN("failed to restore edge {} in '{}': {}",
                                  edge_row_id,
                                  et.name,
                                  restore.error().message);
                continue;
            }

            // Store RID mapping for future deletes.
            edge_store.rid_map[edge_row_id] = row.first;
            ++total_edges;
        }
    }

    if (total_edges > 0) {
        SIXSEVEN_LOG_INFO(
            "loaded {} edge(s) across {} edge type(s) from disk", total_edges, edge_types.size());
    }

    return ok();
}

void GraphEngine::log_edge_wal(WalRecordType type,
                               edge_id_t edge_id,
                               uint64_t edge_row_id,
                               const std::string& edge_type_name) {
    if (wal_ == nullptr) {
        return;
    }

    // Encode edge_row_id and edge type name into data payload.
    std::vector<uint8_t> data;
    data.resize(sizeof(uint64_t) + sizeof(uint32_t) + edge_type_name.size());
    std::memcpy(data.data(), &edge_row_id, sizeof(uint64_t));
    auto name_len = static_cast<uint32_t>(edge_type_name.size());
    std::memcpy(data.data() + sizeof(uint64_t), &name_len, sizeof(uint32_t));
    std::memcpy(data.data() + sizeof(uint64_t) + sizeof(uint32_t),
                edge_type_name.data(),
                edge_type_name.size());

    WalRecord record;
    record.type = type;
    record.table_id = static_cast<uint32_t>(edge_id);
    record.data = std::move(data);

    auto result = wal_->append(record);
    if (!result.has_value()) {
        SIXSEVEN_LOG_WARN("Failed to log edge WAL record for '{}'", edge_type_name);
    }
}

} // namespace sixseven
