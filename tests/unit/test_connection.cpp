#include "sixseven/server/connection.h"

#include <gtest/gtest.h>

#include "sixseven/common/platform.h"

#include <cstring>

namespace sixseven {

// ─── connection_state_name ──────────────────────────────────────────────────

TEST(ConnectionStateName, ReturnsCorrectNames) {
    EXPECT_STREQ(connection_state_name(ConnectionState::INIT), "INIT");
    EXPECT_STREQ(connection_state_name(ConnectionState::AUTH), "AUTH");
    EXPECT_STREQ(connection_state_name(ConnectionState::READY), "READY");
    EXPECT_STREQ(connection_state_name(ConnectionState::QUERY), "QUERY");
    EXPECT_STREQ(connection_state_name(ConnectionState::CLOSED), "CLOSED");
}

// ─── State machine transitions ──────────────────────────────────────────────

class ConnectionStateTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a socketpair for testing.
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

    int fd_a_ = -1; // Owned by the Connection under test.
    int fd_b_ = -1; // The "peer" side.
};

TEST_F(ConnectionStateTest, InitialStateIsInit) {
    Connection conn(fd_a_);
    fd_a_ = -1; // Connection owns it now.
    EXPECT_EQ(conn.state(), ConnectionState::INIT);
}

TEST_F(ConnectionStateTest, ValidTransitionInitToAuth) {
    Connection conn(fd_a_);
    fd_a_ = -1;
    auto result = conn.transition_to(ConnectionState::AUTH);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(conn.state(), ConnectionState::AUTH);
}

TEST_F(ConnectionStateTest, ValidTransitionAuthToReady) {
    Connection conn(fd_a_);
    fd_a_ = -1;
    ASSERT_TRUE(conn.transition_to(ConnectionState::AUTH).has_value());
    auto result = conn.transition_to(ConnectionState::READY);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(conn.state(), ConnectionState::READY);
}

TEST_F(ConnectionStateTest, ValidTransitionReadyToQuery) {
    Connection conn(fd_a_);
    fd_a_ = -1;
    ASSERT_TRUE(conn.transition_to(ConnectionState::AUTH).has_value());
    ASSERT_TRUE(conn.transition_to(ConnectionState::READY).has_value());
    auto result = conn.transition_to(ConnectionState::QUERY);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(conn.state(), ConnectionState::QUERY);
}

TEST_F(ConnectionStateTest, ValidTransitionQueryToReady) {
    Connection conn(fd_a_);
    fd_a_ = -1;
    ASSERT_TRUE(conn.transition_to(ConnectionState::AUTH).has_value());
    ASSERT_TRUE(conn.transition_to(ConnectionState::READY).has_value());
    ASSERT_TRUE(conn.transition_to(ConnectionState::QUERY).has_value());
    auto result = conn.transition_to(ConnectionState::READY);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(conn.state(), ConnectionState::READY);
}

TEST_F(ConnectionStateTest, ValidTransitionToClosed) {
    Connection conn(fd_a_);
    fd_a_ = -1;
    ASSERT_TRUE(conn.transition_to(ConnectionState::AUTH).has_value());
    ASSERT_TRUE(conn.transition_to(ConnectionState::READY).has_value());
    auto result = conn.transition_to(ConnectionState::CLOSED);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(conn.state(), ConnectionState::CLOSED);
}

TEST_F(ConnectionStateTest, InvalidTransitionInitToReady) {
    Connection conn(fd_a_);
    fd_a_ = -1;
    auto result = conn.transition_to(ConnectionState::READY);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(conn.state(), ConnectionState::INIT); // State unchanged.
}

