module;

export module scpp.lexer;

import std;

export namespace scpp {

enum class TokenKind {
    // literals / identifiers
    Identifier,
    // An integer literal in any of [lex.icon]'s four bases -- decimal
    // (`42`), hexadecimal (`0x2a`, `0X2A`), octal (`052`), or binary
    // (`0b101010`) -- optionally with `'` digit separators and an
    // integer-suffix (`u`/`U`, `l`/`L`, `ll`/`LL`, `z`/`Z` and the legal
    // combinations of those). spec §16.2 changes only what *type* such a
    // literal has, not how it is spelled, so the whole of [lex.icon]'s
    // grammar is available here.
    //
    // `text` is the exact source substring, prefix/separators/suffix and
    // all, like every other literal token; turning it into a value
    // happens in the parser (see decode_integer_literal), not here. The
    // lexer finds the token's extent -- it deliberately accepts an
    // ill-formed spelling (`0x`, `08`, `1ULLL`, `1abc`) as one token so
    // the parser can report it against the whole literal instead of
    // silently splitting it into a valid literal plus an identifier.
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
    // ch06 §6: the rest of the numeric family's own keyword spellings.
    // `unsigned` is only ever legal directly before `int`/`long` (ch06:
    // the bare one-word shorthand isn't valid scpp) -- parse_
    // unqualified_type enforces this, not the lexer. The fixed-width
    // integer names plus the ubiquitous `<cstddef>` scalar aliases
    // `size_t`/`ptrdiff_t` are promoted to true keywords too, even though
    // real C++ exposes them as typedef names.
    KwLong,
    KwFloat,
    KwDouble,
    KwUnsigned,
    KwSizeT,
    KwPtrdiffT,
    // ch06 §6: the null pointer literal `nullptr` and its type
    // `nullptr_t`. Real C++ makes `nullptr` a keyword and exposes the
    // type only as the library alias `std::nullptr_t` (over the
    // unspellable `decltype(nullptr)`); scpp gives the type a real
    // builtin spelling instead, exactly as it already does for
    // `size_t`/`ptrdiff_t` above -- so `nullptr`'s type is known to the
    // compiler with no module imported at all.
    KwNullptr,
    KwNullptrT,
    KwInt8T,
    KwUInt8T,
    KwInt16T,
    KwUInt16T,
    KwInt32T,
    KwUInt32T,
    KwInt64T,
    KwUInt64T,
    KwVoid, // ch02 §2.1: valid only as a function return type or as a
            // pointer's pointee (`void*`) -- never as a bare
            // variable/parameter/field type. Needed for `extern "C"`
            // signatures (e.g. `void free(void* p);`).
    KwReturn,
    KwIf,
    KwElse,
    KwWhile,
    KwSwitch,
    KwCase,
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
    KwInline,
    KwClass,   // ch04 §4.2: owns resources, participates in move/borrow
               // checking, private-by-default access control -- unlike
               // `struct` (trivial aggregate, always-public fields).
    KwStatic,  // class member function specifier: no implicit `this`,
               // callable as `ClassName::method(...)`.
    KwVirtual, // class member/base-specifier `virtual`.
    KwOverride, // trailing virt-specifier on a member function.
    KwUsing,   // class-scope `using Base::member;`.
    KwDefault, // `= default` on a member declaration.
    KwExplicit, // Constructor specifier (docs/book/en/ch05-01: "One-
                // argument constructors can convert at call sites").
                // Only accepted directly before an in-class constructor
                // declaration in this version; actually suppressing the
                // converting-constructor behavior isn't implemented yet
                // -- parsed and accepted like a free function's own
                // `inline` (parse_function), but not yet semantically
                // enforced.
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
    KwAlignas,
    KwAlignof,
    KwSizeof,    // ch06: `sizeof(T)` / `sizeof(expr)` -- target-ABI size in
                 // bytes of a type or unevaluated expression operand.
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
    Tilde, // `~` -- either a destructor declarator prefix (`~ClassName()`,
           // ch04 §4.2) or the unary bitwise-complement operator
           // ([expr.unary.op]/9); the parser tells the two apart by
           // context, exactly as it already does for `*`, `-` and `&`.

