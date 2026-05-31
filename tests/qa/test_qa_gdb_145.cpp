/// @file test_qa_gdb_145.cpp
/// @brief QA adversarial tests for GDB-145: Graceful shutdown and server lifecycle.
///
/// Targets edge cases: shutdown with pending writes, shutdown timing,
/// concurrent shutdown from multiple threads, health info after shutdown,
/// request_shutdown vs shutdown semantics, connections during shutdown.

#include "sixseven/server/server.h"

#include <gtest/gtest.h>

#include "sixseven/common/platform.h"

#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>
#include <vector>

using namespace sixseven;

class QA145LifecycleTest : public ::testing::Test {
protected:
    Config make_config(uint16_t port = 0, size_t max_conn = 10) {
        Config cfg = Config::load_defaults();
        cfg.port = port;
        cfg.max_connections = max_conn;
        cfg.shutdown_timeout_s = 2;
        return cfg;
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

    void wait_for_running(Server& server, int timeout_ms = 2000) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};

// --------------------------------------------------------------------------
// request_shutdown vs shutdown semantics
// --------------------------------------------------------------------------

TEST_F(QA145LifecycleTest, RequestShutdownSetsFlag) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);
    EXPECT_TRUE(server.is_running());

    server.request_shutdown();
    t.join();
    EXPECT_FALSE(server.is_running());
}

TEST_F(QA145LifecycleTest, ShutdownEquivalentToRequestShutdown) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    server.shutdown();
    t.join();
    EXPECT_FALSE(server.is_running());
}

// --------------------------------------------------------------------------
// Double / triple shutdown idempotency
// --------------------------------------------------------------------------

TEST_F(QA145LifecycleTest, TripleShutdownIsSafe) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    server.shutdown();
    server.shutdown();
    server.shutdown();
    t.join();
    EXPECT_FALSE(server.is_running());
}

// --------------------------------------------------------------------------
// Concurrent shutdown from multiple threads
// --------------------------------------------------------------------------

TEST_F(QA145LifecycleTest, ConcurrentShutdownFromMultipleThreads) {
    Server server(make_config());
    std::thread event_thread([&server] { (void)server.start(); });
    wait_for_running(server);

    std::vector<std::thread> shutters;
    for (int i = 0; i < 5; ++i) {
        shutters.emplace_back([&server] { server.shutdown(); });
    }
    for (auto& t : shutters) {
        t.join();
    }
    event_thread.join();
    EXPECT_FALSE(server.is_running());
}

// --------------------------------------------------------------------------
// Shutdown timing — fast when no work
// --------------------------------------------------------------------------

TEST_F(QA145LifecycleTest, ShutdownCompletesQuicklyWithNoWork) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    auto start = std::chrono::steady_clock::now();
    server.shutdown();
    auto elapsed = std::chrono::steady_clock::now() - start;
    t.join();

    // Shutdown with no connections should be under 1 second.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1000);
}

// --------------------------------------------------------------------------
// Health info after shutdown
// --------------------------------------------------------------------------

TEST_F(QA145LifecycleTest, HealthAfterShutdownShowsZeroConnections) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    int c = connect_to(server.bound_port());
    ASSERT_GE(c, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(server.health().active_connections, 1u);

    server.shutdown();
    t.join();

    auto h = server.health();
    EXPECT_EQ(h.active_connections, 0u);
    EXPECT_FALSE(server.is_running());

    sixseven_platform::socket_close(c);
}

TEST_F(QA145LifecycleTest, HealthUptimeStopsAfterShutdown) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    server.shutdown();
    t.join();

    // After shutdown, uptime should be 0 (running_ is false, so no uptime computed).
    auto h = server.health();
    EXPECT_EQ(h.uptime.count(), 0);
}

TEST_F(QA145LifecycleTest, HealthMaxConnectionsAlwaysAvailable) {
    auto cfg = make_config(0, 42);
    Server server(std::move(cfg));
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    EXPECT_EQ(server.health().max_connections, 42u);

    server.shutdown();
    t.join();

    // max_connections is config-based, still available after shutdown.
    EXPECT_EQ(server.health().max_connections, 42u);
}

// --------------------------------------------------------------------------
// Shutdown with many connections
// --------------------------------------------------------------------------

