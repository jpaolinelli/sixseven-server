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

/// Shared buffer that outlives the connection object so callers can drain
/// bytes even after the WalSender has destroyed its ReplicationConnection.
struct SentBuffer {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<uint8_t> data;
};

class SimpleReplicationConnection : public ReplicationConnection {
public:
    explicit SimpleReplicationConnection(std::string peer = "test-replica")
        : peer_(std::move(peer)), buf_(std::make_shared<SentBuffer>()) {}

    Result<void> send(std::span<const uint8_t> data) override {
        if (!open_.load()) {
            return make_error(StatusCode::NETWORK_ERROR, "closed");
        }
        std::lock_guard lock(buf_->mu);
        buf_->data.insert(buf_->data.end(), data.begin(), data.end());
        buf_->cv.notify_all();
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
        std::unique_lock lock(buf_->mu);
        return buf_->cv.wait_for(lock, timeout, [&] { return buf_->data.size() >= n; });
    }

    /// Drain all bytes that were sent via this connection.  Safe to call after
    /// the connection has been destroyed by the WalSender because the buffer is
    /// reference-counted independently.
    std::vector<uint8_t> drain_sent() {
        std::lock_guard lock(buf_->mu);
        std::vector<uint8_t> result;
        result.swap(buf_->data);
        return result;
    }

    /// Return a handle to the shared buffer so callers can hold it past the
    /// lifetime of this object.
    std::shared_ptr<SentBuffer> sent_buffer() const { return buf_; }

private:
    std::string peer_;
    std::atomic<bool> open_{true};
    std::shared_ptr<SentBuffer> buf_;
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

// Helper: parse all message types from a raw byte buffer.
static std::vector<ReplicationMessageType>
parse_replication_message_types(const std::vector<uint8_t>& bytes) {
    std::vector<ReplicationMessageType> types;
    size_t offset = 0;
    while (offset < bytes.size()) {
        std::span<const uint8_t> remaining(bytes.data() + offset, bytes.size() - offset);
        if (remaining.size() < replication_header_size) {
            break;
        }
        auto type = peek_message_type(remaining);
        if (!type) {
            break;
        }
        auto sz = message_total_size(remaining);
        if (!sz || remaining.size() < *sz) {
            break;
        }
        types.push_back(*type);
        offset += *sz;
    }
    return types;
}

TEST(WalSenderManager, BroadcastNotify) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(100);
    opts.sender_timeout = std::chrono::milliseconds(5000);

    WalSenderManager manager(dir.path(), nullptr, writer, 10, opts);

    // Accept two connections.  Retain the shared send-buffers so we can inspect
    // bytes after the WalSender has taken ownership of (and eventually destroyed)
    // the connection objects.
    auto conn1 = std::make_unique<SimpleReplicationConnection>("replica-1");
    auto buf1 = conn1->sent_buffer();
    ASSERT_TRUE(manager.accept_connection(std::move(conn1), 1).has_value());

    auto conn2 = std::make_unique<SimpleReplicationConnection>("replica-2");
    auto buf2 = conn2->sent_buffer();
    ASSERT_TRUE(manager.accept_connection(std::move(conn2), 1).has_value());

    // Helpers operating directly on the shared buffers (safe after stop_all
    // because the buffers are ref-counted independently of the connection).
    auto wait_buf = [](const std::shared_ptr<SentBuffer>& buf,
                       size_t min_bytes,
                       std::chrono::milliseconds timeout) -> bool {
        std::unique_lock lock(buf->mu);
        return buf->cv.wait_for(lock, timeout, [&] { return buf->data.size() >= min_bytes; });
    };
    auto drain_buf = [](const std::shared_ptr<SentBuffer>& buf) -> std::vector<uint8_t> {
        std::lock_guard lock(buf->mu);
        std::vector<uint8_t> out;
        out.swap(buf->data);
        return out;
    };

    // Phase 1: wait for both senders to complete catch-up.
    // CatchupComplete is header(5) + lsn(8) + crc(4) = 17 bytes.
    static constexpr size_t catchup_complete_size = 17;
    ASSERT_TRUE(wait_buf(buf1, catchup_complete_size, std::chrono::milliseconds(2000)))
        << "replica-1 never completed catch-up";
    ASSERT_TRUE(wait_buf(buf2, catchup_complete_size, std::chrono::milliseconds(2000)))
        << "replica-2 never completed catch-up";

    // Drain catch-up traffic so the buffer is empty before the broadcast.
    drain_buf(buf1);
    drain_buf(buf2);

    // Phase 2: write new WAL records and broadcast.
    write_committed_txn(writer, 1, 1, "broadcast");
    ASSERT_TRUE(writer.flush().has_value());
    manager.notify_new_wal(writer.flushed_lsn());

    // Phase 3: assert that BOTH senders deliver new bytes within the timeout.
    // If notify_new_wal were a no-op, the senders would be idle (only periodic
    // keepalives every 100 ms); the predicate wait_for returns false after 2 s
    // and the ASSERT_TRUE below FAILS — catching the broken broadcast.
    ASSERT_TRUE(wait_buf(buf1, replication_header_size, std::chrono::milliseconds(2000)))
        << "replica-1 received no data after notify_new_wal — broadcast may be broken";
    ASSERT_TRUE(wait_buf(buf2, replication_header_size, std::chrono::milliseconds(2000)))
        << "replica-2 received no data after notify_new_wal — broadcast may be broken";

    // Both senders must still be alive.
    EXPECT_EQ(manager.active_sender_count(), 2u);

    // Drain real-time bytes before stopping so we capture them from the
    // shared buffer (which outlives the connection objects destroyed by stop_all).
    auto sent1 = drain_buf(buf1);
    auto sent2 = drain_buf(buf2);

    manager.stop_all();
    EXPECT_EQ(manager.active_sender_count(), 0u);

    // Phase 4: parse message types and assert at least one WAL_DATA was
    // delivered to each replica.  Only a working notify_new_wal fan-out can
    // produce WAL_DATA on a freshly-drained buffer; a no-op broadcast delivers
    // only keepalives at best.
    auto check_wal_data = [](const std::vector<uint8_t>& bytes, const char* replica) {
        auto types = parse_replication_message_types(bytes);
        bool has_wal_data = false;
        for (auto t : types) {
            if (t == ReplicationMessageType::WAL_DATA) {
                has_wal_data = true;
            }
        }
        EXPECT_TRUE(has_wal_data) << replica
                                  << " did not receive a WAL_DATA message after"
                                     " notify_new_wal — broadcast fan-out may be broken";
    };

    check_wal_data(sent1, "replica-1");
    check_wal_data(sent2, "replica-2");

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
