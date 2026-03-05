#pragma once

#include "sixseven/executor/iterator.h"
#include "sixseven/executor/sort.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace sixseven {

/// Two-phase external merge sort operator.
///
/// When the input fits in work_mem, sorts entirely in memory (identical to
/// SortOperator).  When the input exceeds work_mem the operator:
///
///   Phase 1 — Run Generation:
///     Read tuples into memory up to work_mem, sort, write to a temp file.
///     Repeat until all input is consumed, producing N sorted run files.
///
///   Phase 1.5 — Multi-pass Merge (if N > max_merge_width):
///     Merge groups of max_merge_width runs into new temp files until the
///     total run count is <= max_merge_width.
///
///   Phase 2 — K-way Merge:
///     Lazily merge the remaining runs via a min-heap, returning one sorted
///     tuple per next() call.
///
/// Temp files are cleaned up in close() and in the destructor.
class ExternalSortOperator : public Iterator {
public:
    /// @param child           Child operator to pull tuples from.
    /// @param keys            ORDER BY specification (expression + ASC/DESC).
    /// @param bound           BoundStatement with expr_types for evaluation.
    /// @param work_mem_bytes  Memory budget for sorting (default 64 MB).
    /// @param max_merge_width Max runs to merge simultaneously (default 128).
    /// @param temp_dir        Directory for temp files (default: system temp).
    ExternalSortOperator(std::unique_ptr<Iterator> child,
                         std::vector<SortKey> keys,
                         const BoundStatement& bound,
                         size_t work_mem_bytes = 64ULL * 1024 * 1024,
                         size_t max_merge_width = 128,
                         std::filesystem::path temp_dir = {});

    ~ExternalSortOperator() override;

    const OutputSchema& output_schema() const override;

    /// Estimate the in-memory size of a tuple (for memory accounting).
    static size_t estimate_tuple_size(const Tuple& tuple);

    // Plan inspection
    std::string plan_node_name() const override;
    std::string plan_node_detail() const override;
    std::vector<const Iterator*> plan_children() const override;

protected:
    Result<void> do_open() override;
    Result<std::optional<Tuple>> do_next() override;
    void do_close() override;
    std::vector<Iterator*> plan_children_mutable() override;

private:
    // -- Run generation -------------------------------------------------------
    Result<void> generate_runs();
    Result<void> flush_run(std::vector<Tuple>& buffer);

    // -- Multi-pass merge -----------------------------------------------------
    Result<void> reduce_runs();
    Result<void> merge_runs_to_file(size_t begin, size_t end, const std::filesystem::path& output);

    // -- Final k-way merge ----------------------------------------------------
    Result<void> setup_final_merge();

    // -- Tuple I/O ------------------------------------------------------------
    Result<void> write_tuple(std::ofstream& out, const Tuple& tuple);
    Result<std::optional<Tuple>> read_tuple(std::ifstream& in);

    // -- Comparison -----------------------------------------------------------
    bool compare_tuples(const Tuple& a, const Tuple& b) const;

    // -- Temp-file management -------------------------------------------------
    std::filesystem::path next_temp_path();
    void cleanup_temp_files();

    // -- Configuration --------------------------------------------------------
    std::unique_ptr<Iterator> child_;
    std::vector<SortKey> keys_;
    const BoundStatement& bound_;
    size_t work_mem_bytes_;
    size_t max_merge_width_;
    std::filesystem::path temp_dir_;

    // -- In-memory mode -------------------------------------------------------
    bool in_memory_mode_ = false;
    std::vector<Tuple> sorted_;
    size_t cursor_ = 0;

    // -- External merge state -------------------------------------------------
    struct RunReader {
        std::ifstream stream;
        std::filesystem::path path;
    };

    /// Min-heap entry: the current tuple from a run and its index.
    struct HeapEntry {
        Tuple tuple;
        size_t run_index;
    };

    std::vector<std::filesystem::path> run_files_;
    std::vector<RunReader> readers_;
    std::vector<HeapEntry> heap_;

    size_t temp_counter_ = 0;

    // -- Heap helpers ---------------------------------------------------------
    void heap_push(HeapEntry entry);
    HeapEntry heap_pop();
    void sift_down(size_t i);
    void sift_up(size_t i);
};

} // namespace sixseven
