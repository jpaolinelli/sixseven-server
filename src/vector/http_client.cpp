#include "sixseven/vector/http_client.h"

#include "sixseven/common/logging.h"
#include "sixseven/common/parse_utils.h"

#include <httplib.h>

#include <memory>
#include <mutex>
#include <regex>
#include <unordered_map>

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

// RealHttpClient keeps one long-lived httplib::Client per (scheme, host, port)
// so repeated requests to the same origin reuse the underlying TCP connection
// (keep-alive) instead of paying a fresh connection setup on every call.
//
// Thread-safety contract: httplib::Client is NOT safe for concurrent use from
// multiple threads on the same instance. Rather than a per-thread client pool
// (which would multiply idle sockets per host with no bound) or a full
// connection pool (more machinery than this workload needs -- vector provider
// calls are not high-QPS hot paths), RealHttpClient uses the simplest
// defensible option: a mutex-guarded map from origin to a single shared
// httplib::Client, and holds the mutex for the duration of each request.
// This serializes concurrent requests to the same host but keeps the
// connection warm across calls and requires no extra background threads or
// pool bookkeeping. Requests to different hosts proceed fully in parallel
// since each origin gets its own client + lock.
class RealHttpClient : public HttpClient {
public:
    explicit RealHttpClient(HttpClientConfig config) : config_(config) {}

    Result<HttpResponse>
    post(const std::string& url,
         const std::string& body,
         const std::vector<std::pair<std::string, std::string>>& headers) override {
        return do_request(url, headers, [&](httplib::Client& client, const ParsedUrl& parsed) {
            httplib::Headers hdrs;
            for (const auto& [key, value] : headers) {
                hdrs.emplace(key, value);
            }
            return client.Post(parsed.path, hdrs, body, "application/json");
        });
    }

    Result<HttpResponse>
    get(const std::string& url,
        const std::vector<std::pair<std::string, std::string>>& headers) override {
        return do_request(url, headers, [&](httplib::Client& client, const ParsedUrl& parsed) {
            httplib::Headers hdrs;
            for (const auto& [key, value] : headers) {
                hdrs.emplace(key, value);
            }
            return client.Get(parsed.path, hdrs);
        });
    }

private:
    struct HostEntry {
        std::mutex mutex;
        std::unique_ptr<httplib::Client> client;
    };

    /// Get (creating if needed) the shared client for this origin. The
    /// returned entry's mutex must be held by the caller while using the
    /// client, for the duration of the request.
    HostEntry& entry_for(const ParsedUrl& parsed) {
        auto base = parsed.scheme + "://" + parsed.host + ":" + std::to_string(parsed.port);

        std::lock_guard<std::mutex> map_lock(map_mutex_);
        auto it = hosts_.find(base);
        if (it == hosts_.end()) {
            auto entry = std::make_unique<HostEntry>();
            entry->client = std::make_unique<httplib::Client>(base);
            entry->client->set_keep_alive(true);
            entry->client->set_connection_timeout(config_.connect_timeout);
            entry->client->set_read_timeout(config_.read_timeout);
            it = hosts_.emplace(base, std::move(entry)).first;
        }
        return *it->second;
    }

    template <typename RequestFn>
    Result<HttpResponse>
    do_request(const std::string& url,
               const std::vector<std::pair<std::string, std::string>>& /*headers*/,
               RequestFn&& request_fn) {
        auto parsed = parse_url(url);
        if (!parsed.has_value()) {
            return tl::unexpected(parsed.error());
        }

        HostEntry& entry = entry_for(*parsed);
        std::lock_guard<std::mutex> client_lock(entry.mutex);

        auto result = request_fn(*entry.client, *parsed);
        if (!result) {
            return make_error(StatusCode::NETWORK_ERROR,
                              "HTTP request failed: " + httplib::to_string(result.error()));
        }

        HttpResponse response;
        response.status_code = result->status;
        response.body = std::move(result->body);
        response.content_type = result->get_header_value("Content-Type");
        return ok(std::move(response));
    }

    HttpClientConfig config_;
    std::mutex map_mutex_;
    std::unordered_map<std::string, std::unique_ptr<HostEntry>> hosts_;
};

} // namespace

std::unique_ptr<HttpClient> make_http_client(HttpClientConfig config) {
    return std::make_unique<RealHttpClient>(std::move(config));
}

} // namespace sixseven
