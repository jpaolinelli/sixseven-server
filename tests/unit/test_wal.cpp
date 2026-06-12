#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_record.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

#include "test_wal_helpers.h"

namespace sixseven {
namespace {

using test::TempWalDir;

/// Create a simple WAL record for testing.
WalRecord make_test_record(WalRecordType type,
                           txn_id_t txn_id = 1,
                           uint32_t table_id = 0,
                           const std::string& data = "") {
    WalRecord r;
    r.type = type;
    r.txn_id = txn_id;
    r.table_id = table_id;
    if (!data.empty()) {
        r.data.assign(data.begin(), data.end());
    }
    return r;
}

// -- Basic open/close ---------------------------------------------------------

TEST(WalWriter, OpenCreatesDirectoryAndSegment) {
    TempWalDir dir;
    // Remove the directory so open() must create it.
    std::filesystem::remove_all(dir.path());

    WalWriter writer(dir.path());
    auto result = writer.open();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // The directory should exist.
    EXPECT_TRUE(std::filesystem::exists(dir.path()));

    // Segment file wal_000001 should exist.
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "wal_000001"));

    auto close_result = writer.close();
    ASSERT_TRUE(close_result.has_value()) << close_result.error().message;
}

TEST(WalWriter, OpenTwiceFails) {
    TempWalDir dir;
    WalWriter writer(dir.path());
    ASSERT_TRUE(writer.open().has_value());

    auto result = writer.open();
    EXPECT_FALSE(result.has_value());

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalWriter, CloseIdempotent) {
    TempWalDir dir;
    WalWriter writer(dir.path());
    ASSERT_TRUE(writer.open().has_value());

    ASSERT_TRUE(writer.close().has_value());
    ASSERT_TRUE(writer.close().has_value()); // Second close is OK.
}

TEST(WalWriter, DestructorClosesWriter) {
    TempWalDir dir;
    {
        WalWriter writer(dir.path());
        ASSERT_TRUE(writer.open().has_value());
        // Destructor should handle close.
    }
    // If we get here without a crash, the destructor worked.
    SUCCEED();
}

// -- Append -------------------------------------------------------------------

TEST(WalWriter, AppendAssignsSequentialLsns) {
    TempWalDir dir;
    WalWriter writer(dir.path());
    ASSERT_TRUE(writer.open().has_value());

    // Initial LSN should be 1.
    EXPECT_EQ(writer.current_lsn(), 1);

    auto r1 = make_test_record(WalRecordType::BEGIN, 1);
    auto lsn1 = writer.append(r1);
    ASSERT_TRUE(lsn1.has_value()) << lsn1.error().message;
    EXPECT_EQ(*lsn1, 1);
    EXPECT_EQ(r1.lsn, 1);

    auto r2 = make_test_record(WalRecordType::INSERT, 1, 10, "hello");
    auto lsn2 = writer.append(r2);
    ASSERT_TRUE(lsn2.has_value()) << lsn2.error().message;
    EXPECT_EQ(*lsn2, 2);
    EXPECT_EQ(r2.lsn, 2);

    auto r3 = make_test_record(WalRecordType::COMMIT, 1);
    auto lsn3 = writer.append(r3);
    ASSERT_TRUE(lsn3.has_value()) << lsn3.error().message;
    EXPECT_EQ(*lsn3, 3);

    // Next LSN should be 4.
    EXPECT_EQ(writer.current_lsn(), 4);

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalWriter, AppendWithoutOpenFails) {
    TempWalDir dir;
    WalWriter writer(dir.path());

    auto r = make_test_record(WalRecordType::BEGIN, 1);
    auto result = writer.append(r);
    EXPECT_FALSE(result.has_value());
}

TEST(WalWriter, AppendEmptyAndLargeData) {
    TempWalDir dir;
    WalWriter writer(dir.path());
    ASSERT_TRUE(writer.open().has_value());

    // Empty data record.
    auto r1 = make_test_record(WalRecordType::CHECKPOINT);
    auto lsn1 = writer.append(r1);
    ASSERT_TRUE(lsn1.has_value());

    // Large data record (64KB).
    WalRecord r2;
    r2.type = WalRecordType::INSERT;
    r2.txn_id = 1;
    r2.table_id = 5;
    r2.data.resize(65536, 0xAB);
    auto lsn2 = writer.append(r2);
    ASSERT_TRUE(lsn2.has_value());
    EXPECT_EQ(*lsn2, 2);

    ASSERT_TRUE(writer.close().has_value());
}

