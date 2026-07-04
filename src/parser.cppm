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
        : std::runtime_error(message), line(line), column(column), loc{line, column} {}
    int line;
    int column;
    // Same position as line/column above, just packaged as a
    // SourceLocation (ast.cppm) so cli.cppm's diagnostic printer can
    // treat every error kind (Parse/Dataflow/Codegen) uniformly.
    SourceLocation loc;
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    Program parse_program() {
        Program program;
        while (!check(TokenKind::EndOfFile)) {
            if (check(TokenKind::KwStruct)) {
                program.structs.push_back(parse_struct_def());
            } else if (check(TokenKind::KwClass)) {
                parse_class_def(program);
            } else {
                parse_top_level_function_or_extern_group(program);
            }
        }
        return program;
    }

private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;
    // Names introduced by `struct X { ... };` or `class X { ... };` seen
    // so far. The parser is single-pass, so (like C) either must be
    // declared before it is used as a type; this set is what lets
    // `looks_like_type_start()` recognize `Point p;` as a variable
    // declaration rather than an expression statement starting with the
    // identifier `Point`. Both kinds share one set since, once parsed,
    // they're structurally identical fixed-layout aggregates as far as
    // "is this identifier a type name" is concerned -- `class_names_`
    // below separately tracks *which* of those are specifically classes,
    // for the handful of decisions that do need to tell them apart
    // (constructor-call VarDecl syntax, access control).
    std::unordered_set<std::string> struct_names_;
    // Class names specifically (ch04 §4.2) -- see struct_names_ above for
    // why this is a second, narrower set rather than the only one.
    std::unordered_set<std::string> class_names_;

    [[nodiscard]] const Token& peek() const { return tokens_[pos_]; }
    [[nodiscard]] bool check(TokenKind kind) const { return peek().kind == kind; }

    // The position of the *next* token to be consumed -- called at the
    // start of parsing a new Expr/Stmt/Function, before any of its own
    // tokens are consumed, so the resulting node's `.loc` points at
    // wherever it syntactically begins (see SourceLocation, ast.cppm).
    [[nodiscard]] SourceLocation current_loc() const { return SourceLocation{peek().line, peek().column}; }

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
            tok.kind == TokenKind::KwVoid || tok.kind == TokenKind::KwChar) {
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
    // name*, not the type. `const_qualifies_first_pointer` is set by
    // parse_type() when it saw a leading `const` immediately before this
    // call: it makes only the *innermost* (first-parsed) `*` level's
    // pointee const (`const T*`, or `const T**`'s inner pointer -- matching
    // real C++'s own reading of `const` as binding to the base type, not
    // an outer pointer level), never a later/outer one, mirroring how
    // real C++ reads `const int**` as "pointer to (pointer to const int)".
    Type parse_unqualified_type(bool const_qualifies_first_pointer = false) {
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
        } else if (tok.kind == TokenKind::KwChar) {
            type.name = "char";
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

        bool first_star = true;
        while (match(TokenKind::Star)) {
            auto pointee = std::make_shared<Type>(type);
            type = Type{};
            type.kind = TypeKind::Pointer;
            type.pointee = std::move(pointee);
            type.is_mutable_pointee = !(first_star && const_qualifies_first_pointer);
            first_star = false;
        }
        return type;
    }

    // Parses a full type, including the borrow-checking sugar from ch03:
    // an optional leading `const` plus a trailing `&` turns the
    // unqualified type into a Reference -- `T&` is a mutable/exclusive
    // borrow, `const T&` a shared borrow (ch05.2). `const` immediately
    // before a *pointer* type (`const T*`, e.g. `const char* fmt` in a
    // realistic `extern "C"` signature -- ch02 §2.1) is also accepted and,
    // like a reference's `is_mutable_ref`, properly tracked: `const T*`
    // and `T*` are genuinely distinct types (ch05 §5.7, ch08 Q9), not
    // unified the way an earlier draft of that section assumed. `const`
    // is rejected everywhere else (a bare `const T`, no `&`/`*`) since
    // scpp has no other const-qualification yet.
    Type parse_type() {
        bool has_const_prefix = match(TokenKind::KwConst);
        Type type = parse_unqualified_type(/*const_qualifies_first_pointer=*/has_const_prefix);

        if (match(TokenKind::Amp)) {
            auto pointee = std::make_shared<Type>(std::move(type));
            type = Type{};
            type.kind = TypeKind::Reference;
            type.pointee = std::move(pointee);
            type.is_mutable_ref = !has_const_prefix;
            return type;
        }

        if (has_const_prefix && type.kind != TypeKind::Pointer) {
            const Token& tok = peek();
            throw ParseError(tok.line, tok.column,
                              "'const' is only supported directly before a reference type ('const T&') "
                              "or a pointer type ('const T*') in this version");
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
        SourceLocation loc = current_loc();
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
                    SourceLocation item_loc = current_loc();
                    bool item_is_safe = match(TokenKind::KwSafe);
                    Function item_fn = parse_function(item_is_safe, /*is_extern_c=*/true);
                    item_fn.loc = item_loc;
                    program.functions.push_back(std::move(item_fn));
                }
                expect(TokenKind::RBrace, "'}'");
                return;
            }
            Function fn = parse_function(is_safe, /*is_extern_c=*/true);
            fn.loc = loc;
            program.functions.push_back(std::move(fn));
            return;
        }
        Function fn = parse_function(is_safe, /*is_extern_c=*/false);
        fn.loc = loc;
        program.functions.push_back(std::move(fn));
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

    // Decodes a StringLiteral token's text (e.g. "a\nb") into its byte
    // content. `tok.text` includes the surrounding double quotes (see
    // StringLiteral's definition in lexer.cppm). Supports the same
    // minimal named-escape set as decode_char_literal above: \n \t \r \\
    // \' \" \0 -- no hex/octal escapes. Unlike a char literal, any number
    // of characters (including zero -- an empty string "") is valid.
    std::string decode_string_literal(const Token& tok) {
        if (tok.text.size() < 2) {
            throw ParseError(tok.line, tok.column, "unterminated string literal " + std::string(tok.text));
        }
        std::string_view inner = tok.text.substr(1, tok.text.size() - 2);
        std::string result;
        result.reserve(inner.size());
        for (size_t i = 0; i < inner.size(); i++) {
            if (inner[i] != '\\') {
                result.push_back(inner[i]);
                continue;
            }
            if (i + 1 >= inner.size()) {
                throw ParseError(tok.line, tok.column,
                                  "invalid string literal " + std::string(tok.text) +
                                      ": trailing '\\' with no following escape character");
            }
            i++;
            switch (inner[i]) {
                case 'n': result.push_back('\n'); break;
                case 't': result.push_back('\t'); break;
                case 'r': result.push_back('\r'); break;
                case '0': result.push_back('\0'); break;
                case '\\': result.push_back('\\'); break;
                case '\'': result.push_back('\''); break;
                case '"': result.push_back('"'); break;
                default:
                    throw ParseError(tok.line, tok.column,
                                      "invalid string literal " + std::string(tok.text) +
                                          ": unsupported escape sequence '\\" + std::string(1, inner[i]) +
                                          "' (supported: \\n \\t \\r \\\\ \\' \\\" \\0)");
            }
        }
        return result;
    }

    // Decodes a CharLiteral token's text (e.g. 'a', '\n', '\\', '\'', '\0')
    // into its ordinal value. `tok.text` includes the surrounding single
    // quotes (see CharLiteral's definition in lexer.cppm). Supports the
    // same minimal named-escape set as decode_string_literal above: \n \t
    // \r \\ \' \" \0 -- no hex/octal escapes.
    long long decode_char_literal(const Token& tok) {
        // A well-formed literal is always at least `''` (2 quote chars);
        // anything shorter means the lexer hit EOF before a closing
        // quote (an unterminated literal) -- guard before the substr
        // below so that case reports a clear error instead of
        // underflowing `tok.text.size() - 2`.
        if (tok.text.size() < 2) {
            throw ParseError(tok.line, tok.column,
                              "unterminated char literal " + std::string(tok.text));
        }
        std::string_view inner = tok.text.substr(1, tok.text.size() - 2);
        if (inner.size() == 1 && inner[0] != '\\') {
            return static_cast<unsigned char>(inner[0]);
        }
        if (inner.size() == 2 && inner[0] == '\\') {
            switch (inner[1]) {
                case 'n': return '\n';
                case 't': return '\t';
                case 'r': return '\r';
                case '0': return '\0';
                case '\\': return '\\';
                case '\'': return '\'';
                case '"': return '"';
                default: break;
            }
        }
        throw ParseError(tok.line, tok.column,
                          "invalid char literal " + std::string(tok.text) +
                              ": must be exactly one character or one of the supported escape "
                              "sequences (\\n \\t \\r \\\\ \\' \\\" \\0)");
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

    // Parses a parenthesized, comma-separated parameter list `(<type>
    // <name>, ...)`, including the enclosing parens -- shared by every
    // class member function (method/constructor; a destructor is always
    // zero-arg, parsed directly). Deliberately separate from
    // parse_function's own inline version (which also handles extern
    // "C"'s trailing `...`, never relevant to a method/constructor): the
    // two are simple and small enough that duplicating this one loop
    // body is lower-risk than threading varargs-specific logic through a
    // shared helper.
    std::vector<Param> parse_param_list() {
        std::vector<Param> params;
        expect(TokenKind::LParen, "'('");
        if (!check(TokenKind::RParen)) {
            do {
                Param param;
                Type base_type = parse_type();
                param.name = std::string(expect(TokenKind::Identifier, "parameter name").text);
                // Same C-style declarator order (array suffix after the
                // name, decaying to pointer) as parse_function's own
                // parameter loop.
                Type param_type = parse_array_suffix(base_type);
                if (param_type.kind == TypeKind::Array) {
                    Type decayed;
                    decayed.kind = TypeKind::Pointer;
                    decayed.pointee = param_type.element;
                    param_type = std::move(decayed);
                }
                param.type = std::move(param_type);
                params.push_back(std::move(param));
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RParen, "')'");
        return params;
    }

    // Builds the implicit `this` parameter every class member function
    // gets as params[0] (ch05 §5.9): a Reference to `class_name` --
    // `const T&` for a `const` method (or always for a destructor, which
    // needs to mutate/tear down the receiver, so never `const`... wait,
    // no: a destructor is never `const` either way, callers pass
    // `is_const=false` for it directly), `T&` otherwise. This -- an
    // ordinary Reference-typed parameter -- is the *entire* mechanism
    // scpp needs for `this`: every existing reference/borrow-checking
    // rule (elision, dangling checks, alias-XOR-mutability) already
    // applies with no new logic once a method is shaped this way (see
    // ClassDef's own comment).
    Param make_this_param(const std::string& class_name, bool is_const) {
        Param this_param;
        this_param.name = "this";
        Type this_type;
        this_type.kind = TypeKind::Reference;
        this_type.pointee = std::make_shared<Type>();
        this_type.pointee->kind = TypeKind::Named;
        this_type.pointee->name = class_name;
        this_type.is_mutable_ref = !is_const;
        this_param.type = std::move(this_type);
        return this_param;
    }

    // Parses `class Name { ... };` (ch04 §4.2/ch05 §5.9): fields (with
    // access-specifier sections, defaulting to `private` like real C++,
    // unlike `struct`'s always-public fields) plus constructor/
    // destructor/method definitions, each of which is synthesized
    // directly into `program.functions` as an ordinary top-level
    // Function -- see ClassDef's own comment for the full reasoning and
    // the `ClassName_memberName` naming scheme used.
    void parse_class_def(Program& program) {
        expect(TokenKind::KwClass, "'class'");
        std::string class_name = std::string(expect(TokenKind::Identifier, "class name").text);
        // Register the name before parsing the body so a field/method can
        // refer to the enclosing class via a pointer, and so a
        // self-referential constructor call/access-control decision below
        // already recognizes it -- same before-parsing-the-body
        // registration order as parse_struct_def.
        struct_names_.insert(class_name);
        class_names_.insert(class_name);

        ClassDef def;
        def.name = class_name;

        expect(TokenKind::LBrace, "'{'");
        // Real C++'s own default: a class body starts `private` until the
        // first access-specifier section (unlike `struct`, which has no
        // access control at all -- ch04 §4.2).
        AccessSpecifier current_access = AccessSpecifier::Private;
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
            if (match(TokenKind::KwPublic)) {
                expect(TokenKind::Colon, "':'");
                current_access = AccessSpecifier::Public;
                continue;
            }
            if (match(TokenKind::KwPrivate)) {
                expect(TokenKind::Colon, "':'");
                current_access = AccessSpecifier::Private;
                continue;
            }

            SourceLocation member_loc = current_loc();
            bool member_is_safe = match(TokenKind::KwSafe);
            if (match(TokenKind::Tilde)) {
                // Destructor: `~ClassName() { ... }` -- no parameters, no
                // return type, and always a mutable (non-`const`) `this`
                // (it always needs to tear the receiver down).
                const Token& name_tok = expect(TokenKind::Identifier, "destructor name");
                if (std::string(name_tok.text) != class_name) {
                    throw ParseError(name_tok.line, name_tok.column,
                                      "destructor name '~" + std::string(name_tok.text) +
                                          "' must match the enclosing class name '" + class_name + "'");
                }
                expect(TokenKind::LParen, "'('");
                expect(TokenKind::RParen, "')'");
                Function fn;
                fn.loc = member_loc;
                fn.is_safe = member_is_safe;
                fn.return_type.kind = TypeKind::Named;
                fn.return_type.name = "void";
                fn.name = class_name + "_delete";
                fn.params.push_back(make_this_param(class_name, /*is_const=*/false));
                fn.body = parse_block();
                program.functions.push_back(std::move(fn));
                continue;
            }

            if (check(TokenKind::Identifier) && std::string(peek().text) == class_name &&
                peek_at(1).kind == TokenKind::LParen) {
                // Constructor: `ClassName(args) { ... }` -- distinguished
                // from an ordinary method/field by its name matching the
                // class exactly with *no* declared return type at all
                // (real C++'s own rule: the constructor is the one member
                // that never has a return type, not even `void`) --
                // `this` is always mutable here too (it's what the
                // constructor initializes).
                advance(); // class name
                Function fn;
                fn.loc = member_loc;
                fn.is_safe = member_is_safe;
                fn.return_type.kind = TypeKind::Named;
                fn.return_type.name = "void";
                fn.name = class_name + "_new";
                fn.params = parse_param_list();
                fn.params.insert(fn.params.begin(), make_this_param(class_name, /*is_const=*/false));
                fn.body = parse_block();
                program.functions.push_back(std::move(fn));
                continue;
            }

            // Otherwise: an ordinary field or method, both starting with
            // a declared type -- same "parse a type, then a name, then
            // see what follows" disambiguation parse_var_decl/
            // parse_struct_def already use.
            Type member_type = parse_type();
            std::string member_name = std::string(expect(TokenKind::Identifier, "field or method name").text);
            if (check(TokenKind::LParen)) {
                Function fn;
                fn.loc = member_loc;
                fn.params = parse_param_list();
                // `const` trails the parameter list, exactly like real
                // C++ (`int length() const { ... }`), so it's only
                // knowable -- and `this`'s mutability with it -- after
                // parsing the params above.
                bool is_const = match(TokenKind::KwConst);
                fn.is_safe = member_is_safe;
                fn.return_type = std::move(member_type);
                fn.name = class_name + "_" + member_name;
                fn.params.insert(fn.params.begin(), make_this_param(class_name, is_const));
                fn.body = parse_block();
                program.functions.push_back(std::move(fn));
                continue;
            }

            // A field: `safe` makes no sense on a variable, and ch04
            // §4.2 permanently forbids a public one.
            if (member_is_safe) {
                const Token& tok = peek();
                throw ParseError(tok.line, tok.column,
                                  "'safe' cannot prefix a member variable, only a member function");
            }
            if (current_access == AccessSpecifier::Public) {
                const Token& tok = peek();
                throw ParseError(tok.line, tok.column,
                                  "member variable '" + member_name + "' cannot be 'public' (ch04 §4.2) -- "
                                  "only member functions can be; expose read access through a method instead");
            }
            ClassField field;
            field.type = parse_array_suffix(member_type);
            field.name = member_name;
            field.access = current_access;
            expect(TokenKind::Semicolon, "';'");
            def.fields.push_back(std::move(field));
        }
        expect(TokenKind::RBrace, "'}'");
        expect(TokenKind::Semicolon, "';'");
        program.classes.push_back(std::move(def));
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
        SourceLocation loc = current_loc();
        expect(TokenKind::LBrace, "'{'");
        auto block = std::make_unique<Stmt>();
        block->kind = StmtKind::Block;
        block->loc = loc;
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
    // Grammar accepts this anywhere a statement is expected, regardless
    // of the enclosing function's own `safe`-ness -- rejecting it inside
    // a native (non-`safe`) function is movecheck's job (check_function),
    // not a grammar restriction, since it needs `Function::is_safe`
    // context this parser method doesn't have.
    StmtPtr parse_unsafe_block() {
        expect(TokenKind::KwUnsafe, "'unsafe'");
        StmtPtr block = parse_block();
        block->is_unsafe = true;
        return block;
    }

    StmtPtr parse_var_decl() {
        SourceLocation loc = current_loc();
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::VarDecl;
        stmt->loc = loc;
        Type base = parse_type();
        stmt->var_name = std::string(expect(TokenKind::Identifier, "variable name").text);
        stmt->type = parse_array_suffix(base);
        if (match(TokenKind::Assign)) {
            stmt->init = parse_expr();
        } else if (match(TokenKind::LParen)) {
            // `ClassName name(args);` (ch04 §4.2): direct-initialization
            // via an explicit constructor call -- the concrete way a
            // `class`-typed local is constructed in this version (there
            // is no `=`-initializer form for a class type yet, only this
            // or a bare, zero-initialized declaration calling no
            // constructor at all, e.g. `ClassName name;`). Movecheck/
            // codegen resolve the callee by recomputing `ClassName_new`
            // from `stmt->type`, not from anything recorded here.
            stmt->has_ctor_args = true;
            if (!check(TokenKind::RParen)) {
                do {
                    stmt->ctor_args.push_back(parse_expr());
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::RParen, "')'");
        }
        expect(TokenKind::Semicolon, "';'");
        return stmt;
    }

    StmtPtr parse_return() {
        SourceLocation loc = current_loc();
        expect(TokenKind::KwReturn, "'return'");
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::Return;
        stmt->loc = loc;
        if (!check(TokenKind::Semicolon)) {
            stmt->expr = parse_expr();
        }
        expect(TokenKind::Semicolon, "';'");
        return stmt;
    }

    StmtPtr parse_if() {
        SourceLocation loc = current_loc();
        expect(TokenKind::KwIf, "'if'");
        expect(TokenKind::LParen, "'('");
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::If;
        stmt->loc = loc;
        stmt->condition = parse_expr();
        expect(TokenKind::RParen, "')'");
        stmt->then_branch = parse_statement();
        if (match(TokenKind::KwElse)) {
            stmt->else_branch = parse_statement();
        }
        return stmt;
    }

    StmtPtr parse_while() {
        SourceLocation loc = current_loc();
        expect(TokenKind::KwWhile, "'while'");
        expect(TokenKind::LParen, "'('");
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::While;
        stmt->loc = loc;
        stmt->condition = parse_expr();
        expect(TokenKind::RParen, "')'");
        stmt->then_branch = parse_statement();
        return stmt;
    }

    StmtPtr parse_expr_stmt() {
        SourceLocation loc = current_loc();
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::ExprStmt;
        stmt->loc = loc;
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
            node->loc = lhs->loc;
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
        SourceLocation loc = current_loc();
        if (match(TokenKind::Minus)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Unary;
            node->loc = loc;
            node->unary_op = UnaryOp::Neg;
            node->lhs = parse_unary();
            return node;
        }
        if (match(TokenKind::Bang)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Unary;
            node->loc = loc;
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
            node->loc = loc;
            node->unary_op = UnaryOp::Deref;
            node->lhs = parse_unary();
            return node;
        }
        if (match(TokenKind::Amp)) {
            // `&expr` (address-of, ch05 §5.7) -- unlike `*`, `Amp` never
            // doubles as a binary operator in scpp (there is no bitwise
            // `&`; `T&`/`const T&` reference syntax is only recognized by
            // parse_type, never reached from an expression context), so
            // this is unconditionally a prefix operator here, no
            // position-based disambiguation needed. `expr.lhs`'s shape
            // (must resolve to a place) is a semantic check, not a
            // grammar one -- deferred to movecheck's
            // resolve_borrow_source_root, same division of labor as
            // Deref's operand above.
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Unary;
            node->loc = loc;
            node->unary_op = UnaryOp::AddressOf;
            node->lhs = parse_unary();
            return node;
        }
        return parse_postfix(parse_primary());
    }

    // Applies trailing `.name` (Member, or a method call -- ch05 §5.9 --
    // if `(` follows), `->name` (same, off a dereference -- sugar for
    // `(*p).name`, same as real C++ -- unless `p` is literally `this`,
    // see below), and `[index]` (Subscript) operators, e.g. `p.x`,
    // `arr[i]`, `p.inner.x`, `arr[i].x`, `p->x`, `obj.method(args)`.
    ExprPtr parse_postfix(ExprPtr expr) {
        for (;;) {
            if (match(TokenKind::Dot)) {
                std::string name = std::string(expect(TokenKind::Identifier, "field or method name").text);
                expr = parse_member_or_method_call(std::move(expr), name);
            } else if (match(TokenKind::Arrow)) {
                std::string name = std::string(expect(TokenKind::Identifier, "field or method name").text);
                // `this->x` (ch05 §5.9): `this` is represented as an
                // ordinary Reference-typed pseudo-parameter (see parser's
                // make_this_param), which already auto-dereferences on
                // every use (codegen_lvalue's Identifier case) exactly
                // like `a.x` already does for any other reference-typed
                // local `a` -- so unlike `p->x` for a real pointer/
                // unique_ptr `p` below, there is no separate pointee to
                // Deref through first.
                if (expr->kind == ExprKind::Identifier && expr->name == "this") {
                    expr = parse_member_or_method_call(std::move(expr), name);
                    continue;
                }
                auto deref = std::make_unique<Expr>();
                deref->kind = ExprKind::Unary;
                deref->unary_op = UnaryOp::Deref;
                deref->loc = expr->loc;
                deref->lhs = std::move(expr);
                expr = parse_member_or_method_call(std::move(deref), name);
            } else if (match(TokenKind::LBracket)) {
                ExprPtr index = parse_expr();
                expect(TokenKind::RBracket, "']'");
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Subscript;
                node->loc = expr->loc;
                node->lhs = std::move(expr);
                node->rhs = std::move(index);
                expr = std::move(node);
            } else {
                break;
            }
        }
        return expr;
    }

    // Shared by parse_postfix's `.name`/(this-adjusted) `->name` cases:
    // `name(args)` is a method call (ch05 §5.9) -- `base` (the receiver)
    // is stored in the resulting Call's `lhs` (nullptr for an ordinary
    // free-function call, see ast.cppm's Expr), resolved to a concrete
    // synthesized function symbol only once `base`'s static type is known
    // (movecheck/codegen, not the parser). Otherwise it's a plain field
    // access, unchanged from before method calls existed.
    ExprPtr parse_member_or_method_call(ExprPtr base, const std::string& name) {
        SourceLocation loc = base->loc;
        if (match(TokenKind::LParen)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Call;
            node->loc = loc;
            node->name = name;
            node->lhs = std::move(base);
            if (!check(TokenKind::RParen)) {
                do {
                    node->args.push_back(parse_expr());
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::RParen, "')'");
            return node;
        }
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::Member;
        node->loc = loc;
        node->name = name;
        node->lhs = std::move(base);
        return node;
    }

    ExprPtr parse_primary() {
        const Token& tok = peek();
        SourceLocation loc{tok.line, tok.column};

        if (check_std_qualified("move")) {
            consume_std_qualified();
            expect(TokenKind::LParen, "'('");
            ExprPtr inner = parse_expr();
            expect(TokenKind::RParen, "')'");
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Move;
            node->loc = loc;
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
            node->loc = loc;
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
            node->loc = loc;
            node->int_value = std::stoll(std::string(tok.text));
            return node;
        }
        if (match(TokenKind::CharLiteral)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::CharLiteral;
            node->loc = loc;
            node->int_value = decode_char_literal(tok);
            return node;
        }
        if (match(TokenKind::StringLiteral)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::StringLiteral;
            node->loc = loc;
            node->name = decode_string_literal(tok);
            return node;
        }
        if (match(TokenKind::KwThis)) {
            // ch05 §5.9: `this` is a keyword (not an ordinary identifier
            // -- so a user can never accidentally shadow it with a
            // same-named parameter/local), but behaves exactly like an
            // Identifier expression bound to the name "this" everywhere
            // downstream: it resolves through the exact same
            // `body.local_types`/`locals_` lookup as any other reference-
            // typed local, since parse_class_def's make_this_param
            // already registered it as an ordinary params[0] named
            // "this".
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Identifier;
            node->loc = loc;
            node->name = "this";
            return node;
        }
        if (match(TokenKind::KwTrue)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::BoolLiteral;
            node->loc = loc;
            node->bool_value = true;
            return node;
        }
        if (match(TokenKind::KwFalse)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::BoolLiteral;
            node->loc = loc;
            node->bool_value = false;
            return node;
        }
        if (check(TokenKind::Identifier)) {
            advance();
            std::string name(tok.text);
            if (match(TokenKind::LParen)) {
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Call;
                node->loc = loc;
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
            node->loc = loc;
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
        node->loc = lhs->loc;
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
