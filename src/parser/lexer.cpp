#include "sixseven/parser/lexer.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace sixseven {

namespace {

/// Map of uppercase keyword strings to their token types.
/// Used for case-insensitive keyword lookup after scanning an identifier.
const std::unordered_map<std::string, TokenType>& keyword_map() {
    static const std::unordered_map<std::string, TokenType> map = {
        // SQL keywords
        {"ALL", TokenType::ALL},
        {"ALTER", TokenType::ALTER},
        {"ANALYZE", TokenType::ANALYZE},
        {"ANY", TokenType::ANY},
        {"AND", TokenType::AND},
        {"AS", TokenType::AS},
        {"ASC", TokenType::ASC},
        {"AUTOINCREMENT", TokenType::AUTOINCREMENT},
        {"AVG", TokenType::AVG},
        {"BEGIN", TokenType::BEGIN},
        {"BETWEEN", TokenType::BETWEEN},
        {"BIGINT", TokenType::BIGINT},
        {"BLOB", TokenType::BLOB_KW},
        {"BOOL", TokenType::BOOLEAN},
        {"BOOLEAN", TokenType::BOOLEAN},
        {"BY", TokenType::BY},
        {"CASCADE", TokenType::CASCADE},
        {"CASE", TokenType::CASE},
        {"CHAR", TokenType::CHAR},
        {"CHECK", TokenType::CHECK},
        {"COLUMN", TokenType::COLUMN},
        {"COMMIT", TokenType::COMMIT},
        {"CONSTRAINT", TokenType::CONSTRAINT},
        {"COUNT", TokenType::COUNT},
        {"CREATE", TokenType::CREATE},
        {"CROSS", TokenType::CROSS},
        {"DATABASE", TokenType::DATABASE},
        {"DATE", TokenType::DATE},
        {"DECIMAL", TokenType::DECIMAL},
        {"BACKFILL", TokenType::BACKFILL},
        {"DEFAULT", TokenType::DEFAULT},
        {"DELETE", TokenType::DELETE},
        {"DESC", TokenType::DESC},
        {"DESCRIBE", TokenType::DESCRIBE},
        {"DIRECTION", TokenType::DIRECTION},
        {"DISTINCT", TokenType::DISTINCT},
        {"DOUBLE", TokenType::DOUBLE},
        {"DROP", TokenType::DROP},
        {"EDGE", TokenType::EDGE},
        {"ELSE", TokenType::ELSE},
        {"EMBEDDING", TokenType::EMBEDDING},
        {"END", TokenType::END},
        {"EXCEPT", TokenType::EXCEPT},
        {"EXISTS", TokenType::EXISTS},
        {"EXPLAIN", TokenType::EXPLAIN},
        {"FALSE", TokenType::FALSE_KW},
        {"FETCH", TokenType::FETCH},
        {"FLOAT", TokenType::FLOAT},
        {"FOREIGN", TokenType::FOREIGN},
        {"FROM", TokenType::FROM},
        {"FULL", TokenType::FULL},
        {"GROUP", TokenType::GROUP},
        {"HAVING", TokenType::HAVING},
        {"IF", TokenType::IF},
        {"IN", TokenType::IN},
        {"INDEX", TokenType::INDEX},
        {"INNER", TokenType::INNER},
        {"INSERT", TokenType::INSERT},
        {"INT", TokenType::INT},
        {"INTEGER", TokenType::INTEGER},
        {"INTERSECT", TokenType::INTERSECT},
        {"INTERVAL", TokenType::INTERVAL},
        {"INTO", TokenType::INTO},
        {"IS", TokenType::IS},
        {"JOIN", TokenType::JOIN},
        {"JSON", TokenType::JSON_KW},
        {"KEY", TokenType::KEY},
        {"LEFT", TokenType::LEFT},
        {"LIKE", TokenType::LIKE},
        {"LIMIT", TokenType::LIMIT},
        {"LINK", TokenType::LINK},
        {"MATCH", TokenType::MATCH},
        {"MAX", TokenType::MAX_KW},
        {"MAX_DEPTH", TokenType::MAX_DEPTH},
        {"MIN", TokenType::MIN_KW},
        {"NEAREST", TokenType::NEAREST},
        {"NOT", TokenType::NOT},
        {"NULL", TokenType::NULL_KW},
        {"NUMERIC", TokenType::NUMERIC},
        {"OFFSET", TokenType::OFFSET},
        {"ON", TokenType::ON},
        {"OR", TokenType::OR},
        {"ORDER", TokenType::ORDER},
        {"OUTER", TokenType::OUTER},
        {"OVER", TokenType::OVER},
        {"PARTITION", TokenType::PARTITION},
        {"PASSWORD", TokenType::PASSWORD},
        {"PATH", TokenType::PATH},
        {"POINT", TokenType::POINT_KW},
        {"PRIMARY", TokenType::PRIMARY},
        {"RECURSIVE", TokenType::RECURSIVE},
        {"REEMBED", TokenType::REEMBED},
        {"REINDEX", TokenType::REINDEX},
        {"REFERENCES", TokenType::REFERENCES},
        {"RELEASE", TokenType::RELEASE},
        {"RETURN", TokenType::RETURN},
        {"RANGE", TokenType::RANGE},
        {"RESTRICT", TokenType::RESTRICT},
        {"RETURNING", TokenType::RETURNING},
        {"RIGHT", TokenType::RIGHT},
        {"ROLLBACK", TokenType::ROLLBACK},
        {"ROWS", TokenType::ROWS},
        {"SAVEPOINT", TokenType::SAVEPOINT},
        {"SELECT", TokenType::SELECT},
        {"SET", TokenType::SET},
        {"SHORTEST", TokenType::SHORTEST},
        {"SHOW", TokenType::SHOW},
        {"SMALLINT", TokenType::SMALLINT},
        {"SUM", TokenType::SUM},
        {"TABLE", TokenType::TABLE},
        {"TEXT", TokenType::TEXT},
        {"THEN", TokenType::THEN},
        {"TIME", TokenType::TIME},
        {"TIMESTAMP", TokenType::TIMESTAMP},
        {"TINYINT", TokenType::TINYINT},
        {"TRANSACTION", TokenType::TRANSACTION},
        {"TRACE", TokenType::TRACE},
        {"TRAVERSE", TokenType::TRAVERSE},
        {"TRUE", TokenType::TRUE_KW},
        {"UNBOUNDED", TokenType::UNBOUNDED},
        {"TYPE", TokenType::TYPE},
        {"UNION", TokenType::UNION},
        {"UNIQUE", TokenType::UNIQUE},
        {"UNLINK", TokenType::UNLINK},
        {"UPDATE", TokenType::UPDATE},
        {"USER", TokenType::USER},
        {"UUID", TokenType::UUID_KW},
        {"VACUUM", TokenType::VACUUM},
        {"VALUES", TokenType::VALUES},
        {"VARCHAR", TokenType::VARCHAR},
        {"VIA", TokenType::VIA},
        {"CURRENT", TokenType::CURRENT},
        {"FOLLOWING", TokenType::FOLLOWING},
        {"PRECEDING", TokenType::PRECEDING},
        {"WEIGHT", TokenType::WEIGHT},
        {"WHEN", TokenType::WHEN},
        {"WHERE", TokenType::WHERE},
        {"WITH", TokenType::WITH},
    };
    return map;
}

/// Convert a character to uppercase (ASCII only).
char to_upper(char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

/// Check if a character can start an identifier (letter or underscore).
bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

/// Check if a character can continue an identifier (letter, digit, underscore).
bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

} // namespace

// -- Lexer public -------------------------------------------------------------

Lexer::Lexer(std::string_view source) : source_(source) {}

Result<std::vector<Token>> Lexer::tokenize() {
    // Reset state so tokenize() can be called multiple times.
    start_ = 0;
    current_ = 0;
    line_ = 1;
    column_ = 1;

    std::vector<Token> tokens;

    while (true) {
        auto skip = skip_whitespace_and_comments();
        if (!skip) {
            return tl::unexpected(skip.error());
        }

        if (at_end()) {
            tokens.push_back(Token{TokenType::END_OF_FILE, {}, line_, column_,
                                   static_cast<uint32_t>(source_.size() + 1)});
            break;
        }

        auto tok = scan_token();
        if (!tok) {
            return tl::unexpected(tok.error());
        }
        tokens.push_back(*tok);
    }

    return ok(std::move(tokens));
}

// -- Lexer private: character helpers -----------------------------------------

char Lexer::advance() {
    char c = source_[current_++];
    if (c == '\n') {
        line_++;
        column_ = 1;
    } else {
        column_++;
    }
    return c;
}

char Lexer::peek() const {
    if (at_end())
        return '\0';
    return source_[current_];
}

char Lexer::peek_next() const {
    if (current_ + 1 >= source_.size())
        return '\0';
    return source_[current_ + 1];
}

bool Lexer::at_end() const {
    return current_ >= source_.size();
}

// -- Lexer private: whitespace and comments -----------------------------------

Result<void> Lexer::skip_whitespace_and_comments() {
    while (!at_end()) {
        char c = peek();

        // Whitespace.
        if (std::isspace(static_cast<unsigned char>(c))) {
            advance();
            continue;
        }

        // Line comment: -- ...
        // But NOT a MATCH edge pattern like --> or --[ or --(.
        if (c == '-' && peek_next() == '-') {
            char after = (current_ + 2 < source_.size()) ? source_[current_ + 2] : '\0';
            if (after != '>' && after != '[' && after != '(') {
                advance(); // skip first -
                advance(); // skip second -
                while (!at_end() && peek() != '\n') {
                    advance();
                }
                continue;
            }
        }

        // Block comment: /* ... */
        if (c == '/' && peek_next() == '*') {
            uint32_t comment_line = line_;
            uint32_t comment_col = column_;
            advance(); // skip /
            advance(); // skip *
            int depth = 1;
            while (!at_end() && depth > 0) {
                if (peek() == '/' && peek_next() == '*') {
                    advance();
                    advance();
                    depth++;
                } else if (peek() == '*' && peek_next() == '/') {
                    advance();
                    advance();
                    depth--;
                } else {
                    advance();
                }
            }
            if (depth > 0) {
                return make_error(StatusCode::PARSE_ERROR,
                                  "unterminated block comment starting at line " +
                                      std::to_string(comment_line) + ", column " +
                                      std::to_string(comment_col));
            }
            continue;
        }

        // Not whitespace or comment — done.
        break;
    }
    return ok();
}

// -- Lexer private: token scanning --------------------------------------------

Result<Token> Lexer::scan_token() {
    start_ = current_;
    token_start_line_ = line_;
    token_start_column_ = column_;

    char c = advance();

    // Single-character tokens.
    switch (c) {
    case '+':
        return make_token(TokenType::PLUS);
    case '-':
        return make_token(TokenType::MINUS);
    case '*':
        return make_token(TokenType::STAR);
    case '/':
        return make_token(TokenType::SLASH);
    case '%':
        return make_token(TokenType::PERCENT);
    case ',':
        return make_token(TokenType::COMMA);
    case ';':
        return make_token(TokenType::SEMICOLON);
    case '.':
        // Check for float literal starting with dot: .123, .5e10, .5E-3
        if (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
            while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
                advance();
            }
            // Check for scientific notation: .5e10, .123E-3
            if (has_valid_exponent()) {
                advance(); // consume e/E
                if (!at_end() && (peek() == '+' || peek() == '-')) {
                    advance();
                }
                while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
                    advance();
                }
            }
            return make_token(TokenType::FLOAT_LITERAL);
        }
        return make_token(TokenType::DOT);
    case '(':
        return make_token(TokenType::LPAREN);
    case ')':
        return make_token(TokenType::RPAREN);
    case '[':
        return make_token(TokenType::LBRACKET);
    case ']':
        return make_token(TokenType::RBRACKET);
    case '{':
        return make_token(TokenType::LBRACE);
    case '}':
        return make_token(TokenType::RBRACE);
    case '=':
        return make_token(TokenType::EQUAL);
    default:
        break;
    }

    // Two-character operators.
    if (c == '!' && !at_end() && peek() == '=') {
        advance(); // consume =
        return make_token(TokenType::NOT_EQUAL);
    }

    if (c == '<') {
        if (!at_end() && peek() == '=') {
            advance();
            return make_token(TokenType::LESS_EQUAL);
        }
        if (!at_end() && peek() == '>') {
            advance();
            return make_token(TokenType::NOT_EQUAL);
        }
        return make_token(TokenType::LESS);
    }

    if (c == '>') {
        if (!at_end() && peek() == '=') {
            advance();
            return make_token(TokenType::GREATER_EQUAL);
        }
        return make_token(TokenType::GREATER);
    }

    if (c == '|' && !at_end() && peek() == '|') {
        advance();
        return make_token(TokenType::PIPE_PIPE);
    }

    if (c == ':') {
        if (!at_end() && peek() == ':') {
            advance();
            return make_token(TokenType::COLON_COLON);
        }
        if (!at_end() && peek() == '=') {
            advance();
            return make_token(TokenType::COLON_EQUAL);
        }
        return make_token(TokenType::COLON);
    }

    // String literal.
    if (c == '\'') {
        return scan_string();
    }

    // Number literal.
    if (std::isdigit(static_cast<unsigned char>(c))) {
        return scan_number();
    }

    // Positional parameter reference ($1, $2, ...).
    if (c == '$' && current_ < source_.size() &&
        std::isdigit(static_cast<unsigned char>(source_[current_]))) {
        while (current_ < source_.size() &&
               std::isdigit(static_cast<unsigned char>(source_[current_]))) {
            advance();
        }
        return make_token(TokenType::PARAM_REF);
    }

    // Identifier or keyword.
    if (is_ident_start(c)) {
        return scan_identifier();
    }

    return make_error(StatusCode::PARSE_ERROR,
                      "unexpected character '" + std::string(1, c) + "' at line " +
                          std::to_string(token_start_line_) + ", column " +
                          std::to_string(token_start_column_));
}