// -- Flush --------------------------------------------------------------------

TEST(WalWriter, FlushUpdatesFlushLsn) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.enable_group_commit = false; // Manual flush only.
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    // Initially flushed_lsn is 0.
    EXPECT_EQ(writer.flushed_lsn(), 0);

    auto r1 = make_test_record(WalRecordType::BEGIN, 1);
    ASSERT_TRUE(writer.append(r1).has_value());
    auto r2 = make_test_record(WalRecordType::COMMIT, 1);
    ASSERT_TRUE(writer.append(r2).has_value());

    // Flushed LSN should still be 0 before explicit flush.
    EXPECT_EQ(writer.flushed_lsn(), 0);

    ASSERT_TRUE(writer.flush().has_value());

    // Now flushed_lsn should be 2 (latest written LSN).
    EXPECT_EQ(writer.flushed_lsn(), 2);

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalWriter, FlushWhenNotOpenIsNoop) {
    TempWalDir dir;
    WalWriter writer(dir.path());
    // flush() without open should not crash.
    auto result = writer.flush();
    EXPECT_TRUE(result.has_value());
}

// -- Segment rotation ---------------------------------------------------------

TEST(WalWriter, SegmentRotationOnSizeLimit) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.segment_size = 256; // Very small segment to force rotation.
    opts.enable_group_commit = false;
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    EXPECT_EQ(writer.current_segment_id(), 1);

    // Write enough records to exceed the 256-byte segment limit.
    // Each record is at least 47 bytes (overhead) + data.
    for (int i = 0; i < 20; ++i) {
        auto r = make_test_record(WalRecordType::INSERT, 1, 10, "payload-data-for-rotation-test");
        auto lsn = writer.append(r);
        ASSERT_TRUE(lsn.has_value()) << lsn.error().message;
    }

    // Should have rotated to at least segment 2.
    EXPECT_GT(writer.current_segment_id(), 1);

    // Multiple segment files should exist.
    int segment_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
        if (entry.path().filename().string().substr(0, 4) == "wal_") {
            ++segment_count;
        }
    }
    EXPECT_GT(segment_count, 1);

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalWriter, SegmentRotationPreservesLsnContinuity) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.segment_size = 128; // Tiny segment.
    opts.enable_group_commit = false;
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    lsn_t prev_lsn = 0;
    for (int i = 0; i < 30; ++i) {
        auto r = make_test_record(WalRecordType::INSERT, 1, 10, "data");
        auto lsn = writer.append(r);
        ASSERT_TRUE(lsn.has_value()) << lsn.error().message;
        EXPECT_EQ(*lsn, prev_lsn + 1);
        prev_lsn = *lsn;
    }

    EXPECT_EQ(prev_lsn, 30);
    EXPECT_GT(writer.current_segment_id(), 1);

    ASSERT_TRUE(writer.close().has_value());
}

// -- Reopen / scan_segment ----------------------------------------------------

TEST(WalWriter, ReopenResumesFromLastRecord) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.enable_group_commit = false;
    lsn_t last_lsn = 0;

    {
        WalWriter writer(dir.path(), opts);
        ASSERT_TRUE(writer.open().has_value());

        for (int i = 0; i < 5; ++i) {
            auto r = make_test_record(WalRecordType::INSERT, 1, 10, "data");
            auto lsn = writer.append(r);
            ASSERT_TRUE(lsn.has_value());
            last_lsn = *lsn;
        }

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    EXPECT_EQ(last_lsn, 5);

    // Reopen and verify that LSNs continue from 6.
    {
        WalWriter writer(dir.path(), opts);
        ASSERT_TRUE(writer.open().has_value());

        EXPECT_EQ(writer.current_lsn(), 6);
        EXPECT_EQ(writer.flushed_lsn(), 5);
        EXPECT_EQ(writer.current_segment_id(), 1);

        // Append more records.
        auto r = make_test_record(WalRecordType::COMMIT, 1);
        auto lsn = writer.append(r);
        ASSERT_TRUE(lsn.has_value());
        EXPECT_EQ(*lsn, 6);

        ASSERT_TRUE(writer.close().has_value());
    }
}

