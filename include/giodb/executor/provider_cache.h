#pragma once

#include "giodb/catalog/schema.h"
#include "giodb/common/result.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace giodb {

// Forward declarations.
class Catalog;
class QueryEngine;
class SecretsManager;

/// In-memory cache of sys_providers for fast provider lookups.
///
/// Loaded from the sys_providers table at startup. Automatically reloaded
/// after DML operations on sys_providers (INSERT/UPDATE/DELETE).
/// SHOW PROVIDERS reads from the cache (fast path).
///
/// When a SecretsManager is set, API keys are decrypted on load and
/// plaintext keys found in the database are encrypted in-place.
///
/// Thread safety: All public methods are protected by a mutex.
class ProviderCache {
public:
    /// Set the secrets manager for encrypting/decrypting API keys.
    /// Must be called before load(). Lifetime must exceed the cache's.
    void set_secrets_manager(const SecretsManager* secrets_manager);

    /// Load all providers from sys_providers into the cache.
    /// Requires the QueryEngine to execute SQL against the system database.
    /// If a SecretsManager is set, decrypts api_key_encrypted values and
    /// encrypts any plaintext keys found in the database.
    [[nodiscard]] Result<void> load(QueryEngine& engine);

    /// Returns true if the cache is currently being loaded.
    /// Used by QueryEngine to skip masking and cache invalidation during load.
    [[nodiscard]] bool is_loading() const;

    /// Get a provider by name. Returns nullopt if not found.
    [[nodiscard]] std::optional<ProviderConfig> get(const std::string& name) const;

    /// Get the default provider. Returns nullopt if no default is set.
    [[nodiscard]] std::optional<ProviderConfig> get_default() const;

    /// Get all providers, sorted by name.
    [[nodiscard]] std::vector<ProviderConfig> get_all() const;

    /// Validate that a provider name exists in the cache.
    /// Used when EMBEDDING columns reference a provider by name.
    [[nodiscard]] Result<void> validate_provider_exists(const std::string& name) const;

    /// Check whether a provider is referenced by any EMBEDDING columns.
    /// Returns true if any embedding column uses this provider name.
    [[nodiscard]] bool is_provider_in_use(const std::string& name, const Catalog& catalog) const;

    /// Return the number of cached providers.
    [[nodiscard]] size_t size() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, ProviderConfig> providers_;
    int32_t next_provider_id_ = 1;
    const SecretsManager* secrets_manager_ = nullptr;
    bool loading_ = false; // Prevents re-entrant loads during encryption UPDATEs.
};

} // namespace giodb
