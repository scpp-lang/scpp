import scpp.lexer;
import std;

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        failures++;
    }
}

void expect_kinds(std::string_view source, std::vector<scpp::TokenKind> expected, std::string_view case_name) {
    std::vector<scpp::Token> tokens = scpp::tokenize(source);
    expect(tokens.size() == expected.size(),
           std::string(case_name) + ": expected " + std::to_string(expected.size()) +
               " tokens, got " + std::to_string(tokens.size()));
    for (std::size_t i = 0; i < tokens.size() && i < expected.size(); i++) {
        expect(tokens[i].kind == expected[i],
               std::string(case_name) + ": token " + std::to_string(i) + " kind mismatch");
    }
}

void test_empty_source() {
    expect_kinds("", {scpp::TokenKind::EndOfFile}, "empty_source");
}

void test_int_main_return() {
    expect_kinds(
        "int main() {\n    return 42;\n}\n",
        {
            scpp::TokenKind::KwInt,
            scpp::TokenKind::Identifier,
            scpp::TokenKind::LParen,
            scpp::TokenKind::RParen,
            scpp::TokenKind::LBrace,
            scpp::TokenKind::KwReturn,
            scpp::TokenKind::IntegerLiteral,
            scpp::TokenKind::Semicolon,
            scpp::TokenKind::RBrace,
            scpp::TokenKind::EndOfFile,
        },
        "int_main_return");
}

void test_keywords() {
    expect_kinds(
        "bool if else while for true false struct const new delete virtual override using default",
        {
            scpp::TokenKind::KwBool,
            scpp::TokenKind::KwIf,
            scpp::TokenKind::KwElse,
            scpp::TokenKind::KwWhile,
            scpp::TokenKind::KwFor,
            scpp::TokenKind::KwTrue,
            scpp::TokenKind::KwFalse,
            scpp::TokenKind::KwStruct,
            scpp::TokenKind::KwConst,
            scpp::TokenKind::KwNew,
            scpp::TokenKind::KwDelete,
            scpp::TokenKind::KwVirtual,
            scpp::TokenKind::KwOverride,
            scpp::TokenKind::KwUsing,
            scpp::TokenKind::KwDefault,
            scpp::TokenKind::EndOfFile,
        },
        "keywords");
}

// ch00 §2/ch01 §1.3: `unsafe` is no longer a keyword at all -- it's the
// attribute-token `unsafe` in the `scpp` namespace, spelled
// `[[scpp::unsafe]]`. A bare `unsafe` identifier must therefore lex as
// an ordinary Identifier, exactly like any other undeclared name (e.g.
// it could be used as a variable name without conflict, unlike a real
// keyword).
void test_unsafe_is_not_a_keyword() {
    expect_kinds("unsafe", {scpp::TokenKind::Identifier, scpp::TokenKind::EndOfFile}, "unsafe_is_not_a_keyword");
}

// ch01 (safety-context reversal): `safe` is no longer a keyword at all --
// every function is checked by default, with no per-function annotation
// -- so it now lexes as an ordinary Identifier, exactly like any other
// unreserved word.
void test_safe_is_no_longer_a_keyword() {
    expect_kinds("safe", {scpp::TokenKind::Identifier, scpp::TokenKind::EndOfFile}, "safe_is_no_longer_a_keyword");
}

void test_static_is_a_keyword() {
    expect_kinds("static", {scpp::TokenKind::KwStatic, scpp::TokenKind::EndOfFile}, "static_is_a_keyword");
}

void test_inline_is_a_keyword() {
    expect_kinds("inline", {scpp::TokenKind::KwInline, scpp::TokenKind::EndOfFile}, "inline_is_a_keyword");
}

void test_fixed_width_integer_keywords() {
    expect_kinds(
        "int64_t uint32_t std::int64_t std::uint32_t",
        {
            scpp::TokenKind::KwInt64T,
            scpp::TokenKind::KwUInt32T,
            scpp::TokenKind::Identifier,
            scpp::TokenKind::ColonColon,
            scpp::TokenKind::KwInt64T,
            scpp::TokenKind::Identifier,
            scpp::TokenKind::ColonColon,
            scpp::TokenKind::KwUInt32T,
            scpp::TokenKind::EndOfFile,
        },
        "fixed_width_integer_keywords");
}

