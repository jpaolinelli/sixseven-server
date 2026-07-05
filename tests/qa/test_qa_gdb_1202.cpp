// QA regression tests for GDB-1202.
//
// GDB-1202 strengthened tests/unit/test_wal_sender.cpp's
// WalSender.ConnectionErrorStopsSender from a vacuous assertion (which only
// verified that stop() drives state to STOPPED -- true even with connection
// detection fully removed) into a deadline-based poll that requires the
// sender to reach STOPPED *autonomously*, before stop() is ever called.
//
// This file adversarially probes:
//   (1) Flakiness of the new deadline-poll test under CPU load.
//   (2) Discrimination -- that the test would in fact fail (via timeout) if
//       connection-close detection were removed/broken.
//   (3) Coverage gaps -- connection-failure modes the sender's detection
//       paths (is_open() check, send() failure, receive() failure) do NOT
//       catch, which could leave a WalSender RUNNING forever in production.
//
// Detection paths in src/server/wal_sender.cpp run_streaming():
//   - send_wal_batch() / send_keepalive() propagate send() errors upward.
//   - process_replica_messages() calls receive(); NOT_FOUND/NETWORK_ERROR
//     from receive() are swallowed as ok() (line ~415-417 in wal_sender.cpp).
//   - explicit `if (!connection_->is_open()) return NETWORK_ERROR;` backstop,
//     evaluated once per loop iteration (bounded by keepalive_interval when
//     there is no new WAL and no incoming replica data).

#include "sixseven/server/replication_message.h"
#include "sixseven/server/wal_sender.h"
#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_record.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "test_wal_helpers.h"

using namespace sixseven;
using namespace sixseven::test;

namespace {

struct SharedBuffer {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<uint8_t> data;
    bool closed = false;
};

/// Same in-memory connection semantics as tests/unit/test_wal_sender.cpp's
/// InMemoryReplicationConnection, duplicated here (that class lives in an
/// anonymous namespace and is not exported) plus a few adversarial knobs:
///   - fail_send_only(): connection stays "open" (is_open() == true) but
///     send() always errors -- simulates a half-closed TCP write side.
///   - fail_receive_only(): is_open() stays true but receive() errors --
///     simulates a half-closed read side.
///   - flap(): close()/reopen() repeatedly from another thread.
class QaConnection : public ReplicationConnection {
public:
    QaConnection(std::shared_ptr<SharedBuffer> send_buf,
                 std::shared_ptr<SharedBuffer> recv_buf,
                 std::string peer)
        : send_buf_(std::move(send_buf)), recv_buf_(std::move(recv_buf)), peer_(std::move(peer)) {}

    Result<void> send(std::span<const uint8_t> data) override {
        if (force_send_fail_.load()) {
            return make_error(StatusCode::NETWORK_ERROR, "forced send failure");
        }
        if (!open_.load()) {
            return make_error(StatusCode::NETWORK_ERROR, "connection closed");
        }
        std::lock_guard lock(send_buf_->mu);
        send_buf_->data.insert(send_buf_->data.end(), data.begin(), data.end());
        send_buf_->cv.notify_all();
        return ok();
    }

    Result<std::vector<uint8_t>> receive(size_t max_bytes,
                                         std::chrono::milliseconds timeout) override {
        if (force_recv_fail_.load()) {
            return make_error(StatusCode::INTERNAL_ERROR, "forced receive failure");
        }
        if (!open_.load()) {
            return make_error(StatusCode::NETWORK_ERROR, "connection closed");
        }
        std::unique_lock lock(recv_buf_->mu);
        if (timeout.count() > 0) {
            recv_buf_->cv.wait_for(
                lock, timeout, [this] { return !recv_buf_->data.empty() || recv_buf_->closed; });
        }
        if (recv_buf_->data.empty()) {
            return ok(std::vector<uint8_t>{});
        }
        size_t to_read = std::min(max_bytes, recv_buf_->data.size());
        std::vector<uint8_t> result(recv_buf_->data.begin(),
                                    recv_buf_->data.begin() + static_cast<ptrdiff_t>(to_read));
        recv_buf_->data.erase(recv_buf_->data.begin(),
                              recv_buf_->data.begin() + static_cast<ptrdiff_t>(to_read));
        return ok(std::move(result));
    }

    void close() override {
        open_.store(false);
        {
            std::lock_guard lock(send_buf_->mu);
            send_buf_->closed = true;
        }
        send_buf_->cv.notify_all();
    }

    bool is_open() const override { return open_.load(); }

    std::string peer_description() const override { return peer_; }

    void reopen() { open_.store(true); }

    /// Simulate a half-closed connection: is_open() still reports true (as a
    /// real socket might transiently do), but every send() fails.
    void fail_send_only() { force_send_fail_.store(true); }

