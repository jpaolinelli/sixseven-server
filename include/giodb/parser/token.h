#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace giodb {

/// All token types recognized by the SQL lexer.
///
/// Organized into groups: keywords, GioDB-specific keywords, operators,
/// punctuation, literals, and special tokens.
enum class TokenType : uint8_t {
    // -- SQL keywords (alphabetical) ------------------------------------------

    ALL,
    ALTER,
    ANALYZE,
    AND,
    AS,
    ASC,
    BEGIN,
    BETWEEN,
    BIGINT,
    BLOB_KW,
    BOOLEAN,
    BY,
    CASCADE,
    CASE,
    CHAR,
    CHECK,
    COLUMN,
    COMMIT,
    CONSTRAINT,
    COUNT,
    CREATE,
    CROSS,
    DATE,
    DECIMAL,
    DEFAULT,
    DELETE,
    DESC,
    DESCRIBE,
    DISTINCT,
    DOUBLE,
    DROP,
    ELSE,
    END,
    EXCEPT,
    EXISTS,
    EXPLAIN,
    FALSE_KW,
    FLOAT,
    FOREIGN,
    FROM,
    FULL,
    GROUP,
    HAVING,
    IF,
    IN,
    INDEX,
    INNER,
    INSERT,
    INT,
    INTEGER,
    INTERSECT,
    INTERVAL,
    INTO,
    IS,
    JOIN,
    JSON_KW,
    KEY,
    LEFT,
    LIKE,
    LIMIT,
    NOT,
    NULL_KW,
    NUMERIC,
    OFFSET,
    ON,
    OR,
    ORDER,
    OUTER,
    POINT_KW,
    PRIMARY,
    REFERENCES,
    RESTRICT,
    RETURNING,
    RIGHT,
    ROLLBACK,
    SAVEPOINT,
    SELECT,
    SET,
    SHOW,
    SMALLINT,
    TABLE,
    TEXT,
    THEN,
    TIME,
    TIMESTAMP,
    TINYINT,
    TRANSACTION,
    TRUE_KW,
    UNION,
    UNIQUE,
    UPDATE,
    UUID_KW,
    VACUUM,
    VALUES,
    VARCHAR,
    WHEN,
    WHERE,
    WITH,

    // -- Aggregate functions --------------------------------------------------

    AVG,
    MAX_KW,
    MIN_KW,
    SUM,

    // -- GioDB-specific keywords ----------------------------------------------

    DIRECTION,
    EDGE,
    EMBEDDING,
    FETCH,
    LINK,
    MATCH,
    MAX_DEPTH,
    NEAREST,
    PATH,
    RECURSIVE,
    REEMBED,
    RETURN,
    SHORTEST,
    TRAVERSE,
    TYPE,
    UNLINK,
    VIA,

    // -- Operators ------------------------------------------------------------

    PLUS,          // +
    MINUS,         // -
    STAR,          // *
    SLASH,         // /
    PERCENT,       // %
    EQUAL,         // =
    NOT_EQUAL,     // != or <>
    LESS,          // <
    GREATER,       // >
    LESS_EQUAL,    // <=
    GREATER_EQUAL, // >=
    PIPE_PIPE,     // ||  (string concatenation)
    COLON_COLON,   // ::  (type cast)

    // -- Punctuation ----------------------------------------------------------

    COMMA,     // ,
    SEMICOLON, // ;
    COLON,     // :
    DOT,       // .
    LPAREN,    // (
    RPAREN,    // )
    LBRACKET,  // [
    RBRACKET,  // ]

    // -- Literals -------------------------------------------------------------

    INTEGER_LITERAL,
    FLOAT_LITERAL,
    STRING_LITERAL,

    // -- Special --------------------------------------------------------------

    IDENTIFIER,
    END_OF_FILE,
};

