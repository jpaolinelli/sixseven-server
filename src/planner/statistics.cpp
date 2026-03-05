#include "sixseven/planner/statistics.h"

#include "sixseven/common/coercion.h"
#include "sixseven/common/logging.h"
#include "sixseven/table/tuple.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_map>

namespace sixseven {

// =============================================================================
// StatisticsStore
// =============================================================================

void StatisticsStore::set_table_stats(table_id_t table_id, TableStats stats) {
    table_stats_[table_id] = std::move(stats);
}

const TableStats* StatisticsStore::get_table_stats(table_id_t table_id) const {
    auto it = table_stats_.find(table_id);
    return it != table_stats_.end() ? &it->second : nullptr;
}

void StatisticsStore::set_column_stats(table_id_t table_id, int32_t ordinal, ColumnStats stats) {
    column_stats_[column_key(table_id, ordinal)] = std::move(stats);
}

const ColumnStats* StatisticsStore::get_column_stats(table_id_t table_id, int32_t ordinal) const {
    auto it = column_stats_.find(column_key(table_id, ordinal));
    return it != column_stats_.end() ? &it->second : nullptr;
}

void StatisticsStore::remove_table(table_id_t table_id) {
    table_stats_.erase(table_id);
    // Remove all column stats for this table. The key encodes table_id in upper 32 bits.
    for (auto it = column_stats_.begin(); it != column_stats_.end();) {
        if (it->second.table_id == table_id) {
            it = column_stats_.erase(it);
        } else {
            ++it;
        }
    }
}

void StatisticsStore::clear() {
    table_stats_.clear();
    column_stats_.clear();
}

// =============================================================================
// Reservoir sampling
// =============================================================================

/// Collect all rows (if small enough) or a reservoir sample of rows from the table.
/// Returns rows as vectors of Value.
static Result<std::vector<std::vector<Value>>> sample_table(TableHeap& heap,
                                                            const Schema& storage_schema,
                                                            uint32_t max_sample_size,
                                                            uint64_t& total_row_count,
                                                            uint64_t& total_row_bytes) {
    std::vector<std::vector<Value>> sample;
    sample.reserve(std::min(max_sample_size, static_cast<uint32_t>(10000)));

    auto iter_result = heap.begin();
    if (!iter_result) {
        return make_error(iter_result.error().code, iter_result.error().message);
    }
    auto& iter = *iter_result;

    std::mt19937_64 rng(42); // Deterministic seed for reproducibility.
    uint64_t row_index = 0;
    total_row_bytes = 0;

    while (auto row = iter.next()) {
        auto& [rid, data] = *row;
        total_row_bytes += data.size();

        auto values = TupleSerializer::deserialize(data, storage_schema);
        if (!values) {
            // Skip corrupt tuples.
            ++row_index;
            continue;
        }

        if (row_index < max_sample_size) {
            sample.push_back(std::move(*values));
        } else {
            // Reservoir sampling: replace a random element with decreasing probability.
            std::uniform_int_distribution<uint64_t> dist(0, row_index);
            auto j = dist(rng);
            if (j < max_sample_size) {
                sample[j] = std::move(*values);
            }
        }
        ++row_index;
    }

    total_row_count = row_index;
    return ok(std::move(sample));
}

// =============================================================================
// Column statistics computation
// =============================================================================

/// Value wrapper that can be used as a hash map key.
/// Uses string representation for hashing (simple but effective for statistics).
struct ValueKey {
    Value value;

    bool operator==(const ValueKey& other) const {
        if (value.is_null() && other.value.is_null()) {
            return true;
        }
        if (value.is_null() || other.value.is_null()) {
            return false;
        }
        auto cmp = compare(value, other.value);
        return cmp && *cmp == std::strong_ordering::equal;
    }
};

struct ValueKeyHash {
    size_t operator()(const ValueKey& vk) const {
        if (vk.value.is_null()) {
            return 0;
        }
        // Use variant index + data hash.
        const auto& data = vk.value.data();
        size_t h = std::hash<size_t>{}(data.index());

        std::visit(
            [&h](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::monostate>) {
                    // NULL — already handled.
                } else if constexpr (std::is_same_v<T, std::string>) {
                    h ^= std::hash<std::string>{}(v) << 1;
                } else if constexpr (std::is_same_v<T, bool>) {
                    h ^= std::hash<bool>{}(v) << 1;
                } else if constexpr (std::is_same_v<T, int8_t> || std::is_same_v<T, int16_t> ||
                                     std::is_same_v<T, int32_t> || std::is_same_v<T, int64_t>) {
                    h ^= std::hash<int64_t>{}(static_cast<int64_t>(v)) << 1;
                } else if constexpr (std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t> ||
                                     std::is_same_v<T, uint32_t> || std::is_same_v<T, uint64_t>) {
                    h ^= std::hash<uint64_t>{}(static_cast<uint64_t>(v)) << 1;
                } else if constexpr (std::is_same_v<T, float>) {
                    h ^= std::hash<float>{}(v) << 1;
                } else if constexpr (std::is_same_v<T, double>) {
                    h ^= std::hash<double>{}(v) << 1;
                } else {
                    // For complex types (Date, Time, etc.), use a simple hash.
                    h ^= std::hash<size_t>{}(42) << 1;
                }
            },
            data);