Token Lexer::scan_number() {
    // We've already consumed the first digit.
    while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
        advance();
    }

    // Check for fractional part.
    if (!at_end() && peek() == '.' &&
        (current_ + 1 >= source_.size() || !is_ident_start(peek_next()))) {
        advance(); // consume .
        while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }
        // Check for scientific notation: 1.5e10, 1.5E-3
        // Only consume e/E if followed by digits (or +/- then digits).
        if (has_valid_exponent()) {
            advance(); // consume e/E
            if (!at_end() && (peek() == '+' || peek() == '-')) {
                advance();
            }
            while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
                advance();
            }
        }
        return make_token(TokenType::FLOAT_LITERAL);
    }

    // Check for scientific notation on integer: 1e10
    // Only consume e/E if followed by digits (or +/- then digits).
    if (has_valid_exponent()) {
        advance(); // consume e/E
        if (!at_end() && (peek() == '+' || peek() == '-')) {
            advance();
        }
        while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }
        return make_token(TokenType::FLOAT_LITERAL);
    }

    return make_token(TokenType::INTEGER_LITERAL);
}

Result<Token> Lexer::scan_string() {
    // Opening quote already consumed.
    while (true) {
        if (at_end()) {
            return make_error(StatusCode::PARSE_ERROR,
                              "unterminated string literal starting at line " +
                                  std::to_string(token_start_line_) + ", column " +
                                  std::to_string(token_start_column_));
        }
        char c = advance();
        if (c == '\'') {
            // Check for escaped quote ('').
            if (!at_end() && peek() == '\'') {
                advance(); // consume second quote
                continue;
            }
            // End of string.
            return make_token(TokenType::STRING_LITERAL);
        }
    }
}

