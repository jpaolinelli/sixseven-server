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

// -- In-memory replication connection for testing -----------------------------

namespace {

struct SharedBuffer {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<uint8_t> data;
    bool closed = false;
};

class InMemoryReplicationConnection : public ReplicationConnection {
public:
    InMemoryReplicationConnection(std::shared_ptr<SharedBuffer> send_buf,
                                  std::shared_ptr<SharedBuffer> recv_buf,
                                  std::string peer)
        : send_buf_(std::move(send_buf)), recv_buf_(std::move(recv_buf)), peer_(std::move(peer)) {}

    Result<void> send(std::span<const uint8_t> data) override {
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

    /// Drain all data that was sent by this connection.
    std::vector<uint8_t> drain_sent() {
        std::lock_guard lock(send_buf_->mu);
        std::vector<uint8_t> result;
        result.swap(send_buf_->data);
        return result;
    }

    /// Wait until at least `n` bytes are in the send buffer.
    bool wait_for_sent(size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lock(send_buf_->mu);
        return send_buf_->cv.wait_for(lock, timeout, [&] { return send_buf_->data.size() >= n; });
    }

    /// Inject data into the receive buffer (simulating replica -> primary).
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
};

/// Create a pair of connected in-memory connections.
std::pair<std::unique_ptr<InMemoryReplicationConnection>,
          std::unique_ptr<InMemoryReplicationConnection>>
create_connection_pair() {
    auto buf_a = std::make_shared<SharedBuffer>();
    auto buf_b = std::make_shared<SharedBuffer>();
    auto conn_a = std::make_unique<InMemoryReplicationConnection>(buf_a, buf_b, "replica-a");
    auto conn_b = std::make_unique<InMemoryReplicationConnection>(buf_b, buf_a, "primary-b");
    return {std::move(conn_a), std::move(conn_b)};
}

/// Helper to parse all messages from raw bytes.
std::vector<ReplicationMessageType> parse_message_types(const std::vector<uint8_t>& data) {
    std::vector<ReplicationMessageType> types;
    size_t offset = 0;
    while (offset < data.size()) {
        std::span<const uint8_t> remaining(data.data() + offset, data.size() - offset);
        if (remaining.size() < replication_header_size)
            break;
        auto type = peek_message_type(remaining);
        if (!type)
            break;
        auto size = message_total_size(remaining);
        if (!size)
            break;
        if (remaining.size() < *size)
            break;
        types.push_back(*type);
        offset += *size;
    }
    return types;
}

} // namespace

// -- Tests --------------------------------------------------------------------

TEST(WalSender, CatchUpFromWalDirectory) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    // Write some WAL records.
    write_committed_txn(writer, 1, 1, "data1");
    write_committed_txn(writer, 2, 1, "data2");
    ASSERT_TRUE(writer.flush().has_value());

    auto [primary_conn, replica_conn] = create_connection_pair();
    auto* primary_raw = primary_conn.get();

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(50);
    opts.sender_timeout = std::chrono::milliseconds(5000);

    WalSender sender(std::move(primary_conn), dir.path(), nullptr, writer, opts);
    ASSERT_TRUE(sender.start_streaming(1).has_value());

    // Wait for data to be sent.
    primary_raw->wait_for_sent(replication_header_size, std::chrono::milliseconds(2000));

    // Give it a moment to complete catch-up.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    sender.stop();
    EXPECT_EQ(sender.state(), WalSender::State::STOPPED);

    // Verify messages were sent.
    auto sent = primary_raw->drain_sent();
    auto types = parse_message_types(sent);
    ASSERT_FALSE(types.empty());

