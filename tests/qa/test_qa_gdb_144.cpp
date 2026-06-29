/// @file test_qa_gdb_144.cpp
/// @brief QA adversarial tests for GDB-144: Event-driven TCP server.
///
/// Targets edge cases: event loop with multiple fds, zero-timeout poll,
/// connection state machine exhaustive invalid transitions, large I/O buffers,
/// binary data in write buffers, thread pool with zero workers,
/// server rapid connect/disconnect, concurrent connect stress.

#include "sixseven/server/connection.h"
#include "sixseven/server/event_loop.h"
#include "sixseven/server/server.h"
#include "sixseven/server/thread_pool.h"

#include <gtest/gtest.h>

#include "sixseven/common/platform.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using namespace sixseven;

// =============================================================================
// EventLoop adversarial tests
// =============================================================================

class QA144EventLoopTest : public ::testing::Test {
protected:
    void SetUp() override {
        loop_ = EventLoop::create();
        ASSERT_NE(loop_, nullptr);
        ASSERT_TRUE(loop_->init().has_value());
    }

    std::unique_ptr<EventLoop> loop_;
};

TEST_F(QA144EventLoopTest, ZeroTimeoutPoll) {
    int fds[2];
    ASSERT_EQ(sixseven_platform::pipe(fds), 0);
    ASSERT_TRUE(loop_->add_fd(fds[0], EventType::READ).has_value());

    // Zero-timeout poll: should return immediately with no events.
    auto result = loop_->poll(0);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_F(QA144EventLoopTest, MultipleFdsReportCorrectly) {
    int p1[2];
    int p2[2];
    ASSERT_EQ(sixseven_platform::pipe(p1), 0);
    ASSERT_EQ(sixseven_platform::pipe(p2), 0);

    ASSERT_TRUE(loop_->add_fd(p1[0], EventType::READ).has_value());
    ASSERT_TRUE(loop_->add_fd(p2[0], EventType::READ).has_value());

    // Write to both pipes.
    ASSERT_EQ(::write(p1[1], "a", 1), 1);
    ASSERT_EQ(::write(p2[1], "b", 1), 1);

    auto result = loop_->poll(100);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 2u);

    // Both fds should be present.
    bool found_p1 = false;
    bool found_p2 = false;
    for (const auto& ev : *result) {
        if (ev.fd == p1[0]) {
            found_p1 = true;
        }
        if (ev.fd == p2[0]) {
            found_p2 = true;
        }
    }
    EXPECT_TRUE(found_p1);
    EXPECT_TRUE(found_p2);

    ::close(p1[0]);
    ::close(p1[1]);
    ::close(p2[0]);
    ::close(p2[1]);
}

TEST_F(QA144EventLoopTest, ReadWriteEventType) {
    int socks[2];
    ASSERT_EQ(sixseven_platform::socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

    // Register for both read and write.
    ASSERT_TRUE(loop_->add_fd(socks[0], EventType::READ_WRITE).has_value());

    // Write data on the peer so socks[0] is both readable and writable.
    ASSERT_EQ(::write(socks[1], "x", 1), 1);

    auto result = loop_->poll(100);
    ASSERT_TRUE(result.has_value());
    EXPECT_GE(result->size(), 1u);

    sixseven_platform::socket_close(socks[0]);
    sixseven_platform::socket_close(socks[1]);
}

TEST_F(QA144EventLoopTest, AddRemoveAddSameFd) {
    int fds[2];
    ASSERT_EQ(sixseven_platform::pipe(fds), 0);

    ASSERT_TRUE(loop_->add_fd(fds[0], EventType::READ).has_value());
    ASSERT_TRUE(loop_->remove_fd(fds[0]).has_value());
    ASSERT_TRUE(loop_->add_fd(fds[0], EventType::READ).has_value());

    // Write data and poll.
    ASSERT_EQ(::write(fds[1], "y", 1), 1);
    auto result = loop_->poll(100);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 1u);

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_F(QA144EventLoopTest, ConsecutivePollCalls) {
    int fds[2];
    ASSERT_EQ(sixseven_platform::pipe(fds), 0);
    ASSERT_TRUE(loop_->add_fd(fds[0], EventType::READ).has_value());

    // First poll: no data yet.
    auto r1 = loop_->poll(0);
    ASSERT_TRUE(r1.has_value());
    EXPECT_TRUE(r1->empty());

    // Write data.
    ASSERT_EQ(::write(fds[1], "z", 1), 1);

    // Second poll: data available.
    auto r2 = loop_->poll(100);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->size(), 1u);

    ::close(fds[0]);
    ::close(fds[1]);
}