    /// Simulate a half-closed connection: is_open() still reports true, but
    /// every receive() fails with a non-NOT_FOUND/NETWORK_ERROR code (the
    /// only codes process_replica_messages() swallows).
    void fail_receive_only() { force_recv_fail_.store(true); }

    void inject_recv(std::span<const uint8_t> data) {
        std::lock_guard lock(recv_buf_->mu);
        recv_buf_->data.insert(recv_buf_->data.end(), data.begin(), data.end());
        recv_buf_->cv.notify_all();
    }

private:
    std::shared_ptr<SharedBuffer> send_buf_;
    std::shared_ptr<SharedBuffer> recv_buf_;
    std::string peer_;
    std::atomic<bool> open_{true};
    std::atomic<bool> force_send_fail_{false};
    std::atomic<bool> force_recv_fail_{false};
};

std::pair<std::unique_ptr<QaConnection>, std::unique_ptr<QaConnection>> make_pair() {
    auto buf_a = std::make_shared<SharedBuffer>();
    auto buf_b = std::make_shared<SharedBuffer>();
    auto conn_a = std::make_unique<QaConnection>(buf_a, buf_b, "qa-replica");
    auto conn_b = std::make_unique<QaConnection>(buf_b, buf_a, "qa-primary");
    return {std::move(conn_a), std::move(conn_b)};
}

/// Busy-spin for `dur` to create CPU contention on the box, used to stress
/// the deadline-poll test for flakiness.
void burn_cpu(std::chrono::milliseconds dur) {
    auto end = std::chrono::steady_clock::now() + dur;
    volatile uint64_t sink = 0;
    while (std::chrono::steady_clock::now() < end) {
        for (int i = 0; i < 100000; ++i) {
            sink += static_cast<uint64_t>(i) * 2654435761u;
        }
    }
}

WalSender::State wait_for_state(WalSender& sender,
                                 WalSender::State target,
                                 std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    WalSender::State observed = sender.state();
    while (observed != target && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        observed = sender.state();
    }
    return observed;
}

} // namespace

// -- (1) Flakiness stress: under normal load ----------------------------------

TEST(QA_GDB1202, DeadlinePollDetectsCloseRepeatedlyUnderNormalLoad) {
    constexpr int kRepeats = 30;
    int passed = 0;
    for (int i = 0; i < kRepeats; ++i) {
        TempWalDir dir;
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        auto [primary_conn, replica_conn] = make_pair();
        auto* primary_raw = primary_conn.get();

        WalSenderOptions opts;
        opts.keepalive_interval = std::chrono::milliseconds(50);
        opts.sender_timeout = std::chrono::milliseconds(5000);

        WalSender sender(std::move(primary_conn), dir.path(), nullptr, writer, opts);
        ASSERT_TRUE(sender.start_streaming(1).has_value());

        ASSERT_EQ(wait_for_state(sender, WalSender::State::STREAMING, std::chrono::seconds(5)),
                  WalSender::State::STREAMING);

        primary_raw->close();

        auto observed = wait_for_state(sender, WalSender::State::STOPPED, std::chrono::seconds(5));
        if (observed == WalSender::State::STOPPED) {
            ++passed;
        } else {
            ADD_FAILURE() << "iteration " << i << " did not reach STOPPED (state="
                          << static_cast<int>(observed) << ")";
        }

        sender.stop();
        ASSERT_TRUE(writer.close().has_value());
    }
    EXPECT_EQ(passed, kRepeats) << "pass rate under normal load: " << passed << "/" << kRepeats;
}

// -- (1) Flakiness stress: under CPU saturation -------------------------------

TEST(QA_GDB1202, DeadlinePollDetectsCloseUnderCpuSaturation) {
    std::atomic<bool> stop_burn{false};
    std::vector<std::thread> burners;
    unsigned hw = std::thread::hardware_concurrency();
    unsigned n_burners = hw > 1 ? hw : 2;
    for (unsigned i = 0; i < n_burners; ++i) {
        burners.emplace_back([&stop_burn] {
            while (!stop_burn.load()) {
                burn_cpu(std::chrono::milliseconds(20));
            }
        });
    }

    constexpr int kRepeats = 15;
    int passed = 0;
    for (int i = 0; i < kRepeats; ++i) {
        TempWalDir dir;
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        auto [primary_conn, replica_conn] = make_pair();
        auto* primary_raw = primary_conn.get();

        WalSenderOptions opts;
        opts.keepalive_interval = std::chrono::milliseconds(50);
        opts.sender_timeout = std::chrono::milliseconds(5000);

        WalSender sender(std::move(primary_conn), dir.path(), nullptr, writer, opts);
        ASSERT_TRUE(sender.start_streaming(1).has_value());

        auto streaming = wait_for_state(sender, WalSender::State::STREAMING, std::chrono::seconds(5));
        if (streaming != WalSender::State::STREAMING) {
            ADD_FAILURE() << "iteration " << i << " never reached STREAMING under load";
            sender.stop();
            ASSERT_TRUE(writer.close().has_value());
            continue;
        }

        primary_raw->close();

        auto observed = wait_for_state(sender, WalSender::State::STOPPED, std::chrono::seconds(5));
        if (observed == WalSender::State::STOPPED) {
            ++passed;
        } else {
            ADD_FAILURE() << "iteration " << i << " under CPU load did not reach STOPPED";
        }

        sender.stop();
        ASSERT_TRUE(writer.close().has_value());
    }

    stop_burn.store(true);
    for (auto& t : burners) t.join();

    EXPECT_EQ(passed, kRepeats) << "pass rate under CPU saturation: " << passed << "/" << kRepeats;
}