    // operators
    PlusPlus,
    MinusMinus,
    PlusAssign,
    MinusAssign,
    StarAssign,
    SlashAssign,
    PercentAssign,
    AmpAssign,
    CaretAssign,
    PipeAssign,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Caret, // `^` -- bitwise exclusive-or ([expr.xor]).
    Assign,
    EqualEqual,
    NotEqual,
    // `<<`, `>>`, `<<=` and `>>=` deliberately have no token of their
    // own: `>>` also closes two nested template-argument-lists
    // ([temp.names]/3), and one `>>` token would have to be split back
    // apart at every one of those closings. The lexer keeps emitting a
    // token per `>`/`<` and the parser's shift level re-joins two
    // *adjacent* ones (see Parser::shift_operator_at), so a shift is
    // recognised exactly where the grammar allows one and template
    // parsing is untouched.
    Less,
    Greater,
    LessEqual,
    GreaterEqual,
    AmpAmp,
    Amp,
    PipePipe,
    Pipe, // `|` -- bitwise inclusive-or ([expr.or]).
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

    Token(TokenKind kind, std::string_view text, int line, int column)
        : kind{kind}, text{text}, line{line}, column{column} {}
};

// What `c` is worth as a digit, or 99 -- a value above every base --
// when it is not a digit at all. One character-classification question
// serves all four of [lex.icon]'s bases: a caller just asks whether the
// answer is below the base it is working in, so `8` ends an
// octal-literal and `f` ends a decimal one without either needing its
// own predicate.
//
// Shared with the parser so that the extent the lexer scans and the
// digits the parser reads can never disagree about what a digit is.
[[nodiscard]] inline int integer_digit_value(char c) {
    if (c >= '0' && c <= '9') return static_cast<int>(c) - static_cast<int>('0');
    if (c >= 'a' && c <= 'f') return static_cast<int>(c) - static_cast<int>('a') + 10;
    if (c >= 'A' && c <= 'F') return static_cast<int>(c) - static_cast<int>('A') + 10;
    return 99;
}

// [lex.icon]'s *integer-suffix*: an optional unsigned-suffix (`u`/`U`)
// in either position around an optional size suffix, which is `l`/`L`,
// `ll`/`LL` (both letters the same case -- `lL` is not a suffix) or
// C++23's `z`/`Z`. The empty suffix is valid: most literals have none.
//
// spec §16.2(1) gives an integer-literal "no type of its own", so a
// suffix does not select a type the way it does in C++; it is accepted
// because it is part of how an integer-literal is spelled, and is then
// ignored. Validating it anyway keeps `1ULLL` an error rather than a
// literal that quietly means something else than it says.
[[nodiscard]] inline bool is_valid_integer_suffix(std::string_view suffix) {
    std::size_t i = 0;
    bool seen_unsigned = false;
    if (i < suffix.size() && (suffix.at(i) == 'u' || suffix.at(i) == 'U')) {
        seen_unsigned = true;
        i = i + 1;
    }
    if (i < suffix.size()) {
        char c = suffix.at(i);
        if (c == 'l' || c == 'L') {
            i = i + 1;
            if (i < suffix.size() && suffix.at(i) == c) i = i + 1;
        } else if (c == 'z' || c == 'Z') {
            i = i + 1;
        }
    }
    if (!seen_unsigned && i < suffix.size() && (suffix.at(i) == 'u' || suffix.at(i) == 'U')) {
        i = i + 1;
    }
    return i == suffix.size();
}

struct Lexer {
public:
    explicit Lexer(std::string_view source) : source_{source} {}

