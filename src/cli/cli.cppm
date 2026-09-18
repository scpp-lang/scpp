module;

export module scpp.cli;

import std;
import scpp.ast;
import scpp.lexer;
import scpp.parser;
import scpp.driver;
import scpp.project;

extern "C" {
    int open(const char* pathname, int flags, ...);
    int close(int fd);
    long read(int fd, void* buf, unsigned long count);
    long write(int fd, const void* buf, unsigned long count);
    char* getcwd(char* buf, unsigned long size);
    int snprintf(char* str, unsigned long size, const char* format, ...);
}

namespace scpp {

export constexpr std::string_view version = "26.0.0";

inline void oprint(std::string_view s) {
    if (s.empty()) return;
    [[scpp::unsafe]] {
        write(1, s.data(), static_cast<unsigned long>(s.size()));
    }
}

inline void oprintln(std::string_view s = "") {
    oprint(s);
    char nl = '\n';
    [[scpp::unsafe]] {
        write(1, &nl, 1);
    }
}

inline void eprint(std::string_view s) {
    if (s.empty()) return;
    [[scpp::unsafe]] {
        write(2, s.data(), static_cast<unsigned long>(s.size()));
    }
}

inline void eprintln(std::string_view s = "") {
    eprint(s);
    char nl = '\n';
    [[scpp::unsafe]] {
        write(2, &nl, 1);
    }
}

inline std::string string_from_view(std::string_view sv) {
    return std::string{sv.data(), sv.size()};
}

inline bool ends_with(std::string_view str, std::string_view suffix) {
    if (str.size() < suffix.size()) return false;
    return str.substr(str.size() - suffix.size()) == suffix;
}

inline bool starts_with(std::string_view str, std::string_view prefix) {
    if (str.size() < prefix.size()) return false;
    return str.substr(0, prefix.size()) == prefix;
}

inline std::size_t find_char(std::string_view s, char c) {
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s.at(i) == c) return i;
    }
    return std::string_view::npos;
}

inline std::string path_current() {
    char buf[4096] = {};
    char* res = nullptr;
    [[scpp::unsafe]] {
        res = getcwd(buf, 4096);
    }
    if (res == nullptr) return std::string{"."};
    return std::string{buf};
}

inline const char* get_arg(char** argv, int index) {
    [[scpp::unsafe]] {
        return argv[index];
    }
}

std::string_view token_kind_name(scpp::TokenKind kind) {
    switch (kind) {
        case scpp::TokenKind::Identifier: return "Identifier";
        case scpp::TokenKind::IntegerLiteral: return "IntegerLiteral";
        case scpp::TokenKind::FloatLiteral: return "FloatLiteral";
        case scpp::TokenKind::CharLiteral: return "CharLiteral";
        case scpp::TokenKind::StringLiteral: return "StringLiteral";
        case scpp::TokenKind::KwNullptr: return "KwNullptr";
        case scpp::TokenKind::KwNullptrT: return "KwNullptrT";
        case scpp::TokenKind::KwInt: return "KwInt";
        case scpp::TokenKind::KwBool: return "KwBool";
        case scpp::TokenKind::KwChar: return "KwChar";
        case scpp::TokenKind::KwLong: return "KwLong";
        case scpp::TokenKind::KwFloat: return "KwFloat";
        case scpp::TokenKind::KwDouble: return "KwDouble";
        case scpp::TokenKind::KwUnsigned: return "KwUnsigned";
        case scpp::TokenKind::KwSizeT: return "KwSizeT";
        case scpp::TokenKind::KwPtrdiffT: return "KwPtrdiffT";
        case scpp::TokenKind::KwInt8T: return "KwInt8T";
        case scpp::TokenKind::KwUInt8T: return "KwUInt8T";
        case scpp::TokenKind::KwInt16T: return "KwInt16T";
        case scpp::TokenKind::KwUInt16T: return "KwUInt16T";
        case scpp::TokenKind::KwInt32T: return "KwInt32T";
        case scpp::TokenKind::KwUInt32T: return "KwUInt32T";
        case scpp::TokenKind::KwInt64T: return "KwInt64T";
        case scpp::TokenKind::KwUInt64T: return "KwUInt64T";
        case scpp::TokenKind::KwVoid: return "KwVoid";
        case scpp::TokenKind::KwReturn: return "KwReturn";
        case scpp::TokenKind::KwIf: return "KwIf";
        case scpp::TokenKind::KwElse: return "KwElse";
        case scpp::TokenKind::KwWhile: return "KwWhile";
        case scpp::TokenKind::KwSwitch: return "KwSwitch";
        case scpp::TokenKind::KwCase: return "KwCase";
        case scpp::TokenKind::KwBreak: return "KwBreak";
        case scpp::TokenKind::KwContinue: return "KwContinue";
        case scpp::TokenKind::KwFor: return "KwFor";
        case scpp::TokenKind::KwExtern: return "KwExtern";
        case scpp::TokenKind::KwTrue: return "KwTrue";
        case scpp::TokenKind::KwFalse: return "KwFalse";
        case scpp::TokenKind::KwEnum: return "KwEnum";
        case scpp::TokenKind::KwStruct: return "KwStruct";
        case scpp::TokenKind::KwUnion: return "KwUnion";
        case scpp::TokenKind::KwConst: return "KwConst";
        case scpp::TokenKind::KwConstexpr: return "KwConstexpr";
        case scpp::TokenKind::KwConsteval: return "KwConsteval";
        case scpp::TokenKind::KwInline: return "KwInline";
        case scpp::TokenKind::KwNew: return "KwNew";
        case scpp::TokenKind::KwDelete: return "KwDelete";
        case scpp::TokenKind::KwClass: return "KwClass";
        case scpp::TokenKind::KwStatic: return "KwStatic";
        case scpp::TokenKind::KwVirtual: return "KwVirtual";
        case scpp::TokenKind::KwOverride: return "KwOverride";
        case scpp::TokenKind::KwUsing: return "KwUsing";
        case scpp::TokenKind::KwDefault: return "KwDefault";
        case scpp::TokenKind::KwExplicit: return "KwExplicit";
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
        case scpp::TokenKind::KwAlignas: return "KwAlignas";
        case scpp::TokenKind::KwAlignof: return "KwAlignof";
        case scpp::TokenKind::KwSizeof: return "KwSizeof";
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
        case scpp::TokenKind::PlusPlus: return "PlusPlus";
        case scpp::TokenKind::MinusMinus: return "MinusMinus";
        case scpp::TokenKind::PlusAssign: return "PlusAssign";
        case scpp::TokenKind::MinusAssign: return "MinusAssign";
        case scpp::TokenKind::StarAssign: return "StarAssign";
        case scpp::TokenKind::SlashAssign: return "SlashAssign";
        case scpp::TokenKind::PercentAssign: return "PercentAssign";
        case scpp::TokenKind::AmpAssign: return "AmpAssign";
        case scpp::TokenKind::CaretAssign: return "CaretAssign";
        case scpp::TokenKind::PipeAssign: return "PipeAssign";
        case scpp::TokenKind::Plus: return "Plus";
        case scpp::TokenKind::Minus: return "Minus";
        case scpp::TokenKind::Star: return "Star";
        case scpp::TokenKind::Slash: return "Slash";
        case scpp::TokenKind::Percent: return "Percent";
        case scpp::TokenKind::Caret: return "Caret";
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
        case scpp::TokenKind::Pipe: return "Pipe";
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
            if (type.template_args.empty()) return std::string{type.name};
            std::string result = type.name + "<";
            for (std::size_t i = 0; i < type.template_args.size(); i++) {
                if (i > 0) result += ", ";
                result += type_to_string(type.template_args.at(i));
            }
            result += ">";
            return result;
        }
        case scpp::TypeKind::Pointer:
            return (type.is_mutable_pointee ? std::string{} : std::string{"const "}) + type_to_string(*type.pointee) +
                   "*";
        case scpp::TypeKind::Function: {
            std::string result = type_to_string(*type.function_return) + "(";
            for (std::size_t i = 0; i < type.function_params.size(); i++) {
                if (i > 0) result += ", ";
                result += type_to_string(type.function_params.at(i));
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
            for (std::size_t i = 0; i < type.function_params.size(); i++) {
                if (i > 0) result += ", ";
                result += type_to_string(type.function_params.at(i));
            }
            result += ")";
            return result;
        }
        case scpp::TypeKind::Array:
            return type_to_string(*type.element) + "[" + std::to_string(static_cast<std::size_t>(type.array_size)) + "]";
        case scpp::TypeKind::Reference:
            if (type.is_rvalue_ref) return type_to_string(*type.pointee) + "&&";
            return (type.is_mutable_ref ? std::string{} : std::string{"const "}) + type_to_string(*type.pointee) +
                   "&";
        case scpp::TypeKind::Span:
            return "std::span<" + (type.is_mutable_ref ? std::string{} : std::string{"const "}) +
                   type_to_string(*type.pointee) + ">";
    }
    return "?";
}