/// Returns the human-readable name for a token type.
constexpr std::string_view token_type_name(TokenType type) {
    switch (type) {
    case TokenType::ALL:            return "ALL";
    case TokenType::ALTER:          return "ALTER";
    case TokenType::ANALYZE:        return "ANALYZE";
    case TokenType::AND:            return "AND";
    case TokenType::AS:             return "AS";
    case TokenType::ASC:            return "ASC";
    case TokenType::BEGIN:          return "BEGIN";
    case TokenType::BETWEEN:        return "BETWEEN";
    case TokenType::BIGINT:         return "BIGINT";
    case TokenType::BLOB_KW:        return "BLOB";
    case TokenType::BOOLEAN:        return "BOOLEAN";
    case TokenType::BY:             return "BY";
    case TokenType::CASCADE:        return "CASCADE";
    case TokenType::CASE:           return "CASE";
    case TokenType::CHAR:           return "CHAR";
    case TokenType::CHECK:          return "CHECK";
    case TokenType::COLUMN:         return "COLUMN";
    case TokenType::COMMIT:         return "COMMIT";
    case TokenType::CONSTRAINT:     return "CONSTRAINT";
    case TokenType::COUNT:          return "COUNT";
    case TokenType::CREATE:         return "CREATE";
    case TokenType::CROSS:          return "CROSS";
    case TokenType::DATE:           return "DATE";
    case TokenType::DECIMAL:        return "DECIMAL";
    case TokenType::DEFAULT:        return "DEFAULT";
    case TokenType::DELETE:         return "DELETE";
    case TokenType::DESC:           return "DESC";
    case TokenType::DESCRIBE:       return "DESCRIBE";
    case TokenType::DISTINCT:       return "DISTINCT";
    case TokenType::DOUBLE:         return "DOUBLE";
    case TokenType::DROP:           return "DROP";
    case TokenType::ELSE:           return "ELSE";
    case TokenType::END:            return "END";
    case TokenType::EXCEPT:         return "EXCEPT";
    case TokenType::EXISTS:         return "EXISTS";
    case TokenType::EXPLAIN:        return "EXPLAIN";
    case TokenType::FALSE_KW:       return "FALSE";
    case TokenType::FLOAT:          return "FLOAT";
    case TokenType::FOREIGN:        return "FOREIGN";
    case TokenType::FROM:           return "FROM";
    case TokenType::FULL:           return "FULL";
    case TokenType::GROUP:          return "GROUP";
    case TokenType::HAVING:         return "HAVING";
    case TokenType::IF:             return "IF";
    case TokenType::IN:             return "IN";
    case TokenType::INDEX:          return "INDEX";
    case TokenType::INNER:          return "INNER";
    case TokenType::INSERT:         return "INSERT";
    case TokenType::INT:            return "INT";
    case TokenType::INTEGER:        return "INTEGER";
    case TokenType::INTERSECT:      return "INTERSECT";
    case TokenType::INTERVAL:       return "INTERVAL";
    case TokenType::INTO:           return "INTO";
    case TokenType::IS:             return "IS";
    case TokenType::JOIN:           return "JOIN";
    case TokenType::JSON_KW:        return "JSON";
    case TokenType::KEY:            return "KEY";
    case TokenType::LEFT:           return "LEFT";
    case TokenType::LIKE:           return "LIKE";
    case TokenType::LIMIT:          return "LIMIT";
    case TokenType::NOT:            return "NOT";
    case TokenType::NULL_KW:        return "NULL";
    case TokenType::NUMERIC:        return "NUMERIC";
    case TokenType::OFFSET:         return "OFFSET";
    case TokenType::ON:             return "ON";
    case TokenType::OR:             return "OR";
    case TokenType::ORDER:          return "ORDER";
    case TokenType::OUTER:          return "OUTER";
    case TokenType::POINT_KW:       return "POINT";
    case TokenType::PRIMARY:        return "PRIMARY";
    case TokenType::REFERENCES:     return "REFERENCES";
    case TokenType::RESTRICT:       return "RESTRICT";
    case TokenType::RETURNING:      return "RETURNING";
    case TokenType::RIGHT:          return "RIGHT";
    case TokenType::ROLLBACK:       return "ROLLBACK";
    case TokenType::SAVEPOINT:      return "SAVEPOINT";
    case TokenType::SELECT:         return "SELECT";
    case TokenType::SET:            return "SET";
    case TokenType::SHOW:           return "SHOW";
    case TokenType::SMALLINT:       return "SMALLINT";
    case TokenType::TABLE:          return "TABLE";
    case TokenType::TEXT:           return "TEXT";
    case TokenType::THEN:           return "THEN";
    case TokenType::TIME:           return "TIME";
    case TokenType::TIMESTAMP:      return "TIMESTAMP";
    case TokenType::TINYINT:        return "TINYINT";
    case TokenType::TRANSACTION:    return "TRANSACTION";
    case TokenType::TRUE_KW:        return "TRUE";
    case TokenType::UNION:          return "UNION";
    case TokenType::UNIQUE:         return "UNIQUE";
    case TokenType::UPDATE:         return "UPDATE";
    case TokenType::UUID_KW:        return "UUID";
    case TokenType::VACUUM:         return "VACUUM";
    case TokenType::VALUES:         return "VALUES";
    case TokenType::VARCHAR:        return "VARCHAR";
    case TokenType::WHEN:           return "WHEN";
    case TokenType::WHERE:          return "WHERE";
    case TokenType::WITH:           return "WITH";
    case TokenType::AVG:            return "AVG";
    case TokenType::MAX_KW:         return "MAX";
    case TokenType::MIN_KW:         return "MIN";
    case TokenType::SUM:            return "SUM";
    case TokenType::DIRECTION:      return "DIRECTION";
    case TokenType::EDGE:           return "EDGE";
    case TokenType::EMBEDDING:      return "EMBEDDING";
    case TokenType::FETCH:          return "FETCH";
    case TokenType::LINK:           return "LINK";
    case TokenType::MATCH:          return "MATCH";
    case TokenType::MAX_DEPTH:      return "MAX_DEPTH";
    case TokenType::NEAREST:        return "NEAREST";
    case TokenType::PATH:           return "PATH";
    case TokenType::RECURSIVE:      return "RECURSIVE";
    case TokenType::REEMBED:        return "REEMBED";
    case TokenType::RETURN:         return "RETURN";
    case TokenType::SHORTEST:       return "SHORTEST";
    case TokenType::TRAVERSE:       return "TRAVERSE";
    case TokenType::TYPE:           return "TYPE";
    case TokenType::UNLINK:         return "UNLINK";
    case TokenType::VIA:            return "VIA";
    case TokenType::PLUS:           return "PLUS";
    case TokenType::MINUS:          return "MINUS";
    case TokenType::STAR:           return "STAR";
    case TokenType::SLASH:          return "SLASH";
    case TokenType::PERCENT:        return "PERCENT";
    case TokenType::EQUAL:          return "EQUAL";
    case TokenType::NOT_EQUAL:      return "NOT_EQUAL";
    case TokenType::LESS:           return "LESS";
    case TokenType::GREATER:        return "GREATER";
    case TokenType::LESS_EQUAL:     return "LESS_EQUAL";
    case TokenType::GREATER_EQUAL:  return "GREATER_EQUAL";
    case TokenType::PIPE_PIPE:      return "PIPE_PIPE";
    case TokenType::COLON_COLON:    return "COLON_COLON";
    case TokenType::COMMA:          return "COMMA";
    case TokenType::SEMICOLON:      return "SEMICOLON";
    case TokenType::COLON:          return "COLON";
    case TokenType::DOT:            return "DOT";
    case TokenType::LPAREN:         return "LPAREN";
    case TokenType::RPAREN:         return "RPAREN";
    case TokenType::LBRACKET:       return "LBRACKET";
    case TokenType::RBRACKET:       return "RBRACKET";
    case TokenType::INTEGER_LITERAL: return "INTEGER_LITERAL";
    case TokenType::FLOAT_LITERAL:  return "FLOAT_LITERAL";
    case TokenType::STRING_LITERAL: return "STRING_LITERAL";
    case TokenType::IDENTIFIER:     return "IDENTIFIER";
    case TokenType::END_OF_FILE:    return "END_OF_FILE";
    }
    return "UNKNOWN";
}

/// A single token produced by the lexer.
///
/// The lexeme is a view into the original SQL source string. The caller
/// must keep the source string alive for the lifetime of the token.
struct Token {
    TokenType type = TokenType::END_OF_FILE;
    std::string_view lexeme;
    uint32_t line = 1;
    uint32_t column = 1;
};

} // namespace giodb
