#pragma once

#include "giodb/catalog/schema.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/index/rid.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace giodb {

// ---------------------------------------------------------------------------
// Tuple — executor row representation
// ---------------------------------------------------------------------------

/// A row of values flowing through the executor pipeline.
/// Each operator produces and consumes Tuples.
struct Tuple {
    std::vector<Value> values;
    std::optional<RID> rid; ///< Set for base-table tuples, empty for computed.
};

// ---------------------------------------------------------------------------
// OutputColumn / OutputSchema — logical column metadata for the executor
// ---------------------------------------------------------------------------

/// Metadata for a single column in an executor operator's output.
struct OutputColumn {
    std::string table_name;
    std::string name;
    TypeId type_id = TypeId::INT32;
    bool nullable = true;
    table_id_t table_id = 0;
};

/// Describes the logical output schema of an executor operator.
/// Unlike the byte-level `Schema` (used by TupleSerializer), this carries
/// column names and table provenance for expression evaluation.
class OutputSchema {
public:
    OutputSchema() = default;
    explicit OutputSchema(std::vector<OutputColumn> columns) : columns_(std::move(columns)) {}

    /// Number of output columns.
    [[nodiscard]] size_t column_count() const { return columns_.size(); }

    /// Access a column by index.
    [[nodiscard]] const OutputColumn& column(size_t index) const { return columns_[index]; }

    /// Access all columns.
    [[nodiscard]] const std::vector<OutputColumn>& columns() const { return columns_; }

    /// Find a column index by unqualified name.
    /// Returns nullopt if not found or if the name is ambiguous (appears in multiple tables).
    [[nodiscard]] std::optional<size_t> find_column(const std::string& name) const {
        std::optional<size_t> found;
        for (size_t i = 0; i < columns_.size(); ++i) {
            if (columns_[i].name == name) {
                if (found.has_value()) {
                    return std::nullopt; // ambiguous
                }
                found = i;
            }
        }
        return found;
    }

    /// Find a column index by qualified name (table.column).
    [[nodiscard]] std::optional<size_t> find_column(const std::string& table,
                                                    const std::string& col) const {
        for (size_t i = 0; i < columns_.size(); ++i) {
            if (columns_[i].table_name == table && columns_[i].name == col) {
                return i;
            }
        }
        return std::nullopt;
    }

private:
    std::vector<OutputColumn> columns_;
};

} // namespace giodb