    std::vector<Token> tokenize() {
        std::vector<Token> tokens{};
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
    std::size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;

    [[nodiscard]] bool at_end() const { return pos_ >= source_.size(); }

    char peek(std::size_t offset = 0) const {
        std::size_t idx = pos_ + offset;
        return idx < source_.size() ? source_.at(idx) : '\0';
    }

    char advance() {
        char c = source_.at(pos_++);
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

    static bool is_ident_start(char c) { return std::isalpha(static_cast<std::uint8_t>(c)) || c == '_'; }
    static bool is_ident_continue(char c) { return std::isalnum(static_cast<std::uint8_t>(c)) || c == '_'; }

    static TokenKind keyword_kind(std::string_view text) {
        if (text == "int") return TokenKind::KwInt;
        if (text == "bool") return TokenKind::KwBool;
        if (text == "char") return TokenKind::KwChar;
        if (text == "long") return TokenKind::KwLong;
        if (text == "float") return TokenKind::KwFloat;
        if (text == "double") return TokenKind::KwDouble;
        if (text == "unsigned") return TokenKind::KwUnsigned;
        if (text == "size_t") return TokenKind::KwSizeT;
        if (text == "ptrdiff_t") return TokenKind::KwPtrdiffT;
        if (text == "nullptr") return TokenKind::KwNullptr;
        if (text == "nullptr_t") return TokenKind::KwNullptrT;
        if (text == "int8_t") return TokenKind::KwInt8T;
        if (text == "uint8_t") return TokenKind::KwUInt8T;
        if (text == "int16_t") return TokenKind::KwInt16T;
        if (text == "uint16_t") return TokenKind::KwUInt16T;
        if (text == "int32_t") return TokenKind::KwInt32T;
        if (text == "uint32_t") return TokenKind::KwUInt32T;
        if (text == "int64_t") return TokenKind::KwInt64T;
        if (text == "uint64_t") return TokenKind::KwUInt64T;
        if (text == "void") return TokenKind::KwVoid;
        if (text == "return") return TokenKind::KwReturn;
        if (text == "if") return TokenKind::KwIf;
        if (text == "else") return TokenKind::KwElse;
        if (text == "while") return TokenKind::KwWhile;
        if (text == "switch") return TokenKind::KwSwitch;
        if (text == "case") return TokenKind::KwCase;
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
        if (text == "inline") return TokenKind::KwInline;
        if (text == "class") return TokenKind::KwClass;
        if (text == "static") return TokenKind::KwStatic;
        if (text == "virtual") return TokenKind::KwVirtual;
        if (text == "override") return TokenKind::KwOverride;
        if (text == "using") return TokenKind::KwUsing;
        if (text == "default") return TokenKind::KwDefault;
        if (text == "explicit") return TokenKind::KwExplicit;
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
        if (text == "alignas") return TokenKind::KwAlignas;
        if (text == "alignof") return TokenKind::KwAlignof;
        if (text == "sizeof") return TokenKind::KwSizeof;
        if (text == "mutable") return TokenKind::KwMutable;
        return TokenKind::Identifier;
    }

    Token make_token(TokenKind kind, std::size_t start, int start_line, int start_col) {
        Token tok{kind, source_.substr(start, pos_ - start), start_line, start_col};
        return tok;
    }

    // Consumes digits of `base`, plus [lex.icon]'s `'` digit separators.
    // A `'` is only taken as a separator when a digit of this base
    // follows it, so the `'` of a char-literal never gets swallowed by a
    // number that happens to sit in front of it.
    void scan_digits(int base) {
        for (;;) {
            if (at_end()) return;
            char c = peek();
            if (c == '\'') {
                if (integer_digit_value(peek(1)) >= base) return;
                advance(); // the separator
                advance(); // the digit after it
                continue;
            }
            if (integer_digit_value(c) >= base) return;
            advance();
        }
    }

    // An integer-literal's extent, from just past its first digit.
    //
    // The token runs to the end of the whole number-like run --
    // including any trailing identifier characters, which is what a
    // suffix is made of -- so that an ill-formed spelling stays one
    // token. Splitting `0x` into `0` and `x`, or `1ULLL` into `1` and
    // `ULLL`, would hand the parser a valid literal followed by a
    // stray identifier and produce a diagnostic about the wrong thing;
    // decode_integer_literal validates the spelling and reports it
    // against the literal as a whole.
    Token lex_number(std::size_t start, int start_line, int start_col) {
        // `pos_` is one past the first digit, which `start` still points at.
        char first = source_.at(start);
        if (first == '0' && !at_end() && (peek() == 'x' || peek() == 'X')) {
            advance();
            scan_digits(16);
            return finish_integer_literal(start, start_line, start_col);
        }
        if (first == '0' && !at_end() && (peek() == 'b' || peek() == 'B')) {
            advance();
            scan_digits(2);
            return finish_integer_literal(start, start_line, start_col);
        }
        // Decimal and octal share this scan: both are spelled with
        // decimal digits, and an octal-literal's leading `0` only
        // changes how the digits are *read*, not which ones terminate
        // the run. `08` therefore scans as one literal here and is
        // rejected later as an octal-literal with a non-octal digit,
        // rather than becoming `0` followed by `8`.
        scan_digits(10);
        // A `.` followed by at least one digit makes this a
        // FloatLiteral instead (`1.5`) -- a bare trailing `.` with no
        // following digit (`1.`) is *not* consumed here, left as an
        // IntegerLiteral followed by a separate `.` token (member
        // access on an integer literal is nonsensical but not this
        // lexer's problem to reject). A floating-point literal takes no
        // suffix in this version, so this returns directly rather than
        // going through finish_integer_literal.
        if (!at_end() && peek() == '.' && pos_ + 1 < source_.size() &&
            std::isdigit(static_cast<std::uint8_t>(source_.at(pos_ + 1)))) {
            advance(); // '.'
            scan_digits(10);
            return make_token(TokenKind::FloatLiteral, start, start_line, start_col);
        }
        return finish_integer_literal(start, start_line, start_col);
    }

    Token finish_integer_literal(std::size_t start, int start_line, int start_col) {
        while (!at_end() && is_ident_continue(peek())) advance();
        return make_token(TokenKind::IntegerLiteral, start, start_line, start_col);
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
            std::string_view empty_text{};
            Token eof_tok{TokenKind::EndOfFile, empty_text, pre_skip_line, pre_skip_col};
            return eof_tok;
        }

        std::size_t start = pos_;
        int start_line = line_;
        int start_col = column_;
        char c = advance();

        if (is_ident_start(c)) {
            while (!at_end() && is_ident_continue(peek())) advance();
            std::string_view text = source_.substr(start, pos_ - start);
            Token ident_tok{keyword_kind(text), text, start_line, start_col};
            return ident_tok;
        }

        if (std::isdigit(static_cast<std::uint8_t>(c))) {
            return lex_number(start, start_line, start_col);
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
            case '+':
                if (peek() == '+') { advance(); return make_token(TokenKind::PlusPlus, start, start_line, start_col); }
                if (peek() == '=') { advance(); return make_token(TokenKind::PlusAssign, start, start_line, start_col); }
                return make_token(TokenKind::Plus, start, start_line, start_col);
            case '-':
                if (peek() == '-') { advance(); return make_token(TokenKind::MinusMinus, start, start_line, start_col); }
                if (peek() == '=') { advance(); return make_token(TokenKind::MinusAssign, start, start_line, start_col); }
                if (peek() == '>') { advance(); return make_token(TokenKind::Arrow, start, start_line, start_col); }
                return make_token(TokenKind::Minus, start, start_line, start_col);
            case '*':
                if (peek() == '=') { advance(); return make_token(TokenKind::StarAssign, start, start_line, start_col); }
                return make_token(TokenKind::Star, start, start_line, start_col);
            case '/':
                if (peek() == '=') { advance(); return make_token(TokenKind::SlashAssign, start, start_line, start_col); }
                return make_token(TokenKind::Slash, start, start_line, start_col);
            case '%':
                if (peek() == '=') { advance(); return make_token(TokenKind::PercentAssign, start, start_line, start_col); }
                return make_token(TokenKind::Percent, start, start_line, start_col);
            case '^':
                if (peek() == '=') { advance(); return make_token(TokenKind::CaretAssign, start, start_line, start_col); }
                return make_token(TokenKind::Caret, start, start_line, start_col);
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
                if (peek() == '=') { advance(); return make_token(TokenKind::AmpAssign, start, start_line, start_col); }
                return make_token(TokenKind::Amp, start, start_line, start_col);
            case '|':
                if (peek() == '|') { advance(); return make_token(TokenKind::PipePipe, start, start_line, start_col); }
                if (peek() == '=') { advance(); return make_token(TokenKind::PipeAssign, start, start_line, start_col); }
                return make_token(TokenKind::Pipe, start, start_line, start_col);
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
    Lexer lexer{source};
    return lexer.tokenize();
}

} // namespace scpp
