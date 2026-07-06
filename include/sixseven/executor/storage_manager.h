#pragma once

#include "sixseven/catalog/schema.h"
#include "sixseven/common/result.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace sixseven {

// Forward declaration.
class WalWriter;

/// Physical storage state for a single table.
struct TableStorage {
    database_id_t db_id = 0; ///< Owning database (for db-scoped cleanup).
    FileId file_id = 0;
    std::unique_ptr<BufferPoolManager> bpm;
    std::unique_ptr<TableHeap> heap;
    Schema storage_schema; ///< Byte-level schema for TupleSerializer.
};

/// Physical storage state for a single index (one file per index).
struct IndexStorage {
    database_id_t db_id = 0; ///< Owning database (for db-scoped cleanup).
    FileId file_id = 0;
    std::unique_ptr<BufferPoolManager> bpm;
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

    /// Flushes and closes every table/index file still registered in the
    /// shared DiskManager. Without this, a destroyed StorageManager leaks its
    /// open file descriptors into the DiskManager (which may outlive it); on
    /// Windows those handles block directory removal until process exit.
    ~StorageManager();

    /// Create the directory structure for a new database.
    /// Creates {data_dir}/databases/{db_id}/ and {data_dir}/databases/{db_id}/tables/.
    [[nodiscard]] Result<void> create_database_storage(database_id_t db_id);

    /// Drop all storage for a database. Recursively removes the database directory.
    [[nodiscard]] Result<void> drop_database_storage(database_id_t db_id);

    /// Create physical storage for a new table within a database.
    /// Creates the backing file and initializes the BufferPoolManager + TableHeap.
    [[nodiscard]] Result<void>
    create_table_storage(database_id_t db_id, table_id_t table_id, const TableSchema& table_schema);

    /// Open physical storage for an existing table from disk.
    /// Opens the backing file and initializes the BufferPoolManager + TableHeap.
    /// Fails with NOT_FOUND if the table file does not exist.
    [[nodiscard]] Result<void>
    open_table_storage(database_id_t db_id, table_id_t table_id, const TableSchema& table_schema);

    /// Check whether a table storage file exists on disk.
    [[nodiscard]] bool table_file_exists(database_id_t db_id, table_id_t table_id) const;

    /// Attach a transaction manager used for MVCC visibility filtering
    /// (GDB-747). Applied to all currently open table heaps and to every heap
    /// opened or created afterwards. Pass nullptr to detach.
    /// @param txn_mgr Transaction manager (not owned; must outlive this
    ///                StorageManager or be detached).
    void set_txn_manager(const TransactionManager* txn_mgr);

    /// Attach a WAL writer so every table heap emits WAL records for DML
    /// (GDB-900 / GDB-1276). Applied to all currently open table heaps and to
    /// every heap opened or created afterwards. Pass nullptr to detach.
    ///
    /// Each heap is attached with its own table_id so WAL records carry the
    /// correct table identifier for crash recovery.
    ///
    /// @param writer  WAL writer (not owned; must outlive this StorageManager
    ///                or be detached via set_wal_writer(nullptr)).
    void set_wal_writer(WalWriter* writer);

    /// Get the storage for an existing table.
    [[nodiscard]] Result<TableStorage*> get_table_storage(table_id_t table_id);

    /// Drop storage for a table. Flushes, closes the file, and removes it.
    [[nodiscard]] Result<void> drop_table_storage(database_id_t db_id, table_id_t table_id);

    /// Flush all dirty pages across every open table and index to disk.
    /// Call after bulk data loading to ensure crash safety.
    [[nodiscard]] Result<void> flush_all();

    /// Build a byte-level Schema from a TableSchema (CatalogColumnDef -> ColumnDef).
    [[nodiscard]] static Schema build_storage_schema(const TableSchema& ts);

