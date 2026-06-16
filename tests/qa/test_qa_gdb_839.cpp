/// @file test_qa_gdb_839.cpp
/// @brief QA adversarial tests for GDB-839: verify no coverage gap after
///        deleting the vacuous QA_GDB_129_OpenAI::BatchEmptyList test.
///
/// Focus: OpenAI provider empty-batch and edge-case batch contracts via the
/// mock HttpClient path (offline, no live API key required).

#include "sixseven/vector/http_client.h"
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
        HttpResponse r;
        r.status_code = 200;
        r.body = "{}";
        r.content_type = "application/json";
        return ok(std::move(r));
    }

    std::string last_post_url_;
    std::string last_post_body_;
    int post_call_count_ = 0;

private:
    int post_status_ = 200;
    std::string post_body_;
    std::string network_error_;
};

} // namespace

// ---------------------------------------------------------------------------
// AC: GDB_130::BatchWithEmptyTexts covers the deleted test unconditionally.
// Re-assert the identical contract here as a regression anchor for GDB-839.
// ---------------------------------------------------------------------------

TEST(QA_GDB_839_OpenAI, EmptyBatchReturnsOkEmptyVector) {
    // Strict, unconditional version of the deleted vacuous test.
    // The mock response is {"data":[]} — the correct API response for empty
    // input.  Provider must return ok(empty) unconditionally, never an error.
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": []})");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));
    auto result = provider.embed_batch({});

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST(QA_GDB_839_OpenAI, EmptyBatchNetworkErrorPropagates) {
    // If the network fails on an empty batch POST, the error must propagate —
    // the deleted vacuous test would have silently passed this case.
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_network_error("connection refused");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch({});

    // Network error must surface (not be swallowed).
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NETWORK_ERROR);
}

// ---------------------------------------------------------------------------
// Adversarial: empty string as one of the batch inputs (distinct from empty
// list). The provider sends "" to the API; the API is expected to return an
// embedding for it. Verify the provider handles a correctly-sized response.
// ---------------------------------------------------------------------------

TEST(QA_GDB_839_OpenAI, BatchContainingEmptyStringSucceedsWhenApiResponds) {
    // The provider has no obligation to reject "" at this layer — it passes
    // whatever text the caller supplies.  If the mock returns a valid 1-entry
    // response, embed_batch({""}) must succeed with size 1.
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200,
                            R"({"data": [{"embedding": [0.1, 0.2, 0.3], "index": 0}]})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch({""});

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ((*result)[0].size(), 3u);
}

TEST(QA_GDB_839_OpenAI, BatchMixedEmptyAndNonEmptyStrings) {
    // Batch of {"", "hello"} — API returns 2 embeddings.
    // Provider must map them correctly by index.
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [
            {"embedding": [0.1, 0.2, 0.3], "index": 0},
            {"embedding": [0.4, 0.5, 0.6], "index": 1}
        ]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch({"", "hello"});

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);
    EXPECT_FLOAT_EQ((*result)[0][0], 0.1F);
    EXPECT_FLOAT_EQ((*result)[1][0], 0.4F);
}

TEST(QA_GDB_839_OpenAI, BatchContainingOnlyEmptyStringsApiReturnsError) {
    // If the API rejects "" inputs (e.g. 400 Bad Request), the error must
    // propagate correctly rather than being swallowed.
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(400,
                            R"({"error": {"message": "input cannot be empty"}})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch({""});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NETWORK_ERROR);
}

// ---------------------------------------------------------------------------
// Adversarial: single-element batch (boundary between scalar and batch path).
// ---------------------------------------------------------------------------

TEST(QA_GDB_839_OpenAI, SingleElementBatchSucceeds) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200,
                            R"({"data": [{"embedding": [1.0, 0.0, 0.0], "index": 0}]})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch({"only one"});

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 1u);
    EXPECT_FLOAT_EQ((*result)[0][0], 1.0F);
}

