#include "sixseven/index/bm25_index.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <span>
#include <unordered_map>

namespace sixseven {

namespace {

constexpr uint32_t kBm25Magic = 0x424D3235; // "BM25"
constexpr uint32_t kBm25Version = 1;

// Record tags for data-page tuples.
constexpr uint8_t kRecordDoc = 0;
constexpr uint8_t kRecordPosting = 1;

// --- Little-endian byte packing helpers -------------------------------------

void put_u8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}

void put_u16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void put_u32(std::vector<uint8_t>& buf, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        buf.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

void put_u64(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

void put_double(std::vector<uint8_t>& buf, double v) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u64(buf, bits);
}

void put_bytes(std::vector<uint8_t>& buf, const std::string& s) {
    buf.insert(buf.end(), s.begin(), s.end());
}

/// Cursor-based reader over a byte span. Tracks an offset and an `ok` flag that
/// trips on any out-of-bounds read so callers can validate after the fact.
struct Reader {
    std::span<const uint8_t> data;
    size_t pos = 0;
    bool ok = true;

    bool ensure(size_t n) {
        if (pos + n > data.size()) {
            ok = false;
            return false;
        }
        return true;
    }

    uint8_t u8() {
        if (!ensure(1)) {
            return 0;
        }
        return data[pos++];
    }

    uint16_t u16() {
        if (!ensure(2)) {
            return 0;
        }
        uint16_t v = static_cast<uint16_t>(data[pos]) | (static_cast<uint16_t>(data[pos + 1]) << 8);
        pos += 2;
        return v;
    }

    uint32_t u32() {
        if (!ensure(4)) {
            return 0;
        }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<uint32_t>(data[pos + static_cast<size_t>(i)]) << (8 * i);
        }
        pos += 4;
        return v;
    }

    uint64_t u64() {
        if (!ensure(8)) {
            return 0;
        }
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<uint64_t>(data[pos + static_cast<size_t>(i)]) << (8 * i);
        }
        pos += 8;
        return v;
    }

    double dbl() {
        uint64_t bits = u64();
        double v = 0.0;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }

    std::string str(size_t n) {
        if (!ensure(n)) {
            return {};
        }
        std::string s(reinterpret_cast<const char*>(data.data() + pos), n);
        pos += n;
        return s;
    }
};

} // namespace

void Bm25Index::create(const Bm25Config& config) {
    std::unique_lock lk(latch_);
    config_ = config;
    analyzer_ = Bm25Analyzer(config.analyzer);
    postings_.clear();
    doc_lengths_.clear();
    doc_terms_.clear();
    total_doc_len_ = 0;
}

uint32_t Bm25Index::doc_count() const {
    std::shared_lock lk(latch_);
    return static_cast<uint32_t>(doc_lengths_.size());
}

double Bm25Index::avg_doc_length() const {
    std::shared_lock lk(latch_);
    if (doc_lengths_.empty()) {
        return 0.0;
    }
    return static_cast<double>(total_doc_len_) / static_cast<double>(doc_lengths_.size());
}

Result<void> Bm25Index::add_document(RID rid, const std::string& text) {
    std::unique_lock lk(latch_);

    // Replace an existing document so UPDATE is idempotent.
    if (auto it = doc_terms_.find(rid); it != doc_terms_.end()) {
        for (const auto& term : it->second) {
            auto pit = postings_.find(term);
            if (pit != postings_.end()) {
                auto& list = pit->second;
                std::erase_if(list, [&](const Posting& p) { return p.rid == rid; });
                if (list.empty()) {
                    postings_.erase(pit);
                }
            }
        }
        total_doc_len_ -= doc_lengths_[rid];
        doc_lengths_.erase(rid);
        doc_terms_.erase(it);
    }

    auto terms = analyzer_.analyze(text);

    // Aggregate term frequencies within this document.
    std::unordered_map<std::string, uint32_t> tf;
    tf.reserve(terms.size());
    for (auto& t : terms) {
        ++tf[t];
    }

    std::vector<std::string> distinct;
    distinct.reserve(tf.size());
    for (auto& [term, freq] : tf) {
        postings_[term].push_back(Posting{rid, freq});
        distinct.push_back(term);
    }

    doc_lengths_[rid] = static_cast<uint32_t>(terms.size());
    total_doc_len_ += terms.size();
    doc_terms_[rid] = std::move(distinct);
    return ok();
}

