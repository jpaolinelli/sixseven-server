#pragma once

#include "giodb/catalog/catalog.h"
#include "giodb/common/result.h"
#include "giodb/vector/embedding_worker.h"
#include "giodb/vector/http_client.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace giodb {

/// Resolves provider name strings (e.g., "ollama/all-minilm") into
/// concrete EmbeddingProvider instances.
///
/// Provider configurations are stored in the system catalog. The registry
/// lazily creates provider instances on first use and caches them.
///
/// Supported provider types:
///   - "ollama/<model>"  — Ollama REST API
///   - "openai/<model>"  — OpenAI Embeddings API
///   - "builtin/<dim>"   — Local hash-based provider (no network)
///
/// Usage:
/// ```
///   ProviderRegistry registry(catalog);
///   auto provider = registry.resolve("ollama/all-minilm");
/// ```
class ProviderRegistry {
public:
    explicit ProviderRegistry(Catalog& catalog);

    /// Resolve a provider name to a concrete EmbeddingProvider.
    ///
    /// First checks the instance cache, then looks up config in the catalog,
    /// then falls back to parsing the name as "type/model" and using defaults.
    ///
    /// @param name Provider name, e.g., "ollama/all-minilm" or "builtin/384".
    /// @return Shared pointer to the provider, or error.
    [[nodiscard]] Result<std::shared_ptr<EmbeddingProvider>> resolve(const std::string& name);

    /// Override the HTTP client factory (for testing).
    void set_http_client_factory(std::function<std::unique_ptr<HttpClient>()> factory);

    /// Clear the instance cache (forces re-creation on next resolve).
    void clear_cache();

private:
    /// Parse "type/model" from a provider name string.
    static Result<std::pair<std::string, std::string>> parse_provider_name(const std::string& name);

    /// Create a provider instance from a config.
    Result<std::shared_ptr<EmbeddingProvider>>
    create_provider(const EmbeddingProviderConfig& config);

    Catalog& catalog_;

    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<EmbeddingProvider>> cache_;

    std::function<std::unique_ptr<HttpClient>()> http_client_factory_;
};

} // namespace giodb
