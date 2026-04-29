#include "sixseven/parser/parser.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace sixseven {

namespace {

/// Check if a token type is a keyword that can be used as an identifier.
/// Type names, aggregate names, and other non-structural keywords are allowed.
bool is_name_token(TokenType type) {
    switch (type) {
    case TokenType::IDENTIFIER:
    // Type keywords commonly used as column/table names.
    case TokenType::INT:
    case TokenType::INTEGER:
    case TokenType::TINYINT:
    case TokenType::SMALLINT:
    case TokenType::BIGINT:
    case TokenType::FLOAT:
    case TokenType::DOUBLE:
    case TokenType::DECIMAL:
    case TokenType::NUMERIC:
    case TokenType::BOOLEAN:
    case TokenType::CHAR:
    case TokenType::VARCHAR:
    case TokenType::TEXT:
    case TokenType::BLOB_KW:
    case TokenType::DATE:
    case TokenType::TIME:
    case TokenType::TIMESTAMP:
    case TokenType::INTERVAL:
    case TokenType::POINT_KW:
    case TokenType::JSON_KW:
    case TokenType::UUID_KW:
    case TokenType::EMBEDDING:
    // Other non-structural keywords.
    case TokenType::TYPE:
    case TokenType::DIRECTION:
    case TokenType::PATH:
    case TokenType::KEY:
    case TokenType::COLUMN:
    case TokenType::INDEX:
    case TokenType::EDGE:
    case TokenType::FETCH:
    case TokenType::AVG:
    case TokenType::MAX_KW:
    case TokenType::MIN_KW:
    case TokenType::SUM:
    case TokenType::COUNT:
    case TokenType::CASCADE:
    case TokenType::RESTRICT:
    case TokenType::RECURSIVE:
    case TokenType::ANY:
    case TokenType::NEAREST:
    case TokenType::SHORTEST:
    case TokenType::TRAVERSE:
    case TokenType::MATCH:
    case TokenType::LINK:
    case TokenType::UNLINK:
    case TokenType::BACKFILL:
    case TokenType::REEMBED:
    case TokenType::REINDEX:
    case TokenType::RETURN:
    case TokenType::VIA:
    case TokenType::ANALYZE:
    case TokenType::DESCRIBE:
    case TokenType::EXPLAIN:
    case TokenType::SAVEPOINT:
    case TokenType::SHOW:
    case TokenType::VACUUM:
    case TokenType::MAX_DEPTH:
    case TokenType::DATABASE:
    case TokenType::USER:
    case TokenType::PASSWORD:
    case TokenType::WEIGHT:
    case TokenType::OVER:
    case TokenType::PARTITION:
    case TokenType::ROWS:
    case TokenType::RANGE:
    case TokenType::UNBOUNDED:
    case TokenType::PRECEDING:
    case TokenType::FOLLOWING:
    case TokenType::CURRENT:
        return true;
    default:
        return false;
    }
}

/// Check if a token type is a SQL type keyword (used for type spec parsing).
bool is_type_keyword(TokenType type) {
    switch (type) {
    case TokenType::INT:
    case TokenType::INTEGER:
    case TokenType::TINYINT:
    case TokenType::SMALLINT:
    case TokenType::BIGINT:
    case TokenType::FLOAT:
    case TokenType::DOUBLE:
    case TokenType::DECIMAL:
    case TokenType::NUMERIC:
    case TokenType::BOOLEAN:
    case TokenType::CHAR:
    case TokenType::VARCHAR:
    case TokenType::TEXT:
    case TokenType::BLOB_KW:
    case TokenType::DATE:
    case TokenType::TIME:
    case TokenType::TIMESTAMP:
    case TokenType::INTERVAL:
    case TokenType::POINT_KW:
    case TokenType::JSON_KW:
    case TokenType::UUID_KW:
    case TokenType::EMBEDDING:
        return true;
    default:
        return false;
    }
}

/// Check if token is an identifier matching the given uppercase name
/// (case-insensitive).
bool match_ident_ci(const Token& tok, std::string_view upper_name) {
    if (tok.type != TokenType::IDENTIFIER)
        return false;
    if (tok.lexeme.size() != upper_name.size())
        return false;
    for (size_t i = 0; i < tok.lexeme.size(); ++i) {
        if (static_cast<char>(std::toupper(static_cast<unsigned char>(tok.lexeme[i]))) !=
            upper_name[i]) {
            return false;
        }
    }
    return true;
}

/// Check if a token type is a clause-starting keyword (not usable as alias).
bool is_clause_keyword(TokenType type) {
    switch (type) {
    case TokenType::FROM:
    case TokenType::WHERE:
    case TokenType::JOIN:
    case TokenType::INNER:
    case TokenType::LEFT:
    case TokenType::RIGHT:
    case TokenType::FULL:
    case TokenType::CROSS:
    case TokenType::ON:
    case TokenType::GROUP:
    case TokenType::HAVING:
    case TokenType::ORDER:
    case TokenType::LIMIT:
    case TokenType::OFFSET:
    case TokenType::UNION:
    case TokenType::INTERSECT:
    case TokenType::EXCEPT:
    case TokenType::RETURNING:
    case TokenType::SET:
    case TokenType::VALUES:
    case TokenType::INTO:
    case TokenType::SELECT:
    case TokenType::INSERT:
    case TokenType::UPDATE:
    case TokenType::DELETE:
    case TokenType::CREATE:
    case TokenType::DROP:
    case TokenType::ALTER:
    case TokenType::WITH:
    case TokenType::AND:
    case TokenType::OR:
    case TokenType::NOT:
    case TokenType::AS:
    case TokenType::OVER:
    case TokenType::BEGIN:
    case TokenType::COMMIT:
    case TokenType::ROLLBACK:
    case TokenType::END_OF_FILE:
        return true;
    default:
        return false;
    }
}

/// Strip single quotes from a string literal lexeme and unescape '' -> '.
std::string unquote_string(std::string_view lexeme) {
    // Remove surrounding quotes.
    if (lexeme.size() >= 2 && lexeme.front() == '\'' && lexeme.back() == '\'') {
        lexeme = lexeme.substr(1, lexeme.size() - 2);
    }
    std::string result;
    result.reserve(lexeme.size());
    for (size_t i = 0; i < lexeme.size(); ++i) {
        if (lexeme[i] == '\'' && i + 1 < lexeme.size() && lexeme[i + 1] == '\'') {
            result += '\'';
            ++i;
        } else {
            result += lexeme[i];
        }
    }
    return result;
}

/// Safe std::stoi wrapper that returns a Result instead of throwing.
Result<int> safe_stoi(std::string_view s) {
    try {
        return ok(std::stoi(std::string(s)));
    } catch (const std::out_of_range&) {
        return make_error(StatusCode::PARSE_ERROR, "integer literal out of range");
    } catch (const std::invalid_argument&) {
        return make_error(StatusCode::PARSE_ERROR, "invalid integer literal");
    }
}

} // namespace

// -- Constructor --------------------------------------------------------------

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

// -- Public API ---------------------------------------------------------------

Result<StmtPtr> Parser::parse() {
    if (at_end()) {
        return error("unexpected end of input");
    }
    auto stmt = parse_statement();
    if (!stmt)
        return stmt;

    // Consume optional trailing semicolon.
    match(TokenType::SEMICOLON);
    return stmt;
}

Result<std::vector<StmtPtr>> Parser::parse_all() {
    std::vector<StmtPtr> statements;
    std::vector<std::string> errors;

    while (!at_end()) {
        // Skip stray semicolons.
        if (match(TokenType::SEMICOLON))
            continue;

        auto stmt = parse_statement();
        if (stmt) {
            statements.push_back(std::move(*stmt));
            // Consume optional trailing semicolon.
            match(TokenType::SEMICOLON);
        } else {
            errors.push_back(stmt.error().message);
            synchronize();
        }
    }

    if (!errors.empty()) {
        std::string combined = std::to_string(errors.size()) + " parse error(s):";
        for (size_t i = 0; i < errors.size(); ++i) {
            combined += "\n  " + std::to_string(i + 1) + ". " + errors[i];
        }
        return make_error(StatusCode::PARSE_ERROR, combined);
    }

    return ok(std::move(statements));
}

// -- Token navigation --------------------------------------------------------

const Token& Parser::peek() const {
    if (current_ >= tokens_.size()) {
        static const Token eof{TokenType::END_OF_FILE, {}, 0, 0};
        return eof;
    }
    return tokens_[current_];
}

const Token& Parser::previous() const {
    return tokens_[current_ - 1];
}

Token Parser::advance() {
    if (!at_end()) {
        return tokens_[current_++];
    }
    return peek();
}

bool Parser::check(TokenType type) const {
    return peek().type == type;
}

