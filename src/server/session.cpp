#include "sixseven/server/session.h"

#include "sixseven/common/logging.h"
#include "sixseven/executor/query_engine.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace sixseven {

namespace {

/// Convert a string to lowercase.
std::string to_lower(std::string_view sv) {
    std::string result(sv);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

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

/// Case-insensitive string prefix check.
bool starts_with_ci(std::string_view str, std::string_view prefix) {
    if (str.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(str[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

// -- Default session variable values ------------------------------------------

const std::unordered_map<std::string, std::string> Session::DEFAULT_VARIABLES = {
    {"work_mem", "4MB"},
    {"default_transaction_isolation", "read committed"},
    {"search_path", "public"},
    {"embedding_provider_url", ""},
    {"embedding_api_key", ""},
    {"statement_timeout", "0"},
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
    if (txn_state_ == TransactionState::IDLE) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "SAVEPOINT can only be used in a transaction block");
    }
    if (txn_state_ == TransactionState::FAILED) {
        return make_error(StatusCode::TXN_ABORTED,
                          "current transaction is aborted, commands ignored until end of "
                          "transaction block");
    }
    savepoints_.push_back(name);
    SIXSEVEN_LOG_DEBUG("session {}: SAVEPOINT {}", backend_pid_, name);
    return ok();
}

Result<void> Session::release_savepoint(const std::string& name) {
    if (txn_state_ == TransactionState::IDLE) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "RELEASE SAVEPOINT can only be used in a transaction block");
    }
    if (txn_state_ == TransactionState::FAILED) {
        return make_error(StatusCode::TXN_ABORTED,
                          "current transaction is aborted, commands ignored until end of "
                          "transaction block");
    }
    // Find the savepoint from the back (most recent first).
    for (auto it = savepoints_.rbegin(); it != savepoints_.rend(); ++it) {
        if (*it == name) {
            savepoints_.erase(std::next(it).base());
            SIXSEVEN_LOG_DEBUG("session {}: RELEASE SAVEPOINT {}", backend_pid_, name);
            return ok();
        }
    }
    return make_error(StatusCode::INVALID_ARGUMENT, "savepoint \"" + name + "\" does not exist");
}

Result<void> Session::rollback_to_savepoint(const std::string& name) {
    if (txn_state_ == TransactionState::IDLE) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "ROLLBACK TO SAVEPOINT can only be used in a transaction block");
    }
    // Find the savepoint from the back.
    for (auto it = savepoints_.rbegin(); it != savepoints_.rend(); ++it) {
        if (*it == name) {
            // Pop all savepoints after this one (keep the named one).
            auto forward_it = std::next(it).base();
            savepoints_.erase(std::next(forward_it), savepoints_.end());
            // If in FAILED state, recover to IN_TRANSACTION.
            if (txn_state_ == TransactionState::FAILED) {
                txn_state_ = TransactionState::IN_TRANSACTION;
                SIXSEVEN_LOG_DEBUG("session {}: ROLLBACK TO SAVEPOINT {} (recovered from FAILED)",
                                   backend_pid_,
                                   name);
            } else {
                SIXSEVEN_LOG_DEBUG("session {}: ROLLBACK TO SAVEPOINT {}", backend_pid_, name);
            }
            return ok();
        }
    }
    return make_error(StatusCode::INVALID_ARGUMENT, "savepoint \"" + name + "\" does not exist");
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

    if (trimmed == "begin" || trimmed == "start transaction") {
        if (success) {
            txn_state_ = TransactionState::IN_TRANSACTION;
        }
        return;
    }

    if (trimmed == "commit" || trimmed == "end") {
        if (success) {
            txn_state_ = TransactionState::IDLE;
            savepoints_.clear();
        }
        return;
    }

    if (trimmed == "rollback" || trimmed == "abort") {
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
