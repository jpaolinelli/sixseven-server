#include "giodb/parser/parser.h"

#include <algorithm>

namespace giodb {

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
    case TokenType::NEAREST:
    case TokenType::SHORTEST:
    case TokenType::TRAVERSE:
    case TokenType::MATCH:
    case TokenType::LINK:
    case TokenType::UNLINK:
    case TokenType::REEMBED:
    case TokenType::VIA:
    case TokenType::MAX_DEPTH:
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

/// Strip single quotes from a string literal lexeme and unescape '' -> '.
std::string unquote_string(std::string_view lexeme) {
    // Remove surrounding quotes.
    if (lexeme.size() >= 2 && lexeme.front() == '\'' && lexeme.back() == '\'') {
        lexeme = lexeme.substr(1, lexeme.size() - 2);
    }
    std::string result;
    result.reserve(lexeme.size());
    for (size_t i = 0; i < lexeme.size(); ++i) {
        if (lexeme[i] == '\'' && i + 1 < lexeme.size() &&
            lexeme[i + 1] == '\'') {
            result += '\'';
            ++i;
        } else {
            result += lexeme[i];
        }
    }
    return result;
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
    if (!stmt) return stmt;

    // Consume optional trailing semicolon.
    match(TokenType::SEMICOLON);
    return stmt;
}

Result<std::vector<StmtPtr>> Parser::parse_all() {
    std::vector<StmtPtr> statements;
    std::vector<std::string> errors;

    while (!at_end()) {
        // Skip stray semicolons.
        if (match(TokenType::SEMICOLON)) continue;

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
    return make_error(
        StatusCode::PARSE_ERROR,
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
    return make_error(
        StatusCode::PARSE_ERROR,
        "expected " + context + " at line " + std::to_string(tok.line) +
            ", column " + std::to_string(tok.column) + " (got " +
            std::string(token_type_name(tok.type)) + ")");
}

// -- Error recovery -----------------------------------------------------------

void Parser::synchronize() {
    while (!at_end()) {
        if (match(TokenType::SEMICOLON)) return;
        advance();
    }
}

Result<StmtPtr> Parser::error(const std::string& message) {
    const auto& tok = peek();
    return make_error(
        StatusCode::PARSE_ERROR,
        message + " at line " + std::to_string(tok.line) + ", column " +
            std::to_string(tok.column));
}

// -- Statement dispatch -------------------------------------------------------

Result<StmtPtr> Parser::parse_statement() {
    switch (peek().type) {
    case TokenType::CREATE: return parse_create();
    case TokenType::DROP: return parse_drop();
    case TokenType::ALTER: return parse_alter();
    default:
        return error("expected statement (CREATE, DROP, ALTER, ...)");
    }
}

Result<StmtPtr> Parser::parse_create() {
    advance(); // consume CREATE

    if (match(TokenType::TABLE)) return parse_create_table();
    if (match(TokenType::INDEX)) return parse_create_index(false);
    if (check(TokenType::UNIQUE)) {
        advance(); // consume UNIQUE
        auto idx = expect(TokenType::INDEX, "expected INDEX after UNIQUE");
        if (!idx) return tl::unexpected(idx.error());
        return parse_create_index(true);
    }
    if (match(TokenType::EDGE)) {
        auto type = expect(TokenType::TYPE, "expected TYPE after EDGE");
        if (!type) return tl::unexpected(type.error());
        return parse_create_edge_type();
    }

    return error("expected TABLE, INDEX, UNIQUE INDEX, or EDGE TYPE after CREATE");
}

Result<StmtPtr> Parser::parse_drop() {
    advance(); // consume DROP

    if (match(TokenType::TABLE)) return parse_drop_table();
    if (match(TokenType::INDEX)) return parse_drop_index();
    if (match(TokenType::EDGE)) {
        auto type = expect(TokenType::TYPE, "expected TYPE after EDGE");
        if (!type) return tl::unexpected(type.error());
        return parse_drop_edge_type();
    }

    return error("expected TABLE, INDEX, or EDGE TYPE after DROP");
}

Result<StmtPtr> Parser::parse_alter() {
    advance(); // consume ALTER

    auto tbl = expect(TokenType::TABLE, "expected TABLE after ALTER");
    if (!tbl) return tl::unexpected(tbl.error());
    return parse_alter_table();
}

// -- DDL: CREATE TABLE --------------------------------------------------------

Result<StmtPtr> Parser::parse_create_table() {
    auto stmt = std::make_unique<CreateTableStmt>();

    // IF NOT EXISTS
    if (match(TokenType::IF)) {
        auto not_tok = expect(TokenType::NOT, "expected NOT after IF");
        if (!not_tok) return tl::unexpected(not_tok.error());
        auto exists_tok = expect(TokenType::EXISTS, "expected EXISTS after NOT");
        if (!exists_tok) return tl::unexpected(exists_tok.error());
        stmt->if_not_exists = true;
    }

    // Table name.
    auto name = parse_name("table name");
    if (!name) return tl::unexpected(name.error());
    stmt->name = std::move(*name);

    // ( column_defs, ... [, table_constraints, ...] )
    auto lp = expect(TokenType::LPAREN, "expected '(' after table name");
    if (!lp) return tl::unexpected(lp.error());

    bool first = true;
    while (!check(TokenType::RPAREN) && !at_end()) {
        if (!first) {
            auto comma = expect(TokenType::COMMA, "expected ',' between definitions");
            if (!comma) return tl::unexpected(comma.error());
        }
        first = false;

        // Peek ahead to decide: table constraint or column def.
        if (check(TokenType::PRIMARY) || check(TokenType::UNIQUE) ||
            check(TokenType::CHECK) || check(TokenType::FOREIGN) ||
            check(TokenType::CONSTRAINT)) {
            auto tc = parse_table_constraint();
            if (!tc) return tl::unexpected(tc.error());
            stmt->constraints.push_back(std::move(*tc));
        } else {
            auto col = parse_column_def();
            if (!col) return tl::unexpected(col.error());

            // If column has inline PRIMARY KEY, convert to table constraint.
            // (We detect this via a sentinel — see parse_column_def.)
            stmt->columns.push_back(std::move(*col));
        }
    }

    auto rp = expect(TokenType::RPAREN, "expected ')' after column definitions");
    if (!rp) return tl::unexpected(rp.error());

    return ok(StmtPtr(std::move(stmt)));
}

// -- DDL: column definition ---------------------------------------------------

Result<AstColumnDef> Parser::parse_column_def() {
    AstColumnDef col;

    // Column name.
    auto name = parse_name("column name");
    if (!name) return tl::unexpected(name.error());
    col.name = std::move(*name);

    // Column type.
    auto type = parse_type_spec();
    if (!type) return tl::unexpected(type.error());
    col.type = std::move(*type);

    // Column constraints (in any order).
    while (!check(TokenType::COMMA) && !check(TokenType::RPAREN) && !at_end()) {
        if (match(TokenType::NOT)) {
            auto null_tok = expect(TokenType::NULL_KW, "expected NULL after NOT");
            if (!null_tok) return tl::unexpected(null_tok.error());
            col.nullable = false;
        } else if (match(TokenType::NULL_KW)) {
            col.nullable = true;
        } else if (match(TokenType::UNIQUE)) {
            col.is_unique = true;
        } else if (match(TokenType::PRIMARY)) {
            auto key_tok = expect(TokenType::KEY, "expected KEY after PRIMARY");
            if (!key_tok) return tl::unexpected(key_tok.error());
            col.nullable = false; // PRIMARY KEY implies NOT NULL.
            col.is_unique = true;
        } else if (match(TokenType::DEFAULT)) {
            auto expr = parse_expression();
            if (!expr) return tl::unexpected(expr.error());
            col.default_expr = std::move(*expr);
        } else if (match(TokenType::CHECK)) {
            auto lp = expect(TokenType::LPAREN, "expected '(' after CHECK");
            if (!lp) return tl::unexpected(lp.error());
            auto expr = parse_expression();
            if (!expr) return tl::unexpected(expr.error());
            auto rp = expect(TokenType::RPAREN, "expected ')' after CHECK expression");
            if (!rp) return tl::unexpected(rp.error());
            col.check_expr = std::move(*expr);
        } else if (match(TokenType::REFERENCES)) {
            auto tbl_name = parse_name("referenced table name");
            if (!tbl_name) return tl::unexpected(tbl_name.error());
            col.fk_table = std::move(*tbl_name);
            auto lp = expect(TokenType::LPAREN, "expected '(' after table name");
            if (!lp) return tl::unexpected(lp.error());
            auto col_name = parse_name("referenced column name");
            if (!col_name) return tl::unexpected(col_name.error());
            col.fk_column = std::move(*col_name);
            auto rp = expect(TokenType::RPAREN, "expected ')'");
            if (!rp) return tl::unexpected(rp.error());
            // ON DELETE action.
            if (match(TokenType::ON)) {
                auto del = expect(TokenType::DELETE,
                                  "expected DELETE after ON");
                if (!del) return tl::unexpected(del.error());
                if (match(TokenType::CASCADE)) {
                    col.fk_on_delete = ReferentialAction::CASCADE;
                } else if (match(TokenType::RESTRICT)) {
                    col.fk_on_delete = ReferentialAction::RESTRICT;
                } else {
                    return tl::unexpected(
                        error("expected CASCADE or RESTRICT").error());
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
                          "expected type name at line " +
                              std::to_string(tok.line) + ", column " +
                              std::to_string(tok.column));
    }

    ts.name = std::string(advance().lexeme);
    // Normalize to uppercase.
    std::transform(ts.name.begin(), ts.name.end(), ts.name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    // EMBEDDING(dim, source, 'provider')
    if (ts.name == "EMBEDDING" && match(TokenType::LPAREN)) {
        // Dimension.
        auto dim = expect(TokenType::INTEGER_LITERAL, "expected dimension");
        if (!dim) return tl::unexpected(dim.error());
        ts.param1 = std::stoi(std::string(dim->lexeme));

        auto c1 = expect(TokenType::COMMA, "expected ',' after dimension");
        if (!c1) return tl::unexpected(c1.error());

        // Source column name.
        auto src = parse_name("source column");
        if (!src) return tl::unexpected(src.error());
        ts.source = std::move(*src);

        auto c2 = expect(TokenType::COMMA, "expected ',' after source");
        if (!c2) return tl::unexpected(c2.error());

        // Provider (string literal).
        auto prov = expect(TokenType::STRING_LITERAL, "expected provider string");
        if (!prov) return tl::unexpected(prov.error());
        ts.provider = unquote_string(prov->lexeme);

        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp) return tl::unexpected(rp.error());
        return ok(std::move(ts));
    }

    // Types with optional parameters: VARCHAR(n), CHAR(n), DECIMAL(p,s),
    // NUMERIC(p,s).
    if (match(TokenType::LPAREN)) {
        auto p1 = expect(TokenType::INTEGER_LITERAL,
                         "expected parameter value");
        if (!p1) return tl::unexpected(p1.error());
        ts.param1 = std::stoi(std::string(p1->lexeme));

        if (match(TokenType::COMMA)) {
            auto p2 = expect(TokenType::INTEGER_LITERAL,
                             "expected second parameter");
            if (!p2) return tl::unexpected(p2.error());
            ts.param2 = std::stoi(std::string(p2->lexeme));
        }

        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp) return tl::unexpected(rp.error());
    }

    return ok(std::move(ts));
}

// -- DDL: table constraint ----------------------------------------------------

Result<TableConstraint> Parser::parse_table_constraint() {
    TableConstraint tc;

    // Optional CONSTRAINT name.
    if (match(TokenType::CONSTRAINT)) {
        auto name = parse_name("constraint name");
        if (!name) return tl::unexpected(name.error());
        tc.name = std::move(*name);
    }

    if (match(TokenType::PRIMARY)) {
        auto key = expect(TokenType::KEY, "expected KEY after PRIMARY");
        if (!key) return tl::unexpected(key.error());
        tc.kind = TableConstraint::Kind::PRIMARY_KEY;

        auto lp = expect(TokenType::LPAREN, "expected '(' after PRIMARY KEY");
        if (!lp) return tl::unexpected(lp.error());

        do {
            auto col = parse_name("column name");
            if (!col) return tl::unexpected(col.error());
            tc.columns.push_back(std::move(*col));
        } while (match(TokenType::COMMA));

        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp) return tl::unexpected(rp.error());
    } else if (match(TokenType::UNIQUE)) {
        tc.kind = TableConstraint::Kind::UNIQUE;

        auto lp = expect(TokenType::LPAREN, "expected '(' after UNIQUE");
        if (!lp) return tl::unexpected(lp.error());

        do {
            auto col = parse_name("column name");
            if (!col) return tl::unexpected(col.error());
            tc.columns.push_back(std::move(*col));
        } while (match(TokenType::COMMA));

        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp) return tl::unexpected(rp.error());
    } else if (match(TokenType::CHECK)) {
        tc.kind = TableConstraint::Kind::CHECK;

        auto lp = expect(TokenType::LPAREN, "expected '(' after CHECK");
        if (!lp) return tl::unexpected(lp.error());
        auto expr = parse_expression();
        if (!expr) return tl::unexpected(expr.error());
        tc.check_expr = std::move(*expr);
        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp) return tl::unexpected(rp.error());
    } else if (match(TokenType::FOREIGN)) {
        auto key = expect(TokenType::KEY, "expected KEY after FOREIGN");
        if (!key) return tl::unexpected(key.error());
        tc.kind = TableConstraint::Kind::FOREIGN_KEY;

        auto lp = expect(TokenType::LPAREN, "expected '('");
        if (!lp) return tl::unexpected(lp.error());
        do {
            auto col = parse_name("column name");
            if (!col) return tl::unexpected(col.error());
            tc.columns.push_back(std::move(*col));
        } while (match(TokenType::COMMA));
        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp) return tl::unexpected(rp.error());

        auto ref = expect(TokenType::REFERENCES, "expected REFERENCES");
        if (!ref) return tl::unexpected(ref.error());
        auto tbl = parse_name("referenced table");
        if (!tbl) return tl::unexpected(tbl.error());
        tc.fk_table = std::move(*tbl);

        auto lp2 = expect(TokenType::LPAREN, "expected '('");
        if (!lp2) return tl::unexpected(lp2.error());
        do {
            auto col = parse_name("column name");
            if (!col) return tl::unexpected(col.error());
            tc.fk_columns.push_back(std::move(*col));
        } while (match(TokenType::COMMA));
        auto rp2 = expect(TokenType::RPAREN, "expected ')'");
        if (!rp2) return tl::unexpected(rp2.error());

        if (match(TokenType::ON)) {
            auto del = expect(TokenType::DELETE, "expected DELETE");
            if (!del) return tl::unexpected(del.error());
            if (match(TokenType::CASCADE)) {
                tc.on_delete = ReferentialAction::CASCADE;
            } else if (match(TokenType::RESTRICT)) {
                tc.on_delete = ReferentialAction::RESTRICT;
            } else {
                return tl::unexpected(
                    error("expected CASCADE or RESTRICT").error());
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
        if (!exists) return tl::unexpected(exists.error());
        stmt->if_exists = true;
    }

    auto name = parse_name("table name");
    if (!name) return tl::unexpected(name.error());
    stmt->name = std::move(*name);

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
    if (!name) return tl::unexpected(name.error());
    stmt->table_name = std::move(*name);

    // ADD COLUMN
    if (peek().type == TokenType::IDENTIFIER &&
        peek().lexeme == "ADD") {
        advance(); // consume ADD
        match(TokenType::COLUMN); // optional COLUMN keyword
        stmt->action = AlterAction::ADD_COLUMN;
        auto col = parse_column_def();
        if (!col) return tl::unexpected(col.error());
        stmt->column = std::move(*col);
        return ok(StmtPtr(std::move(stmt)));
    }

    if (match(TokenType::DROP)) {
        match(TokenType::COLUMN); // optional COLUMN keyword
        stmt->action = AlterAction::DROP_COLUMN;
        auto col_name = parse_name("column name");
        if (!col_name) return tl::unexpected(col_name.error());
        stmt->column_name = std::move(*col_name);
        return ok(StmtPtr(std::move(stmt)));
    }

    // RENAME COLUMN old TO new
    if (peek().type == TokenType::IDENTIFIER &&
        peek().lexeme == "RENAME") {
        advance(); // consume RENAME
        match(TokenType::COLUMN); // optional COLUMN keyword
        stmt->action = AlterAction::RENAME_COLUMN;
        auto old_name = parse_name("old column name");
        if (!old_name) return tl::unexpected(old_name.error());
        stmt->column_name = std::move(*old_name);

        // Expect TO.
        if (peek().type == TokenType::IDENTIFIER && peek().lexeme == "TO") {
            advance();
        } else {
            return tl::unexpected(
                error("expected TO after old column name").error());
        }

        auto new_name = parse_name("new column name");
        if (!new_name) return tl::unexpected(new_name.error());
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
        if (!not_tok) return tl::unexpected(not_tok.error());
        auto exists_tok = expect(TokenType::EXISTS, "expected EXISTS after NOT");
        if (!exists_tok) return tl::unexpected(exists_tok.error());
        stmt->if_not_exists = true;
    }

    // Index name.
    auto name = parse_name("index name");
    if (!name) return tl::unexpected(name.error());
    stmt->name = std::move(*name);

    // ON table(cols...)
    auto on = expect(TokenType::ON, "expected ON after index name");
    if (!on) return tl::unexpected(on.error());

    auto table = parse_name("table name");
    if (!table) return tl::unexpected(table.error());
    stmt->table_name = std::move(*table);

    auto lp = expect(TokenType::LPAREN, "expected '(' after table name");
    if (!lp) return tl::unexpected(lp.error());

    do {
        auto col = parse_name("column name");
        if (!col) return tl::unexpected(col.error());
        stmt->columns.push_back(std::move(*col));
    } while (match(TokenType::COMMA));

    auto rp = expect(TokenType::RPAREN, "expected ')'");
    if (!rp) return tl::unexpected(rp.error());

    // Optional USING method.
    if (peek().type == TokenType::IDENTIFIER && peek().lexeme == "USING") {
        advance(); // consume USING
        auto method = parse_name("index method");
        if (!method) return tl::unexpected(method.error());
        stmt->method = std::move(*method);
    }

    return ok(StmtPtr(std::move(stmt)));
}

// -- DDL: DROP INDEX ----------------------------------------------------------

Result<StmtPtr> Parser::parse_drop_index() {
    auto stmt = std::make_unique<DropIndexStmt>();

    if (match(TokenType::IF)) {
        auto exists = expect(TokenType::EXISTS, "expected EXISTS after IF");
        if (!exists) return tl::unexpected(exists.error());
        stmt->if_exists = true;
    }

    auto name = parse_name("index name");
    if (!name) return tl::unexpected(name.error());
    stmt->name = std::move(*name);

    return ok(StmtPtr(std::move(stmt)));
}

// -- DDL: CREATE EDGE TYPE ----------------------------------------------------

Result<StmtPtr> Parser::parse_create_edge_type() {
    auto stmt = std::make_unique<CreateEdgeTypeStmt>();

    auto name = parse_name("edge type name");
    if (!name) return tl::unexpected(name.error());
    stmt->name = std::move(*name);

    // Optional property list: (prop type, ...)
    if (match(TokenType::LPAREN)) {
        bool first_prop = true;
        while (!check(TokenType::RPAREN) && !at_end()) {
            if (!first_prop) {
                auto comma = expect(TokenType::COMMA, "expected ','");
                if (!comma) return tl::unexpected(comma.error());
            }
            first_prop = false;

            EdgeProperty prop;
            auto prop_name = parse_name("property name");
            if (!prop_name) return tl::unexpected(prop_name.error());
            prop.name = std::move(*prop_name);

            auto type = parse_type_spec();
            if (!type) return tl::unexpected(type.error());
            prop.type = std::move(*type);

            stmt->properties.push_back(std::move(prop));
        }
        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp) return tl::unexpected(rp.error());
    }

    // FROM table TO table.
    auto from = expect(TokenType::FROM, "expected FROM");
    if (!from) return tl::unexpected(from.error());
    auto from_tbl = parse_name("source table name");
    if (!from_tbl) return tl::unexpected(from_tbl.error());
    stmt->from_table = std::move(*from_tbl);

    // TO uses the identifier "TO", but we don't have a TO keyword.
    // Check for identifier "TO".
    if (peek().type == TokenType::IDENTIFIER && peek().lexeme == "TO") {
        advance();
    } else {
        return tl::unexpected(error("expected TO").error());
    }

    auto to_tbl = parse_name("target table name");
    if (!to_tbl) return tl::unexpected(to_tbl.error());
    stmt->to_table = std::move(*to_tbl);

    return ok(StmtPtr(std::move(stmt)));
}

// -- DDL: DROP EDGE TYPE ------------------------------------------------------

Result<StmtPtr> Parser::parse_drop_edge_type() {
    auto stmt = std::make_unique<DropEdgeTypeStmt>();

    if (match(TokenType::IF)) {
        auto exists = expect(TokenType::EXISTS, "expected EXISTS after IF");
        if (!exists) return tl::unexpected(exists.error());
        stmt->if_exists = true;
    }

    auto name = parse_name("edge type name");
    if (!name) return tl::unexpected(name.error());
    stmt->name = std::move(*name);

    return ok(StmtPtr(std::move(stmt)));
}

// -- Expression parsing -------------------------------------------------------

Result<ExprPtr> Parser::parse_expression() {
    return parse_or();
}

Result<ExprPtr> Parser::parse_or() {
    auto left = parse_and();
    if (!left) return left;

    while (match(TokenType::OR)) {
        uint32_t line = previous().line;
        uint32_t col = previous().column;
        auto right = parse_and();
        if (!right) return right;

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
    if (!left) return left;

    while (match(TokenType::AND)) {
        uint32_t line = previous().line;
        uint32_t col = previous().column;
        auto right = parse_not();
        if (!right) return right;

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
        if (!operand) return operand;

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
    if (!left) return left;

    // IS [NOT] NULL
    if (match(TokenType::IS)) {
        uint32_t line = previous().line;
        uint32_t col = previous().column;
        bool negated = match(TokenType::NOT);
        auto null_tok = expect(TokenType::NULL_KW, "expected NULL after IS");
        if (!null_tok) return tl::unexpected(null_tok.error());

        auto is_null = std::make_unique<IsNullExpr>();
        is_null->expr = std::move(*left);
        is_null->negated = negated;
        is_null->line = line;
        is_null->col = col;
        return ok(ExprPtr(std::move(is_null)));
    }

    // Comparison operators.
    BinaryOp op;
    bool has_op = false;
    if (match(TokenType::EQUAL)) { op = BinaryOp::EQUAL; has_op = true; }
    else if (match(TokenType::NOT_EQUAL)) { op = BinaryOp::NOT_EQUAL; has_op = true; }
    else if (match(TokenType::LESS)) { op = BinaryOp::LESS; has_op = true; }
    else if (match(TokenType::GREATER)) { op = BinaryOp::GREATER; has_op = true; }
    else if (match(TokenType::LESS_EQUAL)) { op = BinaryOp::LESS_EQUAL; has_op = true; }
    else if (match(TokenType::GREATER_EQUAL)) { op = BinaryOp::GREATER_EQUAL; has_op = true; }

    if (has_op) {
        uint32_t line = previous().line;
        uint32_t col = previous().column;
        auto right = parse_addition();
        if (!right) return right;

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
    if (!left) return left;

    while (check(TokenType::PLUS) || check(TokenType::MINUS) ||
           check(TokenType::PIPE_PIPE)) {
        Token op_tok = advance();
        BinaryOp op;
        switch (op_tok.type) {
        case TokenType::PLUS: op = BinaryOp::ADD; break;
        case TokenType::MINUS: op = BinaryOp::SUBTRACT; break;
        case TokenType::PIPE_PIPE: op = BinaryOp::CONCAT; break;
        default: op = BinaryOp::ADD; break; // unreachable
        }

        auto right = parse_multiplication();
        if (!right) return right;

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
    if (!left) return left;

    while (check(TokenType::STAR) || check(TokenType::SLASH) ||
           check(TokenType::PERCENT)) {
        Token op_tok = advance();
        BinaryOp op;
        switch (op_tok.type) {
        case TokenType::STAR: op = BinaryOp::MULTIPLY; break;
        case TokenType::SLASH: op = BinaryOp::DIVIDE; break;
        case TokenType::PERCENT: op = BinaryOp::MODULO; break;
        default: op = BinaryOp::MULTIPLY; break; // unreachable
        }

        auto right = parse_unary();
        if (!right) return right;

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
        if (!operand) return operand;

        auto unary = std::make_unique<UnaryExpr>();
        unary->op = UnaryOp::NEGATE;
        unary->operand = std::move(*operand);
        unary->line = line;
        unary->col = col;
        return ok(ExprPtr(std::move(unary)));
    }

    return parse_primary();
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

    // Parenthesized expression.
    if (match(TokenType::LPAREN)) {
        auto expr = parse_expression();
        if (!expr) return expr;
        auto rp = expect(TokenType::RPAREN, "expected ')'");
        if (!rp) return tl::unexpected(rp.error());
        return expr;
    }

    // Identifier, column reference, or function call.
    if (is_name_token(tok.type)) {
        std::string name(advance().lexeme);
        uint32_t line = previous().line;
        uint32_t col = previous().column;

        // Function call: name(...)
        if (match(TokenType::LPAREN)) {
            auto fn = std::make_unique<FunctionCallExpr>();
            fn->name = std::move(name);
            fn->line = line;
            fn->col = col;

            if (!check(TokenType::RPAREN)) {
                do {
                    auto arg = parse_expression();
                    if (!arg) return arg;
                    fn->args.push_back(std::move(*arg));
                } while (match(TokenType::COMMA));
            }

            auto rp = expect(TokenType::RPAREN, "expected ')'");
            if (!rp) return tl::unexpected(rp.error());
            return ok(ExprPtr(std::move(fn)));
        }

        // Qualified column: table.column
        if (match(TokenType::DOT)) {
            auto col_name = parse_name("column name");
            if (!col_name) return tl::unexpected(col_name.error());

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
                      "expected expression at line " +
                          std::to_string(tok.line) + ", column " +
                          std::to_string(tok.column) + " (got " +
                          std::string(token_type_name(tok.type)) + ")");
}

} // namespace giodb
