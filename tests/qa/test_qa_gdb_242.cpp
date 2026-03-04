/// @file test_qa_gdb_242.cpp
/// @brief QA adversarial tests for GDB-242: Fix uncaught nlohmann::json
///        exceptions in OpenAIProvider.
///
/// Verifies that non-numeric embedding values and non-numeric index fields
/// return PARSE_ERROR via Result<T> instead of throwing exceptions.
/// Also probes additional edge cases around the fix boundaries.

#include "giodb/vector/http_client.h"
#include "giodb/vector/openai_provider.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace giodb;

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

/// Helper: build a valid OpenAI response JSON with given embeddings.
std::string make_response(const std::vector<std::vector<float>>& embeddings) {
    nlohmann::json data = nlohmann::json::array();
    for (size_t i = 0; i < embeddings.size(); ++i) {
        data.push_back({{"embedding", embeddings[i]}, {"index", i}});
    }
    return nlohmann::json{{"data", data}}.dump();
}

} // namespace

// ===========================================================================
// Bug 1 fix verification: non-numeric embedding values
// ===========================================================================

TEST(QA_GDB_242, EmbeddingWithStringValue) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, "bad", 0.3], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    EXPECT_NE(result.error().message.find("non-numeric"), std::string::npos)
        << "Error message: " << result.error().message;
}

TEST(QA_GDB_242, EmbeddingWithNullValue) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [null, 0.2, 0.3], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_242, EmbeddingWithBooleanValue) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, true, 0.3], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_242, EmbeddingWithObjectValue) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, {"nested": 42}, 0.3], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_242, EmbeddingWithArrayValue) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, [0.2, 0.3], 0.4], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_242, EmbeddingAllNonNumeric) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": ["a", "b", "c"], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_242, EmbeddingFirstValueNonNumeric) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": ["bad", 0.2, 0.3], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    // The error message should report index 0 for the first non-numeric value.
    EXPECT_NE(result.error().message.find("index 0"), std::string::npos)
        << "Error message: " << result.error().message;
}

TEST(QA_GDB_242, EmbeddingLastValueNonNumeric) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2, "bad"], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    // The error message should report index 2 for the last non-numeric value.
    EXPECT_NE(result.error().message.find("index 2"), std::string::npos)
        << "Error message: " << result.error().message;
}

// ===========================================================================
// Bug 2 fix verification: non-numeric index field
// ===========================================================================

TEST(QA_GDB_242, IndexIsString) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2, 0.3], "index": "zero"}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    EXPECT_NE(result.error().message.find("non-numeric"), std::string::npos)
        << "Error message: " << result.error().message;
}

TEST(QA_GDB_242, IndexIsNull) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2, 0.3], "index": null}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_242, IndexIsBoolean) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2, 0.3], "index": true}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_242, IndexIsObject) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2, 0.3], "index": {"val": 0}}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_242, IndexIsArray) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2, 0.3], "index": [0]}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// ===========================================================================
// Boundary / edge cases around the fix
// ===========================================================================

TEST(QA_GDB_242, ValidNumericEmbeddingStillWorks) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, make_response({{0.1f, 0.2f, 0.3f}}));

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 3u);
    EXPECT_FLOAT_EQ((*result)[0], 0.1f);
    EXPECT_FLOAT_EQ((*result)[1], 0.2f);
    EXPECT_FLOAT_EQ((*result)[2], 0.3f);
}

TEST(QA_GDB_242, IntegerEmbeddingValuesAccepted) {
    // Integer values in JSON are valid numbers and should be accepted.
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [1, 2, 3], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FLOAT_EQ((*result)[0], 1.0f);
    EXPECT_FLOAT_EQ((*result)[1], 2.0f);
    EXPECT_FLOAT_EQ((*result)[2], 3.0f);
}

TEST(QA_GDB_242, NegativeFloatEmbeddingValuesAccepted) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [-0.5, -1.0, -0.001], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FLOAT_EQ((*result)[0], -0.5f);
}

TEST(QA_GDB_242, ZeroEmbeddingValuesAccepted) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0, 0.0, -0.0], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

TEST(QA_GDB_242, VerySmallEmbeddingValuesAccepted) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [1e-38, -1e-38, 0.0], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