TEST_F(ConnectionStateTest, InvalidTransitionFromClosed) {
    Connection conn(fd_a_);
    fd_a_ = -1;
    ASSERT_TRUE(conn.transition_to(ConnectionState::AUTH).has_value());
    ASSERT_TRUE(conn.transition_to(ConnectionState::CLOSED).has_value());
    auto result = conn.transition_to(ConnectionState::READY);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(ConnectionStateTest, InvalidTransitionReadyToAuth) {
    Connection conn(fd_a_);
    fd_a_ = -1;
    ASSERT_TRUE(conn.transition_to(ConnectionState::AUTH).has_value());
    ASSERT_TRUE(conn.transition_to(ConnectionState::READY).has_value());
    auto result = conn.transition_to(ConnectionState::AUTH);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ─── I/O buffers ────────────────────────────────────────────────────────────

TEST_F(ConnectionStateTest, EnqueueAndWriteToSocket) {
    Connection conn(fd_a_);
    fd_a_ = -1;

    const std::string msg = "hello";
    conn.enqueue_write(msg);
    EXPECT_TRUE(conn.has_pending_writes());

    auto write_result = conn.write_to_socket();
    ASSERT_TRUE(write_result.has_value());
    EXPECT_EQ(*write_result, msg.size());
    EXPECT_FALSE(conn.has_pending_writes());

    // Read from the peer side.
    char buf[64] = {};
    ssize_t n = ::recv(fd_b_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
    ASSERT_EQ(n, static_cast<ssize_t>(msg.size()));
    EXPECT_EQ(std::string(buf, static_cast<size_t>(n)), msg);
}

TEST_F(ConnectionStateTest, ReadFromSocket) {
    Connection conn(fd_a_);
    fd_a_ = -1;

    // Write from the peer side.
    const std::string msg = "world";
    ssize_t sent = ::send(fd_b_, msg.data(), msg.size(), 0);
    ASSERT_EQ(sent, static_cast<ssize_t>(msg.size()));

    auto read_result = conn.read_from_socket();
    ASSERT_TRUE(read_result.has_value());
    ASSERT_TRUE(read_result->has_value()); // Not EAGAIN.
    EXPECT_EQ(**read_result, msg.size());
    EXPECT_EQ(conn.read_buffer().size(), msg.size());

    // Verify buffer contents.
    std::string got(conn.read_buffer().begin(), conn.read_buffer().end());
    EXPECT_EQ(got, msg);
}

TEST_F(ConnectionStateTest, ConsumeRead) {
    Connection conn(fd_a_);
    fd_a_ = -1;

    const std::string msg = "abcdef";
    ::send(fd_b_, msg.data(), msg.size(), 0);
    ASSERT_TRUE(conn.read_from_socket().has_value());

    conn.consume_read(3);
    EXPECT_EQ(conn.read_buffer().size(), 3u);
    std::string remaining(conn.read_buffer().begin(), conn.read_buffer().end());
    EXPECT_EQ(remaining, "def");

    // Consume more than available — clears the buffer.
    conn.consume_read(100);
    EXPECT_TRUE(conn.read_buffer().empty());
}

TEST_F(ConnectionStateTest, ReadReturnsZeroOnEof) {
    Connection conn(fd_a_);
    fd_a_ = -1;

    // Close the peer to trigger EOF.
    sixseven_platform::socket_close(fd_b_);
    fd_b_ = -1;

    auto result = conn.read_from_socket();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value()); // Not EAGAIN.
    EXPECT_EQ(**result, 0u);
}

// ─── Close ──────────────────────────────────────────────────────────────────

TEST_F(ConnectionStateTest, CloseTransitionsToClosedState) {
    Connection conn(fd_a_);
    fd_a_ = -1;
    conn.close();
    EXPECT_EQ(conn.state(), ConnectionState::CLOSED);
    EXPECT_EQ(conn.fd(), -1);
}

// ─── Move semantics ─────────────────────────────────────────────────────────

TEST_F(ConnectionStateTest, MoveConstructor) {
    Connection conn(fd_a_);
    fd_a_ = -1;
    int original_fd = conn.fd();

    Connection moved(std::move(conn));
    EXPECT_EQ(moved.fd(), original_fd);
    EXPECT_EQ(moved.state(), ConnectionState::INIT);
    EXPECT_EQ(conn.fd(), -1);                         // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(conn.state(), ConnectionState::CLOSED); // NOLINT(bugprone-use-after-move)
}

TEST_F(ConnectionStateTest, MoveAssignment) {
    Connection conn1(fd_a_);
    fd_a_ = -1;
    int fd1 = conn1.fd();

    // Create a second connection with a separate socketpair.
    int fds[2];
    ASSERT_EQ(sixseven_platform::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    Connection conn2(fds[0]);
    sixseven_platform::socket_close(fds[1]);

    conn2 = std::move(conn1);
    EXPECT_EQ(conn2.fd(), fd1);
    EXPECT_EQ(conn1.fd(), -1); // NOLINT(bugprone-use-after-move)
}

} // namespace sixseven
