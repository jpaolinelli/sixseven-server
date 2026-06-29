// GDB-980: QA adversarial tests -- event-loop busy-poll spin fix.
//
// These tests exercise the EventLoop API contract that the GDB-980 fix relies
// on. They run on all platforms (Windows + POSIX). The underlying WsaPollEventLoop
// / EpollEventLoop / KqueueEventLoop implementations are all covered at the API
// level via the factory EventLoop::create().
//
// All tests are pure event-loop API tests (no real TCP sockets, no server).

#include "sixseven/server/event_loop.h"

#include <gtest/gtest.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace sixseven {

// ---------------------------------------------------------------------------
// Fixture: creates and initialises a fresh EventLoop for each test.
// ---------------------------------------------------------------------------

class QA_GDB980EventLoop : public ::testing::Test {
protected:
    void SetUp() override {
        loop_ = EventLoop::create();
        ASSERT_NE(loop_, nullptr);
        auto r = loop_->init();
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    std::unique_ptr<EventLoop> loop_;
};

// ---------------------------------------------------------------------------
// AC: remove_fd on an absent fd returns ok() (double-remove tolerance).
// This is the close-connection path: close_connection() calls remove_fd()
// after handle_read() already removed the fd when it went in-flight. On
// Windows the WSAPoll loop explicitly returns ok() for absent fds (line 328
// of event_loop.cpp). On POSIX epoll ignores ENOENT, kqueue ignores missing
// filters. All three must tolerate a second remove without returning an error
// that would propagate up and disrupt connection teardown.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB980EventLoop, DoubleRemoveFdIsNoOp) {
    // remove_fd on an fd we never registered must not fail.
    // Use an obviously bogus fd value (99999) that will never be in the loop.
    auto r1 = loop_->remove_fd(99999);
    EXPECT_TRUE(r1.has_value()) << "first remove_fd on absent fd must be ok: "
                                << r1.error().message;

    auto r2 = loop_->remove_fd(99999);
    EXPECT_TRUE(r2.has_value()) << "second remove_fd (double-remove) must be ok: "
                                << r2.error().message;
}

// ---------------------------------------------------------------------------
// AC: poll() with no registered client fds and timeout_ms=0 returns
// immediately with an empty event list (confirms poll() does not hang when
// all in-flight fds have been removed).
// ---------------------------------------------------------------------------

TEST_F(QA_GDB980EventLoop, PollWithNoFdsReturnsEmptyImmediately) {
    auto r = loop_->poll(0);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    // The wakeup fd is internal; no client events should appear.
    EXPECT_TRUE(r->empty()) << "poll with no registered fds must return empty events";
}

// ---------------------------------------------------------------------------
// POSIX-only: end-to-end regression for the in-flight busy-poll spin.
// Mirrors test_gdb_980_inflight_no_spin.cpp but targets the QA test binary.
// Skipped cleanly on Windows (_WIN32) by design.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB980EventLoop, GDB980_RemoveFdSilencesBusyPollSpin) {
#if defined(_WIN32)
    GTEST_SKIP() << "POSIX event loop only -- socketpair(AF_UNIX) unavailable on Windows";
#else
    // Create a connected socketpair. Both ends have buffer space and are
    // immediately write-ready -- the exact condition that triggered the spin.
    int socks[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

    // ---- Precondition: WRITE-registered writable socket IS returned by poll.
    {
        auto add = loop_->add_fd(socks[0], EventType::WRITE);
        ASSERT_TRUE(add.has_value()) << add.error().message;

        auto result = loop_->poll(50);
        ASSERT_TRUE(result.has_value()) << result.error().message;

        bool found = false;
        for (const auto& ev : *result) {
            if (ev.fd == socks[0]) {
                found = true;
                break;
            }
        }
        ASSERT_TRUE(found)
            << "precondition: WRITE-registered writable socket must appear in poll()";
    }

    // ---- Fix path: remove_fd (simulates handle_read in-flight transition).
    {
        auto r = loop_->remove_fd(socks[0]);
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    // ---- Assertion: poll must NOT return the removed fd.
    {
        auto result = loop_->poll(50);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        for (const auto& ev : *result) {
            EXPECT_NE(ev.fd, socks[0])
                << "removed fd must not appear in poll() (GDB-980 busy-poll regression)";
        }
    }

    // ---- Double-remove tolerance on the close path.
    {
        auto r = loop_->remove_fd(socks[0]);
        EXPECT_TRUE(r.has_value()) << "double-remove must be a no-op: " << r.error().message;
    }

    ::close(socks[0]);
    ::close(socks[1]);
#endif
}

// ---------------------------------------------------------------------------
// POSIX-only: re-add after remove correctly re-registers the fd.
// This validates process_completed_queries() calling add_fd (not modify_fd)
// after handle_read() called remove_fd. On all three backends, add_fd on a
// previously-removed fd must succeed and make the fd polled again.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB980EventLoop, GDB980_ReAddAfterRemoveRestoresPolling) {
#if defined(_WIN32)
    GTEST_SKIP() << "POSIX event loop only -- socketpair(AF_UNIX) unavailable on Windows";
#else
    int socks[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

    // Add for READ interest initially.
    {
        auto r = loop_->add_fd(socks[0], EventType::READ);
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    // Remove (in-flight simulation).
    {
        auto r = loop_->remove_fd(socks[0]);
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    // Re-add with READ_WRITE (has_pending_writes() == true path from
    // process_completed_queries). This must not fail with EEXIST or similar.
    {
        auto r = loop_->add_fd(socks[0], EventType::READ_WRITE);
        ASSERT_TRUE(r.has_value()) << "re-add after remove must succeed: " << r.error().message;
    }

    // Confirm the fd is now polled (it's writable, so WRITE event expected).
    {
        auto result = loop_->poll(50);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        bool found = false;
        for (const auto& ev : *result) {
            if (ev.fd == socks[0]) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << "re-added fd must appear in poll()";
    }

    // Clean up.
    (void)loop_->remove_fd(socks[0]);
    ::close(socks[0]);
    ::close(socks[1]);
#endif
}

} // namespace sixseven
