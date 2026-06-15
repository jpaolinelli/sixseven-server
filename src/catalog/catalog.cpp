#include "sixseven/catalog/catalog.h"

#include "sixseven/common/types.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace sixseven {

// -- Constructor --------------------------------------------------------------

Catalog::Catalog() {
    // Create the system 'sixseven_system' database.
    databases_by_id_[system_database_id] = Database{system_database_id, system_database_name};
    database_name_to_id_[system_database_name] = system_database_id;
}

// -- Database operations ------------------------------------------------------

Result<database_id_t> Catalog::create_database(const std::string& name) {
    std::lock_guard lock(mu_);

    if (database_name_to_id_.contains(name)) {
        return make_error(StatusCode::ALREADY_EXISTS, "database '" + name + "' already exists");
    }

    database_id_t id = next_database_id_++;
    databases_by_id_[id] = Database{id, name};
    database_name_to_id_[name] = id;

    return ok(id);
}

Result<void> Catalog::drop_database(database_id_t database_id, bool cascade) {
    std::lock_guard lock(mu_);

    if (database_id == system_database_id) {
        return make_error(StatusCode::CONSTRAINT_VIOLATION, "cannot drop the system database");
    }

    auto db_it = databases_by_id_.find(database_id);
    if (db_it == databases_by_id_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "database with id " + std::to_string(database_id) + " not found");
    }

    // Collect all table IDs that belong to this database before any drops,
    // so we can clean up cross-database edge type references afterward.
    std::vector<table_id_t> dropped_table_ids;
    {
        auto name_map_it = table_name_to_id_.find(database_id);
        if (name_map_it != table_name_to_id_.end()) {
            for (auto& [tname, tid] : name_map_it->second) {
                dropped_table_ids.push_back(tid);
            }
        }
    }

    // Check if database contains tables.
    auto& name_map = table_name_to_id_[database_id];
    if (!name_map.empty()) {
        if (!cascade) {
            return make_error(StatusCode::CONSTRAINT_VIOLATION,
                              "database '" + db_it->second.name +
                                  "' contains tables; use cascade to drop");
        }

        // Cascade: drop all tables in this database.
        std::vector<std::string> table_names;
        table_names.reserve(name_map.size());
        for (auto& [tname, _] : name_map) {
            table_names.push_back(tname);
        }
        for (auto& tname : table_names) {
            auto result = drop_table_locked(database_id, tname);
            if (!result.has_value()) {
                return result;
            }
        }
    }

    // Remove any edge types still owned by this database (e.g. those whose
    // source/target tables belonged to a foreign database and were therefore
    // not touched by the cascade table drops above).
    {
        std::vector<edge_id_t> owned_edge_ids;
        for (auto& [eid, edef] : edge_types_by_id_) {
            if (edef.database_id == database_id) {
                owned_edge_ids.push_back(eid);
            }
        }
        for (auto eid : owned_edge_ids) {
            edge_types_by_id_.erase(eid);
        }
        // The name map for this database is fully erased below.
    }

    // Remove edge types in OTHER databases that reference tables which belonged
    // to the dropped database (dangling cross-database references).
    if (!dropped_table_ids.empty()) {
        std::unordered_set<table_id_t> dropped_set(dropped_table_ids.begin(),
                                                   dropped_table_ids.end());

        std::vector<edge_id_t> dangling_edge_ids;
        for (auto& [eid, edef] : edge_types_by_id_) {
            if (dropped_set.count(edef.source_table_id) != 0 ||
                dropped_set.count(edef.target_table_id) != 0) {
                dangling_edge_ids.push_back(eid);
            }
        }

        for (auto eid : dangling_edge_ids) {
            auto& edef = edge_types_by_id_.at(eid);
            // Remove from the owning database's name map.
            auto owner_it = edge_name_to_id_.find(edef.database_id);
            if (owner_it != edge_name_to_id_.end()) {
                owner_it->second.erase(edef.name);
            }
            edge_types_by_id_.erase(eid);
        }
    }

    // Remove the database.
    database_name_to_id_.erase(db_it->second.name);
    databases_by_id_.erase(db_it);
    table_name_to_id_.erase(database_id);
    edge_name_to_id_.erase(database_id);

    return ok();
}

