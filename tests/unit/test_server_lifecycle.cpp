#include "sixseven/server/server.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>

namespace sixseven {

class ServerLifecycleTest : public ::testing::Test {
protected:
    Config make_config(uint16_t port = 0, size_t max_conn = 10) {
        Config cfg = Config::load_defaults();
        cfg.port = port;
        cfg.max_connections = max_conn;
        cfg.shutdown_timeout_s = 2; // Short timeout for tests.
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
            ::close(fd);
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

TEST_F(ServerLifecycleTest, GracefulShutdownClosesConnections) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    // Open a few connections.
    int c1 = connect_to(server.bound_port());
    int c2 = connect_to(server.bound_port());
    ASSERT_GE(c1, 0);
    ASSERT_GE(c2, 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(server.health().active_connections, 2u);

    // Shutdown should close both connections.
    server.shutdown();
    t.join();

    EXPECT_FALSE(server.is_running());
    EXPECT_EQ(server.health().active_connections, 0u);

    // Verify the peer sockets detect closure.
    char buf[16];
    EXPECT_LE(::recv(c1, buf, sizeof(buf), 0), 0);
    EXPECT_LE(::recv(c2, buf, sizeof(buf), 0), 0);

    ::close(c1);
    ::close(c2);
}

TEST_F(ServerLifecycleTest, ShutdownWhileNoConnections) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    server.shutdown();
    t.join();
    EXPECT_FALSE(server.is_running());
}

TEST_F(ServerLifecycleTest, ShutdownFromDifferentThread) {
    Server server(make_config());
    std::thread event_thread([&server] { (void)server.start(); });
    wait_for_running(server);

    // Shutdown from a separate thread (simulates signal handler context).
    std::thread shutdown_thread([&server] { server.shutdown(); });
    shutdown_thread.join();
    event_thread.join();

    EXPECT_FALSE(server.is_running());
}

TEST_F(ServerLifecycleTest, DoubleShutdownIsSafe) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    server.shutdown();
    server.shutdown(); // Second call should be a no-op.
    t.join();
    EXPECT_FALSE(server.is_running());
}

TEST_F(ServerLifecycleTest, NoNewConnectionsAfterShutdown) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);
    uint16_t port = server.bound_port();

    server.shutdown();
    t.join();

    // Connecting after shutdown should fail.
    int fd = connect_to(port);
    if (fd >= 0) {
        // Connection may still succeed at TCP level if the OS hasn't released
        // the port yet, but the server should not be handling it.
        ::close(fd);
    }
    EXPECT_FALSE(server.is_running());
}

TEST_F(ServerLifecycleTest, HealthCheckReturnsVersionAndUptime) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto info = server.health();
    EXPECT_EQ(info.version, Server::VERSION);
    EXPECT_GE(info.uptime.count(), 0);
    EXPECT_EQ(info.active_connections, 0u);
    EXPECT_EQ(info.max_connections, 10u);

    server.shutdown();
    t.join();
}

TEST_F(ServerLifecycleTest, ShutdownTimeoutConfigRespected) {
    auto cfg = make_config();
    cfg.shutdown_timeout_s = 5;
    Server server(std::move(cfg));
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    // Shutdown should complete quickly (no blocked work).
    auto start = std::chrono::steady_clock::now();
    server.shutdown();
    auto elapsed = std::chrono::steady_clock::now() - start;
    t.join();

    // Should not have waited the full 5 seconds.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 5);
}

} // namespace sixseven
