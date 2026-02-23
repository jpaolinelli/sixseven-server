#include "giodb/catalog/catalog.h"
#include "giodb/vector/builtin_provider.h"
#include "giodb/vector/http_client.h"
#include "giodb/vector/ollama_provider.h"
#include "giodb/vector/openai_provider.h"
#include "giodb/vector/provider_registry.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace giodb;

// =============================================================================
// Mock HTTP client for testing Ollama and OpenAI providers
// =============================================================================

class MockHttpClient : public HttpClient {
public:
    /// Set the response that will be returned for the next POST request.
    void set_post_response(int status, const std::string& body) {
        post_status_ = status;
        post_body_ = body;
    }

    /// Set the response that will be returned for the next GET request.
    void set_get_response(int status, const std::string& body) {
        get_status_ = status;
        get_body_ = body;
    }

    /// Make the next request fail with a network error.
    void set_network_error(const std::string& message) { network_error_ = message; }

    Result<HttpResponse>
    post(const std::string& url,
         const std::string& body,
         const std::vector<std::pair<std::string, std::string>>& headers) override {
        last_post_url_ = url;
        last_post_body_ = body;
        last_post_headers_ = headers;

        if (!network_error_.empty()) {
            auto err = network_error_;
            network_error_.clear();
            return make_error(StatusCode::NETWORK_ERROR, err);
        }

        HttpResponse response;
        response.status_code = post_status_;
        response.body = post_body_;
        response.content_type = "application/json";
        return ok(std::move(response));
    }

    Result<HttpResponse>
    get(const std::string& url,
        const std::vector<std::pair<std::string, std::string>>& headers) override {
        last_get_url_ = url;
        last_get_headers_ = headers;

        if (!network_error_.empty()) {
            auto err = network_error_;
            network_error_.clear();
            return make_error(StatusCode::NETWORK_ERROR, err);
        }

        HttpResponse response;
        response.status_code = get_status_;
        response.body = get_body_;
        response.content_type = "application/json";
        return ok(std::move(response));
    }

    std::string last_post_url_;
    std::string last_post_body_;
    std::vector<std::pair<std::string, std::string>> last_post_headers_;
    std::string last_get_url_;
    std::vector<std::pair<std::string, std::string>> last_get_headers_;

private:
    int post_status_ = 200;
    std::string post_body_;
    int get_status_ = 200;
    std::string get_body_;
    std::string network_error_;
};

// =============================================================================
// BuiltinProvider tests
// =============================================================================

TEST(BuiltinProvider, EmbedProducesDeterministicOutput) {
    BuiltinProvider provider(128);

    auto result1 = provider.embed("hello world");
    ASSERT_TRUE(result1.has_value()) << result1.error().message;

    auto result2 = provider.embed("hello world");
    ASSERT_TRUE(result2.has_value()) << result2.error().message;

    EXPECT_EQ(result1->size(), 128u);
    EXPECT_EQ(*result1, *result2);
}

TEST(BuiltinProvider, DifferentTextsDifferentVectors) {
    BuiltinProvider provider(64);

    auto r1 = provider.embed("hello world");
    auto r2 = provider.embed("goodbye universe");
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());

    EXPECT_NE(*r1, *r2);
}

TEST(BuiltinProvider, SimilarTextsMoreSimilar) {
    BuiltinProvider provider(128);

    auto r1 = provider.embed("the cat sat on the mat");
    auto r2 = provider.embed("the cat sat on the rug");
    auto r3 = provider.embed("quantum physics entanglement theory");
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r3.has_value());

    // Compute cosine similarity (vectors are unit-length).
    float sim_12 = 0.0F;
    float sim_13 = 0.0F;
    for (size_t i = 0; i < r1->size(); ++i) {
        sim_12 += (*r1)[i] * (*r2)[i];
        sim_13 += (*r1)[i] * (*r3)[i];
    }

    // Similar texts should have higher similarity than dissimilar ones.
    EXPECT_GT(sim_12, sim_13);
}

TEST(BuiltinProvider, OutputIsUnitNormalized) {
    BuiltinProvider provider(256);

    auto result = provider.embed("test normalization");
    ASSERT_TRUE(result.has_value());

    float norm = 0.0F;
    for (float v : *result) {
        norm += v * v;
    }
    norm = std::sqrt(norm);

    EXPECT_NEAR(norm, 1.0F, 1e-5F);
}

