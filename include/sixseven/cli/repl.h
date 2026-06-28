#pragma once

// repl.h -- REPL / input driver for the sixseven-cli.
//
// Accumulates lines until a semicolon, handles meta-commands (\q, \help),
// and supports non-interactive -c "SQL" mode.

#include "sixseven/common/result.h"

#include <functional>
#include <istream>
#include <ostream>
#include <string>

namespace sixseven::cli {

/// Callback: given a complete SQL statement, execute it and write output.
/// Returns ok() on success (including server-returned SQL errors, which are
/// printed but are not a transport failure); returns error on I/O / protocol
/// failure that should abort the session.
using ExecFn = std::function<Result<void>(const std::string& sql)>;

struct ReplOptions {
    bool interactive{true}; // false when piped or -c mode
    std::string one_shot;   // non-empty: run this SQL then exit (-c flag)
};

/// Run the REPL loop.
/// Reads from `in`, writes prompts/output to `out`.
/// Calls `exec_fn` for each complete SQL statement.
/// Returns ok() when the session ends normally (\q or EOF).
[[nodiscard]] Result<void>
run_repl(std::istream& in, std::ostream& out, const ExecFn& exec_fn, const ReplOptions& opts);

} // namespace sixseven::cli