Result<void> Bm25Index::remove_document(RID rid) {
    std::unique_lock lk(latch_);
    auto it = doc_terms_.find(rid);
    if (it == doc_terms_.end()) {
        return ok(); // not present: no-op.
    }
    for (const auto& term : it->second) {
        auto pit = postings_.find(term);
        if (pit != postings_.end()) {
            auto& list = pit->second;
            std::erase_if(list, [&](const Posting& p) { return p.rid == rid; });
            if (list.empty()) {
                postings_.erase(pit);
            }
        }
    }
    total_doc_len_ -= doc_lengths_[rid];
    doc_lengths_.erase(rid);
    doc_terms_.erase(it);
    return ok();
}

std::vector<Bm25Hit> Bm25Index::search(const std::string& query, uint32_t k) const {
    // analyze() is const and uses no shared state; safe without the latch.
    auto terms = analyzer_.analyze(query);
    return search_terms(terms, k);
}

std::vector<Bm25Hit> Bm25Index::search_terms(const std::vector<std::string>& terms,
                                             uint32_t k) const {
    return score(terms, k);
}

std::vector<Bm25Hit> Bm25Index::score(const std::vector<std::string>& terms, uint32_t k) const {
    std::shared_lock lk(latch_);

    const size_t n = doc_lengths_.size();
    if (n == 0 || terms.empty()) {
        return {};
    }
    const double avgdl = static_cast<double>(total_doc_len_) / static_cast<double>(n);
    const double k1 = config_.k1;
    const double b = config_.b;

    // Distinct query terms: a repeated query term should not be counted twice.
    std::set<std::string> query_terms(terms.begin(), terms.end());

    std::map<RID, double> scores;
    for (const auto& term : query_terms) {
        auto it = postings_.find(term);
        if (it == postings_.end()) {
            continue;
        }
        const auto& list = it->second;
        const double df = static_cast<double>(list.size());
        // BM25 IDF (always positive form).
        const double idf = std::log(1.0 + (static_cast<double>(n) - df + 0.5) / (df + 0.5));
        for (const auto& p : list) {
            auto dl_it = doc_lengths_.find(p.rid);
            const double dl =
                dl_it != doc_lengths_.end() ? static_cast<double>(dl_it->second) : avgdl;
            const double tf = static_cast<double>(p.tf);
            const double denom = tf + k1 * (1.0 - b + b * (dl / avgdl));
            scores[p.rid] += idf * (tf * (k1 + 1.0)) / denom;
        }
    }

    std::vector<Bm25Hit> hits;
    hits.reserve(scores.size());
    for (const auto& [rid, s] : scores) {
        hits.push_back(Bm25Hit{rid, static_cast<float>(s)});
    }
    // Sort by descending score; tie-break by RID for deterministic ordering.
    std::sort(hits.begin(), hits.end(), [](const Bm25Hit& a, const Bm25Hit& c) {
        if (a.score != c.score) {
            return a.score > c.score;
        }
        return a.rid < c.rid;
    });

    if (k != 0 && hits.size() > k) {
        hits.resize(k);
    }
    return hits;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

Result<PageId> Bm25Index::persist(BufferPoolManager& bpm, const Bm25Index& index) {
    std::shared_lock lk(index.latch_);

    // Allocate the metadata page first (page id captured for the return value).
    auto meta_page = bpm.new_page();
    if (!meta_page) {
        return make_error(meta_page.error().code, meta_page.error().message);
    }
    const PageId meta_page_id = (*meta_page)->page_id();

    // Build the data records: one DOC record per document, then one POSTING
    // record per (term, document) pair.
    std::vector<std::vector<uint8_t>> records;
    records.reserve(index.doc_lengths_.size() + index.postings_.size());

    for (const auto& [rid, dl] : index.doc_lengths_) {
        std::vector<uint8_t> rec;
        put_u8(rec, kRecordDoc);
        put_u32(rec, rid.page_id);
        put_u16(rec, rid.slot_id);
        put_u32(rec, dl);
        records.push_back(std::move(rec));
    }
    for (const auto& [term, list] : index.postings_) {
        for (const auto& p : list) {
            std::vector<uint8_t> rec;
            put_u8(rec, kRecordPosting);
            put_u32(rec, p.rid.page_id);
            put_u16(rec, p.rid.slot_id);
            put_u32(rec, p.tf);
            put_u16(rec, static_cast<uint16_t>(term.size()));
            put_bytes(rec, term);
            records.push_back(std::move(rec));
        }
    }

    // Write records into a chain of data pages.
    PageId first_data_page_id = 0;
    uint32_t data_page_count = 0;
    Page* current = nullptr;
    PageId current_id = 0;

    auto close_current = [&]() -> Result<void> {
        if (current != nullptr) {
            auto un = bpm.unpin_page(current_id, /*is_dirty=*/true);
            if (!un) {
                return un;
            }
            current = nullptr;
        }
        return ok();
    };

    for (const auto& rec : records) {
        if (rec.size() + slot_entry_size > page_size - page_header_size) {
            (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/true);
            (void)close_current();
            return make_error(StatusCode::INVALID_ARGUMENT, "BM25 record exceeds page capacity");
        }
        if (current == nullptr || current->free_space() < rec.size()) {
            auto closed = close_current();
            if (!closed) {
                (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/true);
                return make_error(closed.error().code, closed.error().message);
            }
            auto np = bpm.new_page();
            if (!np) {
                (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/true);
                return make_error(np.error().code, np.error().message);
            }
            current = *np;
            current_id = current->page_id();
            current->set_page_type(PageType::BM25_DATA);
            if (first_data_page_id == 0) {
                first_data_page_id = current_id;
            }
            ++data_page_count;
        }
        auto slot = current->insert_tuple(std::span<const uint8_t>(rec.data(), rec.size()));
        if (!slot) {
            (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/true);
            (void)close_current();
            return make_error(slot.error().code, slot.error().message);
        }
    }
    {
        auto closed = close_current();
        if (!closed) {
            (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/true);
            return make_error(closed.error().code, closed.error().message);
        }
    }

    // Serialize and write the metadata tuple.
    std::vector<uint8_t> meta;
    put_u32(meta, kBm25Magic);
    put_u32(meta, kBm25Version);
    put_double(meta, index.config_.k1);
    put_double(meta, index.config_.b);
    put_u8(meta, index.config_.analyzer.lowercase ? 1 : 0);
    put_u8(meta, index.config_.analyzer.remove_stopwords ? 1 : 0);
    put_u8(meta, index.config_.analyzer.stem ? 1 : 0);
    put_u32(meta, index.config_.analyzer.min_token_length);
    put_u32(meta, static_cast<uint32_t>(index.doc_lengths_.size()));
    put_u64(meta, index.total_doc_len_);
    put_u32(meta, first_data_page_id);
    put_u32(meta, data_page_count);
    // Custom stop-word set (empty means "use the default English list").
    put_u32(meta, static_cast<uint32_t>(index.config_.analyzer.stopwords.size()));
    for (const auto& sw : index.config_.analyzer.stopwords) {
        put_u16(meta, static_cast<uint16_t>(sw.size()));
        put_bytes(meta, sw);
    }

    (*meta_page)->set_page_type(PageType::BM25_META);
    auto slot = (*meta_page)->insert_tuple(std::span<const uint8_t>(meta.data(), meta.size()));
    if (!slot) {
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/true);
        return make_error(slot.error().code, "BM25 meta record too large: " + slot.error().message);
    }

    auto un = bpm.unpin_page(meta_page_id, /*is_dirty=*/true);
    if (!un) {
        return make_error(un.error().code, un.error().message);
    }
    auto flush = bpm.flush_all();
    if (!flush) {
        return make_error(flush.error().code, flush.error().message);
    }
    return ok(meta_page_id);
}

