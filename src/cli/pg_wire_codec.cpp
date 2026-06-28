#include "sixseven/cli/pg_wire_codec.h"

#include <cstring>

namespace sixseven::cli {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

void push_be32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

int16_t read_be16(const uint8_t* p) {
    return static_cast<int16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

void push_cstring(std::vector<uint8_t>& buf, const std::string& s) {
    buf.insert(buf.end(), s.begin(), s.end());
    buf.push_back(0);
}

} // namespace

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

std::vector<uint8_t> encode_startup_message(const std::string& user, const std::string& database) {
    // Protocol version 3.0 = (3 << 16) | 0 = 196608.
    constexpr uint32_t PROTOCOL_V3 = 196608;

    // Build body: protocol version + key/value pairs + terminator.
    std::vector<uint8_t> body;
    push_be32(body, PROTOCOL_V3);
    push_cstring(body, "user");
    push_cstring(body, user);
    push_cstring(body, "database");
    push_cstring(body, database);
    body.push_back(0); // Terminating null byte.

    // Prepend 4-byte total length (including the length field itself).
    std::vector<uint8_t> msg;
    push_be32(msg, static_cast<uint32_t>(body.size() + 4));
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

std::vector<uint8_t> encode_query_message(const std::string& sql) {
    // 'Q' + int32 length (includes itself, not type byte) + sql + null.
    std::vector<uint8_t> msg;
    msg.push_back('Q');
    uint32_t len = static_cast<uint32_t>(4 + sql.size() + 1);
    push_be32(msg, len);
    msg.insert(msg.end(), sql.begin(), sql.end());
    msg.push_back(0);
    return msg;
}

std::vector<uint8_t> encode_terminate_message() {
    std::vector<uint8_t> msg;
    msg.push_back('X');
    push_be32(msg, 4); // Length = 4 (only the length field, no body).
    return msg;
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

Result<ServerMessage> decode_one_message(const std::vector<uint8_t>& buf, size_t& consumed) {
    // Every regular (post-startup) server message:
    //   byte type + int32 length (includes itself, not type byte) + body.
    if (buf.size() < 5) {
        return make_error(StatusCode::NOT_FOUND, "incomplete message header");
    }

    const uint8_t type = buf[0];
    const uint32_t msg_len = read_be32(buf.data() + 1); // includes 4-byte length field

    if (msg_len < 4) {
        return make_error(StatusCode::PARSE_ERROR, "message length < 4");
    }

    const size_t total = 1 + static_cast<size_t>(msg_len);
    if (buf.size() < total) {
        return make_error(StatusCode::NOT_FOUND, "incomplete message body");
    }

    // Body starts after the type byte + 4-byte length.
    const uint8_t* body = buf.data() + 5;
    const size_t body_len = msg_len - 4;

    ServerMessage msg;
    msg.raw_type = type;

    auto read_cstring_from = [](const uint8_t* start, size_t len, size_t& off) -> std::string {
        std::string s;
        while (off < len && start[off] != 0) {
            s += static_cast<char>(start[off]);
            ++off;
        }
        if (off < len) {
            ++off; // Skip null terminator.
        }
        return s;
    };

    switch (type) {
    case 'R': { // Authentication
        if (body_len < 4) {
            return make_error(StatusCode::PARSE_ERROR, "Authentication message too short");
        }
        msg.tag = ServerMsgTag::Authentication;
        msg.auth.auth_type = static_cast<int32_t>(read_be32(body));
        if (body_len > 4) {
            msg.auth.payload.assign(body + 4, body + body_len);
        }
        break;
    }
    case 'K': { // BackendKeyData
        if (body_len < 8) {
            return make_error(StatusCode::PARSE_ERROR, "BackendKeyData message too short");
        }
        msg.tag = ServerMsgTag::BackendKeyData;
        msg.backend_key.pid = static_cast<int32_t>(read_be32(body));
        msg.backend_key.secret_key = static_cast<int32_t>(read_be32(body + 4));
        break;
    }
    case 'S': { // ParameterStatus
        msg.tag = ServerMsgTag::ParameterStatus;
        size_t off = 0;
        msg.param_status.name = read_cstring_from(body, body_len, off);
        msg.param_status.value = read_cstring_from(body, body_len, off);
        break;
    }
    case 'T': { // RowDescription
        if (body_len < 2) {
            return make_error(StatusCode::PARSE_ERROR, "RowDescription message too short");
        }
        msg.tag = ServerMsgTag::RowDescription;
        const auto num_cols = static_cast<uint16_t>(read_be16(body));
        size_t off = 2;
        msg.row_desc.columns.reserve(num_cols);
        for (uint16_t i = 0; i < num_cols; ++i) {
            ColumnDesc col;
            col.name = read_cstring_from(body, body_len, off);
            if (off + 18 > body_len) {
                return make_error(StatusCode::PARSE_ERROR, "RowDescription column data truncated");
            }
            col.table_oid = static_cast<int32_t>(read_be32(body + off));
            off += 4;
            col.column_attr = read_be16(body + off);
            off += 2;
            col.type_oid = static_cast<int32_t>(read_be32(body + off));
            off += 4;
            col.type_size = read_be16(body + off);
            off += 2;
            col.type_modifier = static_cast<int32_t>(read_be32(body + off));
            off += 4;
            col.format_code = read_be16(body + off);
            off += 2;
            msg.row_desc.columns.push_back(std::move(col));
        }
        break;
    }
    case 'D': { // DataRow
        if (body_len < 2) {
            return make_error(StatusCode::PARSE_ERROR, "DataRow message too short");
        }
        msg.tag = ServerMsgTag::DataRow;
        const auto num_fields = static_cast<uint16_t>(read_be16(body));
        size_t off = 2;
        msg.data_row.fields.reserve(num_fields);
        for (uint16_t i = 0; i < num_fields; ++i) {
            if (off + 4 > body_len) {
                return make_error(StatusCode::PARSE_ERROR, "DataRow field length truncated");
            }
            auto field_len = static_cast<int32_t>(read_be32(body + off));
            off += 4;
            if (field_len == -1) {
                // SQL NULL.
                msg.data_row.fields.push_back(std::nullopt);
            } else {
                if (off + static_cast<size_t>(field_len) > body_len) {
                    return make_error(StatusCode::PARSE_ERROR, "DataRow field data truncated");
                }
                std::string val(reinterpret_cast<const char*>(body + off),
                                static_cast<size_t>(field_len));
                off += static_cast<size_t>(field_len);
                msg.data_row.fields.push_back(std::move(val));
            }
        }
        break;
    }
    case 'C': { // CommandComplete
        msg.tag = ServerMsgTag::CommandComplete;
        size_t off = 0;
        msg.cmd_complete.tag = read_cstring_from(body, body_len, off);
        break;
    }
    case 'E': { // ErrorResponse
        msg.tag = ServerMsgTag::ErrorResponse;
        size_t off = 0;
        while (off < body_len) {
            char field_type = static_cast<char>(body[off]);
            ++off;
            if (field_type == 0) {
                break; // Terminator.
            }
            std::string field_val = read_cstring_from(body, body_len, off);
            switch (field_type) {
            case 'S':
                msg.error_resp.severity = field_val;
                break;
            case 'C':
                msg.error_resp.sql_state = field_val;
                break;
            case 'M':
                msg.error_resp.message = field_val;
                break;
            case 'D':
                msg.error_resp.detail = field_val;
                break;
            case 'H':
                msg.error_resp.hint = field_val;
                break;
            default:
                break; // Ignore unknown fields.
            }
        }
        break;
    }
    case 'Z': { // ReadyForQuery
        if (body_len < 1) {
            return make_error(StatusCode::PARSE_ERROR, "ReadyForQuery message too short");
        }
        msg.tag = ServerMsgTag::ReadyForQuery;
        msg.ready.transaction_status = static_cast<char>(body[0]);
        break;
    }
    case 'N': { // NoticeResponse (same format as ErrorResponse)
        msg.tag = ServerMsgTag::NoticeResponse;
        size_t off = 0;
        while (off < body_len) {
            char field_type = static_cast<char>(body[off]);
            ++off;
            if (field_type == 0) {
                break;
            }
            std::string field_val = read_cstring_from(body, body_len, off);
            if (field_type == 'M') {
                msg.notice.message = field_val;
            }
        }
        break;
    }
    default:
        msg.tag = ServerMsgTag::Unknown;
        break;
    }

    consumed = total;
    return ok(std::move(msg));
}

} // namespace sixseven::cli
