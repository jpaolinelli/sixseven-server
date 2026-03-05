#include "sixseven/server/replication_connection.h"
#include "sixseven/server/replication_message.h"
#include "sixseven/server/wal_sender_manager.h"
#include "sixseven/storage/wal.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "test_wal_helpers.h"

using namespace sixseven;
using namespace sixseven::test;

// -- Simple mock connection for manager tests ---------------------------------

namespace {

class SimpleReplicationConnection : public ReplicationConnection {
public:
    explicit SimpleReplicationConnection(std::string peer = "test-replica")
        : peer_(std::move(peer)) {}

    Result<void> send(std::span<const uint8_t> data) override {
        if (!open_.load()) {
            return make_error(StatusCode::NETWORK_ERROR, "closed");
        }
        std::lock_guard lock(mu_);
        sent_.insert(sent_.end(), data.begin(), data.end());
        cv_.notify_all();
        return ok();
    }

    Result<std::vector<uint8_t>> receive(size_t /*max_bytes*/,
                                         std::chrono::milliseconds /*timeout*/) override {
        if (!open_.load()) {
            return make_error(StatusCode::NETWORK_ERROR, "closed");
        }
        return ok(std::vector<uint8_t>{});
    }

    void close() override { open_.store(false); }
    bool is_open() const override { return open_.load(); }
    std::string peer_description() const override { return peer_; }

    bool wait_for_sent(size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mu_);
        return cv_.wait_for(lock, timeout, [&] { return sent_.size() >= n; });
    }

private:
    std::string peer_;
    std::atomic<bool> open_{true};
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<uint8_t> sent_;
};

} // namespace

// -- Tests --------------------------------------------------------------------

TEST(WalSenderManager, AcceptConnection) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(100);
    opts.sender_timeout = std::chrono::milliseconds(5000);

    WalSenderManager manager(dir.path(), nullptr, writer, 10, opts);

    auto conn = std::make_unique<SimpleReplicationConnection>("replica-1");
    auto result = manager.accept_connection(std::move(conn), 1);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(manager.active_sender_count(), 1u);

    manager.stop_all();
    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalSenderManager, MaxWalSendersEnforced) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(100);
    opts.sender_timeout = std::chrono::milliseconds(5000);

    uint32_t max_senders = 2;
    WalSenderManager manager(dir.path(), nullptr, writer, max_senders, opts);
    EXPECT_EQ(manager.max_wal_senders(), max_senders);

    // Accept up to the limit.
    for (uint32_t i = 0; i < max_senders; ++i) {
        auto conn = std::make_unique<SimpleReplicationConnection>("replica-" + std::to_string(i));
        ASSERT_TRUE(manager.accept_connection(std::move(conn), 1).has_value());
    }

    // The next one should be rejected.
    auto conn = std::make_unique<SimpleReplicationConnection>("replica-overflow");
    auto result = manager.accept_connection(std::move(conn), 1);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::REPLICATION_ERROR);

    manager.stop_all();
    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalSenderManager, BroadcastNotify) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(100);
    opts.sender_timeout = std::chrono::milliseconds(5000);

    WalSenderManager manager(dir.path(), nullptr, writer, 10, opts);

    // Accept two connections.
    auto conn1 = std::make_unique<SimpleReplicationConnection>("replica-1");
    auto* raw1 = conn1.get();
    ASSERT_TRUE(manager.accept_connection(std::move(conn1), 1).has_value());

    auto conn2 = std::make_unique<SimpleReplicationConnection>("replica-2");
    auto* raw2 = conn2.get();
    ASSERT_TRUE(manager.accept_connection(std::move(conn2), 1).has_value());

    // Write some data and broadcast.
    write_committed_txn(writer, 1, 1, "broadcast");
    ASSERT_TRUE(writer.flush().has_value());
    manager.notify_new_wal(writer.flushed_lsn());

    // Wait for both connections to have received data.
    raw1->wait_for_sent(replication_header_size, std::chrono::milliseconds(2000));
    raw2->wait_for_sent(replication_header_size, std::chrono::milliseconds(2000));

    // Both should have gotten data (catch-up messages + WAL data).
    EXPECT_EQ(manager.active_sender_count(), 2u);

    manager.stop_all();
    EXPECT_EQ(manager.active_sender_count(), 0u);
    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalSenderManager, CleanupStopped) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(50);
    opts.sender_timeout = std::chrono::milliseconds(5000);

    WalSenderManager manager(dir.path(), nullptr, writer, 10, opts);

    auto conn = std::make_unique<SimpleReplicationConnection>("replica-1");
    auto* raw = conn.get();
    ASSERT_TRUE(manager.accept_connection(std::move(conn), 1).has_value());

    // Wait for catch-up to start.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Force the connection closed so the sender stops.
    raw->close();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    manager.cleanup_stopped();
    EXPECT_EQ(manager.active_sender_count(), 0u);

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalSenderManager, StopAll) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(100);
    opts.sender_timeout = std::chrono::milliseconds(5000);

    WalSenderManager manager(dir.path(), nullptr, writer, 10, opts);

    for (int i = 0; i < 3; ++i) {
        auto conn = std::make_unique<SimpleReplicationConnection>("replica-" + std::to_string(i));
        ASSERT_TRUE(manager.accept_connection(std::move(conn), 1).has_value());
    }

    EXPECT_EQ(manager.active_sender_count(), 3u);

    manager.stop_all();
    EXPECT_EQ(manager.active_sender_count(), 0u);

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalSenderManager, SenderStatuses) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(100);
    opts.sender_timeout = std::chrono::milliseconds(5000);

    WalSenderManager manager(dir.path(), nullptr, writer, 10, opts);

    auto conn = std::make_unique<SimpleReplicationConnection>("replica-1");
    ASSERT_TRUE(manager.accept_connection(std::move(conn), 1).has_value());

    auto statuses = manager.get_sender_statuses();
    ASSERT_EQ(statuses.size(), 1u);
    EXPECT_EQ(statuses[0].peer, "replica-1");

    manager.stop_all();
    ASSERT_TRUE(writer.close().has_value());
}