Result<Database> Catalog::get_database(const std::string& name) const {
    std::lock_guard lock(mu_);

    auto name_it = database_name_to_id_.find(name);
    if (name_it == database_name_to_id_.end()) {
        return make_error(StatusCode::NOT_FOUND, "database '" + name + "' not found");
    }

    return ok(databases_by_id_.at(name_it->second));
}

std::vector<Database> Catalog::list_databases() const {
    std::lock_guard lock(mu_);

    std::vector<Database> result;
    result.reserve(databases_by_id_.size());
    for (auto& [id, db] : databases_by_id_) {
        result.push_back(db);
    }

    std::sort(result.begin(), result.end(), [](const Database& a, const Database& b) {
        return a.database_id < b.database_id;
    });

    return result;
}

// -- Table operations ---------------------------------------------------------

Result<table_id_t> Catalog::create_table(database_id_t database_id, TableSchema schema) {
    std::lock_guard lock(mu_);

    // Validate the database exists.
    if (!databases_by_id_.contains(database_id)) {
        return make_error(StatusCode::NOT_FOUND,
                          "database with id " + std::to_string(database_id) + " not found");
    }

    auto& name_map = table_name_to_id_[database_id];
    if (name_map.contains(schema.name)) {
        return make_error(StatusCode::ALREADY_EXISTS, "table '" + schema.name + "' already exists");
    }

    // Check for duplicate column names (case-insensitive).
    for (size_t i = 0; i < schema.columns.size(); ++i) {
        auto upper_i = schema.columns[i].name;
        std::transform(upper_i.begin(), upper_i.end(), upper_i.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        for (size_t j = i + 1; j < schema.columns.size(); ++j) {
            auto upper_j = schema.columns[j].name;
            std::transform(upper_j.begin(), upper_j.end(), upper_j.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            if (upper_i == upper_j) {
                return make_error(StatusCode::ALREADY_EXISTS,
                                  "duplicate column name '" + schema.columns[j].name +
                                      "' in CREATE TABLE '" + schema.name + "'");
            }
        }
    }

    table_id_t id = next_table_id_++;
    schema.table_id = id;

    // Assign ordinals to columns if not already set.
    for (int32_t i = 0; i < static_cast<int32_t>(schema.columns.size()); ++i) {
        schema.columns[static_cast<size_t>(i)].ordinal = i;
    }

    name_map[schema.name] = id;
    table_to_database_[id] = database_id;
    tables_by_id_[id] = std::move(schema);

    return ok(id);
}

Result<void> Catalog::drop_table(database_id_t database_id, const std::string& name) {
    std::lock_guard lock(mu_);
    return drop_table_locked(database_id, name);
}

Result<void> Catalog::drop_table_locked(database_id_t database_id, const std::string& name) {
    // Protect system tables in the sixseven_system database.
    if (database_id == system_database_id) {
        return make_error(StatusCode::CONSTRAINT_VIOLATION,
                          "cannot drop system table '" + name + "'");
    }

    auto db_map_it = table_name_to_id_.find(database_id);
    if (db_map_it == table_name_to_id_.end()) {
        return make_error(StatusCode::NOT_FOUND, "table '" + name + "' not found");
    }

    auto& name_map = db_map_it->second;
    auto name_it = name_map.find(name);
    if (name_it == name_map.end()) {
        return make_error(StatusCode::NOT_FOUND, "table '" + name + "' not found");
    }

    table_id_t tid = name_it->second;

    // Remove all indexes for this table.
    std::vector<std::string> idx_names_to_remove;
    for (auto& [idx_id, idx_def] : indexes_by_id_) {
        if (idx_def.table_id == tid) {
            idx_names_to_remove.push_back(idx_def.name);
        }
    }
    for (auto& idx_name : idx_names_to_remove) {
        auto db_idx_it = index_name_to_id_.find(database_id);
        if (db_idx_it != index_name_to_id_.end()) {
            auto idx_name_it = db_idx_it->second.find(idx_name);
            if (idx_name_it != db_idx_it->second.end()) {
                indexes_by_id_.erase(idx_name_it->second);
                db_idx_it->second.erase(idx_name_it);
            }
        }
    }

    // Remove all edge types in this database referencing this table (source or target).
    std::vector<std::string> edge_names_to_remove;
    for (auto& [eid, edef] : edge_types_by_id_) {
        if (edef.database_id == database_id &&
            (edef.source_table_id == tid || edef.target_table_id == tid)) {
            edge_names_to_remove.push_back(edef.name);
        }
    }
    auto& edge_name_map = edge_name_to_id_[database_id];
    for (auto& ename : edge_names_to_remove) {
        auto ename_it = edge_name_map.find(ename);
        if (ename_it != edge_name_map.end()) {
            edge_types_by_id_.erase(ename_it->second);
            edge_name_map.erase(ename_it);
        }
    }

    // Remove all embedding column definitions for this table.
    std::erase_if(embedding_columns_,
                  [tid](const EmbeddingColumnDef& e) { return e.table_id == tid; });

    // Remove the auto-increment counter for this table (if any).
    autoincrement_counters_.erase(tid);

    tables_by_id_.erase(tid);
    table_to_database_.erase(tid);
    name_map.erase(name_it);

    return ok();
}

Result<TableSchema> Catalog::get_table(database_id_t database_id, const std::string& name) const {
    std::lock_guard lock(mu_);

    auto db_map_it = table_name_to_id_.find(database_id);
    if (db_map_it == table_name_to_id_.end()) {
        return make_error(StatusCode::NOT_FOUND, "table '" + name + "' not found");
    }

    auto& name_map = db_map_it->second;
    auto name_it = name_map.find(name);
    if (name_it == name_map.end()) {
        return make_error(StatusCode::NOT_FOUND, "table '" + name + "' not found");
    }

    return ok(tables_by_id_.at(name_it->second));
}

Result<TableSchema> Catalog::get_table_by_id(table_id_t id) const {
    std::lock_guard lock(mu_);

    auto it = tables_by_id_.find(id);
    if (it == tables_by_id_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "table with id " + std::to_string(id) + " not found");
    }

    return ok(it->second);
}

database_id_t Catalog::get_table_database_id(table_id_t id) const {
    std::lock_guard lock(mu_);
    auto it = table_to_database_.find(id);
    return it != table_to_database_.end() ? it->second : 0;
}

std::vector<TableSchema> Catalog::list_tables(database_id_t database_id) const {
    std::lock_guard lock(mu_);

    std::vector<TableSchema> result;

    auto db_map_it = table_name_to_id_.find(database_id);
    if (db_map_it == table_name_to_id_.end()) {
        return result;
    }

    auto& name_map = db_map_it->second;
    result.reserve(name_map.size());
    for (auto& [tname, tid] : name_map) {
        result.push_back(tables_by_id_.at(tid));
    }

    // Sort by table_id for deterministic output.
    std::sort(result.begin(), result.end(), [](const TableSchema& a, const TableSchema& b) {
        return a.table_id < b.table_id;
    });

    return result;
}

// -- Column mutation operations -----------------------------------------------

Result<void> Catalog::add_column(table_id_t table_id, CatalogColumnDef column) {
    std::lock_guard lock(mu_);

    auto it = tables_by_id_.find(table_id);
    if (it == tables_by_id_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "table with id " + std::to_string(table_id) + " not found");
    }

    auto& schema = it->second;

    // Check for duplicate column name (case-insensitive).
    for (const auto& col : schema.columns) {
        auto upper_existing = col.name;
        auto upper_new = column.name;
        std::transform(upper_existing.begin(),
                       upper_existing.end(),
                       upper_existing.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        std::transform(upper_new.begin(), upper_new.end(), upper_new.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        if (upper_existing == upper_new) {
            return make_error(StatusCode::ALREADY_EXISTS,
                              "column '" + column.name + "' already exists");
        }
    }

    // Assign ordinal as next sequential value.
    column.ordinal = static_cast<int32_t>(schema.columns.size());
    schema.columns.push_back(std::move(column));

    return ok();
}

Result<void> Catalog::drop_column(table_id_t table_id, const std::string& column_name) {
    std::lock_guard lock(mu_);

    auto it = tables_by_id_.find(table_id);
    if (it == tables_by_id_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "table with id " + std::to_string(table_id) + " not found");
    }

    auto& schema = it->second;

    // Find the column (case-insensitive).
    auto col_it = schema.columns.end();
    for (auto ci = schema.columns.begin(); ci != schema.columns.end(); ++ci) {
        auto upper_existing = ci->name;
        auto upper_target = column_name;
        std::transform(upper_existing.begin(),
                       upper_existing.end(),
                       upper_existing.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        std::transform(upper_target.begin(),
                       upper_target.end(),
                       upper_target.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (upper_existing == upper_target) {
            col_it = ci;
            break;
        }
    }

    if (col_it == schema.columns.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "column '" + column_name + "' not found in table '" + schema.name + "'");
    }

    schema.columns.erase(col_it);

    // Renumber ordinals.
    for (int32_t i = 0; i < static_cast<int32_t>(schema.columns.size()); ++i) {
        schema.columns[static_cast<size_t>(i)].ordinal = i;
    }

    return ok();
}

Result<void> Catalog::rename_column(table_id_t table_id,
                                    const std::string& old_name,
                                    const std::string& new_name) {
    std::lock_guard lock(mu_);

    auto it = tables_by_id_.find(table_id);
    if (it == tables_by_id_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "table with id " + std::to_string(table_id) + " not found");
    }

    auto& schema = it->second;

    // Check that the new name doesn't already exist (case-insensitive).
    auto upper_new = new_name;
    std::transform(upper_new.begin(), upper_new.end(), upper_new.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    for (const auto& col : schema.columns) {
        auto upper_existing = col.name;
        std::transform(upper_existing.begin(),
                       upper_existing.end(),
                       upper_existing.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (upper_existing == upper_new) {
            return make_error(StatusCode::ALREADY_EXISTS,
                              "column '" + new_name + "' already exists");
        }
    }

    // Find and rename the column.
    auto upper_old = old_name;
    std::transform(upper_old.begin(), upper_old.end(), upper_old.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    for (auto& col : schema.columns) {
        auto upper_existing = col.name;
        std::transform(upper_existing.begin(),
                       upper_existing.end(),
                       upper_existing.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (upper_existing == upper_old) {
            // Update pk_columns if this column is part of the primary key.
            if (!schema.pk_columns.empty()) {
                // pk_columns is comma-separated. Replace matching name.
                std::string new_pk;
                std::istringstream ss(schema.pk_columns);
                std::string part;
                while (std::getline(ss, part, ',')) {
                    auto upper_part = part;
                    std::transform(
                        upper_part.begin(),
                        upper_part.end(),
                        upper_part.begin(),
                        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                    if (!new_pk.empty()) {
                        new_pk += ",";
                    }
                    if (upper_part == upper_old) {
                        new_pk += new_name;
                    } else {
                        new_pk += part;
                    }
                }
                schema.pk_columns = new_pk;
            }

            col.name = new_name;
            return ok();
        }
    }

    return make_error(StatusCode::NOT_FOUND,
                      "column '" + old_name + "' not found in table '" + schema.name + "'");
}

// -- Index operations ---------------------------------------------------------

Result<index_id_t> Catalog::create_index(IndexDef def) {
    std::lock_guard lock(mu_);

    // Validate that the referenced table exists.
    if (!tables_by_id_.contains(def.table_id)) {
        return make_error(StatusCode::NOT_FOUND,
                          "table with id " + std::to_string(def.table_id) + " not found");
    }

    // Index names are unique within a database (not globally).
    auto db_id = table_to_database_.contains(def.table_id) ? table_to_database_[def.table_id]
                                                           : default_database_id;
    auto& db_index_names = index_name_to_id_[db_id];
    if (db_index_names.contains(def.name)) {
        return make_error(StatusCode::ALREADY_EXISTS, "index '" + def.name + "' already exists");
    }

    index_id_t id = next_index_id_++;
    def.index_id = id;

    db_index_names[def.name] = id;
    indexes_by_id_[id] = std::move(def);

    return ok(id);
}

Result<void> Catalog::drop_index(const std::string& name) {
    std::lock_guard lock(mu_);

    // Search across all databases for the index name.
    for (auto& [db_id, db_names] : index_name_to_id_) {
        auto name_it = db_names.find(name);
        if (name_it != db_names.end()) {
            indexes_by_id_.erase(name_it->second);
            db_names.erase(name_it);
            return ok();
        }
    }

    return make_error(StatusCode::NOT_FOUND, "index '" + name + "' not found");
}

Result<IndexDef> Catalog::get_index(const std::string& name) const {
    std::lock_guard lock(mu_);

    // Search across all databases for the index name.
    for (const auto& [db_id, db_names] : index_name_to_id_) {
        auto name_it = db_names.find(name);
        if (name_it != db_names.end()) {
            return ok(indexes_by_id_.at(name_it->second));
        }
    }

    return make_error(StatusCode::NOT_FOUND, "index '" + name + "' not found");
}

std::vector<IndexDef> Catalog::list_indexes(table_id_t table_id) const {
    std::lock_guard lock(mu_);

    std::vector<IndexDef> result;
    for (auto& [id, def] : indexes_by_id_) {
        if (def.table_id == table_id) {
            result.push_back(def);
        }
    }

    std::sort(result.begin(), result.end(), [](const IndexDef& a, const IndexDef& b) {
        return a.index_id < b.index_id;
    });

    return result;
}

std::vector<IndexDef> Catalog::list_all_indexes() const {
    std::lock_guard lock(mu_);

    std::vector<IndexDef> result;
    result.reserve(indexes_by_id_.size());
    for (auto& [id, def] : indexes_by_id_) {
        result.push_back(def);
    }

    std::sort(result.begin(), result.end(), [](const IndexDef& a, const IndexDef& b) {
        return a.index_id < b.index_id;
    });

    return result;
}

// -- Edge type operations -----------------------------------------------------

Result<edge_id_t> Catalog::create_edge_type(database_id_t database_id, EdgeTypeDef def) {
    std::lock_guard lock(mu_);

    auto& name_map = edge_name_to_id_[database_id];
    if (name_map.contains(def.name)) {
        return make_error(StatusCode::ALREADY_EXISTS,
                          "edge type '" + def.name + "' already exists");
    }

    // Validate source table exists.
    if (!tables_by_id_.contains(def.source_table_id)) {
        return make_error(StatusCode::NOT_FOUND,
                          "source table with id " + std::to_string(def.source_table_id) +
                              " not found");
    }

    // Validate target table exists.
    if (!tables_by_id_.contains(def.target_table_id)) {
        return make_error(StatusCode::NOT_FOUND,
                          "target table with id " + std::to_string(def.target_table_id) +
                              " not found");
    }

    edge_id_t id = next_edge_id_++;
    def.edge_id = id;
    def.database_id = database_id;

    name_map[def.name] = id;
    edge_types_by_id_[id] = std::move(def);

    return ok(id);
}

Result<void> Catalog::drop_edge_type(database_id_t database_id, const std::string& name) {
    std::lock_guard lock(mu_);

    auto db_map_it = edge_name_to_id_.find(database_id);
    if (db_map_it == edge_name_to_id_.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + name + "' not found");
    }

    auto& name_map = db_map_it->second;
    auto name_it = name_map.find(name);
    if (name_it == name_map.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + name + "' not found");
    }

    edge_types_by_id_.erase(name_it->second);
    name_map.erase(name_it);

    return ok();
}

Result<EdgeTypeDef> Catalog::get_edge_type(database_id_t database_id,
                                           const std::string& name) const {
    std::lock_guard lock(mu_);

    auto db_map_it = edge_name_to_id_.find(database_id);
    if (db_map_it == edge_name_to_id_.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + name + "' not found");
    }

    auto& name_map = db_map_it->second;
    auto name_it = name_map.find(name);
    if (name_it == name_map.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + name + "' not found");
    }

    return ok(edge_types_by_id_.at(name_it->second));
}

std::vector<EdgeTypeDef> Catalog::list_edge_types(database_id_t database_id) const {
    std::lock_guard lock(mu_);

    std::vector<EdgeTypeDef> result;

    auto db_map_it = edge_name_to_id_.find(database_id);
    if (db_map_it == edge_name_to_id_.end()) {
        return result;
    }

    auto& name_map = db_map_it->second;
    result.reserve(name_map.size());
    for (auto& [ename, eid] : name_map) {
        result.push_back(edge_types_by_id_.at(eid));
    }

    std::sort(result.begin(), result.end(), [](const EdgeTypeDef& a, const EdgeTypeDef& b) {
        return a.edge_id < b.edge_id;
    });

    return result;
}

std::vector<EdgeTypeDef> Catalog::list_all_edge_types() const {
    std::lock_guard lock(mu_);

    std::vector<EdgeTypeDef> result;
    result.reserve(edge_types_by_id_.size());
    for (auto& [id, def] : edge_types_by_id_) {
        result.push_back(def);
    }

    std::sort(result.begin(), result.end(), [](const EdgeTypeDef& a, const EdgeTypeDef& b) {
        return a.edge_id < b.edge_id;
    });

    return result;
}

// -- Embedding column operations ----------------------------------------------

Result<void> Catalog::register_embedding_column(EmbeddingColumnDef def) {
    std::lock_guard lock(mu_);

    // Validate that the referenced table exists.
    if (!tables_by_id_.contains(def.table_id)) {
        return make_error(StatusCode::NOT_FOUND,
                          "table with id " + std::to_string(def.table_id) + " not found");
    }

    // Check for duplicate (table_id, column_id) pair.
    for (auto& existing : embedding_columns_) {
        if (existing.table_id == def.table_id && existing.column_id == def.column_id) {
            return make_error(StatusCode::ALREADY_EXISTS,
                              "embedding column already registered for table " +
                                  std::to_string(def.table_id) + ", column " +
                                  std::to_string(def.column_id));
        }
    }

    embedding_columns_.push_back(std::move(def));
    return ok();
}

std::vector<EmbeddingColumnDef> Catalog::list_embedding_columns(table_id_t table_id) const {
    std::lock_guard lock(mu_);

    std::vector<EmbeddingColumnDef> result;
    for (auto& def : embedding_columns_) {
        if (def.table_id == table_id) {
            result.push_back(def);
        }
    }

    return result;
}

std::vector<EmbeddingColumnDef> Catalog::list_all_embedding_columns() const {
    std::lock_guard lock(mu_);
    return embedding_columns_;
}

// -- Embedding provider operations --------------------------------------------

Result<void> Catalog::register_embedding_provider(EmbeddingProviderConfig config) {
    std::lock_guard lock(mu_);

    if (embedding_providers_.contains(config.name)) {
        return make_error(StatusCode::ALREADY_EXISTS,
                          "embedding provider '" + config.name + "' already exists");
    }

    auto name = config.name;
    embedding_providers_.emplace(std::move(name), std::move(config));
    return ok();
}

Result<EmbeddingProviderConfig> Catalog::get_embedding_provider(const std::string& name) const {
    std::lock_guard lock(mu_);

    auto it = embedding_providers_.find(name);
    if (it == embedding_providers_.end()) {
        return make_error(StatusCode::NOT_FOUND, "embedding provider '" + name + "' not found");
    }

    return ok(it->second);
}

std::vector<EmbeddingProviderConfig> Catalog::list_embedding_providers() const {
    std::lock_guard lock(mu_);

    std::vector<EmbeddingProviderConfig> result;
    result.reserve(embedding_providers_.size());
    for (auto& [name, config] : embedding_providers_) {
        result.push_back(config);
    }

    std::sort(result.begin(),
              result.end(),
              [](const EmbeddingProviderConfig& a, const EmbeddingProviderConfig& b) {
                  return a.name < b.name;
              });

    return result;
}

Result<void> Catalog::remove_embedding_provider(const std::string& name) {
    std::lock_guard lock(mu_);

    auto it = embedding_providers_.find(name);
    if (it == embedding_providers_.end()) {
        return make_error(StatusCode::NOT_FOUND, "embedding provider '" + name + "' not found");
    }

    embedding_providers_.erase(it);
    return ok();
}

// -- Persistence restore operations -------------------------------------------

Result<void> Catalog::restore_database(database_id_t id, const std::string& name) {
    std::lock_guard lock(mu_);

    if (databases_by_id_.contains(id)) {
        return make_error(StatusCode::ALREADY_EXISTS,
                          "database with id " + std::to_string(id) + " already exists");
    }

    if (database_name_to_id_.contains(name)) {
        return make_error(StatusCode::ALREADY_EXISTS, "database '" + name + "' already exists");
    }

    databases_by_id_[id] = Database{id, name};
    database_name_to_id_[name] = id;

    // Advance auto-increment counter past this ID.
    if (id >= next_database_id_) {
        next_database_id_ = id + 1;
    }

    return ok();
}

Result<void> Catalog::restore_table(database_id_t database_id, TableSchema schema) {
    std::lock_guard lock(mu_);

    if (!databases_by_id_.contains(database_id)) {
        return make_error(StatusCode::NOT_FOUND,
                          "database with id " + std::to_string(database_id) + " not found");
    }

    auto& name_map = table_name_to_id_[database_id];
    if (name_map.contains(schema.name)) {
        return make_error(StatusCode::ALREADY_EXISTS, "table '" + schema.name + "' already exists");
    }

    table_id_t id = schema.table_id;
    name_map[schema.name] = id;
    table_to_database_[id] = database_id;
    tables_by_id_[id] = std::move(schema);

    // Advance auto-increment counter past this ID.
    if (id >= next_table_id_) {
        next_table_id_ = id + 1;
    }

    return ok();
}

Result<void> Catalog::restore_index(IndexDef def) {
    std::lock_guard lock(mu_);

    auto db_id = table_to_database_.contains(def.table_id) ? table_to_database_[def.table_id]
                                                           : default_database_id;
    auto& db_names = index_name_to_id_[db_id];
    if (db_names.contains(def.name)) {
        return make_error(StatusCode::ALREADY_EXISTS, "index '" + def.name + "' already exists");
    }

    index_id_t id = def.index_id;
    db_names[def.name] = id;
    indexes_by_id_[id] = std::move(def);

    if (id >= next_index_id_) {
        next_index_id_ = id + 1;
    }

    return ok();
}

Result<void> Catalog::restore_edge_type(database_id_t database_id, EdgeTypeDef def) {
    std::lock_guard lock(mu_);

    auto& name_map = edge_name_to_id_[database_id];
    if (name_map.contains(def.name)) {
        return make_error(StatusCode::ALREADY_EXISTS,
                          "edge type '" + def.name + "' already exists");
    }

    edge_id_t id = def.edge_id;
    def.database_id = database_id;
    name_map[def.name] = id;
    edge_types_by_id_[id] = std::move(def);

    if (id >= next_edge_id_) {
        next_edge_id_ = id + 1;
    }

    return ok();
}

void Catalog::restore_embedding_column(EmbeddingColumnDef def) {
    std::lock_guard lock(mu_);
    embedding_columns_.push_back(std::move(def));
}

void Catalog::set_next_table_id(table_id_t id) {
    std::lock_guard lock(mu_);
    next_table_id_ = id;
}

void Catalog::set_next_index_id(index_id_t id) {
    std::lock_guard lock(mu_);
    next_index_id_ = id;
}

void Catalog::set_next_edge_id(edge_id_t id) {
    std::lock_guard lock(mu_);
    next_edge_id_ = id;
}

table_id_t Catalog::next_table_id() const {
    std::lock_guard lock(mu_);
    return next_table_id_;
}

// -- Auto-increment counter operations ----------------------------------------

void Catalog::init_autoincrement(table_id_t table_id, int64_t start_value) {
    std::lock_guard lock(mu_);
    autoincrement_counters_[table_id] = start_value;
}

Result<int64_t> Catalog::next_autoincrement(table_id_t table_id, TypeId type_id) {
    std::lock_guard lock(mu_);
    auto it = autoincrement_counters_.find(table_id);
    if (it == autoincrement_counters_.end()) {
        return make_error(StatusCode::INTERNAL_ERROR,
                          "no autoincrement counter for table " + std::to_string(table_id));
    }

    int64_t value = it->second;

    // Check overflow based on the column's integer type.
    switch (type_id) {
    case TypeId::INT8:
        if (value > std::numeric_limits<int8_t>::max()) {
            return make_error(StatusCode::CONSTRAINT_VIOLATION,
                              "AUTOINCREMENT overflow: value " + std::to_string(value) +
                                  " exceeds INT8 max (" +
                                  std::to_string(std::numeric_limits<int8_t>::max()) + ")");
        }
        break;
    case TypeId::INT16:
        if (value > std::numeric_limits<int16_t>::max()) {
            return make_error(StatusCode::CONSTRAINT_VIOLATION,
                              "AUTOINCREMENT overflow: value " + std::to_string(value) +
                                  " exceeds INT16 max (" +
                                  std::to_string(std::numeric_limits<int16_t>::max()) + ")");
        }
        break;
    case TypeId::INT32:
        if (value > std::numeric_limits<int32_t>::max()) {
            return make_error(StatusCode::CONSTRAINT_VIOLATION,
                              "AUTOINCREMENT overflow: value " + std::to_string(value) +
                                  " exceeds INT32 max (" +
                                  std::to_string(std::numeric_limits<int32_t>::max()) + ")");
        }
        break;
    case TypeId::INT64:
        if (value == std::numeric_limits<int64_t>::max()) {
            return make_error(StatusCode::CONSTRAINT_VIOLATION,
                              "AUTOINCREMENT overflow: INT64 counter exhausted");
        }
        break;
    case TypeId::UINT8:
        if (value > std::numeric_limits<uint8_t>::max()) {
            return make_error(StatusCode::CONSTRAINT_VIOLATION,
                              "AUTOINCREMENT overflow: value " + std::to_string(value) +
                                  " exceeds UINT8 max (" +
                                  std::to_string(std::numeric_limits<uint8_t>::max()) + ")");
        }
        break;
    case TypeId::UINT16:
        if (value > std::numeric_limits<uint16_t>::max()) {
            return make_error(StatusCode::CONSTRAINT_VIOLATION,
                              "AUTOINCREMENT overflow: value " + std::to_string(value) +
                                  " exceeds UINT16 max (" +
                                  std::to_string(std::numeric_limits<uint16_t>::max()) + ")");
        }
        break;
    case TypeId::UINT32:
        if (value > std::numeric_limits<uint32_t>::max()) {
            return make_error(StatusCode::CONSTRAINT_VIOLATION,
                              "AUTOINCREMENT overflow: value " + std::to_string(value) +
                                  " exceeds UINT32 max (" +
                                  std::to_string(std::numeric_limits<uint32_t>::max()) + ")");
        }
        break;
    case TypeId::UINT64:
        // int64_t can't represent the full UINT64 range, but for practical purposes
        // we treat the counter as a signed int64 value (max ~9.2 quintillion).
        if (value == std::numeric_limits<int64_t>::max()) {
            return make_error(StatusCode::CONSTRAINT_VIOLATION,
                              "AUTOINCREMENT overflow: UINT64 counter exhausted");
        }
        break;
    default:
        return make_error(StatusCode::TYPE_ERROR, "AUTOINCREMENT requires an integer type");
    }

    it->second = value + 1;
    return ok(value);
}

void Catalog::advance_autoincrement(table_id_t table_id, int64_t value) {
    std::lock_guard lock(mu_);
    auto it = autoincrement_counters_.find(table_id);
    if (it != autoincrement_counters_.end() && value >= it->second) {
        if (value >= std::numeric_limits<int64_t>::max()) {
            it->second = std::numeric_limits<int64_t>::max();
        } else {
            it->second = value + 1;
        }
    }
}

int64_t Catalog::get_autoincrement_counter(table_id_t table_id) const {
    std::lock_guard lock(mu_);
    auto it = autoincrement_counters_.find(table_id);
    if (it == autoincrement_counters_.end()) {
        return 0;
    }
    return it->second;
}

// ===========================================================================
// Virtual catalog table operations
// ===========================================================================

void Catalog::register_virtual_table(VirtualTableDef def) {
    std::lock_guard lock(mu_);
    if (def.table_id == 0) {
        def.table_id = next_virtual_table_id_--;
    }
    virtual_tables_.emplace(def.name, std::move(def));
}

Result<VirtualTableDef> Catalog::get_virtual_table(const std::string& name) const {
    std::lock_guard lock(mu_);
    auto it = virtual_tables_.find(name);
    if (it == virtual_tables_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "virtual table 'pg_catalog." + name + "' does not exist");
    }
    return it->second;
}

std::vector<VirtualTableDef> Catalog::list_virtual_tables() const {
    std::lock_guard lock(mu_);
    std::vector<VirtualTableDef> result;
    result.reserve(virtual_tables_.size());
    for (const auto& [_, def] : virtual_tables_) {
        result.push_back(def);
    }
    return result;
}

bool Catalog::is_virtual_schema(const std::string& schema_name) {
    return schema_name == pg_catalog_schema;
}

bool Catalog::is_virtual_table(table_id_t id) const {
    std::lock_guard lock(mu_);
    for (const auto& [_, def] : virtual_tables_) {
        if (def.table_id == id) {
            return true;
        }
    }
    return false;
}

} // namespace sixseven
