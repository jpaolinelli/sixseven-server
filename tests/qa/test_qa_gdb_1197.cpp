// QA adversarial tests for GDB-1197: WalArchiveManager destructor drain/join
// semantics.
//
// GDB-1197 itself only strengthens a vacuous dev test (test_wal_archive.cpp);
// no production code changed. These QA tests independently probe the
// destructor/stop() contract described in the ticket: does destruction
// synchronously drain ALL pending segments and join the worker, with no
// segment silently lost and no leaked thread touching freed memory?
//
// Adversarial scenarios covered:
//  1. Many pending segments (burst) at destruction time -- does the
//     destructor drain ALL of them, or does the archive_loop() background
//     thread exit its `while (running_)` loop after processing only one
//     more item once stop() flips running_ to false?
//  2. Destruct immediately after a single enqueue (race between worker
//     start and destructor).
//  3. Destruct with archiving disabled (no thread ever started) -- must be
//     a clean no-op, not a crash/join-on-non-joinable-thread issue.
//  4. Back-to-back manager lifecycles over the same archive dir.
//  5. No partially-written/corrupt segment left behind after destruction
//     (archived file size must match source size exactly).

#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_archive.h"
#include "sixseven/storage/wal_record.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <vector>

#include "../unit/test_wal_helpers.h"

namespace sixseven {
namespace {

using test::TempWalDir;

class TempArchiveDirQa {
public:
    TempArchiveDirQa() {
        path_ = std::filesystem::temp_directory_path() /
                ("wal_archive_qa_gdb1197_" + std::to_string(counter_++));
        std::filesystem::create_directories(path_);
    }
    ~TempArchiveDirQa() { std::filesystem::remove_all(path_); }

