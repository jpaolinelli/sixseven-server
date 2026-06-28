#pragma once

// result_formatter.h -- Pure ASCII table formatter for query results.

#include <optional>
#include <string>
#include <vector>

namespace sixseven::cli {

/// Format a query result as a psql-style ASCII table.
///
///  col_a | col_b
/// -------+-------
///  hello |   42
///  world | NULL
/// (2 rows)
///
/// NULL cells are shown as the string "NULL".
/// If col_names is empty, only the row count line is printed.
[[nodiscard]] std::string
format_result_table(const std::vector<std::string>& col_names,
                    const std::vector<std::vector<std::optional<std::string>>>& rows,
                    const std::string& command_tag);

} // namespace sixseven::cli
