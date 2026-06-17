/// QA adversarial tests for GDB-852 — WAL broadcast fan-out hardening.
///
/// Verifies that WalSenderManager::notify_new_wal delivers WAL_DATA to every
/// connected replica under adversarial conditions:
///   - N > 2 replicas (fan-out covers all, not just first)
///   - Multiple sequential broadcasts with advancing LSN
///   - Late-joining replica receives subsequent broadcasts
///   - Zero replicas — safe no-op
///   - Varying WAL record sizes delivered intact
///   - stop_all mid-stream does not crash and halts delivery
///   - Every replica receives WAL_DATA (not only keepalives)

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
#include <string>
#include <thread>
#include <vector>

#include "test_wal_helpers.h"

using namespace sixseven;
using namespace sixseven::test;

// ---------------------------------------------------------------------------
// Shared send-buffer (ref-counted so it outlives the connection object).
// All accesses are protected by mu; cv signals on every send().
// ---------------------------------------------------------------------------

namespace {

struct SharedBuf {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<uint8_t> data;

    bool wait_min(size_t min_bytes, std::chrono::milliseconds timeout) {
        std::unique_lock lk(mu);
        return cv.wait_for(lk, timeout, [&] { return data.size() >= min_bytes; });
    }

    std::vector<uint8_t> drain() {
        std::lock_guard lk(mu);
        std::vector<uint8_t> out;
        out.swap(data);
        return out;
    }
};

// ---------------------------------------------------------------------------
// Controllable mock connection that feeds into a SharedBuf.
// ---------------------------------------------------------------------------

class MockConn : public ReplicationConnection {
public:
    explicit MockConn(std::string peer = "test")
        : peer_(std::move(peer)), buf_(std::make_shared<SharedBuf>()) {}

    Result<void> send(std::span<const uint8_t> data) override {
        if (!open_.load(std::memory_order_acquire)) {
            return make_error(StatusCode::NETWORK_ERROR, "closed");
        }
        std::lock_guard lk(buf_->mu);
        buf_->data.insert(buf_->data.end(), data.begin(), data.end());
        buf_->cv.notify_all();
        return ok();
    }

    Result<std::vector<uint8_t>> receive(size_t, std::chrono::milliseconds) override {
        if (!open_.load(std::memory_order_acquire)) {
            return make_error(StatusCode::NETWORK_ERROR, "closed");
        }
        return ok(std::vector<uint8_t>{});
    }

    void close() override { open_.store(false, std::memory_order_release); }
    bool is_open() const override { return open_.load(std::memory_order_acquire); }
    std::string peer_description() const override { return peer_; }

    std::shared_ptr<SharedBuf> buf() const { return buf_; }

private:
    std::string peer_;
    std::atomic<bool> open_{true};
    std::shared_ptr<SharedBuf> buf_;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static constexpr std::chrono::milliseconds kCatchupTimeout{3000};
static constexpr std::chrono::milliseconds kBroadcastTimeout{3000};
// CatchupComplete = header(5) + lsn(8) + crc(4) = 17 bytes.
static constexpr size_t kCatchupCompleteSize = 17;

static WalSenderOptions fast_opts() {
    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(200);
    opts.sender_timeout = std::chrono::milliseconds(10000);
    return opts;
}

/// Parse all message types from a raw byte stream.
static std::vector<ReplicationMessageType> parse_types(const std::vector<uint8_t>& bytes) {
    std::vector<ReplicationMessageType> types;
    size_t offset = 0;
    while (offset < bytes.size()) {
        std::span<const uint8_t> rem(bytes.data() + offset, bytes.size() - offset);
        if (rem.size() < replication_header_size) {
            break;
        }
        auto t = peek_message_type(rem);
        if (!t) {
            break;
        }
        auto sz = message_total_size(rem);
        if (!sz || rem.size() < *sz) {
            break;
        }
        types.push_back(*t);
        offset += *sz;
    }
    return types;
}

static bool has_wal_data(const std::vector<uint8_t>& bytes) {
    for (auto t : parse_types(bytes)) {
        if (t == ReplicationMessageType::WAL_DATA) {
            return true;
        }
    }
    return false;
}

/// Accept a connection and wait for its catchup-complete message.
/// Returns the shared buffer (catchup bytes are drained before return).
static std::shared_ptr<SharedBuf> accept_and_catchup(WalSenderManager& mgr,
                                                     const std::string& peer) {
    auto conn = std::make_unique<MockConn>(peer);
    auto buf = conn->buf();
    EXPECT_TRUE(mgr.accept_connection(std::move(conn), 1).has_value());
    bool caught_up = buf->wait_min(kCatchupCompleteSize, kCatchupTimeout);
    EXPECT_TRUE(caught_up) << peer << " never completed catch-up";
    buf->drain(); // discard catch-up traffic
    return buf;
}

} // namespace

// ===========================================================================
// GDB852: N > 2 replicas — fan-out delivers WAL_DATA to ALL
// ===========================================================================

TEST(QA_GDB852, FourReplicasAllReceiveWalData) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderManager manager(dir.path(), nullptr, writer, 10, fast_opts());

