#pragma once

#include "giodb/common/result.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace giodb {

// -- Schema types -------------------------------------------------------------

/// Describes a single column in a table schema.
struct ColumnDef {
    std::string name;
    TypeId type;
};

/// A lightweight schema describing the columns of a table.
/// Used by TupleSerializer to compute offsets and interpret tuple data.
class Schema {
public:
    Schema() = default;
    explicit Schema(std::vector<ColumnDef> columns);

    /// Number of columns.
    [[nodiscard]] size_t column_count() const { return columns_.size(); }

    /// Access column definition by index.
    [[nodiscard]] const ColumnDef& column(size_t index) const { return columns_[index]; }

    /// Access all columns.
    [[nodiscard]] const std::vector<ColumnDef>& columns() const { return columns_; }

    /// Number of bytes in the null bitmap (ceil(column_count / 8)).
    [[nodiscard]] size_t null_bitmap_size() const;

    /// Number of fixed-size columns.
    [[nodiscard]] size_t fixed_column_count() const { return fixed_count_; }

    /// Number of variable-length columns.
    [[nodiscard]] size_t variable_column_count() const { return var_count_; }

    /// Get the byte offset of a fixed-size field within the fixed-fields region.
    /// Returns nullopt if the column is variable-length.
    [[nodiscard]] std::optional<size_t> fixed_field_offset(size_t col_index) const;

    /// Total size of the fixed-fields region in bytes.
    [[nodiscard]] size_t fixed_region_size() const { return fixed_region_size_; }

    /// Get the index of a variable-length column within the var-length offset table.
    /// Returns nullopt if the column is fixed-size.
    [[nodiscard]] std::optional<size_t> var_field_index(size_t col_index) const;

private:
    std::vector<ColumnDef> columns_;

    // Precomputed layout info.
    size_t fixed_count_ = 0;
    size_t var_count_ = 0;
    size_t fixed_region_size_ = 0;

    // Per-column: offset within fixed region (for fixed cols) or index in var table (for var cols).
    // For fixed columns: stores byte offset. For variable columns: stores var index.
    std::vector<size_t> col_offsets_;
    // Per-column: true if fixed-size, false if variable-length.
    std::vector<bool> col_is_fixed_;
};

// -- TupleSerializer ----------------------------------------------------------

/// Size of each variable-length offset/length pair in the tuple.
/// Each entry is: (offset: uint16_t, length: uint16_t) = 4 bytes.
static constexpr size_t var_entry_size = 4;

/// Maximum total tuple size supported by the uint16_t offset scheme.
static constexpr size_t max_tuple_size = UINT16_MAX;

/// Serializes and deserializes rows of Values into compact tuple byte format.
///
/// Tuple layout:
///   [null_bitmap | fixed_fields | var_length_offsets | var_length_data]
///
/// - null_bitmap: ceil(N/8) bytes, 1 bit per column. Bit set = column is NULL.
/// - fixed_fields: fixed-size columns stored at precomputed offsets.
/// - var_length_offsets: one (offset: u16, length: u16) pair per variable-length column.
///   Offsets are relative to the start of the tuple.
/// - var_length_data: variable-length data packed sequentially.
///
/// Callers must route values exceeding the overflow threshold through the
/// OverflowManager before calling serialize(). The uint16_t offset scheme
/// limits individual variable-length values and total tuple size to 65,535 bytes.
namespace TupleSerializer {

/// Serialize a row of Values into a compact byte buffer according to the schema.
/// The values vector must have exactly schema.column_count() elements.
/// Returns INVALID_ARGUMENT if the total tuple size exceeds max_tuple_size (65535).
Result<std::vector<uint8_t>> serialize(const std::vector<Value>& values, const Schema& schema);

/// Deserialize a tuple byte buffer into a vector of Values according to the schema.
Result<std::vector<Value>> deserialize(std::span<const uint8_t> data, const Schema& schema);

/// Extract a single field from a tuple without full deserialization.
/// Returns the Value at col_index.
Result<Value> get_field(std::span<const uint8_t> data, const Schema& schema, size_t col_index);

/// Compute the serialized size of a row of Values for the given schema.
/// Does NOT include a null flag byte per value (unlike the standalone serialization functions).
size_t compute_tuple_size(const std::vector<Value>& values, const Schema& schema);

} // namespace TupleSerializer

} // namespace giodb