[[nodiscard]] std::expected<std::string, std::string> read_file(std::string_view path) {
    std::string p_str = string_from_view(path);
    int fd = -1;
    [[scpp::unsafe]] {
        fd = open(p_str.c_str(), 0);
    }
    if (fd < 0) {
        return std::unexpected(std::string{"cannot open file '"} + p_str + "'");
    }
    std::string result{};
    char buf[4096] = {};
    while (true) {
        long n = 0;
        [[scpp::unsafe]] {
            void* p = &buf[0];
            n = read(fd, p, static_cast<unsigned long>(4096));
        }
        if (n < 0) {
            [[scpp::unsafe]] { close(fd); }
            return std::unexpected(std::string{"error reading file '"} + p_str + "'");
        }
        if (n == 0) break;
        for (long i = 0; i < n; i++) {
            result.push_back(buf[static_cast<std::size_t>(i)]);
        }
    }
    [[scpp::unsafe]] {
        close(fd);
    }
    return result;
}

[[nodiscard]] bool is_implicit_source_path(std::string_view path) {
    return ends_with(path, ".scpp") || ends_with(path, ".cppm") || ends_with(path, ".cpp") ||
           ends_with(path, ".cc") || ends_with(path, ".cxx");
}

bool require_scpp_input_path(std::string_view path, std::string_view role) {
    if (is_implicit_source_path(path)) return true;
    eprint("error: unrecognized positional ");
    eprint(role);
    eprint(" '");
    eprint(path);
    eprintln("'; positional source inputs must use a supported source extension (.scpp, .cppm, .cpp, .cc, .cxx), otherwise pass --source <path>");
    return false;
}

class ScannedModuleDecl {
public:
    virtual ~ScannedModuleDecl() = default;
    ScannedModuleDecl() = default;
    ScannedModuleDecl(const ScannedModuleDecl& other) = default;
    void operator=(const ScannedModuleDecl& other) {
        this->has_value = other.has_value;
        this->module_name = other.module_name;
        this->partition_name = other.partition_name;
    }
    bool has_value{false};
    std::string module_name{};
    std::string partition_name{};
};