Result<std::unique_ptr<Bm25Index>> Bm25Index::load(BufferPoolManager& bpm, PageId meta_page_id) {
    auto meta_page = bpm.fetch_page(meta_page_id);
    if (!meta_page) {
        return make_error(meta_page.error().code, meta_page.error().message);
    }
    auto meta_tuple = (*meta_page)->get_tuple(0);
    if (!meta_tuple) {
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
        return make_error(meta_tuple.error().code, "BM25 meta tuple missing");
    }

    Reader r{std::span<const uint8_t>(meta_tuple->data(), meta_tuple->size())};
    const uint32_t magic = r.u32();
    const uint32_t version = r.u32();
    if (magic != kBm25Magic || version != kBm25Version) {
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
        return make_error(StatusCode::INVALID_ARGUMENT, "BM25 meta page: bad magic/version");
    }

    auto index = std::make_unique<Bm25Index>();
    Bm25Config cfg;
    cfg.k1 = r.dbl();
    cfg.b = r.dbl();
    cfg.analyzer.lowercase = r.u8() != 0;
    cfg.analyzer.remove_stopwords = r.u8() != 0;
    cfg.analyzer.stem = r.u8() != 0;
    cfg.analyzer.min_token_length = r.u32();
    const uint32_t doc_count = r.u32();
    const uint64_t total_doc_len = r.u64();
    const PageId first_data_page_id = r.u32();
    const uint32_t data_page_count = r.u32();
    const uint32_t stopword_count = r.u32();
    for (uint32_t i = 0; i < stopword_count; ++i) {
        const uint16_t len = r.u16();
        cfg.analyzer.stopwords.insert(r.str(len));
    }
    if (!r.ok) {
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
        return make_error(StatusCode::INVALID_ARGUMENT, "BM25 meta page truncated");
    }
    (void)doc_count; // doc_count is recomputed from DOC records below.

    index->create(cfg);

    auto un_meta = bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
    if (!un_meta) {
        return make_error(un_meta.error().code, un_meta.error().message);
    }

    // First pass: DOC records (so document lengths exist before postings).
    // Second pass: POSTING records. Records of both kinds are interleaved on
    // the data pages, so we collect postings during the same scan and apply
    // them after — both maps are rebuilt directly without re-analyzing text.
    std::unique_lock lk(index->latch_);
    index->total_doc_len_ = 0;

    struct PendingPosting {
        std::string term;
        RID rid;
        uint32_t tf;
    };
    std::vector<PendingPosting> pending;

    for (uint32_t i = 0; i < data_page_count; ++i) {
        const PageId pid = first_data_page_id + i;
        auto page = bpm.fetch_page(pid);
        if (!page) {
            return make_error(page.error().code, page.error().message);
        }
        const uint16_t slots = (*page)->slot_count();
        for (uint16_t s = 0; s < slots; ++s) {
            if (!(*page)->is_slot_live(s)) {
                continue;
            }
            auto tuple = (*page)->get_tuple(s);
            if (!tuple) {
                continue;
            }
            Reader rr{std::span<const uint8_t>(tuple->data(), tuple->size())};
            const uint8_t tag = rr.u8();
            if (tag == kRecordDoc) {
                RID rid;
                rid.page_id = rr.u32();
                rid.slot_id = rr.u16();
                const uint32_t dl = rr.u32();
                if (rr.ok) {
                    index->doc_lengths_[rid] = dl;
                    index->total_doc_len_ += dl;
                }
            } else if (tag == kRecordPosting) {
                RID rid;
                rid.page_id = rr.u32();
                rid.slot_id = rr.u16();
                const uint32_t tf = rr.u32();
                const uint16_t term_len = rr.u16();
                std::string term = rr.str(term_len);
                if (rr.ok) {
                    pending.push_back(PendingPosting{std::move(term), rid, tf});
                }
            }
        }
        auto un = bpm.unpin_page(pid, /*is_dirty=*/false);
        if (!un) {
            return make_error(un.error().code, un.error().message);
        }
    }

    for (auto& pp : pending) {
        index->postings_[pp.term].push_back(Posting{pp.rid, pp.tf});
        index->doc_terms_[pp.rid].push_back(pp.term);
    }

    // Ensure every document that was persisted has a doc_terms_ entry, even if
    // it contributed zero terms (e.g. empty/all-stopword text).  Without this,
    // remove_document() would find no doc_terms_ entry for a term-less doc and
    // return early as a no-op, leaving doc_lengths_ / total_doc_len_ stale and
    // inflating N (the corpus document count) permanently after a reload.
    for (const auto& [doc_rid, unused_dl] : index->doc_lengths_) {
        index->doc_terms_.try_emplace(doc_rid); // inserts empty vector if absent
    }

    (void)total_doc_len; // recomputed from DOC records for robustness.
    return ok(std::move(index));
}

} // namespace sixseven
