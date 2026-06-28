#include "sixseven/vector/provider_registry.h"

#include "sixseven/common/logging.h"
#include "sixseven/common/parse_utils.h"
#include "sixseven/vector/builtin_provider.h"
#include "sixseven/vector/ollama_provider.h"
#include "sixseven/vector/onnx_provider.h"
#include "sixseven/vector/openai_provider.h"

namespace sixseven {

ProviderRegistry::ProviderRegistry(Catalog& catalog) : catalog_(catalog) {
    // Default HTTP client factory.
    http_client_factory_ = []() { return make_http_client(); };
}

Result<std::shared_ptr<EmbeddingProvider>> ProviderRegistry::resolve(const std::string& name) {
    {
        std::lock_guard lock(mu_);
        auto it = cache_.find(name);
        if (it != cache_.end()) {
            return ok(it->second);
        }
    }

    // Look up config in the catalog.
    auto config_result = catalog_.get_embedding_provider(name);
    EmbeddingProviderConfig config;

    if (config_result.has_value()) {
        config = std::move(*config_result);
    } else {
        // Fall back to parsing "type/model" from the name.
        auto parsed = parse_provider_name(name);
        if (!parsed.has_value()) {
            return tl::unexpected(parsed.error());
        }

        config.name = name;
        config.type = parsed->first;
        config.model = parsed->second;

        // Apply defaults based on type.
        if (config.type == "ollama") {
            config.base_url = "http://localhost:11434";
        } else if (config.type == "openai") {
            config.base_url = "https://api.openai.com";
        }
    }

    auto provider = create_provider(config);
    if (!provider.has_value()) {
        return tl::unexpected(provider.error());
    }

    std::lock_guard lock(mu_);
    cache_[name] = *provider;
    return provider;
}

void ProviderRegistry::set_http_client_factory(
    std::function<std::unique_ptr<HttpClient>()> factory) {
    http_client_factory_ = std::move(factory);
}

void ProviderRegistry::clear_cache() {
    std::lock_guard lock(mu_);
    cache_.clear();
}

Result<std::pair<std::string, std::string>>
ProviderRegistry::parse_provider_name(const std::string& name) {
    auto slash_pos = name.find('/');
    if (slash_pos == std::string::npos || slash_pos == 0 || slash_pos == name.size() - 1) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "invalid provider name '" + name +
                              "': expected format 'type/model' "
                              "(e.g., 'ollama/all-minilm', 'openai/text-embedding-3-small', "
                              "'builtin/384')");
    }

    return ok(std::make_pair(name.substr(0, slash_pos), name.substr(slash_pos + 1)));
}

Result<std::shared_ptr<EmbeddingProvider>>
ProviderRegistry::create_provider(const EmbeddingProviderConfig& config) {
    if (config.type == "ollama") {
        if (config.base_url.empty()) {
            return make_error(StatusCode::INVALID_ARGUMENT, "Ollama provider requires a base_url");
        }
        auto client = http_client_factory_();
        auto provider = std::make_shared<OllamaProvider>(
            config.base_url,
            config.model,
            config.dimension > 0 ? static_cast<size_t>(config.dimension) : 384,
            std::move(client));
        SIXSEVEN_LOG_INFO(
            "created Ollama provider: model={}, url={}", config.model, config.base_url);
        return ok(std::shared_ptr<EmbeddingProvider>(std::move(provider)));
    }

    if (config.type == "openai") {
        if (config.api_key.empty()) {
            return make_error(StatusCode::AUTH_ERROR, "OpenAI provider requires an api_key");
        }
        auto client = http_client_factory_();
        auto provider = std::make_shared<OpenAIProvider>(
            config.api_key,
            config.model,
            config.dimension > 0 ? static_cast<size_t>(config.dimension) : 1536,
            std::move(client),
            config.base_url.empty() ? "https://api.openai.com" : config.base_url);
        SIXSEVEN_LOG_INFO("created OpenAI provider: model={}", config.model);
        return ok(std::shared_ptr<EmbeddingProvider>(std::move(provider)));
    }

    if (config.type == "builtin") {
        size_t dim = config.dimension > 0 ? static_cast<size_t>(config.dimension) : 384;
        // If model field looks like a number, use it as dimension.
        // Reject any leading sign or whitespace — dimension must be a plain
        // non-negative decimal integer with no prefix characters.
        if (!config.model.empty()) {
            if (config.model[0] == '-' || config.model[0] == '+' || config.model[0] == ' ' ||
                config.model[0] == '\t') {
                return make_error(StatusCode::INVALID_ARGUMENT,
                                  "builtin provider dimension must be a positive integer, got: '" +
                                      config.model + "'");
            }
            {
                // Preserve the existing trailing-character rejection: all
                // characters must be ASCII digits (no "384abc").
                bool all_digits = std::all_of(config.model.begin(), config.model.end(),
                                              [](unsigned char c) { return c >= '0' && c <= '9'; });
                if (!all_digits) {
                    return make_error(
                        StatusCode::INVALID_ARGUMENT,
                        "builtin provider dimension must be a positive integer, got: '" +
                            config.model + "'");
                }
                auto pv = safe_stoul(config.model);
                if (!pv) {
                    return make_error(StatusCode::INVALID_ARGUMENT,
                                     "builtin provider dimension must be a positive integer, got: '" +
                                         config.model + "'");
                }
                dim = *pv;
            }
        }
        auto provider = std::make_shared<BuiltinProvider>(dim);
        SIXSEVEN_LOG_INFO("created builtin provider: dimension={}", dim);
        return ok(std::shared_ptr<EmbeddingProvider>(std::move(provider)));
    }

    if (config.type == "onnx") {
        if (config.model.empty()) {
            return make_error(StatusCode::INVALID_ARGUMENT, "ONNX provider requires a model path");
        }
        size_t dim = config.dimension > 0 ? static_cast<size_t>(config.dimension) : 384;
        auto provider = create_onnx_provider(config.model, dim);
        if (!provider.has_value()) {
            return tl::unexpected(provider.error());
        }
        SIXSEVEN_LOG_INFO("created ONNX provider: model={}, dimension={}", config.model, dim);
        return ok(std::shared_ptr<EmbeddingProvider>(std::move(*provider)));
    }

    return make_error(StatusCode::INVALID_ARGUMENT, "unknown provider type: '" + config.type + "'");
}

} // namespace sixseven
