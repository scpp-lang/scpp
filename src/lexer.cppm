module;

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

export module scpp.lexer;

export namespace scpp {

enum class TokenKind {
    // literals / identifiers
    Identifier,
    IntegerLiteral,
    // A floating-point literal (`1.5`, `0.25`, ...) -- digits, a decimal
    // point, then more digits (no exponent notation in this version).
    // `text` is the exact source substring, like every other literal
    // token; decoding it into a double happens in the parser (see
    // decode_float_literal), not here.
    FloatLiteral,
    // A char literal (`'a'`, `'\n'`, ...). `text` includes the
    // surrounding single quotes (same exact-source-substring convention
    // as StringLiteral below); decoding the escape sequence into an
    // ordinal value happens in the parser (see decode_char_literal), not
    // here -- the lexer just finds the token's extent.
    CharLiteral,
    // String literal (`"hello\n"`), also reused as-is for the linkage
    // token `"C"` in `extern "C"` (ch02 §2.1). `text` includes the
    // surrounding quotes (matching every other literal token's
    // exact-source-substring convention); decoding the escape sequences
    // into byte content happens in the parser (see decode_string_literal),
    // not here -- the lexer just finds the token's extent, same division
    // of labor as CharLiteral.
    StringLiteral,

    // keywords
    KwInt,
    KwBool,
    KwChar, // a scalar byte type (LLVM i8, signed) -- see codegen's
            // to_llvm_type. Parses exactly like int/bool everywhere a
            // type is expected (locals, params, fields, arrays,
            // pointers); no implicit promotion to/from `int` exists yet
            // (matching the same pre-existing lack of promotion between
            // `bool` and `int`), so mixing a `char` with a plain integer
            // literal/expression in one arithmetic/comparison op isn't
            // supported -- use a char literal (`'a'`) or another `char`
            // value on both sides instead.
    // ch06 §6: the rest of the numeric family's own *keyword* members --
    // real C++ keywords in their own right (unlike int8_t/uint8_t/.../
    // size_t/ptrdiff_t/float32_t/float64_t below, ordinary <cstdint>/
    // <cstddef>/<stdfloat> typedef names, not keywords at all in real
    // C++ -- those are instead recognized as pre-registered identifiers,
    // see Parser's own constructor). `unsigned` is only ever legal
    // directly before `int`/`long` (ch06: the bare one-word shorthand
    // isn't valid scpp) -- parse_unqualified_type enforces this, not the
    // lexer.
    KwLong,
    KwFloat,
    KwDouble,
    KwUnsigned,
    KwVoid, // ch02 §2.1: valid only as a function return type or as a
            // pointer's pointee (`void*`) -- never as a bare
            // variable/parameter/field type. Needed for `extern "C"`
            // signatures (e.g. `void free(void* p);`).
    KwReturn,
    KwIf,
    KwElse,
    KwWhile,
    KwBreak,
    KwContinue,
    KwFor,
    KwNew,
    KwDelete,
    KwExtern,
    KwTrue,
    KwFalse,
    KwEnum,
    KwStruct,
    KwUnion,
    KwConst,
    KwConstexpr,
    KwConsteval,
    KwClass,   // ch04 §4.2: owns resources, participates in move/borrow
               // checking, private-by-default access control -- unlike
               // `struct` (trivial aggregate, always-public fields).
    KwPublic,  // Only legal directly above a member *function* -- ch04
               // §4.2 permanently forbids a public member variable
               // (including a class-level constant).
    KwPrivate, // Default access if a class body has no leading
               // access-specifier section at all, matching real C++.
    KwThis,    // ch05 §5.9: implicit reference parameter of every method
               // -- `const T&` in a `const` method, `T&` otherwise.
    KwModule,    // ch11 §11.3: `export module name;` / `module name;` --
                 // module declaration, must be the first thing in a file.
    KwExport,    // ch11 §11.3/§11.7: prefixes `module` (interface unit),
                 // `import` (re-export), or an individual top-level
                 // declaration/`export { ... }` group (marks it visible
                 // to importers).
    KwImport,    // ch11 §11.7: `import name;` (private) / `export import
                 // name;` (re-exporting).
    KwNamespace, // ch11 §11.4: `namespace a::b::c { ... }` (real C++
                 // syntax, including the C++17 one-line nested form).
    KwTemplate,  // ch05 §5.11: `template<typename T>` -- required
                 // (real C++ grammar has no other way to declare a
                 // `concept`) directly above a `concept` declaration
                 // only; a *function* is never spelled with this header
                 // in v0.1 (abbreviated `Concept auto` form only).
    KwTypename,  // ch05 §5.11: names the single template parameter in a
                 // concept's own `template<typename T>` header.
    KwConcept,   // ch05 §5.11: `concept Name = requires(...) { ... };` --
                 // a compile-time structural predicate over one type.
    KwRequires,  // ch05 §5.11: introduces a requires-expression's
                 // parenthesized placeholder parameter list + brace-
                 // enclosed requirement sequence.
    KwAuto,      // ch05 §5.11: only meaningful directly after a concept
                 // name in a generic function parameter (`Shape auto&
                 // s`, `Shape auto&&`, `const Shape auto&`) -- the
                 // abbreviated C++20 generic-function form; v0.1 has no
                 // other use for `auto` (no type inference for ordinary
                 // variables).
    KwMutable,   // ch05 §5.12: trailing qualifier on a lambda's parameter
                 // list (`[x](int y) mutable { ... }`), allowing the
                 // synthesized closure's own operator() to modify
                 // by-value-captured fields -- the mirror image of an
                 // ordinary method's own trailing `const` (ch05 §5.9):
                 // absent means a `const` operator(), present means a
                 // non-`const` one.

