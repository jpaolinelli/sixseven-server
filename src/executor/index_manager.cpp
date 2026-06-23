#include "sixseven/executor/index_manager.h"

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/logging.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/index/btree_persistence.h"
#include "sixseven/index/hash_persistence.h"
#include "sixseven/table/tuple.h"
#include "sixseven/vector/embedding_column.h"

#include <algorithm>
#include <chrono>
#include <unordered_set>

namespace sixseven {

IndexManager::IndexManager(Catalog& catalog, StorageManager& storage)
    : catalog_(catalog), storage_(storage) {}

void IndexManager::set_catalog_persistence(CatalogPersistence* persistence) {
    persistence_ = persistence;
}

// ---------------------------------------------------------------------------
// Column name parsing / resolution
// ---------------------------------------------------------------------------

std::vector<std::string> IndexManager::parse_columns(const std::string& columns) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start < columns.size()) {
        size_t end = columns.find(',', start);
        if (end == std::string::npos) {
            end = columns.size();
        }
        auto s = start;
        while (s < end && columns[s] == ' ') {
            ++s;
        }
        auto e = end;
        while (e > s && columns[e - 1] == ' ') {
            --e;
        }
        if (s < e) {
            result.emplace_back(columns.substr(s, e - s));
        }
        start = end + 1;
    }
    return result;
}