// =============================================================================
// Connection adversarial tests
// =============================================================================

class QA144ConnectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        int fds[2];
        ASSERT_EQ(sixseven_platform::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
        fd_a_ = fds[0];
        fd_b_ = fds[1];
    }

    void TearDown() override {
        if (fd_b_ >= 0) {
            sixseven_platform::socket_close(fd_b_);
        }
    }

    int fd_a_ = -1;
    int fd_b_ = -1;
};

// Binary data in write buffer.
TEST_F(QA144ConnectionTest, BinaryWriteData) {
    Connection conn(fd_a_);
    fd_a_ = -1;

    // Write binary data including null bytes.
    std::vector<uint8_t> binary = {0x00, 0x01, 0xFF, 0xFE, 0x00, 0x42};
    conn.enqueue_write(binary.data(), binary.size());
    EXPECT_TRUE(conn.has_pending_writes());

    auto wr = conn.write_to_socket();
    ASSERT_TRUE(wr.has_value());
    EXPECT_EQ(*wr, binary.size());

    // Read from peer.
    uint8_t buf[64] = {};
    ssize_t n = ::recv(fd_b_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
    ASSERT_EQ(n, static_cast<ssize_t>(binary.size()));
    EXPECT_EQ(std::memcmp(buf, binary.data(), binary.size()), 0);
}

// Large write buffer (multiple enqueues).
TEST_F(QA144ConnectionTest, MultipleEnqueueWrites) {
    Connection conn(fd_a_);
    fd_a_ = -1;

    conn.enqueue_write("hello");
    conn.enqueue_write(" ");
    conn.enqueue_write("world");

    auto wr = conn.write_to_socket();
    ASSERT_TRUE(wr.has_value());
    EXPECT_EQ(*wr, 11u);

    char buf[64] = {};
    ssize_t n = ::recv(fd_b_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
    ASSERT_EQ(n, 11);
    EXPECT_EQ(std::string(buf, 11), "hello world");
}

// Write to socket with nothing enqueued.
TEST_F(QA144ConnectionTest, WriteEmptyBuffer) {
    Connection conn(fd_a_);
    fd_a_ = -1;

    EXPECT_FALSE(conn.has_pending_writes());
    auto wr = conn.write_to_socket();
    ASSERT_TRUE(wr.has_value());
    EXPECT_EQ(*wr, 0u);
}

// Read from closed peer returns EOF.
TEST_F(QA144ConnectionTest, ReadAfterPeerClose) {
    Connection conn(fd_a_);
    fd_a_ = -1;

    sixseven_platform::socket_close(fd_b_);
    fd_b_ = -1;

    auto r = conn.read_from_socket();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(**r, 0u); // EOF.
}

// consume_read with exact buffer size clears it.
TEST_F(QA144ConnectionTest, ConsumeExactBufferSize) {
    Connection conn(fd_a_);
    fd_a_ = -1;

    ::send(fd_b_, "abcde", 5, 0);
    ASSERT_TRUE(conn.read_from_socket().has_value());
    EXPECT_EQ(conn.read_buffer().size(), 5u);

    conn.consume_read(5);
    EXPECT_TRUE(conn.read_buffer().empty());
}

// close() is idempotent.
TEST_F(QA144ConnectionTest, DoubleCloseIsSafe) {
    Connection conn(fd_a_);
    fd_a_ = -1;

    conn.close();
    EXPECT_EQ(conn.fd(), -1);
    EXPECT_EQ(conn.state(), ConnectionState::CLOSED);

    conn.close(); // Should not crash.
    EXPECT_EQ(conn.fd(), -1);
}

// =============================================================================
// ThreadPool adversarial tests
// =============================================================================

TEST(QA144ThreadPool, SingleWorkerPool) {
    ThreadPool pool(1);
    EXPECT_EQ(pool.num_workers(), 1u);

    std::atomic<int> counter{0};
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(pool.submit([&counter] { counter.fetch_add(1); }));
    }
    pool.shutdown();
    EXPECT_EQ(counter.load(), 50);
}

TEST(QA144ThreadPool, LargeWorkerPool) {
    ThreadPool pool(16);
    EXPECT_EQ(pool.num_workers(), 16u);

    std::atomic<int> counter{0};
    constexpr int N = 1000;
    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(pool.submit([&counter] { counter.fetch_add(1); }));
    }
    pool.shutdown();
    EXPECT_EQ(counter.load(), N);
}