    TempArchiveDirQa(const TempArchiveDirQa&) = delete;
    TempArchiveDirQa& operator=(const TempArchiveDirQa&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    static inline std::atomic<int> counter_{0};
};

WalRecord make_record(WalRecordType type,
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

struct CompletedSegment {
    uint64_t segment_id;
    lsn_t last_lsn;
};

// Writes enough records to force exactly one segment rotation and returns
// the segment that just completed.
CompletedSegment write_and_rotate(WalWriter& writer) {
    lsn_t last_lsn = 0;
    uint64_t seg_before = writer.current_segment_id();
    while (writer.current_segment_id() == seg_before) {
        auto r = make_record(WalRecordType::INSERT, 1, 10, "payload-for-rotation-qa-gdb1197");
        auto lsn = writer.append(r);
        if (lsn.has_value()) {
            last_lsn = *lsn;
        }
    }
    return {seg_before, last_lsn - 1};
}

// -- 1. Burst of many pending segments at destruction time -------------------
//
// This is the critical adversarial case. archive_loop() is:
//   while (running_) {
//       wait for !pending_.empty() || !running_
//       if (!pending_.empty()) { pop ONE item; archive it; }
//   }
// stop() sets running_ = false THEN notifies. If several items are queued
// when stop() runs, the worker wakes, the outer while(running_) is
// re-checked EVERY iteration -- but running_ is already false, so the loop
// body executes at most once more (draining a single item) before the
// while-condition kills the loop. Any additional queued items are left
// un-archived: silent data loss. Enqueue many segments right before the
// manager goes out of scope (as the destructor does implicitly) and verify
// every single one shows up in the archive directory afterward.
TEST(QA_GDB1197_WalArchiveManager, DestructorDrainsAllPendingSegmentsInBurst) {
    TempWalDir wal_dir;
    TempArchiveDirQa archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    WalArchiveOptions archive_opts;
    archive_opts.enabled = true;

    constexpr int kNumSegments = 25; // deliberately large burst
    std::vector<CompletedSegment> segments;
    segments.reserve(kNumSegments);

    {
        WalArchiveManager mgr(wal_dir.path(), archive_dir.path(), archive_opts);
        ASSERT_TRUE(mgr.start().has_value());

        // Enqueue as fast as possible, back-to-back, with no synchronization
        // wait in between -- this maximizes the chance that the worker is
        // still processing item N when items N+1..kNumSegments-1 are pushed,
        // so that when the destructor fires there is a real backlog.
        for (int i = 0; i < kNumSegments; ++i) {
            auto seg = write_and_rotate(writer);
            segments.push_back(seg);
            mgr.enqueue_segment(seg.segment_id, seg.last_lsn);
        }
        // Destructor fires here with (likely) several items still queued.
    }

    WalArchiveManager verify_mgr(wal_dir.path(), archive_dir.path());
    auto list = verify_mgr.list_archived_segments();
    ASSERT_TRUE(list.has_value()) << list.error().message;
    EXPECT_EQ(list->size(), static_cast<size_t>(kNumSegments))
        << "destructor must drain ALL pending segments, not just one more "
           "after stop() flips running_ to false";

    for (const auto& seg : segments) {
        auto archived = verify_mgr.get_archived_segment(seg.segment_id);
        EXPECT_TRUE(archived.has_value())
            << "segment " << seg.segment_id << " missing from archive -- lost on destruction";
        if (archived.has_value()) {
            EXPECT_TRUE(std::filesystem::exists(*archived));
        }
    }

    ASSERT_TRUE(writer.close().has_value());
}

// -- 2. Destruct immediately after a single enqueue (start/enqueue race) -----
TEST(QA_GDB1197_WalArchiveManager, DestructImmediatelyAfterSingleEnqueue) {
    TempWalDir wal_dir;
    TempArchiveDirQa archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    WalArchiveOptions archive_opts;
    archive_opts.enabled = true;

    CompletedSegment seg{};
    {
        WalArchiveManager mgr(wal_dir.path(), archive_dir.path(), archive_opts);
        ASSERT_TRUE(mgr.start().has_value());
        seg = write_and_rotate(writer);
        mgr.enqueue_segment(seg.segment_id, seg.last_lsn);
        // No sleep, no wait -- destructor races the worker thread's startup.
    }

    WalArchiveManager verify_mgr(wal_dir.path(), archive_dir.path());
    auto archived = verify_mgr.get_archived_segment(seg.segment_id);
    ASSERT_TRUE(archived.has_value()) << "segment enqueued immediately before destruction was lost";
    EXPECT_TRUE(std::filesystem::exists(*archived));

    ASSERT_TRUE(writer.close().has_value());
}

// -- 3. Destruct with archiving disabled: clean no-op, no thread ever ran ----
TEST(QA_GDB1197_WalArchiveManager, DestructWithArchivingDisabledIsCleanNoOp) {
    TempWalDir wal_dir;
    TempArchiveDirQa archive_dir;

    WalArchiveOptions archive_opts;
    archive_opts.enabled = false;

    {
        WalArchiveManager mgr(wal_dir.path(), archive_dir.path(), archive_opts);
        auto start_result = mgr.start();
        ASSERT_TRUE(start_result.has_value());
        EXPECT_FALSE(mgr.is_running());
        // enqueue_segment on a non-running manager must be a safe no-op.
        mgr.enqueue_segment(1, 100);
        // Destructor must not attempt to join a thread that was never
        // started (archive_thread_.joinable() must guard correctly).
    }
    SUCCEED();
}

// -- 4. Back-to-back manager lifecycles over the same archive dir -----------
TEST(QA_GDB1197_WalArchiveManager, BackToBackLifecyclesOverSameArchiveDir) {
    TempWalDir wal_dir;
    TempArchiveDirQa archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    WalArchiveOptions archive_opts;
    archive_opts.enabled = true;

    std::vector<CompletedSegment> all_segments;

    // Run several independent manager lifecycles in a row, each constructing,
    // starting, enqueuing, and destructing (no explicit stop()) before the
    // next one starts.
    for (int cycle = 0; cycle < 5; ++cycle) {
        WalArchiveManager mgr(wal_dir.path(), archive_dir.path(), archive_opts);
        ASSERT_TRUE(mgr.start().has_value());
        auto seg = write_and_rotate(writer);
        all_segments.push_back(seg);
        mgr.enqueue_segment(seg.segment_id, seg.last_lsn);
        // Destructor runs at end of each loop iteration.
    }

    WalArchiveManager verify_mgr(wal_dir.path(), archive_dir.path());
    auto list = verify_mgr.list_archived_segments();
    ASSERT_TRUE(list.has_value()) << list.error().message;
    EXPECT_EQ(list->size(), all_segments.size());
    for (const auto& seg : all_segments) {
        auto archived = verify_mgr.get_archived_segment(seg.segment_id);
        EXPECT_TRUE(archived.has_value()) << "segment " << seg.segment_id << " lost across cycle";
    }

    ASSERT_TRUE(writer.close().has_value());
}

// -- 5. No partial/corrupt archived file left after destruction -------------
TEST(QA_GDB1197_WalArchiveManager, NoCorruptSegmentAfterDestruction) {
    TempWalDir wal_dir;
    TempArchiveDirQa archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    WalArchiveOptions archive_opts;
    archive_opts.enabled = true;

    CompletedSegment seg{};
    std::filesystem::path wal_segment_file;
    {
        WalArchiveManager mgr(wal_dir.path(), archive_dir.path(), archive_opts);
        ASSERT_TRUE(mgr.start().has_value());
        seg = write_and_rotate(writer);
        mgr.enqueue_segment(seg.segment_id, seg.last_lsn);
    }

    // Locate source WAL segment file to compare sizes.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "wal_%06llu", static_cast<unsigned long long>(seg.segment_id));
    wal_segment_file = wal_dir.path() / buf;
    ASSERT_TRUE(std::filesystem::exists(wal_segment_file));

    WalArchiveManager verify_mgr(wal_dir.path(), archive_dir.path());
    auto archived = verify_mgr.get_archived_segment(seg.segment_id);
    ASSERT_TRUE(archived.has_value()) << archived.error().message;
    ASSERT_TRUE(std::filesystem::exists(*archived));

    auto src_size = std::filesystem::file_size(wal_segment_file);
    auto dst_size = std::filesystem::file_size(*archived);
    EXPECT_EQ(src_size, dst_size) << "archived segment size mismatch -- possible partial copy";

    ASSERT_TRUE(writer.close().has_value());
}

} // namespace
} // namespace sixseven
