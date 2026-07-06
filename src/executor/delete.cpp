#include "sixseven/executor/delete.h"

#include "sixseven/common/logging.h"
#include "sixseven/index/rid.h"

namespace sixseven {

DeleteOperator::DeleteOperator(TableHeap& heap, std::unique_ptr<Iterator> child)
    : heap_(heap), child_(std::move(child)),
      schema_(OutputSchema({OutputColumn{"", "count", TypeId::INT64, false, 0}})) {}

Result<void> DeleteOperator::do_open() {
    executed_ = false;
    return child_->open();
}

Result<std::optional<Tuple>> DeleteOperator::do_next() {
    if (executed_) {
        return ok(std::optional<Tuple>(std::nullopt));
    }
    executed_ = true;

    int64_t count = 0;
    row_delta_so_far_ = 0;

    while (true) {
        auto row = child_->next();
        if (!row) {
            return make_error(row.error().code, row.error().message);
        }
        if (!row->has_value()) {
            break;
        }

        auto& tuple = row->value();
        if (!tuple.rid.has_value()) {
            return make_error(StatusCode::INTERNAL_ERROR, "DELETE: tuple has no RID");
        }

        // Acquire IX table + X row lock before deleting this version (GDB-930).
        // lock_row auto-acquires the IX intent lock on the table, so a single
        // call suffices here (no separate lock_table call needed per row).
        if (lock_mgr_ != nullptr && txn_id_ != frozen_txn_id) {
            if (auto lr = lock_mgr_->lock_row(
                    txn_id_, lock_table_id_, tuple.rid->page_id, tuple.rid->slot_id, LockMode::X);
                !lr) {
                return tl::unexpected(lr.error());
            }
        }

        // MVCC heaps (GDB-747): logical delete — stamp xmax and leave the
        // version on the page so an aborted transaction undeletes it.
        // Legacy headerless heaps keep the physical delete.
        auto del = heap_.mvcc_headers() ? heap_.mark_deleted(*tuple.rid, txn_id_)
                                        : heap_.delete_tuple(*tuple.rid);
        if (!del) {
            return make_error(del.error().code, del.error().message);
        }
        // Row is now physically/logically deleted from the heap's live count
        // (row_count_ was already decremented inside mark_deleted/delete_tuple).
        // Track it here so a later error in this same row's index maintenance
        // still leaves an accurate partial delta for the caller to compensate.
        --row_delta_so_far_;

        // Maintain BM25 indexes: remove the deleted document's postings so it
        // no longer matches (and its RID can't be wrongly reused by a later
        // insert). remove_document is a no-op for un-indexed RIDs (NULL text
        // rows were never added), so no NOT_FOUND special-case is needed.
        for (const auto& target : bm25_targets_) {
            if (target.index != nullptr) {
                auto r = target.index->remove_document(*tuple.rid);
                if (!r) {
                    return tl::unexpected(r.error());
                }
            }
        }

        // Maintain HNSW indexes: tombstone the node for the deleted RID so the
        // vector is removed from the graph (not merely skipped post-hoc).
        // The rid_map linear scan guards entry: remove is only called when the
        // RID is found in rid_map, so NOT_FOUND from HnswIndex::remove indicates
        // a real internal inconsistency and is propagated as an error.
        for (auto& target : hnsw_targets_) {
            if (target.index == nullptr || target.rid_map == nullptr) {
                continue;
            }
            // Reverse-lookup: node_id is the position in rid_map where the RID
            // matches. The map is never compacted (shifting would renumber nodes),
            // so this linear scan is correct and stable.
            auto& rmap = *target.rid_map;
            for (size_t node_id = 0; node_id < rmap.size(); ++node_id) {
                if (rmap[node_id] == *tuple.rid) {
                    auto r = target.index->remove(static_cast<uint32_t>(node_id));
                    if (!r) {
                        return tl::unexpected(r.error());
                    }
                    // Tombstone the rid_map slot so nearest_scan's node->rid
                    // resolution returns invalid() and get_tuple fails cleanly.
                    rmap[node_id] = RID::invalid();
                    break;
                }
            }
        }
        ++count;
    }

    Tuple result;
    result.values.push_back(Value(count));
    return ok(std::optional<Tuple>(std::move(result)));
}

void DeleteOperator::do_close() {
    child_->close();
}

const OutputSchema& DeleteOperator::output_schema() const {
    return schema_;
}

std::vector<const Iterator*> DeleteOperator::plan_children() const {
    return {child_.get()};
}

std::vector<Iterator*> DeleteOperator::plan_children_mutable() {
    return {child_.get()};
}

} // namespace sixseven
