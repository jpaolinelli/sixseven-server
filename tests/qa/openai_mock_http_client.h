/// @file openai_mock_http_client.h
/// @brief Shared MockHttpClient for OpenAIProvider/OllamaProvider QA tests.
///
/// Extracted from test_qa_gdb_242.cpp, test_qa_gdb_241.cpp, and
/// test_qa_gdb_244_245_246.cpp to eliminate triple-maintained identical
/// mock definitions (GDB-1154).
///
/// Include this header inside an anonymous namespace in each QA file that
/// needs a mock HTTP client:
///
///   namespace {
///   #include "openai_mock_http_client.h"
///   } // namespace

#pragma once

#include "sixseven/vector/http_client.h"

#include <string>
#include <utility>
#include <vector>

namespace sixseven {

/// Minimal mock HTTP client for vector-provider QA tests.
/// Supports configurable POST and GET responses.
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

} // namespace sixseven
