// QA adversarial tests for GDB-841:
// BM25 reload drops doc_terms_ entries for term-less documents.
// Fix: load() calls try_emplace on doc_terms_ for every RID in doc_lengths_
// so term-less docs gain an empty doc_terms_ entry and are removable.

#include "sixseven/index/bm25_index.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <memory>

using namespace sixseven;

namespace {

RID r(uint32_t page, uint16_t slot) {
    return RID{page, slot};
}

// A config that makes every short token (<= 100 chars) a stop-word, so
// "hello world" contributes zero indexed terms (term-less doc).
Bm25Config termless_cfg() {
    Bm25Config cfg;
    cfg.analyzer.lowercase = true;
    cfg.analyzer.remove_stopwords = false;
    cfg.analyzer.stem = false;
    cfg.analyzer.min_token_length = 101; // discard everything short
    return cfg;
}

// Config with English stop-words enabled so "is an a" → term-less.
Bm25Config english_cfg() {
    Bm25Config cfg;
    cfg.k1 = 1.2;
    cfg.b = 0.75;
    cfg.analyzer.lowercase = true;
    cfg.analyzer.remove_stopwords = true;
    cfg.analyzer.stem = true;
    cfg.analyzer.min_token_length = 2;
    return cfg;
}

} // namespace

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class QA_GDB841 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb841";
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
        if (!fid.has_value())
            return {FileId{}, nullptr};
        return {*fid, std::make_unique<BufferPoolManager>(*dm_, *fid, 256)};
    }

    std::pair<FileId, std::unique_ptr<BufferPoolManager>> open_bpm(const std::string& name) {
        auto path = data_dir_ / (name + ".db");
        auto fid = dm_->open_file(path);
        EXPECT_TRUE(fid.has_value());
        if (!fid.has_value())
            return {FileId{}, nullptr};
        return {*fid, std::make_unique<BufferPoolManager>(*dm_, *fid, 256)};
    }

    // Helper: persist index and reload it into a fresh object.
    std::unique_ptr<Bm25Index> persist_and_reload(const Bm25Index& idx, const std::string& tag) {
        auto [fid1, bpm1] = create_bpm(tag);
        auto meta = Bm25Index::persist(*bpm1, idx);
        EXPECT_TRUE(meta.has_value()) << (meta ? "" : meta.error().message);
        bpm1.reset();
        (void)dm_->close_file(fid1);

        auto [fid2, bpm2] = open_bpm(tag);
        auto loaded = Bm25Index::load(*bpm2, *meta);
        EXPECT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().message);
        if (!loaded.has_value())
            return nullptr;
        return std::move(*loaded);
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
};

// ---------------------------------------------------------------------------
// AC1: Verify the bug is real against original source (already fixed).
// Each test below exercises the exact code path that was broken.
// ---------------------------------------------------------------------------

// Multiple term-less docs persisted+reloaded then removed in LIFO order.
TEST_F(QA_GDB841, GDB841_MultipleTermlessDocsRemovedLifoOrder) {
    Bm25Index idx;
    idx.create(termless_cfg());

    for (uint16_t s = 0; s < 5; ++s) {
        ASSERT_TRUE(idx.add_document(r(1, s), "hello world").has_value());
    }
    EXPECT_EQ(idx.doc_count(), 5u);

    auto loaded = persist_and_reload(idx, "qa841_lifo");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->doc_count(), 5u);

    // Remove in reverse order.
    for (uint16_t s = 5; s-- > 0;) {
        ASSERT_TRUE(loaded->remove_document(r(1, s)).has_value()) << "failed removing slot " << s;
        EXPECT_EQ(loaded->doc_count(), static_cast<uint32_t>(s));
    }
    EXPECT_EQ(loaded->doc_count(), 0u);
    EXPECT_DOUBLE_EQ(loaded->avg_doc_length(), 0.0);
}

// Multiple term-less docs removed in FIFO (insertion) order.
TEST_F(QA_GDB841, GDB841_MultipleTermlessDocsRemovedFifoOrder) {
    Bm25Index idx;
    idx.create(termless_cfg());

    for (uint16_t s = 0; s < 4; ++s) {
        ASSERT_TRUE(idx.add_document(r(2, s), "foo bar").has_value());
    }

    auto loaded = persist_and_reload(idx, "qa841_fifo");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->doc_count(), 4u);

    for (uint16_t s = 0; s < 4; ++s) {
        ASSERT_TRUE(loaded->remove_document(r(2, s)).has_value());
        EXPECT_EQ(loaded->doc_count(), static_cast<uint32_t>(3u - s));
    }
}