    // punctuation
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Semicolon,
    Comma,
    Dot,
    Ellipsis, // `...` -- variadic parameter marker, fold-expression token, or
              // pack expansion marker, depending on parser context.
    ColonColon,
    Colon, // `:` -- class access-specifier sections (`public:`/`private:`,
           // ch04 §4.2) only, in this version.
    Arrow,
    Tilde, // `~` -- destructor declarator prefix only (`~ClassName()`,
           // ch04 §4.2), in this version.

    // operators
    Plus,
    Minus,
    Star,
    Slash,
    Assign,
    EqualEqual,
    NotEqual,
    Less,
    Greater,
    LessEqual,
    GreaterEqual,
    AmpAmp,
    Amp,
    PipePipe,
    Bang,
    Question,

    EndOfFile,
    Unknown,
};

struct Token {
    TokenKind kind;
    std::string_view text;
    int line;
    int column;
};

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    std::vector<Token> tokenize() {
        std::vector<Token> tokens;
        for (;;) {
            Token tok = next();
            bool is_eof = tok.kind == TokenKind::EndOfFile;
            tokens.push_back(tok);
            if (is_eof) break;
        }
        return tokens;
    }

private:
    std::string_view source_;
    size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;

    [[nodiscard]] bool at_end() const { return pos_ >= source_.size(); }

    char peek(size_t offset = 0) const {
        size_t idx = pos_ + offset;
        return idx < source_.size() ? source_[idx] : '\0';
    }

    char advance() {
        char c = source_[pos_++];
        if (c == '\n') {
            line_++;
            column_ = 1;
        } else {
            column_++;
        }
        return c;
    }

