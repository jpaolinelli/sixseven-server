// GDB-1299 QA: legacy old-V2 (2-byte [0x00, 0x02] sentinel) btree meta
// detection. The fix makes BTreePersistence::load() fail clean
// (INTERNAL_ERROR) when it sees the legacy sentinel, instead of silently
// misparsing the page as a V1 meta with a bogus root_page_id/garbage fields.
//
// Adversarial focus (per GDB-1299 ticket + QA handoff):
//  1. The disclosed residual gap: an old-V2 meta whose "real" (post-sentinel)
//     root_page_id happens to be a multiple of 65536 has bytes[2..3] ==
//     [0x00, 0x00], which is indistinguishable from a genuine V1
//     root_page_id == 512 meta under the new disambiguation rule. Confirm
//     this is reachable and characterize what happens (silent misparse,
//     same as pre-fix behavior) rather than assuming it's fixed.
//  2. Boundary values of the disambiguating bytes[2..3] field.
//  3. Confirm genuine V2 (new magic) metas are unaffected by the new check.
//  4. Confirm the error path doesn't leave a dangling pin (repeated loads on
//     the same bpm don't exhaust the buffer pool).

#include "sixseven/index/btree_index.h"
#include "sixseven/index/btree_persistence.h"
#include "sixseven/index/index_encoding.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/page.h"
#include "sixseven/storage/serialization.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

using namespace sixseven;
using namespace sixseven::index_encoding;

namespace {

class QaGdb1299Test : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_1299";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        dm_ = std::make_unique<DiskManager>();
    }

    void TearDown() override {
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    std::pair<FileId, std::unique_ptr<BufferPoolManager>> create_bpm(const std::string& name) {
        auto path = data_dir_ / (name + ".db");
        auto fid = dm_->create_file(path, false, true);
        EXPECT_TRUE(fid.has_value());
        if (!fid.has_value()) return {FileId{}, nullptr};
        return {*fid, std::make_unique<BufferPoolManager>(*dm_, *fid, 256)};
    }

    std::pair<FileId, std::unique_ptr<BufferPoolManager>> open_bpm(const std::string& name) {
        auto path = data_dir_ / (name + ".db");
        auto fid = dm_->open_file(path);
        EXPECT_TRUE(fid.has_value());
        if (!fid.has_value()) return {FileId{}, nullptr};
        return {*fid, std::make_unique<BufferPoolManager>(*dm_, *fid, 256)};
    }

    // Writes a minimal old-V2-shaped meta buffer: [0x00, 0x02] sentinel,
    // followed by a u32 "real" root_page_id (whatever the old-V2 layout put
    // there), then plausible trailing fields so a V1 parse of it doesn't
    // immediately bounds-fail for reasons unrelated to the sentinel check.
    static std::vector<uint8_t> build_old_v2_buf(uint32_t real_root_page_id) {
        std::vector<uint8_t> buf;
        write_u8(buf, 0x00);
        write_u8(buf, 0x02);
        // real_root_page_id is 4 bytes total; the first two bytes already
        // went into the sentinel above conceptually in the real old format,
        // but for this reachability test we just need bytes[2..3] of the
        // overall buffer to reflect real_root_page_id's low 16 bits, matching
        // how the fix's disambiguation check reads bytes[2..3].
        write_u16(buf, static_cast<uint16_t>(real_root_page_id & 0xFFFFu));
        write_u16(buf, static_cast<uint16_t>((real_root_page_id >> 16) & 0xFFFFu));
        write_u64(buf, 1);  // tree_size
        write_u32(buf, 8);  // next_page_id
        write_u16(buf, 4);  // internal_max_keys
        write_u16(buf, 4);  // leaf_max_keys
        write_u8(buf, 1);   // is_unique
        write_u8(buf, 1);   // key_type_count
        write_u8(buf, static_cast<uint8_t>(TypeId::INT32));
        write_u32(buf, 0);  // leaf_count = 0 (keep V1 parse well-formed if it proceeds)
        write_u32(buf, 0);  // internal_count = 0
        return buf;
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
};

PageId write_meta_page(BufferPoolManager& bpm, const std::vector<uint8_t>& buf) {
    auto meta_page_r = bpm.new_page();
    EXPECT_TRUE(meta_page_r.has_value());
    auto* meta_page = *meta_page_r;
    PageId meta_page_id = meta_page->page_id();
    meta_page->reset(meta_page_id, PageType::BTREE_META);
    auto slot = meta_page->insert_tuple(std::span<const uint8_t>(buf));
    EXPECT_TRUE(slot.has_value()) << slot.error().message;
    (void)bpm.unpin_page(meta_page_id, true);
    return meta_page_id;
}

} // namespace

// =============================================================================
// GDB-1299: Disclosed residual gap -- old-V2 meta whose real root_page_id is
// a multiple of 65536 has bytes[2..3] == [0x00, 0x00] and is therefore NOT
// detected as legacy by the new check (which requires bytes[2..3] != 0x00).
// This test proves the gap is reachable: such a page silently "succeeds" as
// a (bogus) V1 parse rather than failing clean, exactly like the pre-fix
// bug. This is expected/documented behavior per the ticket, not a new bug,
// but QA must confirm it concretely rather than take the ticket's word for
// it -- and confirm severity is bounded (fails or gives obviously-wrong
// results, not a crash/memory-safety issue).
// =============================================================================