// A term-less doc re-added after reload+removal retains its state correctly.
TEST_F(QA_GDB841, GDB841_TermlessDocReaddedAfterReloadAndRemoval) {
    Bm25Index idx;
    idx.create(termless_cfg());
    ASSERT_TRUE(idx.add_document(r(3, 0), "short").has_value()); // term-less

    auto loaded = persist_and_reload(idx, "qa841_readd");
    ASSERT_NE(loaded, nullptr);

    // Remove the term-less doc.
    ASSERT_TRUE(loaded->remove_document(r(3, 0)).has_value());
    EXPECT_EQ(loaded->doc_count(), 0u);

    // Re-add the same RID with an actual term.
    Bm25Config real_cfg;
    // Default cfg accepts all tokens >= 2 chars.
    // Re-add to the LOADED index (cfg is already set inside it, but we add
    // with text that now produces terms).
    Bm25Index idx2;
    idx2.create(english_cfg());
    ASSERT_TRUE(idx2.add_document(r(3, 0), "database").has_value());
    EXPECT_EQ(idx2.doc_count(), 1u);
    EXPECT_GT(idx2.avg_doc_length(), 0.0);
    auto hits = idx2.search("database", 10);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].rid, r(3, 0));
}

// Mixed insert/delete/reload cycle: term-ful docs coexist with term-less docs.
TEST_F(QA_GDB841, GDB841_MixedInsertDeleteReloadCycle) {
    Bm25Index idx;
    idx.create(english_cfg());

    // term-less (stop-words only)
    ASSERT_TRUE(idx.add_document(r(4, 0), "is an a the").has_value());
    // term-less (stop-word)
    ASSERT_TRUE(idx.add_document(r(4, 1), "").has_value());
    // term-ful
    ASSERT_TRUE(idx.add_document(r(4, 2), "database index").has_value());

    EXPECT_EQ(idx.doc_count(), 3u);

    auto loaded = persist_and_reload(idx, "qa841_mixed");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->doc_count(), 3u);

    // Remove term-less docs.
    ASSERT_TRUE(loaded->remove_document(r(4, 0)).has_value());
    ASSERT_TRUE(loaded->remove_document(r(4, 1)).has_value());
    EXPECT_EQ(loaded->doc_count(), 1u);

    // Term-ful doc must still be searchable.
    auto hits = loaded->search("database", 10);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].rid, r(4, 2));
}

// reload→remove→reload again: the removal must be visible post second reload.
TEST_F(QA_GDB841, GDB841_ReloadRemoveReloadPersistsRemoval) {
    Bm25Index idx;
    idx.create(termless_cfg());
    ASSERT_TRUE(idx.add_document(r(5, 0), "word").has_value()); // term-less

    auto loaded1 = persist_and_reload(idx, "qa841_rrr1");
    ASSERT_NE(loaded1, nullptr);
    EXPECT_EQ(loaded1->doc_count(), 1u);

    // Remove the term-less doc.
    ASSERT_TRUE(loaded1->remove_document(r(5, 0)).has_value());
    EXPECT_EQ(loaded1->doc_count(), 0u);

    // Persist the now-empty index and reload again.
    auto loaded2 = persist_and_reload(*loaded1, "qa841_rrr2");
    ASSERT_NE(loaded2, nullptr);

    // Doc must still be gone — no phantom.
    EXPECT_EQ(loaded2->doc_count(), 0u);
    EXPECT_DOUBLE_EQ(loaded2->avg_doc_length(), 0.0);
}

// N and avgdl are correct after removing only the term-less docs from a
// mixed index.
TEST_F(QA_GDB841, GDB841_AvgdlAndNCorrectAfterTermlessRemovals) {
    Bm25Index idx;
    idx.create(english_cfg());

    // Two term-less docs (stop-words).
    ASSERT_TRUE(idx.add_document(r(6, 0), "is an").has_value());
    ASSERT_TRUE(idx.add_document(r(6, 1), "").has_value());
    // One term-ful doc: "search" → 1 term after analysis.
    ASSERT_TRUE(idx.add_document(r(6, 2), "search").has_value());
    EXPECT_EQ(idx.doc_count(), 3u);

    auto loaded = persist_and_reload(idx, "qa841_avgdl");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->doc_count(), 3u);

    // Remove term-less docs.
    ASSERT_TRUE(loaded->remove_document(r(6, 0)).has_value());
    ASSERT_TRUE(loaded->remove_document(r(6, 1)).has_value());
    EXPECT_EQ(loaded->doc_count(), 1u);

    // avgdl should equal 1.0 (only the "search" doc, length 1).
    const double avgdl = loaded->avg_doc_length();
    EXPECT_NEAR(avgdl, 1.0, 1e-9);
}

