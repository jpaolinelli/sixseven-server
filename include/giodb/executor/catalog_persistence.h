#pragma once

#include "giodb/catalog/catalog.h"
#include "giodb/catalog/schema.h"
#include "giodb/common/result.h"
#include "giodb/executor/storage_manager.h"

namespace giodb {

/// Persists and restores catalog metadata using system heap tables.
///
/// The catalog is in-memory and lost on restart. CatalogPersistence bridges
/// this by storing catalog metadata (tables, columns, indexes, edge types,
/// embedding columns) in system tables within the giodb_system database.
///
/// System tables used:
///   - sys_tables: table_id, database_id, name, pk_columns
///   - sys_columns: table_id, ordinal, name, type_id, nullable, default_expr
///   - sys_indexes: index_id, table_id, name, index_type, columns, is_unique
///   - sys_edge_types: edge_id, name, source_table_id, target_table_id, properties
///   - sys_embedding_columns: table_id, column_id, dimension, source_expr, provider
///
/// On first run, create_system_catalog_tables() creates these tables.
/// On subsequent runs, load_catalog() reads them and populates the Catalog.
/// DDL operations call persist_*/remove_* to keep disk in sync.
class CatalogPersistence {
public:
    CatalogPersistence(Catalog& catalog, StorageManager& storage);

    /// Create the 5 system catalog tables (first run only).
    /// Registers schemas in the Catalog and creates physical storage.
    [[nodiscard]] Result<void> create_system_catalog_tables();

    /// Open existing system catalog tables and load all metadata into Catalog.
    /// Also opens user table storage files discovered in sys_tables.
    [[nodiscard]] Result<void> load_catalog();

    /// Persist a newly created table (insert into sys_tables + sys_columns).
    [[nodiscard]] Result<void> persist_table(database_id_t db_id, const TableSchema& schema);

    /// Remove a table from persistence (delete from sys_tables + sys_columns).
    [[nodiscard]] Result<void> remove_table(table_id_t table_id);

    /// Persist a newly created index (insert into sys_indexes).
    [[nodiscard]] Result<void> persist_index(const IndexDef& def);

    /// Remove an index from persistence (delete from sys_indexes).
    [[nodiscard]] Result<void> remove_index(index_id_t index_id);

    /// Persist a newly created edge type (insert into sys_edge_types).
    [[nodiscard]] Result<void> persist_edge_type(const EdgeTypeDef& def);

    /// Remove an edge type from persistence (delete from sys_edge_types).
    [[nodiscard]] Result<void> remove_edge_type(edge_id_t edge_id);

    /// Persist a newly registered embedding column (insert into sys_embedding_columns).
    [[nodiscard]] Result<void> persist_embedding_column(const EmbeddingColumnDef& def);

    /// Register a system table schema in the catalog and create its physical storage.
    /// Used by SystemBootstrap for sys_settings/sys_providers in addition to catalog tables.
    [[nodiscard]] Result<void> create_sys_table_public(const TableSchema& schema);

    /// Register a system table schema in the catalog and open its existing storage.
    /// Used by SystemBootstrap for sys_settings/sys_providers on subsequent runs.
    [[nodiscard]] Result<void> open_sys_table_public(const TableSchema& schema);

private:
    /// Register a system table schema in the catalog and create its physical storage.
    [[nodiscard]] Result<void> create_sys_table(const TableSchema& schema);

    /// Register a system table schema in the catalog and open its existing storage.
    [[nodiscard]] Result<void> open_sys_table(const TableSchema& schema);

    /// Insert a row into a system table heap.
    [[nodiscard]] Result<void> insert_row(table_id_t sys_table_id,
                                          const std::vector<Value>& values);

    /// Delete all rows matching a predicate from a system table.
    /// predicate(values) returns true for rows to delete.
    template <typename Pred>
    [[nodiscard]] Result<void> delete_rows(table_id_t sys_table_id, Pred predicate);

    Catalog& catalog_;
    StorageManager& storage_;
};

} // namespace giodb
