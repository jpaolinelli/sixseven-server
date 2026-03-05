#include "sixseven/server/event_loop.h"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

namespace sixseven {

class EventLoopTest : public ::testing::Test {
protected:
    void SetUp() override {
        loop_ = EventLoop::create();
        ASSERT_NE(loop_, nullptr);
        auto init = loop_->init();
        ASSERT_TRUE(init.has_value()) << init.error().message;

        // Create a pipe for I/O testing.
        int fds[2];
        ASSERT_EQ(::pipe(fds), 0);
        pipe_read_ = fds[0];
        pipe_write_ = fds[1];
    }

    void TearDown() override {
        if (pipe_read_ >= 0)
            ::close(pipe_read_);
        if (pipe_write_ >= 0)
            ::close(pipe_write_);
    }

    std::unique_ptr<EventLoop> loop_;
    int pipe_read_ = -1;
    int pipe_write_ = -1;
};

TEST_F(EventLoopTest, CreateReturnsNonNull) {
    auto loop = EventLoop::create();
    EXPECT_NE(loop, nullptr);
}

TEST_F(EventLoopTest, InitSucceeds) {
    auto loop = EventLoop::create();
    auto result = loop->init();
    EXPECT_TRUE(result.has_value());
}

TEST_F(EventLoopTest, AddAndRemoveFd) {
    auto add = loop_->add_fd(pipe_read_, EventType::READ);
    EXPECT_TRUE(add.has_value()) << add.error().message;

    auto remove = loop_->remove_fd(pipe_read_);
    EXPECT_TRUE(remove.has_value()) << remove.error().message;
}

TEST_F(EventLoopTest, PollTimesOutWithNoEvents) {
    auto add = loop_->add_fd(pipe_read_, EventType::READ);
    ASSERT_TRUE(add.has_value());

    // Poll with a short timeout — nothing to read yet.
    auto result = loop_->poll(10);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

TEST_F(EventLoopTest, PollDetectsReadReady) {
    auto add = loop_->add_fd(pipe_read_, EventType::READ);
    ASSERT_TRUE(add.has_value());

    // Write data to make the read end readable.
    const char data[] = "x";
    ASSERT_EQ(::write(pipe_write_, data, 1), 1);

    auto result = loop_->poll(100);
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->empty());
    EXPECT_EQ(result->at(0).fd, pipe_read_);
    EXPECT_EQ(result->at(0).type, EventType::READ);
}

TEST_F(EventLoopTest, PollDetectsWriteReady) {
    // Use a socketpair — sockets reliably report write-ready on kqueue,
    // whereas pipe write filters on kqueue require a preceding read.
    int socks[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

    auto add = loop_->add_fd(socks[0], EventType::WRITE);
    ASSERT_TRUE(add.has_value());

    auto result = loop_->poll(100);
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->empty());
    EXPECT_EQ(result->at(0).fd, socks[0]);
    EXPECT_EQ(result->at(0).type, EventType::WRITE);

    ::close(socks[0]);
    ::close(socks[1]);
}

TEST_F(EventLoopTest, ModifyFd) {
    auto add = loop_->add_fd(pipe_read_, EventType::READ);
    ASSERT_TRUE(add.has_value());

    // Modify to monitor writes instead (pipe_read_ won't be writable, so poll times out).
    auto modify = loop_->modify_fd(pipe_read_, EventType::WRITE);
    ASSERT_TRUE(modify.has_value());

    auto result = loop_->poll(10);
    ASSERT_TRUE(result.has_value());
    // pipe_read_ is not writable, so no events expected.
    EXPECT_TRUE(result->empty());
}

TEST_F(EventLoopTest, RemoveNonexistentFdIsNoOp) {
    auto result = loop_->remove_fd(9999);
    // Should not error — removal of non-existent fd is a no-op.
    EXPECT_TRUE(result.has_value());
}

} // namespace sixseven