TEST_F(QA145LifecycleTest, ShutdownClosesAllConnections) {
    Server server(make_config(0, 50));
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    std::vector<int> clients;
    for (int i = 0; i < 10; ++i) {
        int fd = connect_to(server.bound_port());
        ASSERT_GE(fd, 0) << "failed to connect on iteration " << i;
        clients.push_back(fd);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(server.health().active_connections, 10u);

    server.shutdown();
    t.join();

    EXPECT_EQ(server.health().active_connections, 0u);

    // All client sockets should detect peer closure.
    for (int fd : clients) {
        char buf[16];
        ssize_t n = ::recv(fd, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        EXPECT_LE(n, 0) << "fd " << fd << " still readable after shutdown";
        sixseven_platform::socket_close(fd);
    }
}

// --------------------------------------------------------------------------
// Server can be destructed without start()
// --------------------------------------------------------------------------

TEST_F(QA145LifecycleTest, DestructWithoutStart) {
    // Constructing and immediately destroying should not crash.
    { Server server(make_config()); }
    SUCCEED();
}

// --------------------------------------------------------------------------
// Server not running before start()
// --------------------------------------------------------------------------

TEST_F(QA145LifecycleTest, NotRunningBeforeStart) {
    Server server(make_config());
    EXPECT_FALSE(server.is_running());
    EXPECT_EQ(server.bound_port(), 0u);
}

// --------------------------------------------------------------------------
// Shutdown immediately after start (race window)
// --------------------------------------------------------------------------

TEST_F(QA145LifecycleTest, ShutdownRightAfterStartBecomesRunning) {
    Server server(make_config());

    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    // Shutdown immediately — no time for any connections.
    server.request_shutdown();
    t.join();

    EXPECT_FALSE(server.is_running());
}

// --------------------------------------------------------------------------
// Connection open during shutdown window
// --------------------------------------------------------------------------

TEST_F(QA145LifecycleTest, ConnectionDuringShutdownWindow) {
    Server server(make_config(0, 50));
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    uint16_t port = server.bound_port();

    // Open a connection, then immediately request shutdown.
    int c = connect_to(port);
    ASSERT_GE(c, 0);

    server.shutdown();
    t.join();

    EXPECT_FALSE(server.is_running());
    sixseven_platform::socket_close(c);
}

// --------------------------------------------------------------------------
// Peer writes data then server shuts down (no crash)
// --------------------------------------------------------------------------

TEST_F(QA145LifecycleTest, PeerWritesDuringShutdown) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    int c = connect_to(server.bound_port());
    ASSERT_GE(c, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send some data (not valid PG protocol, but shouldn't crash the server).
    const char* data = "GARBAGE DATA\r\n";
    ::send(c, data, std::strlen(data), 0);

    // Allow the server to process and potentially close the connection.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    server.shutdown();
    t.join();

    EXPECT_FALSE(server.is_running());
    sixseven_platform::socket_close(c);
}

// --------------------------------------------------------------------------
// SIGPIPE is ignored (write to closed socket doesn't crash)
// --------------------------------------------------------------------------

#if !defined(_WIN32)
TEST_F(QA145LifecycleTest, SigpipeIgnored) {
    // SIGPIPE should be ignored by the server. Verify it's safe to set
    // SIG_IGN as main.cpp does.
    auto prev_handler = std::signal(SIGPIPE, SIG_IGN);
    EXPECT_NE(prev_handler, SIG_ERR);

    // Create a socketpair, close one end, write to the other.
    // If SIGPIPE is properly ignored, we get EPIPE instead of a crash.
    int fds[2];
    ASSERT_EQ(sixseven_platform::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    sixseven_platform::socket_close(fds[0]);

    ssize_t n = ::write(fds[1], "x", 1);
    EXPECT_EQ(n, -1);
    EXPECT_EQ(errno, EPIPE);

    sixseven_platform::socket_close(fds[1]);

    // Restore previous handler.
    std::signal(SIGPIPE, prev_handler);
}
#else
TEST_F(QA145LifecycleTest, SigpipeIgnored) {
    GTEST_SKIP() << "SIGPIPE test is POSIX-only";
}
#endif

// --------------------------------------------------------------------------
// Bound port is valid after start
// --------------------------------------------------------------------------

TEST_F(QA145LifecycleTest, BoundPortIsValidAfterStart) {
    Server server(make_config(0));
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    uint16_t port = server.bound_port();
    EXPECT_GT(port, 0u);
    EXPECT_LE(port, 65535u);

    server.shutdown();
    t.join();
}

// --------------------------------------------------------------------------
// Version string is non-empty
// --------------------------------------------------------------------------

TEST_F(QA145LifecycleTest, VersionStringNonEmpty) {
    EXPECT_NE(Server::VERSION, nullptr);
    EXPECT_GT(std::strlen(Server::VERSION), 0u);
}

// --------------------------------------------------------------------------
// Shutdown timeout config is propagated
// --------------------------------------------------------------------------

TEST_F(QA145LifecycleTest, ShutdownTimeoutConfigPropagated) {
    auto cfg = make_config();
    cfg.shutdown_timeout_s = 7;
    Server server(std::move(cfg));
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    // The shutdown should NOT block for 7 seconds with no work.
    auto start = std::chrono::steady_clock::now();
    server.shutdown();
    auto elapsed = std::chrono::steady_clock::now() - start;
    t.join();

    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 7);
}
