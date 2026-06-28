#include "sixseven/cli/repl.h"

#include <algorithm>
#include <iostream>

namespace sixseven::cli {

namespace {

// Trim whitespace from both ends of a string.
std::string trim(const std::string& s) {
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// Returns true if `stmt` ends with a semicolon (after trimming).
bool has_terminating_semicolon(const std::string& s) {
    auto t = trim(s);
    return !t.empty() && t.back() == ';';
}

// Print help text.
void print_help(std::ostream& out) {
    out << "Meta-commands:\n"
        << "  \\q          Quit\n"
        << "  \\help       Show this help\n"
        << "SQL statements must end with a semicolon (;) to be sent.\n";
}

} // namespace

Result<void>
run_repl(std::istream& in, std::ostream& out, const ExecFn& exec_fn, const ReplOptions& opts) {
    // One-shot mode: execute a single SQL statement and exit.
    if (!opts.one_shot.empty()) {
        std::string sql = trim(opts.one_shot);
        // Strip trailing semicolon if present (we add one for display, server
        // doesn't care either way, but keep it as-is for faithfulness).
        auto result = exec_fn(sql);
        if (!result) {
            out << "ERROR: " << result.error().message << "\n";
            return make_error(result.error().code, result.error().message);
        }
        return ok();
    }

    std::string accumulated; // Lines accumulated for the current statement.

    auto print_prompt = [&]() {
        if (opts.interactive) {
            if (accumulated.empty()) {
                out << "sixseven=> " << std::flush;
            } else {
                out << "sixseven-> " << std::flush;
            }
        }
    };

    print_prompt();

    std::string line;
    while (std::getline(in, line)) {
        std::string trimmed_line = trim(line);

        // Meta-commands (only at statement start, line starts with backslash).
        if (trimmed_line.size() > 0 && trimmed_line[0] == '\\') {
            if (trimmed_line == "\\q" || trimmed_line == "\\quit" || trimmed_line == "\\exit") {
                if (opts.interactive) {
                    out << "Bye!\n";
                }
                return ok();
            }
            if (trimmed_line == "\\help" || trimmed_line == "\\h" || trimmed_line == "\\?") {
                print_help(out);
            } else {
                out << "Unknown meta-command: " << trimmed_line << "  (\\help for help)\n";
            }
            print_prompt();
            continue;
        }

        // Accumulate the line.
        if (!accumulated.empty()) {
            accumulated += '\n';
        }
        accumulated += line;

        // Send when we see a semicolon.
        if (has_terminating_semicolon(accumulated)) {
            std::string sql = trim(accumulated);
            accumulated.clear();

            auto result = exec_fn(sql);
            if (!result) {
                // Transport/protocol failures abort the session.
                out << "FATAL: " << result.error().message << "\n";
                return make_error(result.error().code, result.error().message);
            }
        }

        print_prompt();
    }

    // EOF -- normal exit (piped input finished).
    return ok();
}

} // namespace sixseven::cli
