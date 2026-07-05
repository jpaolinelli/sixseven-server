#include "sixseven/server/session.h"

#include "sixseven/common/logging.h"
#include "sixseven/common/parse_utils.h"
#include "sixseven/common/string_util.h"
#include "sixseven/executor/query_engine.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace sixseven {

namespace {

/// Trim leading and trailing whitespace from a string.
std::string trim(std::string_view sv) {
    auto start = sv.find_first_not_of(" \t\n\r");
    if (start == std::string_view::npos) {
        return "";
    }
    auto end = sv.find_last_not_of(" \t\n\r");
    return std::string(sv.substr(start, end - start + 1));
}

/// Strip surrounding single quotes from a value if present.
std::string strip_quotes(const std::string& val) {
    if (val.size() >= 2 && val.front() == '\'' && val.back() == '\'') {
        return val.substr(1, val.size() - 2);
    }
    return val;
}

} // namespace

// -- Default session variable values ------------------------------------------
//
// Enforcement matrix (GDB-721). Each variable is either:
//   HONORED  — the value changes engine behavior;
//   INERT    — accepted and SHOWn for PostgreSQL driver compatibility
//              (psqlODBC, psql, JDBC issue these at connect time; erroring
//              would break clients, matching PG's tolerance), but the engine
//              does not consume the value yet.
//
//   statement_timeout              HONORED — armed as a thread-local deadline
//                                  (StatementDeadlineGuard) before each query
//                                  executor call; the QueryEngine drain loop
//                                  cancels the statement with SQLSTATE 57014
//                                  once the deadline passes. Value validated
//                                  at SET time (integer ms, or ms/s/min unit).
//   work_mem                       INERT — ExternalSort/HashJoin take memory
//                                  budgets, but the planner wires fixed
//                                  defaults; session plumbing is future work.
//   default_transaction_isolation  HONORED — validated at SET time; consumed
//                                  by QueryEngine::set_session_isolation()
//                                  (GDB-978). The value is stored here for
//                                  SHOW and driver compatibility.
//   search_path                    INERT — no schema namespaces.
//   embedding_provider_url         INERT — providers come from sys_providers
//                                  (ProviderCache), not a session URL.
//   embedding_api_key              INERT — same as above.
//   datestyle, client_encoding, extra_float_digits, application_name,
//   lc_messages, lc_monetary, lc_numeric, lc_time, timezone, intervalstyle,
//   standard_conforming_strings, bytea_output
//                                  INERT — PG-driver compatibility surface.

const std::unordered_map<std::string, std::string> Session::DEFAULT_VARIABLES = {
    {"work_mem", "4MB"},
    {"default_transaction_isolation", "read committed"},
    {"search_path", "public"},
    {"embedding_provider_url", ""},
    {"embedding_api_key", ""},
    {"statement_timeout", "0"},
    // PostgreSQL session variables expected by psqlODBC during connection setup.
    {"datestyle", "ISO, MDY"},
    {"client_encoding", "UTF8"},
    {"extra_float_digits", "3"},
    {"application_name", ""},
    {"lc_messages", "en_US.UTF-8"},
    {"lc_monetary", "en_US.UTF-8"},
    {"lc_numeric", "en_US.UTF-8"},
    {"lc_time", "en_US.UTF-8"},
    {"timezone", "UTC"},
    {"intervalstyle", "postgres"},
    {"standard_conforming_strings", "on"},
    {"bytea_output", "hex"},
};

// -- Construction -------------------------------------------------------------

Session::Session(int32_t backend_pid) : backend_pid_(backend_pid), variables_(DEFAULT_VARIABLES) {}

// -- Session variables --------------------------------------------------------