TEST(BuiltinProvider, EmptyTextReturnsError) {
    BuiltinProvider provider(64);

    auto result = provider.embed("");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(BuiltinProvider, WhitespaceOnlyReturnsError) {
    BuiltinProvider provider(64);

    auto result = provider.embed("   \t\n  ");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(BuiltinProvider, BatchEmbed) {
    BuiltinProvider provider(64);

    auto result = provider.embed_batch({"hello", "world", "test"});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 3u);

    for (const auto& vec : *result) {
        EXPECT_EQ(vec.size(), 64u);
    }
}

TEST(BuiltinProvider, DimensionAndName) {
    BuiltinProvider provider(384);

    EXPECT_EQ(provider.dimension(), 384u);
    EXPECT_EQ(provider.name(), "builtin/384");
}

TEST(BuiltinProvider, HealthCheckAlwaysSucceeds) {
    BuiltinProvider provider(64);

    auto result = provider.health_check();
    EXPECT_TRUE(result.has_value());
}

TEST(BuiltinProvider, CaseInsensitiveTokenization) {
    BuiltinProvider provider(64);

    auto r1 = provider.embed("Hello World");
    auto r2 = provider.embed("hello world");
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());

    EXPECT_EQ(*r1, *r2);
}

// =============================================================================
// OllamaProvider tests (with mock HTTP client)
// =============================================================================

TEST(OllamaProvider, EmbedSuccess) {
    auto mock = std::make_unique<MockHttpClient>();
    auto* mock_ptr = mock.get();

    // Simulate Ollama returning a 3-dimensional embedding.
    mock_ptr->set_post_response(200, R"({"embedding": [0.1, 0.2, 0.3]})");

    OllamaProvider provider("http://localhost:11434", "all-minilm", 3, std::move(mock));

    auto result = provider.embed("test input");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 3u);
    EXPECT_FLOAT_EQ((*result)[0], 0.1F);
    EXPECT_FLOAT_EQ((*result)[1], 0.2F);
    EXPECT_FLOAT_EQ((*result)[2], 0.3F);

    // Verify the URL and body.
    EXPECT_EQ(mock_ptr->last_post_url_, "http://localhost:11434/api/embeddings");
}

TEST(OllamaProvider, EmbedDimensionMismatch) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": [0.1, 0.2]})");

    OllamaProvider provider("http://localhost:11434", "all-minilm", 3, std::move(mock));

    auto result = provider.embed("test");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(OllamaProvider, EmbedHttpError) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(500, "internal server error");

    OllamaProvider provider("http://localhost:11434", "all-minilm", 3, std::move(mock));

    auto result = provider.embed("test");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NETWORK_ERROR);
}

TEST(OllamaProvider, EmbedNetworkFailure) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_network_error("connection refused");

    OllamaProvider provider("http://localhost:11434", "all-minilm", 3, std::move(mock));

    auto result = provider.embed("test");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NETWORK_ERROR);
}

TEST(OllamaProvider, EmbedInvalidJson) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, "not json");

    OllamaProvider provider("http://localhost:11434", "all-minilm", 3, std::move(mock));

    auto result = provider.embed("test");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(OllamaProvider, EmbedMissingEmbeddingField) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"result": "ok"})");

    OllamaProvider provider("http://localhost:11434", "all-minilm", 3, std::move(mock));

    auto result = provider.embed("test");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(OllamaProvider, EmbedBatch) {
    auto mock = std::make_unique<MockHttpClient>();
    // embed_batch calls embed() sequentially, so we need the mock to return
    // the same response for multiple calls.
    mock->set_post_response(200, R"({"embedding": [1.0, 2.0, 3.0]})");

    OllamaProvider provider("http://localhost:11434", "all-minilm", 3, std::move(mock));

    auto result = provider.embed_batch({"hello", "world"});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 2u);
}

TEST(OllamaProvider, HealthCheckSuccess) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_get_response(200, R"({"models": [{"name": "all-minilm:latest"}]})");

    OllamaProvider provider("http://localhost:11434", "all-minilm", 3, std::move(mock));

    auto result = provider.health_check();
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST(OllamaProvider, HealthCheckModelNotFound) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_get_response(200, R"({"models": [{"name": "other-model:latest"}]})");

    OllamaProvider provider("http://localhost:11434", "all-minilm", 3, std::move(mock));

    auto result = provider.health_check();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(OllamaProvider, HealthCheckNetworkFailure) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_network_error("connection refused");

    OllamaProvider provider("http://localhost:11434", "all-minilm", 3, std::move(mock));

    auto result = provider.health_check();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NETWORK_ERROR);
}

