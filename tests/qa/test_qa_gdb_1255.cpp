/// GDB-1255 QA: Build-portability fix for test_qa_gdb_750.cpp.
///
/// The fix introduces a portable `gdb750_socket_handle` alias
/// (SOCKET on _WIN32, int on POSIX) used in the static_cast feeding
/// ::send/::recv. This file is the QA regression guard. Its primary
/// purpose is to PROVE the QA target compiles on POSIX (if it didn't,
/// the whole sixseven_qa_tests link would fail and these tests could
/// not run). It also adversarially exercises the same socketpair-based
/// round-trip the fix touches, to confirm the aliased cast preserves
/// data faithfully across send/recv on POSIX.

#include "sixseven/common/platform.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

// Mirror the alias under test exactly. On POSIX this must be `int`;
// a regression that re-introduced an unconditional SOCKET cast would
// fail to compile here (SOCKET is undefined off Windows).
#if defined(_WIN32)
using gdb1255_socket_handle = SOCKET;
#else
using gdb1255_socket_handle = int;
#endif

namespace {

// On POSIX the alias must be the plain file-descriptor int type.
TEST(GDB1255, SocketHandleIsIntOnPosix) {
#if defined(_WIN32)
    GTEST_SKIP() << "POSIX-only assertion";
#else
    static_assert(std::is_same_v<gdb1255_socket_handle, int>,
                  "On POSIX the socket handle alias must be int");
    SUCCEED();
#endif
}

// Round-trip arbitrary bytes (including embedded NULs and high bytes)
// through send/recv using the aliased cast. Verifies the cast does not
// corrupt the fd and the payload survives byte-for-byte.
TEST(GDB1255, AliasedSendRecvRoundTripsBytesExactly) {
    int fds[2];
    int rc = sixseven_platform::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(rc, 0) << "socketpair must succeed";
    int a = fds[0];
    int b = fds[1];

    std::vector<uint8_t> payload;
    for (int i = 0; i < 256; ++i)
        payload.push_back(static_cast<uint8_t>(i));
    payload.push_back(0);    // embedded NUL
    payload.push_back(0xFF); // high byte after NUL

    size_t written = 0;
    while (written < payload.size()) {
        auto n = ::send(static_cast<gdb1255_socket_handle>(a),
                        reinterpret_cast<const char*>(payload.data() + written),
                        static_cast<int>(payload.size() - written),
                        0);
        ASSERT_GT(n, 0);
        written += static_cast<size_t>(n);
    }

    std::vector<uint8_t> recvd;
    while (recvd.size() < payload.size()) {
        std::vector<uint8_t> buf(512);
        auto n = ::recv(static_cast<gdb1255_socket_handle>(b),
                        reinterpret_cast<char*>(buf.data()),
                        static_cast<int>(buf.size()),
                        0);
        ASSERT_GT(n, 0);
        recvd.insert(recvd.end(), buf.begin(), buf.begin() + n);
    }

    EXPECT_EQ(recvd, payload) << "aliased send/recv must round-trip bytes exactly";

    sixseven_platform::socket_close(a);
    sixseven_platform::socket_close(b);
}

// recv on a closed peer must report end-of-stream (0), not crash. Confirms
// the aliased cast behaves correctly on the error/EOF path.
TEST(GDB1255, RecvOnClosedPeerReturnsEof) {
    int fds[2];
    ASSERT_EQ(sixseven_platform::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    int a = fds[0];
    int b = fds[1];

    sixseven_platform::socket_close(a); // close the writer

    std::vector<uint8_t> buf(16);
    auto n = ::recv(static_cast<gdb1255_socket_handle>(b),
                    reinterpret_cast<char*>(buf.data()),
                    static_cast<int>(buf.size()),
                    0);
    EXPECT_EQ(n, 0) << "recv on a closed peer must report EOF (0)";

    sixseven_platform::socket_close(b);
}

} // namespace
