#include "sixseven/server/replication_message.h"
#include "sixseven/server/wal_receiver.h"
#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_record.h"

#include <gtest/gtest.h>

#include <algorithm>
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
        if (recv_buf_->closed && recv_buf_->data.empty()) {
            return make_error(StatusCode::NETWORK_ERROR, "connection closed");
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
        {
            std::lock_guard lock(recv_buf_->mu);
            recv_buf_->closed = true;
        }
        recv_buf_->cv.notify_all();
    }

    bool is_open() const override { return open_.load(); }
    std::string peer_description() const override { return peer_; }

    void inject_recv(std::span<const uint8_t> data) {
        std::lock_guard lock(recv_buf_->mu);
        recv_buf_->data.insert(recv_buf_->data.end(), data.begin(), data.end());
        recv_buf_->cv.notify_all();
    }

    bool wait_for_sent(size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lock(send_buf_->mu);
        return send_buf_->cv.wait_for(lock, timeout, [&] { return send_buf_->data.size() >= n; });
    }

    std::vector<uint8_t> drain_sent() {
        std::lock_guard lock(send_buf_->mu);
        std::vector<uint8_t> result;
        result.swap(send_buf_->data);
        return result;
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
    auto conn_a = std::make_unique<InMemoryReplicationConnection>(buf_a, buf_b, "standby");
    auto conn_b = std::make_unique<InMemoryReplicationConnection>(buf_b, buf_a, "primary");
    return {std::move(conn_a), std::move(conn_b)};
}

/// Mock recovery handler that tracks redo calls.
class MockRecoveryHandler : public RecoveryHandler {
public:
    Result<void> redo(const WalRecord& record) override {
        std::lock_guard lock(mu_);
        redo_records_.push_back(record);
        redo_cv_.notify_all();
        return ok();
    }

    Result<void> undo(const WalRecord& /*record*/) override {
        // Not used in standby replay.
        return ok();
    }

    std::vector<WalRecord> get_redo_records() {
        std::lock_guard lock(mu_);
        return redo_records_;
    }

    bool wait_for_redo_count(size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mu_);
        return redo_cv_.wait_for(lock, timeout, [&] { return redo_records_.size() >= count; });
    }

private:
    std::mutex mu_;
    std::condition_variable redo_cv_;
    std::vector<WalRecord> redo_records_;
};

/// Build a WalDataMessage with the given records.
WalDataMessage make_wal_data(const std::vector<WalRecord>& records) {
    WalDataMessage msg;
    if (records.empty()) {
        return msg;
    }
    msg.start_lsn = records.front().lsn;
    msg.end_lsn = records.back().lsn + 1;
    msg.record_count = static_cast<uint32_t>(records.size());

    for (const auto& rec : records) {
        auto bytes = serialize_wal_record(rec);
        msg.data.insert(msg.data.end(), bytes.begin(), bytes.end());
    }

    return msg;
}

} // namespace

// =============================================================================
// WalReceiver Tests
// =============================================================================

class WalReceiverTest : public ::testing::Test {
protected:
    void SetUp() override {
        wal_dir_ = std::make_unique<TempWalDir>();
        handler_ = std::make_unique<MockRecoveryHandler>();
    }

    void TearDown() override {
        // Ensure receiver is stopped before cleanup.
        if (receiver_) {
            receiver_->stop();
            receiver_.reset();
        }
        handler_.reset();
        wal_dir_.reset();
    }

    std::unique_ptr<TempWalDir> wal_dir_;
    std::unique_ptr<MockRecoveryHandler> handler_;
    std::unique_ptr<WalReceiver> receiver_;

    // The "primary side" connection for injecting data.
    InMemoryReplicationConnection* primary_conn_ = nullptr;
};

TEST_F(WalReceiverTest, StartAndStop) {
    // Factory that creates a connected pair.
    auto factory = [this](const std::string& /*host*/,
                          uint16_t /*port*/) -> Result<std::unique_ptr<ReplicationConnection>> {
        auto [standby_conn, primary_conn] = create_connection_pair();
        primary_conn_ = primary_conn.get();
        // Keep primary_conn alive by storing it — but for this test we don't need it.
        return ok(std::unique_ptr<ReplicationConnection>(std::move(standby_conn)));
    };

    WalReceiverOptions opts;
    opts.receive_timeout = std::chrono::milliseconds(100);
    receiver_ = std::make_unique<WalReceiver>(factory, wal_dir_->path(), *handler_, opts);

    auto result = receiver_->start("localhost", 6767);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Give it a moment to connect.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto state = receiver_->get_state();
    EXPECT_TRUE(state.is_streaming);

    receiver_->stop();
    state = receiver_->get_state();
    EXPECT_FALSE(state.is_streaming);
}

