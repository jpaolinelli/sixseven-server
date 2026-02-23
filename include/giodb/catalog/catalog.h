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
/// Supports multiple databases. Tables are scoped per database so that
/// the same table name may exist in different databases.
///
/// A default database named "giodb" (id = default_database_id) is created
/// on construction.
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
    /// Cannot drop the default 'giodb' database or the 'giodb_system' database.
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

    /// List all table schemas in the given database.
    [[nodiscard]] std::vector<TableSchema> list_tables(database_id_t database_id) const;

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

    /// Embedding provider configurations keyed by name.
    std::unordered_map<std::string, EmbeddingProviderConfig> embedding_providers_;
};

} // namespace giodb
