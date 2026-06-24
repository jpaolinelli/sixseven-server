#pragma once

#include "sixseven/common/result.h"
#include "sixseven/index/bm25_analyzer.h"
#include "sixseven/index/rid.h"
#include "sixseven/storage/buffer_pool.h"

#include <cstdint>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sixseven {

class Bm25Index;

/// Tuning parameters and analyzer configuration for a BM25 index.
///
/// The analyzer config is persisted with the index so query-time analysis
/// exactly matches index-time analysis after a reload.
struct Bm25Config {
    double k1 = 1.2; ///< Term-frequency saturation.
    double b = 0.75; ///< Document-length normalization.
    Bm25AnalyzerConfig analyzer;
};

/// A single scored search hit: the document's heap RID and its BM25 score.
struct Bm25Hit {
    RID rid;
    float score = 0.0F;
};

/// A BM25 index that DML operators must maintain, paired with the ordinal of
/// the indexed text column in the table's storage schema. Populated by the
/// planner for INSERT/UPDATE/DELETE.
struct Bm25MaintenanceTarget {
    Bm25Index* index = nullptr;
    size_t text_column_index = 0;
};

/// Persistent BM25 inverted index over a single text column.
///
/// The live index is held in memory (term -> posting list, plus per-document
/// length and corpus statistics) so query scoring is fast. Durability follows
/// the B+ tree persistence pattern rather than HNSW's live-page navigation:
/// the in-memory state is the source of truth and is serialized to a fresh
/// index file on flush (persist), and reconstructed on load. A missed
/// maintenance event self-heals via the IndexManager's startup rebuild, the
/// same safety net the other secondary indexes rely on.
///
/// Thread-safety: all public operations take an internal shared_mutex (shared
/// for search, exclusive for add/remove).
class Bm25Index {
public:
    Bm25Index() = default;

    Bm25Index(const Bm25Index&) = delete;
    Bm25Index& operator=(const Bm25Index&) = delete;
    Bm25Index(Bm25Index&&) = delete;
    Bm25Index& operator=(Bm25Index&&) = delete;

    // -- Lifecycle -------------------------------------------------------------

    /// Initialize an empty index with the given configuration.
    void create(const Bm25Config& config);

    // -- Maintenance -----------------------------------------------------------

    /// Index a document: analyze `text`, record its term frequencies, document
    /// length, and corpus statistics. If the RID already exists it is replaced
    /// (remove + re-add) so UPDATE is idempotent.
    [[nodiscard]] Result<void> add_document(RID rid, const std::string& text);

    /// Remove a document from the index (DELETE / UPDATE maintenance). No-op if
    /// the RID is not present.
    [[nodiscard]] Result<void> remove_document(RID rid);

    // -- Query -----------------------------------------------------------------

    /// Score documents against the raw query string (analyzed internally) and
    /// return up to `k` hits sorted by descending score. A `k` of 0 returns all
    /// matching documents.
    [[nodiscard]] std::vector<Bm25Hit> search(const std::string& query, uint32_t k) const;

    /// Score documents against already-analyzed query terms.
    [[nodiscard]] std::vector<Bm25Hit> search_terms(const std::vector<std::string>& terms,
                                                    uint32_t k) const;

    // -- Accessors -------------------------------------------------------------

    [[nodiscard]] const Bm25Config& config() const { return config_; }
    [[nodiscard]] const Bm25Analyzer& analyzer() const { return analyzer_; }
    [[nodiscard]] uint32_t doc_count() const;
    [[nodiscard]] double avg_doc_length() const;

    // -- Test-only fault injection ---------------------------------------------

    /// When set to true, the next call to add_document or remove_document
    /// immediately returns an INTERNAL_ERROR instead of mutating state.
    /// This field is intentionally public so test code can set it without
    /// requiring a friendship declaration or a separate test-only subclass.
    /// Production code never sets this flag.
    bool fault_inject_ = false;

    // -- Persistence (mirrors BTreePersistence) --------------------------------

    /// Serialize an index to a freshly created index file via `bpm`.
    /// Returns the metadata page ID (always 1).
    [[nodiscard]] static Result<PageId> persist(BufferPoolManager& bpm, const Bm25Index& index);

    /// Deserialize an index from disk pages via `bpm`.
    [[nodiscard]] static Result<std::unique_ptr<Bm25Index>> load(BufferPoolManager& bpm,
                                                                 PageId meta_page_id);

private:
    struct Posting {
        RID rid;
        uint32_t tf = 0;
    };

    /// Compute BM25 score contributions for the given analyzed terms.
    [[nodiscard]] std::vector<Bm25Hit> score(const std::vector<std::string>& terms,
                                             uint32_t k) const;

    Bm25Config config_;
    Bm25Analyzer analyzer_;

    /// term -> posting list (one entry per document containing the term).
    std::unordered_map<std::string, std::vector<Posting>> postings_;

    /// RID -> document length (token count). Ordered so persistence is stable.
    std::map<RID, uint32_t> doc_lengths_;

    /// RID -> distinct terms in the document, for O(terms) removal.
    std::map<RID, std::vector<std::string>> doc_terms_;

    /// Sum of all document lengths (for average-document-length normalization).
    uint64_t total_doc_len_ = 0;

    mutable std::shared_mutex latch_;
};

} // namespace sixseven