    constexpr int kN = 4;
    std::vector<std::shared_ptr<SharedBuf>> bufs;
    bufs.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        bufs.push_back(accept_and_catchup(manager, "replica-" + std::to_string(i)));
    }
    ASSERT_EQ(manager.active_sender_count(), static_cast<size_t>(kN));

    // Broadcast one batch.
    write_committed_txn(writer, 100, 1, "fanout-4");
    ASSERT_TRUE(writer.flush().has_value());
    manager.notify_new_wal(writer.flushed_lsn());

    // Every replica must receive at least the WAL_DATA header.
    for (int i = 0; i < kN; ++i) {
        bool got = bufs[i]->wait_min(replication_header_size, kBroadcastTimeout);
        ASSERT_TRUE(got) << "replica-" << i << " received nothing after notify_new_wal";
        auto sent = bufs[i]->drain();
        EXPECT_TRUE(has_wal_data(sent)) << "replica-" << i << " received no WAL_DATA message";
    }

    manager.stop_all();
    EXPECT_EQ(manager.active_sender_count(), 0u);
    ASSERT_TRUE(writer.close().has_value());
}

// ===========================================================================
// GDB852: 3 replicas (boundary between 2 and 4)
// ===========================================================================

TEST(QA_GDB852, ThreeReplicasAllReceiveWalData) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderManager manager(dir.path(), nullptr, writer, 10, fast_opts());

    constexpr int kN = 3;
    std::vector<std::shared_ptr<SharedBuf>> bufs;
    bufs.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        bufs.push_back(accept_and_catchup(manager, "r-" + std::to_string(i)));
    }

    write_committed_txn(writer, 200, 2, "three-replicas");
    ASSERT_TRUE(writer.flush().has_value());
    manager.notify_new_wal(writer.flushed_lsn());

    for (int i = 0; i < kN; ++i) {
        ASSERT_TRUE(bufs[i]->wait_min(replication_header_size, kBroadcastTimeout))
            << "replica " << i << " missed the broadcast";
        auto sent = bufs[i]->drain();
        EXPECT_TRUE(has_wal_data(sent)) << "replica " << i << " has no WAL_DATA";
    }

    manager.stop_all();
    ASSERT_TRUE(writer.close().has_value());
}

// ===========================================================================
// GDB852: Multiple sequential broadcasts — each delivers NEW records,
//         LSN advances, no duplicate / lost WAL_DATA.
// ===========================================================================

TEST(QA_GDB852, SequentialBroadcastsAdvancingLsn) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderManager manager(dir.path(), nullptr, writer, 10, fast_opts());

    auto buf1 = accept_and_catchup(manager, "seq-1");
    auto buf2 = accept_and_catchup(manager, "seq-2");

    // Three rounds of write + notify.
    for (int round = 0; round < 3; ++round) {
        write_committed_txn(
            writer, static_cast<txn_id_t>(300 + round), 1, "round-" + std::to_string(round));
        ASSERT_TRUE(writer.flush().has_value());
        lsn_t lsn_before = writer.flushed_lsn();
        manager.notify_new_wal(lsn_before);

        // Both replicas must receive new bytes for this round.
        ASSERT_TRUE(buf1->wait_min(replication_header_size, kBroadcastTimeout))
            << "seq-1 missed round " << round;
        ASSERT_TRUE(buf2->wait_min(replication_header_size, kBroadcastTimeout))
            << "seq-2 missed round " << round;

        auto sent1 = buf1->drain();
        auto sent2 = buf2->drain();

        EXPECT_TRUE(has_wal_data(sent1)) << "seq-1 no WAL_DATA in round " << round;
        EXPECT_TRUE(has_wal_data(sent2)) << "seq-2 no WAL_DATA in round " << round;

        // Deserialize the WAL_DATA and verify end_lsn >= lsn_before.
        auto types1 = parse_types(sent1);
        bool found_wal = false;
        size_t offset = 0;
        while (offset < sent1.size()) {
            std::span<const uint8_t> rem(sent1.data() + offset, sent1.size() - offset);
            if (rem.size() < replication_header_size) {
                break;
            }
            auto t = peek_message_type(rem);
            auto sz = message_total_size(rem);
            if (!t || !sz || rem.size() < *sz) {
                break;
            }
            if (*t == ReplicationMessageType::WAL_DATA) {
                auto msg = deserialize_wal_data(rem.subspan(0, *sz));
                ASSERT_TRUE(msg.has_value()) << "failed to deserialize WAL_DATA round " << round;
                EXPECT_GE(msg->end_lsn, lsn_before) << "end_lsn did not advance in round " << round;
                found_wal = true;
                break;
            }
            offset += *sz;
        }
        EXPECT_TRUE(found_wal) << "seq-1 no WAL_DATA parseable in round " << round;
    }

    manager.stop_all();
    ASSERT_TRUE(writer.close().has_value());
}

