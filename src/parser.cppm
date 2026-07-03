module;

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

export module scpp.parser;

import scpp.lexer;
import scpp.ast;

export namespace scpp {

struct ParseError : std::runtime_error {
    ParseError(int line, int column, const std::string& message)
        : std::runtime_error("parse error at " + std::to_string(line) + ":" +
                              std::to_string(column) + ": " + message),
          line(line), column(column) {}
    int line;
    int column;
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    Program parse_program() {
        Program program;
        while (!check(TokenKind::EndOfFile)) {
            program.functions.push_back(parse_function());
        }
        return program;
    }

private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;

    [[nodiscard]] const Token& peek() const { return tokens_[pos_]; }
    [[nodiscard]] bool check(TokenKind kind) const { return peek().kind == kind; }

    const Token& advance() {
        const Token& tok = tokens_[pos_];
        if (pos_ + 1 < tokens_.size()) pos_++;
        return tok;
    }

    bool match(TokenKind kind) {
        if (!check(kind)) return false;
        advance();
        return true;
    }

    const Token& expect(TokenKind kind, const std::string& what) {
        if (!check(kind)) {
            const Token& tok = peek();
            throw ParseError(tok.line, tok.column, "expected " + what + " but found '" +
                                                        std::string(tok.text) + "'");
        }
        return advance();
    }

    [[nodiscard]] static bool is_type_token(TokenKind kind) {
        return kind == TokenKind::KwInt || kind == TokenKind::KwBool;
    }

    std::string parse_type_name() {
        const Token& tok = peek();
        if (!is_type_token(tok.kind)) {
            throw ParseError(tok.line, tok.column, "expected a type name");
        }
        advance();
        return std::string(tok.text);
    }

    Function parse_function() {
        Function fn;
        fn.is_safe = match(TokenKind::KwSafe);
        fn.return_type = parse_type_name();
        fn.name = std::string(expect(TokenKind::Identifier, "function name").text);

        expect(TokenKind::LParen, "'('");
        if (!check(TokenKind::RParen)) {
            do {
                Param param;
                param.type_name = parse_type_name();
                param.name = std::string(expect(TokenKind::Identifier, "parameter name").text);
                fn.params.push_back(std::move(param));
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RParen, "')'");

        fn.body = parse_block();
        return fn;
    }

    StmtPtr parse_block() {
        expect(TokenKind::LBrace, "'{'");
        auto block = std::make_unique<Stmt>();
        block->kind = StmtKind::Block;
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
            block->statements.push_back(parse_statement());
        }
        expect(TokenKind::RBrace, "'}'");
        return block;
    }

    StmtPtr parse_statement() {
        if (check(TokenKind::LBrace)) return parse_block();
        if (is_type_token(peek().kind)) return parse_var_decl();
        if (check(TokenKind::KwReturn)) return parse_return();
        if (check(TokenKind::KwIf)) return parse_if();
        if (check(TokenKind::KwWhile)) return parse_while();
        return parse_expr_stmt();
    }

    StmtPtr parse_var_decl() {
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::VarDecl;
        stmt->type_name = parse_type_name();
        stmt->var_name = std::string(expect(TokenKind::Identifier, "variable name").text);
        if (match(TokenKind::Assign)) {
            stmt->init = parse_expr();
        }
        expect(TokenKind::Semicolon, "';'");
        return stmt;
    }

    StmtPtr parse_return() {
        expect(TokenKind::KwReturn, "'return'");
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::Return;
        if (!check(TokenKind::Semicolon)) {
            stmt->expr = parse_expr();
        }
        expect(TokenKind::Semicolon, "';'");
        return stmt;
    }

    StmtPtr parse_if() {
        expect(TokenKind::KwIf, "'if'");
        expect(TokenKind::LParen, "'('");
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::If;
        stmt->condition = parse_expr();
        expect(TokenKind::RParen, "')'");
        stmt->then_branch = parse_statement();
        if (match(TokenKind::KwElse)) {
            stmt->else_branch = parse_statement();
        }
        return stmt;
    }

    StmtPtr parse_while() {
        expect(TokenKind::KwWhile, "'while'");
        expect(TokenKind::LParen, "'('");
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::While;
        stmt->condition = parse_expr();
        expect(TokenKind::RParen, "')'");
        stmt->then_branch = parse_statement();
        return stmt;
    }

    StmtPtr parse_expr_stmt() {
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::ExprStmt;
        stmt->expr = parse_expr();
        expect(TokenKind::Semicolon, "';'");
        return stmt;
    }

    // Precedence climbing, lowest to highest:
    // assignment -> logic_or -> logic_and -> equality -> relational
    // -> additive -> multiplicative -> unary -> primary

    ExprPtr parse_expr() { return parse_assignment(); }