    // Should contain at least one WAL_DATA and a CATCHUP_COMPLETE.
    bool has_wal_data = false;
    bool has_catchup_complete = false;
    for (auto t : types) {
        if (t == ReplicationMessageType::WAL_DATA)
            has_wal_data = true;
        if (t == ReplicationMessageType::CATCHUP_COMPLETE)
            has_catchup_complete = true;
    }
    EXPECT_TRUE(has_wal_data);
    EXPECT_TRUE(has_catchup_complete);

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalSender, RealTimeStreaming) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    auto [primary_conn, replica_conn] = create_connection_pair();
    auto* primary_raw = primary_conn.get();

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(50);
    opts.sender_timeout = std::chrono::milliseconds(5000);

    WalSender sender(std::move(primary_conn), dir.path(), nullptr, writer, opts);

    // Start with LSN 1 (nothing written yet, so catch-up should be instant).
    ASSERT_TRUE(sender.start_streaming(1).has_value());

    // Wait for catch-up to complete.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Now write new WAL records and notify.
    write_committed_txn(writer, 1, 1, "realtime_data");
    ASSERT_TRUE(writer.flush().has_value());
    sender.notify_new_wal(writer.flushed_lsn());

    // Wait for data to be sent.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    sender.stop();

    auto sent = primary_raw->drain_sent();
    auto types = parse_message_types(sent);

    // Should contain CatchupComplete + at least one WalData from real-time.
    bool has_wal_data = false;
    bool has_catchup_complete = false;
    for (auto t : types) {
        if (t == ReplicationMessageType::WAL_DATA)
            has_wal_data = true;
        if (t == ReplicationMessageType::CATCHUP_COMPLETE)
            has_catchup_complete = true;
    }
    EXPECT_TRUE(has_catchup_complete);
    EXPECT_TRUE(has_wal_data);

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalSender, KeepaliveGeneration) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    auto [primary_conn, replica_conn] = create_connection_pair();
    auto* primary_raw = primary_conn.get();

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(50); // Short for testing.
    opts.sender_timeout = std::chrono::milliseconds(5000);

    WalSender sender(std::move(primary_conn), dir.path(), nullptr, writer, opts);
    ASSERT_TRUE(sender.start_streaming(1).has_value());

    // Wait long enough for a keepalive to be generated.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    sender.stop();

    auto sent = primary_raw->drain_sent();
    auto types = parse_message_types(sent);

    bool has_keepalive = false;
    for (auto t : types) {
        if (t == ReplicationMessageType::KEEPALIVE)
            has_keepalive = true;
    }
    EXPECT_TRUE(has_keepalive);

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalSender, StandbyStatusHandling) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    auto [primary_conn, replica_conn] = create_connection_pair();
    auto* primary_raw = primary_conn.get();

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(100);
    opts.sender_timeout = std::chrono::milliseconds(5000);

    WalSender sender(std::move(primary_conn), dir.path(), nullptr, writer, opts);
    ASSERT_TRUE(sender.start_streaming(1).has_value());

    // Inject a StandbyStatus message from the replica side.
    StandbyStatusMessage status;
    status.received_lsn = 50;
    status.applied_lsn = 40;
    status.flushed_lsn = 30;
    status.timestamp_us = 12345;
    auto status_buf = serialize_standby_status(status);
    primary_raw->inject_recv(status_buf);

    // Wait for the sender to process it.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    EXPECT_EQ(sender.replica_received_lsn(), 50u);
    EXPECT_EQ(sender.replica_applied_lsn(), 40u);
    EXPECT_EQ(sender.replica_flushed_lsn(), 30u);

    sender.stop();
    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalSender, PeerDescription) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    auto [primary_conn, replica_conn] = create_connection_pair();

    WalSender sender(std::move(primary_conn), dir.path(), nullptr, writer);
    EXPECT_EQ(sender.peer_description(), "replica-a");

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalSender, ConnectionErrorStopsSender) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    auto [primary_conn, replica_conn] = create_connection_pair();
    auto* primary_raw = primary_conn.get();

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(50);
    opts.sender_timeout = std::chrono::milliseconds(5000);

    WalSender sender(std::move(primary_conn), dir.path(), nullptr, writer, opts);
    ASSERT_TRUE(sender.start_streaming(1).has_value());

    // Wait for catch-up to complete (sender reaches STREAMING) before severing the
    // connection, so the closure below is unambiguously what drives the later
    // state transition rather than racing initial catch-up.
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (sender.state() != WalSender::State::STREAMING &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_EQ(sender.state(), WalSender::State::STREAMING);
    }

    // Close the connection from the outside. Do NOT call sender.stop() here --
    // stop() unconditionally drives the sender to STOPPED regardless of whether
    // the sender itself ever detected the closed connection, which would make
    // the final assertion pass vacuously even if connection-close detection were
    // removed. Instead, poll state() with a deadline and require that the sender
    // transitions to STOPPED on its own, as a *result* of detecting the closed
    // connection inside its streaming loop (see streaming_loop() in
    // wal_sender.cpp: run_streaming() returns an error once send()/receive()
    // fail on the closed connection, and the loop then stores State::STOPPED).
    primary_raw->close();

    constexpr auto poll_interval = std::chrono::milliseconds(10);
    constexpr auto poll_timeout = std::chrono::seconds(5);
    auto deadline = std::chrono::steady_clock::now() + poll_timeout;
    WalSender::State observed = sender.state();
    while (observed != WalSender::State::STOPPED && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(poll_interval);
        observed = sender.state();
    }

    // The sender must have detected the closed connection and stopped itself
    // without any external stop() call. If connection-close detection regresses,
    // the sender stays STREAMING forever and this poll times out, failing the test.
    EXPECT_EQ(observed, WalSender::State::STOPPED)
        << "WalSender did not autonomously detect the closed connection within "
        << poll_timeout.count() << "s";

    // Bring the background thread down cleanly; state is already STOPPED so this
    // only joins the thread.
    sender.stop();

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalSender, DoubleStartFails) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    auto [primary_conn, replica_conn] = create_connection_pair();

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(100);
    opts.sender_timeout = std::chrono::milliseconds(5000);

    WalSender sender(std::move(primary_conn), dir.path(), nullptr, writer, opts);
    ASSERT_TRUE(sender.start_streaming(1).has_value());

    auto result = sender.start_streaming(1);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::REPLICATION_ERROR);

    sender.stop();
    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalSender, InitialState) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    auto [primary_conn, replica_conn] = create_connection_pair();

    WalSender sender(std::move(primary_conn), dir.path(), nullptr, writer);
    EXPECT_EQ(sender.state(), WalSender::State::CREATED);
    EXPECT_EQ(sender.replica_received_lsn(), invalid_lsn);
    EXPECT_EQ(sender.replica_applied_lsn(), invalid_lsn);
    EXPECT_EQ(sender.replica_flushed_lsn(), invalid_lsn);
    EXPECT_EQ(sender.replica_xmin(), 0u);

    ASSERT_TRUE(writer.close().has_value());
}
