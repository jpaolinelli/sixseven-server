// GDB-980: Regression test -- remove_fd silences busy-poll spin on in-flight fds.
//
// CONTRACT: When a connected socket is removed from the event loop (simulating
// what handle_read() now does for in-flight fds), a subsequent poll() must NOT
// return that fd as writable. Under the old code (modify_fd -> WRITE), a TCP
// socket with available send-buffer space is always write-ready in
// level-triggered mode, so poll() would spin at 100% CPU for the entire query
// duration. Under the fixed code (remove_fd), the fd is absent from the loop
// and poll() must time out with no events.
//
// POSIX-only: socketpair(AF_UNIX) is unavailable on Windows. The test body is
// skipped on _WIN32 with GTEST_SKIP so the file still compiles and links on
// all platforms.

#include "sixseven/server/event_loop.h"

#include <gtest/gtest.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace sixseven {

// ---------------------------------------------------------------------------
// GDB980InflightNoSpin
// ---------------------------------------------------------------------------

TEST(GDB980InflightNoSpin, RemoveFdSilencesWriteSpin) {
#if defined(_WIN32)
    GTEST_SKIP() << "POSIX event loop only -- socketpair(AF_UNIX) unavailable on Windows";
#else
    // Create the event loop.
    auto loop = EventLoop::create();
    ASSERT_NE(loop, nullptr);
    auto init = loop->init();
    ASSERT_TRUE(init.has_value()) << init.error().message;

    // Create a connected socketpair. Both ends are immediately write-ready
    // (send buffer has space) -- this is exactly the condition that caused the
    // busy-poll spin under the old code.
    int socks[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

    // Register the fd for WRITE interest -- mirrors the old broken behavior.
    // With WRITE registered on a writable socket, poll() must return it.
    {
        auto add = loop->add_fd(socks[0], EventType::WRITE);
        ASSERT_TRUE(add.has_value()) << add.error().message;

        auto result = loop->poll(50);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        // Confirm the socket IS write-ready (precondition for the regression).
        bool found = false;
        for (const auto& ev : *result) {
            if (ev.fd == socks[0]) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found)
            << "precondition: WRITE-registered writable socket must appear in poll()";
    }

    // Now simulate the in-flight transition: remove the fd from the loop.
    // This is the fix introduced in GDB-980 (handle_read now calls remove_fd).
    {
        auto remove = loop->remove_fd(socks[0]);
        ASSERT_TRUE(remove.has_value()) << remove.error().message;
    }

    // After remove_fd, poll() must NOT return socks[0] -- even though the
    // socket is still writable. This proves the busy-poll spin is gone.
    {
        auto result = loop->poll(50);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        for (const auto& ev : *result) {
            EXPECT_NE(ev.fd, socks[0])
                << "removed fd must not appear in poll() -- fix regression: GDB-980";
        }
    }

    // Also verify that a second remove_fd (the close_connection() path, which
    // calls remove_fd again after the fd was already removed) is tolerant and
    // does not crash or return a hard error.
    {
        auto remove2 = loop->remove_fd(socks[0]);
        EXPECT_TRUE(remove2.has_value())
            << "double-remove (absent fd) must be a no-op: " << remove2.error().message;
    }

    ::close(socks[0]);
    ::close(socks[1]);
#endif
}

} // namespace sixseven