        return h;
    }
};

/// Compute statistics for a single column from sampled rows.
static ColumnStats compute_column_stats(table_id_t table_id,
                                        int32_t ordinal,
                                        const std::string& column_name,
                                        TypeId type_id,
                                        const std::vector<std::vector<Value>>& sample,
                                        uint32_t mcv_count,
                                        uint32_t histogram_buckets) {
    ColumnStats stats;
    stats.table_id = table_id;
    stats.column_ordinal = ordinal;
    stats.column_name = column_name;
    stats.type_id = type_id;

    if (sample.empty()) {
        return stats;
    }

    // Count nulls and collect non-null values.
    uint64_t null_count = 0;
    std::vector<const Value*> non_null_values;
    non_null_values.reserve(sample.size());

    for (const auto& row : sample) {
        if (ordinal < 0 || static_cast<size_t>(ordinal) >= row.size()) {
            continue;
        }
        const auto& val = row[static_cast<size_t>(ordinal)];
        if (val.is_null()) {
            ++null_count;
        } else {
            non_null_values.push_back(&val);
        }
    }

    stats.null_fraction = static_cast<double>(null_count) / static_cast<double>(sample.size());

    if (non_null_values.empty()) {
        return stats;
    }

    // Count distinct values and frequencies.
    std::unordered_map<ValueKey, uint64_t, ValueKeyHash> freq_map;
    for (const auto* val : non_null_values) {
        freq_map[ValueKey{*val}]++;
    }

    stats.ndistinct = freq_map.size();

    // Build MCV list: top mcv_count values by frequency.
    std::vector<std::pair<ValueKey, uint64_t>> freq_vec(freq_map.begin(), freq_map.end());
    std::sort(freq_vec.begin(), freq_vec.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });

    auto mcv_limit = std::min(static_cast<size_t>(mcv_count), freq_vec.size());
    double non_null_count_d = static_cast<double>(non_null_values.size());

    // Only include values that appear more than once (or all if few distinct values).
    std::unordered_map<ValueKey, bool, ValueKeyHash> is_mcv;
    for (size_t i = 0; i < mcv_limit; ++i) {
        McvEntry entry;
        entry.value = freq_vec[i].first.value;
        entry.frequency = static_cast<double>(freq_vec[i].second) / non_null_count_d;
        stats.mcv_list.push_back(std::move(entry));
        is_mcv[freq_vec[i].first] = true;
    }

    // Compute min/max using the compare function.
    // Only do this for comparable types.
    if (is_comparable(type_id)) {
        const Value* min_val = non_null_values[0];
        const Value* max_val = non_null_values[0];
        for (size_t i = 1; i < non_null_values.size(); ++i) {
            auto cmp = compare(*non_null_values[i], *min_val);
            if (cmp && *cmp == std::strong_ordering::less) {
                min_val = non_null_values[i];
            }
            cmp = compare(*non_null_values[i], *max_val);
            if (cmp && *cmp == std::strong_ordering::greater) {
                max_val = non_null_values[i];
            }
        }
        stats.min_value = *min_val;
        stats.max_value = *max_val;
    }

    // Build equi-depth histogram from non-MCV values for comparable types.
    if (is_comparable(type_id) && histogram_buckets > 0) {
        // Collect non-MCV values.
        std::vector<const Value*> hist_values;
        hist_values.reserve(non_null_values.size());
        for (const auto* val : non_null_values) {
            if (!is_mcv.contains(ValueKey{*val})) {
                hist_values.push_back(val);
            }
        }

        if (!hist_values.empty()) {
            // Sort values.
            std::sort(hist_values.begin(), hist_values.end(), [](const Value* a, const Value* b) {
                auto cmp = compare(*a, *b);
                return cmp && *cmp == std::strong_ordering::less;
            });

            // Divide into equi-depth buckets.
            auto bucket_count =
                std::min(static_cast<size_t>(histogram_buckets), hist_values.size());
            auto values_per_bucket = hist_values.size() / bucket_count;
            if (values_per_bucket == 0) {
                values_per_bucket = 1;
            }

            for (size_t b = 0; b < bucket_count; ++b) {
                auto start = b * values_per_bucket;
                auto end =
                    (b == bucket_count - 1) ? hist_values.size() : (b + 1) * values_per_bucket;
                if (start >= hist_values.size()) {
                    break;
                }

                HistogramBucket bucket;
                bucket.lower = *hist_values[start];
                bucket.upper = *hist_values[end - 1];
                bucket.count = end - start;

                // Count distinct values in this bucket.
                uint64_t distinct = 1;
                for (size_t i = start + 1; i < end; ++i) {
                    auto cmp = compare(*hist_values[i], *hist_values[i - 1]);
                    if (cmp && *cmp != std::strong_ordering::equal) {
                        ++distinct;
                    }
                }
                bucket.ndistinct = distinct;

                stats.histogram.push_back(std::move(bucket));
            }
        }
    }

    return stats;
}

