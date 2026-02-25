#include "giodb/executor/nearest_scan.h"

#include "giodb/executor/expr_evaluator.h"

#include <algorithm>
#include <cstddef>
#include <span>

namespace giodb {

NearestScanOperator::NearestScanOperator(TableHeap& heap,
                                         const Schema& storage_schema,
                                         NearestScanConfig config,
                                         OutputSchema schema,
                                         const Expr* where_expr,
                                         const BoundStatement& bound,
                                         HnswIndex* hnsw_index)
    : heap_(heap), storage_schema_(storage_schema), config_(std::move(config)),
      schema_(std::move(schema)), where_expr_(where_expr), bound_(bound), hnsw_index_(hnsw_index) {}

std::string NearestScanOperator::plan_node_name() const {
    return "Nearest Scan";
}
std::string NearestScanOperator::plan_node_detail() const {
    return "";
}

Result<void> NearestScanOperator::do_open() {
    results_.clear();
    cursor_ = 0;

    // Build the WHERE filter schema once (if needed).
    if (where_expr_ != nullptr) {
        where_filter_schema_ = build_where_filter_schema();
    }

    if (hnsw_index_ != nullptr && hnsw_index_->node_count() > 0) {
        return execute_hnsw_search();
    }
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

        float dist = compute_distance(config_.metric, query_span, emb_span);

        candidates.push_back({dist, std::move(tuple)});
        ++node_ordinal;
    }

    // Sort by distance ASC.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.distance < b.distance;
    });

    // Take top-k and apply WHERE filter.
    size_t count = 0;
    for (auto& cand : candidates) {
        if (count >= config_.k) {
            break;
        }

        auto emitted = filter_and_emit(cand.tuple, cand.distance);
        if (!emitted) {
            return make_error(emitted.error().code, emitted.error().message);
        }
        count += *emitted;
    }

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

    // Execute HNSW search.
    // Over-fetch when WHERE filter is present to account for filtered-out rows.
    uint32_t search_k = config_.k;
    if (where_expr_ != nullptr) {
        search_k = config_.k * 4;
    }

    Result<std::vector<HnswSearchResult>> search_results;
    if (predicate) {
        search_results =
            hnsw_index_->search(std::span<const float>(config_.query_vector), search_k, predicate);
    } else {
        search_results =
            hnsw_index_->search(std::span<const float>(config_.query_vector), search_k);
    }

    if (!search_results) {
        return make_error(search_results.error().code, search_results.error().message);
    }

    // Build node_id → distance mapping for the search results.
    std::unordered_map<uint32_t, float> result_distances;
    for (const auto& sr : *search_results) {
        result_distances[sr.node_id] = sr.distance;
    }

    // Scan table to collect matching rows.
    auto it = heap_.begin();
    if (!it) {
        return make_error(it.error().code, it.error().message);
    }

    struct MatchedRow {
        float distance;
        Tuple tuple;
    };
    std::vector<MatchedRow> matched;

    uint32_t node_ordinal = 0;
    while (true) {
        auto row = it->next();
        if (!row) {
            break;
        }

        auto dist_it = result_distances.find(node_ordinal);
        if (dist_it == result_distances.end()) {
            ++node_ordinal;
            continue;
        }

        auto& [rid, data] = *row;
        auto values = TupleSerializer::deserialize(data, storage_schema_);
        if (!values) {
            return make_error(values.error().code, values.error().message);
        }

        Tuple tuple{std::move(*values), rid};
        matched.push_back({dist_it->second, std::move(tuple)});
        ++node_ordinal;
    }

    // Sort by distance ASC.
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

    return ok();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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
    results_.push_back(std::move(result));
    return ok(size_t{1});
}

} // namespace giodb