    void skip_whitespace_and_comments() {
        for (;;) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                advance();
            } else if (c == '/' && peek(1) == '/') {
                while (!at_end() && peek() != '\n') advance();
            } else if (c == '/' && peek(1) == '*') {
                advance();
                advance();
                while (!at_end() && !(peek() == '*' && peek(1) == '/')) advance();
                if (!at_end()) {
                    advance();
                    advance();
                }
            } else {
                break;
            }
        }
    }

    static bool is_ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
    static bool is_ident_continue(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

    static TokenKind keyword_kind(std::string_view text) {
        if (text == "int") return TokenKind::KwInt;
        if (text == "bool") return TokenKind::KwBool;
        if (text == "char") return TokenKind::KwChar;
        if (text == "long") return TokenKind::KwLong;
        if (text == "float") return TokenKind::KwFloat;
        if (text == "double") return TokenKind::KwDouble;
        if (text == "unsigned") return TokenKind::KwUnsigned;
        if (text == "void") return TokenKind::KwVoid;
        if (text == "return") return TokenKind::KwReturn;
        if (text == "if") return TokenKind::KwIf;
        if (text == "else") return TokenKind::KwElse;
        if (text == "while") return TokenKind::KwWhile;
        if (text == "break") return TokenKind::KwBreak;
        if (text == "continue") return TokenKind::KwContinue;
        if (text == "for") return TokenKind::KwFor;
        if (text == "new") return TokenKind::KwNew;
        if (text == "delete") return TokenKind::KwDelete;
        if (text == "extern") return TokenKind::KwExtern;
        if (text == "true") return TokenKind::KwTrue;
        if (text == "false") return TokenKind::KwFalse;
        if (text == "enum") return TokenKind::KwEnum;
        if (text == "struct") return TokenKind::KwStruct;
        if (text == "union") return TokenKind::KwUnion;
        if (text == "const") return TokenKind::KwConst;
        if (text == "constexpr") return TokenKind::KwConstexpr;
        if (text == "consteval") return TokenKind::KwConsteval;
        if (text == "class") return TokenKind::KwClass;
        if (text == "public") return TokenKind::KwPublic;
        if (text == "private") return TokenKind::KwPrivate;
        if (text == "this") return TokenKind::KwThis;
        if (text == "module") return TokenKind::KwModule;
        if (text == "export") return TokenKind::KwExport;
        if (text == "import") return TokenKind::KwImport;
        if (text == "namespace") return TokenKind::KwNamespace;
        if (text == "template") return TokenKind::KwTemplate;
        if (text == "typename") return TokenKind::KwTypename;
        if (text == "concept") return TokenKind::KwConcept;
        if (text == "requires") return TokenKind::KwRequires;
        if (text == "auto") return TokenKind::KwAuto;
        if (text == "mutable") return TokenKind::KwMutable;
        return TokenKind::Identifier;
    }

    Token make_token(TokenKind kind, size_t start, int start_line, int start_col) {
        return Token{kind, source_.substr(start, pos_ - start), start_line, start_col};
    }

    Token next() {
        // Captured *before* skipping trailing whitespace/comments so the
        // EndOfFile token below reports the position right after the
        // last real content, not wherever the cursor lands after
        // consuming any trailing blank lines/whitespace -- matching how
        // Clang/GCC position an "unexpected end of file" diagnostic
        // (e.g. `int sfsf\n` reports line 1 column 9, immediately after
        // "sfsf", never line 2 column 1 just because the file happens to
        // end with a trailing newline).
        int pre_skip_line = line_;
        int pre_skip_col = column_;
        skip_whitespace_and_comments();

        if (at_end()) {
            return Token{TokenKind::EndOfFile, {}, pre_skip_line, pre_skip_col};
        }

        size_t start = pos_;
        int start_line = line_;
        int start_col = column_;
        char c = advance();

        if (is_ident_start(c)) {
            while (!at_end() && is_ident_continue(peek())) advance();
            std::string_view text = source_.substr(start, pos_ - start);
            return Token{keyword_kind(text), text, start_line, start_col};
        }

        if (std::isdigit(static_cast<unsigned char>(c))) {
            while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
            // A `.` followed by at least one digit makes this a
            // FloatLiteral instead (`1.5`) -- a bare trailing `.` with no
            // following digit (`1.`) is *not* consumed here, left as an
            // IntegerLiteral followed by a separate `.` token (member
            // access on an integer literal is nonsensical but not this
            // lexer's problem to reject).
            if (!at_end() && peek() == '.' && pos_ + 1 < source_.size() &&
                std::isdigit(static_cast<unsigned char>(source_[pos_ + 1]))) {
                advance(); // '.'
                while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
                return make_token(TokenKind::FloatLiteral, start, start_line, start_col);
            }
            return make_token(TokenKind::IntegerLiteral, start, start_line, start_col);
        }

        if (c == '"') {
            // String literal (ch02 §2.1's linkage token "C" reuses this
            // too): finds the token's extent the same way CharLiteral
            // does just below (a backslash escapes the following
            // character so e.g. `"\""` doesn't end the literal early).
            // Decoding the escape sequences into byte content is the
            // parser's job (decode_string_literal), not the lexer's.
            while (!at_end() && peek() != '"') {
                if (peek() == '\\' && !at_end()) advance();
                advance();
            }
            if (!at_end()) advance(); // closing quote
            return make_token(TokenKind::StringLiteral, start, start_line, start_col);
        }

        if (c == '\'') {
            // Char literal (`'a'`, `'\n'`, ...): finds the token's extent
            // the same way the string literal above does (a backslash
            // escapes the following character so e.g. `'\''` doesn't end
            // the literal early). Decoding the escape sequence into an
            // ordinal value is the parser's job (decode_char_literal),
            // not the lexer's.
            while (!at_end() && peek() != '\'') {
                if (peek() == '\\' && !at_end()) advance();
                advance();
            }
            if (!at_end()) advance(); // closing quote
            return make_token(TokenKind::CharLiteral, start, start_line, start_col);
        }

        switch (c) {
            case '(': return make_token(TokenKind::LParen, start, start_line, start_col);
            case ')': return make_token(TokenKind::RParen, start, start_line, start_col);
            case '{': return make_token(TokenKind::LBrace, start, start_line, start_col);
            case '}': return make_token(TokenKind::RBrace, start, start_line, start_col);
            case '[': return make_token(TokenKind::LBracket, start, start_line, start_col);
            case ']': return make_token(TokenKind::RBracket, start, start_line, start_col);
            case ';': return make_token(TokenKind::Semicolon, start, start_line, start_col);
            case ',': return make_token(TokenKind::Comma, start, start_line, start_col);
            case '.':
                if (peek() == '.' && peek(1) == '.') {
                    advance();
                    advance();
                    return make_token(TokenKind::Ellipsis, start, start_line, start_col);
                }
                return make_token(TokenKind::Dot, start, start_line, start_col);
            case '+': return make_token(TokenKind::Plus, start, start_line, start_col);
            case '-':
                if (peek() == '>') { advance(); return make_token(TokenKind::Arrow, start, start_line, start_col); }
                return make_token(TokenKind::Minus, start, start_line, start_col);
            case '*': return make_token(TokenKind::Star, start, start_line, start_col);
            case '/': return make_token(TokenKind::Slash, start, start_line, start_col);
            case '!':
                if (peek() == '=') { advance(); return make_token(TokenKind::NotEqual, start, start_line, start_col); }
                return make_token(TokenKind::Bang, start, start_line, start_col);
            case '?':
                return make_token(TokenKind::Question, start, start_line, start_col);
            case '=':
                if (peek() == '=') { advance(); return make_token(TokenKind::EqualEqual, start, start_line, start_col); }
                return make_token(TokenKind::Assign, start, start_line, start_col);
            case '<':
                if (peek() == '=') { advance(); return make_token(TokenKind::LessEqual, start, start_line, start_col); }
                return make_token(TokenKind::Less, start, start_line, start_col);
            case '>':
                if (peek() == '=') { advance(); return make_token(TokenKind::GreaterEqual, start, start_line, start_col); }
                return make_token(TokenKind::Greater, start, start_line, start_col);
            case '&':
                if (peek() == '&') { advance(); return make_token(TokenKind::AmpAmp, start, start_line, start_col); }
                return make_token(TokenKind::Amp, start, start_line, start_col);
            case '|':
                if (peek() == '|') { advance(); return make_token(TokenKind::PipePipe, start, start_line, start_col); }
                return make_token(TokenKind::Unknown, start, start_line, start_col);
            case ':':
                if (peek() == ':') { advance(); return make_token(TokenKind::ColonColon, start, start_line, start_col); }
                return make_token(TokenKind::Colon, start, start_line, start_col);
            case '~':
                return make_token(TokenKind::Tilde, start, start_line, start_col);
            default:
                return make_token(TokenKind::Unknown, start, start_line, start_col);
        }
    }
};

std::vector<Token> tokenize(std::string_view source) {
    Lexer lexer(source);
    return lexer.tokenize();
}

} // namespace scpp