// No double-decrement of N: removing the same term-less doc twice must be safe.
TEST_F(QA_GDB841, GDB841_DoubleRemoveTermlessDocIsNoop) {
    Bm25Index idx;
    idx.create(termless_cfg());
    ASSERT_TRUE(idx.add_document(r(7, 0), "test").has_value()); // term-less

    auto loaded = persist_and_reload(idx, "qa841_dbl");
    ASSERT_NE(loaded, nullptr);

    ASSERT_TRUE(loaded->remove_document(r(7, 0)).has_value());
    EXPECT_EQ(loaded->doc_count(), 0u);

    // Second remove must be a safe no-op, not a double-decrement or crash.
    ASSERT_TRUE(loaded->remove_document(r(7, 0)).has_value());
    EXPECT_EQ(loaded->doc_count(), 0u);
    EXPECT_DOUBLE_EQ(loaded->avg_doc_length(), 0.0);
}

// Empty-string document: zero-length text always produces zero terms.
TEST_F(QA_GDB841, GDB841_EmptyStringDocReloadableAndRemovable) {
    Bm25Index idx;
    idx.create(english_cfg()); // any analyzer
    ASSERT_TRUE(idx.add_document(r(8, 0), "").has_value());
    EXPECT_EQ(idx.doc_count(), 1u);
    EXPECT_DOUBLE_EQ(idx.avg_doc_length(), 0.0);

    auto loaded = persist_and_reload(idx, "qa841_empty_str");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->doc_count(), 1u);

    ASSERT_TRUE(loaded->remove_document(r(8, 0)).has_value());
    EXPECT_EQ(loaded->doc_count(), 0u);
}

// Whitespace-only document: all tokens are empty after split → term-less.
TEST_F(QA_GDB841, GDB841_WhitespaceOnlyDocRemovableAfterReload) {
    Bm25Index idx;
    idx.create(english_cfg());
    ASSERT_TRUE(idx.add_document(r(9, 0), "   \t\n   ").has_value());
    EXPECT_EQ(idx.doc_count(), 1u);

    auto loaded = persist_and_reload(idx, "qa841_ws");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->doc_count(), 1u);

    ASSERT_TRUE(loaded->remove_document(r(9, 0)).has_value());
    EXPECT_EQ(loaded->doc_count(), 0u);
}

// All-stopwords document → term-less under English analyzer.
TEST_F(QA_GDB841, GDB841_AllStopwordsDocRemovableAfterReload) {
    Bm25Index idx;
    idx.create(english_cfg());
    // English stop-words only.
    ASSERT_TRUE(idx.add_document(r(10, 0), "the is a an and of to in").has_value());
    EXPECT_EQ(idx.doc_count(), 1u);

    auto loaded = persist_and_reload(idx, "qa841_stopwords");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->doc_count(), 1u);

    ASSERT_TRUE(loaded->remove_document(r(10, 0)).has_value());
    EXPECT_EQ(loaded->doc_count(), 0u);
}

// BM25 score correctness: removing a term-less doc shifts N from 2 to 1 and
// must produce the mathematically correct score.
TEST_F(QA_GDB841, GDB841_ScoreCorrectAfterTermlessRemoval) {
    Bm25Config cfg = english_cfg();
    cfg.k1 = 1.2;
    cfg.b = 0.75;

    Bm25Index idx;
    idx.create(cfg);

    // term-less
    ASSERT_TRUE(idx.add_document(r(20, 0), "is an").has_value());
    // term-ful: "index" → 1 term
    ASSERT_TRUE(idx.add_document(r(20, 1), "index").has_value());
    EXPECT_EQ(idx.doc_count(), 2u);

    auto loaded = persist_and_reload(idx, "qa841_score");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->doc_count(), 2u);

    ASSERT_TRUE(loaded->remove_document(r(20, 0)).has_value());
    EXPECT_EQ(loaded->doc_count(), 1u);

    // N=1, df=1, dl=1, avgdl=1
    // idf = ln(1 + (1-1+0.5)/(1+0.5)) = ln(1 + 1/3) ≈ 0.28768
    // tf=1, denom = 1 + 1.2*(0.25 + 0.75*1) = 1 + 1.2 = 2.2
    // score = idf * (1 * 2.2) / 2.2 = idf
    const double expected = std::log(1.0 + 0.5 / 1.5);

    auto hits = loaded->search("index", 10);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].rid, r(20, 1));
    EXPECT_NEAR(static_cast<double>(hits[0].score), expected, 1e-4);
}