    ExprPtr parse_assignment() {
        ExprPtr lhs = parse_logic_or();
        if (match(TokenKind::Assign)) {
            ExprPtr rhs = parse_assignment();
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Binary;
            node->binary_op = BinaryOp::Assign;
            node->lhs = std::move(lhs);
            node->rhs = std::move(rhs);
            return node;
        }
        return lhs;
    }

    ExprPtr parse_logic_or() {
        ExprPtr lhs = parse_logic_and();
        while (check(TokenKind::PipePipe)) {
            advance();
            lhs = make_binary(BinaryOp::Or, std::move(lhs), parse_logic_and());
        }
        return lhs;
    }

    ExprPtr parse_logic_and() {
        ExprPtr lhs = parse_equality();
        while (check(TokenKind::AmpAmp)) {
            advance();
            lhs = make_binary(BinaryOp::And, std::move(lhs), parse_equality());
        }
        return lhs;
    }

    ExprPtr parse_equality() {
        ExprPtr lhs = parse_relational();
        for (;;) {
            if (match(TokenKind::EqualEqual)) {
                lhs = make_binary(BinaryOp::Eq, std::move(lhs), parse_relational());
            } else if (match(TokenKind::NotEqual)) {
                lhs = make_binary(BinaryOp::Ne, std::move(lhs), parse_relational());
            } else {
                break;
            }
        }
        return lhs;
    }

    ExprPtr parse_relational() {
        ExprPtr lhs = parse_additive();
        for (;;) {
            if (match(TokenKind::Less)) {
                lhs = make_binary(BinaryOp::Lt, std::move(lhs), parse_additive());
            } else if (match(TokenKind::Greater)) {
                lhs = make_binary(BinaryOp::Gt, std::move(lhs), parse_additive());
            } else if (match(TokenKind::LessEqual)) {
                lhs = make_binary(BinaryOp::Le, std::move(lhs), parse_additive());
            } else if (match(TokenKind::GreaterEqual)) {
                lhs = make_binary(BinaryOp::Ge, std::move(lhs), parse_additive());
            } else {
                break;
            }
        }
        return lhs;
    }

    ExprPtr parse_additive() {
        ExprPtr lhs = parse_multiplicative();
        for (;;) {
            if (match(TokenKind::Plus)) {
                lhs = make_binary(BinaryOp::Add, std::move(lhs), parse_multiplicative());
            } else if (match(TokenKind::Minus)) {
                lhs = make_binary(BinaryOp::Sub, std::move(lhs), parse_multiplicative());
            } else {
                break;
            }
        }
        return lhs;
    }

    ExprPtr parse_multiplicative() {
        ExprPtr lhs = parse_unary();
        for (;;) {
            if (match(TokenKind::Star)) {
                lhs = make_binary(BinaryOp::Mul, std::move(lhs), parse_unary());
            } else if (match(TokenKind::Slash)) {
                lhs = make_binary(BinaryOp::Div, std::move(lhs), parse_unary());
            } else {
                break;
            }
        }
        return lhs;
    }

    ExprPtr parse_unary() {
        if (match(TokenKind::Minus)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Unary;
            node->unary_op = UnaryOp::Neg;
            node->lhs = parse_unary();
            return node;
        }
        if (match(TokenKind::Bang)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Unary;
            node->unary_op = UnaryOp::Not;
            node->lhs = parse_unary();
            return node;
        }
        return parse_primary();
    }

    ExprPtr parse_primary() {
        const Token& tok = peek();

        if (match(TokenKind::IntegerLiteral)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::IntegerLiteral;
            node->int_value = std::stoll(std::string(tok.text));
            return node;
        }
        if (match(TokenKind::KwTrue)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::BoolLiteral;
            node->bool_value = true;
            return node;
        }
        if (match(TokenKind::KwFalse)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::BoolLiteral;
            node->bool_value = false;
            return node;
        }
        if (check(TokenKind::Identifier)) {
            advance();
            std::string name(tok.text);
            if (match(TokenKind::LParen)) {
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Call;
                node->name = name;
                if (!check(TokenKind::RParen)) {
                    do {
                        node->args.push_back(parse_expr());
                    } while (match(TokenKind::Comma));
                }
                expect(TokenKind::RParen, "')'");
                return node;
            }
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Identifier;
            node->name = name;
            return node;
        }
        if (match(TokenKind::LParen)) {
            ExprPtr inner = parse_expr();
            expect(TokenKind::RParen, "')'");
            return inner;
        }

        throw ParseError(tok.line, tok.column, "expected an expression but found '" +
                                                    std::string(tok.text) + "'");
    }

    static ExprPtr make_binary(BinaryOp op, ExprPtr lhs, ExprPtr rhs) {
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::Binary;
        node->binary_op = op;
        node->lhs = std::move(lhs);
        node->rhs = std::move(rhs);
        return node;
    }
};

Program parse(std::vector<Token> tokens) {
    Parser parser(std::move(tokens));
    return parser.parse_program();
}

Program parse(std::string_view source) {
    return parse(tokenize(source));
}

} // namespace scpp
