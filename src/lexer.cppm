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
    // A char literal (`'a'`, `'\n'`, ...). `text` includes the
    // surrounding single quotes (same exact-source-substring convention
    // as StringLiteral below); decoding the escape sequence into an
    // ordinal value happens in the parser (see decode_char_literal), not
    // here -- the lexer just finds the token's extent.
    CharLiteral,
    // Minimal string literal (ch02 §2.1): just enough to recognize the
    // linkage token `"C"` in `extern "C"`. `text` includes the
    // surrounding quotes (matching every other literal token's
    // exact-source-substring convention); v0.1 has no general
    // string-literal expression type yet, so this is never produced or
    // consumed anywhere outside the `extern "C"` grammar.
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
    KwVoid, // ch02 §2.1: valid only as a function return type or as a
            // pointer's pointee (`void*`) -- never as a bare
            // variable/parameter/field type. Needed for `extern "C"`
            // signatures (e.g. `void free(void* p);`).
    KwReturn,
    KwIf,
    KwElse,
    KwWhile,
    KwFor,
    KwSafe,
    KwUnsafe,
    KwExtern,
    KwTrue,
    KwFalse,
    KwStruct,
    KwConst,

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
    Ellipsis, // `...` -- extern "C" variadic parameter marker only (ch02 §2.1)
    ColonColon,
    Arrow,

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
        if (text == "void") return TokenKind::KwVoid;
        if (text == "return") return TokenKind::KwReturn;
        if (text == "if") return TokenKind::KwIf;
        if (text == "else") return TokenKind::KwElse;
        if (text == "while") return TokenKind::KwWhile;
        if (text == "for") return TokenKind::KwFor;
        if (text == "safe") return TokenKind::KwSafe;
        if (text == "unsafe") return TokenKind::KwUnsafe;
        if (text == "extern") return TokenKind::KwExtern;
        if (text == "true") return TokenKind::KwTrue;
        if (text == "false") return TokenKind::KwFalse;
        if (text == "struct") return TokenKind::KwStruct;
        if (text == "const") return TokenKind::KwConst;
        return TokenKind::Identifier;
    }

    Token make_token(TokenKind kind, size_t start, int start_line, int start_col) {
        return Token{kind, source_.substr(start, pos_ - start), start_line, start_col};
    }

    Token next() {
        skip_whitespace_and_comments();

        if (at_end()) {
            return Token{TokenKind::EndOfFile, {}, line_, column_};
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
            return make_token(TokenKind::IntegerLiteral, start, start_line, start_col);
        }

        if (c == '"') {
            // Minimal string-literal lexing (ch02 §2.1): just enough to
            // recognize `"C"` in `extern "C"`. Supports a backslash escape
            // for an embedded quote/backslash (so e.g. `"\""` doesn't end
            // the literal early), but doesn't interpret any other escape
            // sequence -- v0.1 has no general string-literal expression
            // type yet, so nothing downstream needs escape decoding.
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
                return make_token(TokenKind::Unknown, start, start_line, start_col);
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
