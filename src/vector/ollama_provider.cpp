#include "giodb/vector/ollama_provider.h"

#include "giodb/common/logging.h"

#include <nlohmann/json.hpp>

namespace giodb {

OllamaProvider::OllamaProvider(std::string base_url,
                               std::string model,
                               size_t dim,
                               std::unique_ptr<HttpClient> client)
    : base_url_(std::move(base_url)), model_(std::move(model)), dimension_(dim),
      client_(std::move(client)) {}

Result<std::vector<float>> OllamaProvider::embed(const std::string& text) {
    nlohmann::json request_body = {
        {"model", model_},
        {"prompt", text},
    };

    auto url = base_url_ + "/api/embeddings";
    auto response = client_->post(url, request_body.dump());
    if (!response.has_value()) {
        return tl::unexpected(response.error());
    }

    if (response->status_code != 200) {
        return make_error(StatusCode::NETWORK_ERROR,
                          "Ollama API returned status " + std::to_string(response->status_code) +
                              ": " + response->body);
    }

    nlohmann::json json;
    try {
        json = nlohmann::json::parse(response->body);
    } catch (const nlohmann::json::parse_error& e) {
        return make_error(StatusCode::PARSE_ERROR,
                          "failed to parse Ollama response: " + std::string(e.what()));
    }

    if (!json.contains("embedding") || !json["embedding"].is_array()) {
        return make_error(StatusCode::PARSE_ERROR, "Ollama response missing 'embedding' array");
    }

    auto& embedding_json = json["embedding"];
    std::vector<float> embedding;
    embedding.reserve(embedding_json.size());
    for (const auto& val : embedding_json) {
        embedding.push_back(val.get<float>());
    }

    if (embedding.size() != dimension_) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "dimension mismatch: expected " + std::to_string(dimension_) + ", got " +
                              std::to_string(embedding.size()));
    }

    return ok(std::move(embedding));
}

Result<std::vector<std::vector<float>>>
OllamaProvider::embed_batch(const std::vector<std::string>& texts) {
    // Ollama does not natively support batch embedding.
    // Process each text sequentially with connection reuse.
    std::vector<std::vector<float>> results;
    results.reserve(texts.size());

    for (const auto& text : texts) {
        auto result = embed(text);
        if (!result.has_value()) {
            return tl::unexpected(result.error());
        }
        results.push_back(std::move(*result));
    }

    return ok(std::move(results));
}

std::string OllamaProvider::name() const {
    return "ollama/" + model_;
}

size_t OllamaProvider::dimension() const {
    return dimension_;
}

Result<void> OllamaProvider::health_check() {
    // Call /api/tags to verify the server is running and the model is available.
    auto url = base_url_ + "/api/tags";
    auto response = client_->get(url);
    if (!response.has_value()) {
        return tl::unexpected(response.error());
    }

    if (response->status_code != 200) {
        return make_error(StatusCode::NETWORK_ERROR,
                          "Ollama health check failed with status " +
                              std::to_string(response->status_code));
    }

    nlohmann::json json;
    try {
        json = nlohmann::json::parse(response->body);
    } catch (const nlohmann::json::parse_error& e) {
        return make_error(StatusCode::PARSE_ERROR,
                          "failed to parse Ollama tags response: " + std::string(e.what()));
    }

    // Check if the model is in the list.
    if (json.contains("models") && json["models"].is_array()) {
        for (const auto& model_entry : json["models"]) {
            if (model_entry.contains("name") && model_entry["name"].is_string()) {
                auto model_name = model_entry["name"].get<std::string>();
                // Ollama model names may include tags like ":latest".
                if (model_name == model_ || model_name.starts_with(model_ + ":")) {
                    GIODB_LOG_DEBUG("Ollama health check passed: model '{}' available", model_);
                    return ok();
                }
            }
        }
    }

    return make_error(StatusCode::NOT_FOUND, "model '" + model_ + "' not found on Ollama server");
}

} // namespace giodb
