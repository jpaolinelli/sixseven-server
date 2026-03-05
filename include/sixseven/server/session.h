#pragma once

#include "sixseven/common/result.h"
#include "sixseven/server/pg_protocol.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sixseven {

// Forward declarations.
struct QueryResult;

/// Transaction state for a session.
enum class TransactionState : uint8_t {
    IDLE,           ///< No active transaction ('I').
    IN_TRANSACTION, ///< Inside a transaction block ('T').
    FAILED,         ///< Inside a failed transaction block ('E').
};

/// Per-connection session state.
///
/// Owns session-scoped variables, SQL-level prepared statements, portals,
/// and transaction state.  Each client connection gets its own Session
/// instance so state is fully isolated between connections.
class Session {
public:
    explicit Session(int32_t backend_pid);

    // -- Session variables ----------------------------------------------------

    /// Set a session variable. Returns error if the variable is not a known
    /// built-in variable.
    [[nodiscard]] Result<void> set_variable(const std::string& name, const std::string& value);

    /// Get a session variable by name. Returns nullopt if the variable is not
    /// a known built-in.
    [[nodiscard]] std::optional<std::string> get_variable(const std::string& name) const;

    /// Reset a session variable to its default value. Returns error if not
    /// a known built-in.
    [[nodiscard]] Result<void> reset_variable(const std::string& name);

    /// Reset all session variables to their default values.
    void reset_all_variables();

    /// Get all session variables as (name, value) pairs.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> get_all_variables() const;

    /// Return true if the name is a known built-in session variable.
    [[nodiscard]] bool is_session_variable(const std::string& name) const;

    // -- Prepared statements (shared with wire protocol) ----------------------

    /// Store a prepared statement (SQL-level PREPARE or wire protocol Parse).
    void add_prepared_statement(const std::string& name, PreparedStatement stmt);

    /// Look up a prepared statement by name. Returns nullptr if not found.
    [[nodiscard]] const PreparedStatement* get_prepared_statement(const std::string& name) const;

    /// Remove a prepared statement by name.
    void remove_prepared_statement(const std::string& name);

    /// Remove all prepared statements.
    void remove_all_prepared_statements();

    /// Access prepared statements map (for testing / wire protocol).
    [[nodiscard]] const std::unordered_map<std::string, PreparedStatement>&
    prepared_statements() const;

    // -- Portals (wire protocol only) -----------------------------------------

    /// Store a portal (wire protocol Bind).
    void add_portal(const std::string& name, Portal portal);

    /// Look up a portal by name. Returns nullptr if not found.
    [[nodiscard]] const Portal* get_portal(const std::string& name) const;

    /// Remove a portal by name.
    void remove_portal(const std::string& name);

    /// Access portals map (for testing / wire protocol).
    [[nodiscard]] const std::unordered_map<std::string, Portal>& portals() const;

    // -- Transaction state ----------------------------------------------------

    [[nodiscard]] TransactionState transaction_state() const;
    void set_transaction_state(TransactionState state);

    /// Return the ReadyForQuery status byte: 'I', 'T', or 'E'.
    [[nodiscard]] char ready_for_query_status() const;

    // -- Session lifecycle ----------------------------------------------------

    /// Clean up all session resources (prepared statements, portals, variables).
    /// Called on disconnect.
    void cleanup();

    [[nodiscard]] int32_t backend_pid() const { return backend_pid_; }

    // -- Session command handling ----------------------------------------------

    /// Try to handle a SQL statement as a session-level command.
    /// Returns a QueryResult if handled, or nullopt if the statement should
    /// be passed to the query executor.
    [[nodiscard]] std::optional<Result<QueryResult>> try_handle_command(const std::string& sql);

    /// Update transaction state based on statement text and whether it succeeded.
    /// Called by the protocol handler after executing each statement.
    void update_transaction_state(const std::string& sql, bool success);

private:
    /// Try to handle a SET command.
    [[nodiscard]] std::optional<Result<QueryResult>> try_handle_set(const std::string& sql);

    /// Try to handle a SHOW command.
    [[nodiscard]] std::optional<Result<QueryResult>> try_handle_show(const std::string& sql);

    /// Try to handle a RESET command.
    [[nodiscard]] std::optional<Result<QueryResult>> try_handle_reset(const std::string& sql);

    /// Try to handle a PREPARE command.
    [[nodiscard]] std::optional<Result<QueryResult>> try_handle_prepare(const std::string& sql);

    /// Try to handle a DEALLOCATE command.
    [[nodiscard]] std::optional<Result<QueryResult>> try_handle_deallocate(const std::string& sql);

    int32_t backend_pid_;
    std::unordered_map<std::string, std::string> variables_;
    std::unordered_map<std::string, PreparedStatement> prepared_statements_;
    std::unordered_map<std::string, Portal> portals_;
    TransactionState txn_state_ = TransactionState::IDLE;

    /// Default values for built-in session variables.
    static const std::unordered_map<std::string, std::string> DEFAULT_VARIABLES;
};

} // namespace sixseven
