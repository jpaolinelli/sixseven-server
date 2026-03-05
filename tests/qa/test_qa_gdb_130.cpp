/// @file test_qa_gdb_130.cpp
/// @brief QA adversarial tests for GDB-130: OpenAI and ONNX embedding providers.
///
/// Targets edge cases: non-numeric embedding values, non-numeric index field,
/// duplicate indices, more entries than requested, empty text in batch,
/// health check failures, registry edge cases, and verifies ONNX provider
/// is not implemented.

#include "sixseven/catalog/catalog.h"
#include "sixseven/vector/builtin_provider.h"
#include "sixseven/vector/http_client.h"
#include "sixseven/vector/ollama_provider.h"
#include "sixseven/vector/openai_provider.h"
#include "sixseven/vector/provider_registry.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace sixseven;

namespace {

class MockHttpClient : public HttpClient {
public:
    void set_post_response(int status, const std::string& body) {
        post_status_ = status;
        post_body_ = body;
    }
    void set_get_response(int status, const std::string& body) {
        get_status_ = status;
        get_body_ = body;
    }
    void set_network_error(const std::string& msg) { network_error_ = msg; }

    Result<HttpResponse>
    post(const std::string& url,
         const std::string& body,
         const std::vector<std::pair<std::string, std::string>>& headers) override {
        last_post_url_ = url;
        last_post_body_ = body;
        last_post_headers_ = headers;
        post_call_count_++;
        if (!network_error_.empty()) {
            auto err = network_error_;
            network_error_.clear();
            return make_error(StatusCode::NETWORK_ERROR, err);
        }
        HttpResponse r;
        r.status_code = post_status_;
        r.body = post_body_;
        r.content_type = "application/json";
        return ok(std::move(r));
    }

    Result<HttpResponse>
    get(const std::string& /*url*/,
        const std::vector<std::pair<std::string, std::string>>& /*headers*/) override {
        if (!network_error_.empty()) {
            auto err = network_error_;
            network_error_.clear();
            return make_error(StatusCode::NETWORK_ERROR, err);
        }
        HttpResponse r;
        r.status_code = get_status_;
        r.body = get_body_;
        r.content_type = "application/json";
        return ok(std::move(r));
    }

    std::string last_post_url_;
    std::string last_post_body_;
    std::vector<std::pair<std::string, std::string>> last_post_headers_;
    int post_call_count_ = 0;

private:
    int post_status_ = 200;
    std::string post_body_;
    int get_status_ = 200;
    std::string get_body_;
    std::string network_error_;
};

} // namespace

// ---------------------------------------------------------------------------
// OpenAI: Non-numeric values in embedding (same class of bug as GDB-241)
// ---------------------------------------------------------------------------

TEST(QA_GDB_130_OpenAI, EmbeddingContainsNonNumber) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, "bad", 0.3], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    // GDB-242: Returns PARSE_ERROR instead of throwing.
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_130_OpenAI, EmbeddingContainsNull) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, null, 0.3], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    // GDB-242: Returns PARSE_ERROR instead of throwing.
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// OpenAI: Index field edge cases
// ---------------------------------------------------------------------------

TEST(QA_GDB_130_OpenAI, IndexIsString) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2, 0.3], "index": "zero"}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    // GDB-242: Returns PARSE_ERROR instead of throwing.
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_130_OpenAI, IndexIsNegative) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2, 0.3], "index": -1}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    // -1 as size_t wraps around to a very large number — should be out of range.
    auto result = provider.embed("hello");
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB_130_OpenAI, IndexMissing) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2, 0.3]}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    // Missing index defaults to 0 — should succeed for single embed.
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 3u);
}

// ---------------------------------------------------------------------------
// OpenAI: Batch edge cases
// ---------------------------------------------------------------------------

