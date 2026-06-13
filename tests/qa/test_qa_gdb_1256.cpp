// QA adversarial tests for GDB-1256: stabilize ServerTest.PgStartupHandshake.
//
// The fix (1) sets auth_method="trust" in the test config so the startup
// handshake reaches ReadyForQuery, and (2) replaces a fixed-sleep single recv
// with a deadline-bounded accumulating recv loop. These tests verify the
// fix's premises hold and probe edge cases the unit test does not cover:
//   - trust auth actually drives the handshake to a *well-formed* ReadyForQuery
//     (parsed by message framing, not a naive byte scan),
//   - the default scram-sha-256 config produces a SASL challenge and never
//     reaches ReadyForQuery (the documented root cause),
//   - repeated handshakes on the same server are stable,
//   - multiple concurrent clients each complete the handshake,
//   - an ephemeral (port 0) bind yields a usable port.

#include "sixseven/common/config.h"
#include "sixseven/common/platform.h"
#include "sixseven/server/server.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace sixseven {
namespace {

class QaPgHandshake : public ::testing::Test {
protected:
    Config make_config(const std::string& auth, uint16_t port = 0, size_t max_conn = 50) {
        Config cfg = Config::load_defaults();
        cfg.port = port;
        cfg.max_connections = max_conn;
        cfg.auth_method = auth;
        return cfg;
    }

    void wait_for_running(Server& server, int timeout_ms = 3000) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    int connect_to(uint16_t port) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            sixseven_platform::socket_close(fd);
            return -1;
        }
        return fd;
    }

    std::vector<uint8_t> build_startup(const std::string& user) {
        std::vector<uint8_t> startup;
        std::string params = "user";
        params.push_back('\0');
        params += user;
        params.push_back('\0');
        params.push_back('\0'); // params terminator.
        uint32_t length = static_cast<uint32_t>(4 + 4 + params.size());
        auto push_be32 = [&startup](uint32_t v) {
            startup.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
            startup.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
            startup.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
            startup.push_back(static_cast<uint8_t>(v & 0xFF));
        };
        push_be32(length);
        push_be32(196608u); // protocol 3.0
        startup.insert(startup.end(), params.begin(), params.end());
        return startup;
    }

    void set_recv_timeout(int fd, int ms) {
#ifdef _WIN32
        DWORD t = static_cast<DWORD>(ms);
#else
        struct timeval t{};
        t.tv_sec = ms / 1000;
        t.tv_usec = (ms % 1000) * 1000;
#endif
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
    }

    // Accumulate bytes until the deadline or until `done(response)` is true.
    template <typename Pred>
    std::vector<uint8_t> recv_until(int fd, Pred done, int deadline_ms = 2000) {
        set_recv_timeout(fd, 100);
        std::vector<uint8_t> response;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            char buf[4096];
            ssize_t n = ::recv(fd, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n > 0) {
                response.insert(response.end(), buf, buf + n);
                if (done(response))
                    break;
            } else if (n == 0) {
                break;
            }
        }
        return response;
    }

    // Walk the PG backend message stream by framing ('type' + int32 len incl.
    // length field) and return true if a *well-formed* ReadyForQuery ('Z',
    // length 5) message is present. This is stricter than a raw byte scan: a
    // stray 'Z' byte inside a ParameterStatus payload will not match.
    static bool has_framed_ready_for_query(const std::vector<uint8_t>& data) {
        size_t i = 0;
        while (i + 5 <= data.size()) {
            uint8_t type = data[i];
            uint32_t len = (static_cast<uint32_t>(data[i + 1]) << 24) |
                           (static_cast<uint32_t>(data[i + 2]) << 16) |
                           (static_cast<uint32_t>(data[i + 3]) << 8) |
                           (static_cast<uint32_t>(data[i + 4]));
            if (len < 4)
                return false; // malformed framing.
            if (type == 'Z' && len == 5)
                return true;
            i += 1 + len;
        }
        return false;
    }

    // True if any complete AuthenticationSASL ('R', len, int32 subtype==10)
    // message is present in the stream.
    static bool has_sasl_challenge(const std::vector<uint8_t>& data) {
        size_t i = 0;
        while (i + 5 <= data.size()) {
            uint8_t type = data[i];
            uint32_t len = (static_cast<uint32_t>(data[i + 1]) << 24) |
                           (static_cast<uint32_t>(data[i + 2]) << 16) |
                           (static_cast<uint32_t>(data[i + 3]) << 8) |
                           (static_cast<uint32_t>(data[i + 4]));
            if (len < 4)
                return false;
            if (type == 'R' && len >= 8 && i + 9 <= data.size()) {
                uint32_t sub = (static_cast<uint32_t>(data[i + 5]) << 24) |
                               (static_cast<uint32_t>(data[i + 6]) << 16) |
                               (static_cast<uint32_t>(data[i + 7]) << 8) |
                               (static_cast<uint32_t>(data[i + 8]));
                if (sub == 10u)
                    return true;
            }
            i += 1 + len;
        }
        return false;
    }
};

