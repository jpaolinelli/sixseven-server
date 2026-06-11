#include "sixseven/executor/nearest_scan.h"

#include "sixseven/common/logging.h"
#include "sixseven/executor/expr_evaluator.h"

#include <algorithm>
#include <cstddef>
#include <span>

namespace sixseven {

namespace {

/// Metric used for the candidate sort key. NEAREST orders candidates by
/// distance ascending, but DOT_PRODUCT is a raw similarity (higher = more
/// similar) — sorting it ascending would return the k LEAST similar rows.
/// Normalize it to the negated INNER_PRODUCT variant so ascending order is
/// always "most similar first" (GDB-717).
DistanceMetric sort_metric(DistanceMetric m) {
    return m == DistanceMetric::DOT_PRODUCT ? DistanceMetric::INNER_PRODUCT : m;
}

} // anonymous namespace

NearestScanOperator::NearestScanOperator(TableHeap& heap,
                                         const Schema& storage_schema,
                                         NearestScanConfig config,
                                         OutputSchema schema,
                                         const Expr* where_expr,
                                         const BoundStatement& bound,
                                         HnswIndex* hnsw_index,
                                         std::vector<RID>* hnsw_rid_map)
    : heap_(heap), storage_schema_(storage_schema), config_(std::move(config)),
      schema_(std::move(schema)), where_expr_(where_expr), bound_(bound), hnsw_index_(hnsw_index),
      hnsw_rid_map_(hnsw_rid_map) {}

std::string NearestScanOperator::plan_node_name() const {
    return "Nearest Scan";
}
std::string NearestScanOperator::plan_node_detail() const {
    return "";
}

Result<void> NearestScanOperator::do_open() {
    results_.clear();
    seen_rids_.clear();
    cursor_ = 0;

    // Build the WHERE filter schema once (if needed).
    if (where_expr_ != nullptr) {
        where_filter_schema_ = build_where_filter_schema();
    }

    // Pre-filtered path: btree already identified candidate RIDs — compute
    // distances only for those rows (brute-force on a small set).
    if (!config_.prefiltered_rids.empty()) {
        SIXSEVEN_LOG_DEBUG("NEAREST: using prefiltered search ({} candidate RIDs)",
                           config_.prefiltered_rids.size());
        return execute_prefiltered_search();
    }

    if (hnsw_index_ != nullptr && hnsw_index_->node_count() > 0) {
        SIXSEVEN_LOG_DEBUG("NEAREST: using HNSW index (node_count={}, rid_map={})",
                           hnsw_index_->node_count(),
                           hnsw_rid_map_ ? hnsw_rid_map_->size() : 0);
        return execute_hnsw_search();
    }
    SIXSEVEN_LOG_DEBUG("NEAREST: using brute-force scan (hnsw={})",
                       hnsw_index_ != nullptr ? "empty" : "null");
    return execute_brute_force();
}

Result<std::optional<Tuple>> NearestScanOperator::do_next() {
    if (cursor_ >= results_.size()) {
        return ok(std::optional<Tuple>(std::nullopt));
    }
    return ok(std::make_optional(std::move(results_[cursor_++])));
}

void NearestScanOperator::do_close() {
    results_.clear();
    cursor_ = 0;
}

const OutputSchema& NearestScanOperator::output_schema() const {
    return schema_;
}

// ---------------------------------------------------------------------------
// Brute-force scan
// ---------------------------------------------------------------------------

Result<void> NearestScanOperator::execute_brute_force() {
    auto it = heap_.begin();
    if (!it) {
        return make_error(it.error().code, it.error().message);
    }

    // Candidate: (distance, tuple with all table columns).
    struct Candidate {
        float distance;
        Tuple tuple;
    };
    std::vector<Candidate> candidates;

    std::span<const float> query_span(config_.query_vector);
    uint32_t node_ordinal = 0;

    while (true) {
        auto row = it->next();
        if (!row) {
            break;
        }

        auto& [rid, data] = *row;

        auto values = TupleSerializer::deserialize(data, storage_schema_);
        if (!values) {
            return make_error(values.error().code, values.error().message);
        }

        Tuple tuple{std::move(*values), rid};

        // Check embedding column is valid and non-null.
        auto col_idx = static_cast<size_t>(config_.embedding_column_index);
        if (col_idx >= tuple.values.size() || tuple.values[col_idx].is_null()) {
            ++node_ordinal;
            continue;
        }

        // Check graph-scoped filter.
        if (!config_.allowed_node_ids.empty() &&
            config_.allowed_node_ids.count(node_ordinal) == 0) {
            ++node_ordinal;
            continue;
        }

        const auto& embedding = tuple.values[col_idx].as_embedding();
        std::span<const float> emb_span(embedding);

        float dist = compute_distance(sort_metric(config_.metric), query_span, emb_span);

        candidates.push_back({dist, std::move(tuple)});
        ++node_ordinal;
    }

    // Sort by distance ASC (lower sort key = more similar for every metric).
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.distance < b.distance;
    });

    // Take top-k and apply WHERE filter.
    size_t count = 0;
    for (auto& cand : candidates) {
        if (count >= config_.k) {
            break;
        }

        auto emitted = filter_and_emit(cand.tuple, display_distance(cand.distance));
        if (!emitted) {
            return make_error(emitted.error().code, emitted.error().message);
        }
        count += *emitted;
    }

    return ok();
}