ScannedModuleDecl scan_declared_module_from_source(const std::string& source) {
    std::vector<scpp::Token> tokens = scpp::tokenize(source);
    std::size_t i = 0;
    if (i + 1 < tokens.size() && tokens.at(i).kind == scpp::TokenKind::KwModule &&
        tokens.at(i + 1).kind == scpp::TokenKind::Semicolon) {
        i += 2;
    }
    if (i < tokens.size() && tokens.at(i).kind == scpp::TokenKind::KwExport) {
        if (i + 1 < tokens.size() && tokens.at(i + 1).kind == scpp::TokenKind::KwModule) i++;
    }
    ScannedModuleDecl empty_decl{};
    if (i >= tokens.size() || tokens.at(i).kind != scpp::TokenKind::KwModule) return empty_decl;
    i++;
    if (i >= tokens.size() || tokens.at(i).kind != scpp::TokenKind::Identifier) return empty_decl;

    ScannedModuleDecl decl{};
    decl.has_value = true;
    decl.module_name = string_from_view(tokens.at(i).text);
    i++;
    while (i + 1 < tokens.size() && tokens.at(i).kind == scpp::TokenKind::Dot &&
           tokens.at(i + 1).kind == scpp::TokenKind::Identifier) {
        decl.module_name += ".";
        decl.module_name += string_from_view(tokens.at(i + 1).text);
        i += 2;
    }
    if (i < tokens.size() && tokens.at(i).kind == scpp::TokenKind::Colon) {
        i++;
        if (i >= tokens.size() || tokens.at(i).kind != scpp::TokenKind::Identifier) {
            ScannedModuleDecl empty_colon{};
            return empty_colon;
        }
        decl.partition_name = string_from_view(tokens.at(i).text);
    }
    return decl;
}

class ImportEntry {
public:
    virtual ~ImportEntry() = default;
    ImportEntry() = default;
    ImportEntry(std::string n, std::string p) : name{std::move(n)}, path{std::move(p)} {}
    std::string name{};
    std::string path{};
};

bool validate_import_paths(const std::vector<ImportEntry>& import_entries) {
    for (std::size_t i = 0; i < import_entries.size(); i++) {
        const std::string& module_name = import_entries.at(i).name;
        const std::string& path = import_entries.at(i).path;
        if (ends_with(path, ".scpp") || ends_with(path, ".scppm") || ends_with(path, ".cppm")) continue;
        eprint("error: import path for module '");
        eprint(module_name);
        eprint("' must use the .scpp or .scppm extension, got '");
        eprint(path);
        eprintln("'");
        return false;
    }
    return true;
}

bool append_explicit_source_imports(const std::vector<std::string>& source_paths,
                                    std::unordered_map<std::string, std::string>& import_paths) {
    for (std::size_t i = 1; i < source_paths.size(); i++) {
        auto source_result = read_file(source_paths.at(i));
        if (!source_result.has_value()) {
            eprint("error: ");
            eprintln(source_result.error());
            return false;
        }
        const std::string& source = source_result.value();
        ScannedModuleDecl decl = scan_declared_module_from_source(source);
        if (!decl.has_value) {
            eprint("error: supplemental source '");
            eprint(source_paths.at(i));
            eprintln("' must declare a module or partition so it can be imported; only the first source may be a plain translation unit");
            return false;
        }
        std::string key = decl.module_name;
        if (!decl.partition_name.empty()) {
            key += ":";
            key += decl.partition_name;
        }
        auto res = import_paths.emplace(key, source_paths.at(i));
        if (!res.second && res.first->second != source_paths.at(i)) {
            eprint("error: source inputs conflict for module '");
            eprint(key);
            eprint("': '");
            eprint(res.first->second);
            eprint("' and '");
            eprint(source_paths.at(i));
            eprintln("'");
            return false;
        }
    }
    return true;
}

enum class SourceArgParseResult {
    NotSource,
    AddedSource,
    Error,
};

SourceArgParseResult maybe_collect_source_arg(std::string_view arg, int& i, int argc, char** argv,
                                             std::vector<std::string>& source_paths, std::string_view role = "input") {
    if (arg == "--source") {
        if (i + 1 >= argc) {
            eprintln("error: --source requires <path>");
            return SourceArgParseResult::Error;
        }
        i++;
        const char* next_arg = get_arg(argv, i);
        source_paths.push_back(std::string{next_arg});
        return SourceArgParseResult::AddedSource;
    }
    if (!arg.empty() && arg.at(0) != '-') {
        if (!require_scpp_input_path(arg, role)) return SourceArgParseResult::Error;
        source_paths.push_back(string_from_view(arg));
        return SourceArgParseResult::AddedSource;
    }
    return SourceArgParseResult::NotSource;
}

void print_diagnostic(std::string_view path, const std::string& source, scpp::SourceLocation loc,
                      const std::string& message) {
    std::string effective_path = string_from_view(path);
    std::string reread_source{};
    if (loc.has_source_path() && loc.source_path_text() != path) {
        effective_path = string_from_view(loc.source_path_text());
        auto reread_result = read_file(effective_path);
        if (reread_result.has_value()) {
            reread_source = std::move(reread_result).value();
        }
    }
    const std::string& effective_source = reread_source.empty() ? source : reread_source;

    eprint(effective_path);
    eprint(":");
    if (loc.is_known()) {
        eprint(std::to_string(static_cast<std::int64_t>(loc.line)) + ":" +
               std::to_string(static_cast<std::int64_t>(loc.column)) + ":");
    }
    eprint(" error: " + message + "\n");
    if (!loc.is_known()) return;

    std::size_t line_start = 0;
    int current_line = 1;
    while (current_line < loc.line) {
        std::size_t next_nl = effective_source.find('\n', line_start);
        if (next_nl == std::string::npos) return;
        line_start = next_nl + 1;
        current_line++;
    }
    std::size_t line_end = effective_source.find('\n', line_start);
    if (line_end == std::string::npos) line_end = effective_source.size();
    std::string_view line_text = std::string_view{effective_source}.substr(line_start, line_end - line_start);

    std::string line_num_str = std::to_string(static_cast<std::int64_t>(loc.line));
    std::string gutter{};
    for (std::size_t i = 0; i < line_num_str.size(); i++) {
        gutter.push_back(' ');
    }
    eprintln(" " + line_num_str + " | " + string_from_view(line_text));
    eprint(" " + gutter + " | ");
    for (int i = 0; i < loc.column - 1 && static_cast<std::size_t>(i) < line_text.size(); i++) {
        eprint(line_text.at(static_cast<std::size_t>(i)) == '	' ? "	" : " ");
    }
    eprint("^\n");
}

