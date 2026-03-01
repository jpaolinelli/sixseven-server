#pragma once

#include "giodb/common/result.h"
#include "giodb/parser/ast.h"
#include "giodb/parser/token.h"

#include <string>
#include <vector>

namespace giodb {

/// Recursive descent SQL parser.
///
/// Takes a vector of tokens produced by the Lexer and produces an AST.
/// Supports error recovery by synchronizing to the next semicolon on
/// parse errors, allowing multiple errors to be reported.
///
/// Usage:
/// ```
///   Lexer lexer(sql);
///   auto tokens = lexer.tokenize().value();
///   Parser parser(std::move(tokens));
///   auto stmts = parser.parse_all();
/// ```
class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    /// Parse a single statement.
    [[nodiscard]] Result<StmtPtr> parse();

    /// Parse all semicolon-separated statements until EOF.
    [[nodiscard]] Result<std::vector<StmtPtr>> parse_all();

    /// Parse a single expression. Public so that callers (e.g. the planner)
    /// can parse default-value strings stored in the catalog.
    [[nodiscard]] Result<ExprPtr> parse_expression();

private:
    // -- Token navigation ---------------------------------------------------

    [[nodiscard]] const Token& peek() const;
    [[nodiscard]] const Token& previous() const;
    Token advance();
    [[nodiscard]] bool check(TokenType type) const;
    bool match(TokenType type);
    [[nodiscard]] Result<Token> expect(TokenType type, const std::string& message);
    [[nodiscard]] bool at_end() const;

    /// Accept IDENTIFIER or any keyword that can serve as a name.
    [[nodiscard]] Result<std::string> parse_name(const std::string& context);

    // -- Error recovery -----------------------------------------------------

    void synchronize();
    [[nodiscard]] Result<StmtPtr> error(const std::string& message);

    // -- Statement dispatch -------------------------------------------------

    [[nodiscard]] Result<StmtPtr> parse_statement();
    [[nodiscard]] Result<StmtPtr> parse_create();
    [[nodiscard]] Result<StmtPtr> parse_drop();
    [[nodiscard]] Result<StmtPtr> parse_alter();

    // -- DDL ----------------------------------------------------------------

    [[nodiscard]] Result<StmtPtr> parse_create_table();
    [[nodiscard]] Result<StmtPtr> parse_create_index(bool is_unique);
    [[nodiscard]] Result<StmtPtr> parse_create_edge_type();
    [[nodiscard]] Result<StmtPtr> parse_drop_table();
    [[nodiscard]] Result<StmtPtr> parse_drop_index();
    [[nodiscard]] Result<StmtPtr> parse_drop_edge_type();
    [[nodiscard]] Result<StmtPtr> parse_create_database();
    [[nodiscard]] Result<StmtPtr> parse_drop_database();
    [[nodiscard]] Result<StmtPtr> parse_alter_table();
    [[nodiscard]] Result<StmtPtr> parse_create_user();
    [[nodiscard]] Result<StmtPtr> parse_drop_user();
    [[nodiscard]] Result<StmtPtr> parse_alter_user();

    // -- DDL helpers --------------------------------------------------------

    [[nodiscard]] Result<AstColumnDef> parse_column_def();
    [[nodiscard]] Result<TypeSpec> parse_type_spec();
    [[nodiscard]] Result<TableConstraint> parse_table_constraint();

    // -- DML ----------------------------------------------------------------

    [[nodiscard]] Result<StmtPtr> parse_insert();
    [[nodiscard]] Result<StmtPtr> parse_update();
    [[nodiscard]] Result<StmtPtr> parse_delete();
    [[nodiscard]] Result<StmtPtr> parse_link();
    [[nodiscard]] Result<StmtPtr> parse_unlink();

    // -- DML helpers --------------------------------------------------------

    [[nodiscard]] Result<std::vector<SelectItem>> parse_returning();

    // -- Query --------------------------------------------------------------

    [[nodiscard]] Result<StmtPtr> parse_select();
    [[nodiscard]] Result<StmtPtr> parse_with();
    [[nodiscard]] Result<SelectItem> parse_select_item();
    [[nodiscard]] Result<TableRef> parse_table_ref();

    // -- Graph / Vector -----------------------------------------------------

    [[nodiscard]] Result<StmtPtr> parse_traverse();
    [[nodiscard]] Result<StmtPtr> parse_nearest();
    [[nodiscard]] Result<StmtPtr> parse_match();
    [[nodiscard]] Result<StmtPtr> parse_shortest_path();

    // -- TCL ----------------------------------------------------------------

    [[nodiscard]] Result<StmtPtr> parse_begin();
    [[nodiscard]] Result<StmtPtr> parse_commit();
    [[nodiscard]] Result<StmtPtr> parse_rollback();
    [[nodiscard]] Result<StmtPtr> parse_savepoint();

    // -- Admin --------------------------------------------------------------

    [[nodiscard]] Result<StmtPtr> parse_set_stmt();
    [[nodiscard]] Result<StmtPtr> parse_show();
    [[nodiscard]] Result<StmtPtr> parse_explain();
    [[nodiscard]] Result<StmtPtr> parse_describe();
    [[nodiscard]] Result<StmtPtr> parse_reembed();
    [[nodiscard]] Result<StmtPtr> parse_vacuum();
    [[nodiscard]] Result<StmtPtr> parse_analyze_stmt();

    // -- Expression parsing -------------------------------------------------

    [[nodiscard]] Result<ExprPtr> parse_or();
    [[nodiscard]] Result<ExprPtr> parse_and();
    [[nodiscard]] Result<ExprPtr> parse_not();
    [[nodiscard]] Result<ExprPtr> parse_comparison();
    [[nodiscard]] Result<ExprPtr> parse_addition();
    [[nodiscard]] Result<ExprPtr> parse_multiplication();
    [[nodiscard]] Result<ExprPtr> parse_unary();
    [[nodiscard]] Result<ExprPtr> parse_postfix();
    [[nodiscard]] Result<ExprPtr> parse_primary();

    std::vector<Token> tokens_;
    size_t current_ = 0;
};

} // namespace giodb
