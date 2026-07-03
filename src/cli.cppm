module;

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

export module scpp.cli;

import scpp.lexer;

namespace {

std::string_view token_kind_name(scpp::TokenKind kind) {
    switch (kind) {
        case scpp::TokenKind::Identifier: return "Identifier";
        case scpp::TokenKind::IntegerLiteral: return "IntegerLiteral";
        case scpp::TokenKind::KwInt: return "KwInt";
        case scpp::TokenKind::KwBool: return "KwBool";
        case scpp::TokenKind::KwReturn: return "KwReturn";
        case scpp::TokenKind::KwIf: return "KwIf";
        case scpp::TokenKind::KwElse: return "KwElse";
        case scpp::TokenKind::KwWhile: return "KwWhile";
        case scpp::TokenKind::KwFor: return "KwFor";
        case scpp::TokenKind::KwSafe: return "KwSafe";
        case scpp::TokenKind::KwUnsafe: return "KwUnsafe";
        case scpp::TokenKind::KwTrue: return "KwTrue";
        case scpp::TokenKind::KwFalse: return "KwFalse";
        case scpp::TokenKind::LParen: return "LParen";
        case scpp::TokenKind::RParen: return "RParen";
        case scpp::TokenKind::LBrace: return "LBrace";
        case scpp::TokenKind::RBrace: return "RBrace";
        case scpp::TokenKind::Semicolon: return "Semicolon";
        case scpp::TokenKind::Comma: return "Comma";
        case scpp::TokenKind::Plus: return "Plus";
        case scpp::TokenKind::Minus: return "Minus";
        case scpp::TokenKind::Star: return "Star";
        case scpp::TokenKind::Slash: return "Slash";
        case scpp::TokenKind::Assign: return "Assign";
        case scpp::TokenKind::EqualEqual: return "EqualEqual";
        case scpp::TokenKind::NotEqual: return "NotEqual";
        case scpp::TokenKind::Less: return "Less";
        case scpp::TokenKind::Greater: return "Greater";
        case scpp::TokenKind::LessEqual: return "LessEqual";
        case scpp::TokenKind::GreaterEqual: return "GreaterEqual";
        case scpp::TokenKind::AmpAmp: return "AmpAmp";
        case scpp::TokenKind::PipePipe: return "PipePipe";
        case scpp::TokenKind::Bang: return "Bang";
        case scpp::TokenKind::EndOfFile: return "EndOfFile";
        case scpp::TokenKind::Unknown: return "Unknown";
    }
    return "?";
}

int run_lex(std::string_view path) {
    std::ifstream file{std::string(path)};
    if (!file) {
        std::cerr << "error: cannot open file '" << path << "'\n";
        return 1;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    for (const scpp::Token& tok : scpp::tokenize(source)) {
        std::cout << tok.line << ":" << tok.column << "\t" << token_kind_name(tok.kind);
        if (tok.kind != scpp::TokenKind::EndOfFile) {
            std::cout << "\t'" << tok.text << "'";
        }
        std::cout << "\n";
    }
    return 0;
}

} // namespace

export namespace scpp {

constexpr std::string_view version = "0.1.0";

int run(int argc, char** argv) {
    if (argc >= 3 && std::string_view(argv[1]) == "lex") {
        return run_lex(argv[2]);
    }

    std::string_view name = argc > 0 ? argv[0] : "scpp";
    std::cout << "Hello from " << name << " " << version << "!\n";
    std::cout << "Usage: " << name << " lex <file>\n";
    return 0;
}

} // namespace scpp
