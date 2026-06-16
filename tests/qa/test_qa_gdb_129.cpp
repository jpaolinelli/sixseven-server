/// @file test_qa_gdb_129.cpp
/// @brief QA adversarial tests for GDB-129: EmbeddingProvider interface and
///        OllamaProvider.
///
/// Targets edge cases: truncated/malformed JSON, empty embedding arrays, very
/// long text, unicode, batch with empty strings, partial batch failure, provider
/// name parsing, health check edge cases, zero-dimension provider.

#include "sixseven/catalog/catalog.h"
#include "sixseven/vector/builtin_provider.h"
#include "sixseven/vector/http_client.h"
#include "sixseven/vector/ollama_provider.h"
#include "sixseven/vector/openai_provider.h"
#include "sixseven/vector/provider_registry.h"

#include <gtest/gtest.h>

#include <cmath>
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
         const std::vector<std::pair<std::string, std::string>>& /*headers*/) override {
        last_post_url_ = url;
        last_post_body_ = body;
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
// OllamaProvider: malformed response edge cases
// ---------------------------------------------------------------------------

TEST(QA_GDB_129_Ollama, EmptyResponseBody) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, "");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB_129_Ollama, EmptyEmbeddingArray) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": []})");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB_129_Ollama, EmbeddingNotArray) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": "not an array"})");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB_129_Ollama, EmbeddingContainsNonNumber) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": [0.1, "bad", 0.3]})");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    EXPECT_NE(result.error().message.find("non-numeric"), std::string::npos);
}

TEST(QA_GDB_129_Ollama, TruncatedJson) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": [0.1, 0.2)");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_129_Ollama, ExtraDimensionInResponse) {
    auto mock = std::make_unique<MockHttpClient>();
    // Provider expects dim=3, response has dim=5.
    mock->set_post_response(200, R"({"embedding": [0.1, 0.2, 0.3, 0.4, 0.5]})");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB_129_Ollama, HttpStatus404) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(404, R"({"error": "model not found"})");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB_129_Ollama, BatchWithSingleItem) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": [1.0, 2.0, 3.0]})");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed_batch({"hello"});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 1u);
    EXPECT_EQ((*result)[0].size(), 3u);
}

TEST(QA_GDB_129_Ollama, BatchCallsPostPerItem) {
    auto mock = std::make_unique<MockHttpClient>();
    auto* mock_ptr = mock.get();
    mock_ptr->set_post_response(200, R"({"embedding": [1.0, 2.0]})");

    OllamaProvider provider("http://localhost:11434", "test", 2, std::move(mock));
    auto result = provider.embed_batch({"a", "b", "c"});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 3u);
    // Ollama doesn't support native batch, so it should call POST 3 times.
    EXPECT_EQ(mock_ptr->post_call_count_, 3);
}

TEST(QA_GDB_129_Ollama, HealthCheckEmptyModelList) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_get_response(200, R"({"models": []})");

    OllamaProvider provider("http://localhost:11434", "test-model", 3, std::move(mock));
    auto result = provider.health_check();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_GDB_129_Ollama, HealthCheckMalformedJson) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_get_response(200, "not json at all");

    OllamaProvider provider("http://localhost:11434", "test-model", 3, std::move(mock));
    auto result = provider.health_check();
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB_129_Ollama, HealthCheckHttp500) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_get_response(500, "internal server error");

    OllamaProvider provider("http://localhost:11434", "test-model", 3, std::move(mock));
    auto result = provider.health_check();
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// BuiltinProvider edge cases
// ---------------------------------------------------------------------------

TEST(QA_GDB_129_Builtin, VeryLongText) {
    BuiltinProvider provider(64);
    // 10KB of text.
    std::string long_text(10000, 'a');
    auto result = provider.embed(long_text);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 64u);

    // Should be unit normalized.
    float norm = 0.0F;
    for (float v : *result) {
        norm += v * v;
    }
    EXPECT_NEAR(std::sqrt(norm), 1.0F, 1e-5F);
}

TEST(QA_GDB_129_Builtin, UnicodeText) {
    BuiltinProvider provider(64);
    // BuiltinProvider uses ASCII-only tokenization, so pure-unicode text
    // yields no tokens and returns an error.  This is expected behaviour.
    auto result = provider.embed("日本語テスト");
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB_129_Builtin, SpecialCharactersInText) {
    BuiltinProvider provider(64);
    auto result = provider.embed("hello!@#$%^&*()_+-=[]{}|;':\",./<>?");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 64u);
}

