#include "giodb/executor/provider_cache.h"

#include "giodb/catalog/catalog.h"
#include "giodb/catalog/schema.h"
#include "giodb/common/logging.h"
#include "giodb/executor/query_engine.h"

#include <algorithm>

namespace giodb {

Result<void> ProviderCache::load(QueryEngine& engine) {
    // Switch to system database to query sys_providers.
    auto prev_db = engine.current_database_id();
    engine.set_current_database(system_database_id);

    auto result = engine.execute("SELECT provider_id, name, type, endpoint, model, "
                                 "api_key_encrypted, is_default "
                                 "FROM sys_providers");
    engine.set_current_database(prev_db);

    if (!result) {
        return make_error(result.error().code,
                          "failed to load provider cache: " + result.error().message);
    }

    std::lock_guard<std::mutex> lock(mu_);
    providers_.clear();
    next_provider_id_ = 1;

    for (const auto& row : result->rows) {
        if (row.size() < 7 || row[0].is_null() || row[1].is_null()) {
            continue;
        }

        ProviderConfig config;
        config.provider_id = row[0].as_int32();
        config.name = row[1].as_string();
        config.type = row[2].is_null() ? "" : row[2].as_string();
        config.endpoint = row[3].is_null() ? "" : row[3].as_string();
        config.model = row[4].is_null() ? "" : row[4].as_string();
        config.api_key = row[5].is_null() ? "" : row[5].as_string();
        config.is_default = !row[6].is_null() && row[6].as_bool();

        if (config.provider_id >= next_provider_id_) {
            next_provider_id_ = config.provider_id + 1;
        }

        providers_[config.name] = std::move(config);
    }

    GIODB_LOG_INFO("provider cache: loaded {} providers", providers_.size());
    return ok();
}

std::optional<ProviderConfig> ProviderCache::get(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = providers_.find(name);
    if (it == providers_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<ProviderConfig> ProviderCache::get_default() const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& [name, config] : providers_) {
        if (config.is_default) {
            return config;
        }
    }
    return std::nullopt;
}

std::vector<ProviderConfig> ProviderCache::get_all() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ProviderConfig> result;
    result.reserve(providers_.size());
    for (const auto& [name, config] : providers_) {
        result.push_back(config);
    }
    // Sort by name for deterministic output.
    std::sort(result.begin(), result.end(), [](const ProviderConfig& a, const ProviderConfig& b) {
        return a.name < b.name;
    });
    return result;
}

Result<void> ProviderCache::validate_provider_exists(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = providers_.find(name);
    if (it == providers_.end()) {
        return make_error(StatusCode::NOT_FOUND, "provider '" + name + "' not found");
    }
    return ok();
}

bool ProviderCache::is_provider_in_use(const std::string& name, const Catalog& catalog) const {
    auto all_emb_cols = catalog.list_all_embedding_columns();
    for (const auto& ec : all_emb_cols) {
        if (ec.provider == name) {
            return true;
        }
    }
    return false;
}

size_t ProviderCache::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return providers_.size();
}

} // namespace giodb
