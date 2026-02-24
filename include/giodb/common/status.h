#pragma once

#include <string>

namespace giodb {

enum class StatusCode {
    OK,
    NOT_FOUND,
    ALREADY_EXISTS,
    INVALID_ARGUMENT,
    INTERNAL_ERROR,
    NOT_IMPLEMENTED,
    IO_ERROR,
    PARSE_ERROR,
    TYPE_ERROR,
    CONSTRAINT_VIOLATION,
    TXN_CONFLICT,
    TXN_ABORTED,
    NETWORK_ERROR,
    AUTH_ERROR,
    REPLICATION_ERROR,
    READ_ONLY,
    LOCK_TIMEOUT,
    DEADLOCK,
};

/// Return a human-readable name for a StatusCode.
inline const char* status_code_name(StatusCode code) {
    switch (code) {
    case StatusCode::OK:
        return "OK";
    case StatusCode::NOT_FOUND:
        return "NOT_FOUND";
    case StatusCode::ALREADY_EXISTS:
        return "ALREADY_EXISTS";
    case StatusCode::INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case StatusCode::INTERNAL_ERROR:
        return "INTERNAL_ERROR";
    case StatusCode::NOT_IMPLEMENTED:
        return "NOT_IMPLEMENTED";
    case StatusCode::IO_ERROR:
        return "IO_ERROR";
    case StatusCode::PARSE_ERROR:
        return "PARSE_ERROR";
    case StatusCode::TYPE_ERROR:
        return "TYPE_ERROR";
    case StatusCode::CONSTRAINT_VIOLATION:
        return "CONSTRAINT_VIOLATION";
    case StatusCode::TXN_CONFLICT:
        return "TXN_CONFLICT";
    case StatusCode::TXN_ABORTED:
        return "TXN_ABORTED";
    case StatusCode::NETWORK_ERROR:
        return "NETWORK_ERROR";
    case StatusCode::AUTH_ERROR:
        return "AUTH_ERROR";
    case StatusCode::REPLICATION_ERROR:
        return "REPLICATION_ERROR";
    case StatusCode::READ_ONLY:
        return "READ_ONLY";
    case StatusCode::LOCK_TIMEOUT:
        return "LOCK_TIMEOUT";
    case StatusCode::DEADLOCK:
        return "DEADLOCK";
    }
    return "UNKNOWN";
}

} // namespace giodb