Result<void> Session::set_variable(const std::string& name, const std::string& value) {
    auto lower_name = to_lower(name);
    if (DEFAULT_VARIABLES.find(lower_name) == DEFAULT_VARIABLES.end()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "unrecognized session variable \"" + name + "\"");
    }
    // statement_timeout is honored by the engine, so reject values we could
    // not enforce instead of silently storing garbage.
    if (lower_name == "statement_timeout") {
        auto parsed = parse_timeout_ms(value);
        if (!parsed) {
            return make_error(parsed.error().code, parsed.error().message);
        }
    }
    // default_transaction_isolation is validated at SET time so drivers that
    // pass unsupported values get a clear error (GDB-978).
    if (lower_name == "default_transaction_isolation") {
        auto lv = to_lower(value);
        if (lv != "read committed" && lv != "snapshot isolation" && lv != "repeatable read" &&
            lv != "serializable") {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "invalid value for parameter \"default_transaction_isolation\": \"" +
                                  value +
                                  "\"; expected \"read committed\", \"snapshot isolation\", "
                                  "\"repeatable read\", or \"serializable\"");
        }
        if (isolation_change_cb_) {
            // Notify the engine (or any consumer) of the new level.
            isolation_change_cb_(lv);
        }
    }
    variables_[lower_name] = value;
    SIXSEVEN_LOG_DEBUG("session {}: SET {} = '{}'", backend_pid_, lower_name, value);
    return ok();
}

std::optional<std::string> Session::get_variable(const std::string& name) const {
    auto lower_name = to_lower(name);
    auto it = variables_.find(lower_name);
    if (it != variables_.end()) {
        return it->second;
    }
    return std::nullopt;
}

Result<void> Session::reset_variable(const std::string& name) {
    auto lower_name = to_lower(name);
    auto def_it = DEFAULT_VARIABLES.find(lower_name);
    if (def_it == DEFAULT_VARIABLES.end()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "unrecognized session variable \"" + name + "\"");
    }
    variables_[lower_name] = def_it->second;
    SIXSEVEN_LOG_DEBUG("session {}: RESET {}", backend_pid_, lower_name);
    return ok();
}

void Session::reset_all_variables() {
    variables_ = DEFAULT_VARIABLES;
    SIXSEVEN_LOG_DEBUG("session {}: RESET ALL", backend_pid_);
}

std::vector<std::pair<std::string, std::string>> Session::get_all_variables() const {
    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(variables_.size());
    for (const auto& [k, v] : variables_) {
        result.emplace_back(k, v);
    }
    // Sort by name for consistent output.
    std::sort(result.begin(), result.end());
    return result;
}

bool Session::is_session_variable(const std::string& name) const {
    return DEFAULT_VARIABLES.find(to_lower(name)) != DEFAULT_VARIABLES.end();
}

void Session::set_isolation_change_callback(IsolationChangeCallback cb) {
    isolation_change_cb_ = std::move(cb);
}

Result<int64_t> Session::parse_timeout_ms(const std::string& value) {
    auto trimmed = trim(value);
    if (trimmed.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "invalid value for parameter \"statement_timeout\": \"" + value + "\"");
    }
    size_t pos = 0;
    while (pos < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[pos]))) {
        ++pos;
    }
    if (pos == 0) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "invalid value for parameter \"statement_timeout\": \"" + value + "\"");
    }
    int64_t ms = 0;
    {
        auto pv = safe_stoll(trimmed.substr(0, pos));
        if (!pv) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "invalid value for parameter \"statement_timeout\": \"" + value +
                                  "\"");
        }
        ms = *pv;
    }
    auto unit = to_lower(trim(trimmed.substr(pos)));
    if (unit == "s") {
        ms *= 1000;
    } else if (unit == "min") {
        ms *= 60000;
    } else if (!unit.empty() && unit != "ms") {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "invalid value for parameter \"statement_timeout\": \"" + value + "\"");
    }
    return ok(ms);
}

int64_t Session::statement_timeout_ms() const {
    auto val = get_variable("statement_timeout");
    if (!val) {
        return 0;
    }
    auto parsed = parse_timeout_ms(*val);
    return parsed ? *parsed : 0;
}

// -- Prepared statements ------------------------------------------------------

void Session::add_prepared_statement(const std::string& name, PreparedStatement stmt) {
    prepared_statements_[name] = std::move(stmt);
}

const PreparedStatement* Session::get_prepared_statement(const std::string& name) const {
    auto it = prepared_statements_.find(name);
    return it != prepared_statements_.end() ? &it->second : nullptr;
}

void Session::remove_prepared_statement(const std::string& name) {
    prepared_statements_.erase(name);
}

void Session::remove_all_prepared_statements() {
    prepared_statements_.clear();
}

