/// @file test_qa_gdb_145.cpp
/// @brief QA adversarial tests for GDB-145: Graceful shutdown and server lifecycle.
///
/// Targets edge cases: shutdown with pending writes, shutdown timing,
/// concurrent shutdown from multiple threads, health info after shutdown,
/// request_shutdown vs shutdown semantics, connections during shutdown.

#include "sixseven/common/platform.h"
#include "sixseven/server/server.h"

#include <gtest/gtest.h>

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

// Tests that the server survives a client that disconnects mid-write without
// being killed by SIGPIPE.
//
// Protection layers (both must hold for the server to survive):
//   1. connection.cpp: ::send() uses MSG_NOSIGNAL on Linux so a broken-pipe
//      write returns -1/EPIPE without raising a signal.
//   2. server.cpp accept_connection: SO_NOSIGPIPE socket option on macOS/BSD
//      where MSG_NOSIGNAL is unavailable.
//   3. main.cpp:59: process-global std::signal(SIGPIPE, SIG_IGN) as a final
//      belt-and-suspenders layer.
//
// Note on SIG_DFL: we intentionally do NOT set SIGPIPE to SIG_DFL in this
// test.  The gtest binary is a single process; setting SIG_DFL while the
// server thread might be in ::send() would create a window where a stray
// SIGPIPE kills the entire test runner, making the test itself flaky and
// destructive.  Instead we assert the server-survives outcome: the server
// stays running after a client abrupt disconnect and can accept a new
// connection.  This is a meaningful functional assertion -- if the server's
// send path raised SIGPIPE and the process-global handler were missing, the
// server thread would die (or the process would crash) and is_running() would
// return false.
#if !defined(_WIN32)
TEST_F(QA145LifecycleTest, ServerSurvivesClientDisconnectMidWrite) {
    Server server(make_config(0, 50));
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);
    ASSERT_TRUE(server.is_running());

    uint16_t port = server.bound_port();
    ASSERT_GT(port, 0u);

    int c = connect_to(port);
    ASSERT_GE(c, 0) << "failed to connect to server on port " << port;

    // Send a minimal PostgreSQL startup packet (length=8, protocol=3.0, no
    // params) so the server's PG handler sees a valid request and enqueues a
    // response into its write buffer.  This forces the server to call ::send()
    // to the peer -- the exact path that MSG_NOSIGNAL (Linux) / SO_NOSIGPIPE
    // (macOS) must protect when the peer disconnects abruptly.
    const uint8_t startup[8] = {0x00, 0x00, 0x00, 0x08, 0x00, 0x03, 0x00, 0x00};
    ::send(c, reinterpret_cast<const char*>(startup), sizeof(startup), 0);

    // Give the server time to receive the startup packet and queue a response.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Set SO_LINGER with l_linger=0 on the client fd so that close() sends a
    // TCP RST rather than a graceful FIN.  The RST causes the server's pending
    // ::send() to return -1/EPIPE (or ECONNRESET) instead of completing against
    // a half-open socket, genuinely provoking the broken-pipe condition that the
    // MSG_NOSIGNAL / SO_NOSIGPIPE hardening exists to suppress.
    struct linger lg{};
    lg.l_onoff = 1;
    lg.l_linger = 0;
    ::setsockopt(c, SOL_SOCKET, SO_LINGER, &lg, static_cast<socklen_t>(sizeof(lg)));

    // RST-close the client.  The server's event loop will wake on
    // EventType::WRITE and attempt ::send() to the now-dead peer.  With
    // MSG_NOSIGNAL (Linux) / SO_NOSIGPIPE (macOS) that ::send() returns -1
    // without raising SIGPIPE; main.cpp:59 SIG_IGN is the process-level
    // backstop.  If either layer were missing, SIGPIPE would kill the process
    // and the is_running / reconnect assertions below would fail.
    sixseven_platform::socket_close(c);

    // Wait for the event loop to wake on the write event and handle the error.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Primary assertion: the server is still running (not killed by SIGPIPE).
    EXPECT_TRUE(server.is_running())
        << "server stopped after client RST-disconnect -- possible SIGPIPE kill";

    // Secondary assertion: the server can still accept a fresh connection,
    // proving the event loop and listen socket are healthy after the error.
    int c2 = connect_to(port);
    EXPECT_GE(c2, 0) << "server refused reconnect after client RST-disconnect";
    if (c2 >= 0) {
        sixseven_platform::socket_close(c2);
    }

    server.shutdown();
    t.join();
    EXPECT_FALSE(server.is_running());
}
#else
TEST_F(QA145LifecycleTest, ServerSurvivesClientDisconnectMidWrite) {
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