// ===========================================================================
// GDB852: Late-joining replica receives subsequent broadcasts.
// ===========================================================================

TEST(QA_GDB852, LateJoiningReplicaReceivesSubsequentBroadcasts) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderManager manager(dir.path(), nullptr, writer, 10, fast_opts());

    // First broadcast (early replica only).
    auto buf_early = accept_and_catchup(manager, "early");
    write_committed_txn(writer, 400, 1, "pre-join");
    ASSERT_TRUE(writer.flush().has_value());
    manager.notify_new_wal(writer.flushed_lsn());

    // Drain the early replica's first broadcast; we don't care about it here.
    ASSERT_TRUE(buf_early->wait_min(replication_header_size, kBroadcastTimeout));
    buf_early->drain();

    // Late replica joins now.
    auto buf_late = accept_and_catchup(manager, "late");

    // Second broadcast — both should receive it.
    write_committed_txn(writer, 401, 1, "post-join");
    ASSERT_TRUE(writer.flush().has_value());
    manager.notify_new_wal(writer.flushed_lsn());

    ASSERT_TRUE(buf_early->wait_min(replication_header_size, kBroadcastTimeout))
        << "early replica missed second broadcast";
    ASSERT_TRUE(buf_late->wait_min(replication_header_size, kBroadcastTimeout))
        << "late replica missed broadcast after joining";

    EXPECT_TRUE(has_wal_data(buf_early->drain())) << "early replica: no WAL_DATA";
    EXPECT_TRUE(has_wal_data(buf_late->drain())) << "late replica: no WAL_DATA";

    manager.stop_all();
    ASSERT_TRUE(writer.close().has_value());
}

// ===========================================================================
// GDB852: Zero replicas — notify_new_wal is a safe no-op (no crash).
// ===========================================================================

TEST(QA_GDB852, ZeroReplicasNoopNoCrash) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderManager manager(dir.path(), nullptr, writer, 10, fast_opts());
    EXPECT_EQ(manager.active_sender_count(), 0u);

    // Multiple calls with no senders must not crash.
    write_committed_txn(writer, 500, 1, "noop");
    ASSERT_TRUE(writer.flush().has_value());
    manager.notify_new_wal(writer.flushed_lsn());
    manager.notify_new_wal(writer.flushed_lsn());
    manager.notify_new_wal(0);

    EXPECT_EQ(manager.active_sender_count(), 0u);
    ASSERT_TRUE(writer.close().has_value());
}

// ===========================================================================
// GDB852: Small WAL record — delivered intact (byte-count check).
// ===========================================================================

TEST(QA_GDB852, SmallWalRecordDeliveredIntact) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderManager manager(dir.path(), nullptr, writer, 10, fast_opts());
    auto buf = accept_and_catchup(manager, "small-rx");

    const std::string payload = "x"; // 1-byte data field
    write_committed_txn(writer, 600, 1, payload);
    ASSERT_TRUE(writer.flush().has_value());
    manager.notify_new_wal(writer.flushed_lsn());

    ASSERT_TRUE(buf->wait_min(replication_header_size, kBroadcastTimeout))
        << "small record not received";
    auto sent = buf->drain();

    // Must have at least one WAL_DATA message.
    EXPECT_TRUE(has_wal_data(sent)) << "no WAL_DATA for small record";

    // Every parseable WAL_DATA must have record_count > 0 and non-empty data.
    size_t offset = 0;
    bool found = false;
    while (offset < sent.size()) {
        std::span<const uint8_t> rem(sent.data() + offset, sent.size() - offset);
        if (rem.size() < replication_header_size) {
            break;
        }
        auto t = peek_message_type(rem);
        auto sz = message_total_size(rem);
        if (!t || !sz || rem.size() < *sz) {
            break;
        }
        if (*t == ReplicationMessageType::WAL_DATA) {
            auto msg = deserialize_wal_data(rem.subspan(0, *sz));
            ASSERT_TRUE(msg.has_value());
            EXPECT_GT(msg->record_count, 0u) << "WAL_DATA has zero records";
            EXPECT_FALSE(msg->data.empty()) << "WAL_DATA has empty data payload";
            found = true;
            break;
        }
        offset += *sz;
    }
    EXPECT_TRUE(found) << "no WAL_DATA parseable";

    manager.stop_all();
    ASSERT_TRUE(writer.close().has_value());
}