TEST(QA_GDB_130_OpenAI, BatchWithEmptyTexts) {
    auto mock = std::make_unique<MockHttpClient>();
    // Server returns no data for empty input.
    mock->set_post_response(200, R"({"data": []})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch({});
    // Empty input with empty output should succeed.
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST(QA_GDB_130_OpenAI, BatchMoreEntriesThanRequested) {
    auto mock = std::make_unique<MockHttpClient>();
    // Asked for 1 text, server returns 2 entries.
    mock->set_post_response(200, R"({
        "data": [
            {"embedding": [0.1, 0.2, 0.3], "index": 0},
            {"embedding": [0.4, 0.5, 0.6], "index": 1}
        ]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    // Index 1 is out of range for single text — should error.
    EXPECT_FALSE(result.has_value());
    if (!result.has_value()) {
        EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    }
}

TEST(QA_GDB_130_OpenAI, BatchPartialResponse) {
    auto mock = std::make_unique<MockHttpClient>();
    // Asked for 3 texts, server only returns 2.
    mock->set_post_response(200, R"({
        "data": [
            {"embedding": [0.1, 0.2, 0.3], "index": 0},
            {"embedding": [0.4, 0.5, 0.6], "index": 2}
        ]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch({"a", "b", "c"});
    // Index 1 is missing — should error.
    EXPECT_FALSE(result.has_value());
    if (!result.has_value()) {
        EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    }
}

TEST(QA_GDB_130_OpenAI, BatchDuplicateIndices) {
    auto mock = std::make_unique<MockHttpClient>();
    // Both entries claim index 0 — the second overwrites the first,
    // but index 1 is never received.
    mock->set_post_response(200, R"({
        "data": [
            {"embedding": [0.1, 0.2, 0.3], "index": 0},
            {"embedding": [0.4, 0.5, 0.6], "index": 0}
        ]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch({"a", "b"});
    // Index 1 never received — should error.
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB_130_OpenAI, BatchMixedDimensions) {
    auto mock = std::make_unique<MockHttpClient>();
    // First embedding has 3 dims, second has 2.
    mock->set_post_response(200, R"({
        "data": [
            {"embedding": [0.1, 0.2, 0.3], "index": 0},
            {"embedding": [0.4, 0.5], "index": 1}
        ]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch({"a", "b"});
    EXPECT_FALSE(result.has_value());
    if (!result.has_value()) {
        EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    }
}

// ---------------------------------------------------------------------------
// OpenAI: HTTP error edge cases
// ---------------------------------------------------------------------------

TEST(QA_GDB_130_OpenAI, Http429RateLimit) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(429, R"({"error": {"message": "rate limit exceeded"}})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NETWORK_ERROR);
}

TEST(QA_GDB_130_OpenAI, Http401InvalidKey) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(401, R"({"error": {"message": "invalid api key"}})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

TEST(QA_GDB_130_OpenAI, Http500WithMalformedErrorBody) {
    auto mock = std::make_unique<MockHttpClient>();
    // Non-JSON body on error.
    mock->set_post_response(500, "Internal Server Error");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NETWORK_ERROR);
}

TEST(QA_GDB_130_OpenAI, Http500WithJsonErrorBody) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(500, R"({"error": {"message": "server overloaded"}})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NETWORK_ERROR);
    EXPECT_NE(result.error().message.find("server overloaded"), std::string::npos);
}

TEST(QA_GDB_130_OpenAI, EmptyResponseBody) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, "");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_130_OpenAI, TruncatedJsonResponse) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": [{"embedding": [0.1, 0.2)");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// OpenAI: Health check edge cases
// ---------------------------------------------------------------------------

TEST(QA_GDB_130_OpenAI, HealthCheckNetworkError) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_network_error("connection refused");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.health_check();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NETWORK_ERROR);
}

TEST(QA_GDB_130_OpenAI, HealthCheckDimensionMismatch) {
    auto mock = std::make_unique<MockHttpClient>();
    // Provider expects dim=3, but API returns dim=5.
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2, 0.3, 0.4, 0.5], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.health_check();
    // request_embeddings validates dimension before health_check even checks.
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// OpenAI: Authorization header verification
// ---------------------------------------------------------------------------

TEST(QA_GDB_130_OpenAI, AuthHeaderContainsBearerToken) {
    auto mock = std::make_unique<MockHttpClient>();
    auto* mock_ptr = mock.get();
    mock_ptr->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2, 0.3], "index": 0}]
    })");

    OpenAIProvider provider("sk-my-secret-key", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Verify the Authorization header was set correctly.
    bool found_auth = false;
    for (const auto& [key, value] : mock_ptr->last_post_headers_) {
        if (key == "Authorization") {
            EXPECT_EQ(value, "Bearer sk-my-secret-key");
            found_auth = true;
        }
    }
    EXPECT_TRUE(found_auth) << "Authorization header not found in request";
}