TEST_F(WalReceiverTest, ReceiveAndReplayWalData) {
    std::unique_ptr<InMemoryReplicationConnection> primary_side;

    auto factory = [&](const std::string& /*host*/,
                       uint16_t /*port*/) -> Result<std::unique_ptr<ReplicationConnection>> {
        auto [standby_conn, primary_conn] = create_connection_pair();
        primary_side = std::move(primary_conn);
        return ok(std::unique_ptr<ReplicationConnection>(std::move(standby_conn)));
    };

    WalReceiverOptions opts;
    opts.receive_timeout = std::chrono::milliseconds(200);
    receiver_ = std::make_unique<WalReceiver>(factory, wal_dir_->path(), *handler_, opts);

    auto result = receiver_->start("localhost", 6767);
    ASSERT_TRUE(result.has_value());

    // Wait for connection.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Create WAL records and send as a WalDataMessage.
    WalRecord insert;
    insert.lsn = 1;
    insert.txn_id = 100;
    insert.type = WalRecordType::INSERT;
    insert.table_id = 1;
    insert.data = {0x01, 0x02, 0x03};

    WalRecord update;
    update.lsn = 2;
    update.txn_id = 100;
    update.type = WalRecordType::UPDATE;
    update.table_id = 1;
    update.data = {0x04, 0x05};

    auto wal_msg = make_wal_data({insert, update});
    auto serialized = serialize_wal_data(wal_msg);
    auto send_result = primary_side->send(serialized);
    ASSERT_TRUE(send_result.has_value());

    // Wait for records to be replayed.
    ASSERT_TRUE(handler_->wait_for_redo_count(2, std::chrono::milliseconds(2000)));

    auto redo_records = handler_->get_redo_records();
    ASSERT_EQ(redo_records.size(), 2);
    EXPECT_EQ(redo_records[0].type, WalRecordType::INSERT);
    EXPECT_EQ(redo_records[0].table_id, 1u);
    EXPECT_EQ(redo_records[1].type, WalRecordType::UPDATE);

    // Check state was updated.
    auto state = receiver_->get_state();
    EXPECT_NE(state.received_lsn, invalid_lsn);
    EXPECT_NE(state.applied_lsn, invalid_lsn);
}