int run_lex(std::string_view path) {
    auto source_result = read_file(path);
    if (!source_result.has_value()) {
        eprint("error: ");
        eprintln(source_result.error());
        return 1;
    }
    const std::string& source = source_result.value();

    for (const scpp::Token& tok : scpp::tokenize(source)) {
        oprint(std::to_string(static_cast<std::int64_t>(tok.line)) + ":" +
               std::to_string(static_cast<std::int64_t>(tok.column)) + "	" +
               string_from_view(token_kind_name(tok.kind)));
        if (tok.kind != scpp::TokenKind::EndOfFile) {
            oprint("	'");
            oprint(tok.text);
            oprint("'");
        }
        oprintln();
    }
    return 0;
}

std::string_view binary_op_name(scpp::BinaryOp op) {
    switch (op) {
        case scpp::BinaryOp::Add: return "+";
        case scpp::BinaryOp::Sub: return "-";
        case scpp::BinaryOp::Mul: return "*";
        case scpp::BinaryOp::Div: return "/";
        case scpp::BinaryOp::Mod: return "%";
        case scpp::BinaryOp::BitAnd: return "&";
        case scpp::BinaryOp::BitXor: return "^";
        case scpp::BinaryOp::BitOr: return "|";
        case scpp::BinaryOp::Shl: return "<<";
        case scpp::BinaryOp::Shr: return ">>";
        case scpp::BinaryOp::AddAssign: return "+=";
        case scpp::BinaryOp::SubAssign: return "-=";
        case scpp::BinaryOp::MulAssign: return "*=";
        case scpp::BinaryOp::DivAssign: return "/=";
        case scpp::BinaryOp::ModAssign: return "%=";
        case scpp::BinaryOp::BitAndAssign: return "&=";
        case scpp::BinaryOp::BitXorAssign: return "^=";
        case scpp::BinaryOp::BitOrAssign: return "|=";
        case scpp::BinaryOp::ShlAssign: return "<<=";
        case scpp::BinaryOp::ShrAssign: return ">>=";
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
        case scpp::UnaryOp::BitNot: return "~";
        case scpp::UnaryOp::Not: return "!";
        case scpp::UnaryOp::PreInc: return "++";
        case scpp::UnaryOp::PreDec: return "--";
        case scpp::UnaryOp::PostInc: return "++ (post)";
        case scpp::UnaryOp::PostDec: return "-- (post)";
        case scpp::UnaryOp::Deref: return "*";
        case scpp::UnaryOp::AddressOf: return "&";
    }
    return "?";
}

void print_indent(int depth) {
    for (int i = 0; i < depth; i++) oprint("  ");
}

void print_stmt(const scpp::Stmt& stmt, int depth);