void test_identifier_text() {
    std::vector<scpp::Token> tokens = scpp::tokenize("foo_bar1");
    expect(tokens.size() == 2, "identifier_text: expected 2 tokens");
    expect(tokens[0].kind == scpp::TokenKind::Identifier, "identifier_text: kind should be Identifier");
    expect(tokens[0].text == "foo_bar1", "identifier_text: text should match 'foo_bar1'");
}

void test_integer_literal_text() {
    std::vector<scpp::Token> tokens = scpp::tokenize("12345");
    expect(tokens.size() == 2, "integer_literal_text: expected 2 tokens");
    expect(tokens[0].kind == scpp::TokenKind::IntegerLiteral, "integer_literal_text: kind should be IntegerLiteral");
    expect(tokens[0].text == "12345", "integer_literal_text: text should match '12345'");
}

void test_operators() {
    expect_kinds(
        "+ - * / = == != < > <= >= && || ! &",
        {
            scpp::TokenKind::Plus,
            scpp::TokenKind::Minus,
            scpp::TokenKind::Star,
            scpp::TokenKind::Slash,
            scpp::TokenKind::Assign,
            scpp::TokenKind::EqualEqual,
            scpp::TokenKind::NotEqual,
            scpp::TokenKind::Less,
            scpp::TokenKind::Greater,
            scpp::TokenKind::LessEqual,
            scpp::TokenKind::GreaterEqual,
            scpp::TokenKind::AmpAmp,
            scpp::TokenKind::PipePipe,
            scpp::TokenKind::Bang,
            scpp::TokenKind::Amp,
            scpp::TokenKind::EndOfFile,
        },
        "operators");
}

void test_arrow_operator() {
    expect_kinds(
        "p->x - 1",
        {
            scpp::TokenKind::Identifier,
            scpp::TokenKind::Arrow,
            scpp::TokenKind::Identifier,
            scpp::TokenKind::Minus,
            scpp::TokenKind::IntegerLiteral,
            scpp::TokenKind::EndOfFile,
        },
        "arrow_operator");
}

void test_comments_are_skipped() {
    expect_kinds(
        "// line comment\n"
        "int /* block\ncomment */ x;",
        {
            scpp::TokenKind::KwInt,
            scpp::TokenKind::Identifier,
            scpp::TokenKind::Semicolon,
            scpp::TokenKind::EndOfFile,
        },
        "comments_are_skipped");
}

void test_line_and_column_tracking() {
    std::vector<scpp::Token> tokens = scpp::tokenize("int\n  x;");
    expect(tokens.size() == 4, "line_and_column_tracking: expected 4 tokens");
    expect(tokens[0].line == 1 && tokens[0].column == 1, "line_and_column_tracking: 'int' at 1:1");
    expect(tokens[1].line == 2 && tokens[1].column == 3, "line_and_column_tracking: 'x' at 2:3");
}

// The EndOfFile token's position must point immediately after the last
// real content, not wherever the cursor lands after skipping trailing
// whitespace/blank lines/comments -- matching Clang/GCC (e.g. `clang -c`
// on `int sfsf\n` reports the missing-';' diagnostic at line 1 column 9,
// never line 2 column 1 just because the file happens to end with a
// trailing newline). See Lexer::next()'s pre_skip_line/pre_skip_col.
void test_eof_position_ignores_trailing_whitespace() {
    std::vector<scpp::Token> no_trailing_newline = scpp::tokenize("int sfsf");
    expect(no_trailing_newline.back().kind == scpp::TokenKind::EndOfFile,
           "eof_position: last token should be EndOfFile");
    expect(no_trailing_newline.back().line == 1 && no_trailing_newline.back().column == 9,
           "eof_position (no trailing newline): expected EOF at 1:9");

    std::vector<scpp::Token> one_trailing_newline = scpp::tokenize("int sfsf\n");
    expect(one_trailing_newline.back().line == 1 && one_trailing_newline.back().column == 9,
           "eof_position (one trailing newline): expected EOF at 1:9, not 2:1");

    std::vector<scpp::Token> trailing_blank_lines_and_comment = scpp::tokenize("int sfsf\n\n\n// trailing\n");
    expect(trailing_blank_lines_and_comment.back().line == 1 &&
               trailing_blank_lines_and_comment.back().column == 9,
           "eof_position (trailing blank lines + comment): expected EOF at 1:9");
}