TEST(WalWriter, ReopenWithEmptySegment) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.enable_group_commit = false;

    {
        WalWriter writer(dir.path(), opts);
        ASSERT_TRUE(writer.open().has_value());
        // Don't write anything — just close.
        ASSERT_TRUE(writer.close().has_value());
    }

    // Reopen.
    {
        WalWriter writer(dir.path(), opts);
        ASSERT_TRUE(writer.open().has_value());
        EXPECT_EQ(writer.current_lsn(), 1); // Should still be 1.
        ASSERT_TRUE(writer.close().has_value());
    }
}

TEST(WalWriter, ReopenWithMultipleSegments) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.segment_size = 128; // Tiny to force rotations.
    opts.enable_group_commit = false;

    uint64_t expected_next_lsn = 0;
    uint64_t expected_segment = 0;

    {
        WalWriter writer(dir.path(), opts);
        ASSERT_TRUE(writer.open().has_value());

        for (int i = 0; i < 20; ++i) {
            auto r = make_test_record(WalRecordType::INSERT, 1, 10, "test-data");
            auto lsn = writer.append(r);
            ASSERT_TRUE(lsn.has_value());
        }

        expected_next_lsn = writer.current_lsn();
        expected_segment = writer.current_segment_id();

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    // Reopen should resume from the latest segment.
    {
        WalWriter writer(dir.path(), opts);
        ASSERT_TRUE(writer.open().has_value());

        EXPECT_EQ(writer.current_lsn(), expected_next_lsn);
        EXPECT_EQ(writer.current_segment_id(), expected_segment);

        // Continue writing.
        auto r = make_test_record(WalRecordType::COMMIT, 1);
        auto lsn = writer.append(r);
        ASSERT_TRUE(lsn.has_value());
        EXPECT_EQ(*lsn, expected_next_lsn);

        ASSERT_TRUE(writer.close().has_value());
    }
}

// -- Data integrity -----------------------------------------------------------

TEST(WalWriter, WrittenDataIsReadableAndValid) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.enable_group_commit = false;
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    // Write a few records.
    auto r1 = make_test_record(WalRecordType::BEGIN, 42);
    ASSERT_TRUE(writer.append(r1).has_value());

    WalRecord r2;
    r2.type = WalRecordType::INSERT;
    r2.txn_id = 42;
    r2.table_id = 7;
    r2.page_id = 100;
    r2.slot_id = 3;
    r2.data = {0xDE, 0xAD, 0xBE, 0xEF};
    ASSERT_TRUE(writer.append(r2).has_value());

    ASSERT_TRUE(writer.flush().has_value());
    ASSERT_TRUE(writer.close().has_value());

    // Read the segment file and deserialize records.
    auto seg_path = dir.path() / "wal_000001";
    auto file_size = std::filesystem::file_size(seg_path);
    ASSERT_GT(file_size, 0u);

    std::vector<uint8_t> buf(file_size);
    std::ifstream ifs(seg_path, std::ios::binary);
    ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(file_size));

    // Deserialize first record.
    size_t offset = 0;
    uint32_t rec_len1 = 0;
    std::memcpy(&rec_len1, buf.data() + offset, sizeof(uint32_t));
    size_t total1 = static_cast<size_t>(rec_len1) + sizeof(uint32_t);

    auto d1 = deserialize_wal_record(std::span<const uint8_t>(buf.data() + offset, total1));
    ASSERT_TRUE(d1.has_value()) << d1.error().message;
    EXPECT_EQ(d1->lsn, 1);
    EXPECT_EQ(d1->txn_id, 42);
    EXPECT_EQ(d1->type, WalRecordType::BEGIN);
    offset += total1;

    // Deserialize second record.
    uint32_t rec_len2 = 0;
    std::memcpy(&rec_len2, buf.data() + offset, sizeof(uint32_t));
    size_t total2 = static_cast<size_t>(rec_len2) + sizeof(uint32_t);

    auto d2 = deserialize_wal_record(std::span<const uint8_t>(buf.data() + offset, total2));
    ASSERT_TRUE(d2.has_value()) << d2.error().message;
    EXPECT_EQ(d2->lsn, 2);
    EXPECT_EQ(d2->txn_id, 42);
    EXPECT_EQ(d2->type, WalRecordType::INSERT);
    EXPECT_EQ(d2->table_id, 7u);
    EXPECT_EQ(d2->page_id, 100u);
    EXPECT_EQ(d2->slot_id, 3);
    ASSERT_EQ(d2->data.size(), 4u);
    EXPECT_EQ(d2->data[0], 0xDE);
    EXPECT_EQ(d2->data[1], 0xAD);
    EXPECT_EQ(d2->data[2], 0xBE);
    EXPECT_EQ(d2->data[3], 0xEF);
}

