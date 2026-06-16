#include "sixseven/index/bm25_index.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <memory>

using namespace sixseven;

namespace {

RID rid(uint32_t page, uint16_t slot) {
    return RID{page, slot};
}

} // namespace

class Bm25IndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_bm25_index";
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
        if (!fid.has_value()) {
            return {FileId{}, nullptr};
        }
        return {*fid, std::make_unique<BufferPoolManager>(*dm_, *fid, 256)};
    }

    std::pair<FileId, std::unique_ptr<BufferPoolManager>> open_bpm(const std::string& name) {
        auto path = data_dir_ / (name + ".db");
        auto fid = dm_->open_file(path);
        EXPECT_TRUE(fid.has_value());
        if (!fid.has_value()) {
            return {FileId{}, nullptr};
        }
        return {*fid, std::make_unique<BufferPoolManager>(*dm_, *fid, 256)};
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
};

// ===================================================================
// In-memory behavior
// ===================================================================

TEST_F(Bm25IndexTest, EmptySearchReturnsNothing) {
    Bm25Index idx;
    idx.create({});
    EXPECT_TRUE(idx.search("anything", 10).empty());
    EXPECT_EQ(idx.doc_count(), 0u);
}

TEST_F(Bm25IndexTest, BasicRankingByRelevance) {
    Bm25Index idx;
    idx.create({});
    ASSERT_TRUE(idx.add_document(rid(1, 0), "the quick brown fox").has_value());
    ASSERT_TRUE(idx.add_document(rid(1, 1), "a lazy dog sleeps").has_value());
    ASSERT_TRUE(idx.add_document(rid(1, 2), "quick quick quick rabbit").has_value());

    auto hits = idx.search("quick", 10);
    ASSERT_FALSE(hits.empty());
    // Doc with three "quick" occurrences ranks above the single-occurrence doc.
    EXPECT_EQ(hits.front().rid, rid(1, 2));
    // The lazy-dog doc has no "quick" and must not appear.
    for (const auto& h : hits) {
        EXPECT_NE(h.rid, rid(1, 1));
    }
}

TEST_F(Bm25IndexTest, StemmingMatchesInflections) {
    Bm25Index idx;
    idx.create({}); // default analyzer: stemming on.
    ASSERT_TRUE(idx.add_document(rid(2, 0), "machines are learning quickly").has_value());
    // Query "learn"/"machine" should match via Porter stemming.
    auto hits = idx.search("learning machine", 10);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits.front().rid, rid(2, 0));
}

TEST_F(Bm25IndexTest, TopKLimit) {
    Bm25Index idx;
    idx.create({});
    for (uint16_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(idx.add_document(rid(3, i), "common term here").has_value());
    }
    EXPECT_EQ(idx.search("term", 3).size(), 3u);
    EXPECT_EQ(idx.search("term", 0).size(), 5u); // k=0 returns all.
}

TEST_F(Bm25IndexTest, RemoveDocument) {
    Bm25Index idx;
    idx.create({});
    ASSERT_TRUE(idx.add_document(rid(4, 0), "apple banana").has_value());
    ASSERT_TRUE(idx.add_document(rid(4, 1), "apple cherry").has_value());
    EXPECT_EQ(idx.search("apple", 10).size(), 2u);

    ASSERT_TRUE(idx.remove_document(rid(4, 0)).has_value());
    auto hits = idx.search("apple", 10);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits.front().rid, rid(4, 1));
    EXPECT_EQ(idx.doc_count(), 1u);

    // banana belonged only to the removed doc.
    EXPECT_TRUE(idx.search("banana", 10).empty());
}

TEST_F(Bm25IndexTest, AddDocumentReplacesExisting) {
    Bm25Index idx;
    idx.create({});
    ASSERT_TRUE(idx.add_document(rid(5, 0), "original content").has_value());
    ASSERT_TRUE(idx.add_document(rid(5, 0), "replacement words").has_value());
    EXPECT_EQ(idx.doc_count(), 1u);
    EXPECT_TRUE(idx.search("original", 10).empty());
    EXPECT_EQ(idx.search("replacement", 10).size(), 1u);
}

// ===================================================================
// Persistence round-trip
// ===================================================================

TEST_F(Bm25IndexTest, EmptyRoundtrip) {
    Bm25Index idx;
    idx.create({});

    auto [fid1, bpm1] = create_bpm("empty");
    auto meta = Bm25Index::persist(*bpm1, idx);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("empty");
    auto loaded = Bm25Index::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->doc_count(), 0u);
    EXPECT_TRUE((*loaded)->search("x", 10).empty());
}