bool Parser::match(TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

Result<Token> Parser::expect(TokenType type, const std::string& message) {
    if (check(type)) {
        return ok(advance());
    }
    const auto& tok = peek();
    return make_error(StatusCode::PARSE_ERROR,
                      message + " at line " + std::to_string(tok.line) + ", column " +
                          std::to_string(tok.column) + " (got " +
                          std::string(token_type_name(tok.type)) + ")");
}

bool Parser::at_end() const {
    return peek().type == TokenType::END_OF_FILE;
}

Result<std::string> Parser::parse_name(const std::string& context) {
    if (is_name_token(peek().type)) {
        return ok(std::string(advance().lexeme));
    }
    const auto& tok = peek();
    return make_error(StatusCode::PARSE_ERROR,
                      "expected " + context + " at line " + std::to_string(tok.line) + ", column " +
                          std::to_string(tok.column) + " (got " +
                          std::string(token_type_name(tok.type)) + ")");
}

// -- Error recovery -----------------------------------------------------------

void Parser::synchronize() {
    while (!at_end()) {
        if (match(TokenType::SEMICOLON))
            return;
        advance();
    }
}

Result<StmtPtr> Parser::error(const std::string& message) {
    const auto& tok = peek();
    return make_error(StatusCode::PARSE_ERROR,
                      message + " at line " + std::to_string(tok.line) + ", column " +
                          std::to_string(tok.column));
}

// -- Statement dispatch -------------------------------------------------------

Result<StmtPtr> Parser::parse_statement() {
    switch (peek().type) {
    case TokenType::CREATE:
        return parse_create();
    case TokenType::DROP:
        return parse_drop();
    case TokenType::ALTER:
        return parse_alter();
    case TokenType::INSERT:
        return parse_insert();
    case TokenType::UPDATE:
        return parse_update();
    case TokenType::DELETE:
        return parse_delete();
    case TokenType::SELECT:
        return parse_select();
    case TokenType::WITH:
        return parse_with();
    case TokenType::LINK:
        return parse_link();
    case TokenType::UNLINK:
        return parse_unlink();
    case TokenType::TRAVERSE:
        return parse_traverse();
    case TokenType::NEAREST:
        return parse_nearest();
    case TokenType::MATCH:
        return parse_match();
    case TokenType::SHORTEST:
        return parse_shortest_path();
    case TokenType::BEGIN:
        return parse_begin();
    case TokenType::COMMIT:
        return parse_commit();
    case TokenType::ROLLBACK:
        return parse_rollback();
    case TokenType::SAVEPOINT:
        return parse_savepoint();
    case TokenType::SET:
        return parse_set_stmt();
    case TokenType::SHOW:
        return parse_show();
    case TokenType::EXPLAIN:
        return parse_explain();
    case TokenType::DESCRIBE:
        return parse_describe();
    case TokenType::BACKFILL:
        return parse_backfill();
    case TokenType::REEMBED:
        return parse_reembed();
    case TokenType::REINDEX:
        return parse_reindex();
    case TokenType::VACUUM:
        return parse_vacuum();
    case TokenType::ANALYZE:
        return parse_analyze_stmt();
    default:
        return error("expected statement");
    }
}

Result<StmtPtr> Parser::parse_create() {
    advance(); // consume CREATE

    if (match(TokenType::TABLE))
        return parse_create_table();
    if (match(TokenType::DATABASE))
        return parse_create_database();
    if (match(TokenType::INDEX))
        return parse_create_index(false);
    if (check(TokenType::UNIQUE)) {
        advance(); // consume UNIQUE
        auto idx = expect(TokenType::INDEX, "expected INDEX after UNIQUE");
        if (!idx)
            return tl::unexpected(idx.error());
        return parse_create_index(true);
    }
    if (match(TokenType::EDGE)) {
        auto type = expect(TokenType::TYPE, "expected TYPE after EDGE");
        if (!type)
            return tl::unexpected(type.error());
        return parse_create_edge_type();
    }
    if (match(TokenType::USER))
        return parse_create_user();

    return error("expected TABLE, DATABASE, INDEX, UNIQUE INDEX, EDGE TYPE, or USER after CREATE");
}

Result<StmtPtr> Parser::parse_drop() {
    advance(); // consume DROP

    if (match(TokenType::TABLE))
        return parse_drop_table();
    if (match(TokenType::DATABASE))
        return parse_drop_database();
    if (match(TokenType::INDEX))
        return parse_drop_index();
    if (match(TokenType::EDGE)) {
        auto type = expect(TokenType::TYPE, "expected TYPE after EDGE");
        if (!type)
            return tl::unexpected(type.error());
        return parse_drop_edge_type();
    }
    if (match(TokenType::USER))
        return parse_drop_user();

    return error("expected TABLE, DATABASE, INDEX, EDGE TYPE, or USER after DROP");
}

Result<StmtPtr> Parser::parse_alter() {
    advance(); // consume ALTER

    if (match(TokenType::TABLE))
        return parse_alter_table();
    if (match(TokenType::USER))
        return parse_alter_user();

    return error("expected TABLE or USER after ALTER");
}

// -- DDL: CREATE TABLE --------------------------------------------------------

Result<StmtPtr> Parser::parse_create_table() {
    auto stmt = std::make_unique<CreateTableStmt>();

    // IF NOT EXISTS
    if (match(TokenType::IF)) {
        auto not_tok = expect(TokenType::NOT, "expected NOT after IF");
        if (!not_tok)
            return tl::unexpected(not_tok.error());
        auto exists_tok = expect(TokenType::EXISTS, "expected EXISTS after NOT");
        if (!exists_tok)
            return tl::unexpected(exists_tok.error());
        stmt->if_not_exists = true;
    }

    // Table name.
    auto name = parse_name("table name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->name = std::move(*name);

    // ( column_defs, ... [, table_constraints, ...] )
    auto lp = expect(TokenType::LPAREN, "expected '(' after table name");
    if (!lp)
        return tl::unexpected(lp.error());

    bool first = true;
    while (!check(TokenType::RPAREN) && !at_end()) {
        if (!first) {
            auto comma = expect(TokenType::COMMA, "expected ',' between definitions");
            if (!comma)
                return tl::unexpected(comma.error());
        }
        first = false;

        // Peek ahead to decide: table constraint or column def.
        if (check(TokenType::PRIMARY) || check(TokenType::UNIQUE) || check(TokenType::CHECK) ||
            check(TokenType::FOREIGN) || check(TokenType::CONSTRAINT)) {
            auto tc = parse_table_constraint();
            if (!tc)
                return tl::unexpected(tc.error());
            stmt->constraints.push_back(std::move(*tc));
        } else {
            auto col = parse_column_def();
            if (!col)
                return tl::unexpected(col.error());

            // If column has inline PRIMARY KEY, convert to table constraint.
            // (We detect this via a sentinel — see parse_column_def.)
            stmt->columns.push_back(std::move(*col));
        }
    }

    auto rp = expect(TokenType::RPAREN, "expected ')' after column definitions");
    if (!rp)
        return tl::unexpected(rp.error());

    return ok(StmtPtr(std::move(stmt)));
}

// -- DDL: column definition ---------------------------------------------------

Result<AstColumnDef> Parser::parse_column_def() {
    AstColumnDef col;

    // Column name.
    auto name = parse_name("column name");
    if (!name)
        return tl::unexpected(name.error());
    col.name = std::move(*name);

    // Column type.
    auto type = parse_type_spec();
    if (!type)
        return tl::unexpected(type.error());
    col.type = std::move(*type);

    // Column constraints (in any order).
    while (!check(TokenType::COMMA) && !check(TokenType::RPAREN) && !at_end()) {
        if (match(TokenType::NOT)) {
            auto null_tok = expect(TokenType::NULL_KW, "expected NULL after NOT");
            if (!null_tok)
                return tl::unexpected(null_tok.error());
            col.nullable = false;
        } else if (match(TokenType::NULL_KW)) {
            col.nullable = true;
        } else if (match(TokenType::UNIQUE)) {
            col.is_unique = true;
        } else if (match(TokenType::PRIMARY)) {
            auto key_tok = expect(TokenType::KEY, "expected KEY after PRIMARY");
            if (!key_tok)
                return tl::unexpected(key_tok.error());
            col.nullable = false; // PRIMARY KEY implies NOT NULL.
            col.is_unique = true;
            col.is_primary_key = true;
        } else if (match(TokenType::AUTOINCREMENT)) {
            col.is_autoincrement = true;
            col.nullable = false; // AUTOINCREMENT implies NOT NULL.
        } else if (match(TokenType::DEFAULT)) {
            auto expr = parse_expression();
            if (!expr)
                return tl::unexpected(expr.error());
            col.default_expr = std::move(*expr);
        } else if (match(TokenType::CHECK)) {
            auto lp = expect(TokenType::LPAREN, "expected '(' after CHECK");
            if (!lp)
                return tl::unexpected(lp.error());
            auto expr = parse_expression();
            if (!expr)
                return tl::unexpected(expr.error());
            auto rp = expect(TokenType::RPAREN, "expected ')' after CHECK expression");
            if (!rp)
                return tl::unexpected(rp.error());
            col.check_expr = std::move(*expr);
        } else if (match(TokenType::REFERENCES)) {
            auto tbl_name = parse_name("referenced table name");
            if (!tbl_name)
                return tl::unexpected(tbl_name.error());
            col.fk_table = std::move(*tbl_name);
            auto lp = expect(TokenType::LPAREN, "expected '(' after table name");
            if (!lp)
                return tl::unexpected(lp.error());
            auto col_name = parse_name("referenced column name");
            if (!col_name)
                return tl::unexpected(col_name.error());
            col.fk_column = std::move(*col_name);
            auto rp = expect(TokenType::RPAREN, "expected ')'");
            if (!rp)
                return tl::unexpected(rp.error());
            // ON DELETE action.
            if (match(TokenType::ON)) {
                auto del = expect(TokenType::DELETE, "expected DELETE after ON");
                if (!del)
                    return tl::unexpected(del.error());
                if (match(TokenType::CASCADE)) {
                    col.fk_on_delete = ReferentialAction::CASCADE;
                } else if (match(TokenType::RESTRICT)) {
                    col.fk_on_delete = ReferentialAction::RESTRICT;
                } else {
                    return tl::unexpected(error("expected CASCADE or RESTRICT").error());
                }
            }
        } else {
            break;
        }
    }

    return ok(std::move(col));
}

// -- DDL: type spec -----------------------------------------------------------

Result<TypeSpec> Parser::parse_type_spec() {
    TypeSpec ts;

    if (!is_type_keyword(peek().type)) {
        const auto& tok = peek();
        return make_error(StatusCode::PARSE_ERROR,
                          "expected type name at line " + std::to_string(tok.line) + ", column " +
                              std::to_string(tok.column));
    }

    ts.name = std::string(advance().lexeme);
    // Normalize to uppercase.
    std::transform(ts.name.begin(), ts.name.end(), ts.name.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    // EMBEDDING(dim, source, 'provider')
    // Supports both positional and named-parameter syntax:
    //   EMBEDDING(384, title, 'openai')
    //   EMBEDDING(384, source='title', provider='openai')
    if (ts.name == "EMBEDDING" && match(TokenType::LPAREN)) {
        // Dimension.
        auto dim = expect(TokenType::INTEGER_LITERAL, "expected dimension");
        if (!dim)
            return tl::unexpected(dim.error());
        auto dim_val = safe_stoi(dim->lexeme);
        if (!dim_val)
            return tl::unexpected(dim_val.error());
        ts.param1 = *dim_val;

        auto c1 = expect(TokenType::COMMA, "expected ',' after dimension");
        if (!c1)
            return tl::unexpected(c1.error());

        // Detect named-parameter syntax: source='...' or positional: column_name
        const auto& next_tok = peek();
        bool named_syntax = false;
        if (next_tok.type == TokenType::IDENTIFIER) {
            // Look ahead: if identifier followed by '=', it's named syntax.
            size_t save_pos = current_;
            advance(); // consume the identifier
            if (peek().type == TokenType::EQUAL) {
                named_syntax = true;
            }
            current_ = save_pos; // restore position
        }

        if (named_syntax) {
            // Named-parameter syntax: source='...', provider='...'
            // Parse in any order.
            bool got_source = false;
            bool got_provider = false;
            for (int param_count = 0; param_count < 2; ++param_count) {
                auto param_name = parse_name("parameter name");
                if (!param_name)
                    return tl::unexpected(param_name.error());

                auto eq = expect(TokenType::EQUAL, "expected '=' after parameter name");
                if (!eq)
                    return tl::unexpected(eq.error());

                auto param_val = expect(TokenType::STRING_LITERAL, "expected string value");
                if (!param_val)
                    return tl::unexpected(param_val.error());

                std::string name_lower = *param_name;
                std::transform(name_lower.begin(),
                               name_lower.end(),
                               name_lower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                if (name_lower == "source") {
                    if (got_source) {
                        return make_error(StatusCode::PARSE_ERROR, "duplicate 'source' parameter");
                    }
                    ts.source = unquote_string(param_val->lexeme);
                    got_source = true;
                } else if (name_lower == "provider") {
                    if (got_provider) {
                        return make_error(StatusCode::PARSE_ERROR,
                                          "duplicate 'provider' parameter");
                    }
                    ts.provider = unquote_string(param_val->lexeme);
                    got_provider = true;
                } else {
                    return make_error(StatusCode::PARSE_ERROR,
                                      "unknown EMBEDDING parameter '" + *param_name + "'");
                }

                if (param_count == 0) {
                    auto comma = expect(TokenType::COMMA, "expected ','");
                    if (!comma)
                        return tl::unexpected(comma.error());
                }
            }

            if (!got_source) {
                return make_error(StatusCode::PARSE_ERROR,
                                  "EMBEDDING missing required 'source' parameter");
            }
            if (!got_provider) {
                return make_error(StatusCode::PARSE_ERROR,
                                  "EMBEDDING missing required 'provider' parameter");
            }
        } else {
            // Positional syntax: source_column, 'provider'
            auto src = parse_name("source column");
            if (!src)
                return tl::unexpected(src.error());
            ts.source = std::move(*src);

            auto c2 = expect(TokenType::COMMA, "expected ',' after source");
            if (!c2)
                return tl::unexpected(c2.error());

            auto prov = expect(TokenType::STRING_LITERAL, "expected provider string");
            if (!prov)
                return tl::unexpected(prov.error());
            ts.provider = unquote_string(prov->lexeme);
        }

        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp)
            return tl::unexpected(rp.error());
        return ok(std::move(ts));
    }

    // Types with optional parameters: VARCHAR(n), CHAR(n), DECIMAL(p,s),
    // NUMERIC(p,s).
    if (match(TokenType::LPAREN)) {
        auto p1 = expect(TokenType::INTEGER_LITERAL, "expected parameter value");
        if (!p1)
            return tl::unexpected(p1.error());
        auto p1_val = safe_stoi(p1->lexeme);
        if (!p1_val)
            return tl::unexpected(p1_val.error());
        ts.param1 = *p1_val;

        if (match(TokenType::COMMA)) {
            auto p2 = expect(TokenType::INTEGER_LITERAL, "expected second parameter");
            if (!p2)
                return tl::unexpected(p2.error());
            auto p2_val = safe_stoi(p2->lexeme);
            if (!p2_val)
                return tl::unexpected(p2_val.error());
            ts.param2 = *p2_val;
        }

        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp)
            return tl::unexpected(rp.error());
    }

    return ok(std::move(ts));
}

// -- DDL: table constraint ----------------------------------------------------

Result<TableConstraint> Parser::parse_table_constraint() {
    TableConstraint tc;

    // Optional CONSTRAINT name.
    if (match(TokenType::CONSTRAINT)) {
        auto name = parse_name("constraint name");
        if (!name)
            return tl::unexpected(name.error());
        tc.name = std::move(*name);
    }

    if (match(TokenType::PRIMARY)) {
        auto key = expect(TokenType::KEY, "expected KEY after PRIMARY");
        if (!key)
            return tl::unexpected(key.error());
        tc.kind = TableConstraint::Kind::PRIMARY_KEY;

        auto lp = expect(TokenType::LPAREN, "expected '(' after PRIMARY KEY");
        if (!lp)
            return tl::unexpected(lp.error());

        do {
            auto col = parse_name("column name");
            if (!col)
                return tl::unexpected(col.error());
            tc.columns.push_back(std::move(*col));
        } while (match(TokenType::COMMA));

        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp)
            return tl::unexpected(rp.error());
    } else if (match(TokenType::UNIQUE)) {
        tc.kind = TableConstraint::Kind::UNIQUE;

        auto lp = expect(TokenType::LPAREN, "expected '(' after UNIQUE");
        if (!lp)
            return tl::unexpected(lp.error());

        do {
            auto col = parse_name("column name");
            if (!col)
                return tl::unexpected(col.error());
            tc.columns.push_back(std::move(*col));
        } while (match(TokenType::COMMA));

        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp)
            return tl::unexpected(rp.error());
    } else if (match(TokenType::CHECK)) {
        tc.kind = TableConstraint::Kind::CHECK;

        auto lp = expect(TokenType::LPAREN, "expected '(' after CHECK");
        if (!lp)
            return tl::unexpected(lp.error());
        auto expr = parse_expression();
        if (!expr)
            return tl::unexpected(expr.error());
        tc.check_expr = std::move(*expr);
        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp)
            return tl::unexpected(rp.error());
    } else if (match(TokenType::FOREIGN)) {
        auto key = expect(TokenType::KEY, "expected KEY after FOREIGN");
        if (!key)
            return tl::unexpected(key.error());
        tc.kind = TableConstraint::Kind::FOREIGN_KEY;

        auto lp = expect(TokenType::LPAREN, "expected '('");
        if (!lp)
            return tl::unexpected(lp.error());
        do {
            auto col = parse_name("column name");
            if (!col)
                return tl::unexpected(col.error());
            tc.columns.push_back(std::move(*col));
        } while (match(TokenType::COMMA));
        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp)
            return tl::unexpected(rp.error());

        auto ref = expect(TokenType::REFERENCES, "expected REFERENCES");
        if (!ref)
            return tl::unexpected(ref.error());
        auto tbl = parse_name("referenced table");
        if (!tbl)
            return tl::unexpected(tbl.error());
        tc.fk_table = std::move(*tbl);

        auto lp2 = expect(TokenType::LPAREN, "expected '('");
        if (!lp2)
            return tl::unexpected(lp2.error());
        do {
            auto col = parse_name("column name");
            if (!col)
                return tl::unexpected(col.error());
            tc.fk_columns.push_back(std::move(*col));
        } while (match(TokenType::COMMA));
        auto rp2 = expect(TokenType::RPAREN, "expected ')'");
        if (!rp2)
            return tl::unexpected(rp2.error());

        if (match(TokenType::ON)) {
            auto del = expect(TokenType::DELETE, "expected DELETE");
            if (!del)
                return tl::unexpected(del.error());
            if (match(TokenType::CASCADE)) {
                tc.on_delete = ReferentialAction::CASCADE;
            } else if (match(TokenType::RESTRICT)) {
                tc.on_delete = ReferentialAction::RESTRICT;
            } else {
                return tl::unexpected(error("expected CASCADE or RESTRICT").error());
            }
        }
    } else {
        return tl::unexpected(error("expected table constraint").error());
    }

    return ok(std::move(tc));
}

// -- DDL: DROP TABLE ----------------------------------------------------------