// -- Group commit -------------------------------------------------------------

TEST(WalWriter, GroupCommitFlushesPeriodically) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.enable_group_commit = true;
    opts.flush_interval = std::chrono::milliseconds(20); // 20ms flush interval.
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    // Write a record.
    auto r = make_test_record(WalRecordType::BEGIN, 1);
    ASSERT_TRUE(writer.append(r).has_value());

    // Wait long enough for the group commit thread to flush.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // flushed_lsn should be updated by the background thread.
    EXPECT_GE(writer.flushed_lsn(), 1);

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalWriter, GroupCommitDisabled) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.enable_group_commit = false;
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    auto r = make_test_record(WalRecordType::BEGIN, 1);
    ASSERT_TRUE(writer.append(r).has_value());

    // Without group commit, flushed_lsn remains 0 until explicit flush.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(writer.flushed_lsn(), 0);

    // Explicit flush should work.
    ASSERT_TRUE(writer.flush().has_value());
    EXPECT_EQ(writer.flushed_lsn(), 1);

    ASSERT_TRUE(writer.close().has_value());
}

// -- Concurrent access --------------------------------------------------------

TEST(WalWriter, ConcurrentAppendsSafe) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.enable_group_commit = true;
    opts.flush_interval = std::chrono::milliseconds(10);
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    constexpr int num_threads = 4;
    constexpr int records_per_thread = 100;
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < records_per_thread; ++i) {
                auto r = make_test_record(
                    WalRecordType::INSERT, static_cast<txn_id_t>(t + 1), 10, "concurrent-data");
                auto lsn = writer.append(r);
                if (!lsn.has_value()) {
                    errors.fetch_add(1);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(errors.load(), 0);

    // Total records should be num_threads * records_per_thread.
    int total = num_threads * records_per_thread;
    EXPECT_EQ(writer.current_lsn(), static_cast<lsn_t>(total + 1));

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalWriter, ConcurrentAppendsAndFlushes) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.enable_group_commit = false; // Manual flushes.
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    std::atomic<bool> done{false};
    std::atomic<int> errors{0};

    // Writer thread.
    std::thread writer_thread([&] {
        for (int i = 0; i < 200; ++i) {
            auto r = make_test_record(WalRecordType::INSERT, 1, 10, "data");
            auto lsn = writer.append(r);
            if (!lsn.has_value()) {
                errors.fetch_add(1);
            }
        }
        done.store(true);
    });

    // Flusher thread.
    std::thread flusher_thread([&] {
        while (!done.load()) {
            auto result = writer.flush();
            if (!result.has_value()) {
                errors.fetch_add(1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // One final flush.
        (void)writer.flush();
    });

    writer_thread.join();
    flusher_thread.join();

    EXPECT_EQ(errors.load(), 0);
    EXPECT_EQ(writer.current_lsn(), 201);

    ASSERT_TRUE(writer.close().has_value());
}

// -- Edge cases ---------------------------------------------------------------

TEST(WalWriter, SegmentPathFormat) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.enable_group_commit = false;
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    // Segment 1 file should be named wal_000001.
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "wal_000001"));

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalWriter, CloseSetsFlushLsn) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.enable_group_commit = false;
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    auto r1 = make_test_record(WalRecordType::BEGIN, 1);
    ASSERT_TRUE(writer.append(r1).has_value());
    auto r2 = make_test_record(WalRecordType::COMMIT, 1);
    ASSERT_TRUE(writer.append(r2).has_value());

    // Before close, flushed_lsn is 0.
    EXPECT_EQ(writer.flushed_lsn(), 0);

    ASSERT_TRUE(writer.close().has_value());

    // After close, flushed_lsn should be 2 (last written).
    EXPECT_EQ(writer.flushed_lsn(), 2);
}