TEST_F(Bm25IndexTest, PopulatedRoundtripPreservesRanking) {
    Bm25Index idx;
    Bm25Config cfg;
    cfg.k1 = 1.5;
    cfg.b = 0.8;
    idx.create(cfg);
    ASSERT_TRUE(idx.add_document(rid(1, 0), "the quick brown fox jumps").has_value());
    ASSERT_TRUE(idx.add_document(rid(1, 1), "quick quick brown bear").has_value());
    ASSERT_TRUE(idx.add_document(rid(1, 2), "slow green turtle").has_value());

    auto before = idx.search("quick brown", 10);

    auto [fid1, bpm1] = create_bpm("pop");
    auto meta = Bm25Index::persist(*bpm1, idx);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("pop");
    auto loaded = Bm25Index::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    EXPECT_EQ((*loaded)->doc_count(), 3u);
    EXPECT_DOUBLE_EQ((*loaded)->config().k1, 1.5);
    EXPECT_DOUBLE_EQ((*loaded)->config().b, 0.8);

    auto after = (*loaded)->search("quick brown", 10);
    ASSERT_EQ(before.size(), after.size());
    for (size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(before[i].rid, after[i].rid);
        EXPECT_FLOAT_EQ(before[i].score, after[i].score);
    }
}

TEST_F(Bm25IndexTest, RoundtripSupportsMaintenanceAfterLoad) {
    Bm25Index idx;
    idx.create({});
    ASSERT_TRUE(idx.add_document(rid(7, 0), "alpha beta").has_value());

    auto [fid1, bpm1] = create_bpm("maint");
    auto meta = Bm25Index::persist(*bpm1, idx);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("maint");
    auto loaded = Bm25Index::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    // Removing the loaded doc must clear its postings (doc_terms_ rebuilt).
    ASSERT_TRUE((*loaded)->remove_document(rid(7, 0)).has_value());
    EXPECT_EQ((*loaded)->doc_count(), 0u);
    EXPECT_TRUE((*loaded)->search("alpha", 10).empty());
}

TEST_F(Bm25IndexTest, MultiPageRoundtrip) {
    // Enough distinct terms to spill across many data pages.
    Bm25Index idx;
    Bm25AnalyzerConfig acfg;
    acfg.remove_stopwords = false;
    acfg.stem = false;
    Bm25Config cfg;
    cfg.analyzer = acfg;
    idx.create(cfg);

    // Tokens are alphanumeric only (no separators) so each "t<d>x<w>" stays a
    // single unique term; "shared" is common to every document.
    for (uint16_t d = 0; d < 200; ++d) {
        std::string text;
        for (int w = 0; w < 20; ++w) {
            text += "t" + std::to_string(d) + "x" + std::to_string(w) + " ";
        }
        text += "shared";
        ASSERT_TRUE(idx.add_document(rid(9, d), text).has_value());
    }

    auto [fid1, bpm1] = create_bpm("multipage");
    auto meta = Bm25Index::persist(*bpm1, idx);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("multipage");
    auto loaded = Bm25Index::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    EXPECT_EQ((*loaded)->doc_count(), 200u);
    EXPECT_EQ((*loaded)->search("shared", 0).size(), 200u);
    EXPECT_EQ((*loaded)->search("t5x3", 10).size(), 1u);
}

// ===================================================================
// GDB-841: term-less doc survives persist→reload and is fully removable
// ===================================================================

// Helper: build a config whose analyzer strips ALL tokens so any text becomes
// term-less.  We do this by setting min_token_length to a very high value so
// normal words are all filtered out.
static Bm25Config termless_doc_config() {
    Bm25Config cfg;
    // Disable stop-word removal and stemming so the only filter is min length.
    cfg.analyzer.remove_stopwords = false;
    cfg.analyzer.stem = false;
    cfg.analyzer.lowercase = true;
    // Any token shorter than 100 characters is discarded → every realistic word
    // produces zero indexed terms.
    cfg.analyzer.min_token_length = 100;
    return cfg;
}

