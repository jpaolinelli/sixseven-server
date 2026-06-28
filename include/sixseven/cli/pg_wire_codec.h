#pragma once

// pg_wire_codec.h -- Pure (socket-free) PostgreSQL wire protocol v3 codec.
//
// Encodes client->server messages and decodes server->client messages.
// No I/O, no sockets.  All functions operate on byte vectors.

#include "sixseven/common/result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sixseven::cli {

// ---------------------------------------------------------------------------
// Encoded message types (client->server)
// ---------------------------------------------------------------------------

/// Encode a StartupMessage: 4-byte total length, 4-byte protocol version
/// (196608 = 3.0), then null-terminated key/value pairs + terminating 0.
[[nodiscard]] std::vector<uint8_t> encode_startup_message(const std::string& user,
                                                          const std::string& database);

/// Encode a simple Query message: 'Q' + int32 len + sql + null terminator.
[[nodiscard]] std::vector<uint8_t> encode_query_message(const std::string& sql);

/// Encode a Terminate message: 'X' + int32 len (4).
[[nodiscard]] std::vector<uint8_t> encode_terminate_message();

// ---------------------------------------------------------------------------
// Decoded message types (server->client)
// ---------------------------------------------------------------------------

struct ColumnDesc {
    std::string name;
    int32_t table_oid{0};
    int16_t column_attr{0};
    int32_t type_oid{0};
    int16_t type_size{0};
    int32_t type_modifier{-1};
    int16_t format_code{0};
};

struct RowDescriptionMsg {
    std::vector<ColumnDesc> columns;
};

struct DataRowMsg {
    /// Each field: nullopt = SQL NULL, otherwise the text value.
    std::vector<std::optional<std::string>> fields;
};

struct CommandCompleteMsg {
    std::string tag; // e.g. "SELECT 3", "INSERT 0 1"
};

struct ErrorResponseMsg {
    std::string severity;  // e.g. "ERROR", "FATAL"
    std::string sql_state; // 5-char SQLSTATE
    std::string message;   // human-readable
    std::string detail;
    std::string hint;
};

struct ReadyForQueryMsg {
    char transaction_status{'I'}; // 'I'=idle, 'T'=in tx, 'E'=failed tx
};

struct AuthenticationMsg {
    int32_t auth_type{0};         // 0=OK, 3=cleartext, 5=MD5, 10=SASL
    std::vector<uint8_t> payload; // salt for MD5, SASL data, etc.
};

struct BackendKeyDataMsg {
    int32_t pid{0};
    int32_t secret_key{0};
};

struct ParameterStatusMsg {
    std::string name;
    std::string value;
};

struct NoticeResponseMsg {
    std::string message;
};

/// Tagged union for all server messages the client cares about.
enum class ServerMsgTag {
    Authentication,
    BackendKeyData,
    ParameterStatus,
    RowDescription,
    DataRow,
    CommandComplete,
    ErrorResponse,
    ReadyForQuery,
    NoticeResponse,
    Unknown,
};

struct ServerMessage {
    ServerMsgTag tag{ServerMsgTag::Unknown};
    uint8_t raw_type{0};

    AuthenticationMsg auth;
    BackendKeyDataMsg backend_key;
    ParameterStatusMsg param_status;
    RowDescriptionMsg row_desc;
    DataRowMsg data_row;
    CommandCompleteMsg cmd_complete;
    ErrorResponseMsg error_resp;
    ReadyForQueryMsg ready;
    NoticeResponseMsg notice;
};

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

/// Attempt to parse one complete server message from the front of `buf`.
/// On success: returns the parsed message and sets `consumed` to how many
///             bytes were consumed.
/// On incomplete: returns an error with StatusCode::NOT_FOUND to signal
///                "need more data" (the caller should keep reading).
/// On protocol error: returns a hard error.
[[nodiscard]] Result<ServerMessage> decode_one_message(const std::vector<uint8_t>& buf,
                                                       size_t& consumed);

} // namespace sixseven::cli