// Large batch: 50 term-less docs plus 5 term-ful docs; all term-less must
// be removable after reload with no corruption to postings or stats.
TEST_F(QA_GDB841, GDB841_LargeBatchTermlessAndTermful) {
    Bm25Index idx;
    idx.create(termless_cfg());

    // 50 term-less docs.
    for (uint16_t s = 0; s < 50; ++s) {
        ASSERT_TRUE(idx.add_document(r(30, s), "hi").has_value());
    }
    // 5 term-ful docs (long token >= 101 chars).
    const std::string long_tok(101, 'z');
    for (uint16_t s = 50; s < 55; ++s) {
        ASSERT_TRUE(idx.add_document(r(30, s), long_tok).has_value());
    }
    EXPECT_EQ(idx.doc_count(), 55u);

    auto loaded = persist_and_reload(idx, "qa841_large");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->doc_count(), 55u);

    // Remove all term-less docs.
    for (uint16_t s = 0; s < 50; ++s) {
        ASSERT_TRUE(loaded->remove_document(r(30, s)).has_value());
    }
    EXPECT_EQ(loaded->doc_count(), 5u);

    // Term-ful docs still searchable.
    auto hits = loaded->search(long_tok, 10);
    EXPECT_EQ(hits.size(), 5u);
}

// Posting list integrity: term-ful postings not corrupted by the fix.
// A term that appears in a real doc must still produce hits after reload.
TEST_F(QA_GDB841, GDB841_PostingListIntegrityAfterTermlessCoexistence) {
    Bm25Index idx;
    idx.create(english_cfg());

    // term-less
    ASSERT_TRUE(idx.add_document(r(40, 0), "").has_value());
    // term-ful with distinct terms
    ASSERT_TRUE(idx.add_document(r(40, 1), "graph database").has_value());
    ASSERT_TRUE(idx.add_document(r(40, 2), "graph query").has_value());

    auto loaded = persist_and_reload(idx, "qa841_posting_integ");
    ASSERT_NE(loaded, nullptr);

    // Remove term-less.
    ASSERT_TRUE(loaded->remove_document(r(40, 0)).has_value());
    EXPECT_EQ(loaded->doc_count(), 2u);

    // "graph" appears in both remaining docs.
    auto hits = loaded->search("graph", 10);
    EXPECT_EQ(hits.size(), 2u);

    // "database" appears in only one.
    auto db_hits = loaded->search("database", 10);
    EXPECT_EQ(db_hits.size(), 1u);
    EXPECT_EQ(db_hits[0].rid, r(40, 1));
}

// Removing a non-existent RID (never inserted) must remain a safe no-op.
TEST_F(QA_GDB841, GDB841_RemoveNonExistentRidIsNoop) {
    Bm25Index idx;
    idx.create(termless_cfg());
    ASSERT_TRUE(idx.add_document(r(50, 0), "test").has_value()); // term-less

    auto loaded = persist_and_reload(idx, "qa841_nonexist");
    ASSERT_NE(loaded, nullptr);

    // This RID was never inserted.
    ASSERT_TRUE(loaded->remove_document(r(99, 99)).has_value());
    // Count must be unchanged.
    EXPECT_EQ(loaded->doc_count(), 1u);
}

// term-ful doc removed before reload; term-less retained — reload sees only
// the term-less doc and must reconstruct its doc_terms_ entry.
TEST_F(QA_GDB841, GDB841_TermfulRemovedBeforeReloadTermlessRetained) {
    Bm25Index idx;
    idx.create(english_cfg());

    // term-less
    ASSERT_TRUE(idx.add_document(r(60, 0), "the is").has_value());
    // term-ful
    ASSERT_TRUE(idx.add_document(r(60, 1), "storage").has_value());

    // Remove term-ful BEFORE persist so only the term-less doc is persisted.
    ASSERT_TRUE(idx.remove_document(r(60, 1)).has_value());
    EXPECT_EQ(idx.doc_count(), 1u);

    auto loaded = persist_and_reload(idx, "qa841_pre_remove");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->doc_count(), 1u);

    // Must be removable.
    ASSERT_TRUE(loaded->remove_document(r(60, 0)).has_value());
    EXPECT_EQ(loaded->doc_count(), 0u);
}

