#pragma once

#include "sixseven/common/result.h"
#include "sixseven/parser/token.h"

#include <string_view>
#include <vector>

namespace sixseven {

/// SQL lexer that converts raw SQL text into a stream of tokens.
///
/// Supports all standard SQL keywords, SixSevenDB-specific keywords (TRAVERSE,
/// NEAREST, MATCH, EMBEDDING, LINK, UNLINK, etc.), operators, literals,
/// identifiers, and punctuation. Keywords are case-insensitive.
///
/// Identifiers may be unquoted (`users`, `id`) or double-quoted/delimited
/// (`"users"`, `"Weird Col"`). Both produce an IDENTIFIER token; the raw
/// lexeme retains the surrounding quotes and any `""` escapes for delimited
/// identifiers, exactly as STRING_LITERAL retains its surrounding `'` quotes.
/// Callers that materialize an identifier's text (see
/// `Parser::parse_name`/`unquote_identifier` in parser.cpp) must strip the
/// quotes and un-escape `""` -> `"` when the lexeme is quoted. Quoted
/// identifiers bypass keyword lookup, so `"select"` is an IDENTIFIER, not the
/// SELECT keyword.
///
/// The returned tokens contain string_view lexemes that reference the
/// original input string. The caller must keep the input alive for the
/// lifetime of the token vector.
///
/// Usage:
/// ```
///   std::string sql = "SELECT id, name FROM users WHERE active = TRUE";
///   Lexer lexer(sql);
///   auto tokens = lexer.tokenize();
///   for (auto& tok : tokens.value()) {
///       // process token...
///   }
/// ```
class Lexer {
public:
    /// Construct a lexer for the given SQL source text.
    /// @param source SQL text to tokenize. Must remain valid for the
    ///               lifetime of the returned tokens.
    explicit Lexer(std::string_view source);

    /// Tokenize the entire source into a vector of tokens.
    /// The last token is always END_OF_FILE.
    /// @return A vector of tokens, or an error with line/column info.
    [[nodiscard]] Result<std::vector<Token>> tokenize();

private:
    /// Advance past the current character and return it.
    char advance();

    /// Return the current character without advancing.
    [[nodiscard]] char peek() const;

    /// Return the character after the current one without advancing.
    [[nodiscard]] char peek_next() const;

    /// Check if we've reached the end of input.
    [[nodiscard]] bool at_end() const;

    /// Skip whitespace and comments.
    /// @return Error if an unterminated block comment is found.
    [[nodiscard]] Result<void> skip_whitespace_and_comments();

    /// Scan a single token starting at the current position.
    [[nodiscard]] Result<Token> scan_token();

    /// Scan a number literal (integer or float).
    [[nodiscard]] Token scan_number();

    /// Scan a string literal (single-quoted, with '' escape).
    [[nodiscard]] Result<Token> scan_string();

    /// Scan an identifier or keyword.
    [[nodiscard]] Token scan_identifier();

    /// Scan a double-quoted (delimited) identifier, with "" escape for an
    /// embedded double quote. Always produces an IDENTIFIER token — quoted
    /// identifiers bypass keyword lookup, per SQL/PostgreSQL semantics.
    [[nodiscard]] Result<Token> scan_quoted_identifier();

    /// Check if current position has a valid exponent (e/E followed by digits).
    [[nodiscard]] bool has_valid_exponent() const;

    /// Create a token with the current lexeme range.
    [[nodiscard]] Token make_token(TokenType type) const;

    std::string_view source_;
    size_t start_ = 0;   ///< Start of current lexeme.
    size_t current_ = 0; ///< Current scan position.
    uint32_t line_ = 1;
    uint32_t column_ = 1;
    uint32_t token_start_line_ = 1;
    uint32_t token_start_column_ = 1;
};

} // namespace sixseven
