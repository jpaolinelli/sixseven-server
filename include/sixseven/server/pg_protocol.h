#pragma once

#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/server/auth.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sixseven {

// Forward declarations.
class Connection;
class Session;
struct ColumnDescription;
struct QueryResult;

// -- PostgreSQL type OIDs -----------------------------------------------------

/// Map a SixSevenDB TypeId to its closest PostgreSQL type OID.
uint32_t type_to_pg_oid(TypeId type);

/// Map a PostgreSQL OID back to a SixSevenDB TypeId.
/// Returns INVALID_ARGUMENT error for unknown OIDs.
Result<TypeId> pg_oid_to_type(uint32_t oid);

// -- SQLSTATE mapping ---------------------------------------------------------

/// Map a SixSevenDB StatusCode to a five-character PostgreSQL SQLSTATE code.
std::string_view status_to_sqlstate(StatusCode code);

// -- Value text formatting ----------------------------------------------------

/// Convert a Value to its PostgreSQL text-format representation.
std::string value_to_pg_text(const Value& value);

/// Convert a Value to its PostgreSQL binary-format representation.
/// Returns raw bytes for supported types (BOOL, INT8–INT64, UINT8–UINT32,
/// FLOAT32, FLOAT64, STRING). Unsupported types fall back to text.
std::vector<uint8_t> value_to_pg_binary(const Value& value);

// -- Parameter substitution ---------------------------------------------------

/// Substitute $1, $2, ... placeholders in SQL with parameter values.
/// Uses param_oids to determine quoting: numeric types are unquoted,
/// string-like types are single-quoted with escaping.
/// NULL parameters (nullopt) are substituted as the literal SQL NULL.
[[nodiscard]] Result<std::string>
substitute_parameters(const std::string& sql,
                      const std::vector<std::optional<std::string>>& param_values,
                      const std::vector<uint32_t>& param_oids);

// -- SQL splitting ------------------------------------------------------------

/// Split a SQL string on semicolons into individual statements.
/// Respects single-quoted ('...') and double-quoted ("...") strings,
/// and dollar-quoted ($$...$$) strings so that semicolons inside literals
/// are not treated as separators.  Leading/trailing whitespace is trimmed
/// from each statement and empty statements are dropped.
std::vector<std::string> split_sql_statements(std::string_view sql);

// -- Message writer -----------------------------------------------------------

/// Builds a single PostgreSQL wire-protocol backend message.
class MessageWriter {
public:
    /// Begin a message with the given type byte.
    void begin_message(uint8_t type);

    /// Begin a message with no type byte (for startup-phase responses).
    void begin_message_no_type();

    void write_byte(uint8_t val);
    void write_int16(int16_t val);
    void write_int32(int32_t val);

    /// Write a null-terminated string.
    void write_cstring(std::string_view str);

    /// Write raw bytes (no length prefix, no null terminator).
    void write_bytes(const uint8_t* data, size_t len);

    /// Finalize the message: patch the 4-byte length field and return the bytes.
    std::vector<uint8_t> finish();

private:
    std::vector<uint8_t> buf_;
    size_t length_offset_ = 0;
    bool has_type_ = false;
};

// -- Message reader -----------------------------------------------------------

/// Reads fields from a single PostgreSQL wire-protocol message payload.
/// The caller is responsible for framing (extracting the type byte and length).
class MessageReader {
public:
    MessageReader(const uint8_t* data, size_t len);

    uint8_t read_byte();
    int16_t read_int16();
    int32_t read_int32();

    /// Read a null-terminated C string. Returns the string (without terminator).
    std::string_view read_cstring();

    /// Read exactly `len` raw bytes.
    const uint8_t* read_bytes(size_t len);

    bool has_remaining() const;
    size_t remaining() const;

private:
    const uint8_t* data_;
    size_t len_;
    size_t pos_ = 0;
};

// -- Protocol handler ---------------------------------------------------------

/// Callback type for executing SQL. Provided by the server.
/// The second parameter is the database name from the client's startup message.
using QueryExecutor =
    std::function<Result<QueryResult>(const std::string& sql, const std::string& database)>;

/// Callback type for describing SQL result columns without executing.
/// Returns column metadata (names + types) for SELECT statements,
/// or an empty vector for non-SELECT statements.
using QueryDescriber =
    std::function<Result<std::vector<ColumnDescription>>(const std::string& sql,
                                                         const std::string& database)>;

/// Protocol state machine phases.
enum class ProtocolState : uint8_t {
    WAIT_FOR_STARTUP,
    WAIT_FOR_AUTH,
    READY,
    CLOSED,
};

/// A parsed (prepared) statement stored by name.
struct PreparedStatement {
    std::string name;
    std::string sql;
    std::vector<uint32_t> param_oids;
};

/// A bound portal ready for execution.
struct Portal {
    std::string name;
    std::string sql;
    std::vector<std::optional<std::string>> param_values; ///< nullopt = SQL NULL.
    std::vector<uint32_t> param_oids;                     ///< Type OIDs from Parse.
    std::vector<int16_t> result_format_codes;
};

/// Per-connection PostgreSQL v3 protocol handler.
///
/// Reads from the connection's read buffer, processes complete messages, writes
/// response messages to the connection's write buffer, and consumes processed
/// bytes from the read buffer.
///
/// Each handler owns a Session that holds per-connection state: session
/// variables, prepared statements, portals, and transaction state.
class PgProtocolHandler {
public:
    explicit PgProtocolHandler(int32_t backend_pid);
    ~PgProtocolHandler();