Token Lexer::scan_identifier() {
    // First character already consumed.
    while (!at_end() && is_ident_char(peek())) {
        advance();
    }

    // Check if it's a keyword (case-insensitive).
    std::string_view lexeme = source_.substr(start_, current_ - start_);
    std::string upper;
    upper.reserve(lexeme.size());
    for (char ch : lexeme) {
        upper += to_upper(ch);
    }

    auto& kw = keyword_map();
    auto it = kw.find(upper);
    if (it != kw.end()) {
        return make_token(it->second);
    }

    return make_token(TokenType::IDENTIFIER);
}

bool Lexer::has_valid_exponent() const {
    if (at_end() || (peek() != 'e' && peek() != 'E')) {
        return false;
    }
    size_t lookahead = current_ + 1;
    if (lookahead < source_.size() && (source_[lookahead] == '+' || source_[lookahead] == '-')) {
        lookahead++;
    }
    return lookahead < source_.size() &&
           std::isdigit(static_cast<unsigned char>(source_[lookahead]));
}

Token Lexer::make_token(TokenType type) const {
    // byte_offset is 1-based (PostgreSQL 'P' field convention).
    return Token{type,
                 source_.substr(start_, current_ - start_),
                 token_start_line_,
                 token_start_column_,
                 static_cast<uint32_t>(start_ + 1)};
}

} // namespace sixseven