// =============================================================================
// analyze_table
// =============================================================================

Result<void> analyze_table(table_id_t table_id,
                           const TableSchema& table_schema,
                           TableHeap& heap,
                           const Schema& storage_schema,
                           StatisticsStore& store,
                           const AnalyzeConfig& config) {
    // Sample or scan the table.
    uint64_t total_row_count = 0;
    uint64_t total_row_bytes = 0;
    auto sample_result =
        sample_table(heap, storage_schema, config.sample_size, total_row_count, total_row_bytes);
    if (!sample_result) {
        return make_error(sample_result.error().code, sample_result.error().message);
    }
    const auto& sample = *sample_result;

    // Compute table-level statistics.
    auto page_count_result = heap.page_count();
    uint32_t pages = page_count_result ? *page_count_result : 0;

    TableStats table_stats;
    table_stats.table_id = table_id;
    table_stats.row_count = total_row_count;
    table_stats.page_count = pages;
    table_stats.avg_row_width = total_row_count > 0 ? static_cast<double>(total_row_bytes) /
                                                          static_cast<double>(total_row_count)
                                                    : 0.0;

    store.set_table_stats(table_id, table_stats);

    // Compute per-column statistics.
    for (const auto& col : table_schema.columns) {
        auto col_stats = compute_column_stats(table_id,
                                              col.ordinal,
                                              col.name,
                                              col.type_id,
                                              sample,
                                              config.mcv_count,
                                              config.histogram_buckets);
        store.set_column_stats(table_id, col.ordinal, std::move(col_stats));
    }

    SIXSEVEN_LOG_INFO("ANALYZE: table {} ({}) — {} rows, {} pages, {:.1f} avg width",
                   table_schema.name,
                   table_id,
                   total_row_count,
                   pages,
                   table_stats.avg_row_width);

    return ok();
}

// =============================================================================
// Selectivity estimation
// =============================================================================

double estimate_equality_selectivity(const ColumnStats& stats, const Value& value) {
    if (value.is_null()) {
        return stats.null_fraction;
    }

    // Check MCV list first.
    for (const auto& mcv : stats.mcv_list) {
        auto cmp = compare(mcv.value, value);
        if (cmp && *cmp == std::strong_ordering::equal) {
            // Return MCV frequency scaled to total rows (including nulls).
            return mcv.frequency * (1.0 - stats.null_fraction);
        }
    }

    // Value not in MCV list. Assume uniform distribution among remaining values.
    // Remaining fraction = (1 - null_fraction) - sum(mcv frequencies) * (1 - null_fraction)
    double mcv_fraction = 0.0;
    for (const auto& mcv : stats.mcv_list) {
        mcv_fraction += mcv.frequency;
    }

    double remaining_fraction = (1.0 - stats.null_fraction) * (1.0 - mcv_fraction);

    // Remaining distinct values.
    auto mcv_distinct = static_cast<uint64_t>(stats.mcv_list.size());
    uint64_t remaining_distinct =
        stats.ndistinct > mcv_distinct ? stats.ndistinct - mcv_distinct : 1;

    return remaining_fraction / static_cast<double>(remaining_distinct);
}

