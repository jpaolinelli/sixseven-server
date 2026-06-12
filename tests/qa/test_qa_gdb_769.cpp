// QA regression tests for GDB-769: WAL group commit must support waiting for
// durability of a specific LSN (flush_until), with prompt wakeup, shared
// fsyncs across concurrent committers, and safe shutdown semantics.

#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_record.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace sixseven {
namespace {

class QA_GDB769 : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb769";
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    static WalRecord commit_record(txn_id_t txn_id) {
        WalRecord r;
        r.type = WalRecordType::COMMIT;
        r.txn_id = txn_id;
        return r;
    }

    std::filesystem::path dir_;
};

// AC: flush_until on an LSN that has never been appended must fail fast with
// INVALID_ARGUMENT instead of blocking forever.
TEST_F(QA_GDB769, LsnBeyondAppendedFailsFast) {
    WalWriter writer(dir_);
    ASSERT_TRUE(writer.open().has_value());

    auto r = commit_record(1);
    auto lsn = writer.append(r);
    ASSERT_TRUE(lsn.has_value());

    auto start = std::chrono::steady_clock::now();
    auto next = writer.flush_until(writer.current_lsn());
    auto huge = writer.flush_until(UINT64_MAX);
    auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_FALSE(next.has_value());
    EXPECT_EQ(next.error().code, StatusCode::INVALID_ARGUMENT);
    ASSERT_FALSE(huge.has_value());
    EXPECT_EQ(huge.error().code, StatusCode::INVALID_ARGUMENT);
    // Must not have waited for any flush interval.
    EXPECT_LT(elapsed, std::chrono::seconds(5));

    ASSERT_TRUE(writer.close().has_value());
}

// AC: durability — after flush_until(lsn) returns ok, flushed_lsn() >= lsn
// even with an effectively-disabled periodic timer (the waiter must wake the
// flush thread itself).
TEST_F(QA_GDB769, DurableBeforeAckWithoutTimer) {
    WalWriterOptions opts;
    opts.flush_interval = std::chrono::seconds(30);
    WalWriter writer(dir_, opts);
    ASSERT_TRUE(writer.open().has_value());

    auto r = commit_record(7);
    auto lsn = writer.append(r);
    ASSERT_TRUE(lsn.has_value());
    ASSERT_LT(writer.flushed_lsn(), *lsn) << "record durable before any flush?";

    auto start = std::chrono::steady_clock::now();
    auto result = writer.flush_until(*lsn);
    auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_GE(writer.flushed_lsn(), *lsn);
    EXPECT_LT(elapsed, std::chrono::seconds(10)) << "waiter rode the 30s timer";

    ASSERT_TRUE(writer.close().has_value());
}

// Adversarial: many threads hammering append + flush_until concurrently while
// segments rotate. Every call must succeed and every acked LSN must be
// durable at ack time.
TEST_F(QA_GDB769, ConcurrentAppendFlushUntilHammer) {
    WalWriterOptions opts;
    opts.segment_size = 4096; // Force frequent rotation under load.
    opts.flush_interval = std::chrono::milliseconds(100);
    WalWriter writer(dir_, opts);
    ASSERT_TRUE(writer.open().has_value());

    constexpr int num_threads = 8;
    constexpr int iterations = 50;
    std::atomic<int> errors{0};
    std::atomic<int> durability_violations{0};

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&writer, &errors, &durability_violations, t] {
            for (int i = 0; i < iterations; ++i) {
                WalRecord r;
                r.type = WalRecordType::INSERT;
                r.txn_id = static_cast<txn_id_t>(t + 1);
                r.table_id = 42;
                std::string payload =
                    "qa-769-hammer-" + std::to_string(t) + "-" + std::to_string(i);
                r.data.assign(payload.begin(), payload.end());

                auto lsn = writer.append(r);
                if (!lsn.has_value()) {
                    ++errors;
                    return;
                }
                auto flushed = writer.flush_until(*lsn);
                if (!flushed.has_value()) {
                    ++errors;
                    return;
                }
                if (writer.flushed_lsn() < *lsn) {
                    ++durability_violations;
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(errors.load(), 0);
    EXPECT_EQ(durability_violations.load(), 0);
    EXPECT_EQ(writer.current_lsn(), static_cast<lsn_t>(num_threads * iterations + 1));

    ASSERT_TRUE(writer.close().has_value());

    // The records must actually be readable back (durable on disk).
    WalReader reader(dir_);
    ASSERT_TRUE(reader.open().has_value());
    int count = 0;
    while (reader.next().has_value()) {
        ++count;
    }
    EXPECT_EQ(count, num_threads * iterations);
    ASSERT_TRUE(reader.close().has_value());
}

// Adversarial: closing the writer while a waiter is blocked must not hang and
// must leave the waiter with either durability (ok) or an IO_ERROR.
TEST_F(QA_GDB769, CloseWhileWaiterBlockedDoesNotHang) {
    WalWriterOptions opts;
    opts.flush_interval = std::chrono::seconds(30);
    auto writer = std::make_unique<WalWriter>(dir_, opts);
    ASSERT_TRUE(writer->open().has_value());

    auto r = commit_record(1);
    auto lsn = writer->append(r);
    ASSERT_TRUE(lsn.has_value());

    std::atomic<bool> returned{false};
    std::thread waiter([&writer, &returned, target = *lsn] {
        auto result = writer->flush_until(target);
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
        }
        returned.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_TRUE(writer->close().has_value());
    waiter.join();
    EXPECT_TRUE(returned.load());
}

// flush_until on a closed writer must fail with INVALID_ARGUMENT, not hang.
TEST_F(QA_GDB769, FlushUntilAfterCloseFails) {
    WalWriter writer(dir_);
    ASSERT_TRUE(writer.open().has_value());
    auto r = commit_record(1);
    auto lsn = writer.append(r);
    ASSERT_TRUE(lsn.has_value());
    ASSERT_TRUE(writer.close().has_value());

    // LSN was flushed during close, so the fast path may legitimately return
    // ok. An LSN beyond anything appended must fail.
    auto result = writer.flush_until(*lsn + 100);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

} // namespace
} // namespace sixseven