Result<StmtPtr> Parser::parse_drop_table() {
    auto stmt = std::make_unique<DropTableStmt>();

    if (match(TokenType::IF)) {
        auto exists = expect(TokenType::EXISTS, "expected EXISTS after IF");
        if (!exists)
            return tl::unexpected(exists.error());
        stmt->if_exists = true;
    }

    auto name = parse_name("table name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->name = std::move(*name);

    if (match(TokenType::CASCADE)) {
        stmt->cascade = true;
    } else {
        match(TokenType::RESTRICT);
    }

    return ok(StmtPtr(std::move(stmt)));
}

// -- DDL: CREATE DATABASE -----------------------------------------------------

Result<StmtPtr> Parser::parse_create_database() {
    auto stmt = std::make_unique<CreateDatabaseStmt>();

    if (match(TokenType::IF)) {
        auto not_kw = expect(TokenType::NOT, "expected NOT after IF");
        if (!not_kw)
            return tl::unexpected(not_kw.error());
        auto exists = expect(TokenType::EXISTS, "expected EXISTS after IF NOT");
        if (!exists)
            return tl::unexpected(exists.error());
        stmt->if_not_exists = true;
    }

    auto name = parse_name("database name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->database_name = std::move(*name);

    // Also support IF NOT EXISTS after the database name.
    if (!stmt->if_not_exists && match(TokenType::IF)) {
        auto not_kw = expect(TokenType::NOT, "expected NOT after IF");
        if (!not_kw)
            return tl::unexpected(not_kw.error());
        auto exists = expect(TokenType::EXISTS, "expected EXISTS after IF NOT");
        if (!exists)
            return tl::unexpected(exists.error());
        stmt->if_not_exists = true;
    }

    return ok(StmtPtr(std::move(stmt)));
}

// -- DDL: DROP DATABASE -------------------------------------------------------

Result<StmtPtr> Parser::parse_drop_database() {
    auto stmt = std::make_unique<DropDatabaseStmt>();

    if (match(TokenType::IF)) {
        auto exists = expect(TokenType::EXISTS, "expected EXISTS after IF");
        if (!exists)
            return tl::unexpected(exists.error());
        stmt->if_exists = true;
    }

    auto name = parse_name("database name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->database_name = std::move(*name);

    // Also accept IF EXISTS after the name (alternative syntax).
    if (!stmt->if_exists && match(TokenType::IF)) {
        auto exists = expect(TokenType::EXISTS, "expected EXISTS after IF");
        if (!exists)
            return tl::unexpected(exists.error());
        stmt->if_exists = true;
    }

    if (match(TokenType::CASCADE)) {
        stmt->cascade = true;
    } else {
        match(TokenType::RESTRICT);
    }

    return ok(StmtPtr(std::move(stmt)));
}

// -- DDL: ALTER TABLE ---------------------------------------------------------

Result<StmtPtr> Parser::parse_alter_table() {
    auto stmt = std::make_unique<AlterTableStmt>();

    auto name = parse_name("table name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->table_name = std::move(*name);

    // ADD COLUMN
    if (match_ident_ci(peek(), "ADD")) {
        advance();                // consume ADD
        match(TokenType::COLUMN); // optional COLUMN keyword
        stmt->action = AlterAction::ADD_COLUMN;
        auto col = parse_column_def();
        if (!col)
            return tl::unexpected(col.error());
        stmt->column = std::move(*col);
        return ok(StmtPtr(std::move(stmt)));
    }

    if (match(TokenType::DROP)) {
        match(TokenType::COLUMN); // optional COLUMN keyword
        stmt->action = AlterAction::DROP_COLUMN;
        auto col_name = parse_name("column name");
        if (!col_name)
            return tl::unexpected(col_name.error());
        stmt->column_name = std::move(*col_name);
        return ok(StmtPtr(std::move(stmt)));
    }

    // RENAME COLUMN old TO new
    if (match_ident_ci(peek(), "RENAME")) {
        advance();                // consume RENAME
        match(TokenType::COLUMN); // optional COLUMN keyword
        stmt->action = AlterAction::RENAME_COLUMN;
        auto old_name = parse_name("old column name");
        if (!old_name)
            return tl::unexpected(old_name.error());
        stmt->column_name = std::move(*old_name);

        // Expect TO.
        if (match_ident_ci(peek(), "TO")) {
            advance();
        } else {
            return tl::unexpected(error("expected TO after old column name").error());
        }

        auto new_name = parse_name("new column name");
        if (!new_name)
            return tl::unexpected(new_name.error());
        stmt->new_column_name = std::move(*new_name);
        return ok(StmtPtr(std::move(stmt)));
    }

    return error("expected ADD, DROP, or RENAME after ALTER TABLE name");
}

// -- DDL: CREATE INDEX --------------------------------------------------------

Result<StmtPtr> Parser::parse_create_index(bool is_unique) {
    auto stmt = std::make_unique<CreateIndexStmt>();
    stmt->is_unique = is_unique;

    // IF NOT EXISTS
    if (match(TokenType::IF)) {
        auto not_tok = expect(TokenType::NOT, "expected NOT after IF");
        if (!not_tok)
            return tl::unexpected(not_tok.error());
        auto exists_tok = expect(TokenType::EXISTS, "expected EXISTS after NOT");
        if (!exists_tok)
            return tl::unexpected(exists_tok.error());
        stmt->if_not_exists = true;
    }

    // Index name.
    auto name = parse_name("index name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->name = std::move(*name);

    // ON table(cols...)
    auto on = expect(TokenType::ON, "expected ON after index name");
    if (!on)
        return tl::unexpected(on.error());

    auto table = parse_name("table name");
    if (!table)
        return tl::unexpected(table.error());
    stmt->table_name = std::move(*table);

    auto lp = expect(TokenType::LPAREN, "expected '(' after table name");
    if (!lp)
        return tl::unexpected(lp.error());

    do {
        auto col = parse_name("column name");
        if (!col)
            return tl::unexpected(col.error());
        stmt->columns.push_back(std::move(*col));
    } while (match(TokenType::COMMA));

    auto rp = expect(TokenType::RPAREN, "expected ')'");
    if (!rp)
        return tl::unexpected(rp.error());

    // Optional USING method.
    if (match_ident_ci(peek(), "USING")) {
        advance(); // consume USING
        auto method = parse_name("index method");
        if (!method)
            return tl::unexpected(method.error());
        stmt->method = std::move(*method);
    }

    return ok(StmtPtr(std::move(stmt)));
}

// -- DDL: DROP INDEX ----------------------------------------------------------

Result<StmtPtr> Parser::parse_drop_index() {
    auto stmt = std::make_unique<DropIndexStmt>();

    if (match(TokenType::IF)) {
        auto exists = expect(TokenType::EXISTS, "expected EXISTS after IF");
        if (!exists)
            return tl::unexpected(exists.error());
        stmt->if_exists = true;
    }

    auto name = parse_name("index name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->name = std::move(*name);

    return ok(StmtPtr(std::move(stmt)));
}

// -- DDL: CREATE EDGE TYPE ----------------------------------------------------

Result<StmtPtr> Parser::parse_create_edge_type() {
    auto stmt = std::make_unique<CreateEdgeTypeStmt>();

    auto name = parse_name("edge type name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->name = std::move(*name);

    // Optional property list: (prop type, ...)
    if (match(TokenType::LPAREN)) {
        bool first_prop = true;
        while (!check(TokenType::RPAREN) && !at_end()) {
            if (!first_prop) {
                auto comma = expect(TokenType::COMMA, "expected ','");
                if (!comma)
                    return tl::unexpected(comma.error());
            }
            first_prop = false;

            EdgeProperty prop;
            auto prop_name = parse_name("property name");
            if (!prop_name)
                return tl::unexpected(prop_name.error());
            prop.name = std::move(*prop_name);

            auto type = parse_type_spec();
            if (!type)
                return tl::unexpected(type.error());
            prop.type = std::move(*type);

            stmt->properties.push_back(std::move(prop));
        }
        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp)
            return tl::unexpected(rp.error());
    }

    // FROM table TO table.
    auto from = expect(TokenType::FROM, "expected FROM");
    if (!from)
        return tl::unexpected(from.error());
    auto from_tbl = parse_name("source table name");
    if (!from_tbl)
        return tl::unexpected(from_tbl.error());
    stmt->from_table = std::move(*from_tbl);

    // TO uses the identifier "TO", but we don't have a TO keyword.
    if (match_ident_ci(peek(), "TO")) {
        advance();
    } else {
        return tl::unexpected(error("expected TO").error());
    }

    auto to_tbl = parse_name("target table name");
    if (!to_tbl)
        return tl::unexpected(to_tbl.error());
    stmt->to_table = std::move(*to_tbl);

    return ok(StmtPtr(std::move(stmt)));
}

// -- DDL: DROP EDGE TYPE ------------------------------------------------------

Result<StmtPtr> Parser::parse_drop_edge_type() {
    auto stmt = std::make_unique<DropEdgeTypeStmt>();

    if (match(TokenType::IF)) {
        auto exists = expect(TokenType::EXISTS, "expected EXISTS after IF");
        if (!exists)
            return tl::unexpected(exists.error());
        stmt->if_exists = true;
    }

    auto name = parse_name("edge type name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->name = std::move(*name);

    return ok(StmtPtr(std::move(stmt)));
}

// -- DDL: CREATE USER ---------------------------------------------------------

Result<StmtPtr> Parser::parse_create_user() {
    auto stmt = std::make_unique<CreateUserStmt>();

    // Username.
    auto name = parse_name("user name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->username = std::move(*name);

    // WITH PASSWORD 'password'.
    auto with_tok = expect(TokenType::WITH, "expected WITH after user name");
    if (!with_tok)
        return tl::unexpected(with_tok.error());
    auto pw_tok = expect(TokenType::PASSWORD, "expected PASSWORD after WITH");
    if (!pw_tok)
        return tl::unexpected(pw_tok.error());
    auto pass = expect(TokenType::STRING_LITERAL, "expected password string");
    if (!pass)
        return tl::unexpected(pass.error());
    stmt->password = unquote_string(pass->lexeme);

    return ok(StmtPtr(std::move(stmt)));
}

// -- DDL: DROP USER -----------------------------------------------------------

Result<StmtPtr> Parser::parse_drop_user() {
    auto stmt = std::make_unique<DropUserStmt>();

    // IF EXISTS.
    if (match(TokenType::IF)) {
        auto exists = expect(TokenType::EXISTS, "expected EXISTS after IF");
        if (!exists)
            return tl::unexpected(exists.error());
        stmt->if_exists = true;
    }

    // Username.
    auto name = parse_name("user name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->username = std::move(*name);

    return ok(StmtPtr(std::move(stmt)));
}

// -- DDL: ALTER USER ----------------------------------------------------------

Result<StmtPtr> Parser::parse_alter_user() {
    auto stmt = std::make_unique<AlterUserStmt>();

    // Username.
    auto name = parse_name("user name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->username = std::move(*name);

    // WITH PASSWORD 'new_password'.
    auto with_tok = expect(TokenType::WITH, "expected WITH after user name");
    if (!with_tok)
        return tl::unexpected(with_tok.error());
    auto pw_tok = expect(TokenType::PASSWORD, "expected PASSWORD after WITH");
    if (!pw_tok)
        return tl::unexpected(pw_tok.error());
    auto pass = expect(TokenType::STRING_LITERAL, "expected password string");
    if (!pass)
        return tl::unexpected(pass.error());
    stmt->password = unquote_string(pass->lexeme);

    return ok(StmtPtr(std::move(stmt)));
}

// -- DML: INSERT --------------------------------------------------------------

Result<StmtPtr> Parser::parse_insert() {
    advance(); // consume INSERT
    auto into = expect(TokenType::INTO, "expected INTO after INSERT");
    if (!into)
        return tl::unexpected(into.error());

    auto stmt = std::make_unique<InsertStmt>();

    auto name = parse_name("table name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->table_name = std::move(*name);

    // Optional column list.
    if (match(TokenType::LPAREN)) {
        do {
            auto col = parse_name("column name");
            if (!col)
                return tl::unexpected(col.error());
            stmt->columns.push_back(std::move(*col));
        } while (match(TokenType::COMMA));

        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp)
            return tl::unexpected(rp.error());
    }

    // VALUES or SELECT.
    if (match(TokenType::VALUES)) {
        do {
            auto lp = expect(TokenType::LPAREN, "expected '(' for value row");
            if (!lp)
                return tl::unexpected(lp.error());

            std::vector<ExprPtr> row;
            do {
                auto val = parse_expression();
                if (!val)
                    return tl::unexpected(val.error());
                row.push_back(std::move(*val));
            } while (match(TokenType::COMMA));

            auto rp = expect(TokenType::RPAREN, "expected ')'");
            if (!rp)
                return tl::unexpected(rp.error());

            stmt->values.push_back(std::move(row));
        } while (match(TokenType::COMMA));
    } else if (check(TokenType::SELECT)) {
        auto sel = parse_select();
        if (!sel)
            return tl::unexpected(sel.error());
        stmt->select = std::move(*sel);
    } else {
        return error("expected VALUES or SELECT after INSERT INTO");
    }

    // Optional RETURNING.
    auto ret = parse_returning();
    if (!ret)
        return tl::unexpected(ret.error());
    stmt->returning = std::move(*ret);

    return ok(StmtPtr(std::move(stmt)));
}

// -- DML: UPDATE --------------------------------------------------------------

Result<StmtPtr> Parser::parse_update() {
    advance(); // consume UPDATE
    auto stmt = std::make_unique<UpdateStmt>();

    auto name = parse_name("table name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->table_name = std::move(*name);

    auto set = expect(TokenType::SET, "expected SET after table name");
    if (!set)
        return tl::unexpected(set.error());

    // Assignments: col = expr, ...
    do {
        Assignment a;
        auto col = parse_name("column name");
        if (!col)
            return tl::unexpected(col.error());
        a.column = std::move(*col);

        auto eq = expect(TokenType::EQUAL, "expected '=' in SET clause");
        if (!eq)
            return tl::unexpected(eq.error());

        auto val = parse_expression();
        if (!val)
            return tl::unexpected(val.error());
        a.value = std::move(*val);

        stmt->assignments.push_back(std::move(a));
    } while (match(TokenType::COMMA));

    // Optional WHERE.
    if (match(TokenType::WHERE)) {
        auto expr = parse_expression();
        if (!expr)
            return tl::unexpected(expr.error());
        stmt->where_expr = std::move(*expr);
    }

    // Optional RETURNING.
    auto ret = parse_returning();
    if (!ret)
        return tl::unexpected(ret.error());
    stmt->returning = std::move(*ret);

    return ok(StmtPtr(std::move(stmt)));
}

// -- DML: DELETE --------------------------------------------------------------

Result<StmtPtr> Parser::parse_delete() {
    advance(); // consume DELETE
    auto from = expect(TokenType::FROM, "expected FROM after DELETE");
    if (!from)
        return tl::unexpected(from.error());

    auto stmt = std::make_unique<DeleteStmt>();

    auto name = parse_name("table name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->table_name = std::move(*name);

    // Optional WHERE.
    if (match(TokenType::WHERE)) {
        auto expr = parse_expression();
        if (!expr)
            return tl::unexpected(expr.error());
        stmt->where_expr = std::move(*expr);
    }

    // Optional RETURNING.
    auto ret = parse_returning();
    if (!ret)
        return tl::unexpected(ret.error());
    stmt->returning = std::move(*ret);

    return ok(StmtPtr(std::move(stmt)));
}

// -- DML: LINK ----------------------------------------------------------------

Result<StmtPtr> Parser::parse_link() {
    advance(); // consume LINK

    // Parse the source table name. Then disambiguate:
    //   LINK table(key) TO ...  → single LinkStmt (existing)
    //   LINK table TO ...       → BulkLinkStmt (new bulk form)
    auto src_tbl = parse_name("source table");
    if (!src_tbl)
        return tl::unexpected(src_tbl.error());

    // Bulk form: next token is TO (identifier), not '('.
    if (match_ident_ci(peek(), "TO") && !check(TokenType::LPAREN)) {
        return parse_bulk_link(std::move(*src_tbl));
    }

    // Single-edge form: LINK table(key) TO table(key) VIA edge (props).
    auto stmt = std::make_unique<LinkStmt>();
    stmt->source_table = std::move(*src_tbl);

    auto lp1 = expect(TokenType::LPAREN, "expected '('");
    if (!lp1)
        return tl::unexpected(lp1.error());
    auto src_key = parse_expression();
    if (!src_key)
        return tl::unexpected(src_key.error());
    stmt->source_key = std::move(*src_key);
    auto rp1 = expect(TokenType::RPAREN, "expected ')'");
    if (!rp1)
        return tl::unexpected(rp1.error());

    // TO
    if (!match_ident_ci(peek(), "TO")) {
        return error("expected TO after source key");
    }
    advance();

    // target_table(target_key)
    auto tgt_tbl = parse_name("target table");
    if (!tgt_tbl)
        return tl::unexpected(tgt_tbl.error());
    stmt->target_table = std::move(*tgt_tbl);

    auto lp2 = expect(TokenType::LPAREN, "expected '('");
    if (!lp2)
        return tl::unexpected(lp2.error());
    auto tgt_key = parse_expression();
    if (!tgt_key)
        return tl::unexpected(tgt_key.error());
    stmt->target_key = std::move(*tgt_key);
    auto rp2 = expect(TokenType::RPAREN, "expected ')'");
    if (!rp2)
        return tl::unexpected(rp2.error());

    // VIA edge_type
    auto via = expect(TokenType::VIA, "expected VIA");
    if (!via)
        return tl::unexpected(via.error());
    auto edge = parse_name("edge type name");
    if (!edge)
        return tl::unexpected(edge.error());
    stmt->edge_type = std::move(*edge);

    // Optional properties: (prop = val, ...)
    if (match(TokenType::LPAREN)) {
        do {
            Assignment a;
            auto pname = parse_name("property name");
            if (!pname)
                return tl::unexpected(pname.error());
            a.column = std::move(*pname);

            auto eq = expect(TokenType::EQUAL, "expected '='");
            if (!eq)
                return tl::unexpected(eq.error());

            auto val = parse_expression();
            if (!val)
                return tl::unexpected(val.error());
            a.value = std::move(*val);

            stmt->properties.push_back(std::move(a));
        } while (match(TokenType::COMMA));

        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp)
            return tl::unexpected(rp.error());
    }

    return ok(StmtPtr(std::move(stmt)));
}

// -- DML: BULK LINK -----------------------------------------------------------

Result<StmtPtr> Parser::parse_bulk_link(std::string source_table) {
    auto stmt = std::make_unique<BulkLinkStmt>();
    stmt->source_table = std::move(source_table);

    // Consume TO (already verified by caller).
    advance();

    // target_table
    auto tgt_tbl = parse_name("target table");
    if (!tgt_tbl)
        return tl::unexpected(tgt_tbl.error());
    stmt->target_table = std::move(*tgt_tbl);

    // VIA edge_type
    auto via = expect(TokenType::VIA, "expected VIA after target table");
    if (!via)
        return tl::unexpected(via.error());
    auto edge = parse_name("edge type name");
    if (!edge)
        return tl::unexpected(edge.error());
    stmt->edge_type = std::move(*edge);

    // VALUES (row), (row), ...
    auto values_kw = expect(TokenType::VALUES, "expected VALUES after edge type");
    if (!values_kw)
        return tl::unexpected(values_kw.error());

    // Parse row tuples — same pattern as parse_insert() VALUES loop.
    do {
        auto lp = expect(TokenType::LPAREN, "expected '(' for value row");
        if (!lp)
            return tl::unexpected(lp.error());

        std::vector<ExprPtr> row;
        do {
            auto val = parse_expression();
            if (!val)
                return tl::unexpected(val.error());
            row.push_back(std::move(*val));
        } while (match(TokenType::COMMA));

        if (row.size() < 2) {
            return error("each bulk LINK row must have at least 2 values "
                         "(source_key, target_key)");
        }

        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp)
            return tl::unexpected(rp.error());

        stmt->rows.push_back(std::move(row));
    } while (match(TokenType::COMMA));

    if (stmt->rows.empty()) {
        return error("LINK ... VALUES requires at least one row");
    }

    return ok(StmtPtr(std::move(stmt)));
}

// -- DML: UNLINK --------------------------------------------------------------

Result<StmtPtr> Parser::parse_unlink() {
    advance(); // consume UNLINK
    auto stmt = std::make_unique<UnlinkStmt>();

    // source_table(source_key)
    auto src_tbl = parse_name("source table");
    if (!src_tbl)
        return tl::unexpected(src_tbl.error());
    stmt->source_table = std::move(*src_tbl);

    auto lp1 = expect(TokenType::LPAREN, "expected '('");
    if (!lp1)
        return tl::unexpected(lp1.error());
    auto src_key = parse_expression();
    if (!src_key)
        return tl::unexpected(src_key.error());
    stmt->source_key = std::move(*src_key);
    auto rp1 = expect(TokenType::RPAREN, "expected ')'");
    if (!rp1)
        return tl::unexpected(rp1.error());

    // FROM
    auto from = expect(TokenType::FROM, "expected FROM");
    if (!from)
        return tl::unexpected(from.error());

    // target_table(target_key)
    auto tgt_tbl = parse_name("target table");
    if (!tgt_tbl)
        return tl::unexpected(tgt_tbl.error());
    stmt->target_table = std::move(*tgt_tbl);

    auto lp2 = expect(TokenType::LPAREN, "expected '('");
    if (!lp2)
        return tl::unexpected(lp2.error());
    auto tgt_key = parse_expression();
    if (!tgt_key)
        return tl::unexpected(tgt_key.error());
    stmt->target_key = std::move(*tgt_key);
    auto rp2 = expect(TokenType::RPAREN, "expected ')'");
    if (!rp2)
        return tl::unexpected(rp2.error());

    // VIA edge_type
    auto via = expect(TokenType::VIA, "expected VIA");
    if (!via)
        return tl::unexpected(via.error());
    auto edge = parse_name("edge type name");
    if (!edge)
        return tl::unexpected(edge.error());
    stmt->edge_type = std::move(*edge);

    // Optional WHERE.
    if (match(TokenType::WHERE)) {
        auto expr = parse_expression();
        if (!expr)
            return tl::unexpected(expr.error());
        stmt->where_expr = std::move(*expr);
    }

    return ok(StmtPtr(std::move(stmt)));
}

// -- DML helpers: RETURNING ---------------------------------------------------

Result<std::vector<SelectItem>> Parser::parse_returning() {
    std::vector<SelectItem> items;
    if (!match(TokenType::RETURNING))
        return ok(std::move(items));

    do {
        auto item = parse_select_item();
        if (!item)
            return tl::unexpected(item.error());
        items.push_back(std::move(*item));
    } while (match(TokenType::COMMA));

    return ok(std::move(items));
}

// -- Query: WITH (CTEs) -------------------------------------------------------

Result<StmtPtr> Parser::parse_with() {
    advance(); // consume WITH
    std::vector<SelectStmt::CTE> ctes;

    do {
        SelectStmt::CTE cte;
        auto name = parse_name("CTE name");
        if (!name)
            return tl::unexpected(name.error());
        cte.name = std::move(*name);

        auto as_tok = expect(TokenType::AS, "expected AS after CTE name");
        if (!as_tok)
            return tl::unexpected(as_tok.error());

        auto lp = expect(TokenType::LPAREN, "expected '(' before CTE query");
        if (!lp)
            return tl::unexpected(lp.error());

        auto q = parse_select();
        if (!q)
            return tl::unexpected(q.error());
        cte.query = std::move(*q);

        auto rp = expect(TokenType::RPAREN, "expected ')' after CTE query");
        if (!rp)
            return tl::unexpected(rp.error());

        ctes.push_back(std::move(cte));
    } while (match(TokenType::COMMA));

    // Parse the main SELECT.
    auto sel = parse_select();
    if (!sel)
        return sel;
    auto* stmt = dynamic_cast<SelectStmt*>(sel->get());
    stmt->ctes = std::move(ctes);
    return sel;
}

// -- Query: SELECT ------------------------------------------------------------

Result<StmtPtr> Parser::parse_select() {
    advance(); // consume SELECT
    auto stmt = std::make_unique<SelectStmt>();

    // DISTINCT
    if (match(TokenType::DISTINCT)) {
        stmt->distinct = true;
    }

    // Select items.
    do {
        auto item = parse_select_item();
        if (!item)
            return tl::unexpected(item.error());
        stmt->items.push_back(std::move(*item));
    } while (match(TokenType::COMMA));

    // FROM
    if (match(TokenType::FROM)) {
        do {
            auto ref = parse_table_ref();
            if (!ref)
                return tl::unexpected(ref.error());
            stmt->from.push_back(std::move(*ref));
        } while (match(TokenType::COMMA));
    }

    // JOINs.
    while (check(TokenType::INNER) || check(TokenType::LEFT) || check(TokenType::RIGHT) ||
           check(TokenType::FULL) || check(TokenType::CROSS) || check(TokenType::JOIN)) {
        JoinClause join;

        if (match(TokenType::INNER)) {
            join.type = JoinType::INNER;
            auto j = expect(TokenType::JOIN, "expected JOIN after INNER");
            if (!j)
                return tl::unexpected(j.error());
        } else if (match(TokenType::LEFT)) {
            join.type = JoinType::LEFT;
            match(TokenType::OUTER); // optional OUTER
            auto j = expect(TokenType::JOIN, "expected JOIN");
            if (!j)
                return tl::unexpected(j.error());
        } else if (match(TokenType::RIGHT)) {
            join.type = JoinType::RIGHT;
            match(TokenType::OUTER);
            auto j = expect(TokenType::JOIN, "expected JOIN");
            if (!j)
                return tl::unexpected(j.error());
        } else if (match(TokenType::FULL)) {
            join.type = JoinType::FULL;
            match(TokenType::OUTER);
            auto j = expect(TokenType::JOIN, "expected JOIN");
            if (!j)
                return tl::unexpected(j.error());
        } else if (match(TokenType::CROSS)) {
            join.type = JoinType::CROSS;
            auto j = expect(TokenType::JOIN, "expected JOIN after CROSS");
            if (!j)
                return tl::unexpected(j.error());
        } else {
            advance();                   // consume JOIN
            join.type = JoinType::INNER; // bare JOIN defaults to INNER
        }

        auto ref = parse_table_ref();
        if (!ref)
            return tl::unexpected(ref.error());
        join.table = std::move(*ref);

        // ON condition (not for CROSS JOIN).
        if (join.type != JoinType::CROSS) {
            auto on = expect(TokenType::ON, "expected ON after JOIN table");
            if (!on)
                return tl::unexpected(on.error());
            auto expr = parse_expression();
            if (!expr)
                return tl::unexpected(expr.error());
            join.on_expr = std::move(*expr);
        }

        stmt->joins.push_back(std::move(join));
    }

    // WHERE
    if (match(TokenType::WHERE)) {
        auto expr = parse_expression();
        if (!expr)
            return tl::unexpected(expr.error());
        stmt->where_expr = std::move(*expr);
    }

    // GROUP BY
    if (match(TokenType::GROUP)) {
        auto by = expect(TokenType::BY, "expected BY after GROUP");
        if (!by)
            return tl::unexpected(by.error());

        do {
            auto expr = parse_expression();
            if (!expr)
                return tl::unexpected(expr.error());
            stmt->group_by.push_back(std::move(*expr));
        } while (match(TokenType::COMMA));

        // HAVING
        if (match(TokenType::HAVING)) {
            auto expr = parse_expression();
            if (!expr)
                return tl::unexpected(expr.error());
            stmt->having_expr = std::move(*expr);
        }
    }

    // ORDER BY
    if (match(TokenType::ORDER)) {
        auto by = expect(TokenType::BY, "expected BY after ORDER");
        if (!by)
            return tl::unexpected(by.error());

        do {
            OrderByItem item;
            auto expr = parse_expression();
            if (!expr)
                return tl::unexpected(expr.error());
            item.expr = std::move(*expr);

            if (match(TokenType::DESC)) {
                item.direction = SortDirection::DESC;
            } else {
                match(TokenType::ASC);
            }

            stmt->order_by.push_back(std::move(item));
        } while (match(TokenType::COMMA));
    }

    // LIMIT
    if (match(TokenType::LIMIT)) {
        auto expr = parse_expression();
        if (!expr)
            return tl::unexpected(expr.error());
        stmt->limit = std::move(*expr);
    }

    // OFFSET
    if (match(TokenType::OFFSET)) {
        auto expr = parse_expression();
        if (!expr)
            return tl::unexpected(expr.error());
        stmt->offset = std::move(*expr);
    }

    // Set operations: UNION [ALL] / INTERSECT / EXCEPT.
    if (match(TokenType::UNION)) {
        if (match(TokenType::ALL)) {
            stmt->set_op = SelectStmt::SetOp::UNION_ALL;
        } else {
            stmt->set_op = SelectStmt::SetOp::UNION;
        }
        auto rhs = parse_select();
        if (!rhs)
            return tl::unexpected(rhs.error());
        stmt->set_rhs = std::move(*rhs);
    } else if (match(TokenType::INTERSECT)) {
        stmt->set_op = SelectStmt::SetOp::INTERSECT;
        auto rhs = parse_select();
        if (!rhs)
            return tl::unexpected(rhs.error());
        stmt->set_rhs = std::move(*rhs);
    } else if (match(TokenType::EXCEPT)) {
        stmt->set_op = SelectStmt::SetOp::EXCEPT;
        auto rhs = parse_select();
        if (!rhs)
            return tl::unexpected(rhs.error());
        stmt->set_rhs = std::move(*rhs);
    }

    return ok(StmtPtr(std::move(stmt)));
}

// -- Query: SELECT item -------------------------------------------------------

Result<SelectItem> Parser::parse_select_item() {
    SelectItem item;

    // *
    if (match(TokenType::STAR)) {
        item.is_star = true;
        return ok(std::move(item));
    }

    // table.* — lookahead: name DOT STAR
    if (is_name_token(peek().type) && current_ + 2 < tokens_.size() &&
        tokens_[current_ + 1].type == TokenType::DOT &&
        tokens_[current_ + 2].type == TokenType::STAR) {
        item.is_star = true;
        item.table_star = std::string(advance().lexeme);
        advance(); // consume DOT
        advance(); // consume STAR
        return ok(std::move(item));
    }

    // Expression [AS alias] or expression implicit_alias.
    auto expr = parse_expression();
    if (!expr)
        return tl::unexpected(expr.error());
    item.expr = std::move(*expr);

    if (match(TokenType::AS)) {
        auto alias = parse_name("alias");
        if (!alias)
            return tl::unexpected(alias.error());
        item.alias = std::move(*alias);
    } else if (is_name_token(peek().type) && !is_clause_keyword(peek().type)) {
        // Implicit alias (without AS).
        item.alias = std::string(advance().lexeme);
    }

    return ok(std::move(item));
}

// -- Query: table reference ---------------------------------------------------

Result<TableRef> Parser::parse_table_ref() {
    TableRef ref;

    // Subquery: (SELECT ...) [AS] alias
    if (match(TokenType::LPAREN)) {
        auto subquery = parse_select();
        if (!subquery)
            return tl::unexpected(subquery.error());
        ref.subquery = std::move(*subquery);

        auto rp = expect(TokenType::RPAREN, "expected ')' after subquery");
        if (!rp)
            return tl::unexpected(rp.error());

        // Alias (required for subqueries in practice, optional in grammar).
        if (match(TokenType::AS)) {
            auto alias = parse_name("subquery alias");
            if (!alias)
                return tl::unexpected(alias.error());
            ref.alias = std::move(*alias);
        } else if (is_name_token(peek().type) && !is_clause_keyword(peek().type)) {
            ref.alias = std::string(advance().lexeme);
        }

        return ok(std::move(ref));
    }

    // MATCH source: FROM MATCH [p = ANY/ALL SHORTEST | SHORTEST K] (pattern) [AS] alias
    if (peek().type == TokenType::MATCH) {
        advance(); // consume MATCH
        auto match_stmt = std::make_unique<MatchStmt>();

        // Parse optional path selector.
        if (is_name_token(peek().type) && current_ + 1 < tokens_.size() &&
            tokens_[current_ + 1].type == TokenType::EQUAL) {
            match_stmt->path_variable = std::string(advance().lexeme);
            advance(); // consume =

            if (check(TokenType::ANY)) {
                advance();
                auto s = expect(TokenType::SHORTEST, "expected SHORTEST after ANY");
                if (!s)
                    return tl::unexpected(s.error());
                match_stmt->path_selector = PathSelector::ANY_SHORTEST;
            } else if (check(TokenType::ALL)) {
                advance();
                auto s = expect(TokenType::SHORTEST, "expected SHORTEST after ALL");
                if (!s)
                    return tl::unexpected(s.error());
                match_stmt->path_selector = PathSelector::ALL_SHORTEST;
            } else if (check(TokenType::SHORTEST)) {
                advance();
                auto k = expect(TokenType::INTEGER_LITERAL, "expected integer K after SHORTEST");
                if (!k)
                    return tl::unexpected(k.error());
                auto k_val = safe_stoi(k->lexeme);
                if (!k_val)
                    return tl::unexpected(k_val.error());
                if (*k_val < 1) {
                    return make_error(StatusCode::PARSE_ERROR, "SHORTEST K requires K >= 1");
                }
                match_stmt->path_selector = PathSelector::SHORTEST_K;
                match_stmt->shortest_k = *k_val;
            } else {
                return make_error(
                    StatusCode::PARSE_ERROR,
                    "expected ANY SHORTEST, ALL SHORTEST, or SHORTEST K after path variable");
            }
        }

        auto pattern = parse_match_pattern();
        if (!pattern)
            return tl::unexpected(pattern.error());
        match_stmt->pattern = std::move(*pattern);

        // Optional WEIGHT expression (only valid with path selectors).
        if (match(TokenType::WEIGHT)) {
            if (match_stmt->path_selector == PathSelector::NONE) {
                return make_error(StatusCode::PARSE_ERROR,
                                  "WEIGHT clause requires a path selector");
            }
            auto weight = parse_expression();
            if (!weight)
                return tl::unexpected(weight.error());
            match_stmt->weight_expr = std::move(*weight);
        }

        ref.match_source = std::move(match_stmt);

        // Optional alias (explicit AS or implicit).
        if (match(TokenType::AS)) {
            auto alias = parse_name("match alias");
            if (!alias)
                return tl::unexpected(alias.error());
            ref.alias = std::move(*alias);
        } else if (is_name_token(peek().type) && !is_clause_keyword(peek().type)) {
            ref.alias = std::string(advance().lexeme);
        }

        return ok(std::move(ref));
    }

    // TRAVERSE source: TRAVERSE ... [AS] alias
    if (peek().type == TokenType::TRAVERSE) {
        auto traverse = parse_traverse();
        if (!traverse)
            return tl::unexpected(traverse.error());
        ref.traverse_source = std::move(*traverse);

        // Optional alias (explicit AS or implicit).
        if (match(TokenType::AS)) {
            auto alias = parse_name("traverse alias");
            if (!alias)
                return tl::unexpected(alias.error());
            ref.alias = std::move(*alias);
        } else if (is_name_token(peek().type) && !is_clause_keyword(peek().type)) {
            ref.alias = std::string(advance().lexeme);
        }

        return ok(std::move(ref));
    }

    // Regular table name or algorithm function call.
    auto name = parse_name("table name");
    if (!name)
        return tl::unexpected(name.error());

    // Algorithm function call: name(args..., param := value, ...).
    if (check(TokenType::LPAREN)) {
        // Re-parse as a function call expression by rewinding one token.
        // Build a FunctionCallExpr manually using the already-parsed name.
        advance(); // consume (

        auto fn = std::make_unique<FunctionCallExpr>();
        fn->name = *name;

        if (!check(TokenType::RPAREN)) {
            bool seen_named = false;
            do {
                // Check for named parameter: identifier := value.
                if (!seen_named && is_name_token(peek().type) && current_ + 1 < tokens_.size() &&
                    tokens_[current_ + 1].type == TokenType::COLON_EQUAL) {
                    seen_named = true;
                }

                if (seen_named && is_name_token(peek().type) && current_ + 1 < tokens_.size() &&
                    tokens_[current_ + 1].type == TokenType::COLON_EQUAL) {
                    auto param_name = std::string(advance().lexeme);
                    advance(); // consume :=
                    auto val = parse_expression();
                    if (!val)
                        return tl::unexpected(val.error());
                    fn->named_args.push_back({std::move(param_name), std::move(*val)});
                } else if (seen_named) {
                    return tl::unexpected(
                        make_error(StatusCode::PARSE_ERROR,
                                   "positional argument not allowed after named arguments")
                            .value());
                } else {
                    auto arg = parse_expression();
                    if (!arg)
                        return tl::unexpected(arg.error());
                    fn->args.push_back(std::move(*arg));
                }
            } while (match(TokenType::COMMA));
        }

        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp)
            return tl::unexpected(rp.error());

        ref.name = *name;
        ref.algorithm_call = std::move(fn);

        // Optional alias.
        if (match(TokenType::AS)) {
            auto alias = parse_name("function alias");
            if (!alias)
                return tl::unexpected(alias.error());
            ref.alias = std::move(*alias);
        } else if (is_name_token(peek().type) && !is_clause_keyword(peek().type)) {
            ref.alias = std::string(advance().lexeme);
        }

        return ok(std::move(ref));
    }

    // Check for schema-qualified name: schema.table
    if (check(TokenType::DOT)) {
        advance(); // consume DOT
        auto table_name = parse_name("table name after schema qualifier");
        if (!table_name)
            return tl::unexpected(table_name.error());
        ref.schema = std::move(*name);
        ref.name = std::move(*table_name);
    } else {
        ref.name = std::move(*name);
    }

    // Optional alias (explicit AS or implicit).
    if (match(TokenType::AS)) {
        auto alias = parse_name("table alias");
        if (!alias)
            return tl::unexpected(alias.error());
        ref.alias = std::move(*alias);
    } else if (is_name_token(peek().type) && !is_clause_keyword(peek().type)) {
        ref.alias = std::string(advance().lexeme);
    }

    return ok(std::move(ref));
}

// -- Graph: TRAVERSE ----------------------------------------------------------

Result<StmtPtr> Parser::parse_traverse() {
    advance(); // consume TRAVERSE
    auto stmt = std::make_unique<TraverseStmt>();

    auto edge = parse_name("edge type");
    if (!edge)
        return tl::unexpected(edge.error());
    stmt->edge_type = std::move(*edge);

    auto from = expect(TokenType::FROM, "expected FROM after edge type");
    if (!from)
        return tl::unexpected(from.error());

    auto tbl = parse_name("table name");
    if (!tbl)
        return tl::unexpected(tbl.error());
    stmt->from_table = std::move(*tbl);

    auto lp = expect(TokenType::LPAREN, "expected '(' after table name");
    if (!lp)
        return tl::unexpected(lp.error());
    auto key = parse_expression();
    if (!key)
        return tl::unexpected(key.error());
    stmt->from_key = std::move(*key);
    auto rp = expect(TokenType::RPAREN, "expected ')'");
    if (!rp)
        return tl::unexpected(rp.error());

    // Optional DIRECTION IN|OUT|BOTH.
    if (match(TokenType::DIRECTION)) {
        if (match(TokenType::IN)) {
            stmt->direction = TraverseDirection::IN;
        } else if (match_ident_ci(peek(), "OUT")) {
            advance();
            stmt->direction = TraverseDirection::OUT;
        } else if (match_ident_ci(peek(), "BOTH")) {
            advance();
            stmt->direction = TraverseDirection::BOTH;
        } else {
            return error("expected IN, OUT, or BOTH after DIRECTION");
        }
    }

    // Optional MAX_DEPTH n.
    if (match(TokenType::MAX_DEPTH)) {
        auto depth = expect(TokenType::INTEGER_LITERAL, "expected integer after MAX_DEPTH");
        if (!depth)
            return tl::unexpected(depth.error());
        auto depth_val = safe_stoi(depth->lexeme);
        if (!depth_val)
            return tl::unexpected(depth_val.error());
        stmt->max_depth = *depth_val;
    }

    // Optional MODE NODES|EDGES.
    if (match_ident_ci(peek(), "MODE")) {
        advance(); // consume MODE
        if (match_ident_ci(peek(), "EDGES")) {
            advance();
            stmt->mode = TraverseMode::EDGES;
        } else if (match_ident_ci(peek(), "NODES")) {
            advance();
            stmt->mode = TraverseMode::NODES;
        } else {
            return error("expected EDGES or NODES after MODE");
        }
    }

    // Optional WHERE.
    if (match(TokenType::WHERE)) {
        auto expr = parse_expression();
        if (!expr)
            return tl::unexpected(expr.error());
        stmt->where_expr = std::move(*expr);
    }

    // Optional FETCH.
    if (match(TokenType::FETCH)) {
        stmt->fetch = true;
    }

    return ok(StmtPtr(std::move(stmt)));
}

// -- Graph: NEAREST -----------------------------------------------------------

Result<StmtPtr> Parser::parse_nearest() {
    advance(); // consume NEAREST
    auto stmt = std::make_unique<NearestStmt>();

    // k (number of neighbors).
    auto k = parse_expression();
    if (!k)
        return tl::unexpected(k.error());
    stmt->k = std::move(*k);

    // FROM table.column.
    auto from = expect(TokenType::FROM, "expected FROM");
    if (!from)
        return tl::unexpected(from.error());

    auto tbl = parse_name("table name");
    if (!tbl)
        return tl::unexpected(tbl.error());
    stmt->table_name = std::move(*tbl);

    auto dot = expect(TokenType::DOT, "expected '.' after table name");
    if (!dot)
        return tl::unexpected(dot.error());

    auto col = parse_name("column name");
    if (!col)
        return tl::unexpected(col.error());
    stmt->column_name = std::move(*col);

    // TO target.
    if (!match_ident_ci(peek(), "TO")) {
        return error("expected TO after column name");
    }
    advance();

    auto target = parse_expression();
    if (!target)
        return tl::unexpected(target.error());
    stmt->target = std::move(*target);

    // Optional WITHIN TRAVERSE scope.
    // Only parses the core traverse clause (edge, FROM, DIRECTION, MAX_DEPTH)
    // without WHERE/FETCH, since those belong to the outer NEAREST.
    if (match_ident_ci(peek(), "WITHIN")) {
        advance(); // consume WITHIN
        if (!match(TokenType::TRAVERSE)) {
            return error("expected TRAVERSE after WITHIN");
        }
        auto trav = std::make_unique<TraverseStmt>();

        auto edge = parse_name("edge type");
        if (!edge)
            return tl::unexpected(edge.error());
        trav->edge_type = std::move(*edge);

        auto from_tok = expect(TokenType::FROM, "expected FROM after edge type");
        if (!from_tok)
            return tl::unexpected(from_tok.error());

        auto tbl_name = parse_name("table name");
        if (!tbl_name)
            return tl::unexpected(tbl_name.error());
        trav->from_table = std::move(*tbl_name);

        auto lp = expect(TokenType::LPAREN, "expected '(' after table name");
        if (!lp)
            return tl::unexpected(lp.error());
        auto key_expr = parse_expression();
        if (!key_expr)
            return tl::unexpected(key_expr.error());
        trav->from_key = std::move(*key_expr);
        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp)
            return tl::unexpected(rp.error());

        if (match(TokenType::DIRECTION)) {
            if (match(TokenType::IN)) {
                trav->direction = TraverseDirection::IN;
            } else if (match_ident_ci(peek(), "OUT")) {
                advance();
                trav->direction = TraverseDirection::OUT;
            } else if (match_ident_ci(peek(), "BOTH")) {
                advance();
                trav->direction = TraverseDirection::BOTH;
            } else {
                return error("expected IN, OUT, or BOTH after DIRECTION");
            }
        }

        if (match(TokenType::MAX_DEPTH)) {
            auto depth = expect(TokenType::INTEGER_LITERAL, "expected integer after MAX_DEPTH");
            if (!depth)
                return tl::unexpected(depth.error());
            auto depth_val = safe_stoi(depth->lexeme);
            if (!depth_val)
                return tl::unexpected(depth_val.error());
            trav->max_depth = *depth_val;
        }

        stmt->within_traverse = std::move(trav);
    }

    // Optional WHERE.
    if (match(TokenType::WHERE)) {
        auto expr = parse_expression();
        if (!expr)
            return tl::unexpected(expr.error());
        stmt->where_expr = std::move(*expr);
    }

    // Optional USING metric.
    if (match_ident_ci(peek(), "USING")) {
        advance();
        if (match_ident_ci(peek(), "COSINE")) {
            advance();
            stmt->metric = NearestMetric::COSINE;
        } else if (match_ident_ci(peek(), "L2")) {
            advance();
            stmt->metric = NearestMetric::L2;
        } else if (match_ident_ci(peek(), "DOT")) {
            advance();
            stmt->metric = NearestMetric::DOT;
        } else {
            return error("expected COSINE, L2, or DOT after USING");
        }
    }

    return ok(StmtPtr(std::move(stmt)));
}

// -- Graph: MATCH -------------------------------------------------------------

Result<std::vector<PathElement>> Parser::parse_match_pattern() {
    std::vector<PathElement> pattern;

    while (true) {
        auto lp = expect(TokenType::LPAREN, "expected '(' for node pattern");
        if (!lp)
            return tl::unexpected(lp.error());

        NodePattern node;

        if (check(TokenType::COLON)) {
            advance(); // consume :
            auto label = parse_name("node label");
            if (!label)
                return tl::unexpected(label.error());
            node.label = std::move(*label);
        } else if (!check(TokenType::RPAREN) && is_name_token(peek().type)) {
            auto name = parse_name("node variable or label");
            if (!name)
                return tl::unexpected(name.error());

            if (match(TokenType::COLON)) {
                node.variable = std::move(*name);
                auto label = parse_name("node label");
                if (!label)
                    return tl::unexpected(label.error());
                node.label = std::move(*label);
            } else {
                node.variable = std::move(*name);
            }
        }

        // Parse optional inline WHERE predicate: (a:label WHERE expr)
        if (match(TokenType::WHERE)) {
            auto filter = parse_expression();
            if (!filter)
                return tl::unexpected(filter.error());
            node.filter_expr = std::move(*filter);
        }

        auto rp = expect(TokenType::RPAREN, "expected ')' for node pattern");
        if (!rp)
            return tl::unexpected(rp.error());

        PathElement elem;
        elem.node = std::move(node);

        if (check(TokenType::MINUS) || check(TokenType::LESS)) {
            EdgePatternDef edge;
            bool incoming_prefix = false;

            if (match(TokenType::LESS)) {
                incoming_prefix = true;
                auto dash = expect(TokenType::MINUS, "expected '-' after '<'");
                if (!dash)
                    return tl::unexpected(dash.error());
            } else {
                advance(); // consume -
            }

            if (match(TokenType::LBRACKET)) {
                if (check(TokenType::COLON)) {
                    advance(); // consume :
                    auto etype = parse_name("edge type");
                    if (!etype)
                        return tl::unexpected(etype.error());
                    edge.edge_type = std::move(*etype);
                } else if (!check(TokenType::RBRACKET) && is_name_token(peek().type)) {
                    auto name = parse_name("edge variable or type");
                    if (!name)
                        return tl::unexpected(name.error());

                    if (match(TokenType::COLON)) {
                        edge.variable = std::move(*name);
                        auto etype = parse_name("edge type");
                        if (!etype)
                            return tl::unexpected(etype.error());
                        edge.edge_type = std::move(*etype);
                    } else {
                        edge.edge_type = std::move(*name);
                    }
                }

                // Parse optional inline WHERE predicate: [r:type WHERE expr]
                if (match(TokenType::WHERE)) {
                    auto filter = parse_expression();
                    if (!filter)
                        return tl::unexpected(filter.error());
                    edge.filter_expr = std::move(*filter);
                }

                auto rb = expect(TokenType::RBRACKET, "expected ']'");
                if (!rb)
                    return tl::unexpected(rb.error());
            }

            auto dash = expect(TokenType::MINUS, "expected '-' in edge pattern");
            if (!dash)
                return tl::unexpected(dash.error());

            bool outgoing_suffix = match(TokenType::GREATER);

            if (incoming_prefix && !outgoing_suffix) {
                edge.direction = TraverseDirection::IN;
            } else if (!incoming_prefix && outgoing_suffix) {
                edge.direction = TraverseDirection::OUT;
            } else {
                edge.direction = TraverseDirection::BOTH;
            }

            // Parse optional hop quantifier: {min,max}, {n}, +, *
            if (match(TokenType::LBRACE)) {
                auto min_tok = expect(TokenType::INTEGER_LITERAL, "expected integer in quantifier");
                if (!min_tok)
                    return tl::unexpected(min_tok.error());

                int32_t min_val = static_cast<int32_t>(std::stoll(std::string(min_tok->lexeme)));
                edge.min_hops = min_val;

                if (match(TokenType::COMMA)) {
                    // {min,max} form.
                    if (check(TokenType::RBRACE)) {
                        // {min,} — unbounded max.
                        edge.max_hops = std::nullopt;
                    } else {
                        auto max_tok =
                            expect(TokenType::INTEGER_LITERAL, "expected integer for max hops");
                        if (!max_tok)
                            return tl::unexpected(max_tok.error());
                        edge.max_hops =
                            static_cast<int32_t>(std::stoll(std::string(max_tok->lexeme)));
                    }
                } else {
                    // {n} — exact hop count.
                    edge.max_hops = min_val;
                }

                auto rb = expect(TokenType::RBRACE, "expected '}' after quantifier");
                if (!rb)
                    return tl::unexpected(rb.error());
            } else if (match(TokenType::PLUS)) {
                // + shorthand: one or more hops.
                edge.min_hops = 1;
                edge.max_hops = std::nullopt;
            } else if (match(TokenType::STAR)) {
                // * shorthand: zero or more hops.
                edge.min_hops = 0;
                edge.max_hops = std::nullopt;
            }

            elem.outgoing_edge = std::move(edge);
            pattern.push_back(std::move(elem));
        } else {
            pattern.push_back(std::move(elem));
            break;
        }
    }

    return ok(std::move(pattern));
}

Result<StmtPtr> Parser::parse_match() {
    advance(); // consume MATCH
    auto stmt = std::make_unique<MatchStmt>();

    // Parse optional path selector: p = ANY SHORTEST | p = ALL SHORTEST | p = SHORTEST K
    if (is_name_token(peek().type) && current_ + 1 < tokens_.size() &&
        tokens_[current_ + 1].type == TokenType::EQUAL) {
        // Path variable binding: p = ...
        stmt->path_variable = std::string(advance().lexeme); // consume variable name
        advance();                                           // consume =

        if (check(TokenType::ANY)) {
            advance(); // consume ANY
            auto s = expect(TokenType::SHORTEST, "expected SHORTEST after ANY");
            if (!s)
                return tl::unexpected(s.error());
            stmt->path_selector = PathSelector::ANY_SHORTEST;
        } else if (check(TokenType::ALL)) {
            advance(); // consume ALL
            auto s = expect(TokenType::SHORTEST, "expected SHORTEST after ALL");
            if (!s)
                return tl::unexpected(s.error());
            stmt->path_selector = PathSelector::ALL_SHORTEST;
        } else if (check(TokenType::SHORTEST)) {
            advance(); // consume SHORTEST
            auto k = expect(TokenType::INTEGER_LITERAL, "expected integer K after SHORTEST");
            if (!k)
                return tl::unexpected(k.error());
            auto k_val = safe_stoi(k->lexeme);
            if (!k_val)
                return tl::unexpected(k_val.error());
            if (*k_val < 1) {
                return error("SHORTEST K requires K >= 1");
            }
            stmt->path_selector = PathSelector::SHORTEST_K;
            stmt->shortest_k = *k_val;
        } else {
            return error("expected ANY SHORTEST, ALL SHORTEST, or SHORTEST K after path variable");
        }
    }

    auto pattern = parse_match_pattern();
    if (!pattern)
        return tl::unexpected(pattern.error());
    stmt->pattern = std::move(*pattern);

    // Optional WEIGHT expression (only valid with path selectors).
    if (match(TokenType::WEIGHT)) {
        if (stmt->path_selector == PathSelector::NONE) {
            return error("WEIGHT clause requires a path selector (ANY SHORTEST, ALL SHORTEST, or "
                         "SHORTEST K)");
        }
        auto weight = parse_expression();
        if (!weight)
            return tl::unexpected(weight.error());
        stmt->weight_expr = std::move(*weight);
    }

    // Optional WHERE.
    if (match(TokenType::WHERE)) {
        auto expr = parse_expression();
        if (!expr)
            return tl::unexpected(expr.error());
        stmt->where_expr = std::move(*expr);
    }

    // RETURN select_items.
    auto ret = expect(TokenType::RETURN, "expected RETURN");
    if (!ret)
        return tl::unexpected(ret.error());

    do {
        auto item = parse_select_item();
        if (!item)
            return tl::unexpected(item.error());
        stmt->return_items.push_back(std::move(*item));
    } while (match(TokenType::COMMA));

    return ok(StmtPtr(std::move(stmt)));
}

// -- Graph: SHORTEST PATH -----------------------------------------------------

Result<StmtPtr> Parser::parse_shortest_path() {
    advance(); // consume SHORTEST
    auto path = expect(TokenType::PATH, "expected PATH after SHORTEST");
    if (!path)
        return tl::unexpected(path.error());

    auto stmt = std::make_unique<ShortestPathStmt>();

    // FROM table(pk).
    auto from = expect(TokenType::FROM, "expected FROM");
    if (!from)
        return tl::unexpected(from.error());

    auto from_tbl = parse_name("source table");
    if (!from_tbl)
        return tl::unexpected(from_tbl.error());
    stmt->from_table = std::move(*from_tbl);

    auto lp1 = expect(TokenType::LPAREN, "expected '('");
    if (!lp1)
        return tl::unexpected(lp1.error());
    auto from_key = parse_expression();
    if (!from_key)
        return tl::unexpected(from_key.error());
    stmt->from_key = std::move(*from_key);
    auto rp1 = expect(TokenType::RPAREN, "expected ')'");
    if (!rp1)
        return tl::unexpected(rp1.error());

    // TO table(pk).
    if (!match_ident_ci(peek(), "TO")) {
        return error("expected TO");
    }
    advance();

    auto to_tbl = parse_name("target table");
    if (!to_tbl)
        return tl::unexpected(to_tbl.error());
    stmt->to_table = std::move(*to_tbl);

    auto lp2 = expect(TokenType::LPAREN, "expected '('");
    if (!lp2)
        return tl::unexpected(lp2.error());
    auto to_key = parse_expression();
    if (!to_key)
        return tl::unexpected(to_key.error());
    stmt->to_key = std::move(*to_key);
    auto rp2 = expect(TokenType::RPAREN, "expected ')'");
    if (!rp2)
        return tl::unexpected(rp2.error());

    // VIA edge_type.
    auto via = expect(TokenType::VIA, "expected VIA");
    if (!via)
        return tl::unexpected(via.error());
    auto edge = parse_name("edge type");
    if (!edge)
        return tl::unexpected(edge.error());
    stmt->edge_type = std::move(*edge);

    // Optional DIRECTION.
    if (match(TokenType::DIRECTION)) {
        if (match(TokenType::IN)) {
            stmt->direction = TraverseDirection::IN;
        } else if (match_ident_ci(peek(), "OUT")) {
            advance();
            stmt->direction = TraverseDirection::OUT;
        } else if (match_ident_ci(peek(), "BOTH")) {
            advance();
            stmt->direction = TraverseDirection::BOTH;
        } else {
            return error("expected IN, OUT, or BOTH after DIRECTION");
        }
    }

    // Optional MAX_DEPTH.
    if (match(TokenType::MAX_DEPTH)) {
        auto depth = expect(TokenType::INTEGER_LITERAL, "expected integer after MAX_DEPTH");
        if (!depth)
            return tl::unexpected(depth.error());
        auto depth_val = safe_stoi(depth->lexeme);
        if (!depth_val)
            return tl::unexpected(depth_val.error());
        stmt->max_depth = *depth_val;
    }

    return ok(StmtPtr(std::move(stmt)));
}

// -- TCL: BEGIN / COMMIT / ROLLBACK / SAVEPOINT -------------------------------

Result<StmtPtr> Parser::parse_begin() {
    advance();                     // consume BEGIN
    match(TokenType::TRANSACTION); // optional TRANSACTION
    return ok(StmtPtr(std::make_unique<BeginStmt>()));
}

Result<StmtPtr> Parser::parse_commit() {
    advance(); // consume COMMIT
    return ok(StmtPtr(std::make_unique<CommitStmt>()));
}

Result<StmtPtr> Parser::parse_rollback() {
    advance(); // consume ROLLBACK
    auto stmt = std::make_unique<RollbackStmt>();

    // Optional TO savepoint_name.
    if (match_ident_ci(peek(), "TO")) {
        advance();
        auto name = parse_name("savepoint name");
        if (!name)
            return tl::unexpected(name.error());
        stmt->savepoint = std::move(*name);
    }

    return ok(StmtPtr(std::move(stmt)));
}

Result<StmtPtr> Parser::parse_savepoint() {
    advance(); // consume SAVEPOINT
    auto stmt = std::make_unique<SavepointStmt>();

    auto name = parse_name("savepoint name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->name = std::move(*name);

    return ok(StmtPtr(std::move(stmt)));
}

// -- Admin: SET ---------------------------------------------------------------

Result<StmtPtr> Parser::parse_set_stmt() {
    advance(); // consume SET
    auto stmt = std::make_unique<SetStmt>();

    // Parse dotted parameter name (e.g., logging.level).
    auto param = parse_name("parameter name");
    if (!param)
        return tl::unexpected(param.error());
    stmt->parameter = std::move(*param);
    while (check(TokenType::DOT)) {
        advance(); // consume DOT
        auto part = parse_name("parameter name component");
        if (!part)
            return tl::unexpected(part.error());
        stmt->parameter += "." + *part;
    }

    auto eq = expect(TokenType::EQUAL, "expected '=' after parameter");
    if (!eq)
        return tl::unexpected(eq.error());

    auto val = parse_expression();
    if (!val)
        return tl::unexpected(val.error());
    stmt->value = std::move(*val);

    return ok(StmtPtr(std::move(stmt)));
}

// -- Admin: SHOW --------------------------------------------------------------

Result<StmtPtr> Parser::parse_show() {
    advance(); // consume SHOW
    auto stmt = std::make_unique<ShowStmt>();

    // SHOW DATABASES
    if (match_ident_ci(peek(), "DATABASES")) {
        advance();
        stmt->target = ShowTarget::DATABASES;
        return ok(StmtPtr(std::move(stmt)));
    }

    // SHOW TABLES
    if (match_ident_ci(peek(), "TABLES")) {
        advance();
        stmt->target = ShowTarget::TABLES;
        return ok(StmtPtr(std::move(stmt)));
    }

    // SHOW COLUMNS FROM table / SHOW COLUMN FROM table
    if (check(TokenType::COLUMN) || match_ident_ci(peek(), "COLUMNS")) {
        advance(); // consume COLUMN or COLUMNS
        stmt->target = ShowTarget::COLUMNS;
        auto from = expect(TokenType::FROM, "expected FROM after COLUMNS");
        if (!from)
            return tl::unexpected(from.error());
        auto name = parse_name("table name");
        if (!name)
            return tl::unexpected(name.error());
        stmt->name = std::move(*name);
        return ok(StmtPtr(std::move(stmt)));
    }

    // SHOW EDGE TYPE(S)
    if (check(TokenType::EDGE)) {
        advance(); // consume EDGE
        if (check(TokenType::TYPE)) {
            advance();
        } else if (match_ident_ci(peek(), "TYPES")) {
            advance();
        } else {
            return error("expected TYPE(S) after EDGE");
        }
        stmt->target = ShowTarget::EDGE_TYPES;
        return ok(StmtPtr(std::move(stmt)));
    }

    // SHOW INDEXES / SHOW INDEX
    if (check(TokenType::INDEX) || match_ident_ci(peek(), "INDEXES")) {
        advance();
        stmt->target = ShowTarget::INDEXES;
        return ok(StmtPtr(std::move(stmt)));
    }

    // SHOW EMBEDDINGS [FROM table]
    if (match_ident_ci(peek(), "EMBEDDINGS") || match_ident_ci(peek(), "EMBEDDING")) {
        advance();
        stmt->target = ShowTarget::EMBEDDINGS;
        if (check(TokenType::FROM)) {
            advance(); // consume FROM
            auto name = parse_name("table name");
            if (!name)
                return tl::unexpected(name.error());
            stmt->name = std::move(*name);
        }
        return ok(StmtPtr(std::move(stmt)));
    }

    // SHOW BACKFILL [STATUS]
    if (check(TokenType::BACKFILL)) {
        advance(); // consume BACKFILL
        // Optional STATUS keyword.
        if (match_ident_ci(peek(), "STATUS")) {
            advance();
        }
        stmt->target = ShowTarget::BACKFILL;
        return ok(StmtPtr(std::move(stmt)));
    }

    // SHOW PROVIDERS / SHOW PROVIDER
    if (match_ident_ci(peek(), "PROVIDERS") || match_ident_ci(peek(), "PROVIDER")) {
        advance();
        stmt->target = ShowTarget::PROVIDERS;
        return ok(StmtPtr(std::move(stmt)));
    }

    // SHOW REPLICATION SLOTS / SHOW REPLICATION STATUS
    if (match_ident_ci(peek(), "REPLICATION")) {
        advance(); // consume REPLICATION
        if (match_ident_ci(peek(), "SLOTS")) {
            advance(); // consume SLOTS
            stmt->target = ShowTarget::REPLICATION_SLOTS;
            return ok(StmtPtr(std::move(stmt)));
        }
        if (match_ident_ci(peek(), "STATUS")) {
            advance(); // consume STATUS
            stmt->target = ShowTarget::REPLICATION_STATUS;
            return ok(StmtPtr(std::move(stmt)));
        }
        return error("expected SLOTS or STATUS after REPLICATION");
    }

    // SHOW STANDBY STATUS
    if (match_ident_ci(peek(), "STANDBY")) {
        advance(); // consume STANDBY
        if (!match_ident_ci(peek(), "STATUS")) {
            return error("expected STATUS after STANDBY");
        }
        advance(); // consume STATUS
        stmt->target = ShowTarget::STANDBY_STATUS;
        return ok(StmtPtr(std::move(stmt)));
    }

    // SHOW ALL / SHOW SETTINGS
    if (check(TokenType::ALL) || match_ident_ci(peek(), "SETTINGS")) {
        advance();
        stmt->target = ShowTarget::ALL;
        return ok(StmtPtr(std::move(stmt)));
    }

    // SHOW parameter_name (supports dotted names like logging.level)
    stmt->target = ShowTarget::PARAMETER;
    auto name = parse_name("parameter name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->name = std::move(*name);
    while (check(TokenType::DOT)) {
        advance(); // consume DOT
        auto part = parse_name("parameter name component");
        if (!part)
            return tl::unexpected(part.error());
        stmt->name += "." + *part;
    }
    return ok(StmtPtr(std::move(stmt)));
}

// -- Admin: EXPLAIN -----------------------------------------------------------

Result<StmtPtr> Parser::parse_explain() {
    advance(); // consume EXPLAIN
    auto stmt = std::make_unique<ExplainStmt>();

    // Optional ANALYZE.
    if (match(TokenType::ANALYZE)) {
        stmt->analyze = true;
    }

    // Optional FORMAT TEXT|JSON (context-sensitive keyword).
    if (match_ident_ci(peek(), "FORMAT")) {
        advance(); // consume FORMAT
        if (match(TokenType::TEXT)) {
            stmt->format = ExplainFormat::TEXT;
        } else if (match(TokenType::JSON_KW)) {
            stmt->format = ExplainFormat::JSON;
        } else {
            return error("expected TEXT or JSON after FORMAT");
        }
    }

    // Parse the inner statement.
    auto inner = parse_statement();
    if (!inner)
        return tl::unexpected(inner.error());
    stmt->statement = std::move(*inner);

    return ok(StmtPtr(std::move(stmt)));
}

// -- Admin: DESCRIBE ----------------------------------------------------------

Result<StmtPtr> Parser::parse_describe() {
    advance(); // consume DESCRIBE
    auto stmt = std::make_unique<DescribeStmt>();

    auto name = parse_name("table name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->table_name = std::move(*name);

    return ok(StmtPtr(std::move(stmt)));
}

// -- Admin: BACKFILL ----------------------------------------------------------

Result<StmtPtr> Parser::parse_backfill() {
    advance(); // consume BACKFILL

    // Expect EMBEDDINGS keyword.
    if (!check(TokenType::EMBEDDING) && !match_ident_ci(peek(), "EMBEDDINGS")) {
        return error("expected EMBEDDINGS after BACKFILL");
    }
    advance(); // consume EMBEDDINGS

    // Expect ON keyword.
    auto on = expect(TokenType::ON, "expected ON after EMBEDDINGS");
    if (!on)
        return tl::unexpected(on.error());

    auto stmt = std::make_unique<BackfillStmt>();

    auto name = parse_name("table name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->table_name = std::move(*name);

    // Optional BATCH <n>.
    if (match_ident_ci(peek(), "BATCH")) {
        advance(); // consume BATCH
        if (!check(TokenType::INTEGER_LITERAL)) {
            return error("expected integer after BATCH");
        }
        stmt->batch_size = static_cast<uint32_t>(std::stoul(std::string(peek().lexeme)));
        advance();
    }

    // Optional RATE_LIMIT <n>.
    if (match_ident_ci(peek(), "RATE_LIMIT")) {
        advance(); // consume RATE_LIMIT
        if (!check(TokenType::INTEGER_LITERAL)) {
            return error("expected integer after RATE_LIMIT");
        }
        stmt->rate_limit = static_cast<uint32_t>(std::stoul(std::string(peek().lexeme)));
        advance();
    }

    return ok(StmtPtr(std::move(stmt)));
}

// -- Admin: REEMBED -----------------------------------------------------------

Result<StmtPtr> Parser::parse_reembed() {
    advance();               // consume REEMBED
    match(TokenType::TABLE); // optional TABLE keyword
    auto stmt = std::make_unique<ReembedStmt>();

    auto name = parse_name("table name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->table_name = std::move(*name);

    return ok(StmtPtr(std::move(stmt)));
}

// -- Admin: REINDEX -----------------------------------------------------------

Result<StmtPtr> Parser::parse_reindex() {
    advance(); // consume REINDEX
    // Optional INDEX or TABLE keyword.
    if (!match(TokenType::INDEX)) {
        match(TokenType::TABLE);
    }
    auto stmt = std::make_unique<ReindexStmt>();

    auto name = parse_name("index or table name");
    if (!name)
        return tl::unexpected(name.error());
    stmt->name = std::move(*name);

    return ok(StmtPtr(std::move(stmt)));
}

// -- Admin: VACUUM ------------------------------------------------------------

Result<StmtPtr> Parser::parse_vacuum() {
    advance(); // consume VACUUM
    auto stmt = std::make_unique<VacuumStmt>();

    // Optional table name.
    if (!at_end() && !check(TokenType::SEMICOLON) && is_name_token(peek().type)) {
        auto name = parse_name("table name");
        if (!name)
            return tl::unexpected(name.error());
        stmt->table_name = std::move(*name);
    }

    return ok(StmtPtr(std::move(stmt)));
}

// -- Admin: ANALYZE -----------------------------------------------------------

Result<StmtPtr> Parser::parse_analyze_stmt() {
    advance(); // consume ANALYZE
    auto stmt = std::make_unique<AnalyzeStmt>();

    // Optional table name.
    if (!at_end() && !check(TokenType::SEMICOLON) && is_name_token(peek().type)) {
        auto name = parse_name("table name");
        if (!name)
            return tl::unexpected(name.error());
        stmt->table_name = std::move(*name);
    }

    return ok(StmtPtr(std::move(stmt)));
}

// -- Expression parsing -------------------------------------------------------

Result<ExprPtr> Parser::parse_expression() {
    return parse_or();
}

Result<ExprPtr> Parser::parse_or() {
    auto left = parse_and();
    if (!left)
        return left;

    while (match(TokenType::OR)) {
        uint32_t line = previous().line;
        uint32_t col = previous().column;
        auto right = parse_and();
        if (!right)
            return right;

        auto bin = std::make_unique<BinaryExpr>();
        bin->op = BinaryOp::OR;
        bin->lhs = std::move(*left);
        bin->rhs = std::move(*right);
        bin->line = line;
        bin->col = col;
        left = ok(ExprPtr(std::move(bin)));
    }

    return left;
}

Result<ExprPtr> Parser::parse_and() {
    auto left = parse_not();
    if (!left)
        return left;

    while (match(TokenType::AND)) {
        uint32_t line = previous().line;
        uint32_t col = previous().column;
        auto right = parse_not();
        if (!right)
            return right;

        auto bin = std::make_unique<BinaryExpr>();
        bin->op = BinaryOp::AND;
        bin->lhs = std::move(*left);
        bin->rhs = std::move(*right);
        bin->line = line;
        bin->col = col;
        left = ok(ExprPtr(std::move(bin)));
    }

    return left;
}

Result<ExprPtr> Parser::parse_not() {
    if (match(TokenType::NOT)) {
        uint32_t line = previous().line;
        uint32_t col = previous().column;
        auto operand = parse_not();
        if (!operand)
            return operand;

        auto unary = std::make_unique<UnaryExpr>();
        unary->op = UnaryOp::NOT;
        unary->operand = std::move(*operand);
        unary->line = line;
        unary->col = col;
        return ok(ExprPtr(std::move(unary)));
    }

    return parse_comparison();
}

Result<ExprPtr> Parser::parse_comparison() {
    auto left = parse_addition();
    if (!left)
        return left;

    // IS [NOT] NULL
    if (match(TokenType::IS)) {
        uint32_t line = previous().line;
        uint32_t col = previous().column;
        bool negated = match(TokenType::NOT);
        auto null_tok = expect(TokenType::NULL_KW, "expected NULL after IS");
        if (!null_tok)
            return tl::unexpected(null_tok.error());

        auto is_null = std::make_unique<IsNullExpr>();
        is_null->expr = std::move(*left);
        is_null->negated = negated;
        is_null->line = line;
        is_null->col = col;
        return ok(ExprPtr(std::move(is_null)));
    }

    // [NOT] IN / BETWEEN / LIKE — check for NOT modifier.
    bool negated = false;
    if (check(TokenType::NOT) && current_ + 1 < tokens_.size()) {
        auto next_type = tokens_[current_ + 1].type;
        if (next_type == TokenType::IN || next_type == TokenType::BETWEEN ||
            next_type == TokenType::LIKE) {
            advance(); // consume NOT
            negated = true;
        }
    }

    // IN (values...) or IN (SELECT ...)
    if (match(TokenType::IN)) {
        uint32_t line = previous().line;
        uint32_t col = previous().column;

        auto lp = expect(TokenType::LPAREN, "expected '(' after IN");
        if (!lp)
            return tl::unexpected(lp.error());

        auto in_expr = std::make_unique<InExpr>();
        in_expr->expr = std::move(*left);
        in_expr->negated = negated;
        in_expr->line = line;
        in_expr->col = col;

        if (check(TokenType::SELECT)) {
            auto subquery = parse_select();
            if (!subquery)
                return tl::unexpected(subquery.error());
            in_expr->subquery = std::move(*subquery);
        } else {
            do {
                auto val = parse_expression();
                if (!val)
                    return tl::unexpected(val.error());
                in_expr->values.push_back(std::move(*val));
            } while (match(TokenType::COMMA));
        }

        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp)
            return tl::unexpected(rp.error());
        return ok(ExprPtr(std::move(in_expr)));
    }

    // BETWEEN low AND high
    if (match(TokenType::BETWEEN)) {
        uint32_t line = previous().line;
        uint32_t col = previous().column;

        auto low = parse_addition();
        if (!low)
            return tl::unexpected(low.error());

        auto and_tok = expect(TokenType::AND, "expected AND in BETWEEN");
        if (!and_tok)
            return tl::unexpected(and_tok.error());

        auto high = parse_addition();
        if (!high)
            return tl::unexpected(high.error());

        auto between = std::make_unique<BetweenExpr>();
        between->expr = std::move(*left);
        between->low = std::move(*low);
        between->high = std::move(*high);
        between->negated = negated;
        between->line = line;
        between->col = col;
        return ok(ExprPtr(std::move(between)));
    }

    // LIKE pattern
    if (match(TokenType::LIKE)) {
        uint32_t line = previous().line;
        uint32_t col = previous().column;

        auto pattern = parse_addition();
        if (!pattern)
            return tl::unexpected(pattern.error());

        auto like = std::make_unique<LikeExpr>();
        like->expr = std::move(*left);
        like->pattern = std::move(*pattern);
        like->negated = negated;
        like->line = line;
        like->col = col;
        return ok(ExprPtr(std::move(like)));
    }

    // Comparison operators.
    BinaryOp op;
    bool has_op = false;
    if (match(TokenType::EQUAL)) {
        op = BinaryOp::EQUAL;
        has_op = true;
    } else if (match(TokenType::NOT_EQUAL)) {
        op = BinaryOp::NOT_EQUAL;
        has_op = true;
    } else if (match(TokenType::LESS)) {
        op = BinaryOp::LESS;
        has_op = true;
    } else if (match(TokenType::GREATER)) {
        op = BinaryOp::GREATER;
        has_op = true;
    } else if (match(TokenType::LESS_EQUAL)) {
        op = BinaryOp::LESS_EQUAL;
        has_op = true;
    } else if (match(TokenType::GREATER_EQUAL)) {
        op = BinaryOp::GREATER_EQUAL;
        has_op = true;
    }

    if (has_op) {
        uint32_t line = previous().line;
        uint32_t col = previous().column;
        auto right = parse_addition();
        if (!right)
            return right;

        auto bin = std::make_unique<BinaryExpr>();
        bin->op = op;
        bin->lhs = std::move(*left);
        bin->rhs = std::move(*right);
        bin->line = line;
        bin->col = col;
        return ok(ExprPtr(std::move(bin)));
    }

    return left;
}

Result<ExprPtr> Parser::parse_addition() {
    auto left = parse_multiplication();
    if (!left)
        return left;

    while (check(TokenType::PLUS) || check(TokenType::MINUS) || check(TokenType::PIPE_PIPE)) {
        Token op_tok = advance();
        BinaryOp op;
        switch (op_tok.type) {
        case TokenType::PLUS:
            op = BinaryOp::ADD;
            break;
        case TokenType::MINUS:
            op = BinaryOp::SUBTRACT;
            break;
        case TokenType::PIPE_PIPE:
            op = BinaryOp::CONCAT;
            break;
        default:
            op = BinaryOp::ADD;
            break; // unreachable
        }

        auto right = parse_multiplication();
        if (!right)
            return right;

        auto bin = std::make_unique<BinaryExpr>();
        bin->op = op;
        bin->lhs = std::move(*left);
        bin->rhs = std::move(*right);
        bin->line = op_tok.line;
        bin->col = op_tok.column;
        left = ok(ExprPtr(std::move(bin)));
    }

    return left;
}

Result<ExprPtr> Parser::parse_multiplication() {
    auto left = parse_unary();
    if (!left)
        return left;

    while (check(TokenType::STAR) || check(TokenType::SLASH) || check(TokenType::PERCENT)) {
        Token op_tok = advance();
        BinaryOp op;
        switch (op_tok.type) {
        case TokenType::STAR:
            op = BinaryOp::MULTIPLY;
            break;
        case TokenType::SLASH:
            op = BinaryOp::DIVIDE;
            break;
        case TokenType::PERCENT:
            op = BinaryOp::MODULO;
            break;
        default:
            op = BinaryOp::MULTIPLY;
            break; // unreachable
        }

        auto right = parse_unary();
        if (!right)
            return right;

        auto bin = std::make_unique<BinaryExpr>();
        bin->op = op;
        bin->lhs = std::move(*left);
        bin->rhs = std::move(*right);
        bin->line = op_tok.line;
        bin->col = op_tok.column;
        left = ok(ExprPtr(std::move(bin)));
    }

    return left;
}

Result<ExprPtr> Parser::parse_unary() {
    if (match(TokenType::MINUS)) {
        uint32_t line = previous().line;
        uint32_t col = previous().column;
        auto operand = parse_unary();
        if (!operand)
            return operand;

        auto unary = std::make_unique<UnaryExpr>();
        unary->op = UnaryOp::NEGATE;
        unary->operand = std::move(*operand);
        unary->line = line;
        unary->col = col;
        return ok(ExprPtr(std::move(unary)));
    }

    return parse_postfix();
}

Result<ExprPtr> Parser::parse_postfix() {
    auto expr = parse_primary();
    if (!expr)
        return expr;

    // :: type cast (PostgreSQL-style).
    while (match(TokenType::COLON_COLON)) {
        uint32_t line = previous().line;
        uint32_t col = previous().column;

        auto ts = parse_type_spec();
        if (!ts)
            return tl::unexpected(ts.error());

        auto cast = std::make_unique<CastExpr>();
        cast->expr = std::move(*expr);
        cast->target_type = std::move(*ts);
        cast->line = line;
        cast->col = col;
        expr = ok(ExprPtr(std::move(cast)));
    }

    return expr;
}

Result<ExprPtr> Parser::parse_primary() {
    const auto& tok = peek();

    // Integer literal.
    if (match(TokenType::INTEGER_LITERAL)) {
        auto lit = std::make_unique<LiteralExpr>();
        lit->kind = LiteralKind::INTEGER;
        lit->value = std::string(previous().lexeme);
        lit->line = previous().line;
        lit->col = previous().column;
        return ok(ExprPtr(std::move(lit)));
    }

    // Float literal.
    if (match(TokenType::FLOAT_LITERAL)) {
        auto lit = std::make_unique<LiteralExpr>();
        lit->kind = LiteralKind::FLOAT;
        lit->value = std::string(previous().lexeme);
        lit->line = previous().line;
        lit->col = previous().column;
        return ok(ExprPtr(std::move(lit)));
    }

    // String literal.
    if (match(TokenType::STRING_LITERAL)) {
        auto lit = std::make_unique<LiteralExpr>();
        lit->kind = LiteralKind::STRING;
        lit->value = unquote_string(previous().lexeme);
        lit->line = previous().line;
        lit->col = previous().column;
        return ok(ExprPtr(std::move(lit)));
    }

    // Boolean literals.
    if (match(TokenType::TRUE_KW)) {
        auto lit = std::make_unique<LiteralExpr>();
        lit->kind = LiteralKind::BOOLEAN;
        lit->value = "true";
        lit->line = previous().line;
        lit->col = previous().column;
        return ok(ExprPtr(std::move(lit)));
    }
    if (match(TokenType::FALSE_KW)) {
        auto lit = std::make_unique<LiteralExpr>();
        lit->kind = LiteralKind::BOOLEAN;
        lit->value = "false";
        lit->line = previous().line;
        lit->col = previous().column;
        return ok(ExprPtr(std::move(lit)));
    }

    // NULL.
    if (match(TokenType::NULL_KW)) {
        auto lit = std::make_unique<LiteralExpr>();
        lit->kind = LiteralKind::NULL_LITERAL;
        lit->line = previous().line;
        lit->col = previous().column;
        return ok(ExprPtr(std::move(lit)));
    }

    // EXISTS (SELECT ...).
    if (match(TokenType::EXISTS)) {
        uint32_t line = previous().line;
        uint32_t col = previous().column;

        auto lp = expect(TokenType::LPAREN, "expected '(' after EXISTS");
        if (!lp)
            return tl::unexpected(lp.error());

        auto subquery = parse_select();
        if (!subquery)
            return tl::unexpected(subquery.error());

        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp)
            return tl::unexpected(rp.error());

        auto exists = std::make_unique<ExistsExpr>();
        exists->subquery = std::move(*subquery);
        exists->line = line;
        exists->col = col;
        return ok(ExprPtr(std::move(exists)));
    }

    // CASE expression.
    if (match(TokenType::CASE)) {
        uint32_t line = previous().line;
        uint32_t col = previous().column;

        auto case_expr = std::make_unique<CaseExpr>();
        case_expr->line = line;
        case_expr->col = col;

        // Simple CASE: CASE operand WHEN ...
        if (!check(TokenType::WHEN)) {
            auto operand = parse_expression();
            if (!operand)
                return tl::unexpected(operand.error());
            case_expr->operand = std::move(*operand);
        }

        // WHEN clauses.
        while (match(TokenType::WHEN)) {
            CaseWhen cw;
            auto cond = parse_expression();
            if (!cond)
                return tl::unexpected(cond.error());
            cw.condition = std::move(*cond);

            auto then_tok = expect(TokenType::THEN, "expected THEN");
            if (!then_tok)
                return tl::unexpected(then_tok.error());

            auto result = parse_expression();
            if (!result)
                return tl::unexpected(result.error());
            cw.result = std::move(*result);

            case_expr->whens.push_back(std::move(cw));
        }

        // Optional ELSE.
        if (match(TokenType::ELSE)) {
            auto else_expr = parse_expression();
            if (!else_expr)
                return tl::unexpected(else_expr.error());
            case_expr->else_expr = std::move(*else_expr);
        }

        auto end = expect(TokenType::END, "expected END");
        if (!end)
            return tl::unexpected(end.error());
        return ok(ExprPtr(std::move(case_expr)));
    }

    // Parenthesized expression or subquery.
    if (match(TokenType::LPAREN)) {
        // Subquery: (SELECT ...)
        if (check(TokenType::SELECT)) {
            auto subquery = parse_select();
            if (!subquery)
                return tl::unexpected(subquery.error());

            auto rp = expect(TokenType::RPAREN, "expected ')'");
            if (!rp)
                return tl::unexpected(rp.error());

            auto sub = std::make_unique<SubqueryExpr>();
            sub->subquery = std::move(*subquery);
            return ok(ExprPtr(std::move(sub)));
        }

        auto expr = parse_expression();
        if (!expr)
            return expr;
        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp)
            return tl::unexpected(rp.error());
        return expr;
    }

    // Array literal: [elem, ...]
    if (match(TokenType::LBRACKET)) {
        uint32_t line = previous().line;
        uint32_t col = previous().column;

        auto arr = std::make_unique<ArrayExpr>();
        arr->line = line;
        arr->col = col;

        if (!check(TokenType::RBRACKET)) {
            do {
                auto elem = parse_expression();
                if (!elem)
                    return tl::unexpected(elem.error());
                arr->elements.push_back(std::move(*elem));
            } while (match(TokenType::COMMA));
        }

        auto rb = expect(TokenType::RBRACKET, "expected ']'");
        if (!rb)
            return tl::unexpected(rb.error());
        return ok(ExprPtr(std::move(arr)));
    }

    // Identifier, column reference, function call, or CAST.
    if (is_name_token(tok.type)) {
        std::string name(advance().lexeme);
        uint32_t line = previous().line;
        uint32_t col = previous().column;

        // Normalize to uppercase for keyword-like checks.
        std::string name_upper;
        name_upper.reserve(name.size());
        for (char c : name) {
            name_upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }

        // CAST(expr AS type)
        if (name_upper == "CAST" && check(TokenType::LPAREN)) {
            advance(); // consume (
            auto expr = parse_expression();
            if (!expr)
                return tl::unexpected(expr.error());

            auto as_tok = expect(TokenType::AS, "expected AS in CAST");
            if (!as_tok)
                return tl::unexpected(as_tok.error());

            auto ts = parse_type_spec();
            if (!ts)
                return tl::unexpected(ts.error());

            auto rp = expect(TokenType::RPAREN, "expected ')' after CAST");
            if (!rp)
                return tl::unexpected(rp.error());

            auto cast = std::make_unique<CastExpr>();
            cast->expr = std::move(*expr);
            cast->target_type = std::move(*ts);
            cast->line = line;
            cast->col = col;
            return ok(ExprPtr(std::move(cast)));
        }

        // Function call: name(...)
        if (match(TokenType::LPAREN)) {
            auto fn = std::make_unique<FunctionCallExpr>();
            fn->name = std::move(name);
            fn->line = line;
            fn->col = col;

            // DISTINCT for aggregates: COUNT(DISTINCT col).
            if (match(TokenType::DISTINCT)) {
                fn->distinct = true;
            }

            // STAR for COUNT(*).
            if (check(TokenType::STAR) && !fn->distinct) {
                advance(); // consume *
                // Represent * as a ColumnRefExpr with column="*".
                auto star = std::make_unique<ColumnRefExpr>();
                star->column = "*";
                fn->args.push_back(std::move(star));
            } else if (!check(TokenType::RPAREN)) {
                bool seen_named = false;
                do {
                    // Check for named parameter: identifier := value.
                    if (!seen_named && is_name_token(peek().type) &&
                        current_ + 1 < tokens_.size() &&
                        tokens_[current_ + 1].type == TokenType::COLON_EQUAL) {
                        seen_named = true;
                    }

                    if (seen_named && is_name_token(peek().type) && current_ + 1 < tokens_.size() &&
                        tokens_[current_ + 1].type == TokenType::COLON_EQUAL) {
                        auto param_name = std::string(advance().lexeme);
                        advance(); // consume :=
                        auto val = parse_expression();
                        if (!val)
                            return val;
                        fn->named_args.push_back({std::move(param_name), std::move(*val)});
                    } else if (seen_named) {
                        return make_error(StatusCode::PARSE_ERROR,
                                          "positional argument not allowed after named arguments");
                    } else {
                        auto arg = parse_expression();
                        if (!arg)
                            return arg;
                        fn->args.push_back(std::move(*arg));
                    }
                } while (match(TokenType::COMMA));
            }

            auto rp = expect(TokenType::RPAREN, "expected ')'");
            if (!rp)
                return tl::unexpected(rp.error());

            // Window function: func(...) OVER (...)
            if (match(TokenType::OVER)) {
                auto win = std::make_unique<WindowFunctionExpr>();
                win->name = std::move(fn->name);
                win->args = std::move(fn->args);
                win->distinct = fn->distinct;
                win->line = line;
                win->col = col;

                auto lp = expect(TokenType::LPAREN, "expected '(' after OVER");
                if (!lp)
                    return tl::unexpected(lp.error());

                // PARTITION BY clause (optional).
                if (match(TokenType::PARTITION)) {
                    auto by_tok = expect(TokenType::BY, "expected BY after PARTITION");
                    if (!by_tok)
                        return tl::unexpected(by_tok.error());
                    do {
                        auto pb_expr = parse_expression();
                        if (!pb_expr)
                            return pb_expr;
                        win->partition_by.push_back(std::move(*pb_expr));
                    } while (match(TokenType::COMMA));
                }

                // ORDER BY clause (optional).
                if (match(TokenType::ORDER)) {
                    auto by_tok = expect(TokenType::BY, "expected BY after ORDER");
                    if (!by_tok)
                        return tl::unexpected(by_tok.error());
                    do {
                        auto ob_expr = parse_expression();
                        if (!ob_expr)
                            return ob_expr;
                        SortDirection dir = SortDirection::ASC;
                        if (match(TokenType::DESC)) {
                            dir = SortDirection::DESC;
                        } else {
                            match(TokenType::ASC); // consume optional ASC
                        }
                        win->order_by.push_back({std::move(*ob_expr), dir});
                    } while (match(TokenType::COMMA));
                }

                // Frame specification (optional): ROWS/RANGE BETWEEN ... AND ...
                if (check(TokenType::ROWS) || check(TokenType::RANGE)) {
                    AstWindowFrame frame;
                    frame.type = (advance().type == TokenType::ROWS) ? AstFrameType::ROWS
                                                                     : AstFrameType::RANGE;

                    // Parse frame bounds.
                    auto parse_bound = [&](AstFrameBound& bound, int64_t& offset) -> Result<void> {
                        if (match(TokenType::CURRENT)) {
                            auto row_tok = parse_name("ROW");
                            if (!row_tok)
                                return tl::unexpected(row_tok.error());
                            bound = AstFrameBound::CURRENT_ROW;
                        } else if (match(TokenType::UNBOUNDED)) {
                            if (match(TokenType::PRECEDING)) {
                                bound = AstFrameBound::UNBOUNDED_PRECEDING;
                            } else if (match(TokenType::FOLLOWING)) {
                                bound = AstFrameBound::UNBOUNDED_FOLLOWING;
                            } else {
                                return make_error(
                                    StatusCode::PARSE_ERROR,
                                    "expected PRECEDING or FOLLOWING after UNBOUNDED");
                            }
                        } else if (check(TokenType::INTEGER_LITERAL)) {
                            offset = std::stoll(std::string(advance().lexeme));
                            if (match(TokenType::PRECEDING)) {
                                bound = AstFrameBound::N_PRECEDING;
                            } else if (match(TokenType::FOLLOWING)) {
                                bound = AstFrameBound::N_FOLLOWING;
                            } else {
                                return make_error(StatusCode::PARSE_ERROR,
                                                  "expected PRECEDING or FOLLOWING after offset");
                            }
                        } else {
                            return make_error(StatusCode::PARSE_ERROR,
                                              "expected frame bound specification");
                        }
                        return ok();
                    };

                    if (match(TokenType::BETWEEN)) {
                        auto start_res = parse_bound(frame.start_bound, frame.start_offset);
                        if (!start_res)
                            return tl::unexpected(start_res.error());
                        auto and_tok = expect(TokenType::AND, "expected AND in frame clause");
                        if (!and_tok)
                            return tl::unexpected(and_tok.error());
                        auto end_res = parse_bound(frame.end_bound, frame.end_offset);
                        if (!end_res)
                            return tl::unexpected(end_res.error());
                    } else {
                        // Shorthand: ROWS/RANGE <bound> (end is CURRENT ROW).
                        auto start_res = parse_bound(frame.start_bound, frame.start_offset);
                        if (!start_res)
                            return tl::unexpected(start_res.error());
                        frame.end_bound = AstFrameBound::CURRENT_ROW;
                    }

                    win->frame = frame;
                }

                auto rp2 = expect(TokenType::RPAREN, "expected ')' after OVER clause");
                if (!rp2)
                    return tl::unexpected(rp2.error());
                return ok(ExprPtr(std::move(win)));
            }

            return ok(ExprPtr(std::move(fn)));
        }

        // Qualified column: table.column
        if (match(TokenType::DOT)) {
            auto col_name = parse_name("column name");
            if (!col_name)
                return tl::unexpected(col_name.error());

            auto ref = std::make_unique<ColumnRefExpr>();
            ref->table = std::move(name);
            ref->column = std::move(*col_name);
            ref->line = line;
            ref->col = col;
            return ok(ExprPtr(std::move(ref)));
        }

        // Unqualified column reference.
        auto ref = std::make_unique<ColumnRefExpr>();
        ref->column = std::move(name);
        ref->line = line;
        ref->col = col;
        return ok(ExprPtr(std::move(ref)));
    }

    return make_error(StatusCode::PARSE_ERROR,
                      "expected expression at line " + std::to_string(tok.line) + ", column " +
                          std::to_string(tok.column) + " (got " +
                          std::string(token_type_name(tok.type)) + ")");
}

} // namespace sixseven