const std::unordered_map<std::string, PreparedStatement>& Session::prepared_statements() const {
    return prepared_statements_;
}

// -- Portals ------------------------------------------------------------------

void Session::add_portal(const std::string& name, Portal portal) {
    portals_[name] = std::move(portal);
}

const Portal* Session::get_portal(const std::string& name) const {
    auto it = portals_.find(name);
    return it != portals_.end() ? &it->second : nullptr;
}

Portal* Session::get_portal_mutable(const std::string& name) {
    auto it = portals_.find(name);
    return it != portals_.end() ? &it->second : nullptr;
}

void Session::remove_portal(const std::string& name) {
    portals_.erase(name);
}

const std::unordered_map<std::string, Portal>& Session::portals() const {
    return portals_;
}

// -- Transaction state --------------------------------------------------------

TransactionState Session::transaction_state() const {
    return txn_state_;
}

void Session::set_transaction_state(TransactionState state) {
    txn_state_ = state;
}

char Session::ready_for_query_status() const {
    switch (txn_state_) {
    case TransactionState::IDLE:
        return 'I';
    case TransactionState::IN_TRANSACTION:
        return 'T';
    case TransactionState::FAILED:
        return 'E';
    }
    return 'I'; // Unreachable.
}

// -- Savepoints ---------------------------------------------------------------

Result<void> Session::create_savepoint(const std::string& name) {
    // Subtransaction rollback is not implemented. Returning success here would
    // silently allow data written after this savepoint to be committed even when
    // the client issues ROLLBACK TO SAVEPOINT (audit finding C5, GDB-883).
    (void)name;
    return make_error(StatusCode::NOT_IMPLEMENTED,
                      "SAVEPOINT is not supported: subtransaction rollback is not implemented "
                      "(savepoints would not actually roll back data)");
}

Result<void> Session::release_savepoint(const std::string& name) {
    (void)name;
    return make_error(StatusCode::NOT_IMPLEMENTED,
                      "RELEASE SAVEPOINT is not supported: subtransaction rollback is not "
                      "implemented (savepoints would not actually roll back data)");
}

Result<void> Session::rollback_to_savepoint(const std::string& name) {
    // Returning success here would silently commit data that the client
    // intended to roll back (audit finding C5, GDB-883).
    (void)name;
    return make_error(StatusCode::NOT_IMPLEMENTED,
                      "ROLLBACK TO SAVEPOINT is not supported: subtransaction rollback is not "
                      "implemented (savepoints would not actually roll back data)");
}

const std::deque<std::string>& Session::savepoints() const {
    return savepoints_;
}

// -- Session lifecycle --------------------------------------------------------

void Session::cleanup() {
    SIXSEVEN_LOG_DEBUG("session {}: cleanup", backend_pid_);
    prepared_statements_.clear();
    portals_.clear();
    reset_all_variables();
    txn_state_ = TransactionState::IDLE;
    savepoints_.clear();
}

// -- Session command handling --------------------------------------------------

std::optional<Result<QueryResult>> Session::try_handle_command(const std::string& sql) {
    auto trimmed = trim(sql);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    // Check for session-level commands by prefix.
    if (starts_with_ci(trimmed, "SET ")) {
        return try_handle_set(trimmed);
    }
    if (starts_with_ci(trimmed, "SHOW ")) {
        return try_handle_show(trimmed);
    }
    if (starts_with_ci(trimmed, "RESET ")) {
        return try_handle_reset(trimmed);
    }
    if (starts_with_ci(trimmed, "PREPARE ")) {
        return try_handle_prepare(trimmed);
    }
    if (starts_with_ci(trimmed, "DEALLOCATE ")) {
        return try_handle_deallocate(trimmed);
    }

    // Handle savepoint commands at the session level.
    if (starts_with_ci(trimmed, "SAVEPOINT ")) {
        return try_handle_savepoint(trimmed);
    }
    if (starts_with_ci(trimmed, "RELEASE ")) {
        return try_handle_release_savepoint(trimmed);
    }
    if (starts_with_ci(trimmed, "ROLLBACK TO ")) {
        return try_handle_rollback_to(trimmed);
    }

    // Track transaction state for BEGIN/COMMIT/ROLLBACK.
    // These are not handled here (they pass to the query executor),
    // but we note the state change intent for update_transaction_state().

    return std::nullopt;
}

