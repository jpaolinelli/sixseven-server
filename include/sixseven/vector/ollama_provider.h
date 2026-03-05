#pragma once

#include "sixseven/common/result.h"
#include "sixseven/vector/embedding_worker.h"
#include "sixseven/vector/http_client.h"

#include <memory>
#include <string>
#include <vector>

namespace sixseven {

/// Embedding provider that uses the Ollama REST API.
///
/// Ollama runs local LLMs and exposes an HTTP API for embedding generation.
/// This provider calls POST /api/embeddings for each text input.
///
/// Usage:
/// ```
///   auto client = make_http_client();
///   OllamaProvider provider("http://localhost:11434", "all-minilm", 384,
///                           std::move(client));
///   auto vec = provider.embed("hello world");
/// ```
class OllamaProvider : public EmbeddingProvider {
public:
    /// @param base_url Ollama server URL, e.g., "http://localhost:11434".
    /// @param model    Model name, e.g., "all-minilm", "nomic-embed-text".
    /// @param dim      Expected embedding dimension.
    /// @param client   HTTP client for making requests.
    OllamaProvider(std::string base_url,
                   std::string model,
                   size_t dim,
                   std::unique_ptr<HttpClient> client);

    [[nodiscard]] Result<std::vector<float>> embed(const std::string& text) override;

    [[nodiscard]] Result<std::vector<std::vector<float>>>
    embed_batch(const std::vector<std::string>& texts) override;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] size_t dimension() const override;
    [[nodiscard]] Result<void> health_check() override;

private:
    std::string base_url_;
    std::string model_;
    size_t dimension_;
    std::unique_ptr<HttpClient> client_;
};

} // namespace sixseven
