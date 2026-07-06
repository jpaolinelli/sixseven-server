// QA regression tests for GDB-1233.
//
// GDB-1233 replaced CRT ::write/::read with ::send/::recv (+ platform_init())
// in tests/unit/pg_wire_test_helpers.h to fix a Windows CRT abort
// (0xC0000409) when calling ::write/::read on raw SOCKET handles returned by
// create_socketpair().
//
// These tests exercise the *socket transfer path* itself (create_socketpair,
// write_to_fd, read_from_fd) end-to-end over the socketpair, which is exactly
// the code GDB-1233 touched. The existing GDB-867 QA suite only covers the
// pure byte-builders (build_startup_message, etc.) and never calls
// write_to_fd/read_from_fd, so it gives no coverage of the send/recv swap.

#include <gtest/gtest.h>

#include "pg_wire_test_helpers.h"

#include "sixseven/common/platform.h"

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

using pg_wire_test::create_socketpair;
using pg_wire_test::ensure_platform_init;
using pg_wire_test::read_from_fd;
using pg_wire_test::write_to_fd;

TEST(QA_GDB1233_SocketTransfer, PlatformInitIdempotentAcrossMultipleCalls) {
    // Calling ensure_platform_init() repeatedly must never abort/crash and
    // must consistently report success (AC: helpers use platform_init()).
    EXPECT_TRUE(ensure_platform_init());
    EXPECT_TRUE(ensure_platform_init());
    EXPECT_TRUE(ensure_platform_init());
}

TEST(QA_GDB1233_SocketTransfer, RoundTripSmallMessageExactBytes) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    ASSERT_GE(server_fd, 0);
    ASSERT_GE(client_fd, 0);

    std::vector<uint8_t> payload = {'Q', 0, 0, 0, 9, 'S', 'E', 'L', 0};
    write_to_fd(client_fd, payload);

    auto received = read_from_fd(server_fd, 64);
    EXPECT_EQ(received, payload);

    sixseven_platform::socket_close(client_fd);
    sixseven_platform::socket_close(server_fd);
}

TEST(QA_GDB1233_SocketTransfer, RoundTripEmptyPayloadDoesNotHang) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    ASSERT_GE(server_fd, 0);

    // write_to_fd with an empty vector must return immediately (loop
    // condition `written < data.size()` is false at 0 < 0).
    std::vector<uint8_t> empty;
    write_to_fd(client_fd, empty);

    // No assertion possible on the read side without blocking forever, so we
    // only verify write_to_fd itself returned control to us (i.e. we reached
    // this line) -- the real risk here is an infinite loop/hang, which would
    // time out the test binary rather than fail an assertion.
    SUCCEED();

    sixseven_platform::socket_close(client_fd);
    sixseven_platform::socket_close(server_fd);
}

TEST(QA_GDB1233_SocketTransfer, RoundTripLargeMessageAcrossMultipleSegments) {
    // A message larger than a typical single TCP segment (64KB), to shake
    // out any short-write/short-read truncation introduced by the send/recv
    // swap. write_to_fd retries on short sends, so this must reassemble
    // byte-for-byte on the receiving side across possibly many recv() calls
    // by the test itself.
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    ASSERT_GE(server_fd, 0);

    constexpr size_t kSize = 256 * 1024;
    std::vector<uint8_t> payload(kSize);
    for (size_t i = 0; i < kSize; ++i) {
        payload[i] = static_cast<uint8_t>(i & 0xFF);
    }

    // Writer on a separate thread so the (bounded) socketpair buffer doesn't
    // deadlock the single-threaded write-then-read pattern used elsewhere.
    std::thread writer([&]() { write_to_fd(client_fd, payload); });

    std::vector<uint8_t> received;
    received.reserve(kSize);
    while (received.size() < kSize) {
        auto chunk = read_from_fd(server_fd, kSize - received.size());
        ASSERT_FALSE(chunk.empty()) << "read_from_fd returned no bytes before full payload received ("
                                     << received.size() << "/" << kSize << ")";
        received.insert(received.end(), chunk.begin(), chunk.end());
    }
    writer.join();

    ASSERT_EQ(received.size(), kSize);
    EXPECT_EQ(received, payload);

    sixseven_platform::socket_close(client_fd);
    sixseven_platform::socket_close(server_fd);
}

