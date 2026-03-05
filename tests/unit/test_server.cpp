#include "sixseven/server/server.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>

namespace sixseven {

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

TEST_F(ServerTest, PgStartupHandshake) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    int client = connect_to(server.bound_port());
    ASSERT_GE(client, 0);

    // Build a PG v3 StartupMessage: length(4) + version(4) + "user\0test\0\0".
    std::vector<uint8_t> startup;
    std::string params = std::string("user\0test\0", 10) + '\0'; // key=user, val=test, terminator.
    uint32_t length = static_cast<uint32_t>(4 + 4 + params.size());
    startup.push_back(static_cast<uint8_t>((length >> 24) & 0xFF));
    startup.push_back(static_cast<uint8_t>((length >> 16) & 0xFF));
    startup.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
    startup.push_back(static_cast<uint8_t>(length & 0xFF));
    // Protocol version 3.0 = 196608.
    uint32_t version = 196608;
    startup.push_back(static_cast<uint8_t>((version >> 24) & 0xFF));
    startup.push_back(static_cast<uint8_t>((version >> 16) & 0xFF));
    startup.push_back(static_cast<uint8_t>((version >> 8) & 0xFF));
    startup.push_back(static_cast<uint8_t>(version & 0xFF));
    startup.insert(startup.end(), params.begin(), params.end());

    ASSERT_EQ(::send(client, startup.data(), startup.size(), 0),
              static_cast<ssize_t>(startup.size()));

    // Wait for the server response.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    char buf[4096] = {};
    ssize_t n = ::recv(client, buf, sizeof(buf), 0);
    ASSERT_GT(n, 0);

    // First message should be AuthenticationOk: 'R' + int32(8) + int32(0).
    EXPECT_EQ(static_cast<uint8_t>(buf[0]), 'R');

    // Find ReadyForQuery 'Z' somewhere in the response.
    bool found_ready = false;
    for (ssize_t i = 0; i < n; ++i) {
        if (static_cast<uint8_t>(buf[i]) == 'Z' && i + 5 < n) {
            found_ready = true;
            break;
        }
    }
    EXPECT_TRUE(found_ready);

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

} // namespace sixseven
