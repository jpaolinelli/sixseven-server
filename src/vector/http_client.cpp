#include "sixseven/vector/http_client.h"

#include "sixseven/common/logging.h"
#include "sixseven/common/parse_utils.h"

#include <httplib.h>

#include <regex>

namespace sixseven {

namespace {

/// Parse a URL into scheme, host, port, and path.
struct ParsedUrl {
    std::string scheme;
    std::string host;
    int port = 0;
    std::string path;
};

Result<ParsedUrl> parse_url(const std::string& url) {
    // Simple URL parser: scheme://host[:port][/path]
    static const std::regex url_regex(R"(^(https?)://([^/:]+)(?::(\d+))?(/.*)?)");

    std::smatch match;
    if (!std::regex_match(url, match, url_regex)) {
        return make_error(StatusCode::INVALID_ARGUMENT, "invalid URL: " + url);
    }

    ParsedUrl parsed;
    parsed.scheme = match[1].str();
    parsed.host = match[2].str();

    if (match[3].matched) {
        auto pv = safe_stoi(match[3].str());
        if (!pv) {
            return tl::unexpected(pv.error());
        }
        parsed.port = *pv;
    } else {
        parsed.port = (parsed.scheme == "https") ? 443 : 80;
    }

    parsed.path = match[4].matched ? match[4].str() : "/";
    if (parsed.path.empty()) {
        parsed.path = "/";
    }

    return ok(std::move(parsed));
}

class RealHttpClient : public HttpClient {
public:
    explicit RealHttpClient(HttpClientConfig config) : config_(config) {}

    Result<HttpResponse>
    post(const std::string& url,
         const std::string& body,
         const std::vector<std::pair<std::string, std::string>>& headers) override {

        auto parsed = parse_url(url);
        if (!parsed.has_value()) {
            return tl::unexpected(parsed.error());
        }

        auto base = parsed->scheme + "://" + parsed->host + ":" + std::to_string(parsed->port);
        httplib::Client client(base);
        client.set_connection_timeout(config_.connect_timeout);
        client.set_read_timeout(config_.read_timeout);

        httplib::Headers hdrs;
        for (const auto& [key, value] : headers) {
            hdrs.emplace(key, value);
        }

        auto result = client.Post(parsed->path, hdrs, body, "application/json");
        if (!result) {
            return make_error(StatusCode::NETWORK_ERROR,
                              "HTTP POST failed: " + httplib::to_string(result.error()));
        }

        HttpResponse response;
        response.status_code = result->status;
        response.body = std::move(result->body);
        auto ct = result->get_header_value("Content-Type");
        response.content_type = ct;
        return ok(std::move(response));
    }

    Result<HttpResponse>
    get(const std::string& url,
        const std::vector<std::pair<std::string, std::string>>& headers) override {

        auto parsed = parse_url(url);
        if (!parsed.has_value()) {
            return tl::unexpected(parsed.error());
        }

        auto base = parsed->scheme + "://" + parsed->host + ":" + std::to_string(parsed->port);
        httplib::Client client(base);
        client.set_connection_timeout(config_.connect_timeout);
        client.set_read_timeout(config_.read_timeout);

        httplib::Headers hdrs;
        for (const auto& [key, value] : headers) {
            hdrs.emplace(key, value);
        }

        auto result = client.Get(parsed->path, hdrs);
        if (!result) {
            return make_error(StatusCode::NETWORK_ERROR,
                              "HTTP GET failed: " + httplib::to_string(result.error()));
        }

        HttpResponse response;
        response.status_code = result->status;
        response.body = std::move(result->body);
        auto ct = result->get_header_value("Content-Type");
        response.content_type = ct;
        return ok(std::move(response));
    }

private:
    HttpClientConfig config_;
};

} // namespace

std::unique_ptr<HttpClient> make_http_client(HttpClientConfig config) {
    return std::make_unique<RealHttpClient>(std::move(config));
}

} // namespace sixseven