// GDB-245 fix: worker_loop() now wraps task() in try-catch so that a single
// bad task doesn't crash the server.
TEST(QA144ThreadPool, TasksThatThrowDontCrash) {
    ThreadPool pool(2);
    std::atomic<int> counter{0};

    // Submit a task that throws — should not crash the pool.
    pool.submit([] { throw std::runtime_error("test exception"); });

    // Submit a normal task after the throwing one.
    pool.submit([&counter] { counter.fetch_add(1); });

    // Give some time for tasks to execute.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Both workers should still be alive. The throwing task is caught,
    // and the second task runs normally.
    pool.shutdown();
    EXPECT_EQ(counter.load(), 1);
}

TEST(QA144ThreadPool, PendingTasksCountDropsAfterExecution) {
    ThreadPool pool(1);

    std::atomic<bool> gate{false};
    pool.submit([&gate] {
        while (!gate.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // Let the blocking task be picked up.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    pool.submit([] {});
    pool.submit([] {});
    EXPECT_GE(pool.pending_tasks(), 1u);

    gate.store(true);
    pool.shutdown();
    EXPECT_EQ(pool.pending_tasks(), 0u);
}

// =============================================================================
// Server adversarial tests
// =============================================================================

class QA144ServerTest : public ::testing::Test {
protected:
    Config make_config(uint16_t port = 0, size_t max_conn = 10) {
        Config cfg = Config::load_defaults();
        cfg.port = port;
        cfg.max_connections = max_conn;
        return cfg;
    }

    int connect_to(uint16_t port) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }

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

TEST_F(QA144ServerTest, RapidConnectDisconnect) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    // Rapidly connect and immediately disconnect 20 times.
    for (int i = 0; i < 20; ++i) {
        int fd = connect_to(server.bound_port());
        if (fd >= 0) {
            sixseven_platform::socket_close(fd);
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    // Server should have cleaned up all connections.
    EXPECT_EQ(server.health().active_connections, 0u);

    server.shutdown();
    t.join();
}

TEST_F(QA144ServerTest, HealthUptimeIncreases) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    auto h1 = server.health();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    auto h2 = server.health();
    // uptime is seconds-granularity; the >1s sleep guarantees it advances by at
    // least one second, so assert a STRICT increase. EXPECT_GE would pass for an
    // implementation whose uptime is permanently frozen at a constant (0 >= 0).
    EXPECT_GT(h2.uptime.count(), h1.uptime.count());

    server.shutdown();
    t.join();
}

TEST_F(QA144ServerTest, HealthVersionCorrect) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    auto h = server.health();
    EXPECT_EQ(h.version, Server::VERSION);

    server.shutdown();
    t.join();
}

TEST_F(QA144ServerTest, ShutdownImmediatelyAfterStart) {
    Server server(make_config());
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    // Shut down immediately — should not hang.
    server.shutdown();
    t.join();
    EXPECT_FALSE(server.is_running());
}

TEST_F(QA144ServerTest, MaxConnectionsOne) {
    Server server(make_config(0, 1));
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    int c1 = connect_to(server.bound_port());
    ASSERT_GE(c1, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(server.health().active_connections, 1u);

    // Second connection should be rejected.
    int c2 = connect_to(server.bound_port());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(server.health().active_connections, 1u);

    if (c2 >= 0) {
        sixseven_platform::socket_close(c2);
    }
    sixseven_platform::socket_close(c1);
    server.shutdown();
    t.join();
}

TEST_F(QA144ServerTest, ConnectionCountDecreasesOnDisconnect) {
    Server server(make_config(0, 10));
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    std::vector<int> clients;
    for (int i = 0; i < 5; ++i) {
        int fd = connect_to(server.bound_port());
        ASSERT_GE(fd, 0);
        clients.push_back(fd);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(server.health().active_connections, 5u);

    // Close 3 clients.
    for (int i = 0; i < 3; ++i) {
        sixseven_platform::socket_close(clients[i]);
        clients[i] = -1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(server.health().active_connections, 2u);

    // Close remaining.
    for (int fd : clients) {
        if (fd >= 0) {
            sixseven_platform::socket_close(fd);
        }
    }

    server.shutdown();
    t.join();
}

TEST_F(QA144ServerTest, BoundPortIsNonZero) {
    Server server(make_config(0));
    std::thread t([&server] { (void)server.start(); });
    wait_for_running(server);

    EXPECT_GT(server.bound_port(), 0u);

    server.shutdown();
    t.join();
}
