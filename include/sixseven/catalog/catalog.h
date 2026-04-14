#pragma once

#include "sixseven/catalog/schema.h"
#include "sixseven/common/result.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sixseven {

/// System catalog that stores and manages schema metadata.
///
/// Supports multiple databases. Tables are scoped per database so that
/// the same table name may exist in different databases.
///
/// Only the system database "sixseven_system" is created on construction.
/// The default "sixseven" database is created during bootstrap and persisted.
///
/// Thread safety: All public methods are protected by a mutex.
///
/// Usage:
/// ```
///   Catalog catalog;
///   auto db_id = default_database_id;
///   TableSchema schema;
///   schema.name = "users";
///   schema.columns = { ... };
///   auto id = catalog.create_table(db_id, schema).value();
///   auto retrieved = catalog.get_table(db_id, "users").value();
///   catalog.drop_table(db_id, "users");
/// ```
class Catalog {
public:
    Catalog();

    // -- Database operations --------------------------------------------------

    /// Create a new database. Assigns a sequential database_id.
    /// Fails with ALREADY_EXISTS if a database with the same name exists.
    [[nodiscard]] Result<database_id_t> create_database(const std::string& name);

    /// Drop a database by id.
    /// If cascade is false, fails with CONSTRAINT_VIOLATION if the database
    /// contains tables. If cascade is true, drops all tables first.
    /// Cannot drop the 'sixseven_system' database.
    [[nodiscard]] Result<void> drop_database(database_id_t database_id, bool cascade);

    /// Retrieve a database by name.
    /// Fails with NOT_FOUND if the database does not exist.
    [[nodiscard]] Result<Database> get_database(const std::string& name) const;

    /// List all databases, sorted by database_id.
    [[nodiscard]] std::vector<Database> list_databases() const;

    // -- Table operations -----------------------------------------------------

    /// Create a new table in the given database. Assigns a sequential table_id.
    /// Fails with ALREADY_EXISTS if a table with the same name exists in
    /// that database.
    [[nodiscard]] Result<table_id_t> create_table(database_id_t database_id, TableSchema schema);

    /// Drop a table by name within a database. Also removes all associated
    /// indexes, edge types, and embedding columns.
    /// Fails with NOT_FOUND if the table does not exist.
    [[nodiscard]] Result<void> drop_table(database_id_t database_id, const std::string& name);

    /// Retrieve a table schema by name within a database.
    /// Fails with NOT_FOUND if the table does not exist.
    [[nodiscard]] Result<TableSchema> get_table(database_id_t database_id,
                                                const std::string& name) const;

    /// Retrieve a table schema by table_id (global lookup, not scoped by db).
    /// Fails with NOT_FOUND if the table does not exist.
    [[nodiscard]] Result<TableSchema> get_table_by_id(table_id_t id) const;

    /// Get the database_id that owns a table. Returns 0 if not found.
    [[nodiscard]] database_id_t get_table_database_id(table_id_t id) const;

    /// List all table schemas in the given database.
    [[nodiscard]] std::vector<TableSchema> list_tables(database_id_t database_id) const;

    /// Add a column to an existing table. Assigns the next ordinal.
    /// Fails with NOT_FOUND if the table does not exist.
    /// Fails with ALREADY_EXISTS if a column with the same name exists.
    [[nodiscard]] Result<void> add_column(table_id_t table_id, CatalogColumnDef column);

    /// Drop a column from an existing table by name. Renumbers ordinals.
    /// Fails with NOT_FOUND if the table or column does not exist.
    [[nodiscard]] Result<void> drop_column(table_id_t table_id, const std::string& column_name);

    /// Rename a column in an existing table.
    /// Fails with NOT_FOUND if the table or column does not exist.
    /// Fails with ALREADY_EXISTS if the new name conflicts.
    [[nodiscard]] Result<void>
    rename_column(table_id_t table_id, const std::string& old_name, const std::string& new_name);

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
    /// Fails with ALREADY_EXISTS if an edge type with the same name exists in the database.
    /// Fails with NOT_FOUND if source or target table does not exist.
    [[nodiscard]] Result<edge_id_t> create_edge_type(database_id_t database_id, EdgeTypeDef def);

    /// Drop an edge type by name within a database.
    /// Fails with NOT_FOUND if the edge type does not exist.
    [[nodiscard]] Result<void> drop_edge_type(database_id_t database_id, const std::string& name);

    /// Retrieve an edge type by name within a database.
    [[nodiscard]] Result<EdgeTypeDef> get_edge_type(database_id_t database_id,
                                                    const std::string& name) const;

    /// List all edge types in the given database.
    [[nodiscard]] std::vector<EdgeTypeDef> list_edge_types(database_id_t database_id) const;

    /// List all edge types across all databases.
    [[nodiscard]] std::vector<EdgeTypeDef> list_all_edge_types() const;

    // -- Embedding column operations ------------------------------------------

    /// Register an embedding column configuration.
    /// Validates that the referenced table exists.
    /// Fails with NOT_FOUND if the table does not exist.
    /// Fails with ALREADY_EXISTS if an embedding is already registered
    /// for (table_id, column_id).
    [[nodiscard]] Result<void> register_embedding_column(EmbeddingColumnDef def);

    /// List all embedding column definitions for a given table.
    [[nodiscard]] std::vector<EmbeddingColumnDef> list_embedding_columns(table_id_t table_id) const;

    /// List all embedding column definitions.
    [[nodiscard]] std::vector<EmbeddingColumnDef> list_all_embedding_columns() const;