TEST(QA_GDB_839_OpenAI, SingleElementBatchApiReturnsEmptyDataError) {
    // 1 input but API returns {"data":[]} — count mismatch must be detected.
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": []})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch({"hello"});

    // received[0] stays false → PARSE_ERROR.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// Adversarial: count mismatch — data.size() != texts.size().
// The provider must detect these cases; silent wrong-size returns are a bug.
// ---------------------------------------------------------------------------

TEST(QA_GDB_839_OpenAI, DataCountExceedsInputCountIsRejected) {
    // 1 text but data has 2 entries: second entry's index=1 is out of range
    // (results vector is size 1).  Must return PARSE_ERROR.
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [
            {"embedding": [0.1, 0.2, 0.3], "index": 0},
            {"embedding": [0.7, 0.8, 0.9], "index": 1}
        ]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch({"hello"});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_839_OpenAI, DataCountBelowInputCountIsRejected) {
    // 3 texts but data only provides 2 embeddings (index 0 and 1 received;
    // index 2 missing).  Must return PARSE_ERROR for missing slot.
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [
            {"embedding": [0.1, 0.2, 0.3], "index": 0},
            {"embedding": [0.4, 0.5, 0.6], "index": 1}
        ]
    })");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch({"a", "b", "c"});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB_839_OpenAI, DataCountZeroForNonEmptyInputIsRejected) {
    // 2 texts but data is completely empty — every received[i] stays false.
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": []})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch({"x", "y"});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// Adversarial: large batch (20 elements), all delivered — must succeed and
// maintain correct index-to-embedding mapping.
// ---------------------------------------------------------------------------

TEST(QA_GDB_839_OpenAI, LargeBatchAllDeliveredSucceeds) {
    constexpr int N = 20;
    // Build response: indices 0..N-1, each with a unique first float.
    std::string data_json = R"({"data": [)";
    for (int i = 0; i < N; ++i) {
        if (i > 0) {
            data_json += ",";
        }
        // First float == index as float so we can verify mapping.
        data_json += R"({"embedding": [)" + std::to_string(static_cast<float>(i)) +
                     R"(, 0.0, 0.0], "index": )" + std::to_string(i) + "}";
    }
    data_json += "]}";

    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, data_json);

    std::vector<std::string> texts(N, "text");
    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch(texts);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), static_cast<size_t>(N));

    for (int i = 0; i < N; ++i) {
        ASSERT_EQ((*result)[i].size(), 3u);
        EXPECT_FLOAT_EQ((*result)[i][0], static_cast<float>(i))
            << "wrong embedding at index " << i;
    }
}

TEST(QA_GDB_839_OpenAI, LargeBatchDeliveredInReverseOrderSucceeds) {
    // API may deliver entries out of order (higher indices first).
    // The index field must be used to place each embedding correctly.
    constexpr int N = 5;
    std::string data_json = R"({"data": [)";
    for (int i = N - 1; i >= 0; --i) {
        if (i < N - 1) {
            data_json += ",";
        }
        data_json += R"({"embedding": [)" + std::to_string(static_cast<float>(i)) +
                     R"(, 0.0, 0.0], "index": )" + std::to_string(i) + "}";
    }
    data_json += "]}";

    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, data_json);

    std::vector<std::string> texts(N, "text");
    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch(texts);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), static_cast<size_t>(N));

    for (int i = 0; i < N; ++i) {
        EXPECT_FLOAT_EQ((*result)[i][0], static_cast<float>(i))
            << "out-of-order delivery placed embedding at wrong slot: index " << i;
    }
}

// ---------------------------------------------------------------------------
// Adversarial: embed() delegates to request_embeddings with a single text.
// Verify that an empty-string embed call propagates errors correctly.
// ---------------------------------------------------------------------------

TEST(QA_GDB_839_OpenAI, EmbedWithEmptyStringApiRespondsWithEmbedding) {
    // embed("") should succeed if the API returns a valid embedding.
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200,
                            R"({"data": [{"embedding": [0.5, 0.5, 0.5], "index": 0}]})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("");

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 3u);
}

TEST(QA_GDB_839_OpenAI, EmbedWithEmptyStringApiReturnsEmptyDataErrors) {
    // embed("") where the API returns {"data":[]} — internal error path.
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": []})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("");

    // received[0] stays false → PARSE_ERROR from request_embeddings,
    // propagated through embed().
    ASSERT_FALSE(result.has_value());
}