// The fix's central claim: with trust auth the handshake terminates with a
// properly framed ReadyForQuery, not merely a byte that happens to be 'Z'.
TEST_F(QaPgHandshake, GDB1256_TrustReachesFramedReadyForQuery) {
    Server server(make_config("trust"));
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);
    ASSERT_TRUE(server.is_running());

    int client = connect_to(server.bound_port());
    ASSERT_GE(client, 0);

    auto startup = build_startup("test");
    ASSERT_EQ(::send(client, reinterpret_cast<const char*>(startup.data()), startup.size(), 0),
              static_cast<ssize_t>(startup.size()));

    auto response = recv_until(client, has_framed_ready_for_query);

    // In a normal CI/dev environment loopback must deliver bytes.
    ASSERT_FALSE(response.empty()) << "no handshake bytes delivered";
    EXPECT_EQ(response.front(), static_cast<uint8_t>('R')) << "first message must be AuthenticationOk";
    EXPECT_TRUE(has_framed_ready_for_query(response))
        << "trust handshake must end with a framed ReadyForQuery";

    sixseven_platform::socket_close(client);
    server.shutdown();
    t.join();
}

// Documents the root cause: the *default* config (scram-sha-256) sends a SASL
// challenge and never reaches ReadyForQuery without client credentials. This
// is exactly why the unit test had to override auth_method to "trust".
TEST_F(QaPgHandshake, GDB1256_ScramDefaultSendsSaslAndNoReadyForQuery) {
    ASSERT_EQ(Config::load_defaults().auth_method, "scram-sha-256")
        << "test premise: default auth is scram-sha-256";

    Server server(make_config("scram-sha-256"));
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);
    ASSERT_TRUE(server.is_running());

    int client = connect_to(server.bound_port());
    ASSERT_GE(client, 0);

    auto startup = build_startup("admin");
    ASSERT_EQ(::send(client, reinterpret_cast<const char*>(startup.data()), startup.size(), 0),
              static_cast<ssize_t>(startup.size()));

    // Collect for a bounded window; the server should stop at the SASL prompt.
    auto response = recv_until(client, has_sasl_challenge, 1500);

    ASSERT_FALSE(response.empty()) << "no handshake bytes delivered";
    EXPECT_TRUE(has_sasl_challenge(response))
        << "scram default must issue an AuthenticationSASL challenge";
    EXPECT_FALSE(has_framed_ready_for_query(response))
        << "scram handshake must NOT reach ReadyForQuery without credentials";

    sixseven_platform::socket_close(client);
    server.shutdown();
    t.join();
}

// Stress the stability the ticket is about: repeated handshakes on the same
// long-lived server must each complete. A flake would surface here.
TEST_F(QaPgHandshake, GDB1256_RepeatedHandshakesAreStable) {
    Server server(make_config("trust"));
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);
    ASSERT_TRUE(server.is_running());

    constexpr int ITER = 15;
    int completed = 0;
    for (int i = 0; i < ITER; ++i) {
        int client = connect_to(server.bound_port());
        ASSERT_GE(client, 0) << "connect failed on iteration " << i;
        auto startup = build_startup("test");
        ASSERT_EQ(::send(client, reinterpret_cast<const char*>(startup.data()), startup.size(), 0),
                  static_cast<ssize_t>(startup.size()));
        auto response = recv_until(client, has_framed_ready_for_query);
        ASSERT_FALSE(response.empty()) << "no bytes on iteration " << i;
        EXPECT_TRUE(has_framed_ready_for_query(response)) << "iteration " << i;
        if (has_framed_ready_for_query(response))
            ++completed;
        sixseven_platform::socket_close(client);
    }
    EXPECT_EQ(completed, ITER);

    server.shutdown();
    t.join();
}

// Multiple concurrent clients must each independently reach ReadyForQuery.
TEST_F(QaPgHandshake, GDB1256_ConcurrentHandshakesEachComplete) {
    constexpr int N = 6;
    Server server(make_config("trust", 0, 100));
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);
    ASSERT_TRUE(server.is_running());

    uint16_t port = server.bound_port();
    std::vector<int> results(N, 0);
    std::vector<std::thread> workers;
    for (int i = 0; i < N; ++i) {
        workers.emplace_back([this, port, i, &results] {
            int client = connect_to(port);
            if (client < 0)
                return;
            auto startup = build_startup("test");
            if (::send(client,
                       reinterpret_cast<const char*>(startup.data()),
                       startup.size(),
                       0) != static_cast<ssize_t>(startup.size())) {
                sixseven_platform::socket_close(client);
                return;
            }
            auto response = recv_until(client, has_framed_ready_for_query);
            if (!response.empty() && has_framed_ready_for_query(response))
                results[i] = 1;
            sixseven_platform::socket_close(client);
        });
    }
    for (auto& w : workers)
        w.join();

    int ok_count = 0;
    for (int r : results)
        ok_count += r;
    EXPECT_EQ(ok_count, N) << "every concurrent client must complete the handshake";

    server.shutdown();
    t.join();
}

// The ephemeral-port mechanism the fix relies on must yield a real bound port.
TEST_F(QaPgHandshake, GDB1256_EphemeralPortIsAssigned) {
    Server server(make_config("trust", 0));
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);
    ASSERT_TRUE(server.is_running());
    EXPECT_GT(server.bound_port(), 0) << "port 0 must resolve to a concrete ephemeral port";
    server.shutdown();
    t.join();
}

} // namespace
} // namespace sixseven