TEST(QA_GDB1233_SocketTransfer, ReadFromFdRespectsMaxBytesCap) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    ASSERT_GE(server_fd, 0);

    std::vector<uint8_t> payload(100, 0xAB);
    write_to_fd(client_fd, payload);

    // Request fewer bytes than were sent; read_from_fd must not over-read or
    // corrupt the buffer, and must return no more than max_bytes.
    auto received = read_from_fd(server_fd, 10);
    EXPECT_LE(received.size(), 10u);
    EXPECT_FALSE(received.empty());

    sixseven_platform::socket_close(client_fd);
    sixseven_platform::socket_close(server_fd);
}

TEST(QA_GDB1233_SocketTransfer, ReadFromFdOnClosedPeerReturnsEmpty) {
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    ASSERT_GE(server_fd, 0);

    // Close the write side without sending anything; recv() on the other
    // end should observe orderly shutdown (n == 0), which read_from_fd must
    // translate into an empty vector rather than crashing or looping.
    sixseven_platform::socket_close(client_fd);

    auto received = read_from_fd(server_fd, 64);
    EXPECT_TRUE(received.empty());

    sixseven_platform::socket_close(server_fd);
}

TEST(QA_GDB1233_SocketTransfer, MultipleSequentialMessagesPreserveOrderAndBoundaries) {
    // Simulates the realistic pg-wire pattern used by ParamSubstitutionWire
    // tests: several distinct writes in sequence, each read back separately.
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    ASSERT_GE(server_fd, 0);

    std::vector<uint8_t> msg1 = pg_wire_test::build_query_message("SELECT 1;");
    std::vector<uint8_t> msg2 = pg_wire_test::build_sync_message();

    write_to_fd(client_fd, msg1);
    auto r1 = read_from_fd(server_fd);
    EXPECT_EQ(r1, msg1);

    write_to_fd(client_fd, msg2);
    auto r2 = read_from_fd(server_fd);
    EXPECT_EQ(r2, msg2);

    sixseven_platform::socket_close(client_fd);
    sixseven_platform::socket_close(server_fd);
}

TEST(QA_GDB1233_SocketTransfer, ParameterizedQueryRoundTripsCorrectly) {
    // Message-integrity sanity per the adversarial-focus item 4: a
    // parameterized wire query (Parse/Bind/Execute/Sync) must reassemble
    // byte-identical on the wire through the new send/recv path.
    int client_fd = -1;
    int server_fd = create_socketpair(client_fd);
    ASSERT_GE(server_fd, 0);

    auto parse = pg_wire_test::build_parse_message("", "SELECT * FROM t WHERE id = $1", {23});
    auto bind = pg_wire_test::build_bind_message(
        "", "", std::vector<std::optional<std::string>>{std::string("42")});
    auto execute = pg_wire_test::build_execute_message("", 0);
    auto sync = pg_wire_test::build_sync_message();

    std::vector<uint8_t> batch;
    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), execute.begin(), execute.end());
    batch.insert(batch.end(), sync.begin(), sync.end());

    write_to_fd(client_fd, batch);

    std::vector<uint8_t> received;
    while (received.size() < batch.size()) {
        auto chunk = read_from_fd(server_fd, batch.size() - received.size());
        ASSERT_FALSE(chunk.empty());
        received.insert(received.end(), chunk.begin(), chunk.end());
    }

    EXPECT_EQ(received, batch);

    sixseven_platform::socket_close(client_fd);
    sixseven_platform::socket_close(server_fd);
}

}  // namespace