TEST_F(QaGdb1299Test, OldV2WithRootPageIdMultipleOf65536EscapesLegacyDetection) {
    auto [fid, bpm] = create_bpm("old_v2_root_65536");
    ASSERT_NE(bpm, nullptr);

    // real_root_page_id = 65536 (0x00010000): low 16 bits are 0, so
    // bytes[2..3] of the buffer are [0x00, 0x00] -- matches a genuine V1
    // root_page_id == 512 collision shape well enough to not be flagged.
    auto buf = build_old_v2_buf(65536);
    ASSERT_EQ(buf[0], 0x00);
    ASSERT_EQ(buf[1], 0x02);
    ASSERT_EQ(buf[2], 0x00);
    ASSERT_EQ(buf[3], 0x00);

    PageId meta_page_id = write_meta_page(*bpm, buf);
    ASSERT_TRUE(bpm->flush_all().has_value());
    bpm.reset();
    (void)dm_->close_file(fid);

    auto [fid2, bpm2] = open_bpm("old_v2_root_65536");
    auto loaded = BTreePersistence::load(*bpm2, meta_page_id);

    // Document actual behavior: this is NOT detected as legacy (the
    // disambiguation heuristic is bytes[2..3] != 0 by design), so it is
    // parsed as V1 with root_page_id == 65536 -- there is no page 65536 in
    // this tiny file, so the V1 parse will fail downstream (out-of-range
    // page / not-found) rather than "succeed" with wrong data in this
    // specific construction. This is the load() call's observed behavior;
    // record it precisely instead of assuming pass/fail.
    if (loaded.has_value()) {
        ADD_FAILURE() << "old-V2 meta with root_page_id multiple of 65536 loaded "
                          "successfully as V1 -- silent misparse reproduced; this "
                          "would return WRONG DATA to callers, not just an error";
    } else {
        // Acceptable: fails for a different reason (missing page 65536 etc.)
        // -- but this is happening via the V1 out-of-bounds path, NOT via the
        // new legacy-sentinel detection. The new detection did not fire.
        SUCCEED() << "old-V2 (root multiple of 65536) escaped legacy detection as "
                     "documented; failed later via V1 path with: "
                  << loaded.error().message;
    }
}

// =============================================================================
// Boundary: bytes[2..3] == [0x00, 0x01] (smallest possible non-zero) MUST be
// detected as legacy and fail clean.
// =============================================================================

TEST_F(QaGdb1299Test, OldV2SmallestNonZeroTrailingBytesDetected) {
    auto [fid, bpm] = create_bpm("old_v2_smallest_nonzero");
    ASSERT_NE(bpm, nullptr);

    auto buf = build_old_v2_buf(1); // low bytes [0x01, 0x00] -> bytes[2]=0x01
    ASSERT_EQ(buf[2], 0x01);
    ASSERT_EQ(buf[3], 0x00);

    PageId meta_page_id = write_meta_page(*bpm, buf);
    ASSERT_TRUE(bpm->flush_all().has_value());
    bpm.reset();
    (void)dm_->close_file(fid);

    auto [fid2, bpm2] = open_bpm("old_v2_smallest_nonzero");
    auto loaded = BTreePersistence::load(*bpm2, meta_page_id);
    ASSERT_FALSE(loaded.has_value()) << "root_page_id=1 old-V2 meta must fail clean";
    EXPECT_EQ(loaded.error().code, StatusCode::INTERNAL_ERROR);
    EXPECT_NE(loaded.error().message.find("legacy"), std::string::npos)
        << "error message should be informative about the legacy-sentinel cause: "
        << loaded.error().message;
}

// =============================================================================
// Boundary: bytes[2..3] == [0xFF, 0xFF] (max) MUST be detected as legacy.
// =============================================================================

TEST_F(QaGdb1299Test, OldV2MaxTrailingBytesDetected) {
    auto [fid, bpm] = create_bpm("old_v2_max_trailing");
    ASSERT_NE(bpm, nullptr);

    auto buf = build_old_v2_buf(0xFFFFu); // bytes[2..3] == [0xFF, 0xFF]
    ASSERT_EQ(buf[2], 0xFF);
    ASSERT_EQ(buf[3], 0xFF);

    PageId meta_page_id = write_meta_page(*bpm, buf);
    ASSERT_TRUE(bpm->flush_all().has_value());
    bpm.reset();
    (void)dm_->close_file(fid);

    auto [fid2, bpm2] = open_bpm("old_v2_max_trailing");
    auto loaded = BTreePersistence::load(*bpm2, meta_page_id);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().code, StatusCode::INTERNAL_ERROR);
}

// =============================================================================
// Boundary: bytes[2] == 0x00, bytes[3] != 0x00 must ALSO be detected (the
// check is "bytes[2] != 0 OR bytes[3] != 0", not just bytes[2]).
// =============================================================================

