#include "giodb/executor/settings_cache.h"

#include "giodb/catalog/schema.h"
#include "giodb/common/logging.h"
#include "giodb/executor/query_engine.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace giodb {

Result<void> SettingsCache::load(QueryEngine& engine) {
    // Switch to system database to query sys_settings.
    auto prev_db = engine.current_database_id();
    engine.set_current_database(system_database_id);

    auto result = engine.execute("SELECT key, value, category, description, is_runtime_mutable "
                                 "FROM sys_settings");
    engine.set_current_database(prev_db);

    if (!result) {
        return make_error(result.error().code,
                          "failed to load settings cache: " + result.error().message);
    }

    std::lock_guard<std::mutex> lock(mu_);
    entries_.clear();

    for (const auto& row : result->rows) {
        if (row.size() < 5 || row[0].is_null()) {
            continue;
        }

        SettingEntry entry;
        entry.key = row[0].as_string();
        entry.value = row[1].is_null() ? "" : row[1].as_string();
        entry.category = row[2].is_null() ? "" : row[2].as_string();
        entry.description = row[3].is_null() ? "" : row[3].as_string();
        entry.is_runtime_mutable = !row[4].is_null() && row[4].as_bool();

        entries_[entry.key] = std::move(entry);
    }

    GIODB_LOG_INFO("settings cache: loaded {} settings", entries_.size());
    return ok();
}

std::optional<SettingEntry> SettingsCache::get(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<SettingEntry> SettingsCache::get_all() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<SettingEntry> result;
    result.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
        result.push_back(entry);
    }
    // Sort by key for deterministic output.
    std::sort(result.begin(), result.end(), [](const SettingEntry& a, const SettingEntry& b) {
        return a.key < b.key;
    });
    return result;
}

Result<void> SettingsCache::update(const std::string& key, const std::string& new_value) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return make_error(StatusCode::NOT_FOUND, "unrecognized parameter '" + key + "'");
    }
    if (!it->second.is_runtime_mutable) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "parameter '" + key +
                              "' cannot be changed at runtime (requires restart)");
    }
    it->second.value = new_value;
    return ok();
}

void SettingsCache::apply_runtime_change(const std::string& key, const std::string& value) {
    if (key == "logging.level") {
        auto level = spdlog::level::from_str(value);
        spdlog::set_level(level);
        GIODB_LOG_INFO("runtime: logging.level changed to '{}'", value);
    }
}

} // namespace giodb
