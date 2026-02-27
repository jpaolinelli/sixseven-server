#include "giodb/executor/storage_manager.h"

#include "giodb/common/status.h"

#include <string>

namespace giodb {

StorageManager::StorageManager(DiskManager& dm, std::filesystem::path data_dir, uint32_t pool_size)
    : dm_(dm), data_dir_(std::move(data_dir)), pool_size_(pool_size) {
    // Create the default database directory on initialization.
    auto db_dir = database_path(default_database_id) / "tables";
    std::filesystem::create_directories(db_dir);
}

std::filesystem::path StorageManager::database_path(database_id_t db_id) const {
    return data_dir_ / "databases" / std::to_string(db_id);
}

std::filesystem::path StorageManager::table_path(database_id_t db_id, table_id_t id) const {
    return database_path(db_id) / "tables" / ("table_" + std::to_string(id) + ".db");
}

Schema StorageManager::build_storage_schema(const TableSchema& ts) {
    std::vector<ColumnDef> cols;
    cols.reserve(ts.columns.size());
    for (const auto& c : ts.columns) {
        cols.push_back({c.name, c.type_id});
    }
    return Schema(std::move(cols));
}

Result<void> StorageManager::create_database_storage(database_id_t db_id) {
    std::lock_guard lock(mu_);

    auto db_dir = database_path(db_id);
    auto tables_dir = db_dir / "tables";

    std::error_code ec;
    std::filesystem::create_directories(tables_dir, ec);
    if (ec) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to create database directory: " + ec.message());
    }

    return ok();
}

Result<void> StorageManager::drop_database_storage(database_id_t db_id) {
    std::lock_guard lock(mu_);

    auto db_dir = database_path(db_id);

    std::error_code ec;
    std::filesystem::remove_all(db_dir, ec);
    if (ec) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to remove database directory: " + ec.message());
    }

    return ok();
}

Result<void> StorageManager::create_table_storage(database_id_t db_id,
                                                  table_id_t table_id,
                                                  const TableSchema& table_schema) {
    std::lock_guard lock(mu_);

    if (tables_.count(table_id) != 0) {
        return make_error(StatusCode::ALREADY_EXISTS,
                          "storage already exists for table_id " + std::to_string(table_id));
    }

    auto path = table_path(db_id, table_id);
    std::filesystem::create_directories(path.parent_path());

    auto fid = dm_.create_file(path, /*direct_io=*/false, /*overwrite=*/true);
    if (!fid) {
        return make_error(fid.error().code, fid.error().message);
    }

    auto storage = std::make_unique<TableStorage>();
    storage->file_id = *fid;
    storage->bpm = std::make_unique<BufferPoolManager>(dm_, *fid, pool_size_);
    storage->heap = std::make_unique<TableHeap>(*storage->bpm, dm_, *fid);
    storage->storage_schema = build_storage_schema(table_schema);

    tables_[table_id] = std::move(storage);
    return ok();
}

Result<void> StorageManager::open_table_storage(database_id_t db_id,
                                                table_id_t table_id,
                                                const TableSchema& table_schema) {
    std::lock_guard lock(mu_);

    if (tables_.count(table_id) != 0) {
        return ok(); // Already open.
    }

    auto path = table_path(db_id, table_id);
    if (!std::filesystem::exists(path)) {
        return make_error(StatusCode::NOT_FOUND, "table file not found: " + path.string());
    }

    auto fid = dm_.open_file(path);
    if (!fid) {
        return make_error(fid.error().code, fid.error().message);
    }

    auto storage = std::make_unique<TableStorage>();
    storage->file_id = *fid;
    storage->bpm = std::make_unique<BufferPoolManager>(dm_, *fid, pool_size_);
    storage->heap = std::make_unique<TableHeap>(*storage->bpm, dm_, *fid);
    storage->storage_schema = build_storage_schema(table_schema);

    tables_[table_id] = std::move(storage);
    return ok();
}

bool StorageManager::table_file_exists(database_id_t db_id, table_id_t table_id) const {
    auto path = table_path(db_id, table_id);
    return std::filesystem::exists(path);
}

Result<TableStorage*> StorageManager::get_table_storage(table_id_t table_id) {
    std::lock_guard lock(mu_);

    auto it = tables_.find(table_id);
    if (it == tables_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "no storage for table_id " + std::to_string(table_id));
    }
    return ok(it->second.get());
}

Result<void> StorageManager::drop_table_storage(database_id_t db_id, table_id_t table_id) {
    std::lock_guard lock(mu_);

    auto it = tables_.find(table_id);
    if (it == tables_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "no storage for table_id " + std::to_string(table_id));
    }

    auto& storage = it->second;

    // Flush all dirty pages before closing.
    auto flush = storage->bpm->flush_all();
    if (!flush) {
        return make_error(flush.error().code, flush.error().message);
    }

    // Close the file and remove the backing file.
    auto close = dm_.close_file(storage->file_id);
    if (!close) {
        return make_error(close.error().code, close.error().message);
    }

    auto path = table_path(db_id, table_id);
    std::filesystem::remove(path);

    tables_.erase(it);
    return ok();
}

} // namespace giodb
