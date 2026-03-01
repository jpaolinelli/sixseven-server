#include "giodb/executor/catalog_persistence.h"

#include "giodb/common/logging.h"
#include "giodb/common/value.h"
#include "giodb/table/tuple.h"

#include <algorithm>
#include <span>
#include <unordered_map>

namespace giodb {

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

    GIODB_LOG_INFO("catalog persistence: created system catalog tables");
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
    auto restore = catalog_.restore_table(system_database_id, schema);
    if (!restore) {
        return make_error(restore.error().code,
                          "failed to register " + schema.name + ": " + restore.error().message);
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

    // 2. Read sys_tables: collect table metadata (without columns yet).
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
        while (auto row = it->next()) {
            auto values = TupleSerializer::deserialize(row->second, storage_schema);
            if (!values) {
                GIODB_LOG_WARN("catalog persistence: skipping corrupt sys_tables row");
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

    // 3. Read sys_columns: add columns to the collected tables.
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
        while (auto row = it->next()) {
            auto values = TupleSerializer::deserialize(row->second, storage_schema);
            if (!values) {
                GIODB_LOG_WARN("catalog persistence: skipping corrupt sys_columns row");
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
            table_it->second.columns.push_back(std::move(col));
        }
    }

    // 4. Restore tables into the Catalog and open their storage files.
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
            GIODB_LOG_WARN("catalog persistence: failed to create db storage for db {}",
                           info.database_id);
        }

        TableSchema schema;
        schema.table_id = info.table_id;
        schema.name = info.name;
        schema.pk_columns = info.pk_columns;
        schema.columns = std::move(info.columns);

        auto restore = catalog_.restore_table(info.database_id, schema);
        if (!restore) {
            GIODB_LOG_WARN("catalog persistence: failed to restore table '{}': {}",
                           info.name,
                           restore.error().message);
            continue;
        }

        // Open the table's storage file.
        auto open = storage_.open_table_storage(info.database_id, info.table_id, schema);
        if (!open) {
            GIODB_LOG_WARN("catalog persistence: failed to open storage for table '{}': {}",
                           info.name,
                           open.error().message);
            continue;
        }

        // Initialize autoincrement counters by scanning existing data.
        for (const auto& col : schema.columns) {
            if (!col.is_autoincrement) {
                continue;
            }
            int64_t max_val = 0;
            auto ts_ai = storage_.get_table_storage(info.table_id);
            if (ts_ai) {
                auto ai_schema = StorageManager::build_storage_schema(schema);
                auto ai_it = (*ts_ai)->heap->begin();
                if (ai_it) {
                    while (auto row = ai_it->next()) {
                        auto vals = TupleSerializer::deserialize(row->second, ai_schema);
                        if (!vals) {
                            continue;
                        }
                        auto& v_ai = (*vals)[static_cast<size_t>(col.ordinal)];
                        if (v_ai.is_null()) {
                            continue;
                        }
                        int64_t row_val = 0;
                        switch (col.type_id) {
                        case TypeId::INT8:
                            row_val = v_ai.as_int8();
                            break;
                        case TypeId::INT16:
                            row_val = v_ai.as_int16();
                            break;
                        case TypeId::INT32:
                            row_val = v_ai.as_int32();
                            break;
                        case TypeId::INT64:
                            row_val = v_ai.as_int64();
                            break;
                        case TypeId::UINT8:
                            row_val = v_ai.as_uint8();
                            break;
                        case TypeId::UINT16:
                            row_val = v_ai.as_uint16();
                            break;
                        case TypeId::UINT32:
                            row_val = v_ai.as_uint32();
                            break;
                        case TypeId::UINT64:
                            row_val = static_cast<int64_t>(v_ai.as_uint64());
                            break;
                        default:
                            break;
                        }
                        if (row_val > max_val) {
                            max_val = row_val;
                        }
                    }
                }
            }
            catalog_.init_autoincrement(info.table_id, max_val + 1);
            GIODB_LOG_DEBUG("catalog persistence: autoincrement for table '{}' starts at {}",
                            info.name,
                            max_val + 1);
            break; // Only one autoincrement column per table.
        }
    }

    // 5. Read sys_indexes: restore index metadata.
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
        while (auto row = it->next()) {
            auto values = TupleSerializer::deserialize(row->second, storage_schema);
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
                GIODB_LOG_WARN("catalog persistence: failed to restore index: {}",
                               r.error().message);
            }
        }
    }

    // 6. Read sys_edge_types: restore edge type metadata.
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
        while (auto row = it->next()) {
            auto values = TupleSerializer::deserialize(row->second, storage_schema);
            if (!values) {
                continue;
            }
            auto& v = *values;
            EdgeTypeDef def;
            def.edge_id = v[0].as_int32();
            def.name = v[1].as_string();
            def.source_table_id = v[2].as_int32();
            def.target_table_id = v[3].as_int32();
            def.properties = v[4].is_null() ? "" : v[4].as_string();

            auto r = catalog_.restore_edge_type(std::move(def));
            if (!r) {
                GIODB_LOG_WARN("catalog persistence: failed to restore edge type: {}",
                               r.error().message);
            }
        }
    }

    // 7. Read sys_embedding_columns: restore embedding column metadata.
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
        while (auto row = it->next()) {
            auto values = TupleSerializer::deserialize(row->second, storage_schema);
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
    GIODB_LOG_INFO("catalog persistence: loaded {} user tables from disk", table_count);
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
    while (auto row = it->next()) {
        auto values = TupleSerializer::deserialize(row->second, storage_schema);
        if (!values) {
            continue; // Skip corrupt rows.
        }
        if (predicate(*values)) {
            to_delete.push_back(row->first);
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
                             Value(col.is_autoincrement)});
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
        return v[2].as_int32() == table_id || v[3].as_int32() == table_id;
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

} // namespace giodb