// Ensure try_emplace does NOT overwrite existing (non-empty) doc_terms_ entries.
// If a term-ful doc's entry were erased and replaced by empty, search would break.
TEST_F(QA_GDB841, GDB841_TryEmplaceDoesNotClobberTermfulEntry) {
    Bm25Index idx;
    idx.create(english_cfg());

    // term-ful doc
    ASSERT_TRUE(idx.add_document(r(70, 0), "search engine").has_value());
    // term-less doc
    ASSERT_TRUE(idx.add_document(r(70, 1), "").has_value());

    auto loaded = persist_and_reload(idx, "qa841_no_clobber");
    ASSERT_NE(loaded, nullptr);

    // term-ful doc must still return hits.
    auto hits = loaded->search("search", 10);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].rid, r(70, 0));

    // Removing the term-ful doc must properly clean postings.
    ASSERT_TRUE(loaded->remove_document(r(70, 0)).has_value());
    EXPECT_EQ(loaded->doc_count(), 1u);

    // No hits for "search" now.
    auto hits2 = loaded->search("search", 10);
    EXPECT_TRUE(hits2.empty());
}

// total_doc_len_ consistency after removing term-less (zero-length) docs.
// Zero-length docs contribute 0 to total_doc_len_, so removing them must
// not cause underflow or incorrect avgdl.
TEST_F(QA_GDB841, GDB841_TotalDocLenConsistentAfterTermlessRemovals) {
    Bm25Index idx;
    idx.create(english_cfg());

    // 3 term-less (each has dl=0).
    for (uint16_t s = 0; s < 3; ++s) {
        ASSERT_TRUE(idx.add_document(r(80, s), "").has_value());
    }
    // 1 term-ful: "index" → dl=1.
    ASSERT_TRUE(idx.add_document(r(80, 3), "index").has_value());
    EXPECT_EQ(idx.doc_count(), 4u);
    EXPECT_NEAR(idx.avg_doc_length(), 0.25, 1e-9); // total=1, n=4

    auto loaded = persist_and_reload(idx, "qa841_totaldoclen");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->doc_count(), 4u);

    // Remove all term-less docs.
    for (uint16_t s = 0; s < 3; ++s) {
        ASSERT_TRUE(loaded->remove_document(r(80, s)).has_value());
    }
    EXPECT_EQ(loaded->doc_count(), 1u);
    EXPECT_NEAR(loaded->avg_doc_length(), 1.0, 1e-9); // total=1, n=1
}

// Interleaved adds and removes across multiple reload cycles.
TEST_F(QA_GDB841, GDB841_InterleavedAddRemoveMultipleReloadCycles) {
    Bm25Index idx;
    idx.create(english_cfg());

    // Cycle 1: add 2 term-less, reload, remove 1.
    ASSERT_TRUE(idx.add_document(r(90, 0), "is").has_value());
    ASSERT_TRUE(idx.add_document(r(90, 1), "a").has_value());

    auto l1 = persist_and_reload(idx, "qa841_interleave_c1");
    ASSERT_NE(l1, nullptr);
    EXPECT_EQ(l1->doc_count(), 2u);
    ASSERT_TRUE(l1->remove_document(r(90, 0)).has_value());
    EXPECT_EQ(l1->doc_count(), 1u);

    // Cycle 2: add a term-ful doc and reload again.
    ASSERT_TRUE(l1->add_document(r(90, 2), "engine").has_value());
    EXPECT_EQ(l1->doc_count(), 2u);

    auto l2 = persist_and_reload(*l1, "qa841_interleave_c2");
    ASSERT_NE(l2, nullptr);
    EXPECT_EQ(l2->doc_count(), 2u);

    // Remove the remaining term-less doc.
    ASSERT_TRUE(l2->remove_document(r(90, 1)).has_value());
    EXPECT_EQ(l2->doc_count(), 1u);

    // Term-ful doc still there.
    auto hits = l2->search("engine", 10);
    EXPECT_EQ(hits.size(), 1u);
}