    /// Read the persisted autoincrement counter from a table file's header extension.
    /// Returns 0 if no counter was previously written.
    [[nodiscard]] Result<int64_t> read_autoincrement(table_id_t table_id);

    /// Write the autoincrement counter to a table file's header extension.
    [[nodiscard]] Result<void> write_autoincrement(table_id_t table_id, int64_t value);

    // -- Index file management ---------------------------------------------------

    /// Create physical storage for a new index file.
    [[nodiscard]] Result<IndexStorage*> create_index_storage(database_id_t db_id,
                                                             index_id_t index_id);

    /// Open physical storage for an existing index file.
    [[nodiscard]] Result<IndexStorage*> open_index_storage(database_id_t db_id,
                                                           index_id_t index_id);

    /// Get the storage for an already-opened index.
    [[nodiscard]] Result<IndexStorage*> get_index_storage(index_id_t index_id);

    /// Drop storage for an index. Flushes, closes the file, and removes it.
    [[nodiscard]] Result<void> drop_index_storage(database_id_t db_id, index_id_t index_id);

    /// Check whether an index storage file exists on disk.
    [[nodiscard]] bool index_file_exists(database_id_t db_id, index_id_t index_id) const;

    /// Read the meta page ID stored in an index file's header extension.
    /// Returns 0 if not yet written.
    [[nodiscard]] Result<PageId> read_index_meta_page_id(index_id_t index_id);

    /// Write the meta page ID to an index file's header extension.
    [[nodiscard]] Result<void> write_index_meta_page_id(index_id_t index_id, PageId meta_page_id);

    /// Enable the double-write buffer for all table storage files.
    /// When set, each BufferPoolManager opened or created afterwards will
    /// call enable_double_write() with a path derived from the table file
    /// (table_<id>.db.dwb in the same directory). Recovery runs automatically
    /// on each existing DWB file at open time. Defaults to off.
    void set_double_write_enabled(bool enabled);

    /// Invoke @p fn(table_id, heap*) for every open table heap.
    /// Used by WAL crash recovery to register all live heaps with
    /// TableHeapRecoveryHandler before calling WalRecovery::recover().
    /// The callback is invoked under the internal mutex — it must be
    /// lightweight and must not call back into StorageManager.
    void for_each_table_heap(const std::function<void(table_id_t, TableHeap*)>& fn) const;

    /// Access the shared DiskManager (e.g. for Vacuum construction).
    [[nodiscard]] DiskManager& disk_manager() { return dm_; }

private:
    /// Build the directory path for a database: {data_dir}/databases/{db_id}/
    [[nodiscard]] std::filesystem::path database_path(database_id_t db_id) const;

    /// Build the file path for a table: {data_dir}/databases/{db_id}/tables/table_{id}.db
    [[nodiscard]] std::filesystem::path table_path(database_id_t db_id, table_id_t id) const;

    /// Build the file path for an index: {data_dir}/databases/{db_id}/indexes/index_{id}.db
    [[nodiscard]] std::filesystem::path index_path(database_id_t db_id, index_id_t id) const;

    DiskManager& dm_;
    std::filesystem::path data_dir_;
    uint32_t pool_size_;

    /// Transaction manager attached to table heaps for MVCC visibility
    /// filtering (GDB-747, not owned; may be null).
    const TransactionManager* txn_mgr_ = nullptr;

    /// Whether the DWB is enabled for table storage files (GDB-922).
    bool dwb_enabled_ = false;

    /// WAL writer attached to every table heap so DML emits WAL records for
    /// crash recovery (GDB-900 / GDB-1276, not owned; may be null).
    WalWriter* wal_writer_ = nullptr;

    static constexpr uint32_t index_pool_size_ = 64;

    mutable std::mutex mu_;
    std::unordered_map<table_id_t, std::unique_ptr<TableStorage>> tables_;
    std::unordered_map<index_id_t, std::unique_ptr<IndexStorage>> indexes_;
};

} // namespace sixseven
