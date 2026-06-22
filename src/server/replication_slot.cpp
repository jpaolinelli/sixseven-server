#include "sixseven/server/replication_slot.h"

#include "sixseven/common/logging.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>

namespace sixseven {

namespace {

uint64_t now_us() {
    auto now = std::chrono::system_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

nlohmann::json slot_to_json(const ReplicationSlot& slot) {
    return {
        {"slot_name", slot.slot_name},
        {"slot_type", slot.slot_type},
        {"restart_lsn", slot.restart_lsn},
        {"confirmed_flush_lsn", slot.confirmed_flush_lsn},
        {"created_at_us", slot.created_at_us},
    };
}

ReplicationSlot slot_from_json(const nlohmann::json& j) {
    ReplicationSlot slot;
    slot.slot_name = j.at("slot_name").get<std::string>();
    slot.slot_type = j.at("slot_type").get<std::string>();
    slot.active = false; // Always inactive after load.
    slot.restart_lsn = j.at("restart_lsn").get<lsn_t>();
    slot.confirmed_flush_lsn = j.at("confirmed_flush_lsn").get<lsn_t>();
    slot.created_at_us = j.at("created_at_us").get<uint64_t>();
    return slot;
}

} // namespace

// -- ReplicationSlotManager ---------------------------------------------------

ReplicationSlotManager::ReplicationSlotManager(std::filesystem::path data_dir,
                                               size_t wal_retention_warning_bytes)
    : data_dir_(std::move(data_dir)), wal_retention_warning_bytes_(wal_retention_warning_bytes) {}

Result<ReplicationSlot> ReplicationSlotManager::create_slot(const std::string& name) {
    if (name.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "slot name must not be empty");
    }

    std::lock_guard lock(mutex_);

    if (slots_.count(name) > 0) {
        return make_error(StatusCode::ALREADY_EXISTS, "replication slot already exists: " + name);
    }

    ReplicationSlot slot;
    slot.slot_name = name;
    slot.slot_type = "physical";
    slot.active = false;
    slot.restart_lsn = invalid_lsn;
    slot.confirmed_flush_lsn = invalid_lsn;
    slot.created_at_us = now_us();

    slots_.emplace(name, slot);

    SIXSEVEN_LOG_INFO("replication slot created: {}", name);

    // Persist after modification.
    auto save_result = save_impl();
    if (!save_result) {
        SIXSEVEN_LOG_WARN("failed to persist replication slots after create: {}",
                       save_result.error().message);
    }

    return ok(slot);
}

Result<void> ReplicationSlotManager::drop_slot(const std::string& name) {
    std::lock_guard lock(mutex_);

    auto it = slots_.find(name);
    if (it == slots_.end()) {
        return make_error(StatusCode::NOT_FOUND, "replication slot not found: " + name);
    }

    if (it->second.active) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "cannot drop active replication slot: " + name);
    }

    slots_.erase(it);

    SIXSEVEN_LOG_INFO("replication slot dropped: {}", name);

    // Persist after modification.
    auto save_result = save_impl();
    if (!save_result) {
        SIXSEVEN_LOG_WARN("failed to persist replication slots after drop: {}",
                       save_result.error().message);
    }

    return ok();
}

Result<ReplicationSlot> ReplicationSlotManager::get_slot(const std::string& name) const {
    std::lock_guard lock(mutex_);

    auto it = slots_.find(name);
    if (it == slots_.end()) {
        return make_error(StatusCode::NOT_FOUND, "replication slot not found: " + name);
    }

    return ok(it->second);
}

std::vector<ReplicationSlot> ReplicationSlotManager::list_slots() const {
    std::lock_guard lock(mutex_);

    std::vector<ReplicationSlot> result;
    result.reserve(slots_.size());
    for (const auto& [name, slot] : slots_) {
        result.push_back(slot);
    }
    return result;
}

Result<void> ReplicationSlotManager::activate_slot(const std::string& name, lsn_t restart_lsn) {
    std::lock_guard lock(mutex_);

    auto it = slots_.find(name);
    if (it == slots_.end()) {
        return make_error(StatusCode::NOT_FOUND, "replication slot not found: " + name);
    }

    it->second.active = true;
    if (restart_lsn != invalid_lsn) {
        it->second.restart_lsn = restart_lsn;
    }

    SIXSEVEN_LOG_INFO("replication slot activated: {} (restart_lsn={})", name, restart_lsn);
    return ok();
}

Result<void> ReplicationSlotManager::deactivate_slot(const std::string& name) {
    std::lock_guard lock(mutex_);

    auto it = slots_.find(name);
    if (it == slots_.end()) {
        return make_error(StatusCode::NOT_FOUND, "replication slot not found: " + name);
    }

    it->second.active = false;

    SIXSEVEN_LOG_INFO("replication slot deactivated: {}", name);

    // Persist after deactivation to save the latest confirmed_flush_lsn.
    auto save_result = save_impl();
    if (!save_result) {
        SIXSEVEN_LOG_WARN("failed to persist replication slots after deactivate: {}",
                       save_result.error().message);
    }

    return ok();
}

