#pragma once

/// @file qa_mock_http_client.h
/// @brief Shared MockHttpClient for QA tests that exercise embedding providers.
///
/// Extracted from test_qa_gdb_129.cpp and test_qa_gdb_130.cpp as part of
/// GDB-857 (de-duplicate copy-pasted mock).  This is the superset: it captures
/// POST url, body, AND headers, plus a call counter.
///
/// ODR safety: QaMockHttpClient is a class defined in namespace sixseven.
/// Class definitions may appear in multiple TUs as long as they are identical
/// (ODR, basic.def p1); all member functions defined inside the class body are
/// implicitly inline.  Including this header from several .cpp files that link
/// into the same test binary is therefore safe.
///
/// Typical usage — add to each including .cpp's anonymous namespace:
/// @code
///   #include "qa_mock_http_client.h"
///   namespace {
///   using MockHttpClient = sixseven::QaMockHttpClient;
///   } // namespace
/// @endcode

#include "sixseven/common/result.h"
#include "sixseven/vector/http_client.h"

#include <string>
#include <vector>

namespace sixseven {

/// Mock HTTP client for offline QA tests of embedding providers.
///
/// All member functions are defined inline so this header is safe to include
/// from multiple translation units in the same binary.
class QaMockHttpClient : public HttpClient {
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

    [[nodiscard]] Result<HttpResponse>
    post(const std::string& url,
         const std::string& body,
         const std::vector<std::pair<std::string, std::string>>& headers) override {
        last_post_url_ = url;
        last_post_body_ = body;
        last_post_headers_ = headers;
        ++post_call_count_;
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

    [[nodiscard]] Result<HttpResponse>
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

    // Captured state — public so test assertions can read them directly.
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

} // namespace sixseven
