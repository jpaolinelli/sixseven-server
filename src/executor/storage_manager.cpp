#include "sixseven/executor/storage_manager.h"

#include "sixseven/common/status.h"

#include <string>

namespace sixseven {

StorageManager::StorageManager(DiskManager& dm, std::filesystem::path data_dir, uint32_t pool_size)
    : dm_(dm), data_dir_(std::move(data_dir)), pool_size_(pool_size) {}

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

// -- Autoincrement persistence -----------------------------------------------

Result<int64_t> StorageManager::read_autoincrement(table_id_t table_id) {
    std::lock_guard lock(mu_);

    auto it = tables_.find(table_id);
    if (it == tables_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "no storage for table_id " + std::to_string(table_id));
    }

    // Offset 8: autoincrement counter (offset 0 is used by TableHeap for row count).
    auto val = dm_.read_header_ext_u64(it->second->file_id, 8);
    if (!val) {
        return ok(int64_t(0)); // No counter written yet.
    }
    return ok(static_cast<int64_t>(*val));
}

Result<void> StorageManager::write_autoincrement(table_id_t table_id, int64_t value) {
    std::lock_guard lock(mu_);

    auto it = tables_.find(table_id);
    if (it == tables_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "no storage for table_id " + std::to_string(table_id));
    }

    return dm_.write_header_ext_u64(it->second->file_id, 8, static_cast<uint64_t>(value));
}

// -- Index file management ---------------------------------------------------

std::filesystem::path StorageManager::index_path(database_id_t db_id, index_id_t id) const {
    return database_path(db_id) / "indexes" / ("index_" + std::to_string(id) + ".db");
}

bool StorageManager::index_file_exists(database_id_t db_id, index_id_t index_id) const {
    auto path = index_path(db_id, index_id);
    return std::filesystem::exists(path);
}

Result<IndexStorage*> StorageManager::create_index_storage(database_id_t db_id,
                                                           index_id_t index_id) {
    std::lock_guard lock(mu_);

    if (indexes_.count(index_id) != 0) {
        return make_error(StatusCode::ALREADY_EXISTS,
                          "index storage already exists for index_id " + std::to_string(index_id));
    }

    auto path = index_path(db_id, index_id);
    std::filesystem::create_directories(path.parent_path());

    auto fid = dm_.create_file(path, /*direct_io=*/false, /*overwrite=*/true);
    if (!fid) {
        return make_error(fid.error().code, fid.error().message);
    }

    auto storage = std::make_unique<IndexStorage>();
    storage->file_id = *fid;
    storage->bpm = std::make_unique<BufferPoolManager>(dm_, *fid, index_pool_size_);

    auto* ptr = storage.get();
    indexes_[index_id] = std::move(storage);
    return ok(ptr);
}

Result<IndexStorage*> StorageManager::open_index_storage(database_id_t db_id,
                                                         index_id_t index_id) {
    std::lock_guard lock(mu_);

    if (auto it = indexes_.find(index_id); it != indexes_.end()) {
        return ok(it->second.get());
    }

    auto path = index_path(db_id, index_id);
    if (!std::filesystem::exists(path)) {
        return make_error(StatusCode::NOT_FOUND, "index file not found: " + path.string());
    }

    auto fid = dm_.open_file(path);
    if (!fid) {
        return make_error(fid.error().code, fid.error().message);
    }

    auto storage = std::make_unique<IndexStorage>();
    storage->file_id = *fid;
    storage->bpm = std::make_unique<BufferPoolManager>(dm_, *fid, index_pool_size_);

    auto* ptr = storage.get();
    indexes_[index_id] = std::move(storage);
    return ok(ptr);
}

Result<IndexStorage*> StorageManager::get_index_storage(index_id_t index_id) {
    std::lock_guard lock(mu_);

    auto it = indexes_.find(index_id);
    if (it == indexes_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "no storage for index_id " + std::to_string(index_id));
    }
    return ok(it->second.get());
}

Result<void> StorageManager::drop_index_storage(database_id_t db_id, index_id_t index_id) {
    std::lock_guard lock(mu_);

    auto it = indexes_.find(index_id);
    if (it != indexes_.end()) {
        auto& storage = it->second;
        auto flush = storage->bpm->flush_all();
        if (!flush) {
            return make_error(flush.error().code, flush.error().message);
        }
        auto close = dm_.close_file(storage->file_id);
        if (!close) {
            return make_error(close.error().code, close.error().message);
        }
        indexes_.erase(it);
    }

    auto path = index_path(db_id, index_id);
    std::filesystem::remove(path);
    return ok();
}

// -- Index meta page ID persistence ------------------------------------------

Result<PageId> StorageManager::read_index_meta_page_id(index_id_t index_id) {
    std::lock_guard lock(mu_);

    auto it = indexes_.find(index_id);
    if (it == indexes_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "no storage for index_id " + std::to_string(index_id));
    }

    auto val = dm_.read_header_ext_u64(it->second->file_id, 0);
    if (!val) {
        return ok(PageId(0)); // Not written yet.
    }
    return ok(static_cast<PageId>(*val));
}

Result<void> StorageManager::write_index_meta_page_id(index_id_t index_id, PageId meta_page_id) {
    std::lock_guard lock(mu_);

    auto it = indexes_.find(index_id);
    if (it == indexes_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "no storage for index_id " + std::to_string(index_id));
    }

    return dm_.write_header_ext_u64(it->second->file_id, 0, static_cast<uint64_t>(meta_page_id));
}

// -- Flush all dirty pages to disk -------------------------------------------

Result<void> StorageManager::flush_all() {
    std::lock_guard lock(mu_);

    for (auto& [id, ts] : tables_) {
        if (ts && ts->bpm) {
            auto r = ts->bpm->flush_all();
            if (!r) return r;
            auto s = dm_.sync_file(ts->file_id);
            if (!s) return s;
        }
    }
    for (auto& [id, is] : indexes_) {
        if (is && is->bpm) {
            auto r = is->bpm->flush_all();
            if (!r) return r;
            auto s = dm_.sync_file(is->file_id);
            if (!s) return s;
        }
    }
    return ok();
}

} // namespace sixseven
