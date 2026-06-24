#include "sixseven/index/bm25_index.h"

#include "sixseven/index/index_encoding.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <span>
#include <unordered_map>

namespace sixseven {

using namespace index_encoding;

namespace {

constexpr uint32_t kBm25Magic = 0x424D3235; // "BM25"
constexpr uint32_t kBm25Version = 1;

// Record tags for data-page tuples.
constexpr uint8_t kRecordDoc = 0;
constexpr uint8_t kRecordPosting = 1;

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
    if (fault_inject_) {
        return make_error(StatusCode::INTERNAL_ERROR, "BM25 fault injection: add_document");
    }
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
    if (fault_inject_) {
        return make_error(StatusCode::INTERNAL_ERROR, "BM25 fault injection: remove_document");
    }
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
        write_u8(rec, kRecordDoc);
        write_u32(rec, rid.page_id);
        write_u16(rec, rid.slot_id);
        write_u32(rec, dl);
        records.push_back(std::move(rec));
    }
    for (const auto& [term, list] : index.postings_) {
        for (const auto& p : list) {
            std::vector<uint8_t> rec;
            write_u8(rec, kRecordPosting);
            write_u32(rec, p.rid.page_id);
            write_u16(rec, p.rid.slot_id);
            write_u32(rec, p.tf);
            write_u16(rec, static_cast<uint16_t>(term.size()));
            rec.insert(rec.end(), term.begin(), term.end());
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
    write_u32(meta, kBm25Magic);
    write_u32(meta, kBm25Version);
    write_double(meta, index.config_.k1);
    write_double(meta, index.config_.b);
    write_u8(meta, index.config_.analyzer.lowercase ? 1 : 0);
    write_u8(meta, index.config_.analyzer.remove_stopwords ? 1 : 0);
    write_u8(meta, index.config_.analyzer.stem ? 1 : 0);
    write_u32(meta, index.config_.analyzer.min_token_length);
    write_u32(meta, static_cast<uint32_t>(index.doc_lengths_.size()));
    write_u64(meta, index.total_doc_len_);
    write_u32(meta, first_data_page_id);
    write_u32(meta, data_page_count);
    // Custom stop-word set (empty means "use the default English list").
    write_u32(meta, static_cast<uint32_t>(index.config_.analyzer.stopwords.size()));
    for (const auto& sw : index.config_.analyzer.stopwords) {
        write_u16(meta, static_cast<uint16_t>(sw.size()));
        meta.insert(meta.end(), sw.begin(), sw.end());
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

    Reader r(std::span<const uint8_t>(meta_tuple->data(), meta_tuple->size()));

    auto magic_r = r.read_u32();
    if (!magic_r) {
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
        return make_error(magic_r.error().code, "bm25 load: " + magic_r.error().message);
    }
    auto version_r = r.read_u32();
    if (!version_r) {
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
        return make_error(version_r.error().code, "bm25 load: " + version_r.error().message);
    }
    if (*magic_r != kBm25Magic || *version_r != kBm25Version) {
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
        return make_error(StatusCode::INVALID_ARGUMENT, "BM25 meta page: bad magic/version");
    }

    auto index = std::make_unique<Bm25Index>();
    Bm25Config cfg;

    auto k1_r = r.read_double();
    if (!k1_r) {
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
        return make_error(k1_r.error().code, "bm25 load: " + k1_r.error().message);
    }
    cfg.k1 = *k1_r;

    auto b_r = r.read_double();
    if (!b_r) {
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
        return make_error(b_r.error().code, "bm25 load: " + b_r.error().message);
    }
    cfg.b = *b_r;

    auto lowercase_r = r.read_u8();
    if (!lowercase_r) {
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
        return make_error(lowercase_r.error().code, "bm25 load: " + lowercase_r.error().message);
    }
    cfg.analyzer.lowercase = *lowercase_r != 0;

    auto stopwords_flag_r = r.read_u8();
    if (!stopwords_flag_r) {
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
        return make_error(stopwords_flag_r.error().code,
                          "bm25 load: " + stopwords_flag_r.error().message);
    }
    cfg.analyzer.remove_stopwords = *stopwords_flag_r != 0;

    auto stem_r = r.read_u8();
    if (!stem_r) {
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
        return make_error(stem_r.error().code, "bm25 load: " + stem_r.error().message);
    }
    cfg.analyzer.stem = *stem_r != 0;

    auto min_token_r = r.read_u32();
    if (!min_token_r) {
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
        return make_error(min_token_r.error().code, "bm25 load: " + min_token_r.error().message);
    }
    cfg.analyzer.min_token_length = *min_token_r;

    auto doc_count_r = r.read_u32();
    if (!doc_count_r) {
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
        return make_error(doc_count_r.error().code, "bm25 load: " + doc_count_r.error().message);
    }
    const uint32_t doc_count = *doc_count_r;

    auto total_doc_len_r = r.read_u64();
    if (!total_doc_len_r) {
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
        return make_error(total_doc_len_r.error().code,
                          "bm25 load: " + total_doc_len_r.error().message);
    }
    const uint64_t total_doc_len = *total_doc_len_r;

    auto first_data_page_r = r.read_u32();
    if (!first_data_page_r) {
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
        return make_error(first_data_page_r.error().code,
                          "bm25 load: " + first_data_page_r.error().message);
    }
    const PageId first_data_page_id = *first_data_page_r;

    auto data_page_count_r = r.read_u32();
    if (!data_page_count_r) {
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
        return make_error(data_page_count_r.error().code,
                          "bm25 load: " + data_page_count_r.error().message);
    }
    const uint32_t data_page_count = *data_page_count_r;

    auto stopword_count_r = r.read_u32();
    if (!stopword_count_r) {
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
        return make_error(stopword_count_r.error().code,
                          "bm25 load: " + stopword_count_r.error().message);
    }
    const uint32_t stopword_count = *stopword_count_r;

    for (uint32_t i = 0; i < stopword_count; ++i) {
        auto len_r = r.read_u16();
        if (!len_r) {
            (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
            return make_error(len_r.error().code, "bm25 load: " + len_r.error().message);
        }
        auto sw_r = r.read_bytes(*len_r);
        if (!sw_r) {
            (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/false);
            return make_error(sw_r.error().code, "bm25 load: " + sw_r.error().message);
        }
        cfg.analyzer.stopwords.insert(std::move(*sw_r));
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
            Reader rr(std::span<const uint8_t>(tuple->data(), tuple->size()));
            auto tag_r = rr.read_u8();
            if (!tag_r) {
                continue;
            }
            const uint8_t tag = *tag_r;
            if (tag == kRecordDoc) {
                auto page_id_r = rr.read_u32();
                auto slot_id_r = rr.read_u16();
                auto dl_r = rr.read_u32();
                if (!page_id_r || !slot_id_r || !dl_r) {
                    continue;
                }
                RID rid;
                rid.page_id = *page_id_r;
                rid.slot_id = *slot_id_r;
                const uint32_t dl = *dl_r;
                index->doc_lengths_[rid] = dl;
                index->total_doc_len_ += dl;
            } else if (tag == kRecordPosting) {
                auto page_id_r = rr.read_u32();
                auto slot_id_r = rr.read_u16();
                auto tf_r = rr.read_u32();
                auto term_len_r = rr.read_u16();
                if (!page_id_r || !slot_id_r || !tf_r || !term_len_r) {
                    continue;
                }
                auto term_r = rr.read_bytes(*term_len_r);
                if (!term_r) {
                    continue;
                }
                RID rid;
                rid.page_id = *page_id_r;
                rid.slot_id = *slot_id_r;
                pending.push_back(PendingPosting{std::move(*term_r), rid, *tf_r});
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
