#include "giodb/catalog/catalog.h"

#include <algorithm>

namespace giodb {

// -- Table operations ---------------------------------------------------------

Result<table_id_t> Catalog::create_table(TableSchema schema) {
    std::lock_guard lock(mu_);

    if (table_name_to_id_.contains(schema.name)) {
        return make_error(StatusCode::ALREADY_EXISTS,
                          "table '" + schema.name + "' already exists");
    }

    table_id_t id = next_table_id_++;
    schema.table_id = id;

    // Assign ordinals to columns if not already set.
    for (int32_t i = 0; i < static_cast<int32_t>(schema.columns.size()); ++i) {
        schema.columns[static_cast<size_t>(i)].ordinal = i;
    }

    table_name_to_id_[schema.name] = id;
    tables_by_id_[id] = std::move(schema);

    return ok(id);
}

Result<void> Catalog::drop_table(const std::string& name) {
    std::lock_guard lock(mu_);

    auto name_it = table_name_to_id_.find(name);
    if (name_it == table_name_to_id_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "table '" + name + "' not found");
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
    table_name_to_id_.erase(name_it);

    return ok();
}

Result<TableSchema> Catalog::get_table(const std::string& name) const {
    std::lock_guard lock(mu_);

    auto name_it = table_name_to_id_.find(name);
    if (name_it == table_name_to_id_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "table '" + name + "' not found");
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

std::vector<TableSchema> Catalog::list_tables() const {
    std::lock_guard lock(mu_);

    std::vector<TableSchema> result;
    result.reserve(tables_by_id_.size());
    for (auto& [id, schema] : tables_by_id_) {
        result.push_back(schema);
    }

    // Sort by table_id for deterministic output.
    std::sort(result.begin(), result.end(),
              [](const TableSchema& a, const TableSchema& b) {
                  return a.table_id < b.table_id;
              });

    return result;
}

// -- Index operations ---------------------------------------------------------

Result<index_id_t> Catalog::create_index(IndexDef def) {
    std::lock_guard lock(mu_);

    if (index_name_to_id_.contains(def.name)) {
        return make_error(StatusCode::ALREADY_EXISTS,
                          "index '" + def.name + "' already exists");
    }

    // Validate that the referenced table exists.
    if (!tables_by_id_.contains(def.table_id)) {
        return make_error(StatusCode::NOT_FOUND,
                          "table with id " + std::to_string(def.table_id) +
                              " not found");
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
        return make_error(StatusCode::NOT_FOUND,
                          "index '" + name + "' not found");
    }

    indexes_by_id_.erase(name_it->second);
    index_name_to_id_.erase(name_it);

    return ok();
}

Result<IndexDef> Catalog::get_index(const std::string& name) const {
    std::lock_guard lock(mu_);

    auto name_it = index_name_to_id_.find(name);
    if (name_it == index_name_to_id_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "index '" + name + "' not found");
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

    std::sort(result.begin(), result.end(),
              [](const IndexDef& a, const IndexDef& b) {
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

    std::sort(result.begin(), result.end(),
              [](const IndexDef& a, const IndexDef& b) {
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
                          "source table with id " +
                              std::to_string(def.source_table_id) + " not found");
    }

    // Validate target table exists.
    if (!tables_by_id_.contains(def.target_table_id)) {
        return make_error(StatusCode::NOT_FOUND,
                          "target table with id " +
                              std::to_string(def.target_table_id) + " not found");
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
        return make_error(StatusCode::NOT_FOUND,
                          "edge type '" + name + "' not found");
    }

    edge_types_by_id_.erase(name_it->second);
    edge_name_to_id_.erase(name_it);

    return ok();
}

Result<EdgeTypeDef> Catalog::get_edge_type(const std::string& name) const {
    std::lock_guard lock(mu_);

    auto name_it = edge_name_to_id_.find(name);
    if (name_it == edge_name_to_id_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "edge type '" + name + "' not found");
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

    std::sort(result.begin(), result.end(),
              [](const EdgeTypeDef& a, const EdgeTypeDef& b) {
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
                          "table with id " + std::to_string(def.table_id) +
                              " not found");
    }

    // Check for duplicate (table_id, column_id) pair.
    for (auto& existing : embedding_columns_) {
        if (existing.table_id == def.table_id &&
            existing.column_id == def.column_id) {
            return make_error(
                StatusCode::ALREADY_EXISTS,
                "embedding column already registered for table " +
                    std::to_string(def.table_id) + ", column " +
                    std::to_string(def.column_id));
        }
    }

    embedding_columns_.push_back(std::move(def));
    return ok();
}

std::vector<EmbeddingColumnDef>
Catalog::list_embedding_columns(table_id_t table_id) const {
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

} // namespace giodb
