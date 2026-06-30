#include "sixseven/executor/catalog_persistence.h"

#include "sixseven/common/logging.h"
#include "sixseven/common/value.h"
#include "sixseven/table/tuple.h"

#include <algorithm>
#include <set>
#include <span>
#include <unordered_map>

namespace sixseven {

CatalogPersistence::CatalogPersistence(Catalog& catalog, StorageManager& storage)
    : catalog_(catalog), storage_(storage) {}

// ---------------------------------------------------------------------------
// System table creation (first run)
// ---------------------------------------------------------------------------

Result<void> CatalogPersistence::create_system_catalog_tables() {
    auto r1 = create_sys_table(sys_tables_schema());
    if (!r1) {
        return r1;
    }
    auto r2 = create_sys_table(sys_columns_schema());
    if (!r2) {
        return r2;
    }
    auto r3 = create_sys_table(sys_indexes_schema());
    if (!r3) {
        return r3;
    }
    auto r4 = create_sys_table(sys_edge_types_schema());
    if (!r4) {
        return r4;
    }
    auto r5 = create_sys_table(sys_embedding_columns_schema());
    if (!r5) {
        return r5;
    }
    auto r6 = create_sys_table(sys_databases_schema());
    if (!r6) {
        return r6;
    }

    SIXSEVEN_LOG_INFO("catalog persistence: created system catalog tables");
    return ok();
}

Result<void> CatalogPersistence::create_sys_table(const TableSchema& schema) {
    auto restore = catalog_.restore_table(system_database_id, schema);
    if (!restore) {
        return make_error(restore.error().code,
                          "failed to register " + schema.name + ": " + restore.error().message);
    }

    auto storage = storage_.create_table_storage(system_database_id, schema.table_id, schema);
    if (!storage) {
        return make_error(storage.error().code,
                          "failed to create storage for " + schema.name + ": " +
                              storage.error().message);
    }

    return ok();
}

Result<void> CatalogPersistence::create_sys_table_public(const TableSchema& schema) {
    return create_sys_table(schema);
}

Result<void> CatalogPersistence::open_sys_table_public(const TableSchema& schema) {
    return open_sys_table(schema);
}

// ---------------------------------------------------------------------------
// Catalog loading (subsequent runs)
// ---------------------------------------------------------------------------

Result<void> CatalogPersistence::open_sys_table(const TableSchema& schema) {
    // Check the backing file exists BEFORE touching the catalog.  If the file
    // is absent (e.g. sys_databases missing on an old deployment), we must
    // return an error WITHOUT registering the table so that the migration
    // branch can call create_sys_table_public and succeed.  drop_table is
    // blocked for system tables, so the only safe way to keep the catalog
    // clean is to guard the registration behind the file-existence check.
    if (!storage_.table_file_exists(system_database_id, schema.table_id)) {
        return make_error(StatusCode::IO_ERROR,
                          "storage file missing for system table '" + schema.name + "'");
    }

    // Idempotency: if the table was already registered (e.g. by a prior call
    // inside migrate_databases_from_sys_tables), skip restore_table to avoid
    // the "already exists" error.
    auto existing = catalog_.get_table(system_database_id, schema.name);
    if (!existing) {
        auto restore = catalog_.restore_table(system_database_id, schema);
        if (!restore) {
            return make_error(restore.error().code,
                              "failed to register " + schema.name + ": " + restore.error().message);
        }
    }

    // Similarly, if storage is already open, skip re-opening it.
    auto already_open = storage_.get_table_storage(schema.table_id);
    if (already_open) {
        return ok();
    }

    auto storage = storage_.open_table_storage(system_database_id, schema.table_id, schema);
    if (!storage) {
        return make_error(storage.error().code,
                          "failed to open storage for " + schema.name + ": " +
                              storage.error().message);
    }

    return ok();
}

Result<void> CatalogPersistence::load_catalog() {
    // 1. Register and open system catalog table schemas.
    auto r1 = open_sys_table(sys_tables_schema());
    if (!r1) {
        return r1;
    }
    auto r2 = open_sys_table(sys_columns_schema());
    if (!r2) {
        return r2;
    }
    auto r3 = open_sys_table(sys_indexes_schema());
    if (!r3) {
        return r3;
    }
    auto r4 = open_sys_table(sys_edge_types_schema());
    if (!r4) {
        return r4;
    }
    auto r5 = open_sys_table(sys_embedding_columns_schema());
    if (!r5) {
        return r5;
    }
    // Note: sys_databases is opened by SystemBootstrap before load_catalog().

    // 2. Read sys_databases: restore all databases before loading tables.
    {
        auto ts = storage_.get_table_storage(sys_databases_table_id);
        if (!ts) {
            return make_error(ts.error().code, ts.error().message);
        }
        auto storage_schema = StorageManager::build_storage_schema(sys_databases_schema());
        auto it = (*ts)->heap->begin();
        if (!it) {
            return make_error(it.error().code, it.error().message);
        }
        for (;;) {
            auto row_result = it->next();
            if (!row_result) {
                return make_error(row_result.error().code, row_result.error().message);
            }
            if (!row_result->has_value()) {
                break;
            }
            auto& row = **row_result;
            auto values = TupleSerializer::deserialize(row.second, storage_schema);
            if (!values) {
                SIXSEVEN_LOG_WARN("catalog persistence: skipping corrupt sys_databases row");
                continue;
            }
            auto& v = *values;
            auto db_id = static_cast<database_id_t>(v[0].as_int32());
            auto db_name = v[1].as_string();

            // Skip system database (already exists from Catalog constructor).
            if (db_id == system_database_id) {
                continue;
            }

            // Skip if already restored (e.g., by test fixtures).
            auto existing = catalog_.get_database(db_name);
            if (existing) {
                continue;
            }

            auto r = catalog_.restore_database(db_id, db_name);
            if (!r) {
                SIXSEVEN_LOG_WARN("catalog persistence: failed to restore database '{}': {}",
                                  db_name,
                                  r.error().message);
            }
        }
    }

    // 3. Read sys_tables: collect table metadata (without columns yet).
    struct TableInfo {
        table_id_t table_id = 0;
        database_id_t database_id = 0;
        std::string name;
        std::string pk_columns;
        std::vector<CatalogColumnDef> columns;
    };
    std::unordered_map<table_id_t, TableInfo> tables;
    {
        auto ts = storage_.get_table_storage(sys_tables_table_id);
        if (!ts) {
            return make_error(ts.error().code, ts.error().message);
        }
        auto storage_schema = StorageManager::build_storage_schema(sys_tables_schema());
        auto it = (*ts)->heap->begin();
        if (!it) {
            return make_error(it.error().code, it.error().message);
        }
        for (;;) {
            auto row_result = it->next();
            if (!row_result) {
                return make_error(row_result.error().code, row_result.error().message);
            }
            if (!row_result->has_value()) {
                break;
            }
            auto& row = **row_result;
            auto values = TupleSerializer::deserialize(row.second, storage_schema);
            if (!values) {
                SIXSEVEN_LOG_WARN("catalog persistence: skipping corrupt sys_tables row");
                continue;
            }
            auto& v = *values;
            TableInfo info;
            info.table_id = v[0].as_int32();
            info.database_id = v[1].as_int32();
            info.name = v[2].as_string();
            info.pk_columns = v[3].is_null() ? "" : v[3].as_string();
            tables[info.table_id] = std::move(info);
        }
    }

    // 4. Read sys_columns: add columns to the collected tables.
    {
        auto ts = storage_.get_table_storage(sys_columns_table_id);
        if (!ts) {
            return make_error(ts.error().code, ts.error().message);
        }
        auto storage_schema = StorageManager::build_storage_schema(sys_columns_schema());
        auto it = (*ts)->heap->begin();
        if (!it) {
            return make_error(it.error().code, it.error().message);
        }
        for (;;) {
            auto row_result = it->next();
            if (!row_result) {
                return make_error(row_result.error().code, row_result.error().message);
            }
            if (!row_result->has_value()) {
                break;
            }
            auto& row = **row_result;
            auto values = TupleSerializer::deserialize(row.second, storage_schema);
            if (!values) {
                SIXSEVEN_LOG_WARN("catalog persistence: skipping corrupt sys_columns row");
                continue;
            }
            auto& v = *values;
            auto table_id = v[0].as_int32();
            auto table_it = tables.find(table_id);
            if (table_it == tables.end()) {
                continue; // Orphan column — table not in sys_tables.
            }
            CatalogColumnDef col;
            col.ordinal = v[1].as_int32();
            col.name = v[2].as_string();
            col.type_id = static_cast<TypeId>(v[3].as_int32());
            col.nullable = v[4].as_bool();
            col.default_expr = v[5].is_null() ? "" : v[5].as_string();
            col.is_autoincrement = v.size() > 6 && !v[6].is_null() && v[6].as_bool();
            col.precision = (v.size() > 7 && !v[7].is_null()) ? v[7].as_int32() : 0;
            col.scale = (v.size() > 8 && !v[8].is_null()) ? v[8].as_int32() : 0;
            table_it->second.columns.push_back(std::move(col));
        }
    }

    // 5. Restore tables into the Catalog and open their storage files.
    for (auto& [tid, info] : tables) {
        // Sort columns by ordinal.
        std::sort(info.columns.begin(),
                  info.columns.end(),
                  [](const CatalogColumnDef& a, const CatalogColumnDef& b) {
                      return a.ordinal < b.ordinal;
                  });

        // Ensure the database storage directory exists.
        auto dir_result = storage_.create_database_storage(info.database_id);
        if (!dir_result) {
            SIXSEVEN_LOG_WARN("catalog persistence: failed to create db storage for db {}",
                              info.database_id);
        }

        TableSchema schema;
        schema.table_id = info.table_id;
        schema.name = info.name;
        schema.pk_columns = info.pk_columns;
        schema.columns = std::move(info.columns);

        auto restore = catalog_.restore_table(info.database_id, schema);
        if (!restore) {
            SIXSEVEN_LOG_WARN("catalog persistence: failed to restore table '{}': {}",
                              info.name,
                              restore.error().message);
            continue;
        }

        // Open the table's storage file.
        auto open = storage_.open_table_storage(info.database_id, info.table_id, schema);
        if (!open) {
            SIXSEVEN_LOG_WARN("catalog persistence: failed to open storage for table '{}': {}",
                              info.name,
                              open.error().message);
            continue;
        }

        // NOTE: Autoincrement counter initialization is handled by
        // IndexManager::rebuild_all_indexes(), which combines it with index
        // rebuild in a single table scan to avoid duplicate full-table scans.
    }

    // 6. Read sys_indexes: restore index metadata.
    {
        auto ts = storage_.get_table_storage(sys_indexes_table_id);
        if (!ts) {
            return make_error(ts.error().code, ts.error().message);
        }
        auto storage_schema = StorageManager::build_storage_schema(sys_indexes_schema());
        auto it = (*ts)->heap->begin();
        if (!it) {
            return make_error(it.error().code, it.error().message);
        }
        for (;;) {
            auto row_result = it->next();
            if (!row_result) {
                return make_error(row_result.error().code, row_result.error().message);
            }
            if (!row_result->has_value()) {
                break;
            }
            auto& row = **row_result;
            auto values = TupleSerializer::deserialize(row.second, storage_schema);
            if (!values) {
                continue;
            }
            auto& v = *values;
            IndexDef def;
            def.index_id = v[0].as_int32();
            def.table_id = v[1].as_int32();
            def.name = v[2].as_string();
            def.index_type = v[3].as_string();
            def.columns = v[4].as_string();
            def.is_unique = v[5].as_bool();

            auto r = catalog_.restore_index(std::move(def));
            if (!r) {
                SIXSEVEN_LOG_WARN("catalog persistence: failed to restore index: {}",
                                  r.error().message);
            }
        }
    }

    // 7. Read sys_edge_types: restore edge type metadata.
    {
        auto ts = storage_.get_table_storage(sys_edge_types_table_id);
        if (!ts) {
            return make_error(ts.error().code, ts.error().message);
        }
        auto storage_schema = StorageManager::build_storage_schema(sys_edge_types_schema());
        auto it = (*ts)->heap->begin();
        if (!it) {
            return make_error(it.error().code, it.error().message);
        }
        for (;;) {
            auto row_result = it->next();
            if (!row_result) {
                return make_error(row_result.error().code, row_result.error().message);
            }
            if (!row_result->has_value()) {
                break;
            }
            auto& row = **row_result;
            auto values = TupleSerializer::deserialize(row.second, storage_schema);
            if (!values) {
                continue;
            }
            auto& v = *values;
            EdgeTypeDef def;
            def.edge_id = v[0].as_int32();
            auto db_id = static_cast<database_id_t>(v[1].as_int32());
            def.name = v[2].as_string();
            def.source_table_id = v[3].as_int32();
            def.target_table_id = v[4].as_int32();
            def.properties = v[5].is_null() ? "" : v[5].as_string();
            // The "__uniq__" sentinel token in properties encodes prevent_duplicates.
            // Old catalog records without the token default to false (backward compat).
            // Use a whole-token match (split by ',') so that a real property column
            // whose name contains "__uniq__" as a substring (e.g. "__uniq__:STRING")
            // does NOT falsely set prevent_duplicates=true.  The encode side always
            // writes the bare sentinel with no colon suffix, so the only exact match
            // is the token "__uniq__" itself.
            {
                bool found = false;
                const std::string& p = def.properties;
                std::string::size_type start = 0;
                while (start <= p.size()) {
                    auto end = p.find(',', start);
                    if (end == std::string::npos) {
                        end = p.size();
                    }
                    if (p.compare(start, end - start, "__uniq__") == 0) {
                        found = true;
                        break;
                    }
                    start = end + 1;
                }
                def.prevent_duplicates = found;
            }

            auto r = catalog_.restore_edge_type(db_id, std::move(def));
            if (!r) {
                SIXSEVEN_LOG_WARN("catalog persistence: failed to restore edge type: {}",
                                  r.error().message);
            }
        }
    }

    // 8. Read sys_embedding_columns: restore embedding column metadata.
    {
        auto ts = storage_.get_table_storage(sys_embedding_columns_table_id);
        if (!ts) {
            return make_error(ts.error().code, ts.error().message);
        }
        auto storage_schema = StorageManager::build_storage_schema(sys_embedding_columns_schema());
        auto it = (*ts)->heap->begin();
        if (!it) {
            return make_error(it.error().code, it.error().message);
        }
        for (;;) {
            auto row_result = it->next();
            if (!row_result) {
                return make_error(row_result.error().code, row_result.error().message);
            }
            if (!row_result->has_value()) {
                break;
            }
            auto& row = **row_result;
            auto values = TupleSerializer::deserialize(row.second, storage_schema);
            if (!values) {
                continue;
            }
            auto& v = *values;
            EmbeddingColumnDef def;
            def.table_id = v[0].as_int32();
            def.column_id = v[1].as_int32();
            def.dimension = v[2].as_int32();
            def.source_expr = v[3].is_null() ? "" : v[3].as_string();
            def.provider = v[4].as_string();

            catalog_.restore_embedding_column(std::move(def));
        }
    }

    auto table_count = tables.size();
    SIXSEVEN_LOG_INFO("catalog persistence: loaded {} user tables from disk", table_count);
    return ok();
}

// ---------------------------------------------------------------------------
// Row insertion helper
// ---------------------------------------------------------------------------

Result<void> CatalogPersistence::insert_row(table_id_t sys_table_id,
                                            const std::vector<Value>& values) {
    auto ts = storage_.get_table_storage(sys_table_id);
    if (!ts) {
        return make_error(ts.error().code, ts.error().message);
    }

    auto table_schema = catalog_.get_table_by_id(sys_table_id);
    if (!table_schema) {
        return make_error(table_schema.error().code, table_schema.error().message);
    }

    auto storage_schema = StorageManager::build_storage_schema(*table_schema);
    auto data = TupleSerializer::serialize(values, storage_schema);
    if (!data) {
        return make_error(data.error().code, data.error().message);
    }

    auto rid = (*ts)->heap->insert_tuple(*data);
    if (!rid) {
        return make_error(rid.error().code, rid.error().message);
    }

    // Flush to ensure durability.
    auto flush = (*ts)->bpm->flush_all();
    if (!flush) {
        return make_error(flush.error().code, flush.error().message);
    }

    return ok();
}

// ---------------------------------------------------------------------------
// Row deletion helper
// ---------------------------------------------------------------------------

template <typename Pred>
Result<void> CatalogPersistence::delete_rows(table_id_t sys_table_id, Pred predicate) {
    auto ts = storage_.get_table_storage(sys_table_id);
    if (!ts) {
        return make_error(ts.error().code, ts.error().message);
    }

    auto table_schema = catalog_.get_table_by_id(sys_table_id);
    if (!table_schema) {
        return make_error(table_schema.error().code, table_schema.error().message);
    }

    auto storage_schema = StorageManager::build_storage_schema(*table_schema);

    // Scan and collect RIDs to delete.
    std::vector<RID> to_delete;
    auto it = (*ts)->heap->begin();
    if (!it) {
        return make_error(it.error().code, it.error().message);
    }
    for (;;) {
        auto row_result = it->next();
        if (!row_result) {
            return make_error(row_result.error().code, row_result.error().message);
        }
        if (!row_result->has_value()) {
            break;
        }
        auto& row = **row_result;
        auto values = TupleSerializer::deserialize(row.second, storage_schema);
        if (!values) {
            continue; // Skip corrupt rows.
        }
        if (predicate(*values)) {
            to_delete.push_back(row.first);
        }
    }

    // Delete collected rows.
    for (auto rid : to_delete) {
        auto del = (*ts)->heap->delete_tuple(rid);
        if (!del) {
            return make_error(del.error().code, del.error().message);
        }
    }

    // Flush to ensure durability.
    auto flush = (*ts)->bpm->flush_all();
    if (!flush) {
        return make_error(flush.error().code, flush.error().message);
    }

    return ok();
}

// ---------------------------------------------------------------------------
// Persist operations (after DDL)
// ---------------------------------------------------------------------------

Result<void> CatalogPersistence::persist_database(database_id_t db_id, const std::string& name) {
    return insert_row(sys_databases_table_id, {Value(db_id), Value(name)});
}

Result<void> CatalogPersistence::remove_database(database_id_t db_id) {
    return delete_rows(sys_databases_table_id,
                       [db_id](const std::vector<Value>& v) { return v[0].as_int32() == db_id; });
}

Result<void> CatalogPersistence::persist_table(database_id_t db_id, const TableSchema& schema) {
    // Insert into sys_tables.
    auto r1 = insert_row(sys_tables_table_id,
                         {Value(schema.table_id),
                          Value(db_id),
                          Value(schema.name),
                          schema.pk_columns.empty() ? Value() : Value(schema.pk_columns)});
    if (!r1) {
        return r1;
    }

    // Insert columns into sys_columns.
    for (const auto& col : schema.columns) {
        auto r = insert_row(sys_columns_table_id,
                            {Value(schema.table_id),
                             Value(col.ordinal),
                             Value(col.name),
                             Value(static_cast<int32_t>(col.type_id)),
                             Value(col.nullable),
                             col.default_expr.empty() ? Value() : Value(col.default_expr),
                             Value(col.is_autoincrement),
                             Value(col.precision),
                             Value(col.scale)});
        if (!r) {
            return r;
        }
    }

    return ok();
}

Result<void> CatalogPersistence::remove_table(table_id_t table_id) {
    // Delete from sys_tables.
    auto r1 = delete_rows(sys_tables_table_id, [table_id](const std::vector<Value>& v) {
        return v[0].as_int32() == table_id;
    });
    if (!r1) {
        return r1;
    }

    // Delete from sys_columns.
    auto r2 = delete_rows(sys_columns_table_id, [table_id](const std::vector<Value>& v) {
        return v[0].as_int32() == table_id;
    });
    if (!r2) {
        return r2;
    }

    // Also remove any indexes for this table.
    auto r3 = delete_rows(sys_indexes_table_id, [table_id](const std::vector<Value>& v) {
        return v[1].as_int32() == table_id;
    });
    if (!r3) {
        return r3;
    }

    // Remove edge types referencing this table.
    auto r4 = delete_rows(sys_edge_types_table_id, [table_id](const std::vector<Value>& v) {
        return v[3].as_int32() == table_id || v[4].as_int32() == table_id;
    });
    if (!r4) {
        return r4;
    }

    // Remove embedding columns for this table.
    auto r5 = delete_rows(sys_embedding_columns_table_id, [table_id](const std::vector<Value>& v) {
        return v[0].as_int32() == table_id;
    });
    if (!r5) {
        return r5;
    }

    return ok();
}

Result<void> CatalogPersistence::persist_index(const IndexDef& def) {
    return insert_row(sys_indexes_table_id,
                      {Value(def.index_id),
                       Value(def.table_id),
                       Value(def.name),
                       Value(def.index_type),
                       Value(def.columns),
                       Value(def.is_unique)});
}

Result<void> CatalogPersistence::remove_index(index_id_t index_id) {
    return delete_rows(sys_indexes_table_id, [index_id](const std::vector<Value>& v) {
        return v[0].as_int32() == index_id;
    });
}

Result<void> CatalogPersistence::persist_edge_type(const EdgeTypeDef& def) {
    return insert_row(sys_edge_types_table_id,
                      {Value(def.edge_id),
                       Value(def.database_id),
                       Value(def.name),
                       Value(def.source_table_id),
                       Value(def.target_table_id),
                       def.properties.empty() ? Value() : Value(def.properties)});
}

Result<void> CatalogPersistence::remove_edge_type(edge_id_t edge_id) {
    return delete_rows(sys_edge_types_table_id, [edge_id](const std::vector<Value>& v) {
        return v[0].as_int32() == edge_id;
    });
}

Result<void> CatalogPersistence::persist_embedding_column(const EmbeddingColumnDef& def) {
    return insert_row(sys_embedding_columns_table_id,
                      {Value(def.table_id),
                       Value(def.column_id),
                       Value(def.dimension),
                       def.source_expr.empty() ? Value() : Value(def.source_expr),
                       Value(def.provider)});
}

Result<void> CatalogPersistence::persist_columns_update(const TableSchema& schema) {
    // Delete all existing sys_columns rows for this table.
    auto del = delete_rows(sys_columns_table_id,
                           [table_id = schema.table_id](const std::vector<Value>& v) {
                               return v[0].as_int32() == table_id;
                           });
    if (!del) {
        return del;
    }

    // Re-insert all columns from the updated schema.
    for (const auto& col : schema.columns) {
        auto r = insert_row(sys_columns_table_id,
                            {Value(schema.table_id),
                             Value(col.ordinal),
                             Value(col.name),
                             Value(static_cast<int32_t>(col.type_id)),
                             Value(col.nullable),
                             col.default_expr.empty() ? Value() : Value(col.default_expr),
                             Value(col.is_autoincrement),
                             Value(col.precision),
                             Value(col.scale)});
        if (!r) {
            return r;
        }
    }

    return ok();
}

// ---------------------------------------------------------------------------
// Embedding job persistence (sys_embedding_jobs)
// ---------------------------------------------------------------------------

Result<void> CatalogPersistence::create_embedding_jobs_table() {
    return create_sys_table(sys_embedding_jobs_schema());
}

Result<void> CatalogPersistence::open_embedding_jobs_table() {
    return open_sys_table(sys_embedding_jobs_schema());
}

Result<void> CatalogPersistence::migrate_databases_from_sys_tables() {
    // Open sys_tables to scan for distinct database_id values.
    auto ts = storage_.get_table_storage(sys_tables_table_id);
    if (!ts) {
        // sys_tables not open yet — need to open it first.
        auto open_r = open_sys_table(sys_tables_schema());
        if (!open_r) {
            return make_error(open_r.error().code,
                              "migration: failed to open sys_tables: " + open_r.error().message);
        }
        ts = storage_.get_table_storage(sys_tables_table_id);
        if (!ts) {
            return make_error(ts.error().code, ts.error().message);
        }
    }

    auto storage_schema = StorageManager::build_storage_schema(sys_tables_schema());
    auto it = (*ts)->heap->begin();
    if (!it) {
        return make_error(it.error().code, it.error().message);
    }

    // Collect distinct database IDs.
    std::set<database_id_t> db_ids;
    for (;;) {
        auto row_result = it->next();
        if (!row_result) {
            return make_error(row_result.error().code, row_result.error().message);
        }
        if (!row_result->has_value()) {
            break;
        }
        auto& row = **row_result;
        auto values = TupleSerializer::deserialize(row.second, storage_schema);
        if (!values) {
            continue;
        }
        auto db_id = static_cast<database_id_t>((*values)[1].as_int32());
        if (db_id != system_database_id) {
            db_ids.insert(db_id);
        }
    }

    // Always ensure the default database is included.
    db_ids.insert(default_database_id);

    // Insert each database into sys_databases.
    for (auto db_id : db_ids) {
        std::string name = (db_id == default_database_id) ? default_database_name
                                                          : "database_" + std::to_string(db_id);
        auto r = insert_row(sys_databases_table_id, {Value(db_id), Value(name)});
        if (!r) {
            SIXSEVEN_LOG_WARN(
                "migration: failed to insert database {}: {}", db_id, r.error().message);
        }
    }

    SIXSEVEN_LOG_INFO("catalog persistence: migrated {} databases to sys_databases", db_ids.size());
    return ok();
}

Result<void> CatalogPersistence::persist_embedding_job(const EmbeddingJob& job) {
    return insert_row(sys_embedding_jobs_table_id,
                      {Value(job.table_id),
                       Value(job.row_id),
                       Value(job.column_id),
                       Value(job.source_text),
                       Value(job.provider),
                       Value(job.dimension),
                       Value(static_cast<int32_t>(job.type)),
                       Value(job.retry_count)});
}

Result<void>
CatalogPersistence::persist_embedding_jobs_batch(const std::vector<EmbeddingJob>& jobs) {
    if (jobs.empty()) {
        return ok();
    }

    auto ts = storage_.get_table_storage(sys_embedding_jobs_table_id);
    if (!ts) {
        return make_error(ts.error().code, ts.error().message);
    }

    auto table_schema = catalog_.get_table_by_id(sys_embedding_jobs_table_id);
    if (!table_schema) {
        return make_error(table_schema.error().code, table_schema.error().message);
    }

    auto storage_schema = StorageManager::build_storage_schema(*table_schema);

    // Serialize all jobs into byte spans for insert_batch.
    std::vector<std::vector<uint8_t>> serialized;
    serialized.reserve(jobs.size());
    for (const auto& job : jobs) {
        std::vector<Value> values = {Value(job.table_id),
                                     Value(job.row_id),
                                     Value(job.column_id),
                                     Value(job.source_text),
                                     Value(job.provider),
                                     Value(job.dimension),
                                     Value(static_cast<int32_t>(job.type)),
                                     Value(job.retry_count)};
        auto data = TupleSerializer::serialize(values, storage_schema);
        if (!data) {
            return make_error(data.error().code, data.error().message);
        }
        serialized.push_back(std::move(*data));
    }

    // Build spans from the serialized data.
    std::vector<std::span<const uint8_t>> spans;
    spans.reserve(serialized.size());
    for (const auto& s : serialized) {
        spans.emplace_back(s);
    }

    auto rids = (*ts)->heap->insert_batch(spans);
    if (!rids) {
        return make_error(rids.error().code, rids.error().message);
    }

    // Single flush for the entire batch.
    auto flush = (*ts)->bpm->flush_all();
    if (!flush) {
        return make_error(flush.error().code, flush.error().message);
    }

    return ok();
}

Result<void>
CatalogPersistence::remove_embedding_job(table_id_t table_id, int64_t row_id, int32_t column_id) {
    return delete_rows(sys_embedding_jobs_table_id,
                       [table_id, row_id, column_id](const std::vector<Value>& v) {
                           return v[0].as_int32() == table_id && v[1].as_int64() == row_id &&
                                  v[2].as_int32() == column_id;
                       });
}

Result<std::vector<EmbeddingJob>> CatalogPersistence::load_embedding_jobs() {
    auto ts = storage_.get_table_storage(sys_embedding_jobs_table_id);
    if (!ts) {
        return make_error(ts.error().code, ts.error().message);
    }

    auto storage_schema = StorageManager::build_storage_schema(sys_embedding_jobs_schema());
    auto it = (*ts)->heap->begin();
    if (!it) {
        return make_error(it.error().code, it.error().message);
    }

    std::vector<EmbeddingJob> jobs;
    for (;;) {
        auto row_result = it->next();
        if (!row_result) {
            return make_error(row_result.error().code, row_result.error().message);
        }
        if (!row_result->has_value()) {
            break;
        }
        auto& row = **row_result;
        auto values = TupleSerializer::deserialize(row.second, storage_schema);
        if (!values) {
            SIXSEVEN_LOG_WARN("embedding job persistence: skipping corrupt row");
            continue;
        }
        auto& v = *values;
        EmbeddingJob job;
        job.table_id = v[0].as_int32();
        job.row_id = v[1].as_int64();
        job.column_id = v[2].as_int32();
        job.source_text = v[3].as_string();
        job.provider = v[4].as_string();
        job.dimension = v[5].as_int32();
        job.type = static_cast<EmbeddingJob::Type>(v[6].as_int32());
        job.retry_count = v[7].as_int32();
        jobs.push_back(std::move(job));
    }

    SIXSEVEN_LOG_INFO("embedding job persistence: loaded {} pending jobs", jobs.size());
    return ok(std::move(jobs));
}

// ---------------------------------------------------------------------------
// User credential persistence (sys_users)
// ---------------------------------------------------------------------------

Result<void> CatalogPersistence::create_users_table() {
    return create_sys_table(sys_users_schema());
}

Result<void> CatalogPersistence::open_users_table() {
    return open_sys_table(sys_users_schema());
}

Result<void> CatalogPersistence::persist_user(const UserRecord& user) {
    // Upsert: ALTER USER reuses the username, so remove any existing row first.
    auto removed = remove_user(user.username);
    if (!removed) {
        return removed;
    }

    return insert_row(sys_users_table_id,
                      {Value(user.username),
                       Value(user.password_hash),
                       Value(user.salt),
                       Value(user.iterations),
                       Value(auth_method_to_string(user.method)),
                       Value::make_null()});
}

Result<void> CatalogPersistence::remove_user(const std::string& username) {
    return delete_rows(sys_users_table_id, [&username](const std::vector<Value>& v) {
        return !v[0].is_null() && v[0].as_string() == username;
    });
}

Result<std::vector<UserRecord>> CatalogPersistence::load_users() {
    auto ts = storage_.get_table_storage(sys_users_table_id);
    if (!ts) {
        return make_error(ts.error().code, ts.error().message);
    }

    auto storage_schema = StorageManager::build_storage_schema(sys_users_schema());
    auto it = (*ts)->heap->begin();
    if (!it) {
        return make_error(it.error().code, it.error().message);
    }

    std::vector<UserRecord> users;
    for (;;) {
        auto row_result = it->next();
        if (!row_result) {
            return make_error(row_result.error().code, row_result.error().message);
        }
        if (!row_result->has_value()) {
            break;
        }
        auto& row = **row_result;
        auto values = TupleSerializer::deserialize(row.second, storage_schema);
        if (!values) {
            SIXSEVEN_LOG_WARN("user persistence: skipping corrupt row");
            continue;
        }
        auto& v = *values;
        UserRecord record;
        record.username = v[0].is_null() ? "" : v[0].as_string();
        record.password_hash = v[1].is_null() ? "" : v[1].as_string();
        record.salt = v[2].is_null() ? "" : v[2].as_string();
        record.iterations = v[3].is_null() ? 4096 : v[3].as_int32();
        auto method = parse_auth_method(v[4].is_null() ? "trust" : v[4].as_string());
        record.method = method ? *method : AuthMethod::TRUST;
        users.push_back(std::move(record));
    }

    SIXSEVEN_LOG_INFO("user persistence: loaded {} users", users.size());
    return ok(std::move(users));
}

} // namespace sixseven
