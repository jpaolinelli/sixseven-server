#pragma once

#include "sixseven/common/result.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace sixseven {

/// HTTP response from a request.
struct HttpResponse {
    int status_code = 0;
    std::string body;
    std::string content_type;
};

/// Abstract HTTP client interface for testability.
///
/// Providers use this to make HTTP requests. Production code uses
/// RealHttpClient (backed by cpp-httplib); tests inject a mock.
class HttpClient {
public:
    virtual ~HttpClient() = default;

    /// Send a POST request with a JSON body.
    [[nodiscard]] virtual Result<HttpResponse>
    post(const std::string& url,
         const std::string& body,
         const std::vector<std::pair<std::string, std::string>>& headers = {}) = 0;

    /// Send a GET request.
    [[nodiscard]] virtual Result<HttpResponse>
    get(const std::string& url,
        const std::vector<std::pair<std::string, std::string>>& headers = {}) = 0;
};

/// Configuration for the real HTTP client.
struct HttpClientConfig {
    std::chrono::seconds connect_timeout{5};
    std::chrono::seconds read_timeout{30};
};

/// Create a production HTTP client backed by cpp-httplib.
std::unique_ptr<HttpClient> make_http_client(HttpClientConfig config = {});

} // namespace sixseven