void print_expr(const scpp::Expr& expr, int depth) {
    print_indent(depth);
    switch (expr.kind) {
        case scpp::ExprKind::IntegerLiteral:
            oprintln("IntegerLiteral " + std::to_string(static_cast<std::int64_t>(expr.int_value)));
            break;
        case scpp::ExprKind::FloatLiteral: {
            char fbuf[64] = {};
            [[scpp::unsafe]] {
                snprintf(fbuf, 64, "%g", expr.float_value);
            }
            oprintln(std::string{"FloatLiteral "} + fbuf);
            break;
        }
        case scpp::ExprKind::BoolLiteral:
            oprintln(expr.bool_value ? "BoolLiteral true" : "BoolLiteral false");
            break;
        case scpp::ExprKind::NullptrLiteral:
            oprintln("NullptrLiteral");
            break;
        case scpp::ExprKind::TypeTrait:
            oprintln("TypeTrait " + expr.name);
            print_indent(depth + 1);
            oprintln("Type " + expr.type.name);
            break;
        case scpp::ExprKind::CharLiteral:
            oprintln("CharLiteral " + std::to_string(static_cast<std::int64_t>(expr.int_value)));
            break;
        case scpp::ExprKind::StringLiteral:
            oprintln("StringLiteral " + expr.name);
            break;
        case scpp::ExprKind::Identifier:
            oprintln("Identifier " + expr.name);
            break;
        case scpp::ExprKind::Binary:
            oprintln("Binary " + string_from_view(binary_op_name(expr.binary_op)));
            print_expr(*expr.lhs, depth + 1);
            print_expr(*expr.rhs, depth + 1);
            break;
        case scpp::ExprKind::Conditional:
            oprintln("Conditional");
            print_expr(*expr.lhs, depth + 1);
            print_expr(*expr.rhs, depth + 1);
            print_expr(*expr.third, depth + 1);
            break;
        case scpp::ExprKind::Fold:
            oprintln("Fold " + string_from_view(binary_op_name(expr.binary_op)) +
                     (expr.fold_ellipsis_on_left ? " (left)" : " (right)"));
            print_expr(*expr.lhs, depth + 1);
            if (expr.rhs) print_expr(*expr.rhs, depth + 1);
            break;
        case scpp::ExprKind::Unary:
            oprintln("Unary " + string_from_view(unary_op_name(expr.unary_op)));
            print_expr(*expr.lhs, depth + 1);
            break;
        case scpp::ExprKind::Call:
            oprintln("Call " + expr.name);
            if (expr.lhs) {
                print_indent(depth + 1);
                oprintln("Receiver");
                print_expr(*expr.lhs, depth + 2);
            }
            for (std::size_t i = 0; i < expr.args.size(); i++) print_expr(*expr.args.at(i), depth + 1);
            break;
        case scpp::ExprKind::Member:
            oprintln("Member ." + expr.name);
            print_expr(*expr.lhs, depth + 1);
            break;
        case scpp::ExprKind::Subscript:
            oprintln("Subscript");
            print_expr(*expr.lhs, depth + 1);
            print_expr(*expr.rhs, depth + 1);
            break;
        case scpp::ExprKind::Move:
            oprintln("Move");
            print_expr(*expr.lhs, depth + 1);
            break;
        case scpp::ExprKind::Cast:
            oprintln("Cast " + type_to_string(expr.type));
            print_expr(*expr.lhs, depth + 1);
            break;
        case scpp::ExprKind::Alignof:
            oprintln("AlignofType " + type_to_string(expr.type));
            break;
        case scpp::ExprKind::Sizeof:
            if (expr.sizeof_operand_is_type) {
                oprintln("SizeofType " + type_to_string(expr.type));
            } else {
                oprintln("SizeofExpr");
                print_expr(*expr.lhs, depth + 1);
            }
            break;
        case scpp::ExprKind::ValueInit:
            oprintln("ValueInit " + type_to_string(expr.type));
            break;
        case scpp::ExprKind::BracedInitList:
            oprintln("BracedInitList");
            for (std::size_t i = 0; i < expr.args.size(); i++) print_expr(*expr.args.at(i), depth + 1);
            break;
        case scpp::ExprKind::New:
            oprintln("New " + type_to_string(expr.type));
            if (expr.lhs) {
                print_indent(depth + 1);
                oprintln("Placement");
                print_expr(*expr.lhs, depth + 2);
            }
            for (std::size_t i = 0; i < expr.args.size(); i++) print_expr(*expr.args.at(i), depth + 1);
            break;
        case scpp::ExprKind::Delete:
            oprintln("Delete");
            print_expr(*expr.lhs, depth + 1);
            break;
        case scpp::ExprKind::Destroy:
            oprintln((expr.destroy_through_pointer ? "DestroyPtr " : "DestroyObj ") + type_to_string(expr.type));
            print_expr(*expr.lhs, depth + 1);
            break;
        case scpp::ExprKind::PackExpansion:
            oprintln("PackExpansion");
            print_expr(*expr.lhs, depth + 1);
            break;
        case scpp::ExprKind::Lambda: {
            oprint("Lambda");
            if (!expr.name.empty()) oprint(" -> " + expr.name);
            oprintln();
            print_indent(depth + 1);
            oprint("Captures");
            switch (expr.lambda_blanket_mode) {
                case scpp::LambdaCaptureMode::ByValue: oprint(" (blanket =)"); break;
                case scpp::LambdaCaptureMode::ByReference: oprint(" (blanket &)"); break;
                case scpp::LambdaCaptureMode::None: break;
            }
            oprintln();
            for (std::size_t i = 0; i < expr.lambda_captures.size(); i++) {
                const auto& capture = expr.lambda_captures.at(i);
                print_indent(depth + 2);
                oprint((capture.by_reference ? "&" : "") + capture.name);
                if (capture.init) oprint(" = <init-expr>");
                oprintln();
            }
            print_indent(depth + 1);
            oprintln("Params");
            for (std::size_t i = 0; i < expr.lambda_params.size(); i++) {
                const auto& param = expr.lambda_params.at(i);
                print_indent(depth + 2);
                oprintln(type_to_string(param.type) + " " + param.name);
            }
            if (expr.lambda_is_mutable) {
                print_indent(depth + 1);
                oprintln("mutable");
            }
            if (expr.has_lambda_explicit_return_type) {
                print_indent(depth + 1);
                oprintln("-> " + type_to_string(expr.type));
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
            oprintln("VarDecl " + type_to_string(stmt.type) + " " + stmt.var_name);
            if (stmt.init) print_expr(*stmt.init, depth + 1);
            if (stmt.has_ctor_args) {
                print_indent(depth + 1);
                oprintln("CtorArgs");
                for (std::size_t i = 0; i < stmt.ctor_args.size(); i++) print_expr(*stmt.ctor_args.at(i), depth + 2);
            }
            break;
        case scpp::StmtKind::Return:
            oprintln("Return");
            if (stmt.expr) print_expr(*stmt.expr, depth + 1);
            break;
        case scpp::StmtKind::If:
            oprintln("If");
            print_expr(*stmt.condition, depth + 1);
            print_stmt(*stmt.then_branch, depth + 1);
            if (stmt.else_branch) print_stmt(*stmt.else_branch, depth + 1);
            break;
        case scpp::StmtKind::While:
            oprintln("While");
            print_expr(*stmt.condition, depth + 1);
            print_stmt(*stmt.then_branch, depth + 1);
            break;
        case scpp::StmtKind::Switch:
            oprintln("Switch");
            print_expr(*stmt.condition, depth + 1);
            for (std::size_t i = 0; i < stmt.switch_cases.size(); i++) {
                const auto& switch_case = stmt.switch_cases.at(i);
                print_indent(depth + 1);
                oprintln(switch_case.value ? "Case" : "Default");
                if (switch_case.value) print_expr(*switch_case.value, depth + 2);
                for (std::size_t j = 0; j < switch_case.statements.size(); j++) {
                    print_stmt(*switch_case.statements.at(j), depth + 2);
                }
            }
            break;
        case scpp::StmtKind::Break:
            oprintln("Break");
            break;
        case scpp::StmtKind::Continue:
            oprintln("Continue");
            break;
        case scpp::StmtKind::Fallthrough:
            oprintln("Fallthrough");
            break;
        case scpp::StmtKind::ExprStmt:
            oprintln("ExprStmt");
            print_expr(*stmt.expr, depth + 1);
            break;
        case scpp::StmtKind::Block:
            oprintln(stmt.is_unsafe ? "Block (unsafe)" : "Block");
            for (std::size_t i = 0; i < stmt.statements.size(); i++) print_stmt(*stmt.statements.at(i), depth + 1);
            break;
    }
}

int run_parse(std::string_view path) {
    auto source_result = read_file(path);
    if (!source_result.has_value()) {
        eprint("error: ");
        eprintln(source_result.error());
        return 1;
    }
    const std::string& source = source_result.value();

    scpp::Program program{};
    {
        scpp::ModuleResolver resolver{scpp::no_module_resolver};
        scpp::PartitionResolver partition_resolver{scpp::no_partition_resolver};
        std::expected<scpp::Program, scpp::ParseError> parse_result =
            scpp::parse(std::string_view{source}, resolver, partition_resolver,
                        string_from_view(path));
        if (!parse_result.has_value()) {
            print_diagnostic(path, source, parse_result.error().loc, parse_result.error().what());
            return 1;
        }
        program = std::move(parse_result.value());
    }
    {
        for (std::size_t struct_idx = 0; struct_idx < program.structs.size(); struct_idx++) {
            const scpp::StructDef& def = program.structs.at(struct_idx);
            oprintln("Struct " + def.name);
            for (std::size_t field_idx = 0; field_idx < def.fields.size(); field_idx++) {
                const scpp::StructField& field = def.fields.at(field_idx);
                print_indent(1);
                oprintln("Field " + type_to_string(field.type) + " " + field.name);
            }
        }
        for (std::size_t fn_idx = 0; fn_idx < program.functions.size(); fn_idx++) {
            const scpp::Function& fn = program.functions.at(fn_idx);
            oprint("Function ");
            if (fn.is_extern_c) oprint("extern \"C\" ");
            oprint(type_to_string(fn.return_type) + " " + fn.name + "(");
            for (std::size_t i = 0; i < fn.params.size(); i++) {
                if (i > 0) oprint(", ");
                oprint(type_to_string(fn.params.at(i).type) + " " + fn.params.at(i).name);
            }
            if (fn.has_varargs) {
                oprint(fn.params.empty() ? "..." : ", ...");
            }
            oprintln(")");
            if (fn.body) {
                print_stmt(*fn.body, 1);
            } else {
                print_indent(1);
                oprintln("(no body -- external declaration)");
            }
        }
    }
    return 0;
}

int run_build(std::string_view input_path, std::string_view output_path,
              const std::vector<std::string>& extra_link_inputs,
              const std::unordered_map<std::string, std::string>& import_paths,
              const std::vector<std::string>& import_search_dirs, bool static_link, bool emit_debug_info,
              bool compile_only = false, int opt_level = 2) {
    auto source_result = read_file(input_path);
    if (!source_result.has_value()) {
        eprint("error: ");
        eprintln(source_result.error());
        return 1;
    }
    const std::string& source = source_result.value();

    if (compile_only) {
        auto result = scpp::compile_to_object(source, string_from_view(output_path), import_paths,
                                              import_search_dirs, emit_debug_info, string_from_view(input_path),
                                              opt_level);
        if (!result.has_value()) {
            print_diagnostic(input_path, source, result.error().loc, result.error().what());
            return 1;
        }
        return 0;
    }

    auto result = scpp::compile_to_executable(source, string_from_view(output_path), extra_link_inputs, import_paths, static_link,
                                import_search_dirs, emit_debug_info, string_from_view(input_path), opt_level);
    if (!result.has_value()) {
        print_diagnostic(input_path, source, result.error().loc, result.error().what());
        return 1;
    }
    return 0;
}

int run_build_module(std::string_view input_path, std::string_view interface_path, std::string_view archive_path,
                     const std::unordered_map<std::string, std::string>& import_paths,
                     const std::vector<std::string>& import_search_dirs) {
    if (!ends_with(interface_path, ".scppm")) {
        eprint("error: module interface output must use the .scppm extension, got '");
        eprint(interface_path);
        eprintln("'");
        return 1;
    }
    if (!ends_with(archive_path, ".scppa")) {
        eprint("error: module archive output must use the .scppa extension, got '");
        eprint(archive_path);
        eprintln("'");
        return 1;
    }
    auto source_result = read_file(input_path);
    if (!source_result.has_value()) {
        eprint("error: ");
        eprintln(source_result.error());
        return 1;
    }
    const std::string& source = source_result.value();

    auto result = scpp::emit_module_artifacts(source, string_from_view(interface_path), string_from_view(archive_path), import_paths,
                                import_search_dirs, string_from_view(input_path));
    if (!result.has_value()) {
        print_diagnostic(input_path, source, result.error().loc, result.error().what());
        return 1;
    }
    return 0;
}

export int run(int argc, char** argv) {
    std::string_view name = argc > 0 ? std::string_view{get_arg(argv, 0)} : std::string_view{"scpp"};
    if (argc >= 2 && std::string_view{get_arg(argv, 1)} == "lex") {
        std::vector<std::string> source_paths{};
        for (int i = 2; i < argc; i++) {
            std::string_view arg{get_arg(argv, i)};
            switch (maybe_collect_source_arg(arg, i, argc, argv, source_paths, "input file")) {
                case SourceArgParseResult::AddedSource: break;
                case SourceArgParseResult::Error: return 1;
                case SourceArgParseResult::NotSource:
                    eprint("error: unknown lex option '");
                    eprint(arg);
                    eprintln("'");
                    return 1;
            }
        }
        if (source_paths.empty()) {
            eprintln("error: lex requires a source file (pass <file.scpp> or --source <path>)");
            return 1;
        }
        if (source_paths.size() != 1) {
            eprint("error: lex accepts exactly one source file, got ");
            eprintln(std::to_string(static_cast<std::int64_t>(source_paths.size())));
            return 1;
        }
        return run_lex(source_paths.front());
    }
    if (argc >= 2 && std::string_view{get_arg(argv, 1)} == "parse") {
        std::vector<std::string> source_paths{};
        for (int i = 2; i < argc; i++) {
            std::string_view arg{get_arg(argv, i)};
            switch (maybe_collect_source_arg(arg, i, argc, argv, source_paths, "input file")) {
                case SourceArgParseResult::AddedSource: break;
                case SourceArgParseResult::Error: return 1;
                case SourceArgParseResult::NotSource:
                    eprint("error: unknown parse option '");
                    eprint(arg);
                    eprintln("'");
                    return 1;
            }
        }
        if (source_paths.empty()) {
            eprintln("error: parse requires a source file (pass <file.scpp> or --source <path>)");
            return 1;
        }
        if (source_paths.size() != 1) {
            eprint("error: parse accepts exactly one source file, got ");
            eprintln(std::to_string(static_cast<std::int64_t>(source_paths.size())));
            return 1;
        }
        return run_parse(source_paths.front());
    }
    if (argc >= 2 && std::string_view{get_arg(argv, 1)} == "build-module") {
        std::string_view interface_path{};
        std::string_view archive_path{};
        std::vector<std::string> source_paths{};
        std::unordered_map<std::string, std::string> import_paths{};
        std::vector<ImportEntry> import_entries{};
        std::vector<std::string> import_search_dirs{};
        for (int i = 2; i < argc; i++) {
            std::string_view arg{get_arg(argv, i)};
            if (arg == "-I" && i + 1 < argc) {
                i++;
                import_search_dirs.push_back(std::string{get_arg(argv, i)});
            } else if (arg == "--interface-out" && i + 1 < argc) {
                i++;
                interface_path = std::string_view{get_arg(argv, i)};
            } else if (arg == "--archive-out" && i + 1 < argc) {
                i++;
                archive_path = std::string_view{get_arg(argv, i)};
            } else if (arg == "--import" && i + 1 < argc) {
                i++;
                std::string_view mapping{get_arg(argv, i)};
                std::size_t eq = find_char(mapping, '=');
                if (eq == std::string_view::npos) {
                    eprint("error: --import expects 'name=path', got '");
                    eprint(mapping);
                    eprintln("'");
                    return 1;
                }
                std::string mod_name = string_from_view(mapping.substr(0, eq));
                std::string mod_path = string_from_view(mapping.substr(eq + 1));
                import_paths.emplace(mod_name, mod_path);
                import_entries.push_back(ImportEntry{mod_name, mod_path});
            } else {
                switch (maybe_collect_source_arg(arg, i, argc, argv, source_paths, "input file")) {
                    case SourceArgParseResult::AddedSource: break;
                    case SourceArgParseResult::Error: return 1;
                    case SourceArgParseResult::NotSource:
                        eprint("error: unknown build-module option '");
                        eprint(arg);
                        eprintln("'");
                        return 1;
                }
            }
        }
        if (source_paths.empty()) {
            eprintln("error: build-module requires a source file (pass <file.scpp> or --source <path>)");
            return 1;
        }
        if (interface_path.empty()) {
            eprintln("error: build-module requires --interface-out <file.scppm>");
            return 1;
        }
        if (archive_path.empty()) {
            eprintln("error: build-module requires --archive-out <file.scppa>");
            return 1;
        }
        if (!validate_import_paths(import_entries)) return 1;
        if (!append_explicit_source_imports(source_paths, import_paths)) return 1;
        return run_build_module(source_paths.front(), interface_path, archive_path, import_paths, import_search_dirs);
    }
    if (argc >= 2 && std::string_view{get_arg(argv, 1)} == "build") {
        scpp::ProjectBuildOptions options{};
        for (int i = 2; i < argc; i++) {
            std::string_view arg{get_arg(argv, i)};
            if (arg == "--lib") {
                options.build_lib_only = true;
                if (i + 1 < argc) {
                    std::string_view next{get_arg(argv, i + 1)};
                    if (!next.empty() && next.at(0) != '-') {
                        i++;
                        options.selected_lib = string_from_view(std::string_view{get_arg(argv, i)});
                    }
                }
            } else if (arg == "--bin" && i + 1 < argc) {
                i++;
                options.selected_bin = string_from_view(std::string_view{get_arg(argv, i)});
            } else if ((arg == "-p" || arg == "--package") && i + 1 < argc) {
                i++;
                options.selected_package = string_from_view(std::string_view{get_arg(argv, i)});
            } else if (arg == "--workspace") {
                options.build_workspace = true;
            } else {
                eprint("error: unknown build option '");
                eprint(arg);
                eprintln("'");
                return 1;
            }
        }
        return scpp::build_manifest_project(path_current(), options);
    }
    if (argc >= 2) {
        std::string_view explicit_output_path{};
        std::vector<std::string> source_paths{};
        std::vector<std::string> extra_link_inputs{};
        std::unordered_map<std::string, std::string> import_paths{};
        std::vector<ImportEntry> import_entries{};
        std::vector<std::string> import_search_dirs{};
        bool static_link = false;
        bool emit_debug_info = false;
        bool compile_only = false;
        int opt_level = 2;
        bool version_only = false;
        for (int i = 1; i < argc; i++) {
            std::string_view arg{get_arg(argv, i)};
            if (arg == "-o" && i + 1 < argc) {
                i++;
                explicit_output_path = std::string_view{get_arg(argv, i)};
            } else if (starts_with(arg, "-o") && arg.size() > 2) {
                explicit_output_path = arg.substr(2);
            } else if (arg == "-c") {
                compile_only = true;
            } else if (arg == "-I" && i + 1 < argc) {
                i++;
                import_search_dirs.push_back(std::string{get_arg(argv, i)});
            } else if (starts_with(arg, "-I") && arg.size() > 2) {
                import_search_dirs.push_back(string_from_view(arg.substr(2)));
            } else if (arg == "-isystem" && i + 1 < argc) {
                i++;
                import_search_dirs.push_back(std::string{get_arg(argv, i)});
            } else if (starts_with(arg, "-isystem") && arg.size() > 8) {
                import_search_dirs.push_back(string_from_view(arg.substr(8)));
            } else if (arg == "-g") {
                emit_debug_info = true;
            } else if (arg == "-g0") {
                emit_debug_info = false;
            } else if (starts_with(arg, "-g")) {
                emit_debug_info = true;
            } else if (arg == "-O0") {
                opt_level = 0;
            } else if (arg == "-O1") {
                opt_level = 1;
            } else if (arg == "-O2") {
                opt_level = 2;
            } else if (arg == "-O3") {
                opt_level = 3;
            } else if (arg == "-Os" || arg == "-Oz" || arg == "-Og") {
                opt_level = 2;
            } else if (arg == "-Ofast") {
                opt_level = 3;
            } else if (arg == "-O") {
                opt_level = 2;
            } else if (arg == "--static") {
                static_link = true;
            } else if (arg == "--link" && i + 1 < argc) {
                i++;
                extra_link_inputs.push_back(std::string{get_arg(argv, i)});
            } else if (arg == "--import" && i + 1 < argc) {
                i++;
                std::string_view mapping{get_arg(argv, i)};
                std::size_t eq = find_char(mapping, '=');
                if (eq == std::string_view::npos) {
                    eprint("error: --import expects 'name=path', got '");
                    eprint(mapping);
                    eprintln("'");
                    return 1;
                }
                std::string mod_name = string_from_view(mapping.substr(0, eq));
                std::string mod_path = string_from_view(mapping.substr(eq + 1));
                import_paths.emplace(mod_name, mod_path);
                import_entries.push_back(ImportEntry{mod_name, mod_path});
            } else if (arg == "-D" && i + 1 < argc) {
                i++;
            } else if (starts_with(arg, "-D")) {
                // accept macro definition
            } else if (arg == "-U" && i + 1 < argc) {
                i++;
            } else if (starts_with(arg, "-U")) {
                // accept macro undefine
            } else if (starts_with(arg, "-std=") || starts_with(arg, "--std=")) {
                // accept language standard
            } else if (starts_with(arg, "-Wl,")) {
                extra_link_inputs.push_back(string_from_view(arg));
            } else if (starts_with(arg, "-W") || arg == "-w") {
                // accept warning flags
            } else if (starts_with(arg, "-f")) {
                // accept compiler feature flags
            } else if (starts_with(arg, "-m")) {
                // accept machine/architecture flags
            } else if (arg == "-pthread" || arg == "-pthreads") {
                extra_link_inputs.push_back(string_from_view(arg));
            } else if (arg == "-pipe") {
                // accept -pipe
            } else if (arg == "-shared" || arg == "--shared") {
                // accept -shared
            } else if (starts_with(arg, "-l")) {
                extra_link_inputs.push_back(string_from_view(arg));
            } else if (arg == "-L" && i + 1 < argc) {
                i++;
                extra_link_inputs.push_back("-L" + std::string{get_arg(argv, i)});
            } else if (starts_with(arg, "-L")) {
                extra_link_inputs.push_back(string_from_view(arg));
            } else if (arg == "--version") {
                oprintln("scpp version " + string_from_view(version));
                return 0;
            } else if (arg == "-v") {
                version_only = true;
            } else if (!arg.empty() && arg.at(0) != '-' &&
                       (ends_with(arg, ".o") || ends_with(arg, ".a") || ends_with(arg, ".so"))) {
                extra_link_inputs.push_back(string_from_view(arg));
            } else {
                switch (maybe_collect_source_arg(arg, i, argc, argv, source_paths, "input file")) {
                    case SourceArgParseResult::AddedSource: break;
                    case SourceArgParseResult::Error: return 1;
                    case SourceArgParseResult::NotSource:
                        eprint("error: unknown option '");
                        eprint(arg);
                        eprintln("'");
                        return 1;
                }
            }
        }
        if (source_paths.empty()) {
            if (!extra_link_inputs.empty() && !compile_only) {
                std::string link_output = explicit_output_path.empty() ? std::string{"a.out"} : string_from_view(explicit_output_path);
                auto link_r = scpp::link_executable(extra_link_inputs, link_output, static_link);
                if (!link_r.has_value()) {
                    eprintln(link_r.error().what());
                    return 1;
                }
                return 0;
            }
            if (version_only) {
                oprintln("scpp version " + string_from_view(version));
                return 0;
            }
            eprintln("error: build requires a source file (pass <file.scpp> or --source <path>)");
            return 1;
        }
        if (!validate_import_paths(import_entries)) return 1;
        if (!append_explicit_source_imports(source_paths, import_paths)) return 1;
        std::string output_path = !explicit_output_path.empty()
                                      ? string_from_view(explicit_output_path)
                                      : (compile_only ? scpp::derive_object_path(source_paths.front())
                                                      : std::string{"a.out"});
        return run_build(source_paths.front(), output_path, extra_link_inputs, import_paths, import_search_dirs, static_link,
                         emit_debug_info, compile_only, opt_level);
    }

    if (scpp::find_project_manifest(path_current()).has_value()) {
        return scpp::build_manifest_project(path_current(), scpp::ProjectBuildOptions{});
    }

    oprintln("Hello from " + string_from_view(name) + " " + string_from_view(version) + "!");
    oprintln("Usage: " + string_from_view(name) + " lex <file.scpp>|--source <file>");
    oprintln("       " + string_from_view(name) + " parse <file.scpp>|--source <file>");
    oprintln("       " + string_from_view(name) +
             " <file.scpp> [<more.scpp>...] [--source <file>]... [-c] [-o <output>] [-I <dir>]... [-g] [--static] [--link <path>]... [--import name=path]...");
    oprintln("       " + string_from_view(name) +
             " build [--workspace] [-p <package>] [--lib [<name>]] [--bin <name>]");
    oprintln("       " + string_from_view(name) +
             " build-module <file.scpp> [<more.scpp>...] [--source <file>]... --interface-out <file.scppm> --archive-out <file.scppa> [-I <dir>]... [--import name=path]...");
    return 0;
}

} // namespace scpp