// ---------------------------------------------------------------------------
// Pre-filtered search (btree-accelerated)
// ---------------------------------------------------------------------------

Result<void> NearestScanOperator::execute_prefiltered_search() {
    struct Candidate {
        float distance;
        Tuple tuple;
    };
    std::vector<Candidate> candidates;

    std::span<const float> query_span(config_.query_vector);
    auto col_idx = static_cast<size_t>(config_.embedding_column_index);

    for (const auto& rid : config_.prefiltered_rids) {
        auto data = heap_.get_tuple(rid);
        if (!data) {
            // Deleted or invalid row — skip gracefully.
            SIXSEVEN_LOG_DEBUG("NEAREST prefiltered: get_tuple failed for rid=({},{}): {}",
                               rid.page_id,
                               rid.slot_id,
                               data.error().message);
            continue;
        }

        auto values = TupleSerializer::deserialize(*data, storage_schema_);
        if (!values) {
            return make_error(values.error().code, values.error().message);
        }

        // Check embedding column is valid and non-null.
        if (col_idx >= values->size() || (*values)[col_idx].is_null()) {
            continue;
        }

        Tuple tuple{std::move(*values), rid};

        const auto& embedding = tuple.values[col_idx].as_embedding();
        std::span<const float> emb_span(embedding);
        float dist = compute_distance(sort_metric(config_.metric), query_span, emb_span);

        candidates.push_back({dist, std::move(tuple)});
    }

    // Sort by distance ASC (lower sort key = more similar for every metric).
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.distance < b.distance;
    });

    // Take top-k and apply WHERE filter.
    size_t count = 0;
    for (auto& cand : candidates) {
        if (count >= config_.k) {
            break;
        }
        auto emitted = filter_and_emit(cand.tuple, display_distance(cand.distance));
        if (!emitted) {
            return make_error(emitted.error().code, emitted.error().message);
        }
        count += *emitted;
    }

    SIXSEVEN_LOG_DEBUG(
        "NEAREST prefiltered: {} candidates, {} results emitted", candidates.size(), count);
    return ok();
}

// ---------------------------------------------------------------------------
// HNSW index search
// ---------------------------------------------------------------------------