TEST(QA_GDB_130_OpenAI, RequestBodyContainsModelAndInput) {
    auto mock = std::make_unique<MockHttpClient>();
    auto* mock_ptr = mock.get();
    mock_ptr->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2, 0.3], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "my-model", 3, std::move(mock));
    auto result = provider.embed("test input text");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Verify the request body is correct JSON.
    auto json = nlohmann::json::parse(mock_ptr->last_post_body_);
    EXPECT_EQ(json["model"], "my-model");
    ASSERT_TRUE(json["input"].is_array());
    EXPECT_EQ(json["input"][0], "test input text");
}

// ---------------------------------------------------------------------------
// OpenAI: Data field edge cases
// ---------------------------------------------------------------------------

TEST(QA_GDB_130_OpenAI, DataFieldIsNotArray) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": "not an array"})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_130_OpenAI, EntryMissingEmbeddingField) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_130_OpenAI, EmbeddingFieldIsNotArray) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": 42, "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// OpenAI: Name and dimension accessors
// ---------------------------------------------------------------------------

TEST(QA_GDB_130_OpenAI, NameIncludesModel) {
    auto mock = std::make_unique<MockHttpClient>();
    OpenAIProvider provider("sk-test", "text-embedding-3-small", 1536, std::move(mock));
    EXPECT_EQ(provider.name(), "openai/text-embedding-3-small");
}

TEST(QA_GDB_130_OpenAI, DimensionMatchesConstructor) {
    auto mock = std::make_unique<MockHttpClient>();
    OpenAIProvider provider("sk-test", "test", 768, std::move(mock));
    EXPECT_EQ(provider.dimension(), 768u);
}

// ---------------------------------------------------------------------------
// Registry: OpenAI resolution edge cases
// ---------------------------------------------------------------------------

TEST(QA_GDB_130_Registry, OpenAIWithoutApiKeyFails) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    // "openai/model" parsed from name, but no API key available.
    auto result = registry.resolve("openai/text-embedding-3-small");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

TEST(QA_GDB_130_Registry, TrailingSlashInProviderName) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    // "openai/" — model part is empty, parser should reject.
    auto result = registry.resolve("openai/");
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB_130_Registry, LeadingSlashInProviderName) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    // "/model" — type part is empty, parser should reject.
    auto result = registry.resolve("/model");
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB_130_Registry, MultipleSlashesInProviderName) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    // "openai/model/extra" — first slash splits type, rest is model.
    auto result = registry.resolve("openai/model/extra");
    // Should parse as type="openai", model="model/extra" — then fail
    // because no API key.
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB_130_Registry, CacheSameProviderTwice) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    auto r1 = registry.resolve("builtin/64");
    auto r2 = registry.resolve("builtin/64");
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());

    // Same pointer — cached.
    EXPECT_EQ(r1->get(), r2->get());
}

TEST(QA_GDB_130_Registry, ClearCacheCreatesNewInstance) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    auto r1 = registry.resolve("builtin/64");
    ASSERT_TRUE(r1.has_value());
    auto* ptr1 = r1->get();

    registry.clear_cache();

    auto r2 = registry.resolve("builtin/64");
    ASSERT_TRUE(r2.has_value());
    // After clearing cache, should be a new instance.
    EXPECT_NE(r2->get(), ptr1);
}

// ---------------------------------------------------------------------------
// ONNX Provider: Verify recognized (GDB-243 implemented the ONNX provider)
// ---------------------------------------------------------------------------

TEST(QA_GDB_130_ONNX, OnnxProviderTypeRecognizedButModelNotFound) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    // The ONNX provider is now implemented (GDB-243). The registry recognizes
    // "onnx" as a valid type but fails with IO_ERROR because "model-path"
    // does not exist as a file.
    auto result = registry.resolve("onnx/model-path");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}