std::optional<Result<QueryResult>> Session::try_handle_set(const std::string& sql) {
    // Parse: SET name = value  or  SET name TO value
    // Skip the "SET " prefix (4 chars).
    auto rest = trim(sql.substr(4));
    if (rest.empty()) {
        return std::nullopt; // Let query executor handle malformed SET.
    }

    // Find '=' or 'TO' separator.
    std::string name;
    std::string value;

    auto eq_pos = rest.find('=');
    if (eq_pos != std::string::npos) {
        name = trim(rest.substr(0, eq_pos));
        value = trim(rest.substr(eq_pos + 1));
    } else {
        // Look for "TO" keyword.
        auto lower_rest = to_lower(rest);
        auto to_pos = lower_rest.find(" to ");
        if (to_pos == std::string::npos) {
            return std::nullopt; // Not a recognized SET pattern — pass through.
        }
        name = trim(rest.substr(0, to_pos));
        value = trim(rest.substr(to_pos + 4));
    }

    if (name.empty()) {
        return std::nullopt;
    }

    // Only handle known session variables.
    if (!is_session_variable(name)) {
        return std::nullopt; // Pass through to query executor.
    }

    value = strip_quotes(value);

    auto result = set_variable(name, value);
    if (!result) {
        return Result<QueryResult>(tl::unexpected(result.error()));
    }

    QueryResult qr;
    qr.message = "SET";
    return ok(std::move(qr));
}

std::optional<Result<QueryResult>> Session::try_handle_show(const std::string& sql) {
    // Parse: SHOW name  or  SHOW ALL
    auto rest = trim(sql.substr(5));
    if (rest.empty()) {
        return std::nullopt;
    }

    auto lower_rest = to_lower(rest);

    if (lower_rest == "all") {
        // Return all session variables.
        auto vars = get_all_variables();
        QueryResult qr;
        qr.column_names = {"name", "setting"};
        qr.column_types = {TypeId::STRING, TypeId::STRING};
        for (const auto& [k, v] : vars) {
            qr.rows.push_back({Value(k), Value(v)});
        }
        return ok(std::move(qr));
    }

    // Only handle known session variables.
    if (!is_session_variable(rest)) {
        return std::nullopt; // Pass through to query executor (SHOW TABLES, etc.).
    }

    auto val = get_variable(rest);
    if (!val) {
        return Result<QueryResult>(
            make_error(StatusCode::INVALID_ARGUMENT, "unrecognized variable \"" + rest + "\""));
    }

    QueryResult qr;
    qr.column_names = {lower_rest};
    qr.column_types = {TypeId::STRING};
    qr.rows.push_back({Value(*val)});
    return ok(std::move(qr));
}

std::optional<Result<QueryResult>> Session::try_handle_reset(const std::string& sql) {
    // Parse: RESET name  or  RESET ALL
    auto rest = trim(sql.substr(6));
    if (rest.empty()) {
        return std::nullopt;
    }

    auto lower_rest = to_lower(rest);

    if (lower_rest == "all") {
        reset_all_variables();
        QueryResult qr;
        qr.message = "RESET";
        return ok(std::move(qr));
    }

    // Only handle known session variables.
    if (!is_session_variable(rest)) {
        return std::nullopt;
    }

    auto result = reset_variable(rest);
    if (!result) {
        return Result<QueryResult>(tl::unexpected(result.error()));
    }

    QueryResult qr;
    qr.message = "RESET";
    return ok(std::move(qr));
}

