#pragma once

#include "giodb/catalog/schema.h"
#include "giodb/common/result.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/parser/ast.h"
#include "giodb/table/table_heap.h"
#include "giodb/table/tuple.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace giodb {

// -- Per-table statistics -----------------------------------------------------

/// Aggregate statistics for a single table.
struct TableStats {
    table_id_t table_id = 0;
    uint64_t row_count = 0;        ///< Number of live tuples.
    uint32_t page_count = 0;       ///< Number of heap pages.
    double avg_row_width = 0.0;    ///< Average serialized row width in bytes.
    uint64_t dead_tuple_count = 0; ///< Dead tuples (for vacuum decisions).
};

// -- Per-column statistics ----------------------------------------------------

/// A value with its relative frequency (fraction of non-null rows).
struct McvEntry {
    Value value;
    double frequency = 0.0; ///< Fraction of non-null rows with this value.
};

/// An equi-depth histogram bucket for range selectivity estimation.
/// Each bucket represents a range [lower, upper] containing approximately
/// the same number of values.
struct HistogramBucket {
    Value lower;            ///< Inclusive lower bound.
    Value upper;            ///< Inclusive upper bound.
    uint64_t count = 0;     ///< Number of values in this bucket.
    uint64_t ndistinct = 0; ///< Distinct values in this bucket.
};

/// Statistics for a single column of a table.
struct ColumnStats {
    table_id_t table_id = 0;
    int32_t column_ordinal = 0;
    std::string column_name;
    TypeId type_id = TypeId::INT32;

    double null_fraction = 0.0; ///< Fraction of rows that are NULL.
    uint64_t ndistinct = 0;     ///< Estimated number of distinct non-null values.
    Value min_value;            ///< Minimum non-null value.
    Value max_value;            ///< Maximum non-null value.

    std::vector<McvEntry> mcv_list;         ///< Most common values, sorted by frequency desc.
    std::vector<HistogramBucket> histogram; ///< Equi-depth histogram (excludes MCV values).
};

// -- Statistics store ---------------------------------------------------------

/// Configuration for the ANALYZE command.
struct AnalyzeConfig {
    uint32_t sample_size = 30000;     ///< Max rows to sample for large tables.
    uint32_t mcv_count = 10;          ///< Number of most-common values to keep.
    uint32_t histogram_buckets = 100; ///< Number of equi-depth histogram buckets.
};

/// In-memory store of table and column statistics.
/// The optimizer queries this store for cost estimation.
class StatisticsStore {
public:
    /// Set statistics for a table.
    void set_table_stats(table_id_t table_id, TableStats stats);

    /// Get statistics for a table. Returns nullptr if not available.
    [[nodiscard]] const TableStats* get_table_stats(table_id_t table_id) const;

    /// Set statistics for a column.
    void set_column_stats(table_id_t table_id, int32_t ordinal, ColumnStats stats);

    /// Get statistics for a column. Returns nullptr if not available.
    [[nodiscard]] const ColumnStats* get_column_stats(table_id_t table_id, int32_t ordinal) const;

    /// Remove all statistics for a table (e.g., on DROP TABLE).
    void remove_table(table_id_t table_id);

    /// Clear all statistics.
    void clear();

private:
    std::unordered_map<table_id_t, TableStats> table_stats_;

    /// Key: (table_id, column_ordinal) encoded as int64_t.
    static int64_t column_key(table_id_t table_id, int32_t ordinal) {
        return (static_cast<int64_t>(table_id) << 32) | static_cast<uint32_t>(ordinal);
    }
    std::unordered_map<int64_t, ColumnStats> column_stats_;
};

// -- ANALYZE implementation ---------------------------------------------------

/// Gather statistics for a single table, populating the StatisticsStore.
/// Scans the table (or samples if large) and computes table-level and
/// column-level statistics.
///
/// @param table_id       The table to analyze.
/// @param table_schema   Catalog schema for the table.
/// @param heap           The table's heap file for scanning tuples.
/// @param storage_schema Byte-level schema for deserializing tuples.
/// @param store          Statistics store to populate.
/// @param config         ANALYZE configuration (sample size, MCV count, etc.).
[[nodiscard]] Result<void> analyze_table(table_id_t table_id,
                                         const TableSchema& table_schema,
                                         TableHeap& heap,
                                         const Schema& storage_schema,
                                         StatisticsStore& store,
                                         const AnalyzeConfig& config = {});

// -- Selectivity estimation ---------------------------------------------------

/// Estimate the selectivity of an equality predicate: column = value.
/// Returns a fraction in [0, 1].
[[nodiscard]] double estimate_equality_selectivity(const ColumnStats& stats, const Value& value);

/// Estimate the selectivity of a range predicate: column < value (or <=, >, >=).
/// @param op One of BinaryOp::LESS, LESS_EQ, GREATER, GREATER_EQ.
[[nodiscard]] double
estimate_range_selectivity(const ColumnStats& stats, BinaryOp op, const Value& value);

/// Estimate the selectivity of a BETWEEN predicate: column BETWEEN low AND high.
[[nodiscard]] double
estimate_between_selectivity(const ColumnStats& stats, const Value& low, const Value& high);

/// Estimate the selectivity of IS NULL.
[[nodiscard]] double estimate_is_null_selectivity(const ColumnStats& stats);

/// Estimate the selectivity of IS NOT NULL.
[[nodiscard]] double estimate_is_not_null_selectivity(const ColumnStats& stats);

/// Default selectivity for unknown predicates.
static constexpr double kDefaultSelectivity = 0.1;

/// Default selectivity for equality predicates when no statistics available.
static constexpr double kDefaultEqualitySelectivity = 0.01;

} // namespace giodb
