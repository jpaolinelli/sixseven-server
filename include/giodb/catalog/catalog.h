#pragma once

#include "giodb/catalog/schema.h"
#include "giodb/common/result.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace giodb {

/// System catalog that stores and manages schema metadata.
///
/// Maintains in-memory maps for fast table/index lookups, keyed by name.
/// All DDL operations (create/drop) invalidate the relevant cache entries.
///
/// Thread safety: All public methods are protected by a mutex.
///
/// Usage:
/// ```
///   Catalog catalog;
///   TableSchema schema;
///   schema.name = "users";
///   schema.columns = { ... };
///   auto id = catalog.create_table(schema).value();
///   auto retrieved = catalog.get_table("users").value();
///   catalog.drop_table("users");
/// ```
class Catalog {
public:
    Catalog() = default;

    // -- Table operations -----------------------------------------------------

    /// Create a new table. Assigns a sequential table_id.
    /// Fails with ALREADY_EXISTS if a table with the same name exists.
    [[nodiscard]] Result<table_id_t> create_table(TableSchema schema);

    /// Drop a table by name. Also removes all associated indexes.
    /// Fails with NOT_FOUND if the table does not exist.
    [[nodiscard]] Result<void> drop_table(const std::string& name);

    /// Retrieve a table schema by name.
    /// Fails with NOT_FOUND if the table does not exist.
    [[nodiscard]] Result<TableSchema> get_table(const std::string& name) const;

    /// Retrieve a table schema by table_id.
    /// Fails with NOT_FOUND if the table does not exist.
    [[nodiscard]] Result<TableSchema> get_table_by_id(table_id_t id) const;

    /// List all table schemas.
    [[nodiscard]] std::vector<TableSchema> list_tables() const;

    // -- Index operations -----------------------------------------------------

    /// Create a new index. Assigns a sequential index_id.
    /// Fails with ALREADY_EXISTS if an index with the same name exists.
    /// Fails with NOT_FOUND if the referenced table does not exist.
    [[nodiscard]] Result<index_id_t> create_index(IndexDef def);

    /// Drop an index by name.
    /// Fails with NOT_FOUND if the index does not exist.
    [[nodiscard]] Result<void> drop_index(const std::string& name);

    /// Retrieve an index definition by name.
    [[nodiscard]] Result<IndexDef> get_index(const std::string& name) const;

    /// List all indexes for a given table.
    [[nodiscard]] std::vector<IndexDef> list_indexes(table_id_t table_id) const;

    /// List all indexes in the catalog.
    [[nodiscard]] std::vector<IndexDef> list_all_indexes() const;

    // -- Edge type operations -------------------------------------------------

    /// Create a new edge type. Assigns a sequential edge_id.
    /// Validates that source and target tables exist.
    /// Fails with ALREADY_EXISTS if an edge type with the same name exists.
    /// Fails with NOT_FOUND if source or target table does not exist.
    [[nodiscard]] Result<edge_id_t> create_edge_type(EdgeTypeDef def);

    /// Drop an edge type by name.
    /// Fails with NOT_FOUND if the edge type does not exist.
    [[nodiscard]] Result<void> drop_edge_type(const std::string& name);

    /// Retrieve an edge type by name.
    [[nodiscard]] Result<EdgeTypeDef> get_edge_type(const std::string& name) const;

    /// List all edge types.
    [[nodiscard]] std::vector<EdgeTypeDef> list_edge_types() const;

    // -- Embedding column operations ------------------------------------------

    /// Register an embedding column configuration.
    /// Validates that the referenced table exists.
    /// Fails with NOT_FOUND if the table does not exist.
    /// Fails with ALREADY_EXISTS if an embedding is already registered
    /// for (table_id, column_id).
    [[nodiscard]] Result<void> register_embedding_column(EmbeddingColumnDef def);

    /// List all embedding column definitions for a given table.
    [[nodiscard]] std::vector<EmbeddingColumnDef>
    list_embedding_columns(table_id_t table_id) const;

    /// List all embedding column definitions.
    [[nodiscard]] std::vector<EmbeddingColumnDef> list_all_embedding_columns() const;

private:
    mutable std::mutex mu_;

    /// Next auto-increment IDs.
    table_id_t next_table_id_ = 1;
    index_id_t next_index_id_ = 1;
    edge_id_t next_edge_id_ = 1;

    /// Primary storage: table_id -> TableSchema.
    std::unordered_map<table_id_t, TableSchema> tables_by_id_;

    /// Name lookup: table name -> table_id.
    std::unordered_map<std::string, table_id_t> table_name_to_id_;

    /// Primary storage: index_id -> IndexDef.
    std::unordered_map<index_id_t, IndexDef> indexes_by_id_;

    /// Name lookup: index name -> index_id.
    std::unordered_map<std::string, index_id_t> index_name_to_id_;

    /// Primary storage: edge_id -> EdgeTypeDef.
    std::unordered_map<edge_id_t, EdgeTypeDef> edge_types_by_id_;

    /// Name lookup: edge type name -> edge_id.
    std::unordered_map<std::string, edge_id_t> edge_name_to_id_;

    /// Embedding column definitions keyed by (table_id, column_id).
    /// We use a vector and linear scan since the number of embedding
    /// columns is expected to be small.
    std::vector<EmbeddingColumnDef> embedding_columns_;
};

} // namespace giodb
