#include "sixseven/vector/http_client.h"

#include <gtest/gtest.h>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace sixseven {
namespace {

/// A tiny local HTTP server for exercising RealHttpClient end-to-end.
/// Counts distinct TCP connections it accepts (via httplib's per-connection
/// socket callback) as well as the number of logical requests handled, so
/// tests can assert that repeated requests reuse a single connection.
class TestServer {
public:
    TestServer() {
        server_.set_keep_alive_max_count(100);

        server_.Get("/ping", [this](const httplib::Request&, httplib::Response& res) {
            request_count_.fetch_add(1);
            res.set_content("pong", "text/plain");
        });

        server_.Post("/echo", [this](const httplib::Request& req, httplib::Response& res) {
            request_count_.fetch_add(1);
            res.set_content(req.body, "application/json");
        });

        server_.Get("/slow", [this](const httplib::Request&, httplib::Response& res) {
            request_count_.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            res.set_content("done", "text/plain");
        });

        // httplib invokes this once per accepted socket, letting us count
        // distinct TCP connections separately from logical requests.
        server_.set_socket_options([this](socket_t) { connection_count_.fetch_add(1); });

        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this]() { server_.listen_after_bind(); });

        // Wait for the server to actually be ready to accept connections.
        for (int i = 0; i < 200 && !server_.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ~TestServer() {
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;

    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    [[nodiscard]] int connection_count() const { return connection_count_.load(); }
    [[nodiscard]] int request_count() const { return request_count_.load(); }

private:
    httplib::Server server_;
    std::thread thread_;
    int port_ = 0;
    std::atomic<int> connection_count_{0};
    std::atomic<int> request_count_{0};
};

TEST(RealHttpClient, GetReturnsExpectedBodyAndStatus) {
    TestServer server;
    auto client = make_http_client();

    auto response = client->get(server.base_url() + "/ping");

    ASSERT_TRUE(response.has_value()) << response.error().message;
    EXPECT_EQ(response->status_code, 200);
    EXPECT_EQ(response->body, "pong");
}

TEST(RealHttpClient, PostReturnsExpectedBodyAndStatus) {
    TestServer server;
    auto client = make_http_client();

    auto response = client->post(server.base_url() + "/echo", R"({"a":1})");

    ASSERT_TRUE(response.has_value()) << response.error().message;
    EXPECT_EQ(response->status_code, 200);
    EXPECT_EQ(response->body, R"({"a":1})");
}

TEST(RealHttpClient, RepeatedRequestsToSameHostReuseConnection) {
    TestServer server;
    auto client = make_http_client();

    constexpr int kRequests = 10;
    for (int i = 0; i < kRequests; ++i) {
        auto response = client->get(server.base_url() + "/ping");
        ASSERT_TRUE(response.has_value()) << response.error().message;
        EXPECT_EQ(response->status_code, 200);
    }

    EXPECT_EQ(server.request_count(), kRequests);
    // Keep-alive reuse means far fewer TCP connections than requests. Allow
    // some slack (e.g. one initial connection) but assert clear reuse.
    EXPECT_LT(server.connection_count(), kRequests);
}

TEST(RealHttpClient, EmbedBatchStyleLoopReusesConnection) {
    // Simulates OllamaProvider::embed_batch: many sequential POSTs to the
    // same host through one HttpClient instance.
    TestServer server;
    auto client = make_http_client();

    constexpr int kTexts = 8;
    for (int i = 0; i < kTexts; ++i) {
        auto response = client->post(server.base_url() + "/echo", "text-" + std::to_string(i));
        ASSERT_TRUE(response.has_value()) << response.error().message;
    }

    EXPECT_EQ(server.request_count(), kTexts);
    EXPECT_LT(server.connection_count(), kTexts);
}

TEST(RealHttpClient, ConcurrentRequestsToSameHostAllSucceed) {
    // Thread-safety test: RealHttpClient serializes access to a shared
    // per-host httplib::Client via a mutex, so concurrent callers must not
    // crash, deadlock, or corrupt responses -- every request should get back
    // exactly its own echoed body.
    TestServer server;
    auto client = make_http_client();

    constexpr int kThreads = 8;
    std::vector<std::thread> threads;
    std::vector<Result<HttpResponse>> results(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i]() {
            std::string body = "payload-" + std::to_string(i);
            results[static_cast<size_t>(i)] = client->post(server.base_url() + "/echo", body);
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    for (int i = 0; i < kThreads; ++i) {
        const auto& response = results[static_cast<size_t>(i)];
        ASSERT_TRUE(response.has_value()) << response.error().message;
        EXPECT_EQ(response->status_code, 200);
        EXPECT_EQ(response->body, "payload-" + std::to_string(i));
    }

    EXPECT_EQ(server.request_count(), kThreads);
}

TEST(RealHttpClient, ConcurrentRequestsToDifferentHostsDoNotBlockEachOther) {
    // Separate origins get separate host entries (separate mutexes), so
    // concurrent traffic to different hosts should not serialize on a
    // single global lock. Verified indirectly: both servers see all their
    // requests complete successfully.
    TestServer server_a;
    TestServer server_b;
    auto client = make_http_client();

    constexpr int kThreads = 4;
    std::vector<std::thread> threads;
    std::vector<Result<HttpResponse>> results_a(kThreads);
    std::vector<Result<HttpResponse>> results_b(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i]() {
            results_a[static_cast<size_t>(i)] = client->get(server_a.base_url() + "/slow");
        });
        threads.emplace_back([&, i]() {
            results_b[static_cast<size_t>(i)] = client->get(server_b.base_url() + "/slow");
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    for (int i = 0; i < kThreads; ++i) {
        ASSERT_TRUE(results_a[static_cast<size_t>(i)].has_value());
        ASSERT_TRUE(results_b[static_cast<size_t>(i)].has_value());
    }

    EXPECT_EQ(server_a.request_count(), kThreads);
    EXPECT_EQ(server_b.request_count(), kThreads);
}

TEST(RealHttpClient, DifferentHostsGetSeparateConnections) {
    TestServer server_a;
    TestServer server_b;
    auto client = make_http_client();

    auto response_a = client->get(server_a.base_url() + "/ping");
    auto response_b = client->get(server_b.base_url() + "/ping");

    ASSERT_TRUE(response_a.has_value());
    ASSERT_TRUE(response_b.has_value());
    EXPECT_EQ(response_a->body, "pong");
    EXPECT_EQ(response_b->body, "pong");

    // Each host should have accepted its own connection(s); the two servers
    // are independent listeners so there is no cross-talk.
    EXPECT_GE(server_a.connection_count(), 1);
    EXPECT_GE(server_b.connection_count(), 1);
}

TEST(RealHttpClient, TimeoutConfigIsAppliedAndUnreachableHostFails) {
    HttpClientConfig config;
    config.connect_timeout = std::chrono::seconds(1);
    config.read_timeout = std::chrono::seconds(1);
    auto client = make_http_client(config);

    // 10.255.255.1 is a non-routable address commonly used to force a
    // connection timeout rather than an immediate refusal.
    auto start = std::chrono::steady_clock::now();
    auto response = client->get("http://10.255.255.1:9/ping");
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(response.has_value());
    // The call must fail (not hang) and should respect the short configured
    // timeout rather than some much longer default.
    EXPECT_LT(elapsed, std::chrono::seconds(15));
}

TEST(RealHttpClient, InvalidUrlReturnsError) {
    auto client = make_http_client();

    auto response = client->get("not-a-valid-url");

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, StatusCode::INVALID_ARGUMENT);
}

} // namespace
} // namespace sixseven