// -- (2) Discrimination: prove the poll would fail without detection ---------
//
// We can't remove production code from a QA test, but we can demonstrate the
// discriminating mechanism directly: if the sender's only path to STOPPED is
// an explicit stop() call (i.e. no autonomous detection), the poll below
// times out. This uses the sender_timeout-based path (replica silence)
// disabled (very large) and never closes the connection, to confirm the poll
// genuinely can observe "stuck at STREAMING" rather than always racing to
// pass regardless of what happens.
TEST(QA_GDB1202, PollTimesOutIfConnectionNeverClosed) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    auto [primary_conn, replica_conn] = make_pair();

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(50);
    opts.sender_timeout = std::chrono::hours(1); // effectively disabled

    WalSender sender(std::move(primary_conn), dir.path(), nullptr, writer, opts);
    ASSERT_TRUE(sender.start_streaming(1).has_value());

    ASSERT_EQ(wait_for_state(sender, WalSender::State::STREAMING, std::chrono::seconds(5)),
              WalSender::State::STREAMING);

    // Do NOT close the connection. The sender must remain STREAMING; a short
    // poll (well under the 5s ticket timeout) must observe it does NOT
    // transition to STOPPED on its own.
    auto observed = wait_for_state(sender, WalSender::State::STOPPED, std::chrono::milliseconds(500));
    EXPECT_EQ(observed, WalSender::State::STREAMING)
        << "sender transitioned to STOPPED without any connection error or stop() call";

    sender.stop();
    EXPECT_EQ(sender.state(), WalSender::State::STOPPED);
    ASSERT_TRUE(writer.close().has_value());
}

// -- (3) Coverage gap probes: connection-failure modes ------------------------

// Close during the initial handshake / catch-up phase, before STREAMING.
TEST(QA_GDB1202, ConnectionClosedDuringCatchupIsDetected) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());
    // Seed some WAL so catch-up has data to send, giving close() a chance to
    // race against the catch-up send path.
    write_committed_txn(writer, 1, 1, "row-a");

    auto [primary_conn, replica_conn] = make_pair();
    auto* primary_raw = primary_conn.get();

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(50);
    opts.sender_timeout = std::chrono::milliseconds(5000);

    WalSender sender(std::move(primary_conn), dir.path(), nullptr, writer, opts);
    ASSERT_TRUE(sender.start_streaming(1).has_value());

    // Close immediately -- before waiting for STREAMING -- to hit the
    // catch-up path's send_wal_batch()/send_message() error branch.
    primary_raw->close();

    auto observed = wait_for_state(sender, WalSender::State::STOPPED, std::chrono::seconds(5));
    EXPECT_EQ(observed, WalSender::State::STOPPED)
        << "sender did not detect connection closed during catch-up/handshake";

    sender.stop();
    ASSERT_TRUE(writer.close().has_value());
}

// Half-closed connection: is_open() still true, but send() fails (e.g. TCP
// write-side reset). This must be detected via the send-error propagation
// path, not the is_open() backstop.
TEST(QA_GDB1202, SendFailureWithIsOpenStillTrueIsDetected) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    auto [primary_conn, replica_conn] = make_pair();
    auto* primary_raw = primary_conn.get();

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(50);
    opts.sender_timeout = std::chrono::milliseconds(5000);

    WalSender sender(std::move(primary_conn), dir.path(), nullptr, writer, opts);
    ASSERT_TRUE(sender.start_streaming(1).has_value());

    ASSERT_EQ(wait_for_state(sender, WalSender::State::STREAMING, std::chrono::seconds(5)),
              WalSender::State::STREAMING);

    // Do NOT call close() -- is_open() remains true. Force every send() to
    // fail, simulating a half-closed / reset write side.
    primary_raw->fail_send_only();

    auto observed = wait_for_state(sender, WalSender::State::STOPPED, std::chrono::seconds(5));
    EXPECT_EQ(observed, WalSender::State::STOPPED)
        << "sender did not stop when send() started failing while is_open() remained true "
           "-- this would hang a real replication connection with a half-closed write side";

    sender.stop();
    ASSERT_TRUE(writer.close().has_value());
}

