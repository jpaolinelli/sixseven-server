#pragma once

#include "giodb/common/result.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace giodb {

// Forward declarations.
class QueryEngine;

/// A single setting entry in the in-memory cache.
struct SettingEntry {
    std::string key;
    std::string value;
    std::string category;
    std::string description;
    bool is_runtime_mutable = false;
};

/// In-memory cache of sys_settings for fast SHOW reads and validated SET writes.
///
/// Loaded from the sys_settings table at startup. SET updates both the cache
/// and the persistent table. SHOW reads from the cache (fast path).
class SettingsCache {
public:
    /// Load all settings from sys_settings into the cache.
    /// Requires the QueryEngine to execute SQL against the system database.
    [[nodiscard]] Result<void> load(QueryEngine& engine);

    /// Get a setting by key. Returns nullopt if not found.
    [[nodiscard]] std::optional<SettingEntry> get(const std::string& key) const;

    /// Get all settings.
    [[nodiscard]] std::vector<SettingEntry> get_all() const;

    /// Update a setting value in the cache only (caller persists separately).
    /// Validates that the key exists and is runtime-mutable.
    [[nodiscard]] Result<void> update(const std::string& key, const std::string& new_value);

    /// Apply a runtime side-effect for a setting change.
    /// Currently supports: logging.level → spdlog::set_level().
    static void apply_runtime_change(const std::string& key, const std::string& value);

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, SettingEntry> entries_;
};

} // namespace giodb
