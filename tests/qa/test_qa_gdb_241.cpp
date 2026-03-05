/// @file test_qa_gdb_241.cpp
/// @brief QA adversarial tests for GDB-241: Fix uncaught nlohmann::json
///        exception on non-numeric embedding values in OllamaProvider.
///
/// Tests the is_number() validation added to OllamaProvider::embed().
/// Also probes OpenAIProvider for the same vulnerability pattern.

#include "sixseven/vector/http_client.h"
#include "sixseven/vector/ollama_provider.h"
#include "sixseven/vector/openai_provider.h"

#include <gtest/gtest.h>

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

    Result<HttpResponse>
    post(const std::string& /*url*/,
         const std::string& /*body*/,
         const std::vector<std::pair<std::string, std::string>>& /*headers*/) override {
        HttpResponse r;
        r.status_code = post_status_;
        r.body = post_body_;
        r.content_type = "application/json";
        return ok(std::move(r));
    }

    Result<HttpResponse>
    get(const std::string& /*url*/,
        const std::vector<std::pair<std::string, std::string>>& /*headers*/) override {
        HttpResponse r;
        r.status_code = get_status_;
        r.body = get_body_;
        r.content_type = "application/json";
        return ok(std::move(r));
    }

private:
    int post_status_ = 200;
    std::string post_body_;
    int get_status_ = 200;
    std::string get_body_;
};

} // namespace

// ---------------------------------------------------------------------------
// OllamaProvider: non-numeric embedding value validation (the fix under test)
// ---------------------------------------------------------------------------

TEST(QA_GDB_241_Ollama, NonNumericStringValue) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": [0.1, "bad", 0.3]})");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    EXPECT_NE(result.error().message.find("non-numeric"), std::string::npos);
}

TEST(QA_GDB_241_Ollama, NonNumericAtFirstIndex) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": ["bad", 0.2, 0.3]})");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    // Error message should indicate index 0.
    EXPECT_NE(result.error().message.find("index 0"), std::string::npos);
}

TEST(QA_GDB_241_Ollama, NonNumericAtLastIndex) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": [0.1, 0.2, "bad"]})");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    EXPECT_NE(result.error().message.find("index 2"), std::string::npos);
}

TEST(QA_GDB_241_Ollama, BooleanValue) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": [0.1, true, 0.3]})");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    EXPECT_NE(result.error().message.find("non-numeric"), std::string::npos);
}

TEST(QA_GDB_241_Ollama, NullValue) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": [0.1, null, 0.3]})");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    EXPECT_NE(result.error().message.find("non-numeric"), std::string::npos);
}

TEST(QA_GDB_241_Ollama, NestedObjectValue) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": [0.1, {"nested": 1}, 0.3]})");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    EXPECT_NE(result.error().message.find("non-numeric"), std::string::npos);
}

TEST(QA_GDB_241_Ollama, NestedArrayValue) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": [0.1, [0.2, 0.3], 0.4]})");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    EXPECT_NE(result.error().message.find("non-numeric"), std::string::npos);
}

TEST(QA_GDB_241_Ollama, AllNonNumeric) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": ["a", "b", "c"]})");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    EXPECT_NE(result.error().message.find("index 0"), std::string::npos);
}

TEST(QA_GDB_241_Ollama, ValidIntegerValuesAccepted) {
    // Integer values are numeric and should be accepted.
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": [1, 2, 3]})");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 3u);
    EXPECT_FLOAT_EQ((*result)[0], 1.0F);
    EXPECT_FLOAT_EQ((*result)[1], 2.0F);
    EXPECT_FLOAT_EQ((*result)[2], 3.0F);
}

TEST(QA_GDB_241_Ollama, ValidNegativeFloatsAccepted) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": [-0.5, -1.0, -0.001]})");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 3u);
    EXPECT_FLOAT_EQ((*result)[0], -0.5F);
}

TEST(QA_GDB_241_Ollama, ValidZeroValuesAccepted) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": [0, 0.0, 0]})");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 3u);
}

TEST(QA_GDB_241_Ollama, NonNumericInBatchEmbedding) {
    // embed_batch calls embed() per-item, so the validation should
    // propagate through the batch path too.
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": [0.1, "bad", 0.3]})");

    OllamaProvider provider("http://localhost:11434", "test", 3, std::move(mock));
    auto result = provider.embed_batch({"hello"});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    EXPECT_NE(result.error().message.find("non-numeric"), std::string::npos);
}

TEST(QA_GDB_241_Ollama, SingleNonNumericElement) {
    // Single-element embedding array with non-numeric value.
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": ["oops"]})");

    OllamaProvider provider("http://localhost:11434", "test", 1, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// OpenAIProvider: same vulnerability pattern — no is_number() check
// ---------------------------------------------------------------------------

TEST(QA_GDB_241_OpenAI, NonNumericStringValue) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": [{"embedding": [0.1, "bad", 0.3], "index": 0}]})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    // This should return a Result error, not throw an exception.
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_241_OpenAI, BooleanValue) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": [{"embedding": [0.1, true, 0.3], "index": 0}]})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_241_OpenAI, NullValue) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": [{"embedding": [0.1, null, 0.3], "index": 0}]})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_241_OpenAI, NestedObjectValue) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": [{"embedding": [0.1, {"x": 1}, 0.3], "index": 0}]})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}
