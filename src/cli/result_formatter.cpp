#include "sixseven/cli/result_formatter.h"

#include <algorithm>
#include <sstream>

namespace sixseven::cli {

namespace {

// Null marker displayed in the table.
constexpr const char* NULL_MARKER = "NULL";

} // namespace

std::string format_result_table(const std::vector<std::string>& col_names,
                                const std::vector<std::vector<std::optional<std::string>>>& rows,
                                const std::string& command_tag) {
    std::ostringstream out;

    if (col_names.empty()) {
        // No columns -- just print the command tag and row count.
        if (!command_tag.empty()) {
            out << command_tag << "\n";
        }
        return out.str();
    }

    const size_t ncols = col_names.size();

    // Compute column widths (at least as wide as the header).
    std::vector<size_t> widths(ncols);
    for (size_t c = 0; c < ncols; ++c) {
        widths[c] = col_names[c].size();
    }
    for (const auto& row : rows) {
        for (size_t c = 0; c < ncols && c < row.size(); ++c) {
            size_t cell_len = row[c].has_value() ? row[c]->size() : std::strlen(NULL_MARKER);
            widths[c] = std::max(widths[c], cell_len);
        }
    }

    // Header line: " col_a | col_b | ... "
    out << " ";
    for (size_t c = 0; c < ncols; ++c) {
        if (c > 0) {
            out << " | ";
        }
        const std::string& name = col_names[c];
        out << name;
        // Pad to column width.
        if (name.size() < widths[c]) {
            out << std::string(widths[c] - name.size(), ' ');
        }
    }
    out << "\n";

    // Separator: "-------+-------+..."
    for (size_t c = 0; c < ncols; ++c) {
        if (c > 0) {
            out << "+";
        }
        out << std::string(widths[c] + 2, '-');
    }
    out << "\n";

    // Data rows.
    for (const auto& row : rows) {
        out << " ";
        for (size_t c = 0; c < ncols; ++c) {
            if (c > 0) {
                out << " | ";
            }
            std::string cell;
            if (c < row.size()) {
                cell = row[c].has_value() ? *row[c] : NULL_MARKER;
            }
            out << cell;
            if (cell.size() < widths[c]) {
                out << std::string(widths[c] - cell.size(), ' ');
            }
        }
        out << "\n";
    }

    // Row count.
    size_t nrows = rows.size();
    out << "(" << nrows << (nrows == 1 ? " row)" : " rows)") << "\n";

    return out.str();
}

} // namespace sixseven::cli