TEST(WalWriter, SegmentTruncatedToWrittenSize) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.enable_group_commit = false;
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    auto r = make_test_record(WalRecordType::BEGIN, 1);
    ASSERT_TRUE(writer.append(r).has_value());

    ASSERT_TRUE(writer.close().has_value());

    // The segment file size should equal exactly one serialized record.
    auto seg_path = dir.path() / "wal_000001";
    auto file_size = std::filesystem::file_size(seg_path);
    size_t expected_size = serialized_wal_record_size(r);
    EXPECT_EQ(file_size, expected_size);
}

TEST(WalWriter, NonExistentWalDirIsCreated) {
    auto base = std::filesystem::temp_directory_path() / "wal_nested_test";
    std::filesystem::remove_all(base);
    auto deep_path = base / "level1" / "level2" / "wal";

    WalWriterOptions opts;
    opts.enable_group_commit = false;
    WalWriter writer(deep_path, opts);
    ASSERT_TRUE(writer.open().has_value());
    EXPECT_TRUE(std::filesystem::exists(deep_path));
    ASSERT_TRUE(writer.close().has_value());

    std::filesystem::remove_all(base);
}

TEST(WalWriter, ManyRecordsAcrossSegments) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.segment_size = 512;
    opts.enable_group_commit = false;
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    constexpr int count = 100;
    for (int i = 0; i < count; ++i) {
        auto r = make_test_record(WalRecordType::INSERT, 1, 10, "segment-rotation-stress");
        auto lsn = writer.append(r);
        ASSERT_TRUE(lsn.has_value()) << "Failed at record " << i << ": " << lsn.error().message;
        EXPECT_EQ(*lsn, static_cast<lsn_t>(i + 1));
    }

    EXPECT_EQ(writer.current_lsn(), static_cast<lsn_t>(count + 1));
    EXPECT_GT(writer.current_segment_id(), 1u);

    ASSERT_TRUE(writer.close().has_value());
}

// -- flush_until (group commit durability wait) --------------------------------

