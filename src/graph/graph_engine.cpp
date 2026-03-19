#include "sixseven/graph/graph_engine.h"

#include "sixseven/common/logging.h"
#include "sixseven/common/types.h"
#include "sixseven/storage/wal.h"

#include <cstring>

namespace sixseven {

GraphEngine::GraphEngine(Catalog& catalog, WalWriter* wal) : catalog_(catalog), wal_(wal) {}

GraphEngine::GraphEngine(Catalog& catalog,
                         DiskManager& dm,
                         std::filesystem::path data_dir,
                         WalWriter* wal)
    : catalog_(catalog), dm_(&dm), data_dir_(std::move(data_dir)), wal_(wal) {}

GraphEngine::~GraphEngine() {
    // Flush and close all edge storage files.
    for (auto& [key, storage] : edge_storage_) {
        if (storage && storage->bpm) {
            (void)storage->bpm->flush_all();
        }
        if (storage && dm_ != nullptr) {
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

Result<void> GraphEngine::open_edge_storage(database_id_t database_id,
                                            edge_id_t edge_id,
                                            TypeId source_pk_type,
                                            TypeId target_pk_type,
                                            const std::vector<ColumnDef>& property_columns) {
    auto path = edge_file_path(database_id, edge_id);
    if (!std::filesystem::exists(path)) {
        // No storage file yet — edge type was created but no edges persisted.
        // Create a new storage file.
        return create_edge_storage(
            database_id, edge_id, source_pk_type, target_pk_type, property_columns);
    }

    auto fid = dm_->open_file(path);
    if (!fid) {
        return make_error(fid.error().code, fid.error().message);
    }

    auto storage = std::make_unique<EdgeStorage>();
    storage->file_id = *fid;
    storage->bpm = std::make_unique<BufferPoolManager>(*dm_, *fid, 256);
    storage->heap = std::make_unique<TableHeap>(*storage->bpm, *dm_, *fid);
    storage->storage_schema =
        build_edge_storage_schema(source_pk_type, target_pk_type, property_columns);

    // Find the edge type name.
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

    auto flush = storage.bpm->flush_all();
    if (!flush) {
        SIXSEVEN_LOG_WARN("edge flush failed for '{}': {}", edge_key, flush.error().message);
    }

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

    auto flush = storage.bpm->flush_all();
    if (!flush) {
        SIXSEVEN_LOG_WARN("edge flush failed for '{}': {}", edge_key, flush.error().message);
    }

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
    std::string props_str;
    for (size_t i = 0; i < property_columns.size(); ++i) {
        if (i > 0) {
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

    // Remove persistent storage if present.
    auto sit = edge_storage_.find(edge_key);
    if (sit != edge_storage_.end()) {
        auto& storage = *sit->second;
        (void)storage.bpm->flush_all();
        if (dm_ != nullptr) {
            (void)dm_->close_file(storage.file_id);
        }

        auto path = edge_file_path(database_id, edge_id);
        std::filesystem::remove(path);

        edge_storage_.erase(sit);
    }

    // Remove the backing EdgeTable.
    edge_tables_.erase(it);

    SIXSEVEN_LOG_INFO("dropped edge type '{}' (database={})", name, database_id);
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

    // Remove from catalog.
    auto drop_result = catalog_.drop_edge_type(database_id, name);
    if (!drop_result.has_value()) {
        return tl::unexpected(drop_result.error());
    }

    // Remove persistent storage if present.
    auto sit = edge_storage_.find(key);
    if (sit != edge_storage_.end()) {
        auto& storage = *sit->second;
        (void)storage.bpm->flush_all();
        (void)dm_->close_file(storage.file_id);

        auto path = edge_file_path(database_id, edge_id);
        std::filesystem::remove(path);

        edge_storage_.erase(sit);
    }

    // Remove the backing EdgeTable.
    edge_tables_.erase(it);

    SIXSEVEN_LOG_INFO("dropped edge type '{}' (database={})", name, database_id);
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
        config.prevent_duplicates = false; // Not persisted; default to false.

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

        // Scan heap and restore edges.
        auto& edge_table = *edge_tables_[key];
        auto iter = storage->heap->begin();
        if (!iter) {
            SIXSEVEN_LOG_WARN(
                "failed to iterate edge storage for '{}': {}", et.name, iter.error().message);
            edge_storage_[key] = std::move(storage);
            continue;
        }

        while (auto row = iter->next()) {
            auto values = TupleSerializer::deserialize(row->second, storage->storage_schema);
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

            auto restore = edge_table.restore_edge(edge_row_id, src_pk, tgt_pk, props);
            if (!restore) {
                SIXSEVEN_LOG_WARN("failed to restore edge {} in '{}': {}",
                                  edge_row_id,
                                  et.name,
                                  restore.error().message);
                continue;
            }

            // Store RID mapping for future deletes.
            storage->rid_map[edge_row_id] = row->first;
            ++total_edges;
        }

        edge_storage_[key] = std::move(storage);
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
