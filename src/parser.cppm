module;

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
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
            if (check(TokenKind::KwStruct)) {
                program.structs.push_back(parse_struct_def());
            } else {
                parse_top_level_function_or_extern_group(program);
            }
        }
        return program;
    }

private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;
    // Names introduced by `struct X { ... };` seen so far. The parser is
    // single-pass, so (like C) a struct must be declared before it is used
    // as a type; this set is what lets `looks_like_type_start()` recognize
    // `Point p;` as a variable declaration rather than an expression
    // statement starting with the identifier `Point`.
    std::unordered_set<std::string> struct_names_;

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

    [[nodiscard]] bool looks_like_type_start() const {
        const Token& tok = peek();
        if (tok.kind == TokenKind::KwInt || tok.kind == TokenKind::KwBool || tok.kind == TokenKind::KwConst ||
            tok.kind == TokenKind::KwVoid) {
            return true;
        }
        if (check_std_qualified("unique_ptr") || check_std_qualified("span")) return true;
        return tok.kind == TokenKind::Identifier && struct_names_.contains(std::string(tok.text));
    }

    // Bounds-safe lookahead: returns the token `offset` positions ahead of
    // the current one, or the (always-last) EndOfFile token if that would
    // run past the end of the stream.
    [[nodiscard]] const Token& peek_at(size_t offset) const {
        size_t idx = pos_ + offset;
        return idx < tokens_.size() ? tokens_[idx] : tokens_.back();
    }

    // Checks (without consuming) for the 3-token sequence `std :: <member>`,
    // e.g. `std::unique_ptr` or `std::move`. These are recognized as fixed,
    // special-cased spellings rather than via a general `::`-qualified-name
    // grammar, matching the "minimal additions" design philosophy.
    [[nodiscard]] bool check_std_qualified(std::string_view member) const {
        return peek().kind == TokenKind::Identifier && peek().text == "std" &&
               peek_at(1).kind == TokenKind::ColonColon && peek_at(2).kind == TokenKind::Identifier &&
               peek_at(2).text == member;
    }

    void consume_std_qualified() {
        advance(); // std
        advance(); // ::
        advance(); // <member>
    }

    // Parses a base type name (`int`, `bool`, `std::unique_ptr<T>`, or a
    // known struct name) followed by zero or more `*` for pointer levels.
    // Array suffixes (`[N]`) are handled separately by parse_array_suffix,
    // since in C-style declarators the array size follows the *declared
    // name*, not the type.
    Type parse_unqualified_type() {
        if (check_std_qualified("unique_ptr")) {
            consume_std_qualified();
            expect(TokenKind::Less, "'<'");
            Type element = parse_type();
            expect(TokenKind::Greater, "'>'");
            Type type;
            type.kind = TypeKind::UniquePtr;
            type.pointee = std::make_shared<Type>(std::move(element));
            return type;
        }

        if (check_std_qualified("span")) {
            consume_std_qualified();
            expect(TokenKind::Less, "'<'");
            // `const` here qualifies the *element* type (`std::span<const
            // T>`, a read-only view), not a reference -- so it's parsed
            // directly rather than through parse_type() (which only
            // accepts a leading `const` when followed by `&`).
            bool element_is_const = match(TokenKind::KwConst);
            Type element = parse_unqualified_type();
            expect(TokenKind::Greater, "'>'");
            Type type;
            type.kind = TypeKind::Span;
            type.pointee = std::make_shared<Type>(std::move(element));
            type.is_mutable_ref = !element_is_const;
            return type;
        }

        const Token& tok = peek();
        Type type;
        type.kind = TypeKind::Named;
        if (tok.kind == TokenKind::KwInt) {
            type.name = "int";
            advance();
        } else if (tok.kind == TokenKind::KwBool) {
            type.name = "bool";
            advance();
        } else if (tok.kind == TokenKind::KwVoid) {
            // Valid here structurally (like int/bool) so `void*` falls
            // out of the trailing `*` loop below for free; a *bare*
            // (non-pointer) `void` is rejected downstream, not by the
            // parser -- see codegen's declare_function (parameters) and
            // VarDecl codegen (locals). A `void` return type is always
            // fine and needs no rejection anywhere.
            type.name = "void";
            advance();
        } else if (tok.kind == TokenKind::Identifier && struct_names_.contains(std::string(tok.text))) {
            type.name = std::string(tok.text);
            advance();
        } else {
            throw ParseError(tok.line, tok.column, "expected a type name");
        }

        while (match(TokenKind::Star)) {
            auto pointee = std::make_shared<Type>(type);
            type = Type{};
            type.kind = TypeKind::Pointer;
            type.pointee = std::move(pointee);
        }
        return type;
    }

    // Parses a full type, including the borrow-checking sugar from ch03:
    // an optional leading `const` plus a trailing `&` turns the
    // unqualified type into a Reference -- `T&` is a mutable/exclusive
    // borrow, `const T&` a shared borrow (ch05.2). `const` is only
    // meaningful directly before a reference in this version; scpp has no
    // general const-qualification yet, so a bare `const T` (no `&`) is
    // rejected rather than silently ignored.
    Type parse_type() {
        bool has_const_prefix = match(TokenKind::KwConst);
        Type type = parse_unqualified_type();

        if (match(TokenKind::Amp)) {
            auto pointee = std::make_shared<Type>(std::move(type));
            type = Type{};
            type.kind = TypeKind::Reference;
            type.pointee = std::move(pointee);
            type.is_mutable_ref = !has_const_prefix;
            return type;
        }

        if (has_const_prefix) {
            const Token& tok = peek();
            throw ParseError(tok.line, tok.column,
                              "'const' is only supported directly before a reference type ('const T&') in "
                              "this version");
        }
        return type;
    }

    // Wraps `base` in Array types for each trailing `[N]` found after a
    // declared name (e.g. the `[8]` in `int values[8];`). Arrays of
    // references aren't valid C++ (there's no storage layout for a raw
    // reference), so reject up front rather than let it silently codegen
    // as an array of addresses.
    Type parse_array_suffix(Type base) {
        while (check(TokenKind::LBracket)) {
            const Token& bracket_tok = peek();
            if (base.kind == TypeKind::Reference) {
                throw ParseError(bracket_tok.line, bracket_tok.column, "arrays of references are not supported");
            }
            advance();
            const Token& size_tok = expect(TokenKind::IntegerLiteral, "array size");
            expect(TokenKind::RBracket, "']'");
            auto element = std::make_shared<Type>(base);
            base = Type{};
            base.kind = TypeKind::Array;
            base.element = std::move(element);
            base.array_size = std::stoll(std::string(size_tok.text));
        }
        return base;
    }

    // Parses one top-level item that isn't a `struct`: an ordinary
    // function, or an `extern "C"` declaration/definition/block (ch02
    // §2.1). `safe` (if present) is always consumed up front, before
    // checking for `extern` -- matching the spec's own ordering example
    // (`safe extern "C" int add(...) { ... }`) -- so it's available
    // regardless of which of the three shapes below follows:
    //   [safe] <ret> <name>(<params>) { <body> }                 -- an
    //     ordinary definition (is_extern_c=false).
    //   [safe] extern "C" <ret> <name>(<params>) (';' | '{' ... '}')
    //     -- a single extern "C" item: a bodyless declaration (';') or a
    //     definition (a `safe` one is allowed; see parse_function's own
    //     "safe requires a body" check for the bodyless case).
    //   extern "C" { <item> <item> ... }                          -- block
    //     sugar for repeating `extern "C"` on each nested item, so an
    //     item written inside the block is *not* itself re-prefixed with
    //     `extern "C"` (matching real C++) -- but may independently start
    //     with its own `safe`. A leading `safe` directly before this
    //     block form isn't supported (there's no single item for it to
    //     attach to); mark individual items `safe` inside the block
    //     instead.
    void parse_top_level_function_or_extern_group(Program& program) {
        bool is_safe = match(TokenKind::KwSafe);
        if (match(TokenKind::KwExtern)) {
            parse_c_linkage_string();
            if (check(TokenKind::LBrace)) {
                if (is_safe) {
                    const Token& tok = peek();
                    throw ParseError(tok.line, tok.column,
                                      "'safe' cannot prefix an 'extern \"C\"' block; mark individual "
                                      "declarations 'safe' inside the block instead");
                }
                advance(); // '{'
                while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
                    if (check(TokenKind::KwStruct)) {
                        const Token& tok = peek();
                        throw ParseError(tok.line, tok.column,
                                          "an 'extern \"C\"' block currently only supports function "
                                          "declarations/definitions, not structs");
                    }
                    bool item_is_safe = match(TokenKind::KwSafe);
                    program.functions.push_back(parse_function(item_is_safe, /*is_extern_c=*/true));
                }
                expect(TokenKind::RBrace, "'}'");
                return;
            }
            program.functions.push_back(parse_function(is_safe, /*is_extern_c=*/true));
            return;
        }
        program.functions.push_back(parse_function(is_safe, /*is_extern_c=*/false));
    }

    // Consumes and validates the linkage string literal after `extern`.
    // v0.1 only accepts the literal "C" (not "C++" or anything else) --
    // see ch02 §2.1.
    void parse_c_linkage_string() {
        const Token& tok = expect(TokenKind::StringLiteral, "a linkage string (e.g. \"C\")");
        // `tok.text` includes the surrounding quotes (see StringLiteral's
        // definition in lexer.cppm).
        if (tok.text != "\"C\"") {
            throw ParseError(tok.line, tok.column,
                              "unsupported linkage " + std::string(tok.text) +
                                  ": only extern \"C\" is supported in this version");
        }
    }

    StructDef parse_struct_def() {
        expect(TokenKind::KwStruct, "'struct'");
        StructDef def;
        def.name = std::string(expect(TokenKind::Identifier, "struct name").text);
        // Register the name before parsing the body so a field can refer to
        // the enclosing struct via a pointer (e.g. `Node* next;`).
        struct_names_.insert(def.name);

        expect(TokenKind::LBrace, "'{'");
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
            StructField field;
            Type base = parse_type();
            field.name = std::string(expect(TokenKind::Identifier, "field name").text);
            field.type = parse_array_suffix(base);
            expect(TokenKind::Semicolon, "';'");
            def.fields.push_back(std::move(field));
        }
        expect(TokenKind::RBrace, "'}'");
        expect(TokenKind::Semicolon, "';'");
        return def;
    }

    // Parses one function declaration or definition's `<return-type>
    // <name>(<params>)` followed by either `;` (a bodyless declaration --
    // only legal when `is_extern_c`, ch02 §2.1) or `{ <body> }` (an
    // ordinary definition). Both `is_safe` and `is_extern_c` are decided
    // and consumed by the caller (parse_top_level_function_or_extern_group)
    // before this runs, since their combination affects *which* prefixes
    // were already consumed (a `safe` inside an `extern "C" { }` block
    // isn't preceded by its own `extern "C"` -- see that function).
    Function parse_function(bool is_safe, bool is_extern_c) {
        Function fn;
        fn.is_safe = is_safe;
        fn.is_extern_c = is_extern_c;
        fn.return_type = parse_type();
        fn.name = std::string(expect(TokenKind::Identifier, "function name").text);

        expect(TokenKind::LParen, "'('");
        if (!check(TokenKind::RParen)) {
            do {
                if (match(TokenKind::Ellipsis)) {
                    // `...` must be the last thing in the parameter list
                    // (as in real C++) -- anything after it is left
                    // unconsumed, so the expect(RParen) below reports a
                    // clear parse error for e.g. `(..., int x)`.
                    fn.has_varargs = true;
                    break;
                }
                Param param;
                Type base_type = parse_type();
                param.name = std::string(expect(TokenKind::Identifier, "parameter name").text);
                // The array suffix (if any) follows the *declared name*,
                // not the type -- same C-style declarator order as
                // parse_var_decl/parse_struct_def (e.g. `int arr[4]`).
                Type param_type = parse_array_suffix(base_type);
                if (param_type.kind == TypeKind::Array) {
                    // A fixed-size array parameter decays to a pointer to
                    // its element type, exactly as in ordinary C++ (ch02
                    // §2.1's signature-type rules explicitly require this
                    // for `extern "C"`, and there's no reason for it to
                    // behave differently for an ordinary function): the
                    // array's *size* isn't part of the decayed type, only
                    // its element type is -- `int arr[4]` and `int* arr`
                    // are the same parameter type.
                    Type decayed;
                    decayed.kind = TypeKind::Pointer;
                    decayed.pointee = param_type.element;
                    param_type = std::move(decayed);
                }
                param.type = std::move(param_type);
                fn.params.push_back(std::move(param));
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RParen, "')'");

        if (fn.has_varargs && !fn.is_extern_c) {
            const Token& tok = peek();
            throw ParseError(tok.line, tok.column,
                              "variadic parameters ('...') are only supported in an 'extern \"C\"' "
                              "declaration (ch02 §2.1)");
        }

        if (match(TokenKind::Semicolon)) {
            // Bodyless declaration: defined elsewhere, linked in
            // externally. The compiler has no visibility into its
            // implementation, so it can never be marked `safe` -- and,
            // since `has_varargs` combined with a body isn't allowed
            // (checked below), this is currently the *only* place a
            // variadic function can be introduced.
            if (!fn.is_extern_c) {
                const Token& tok = peek();
                throw ParseError(tok.line, tok.column,
                                  "a function declaration without a body is only supported for "
                                  "'extern \"C\"' (ch02 §2.1); every other function must have a "
                                  "definition");
            }
            if (fn.is_safe) {
                const Token& tok = peek();
                throw ParseError(tok.line, tok.column,
                                  "cannot mark an external declaration 'safe': its implementation "
                                  "isn't visible to the compiler (ch02 §2.1)");
            }
            return fn;
        }

        if (fn.has_varargs) {
            const Token& tok = peek();
            throw ParseError(tok.line, tok.column,
                              "variadic parameters ('...') are only supported for a bodyless "
                              "'extern \"C\"' declaration, not a definition (ch02 §2.1)");
        }
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
        if (check(TokenKind::KwUnsafe)) return parse_unsafe_block();
        if (looks_like_type_start()) return parse_var_decl();
        if (check(TokenKind::KwReturn)) return parse_return();
        if (check(TokenKind::KwIf)) return parse_if();
        if (check(TokenKind::KwWhile)) return parse_while();
        return parse_expr_stmt();
    }

    // `unsafe { stmt; stmt; ... }` (ch01 §1.3, design finalized) -- an
    // ordinary brace-delimited block (see parse_block) that additionally
    // marks itself `is_unsafe` so the move checker relaxes exactly the
    // ch05.5 checks it's licensed to for its statements. v0.1 only
    // supports this block-statement form: `unsafe` must always be
    // followed by `{` (parse_block itself rejects anything else), never a
    // single bare statement, a condition expression, or a match-arm body.
    StmtPtr parse_unsafe_block() {
        expect(TokenKind::KwUnsafe, "'unsafe'");
        StmtPtr block = parse_block();
        block->is_unsafe = true;
        return block;
    }

    StmtPtr parse_var_decl() {
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::VarDecl;
        Type base = parse_type();
        stmt->var_name = std::string(expect(TokenKind::Identifier, "variable name").text);
        stmt->type = parse_array_suffix(base);
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
        if (match(TokenKind::Star)) {
            // `*p` (dereference) -- unambiguous with binary `*`
            // (multiplication) since a prefix operator only ever
            // appears where a new operand is expected, never between
            // two already-parsed operands.
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Unary;
            node->unary_op = UnaryOp::Deref;
            node->lhs = parse_unary();
            return node;
        }
        return parse_postfix(parse_primary());
    }

    // Applies trailing `.field` (Member), `->field` (Member off a
    // dereference -- sugar for `(*p).field`, same as real C++), and
    // `[index]` (Subscript) operators, e.g. `p.x`, `arr[i]`, `p.inner.x`,
    // `arr[i].x`, `p->x`.
    ExprPtr parse_postfix(ExprPtr expr) {
        for (;;) {
            if (match(TokenKind::Dot)) {
                std::string field = std::string(expect(TokenKind::Identifier, "field name").text);
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Member;
                node->name = field;
                node->lhs = std::move(expr);
                expr = std::move(node);
            } else if (match(TokenKind::Arrow)) {
                std::string field = std::string(expect(TokenKind::Identifier, "field name").text);
                auto deref = std::make_unique<Expr>();
                deref->kind = ExprKind::Unary;
                deref->unary_op = UnaryOp::Deref;
                deref->lhs = std::move(expr);
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Member;
                node->name = field;
                node->lhs = std::move(deref);
                expr = std::move(node);
            } else if (match(TokenKind::LBracket)) {
                ExprPtr index = parse_expr();
                expect(TokenKind::RBracket, "']'");
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Subscript;
                node->lhs = std::move(expr);
                node->rhs = std::move(index);
                expr = std::move(node);
            } else {
                break;
            }
        }
        return expr;
    }

    ExprPtr parse_primary() {
        const Token& tok = peek();

        if (check_std_qualified("move")) {
            consume_std_qualified();
            expect(TokenKind::LParen, "'('");
            ExprPtr inner = parse_expr();
            expect(TokenKind::RParen, "')'");
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Move;
            node->lhs = std::move(inner);
            return node;
        }

        if (check_std_qualified("make_unique")) {
            consume_std_qualified();
            expect(TokenKind::Less, "'<'");
            Type element_type = parse_type();
            expect(TokenKind::Greater, "'>'");
            expect(TokenKind::LParen, "'('");
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::MakeUnique;
            node->type = std::move(element_type);
            if (!check(TokenKind::RParen)) {
                do {
                    node->args.push_back(parse_expr());
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::RParen, "')'");
            return node;
        }

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