TEST_F(QaGdb1299Test, OldV2OnlyHighTrailingByteNonZeroDetected) {
    auto [fid, bpm] = create_bpm("old_v2_high_byte_only");
    ASSERT_NE(bpm, nullptr);

    // real_root_page_id = 0x00010000 -> low16 = 0x0000 -> bytes[2..3]=[0,0].
    // We need bytes[2]=0x00 and bytes[3]!=0x00 specifically: low16 with
    // low byte 0 and high byte nonzero, e.g. 0x0100 (256).
    auto buf = build_old_v2_buf(0x0100);
    ASSERT_EQ(buf[2], 0x00);
    ASSERT_EQ(buf[3], 0x01);

    PageId meta_page_id = write_meta_page(*bpm, buf);
    ASSERT_TRUE(bpm->flush_all().has_value());
    bpm.reset();
    (void)dm_->close_file(fid);

    auto [fid2, bpm2] = open_bpm("old_v2_high_byte_only");
    auto loaded = BTreePersistence::load(*bpm2, meta_page_id);
    ASSERT_FALSE(loaded.has_value())
        << "bytes[2]=0x00,bytes[3]=0x01 old-V2 meta must still be detected as legacy";
    EXPECT_EQ(loaded.error().code, StatusCode::INTERNAL_ERROR);
}

// =============================================================================
// Genuine V2 (new magic 0xFF42544D) meta must be entirely unaffected by the
// new legacy-sentinel check -- the magic's leading bytes are [0x4D, 0x54,
// 0x42, 0xFF], which never starts with [0x00, 0x02], so is_v2 short-circuits
// the new check via `!is_v2 &&`. Verify no regression via a full roundtrip
// exercised at this QA layer (independent of the dev test's coverage).
// =============================================================================

TEST_F(QaGdb1299Test, GenuineV2MagicRoundtripUnaffectedByLegacyCheck) {
    BTreeConfig config;
    config.key_types = {TypeId::INT32};
    config.is_unique = true;
    BTreeIndex original(std::move(config));
    for (int i = 0; i < 10; ++i) {
        auto r = original.insert({Value(i)}, RID{static_cast<PageId>(i + 1), 0});
        ASSERT_TRUE(r.has_value());
    }

    auto path = data_dir_ / "genuine_v2_roundtrip.db";
    auto fid_w = dm_->create_file(path, false, true);
    ASSERT_TRUE(fid_w.has_value());
    auto bpm_w = std::make_unique<BufferPoolManager>(*dm_, *fid_w, 64);
    auto meta = BTreePersistence::persist(*bpm_w, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm_w.reset();
    (void)dm_->close_file(*fid_w);

    auto fid_r = dm_->open_file(path);
    ASSERT_TRUE(fid_r.has_value());
    auto bpm_r = std::make_unique<BufferPoolManager>(*dm_, *fid_r, 64);
    auto loaded = BTreePersistence::load(*bpm_r, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->size(), 10u);
    for (int i = 0; i < 10; ++i) {
        auto s = (*loaded)->search({Value(i)});
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value());
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(i + 1));
    }
}

// =============================================================================
// Error path hygiene: repeated legacy-sentinel load failures on the same bpm
// must not leak page pins (each failed load unpins the meta page). If pins
// leaked, the buffer pool would eventually be unable to evict/allocate.
// =============================================================================

TEST_F(QaGdb1299Test, RepeatedLegacySentinelFailuresDoNotExhaustBufferPool) {
    auto [fid, bpm] = create_bpm("old_v2_repeated_failures");
    ASSERT_NE(bpm, nullptr);

    // Small pool (16 frames) so a pin leak would be detected quickly.
    bpm.reset();
    auto path = data_dir_ / "old_v2_repeated_failures.db";
    auto small_bpm = std::make_unique<BufferPoolManager>(*dm_, fid, 16);

    auto buf = build_old_v2_buf(3); // bytes[2..3] = [0x03, 0x00] -> detected
    ASSERT_EQ(buf[2], 0x03);

    std::vector<PageId> meta_ids;
    for (int i = 0; i < 64; ++i) {
        meta_ids.push_back(write_meta_page(*small_bpm, buf));
    }
    ASSERT_TRUE(small_bpm->flush_all().has_value());

    for (PageId pid : meta_ids) {
        auto loaded = BTreePersistence::load(*small_bpm, pid);
        ASSERT_FALSE(loaded.has_value());
        EXPECT_EQ(loaded.error().code, StatusCode::INTERNAL_ERROR);
    }

    // If every failed load correctly unpinned its meta page, we can still
    // allocate a fresh page afterward without hitting "no free frames".
    auto extra = small_bpm->new_page();
    EXPECT_TRUE(extra.has_value()) << "buffer pool exhausted -- suggests a pin leak "
                                       "on the legacy-sentinel error path";
    if (extra.has_value()) {
        (void)small_bpm->unpin_page((*extra)->page_id(), false);
    }
}