TEST_F(WalReceiverTest, SkipsNonDataRecords) {
    std::unique_ptr<InMemoryReplicationConnection> primary_side;

    auto factory = [&](const std::string& /*host*/,
                       uint16_t /*port*/) -> Result<std::unique_ptr<ReplicationConnection>> {
        auto [standby_conn, primary_conn] = create_connection_pair();
        primary_side = std::move(primary_conn);
        return ok(std::unique_ptr<ReplicationConnection>(std::move(standby_conn)));
    };

    WalReceiverOptions opts;
    opts.receive_timeout = std::chrono::milliseconds(200);
    receiver_ = std::make_unique<WalReceiver>(factory, wal_dir_->path(), *handler_, opts);

    auto result = receiver_->start("localhost", 6767);
    ASSERT_TRUE(result.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send BEGIN and COMMIT records — these should be skipped.
    WalRecord begin;
    begin.lsn = 1;
    begin.txn_id = 100;
    begin.type = WalRecordType::BEGIN;

    WalRecord insert;
    insert.lsn = 2;
    insert.txn_id = 100;
    insert.type = WalRecordType::INSERT;
    insert.table_id = 1;
    insert.data = {0x01};

    WalRecord commit;
    commit.lsn = 3;
    commit.txn_id = 100;
    commit.type = WalRecordType::COMMIT;

    auto wal_msg = make_wal_data({begin, insert, commit});
    auto serialized = serialize_wal_data(wal_msg);
    auto send_r = primary_side->send(serialized);
    ASSERT_TRUE(send_r.has_value());

    // Only the INSERT should be replayed (BEGIN and COMMIT are skipped).
    ASSERT_TRUE(handler_->wait_for_redo_count(1, std::chrono::milliseconds(2000)));

    auto redo_records = handler_->get_redo_records();
    ASSERT_EQ(redo_records.size(), 1);
    EXPECT_EQ(redo_records[0].type, WalRecordType::INSERT);
}

TEST_F(WalReceiverTest, HandlesKeepalive) {
    std::unique_ptr<InMemoryReplicationConnection> primary_side;

    auto factory = [&](const std::string& /*host*/,
                       uint16_t /*port*/) -> Result<std::unique_ptr<ReplicationConnection>> {
        auto [standby_conn, primary_conn] = create_connection_pair();
        primary_side = std::move(primary_conn);
        return ok(std::unique_ptr<ReplicationConnection>(std::move(standby_conn)));
    };

    WalReceiverOptions opts;
    opts.receive_timeout = std::chrono::milliseconds(200);
    receiver_ = std::make_unique<WalReceiver>(factory, wal_dir_->path(), *handler_, opts);

    auto result = receiver_->start("localhost", 6767);
    ASSERT_TRUE(result.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send a keepalive with reply_requested = true.
    KeepaliveMessage ka;
    ka.current_lsn = 42;
    ka.timestamp_us = 1000000;
    ka.reply_requested = true;
    auto serialized = serialize_keepalive(ka);
    auto send_r = primary_side->send(serialized);
    ASSERT_TRUE(send_r.has_value());

    // Wait for the standby status reply (arrives in primary's receive buffer).
    auto reply = primary_side->receive(4096, std::chrono::milliseconds(2000));
    ASSERT_TRUE(reply.has_value()) << reply.error().message;
    ASSERT_GE(reply->size(), replication_header_size);

    auto type = peek_message_type(*reply);
    ASSERT_TRUE(type.has_value());
    EXPECT_EQ(*type, ReplicationMessageType::STANDBY_STATUS);
}

TEST_F(WalReceiverTest, ReconnectsOnConnectionFailure) {
    std::atomic<int> connect_count{0};
    std::unique_ptr<InMemoryReplicationConnection> primary_side;

    auto factory = [&](const std::string& /*host*/,
                       uint16_t /*port*/) -> Result<std::unique_ptr<ReplicationConnection>> {
        int count = ++connect_count;
        if (count == 1) {
            // First connection fails.
            return make_error(StatusCode::NETWORK_ERROR, "connection refused");
        }
        // Subsequent connections succeed.
        auto [standby_conn, primary_conn] = create_connection_pair();
        primary_side = std::move(primary_conn);
        return ok(std::unique_ptr<ReplicationConnection>(std::move(standby_conn)));
    };

    WalReceiverOptions opts;
    opts.retry_interval = std::chrono::milliseconds(100);
    opts.max_retry_interval = std::chrono::milliseconds(200);
    opts.receive_timeout = std::chrono::milliseconds(200);
    receiver_ = std::make_unique<WalReceiver>(factory, wal_dir_->path(), *handler_, opts);

    auto result = receiver_->start("localhost", 6767);
    ASSERT_TRUE(result.has_value());

    // Wait for reconnection (first attempt fails, second succeeds).
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    EXPECT_GE(connect_count.load(), 2);

    auto state = receiver_->get_state();
    EXPECT_TRUE(state.is_streaming);
}

TEST_F(WalReceiverTest, ExponentialBackoff) {
    std::atomic<int> connect_count{0};
    std::mutex timestamps_mutex;
    std::vector<std::chrono::steady_clock::time_point> attempt_times;

    // Factory that always fails — to test backoff behavior. Records the wall
    // clock time of each connection attempt so we can inspect the gaps
    // between retries directly, rather than inferring backoff from a loose
    // total-attempt-count bound (see GDB-1200: a bound of "< 10" sits exactly
    // on the no-backoff attempt count and does not reliably catch a
    // regression that removes the doubling).
    auto factory = [&](const std::string& /*host*/,
                       uint16_t /*port*/) -> Result<std::unique_ptr<ReplicationConnection>> {
        ++connect_count;
        {
            std::lock_guard lock(timestamps_mutex);
            attempt_times.push_back(std::chrono::steady_clock::now());
        }
        return make_error(StatusCode::NETWORK_ERROR, "connection refused");
    };

    const auto initial_interval = std::chrono::milliseconds(50);
    WalReceiverOptions opts;
    opts.retry_interval = initial_interval;
    opts.max_retry_interval = std::chrono::milliseconds(200);
    opts.receive_timeout = std::chrono::milliseconds(100);
    receiver_ = std::make_unique<WalReceiver>(factory, wal_dir_->path(), *handler_, opts);

    auto result = receiver_->start("localhost", 6767);
    ASSERT_TRUE(result.has_value());

    // With 50ms initial interval and 200ms max, over 500ms we should see
    // fewer attempts than if there were no backoff.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    int attempts = connect_count.load();
    // With no backoff at 50ms interval we'd get ~10 attempts.
    // With backoff (50, 100, 200, 200...) we'd get around 4-6.
    EXPECT_GT(attempts, 1);
    EXPECT_LT(attempts, 10);

    // Strengthened assertion: inspect the actual inter-attempt gaps rather
    // than relying solely on the total attempt count, which is only a weak
    // proxy for backoff (a regression that removes the doubling in
    // wal_receiver.cpp still produces ~9 attempts in 500ms, comfortably
    // under the "< 10" bound above). We instead assert directly that the
    // gaps grow, and that at least one gap is clearly larger than the
    // initial retry interval -- proof the doubling actually happened.
    //
    // Timing-based, not fully deterministic (the factory records real
    // wall-clock timestamps rather than injected/mocked sleep durations,
    // since WalReceiver does not expose a fake-clock seam). Tolerances are
    // deliberately generous to stay robust to scheduler jitter on this
    // Windows box:
    //   - "non-decreasing" allows a small negative slack (25% of the
    //     initial interval) so a gap that is marginally shorter than the
    //     previous one due to scheduling noise doesn't fail the test.
    //   - the "backoff happened" check only requires a gap >= 1.5x the
    //     initial interval (not the full theoretical 2x), and only for ONE
    //     gap among all recorded gaps.
    std::vector<std::chrono::milliseconds> gaps;
    {
        std::lock_guard lock(timestamps_mutex);
        ASSERT_GE(attempt_times.size(), 3u)
            << "need at least 3 attempts to observe two gaps and confirm backoff growth";
        for (size_t i = 1; i < attempt_times.size(); ++i) {
            gaps.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(
                attempt_times[i] - attempt_times[i - 1]));
        }
    }

    const auto slack =
        std::chrono::milliseconds(static_cast<long long>(initial_interval.count() / 4));
    for (size_t i = 1; i < gaps.size(); ++i) {
        EXPECT_GE(gaps[i] + slack, gaps[i - 1])
            << "gap[" << i << "]=" << gaps[i].count() << "ms should be roughly "
            << ">= gap[" << (i - 1) << "]=" << gaps[i - 1].count()
            << "ms (allowing jitter slack), demonstrating non-decreasing backoff";
    }

    const auto backoff_threshold =
        std::chrono::milliseconds(static_cast<long long>(initial_interval.count() * 3 / 2));
    bool saw_backoff_growth = std::any_of(
        gaps.begin(), gaps.end(), [&](const auto& gap) { return gap >= backoff_threshold; });
    EXPECT_TRUE(saw_backoff_growth)
        << "expected at least one inter-attempt gap >= " << backoff_threshold.count()
        << "ms (1.5x the " << initial_interval.count()
        << "ms initial interval), proving the retry interval doubled; if this "
           "fails, backoff growth (wal_receiver.cpp retry_interval *= 2) may be broken";
}

TEST_F(WalReceiverTest, GetStateInitial) {
    auto factory = [](const std::string& /*host*/,
                      uint16_t /*port*/) -> Result<std::unique_ptr<ReplicationConnection>> {
        return make_error(StatusCode::NETWORK_ERROR, "not implemented");
    };

    WalReceiverOptions opts;
    opts.retry_interval = std::chrono::milliseconds(5000);
    receiver_ = std::make_unique<WalReceiver>(factory, wal_dir_->path(), *handler_, opts);

    auto state = receiver_->get_state();
    EXPECT_EQ(state.received_lsn, invalid_lsn);
    EXPECT_EQ(state.applied_lsn, invalid_lsn);
    EXPECT_EQ(state.flushed_lsn, invalid_lsn);
    EXPECT_FALSE(state.is_streaming);
}

TEST_F(WalReceiverTest, SendsStatusAfterWalData) {
    std::unique_ptr<InMemoryReplicationConnection> primary_side;

    auto factory = [&](const std::string& /*host*/,
                       uint16_t /*port*/) -> Result<std::unique_ptr<ReplicationConnection>> {
        auto [standby_conn, primary_conn] = create_connection_pair();
        primary_side = std::move(primary_conn);
        return ok(std::unique_ptr<ReplicationConnection>(std::move(standby_conn)));
    };

    WalReceiverOptions opts;
    opts.receive_timeout = std::chrono::milliseconds(200);
    receiver_ = std::make_unique<WalReceiver>(factory, wal_dir_->path(), *handler_, opts);

    auto result = receiver_->start("localhost", 6767);
    ASSERT_TRUE(result.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send WAL data.
    WalRecord insert;
    insert.lsn = 10;
    insert.txn_id = 1;
    insert.type = WalRecordType::INSERT;
    insert.table_id = 1;
    insert.data = {0x01};

    auto wal_msg = make_wal_data({insert});
    auto serialized = serialize_wal_data(wal_msg);
    auto send_r = primary_side->send(serialized);
    ASSERT_TRUE(send_r.has_value());

    // Wait for status reply (arrives in primary's receive buffer).
    auto reply = primary_side->receive(4096, std::chrono::milliseconds(2000));
    ASSERT_TRUE(reply.has_value()) << reply.error().message;
    ASSERT_GE(reply->size(), replication_header_size);

    auto type = peek_message_type(*reply);
    ASSERT_TRUE(type.has_value());
    EXPECT_EQ(*type, ReplicationMessageType::STANDBY_STATUS);

    // Parse the status message.
    auto status = deserialize_standby_status(*reply);
    ASSERT_TRUE(status.has_value());
    EXPECT_NE(status->applied_lsn, invalid_lsn);
}
