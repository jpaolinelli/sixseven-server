#include "giodb/executor/provider_cache.h"

#include "giodb/catalog/catalog.h"
#include "giodb/catalog/schema.h"
#include "giodb/common/logging.h"
#include "giodb/common/secrets_manager.h"
#include "giodb/executor/query_engine.h"

#include <algorithm>

namespace giodb {

void ProviderCache::set_secrets_manager(const SecretsManager* secrets_manager) {
    secrets_manager_ = secrets_manager;
}

Result<void> ProviderCache::load(QueryEngine& engine) {
    // Prevent re-entrant loads. Encryption UPDATEs below trigger
    // maybe_invalidate_provider_cache which calls load() again.
    if (loading_) {
        return ok();
    }
    loading_ = true;

    // Switch to system database to query sys_providers.
    // Skip masking so we get the raw encrypted values (not '********').
    auto prev_db = engine.current_database_id();
    engine.set_current_database(system_database_id);
    engine.push_skip_masking();

    auto result = engine.execute("SELECT provider_id, name, type, endpoint, model, "
                                 "api_key_encrypted, is_default "
                                 "FROM sys_providers");

    if (!result) {
        engine.pop_skip_masking();
        engine.set_current_database(prev_db);
        loading_ = false;
        return make_error(result.error().code,
                          "failed to load provider cache: " + result.error().message);
    }

    // Collect providers that need their api_key encrypted in the database.
    struct PendingEncrypt {
        int32_t provider_id;
        std::string encrypted_value;
    };
    std::vector<PendingEncrypt> to_encrypt;

    // Build new provider map (outside mutex to avoid deadlock with UPDATEs).
    std::unordered_map<std::string, ProviderConfig> new_providers;
    int32_t new_next_id = 1;

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
        config.is_default = !row[6].is_null() && row[6].as_bool();

        // Handle api_key with optional encryption.
        std::string raw_key = row[5].is_null() ? "" : row[5].as_string();

        if (!raw_key.empty() && secrets_manager_ != nullptr) {
            if (SecretsManager::looks_encrypted(raw_key)) {
                // Value is encrypted — decrypt it for in-memory use.
                auto decrypted = secrets_manager_->decrypt(raw_key);
                if (decrypted) {
                    config.api_key = std::move(*decrypted);
                } else {
                    // Decryption failed — treat as plaintext (may be a false positive
                    // from looks_encrypted, or a key rotation scenario).
                    GIODB_LOG_WARN("provider '{}': failed to decrypt api_key, "
                                   "treating as plaintext: {}",
                                   config.name,
                                   decrypted.error().message);
                    config.api_key = raw_key;
                    auto enc = secrets_manager_->encrypt(raw_key);
                    if (enc) {
                        to_encrypt.push_back({config.provider_id, std::move(*enc)});
                    }
                }
            } else {
                // Value is plaintext — store decrypted and schedule encryption.
                config.api_key = raw_key;
                auto enc = secrets_manager_->encrypt(raw_key);
                if (enc) {
                    to_encrypt.push_back({config.provider_id, std::move(*enc)});
                } else {
                    GIODB_LOG_WARN("provider '{}': failed to encrypt api_key: {}",
                                   config.name,
                                   enc.error().message);
                }
            }
        } else {
            config.api_key = raw_key;
        }

        if (config.provider_id >= new_next_id) {
            new_next_id = config.provider_id + 1;
        }

        new_providers[config.name] = std::move(config);
    }

    // Encrypt plaintext keys that are still on disk.
    // These UPDATEs may trigger maybe_invalidate_provider_cache, but loading_
    // prevents re-entrant loads.
    for (const auto& pe : to_encrypt) {
        std::string sql = "UPDATE sys_providers SET api_key_encrypted = '" + pe.encrypted_value +
                          "' WHERE provider_id = " + std::to_string(pe.provider_id);
        auto upd = engine.execute(sql);
        if (!upd) {
            GIODB_LOG_WARN("failed to encrypt api_key for provider_id {}: {}",
                           pe.provider_id,
                           upd.error().message);
        } else {
            GIODB_LOG_INFO("provider cache: encrypted api_key for provider_id {}", pe.provider_id);
        }
    }

    engine.pop_skip_masking();
    engine.set_current_database(prev_db);

    // Swap the new data into the cache under the lock.
    {
        std::lock_guard<std::mutex> lock(mu_);
        providers_ = std::move(new_providers);
        next_provider_id_ = new_next_id;
    }

    GIODB_LOG_INFO("provider cache: loaded {} providers", providers_.size());
    loading_ = false;
    return ok();
}

bool ProviderCache::is_loading() const {
    return loading_;
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
