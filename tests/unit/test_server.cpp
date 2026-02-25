#include "giodb/server/server.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>

namespace giodb {

class ServerTest : public ::testing::Test {
protected:
    Config make_config(uint16_t port = 0, size_t max_conn = 10) {
        Config cfg = Config::load_defaults();
        cfg.port = port; // 0 = let OS pick an ephemeral port.
        cfg.max_connections = max_conn;
        return cfg;
    }

    /// Connect a raw TCP client to localhost:port. Returns the socket fd.
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

TEST_F(ServerTest, StartAndShutdown) {
    Server server(make_config());

    std::thread t([&server] { auto r = server.start(); });

    wait_for_running(server);
    ASSERT_TRUE(server.is_running());
    EXPECT_GT(server.bound_port(), 0);

    server.shutdown();
    t.join();
    EXPECT_FALSE(server.is_running());
}

TEST_F(ServerTest, AcceptsSingleConnection) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    int client = connect_to(server.bound_port());
    ASSERT_GE(client, 0);

    // Give the server time to accept.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto info = server.health();
    EXPECT_EQ(info.active_connections, 1u);

    ::close(client);
    server.shutdown();
    t.join();
}

TEST_F(ServerTest, AcceptsMultipleConcurrentConnections) {
    constexpr int NUM_CLIENTS = 5;
    Server server(make_config(0, 100));
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    std::vector<int> clients;
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        int fd = connect_to(server.bound_port());
        ASSERT_GE(fd, 0) << "client " << i << " failed to connect";
        clients.push_back(fd);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto info = server.health();
    EXPECT_EQ(info.active_connections, static_cast<size_t>(NUM_CLIENTS));

    for (int fd : clients) {
        ::close(fd);
    }
    server.shutdown();
    t.join();
}

TEST_F(ServerTest, RejectsWhenMaxConnectionsReached) {
    Server server(make_config(0, 2));
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    int c1 = connect_to(server.bound_port());
    int c2 = connect_to(server.bound_port());
    ASSERT_GE(c1, 0);
    ASSERT_GE(c2, 0);

    // Give time to accept.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(server.health().active_connections, 2u);

    // Third connection should be accepted by TCP but closed by server.
    int c3 = connect_to(server.bound_port());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // The connection may have been accepted at TCP level but the server closes it.
    // Verify the server still only has 2 active connections.
    EXPECT_EQ(server.health().active_connections, 2u);

    if (c3 >= 0)
        ::close(c3);
    ::close(c2);
    ::close(c1);
    server.shutdown();
    t.join();
}

TEST_F(ServerTest, EchoesData) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    int client = connect_to(server.bound_port());
    ASSERT_GE(client, 0);

    const std::string msg = "hello server";
    ASSERT_EQ(::send(client, msg.data(), msg.size(), 0), static_cast<ssize_t>(msg.size()));

    // Wait for echo.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    char buf[256] = {};
    ssize_t n = ::recv(client, buf, sizeof(buf), 0);
    ASSERT_GT(n, 0);
    EXPECT_EQ(std::string(buf, static_cast<size_t>(n)), msg);

    ::close(client);
    server.shutdown();
    t.join();
}

TEST_F(ServerTest, HealthReturnsCorrectInfo) {
    Server server(make_config(0, 50));
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    auto info = server.health();
    EXPECT_EQ(info.version, Server::VERSION);
    EXPECT_GE(info.uptime.count(), 0);
    EXPECT_EQ(info.active_connections, 0u);
    EXPECT_EQ(info.max_connections, 50u);

    server.shutdown();
    t.join();
}

TEST_F(ServerTest, HandlesClientDisconnect) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    int client = connect_to(server.bound_port());
    ASSERT_GE(client, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(server.health().active_connections, 1u);

    // Disconnect.
    ::close(client);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Server should have cleaned up.
    EXPECT_EQ(server.health().active_connections, 0u);

    server.shutdown();
    t.join();
}

} // namespace giodb
