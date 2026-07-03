import scpp.lexer;

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

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
    for (size_t i = 0; i < tokens.size() && i < expected.size(); i++) {
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
        "safe unsafe bool if else while for true false struct",
        {
            scpp::TokenKind::KwSafe,
            scpp::TokenKind::KwUnsafe,
            scpp::TokenKind::KwBool,
            scpp::TokenKind::KwIf,
            scpp::TokenKind::KwElse,
            scpp::TokenKind::KwWhile,
            scpp::TokenKind::KwFor,
            scpp::TokenKind::KwTrue,
            scpp::TokenKind::KwFalse,
            scpp::TokenKind::KwStruct,
            scpp::TokenKind::EndOfFile,
        },
        "keywords");
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
        "+ - * / = == != < > <= >= && || !",
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
            scpp::TokenKind::EndOfFile,
        },
        "operators");
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

} // namespace

int main() {
    test_empty_source();
    test_int_main_return();
    test_keywords();
    test_identifier_text();
    test_integer_literal_text();
    test_operators();
    test_comments_are_skipped();
    test_line_and_column_tracking();
    test_unknown_character();
    test_struct_punctuation();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All lexer tests passed.\n";
    return 0;
}
