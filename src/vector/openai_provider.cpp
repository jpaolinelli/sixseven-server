#include "giodb/vector/openai_provider.h"

#include "giodb/common/logging.h"

#include <nlohmann/json.hpp>

namespace giodb {

OpenAIProvider::OpenAIProvider(std::string api_key,
                               std::string model,
                               size_t dim,
                               std::unique_ptr<HttpClient> client,
                               std::string base_url)
    : api_key_(std::move(api_key)), model_(std::move(model)), dimension_(dim),
      client_(std::move(client)), base_url_(std::move(base_url)) {}

Result<std::vector<float>> OpenAIProvider::embed(const std::string& text) {
    auto batch_result = request_embeddings({text});
    if (!batch_result.has_value()) {
        return tl::unexpected(batch_result.error());
    }

    if (batch_result->empty()) {
        return make_error(StatusCode::INTERNAL_ERROR, "OpenAI returned empty result");
    }

    return ok(std::move(batch_result->front()));
}

Result<std::vector<std::vector<float>>>
OpenAIProvider::embed_batch(const std::vector<std::string>& texts) {
    return request_embeddings(texts);
}

std::string OpenAIProvider::name() const {
    return "openai/" + model_;
}

size_t OpenAIProvider::dimension() const {
    return dimension_;
}

Result<void> OpenAIProvider::health_check() {
    // Send a minimal embedding request to validate API key and model.
    auto result = request_embeddings({"health check"});
    if (!result.has_value()) {
        return tl::unexpected(result.error());
    }

    if (result->empty() || result->front().size() != dimension_) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "OpenAI health check dimension mismatch: expected " +
                              std::to_string(dimension_) + ", got " +
                              std::to_string(result->empty() ? 0 : result->front().size()));
    }

    GIODB_LOG_DEBUG("OpenAI health check passed: model '{}' available", model_);
    return ok();
}

Result<std::vector<std::vector<float>>>
OpenAIProvider::request_embeddings(const std::vector<std::string>& texts) {
    nlohmann::json request_body = {
        {"model", model_},
        {"input", texts},
    };

    auto url = base_url_ + "/v1/embeddings";
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", "Bearer " + api_key_},
    };

    auto response = client_->post(url, request_body.dump(), headers);
    if (!response.has_value()) {
        return tl::unexpected(response.error());
    }

    // Handle rate limiting.
    if (response->status_code == 429) {
        return make_error(StatusCode::NETWORK_ERROR, "OpenAI rate limit exceeded");
    }

    // Handle authentication errors.
    if (response->status_code == 401) {
        return make_error(StatusCode::AUTH_ERROR, "invalid OpenAI API key");
    }

    if (response->status_code != 200) {
        // Try to extract error message from response body.
        std::string detail;
        try {
            auto err_json = nlohmann::json::parse(response->body);
            if (err_json.contains("error") && err_json["error"].contains("message")) {
                detail = err_json["error"]["message"].get<std::string>();
            }
        } catch (...) {
            detail = response->body;
        }
        return make_error(StatusCode::NETWORK_ERROR,
                          "OpenAI API error (status " + std::to_string(response->status_code) +
                              "): " + detail);
    }

    nlohmann::json json;
    try {
        json = nlohmann::json::parse(response->body);
    } catch (const nlohmann::json::parse_error& e) {
        return make_error(StatusCode::PARSE_ERROR,
                          "failed to parse OpenAI response: " + std::string(e.what()));
    }

    if (!json.contains("data") || !json["data"].is_array()) {
        return make_error(StatusCode::PARSE_ERROR, "OpenAI response missing 'data' array");
    }

    auto& data = json["data"];
    std::vector<std::vector<float>> results;
    results.resize(texts.size());

    std::vector<bool> received(texts.size(), false);

    for (const auto& entry : data) {
        if (!entry.contains("embedding") || !entry["embedding"].is_array()) {
            return make_error(StatusCode::PARSE_ERROR,
                              "OpenAI response entry missing 'embedding' array");
        }

        size_t index = 0;
        if (entry.contains("index")) {
            index = entry["index"].get<size_t>();
        }

        if (index >= results.size()) {
            return make_error(StatusCode::PARSE_ERROR,
                              "OpenAI response index out of range: " + std::to_string(index));
        }

        auto& embedding_json = entry["embedding"];
        auto& vec = results[index];
        vec.reserve(embedding_json.size());
        for (const auto& val : embedding_json) {
            vec.push_back(val.get<float>());
        }

        if (vec.size() != dimension_) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "dimension mismatch: expected " + std::to_string(dimension_) +
                                  ", got " + std::to_string(vec.size()));
        }

        received[index] = true;
    }

    // Validate that all requested texts received embeddings.
    for (size_t i = 0; i < texts.size(); ++i) {
        if (!received[i]) {
            return make_error(StatusCode::PARSE_ERROR,
                              "OpenAI response missing embedding at index " + std::to_string(i));
        }
    }

    return ok(std::move(results));
}

} // namespace giodb
