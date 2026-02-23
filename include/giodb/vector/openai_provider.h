#pragma once

#include "giodb/common/result.h"
#include "giodb/vector/embedding_worker.h"
#include "giodb/vector/http_client.h"

#include <memory>
#include <string>
#include <vector>

namespace giodb {

/// Embedding provider that uses the OpenAI Embeddings API.
///
/// Calls POST /v1/embeddings with API key authentication.
/// Supports native batch embedding (multiple texts per request).
///
/// Usage:
/// ```
///   auto client = make_http_client();
///   OpenAIProvider provider("sk-...", "text-embedding-3-small", 1536,
///                           std::move(client));
///   auto vec = provider.embed("hello world");
/// ```
class OpenAIProvider : public EmbeddingProvider {
public:
    /// @param api_key  OpenAI API key (sk-...).
    /// @param model    Model name, e.g., "text-embedding-3-small".
    /// @param dim      Expected embedding dimension.
    /// @param client   HTTP client for making requests.
    /// @param base_url API base URL (default: "https://api.openai.com").
    OpenAIProvider(std::string api_key,
                   std::string model,
                   size_t dim,
                   std::unique_ptr<HttpClient> client,
                   std::string base_url = "https://api.openai.com");

    [[nodiscard]] Result<std::vector<float>> embed(const std::string& text) override;

    [[nodiscard]] Result<std::vector<std::vector<float>>>
    embed_batch(const std::vector<std::string>& texts) override;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] size_t dimension() const override;
    [[nodiscard]] Result<void> health_check() override;

private:
    /// Send a batch embedding request and parse the response.
    [[nodiscard]] Result<std::vector<std::vector<float>>>
    request_embeddings(const std::vector<std::string>& texts);

    std::string api_key_;
    std::string model_;
    size_t dimension_;
    std::unique_ptr<HttpClient> client_;
    std::string base_url_;
};

} // namespace giodb
