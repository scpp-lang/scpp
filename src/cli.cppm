module;

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module scpp.cli;

import scpp.lexer;
import scpp.parser;
import scpp.ast;
import scpp.codegen;
import scpp.movecheck;
import scpp.driver;

namespace {

std::string_view token_kind_name(scpp::TokenKind kind) {
    switch (kind) {
        case scpp::TokenKind::Identifier: return "Identifier";
        case scpp::TokenKind::IntegerLiteral: return "IntegerLiteral";
        case scpp::TokenKind::CharLiteral: return "CharLiteral";
        case scpp::TokenKind::StringLiteral: return "StringLiteral";
        case scpp::TokenKind::KwInt: return "KwInt";
        case scpp::TokenKind::KwBool: return "KwBool";
        case scpp::TokenKind::KwChar: return "KwChar";
        case scpp::TokenKind::KwVoid: return "KwVoid";
        case scpp::TokenKind::KwReturn: return "KwReturn";
        case scpp::TokenKind::KwIf: return "KwIf";
        case scpp::TokenKind::KwElse: return "KwElse";
        case scpp::TokenKind::KwWhile: return "KwWhile";
        case scpp::TokenKind::KwFor: return "KwFor";
        case scpp::TokenKind::KwUnsafe: return "KwUnsafe";
        case scpp::TokenKind::KwExtern: return "KwExtern";
        case scpp::TokenKind::KwTrue: return "KwTrue";
        case scpp::TokenKind::KwFalse: return "KwFalse";
        case scpp::TokenKind::KwStruct: return "KwStruct";
        case scpp::TokenKind::KwConst: return "KwConst";
        case scpp::TokenKind::KwClass: return "KwClass";
        case scpp::TokenKind::KwPublic: return "KwPublic";
        case scpp::TokenKind::KwPrivate: return "KwPrivate";
        case scpp::TokenKind::KwThis: return "KwThis";
        case scpp::TokenKind::KwModule: return "KwModule";
        case scpp::TokenKind::KwExport: return "KwExport";
        case scpp::TokenKind::KwImport: return "KwImport";
        case scpp::TokenKind::KwNamespace: return "KwNamespace";
        case scpp::TokenKind::KwAs: return "KwAs";
        case scpp::TokenKind::LParen: return "LParen";
        case scpp::TokenKind::RParen: return "RParen";
        case scpp::TokenKind::LBrace: return "LBrace";
        case scpp::TokenKind::RBrace: return "RBrace";
        case scpp::TokenKind::LBracket: return "LBracket";
        case scpp::TokenKind::RBracket: return "RBracket";
        case scpp::TokenKind::Semicolon: return "Semicolon";
        case scpp::TokenKind::Comma: return "Comma";
        case scpp::TokenKind::Dot: return "Dot";
        case scpp::TokenKind::Ellipsis: return "Ellipsis";
        case scpp::TokenKind::ColonColon: return "ColonColon";
        case scpp::TokenKind::Colon: return "Colon";
        case scpp::TokenKind::Arrow: return "Arrow";
        case scpp::TokenKind::Tilde: return "Tilde";
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
        case scpp::TokenKind::Amp: return "Amp";
        case scpp::TokenKind::PipePipe: return "PipePipe";
        case scpp::TokenKind::Bang: return "Bang";
        case scpp::TokenKind::EndOfFile: return "EndOfFile";
        case scpp::TokenKind::Unknown: return "Unknown";
    }
    return "?";
}

std::string type_to_string(const scpp::Type& type) {
    switch (type.kind) {
        case scpp::TypeKind::Named:
            return type.name;
        case scpp::TypeKind::Pointer:
            return (type.is_mutable_pointee ? std::string() : std::string("const ")) + type_to_string(*type.pointee) +
                   "*";
        case scpp::TypeKind::Array:
            return type_to_string(*type.element) + "[" + std::to_string(type.array_size) + "]";
        case scpp::TypeKind::UniquePtr:
            return "std::unique_ptr<" + type_to_string(*type.pointee) + ">";
        case scpp::TypeKind::Reference:
            return (type.is_mutable_ref ? std::string() : std::string("const ")) + type_to_string(*type.pointee) +
                   "&";
        case scpp::TypeKind::Span:
            return "std::span<" + (type.is_mutable_ref ? std::string() : std::string("const ")) +
                   type_to_string(*type.pointee) + ">";
    }
    return "?";
}

std::string read_file(std::string_view path) {
    std::ifstream file{std::string(path)};
    if (!file) {
        throw std::runtime_error("cannot open file '" + std::string(path) + "'");
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Renders a Clang/GCC-style diagnostic: "path:line:col: error: message",
// followed by the offending source line and a caret pointing at the
// exact column, e.g.:
//
//   foo.cpp:3:9: error: use of undeclared identifier 'foo'
//     3 |     int x = foo;
//       |             ^
//
// Falls back to a bare "path: error: message" (no line/column, no source
// excerpt) when `loc` is unknown ({0,0} -- see SourceLocation) since
// there's nothing to point at; a DriverError (a linker failure) always
// takes this path, as would any diagnostic from a code path that, for
// whatever reason, couldn't determine a location.
void print_diagnostic(std::string_view path, const std::string& source, scpp::SourceLocation loc,
                       const std::string& message) {
    std::cerr << path << ":";
    if (loc.is_known()) std::cerr << loc.line << ":" << loc.column << ":";
    std::cerr << " error: " << message << "\n";
    if (!loc.is_known()) return;

    // Find the start of line `loc.line` (1-based) by counting newlines --
    // `source` is the exact text the lexer/parser walked, so this always
    // agrees with however Lexer::advance() itself counted lines.
    size_t line_start = 0;
    int current_line = 1;
    while (current_line < loc.line) {
        size_t next_nl = source.find('\n', line_start);
        if (next_nl == std::string::npos) return; // loc.line is past EOF -- defensive, shouldn't happen
        line_start = next_nl + 1;
        current_line++;
    }
    size_t line_end = source.find('\n', line_start);
    if (line_end == std::string::npos) line_end = source.size();
    std::string_view line_text(source.data() + line_start, line_end - line_start);

    std::string line_num_str = std::to_string(loc.line);
    std::string gutter(line_num_str.size(), ' ');
    std::cerr << " " << line_num_str << " | " << line_text << "\n";
    std::cerr << " " << gutter << " | ";
    // loc.column is 1-based; print (column - 1) characters of leading
    // context before the caret, preserving tabs so the caret still lines
    // up correctly in a terminal that expands them (a plain space
    // wouldn't match a tab's actual rendered width).
    for (int i = 0; i < loc.column - 1 && static_cast<size_t>(i) < line_text.size(); i++) {
        std::cerr << (line_text[static_cast<size_t>(i)] == '\t' ? '\t' : ' ');
    }
    std::cerr << "^\n";
}

int run_lex(std::string_view path) {
    std::string source;
    try {
        source = read_file(path);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    for (const scpp::Token& tok : scpp::tokenize(source)) {
        std::cout << tok.line << ":" << tok.column << "\t" << token_kind_name(tok.kind);
        if (tok.kind != scpp::TokenKind::EndOfFile) {
            std::cout << "\t'" << tok.text << "'";
        }
        std::cout << "\n";
    }
    return 0;
}

std::string_view binary_op_name(scpp::BinaryOp op) {
    switch (op) {
        case scpp::BinaryOp::Add: return "+";
        case scpp::BinaryOp::Sub: return "-";
        case scpp::BinaryOp::Mul: return "*";
        case scpp::BinaryOp::Div: return "/";
        case scpp::BinaryOp::Eq: return "==";
        case scpp::BinaryOp::Ne: return "!=";
        case scpp::BinaryOp::Lt: return "<";
        case scpp::BinaryOp::Gt: return ">";
        case scpp::BinaryOp::Le: return "<=";
        case scpp::BinaryOp::Ge: return ">=";
        case scpp::BinaryOp::And: return "&&";
        case scpp::BinaryOp::Or: return "||";
        case scpp::BinaryOp::Assign: return "=";
    }
    return "?";
}

std::string_view unary_op_name(scpp::UnaryOp op) {
    switch (op) {
        case scpp::UnaryOp::Neg: return "-";
        case scpp::UnaryOp::Not: return "!";
        case scpp::UnaryOp::Deref: return "*";
        case scpp::UnaryOp::AddressOf: return "&";
    }
    return "?";
}

void print_indent(int depth) {
    for (int i = 0; i < depth; i++) std::cout << "  ";
}

void print_expr(const scpp::Expr& expr, int depth) {
    print_indent(depth);
    switch (expr.kind) {
        case scpp::ExprKind::IntegerLiteral:
            std::cout << "IntegerLiteral " << expr.int_value << "\n";
            break;
        case scpp::ExprKind::BoolLiteral:
            std::cout << "BoolLiteral " << (expr.bool_value ? "true" : "false") << "\n";
            break;
        case scpp::ExprKind::CharLiteral:
            std::cout << "CharLiteral " << expr.int_value << "\n";
            break;
        case scpp::ExprKind::StringLiteral:
            std::cout << "StringLiteral " << expr.name << "\n";
            break;
        case scpp::ExprKind::Identifier:
            std::cout << "Identifier " << expr.name << "\n";
            break;
        case scpp::ExprKind::Binary:
            std::cout << "Binary " << binary_op_name(expr.binary_op) << "\n";
            print_expr(*expr.lhs, depth + 1);
            print_expr(*expr.rhs, depth + 1);
            break;
        case scpp::ExprKind::Unary:
            std::cout << "Unary " << unary_op_name(expr.unary_op) << "\n";
            print_expr(*expr.lhs, depth + 1);
            break;
        case scpp::ExprKind::Call:
            std::cout << "Call " << expr.name << "\n";
            if (expr.lhs) {
                // Method call receiver (ch05 §5.9) -- printed under its
                // own label so it isn't mistaken for an ordinary argument.
                print_indent(depth + 1);
                std::cout << "Receiver\n";
                print_expr(*expr.lhs, depth + 2);
            }
            for (const auto& arg : expr.args) print_expr(*arg, depth + 1);
            break;
        case scpp::ExprKind::Member:
            std::cout << "Member ." << expr.name << "\n";
            print_expr(*expr.lhs, depth + 1);
            break;
        case scpp::ExprKind::Subscript:
            std::cout << "Subscript\n";
            print_expr(*expr.lhs, depth + 1);
            print_expr(*expr.rhs, depth + 1);
            break;
        case scpp::ExprKind::Move:
            std::cout << "Move\n";
            print_expr(*expr.lhs, depth + 1);
            break;
        case scpp::ExprKind::MakeUnique:
            std::cout << "MakeUnique " << type_to_string(expr.type) << "\n";
            for (const auto& arg : expr.args) print_expr(*arg, depth + 1);
            break;
    }
}

void print_stmt(const scpp::Stmt& stmt, int depth) {
    print_indent(depth);
    switch (stmt.kind) {
        case scpp::StmtKind::VarDecl:
            std::cout << "VarDecl " << type_to_string(stmt.type) << " " << stmt.var_name << "\n";
            if (stmt.init) print_expr(*stmt.init, depth + 1);
            if (stmt.has_ctor_args) {
                // `ClassName name(args);` (ch04 §4.2) -- printed under
                // its own label, same reasoning as Call's Receiver above.
                print_indent(depth + 1);
                std::cout << "CtorArgs\n";
                for (const auto& arg : stmt.ctor_args) print_expr(*arg, depth + 2);
            }
            break;
        case scpp::StmtKind::Return:
            std::cout << "Return\n";
            if (stmt.expr) print_expr(*stmt.expr, depth + 1);
            break;
        case scpp::StmtKind::If:
            std::cout << "If\n";
            print_expr(*stmt.condition, depth + 1);
            print_stmt(*stmt.then_branch, depth + 1);
            if (stmt.else_branch) print_stmt(*stmt.else_branch, depth + 1);
            break;
        case scpp::StmtKind::While:
            std::cout << "While\n";
            print_expr(*stmt.condition, depth + 1);
            print_stmt(*stmt.then_branch, depth + 1);
            break;
        case scpp::StmtKind::ExprStmt:
            std::cout << "ExprStmt\n";
            print_expr(*stmt.expr, depth + 1);
            break;
        case scpp::StmtKind::Block:
            std::cout << (stmt.is_unsafe ? "Block (unsafe)\n" : "Block\n");
            for (const auto& s : stmt.statements) print_stmt(*s, depth + 1);
            break;
    }
}

int run_parse(std::string_view path) {
    std::string source;
    try {
        source = read_file(path);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    try {
        scpp::Program program = scpp::parse(source);
        for (const scpp::StructDef& def : program.structs) {
            std::cout << "Struct " << def.name << "\n";
            for (const scpp::StructField& field : def.fields) {
                print_indent(1);
                std::cout << "Field " << type_to_string(field.type) << " " << field.name << "\n";
            }
        }
        for (const scpp::Function& fn : program.functions) {
            std::cout << "Function " << (fn.is_extern_c ? "extern \"C\" " : "")
                       << type_to_string(fn.return_type) << " " << fn.name << "(";
            for (size_t i = 0; i < fn.params.size(); i++) {
                if (i > 0) std::cout << ", ";
                std::cout << type_to_string(fn.params[i].type) << " " << fn.params[i].name;
            }
            if (fn.has_varargs) {
                std::cout << (fn.params.empty() ? "..." : ", ...");
            }
            std::cout << ")\n";
            if (fn.body) {
                print_stmt(*fn.body, 1);
            } else {
                print_indent(1);
                std::cout << "(no body -- external declaration)\n";
            }
        }
    } catch (const scpp::ParseError& e) {
        print_diagnostic(path, source, e.loc, e.what());
        return 1;
    }
    return 0;
}

int run_build(std::string_view input_path, std::string_view output_path,
              const std::vector<std::string>& extra_link_inputs,
              const std::unordered_map<std::string, std::string>& import_paths) {
    std::string source;
    try {
        source = read_file(input_path);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    try {
        scpp::compile_to_executable(source, std::string(output_path), extra_link_inputs, import_paths);
    } catch (const scpp::ParseError& e) {
        print_diagnostic(input_path, source, e.loc, e.what());
        return 1;
    } catch (const scpp::DataflowError& e) {
        print_diagnostic(input_path, source, e.loc, e.what());
        return 1;
    } catch (const scpp::CodegenError& e) {
        print_diagnostic(input_path, source, e.loc, e.what());
        return 1;
    } catch (const scpp::DriverError& e) {
        print_diagnostic(input_path, source, scpp::SourceLocation{}, e.what());
        return 1;
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
    if (argc >= 3 && std::string_view(argv[1]) == "parse") {
        return run_parse(argv[2]);
    }
    if (argc >= 3 && std::string_view(argv[1]) == "build") {
        std::string_view output_path = "a.out";
        std::vector<std::string> extra_link_inputs;
        std::unordered_map<std::string, std::string> import_paths;
        for (int i = 3; i < argc; i++) {
            std::string_view arg = argv[i];
            if (arg == "-o" && i + 1 < argc) {
                output_path = argv[++i];
            } else if (arg == "--link" && i + 1 < argc) {
                extra_link_inputs.emplace_back(argv[++i]);
            } else if (arg == "--import" && i + 1 < argc) {
                // ch11 §11.7/§11.13: `--import name=path` (repeatable),
                // mirroring Clang's `-fmodule-file=name=path` and Rust's
                // `--extern name=path` -- explicit and unambiguous, the
                // only import-resolution mechanism this version supports
                // (no `.scppm`/`-I` search path yet).
                std::string_view mapping = argv[++i];
                size_t eq = mapping.find('=');
                if (eq == std::string_view::npos) {
                    std::cerr << "error: --import expects 'name=path', got '" << mapping << "'\n";
                    return 1;
                }
                import_paths.emplace(std::string(mapping.substr(0, eq)), std::string(mapping.substr(eq + 1)));
            }
        }
        return run_build(argv[2], output_path, extra_link_inputs, import_paths);
    }

    std::string_view name = argc > 0 ? argv[0] : "scpp";
    std::cout << "Hello from " << name << " " << version << "!\n";
    std::cout << "Usage: " << name << " lex <file>\n";
    std::cout << "       " << name << " parse <file>\n";
    std::cout << "       " << name << " build <file> [-o <output>] [--link <path>]... [--import name=path]...\n";
    return 0;
}

} // namespace scpp