// ===========================================================================
// GDB852: Large WAL record (>1 KB payload) — delivered intact.
// ===========================================================================

TEST(QA_GDB852, LargeWalRecordDeliveredIntact) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderManager manager(dir.path(), nullptr, writer, 10, fast_opts());
    auto buf = accept_and_catchup(manager, "large-rx");

    const std::string large_payload(2048, 'Z');
    write_committed_txn(writer, 700, 1, large_payload);
    ASSERT_TRUE(writer.flush().has_value());
    manager.notify_new_wal(writer.flushed_lsn());

    ASSERT_TRUE(buf->wait_min(replication_header_size, kBroadcastTimeout))
        << "large record not received";

    // Give sender a bit more time to deliver the full large batch.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto sent = buf->drain();

    EXPECT_TRUE(has_wal_data(sent)) << "no WAL_DATA for large record";

    size_t offset = 0;
    bool found = false;
    while (offset < sent.size()) {
        std::span<const uint8_t> rem(sent.data() + offset, sent.size() - offset);
        if (rem.size() < replication_header_size) {
            break;
        }
        auto t = peek_message_type(rem);
        auto sz = message_total_size(rem);
        if (!t || !sz || rem.size() < *sz) {
            break;
        }
        if (*t == ReplicationMessageType::WAL_DATA) {
            auto msg = deserialize_wal_data(rem.subspan(0, *sz));
            ASSERT_TRUE(msg.has_value());
            EXPECT_GT(msg->record_count, 0u);
            // The data payload must contain the large record — at minimum it
            // must be non-trivially sized.
            EXPECT_GT(msg->data.size(), 100u)
                << "WAL_DATA payload suspiciously small for a 2 KB record";
            found = true;
            break;
        }
        offset += *sz;
    }
    EXPECT_TRUE(found) << "no WAL_DATA parseable for large record";

    manager.stop_all();
    ASSERT_TRUE(writer.close().has_value());
}

// ===========================================================================
// GDB852: stop_all mid-stream — after stop, notify_new_wal does not crash
//         and delivers nothing to the (now gone) senders.
// ===========================================================================

TEST(QA_GDB852, StopAllMidStreamNoCrashNoDelivery) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderManager manager(dir.path(), nullptr, writer, 10, fast_opts());

    auto buf1 = accept_and_catchup(manager, "stop-1");
    auto buf2 = accept_and_catchup(manager, "stop-2");
    EXPECT_EQ(manager.active_sender_count(), 2u);

    // Trigger one normal broadcast first to confirm both are alive.
    write_committed_txn(writer, 800, 1, "pre-stop");
    ASSERT_TRUE(writer.flush().has_value());
    manager.notify_new_wal(writer.flushed_lsn());
    ASSERT_TRUE(buf1->wait_min(replication_header_size, kBroadcastTimeout));
    ASSERT_TRUE(buf2->wait_min(replication_header_size, kBroadcastTimeout));
    buf1->drain();
    buf2->drain();

    // Stop all senders.
    manager.stop_all();
    EXPECT_EQ(manager.active_sender_count(), 0u);

    // Another write + notify after stop must not crash.
    write_committed_txn(writer, 801, 1, "post-stop");
    ASSERT_TRUE(writer.flush().has_value());
    manager.notify_new_wal(writer.flushed_lsn());

    // Wait briefly — no new bytes should arrive to the old buffers.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto post1 = buf1->drain();
    auto post2 = buf2->drain();

    // Nothing new should have arrived after stop_all.
    EXPECT_TRUE(post1.empty()) << "stop-1 received " << post1.size() << " bytes after stop_all";
    EXPECT_TRUE(post2.empty()) << "stop-2 received " << post2.size() << " bytes after stop_all";

    ASSERT_TRUE(writer.close().has_value());
}