TEST(QA_GDB_242, VeryLargeEmbeddingValuesAccepted) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [1e38, -1e38, 0.0], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

// ===========================================================================
// Batch with mixed valid/invalid entries
// ===========================================================================

TEST(QA_GDB_242, BatchSecondEntryHasNonNumericEmbedding) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [
            {"embedding": [0.1, 0.2, 0.3], "index": 0},
            {"embedding": [0.4, "bad", 0.6], "index": 1}
        ]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch({"a", "b"});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_242, BatchSecondEntryHasStringIndex) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [
            {"embedding": [0.1, 0.2, 0.3], "index": 0},
            {"embedding": [0.4, 0.5, 0.6], "index": "one"}
        ]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch({"a", "b"});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// ===========================================================================
// Data entry type edge cases — entry is not an object
// ===========================================================================

TEST(QA_GDB_242, DataEntryIsNumber) {
    // If data array contains a number instead of an object,
    // entry.contains("embedding") may throw nlohmann::json::type_error.
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [42]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    // Should not crash — should return an error.
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB_242, DataEntryIsString) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": ["not an object"]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB_242, DataEntryIsNull) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [null]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB_242, DataEntryIsArray) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [[0.1, 0.2, 0.3]]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB_242, DataEntryIsBoolean) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [true]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    EXPECT_FALSE(result.has_value());
}

// ===========================================================================
// Error message quality
// ===========================================================================

TEST(QA_GDB_242, NonNumericEmbeddingErrorReportsCorrectIndex) {
    auto mock = std::make_unique<MockHttpClient>();
    // Non-numeric at position 1 (index 1 in the embedding array).
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, null, 0.3], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    // Should mention index 1 (the position of the non-numeric value).
    EXPECT_NE(result.error().message.find("index 1"), std::string::npos)
        << "Error message: " << result.error().message;
}

TEST(QA_GDB_242, NonNumericIndexErrorMessageIsInformative) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2, 0.3], "index": "abc"}]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    // Error message should not be empty.
    EXPECT_FALSE(result.error().message.empty());
}

// ===========================================================================
// Stress: large batch with one bad entry
// ===========================================================================

TEST(QA_GDB_242, LargeBatchWithBadEntryAtEnd) {
    nlohmann::json data = nlohmann::json::array();
    const size_t batch_size = 100;
    for (size_t i = 0; i < batch_size; ++i) {
        if (i == batch_size - 1) {
            // Last entry has a non-numeric value.
            data.push_back(
                {{"embedding", nlohmann::json::array({"bad", 0.2f, 0.3f})}, {"index", i}});
        } else {
            data.push_back({{"embedding", std::vector<float>{0.1f, 0.2f, 0.3f}}, {"index", i}});
        }
    }
    auto response = nlohmann::json{{"data", data}}.dump();

    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, response);

    std::vector<std::string> texts(batch_size, "text");
    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch(texts);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_242, LargeBatchWithBadIndexInMiddle) {
    nlohmann::json data = nlohmann::json::array();
    const size_t batch_size = 50;
    for (size_t i = 0; i < batch_size; ++i) {
        nlohmann::json entry;
        entry["embedding"] = std::vector<float>{0.1f, 0.2f, 0.3f};
        if (i == 25) {
            entry["index"] = "twenty-five";
        } else {
            entry["index"] = i;
        }
        data.push_back(entry);
    }
    auto response = nlohmann::json{{"data", data}}.dump();

    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, response);

    std::vector<std::string> texts(batch_size, "text");
    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch(texts);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// ===========================================================================
// Valid batch still works (regression guard)
// ===========================================================================

TEST(QA_GDB_242, ValidBatchStillWorks) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, make_response({{0.1f, 0.2f, 0.3f}, {0.4f, 0.5f, 0.6f}}));

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch({"a", "b"});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);
    EXPECT_FLOAT_EQ((*result)[0][0], 0.1f);
    EXPECT_FLOAT_EQ((*result)[1][0], 0.4f);
}

TEST(QA_GDB_242, HealthCheckWithValidResponseStillWorks) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, make_response({{0.1f, 0.2f, 0.3f}}));

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.health_check();
    ASSERT_TRUE(result.has_value()) << result.error().message;
}