std::vector<std::pair<size_t, TypeId>>
IndexManager::resolve_index_columns(const std::string& columns, const TableSchema& schema) const {
    auto col_names = parse_columns(columns);
    std::vector<std::pair<size_t, TypeId>> resolved;
    resolved.reserve(col_names.size());

    for (const auto& name : col_names) {
        // Case-insensitive column matching.
        std::string upper_name = name;
        std::transform(upper_name.begin(),
                       upper_name.end(),
                       upper_name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        for (size_t i = 0; i < schema.columns.size(); ++i) {
            std::string upper_col = schema.columns[i].name;
            std::transform(upper_col.begin(),
                           upper_col.end(),
                           upper_col.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (upper_col == upper_name) {
                resolved.emplace_back(i, schema.columns[i].type_id);
                break;
            }
        }
    }

    return resolved;
}

// ---------------------------------------------------------------------------
// Helper: find database_id for a table
// ---------------------------------------------------------------------------

database_id_t IndexManager::find_database_for_table(table_id_t table_id) const {
    return catalog_.get_table_database_id(table_id);
}

// ---------------------------------------------------------------------------
// Disk persistence helpers
// ---------------------------------------------------------------------------

Result<void> IndexManager::load_btree_from_disk(const IndexDef& def, database_id_t db_id) {
    auto storage = storage_.open_index_storage(db_id, def.index_id);
    if (!storage) {
        return make_error(storage.error().code, storage.error().message);
    }

    auto meta_page_id = storage_.read_index_meta_page_id(def.index_id);
    if (!meta_page_id || *meta_page_id == 0) {
        return make_error(StatusCode::NOT_FOUND, "no meta page ID stored in index file header");
    }

    auto index = BTreePersistence::load(*(*storage)->bpm, *meta_page_id);
    if (!index) {
        return make_error(index.error().code, index.error().message);
    }

    auto* ptr = index->get();
    owned_btrees_.push_back(std::move(*index));
    btree_indexes_[def.index_id] = ptr;
    return ok();
}

Result<void> IndexManager::load_hash_from_disk(const IndexDef& def, database_id_t db_id) {
    auto storage = storage_.open_index_storage(db_id, def.index_id);
    if (!storage) {
        return make_error(storage.error().code, storage.error().message);
    }

    auto meta_page_id = storage_.read_index_meta_page_id(def.index_id);
    if (!meta_page_id || *meta_page_id == 0) {
        return make_error(StatusCode::NOT_FOUND, "no meta page ID stored in index file header");
    }

    auto index = HashPersistence::load(*(*storage)->bpm, *meta_page_id);
    if (!index) {
        return make_error(index.error().code, index.error().message);
    }

    auto* ptr = index->get();
    owned_hashes_.push_back(std::move(*index));
    hash_indexes_[def.index_id] = ptr;
    return ok();
}

Result<void>
IndexManager::persist_btree(const IndexDef& def, database_id_t db_id, const BTreeIndex& index) {
    // Drop existing file if present.
    if (storage_.index_file_exists(db_id, def.index_id)) {
        auto drop = storage_.drop_index_storage(db_id, def.index_id);
        if (!drop) {
            SIXSEVEN_LOG_WARN("index manager: failed to drop old index file for '{}': {}",
                              def.name,
                              drop.error().message);
        }
    }

    auto storage = storage_.create_index_storage(db_id, def.index_id);
    if (!storage) {
        return make_error(storage.error().code,
                          "failed to create index storage for '" + def.name +
                              "': " + storage.error().message);
    }

    auto meta = BTreePersistence::persist(*(*storage)->bpm, index);
    if (!meta) {
        return make_error(meta.error().code,
                          "failed to persist btree '" + def.name + "': " + meta.error().message);
    }

    // Store meta page ID in the index file's header extension so load can find it.
    auto wr = storage_.write_index_meta_page_id(def.index_id, *meta);
    if (!wr) {
        return make_error(wr.error().code,
                          "failed to write meta page ID for '" + def.name +
                              "': " + wr.error().message);
    }

    return ok();
}

Result<void>
IndexManager::persist_hash(const IndexDef& def, database_id_t db_id, const HashIndex& index) {
    if (storage_.index_file_exists(db_id, def.index_id)) {
        auto drop = storage_.drop_index_storage(db_id, def.index_id);
        if (!drop) {
            SIXSEVEN_LOG_WARN("index manager: failed to drop old index file for '{}': {}",
                              def.name,
                              drop.error().message);
        }
    }

    auto storage = storage_.create_index_storage(db_id, def.index_id);
    if (!storage) {
        return make_error(storage.error().code,
                          "failed to create index storage for '" + def.name +
                              "': " + storage.error().message);
    }

    auto meta = HashPersistence::persist(*(*storage)->bpm, index);
    if (!meta) {
        return make_error(meta.error().code,
                          "failed to persist hash '" + def.name + "': " + meta.error().message);
    }

    auto wr = storage_.write_index_meta_page_id(def.index_id, *meta);
    if (!wr) {
        return make_error(wr.error().code,
                          "failed to write meta page ID for '" + def.name +
                              "': " + wr.error().message);
    }

    return ok();
}

// ---------------------------------------------------------------------------
// HNSW disk helpers
// ---------------------------------------------------------------------------

Result<void> IndexManager::load_hnsw_from_disk(const IndexDef& def, database_id_t db_id) {
    auto storage = storage_.open_index_storage(db_id, def.index_id);
    if (!storage) {
        return make_error(storage.error().code, storage.error().message);
    }

    auto meta_page_id = storage_.read_index_meta_page_id(def.index_id);
    if (!meta_page_id || *meta_page_id == 0) {
        return make_error(StatusCode::NOT_FOUND, "no meta page ID stored in index file header");
    }

    auto hnsw = std::make_unique<HnswIndex>(*(*storage)->bpm);
    auto load = hnsw->load(*meta_page_id);
    if (!load) {
        return make_error(load.error().code, load.error().message);
    }

    // Legacy indexes persisted before GDB-723 were built with hardcoded L2
    // graph connectivity (their meta deserializes as L2). Auto-created
    // EMBEDDING indexes must serve the default COSINE metric, so rebuild
    // them with the correct metric instead of loading the stale graph.
    if (hnsw->metric() != DistanceMetric::COSINE) {
        SIXSEVEN_LOG_INFO(
            "index manager: HNSW '{}' (id={}) persisted with metric {}, rebuilding as COSINE",
            def.name,
            def.index_id,
            distance_metric_name(hnsw->metric()));
        return rebuild_hnsw_from_table(def, db_id);
    }

    // Build the node_id → RID map by scanning the table in heap order.
    // Node IDs were assigned sequentially during the original rebuild, matching
    // the heap scan order for rows with non-null embeddings.
    std::vector<RID> rid_map;
    auto node_count = hnsw->node_count();

    auto col_names = parse_columns(def.columns);
    if (!col_names.empty()) {
        auto table_schema = catalog_.get_table_by_id(def.table_id);
        if (table_schema) {
            int32_t emb_ordinal = -1;
            for (const auto& col : table_schema->columns) {
                if (col.name == col_names[0]) {
                    emb_ordinal = col.ordinal;
                    break;
                }
            }
            if (emb_ordinal >= 0) {
                auto ts = storage_.get_table_storage(def.table_id);
                if (ts) {
                    auto storage_schema = StorageManager::build_storage_schema(*table_schema);
                    auto it = (*ts)->heap->begin();
                    if (it) {
                        rid_map.reserve(node_count);
                        while (auto row = it->next()) {
                            auto& [rid, data] = *row;
                            auto vals = TupleSerializer::deserialize(data, storage_schema);
                            if (!vals)
                                continue;
                            auto& emb_val = (*vals)[static_cast<size_t>(emb_ordinal)];
                            if (!emb_val.is_null() && emb_val.type_id() == TypeId::EMBEDDING) {
                                rid_map.push_back(rid);
                            }
                        }
                    }
                }
            }
        }
    }

    // If the RID map size doesn't match the HNSW node count, the persisted
    // index is stale (e.g. rows were added after the last REINDEX). Fall back
    // to a full rebuild so node_ids and RIDs are consistent.
    if (!rid_map.empty() && rid_map.size() != node_count) {
        SIXSEVEN_LOG_INFO(
            "index manager: HNSW '{}' (id={}) stale on disk (nodes={}, embeddings={}), rebuilding",
            def.name,
            def.index_id,
            node_count,
            rid_map.size());
        return rebuild_hnsw_from_table(def, db_id);
    }

    auto* ptr = hnsw.get();
    auto rid_count = rid_map.size();
    std::unique_lock lk(maps_latch_);
    owned_hnsw_.push_back(std::move(hnsw));
    hnsw_indexes_[def.index_id] = ptr;
    if (!rid_map.empty()) {
        hnsw_rid_maps_[def.index_id] = std::move(rid_map);
        SIXSEVEN_LOG_INFO("index manager: HNSW '{}' (id={}) loaded from disk with {} RID entries",
                          def.name,
                          def.index_id,
                          rid_count);
    }
    return ok();
}

Result<void> IndexManager::rebuild_hnsw_from_table(const IndexDef& def, database_id_t db_id) {
    // Look up the embedding column definition to get the dimension.
    auto emb_cols = catalog_.list_embedding_columns(def.table_id);
    auto col_names = parse_columns(def.columns);
    if (col_names.empty()) {
        return make_error(StatusCode::NOT_FOUND, "could not parse index columns");
    }

    auto table_schema = catalog_.get_table_by_id(def.table_id);
    if (!table_schema) {
        return make_error(table_schema.error().code, table_schema.error().message);
    }

    // Find the embedding column ordinal and dimension.
    int32_t emb_ordinal = -1;
    uint32_t dimension = 0;
    for (const auto& col : table_schema->columns) {
        if (col.name == col_names[0]) {
            emb_ordinal = col.ordinal;
            break;
        }
    }
    for (const auto& ec : emb_cols) {
        if (ec.column_id == emb_ordinal) {
            dimension = static_cast<uint32_t>(ec.dimension);
            break;
        }
    }
    if (dimension == 0 || emb_ordinal < 0) {
        return make_error(StatusCode::NOT_FOUND,
                          "could not find embedding column dimension for index '" + def.name + "'");
    }

    // Drop existing index file if present.
    if (storage_.index_file_exists(db_id, def.index_id)) {
        (void)storage_.drop_index_storage(db_id, def.index_id);
    }

    auto storage = storage_.create_index_storage(db_id, def.index_id);
    if (!storage) {
        return make_error(storage.error().code,
                          "failed to create HNSW index storage: " + storage.error().message);
    }

    auto hnsw = std::make_unique<HnswIndex>(*(*storage)->bpm);
    HnswIndexConfig config;
    config.dimension = dimension;
    // Auto-created EMBEDDING indexes serve the default NEAREST metric, which
    // is COSINE (GDB-723). Queries with a different USING metric fall back to
    // the exact brute-force scan in NearestScanOperator.
    config.metric = DistanceMetric::COSINE;
    auto create = hnsw->create(config);
    if (!create) {
        return make_error(create.error().code,
                          "failed to create HNSW index: " + create.error().message);
    }

    // Scan table data and insert all non-null embedding values.
    // Also build the node_id → RID mapping for direct tuple lookups.
    std::vector<RID> rid_map;
    auto ts = storage_.get_table_storage(def.table_id);
    if (ts) {
        auto storage_schema = StorageManager::build_storage_schema(*table_schema);
        auto it = (*ts)->heap->begin();
        if (it) {
            uint32_t inserted = 0;
            while (auto row = it->next()) {
                auto& [rid, data] = *row;
                auto vals = TupleSerializer::deserialize(data, storage_schema);
                if (!vals)
                    continue;

                auto& emb_val = (*vals)[static_cast<size_t>(emb_ordinal)];
                if (!emb_val.is_null() && emb_val.type_id() == TypeId::EMBEDDING) {
                    const auto& vec = emb_val.as_embedding();
                    (void)hnsw->insert(std::span<const float>(vec));
                    rid_map.push_back(rid);
                    ++inserted;
                }
            }
            SIXSEVEN_LOG_INFO(
                "index manager: rebuilt HNSW '{}' with {} vectors", def.name, inserted);
        }
    }

    // Flush metadata and store meta page ID.
    auto flush = hnsw->flush_meta();
    if (!flush) {
        SIXSEVEN_LOG_WARN("index manager: failed to flush HNSW meta for '{}': {}",
                          def.name,
                          flush.error().message);
    }
    auto wr = storage_.write_index_meta_page_id(def.index_id, hnsw->meta_page_id());
    if (!wr) {
        SIXSEVEN_LOG_WARN("index manager: failed to write meta page ID for '{}': {}",
                          def.name,
                          wr.error().message);
    }

    auto* ptr = hnsw.get();
    auto rid_count = rid_map.size();
    std::unique_lock lk(maps_latch_);
    owned_hnsw_.push_back(std::move(hnsw));
    hnsw_indexes_[def.index_id] = ptr;
    hnsw_rid_maps_[def.index_id] = std::move(rid_map);
    SIXSEVEN_LOG_INFO("index manager: HNSW '{}' (id={}) rid_map has {} entries",
                      def.name,
                      def.index_id,
                      rid_count);
    return ok();
}

Result<void>
IndexManager::flush_hnsw(const IndexDef& def, database_id_t /*db_id*/, HnswIndex& index) {
    auto flush = index.flush_meta();
    if (!flush) {
        return make_error(flush.error().code,
                          "failed to flush HNSW meta for '" + def.name +
                              "': " + flush.error().message);
    }

    // Store the meta page ID so load can find it after restart.
    auto wr = storage_.write_index_meta_page_id(def.index_id, index.meta_page_id());
    if (!wr) {
        SIXSEVEN_LOG_WARN("index manager: failed to write meta page ID for HNSW '{}': {}",
                          def.name,
                          wr.error().message);
    }

    // Ensure the BPM writes dirty pages to disk.
    auto storage = storage_.get_index_storage(def.index_id);
    if (storage) {
        auto bpm_flush = (*storage)->bpm->flush_all();
        if (!bpm_flush) {
            return make_error(bpm_flush.error().code,
                              "failed to flush BPM for HNSW '" + def.name +
                                  "': " + bpm_flush.error().message);
        }
    }

    return ok();
}

// ---------------------------------------------------------------------------
// BM25 disk helpers
// ---------------------------------------------------------------------------

Result<void>
IndexManager::persist_bm25(const IndexDef& def, database_id_t db_id, const Bm25Index& index) {
    if (storage_.index_file_exists(db_id, def.index_id)) {
        auto drop = storage_.drop_index_storage(db_id, def.index_id);
        if (!drop) {
            SIXSEVEN_LOG_WARN("index manager: failed to drop old index file for '{}': {}",
                              def.name,
                              drop.error().message);
        }
    }

    auto storage = storage_.create_index_storage(db_id, def.index_id);
    if (!storage) {
        return make_error(storage.error().code,
                          "failed to create index storage for '" + def.name +
                              "': " + storage.error().message);
    }

    auto meta = Bm25Index::persist(*(*storage)->bpm, index);
    if (!meta) {
        return make_error(meta.error().code,
                          "failed to persist bm25 '" + def.name + "': " + meta.error().message);
    }

    auto wr = storage_.write_index_meta_page_id(def.index_id, *meta);
    if (!wr) {
        return make_error(wr.error().code,
                          "failed to write meta page ID for '" + def.name +
                              "': " + wr.error().message);
    }

    return ok();
}

Result<void> IndexManager::load_bm25_from_disk(const IndexDef& def, database_id_t db_id) {
    auto storage = storage_.open_index_storage(db_id, def.index_id);
    if (!storage) {
        return make_error(storage.error().code, storage.error().message);
    }

    auto meta_page_id = storage_.read_index_meta_page_id(def.index_id);
    if (!meta_page_id || *meta_page_id == 0) {
        return make_error(StatusCode::NOT_FOUND, "no meta page ID stored in index file header");
    }

    auto idx = Bm25Index::load(*(*storage)->bpm, *meta_page_id);
    if (!idx) {
        return make_error(idx.error().code, idx.error().message);
    }

    auto* ptr = idx->get();
    std::unique_lock lk(maps_latch_);
    owned_bm25_.push_back(std::move(*idx));
    bm25_indexes_[def.index_id] = ptr;
    return ok();
}

Result<void> IndexManager::rebuild_bm25_from_table(const IndexDef& def, database_id_t db_id) {
    auto col_names = parse_columns(def.columns);
    if (col_names.empty()) {
        return make_error(StatusCode::NOT_FOUND, "could not parse index columns");
    }

    auto table_schema = catalog_.get_table_by_id(def.table_id);
    if (!table_schema) {
        return make_error(table_schema.error().code, table_schema.error().message);
    }

    int32_t text_ordinal = -1;
    for (const auto& col : table_schema->columns) {
        if (col.name == col_names[0]) {
            text_ordinal = col.ordinal;
            break;
        }
    }
    if (text_ordinal < 0) {
        return make_error(StatusCode::NOT_FOUND,
                          "BM25 column not found for index '" + def.name + "'");
    }

    auto idx = std::make_unique<Bm25Index>();
    idx->create(Bm25Config{});

    // Scan the table heap and index each row's text value.
    auto ts = storage_.get_table_storage(def.table_id);
    if (ts) {
        auto storage_schema = StorageManager::build_storage_schema(*table_schema);
        auto it = (*ts)->heap->begin();
        if (it) {
            while (auto row = it->next()) {
                auto vals = TupleSerializer::deserialize(row->second, storage_schema);
                if (!vals) {
                    continue;
                }
                auto& v = (*vals)[static_cast<size_t>(text_ordinal)];
                if (v.is_null()) {
                    continue;
                }
                auto s = v.try_as_string();
                if (!s) {
                    continue;
                }
                (void)idx->add_document(row->first, **s);
            }
        }
    }

    auto* ptr = idx.get();
    auto p = persist_bm25(def, db_id, *idx);
    if (!p) {
        SIXSEVEN_LOG_WARN(
            "index manager: failed to persist bm25 '{}' to disk: {}", def.name, p.error().message);
    }

    std::unique_lock lk(maps_latch_);
    owned_bm25_.push_back(std::move(idx));
    bm25_indexes_[def.index_id] = ptr;
    return ok();
}

// ---------------------------------------------------------------------------
// rebuild_all_indexes — startup index rebuild + autoincrement init
// ---------------------------------------------------------------------------

Result<void> IndexManager::rebuild_all_indexes() {
    auto t_start = std::chrono::steady_clock::now();

    auto all_indexes = catalog_.list_all_indexes();

    // Separate HNSW and BM25 indexes (handled independently) from BTree/Hash.
    std::vector<IndexDef> hnsw_indexes;
    std::vector<IndexDef> bm25_indexes;
    std::unordered_map<table_id_t, std::vector<IndexDef>> indexes_by_table;
    for (auto& idx : all_indexes) {
        if (idx.index_type == "hnsw") {
            hnsw_indexes.push_back(std::move(idx));
        } else if (idx.index_type == "bm25") {
            bm25_indexes.push_back(std::move(idx));
        } else {
            indexes_by_table[idx.table_id].push_back(std::move(idx));
        }
    }

    // Load HNSW indexes: from disk (fast) or rebuild from table data (slow).
    size_t loaded_from_disk = 0;
    for (auto& idx : hnsw_indexes) {
        auto db_id = find_database_for_table(idx.table_id);
        if (storage_.index_file_exists(db_id, idx.index_id)) {
            auto r = load_hnsw_from_disk(idx, db_id);
            if (r) {
                ++loaded_from_disk;
                SIXSEVEN_LOG_INFO("index manager: loaded hnsw '{}' from disk", idx.name);
                continue;
            }
            SIXSEVEN_LOG_WARN(
                "index manager: failed to load HNSW '{}' from disk ({}), will rebuild",
                idx.name,
                r.error().message);
        }
        auto r = rebuild_hnsw_from_table(idx, db_id);
        if (!r) {
            SIXSEVEN_LOG_WARN(
                "index manager: failed to rebuild HNSW '{}': {}", idx.name, r.error().message);
        }
    }

    // Load BM25 indexes: from disk (fast) or rebuild from table data (slow).
    for (auto& idx : bm25_indexes) {
        auto db_id = find_database_for_table(idx.table_id);
        if (storage_.index_file_exists(db_id, idx.index_id)) {
            auto r = load_bm25_from_disk(idx, db_id);
            if (r) {
                ++loaded_from_disk;
                SIXSEVEN_LOG_INFO("index manager: loaded bm25 '{}' from disk", idx.name);
                continue;
            }
            SIXSEVEN_LOG_WARN(
                "index manager: failed to load BM25 '{}' from disk ({}), will rebuild",
                idx.name,
                r.error().message);
        }
        auto r = rebuild_bm25_from_table(idx, db_id);
        if (!r) {
            SIXSEVEN_LOG_WARN(
                "index manager: failed to rebuild BM25 '{}': {}", idx.name, r.error().message);
        }
    }

    // Try to load each BTree/Hash index from disk (fast path).
    // Track which indexes we successfully loaded and which need rebuild.
    std::unordered_map<table_id_t, std::vector<IndexDef>> need_rebuild;

    for (auto& [table_id, idxs] : indexes_by_table) {
        auto db_id = find_database_for_table(table_id);
        for (auto& idx : idxs) {
            if (!storage_.index_file_exists(db_id, idx.index_id)) {
                need_rebuild[table_id].push_back(idx);
                continue;
            }

            Result<void> load_result;
            if (idx.index_type == "btree") {
                load_result = load_btree_from_disk(idx, db_id);
            } else if (idx.index_type == "hash") {
                load_result = load_hash_from_disk(idx, db_id);
            } else {
                need_rebuild[table_id].push_back(idx);
                continue;
            }

            if (load_result) {
                ++loaded_from_disk;
                SIXSEVEN_LOG_INFO(
                    "index manager: loaded {} '{}' from disk", idx.index_type, idx.name);
            } else {
                SIXSEVEN_LOG_WARN("index manager: failed to load '{}' from disk ({}), will rebuild",
                                  idx.name,
                                  load_result.error().message);
                need_rebuild[table_id].push_back(idx);
            }
        }
    }

    // Find all tables that need scanning: those with indexes needing rebuild OR autoincrement.
    struct TableWork {
        table_id_t table_id = 0;
        database_id_t database_id = 0;
        TableSchema schema;
        std::vector<IndexDef> indexes;
        int autoincrement_ordinal = -1; // -1 = no autoincrement
        TypeId autoincrement_type = TypeId::INT32;
        bool need_autoincrement_scan = false;
    };

    std::unordered_map<table_id_t, TableWork> work_items;

    // Add tables that have indexes needing rebuild.
    for (auto& [table_id, idxs] : need_rebuild) {
        auto schema = catalog_.get_table_by_id(table_id);
        if (!schema) {
            SIXSEVEN_LOG_WARN("index manager: table_id {} not found, skipping indexes", table_id);
            continue;
        }
        auto& w = work_items[table_id];
        w.table_id = table_id;
        w.database_id = find_database_for_table(table_id);
        w.schema = *schema;
        w.indexes = std::move(idxs);
    }

    // Find tables with autoincrement columns across all databases.
    auto databases = catalog_.list_databases();
    for (const auto& db : databases) {
        auto tables = catalog_.list_tables(db.database_id);
        for (const auto& tbl : tables) {
            for (const auto& col : tbl.columns) {
                if (col.is_autoincrement) {
                    // Try to read persisted autoincrement counter from table file header.
                    auto persisted = storage_.read_autoincrement(tbl.table_id);
                    if (persisted && *persisted > 0) {
                        // Fast path: use persisted counter.
                        catalog_.init_autoincrement(tbl.table_id, *persisted);
                        SIXSEVEN_LOG_DEBUG(
                            "index manager: autoincrement for table '{}' loaded from disk: {}",
                            tbl.name,
                            *persisted);
                    } else {
                        // Slow path: need to scan table to find max value.
                        auto& w = work_items[tbl.table_id];
                        w.table_id = tbl.table_id;
                        w.database_id = db.database_id;
                        w.schema = tbl;
                        w.autoincrement_ordinal = col.ordinal;
                        w.autoincrement_type = col.type_id;
                        w.need_autoincrement_scan = true;
                    }
                    break; // Only one autoincrement column per table.
                }
            }
        }
    }

    if (work_items.empty()) {
        auto elapsed = std::chrono::steady_clock::now() - t_start;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        SIXSEVEN_LOG_INFO(
            "index manager: {} indexes loaded from disk, 0 rebuilt, {}ms", loaded_from_disk, ms);
        return ok();
    }

    size_t total_index_entries = 0;
    size_t total_indexes_rebuilt = 0;

    for (auto& [table_id, work] : work_items) {
        auto ts = storage_.get_table_storage(table_id);
        if (!ts) {
            SIXSEVEN_LOG_WARN("index manager: no storage for table '{}' (id={}), skipping",
                              work.schema.name,
                              table_id);
            continue;
        }

        auto storage_schema = StorageManager::build_storage_schema(work.schema);

        // Create empty index objects for this table.
        struct IndexEntry {
            IndexDef def;
            std::vector<std::pair<size_t, TypeId>> col_info; // (ordinal, type)
            BTreeIndex* btree = nullptr;
            HashIndex* hash = nullptr;
        };
        std::vector<IndexEntry> entries;

        for (const auto& idx_def : work.indexes) {
            IndexEntry entry;
            entry.def = idx_def;
            entry.col_info = resolve_index_columns(idx_def.columns, work.schema);
            if (entry.col_info.empty()) {
                SIXSEVEN_LOG_WARN("index manager: could not resolve columns for index '{}'",
                                  idx_def.name);
                continue;
            }

            std::vector<TypeId> key_types;
            key_types.reserve(entry.col_info.size());
            for (const auto& [ordinal, type_id] : entry.col_info) {
                key_types.push_back(type_id);
            }

            if (idx_def.index_type == "btree") {
                BTreeConfig config;
                config.key_types = std::move(key_types);
                config.is_unique = idx_def.is_unique;
                auto btree = std::make_unique<BTreeIndex>(std::move(config));
                entry.btree = btree.get();
                owned_btrees_.push_back(std::move(btree));
            } else if (idx_def.index_type == "hash") {
                HashIndexConfig config;
                config.key_types = std::move(key_types);
                config.is_unique = idx_def.is_unique;
                auto hash = std::make_unique<HashIndex>(std::move(config));
                entry.hash = hash.get();
                owned_hashes_.push_back(std::move(hash));
            } else {
                SIXSEVEN_LOG_WARN("index manager: unknown index type '{}' for index '{}'",
                                  idx_def.index_type,
                                  idx_def.name);
                continue;
            }

            entries.push_back(std::move(entry));
        }

        // Single-pass scan of the table heap.
        auto it = (*ts)->heap->begin();
        if (!it) {
            SIXSEVEN_LOG_WARN("index manager: failed to iterate table '{}': {}",
                              work.schema.name,
                              it.error().message);
            continue;
        }

        int64_t autoincrement_max = 0;

        while (auto row = it->next()) {
            auto vals = TupleSerializer::deserialize(row->second, storage_schema);
            if (!vals) {
                continue; // Skip corrupt rows.
            }

            // Insert into each index.
            for (auto& entry : entries) {
                KeyType key;
                key.reserve(entry.col_info.size());
                for (const auto& [ordinal, type_id] : entry.col_info) {
                    key.push_back((*vals)[ordinal]);
                }

                RID rid = row->first;

                if (entry.btree != nullptr) {
                    auto ins = entry.btree->insert(key, rid);
                    if (!ins) {
                        SIXSEVEN_LOG_WARN("index manager: failed to insert into btree '{}': {}",
                                          entry.def.name,
                                          ins.error().message);
                    }
                } else if (entry.hash != nullptr) {
                    auto ins = entry.hash->insert(key, rid);
                    if (!ins) {
                        SIXSEVEN_LOG_WARN("index manager: failed to insert into hash '{}': {}",
                                          entry.def.name,
                                          ins.error().message);
                    }
                }
            }

            // Track autoincrement max value.
            if (work.need_autoincrement_scan && work.autoincrement_ordinal >= 0) {
                auto& v = (*vals)[static_cast<size_t>(work.autoincrement_ordinal)];
                if (!v.is_null()) {
                    int64_t row_val = 0;
                    switch (work.autoincrement_type) {
                    case TypeId::INT8:
                        row_val = v.as_int8();
                        break;
                    case TypeId::INT16:
                        row_val = v.as_int16();
                        break;
                    case TypeId::INT32:
                        row_val = v.as_int32();
                        break;
                    case TypeId::INT64:
                        row_val = v.as_int64();
                        break;
                    case TypeId::UINT8:
                        row_val = v.as_uint8();
                        break;
                    case TypeId::UINT16:
                        row_val = v.as_uint16();
                        break;
                    case TypeId::UINT32:
                        row_val = v.as_uint32();
                        break;
                    case TypeId::UINT64:
                        row_val = static_cast<int64_t>(v.as_uint64());
                        break;
                    default:
                        break;
                    }
                    if (row_val > autoincrement_max) {
                        autoincrement_max = row_val;
                    }
                }
            }
        }

        // Initialize autoincrement counter.
        if (work.need_autoincrement_scan && work.autoincrement_ordinal >= 0) {
            int64_t next_val = autoincrement_max + 1;
            catalog_.init_autoincrement(table_id, next_val);
            // Persist to table file header for next startup.
            auto wr = storage_.write_autoincrement(table_id, next_val);
            if (!wr) {
                SIXSEVEN_LOG_WARN("index manager: failed to persist autoincrement for '{}': {}",
                                  work.schema.name,
                                  wr.error().message);
            }
            SIXSEVEN_LOG_DEBUG("index manager: autoincrement for table '{}' starts at {}",
                               work.schema.name,
                               next_val);
        }

        // Register indexes in pointer maps and persist to disk.
        for (auto& entry : entries) {
            if (entry.btree != nullptr) {
                btree_indexes_[entry.def.index_id] = entry.btree;
                ++total_indexes_rebuilt;
                total_index_entries += entry.btree->size();
                SIXSEVEN_LOG_INFO("index manager: rebuilt btree '{}' on table '{}' ({} entries)",
                                  entry.def.name,
                                  work.schema.name,
                                  entry.btree->size());

                // Persist to disk for fast loading next startup.
                auto p = persist_btree(entry.def, work.database_id, *entry.btree);
                if (!p) {
                    SIXSEVEN_LOG_WARN("index manager: failed to persist btree '{}': {}",
                                      entry.def.name,
                                      p.error().message);
                }
            } else if (entry.hash != nullptr) {
                hash_indexes_[entry.def.index_id] = entry.hash;
                ++total_indexes_rebuilt;
                total_index_entries += entry.hash->size();
                SIXSEVEN_LOG_INFO("index manager: rebuilt hash '{}' on table '{}' ({} entries)",
                                  entry.def.name,
                                  work.schema.name,
                                  entry.hash->size());

                auto p = persist_hash(entry.def, work.database_id, *entry.hash);
                if (!p) {
                    SIXSEVEN_LOG_WARN("index manager: failed to persist hash '{}': {}",
                                      entry.def.name,
                                      p.error().message);
                }
            }
        }
    }

    auto elapsed = std::chrono::steady_clock::now() - t_start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    SIXSEVEN_LOG_INFO("index manager: {} loaded from disk, {} rebuilt ({} entries) in {}ms",
                      loaded_from_disk,
                      total_indexes_rebuilt,
                      total_index_entries,
                      ms);

    return ok();
}

// ---------------------------------------------------------------------------
// start_async_load — launch background index loading
// ---------------------------------------------------------------------------

Result<void> IndexManager::start_async_load() {
    auto all_indexes = catalog_.list_all_indexes();

    // Collect work items.
    struct AsyncWork {
        IndexDef def;
        database_id_t db_id = 0;
        bool has_disk_file = false;
    };
    std::vector<AsyncWork> work;
    std::vector<AsyncWork> hnsw_work;
    std::vector<AsyncWork> bm25_work;

    for (auto& idx : all_indexes) {
        auto db_id = find_database_for_table(idx.table_id);
        bool has_file = storage_.index_file_exists(db_id, idx.index_id);
        if (idx.index_type == "hnsw") {
            hnsw_work.push_back({std::move(idx), db_id, has_file});
        } else if (idx.index_type == "bm25") {
            bm25_work.push_back({std::move(idx), db_id, has_file});
        } else {
            work.push_back({std::move(idx), db_id, has_file});
        }
    }

    // Initialize autoincrement counters (must happen before queries).
    auto databases = catalog_.list_databases();
    for (const auto& db : databases) {
        auto tables = catalog_.list_tables(db.database_id);
        for (const auto& tbl : tables) {
            for (const auto& col : tbl.columns) {
                if (col.is_autoincrement) {
                    auto persisted = storage_.read_autoincrement(tbl.table_id);
                    if (persisted && *persisted > 0) {
                        catalog_.init_autoincrement(tbl.table_id, *persisted);
                    } else {
                        // Fall back to synchronous scan for autoincrement.
                        // This is rare (only first startup).
                        auto ts = storage_.get_table_storage(tbl.table_id);
                        if (ts) {
                            auto schema = StorageManager::build_storage_schema(tbl);
                            auto it = (*ts)->heap->begin();
                            if (it) {
                                int64_t max_val = 0;
                                while (auto row = it->next()) {
                                    auto vals = TupleSerializer::deserialize(row->second, schema);
                                    if (!vals)
                                        continue;
                                    auto& v = (*vals)[static_cast<size_t>(col.ordinal)];
                                    if (!v.is_null()) {
                                        int64_t rv = 0;
                                        switch (col.type_id) {
                                        case TypeId::INT32:
                                            rv = v.as_int32();
                                            break;
                                        case TypeId::INT64:
                                            rv = v.as_int64();
                                            break;
                                        default:
                                            break;
                                        }
                                        if (rv > max_val)
                                            max_val = rv;
                                    }
                                }
                                catalog_.init_autoincrement(tbl.table_id, max_val + 1);
                                (void)storage_.write_autoincrement(tbl.table_id, max_val + 1);
                            }
                        }
                    }
                    break;
                }
            }
        }
    }

    if (work.empty() && hnsw_work.empty() && bm25_work.empty()) {
        async_load_done_.store(true);
        SIXSEVEN_LOG_INFO("index manager: no indexes to load");
        return ok();
    }

    // Separate into fast-path (disk) and slow-path (rebuild).
    std::vector<AsyncWork> disk_work;
    std::vector<AsyncWork> rebuild_work;
    for (auto& w : work) {
        if (w.has_disk_file) {
            disk_work.push_back(std::move(w));
        } else {
            rebuild_work.push_back(std::move(w));
        }
    }

    size_t total_work = disk_work.size() + rebuild_work.size() + (hnsw_work.empty() ? 0 : 1) +
                        (bm25_work.empty() ? 0 : 1);
    load_latch_ = std::make_unique<std::latch>(static_cast<ptrdiff_t>(total_work));

    // Load from disk in a background thread.
    if (!disk_work.empty()) {
        load_threads_.emplace_back([this, items = std::move(disk_work)]() {
            for (const auto& item : items) {
                Result<void> r;
                if (item.def.index_type == "btree") {
                    r = load_btree_from_disk(item.def, item.db_id);
                } else if (item.def.index_type == "hash") {
                    r = load_hash_from_disk(item.def, item.db_id);
                }
                if (r) {
                    SIXSEVEN_LOG_INFO("index manager [async]: loaded {} '{}' from disk",
                                      item.def.index_type,
                                      item.def.name);
                } else {
                    SIXSEVEN_LOG_WARN("index manager [async]: failed to load '{}': {}",
                                      item.def.name,
                                      r.error().message);
                }
                load_latch_->count_down();
            }
        });
    }

    // Rebuild indexes in another background thread.
    if (!rebuild_work.empty()) {
        load_threads_.emplace_back([this, items = std::move(rebuild_work)]() {
            for (const auto& item : items) {
                auto schema = catalog_.get_table_by_id(item.def.table_id);
                if (!schema) {
                    SIXSEVEN_LOG_WARN("index manager [async]: table not found for '{}'",
                                      item.def.name);
                    load_latch_->count_down();
                    continue;
                }

                // Create and populate index from table data.
                auto col_info = resolve_index_columns(item.def.columns, *schema);
                if (col_info.empty()) {
                    load_latch_->count_down();
                    continue;
                }

                std::vector<TypeId> key_types;
                for (const auto& [ordinal, type_id] : col_info) {
                    key_types.push_back(type_id);
                }

                BTreeIndex* btree_ptr = nullptr;
                HashIndex* hash_ptr = nullptr;

                if (item.def.index_type == "btree") {
                    BTreeConfig cfg;
                    cfg.key_types = std::move(key_types);
                    cfg.is_unique = item.def.is_unique;
                    auto btree = std::make_unique<BTreeIndex>(std::move(cfg));
                    btree_ptr = btree.get();
                    std::unique_lock lk(maps_latch_);
                    owned_btrees_.push_back(std::move(btree));
                } else if (item.def.index_type == "hash") {
                    HashIndexConfig cfg;
                    cfg.key_types = std::move(key_types);
                    cfg.is_unique = item.def.is_unique;
                    auto hash = std::make_unique<HashIndex>(std::move(cfg));
                    hash_ptr = hash.get();
                    std::unique_lock lk(maps_latch_);
                    owned_hashes_.push_back(std::move(hash));
                }

                // Scan table.
                auto ts = storage_.get_table_storage(item.def.table_id);
                if (ts) {
                    auto storage_schema = StorageManager::build_storage_schema(*schema);
                    auto it = (*ts)->heap->begin();
                    if (it) {
                        while (auto row = it->next()) {
                            auto vals = TupleSerializer::deserialize(row->second, storage_schema);
                            if (!vals)
                                continue;
                            KeyType key;
                            for (const auto& [ordinal, type_id] : col_info) {
                                key.push_back((*vals)[ordinal]);
                            }
                            if (btree_ptr)
                                (void)btree_ptr->insert(key, row->first);
                            if (hash_ptr)
                                (void)hash_ptr->insert(key, row->first);
                        }
                    }
                }

                // Register in maps.
                {
                    std::unique_lock lk(maps_latch_);
                    if (btree_ptr)
                        btree_indexes_[item.def.index_id] = btree_ptr;
                    if (hash_ptr)
                        hash_indexes_[item.def.index_id] = hash_ptr;
                }

                // Persist to disk.
                if (btree_ptr) {
                    (void)persist_btree(item.def, item.db_id, *btree_ptr);
                }
                if (hash_ptr) {
                    (void)persist_hash(item.def, item.db_id, *hash_ptr);
                }

                SIXSEVEN_LOG_INFO(
                    "index manager [async]: rebuilt {} '{}'", item.def.index_type, item.def.name);
                load_latch_->count_down();
            }
        });
    }

    // Load HNSW indexes in a background thread.
    if (!hnsw_work.empty()) {
        load_threads_.emplace_back([this, items = std::move(hnsw_work)]() {
            for (const auto& item : items) {
                Result<void> r;
                if (item.has_disk_file) {
                    r = load_hnsw_from_disk(item.def, item.db_id);
                    if (r) {
                        SIXSEVEN_LOG_INFO("index manager [async]: loaded hnsw '{}' from disk",
                                          item.def.name);
                        continue;
                    }
                    SIXSEVEN_LOG_WARN("index manager [async]: failed to load HNSW '{}' from disk "
                                      "({}), will rebuild",
                                      item.def.name,
                                      r.error().message);
                }
                r = rebuild_hnsw_from_table(item.def, item.db_id);
                if (r) {
                    SIXSEVEN_LOG_INFO("index manager [async]: rebuilt hnsw '{}'", item.def.name);
                } else {
                    SIXSEVEN_LOG_WARN("index manager [async]: failed to rebuild HNSW '{}': {}",
                                      item.def.name,
                                      r.error().message);
                }
            }
            load_latch_->count_down();
        });
    }

    // Load BM25 indexes in a background thread.
    if (!bm25_work.empty()) {
        load_threads_.emplace_back([this, items = std::move(bm25_work)]() {
            for (const auto& item : items) {
                Result<void> r;
                if (item.has_disk_file) {
                    r = load_bm25_from_disk(item.def, item.db_id);
                    if (r) {
                        SIXSEVEN_LOG_INFO("index manager [async]: loaded bm25 '{}' from disk",
                                          item.def.name);
                        continue;
                    }
                    SIXSEVEN_LOG_WARN("index manager [async]: failed to load BM25 '{}' from disk "
                                      "({}), will rebuild",
                                      item.def.name,
                                      r.error().message);
                }
                r = rebuild_bm25_from_table(item.def, item.db_id);
                if (r) {
                    SIXSEVEN_LOG_INFO("index manager [async]: rebuilt bm25 '{}'", item.def.name);
                } else {
                    SIXSEVEN_LOG_WARN("index manager [async]: failed to rebuild BM25 '{}': {}",
                                      item.def.name,
                                      r.error().message);
                }
            }
            load_latch_->count_down();
        });
    }

    return ok();
}

void IndexManager::wait_for_load_complete() {
    if (load_latch_) {
        load_latch_->wait();
    }
    for (auto& t : load_threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    load_threads_.clear();
    async_load_done_.store(true);
}

// ---------------------------------------------------------------------------
// flush_all_indexes — shutdown persistence
// ---------------------------------------------------------------------------

Result<void> IndexManager::flush_all_indexes() {
    auto t_start = std::chrono::steady_clock::now();
    size_t flushed = 0;

    auto all_indexes = catalog_.list_all_indexes();
    for (const auto& idx : all_indexes) {
        auto db_id = find_database_for_table(idx.table_id);

        if (idx.index_type == "btree") {
            auto it = btree_indexes_.find(idx.index_id);
            if (it != btree_indexes_.end()) {
                auto r = persist_btree(idx, db_id, *it->second);
                if (r)
                    ++flushed;
                else
                    SIXSEVEN_LOG_WARN(
                        "index manager: flush btree '{}' failed: {}", idx.name, r.error().message);
            }
        } else if (idx.index_type == "hash") {
            auto it = hash_indexes_.find(idx.index_id);
            if (it != hash_indexes_.end()) {
                auto r = persist_hash(idx, db_id, *it->second);
                if (r)
                    ++flushed;
                else
                    SIXSEVEN_LOG_WARN(
                        "index manager: flush hash '{}' failed: {}", idx.name, r.error().message);
            }
        } else if (idx.index_type == "hnsw") {
            auto it = hnsw_indexes_.find(idx.index_id);
            if (it != hnsw_indexes_.end()) {
                auto r = flush_hnsw(idx, db_id, *it->second);
                if (r)
                    ++flushed;
                else
                    SIXSEVEN_LOG_WARN(
                        "index manager: flush hnsw '{}' failed: {}", idx.name, r.error().message);
            }
        } else if (idx.index_type == "bm25") {
            auto it = bm25_indexes_.find(idx.index_id);
            if (it != bm25_indexes_.end()) {
                auto r = persist_bm25(idx, db_id, *it->second);
                if (r)
                    ++flushed;
                else
                    SIXSEVEN_LOG_WARN(
                        "index manager: flush bm25 '{}' failed: {}", idx.name, r.error().message);
            }
        }
    }

    // Persist autoincrement counters.
    auto databases = catalog_.list_databases();
    for (const auto& db : databases) {
        auto tables = catalog_.list_tables(db.database_id);
        for (const auto& tbl : tables) {
            for (const auto& col : tbl.columns) {
                if (col.is_autoincrement) {
                    auto counter = catalog_.get_autoincrement_counter(tbl.table_id);
                    if (counter > 0) {
                        (void)storage_.write_autoincrement(tbl.table_id, counter);
                    }
                    break;
                }
            }
        }
    }

    auto elapsed = std::chrono::steady_clock::now() - t_start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    SIXSEVEN_LOG_INFO("index manager: flushed {} indexes to disk in {}ms", flushed, ms);

    return ok();
}

// ---------------------------------------------------------------------------
// create_and_populate_index — runtime CREATE INDEX
// ---------------------------------------------------------------------------

Result<void> IndexManager::create_and_populate_index(const IndexDef& def,
                                                     const TableSchema& schema) {
    // HNSW indexes use a separate path: they manage their own pages in a
    // dedicated BPM and don't need key extraction like BTree/Hash.
    if (def.index_type == "hnsw") {
        auto db_id = find_database_for_table(def.table_id);
        return rebuild_hnsw_from_table(def, db_id);
    }

    // BM25 manages its own in-memory inverted index and persistence.
    if (def.index_type == "bm25") {
        auto db_id = find_database_for_table(def.table_id);
        return rebuild_bm25_from_table(def, db_id);
    }

    auto col_info = resolve_index_columns(def.columns, schema);
    if (col_info.empty()) {
        return make_error(StatusCode::NOT_FOUND, "could not resolve index columns");
    }

    std::vector<TypeId> key_types;
    key_types.reserve(col_info.size());
    for (const auto& [ordinal, type_id] : col_info) {
        key_types.push_back(type_id);
    }

    BTreeIndex* btree_ptr = nullptr;
    HashIndex* hash_ptr = nullptr;

    if (def.index_type == "btree") {
        BTreeConfig config;
        config.key_types = std::move(key_types);
        config.is_unique = def.is_unique;
        auto btree = std::make_unique<BTreeIndex>(std::move(config));
        btree_ptr = btree.get();
        owned_btrees_.push_back(std::move(btree));
    } else if (def.index_type == "hash") {
        HashIndexConfig config;
        config.key_types = std::move(key_types);
        config.is_unique = def.is_unique;
        auto hash = std::make_unique<HashIndex>(std::move(config));
        hash_ptr = hash.get();
        owned_hashes_.push_back(std::move(hash));
    } else {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "unsupported index type '" + def.index_type + "'");
    }

    // Scan existing table data and populate the index.
    auto ts = storage_.get_table_storage(schema.table_id);
    if (!ts) {
        return make_error(ts.error().code, ts.error().message);
    }

    auto storage_schema = StorageManager::build_storage_schema(schema);
    auto it = (*ts)->heap->begin();
    if (!it) {
        return make_error(it.error().code, it.error().message);
    }

    while (auto row = it->next()) {
        auto vals = TupleSerializer::deserialize(row->second, storage_schema);
        if (!vals) {
            continue;
        }

        KeyType key;
        key.reserve(col_info.size());
        for (const auto& [ordinal, type_id] : col_info) {
            key.push_back((*vals)[ordinal]);
        }

        RID rid = row->first;

        if (btree_ptr != nullptr) {
            auto ins = btree_ptr->insert(key, rid);
            if (!ins) {
                return make_error(ins.error().code,
                                  "failed to populate index: " + ins.error().message);
            }
        } else if (hash_ptr != nullptr) {
            auto ins = hash_ptr->insert(key, rid);
            if (!ins) {
                return make_error(ins.error().code,
                                  "failed to populate index: " + ins.error().message);
            }
        }
    }

    // Register in pointer maps.
    if (btree_ptr != nullptr) {
        btree_indexes_[def.index_id] = btree_ptr;
        // Persist to disk.
        auto db_id = find_database_for_table(def.table_id);
        auto p = persist_btree(def, db_id, *btree_ptr);
        if (!p) {
            SIXSEVEN_LOG_WARN("index manager: failed to persist btree '{}' to disk: {}",
                              def.name,
                              p.error().message);
        }
    } else if (hash_ptr != nullptr) {
        hash_indexes_[def.index_id] = hash_ptr;
        auto db_id = find_database_for_table(def.table_id);
        auto p = persist_hash(def, db_id, *hash_ptr);
        if (!p) {
            SIXSEVEN_LOG_WARN("index manager: failed to persist hash '{}' to disk: {}",
                              def.name,
                              p.error().message);
        }
    }

    return ok();
}

// ---------------------------------------------------------------------------
// drop_index
// ---------------------------------------------------------------------------

void IndexManager::drop_index(index_id_t id, table_id_t table_id) {
    btree_indexes_.erase(id);
    hash_indexes_.erase(id);

    // For HNSW and BM25, erase by index_id (under the maps latch).
    if (!hnsw_indexes_.empty() || !bm25_indexes_.empty()) {
        std::unique_lock lk(maps_latch_);
        hnsw_indexes_.erase(id);
        hnsw_rid_maps_.erase(id);
        bm25_indexes_.erase(id);
    }

    // Remove the index file from disk.
    if (table_id > 0) {
        auto db_id = find_database_for_table(table_id);
        if (db_id > 0) {
            auto drop = storage_.drop_index_storage(db_id, id);
            if (!drop) {
                SIXSEVEN_LOG_WARN("index manager: failed to drop index file for id {}: {}",
                                  id,
                                  drop.error().message);
            }
        }
    }
    // Owned unique_ptrs remain in vectors until IndexManager is destroyed.
}

// ---------------------------------------------------------------------------
// reindex — manual rebuild of indexes
// ---------------------------------------------------------------------------

Result<void> IndexManager::reindex(const std::string& name, database_id_t db_id) {
    // Wait for any in-progress async loading to finish before rebuilding.
    wait_for_load_complete();

    // Try to find by index name first, scoped to the caller's database.
    auto idx = catalog_.get_index(db_id, name);
    if (idx) {
        auto idx_db_id = db_id;
        SIXSEVEN_LOG_INFO("reindex '{}': found index (type={}, table_id={}, db_id={})",
                          name,
                          idx->index_type,
                          idx->table_id,
                          idx_db_id);
        if (idx->index_type == "hnsw") {
            {
                std::unique_lock lk(maps_latch_);
                hnsw_indexes_.erase(idx->index_id);
                hnsw_rid_maps_.erase(idx->index_id);
            }
            return rebuild_hnsw_from_table(*idx, idx_db_id);
        }
        if (idx->index_type == "bm25") {
            {
                std::unique_lock lk(maps_latch_);
                bm25_indexes_.erase(idx->index_id);
            }
            if (storage_.index_file_exists(idx_db_id, idx->index_id)) {
                (void)storage_.drop_index_storage(idx_db_id, idx->index_id);
            }
            return rebuild_bm25_from_table(*idx, idx_db_id);
        }

        auto schema = catalog_.get_table_by_id(idx->table_id);
        if (!schema) {
            return make_error(schema.error().code, schema.error().message);
        }

        // Drop in-memory index and rebuild.
        btree_indexes_.erase(idx->index_id);
        hash_indexes_.erase(idx->index_id);
        if (storage_.index_file_exists(idx_db_id, idx->index_id)) {
            (void)storage_.drop_index_storage(idx_db_id, idx->index_id);
        }

        // Reuse the normal create-and-populate path for btree/hash.
        return create_and_populate_index(*idx, *schema);
    }

    // Not an index name — try as a table name.
    auto table = catalog_.get_table(db_id, name);
    if (!table) {
        return make_error(StatusCode::NOT_FOUND, "no index or table named '" + name + "'");
    }

    auto indexes = catalog_.list_indexes(table->table_id);
    SIXSEVEN_LOG_INFO(
        "reindex '{}': found {} indexes on table_id={}", name, indexes.size(), table->table_id);
    for (const auto& index_def : indexes) {
        SIXSEVEN_LOG_INFO("reindex '{}': rebuilding index '{}' (type={}, table_id={})",
                          name,
                          index_def.name,
                          index_def.index_type,
                          index_def.table_id);

        // Rebuild directly using the IndexDef from list_indexes.
        Result<void> r;
        if (index_def.index_type == "hnsw") {
            {
                std::unique_lock lk(maps_latch_);
                hnsw_indexes_.erase(index_def.index_id);
                hnsw_rid_maps_.erase(index_def.index_id);
            }
            r = rebuild_hnsw_from_table(index_def, db_id);
        } else if (index_def.index_type == "bm25") {
            {
                std::unique_lock lk(maps_latch_);
                bm25_indexes_.erase(index_def.index_id);
            }
            if (storage_.index_file_exists(db_id, index_def.index_id)) {
                (void)storage_.drop_index_storage(db_id, index_def.index_id);
            }
            r = rebuild_bm25_from_table(index_def, db_id);
        } else {
            btree_indexes_.erase(index_def.index_id);
            hash_indexes_.erase(index_def.index_id);
            if (storage_.index_file_exists(db_id, index_def.index_id)) {
                (void)storage_.drop_index_storage(db_id, index_def.index_id);
            }
            auto schema = catalog_.get_table_by_id(index_def.table_id);
            if (!schema) {
                return make_error(schema.error().code, schema.error().message);
            }
            r = create_and_populate_index(index_def, *schema);
        }
        if (!r) {
            SIXSEVEN_LOG_WARN(
                "reindex '{}': failed on '{}': {}", name, index_def.name, r.error().message);
            return r;
        }
    }

    return ok();
}

// ---------------------------------------------------------------------------
// Accessor maps
// ---------------------------------------------------------------------------

std::unordered_map<index_id_t, BTreeIndex*>* IndexManager::btree_map() {
    return &btree_indexes_;
}

std::unordered_map<index_id_t, HashIndex*>* IndexManager::hash_map() {
    return &hash_indexes_;
}

std::unordered_map<index_id_t, HnswIndex*>* IndexManager::hnsw_map() {
    return &hnsw_indexes_;
}

std::unordered_map<index_id_t, Bm25Index*>* IndexManager::bm25_map() {
    return &bm25_indexes_;
}

std::unordered_map<index_id_t, std::vector<RID>>* IndexManager::hnsw_rid_maps() {
    return &hnsw_rid_maps_;
}

void IndexManager::append_hnsw_rid(index_id_t index_id, RID rid) {
    std::unique_lock lk(maps_latch_);
    hnsw_rid_maps_[index_id].push_back(rid);
}

} // namespace sixseven