    PgProtocolHandler(PgProtocolHandler&&) noexcept;
    PgProtocolHandler& operator=(PgProtocolHandler&&) noexcept;

    /// Set the callback used to execute SQL queries.
    void set_query_executor(QueryExecutor executor);

    /// Set the callback used to describe SQL result columns without executing.
    void set_query_describer(QueryDescriber describer);

    /// Set the authentication method and user manager for this handler.
    void set_auth(AuthMethod method, UserManager* user_mgr);

    /// Process available data in the connection's read buffer.
    /// Handles message framing (partial-read reassembly) internally.
    /// Returns ok() when all complete messages have been processed,
    /// or an error on a fatal protocol violation.
    Result<void> process(Connection& conn);

    ProtocolState state() const { return state_; }
    int32_t backend_pid() const { return backend_pid_; }
    int32_t secret_key() const { return secret_key_; }

    /// Access the session (for testing and external access).
    Session& session() { return *session_; }
    const Session& session() const { return *session_; }

    /// Access prepared statements (for testing — delegates to session).
    const std::unordered_map<std::string, PreparedStatement>& prepared_statements() const;

    /// Access portals (for testing — delegates to session).
    const std::unordered_map<std::string, Portal>& portals() const;

private:
    /// Try to parse and handle one complete message from the read buffer.
    /// Returns the number of bytes consumed, or 0 if a complete message
    /// is not yet available, or error on protocol violation.
    Result<size_t> process_one_message(Connection& conn);

    /// Handle the initial startup message (no type byte).
    Result<size_t> handle_startup_message(Connection& conn);

    /// Handle a typed frontend message (Query, Terminate, etc.).
    Result<size_t> handle_frontend_message(Connection& conn);

    /// Handle a frontend message during the WAIT_FOR_AUTH phase.
    Result<size_t> handle_auth_message(Connection& conn);

    // -- Simple query protocol --

    void handle_simple_query(Connection& conn, std::string_view sql);

    /// Send a QueryResult to the client (shared by simple query and EXECUTE).
    void send_query_result(Connection& conn, const QueryResult& qr);

    /// Try to handle SQL-level EXECUTE command.
    /// Returns an optional<Result<void>>: present if handled, nullopt to pass through.
    std::optional<Result<void>> try_handle_execute(Connection& conn, const std::string& sql);

    // -- Extended query protocol --

    void handle_parse(Connection& conn, const uint8_t* payload, size_t len);
    void handle_bind(Connection& conn, const uint8_t* payload, size_t len);
    void handle_describe(Connection& conn, const uint8_t* payload, size_t len);
    void handle_execute(Connection& conn, const uint8_t* payload, size_t len);
    void handle_sync(Connection& conn);
    void handle_close(Connection& conn, const uint8_t* payload, size_t len);

    // -- Message senders (write to connection's write buffer) --

    void send_auth_ok(Connection& conn);
    void send_parameter_status(Connection& conn, std::string_view name, std::string_view value);
    void send_backend_key_data(Connection& conn);
    void send_ready_for_query(Connection& conn, char status);
    void send_row_description(Connection& conn, const QueryResult& result);
    void send_row_description(Connection& conn,
                              const std::vector<ColumnDescription>& columns,
                              const std::vector<int16_t>& format_codes);
    void send_data_row(Connection& conn,
                       const std::vector<Value>& row,
                       const std::vector<TypeId>& types,
                       const std::vector<int16_t>& format_codes);
    void send_command_complete(Connection& conn, const std::string& tag);
    void send_error_response(Connection& conn,
                             std::string_view severity,
                             std::string_view sqlstate,
                             std::string_view message);
    void send_empty_query_response(Connection& conn);
    void send_parse_complete(Connection& conn);
    void send_bind_complete(Connection& conn);
    void send_close_complete(Connection& conn);
    void send_no_data(Connection& conn);
    void send_parameter_description(Connection& conn, const std::vector<uint32_t>& param_oids);

    // -- Auth message senders --

    void send_auth_md5_password(Connection& conn, const std::array<uint8_t, 4>& salt);
    void send_auth_sasl(Connection& conn);
    void send_auth_sasl_continue(Connection& conn, const std::string& data);
    void send_auth_sasl_final(Connection& conn, const std::string& data);

    /// Return the database name from the client's startup message (empty if none).
    [[nodiscard]] std::string startup_database() const;

    /// Complete the post-auth handshake (parameter status, backend key, ready).
    void complete_startup(Connection& conn);

    ProtocolState state_ = ProtocolState::WAIT_FOR_STARTUP;
    QueryExecutor query_executor_;
    QueryDescriber query_describer_;
    int32_t backend_pid_;
    int32_t secret_key_;

    /// Per-connection session state (variables, prepared stmts, txn state).
    std::unique_ptr<Session> session_;

    /// True when an error occurred during an extended query batch.
    /// Messages are skipped until the next Sync.
    bool error_in_extended_ = false;

    /// Startup parameters sent by the client (user, database, etc.).
    std::unordered_map<std::string, std::string> startup_params_;

    // -- Authentication state --

    AuthMethod auth_method_ = AuthMethod::TRUST;
    UserManager* user_mgr_ = nullptr;
    std::array<uint8_t, 4> md5_salt_{};
    std::optional<ScramServerState> scram_state_;
    bool scram_first_done_ = false; ///< True after server-first-message sent.
};

} // namespace sixseven
