module;

#include <filesystem>
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
import scpp.project;

namespace {

std::string_view token_kind_name(scpp::TokenKind kind) {
    switch (kind) {
        case scpp::TokenKind::Identifier: return "Identifier";
        case scpp::TokenKind::IntegerLiteral: return "IntegerLiteral";
        case scpp::TokenKind::FloatLiteral: return "FloatLiteral";
        case scpp::TokenKind::CharLiteral: return "CharLiteral";
        case scpp::TokenKind::StringLiteral: return "StringLiteral";
        case scpp::TokenKind::KwInt: return "KwInt";
        case scpp::TokenKind::KwBool: return "KwBool";
        case scpp::TokenKind::KwChar: return "KwChar";
        case scpp::TokenKind::KwLong: return "KwLong";
        case scpp::TokenKind::KwFloat: return "KwFloat";
        case scpp::TokenKind::KwDouble: return "KwDouble";
        case scpp::TokenKind::KwUnsigned: return "KwUnsigned";
        case scpp::TokenKind::KwVoid: return "KwVoid";
        case scpp::TokenKind::KwReturn: return "KwReturn";
        case scpp::TokenKind::KwIf: return "KwIf";
        case scpp::TokenKind::KwElse: return "KwElse";
        case scpp::TokenKind::KwWhile: return "KwWhile";
        case scpp::TokenKind::KwBreak: return "KwBreak";
        case scpp::TokenKind::KwContinue: return "KwContinue";
        case scpp::TokenKind::KwFor: return "KwFor";
        case scpp::TokenKind::KwExtern: return "KwExtern";
        case scpp::TokenKind::KwTrue: return "KwTrue";
        case scpp::TokenKind::KwFalse: return "KwFalse";
        case scpp::TokenKind::KwStruct: return "KwStruct";
        case scpp::TokenKind::KwUnion: return "KwUnion";
        case scpp::TokenKind::KwConst: return "KwConst";
        case scpp::TokenKind::KwNew: return "KwNew";
        case scpp::TokenKind::KwDelete: return "KwDelete";
        case scpp::TokenKind::KwClass: return "KwClass";
        case scpp::TokenKind::KwPublic: return "KwPublic";
        case scpp::TokenKind::KwPrivate: return "KwPrivate";
        case scpp::TokenKind::KwThis: return "KwThis";
        case scpp::TokenKind::KwModule: return "KwModule";
        case scpp::TokenKind::KwExport: return "KwExport";
        case scpp::TokenKind::KwImport: return "KwImport";
        case scpp::TokenKind::KwNamespace: return "KwNamespace";
        case scpp::TokenKind::KwTemplate: return "KwTemplate";
        case scpp::TokenKind::KwTypename: return "KwTypename";
        case scpp::TokenKind::KwConcept: return "KwConcept";
        case scpp::TokenKind::KwRequires: return "KwRequires";
        case scpp::TokenKind::KwAuto: return "KwAuto";
        case scpp::TokenKind::KwMutable: return "KwMutable";
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
        case scpp::TokenKind::Question: return "Question";
        case scpp::TokenKind::EndOfFile: return "EndOfFile";
        case scpp::TokenKind::Unknown: return "Unknown";
    }
    return "?";
}

std::string type_to_string(const scpp::Type& type);

std::string type_to_string(const scpp::Type& type) {
    switch (type.kind) {
        case scpp::TypeKind::Named: {
            if (type.template_args.empty()) return type.name;
            std::string result = type.name + "<";
            for (size_t i = 0; i < type.template_args.size(); i++) {
                if (i > 0) result += ", ";
                result += type_to_string(type.template_args[i]);
            }
            result += ">";
            return result;
        }
        case scpp::TypeKind::Pointer:
            return (type.is_mutable_pointee ? std::string() : std::string("const ")) + type_to_string(*type.pointee) +
                   "*";
        case scpp::TypeKind::Function: {
            std::string result = type_to_string(*type.function_return) + "(";
            for (size_t i = 0; i < type.function_params.size(); i++) {
                if (i > 0) result += ", ";
                result += type_to_string(type.function_params[i]);
            }
            result += ")";
            if (type.is_const_function) result += " const";
            if (type.function_ref_qualifier == scpp::ReceiverRefQualifier::LValue) result += " &";
            if (type.function_ref_qualifier == scpp::ReceiverRefQualifier::RValue) result += " &&";
            return result;
        }
        case scpp::TypeKind::FunctionPointer: {
            std::string result = type_to_string(*type.function_return) + " (*";
            if (type.is_unsafe_function_pointer) result += " [[scpp::unsafe]]";
            result += ")(";
            for (size_t i = 0; i < type.function_params.size(); i++) {
                if (i > 0) result += ", ";
                result += type_to_string(type.function_params[i]);
            }
            result += ")";
            return result;
        }
        case scpp::TypeKind::Array:
            return type_to_string(*type.element) + "[" + std::to_string(type.array_size) + "]";
        case scpp::TypeKind::Reference:
            if (type.is_rvalue_ref) return type_to_string(*type.pointee) + "&&";
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

bool require_scpp_input_path(std::string_view path, std::string_view role) {
    if (std::filesystem::path(std::string(path)).extension() == ".scpp") return true;
    std::cerr << "error: " << role << " must use the .scpp extension, got '" << path << "'\n";
    return false;
}

// Renders a Clang/GCC-style diagnostic: "path:line:col: error: message",
// followed by the offending source line and a caret pointing at the
// exact column, e.g.:
//
//   foo.scpp:3:9: error: use of undeclared identifier 'foo'
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
    if (!require_scpp_input_path(path, "input file")) return 1;
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

// Forward-declared: print_expr's own Lambda case (ch05 §5.12) needs to
// print the closure's body, which is a Stmt -- print_stmt itself is
// defined below (after print_expr), and also calls print_expr for
// ordinary sub-expressions, so the two are mutually recursive.
void print_stmt(const scpp::Stmt& stmt, int depth);

void print_expr(const scpp::Expr& expr, int depth) {
    print_indent(depth);
    switch (expr.kind) {
        case scpp::ExprKind::IntegerLiteral:
            std::cout << "IntegerLiteral " << expr.int_value << "\n";
            break;
        case scpp::ExprKind::FloatLiteral:
            std::cout << "FloatLiteral " << expr.float_value << "\n";
            break;
        case scpp::ExprKind::BoolLiteral:
            std::cout << "BoolLiteral " << (expr.bool_value ? "true" : "false") << "\n";
            break;
        case scpp::ExprKind::TypeTrait:
            std::cout << "TypeTrait " << expr.name << "\n";
            print_indent(depth + 1);
            std::cout << "Type " << expr.type.name << "\n";
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
        case scpp::ExprKind::Conditional:
            std::cout << "Conditional\n";
            print_expr(*expr.lhs, depth + 1);
            print_expr(*expr.rhs, depth + 1);
            print_expr(*expr.third, depth + 1);
            break;
        case scpp::ExprKind::Fold:
            std::cout << "Fold " << binary_op_name(expr.binary_op)
                      << (expr.fold_ellipsis_on_left ? " (left)" : " (right)") << "\n";
            print_expr(*expr.lhs, depth + 1);
            if (expr.rhs) print_expr(*expr.rhs, depth + 1);
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
        case scpp::ExprKind::Cast:
            std::cout << "Cast " << type_to_string(expr.type) << "\n";
            print_expr(*expr.lhs, depth + 1);
            break;
        case scpp::ExprKind::New:
            std::cout << "New " << type_to_string(expr.type) << "\n";
            for (const auto& arg : expr.args) print_expr(*arg, depth + 1);
            break;
        case scpp::ExprKind::Delete:
            std::cout << "Delete\n";
            print_expr(*expr.lhs, depth + 1);
            break;
        case scpp::ExprKind::PackExpansion:
            std::cout << "PackExpansion\n";
            print_expr(*expr.lhs, depth + 1);
            break;
        case scpp::ExprKind::Lambda: {
            std::cout << "Lambda";
            if (!expr.name.empty()) std::cout << " -> " << expr.name;
            std::cout << "\n";
            print_indent(depth + 1);
            std::cout << "Captures";
            switch (expr.lambda_blanket_mode) {
                case scpp::LambdaCaptureMode::ByValue: std::cout << " (blanket =)"; break;
                case scpp::LambdaCaptureMode::ByReference: std::cout << " (blanket &)"; break;
                case scpp::LambdaCaptureMode::None: break;
            }
            std::cout << "\n";
            for (const auto& capture : expr.lambda_captures) {
                print_indent(depth + 2);
                std::cout << (capture.by_reference ? "&" : "") << capture.name;
                if (capture.init) std::cout << " = <init-expr>";
                std::cout << "\n";
            }
            print_indent(depth + 1);
            std::cout << "Params\n";
            for (const auto& param : expr.lambda_params) {
                print_indent(depth + 2);
                std::cout << type_to_string(param.type) << " " << param.name << "\n";
            }
            if (expr.lambda_is_mutable) {
                print_indent(depth + 1);
                std::cout << "mutable\n";
            }
            if (expr.has_lambda_explicit_return_type) {
                print_indent(depth + 1);
                std::cout << "-> " << type_to_string(expr.type) << "\n";
            }
            if (expr.lambda_body) print_stmt(*expr.lambda_body, depth + 1);
            break;
        }
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
        case scpp::StmtKind::Break:
            std::cout << "Break\n";
            break;
        case scpp::StmtKind::Continue:
            std::cout << "Continue\n";
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
    if (!require_scpp_input_path(path, "input file")) return 1;
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
              const std::unordered_map<std::string, std::string>& import_paths,
              const std::vector<std::string>& import_search_dirs, bool static_link, bool emit_debug_info) {
    if (!require_scpp_input_path(input_path, "input file")) return 1;
    for (const auto& [module_name, path] : import_paths) {
        std::string extension = std::filesystem::path(path).extension().string();
        if (extension != ".scpp" && extension != ".scppm") {
            std::cerr << "error: import path for module '" << module_name
                      << "' must use the .scpp or .scppm extension, got '" << path << "'\n";
            return 1;
        }
    }
    std::string source;
    try {
        source = read_file(input_path);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    try {
        scpp::compile_to_executable(source, std::string(output_path), extra_link_inputs, import_paths, static_link,
                                    import_search_dirs, emit_debug_info, std::string(input_path));
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

int run_build_module(std::string_view input_path, std::string_view interface_path, std::string_view archive_path,
                     const std::unordered_map<std::string, std::string>& import_paths,
                     const std::vector<std::string>& import_search_dirs) {
    if (!require_scpp_input_path(input_path, "input file")) return 1;
    if (std::filesystem::path(std::string(interface_path)).extension() != ".scppm") {
        std::cerr << "error: module interface output must use the .scppm extension, got '" << interface_path << "'\n";
        return 1;
    }
    if (std::filesystem::path(std::string(archive_path)).extension() != ".scppa") {
        std::cerr << "error: module archive output must use the .scppa extension, got '" << archive_path << "'\n";
        return 1;
    }
    for (const auto& [module_name, path] : import_paths) {
        std::string extension = std::filesystem::path(path).extension().string();
        if (extension != ".scpp" && extension != ".scppm") {
            std::cerr << "error: import path for module '" << module_name
                      << "' must use the .scpp or .scppm extension, got '" << path << "'\n";
            return 1;
        }
    }
    std::string source;
    try {
        source = read_file(input_path);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    try {
        scpp::emit_module_artifacts(source, std::string(interface_path), std::string(archive_path), import_paths,
                                    import_search_dirs, std::string(input_path));
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
    std::string_view name = argc > 0 ? argv[0] : "scpp";
    if (argc >= 2 && std::string_view(argv[1]) == "lex") {
        if (argc != 3) {
            std::cerr << "error: lex requires <file.scpp>\n";
            return 1;
        }
        return run_lex(argv[2]);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "parse") {
        if (argc != 3) {
            std::cerr << "error: parse requires <file.scpp>\n";
            return 1;
        }
        return run_parse(argv[2]);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "build-module") {
        if (argc < 3) {
            std::cerr << "error: build-module requires <file.scpp>\n";
            return 1;
        }
        std::string_view interface_path;
        std::string_view archive_path;
        std::unordered_map<std::string, std::string> import_paths;
        std::vector<std::string> import_search_dirs;
        for (int i = 3; i < argc; i++) {
            std::string_view arg = argv[i];
            if (arg == "-I" && i + 1 < argc) {
                import_search_dirs.emplace_back(argv[++i]);
            } else if (arg == "--interface-out" && i + 1 < argc) {
                interface_path = argv[++i];
            } else if (arg == "--archive-out" && i + 1 < argc) {
                archive_path = argv[++i];
            } else if (arg == "--import" && i + 1 < argc) {
                std::string_view mapping = argv[++i];
                size_t eq = mapping.find('=');
                if (eq == std::string_view::npos) {
                    std::cerr << "error: --import expects 'name=path', got '" << mapping << "'\n";
                    return 1;
                }
                import_paths.emplace(std::string(mapping.substr(0, eq)), std::string(mapping.substr(eq + 1)));
            } else {
                std::cerr << "error: unknown build-module option '" << arg << "'\n";
                return 1;
            }
        }
        if (interface_path.empty()) {
            std::cerr << "error: build-module requires --interface-out <file.scppm>\n";
            return 1;
        }
        if (archive_path.empty()) {
            std::cerr << "error: build-module requires --archive-out <file.scppa>\n";
            return 1;
        }
        return run_build_module(argv[2], interface_path, archive_path, import_paths, import_search_dirs);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "build") {
        scpp::ProjectBuildOptions options;
        for (int i = 2; i < argc; i++) {
            std::string_view arg = argv[i];
            if (arg == "--lib") {
                options.build_lib_only = true;
            } else if (arg == "--bin" && i + 1 < argc) {
                options.selected_bin = argv[++i];
            } else if ((arg == "-p" || arg == "--package") && i + 1 < argc) {
                options.selected_package = argv[++i];
            } else if (arg == "--profile" && i + 1 < argc) {
                options.selected_profile = argv[++i];
            } else if (arg == "--release") {
                options.release = true;
            } else if (arg == "--workspace") {
                options.build_workspace = true;
            } else {
                std::cerr << "error: unknown build option '" << arg << "'\n";
                return 1;
            }
        }
        if (options.release && options.selected_profile.has_value()) {
            std::cerr << "error: --release and --profile cannot be used together\n";
            return 1;
        }
        return scpp::build_manifest_project(std::filesystem::current_path(), options);
    }
    if (argc >= 2) {
        std::string_view output_path = "a.out";
        std::vector<std::string> extra_link_inputs;
        std::unordered_map<std::string, std::string> import_paths;
        std::vector<std::string> import_search_dirs;
        bool static_link = false;
        bool emit_debug_info = false;
        for (int i = 2; i < argc; i++) {
            std::string_view arg = argv[i];
            if (arg == "-o" && i + 1 < argc) {
                output_path = argv[++i];
            } else if (arg == "-I" && i + 1 < argc) {
                import_search_dirs.emplace_back(argv[++i]);
            } else if (arg == "-g") {
                emit_debug_info = true;
            } else if (arg == "--static") {
                static_link = true;
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
            } else {
                std::cerr << "error: unknown option '" << arg << "'\n";
                return 1;
            }
        }
        return run_build(argv[1], output_path, extra_link_inputs, import_paths, import_search_dirs, static_link,
                         emit_debug_info);
    }

    if (scpp::find_project_manifest(std::filesystem::current_path()).has_value()) {
        return scpp::build_manifest_project(std::filesystem::current_path(), scpp::ProjectBuildOptions{});
    }

    std::cout << "Hello from " << name << " " << version << "!\n";
    std::cout << "Usage: " << name << " lex <file.scpp>\n";
    std::cout << "       " << name << " parse <file.scpp>\n";
    std::cout << "       " << name
              << " <file.scpp> [-o <output>] [-I <dir>]... [-g] [--static] [--link <path>]... [--import name=path]...\n";
    std::cout << "       " << name
              << " build [--workspace] [-p <package>] [--lib] [--bin <name>] [--profile <name>] [--release]\n";
    std::cout << "       " << name
              << " build-module <file.scpp> --interface-out <file.scppm> --archive-out <file.scppa> [-I <dir>]... [--import name=path]...\n";
    return 0;
}

} // namespace scpp