void test_unknown_character() {
    std::vector<scpp::Token> tokens = scpp::tokenize("@");
    expect(tokens.size() == 2, "unknown_character: expected 2 tokens");
    expect(tokens[0].kind == scpp::TokenKind::Unknown, "unknown_character: kind should be Unknown");
}

void test_struct_punctuation() {
    expect_kinds(
        "struct Point { int x; }; p.x[0]",
        {
            scpp::TokenKind::KwStruct,
            scpp::TokenKind::Identifier,
            scpp::TokenKind::LBrace,
            scpp::TokenKind::KwInt,
            scpp::TokenKind::Identifier,
            scpp::TokenKind::Semicolon,
            scpp::TokenKind::RBrace,
            scpp::TokenKind::Semicolon,
            scpp::TokenKind::Identifier,
            scpp::TokenKind::Dot,
            scpp::TokenKind::Identifier,
            scpp::TokenKind::LBracket,
            scpp::TokenKind::IntegerLiteral,
            scpp::TokenKind::RBracket,
            scpp::TokenKind::EndOfFile,
        },
        "struct_punctuation");
}

void test_scope_resolution_operator() {
    expect_kinds(
        "std::move(a)",
        {
            scpp::TokenKind::Identifier,
            scpp::TokenKind::ColonColon,
            scpp::TokenKind::Identifier,
            scpp::TokenKind::LParen,
            scpp::TokenKind::Identifier,
            scpp::TokenKind::RParen,
            scpp::TokenKind::EndOfFile,
        },
        "scope_resolution_operator");
}

void test_extern_and_void_keywords() {
    // ch02 §2.1's prerequisites: `extern` and `void`.
    expect_kinds(
        "extern void",
        {
            scpp::TokenKind::KwExtern,
            scpp::TokenKind::KwVoid,
            scpp::TokenKind::EndOfFile,
        },
        "extern_and_void_keywords");
}

void test_string_literal() {
    // Minimal string-literal lexing (ch02 §2.1): just enough to
    // recognize `"C"`. `text` includes the surrounding quotes.
    std::vector<scpp::Token> tokens = scpp::tokenize("extern \"C\" int f();");
    expect(tokens.size() == 8, "string_literal: expected 8 tokens");
    expect(tokens[1].kind == scpp::TokenKind::StringLiteral, "string_literal: kind should be StringLiteral");
    expect(tokens[1].text == "\"C\"", "string_literal: text should be '\"C\"' (quotes included)");
}

void test_string_literal_with_escaped_quote() {
    // A backslash-escaped quote doesn't end the literal early.
    std::vector<scpp::Token> tokens = scpp::tokenize("\"a\\\"b\" 1");
    expect(tokens.size() == 3, "string_literal_with_escaped_quote: expected 3 tokens");
    expect(tokens[0].kind == scpp::TokenKind::StringLiteral,
           "string_literal_with_escaped_quote: kind should be StringLiteral");
    expect(tokens[0].text == "\"a\\\"b\"",
           "string_literal_with_escaped_quote: text should include the escaped quote");
    expect(tokens[1].kind == scpp::TokenKind::IntegerLiteral,
           "string_literal_with_escaped_quote: next token should be the '1' after the literal ends");
}

void test_ellipsis() {
    // `...` (ch02 §2.1's variadic parameter marker) is one Ellipsis
    // token, not three separate Dot tokens.
    expect_kinds(
        "(int a, ...)",
        {
            scpp::TokenKind::LParen,
            scpp::TokenKind::KwInt,
            scpp::TokenKind::Identifier,
            scpp::TokenKind::Comma,
            scpp::TokenKind::Ellipsis,
            scpp::TokenKind::RParen,
            scpp::TokenKind::EndOfFile,
        },
        "ellipsis");
}

void test_char_keyword() {
    expect_kinds("char", {scpp::TokenKind::KwChar, scpp::TokenKind::EndOfFile}, "char_keyword");
}

