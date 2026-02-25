#include "giodb/executor/bitmap_scan.h"

#include "giodb/executor/expr_evaluator.h"
#include "giodb/index/btree_iterator.h"
#include "giodb/table/tuple.h"

#include <algorithm>
#include <unordered_set>

namespace giodb {

BitmapScanOperator::BitmapScanOperator(std::vector<BitmapIndexScan> scans,
                                       BitmapCombineMode combine_mode,
                                       TableHeap& heap,
                                       const Schema& storage_schema,
                                       OutputSchema output_schema,
                                       const Expr* predicate,
                                       const BoundStatement* bound)
    : scans_(std::move(scans)), combine_mode_(combine_mode), heap_(heap),
      storage_schema_(storage_schema), schema_(std::move(output_schema)), predicate_(predicate),
      bound_(bound) {}

Result<void> BitmapScanOperator::open() {
    if (scans_.empty()) {
        return make_error(StatusCode::INTERNAL_ERROR, "BitmapScan: no index scans provided");
    }

    // Phase 1: collect RIDs from each index scan.
    std::vector<std::vector<RID>> rid_sets;
    rid_sets.reserve(scans_.size());

    for (const auto& scan : scans_) {
        auto rids = collect_rids(scan);
        if (!rids) {
            return make_error(rids.error().code, rids.error().message);
        }
        rid_sets.push_back(std::move(*rids));
    }

    // Combine using the specified mode.
    if (rid_sets.size() == 1) {
        rids_ = std::move(rid_sets[0]);
    } else if (combine_mode_ == BitmapCombineMode::OR) {
        rids_ = bitmap_or(rid_sets);
    } else {
        rids_ = bitmap_and(rid_sets);
    }

    // Sort by page_id then slot_id for sequential I/O.
    std::sort(rids_.begin(), rids_.end());

    rid_pos_ = 0;
    return ok();
}

Result<std::optional<Tuple>> BitmapScanOperator::next() {
    while (rid_pos_ < rids_.size()) {
        const auto& rid = rids_[rid_pos_];
        ++rid_pos_;

        auto data = heap_.get_tuple(rid);
        if (!data) {
            return make_error(data.error().code, data.error().message);
        }

        auto values = TupleSerializer::deserialize(*data, storage_schema_);
        if (!values) {
            return make_error(values.error().code, values.error().message);
        }

        Tuple tuple{std::move(*values), rid};

        // Apply optional residual predicate.
        if (predicate_ != nullptr && bound_ != nullptr) {
            auto pass = evaluate_predicate(*predicate_, tuple, schema_, *bound_);
            if (!pass) {
                return make_error(pass.error().code, pass.error().message);
            }
            if (!*pass) {
                continue;
            }
        }

        return ok(std::optional<Tuple>(std::move(tuple)));
    }

    // All RIDs exhausted.
    return ok(std::optional<Tuple>(std::nullopt));
}

void BitmapScanOperator::close() {
    rids_.clear();
    rid_pos_ = 0;
}

const OutputSchema& BitmapScanOperator::output_schema() const {
    return schema_;
}

Result<std::vector<RID>> BitmapScanOperator::collect_rids(const BitmapIndexScan& scan) const {
    if (scan.index == nullptr) {
        return make_error(StatusCode::INTERNAL_ERROR, "BitmapScan: null index");
    }

    auto it = scan.index->range_scan(scan.begin_key, scan.end_key);
    if (!it) {
        return make_error(it.error().code, it.error().message);
    }

    std::vector<RID> rids;
    while (!it->is_end()) {
        auto entry = it->next();
        if (!entry) {
            return make_error(entry.error().code, entry.error().message);
        }
        if (!entry->has_value()) {
            break;
        }
        rids.push_back((*entry)->second);
    }

    return ok(std::move(rids));
}

std::vector<RID> BitmapScanOperator::bitmap_or(const std::vector<std::vector<RID>>& rid_sets) {
    // Use a set to deduplicate.
    struct RidHash {
        size_t operator()(const RID& r) const {
            return std::hash<uint64_t>()(static_cast<uint64_t>(r.page_id) << 16 | r.slot_id);
        }
    };
    std::unordered_set<RID, RidHash> seen;

    for (const auto& set : rid_sets) {
        for (const auto& rid : set) {
            seen.insert(rid);
        }
    }

    return {seen.begin(), seen.end()};
}

std::vector<RID> BitmapScanOperator::bitmap_and(const std::vector<std::vector<RID>>& rid_sets) {
    if (rid_sets.empty()) {
        return {};
    }

    struct RidHash {
        size_t operator()(const RID& r) const {
            return std::hash<uint64_t>()(static_cast<uint64_t>(r.page_id) << 16 | r.slot_id);
        }
    };

    // Start with the smallest set for efficiency.
    size_t smallest_idx = 0;
    for (size_t i = 1; i < rid_sets.size(); ++i) {
        if (rid_sets[i].size() < rid_sets[smallest_idx].size()) {
            smallest_idx = i;
        }
    }

    std::unordered_set<RID, RidHash> current(rid_sets[smallest_idx].begin(),
                                             rid_sets[smallest_idx].end());

    for (size_t i = 0; i < rid_sets.size(); ++i) {
        if (i == smallest_idx) {
            continue;
        }
        std::unordered_set<RID, RidHash> next_set(rid_sets[i].begin(), rid_sets[i].end());
        std::unordered_set<RID, RidHash> intersection;
        for (const auto& rid : current) {
            if (next_set.count(rid) > 0) {
                intersection.insert(rid);
            }
        }
        current = std::move(intersection);
    }

    return {current.begin(), current.end()};
}

} // namespace giodb