// ===========================================================================
// GDB852: Mutation guard — fan-out that skips the last sender must FAIL.
//         We verify this by checking that ALL 4 replicas each individually
//         received WAL_DATA; if any is absent the test fails explicitly.
// ===========================================================================

TEST(QA_GDB852, MutationGuardAllFourIndividuallyVerified) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderManager manager(dir.path(), nullptr, writer, 10, fast_opts());

    constexpr int kN = 4;
    std::vector<std::shared_ptr<SharedBuf>> bufs;
    bufs.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        bufs.push_back(accept_and_catchup(manager, "mg-" + std::to_string(i)));
    }

    write_committed_txn(writer, 900, 1, "mutation-guard");
    ASSERT_TRUE(writer.flush().has_value());
    manager.notify_new_wal(writer.flushed_lsn());

    for (int i = 0; i < kN; ++i) {
        ASSERT_TRUE(bufs[i]->wait_min(replication_header_size, kBroadcastTimeout))
            << "mg-" << i << " received zero bytes — fan-out skipped this sender";
        auto sent = bufs[i]->drain();
        EXPECT_TRUE(has_wal_data(sent))
            << "mg-" << i << " received bytes but NO WAL_DATA — keepalive only is not acceptable";
    }

    manager.stop_all();
    ASSERT_TRUE(writer.close().has_value());
}

// ===========================================================================
// GDB852: Keepalive-only guard — a broadcast that emits only keepalives must
//         fail this test.  We parse each replica's stream and assert at least
//         one WAL_DATA is present and zero-WAL_DATA == test failure.
// ===========================================================================

TEST(QA_GDB852, WalDataNotJustKeepaliveForEveryReplica) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderManager manager(dir.path(), nullptr, writer, 10, fast_opts());

    auto buf_a = accept_and_catchup(manager, "kp-a");
    auto buf_b = accept_and_catchup(manager, "kp-b");
    auto buf_c = accept_and_catchup(manager, "kp-c");

    write_committed_txn(writer, 1000, 1, "keepalive-guard");
    ASSERT_TRUE(writer.flush().has_value());
    manager.notify_new_wal(writer.flushed_lsn());

    auto check = [](std::shared_ptr<SharedBuf> buf, const char* name) {
        bool got = buf->wait_min(replication_header_size, kBroadcastTimeout);
        ASSERT_TRUE(got) << name << " received nothing";
        auto sent = buf->drain();
        auto types = parse_types(sent);
        int wal_count = 0;
        for (auto t : types) {
            if (t == ReplicationMessageType::WAL_DATA) {
                ++wal_count;
            }
        }
        EXPECT_GT(wal_count, 0) << name << " had " << types.size()
                                << " messages but zero WAL_DATA — keepalive-only delivery";
    };

    check(buf_a, "kp-a");
    check(buf_b, "kp-b");
    check(buf_c, "kp-c");

    manager.stop_all();
    ASSERT_TRUE(writer.close().has_value());
}

// ===========================================================================
// GDB852: Stress — 4 replicas, 5 sequential broadcasts each verified.
//         Ensures no delivery is lost across repeated fan-out rounds.
// ===========================================================================

TEST(QA_GDB852, StressFourReplicasFiveRounds) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderManager manager(dir.path(), nullptr, writer, 10, fast_opts());

    constexpr int kN = 4;
    constexpr int kRounds = 5;
    std::vector<std::shared_ptr<SharedBuf>> bufs;
    bufs.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        bufs.push_back(accept_and_catchup(manager, "stress-" + std::to_string(i)));
    }

    for (int r = 0; r < kRounds; ++r) {
        write_committed_txn(
            writer, static_cast<txn_id_t>(1100 + r), 1, "stress-round-" + std::to_string(r));
        ASSERT_TRUE(writer.flush().has_value());
        manager.notify_new_wal(writer.flushed_lsn());

        for (int i = 0; i < kN; ++i) {
            ASSERT_TRUE(bufs[i]->wait_min(replication_header_size, kBroadcastTimeout))
                << "replica " << i << " missed round " << r;
            auto sent = bufs[i]->drain();
            EXPECT_TRUE(has_wal_data(sent)) << "replica " << i << " no WAL_DATA in round " << r;
        }
    }

    manager.stop_all();
    ASSERT_TRUE(writer.close().has_value());
}
