// QA regression tests for GDB-1193.
//
// GDB-1193 strengthened WalArchiveManager.CleanupRespectsRetentionProvider
// (replacing a vacuous EXPECT_GE(size, 2) with exact segment-id assertions)
// and added a fault-injection sibling test (CleanupBeforeNoOpLeavesAllSegments)
// proving the strengthened assertions actually discriminate a no-op cleanup.
//
// This is the regression test backing GDB-196 (replication-slot retention):
// cleanup_before() must never delete an archived WAL segment still needed
// by a standby, as determined by the retention_lsn_provider callback.
//
// These QA tests independently probe the retention-boundary logic in
// src/storage/wal_archive.cpp (cleanup_before()) with adversarial cases the
// dev tests do not cover: exact boundary LSNs, empty archive, single
// segment, retention below/above all segments, and invalid_lsn provider
// behavior.

#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_archive.h"
#include "sixseven/storage/wal_record.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <vector>

#include "test_wal_helpers.h"

namespace sixseven {
namespace {

using test::TempWalDir;

class TempArchiveDirQa {
public:
    TempArchiveDirQa() {
        path_ = std::filesystem::temp_directory_path() /
                ("wal_archive_qa_gdb1193_" + std::to_string(counter_++));
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

CompletedSegment write_and_rotate(WalWriter& writer) {
    lsn_t last_lsn = 0;
    uint64_t seg_before = writer.current_segment_id();

    while (writer.current_segment_id() == seg_before) {
        auto r = make_record(WalRecordType::INSERT, 1, 10, "payload-for-rotation");
        auto lsn = writer.append(r);
        if (lsn.has_value()) {
            last_lsn = *lsn;
        }
    }
    return {seg_before, last_lsn - 1};
}

class WalArchiveRetentionQaTest : public ::testing::Test {
protected:
    void SetUp() override {
        WalWriterOptions opts;
        opts.segment_size = 256;
        opts.enable_group_commit = false;
        writer_ = std::make_unique<WalWriter>(wal_dir_.path(), opts);
        ASSERT_TRUE(writer_->open().has_value());
        mgr_ = std::make_unique<WalArchiveManager>(wal_dir_.path(), archive_dir_.path());
    }

    void TearDown() override {
        if (writer_) {
            auto r = writer_->close();
            (void)r;
        }
    }

    std::vector<CompletedSegment> archive_n_segments(int n) {
        std::vector<CompletedSegment> segments;
        for (int i = 0; i < n; ++i) {
            auto seg = write_and_rotate(*writer_);
            segments.push_back(seg);
            EXPECT_TRUE(mgr_->archive_segment(seg.segment_id, seg.last_lsn).has_value());
        }
        return segments;
    }

    TempWalDir wal_dir_;
    TempArchiveDirQa archive_dir_;
    std::unique_ptr<WalWriter> writer_;
    std::unique_ptr<WalArchiveManager> mgr_;
};

// Retention LSN exactly equal to a segment's last_lsn: that segment must be
// RETAINED (removal condition is seg_last_lsn < lsn, strictly less-than), so
// setting retention to segments[0].last_lsn must not delete segment 0.
TEST_F(WalArchiveRetentionQaTest, GDB1193_RetentionExactlyOnSegmentBoundaryRetainsThatSegment) {
    auto segments = archive_n_segments(3);

    lsn_t retention_lsn = segments[0].last_lsn;
    mgr_->set_retention_lsn_provider([retention_lsn]() { return retention_lsn; });

    ASSERT_TRUE(mgr_->cleanup_before(999999).has_value());

    auto remaining = mgr_->list_archived_segments();
    ASSERT_TRUE(remaining.has_value());
    // Segment 0's last_lsn == retention_lsn, so it must NOT be removed:
    // deleting a segment at the retention boundary is the exact GDB-196
    // failure mode (unrecoverable for a standby positioned there).
    ASSERT_EQ(remaining->size(), 3u);
    EXPECT_EQ((*remaining)[0], segments[0].segment_id);
    EXPECT_EQ((*remaining)[1], segments[1].segment_id);
    EXPECT_EQ((*remaining)[2], segments[2].segment_id);
}

// Retention provider returns the oldest segment's LSN: only that segment's
// predecessors (none) are removable; nothing should be deleted since
// segment 0 is retained by the same boundary rule as above, and 1,2 have
// larger LSNs so they are also retained a fortiori.
TEST_F(WalArchiveRetentionQaTest, GDB1193_RetentionAtOldestSegmentRemovesNothing) {
    auto segments = archive_n_segments(3);

    lsn_t retention_lsn = segments[0].last_lsn;
    mgr_->set_retention_lsn_provider([retention_lsn]() { return retention_lsn; });
    ASSERT_TRUE(mgr_->cleanup_before(segments[0].last_lsn + 1).has_value());

    auto remaining = mgr_->list_archived_segments();
    ASSERT_TRUE(remaining.has_value());
    ASSERT_EQ(remaining->size(), 3u);
}

// Retention provider returns the newest segment's LSN: everything strictly
// before that boundary should be removed (segments 0 and 1), but the
// newest segment itself must be retained (boundary is exclusive).
TEST_F(WalArchiveRetentionQaTest, GDB1193_RetentionAtNewestSegmentKeepsOnlyNewest) {
    auto segments = archive_n_segments(3);

    lsn_t retention_lsn = segments[2].last_lsn;
    mgr_->set_retention_lsn_provider([retention_lsn]() { return retention_lsn; });
    ASSERT_TRUE(mgr_->cleanup_before(999999).has_value());

    auto remaining = mgr_->list_archived_segments();
    ASSERT_TRUE(remaining.has_value());
    ASSERT_EQ(remaining->size(), 1u);
    EXPECT_EQ((*remaining)[0], segments[2].segment_id);
}

// Retention provider returning a value beyond all archived LSNs must not
// cause cleanup to remove anything beyond what cleanup_before's own lsn
// argument allows -- i.e. the clamp only lowers the effective cleanup LSN,
// it should never raise it above what the caller requested.
TEST_F(WalArchiveRetentionQaTest, GDB1193_RetentionBeyondAllSegmentsDoesNotExpandCleanup) {
    auto segments = archive_n_segments(3);

    lsn_t retention_lsn = segments[2].last_lsn + 1000;
    mgr_->set_retention_lsn_provider([retention_lsn]() { return retention_lsn; });

    // Caller only requests cleanup below segment 0's last_lsn + 1: even
    // though retention is far beyond everything, cleanup must not remove
    // segments the caller-supplied lsn doesn't reach either (min of the two
    // bounds), since min_slot_lsn < lsn is false here (retention > lsn) so
    // no clamping occurs and only segment 0 must go.
    ASSERT_TRUE(mgr_->cleanup_before(segments[0].last_lsn + 1).has_value());

    auto remaining = mgr_->list_archived_segments();
    ASSERT_TRUE(remaining.has_value());
    ASSERT_EQ(remaining->size(), 2u);
    EXPECT_EQ((*remaining)[0], segments[1].segment_id);
    EXPECT_EQ((*remaining)[1], segments[2].segment_id);
}

// cleanup_before threshold below all archived segments: nothing should be
// removed regardless of retention provider.
TEST_F(WalArchiveRetentionQaTest, GDB1193_CleanupThresholdBelowAllSegmentsRemovesNothing) {
    auto segments = archive_n_segments(3);
    mgr_->set_retention_lsn_provider([]() { return static_cast<lsn_t>(0); });

    ASSERT_TRUE(mgr_->cleanup_before(1).has_value());

    auto remaining = mgr_->list_archived_segments();
    ASSERT_TRUE(remaining.has_value());
    ASSERT_EQ(remaining->size(), 3u);
}

// cleanup_before threshold above all archived segments, with no retention
// provider set: all segments should be removed (baseline sanity check that
// complements CleanupWithoutProviderRemovesAll in the dev suite, but scoped
// as an explicit QA regression for this ticket).
TEST_F(WalArchiveRetentionQaTest, GDB1193_CleanupThresholdAboveAllSegmentsRemovesAllWithNoProvider) {
    auto segments = archive_n_segments(3);
    ASSERT_TRUE(mgr_->cleanup_before(999999).has_value());

    auto remaining = mgr_->list_archived_segments();
    ASSERT_TRUE(remaining.has_value());
    EXPECT_EQ(remaining->size(), 0u);
}

// Empty archive: cleanup_before must be a safe no-op, not an error.
TEST_F(WalArchiveRetentionQaTest, GDB1193_CleanupBeforeOnEmptyArchiveIsNoOpNotError) {
    lsn_t retention_lsn = 42;
    mgr_->set_retention_lsn_provider([retention_lsn]() { return retention_lsn; });

    auto result = mgr_->cleanup_before(999999);
    ASSERT_TRUE(result.has_value());

    auto remaining = mgr_->list_archived_segments();
    ASSERT_TRUE(remaining.has_value());
    EXPECT_EQ(remaining->size(), 0u);
}

// Single segment archived, retention provider pins it: cleanup_before must
// retain the lone segment exactly (not partially remove / corrupt state).
TEST_F(WalArchiveRetentionQaTest, GDB1193_SingleSegmentRetainedByProviderSurvivesCleanup) {
    auto segments = archive_n_segments(1);

    lsn_t retention_lsn = segments[0].last_lsn;
    mgr_->set_retention_lsn_provider([retention_lsn]() { return retention_lsn; });
    ASSERT_TRUE(mgr_->cleanup_before(999999).has_value());

    auto remaining = mgr_->list_archived_segments();
    ASSERT_TRUE(remaining.has_value());
    ASSERT_EQ(remaining->size(), 1u);
    EXPECT_EQ((*remaining)[0], segments[0].segment_id);
}

// Single segment archived, no retention provider, cleanup threshold above
// it: the lone segment should be fully removed.
TEST_F(WalArchiveRetentionQaTest, GDB1193_SingleSegmentRemovedWithoutProviderAboveThreshold) {
    auto segments = archive_n_segments(1);
    ASSERT_TRUE(mgr_->cleanup_before(segments[0].last_lsn + 1).has_value());

    auto remaining = mgr_->list_archived_segments();
    ASSERT_TRUE(remaining.has_value());
    EXPECT_EQ(remaining->size(), 0u);
}

// Retention provider returning invalid_lsn (sentinel meaning "no active
// slots / don't clamp") must not block cleanup at all -- everything below
// the caller's threshold should be removed as if no provider were set.
TEST_F(WalArchiveRetentionQaTest, GDB1193_RetentionProviderReturningInvalidLsnDoesNotClamp) {
    auto segments = archive_n_segments(3);
    mgr_->set_retention_lsn_provider([]() { return invalid_lsn; });

    ASSERT_TRUE(mgr_->cleanup_before(999999).has_value());

    auto remaining = mgr_->list_archived_segments();
    ASSERT_TRUE(remaining.has_value());
    EXPECT_EQ(remaining->size(), 0u);
}

// Repeated cleanup_before calls with a static retention provider must be
// idempotent: the second call should be a safe no-op given the first call
// already enforced the boundary.
TEST_F(WalArchiveRetentionQaTest, GDB1193_RepeatedCleanupBeforeIsIdempotent) {
    auto segments = archive_n_segments(3);

    lsn_t retention_lsn = segments[1].last_lsn;
    mgr_->set_retention_lsn_provider([retention_lsn]() { return retention_lsn; });

    ASSERT_TRUE(mgr_->cleanup_before(999999).has_value());
    auto first = mgr_->list_archived_segments();
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(first->size(), 2u);

    ASSERT_TRUE(mgr_->cleanup_before(999999).has_value());
    auto second = mgr_->list_archived_segments();
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(second->size(), 2u);
    EXPECT_EQ((*second)[0], segments[1].segment_id);
    EXPECT_EQ((*second)[1], segments[2].segment_id);
}

}  // namespace
}  // namespace sixseven