/// Helper: estimate the fraction of histogram values <= value.
static double histogram_le_fraction(const std::vector<HistogramBucket>& histogram,
                                    const Value& value) {
    if (histogram.empty()) {
        return 0.5; // No histogram: assume 50%.
    }

    double total_count = 0;
    double le_count = 0;

    for (const auto& bucket : histogram) {
        total_count += static_cast<double>(bucket.count);

        auto cmp_upper = compare(value, bucket.upper);
        if (!cmp_upper) {
            continue;
        }

        if (*cmp_upper == std::strong_ordering::greater ||
            *cmp_upper == std::strong_ordering::equal) {
            // Value >= bucket upper: entire bucket is included.
            le_count += static_cast<double>(bucket.count);
            continue;
        }

        auto cmp_lower = compare(value, bucket.lower);
        if (!cmp_lower) {
            continue;
        }

        if (*cmp_lower == std::strong_ordering::less) {
            // Value < bucket lower: none of this bucket is included.
            continue;
        }

        // Value is within this bucket: interpolate linearly.
        // Assume uniform distribution within the bucket.
        // Fraction ≈ 0.5 (midpoint approximation for simplicity).
        le_count += static_cast<double>(bucket.count) * 0.5;
    }

    return total_count > 0 ? le_count / total_count : 0.5;
}

double estimate_range_selectivity(const ColumnStats& stats, BinaryOp op, const Value& value) {
    if (stats.ndistinct == 0) {
        return kDefaultSelectivity;
    }

    // Compute histogram fraction.
    double hist_le = histogram_le_fraction(stats.histogram, value);

    // MCV contribution: count MCV values that satisfy the predicate.
    double mcv_fraction = 0.0;
    double mcv_total_fraction = 0.0;
    for (const auto& mcv : stats.mcv_list) {
        mcv_total_fraction += mcv.frequency;
        auto cmp = compare(mcv.value, value);
        if (!cmp) {
            continue;
        }

        bool satisfies = false;
        switch (op) {
        case BinaryOp::LESS:
            satisfies = (*cmp == std::strong_ordering::less);
            break;
        case BinaryOp::LESS_EQUAL:
            satisfies = (*cmp == std::strong_ordering::less || *cmp == std::strong_ordering::equal);
            break;
        case BinaryOp::GREATER:
            satisfies = (*cmp == std::strong_ordering::greater);
            break;
        case BinaryOp::GREATER_EQUAL:
            satisfies =
                (*cmp == std::strong_ordering::greater || *cmp == std::strong_ordering::equal);
            break;
        default:
            break;
        }

        if (satisfies) {
            mcv_fraction += mcv.frequency;
        }
    }

    // Combine: histogram covers non-MCV portion, MCV covers MCV portion.
    double non_null = 1.0 - stats.null_fraction;
    double hist_portion = non_null * (1.0 - mcv_total_fraction);
    double mcv_portion = non_null * mcv_fraction;

    double selectivity = 0.0;
    switch (op) {
    case BinaryOp::LESS:
    case BinaryOp::LESS_EQUAL:
        selectivity = hist_portion * hist_le + mcv_portion;
        break;
    case BinaryOp::GREATER:
    case BinaryOp::GREATER_EQUAL:
        selectivity = hist_portion * (1.0 - hist_le) + mcv_portion;
        break;
    default:
        return kDefaultSelectivity;
    }

    return std::clamp(selectivity, 0.0, 1.0);
}

double estimate_between_selectivity(const ColumnStats& stats, const Value& low, const Value& high) {
    // BETWEEN is equivalent to (col >= low AND col <= high).
    double sel_ge = estimate_range_selectivity(stats, BinaryOp::GREATER_EQUAL, low);
    double sel_le = estimate_range_selectivity(stats, BinaryOp::LESS_EQUAL, high);

    // Approximate: P(low <= col <= high) ≈ P(col <= high) - P(col < low)
    // But since we have P(col >= low) and P(col <= high):
    // P(BETWEEN) = P(col >= low) + P(col <= high) - 1 + P(NULL)
    // Actually, more accurately: sel = sel_ge + sel_le - (1 - null_fraction)
    double sel = sel_ge + sel_le - (1.0 - stats.null_fraction);
    return std::clamp(sel, 0.0, 1.0);
}

double estimate_is_null_selectivity(const ColumnStats& stats) {
    return stats.null_fraction;
}

double estimate_is_not_null_selectivity(const ColumnStats& stats) {
    return 1.0 - stats.null_fraction;
}

} // namespace sixseven