TEST(QA_GDB_129_Builtin, SingleCharacter) {
    BuiltinProvider provider(64);
    auto result = provider.embed("a");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 64u);
}

TEST(QA_GDB_129_Builtin, BatchEmptyList) {
    BuiltinProvider provider(64);
    auto result = provider.embed_batch({});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST(QA_GDB_129_Builtin, BatchWithEmptyString) {
    BuiltinProvider provider(64);
    // Batch with one empty string should fail.
    auto result = provider.embed_batch({"hello", ""});
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB_129_Builtin, SmallDimension) {
    BuiltinProvider provider(1);
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 1u);
    // Single dimension, normalized to 1 or -1.
    EXPECT_NEAR(std::fabs((*result)[0]), 1.0F, 1e-5F);
}

TEST(QA_GDB_129_Builtin, LargeDimension) {
    BuiltinProvider provider(4096);
    auto result = provider.embed("test large dimension embedding");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 4096u);

    float norm = 0.0F;
    for (float v : *result) {
        norm += v * v;
    }
    EXPECT_NEAR(std::sqrt(norm), 1.0F, 1e-4F);
}

// ---------------------------------------------------------------------------
// ProviderRegistry edge cases
// ---------------------------------------------------------------------------

TEST(QA_GDB_129_Registry, InvalidProviderName) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    // No slash separator.
    auto result = registry.resolve("invalid-name");
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB_129_Registry, EmptyProviderName) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    auto result = registry.resolve("");
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB_129_Registry, UnknownProviderType) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    auto result = registry.resolve("unknown/model");
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB_129_Registry, BuiltinWithNonNumericDimension) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    // "builtin/abc" — the model field "abc" is not a valid positive integer,
    // so the registry must reject it with INVALID_ARGUMENT.
    auto result = registry.resolve("builtin/abc");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB_129_Registry, BuiltinWithFloatDimension) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    // "builtin/3.14" — a float string is not a valid dimension; must fail.
    auto result = registry.resolve("builtin/3.14");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB_129_Registry, BuiltinWithAlphanumericDimension) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    // "builtin/128abc" — partially numeric but not a clean integer; must fail.
    auto result = registry.resolve("builtin/128abc");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB_129_Registry, DifferentBuiltinDimensionsDifferentInstances) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    auto r1 = registry.resolve("builtin/64");
    auto r2 = registry.resolve("builtin/128");
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());

    EXPECT_EQ((*r1)->dimension(), 64u);
    EXPECT_EQ((*r2)->dimension(), 128u);
    EXPECT_NE(r1->get(), r2->get()); // Different instances.
}

// ---------------------------------------------------------------------------
// OpenAI edge cases
// ---------------------------------------------------------------------------

TEST(QA_GDB_129_OpenAI, EmptyDataArray) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": []})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB_129_OpenAI, BatchEmptyList) {
    auto mock = std::make_unique<MockHttpClient>();
    // Empty batch should handle gracefully.
    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch({});
    // Either succeeds with empty result or returns error.
    if (result.has_value()) {
        EXPECT_TRUE(result->empty());
    }
}

TEST(QA_GDB_129_OpenAI, LargeBatch) {
    auto mock = std::make_unique<MockHttpClient>();

    // Build response with 20 embeddings.
    std::string data = R"({"data": [)";
    for (int i = 0; i < 20; ++i) {
        if (i > 0) {
            data += ",";
        }
        data += R"({"embedding": [0.1, 0.2, 0.3], "index": )" + std::to_string(i) + "}";
    }
    data += "]}";
    mock->set_post_response(200, data);

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));

    std::vector<std::string> texts(20, "test");
    auto result = provider.embed_batch(texts);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 20u);
}

TEST(QA_GDB_129_OpenAI, CustomBaseUrl) {
    auto mock = std::make_unique<MockHttpClient>();
    auto* mock_ptr = mock.get();
    mock_ptr->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2, 0.3], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock), "https://custom.endpoint.com");
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(mock_ptr->last_post_url_, "https://custom.endpoint.com/v1/embeddings");
}