void test_char_literal() {
    std::vector<scpp::Token> tokens = scpp::tokenize("char c = 'a';");
    expect(tokens.size() == 6, "char_literal: expected 6 tokens");
    expect(tokens[3].kind == scpp::TokenKind::CharLiteral, "char_literal: kind should be CharLiteral");
    expect(tokens[3].text == "'a'", "char_literal: text should be \"'a'\" (quotes included)");
}

void test_char_literal_escape_sequence() {
    // A backslash-escaped character (e.g. the newline escape `\n`)
    // doesn't end the literal early -- same mechanism as string literals.
    std::vector<scpp::Token> tokens = scpp::tokenize("'\\n'");
    expect(tokens.size() == 2, "char_literal_escape_sequence: expected 2 tokens");
    expect(tokens[0].kind == scpp::TokenKind::CharLiteral,
           "char_literal_escape_sequence: kind should be CharLiteral");
    expect(tokens[0].text == "'\\n'", "char_literal_escape_sequence: text should include the escape");
}

// ch11 §11.3/§11.4/§11.7: module/export/import/namespace keywords. `as`
// is not a reserved word (the `import name as local_name;` aliasing
// syntax it once supported was removed, ch11 -- see "Remove import ...
// as ... aliasing from ch11" commit); a bare `as` lexes as an ordinary
// Identifier, covered by test_identifier, not here.
void test_module_keywords() {
    expect_kinds(
        "module export import namespace",
        {
            scpp::TokenKind::KwModule,
            scpp::TokenKind::KwExport,
            scpp::TokenKind::KwImport,
            scpp::TokenKind::KwNamespace,
            scpp::TokenKind::EndOfFile,
        },
        "module_keywords");
}

// A dotted module name (`std.core`) is just an ordinary Identifier/Dot/
// Identifier sequence -- no new lexer token needed, reusing the same Dot
// token member-access already uses.
void test_dotted_module_name() {
    expect_kinds(
        "import std.core;",
        {
            scpp::TokenKind::KwImport,
            scpp::TokenKind::Identifier,
            scpp::TokenKind::Dot,
            scpp::TokenKind::Identifier,
            scpp::TokenKind::Semicolon,
            scpp::TokenKind::EndOfFile,
        },
        "dotted_module_name");
}

// ch05 §5.11: generic functions/concepts keywords.
void test_concept_keywords() {
    expect_kinds(
        "template typename concept requires auto",
        {
            scpp::TokenKind::KwTemplate,
            scpp::TokenKind::KwTypename,
            scpp::TokenKind::KwConcept,
            scpp::TokenKind::KwRequires,
            scpp::TokenKind::KwAuto,
            scpp::TokenKind::EndOfFile,
        },
        "concept_keywords");
}

// ch05 §5.12: `mutable` for a lambda's own operator().
void test_constexpr_and_consteval_keywords() {
    expect_kinds(
        "constexpr consteval",
        {
            scpp::TokenKind::KwConstexpr,
            scpp::TokenKind::KwConsteval,
            scpp::TokenKind::EndOfFile,
        },
        "constexpr_and_consteval_keywords");
}

void test_mutable_keyword() {
    expect_kinds(
        "mutable",
        {
            scpp::TokenKind::KwMutable,
            scpp::TokenKind::EndOfFile,
        },
        "mutable_keyword");
}

} // namespace

int main() {
    test_empty_source();
    test_int_main_return();
    test_keywords();
    test_safe_is_no_longer_a_keyword();
    test_static_is_a_keyword();
    test_inline_is_a_keyword();
    test_fixed_width_integer_keywords();
    test_unsafe_is_not_a_keyword();
    test_identifier_text();
    test_integer_literal_text();
    test_operators();
    test_arrow_operator();
    test_comments_are_skipped();
    test_line_and_column_tracking();
    test_eof_position_ignores_trailing_whitespace();
    test_unknown_character();
    test_struct_punctuation();
    test_scope_resolution_operator();
    test_extern_and_void_keywords();
    test_string_literal();
    test_string_literal_with_escaped_quote();
    test_ellipsis();
    test_char_keyword();
    test_char_literal();
    test_char_literal_escape_sequence();
    test_module_keywords();
    test_dotted_module_name();
    test_concept_keywords();
    test_constexpr_and_consteval_keywords();
    test_mutable_keyword();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All lexer tests passed.\n";
    return 0;
}