Result<void> NearestScanOperator::execute_hnsw_search() {
    // Build the filter predicate for the HNSW search.
    HnswFilterPredicate predicate;
    if (!config_.allowed_node_ids.empty()) {
        predicate = [this](uint32_t node_id) -> bool {
            return config_.allowed_node_ids.count(node_id) > 0;
        };
    }

    uint32_t total_nodes = hnsw_index_->node_count();

    // When a WHERE clause is present, use iterative widening: start with a
    // moderate over-fetch and keep doubling until we find enough matches or
    // exhaust the index.  Without WHERE, fetch exactly k.
    uint32_t search_k = config_.k;
    if (where_expr_ != nullptr) {
        search_k = std::min(config_.k * 10, total_nodes);
    }

    struct MatchedRow {
        float distance;
        Tuple tuple;
    };

    while (true) {
        Result<std::vector<HnswSearchResult>> search_results;
        if (predicate) {
            search_results = hnsw_index_->search(
                std::span<const float>(config_.query_vector), search_k, predicate);
        } else {
            search_results =
                hnsw_index_->search(std::span<const float>(config_.query_vector), search_k);
        }

        if (!search_results) {
            return make_error(search_results.error().code, search_results.error().message);
        }

        SIXSEVEN_LOG_DEBUG(
            "NEAREST: HNSW returned {} candidates (search_k={})", search_results->size(), search_k);

        std::vector<MatchedRow> matched;

        // Fast path: use node_id → RID map for direct tuple lookups (O(k)).
        if (hnsw_rid_map_ != nullptr && !hnsw_rid_map_->empty()) {
            SIXSEVEN_LOG_DEBUG("NEAREST: using RID map (size={})", hnsw_rid_map_->size());
            for (const auto& sr : *search_results) {
                if (sr.node_id >= hnsw_rid_map_->size()) {
                    SIXSEVEN_LOG_DEBUG("NEAREST: node_id {} out of range (map_size={})",
                                       sr.node_id,
                                       hnsw_rid_map_->size());
                    continue; // Out of range — skip.
                }
                auto rid = (*hnsw_rid_map_)[sr.node_id];
                auto data = heap_.get_tuple(rid);
                if (!data) {
                    SIXSEVEN_LOG_DEBUG("NEAREST: get_tuple failed for node_id={} rid=({},{}): {}",
                                       sr.node_id,
                                       rid.page_id,
                                       rid.slot_id,
                                       data.error().message);
                    continue; // Deleted row — skip.
                }
                auto values = TupleSerializer::deserialize(*data, storage_schema_);
                if (!values) {
                    return make_error(values.error().code, values.error().message);
                }
                Tuple tuple{std::move(*values), rid};
                matched.push_back({sr.distance, std::move(tuple)});
            }
        } else {
            // Slow fallback: full table scan to map node_id → row.
            // node_ordinal only increments for rows with non-null embeddings,
            // matching the insertion order during rebuild_hnsw_from_table.
            std::unordered_map<uint32_t, float> result_distances;
            for (const auto& sr : *search_results) {
                result_distances[sr.node_id] = sr.distance;
            }

            auto it = heap_.begin();
            if (!it) {
                return make_error(it.error().code, it.error().message);
            }

            auto emb_idx = static_cast<size_t>(config_.embedding_column_index);
            uint32_t node_ordinal = 0;
            while (true) {
                auto row = it->next();
                if (!row) {
                    break;
                }

                auto& [rid, data] = *row;
                auto values = TupleSerializer::deserialize(data, storage_schema_);
                if (!values) {
                    return make_error(values.error().code, values.error().message);
                }

                // Only count rows with non-null embeddings as HNSW nodes.
                if (emb_idx >= values->size() || (*values)[emb_idx].is_null()) {
                    continue;
                }

                auto dist_it = result_distances.find(node_ordinal);
                if (dist_it != result_distances.end()) {
                    Tuple tuple{std::move(*values), rid};
                    matched.push_back({dist_it->second, std::move(tuple)});
                }
                ++node_ordinal;
            }
        }

        // Sort by distance ASC. HNSW distances come straight from the index
        // (which currently computes L2 regardless of the requested metric —
        // GDB-723) and are passed through unchanged, so no display
        // translation is applied on this path.
        std::sort(matched.begin(), matched.end(), [](const MatchedRow& a, const MatchedRow& b) {
            return a.distance < b.distance;
        });

        // Apply WHERE filter and take top-k.
        size_t count = 0;
        for (auto& m : matched) {
            if (count >= config_.k) {
                break;
            }

            auto emitted = filter_and_emit(m.tuple, m.distance);
            if (!emitted) {
                return make_error(emitted.error().code, emitted.error().message);
            }
            count += *emitted;
        }

        // If we found enough results, or we've already searched the entire
        // index, we're done. We widen on any under-fill — a WHERE filter or
        // de-duplicated rows can both leave us short of k.
        if (count >= config_.k || search_k >= total_nodes) {
            break;
        }

        // Widen the search: double search_k, capped at total node count.
        results_.clear();
        seen_rids_.clear();
        uint32_t next_k = std::min(search_k * 2, total_nodes);
        if (next_k == search_k) {
            break; // No point searching the same amount again.
        }
        SIXSEVEN_LOG_DEBUG("NEAREST: widening search from {} to {} (found {}/{})",
                           search_k,
                           next_k,
                           count,
                           config_.k);
        search_k = next_k;
    }

    return ok();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

float NearestScanOperator::display_distance(float sort_distance) const {
    // For dot-product metrics the sort key is the negated dot product
    // (INNER_PRODUCT), so ascending order means most-similar-first. The
    // user-visible _distance column reports the raw dot product (higher =
    // more similar), matching the documented USING DOT semantics (GDB-717).
    if (config_.metric == DistanceMetric::DOT_PRODUCT ||
        config_.metric == DistanceMetric::INNER_PRODUCT) {
        return -sort_distance;
    }
    return sort_distance;
}

OutputSchema NearestScanOperator::build_where_filter_schema() const {
    // The output schema has table columns + _distance at the end.
    // The WHERE predicate references table columns only, so strip _distance.
    std::vector<OutputColumn> table_cols;
    for (size_t i = 0; i + 1 < schema_.column_count(); ++i) {
        table_cols.push_back(schema_.column(i));
    }
    return OutputSchema(std::move(table_cols));
}

Result<size_t> NearestScanOperator::filter_and_emit(Tuple& candidate_tuple, float distance) {
    // Skip rows already emitted in this search. A single physical row can be
    // referenced by more than one HNSW node (e.g. it was embedded twice during
    // bootstrap), and must still surface as exactly one result.
    if (candidate_tuple.rid.has_value() && seen_rids_.count(*candidate_tuple.rid) > 0) {
        return ok(size_t{0});
    }

    // Apply WHERE post-filter if present.
    if (where_expr_ != nullptr) {
        auto pass = evaluate_predicate(*where_expr_, candidate_tuple, where_filter_schema_, bound_);
        if (!pass) {
            return make_error(pass.error().code, pass.error().message);
        }
        if (!*pass) {
            return ok(size_t{0});
        }
    }

    // Build result tuple: all table columns + _distance.
    Tuple result;
    result.values = std::move(candidate_tuple.values);
    result.values.push_back(Value(static_cast<double>(distance)));
    result.rid = candidate_tuple.rid;
    if (candidate_tuple.rid.has_value()) {
        seen_rids_.insert(*candidate_tuple.rid);
    }
    results_.push_back(std::move(result));
    return ok(size_t{1});
}

} // namespace sixseven
