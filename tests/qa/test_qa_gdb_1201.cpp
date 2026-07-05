// QA regression tests for GDB-1201.
//
// GDB-1201 removed a dangling raw-pointer fixture member (`primary_conn_`) from
// WalReceiverTest in tests/unit/test_wal_receiver.cpp. The member was assigned
// from a unique_ptr::get() call inside StartAndStop's connection factory, but
// the owning unique_ptr (the local `primary_conn`) was destroyed when the
// factory lambda returned -- leaving `primary_conn_` dangling immediately.
// The member was never read anywhere, so it was pure dead code plus a
// use-after-free trap for the next person who tried to use it.
//
// This is a test-only change (no production code touched). QA here is narrow:
// confirm the start/stop path that StartAndStop exercises is not regressed by
// the removal, and adversarially stress start/stop sequencing (repeat calls,
// stop-before-connect, destruction mid-flight) since that is the behavior the
// removed member's comment ("keep primary_conn alive... but we don't need it")
// touched.

#include "sixseven/server/replication_message.h"
#include "sixseven/server/wal_receiver.h"
#include "sixseven/storage/wal_record.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace sixseven;

namespace {

// Minimal self-contained in-memory ReplicationConnection, duplicated here
// (rather than reused from tests/unit) to keep QA tests independent of dev
// test internals per project convention (QA tests live in tests/qa/ and must
// not modify or depend on tests/unit fixtures).
struct QaSharedBuffer {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<uint8_t> data;
    bool closed = false;
};

class QaInMemoryConnection : public ReplicationConnection {
public:
    QaInMemoryConnection(std::shared_ptr<QaSharedBuffer> send_buf,
                          std::shared_ptr<QaSharedBuffer> recv_buf, std::string peer)
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

private:
    std::shared_ptr<QaSharedBuffer> send_buf_;
    std::shared_ptr<QaSharedBuffer> recv_buf_;
    std::string peer_;
    std::atomic<bool> open_{true};
};

std::pair<std::unique_ptr<QaInMemoryConnection>, std::unique_ptr<QaInMemoryConnection>>
qa_create_connection_pair() {
    auto buf_a = std::make_shared<QaSharedBuffer>();
    auto buf_b = std::make_shared<QaSharedBuffer>();
    auto conn_a = std::make_unique<QaInMemoryConnection>(buf_a, buf_b, "standby");
    auto conn_b = std::make_unique<QaInMemoryConnection>(buf_b, buf_a, "primary");
    return {std::move(conn_a), std::move(conn_b)};
}

class QaMockRecoveryHandler : public RecoveryHandler {
public:
    Result<void> redo(const WalRecord& /*record*/) override { return ok(); }
    Result<void> undo(const WalRecord& /*record*/) override { return ok(); }
};

class QaTempWalDir {
public:
    QaTempWalDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("qa_gdb1201_wal_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~QaTempWalDir() { std::filesystem::remove_all(path_); }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class QaGdb1201WalReceiverTest : public ::testing::Test {
protected:
    void TearDown() override {
        if (receiver_) {
            receiver_->stop();
            receiver_.reset();
        }
    }

    std::unique_ptr<WalReceiver> receiver_;
};

// Reproduces the exact StartAndStop scenario from tests/unit/test_wal_receiver.cpp:
// the factory drops the primary-side connection immediately (no member holds a
// dangling pointer to it). Confirms start/stop still functions correctly.
TEST_F(QaGdb1201WalReceiverTest, StartAndStopWithDroppedPrimarySide) {
    QaTempWalDir wal_dir;
    QaMockRecoveryHandler handler;

    auto factory = [](const std::string& /*host*/,
                      uint16_t /*port*/) -> Result<std::unique_ptr<ReplicationConnection>> {
        auto [standby_conn, primary_conn] = qa_create_connection_pair();
        // primary_conn is intentionally dropped here (no dangling pointer kept).
        return ok(std::unique_ptr<ReplicationConnection>(std::move(standby_conn)));
    };

    WalReceiverOptions opts;
    opts.receive_timeout = std::chrono::milliseconds(100);
    receiver_ = std::make_unique<WalReceiver>(factory, wal_dir.path(), handler, opts);

    auto result = receiver_->start("localhost", 6767);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto state = receiver_->get_state();
    EXPECT_TRUE(state.is_streaming);

    receiver_->stop();
    state = receiver_->get_state();
    EXPECT_FALSE(state.is_streaming);
}

// Adversarial: repeated start/stop cycles on the same receiver instance must
// not crash or leave state inconsistent -- this is the sequencing area the
// removed dangling-pointer member's comment ("we don't need it") touched.
TEST_F(QaGdb1201WalReceiverTest, RepeatedStartStopCyclesDoNotCrash) {
    QaTempWalDir wal_dir;
    QaMockRecoveryHandler handler;

    auto factory = [](const std::string& /*host*/,
                      uint16_t /*port*/) -> Result<std::unique_ptr<ReplicationConnection>> {
        auto [standby_conn, primary_conn] = qa_create_connection_pair();
        return ok(std::unique_ptr<ReplicationConnection>(std::move(standby_conn)));
    };

    WalReceiverOptions opts;
    opts.receive_timeout = std::chrono::milliseconds(50);
    receiver_ = std::make_unique<WalReceiver>(factory, wal_dir.path(), handler, opts);

    for (int i = 0; i < 3; ++i) {
        auto result = receiver_->start("localhost", 6767);
        ASSERT_TRUE(result.has_value()) << "iteration " << i << ": " << result.error().message;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        receiver_->stop();
        auto state = receiver_->get_state();
        EXPECT_FALSE(state.is_streaming) << "iteration " << i;
    }
}

// Adversarial: calling stop() on a receiver that was never started must not
// crash (defensive against future refactors of the started/streaming state).
TEST_F(QaGdb1201WalReceiverTest, StopWithoutStartIsSafe) {
    QaTempWalDir wal_dir;
    QaMockRecoveryHandler handler;

    auto factory = [](const std::string& /*host*/,
                      uint16_t /*port*/) -> Result<std::unique_ptr<ReplicationConnection>> {
        auto [standby_conn, primary_conn] = qa_create_connection_pair();
        return ok(std::unique_ptr<ReplicationConnection>(std::move(standby_conn)));
    };

    WalReceiverOptions opts;
    receiver_ = std::make_unique<WalReceiver>(factory, wal_dir.path(), handler, opts);

    // Should not crash or hang.
    receiver_->stop();
    auto state = receiver_->get_state();
    EXPECT_FALSE(state.is_streaming);
}

} // namespace