// Half-closed connection: is_open() still true, receive() fails with a code
// that process_replica_messages() does NOT swallow (only NOT_FOUND /
// NETWORK_ERROR are treated as "no data"). This should propagate as an error
// and stop the sender via the run_streaming() error path, NOT via the
// is_open() backstop (which would not fire here).
TEST(QA_GDB1202, ReceiveFailureWithIsOpenStillTrueIsDetected) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    auto [primary_conn, replica_conn] = make_pair();
    auto* primary_raw = primary_conn.get();

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(50);
    opts.sender_timeout = std::chrono::milliseconds(5000);

    WalSender sender(std::move(primary_conn), dir.path(), nullptr, writer, opts);
    ASSERT_TRUE(sender.start_streaming(1).has_value());

    ASSERT_EQ(wait_for_state(sender, WalSender::State::STREAMING, std::chrono::seconds(5)),
              WalSender::State::STREAMING);

    primary_raw->fail_receive_only();

    auto observed = wait_for_state(sender, WalSender::State::STOPPED, std::chrono::seconds(5));
    EXPECT_EQ(observed, WalSender::State::STOPPED)
        << "sender did not stop when receive() started failing (non-NOT_FOUND/NETWORK_ERROR) "
           "while is_open() remained true -- a real half-closed read side would hang the sender "
           "forever if this path is not covered";

    sender.stop();
    ASSERT_TRUE(writer.close().has_value());
}

// Repeated close/reopen flapping -- the sender should latch onto STOPPED on
// the first genuine close it observes and never bounce back to STREAMING.
TEST(QA_GDB1202, FlappingConnectionStillLatchesToStopped) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    auto [primary_conn, replica_conn] = make_pair();
    auto* primary_raw = primary_conn.get();

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(30);
    opts.sender_timeout = std::chrono::milliseconds(5000);

    WalSender sender(std::move(primary_conn), dir.path(), nullptr, writer, opts);
    ASSERT_TRUE(sender.start_streaming(1).has_value());

    ASSERT_EQ(wait_for_state(sender, WalSender::State::STREAMING, std::chrono::seconds(5)),
              WalSender::State::STREAMING);

    // Flap close/reopen a few times rapidly, then leave it closed.
    for (int i = 0; i < 5; ++i) {
        primary_raw->close();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        primary_raw->reopen();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    primary_raw->close();

    auto observed = wait_for_state(sender, WalSender::State::STOPPED, std::chrono::seconds(5));
    EXPECT_EQ(observed, WalSender::State::STOPPED)
        << "sender did not settle into STOPPED after a flapping connection was left closed";

    // Once STOPPED, it must not silently flip back to STREAMING (the state
    // machine has no path back from STOPPED, but assert it explicitly).
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(sender.state(), WalSender::State::STOPPED);

    sender.stop();
    ASSERT_TRUE(writer.close().has_value());
}

// Graceful close (well-formed) vs abrupt close both funnel through the same
// close()/is_open() surface in this transport abstraction, but verify both
// orderings explicitly: close while idle-waiting on the condvar (no WAL, no
// keepalive fired yet) vs close right as a keepalive is due.
TEST(QA_GDB1202, CloseWhileIdleBeforeKeepaliveFires) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    auto [primary_conn, replica_conn] = make_pair();
    auto* primary_raw = primary_conn.get();

    WalSenderOptions opts;
    // Long keepalive interval so the idle wait dominates; connection close
    // must still be detected within the ticket's 5s window via the
    // keepalive-interval-bounded is_open() check on the *next* wake, which
    // for a long interval means detection latency approaches the interval.
    opts.keepalive_interval = std::chrono::milliseconds(200);
    opts.sender_timeout = std::chrono::milliseconds(5000);

    WalSender sender(std::move(primary_conn), dir.path(), nullptr, writer, opts);
    ASSERT_TRUE(sender.start_streaming(1).has_value());

    ASSERT_EQ(wait_for_state(sender, WalSender::State::STREAMING, std::chrono::seconds(5)),
              WalSender::State::STREAMING);

    // Close almost immediately after reaching STREAMING, well before the
    // 200ms keepalive interval elapses.
    primary_raw->close();

    auto observed = wait_for_state(sender, WalSender::State::STOPPED, std::chrono::seconds(5));
    EXPECT_EQ(observed, WalSender::State::STOPPED);

    sender.stop();
    ASSERT_TRUE(writer.close().has_value());
}