void ReplicationSlotManager::update_confirmed_lsn(const std::string& slot_name, lsn_t lsn) {
    std::lock_guard lock(mutex_);

    auto it = slots_.find(slot_name);
    if (it == slots_.end()) {
        return;
    }

    auto& slot = it->second;
    if (lsn > slot.confirmed_flush_lsn) {
        slot.confirmed_flush_lsn = lsn;
    }
    // Advance restart_lsn to keep pace with confirmed progress.
    if (lsn > slot.restart_lsn) {
        slot.restart_lsn = lsn;
    }
}

lsn_t ReplicationSlotManager::get_min_restart_lsn() const {
    std::lock_guard lock(mutex_);

    if (slots_.empty()) {
        return invalid_lsn;
    }

    lsn_t min_lsn = std::numeric_limits<lsn_t>::max();
    bool found = false;

    for (const auto& [name, slot] : slots_) {
        if (slot.restart_lsn != invalid_lsn) {
            min_lsn = std::min(min_lsn, slot.restart_lsn);
            found = true;
        } else {
            // A slot with invalid_lsn means it hasn't started yet — it needs
            // all WAL from the beginning.
            return invalid_lsn;
        }
    }

    return found ? min_lsn : invalid_lsn;
}

Result<void> ReplicationSlotManager::save() const {
    std::lock_guard lock(mutex_);
    return save_impl();
}

Result<void> ReplicationSlotManager::save_impl() const {
    nlohmann::json j = nlohmann::json::array();
    for (const auto& [name, slot] : slots_) {
        j.push_back(slot_to_json(slot));
    }

    auto path = persistence_path();

    // Ensure the data directory exists.
    std::error_code ec;
    std::filesystem::create_directories(data_dir_, ec);
    if (ec) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to create data directory for slot persistence: " + ec.message());
    }

    // Write to a temp file first, then rename for atomicity.
    auto tmp_path = path;
    tmp_path += ".tmp";

    std::ofstream out(tmp_path);
    if (!out.is_open()) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to open slot persistence file for writing: " + tmp_path.string());
    }

    out << j.dump(2);
    out.close();

    if (out.fail()) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to write slot persistence file: " + tmp_path.string());
    }

    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to rename slot persistence file: " + ec.message());
    }

    return ok();
}

Result<void> ReplicationSlotManager::load() {
    std::lock_guard lock(mutex_);

    auto path = persistence_path();

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        // No persistence file — start with empty slots.
        return ok();
    }

    std::ifstream in(path);
    if (!in.is_open()) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to open slot persistence file: " + path.string());
    }

    nlohmann::json j;
    try {
        in >> j;
    } catch (const nlohmann::json::parse_error& e) {
        return make_error(StatusCode::PARSE_ERROR,
                          "failed to parse slot persistence file: " + std::string(e.what()));
    }

    if (!j.is_array()) {
        return make_error(StatusCode::PARSE_ERROR, "slot persistence file is not a JSON array");
    }

    slots_.clear();
    for (const auto& item : j) {
        try {
            auto slot = slot_from_json(item);
            // All slots are inactive after a restart.
            slot.active = false;
            slots_.emplace(slot.slot_name, std::move(slot));
        } catch (const nlohmann::json::exception& e) {
            return make_error(StatusCode::PARSE_ERROR,
                              "failed to parse replication slot entry: " + std::string(e.what()));
        }
    }

    SIXSEVEN_LOG_INFO("loaded {} replication slot(s) from disk", slots_.size());
    return ok();
}

std::vector<std::string> ReplicationSlotManager::check_wal_accumulation(lsn_t current_lsn) const {
    std::lock_guard lock(mutex_);

    std::vector<std::string> warned;
    for (const auto& [name, slot] : slots_) {
        if (slot.active || slot.restart_lsn == invalid_lsn) {
            continue;
        }

        // Estimate WAL bytes retained by this inactive slot.
        if (current_lsn > slot.restart_lsn) {
            uint64_t retained_bytes = current_lsn - slot.restart_lsn;
            if (retained_bytes > wal_retention_warning_bytes_) {
                SIXSEVEN_LOG_WARN("inactive replication slot '{}' is retaining approximately {} bytes "
                               "of WAL (restart_lsn={}, current_lsn={})",
                               name,
                               retained_bytes,
                               slot.restart_lsn,
                               current_lsn);
                warned.push_back(name);
            }
        }
    }
    return warned;
}

std::filesystem::path ReplicationSlotManager::persistence_path() const {
    return data_dir_ / "replication_slots.json";
}

} // namespace sixseven
