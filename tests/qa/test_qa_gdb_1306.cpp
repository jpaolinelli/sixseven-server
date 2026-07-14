// GDB-1306 QA: adversarial tests for KqueueEventLoop::remove_fd.
//
// GDB-1306 splits the batched 2-entry EVFILT_READ/EVFILT_WRITE kevent()
// delete call into two separate single-entry calls. Root cause: macOS
// kqueue with nevents==0 aborts changelist processing on the first ENOENT,
// silently skipping any remaining changelist entries. This re-fixes the
// GDB-980 busy-poll spin, now broken again by the batching gap.
//
// POSIX/kqueue-only: socketpair(AF_UNIX) unavailable on Windows; this
// exercises the KqueueEventLoop backend which only exists on __APPLE__.

#include "sixseven/server/event_loop.h"

#include <gtest/gtest.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <chrono>

namespace sixseven {
namespace {

#ifndef _WIN32
bool poll_contains_fd(EventLoop& loop, int fd, int timeout_ms = 50) {
    auto result = loop.poll(timeout_ms);
    if (!result.has_value()) return false;
    for (const auto& ev : *result) {
        if (ev.fd == fd) return true;
    }
    return false;
}
#endif

} // namespace

// ---------------------------------------------------------------------------
// QA_GDB1306
// ---------------------------------------------------------------------------

// Case 1: fd registered for WRITE only -- must be fully deregistered.
TEST(QA_GDB1306, RemoveFdWriteOnlyFullyDeregisters) {
#if defined(_WIN32)
    GTEST_SKIP() << "kqueue-only test";
#else
    auto loop = EventLoop::create();
    ASSERT_NE(loop, nullptr);
    ASSERT_TRUE(loop->init().has_value());

    int socks[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

    ASSERT_TRUE(loop->add_fd(socks[0], EventType::WRITE).has_value());
    // Precondition: writable socket registered for WRITE must appear.
    EXPECT_TRUE(poll_contains_fd(*loop, socks[0]));

    ASSERT_TRUE(loop->remove_fd(socks[0]).has_value());
    EXPECT_FALSE(poll_contains_fd(*loop, socks[0]))
        << "WRITE-only fd must not reappear after remove_fd";

    ::close(socks[0]);
    ::close(socks[1]);
#endif
}

// Case 2: fd registered for READ only -- must be fully deregistered.
TEST(QA_GDB1306, RemoveFdReadOnlyFullyDeregisters) {
#if defined(_WIN32)
    GTEST_SKIP() << "kqueue-only test";
#else
    auto loop = EventLoop::create();
    ASSERT_NE(loop, nullptr);
    ASSERT_TRUE(loop->init().has_value());

    int socks[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

    ASSERT_TRUE(loop->add_fd(socks[0], EventType::READ).has_value());

    // Make socks[0] readable by writing from the peer.
    char byte = 'x';
    ASSERT_EQ(::write(socks[1], &byte, 1), 1);
    EXPECT_TRUE(poll_contains_fd(*loop, socks[0]));

    ASSERT_TRUE(loop->remove_fd(socks[0]).has_value());
    EXPECT_FALSE(poll_contains_fd(*loop, socks[0]))
        << "READ-only fd must not reappear after remove_fd";

    ::close(socks[0]);
    ::close(socks[1]);
#endif
}

// Case 3: fd registered for both READ and WRITE -- must be fully deregistered.
TEST(QA_GDB1306, RemoveFdReadWriteFullyDeregisters) {
#if defined(_WIN32)
    GTEST_SKIP() << "kqueue-only test";
#else
    auto loop = EventLoop::create();
    ASSERT_NE(loop, nullptr);
    ASSERT_TRUE(loop->init().has_value());

    int socks[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

    ASSERT_TRUE(loop->add_fd(socks[0], EventType::READ_WRITE).has_value());
    char byte = 'x';
    ASSERT_EQ(::write(socks[1], &byte, 1), 1);
    EXPECT_TRUE(poll_contains_fd(*loop, socks[0]));

    ASSERT_TRUE(loop->remove_fd(socks[0]).has_value());
    EXPECT_FALSE(poll_contains_fd(*loop, socks[0]))
        << "READ_WRITE fd must not reappear after remove_fd (both filters must be dropped)";

    ::close(socks[0]);
    ::close(socks[1]);
#endif
}

// Case 4: remove_fd on an fd that was never registered -- graceful no-op.
TEST(QA_GDB1306, RemoveFdNeverRegisteredIsGracefulNoOp) {
#if defined(_WIN32)
    GTEST_SKIP() << "kqueue-only test";
#else
    auto loop = EventLoop::create();
    ASSERT_NE(loop, nullptr);
    ASSERT_TRUE(loop->init().has_value());

    int socks[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

    // Never call add_fd.
    auto result = loop->remove_fd(socks[0]);
    EXPECT_TRUE(result.has_value())
        << "remove_fd on unregistered fd must not error: "
        << (result.has_value() ? "" : result.error().message);

    ::close(socks[0]);
    ::close(socks[1]);
#endif
}

// Case 4b: remove_fd on a bogus/closed fd number -- must not crash.
TEST(QA_GDB1306, RemoveFdBogusFdDoesNotCrash) {
#if defined(_WIN32)
    GTEST_SKIP() << "kqueue-only test";
#else
    auto loop = EventLoop::create();
    ASSERT_NE(loop, nullptr);
    ASSERT_TRUE(loop->init().has_value());

    // fd 99999 is very unlikely to be a valid descriptor.
    auto result = loop->remove_fd(99999);
    EXPECT_TRUE(result.has_value());
#endif
}

// Case 5: remove_fd called twice in a row -- idempotent, second call is a no-op.
TEST(QA_GDB1306, RemoveFdCalledTwiceIsIdempotent) {
#if defined(_WIN32)
    GTEST_SKIP() << "kqueue-only test";
#else
    auto loop = EventLoop::create();
    ASSERT_NE(loop, nullptr);
    ASSERT_TRUE(loop->init().has_value());

    int socks[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

    ASSERT_TRUE(loop->add_fd(socks[0], EventType::READ_WRITE).has_value());

    auto r1 = loop->remove_fd(socks[0]);
    EXPECT_TRUE(r1.has_value());
    auto r2 = loop->remove_fd(socks[0]);
    EXPECT_TRUE(r2.has_value()) << "second remove_fd must be tolerant, not error";

    EXPECT_FALSE(poll_contains_fd(*loop, socks[0]));

    ::close(socks[0]);
    ::close(socks[1]);
#endif
}

// Case 6: rapid register/remove/register cycling on the same fd number.
// After a socket is closed and a new one happens to reuse the same fd (or a
// fresh fd), the loop's bookkeeping (or lack thereof, since kqueue itself is
// the source of truth here) must not leave stale filters active.
TEST(QA_GDB1306, RapidRegisterRemoveRegisterCycleOnSameFd) {
#if defined(_WIN32)
    GTEST_SKIP() << "kqueue-only test";
#else
    auto loop = EventLoop::create();
    ASSERT_NE(loop, nullptr);
    ASSERT_TRUE(loop->init().has_value());

    for (int i = 0; i < 50; ++i) {
        int socks[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

        EventType type = (i % 3 == 0) ? EventType::READ
                        : (i % 3 == 1) ? EventType::WRITE
                                       : EventType::READ_WRITE;
        ASSERT_TRUE(loop->add_fd(socks[0], type).has_value());
        ASSERT_TRUE(loop->remove_fd(socks[0]).has_value());
        // Re-register immediately, then remove again.
        ASSERT_TRUE(loop->add_fd(socks[0], type).has_value());
        ASSERT_TRUE(loop->remove_fd(socks[0]).has_value());

        EXPECT_FALSE(poll_contains_fd(*loop, socks[0], 5))
            << "cycle " << i << ": fd reappeared after final remove_fd";

        ::close(socks[0]);
        ::close(socks[1]);
    }
#endif
}

// Case 7: GDB-980 busy-poll-spin repro under sustained load -- after
// remove_fd, repeated poll() calls over ~500ms must consistently return zero
// events for the removed (still-writable) fd. This is the actual spin
// symptom: if even one filter survives, poll() returns immediately every
// time (0ms latency, CPU pegged) instead of respecting the timeout.
TEST(QA_GDB1306, SustainedPollAfterRemoveNeverSpins) {
#if defined(_WIN32)
    GTEST_SKIP() << "kqueue-only test";
#else
    auto loop = EventLoop::create();
    ASSERT_NE(loop, nullptr);
    ASSERT_TRUE(loop->init().has_value());

    int socks[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

    // Register for READ_WRITE (both filters) on an always-writable socket.
    ASSERT_TRUE(loop->add_fd(socks[0], EventType::READ_WRITE).has_value());
    ASSERT_TRUE(loop->remove_fd(socks[0]).has_value());

    const int timeout_ms = 30;
    int spin_count = 0;
    for (int i = 0; i < 10; ++i) {
        auto start = std::chrono::steady_clock::now();
        auto result = loop->poll(timeout_ms);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
        ASSERT_TRUE(result.has_value());
        for (const auto& ev : *result) {
            EXPECT_NE(ev.fd, socks[0]);
        }
        // A spin manifests as poll() returning near-instantly (busy loop)
        // instead of waiting out the timeout, because the event source
        // keeps firing immediately.
        if (elapsed < timeout_ms / 2) {
            ++spin_count;
        }
    }
    EXPECT_EQ(spin_count, 0)
        << "poll() returned early " << spin_count
        << " times after remove_fd -- busy-poll spin regression (GDB-980)";

    ::close(socks[0]);
    ::close(socks[1]);
#endif
}

} // namespace sixseven
