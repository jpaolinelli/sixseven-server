#include "giodb/catalog/catalog.h"

#include <algorithm>

namespace giodb {

// -- Constructor --------------------------------------------------------------

Catalog::Catalog() {
    // Create the default 'giodb' database.
    databases_by_id_[default_database_id] = Database{default_database_id, "giodb"};
    database_name_to_id_["giodb"] = default_database_id;

    // Create the system 'giodb_system' database.
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

    if (database_id == default_database_id) {
        return make_error(StatusCode::CONSTRAINT_VIOLATION,
                          "cannot drop the default 'giodb' database");
    }

    if (database_id == system_database_id) {
        return make_error(StatusCode::CONSTRAINT_VIOLATION, "cannot drop the system database");
    }

    auto db_it = databases_by_id_.find(database_id);
    if (db_it == databases_by_id_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "database with id " + std::to_string(database_id) + " not found");
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

    // Remove the database.
    database_name_to_id_.erase(db_it->second.name);
    databases_by_id_.erase(db_it);
    table_name_to_id_.erase(database_id);

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
    // Protect system tables in the giodb_system database.
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
        auto idx_name_it = index_name_to_id_.find(idx_name);
        if (idx_name_it != index_name_to_id_.end()) {
            indexes_by_id_.erase(idx_name_it->second);
            index_name_to_id_.erase(idx_name_it);
        }
    }

    // Remove all edge types referencing this table (source or target).
    std::vector<std::string> edge_names_to_remove;
    for (auto& [eid, edef] : edge_types_by_id_) {
        if (edef.source_table_id == tid || edef.target_table_id == tid) {
            edge_names_to_remove.push_back(edef.name);
        }
    }
    for (auto& ename : edge_names_to_remove) {
        auto ename_it = edge_name_to_id_.find(ename);
        if (ename_it != edge_name_to_id_.end()) {
            edge_types_by_id_.erase(ename_it->second);
            edge_name_to_id_.erase(ename_it);
        }
    }

    // Remove all embedding column definitions for this table.
    std::erase_if(embedding_columns_,
                  [tid](const EmbeddingColumnDef& e) { return e.table_id == tid; });

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

// -- Index operations ---------------------------------------------------------

Result<index_id_t> Catalog::create_index(IndexDef def) {
    std::lock_guard lock(mu_);

    if (index_name_to_id_.contains(def.name)) {
        return make_error(StatusCode::ALREADY_EXISTS, "index '" + def.name + "' already exists");
    }

    // Validate that the referenced table exists.
    if (!tables_by_id_.contains(def.table_id)) {
        return make_error(StatusCode::NOT_FOUND,
                          "table with id " + std::to_string(def.table_id) + " not found");
    }

    index_id_t id = next_index_id_++;
    def.index_id = id;

    index_name_to_id_[def.name] = id;
    indexes_by_id_[id] = std::move(def);

    return ok(id);
}

Result<void> Catalog::drop_index(const std::string& name) {
    std::lock_guard lock(mu_);

    auto name_it = index_name_to_id_.find(name);
    if (name_it == index_name_to_id_.end()) {
        return make_error(StatusCode::NOT_FOUND, "index '" + name + "' not found");
    }

    indexes_by_id_.erase(name_it->second);
    index_name_to_id_.erase(name_it);

    return ok();
}

Result<IndexDef> Catalog::get_index(const std::string& name) const {
    std::lock_guard lock(mu_);

    auto name_it = index_name_to_id_.find(name);
    if (name_it == index_name_to_id_.end()) {
        return make_error(StatusCode::NOT_FOUND, "index '" + name + "' not found");
    }

    return ok(indexes_by_id_.at(name_it->second));
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

Result<edge_id_t> Catalog::create_edge_type(EdgeTypeDef def) {
    std::lock_guard lock(mu_);

    if (edge_name_to_id_.contains(def.name)) {
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

    edge_name_to_id_[def.name] = id;
    edge_types_by_id_[id] = std::move(def);

    return ok(id);
}

Result<void> Catalog::drop_edge_type(const std::string& name) {
    std::lock_guard lock(mu_);

    auto name_it = edge_name_to_id_.find(name);
    if (name_it == edge_name_to_id_.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + name + "' not found");
    }

    edge_types_by_id_.erase(name_it->second);
    edge_name_to_id_.erase(name_it);

    return ok();
}

Result<EdgeTypeDef> Catalog::get_edge_type(const std::string& name) const {
    std::lock_guard lock(mu_);

    auto name_it = edge_name_to_id_.find(name);
    if (name_it == edge_name_to_id_.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + name + "' not found");
    }

    return ok(edge_types_by_id_.at(name_it->second));
}

std::vector<EdgeTypeDef> Catalog::list_edge_types() const {
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

    if (index_name_to_id_.contains(def.name)) {
        return make_error(StatusCode::ALREADY_EXISTS, "index '" + def.name + "' already exists");
    }

    index_id_t id = def.index_id;
    index_name_to_id_[def.name] = id;
    indexes_by_id_[id] = std::move(def);

    if (id >= next_index_id_) {
        next_index_id_ = id + 1;
    }

    return ok();
}

Result<void> Catalog::restore_edge_type(EdgeTypeDef def) {
    std::lock_guard lock(mu_);

    if (edge_name_to_id_.contains(def.name)) {
        return make_error(StatusCode::ALREADY_EXISTS,
                          "edge type '" + def.name + "' already exists");
    }

    edge_id_t id = def.edge_id;
    edge_name_to_id_[def.name] = id;
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

} // namespace giodb