std::optional<Result<QueryResult>> Session::try_handle_prepare(const std::string& sql) {
    // Parse: PREPARE name [(type, ...)] AS statement
    // Skip "PREPARE " (8 chars).
    auto rest = trim(sql.substr(8));
    if (rest.empty()) {
        return Result<QueryResult>(
            make_error(StatusCode::PARSE_ERROR, "syntax error: empty PREPARE statement"));
    }

    // Extract the statement name (first token).
    auto space_pos = rest.find_first_of(" \t(");
    if (space_pos == std::string::npos) {
        return Result<QueryResult>(
            make_error(StatusCode::PARSE_ERROR, "syntax error: PREPARE requires AS clause"));
    }

    std::string stmt_name = rest.substr(0, space_pos);
    auto after_name = trim(rest.substr(space_pos));

    // Parse optional parameter type list: (type, type, ...)
    std::vector<uint32_t> param_oids;
    if (!after_name.empty() && after_name[0] == '(') {
        auto close_paren = after_name.find(')');
        if (close_paren == std::string::npos) {
            return Result<QueryResult>(
                make_error(StatusCode::PARSE_ERROR, "syntax error: missing ')' in PREPARE"));
        }
        // Parse param types between parens — we store OIDs as 0 (unspecified).
        // Full type resolution would require the catalog; for now just count them.
        auto type_list = trim(after_name.substr(1, close_paren - 1));
        if (!type_list.empty()) {
            // Count comma-separated types.
            size_t count = 1;
            for (char c : type_list) {
                if (c == ',') {
                    ++count;
                }
            }
            param_oids.resize(count, 0);
        }
        after_name = trim(after_name.substr(close_paren + 1));
    }

    // Expect "AS" keyword.
    if (!starts_with_ci(after_name, "AS ") && !starts_with_ci(after_name, "AS\t") &&
        !starts_with_ci(after_name, "AS\n")) {
        return Result<QueryResult>(
            make_error(StatusCode::PARSE_ERROR, "syntax error: PREPARE requires AS clause"));
    }

    auto stmt_sql = trim(after_name.substr(3)); // Skip "AS " + trim.
    if (stmt_sql.empty()) {
        return Result<QueryResult>(
            make_error(StatusCode::PARSE_ERROR, "syntax error: PREPARE requires a statement"));
    }

    SIXSEVEN_LOG_DEBUG("session {}: PREPARE {} (sql='{}')", backend_pid_, stmt_name, stmt_sql);

    PreparedStatement stmt;
    stmt.name = stmt_name;
    stmt.sql = std::move(stmt_sql);
    stmt.param_oids = std::move(param_oids);
    add_prepared_statement(stmt_name, std::move(stmt));

    QueryResult qr;
    qr.message = "PREPARE";
    return ok(std::move(qr));
}

std::optional<Result<QueryResult>> Session::try_handle_deallocate(const std::string& sql) {
    // Parse: DEALLOCATE name  or  DEALLOCATE ALL
    auto rest = trim(sql.substr(11));
    if (rest.empty()) {
        return Result<QueryResult>(
            make_error(StatusCode::PARSE_ERROR, "syntax error: DEALLOCATE requires a name"));
    }

    auto lower_rest = to_lower(rest);
    if (lower_rest == "all") {
        remove_all_prepared_statements();
        QueryResult qr;
        qr.message = "DEALLOCATE ALL";
        return ok(std::move(qr));
    }

    if (get_prepared_statement(rest) == nullptr) {
        return Result<QueryResult>(make_error(
            StatusCode::INVALID_ARGUMENT, "prepared statement \"" + rest + "\" does not exist"));
    }

    remove_prepared_statement(rest);

    QueryResult qr;
    qr.message = "DEALLOCATE";
    return ok(std::move(qr));
}

std::optional<Result<QueryResult>> Session::try_handle_savepoint(const std::string& sql) {
    // Parse: SAVEPOINT name
    auto rest = trim(sql.substr(10)); // Skip "SAVEPOINT "
    if (rest.empty()) {
        return Result<QueryResult>(
            make_error(StatusCode::PARSE_ERROR, "syntax error: SAVEPOINT requires a name"));
    }

    auto result = create_savepoint(rest);
    if (!result) {
        return Result<QueryResult>(tl::unexpected(result.error()));
    }

    QueryResult qr;
    qr.message = "SAVEPOINT";
    return ok(std::move(qr));
}