TEST(WalWriterFlushUntil, ReturnsOnceDurable) {
    TempWalDir dir;
    WalWriterOptions opts;
    // Long interval so the test only passes if flush_until actively wakes
    // the flush thread instead of riding the periodic timer.
    opts.flush_interval = std::chrono::milliseconds(500);
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    auto r = make_test_record(WalRecordType::INSERT, 1, 10, "durable");
    auto lsn = writer.append(r);
    ASSERT_TRUE(lsn.has_value()) << lsn.error().message;
    ASSERT_LT(writer.flushed_lsn(), *lsn);

    auto result = writer.flush_until(*lsn);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_GE(writer.flushed_lsn(), *lsn);

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalWriterFlushUntil, FastPathWhenAlreadyFlushed) {
    TempWalDir dir;
    WalWriter writer(dir.path());
    ASSERT_TRUE(writer.open().has_value());

    auto r = make_test_record(WalRecordType::INSERT, 1, 10, "fastpath");
    auto lsn = writer.append(r);
    ASSERT_TRUE(lsn.has_value());
    ASSERT_TRUE(writer.flush().has_value());
    ASSERT_GE(writer.flushed_lsn(), *lsn);

    // Already durable: must return ok immediately.
    auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(writer.flush_until(*lsn).has_value());
    EXPECT_TRUE(writer.flush_until(0).has_value()); // lsn 0 is trivially durable.
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::seconds(1));

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalWriterFlushUntil, WakesPromptlyWithoutTimer) {
    TempWalDir dir;
    WalWriterOptions opts;
    // Effectively disable the periodic timer: 10s interval. flush_until must
    // still complete quickly by waking the flusher itself.
    opts.flush_interval = std::chrono::seconds(10);
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    auto r = make_test_record(WalRecordType::COMMIT, 1);
    auto lsn = writer.append(r);
    ASSERT_TRUE(lsn.has_value());

    auto start = std::chrono::steady_clock::now();
    auto result = writer.flush_until(*lsn);
    auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_GE(writer.flushed_lsn(), *lsn);
    // Generous bound: far below the 10s interval, so we know the waiter
    // woke the flush thread rather than waiting for the timer.
    EXPECT_LT(elapsed, std::chrono::seconds(5));

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalWriterFlushUntil, NotAppendedLsnIsInvalidArgument) {
    TempWalDir dir;
    WalWriter writer(dir.path());
    ASSERT_TRUE(writer.open().has_value());

    auto r = make_test_record(WalRecordType::INSERT, 1, 10, "x");
    auto lsn = writer.append(r);
    ASSERT_TRUE(lsn.has_value());

    // current_lsn() is the next unassigned LSN — waiting on it (or beyond)
    // can never succeed and must fail fast instead of hanging.
    auto result = writer.flush_until(writer.current_lsn());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);

    auto far = writer.flush_until(writer.current_lsn() + 1000);
    ASSERT_FALSE(far.has_value());
    EXPECT_EQ(far.error().code, StatusCode::INVALID_ARGUMENT);

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalWriterFlushUntil, NotOpenFails) {
    TempWalDir dir;
    WalWriter writer(dir.path());

    auto result = writer.flush_until(1);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(WalWriterFlushUntil, GroupCommitDisabledFallsBackToSyncFlush) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.enable_group_commit = false;
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    auto r = make_test_record(WalRecordType::INSERT, 1, 10, "sync");
    auto lsn = writer.append(r);
    ASSERT_TRUE(lsn.has_value());
    ASSERT_LT(writer.flushed_lsn(), *lsn);

    auto result = writer.flush_until(*lsn);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_GE(writer.flushed_lsn(), *lsn);

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalWriterFlushUntil, ConcurrentWaitersAllReturnDurable) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.flush_interval = std::chrono::milliseconds(200);
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    constexpr int num_threads = 8;
    constexpr int appends_per_thread = 25;

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&writer, &failures] {
            for (int i = 0; i < appends_per_thread; ++i) {
                auto r = make_test_record(WalRecordType::COMMIT, 1);
                auto lsn = writer.append(r);
                if (!lsn.has_value()) {
                    ++failures;
                    return;
                }
                auto result = writer.flush_until(*lsn);
                if (!result.has_value() || writer.flushed_lsn() < *lsn) {
                    ++failures;
                    return;
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(writer.current_lsn(), static_cast<lsn_t>(num_threads * appends_per_thread + 1));
    EXPECT_GE(writer.flushed_lsn(), static_cast<lsn_t>(num_threads * appends_per_thread));

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalWriterFlushUntil, ShutdownWhileWaitingDoesNotHang) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.flush_interval = std::chrono::seconds(10); // Timer effectively off.
    auto writer = std::make_unique<WalWriter>(dir.path(), opts);
    ASSERT_TRUE(writer->open().has_value());

    auto r = make_test_record(WalRecordType::INSERT, 1, 10, "shutdown");
    auto lsn = writer->append(r);
    ASSERT_TRUE(lsn.has_value());

    std::atomic<bool> waiter_returned{false};
    std::thread waiter([&writer, &waiter_returned, target = *lsn] {
        // Either outcome is acceptable: the close-path final flush makes the
        // LSN durable (ok), or the waiter observes shutdown (error). The
        // requirement is that it returns at all.
        auto result = writer->flush_until(target);
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
        }
        waiter_returned.store(true);
    });

    // Give the waiter a moment to block, then close the writer.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_TRUE(writer->close().has_value());

    waiter.join();
    EXPECT_TRUE(waiter_returned.load());
}

} // namespace
} // namespace sixseven