// GDB-841 regression: after persist→reload, doc_terms_ must contain an entry
// for a term-less document.  Without the fix, the entry is absent and
// remove_document() silently skips the doc.
TEST_F(Bm25IndexTest, GDB841_TermlessDocDocTermsReconstructedAfterReload) {
    Bm25Config cfg = termless_doc_config();
    Bm25Index idx;
    idx.create(cfg);

    // rid(10,0) is term-less (all tokens < 100 chars are discarded).
    // rid(10,1) has a real term so we can verify ordinary docs still work.
    ASSERT_TRUE(idx.add_document(rid(10, 0), "hello world").has_value()); // term-less
    ASSERT_TRUE(idx.add_document(rid(10, 1),
                                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1")
                    .has_value()); // term-ful (101 chars)
    EXPECT_EQ(idx.doc_count(), 2u);

    auto [fid1, bpm1] = create_bpm("gdb841_docterms");
    auto meta = Bm25Index::persist(*bpm1, idx);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("gdb841_docterms");
    auto loaded = Bm25Index::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    // Both documents must be present in doc_terms_ (doc_count uses doc_lengths_,
    // which is always rebuilt; doc_terms_ is the one that was missing before fix).
    EXPECT_EQ((*loaded)->doc_count(), 2u);

    // The term-less doc must be removable (not a no-op).
    ASSERT_TRUE((*loaded)->remove_document(rid(10, 0)).has_value());
    EXPECT_EQ((*loaded)->doc_count(), 1u);

    // The term-ful doc must still be present.
    EXPECT_EQ((*loaded)->doc_count(), 1u);
}

// GDB-841 regression: removing a term-less doc after reload must actually
// decrement the document count and correct avgdl / N used in BM25 scoring.
// The independently-derived expected score is computed using N=1 (only the
// term-ful doc remains) and verified against the actual search result.
TEST_F(Bm25IndexTest, GDB841_RemoveTermlessDocDecrementsBm25Stats) {
    // Use default analyzer so rid(11,1) actually has real postings.
    Bm25Config cfg;
    cfg.k1 = 1.2;
    cfg.b = 0.75;
    // analyzer defaults: lowercase=true, remove_stopwords=true, stem=true,
    // min_token_length=2 (standard English analyzer)
    Bm25Index idx;
    idx.create(cfg);

    // "is an a" are all English stopwords → zero indexed terms → term-less doc.
    ASSERT_TRUE(idx.add_document(rid(11, 0), "is an a").has_value());
    // "database" → real term.
    ASSERT_TRUE(idx.add_document(rid(11, 1), "database").has_value());
    EXPECT_EQ(idx.doc_count(), 2u);

    auto [fid1, bpm1] = create_bpm("gdb841_stats");
    auto meta = Bm25Index::persist(*bpm1, idx);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("gdb841_stats");
    auto loaded = Bm25Index::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->doc_count(), 2u);

    // Remove the term-less doc; this must be a real removal, not a no-op.
    ASSERT_TRUE((*loaded)->remove_document(rid(11, 0)).has_value());
    EXPECT_EQ((*loaded)->doc_count(), 1u);

    // After removal N=1, df=1, dl=1 (one term), avgdl=1.
    // BM25 score = idf * (tf*(k1+1)) / (tf + k1*(1-b+b*dl/avgdl))
    //   idf  = ln(1 + (1 - 1 + 0.5)/(1 + 0.5)) = ln(1 + 0.5/1.5) = ln(1.333...)
    //   tf   = 1
    //   denom= 1 + 1.2*(1 - 0.75 + 0.75*1) = 1 + 1.2*1 = 2.2
    //   score= ln(4/3) * (1*2.2) / 2.2 = ln(4/3) ≈ 0.28768
    const double expected_score = std::log(1.0 + 0.5 / 1.5);

    auto hits = (*loaded)->search("database", 10);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].rid, rid(11, 1));
    EXPECT_NEAR(static_cast<double>(hits[0].score), expected_score, 1e-4);
}

// GDB-841 regression: without the fix, a term-less doc is never removed from
// doc_lengths_, so doc_count() stays 2 and avgdl is inflated, corrupting BM25
// scores even when the caller explicitly deletes the doc.
TEST_F(Bm25IndexTest, GDB841_PhantomTermlessDocNotCountedAfterRemoval) {
    Bm25Config cfg = termless_doc_config();
    Bm25Index idx;
    idx.create(cfg);

    ASSERT_TRUE(idx.add_document(rid(12, 0), "short words here").has_value()); // term-less
    EXPECT_EQ(idx.doc_count(), 1u);

    auto [fid1, bpm1] = create_bpm("gdb841_phantom");
    auto meta = Bm25Index::persist(*bpm1, idx);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("gdb841_phantom");
    auto loaded = Bm25Index::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    // Pre-condition: doc_count must be 1 immediately after reload.
    EXPECT_EQ((*loaded)->doc_count(), 1u);

    // Remove: must go from 1 → 0, not be a no-op.
    ASSERT_TRUE((*loaded)->remove_document(rid(12, 0)).has_value());
    EXPECT_EQ((*loaded)->doc_count(), 0u);

    // Index is now empty; avg_doc_length must also be 0.
    EXPECT_DOUBLE_EQ((*loaded)->avg_doc_length(), 0.0);
}