TEST(OllamaProvider, DimensionAndName) {
    auto mock = std::make_unique<MockHttpClient>();
    OllamaProvider provider("http://localhost:11434", "nomic-embed-text", 768, std::move(mock));

    EXPECT_EQ(provider.dimension(), 768u);
    EXPECT_EQ(provider.name(), "ollama/nomic-embed-text");
}

// =============================================================================
// OpenAIProvider tests (with mock HTTP client)
// =============================================================================

TEST(OpenAIProvider, EmbedSuccess) {
    auto mock = std::make_unique<MockHttpClient>();
    auto* mock_ptr = mock.get();

    mock_ptr->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2, 0.3], "index": 0}],
        "usage": {"prompt_tokens": 3, "total_tokens": 3}
    })");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));

    auto result = provider.embed("test input");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 3u);
    EXPECT_FLOAT_EQ((*result)[0], 0.1F);

    // Verify auth header was sent.
    bool found_auth = false;
    for (const auto& [key, value] : mock_ptr->last_post_headers_) {
        if (key == "Authorization") {
            EXPECT_EQ(value, "Bearer sk-test");
            found_auth = true;
        }
    }
    EXPECT_TRUE(found_auth);
}

TEST(OpenAIProvider, BatchEmbedSuccess) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [
            {"embedding": [0.1, 0.2, 0.3], "index": 0},
            {"embedding": [0.4, 0.5, 0.6], "index": 1}
        ]
    })");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));

    auto result = provider.embed_batch({"hello", "world"});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);
    EXPECT_FLOAT_EQ((*result)[0][0], 0.1F);
    EXPECT_FLOAT_EQ((*result)[1][0], 0.4F);
}

TEST(OpenAIProvider, AuthError) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(401, R"({"error": {"message": "Invalid API key"}})");

    OpenAIProvider provider("sk-bad", "text-embedding-3-small", 3, std::move(mock));

    auto result = provider.embed("test");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

TEST(OpenAIProvider, RateLimitError) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(429, "rate limited");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));

    auto result = provider.embed("test");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NETWORK_ERROR);
}

TEST(OpenAIProvider, DimensionMismatch) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));

    auto result = provider.embed("test");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(OpenAIProvider, InvalidJsonResponse) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, "not json");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));

    auto result = provider.embed("test");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(OpenAIProvider, MissingDataField) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"result": "ok"})");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));

    auto result = provider.embed("test");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(OpenAIProvider, NetworkFailure) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_network_error("connection timeout");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));

    auto result = provider.embed("test");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NETWORK_ERROR);
}

TEST(OpenAIProvider, ServerError) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(500, R"({"error": {"message": "Internal server error"}})");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));

    auto result = provider.embed("test");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NETWORK_ERROR);
}

TEST(OpenAIProvider, DimensionAndName) {
    auto mock = std::make_unique<MockHttpClient>();
    OpenAIProvider provider("sk-test", "text-embedding-3-small", 1536, std::move(mock));

    EXPECT_EQ(provider.dimension(), 1536u);
    EXPECT_EQ(provider.name(), "openai/text-embedding-3-small");
}

TEST(OpenAIProvider, HealthCheckSuccess) {
    auto mock = std::make_unique<MockHttpClient>();
    // Health check sends a minimal embedding request.
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2, 0.3], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));

    auto result = provider.health_check();
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST(OpenAIProvider, HealthCheckDimensionMismatch) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));

    auto result = provider.health_check();
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// ProviderRegistry tests
// =============================================================================

TEST(ProviderRegistry, ResolveBuiltinFromName) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    auto result = registry.resolve("builtin/256");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ((*result)->name(), "builtin/256");
    EXPECT_EQ((*result)->dimension(), 256u);
}

TEST(ProviderRegistry, ResolveBuiltinFromCatalog) {
    Catalog catalog;

    EmbeddingProviderConfig config;
    config.name = "my-local";
    config.type = "builtin";
    config.dimension = 512;
    ASSERT_TRUE(catalog.register_embedding_provider(config).has_value());

    ProviderRegistry registry(catalog);
    auto result = registry.resolve("my-local");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ((*result)->dimension(), 512u);
}

TEST(ProviderRegistry, ResolveOllamaFromCatalog) {
    Catalog catalog;

    EmbeddingProviderConfig config;
    config.name = "ollama/all-minilm";
    config.type = "ollama";
    config.base_url = "http://localhost:11434";
    config.model = "all-minilm";
    config.dimension = 384;
    ASSERT_TRUE(catalog.register_embedding_provider(config).has_value());

    ProviderRegistry registry(catalog);

    // Inject mock HTTP client factory.
    registry.set_http_client_factory(
        []() -> std::unique_ptr<HttpClient> { return std::make_unique<MockHttpClient>(); });

    auto result = registry.resolve("ollama/all-minilm");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ((*result)->name(), "ollama/all-minilm");
    EXPECT_EQ((*result)->dimension(), 384u);
}