std::optional<Result<QueryResult>> Session::try_handle_release_savepoint(const std::string& sql) {
    // Parse: RELEASE [SAVEPOINT] name
    auto rest = trim(sql.substr(8)); // Skip "RELEASE "
    if (rest.empty()) {
        return Result<QueryResult>(
            make_error(StatusCode::PARSE_ERROR, "syntax error: RELEASE requires SAVEPOINT name"));
    }

    // Skip optional SAVEPOINT keyword.
    if (starts_with_ci(rest, "SAVEPOINT ")) {
        rest = trim(rest.substr(10));
    }
    if (rest.empty()) {
        return Result<QueryResult>(
            make_error(StatusCode::PARSE_ERROR, "syntax error: RELEASE SAVEPOINT requires a name"));
    }

    auto result = release_savepoint(rest);
    if (!result) {
        return Result<QueryResult>(tl::unexpected(result.error()));
    }

    QueryResult qr;
    qr.message = "RELEASE";
    return ok(std::move(qr));
}

std::optional<Result<QueryResult>> Session::try_handle_rollback_to(const std::string& sql) {
    // Parse: ROLLBACK TO [SAVEPOINT] name
    auto rest = trim(sql.substr(12)); // Skip "ROLLBACK TO "
    if (rest.empty()) {
        return Result<QueryResult>(make_error(
            StatusCode::PARSE_ERROR, "syntax error: ROLLBACK TO requires a savepoint name"));
    }

    // Skip optional SAVEPOINT keyword.
    if (starts_with_ci(rest, "SAVEPOINT ")) {
        rest = trim(rest.substr(10));
    }
    if (rest.empty()) {
        return Result<QueryResult>(make_error(
            StatusCode::PARSE_ERROR, "syntax error: ROLLBACK TO SAVEPOINT requires a name"));
    }

    auto result = rollback_to_savepoint(rest);
    if (!result) {
        return Result<QueryResult>(tl::unexpected(result.error()));
    }

    QueryResult qr;
    qr.message = "ROLLBACK";
    return ok(std::move(qr));
}

void Session::update_transaction_state(const std::string& sql, bool success) {
    auto trimmed = to_lower(trim(sql));

    // Strip a single trailing statement terminator so "BEGIN;" is classified the
    // same as "BEGIN".
    if (!trimmed.empty() && trimmed.back() == ';') {
        trimmed = trim(trimmed.substr(0, trimmed.size() - 1));
    }

    // Classify by the leading keyword(s) rather than whole-string equality. The
    // parser accepts the optional TRANSACTION/WORK suffix and transaction modes
    // (e.g. "BEGIN TRANSACTION", "BEGIN WORK", "COMMIT TRANSACTION",
    // "START TRANSACTION ISOLATION LEVEL ..."), so equality on the full text
    // missed every spelling but the bare keyword and left ReadyForQuery status
    // desynchronized from the engine's actual transaction.
    const auto first_space = trimmed.find(' ');
    const std::string first =
        (first_space == std::string::npos) ? trimmed : trimmed.substr(0, first_space);
    const std::string rest =
        (first_space == std::string::npos) ? "" : trim(trimmed.substr(first_space + 1));
    const auto second_space = rest.find(' ');
    const std::string second =
        (second_space == std::string::npos) ? rest : rest.substr(0, second_space);

    // BEGIN [WORK|TRANSACTION] [mode...] or START TRANSACTION [mode...].
    const bool is_begin = (first == "begin") || (first == "start" && second == "transaction");
    // COMMIT/END [WORK|TRANSACTION].
    const bool is_commit = (first == "commit" || first == "end");
    // ROLLBACK/ABORT [WORK|TRANSACTION], but NOT "ROLLBACK TO <savepoint>", which
    // only unwinds to a savepoint and must leave the transaction open.
    const bool is_rollback = (first == "rollback" || first == "abort") && second != "to";

    if (is_begin) {
        if (success) {
            txn_state_ = TransactionState::IN_TRANSACTION;
        }
        return;
    }

    if (is_commit) {
        if (success) {
            txn_state_ = TransactionState::IDLE;
            savepoints_.clear();
        }
        return;
    }

    if (is_rollback) {
        // Rollback always resets to idle (even from FAILED state).
        txn_state_ = TransactionState::IDLE;
        savepoints_.clear();
        return;
    }

    // If we're in a transaction and a statement fails, move to FAILED.
    if (!success && txn_state_ == TransactionState::IN_TRANSACTION) {
        txn_state_ = TransactionState::FAILED;
    }
}

} // namespace sixseven