    // -- Embedding provider operations ----------------------------------------

    /// Register an embedding provider configuration.
    /// Fails with ALREADY_EXISTS if a provider with the same name exists.
    [[nodiscard]] Result<void> register_embedding_provider(EmbeddingProviderConfig config);

    /// Retrieve an embedding provider by name.
    /// Fails with NOT_FOUND if the provider does not exist.
    [[nodiscard]] Result<EmbeddingProviderConfig>
    get_embedding_provider(const std::string& name) const;

    /// List all registered embedding providers.
    [[nodiscard]] std::vector<EmbeddingProviderConfig> list_embedding_providers() const;

    /// Remove an embedding provider by name.
    /// Fails with NOT_FOUND if the provider does not exist.
    [[nodiscard]] Result<void> remove_embedding_provider(const std::string& name);

    // -- Auto-increment counter operations ------------------------------------

    /// Initialize the auto-increment counter for a table. Called on CREATE TABLE.
    void init_autoincrement(table_id_t table_id, int64_t start_value = 1);

    /// Get the next auto-increment value for a table and advance the counter.
    /// Checks overflow for the given column type.
    [[nodiscard]] Result<int64_t> next_autoincrement(table_id_t table_id, TypeId type_id);

    /// Advance the auto-increment counter past an explicit value.
    /// Used when the user provides an explicit value in INSERT.
    void advance_autoincrement(table_id_t table_id, int64_t value);

    /// Get the current auto-increment counter value for a table.
    /// Returns 0 if no counter is set.
    [[nodiscard]] int64_t get_autoincrement_counter(table_id_t table_id) const;

    // -- Persistence restore operations ----------------------------------------

    /// Restore a database with a pre-assigned database_id (for catalog persistence).
    /// Unlike create_database(), does NOT auto-assign a database_id.
    /// Advances next_database_id_ past the restored ID.
    [[nodiscard]] Result<void> restore_database(database_id_t id, const std::string& name);

    /// Restore a table with a pre-assigned table_id (for catalog persistence).
    /// Unlike create_table(), does NOT auto-assign a table_id.
    [[nodiscard]] Result<void> restore_table(database_id_t database_id, TableSchema schema);

    /// Restore an index with a pre-assigned index_id (for catalog persistence).
    [[nodiscard]] Result<void> restore_index(IndexDef def);

    /// Restore an edge type with a pre-assigned edge_id (for catalog persistence).
    [[nodiscard]] Result<void> restore_edge_type(database_id_t database_id, EdgeTypeDef def);

    /// Restore an embedding column (for catalog persistence).
    /// Skips table existence validation since tables may not be fully loaded yet.
    void restore_embedding_column(EmbeddingColumnDef def);

    /// Set the next auto-increment table ID. Used after loading persisted tables.
    void set_next_table_id(table_id_t id);

    /// Set the next auto-increment index ID. Used after loading persisted indexes.
    void set_next_index_id(index_id_t id);

    /// Set the next auto-increment edge ID. Used after loading persisted edge types.
    void set_next_edge_id(edge_id_t id);

    /// Get the current next_table_id value.
    [[nodiscard]] table_id_t next_table_id() const;

private:
    /// Drop a table by name (caller must hold mu_). Used internally by
    /// drop_table() and drop_database() with cascade.
    Result<void> drop_table_locked(database_id_t database_id, const std::string& name);

    mutable std::mutex mu_;

    /// Next auto-increment IDs. Starts after system_database_id (2).
    database_id_t next_database_id_ = system_database_id + 1;
    table_id_t next_table_id_ = 1;
    index_id_t next_index_id_ = 1;
    edge_id_t next_edge_id_ = 1;

    /// Primary storage: database_id -> Database.
    std::unordered_map<database_id_t, Database> databases_by_id_;

    /// Name lookup: database name -> database_id.
    std::unordered_map<std::string, database_id_t> database_name_to_id_;

    /// Primary storage: table_id -> TableSchema.
    std::unordered_map<table_id_t, TableSchema> tables_by_id_;

    /// Name lookup scoped per database: database_id -> (table name -> table_id).
    std::unordered_map<database_id_t, std::unordered_map<std::string, table_id_t>>
        table_name_to_id_;

    /// Reverse lookup: table_id -> database_id (for get_table_by_id).
    std::unordered_map<table_id_t, database_id_t> table_to_database_;

    /// Primary storage: index_id -> IndexDef.
    std::unordered_map<index_id_t, IndexDef> indexes_by_id_;

    /// Name lookup scoped per database: database_id -> (index name -> index_id).
    std::unordered_map<database_id_t, std::unordered_map<std::string, index_id_t>> index_name_to_id_;

    /// Primary storage: edge_id -> EdgeTypeDef.
    std::unordered_map<edge_id_t, EdgeTypeDef> edge_types_by_id_;

    /// Name lookup scoped per database: database_id -> (edge type name -> edge_id).
    std::unordered_map<database_id_t, std::unordered_map<std::string, edge_id_t>> edge_name_to_id_;

    /// Embedding column definitions keyed by (table_id, column_id).
    /// We use a vector and linear scan since the number of embedding
    /// columns is expected to be small.
    std::vector<EmbeddingColumnDef> embedding_columns_;

    /// Embedding provider configurations keyed by name.
    std::unordered_map<std::string, EmbeddingProviderConfig> embedding_providers_;

    /// Per-table auto-increment counters. Stores the next value to assign.
    std::unordered_map<table_id_t, int64_t> autoincrement_counters_;
};

} // namespace sixseven