TEST(ProviderRegistry, ResolveOpenAIRequiresApiKey) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    // OpenAI without API key should fail.
    auto result = registry.resolve("openai/text-embedding-3-small");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

TEST(ProviderRegistry, ResolveOpenAIFromCatalogWithApiKey) {
    Catalog catalog;

    EmbeddingProviderConfig config;
    config.name = "openai/text-embedding-3-small";
    config.type = "openai";
    config.model = "text-embedding-3-small";
    config.api_key = "sk-test-key";
    config.dimension = 1536;
    ASSERT_TRUE(catalog.register_embedding_provider(config).has_value());

    ProviderRegistry registry(catalog);
    registry.set_http_client_factory(
        []() -> std::unique_ptr<HttpClient> { return std::make_unique<MockHttpClient>(); });

    auto result = registry.resolve("openai/text-embedding-3-small");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ((*result)->dimension(), 1536u);
}

TEST(ProviderRegistry, CachesInstances) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    auto r1 = registry.resolve("builtin/128");
    auto r2 = registry.resolve("builtin/128");
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());

    // Should return the same pointer.
    EXPECT_EQ(r1->get(), r2->get());
}

TEST(ProviderRegistry, ClearCacheForceRecreation) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    auto r1 = registry.resolve("builtin/128");
    ASSERT_TRUE(r1.has_value());
    auto* ptr1 = r1->get();

    registry.clear_cache();

    auto r2 = registry.resolve("builtin/128");
    ASSERT_TRUE(r2.has_value());
    auto* ptr2 = r2->get();

    // Should be different instances after cache clear.
    EXPECT_NE(ptr1, ptr2);
}

TEST(ProviderRegistry, InvalidNameFormat) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    auto result = registry.resolve("no-slash-here");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(ProviderRegistry, UnknownProviderType) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    auto result = registry.resolve("unknown/model");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// Catalog embedding provider operations
// =============================================================================

TEST(CatalogProviders, RegisterAndGet) {
    Catalog catalog;

    EmbeddingProviderConfig config;
    config.name = "test-provider";
    config.type = "builtin";
    config.dimension = 128;

    auto reg = catalog.register_embedding_provider(config);
    ASSERT_TRUE(reg.has_value()) << reg.error().message;

    auto get = catalog.get_embedding_provider("test-provider");
    ASSERT_TRUE(get.has_value()) << get.error().message;
    EXPECT_EQ(get->name, "test-provider");
    EXPECT_EQ(get->type, "builtin");
    EXPECT_EQ(get->dimension, 128);
}

TEST(CatalogProviders, DuplicateRegistrationFails) {
    Catalog catalog;

    EmbeddingProviderConfig config;
    config.name = "dup";
    config.type = "builtin";

    ASSERT_TRUE(catalog.register_embedding_provider(config).has_value());
    auto dup = catalog.register_embedding_provider(config);
    EXPECT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, StatusCode::ALREADY_EXISTS);
}

TEST(CatalogProviders, GetNonexistent) {
    Catalog catalog;

    auto result = catalog.get_embedding_provider("nonexistent");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(CatalogProviders, ListProviders) {
    Catalog catalog;

    EmbeddingProviderConfig c1;
    c1.name = "alpha";
    c1.type = "builtin";
    ASSERT_TRUE(catalog.register_embedding_provider(c1).has_value());

    EmbeddingProviderConfig c2;
    c2.name = "beta";
    c2.type = "ollama";
    ASSERT_TRUE(catalog.register_embedding_provider(c2).has_value());

    auto list = catalog.list_embedding_providers();
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].name, "alpha"); // sorted by name
    EXPECT_EQ(list[1].name, "beta");
}

TEST(CatalogProviders, RemoveProvider) {
    Catalog catalog;

    EmbeddingProviderConfig config;
    config.name = "to-remove";
    config.type = "builtin";
    ASSERT_TRUE(catalog.register_embedding_provider(config).has_value());

    auto remove = catalog.remove_embedding_provider("to-remove");
    ASSERT_TRUE(remove.has_value()) << remove.error().message;

    auto get = catalog.get_embedding_provider("to-remove");
    EXPECT_FALSE(get.has_value());
}

TEST(CatalogProviders, RemoveNonexistent) {
    Catalog catalog;

    auto result = catalog.remove_embedding_provider("nonexistent");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}
