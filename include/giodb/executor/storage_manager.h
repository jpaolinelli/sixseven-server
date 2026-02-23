#pragma once

#include "giodb/catalog/schema.h"
#include "giodb/common/result.h"
#include "giodb/storage/buffer_pool.h"
#include "giodb/storage/disk_manager.h"
#include "giodb/table/table_heap.h"
#include "giodb/table/tuple.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace giodb {

/// Physical storage state for a single table.
struct TableStorage {
    FileId file_id = 0;
    std::unique_ptr<BufferPoolManager> bpm;
    std::unique_ptr<TableHeap> heap;
    Schema storage_schema; ///< Byte-level schema for TupleSerializer.
};

/// Manages the physical storage backing for catalog tables.
///
/// Bridges the gap between the Catalog (metadata-only) and the executor
/// (which needs TableHeap references). Each table gets its own database
/// file, BufferPoolManager, and TableHeap.
///
/// Storage layout:
///   {data_dir}/databases/{db_id}/tables/table_{table_id}.db
///
/// Thread safety: All public methods are protected by a mutex.
class StorageManager {
public:
    /// @param dm         Shared DiskManager for all file I/O.
    /// @param data_dir   Root data directory.
    /// @param pool_size  Buffer pool size (frames) per table. Default 256.
    StorageManager(DiskManager& dm, std::filesystem::path data_dir, uint32_t pool_size = 256);

    /// Create the directory structure for a new database.
    /// Creates {data_dir}/databases/{db_id}/ and {data_dir}/databases/{db_id}/tables/.
    [[nodiscard]] Result<void> create_database_storage(database_id_t db_id);

    /// Drop all storage for a database. Recursively removes the database directory.
    [[nodiscard]] Result<void> drop_database_storage(database_id_t db_id);

    /// Create physical storage for a new table within a database.
    /// Creates the backing file and initializes the BufferPoolManager + TableHeap.
    [[nodiscard]] Result<void>
    create_table_storage(database_id_t db_id, table_id_t table_id, const TableSchema& table_schema);

    /// Get the storage for an existing table.
    [[nodiscard]] Result<TableStorage*> get_table_storage(table_id_t table_id);

    /// Drop storage for a table. Flushes, closes the file, and removes it.
    [[nodiscard]] Result<void> drop_table_storage(database_id_t db_id, table_id_t table_id);

private:
    /// Build the directory path for a database: {data_dir}/databases/{db_id}/
    [[nodiscard]] std::filesystem::path database_path(database_id_t db_id) const;

    /// Build the file path for a table: {data_dir}/databases/{db_id}/tables/table_{id}.db
    [[nodiscard]] std::filesystem::path table_path(database_id_t db_id, table_id_t id) const;

    /// Build a byte-level Schema from a TableSchema (CatalogColumnDef -> ColumnDef).
    [[nodiscard]] static Schema build_storage_schema(const TableSchema& ts);

    DiskManager& dm_;
    std::filesystem::path data_dir_;
    uint32_t pool_size_;

    mutable std::mutex mu_;
    std::unordered_map<table_id_t, std::unique_ptr<TableStorage>> tables_;
};

} // namespace giodb
