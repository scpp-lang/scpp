module;

// Official llvm::LLVM-C (llvm-c/*.h) is itself already a stable, extern "C"
// interface -- the TargetMachine construction and object-file emission
// this file performs go through its llvm-c/TargetMachine.h (module
// `llvm`'s `:target_machine` partition) and llvm-c/Target.h (`:target`
// partition) functions directly instead of any native llvm::LLVM C++ header
// (llvm::TargetRegistry, llvm::TargetMachine, llvm::legacy::PassManager,
// etc.); the few llvm-c/Core.h pieces this file also touches
// (llvm::LLVMModuleRef, llvm::LLVMDisposeMessage) come from the same module's
// `:core` partition instead -- all reached via the single `import llvm;`
// below (module `llvm` re-exports every partition, see
// libs/llvm/llvm.cpp). See libs/README.md for why this project binds
// straight to llvm::LLVM-C wherever it already covers what's needed --
// including llvm::LLVMTargetMachineEmitToFile, which alone replaces the
// raw_fd_ostream + legacy::PassManager + addPassesToEmitFile dance the
// native C++ API required: a rigorous, function-by-function empirical
// audit found llvm::LLVM-C fully covers every llvm::LLVM operation this project's
// driver needs.

export module scpp.driver;

import std;
import llvm;
import scpp.ast;
import scpp.compiler.codegen;
import scpp.constexpression;
import scpp.lexer;
import scpp.mir;
import scpp.compiler.movecheck;
import scpp.parser;

export namespace scpp {

// Distinguishes which underlying stage produced a DriverError. This
// exists purely so that callers reached only through DriverError's
// std::expected channel -- cli.cppm, project.cppm, and tests/driver_test.cpp,
// as of batch 5 (#411) -- can still tell a movecheck failure apart from a
// codegen failure the same way distinguishing DataflowError/CodegenError
// by C++ exception type used to allow, now that this file no longer
// re-throws either of them (see emit_object_file_for_program below).
// `Driver` covers every other DriverError source (module resolution,
// linking, archiving, ParseError/ConstexprError wrapping, etc.) and is
// the default, so the ~40 other DriverError(...) construction sites
// throughout this file are unaffected.
enum class DriverErrorKind {
    Driver,
    Dataflow,
    Codegen,
};

// SourceLocation defaults to {} for driver-native errors that have no
// associated source position (e.g. "cannot find module", linker/archiver
// failures); it is populated only when a DriverError is wrapping a
// propagated ParseError from parser.cppm's own std::expected result, so
// that location survives the wrap -- matching the shape ParseError/
// DataflowError/CodegenError already use. `kind` defaults to
// DriverErrorKind::Driver for the same reason.
class DriverError : public std::runtime_error {
public:
    explicit DriverError(const std::string& message, SourceLocation loc = {}, DriverErrorKind kind = DriverErrorKind::Driver)
        : runtime_error{message}, loc{loc}, kind{kind} {}

    DriverError(const DriverError& other)
        : runtime_error{std::string{other.what()}}, loc{other.loc}, kind{other.kind} {}

    virtual ~DriverError() override = default;

    SourceLocation loc{};
    DriverErrorKind kind{DriverErrorKind::Driver};
};

constexpr std::uint32_t SCPPM_COMPILE_TIME_AST_VERSION = 11;
constexpr std::string_view SCPPM_COMPILE_TIME_AST_MAGIC = "SAST";

class CompileTimePayloadPlan {
public:
    virtual ~CompileTimePayloadPlan() = default;
    std::uint32_t format_version = SCPPM_COMPILE_TIME_AST_VERSION;
    std::vector<std::string> root_function_names{};
    std::vector<std::size_t> reachable_function_indices{};
    std::vector<std::string> reachable_type_names{};
};

[[nodiscard]] CompileTimePayloadPlan plan_compile_time_payload(const Program& program);

} // namespace scpp

// Module-private helpers (ch11 §11.7/§11.8/§11.13): resolving `import
// name;` declarations against a `--import name=path` mapping (see
// cli.cppm) and lowering an already-parsed Program to a native object
// file -- shared by the main source and, once per resolved module, by
// compile_to_executable below.
namespace scpp {

[[nodiscard]] std::string module_key(const Program& program) {
    if (program.partition_name.empty()) {
        std::string name = program.module_name;
        return name;
    }
    return program.module_name + ":" + program.partition_name;
}

class ScannedModuleDecl {
public:
    virtual ~ScannedModuleDecl() = default;
    std::string module_name{};
    std::string partition_name{};
};

inline std::string string_from_view(std::string_view sv) {
    return std::string(sv.data(), sv.size());
}

inline bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline bool is_ascii_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

[[nodiscard]] std::optional<ScannedModuleDecl> scan_declared_module_from_tokens(const std::vector<Token>& tokens) {
    std::size_t i = 0;
    if (i + 1 < tokens.size() && tokens[i].kind == TokenKind::KwModule && tokens[i + 1].kind == TokenKind::Semicolon) {
        i += 2;
    }
    if (i < tokens.size() && tokens[i].kind == TokenKind::KwExport && i + 1 < tokens.size() &&
        tokens[i + 1].kind == TokenKind::KwModule) {
        i++;
    }
    if (i >= tokens.size() || tokens[i].kind != TokenKind::KwModule) return std::nullopt;
    i++;
    if (i >= tokens.size() || tokens[i].kind != TokenKind::Identifier) return std::nullopt;
    ScannedModuleDecl decl{};
    decl.module_name = string_from_view(tokens[i].text);
    i++;
    while (i + 1 < tokens.size() && tokens[i].kind == TokenKind::Dot && tokens[i + 1].kind == TokenKind::Identifier) {
        decl.module_name += ".";
        decl.module_name += string_from_view(tokens[i + 1].text);
        i += 2;
    }
    if (i < tokens.size() && tokens[i].kind == TokenKind::Colon) {
        i++;
        if (i >= tokens.size() || tokens[i].kind != TokenKind::Identifier) return std::nullopt;
        decl.partition_name = string_from_view(tokens[i].text);
    }
    return std::optional<ScannedModuleDecl>{std::move(decl)};
}

[[nodiscard]] std::optional<ScannedModuleDecl> scan_declared_module_from_source(std::string_view source) {
    return scan_declared_module_from_tokens(tokenize(source));
}

[[nodiscard]] std::optional<std::string> declared_module_name_from_source(std::string_view source) {
    if (std::optional<ScannedModuleDecl> decl = scan_declared_module_from_source(source); decl.has_value()) {
        return decl->module_name;
    }
    return std::nullopt;
}

class ByteWriter {
public:
    virtual ~ByteWriter() = default;
    std::string bytes{};
    void put(char c) { bytes.push_back(c); }
    void write(const char* data, std::size_t size) {
        bytes.append(std::string(data, size));
    }
    std::string str() const {
        std::string s = bytes;
        return s;
    }
};

class ByteReader {
public:
    virtual ~ByteReader() = default;
    std::string_view data{};
    std::size_t pos{0};
    bool failed{false};

    ByteReader(std::string_view d = {}) : data{d}, pos{0}, failed{false} {}

    bool read(char* dest, std::size_t size) {
        if (pos + size > data.size()) {
            failed = true;
            return false;
        }
        [[scpp::unsafe]] {
            for (std::size_t i = 0; i < size; i++) {
                dest[i] = data.at(pos + i);
            }
        }
        pos += size;
        return true;
    }

    bool read(std::uint8_t* dest, std::size_t size) {
        if (pos + size > data.size()) {
            failed = true;
            return false;
        }
        [[scpp::unsafe]] {
            for (std::size_t i = 0; i < size; i++) {
                dest[i] = static_cast<std::uint8_t>(data.at(pos + i));
            }
        }
        pos += size;
        return true;
    }

    explicit operator bool() const { return !failed; }
};

inline void write_u32_le(ByteWriter& out, std::uint32_t value) {
    char bytes[4] = {
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8) & 0xffu),
        static_cast<char>((value >> 16) & 0xffu),
        static_cast<char>((value >> 24) & 0xffu)
    };
    out.write(&bytes[0], 4);
}


[[nodiscard]] inline bool is_exported_generic_type_name(const scpp::Program& program, std::string_view name) {
    for (const scpp::StructDef& def : program.structs) {
        if (!def.is_exported || def.name != name) continue;
        if (!def.template_params.empty()) return true;
    }
    for (const scpp::ClassDef& def : program.classes) {
        if (!def.is_exported || def.name != name) continue;
        if (!def.template_params.empty() || def.is_variadic_primary_template || def.is_variadic_specialization ||
            def.is_partial_specialization) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool is_compile_time_root(const scpp::Program& program, const scpp::Function& fn) {
    if (!fn.member_owner_class.empty() && is_exported_generic_type_name(program, fn.member_owner_class)) {
        return true;
    }
    if (!fn.is_exported) return false;
    if (fn.eval_mode != scpp::FunctionEvalMode::RuntimeOnly || fn.is_generic_template || !fn.template_params.empty()) {
        return true;
    }
    return false;
}

inline void collect_type_names(const scpp::Type& type, std::vector<std::string>& out) {
    if (type.kind == scpp::TypeKind::Named && !type.name.empty()) out.push_back(type.name);
    if (type.pointee) collect_type_names(*type.pointee, out);
    if (type.function_return) collect_type_names(*type.function_return, out);
    for (const scpp::Type& arg : type.template_args) collect_type_names(arg, out);
    for (const scpp::Type& param : type.function_params) collect_type_names(param, out);
}

inline void collect_stmt_edges(const scpp::Stmt& stmt, const std::unordered_set<std::string>& known_function_names,
                               const std::unordered_set<std::string>& bound_names,
                               std::vector<std::string>& function_names,
                               std::vector<std::string>& type_names);

inline void collect_expr_edges(const scpp::Expr& expr, const std::unordered_set<std::string>& known_function_names,
                               const std::unordered_set<std::string>& bound_names,
                               std::vector<std::string>& function_names,
                               std::vector<std::string>& type_names);

inline void collect_type_edges(const scpp::Type& type, const std::unordered_set<std::string>& known_function_names,
                               const std::unordered_set<std::string>& bound_names,
                               std::vector<std::string>& function_names,
                               std::vector<std::string>& type_names) {
    if (type.kind == scpp::TypeKind::Named && !type.name.empty()) type_names.push_back(type.name);
    if (type.pointee) collect_type_edges(*type.pointee, known_function_names, bound_names, function_names, type_names);
    if (type.function_return) {
        collect_type_edges(*type.function_return, known_function_names, bound_names, function_names, type_names);
    }
    for (const scpp::Type& arg : type.template_args) {
        collect_type_edges(arg, known_function_names, bound_names, function_names, type_names);
    }
    for (const scpp::Type& param : type.function_params) {
        collect_type_edges(param, known_function_names, bound_names, function_names, type_names);
    }
    for (const std::shared_ptr<scpp::Expr>& arg : type.non_type_args) {
        if (arg) collect_expr_edges(*arg, known_function_names, bound_names, function_names, type_names);
    }
}

[[nodiscard]] inline bool identifier_expr_might_name_function(const scpp::Expr& expr,
                                                              const std::unordered_set<std::string>& known_function_names,
                                                              const std::unordered_set<std::string>& bound_names) {
    if (expr.kind != scpp::ExprKind::Identifier || expr.name.empty()) return false;
    if (!known_function_names.contains(expr.name)) return false;
    if (expr.explicit_global_qualification || expr.name.find("::") != std::string::npos) return true;
    return !bound_names.contains(expr.name);
}

inline void collect_expr_edges(const scpp::Expr& expr, const std::unordered_set<std::string>& known_function_names,
                        const std::unordered_set<std::string>& bound_names,
                        std::vector<std::string>& function_names,
                        std::vector<std::string>& type_names) {
    collect_type_edges(expr.type, known_function_names, bound_names, function_names, type_names);
    if ((expr.kind == scpp::ExprKind::Call || identifier_expr_might_name_function(expr, known_function_names, bound_names)) &&
        !expr.name.empty()) {
        function_names.push_back(expr.name);
    }
    if (expr.kind == scpp::ExprKind::New) {
        collect_type_edges(expr.type, known_function_names, bound_names, function_names, type_names);
    }
    if (expr.lhs) collect_expr_edges(*expr.lhs, known_function_names, bound_names, function_names, type_names);
    if (expr.rhs) collect_expr_edges(*expr.rhs, known_function_names, bound_names, function_names, type_names);
    if (expr.third) collect_expr_edges(*expr.third, known_function_names, bound_names, function_names, type_names);
    for (const auto& arg : expr.args) {
        collect_expr_edges(*arg, known_function_names, bound_names, function_names, type_names);
    }
    for (const scpp::ExplicitTemplateArg& arg : expr.explicit_template_args) {
        if (arg.is_type) {
            collect_type_edges(arg.type, known_function_names, bound_names, function_names, type_names);
        } else if (arg.value) {
            collect_expr_edges(*arg.value, known_function_names, bound_names, function_names, type_names);
        }
    }
    std::unordered_set<std::string> lambda_bound_names = bound_names;
    for (const scpp::LambdaCapture& capture : expr.lambda_captures) {
        if (capture.init) collect_expr_edges(*capture.init, known_function_names, bound_names, function_names, type_names);
        lambda_bound_names.insert(capture.name);
    }
    for (const scpp::Param& param : expr.lambda_params) {
        collect_type_edges(param.type, known_function_names, lambda_bound_names, function_names, type_names);
        lambda_bound_names.insert(param.name);
    }
    if (expr.lambda_body) {
        collect_stmt_edges(*expr.lambda_body, known_function_names, lambda_bound_names, function_names, type_names);
    }
}

inline void collect_stmt_edges(const scpp::Stmt& stmt, const std::unordered_set<std::string>& known_function_names,
                        const std::unordered_set<std::string>& bound_names,
                        std::vector<std::string>& function_names,
                        std::vector<std::string>& type_names) {
    collect_type_edges(stmt.type, known_function_names, bound_names, function_names, type_names);
    if (stmt.init) collect_expr_edges(*stmt.init, known_function_names, bound_names, function_names, type_names);
    for (const auto& ctor_arg : stmt.ctor_args) {
        collect_expr_edges(*ctor_arg, known_function_names, bound_names, function_names, type_names);
    }
    if (stmt.has_ctor_args && stmt.type.kind == scpp::TypeKind::Named && !stmt.type.name.empty()) {
        function_names.push_back(stmt.type.name + "_new");
    }
    if (stmt.expr) collect_expr_edges(*stmt.expr, known_function_names, bound_names, function_names, type_names);
    if (stmt.condition) {
        collect_expr_edges(*stmt.condition, known_function_names, bound_names, function_names, type_names);
    }
    if (stmt.then_branch) {
        collect_stmt_edges(*stmt.then_branch, known_function_names, bound_names, function_names, type_names);
    }
    if (stmt.else_branch) {
        collect_stmt_edges(*stmt.else_branch, known_function_names, bound_names, function_names, type_names);
    }
    if (stmt.kind == scpp::StmtKind::Switch) {
        std::unordered_set<std::string> switch_bound_names = bound_names;
        for (const auto& switch_case : stmt.switch_cases) {
            if (switch_case.value) {
                collect_expr_edges(*switch_case.value, known_function_names, switch_bound_names, function_names, type_names);
            }
            for (const auto& nested : switch_case.statements) {
                collect_stmt_edges(*nested, known_function_names, switch_bound_names, function_names, type_names);
                if (nested->kind == scpp::StmtKind::VarDecl && !nested->var_name.empty()) {
                    switch_bound_names.insert(nested->var_name);
                }
            }
        }
    }
    std::unordered_set<std::string> block_bound_names = bound_names;
    if (stmt.kind == scpp::StmtKind::VarDecl && !stmt.var_name.empty()) block_bound_names.insert(stmt.var_name);
    for (const auto& nested : stmt.statements) {
        collect_stmt_edges(*nested, known_function_names, block_bound_names, function_names, type_names);
        if (nested->kind == scpp::StmtKind::VarDecl && !nested->var_name.empty()) {
            block_bound_names.insert(nested->var_name);
        }
    }
}

inline void collect_function_signature_types(const scpp::Function& fn,
                                     const std::unordered_set<std::string>& known_function_names,
                                     std::vector<std::string>& function_names,
                                     std::vector<std::string>& type_names) {
    std::unordered_set<std::string> empty_bound_names{};
    collect_type_edges(fn.return_type, known_function_names, empty_bound_names, function_names, type_names);
    for (const scpp::Param& param : fn.params) {
        collect_type_edges(param.type, known_function_names, empty_bound_names, function_names, type_names);
    }
}

inline void collect_function_reachable_edges(const scpp::Function& fn, const std::unordered_set<std::string>& known_function_names,
                                     std::vector<std::string>& function_names,
                                     std::vector<std::string>& type_names) {
    collect_function_signature_types(fn, known_function_names, function_names, type_names);
    std::unordered_set<std::string> bound_names{};
    for (const scpp::Param& param : fn.params) {
        if (!param.name.empty()) bound_names.insert(param.name);
    }
    for (const scpp::MemberInitializer& init : fn.member_initializers) {
        if (init.initializer.expr) {
            collect_expr_edges(*init.initializer.expr, known_function_names, bound_names, function_names, type_names);
        }
        for (const scpp::ExprPtr& arg : init.initializer.brace_args) {
            collect_expr_edges(*arg, known_function_names, bound_names, function_names, type_names);
        }
    }
    if (fn.body) collect_stmt_edges(*fn.body, known_function_names, bound_names, function_names, type_names);
}

inline void collect_class_reachable_edges(const scpp::ClassDef& def, const std::unordered_set<std::string>& known_function_names,
                                   const std::vector<std::string>& known_function_names_list,
                                   std::vector<std::string>& function_names,
                                   std::vector<std::string>& type_names) {
    if (!def.name.empty()) {
        function_names.push_back(def.name + "_new");
        function_names.push_back(def.name + "_delete");
        for (const std::string& known_name : known_function_names_list) {
            if (known_name.starts_with(def.name + "_")) function_names.push_back(known_name);
        }
    }
    std::unordered_set<std::string> empty_bound_names{};
    for (const scpp::ClassField& field : def.fields) {
        collect_type_edges(field.type, known_function_names, empty_bound_names, function_names, type_names);
        if (!field.default_initializer) continue;
        if (field.default_initializer->expr) {
            collect_expr_edges(*field.default_initializer->expr, known_function_names, empty_bound_names, function_names,
                               type_names);
        }
        for (const scpp::ExprPtr& arg : field.default_initializer->brace_args) {
            collect_expr_edges(*arg, known_function_names, empty_bound_names, function_names, type_names);
        }
        if (field.default_initializer->has_brace_args && field.type.kind == scpp::TypeKind::Named && !field.type.name.empty()) {
            function_names.push_back(field.type.name + "_new");
        }
    }
    for (const scpp::BaseSpecifier& base : def.base_specifiers) {
        collect_type_edges(base.base_type, known_function_names, empty_bound_names, function_names, type_names);
    }
    for (const scpp::Type& arg : def.specialization_template_args) {
        collect_type_edges(arg, known_function_names, empty_bound_names, function_names, type_names);
    }
    if (def.thread_movable_if_movable_expr) {
        collect_expr_edges(*def.thread_movable_if_movable_expr, known_function_names, empty_bound_names, function_names,
                           type_names);
    }
    if (def.thread_movable_if_shareable_expr) {
        collect_expr_edges(*def.thread_movable_if_shareable_expr, known_function_names, empty_bound_names, function_names,
                           type_names);
    }
}

inline void walk_constexpr_stmt(const Stmt& stmt) {
    if (stmt.then_branch) walk_constexpr_stmt(*stmt.then_branch);
    if (stmt.else_branch) walk_constexpr_stmt(*stmt.else_branch);
    for (const StmtPtr& nested : stmt.statements) walk_constexpr_stmt(*nested);
}

inline void reject_not_yet_lowerable_constexpr_surface(const Program& program) {
    for (const Function& fn : program.functions) {
        if (fn.body) walk_constexpr_stmt(*fn.body);
    }
}

class PayloadCollector {
public:
    virtual ~PayloadCollector() = default;
    const Program& program;
    const std::unordered_map<std::string, std::vector<std::size_t>>& function_indices_by_name;
    std::unordered_set<std::size_t> visited_function_indices{};
    std::unordered_set<std::string> visited_types{};
    std::vector<std::string> pending_types{};
    std::vector<std::size_t> worklist{};
    std::vector<std::string> reachable_type_names{};
    std::vector<std::size_t> reachable_function_indices{};
    std::vector<std::string> root_function_names{};

    PayloadCollector(const Program& p, const std::unordered_map<std::string, std::vector<std::size_t>>& f)
        : program{p}, function_indices_by_name{f} {}

    void enqueue_type(const std::string& name) {
        if (name.empty()) return;
        if (visited_types.insert(name).second) {
            reachable_type_names.push_back(name);
            pending_types.push_back(name);
        }
    }

    void enqueue_function_index(std::size_t index, bool is_root = false) {
        if (index >= program.functions.size()) return;
        const Function& fn = program.functions[index];
        if (visited_function_indices.insert(index).second) {
            reachable_function_indices.push_back(index);
            worklist.push_back(index);
        }
        if (is_root) {
            bool found = false;
            for (const std::string& root : root_function_names) {
                if (root == fn.name) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                root_function_names.push_back(fn.name);
            }
        }
    }

    void enqueue_functions_by_name(const std::string& name) {
        if (name.empty()) return;
        auto it = function_indices_by_name.find(name);
        if (it == function_indices_by_name.end()) return;
        for (std::size_t index : it->second) enqueue_function_index(index);
    }
};

CompileTimePayloadPlan plan_compile_time_payload(const Program& program) {
    CompileTimePayloadPlan plan{};
    std::unordered_map<std::string, std::vector<std::size_t>> function_indices_by_name{};
    std::unordered_set<std::string> known_function_names{};
    std::vector<std::string> known_function_names_list{};
    std::unordered_map<std::string, std::size_t> enums_by_name{};
    std::unordered_map<std::string, std::size_t> structs_by_name{};
    std::unordered_map<std::string, std::size_t> classes_by_name{};
    std::unordered_map<std::string, std::vector<std::size_t>> methods_by_owner{};
    for (std::size_t i = 0; i < program.functions.size(); i++) {
        function_indices_by_name[program.functions[i].name].push_back(i);
        known_function_names.insert(program.functions[i].name);
        known_function_names_list.push_back(program.functions[i].name);
        if (!program.functions[i].member_owner_class.empty()) {
            methods_by_owner[program.functions[i].member_owner_class].push_back(i);
        }
    }
    for (std::size_t i = 0; i < program.enums.size(); i++) enums_by_name.emplace(program.enums[i].name, i);
    for (std::size_t i = 0; i < program.structs.size(); i++) structs_by_name.emplace(program.structs[i].name, i);
    for (std::size_t i = 0; i < program.classes.size(); i++) classes_by_name.emplace(program.classes[i].name, i);

    PayloadCollector collector{program, function_indices_by_name};

    for (std::size_t i = 0; i < program.functions.size(); i++) {
        if (is_compile_time_root(program, program.functions[i])) collector.enqueue_function_index(i, true);
    }

    std::size_t next_function_index = 0;
    while (next_function_index < collector.worklist.size() || !collector.pending_types.empty()) {
        while (next_function_index < collector.worklist.size()) {
            const Function& fn = program.functions[collector.worklist[next_function_index++]];
            std::vector<std::string> local_function_names{};
            std::vector<std::string> local_type_names{};
            collect_function_reachable_edges(fn, known_function_names, local_function_names, local_type_names);
            for (const std::string& callee : local_function_names) collector.enqueue_functions_by_name(callee);
            for (const std::string& type_name : local_type_names) collector.enqueue_type(type_name);
        }

        if (collector.pending_types.empty()) continue;
        std::vector<std::string> batch = collector.pending_types;
        collector.pending_types.clear();
        for (const std::string& type_name : batch) {
            if (auto it = enums_by_name.find(type_name); it != enums_by_name.end()) {
                const EnumDef& def = program.enums[it->second];
                std::vector<std::string> nested{};
                collect_type_names(def.underlying_type, nested);
                for (const std::string& nested_name : nested) collector.enqueue_type(nested_name);
            }
            if (auto it = structs_by_name.find(type_name); it != structs_by_name.end()) {
                const StructDef& def = program.structs[it->second];
                for (const StructField& field : def.fields) {
                    std::vector<std::string> nested{};
                    collect_type_names(field.type, nested);
                    for (const std::string& nested_name : nested) collector.enqueue_type(nested_name);
                }
            }
            if (auto it = classes_by_name.find(type_name); it != classes_by_name.end()) {
                const ClassDef& def = program.classes[it->second];
                std::vector<std::string> nested_function_names{};
                std::vector<std::string> nested_type_names{};
                collect_class_reachable_edges(def, known_function_names, known_function_names_list, nested_function_names, nested_type_names);
                for (const std::string& nested_name : nested_type_names) collector.enqueue_type(nested_name);
                for (const std::string& function_name : nested_function_names) collector.enqueue_functions_by_name(function_name);
                if (auto method_it = methods_by_owner.find(type_name); method_it != methods_by_owner.end()) {
                    for (std::size_t method_index : method_it->second) {
                        collector.enqueue_function_index(method_index);
                    }
                }
            }
        }
    }

    plan.reachable_type_names = collector.reachable_type_names;
    plan.reachable_function_indices = collector.reachable_function_indices;
    plan.root_function_names = collector.root_function_names;
    return plan;
}

class StructuredCompileTimePayload {
public:
    virtual ~StructuredCompileTimePayload() = default;
    std::vector<std::string> root_function_names{};
    std::vector<EnumDef> enums{};
    std::vector<StructDef> structs{};
    std::vector<ClassDef> classes{};
    std::vector<Function> functions{};
};

class LoadedModuleFile {
public:
    virtual ~LoadedModuleFile() = default;
    std::string interface_source{};
    bool is_scppm{false};
    bool has_compile_time_payload{false};
    std::string compile_time_payload_bytes{};
};

inline void write_u8(ByteWriter& out, std::uint8_t value) { out.put(static_cast<char>(value)); }
inline void write_u8(ByteWriter& out, int value) { out.put(static_cast<char>(value)); }
inline void write_u8(ByteWriter& out, unsigned int value) { out.put(static_cast<char>(value)); }

[[nodiscard]] inline std::expected<std::uint8_t, DriverError> read_u8(ByteReader& in, const std::string& context) {
    char byte = '\0';
    if (!in.read(&byte, 1)) return std::unexpected(DriverError("invalid " + context + ": truncated byte"));
    return static_cast<std::uint8_t>(byte);
}

inline void write_i64_le(ByteWriter& out, std::int64_t value) {
    char bytes[8] = {};
    std::uint64_t raw = static_cast<std::uint64_t>(value);
    for (std::size_t i = 0; i < 8; i++) bytes[i] = static_cast<char>((raw >> (8 * i)) & 0xffu);
    out.write(&bytes[0], 8);
}

inline void write_u64_le(ByteWriter& out, std::uint64_t value) {
    char bytes[8] = {};
    for (std::size_t i = 0; i < 8; i++) bytes[i] = static_cast<char>((value >> (8 * i)) & 0xffu);
    out.write(&bytes[0], 8);
}

[[nodiscard]] inline std::expected<std::uint32_t, DriverError> read_u32_le(ByteReader& in, const std::string& context) {
    std::uint8_t bytes[4] = {};
    if (!in.read(bytes, 4)) return std::unexpected(DriverError("invalid " + context + ": truncated u32"));
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

[[nodiscard]] inline std::expected<std::int64_t, DriverError> read_i64_le(ByteReader& in, const std::string& context) {
    std::uint8_t bytes[8] = {};
    if (!in.read(bytes, 8)) return std::unexpected(DriverError("invalid " + context + ": truncated i64"));
    std::uint64_t raw = 0;
    for (std::size_t i = 0; i < 8; i++) raw |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    return static_cast<std::int64_t>(raw);
}

[[nodiscard]] inline std::expected<std::uint64_t, DriverError> read_u64_le(ByteReader& in, const std::string& context) {
    std::uint8_t bytes[8] = {};
    if (!in.read(bytes, 8)) return std::unexpected(DriverError("invalid " + context + ": truncated u64"));
    std::uint64_t raw = 0;
    for (std::size_t i = 0; i < 8; i++) raw |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    return raw;
}

inline void write_double_le(ByteWriter& out, double value) {
    std::uint64_t raw = 0;
    [[scpp::unsafe]] {
        const void* ptr = &value;
        const std::uint64_t* raw_ptr = static_cast<const std::uint64_t*>(ptr);
        raw = *raw_ptr;
    }
    write_i64_le(out, static_cast<std::int64_t>(raw));
}

[[nodiscard]] inline std::expected<double, DriverError> read_double_le(ByteReader& in, const std::string& context) {
    auto raw_result = read_i64_le(in, context);
    if (!raw_result.has_value()) return std::unexpected(std::move(raw_result).error());
    std::uint64_t raw = static_cast<std::uint64_t>(raw_result.value());
    double value = 0.0;
    [[scpp::unsafe]] {
        const void* raw_ptr = &raw;
        const double* ptr = static_cast<const double*>(raw_ptr);
        value = *ptr;
    }
    return value;
}

inline void write_string(ByteWriter& out, std::string_view text) {
    write_u32_le(out, static_cast<std::uint32_t>(text.size()));
    out.write(text.data(), text.size());
}

[[nodiscard]] inline std::expected<std::string, DriverError> read_string(ByteReader& in, const std::string& context) {
    auto size_result = read_u32_le(in, context + " string length");
    if (!size_result.has_value()) return std::unexpected(std::move(size_result).error());
    std::size_t size = static_cast<std::size_t>(size_result.value());
    if (in.pos + size > in.data.size()) {
        in.failed = true;
        return std::unexpected(DriverError("invalid " + context + ": truncated string"));
    }
    std::string text = string_from_view(in.data.substr(in.pos, size));
    in.pos += size;
    return text;
}

inline void write_source_location(ByteWriter& out, const SourceLocation& loc) {
    write_i64_le(out, static_cast<std::int64_t>(loc.line));
    write_i64_le(out, static_cast<std::int64_t>(loc.column));
    write_string(out, loc.source_path_text());
}

[[nodiscard]] inline std::expected<SourceLocation, DriverError> read_source_location(ByteReader& in, const std::string& context) {
    SourceLocation loc{};
    auto line_result = read_i64_le(in, context + " line");
    if (!line_result.has_value()) return std::unexpected(std::move(line_result).error());
    loc.line = static_cast<int>(line_result.value());
    auto column_result = read_i64_le(in, context + " column");
    if (!column_result.has_value()) return std::unexpected(std::move(column_result).error());
    loc.column = static_cast<int>(column_result.value());
    auto source_path_result = read_string(in, context + " source path");
    if (!source_path_result.has_value()) return std::unexpected(std::move(source_path_result).error());
    std::string source_path = std::move(source_path_result).value();
    if (!source_path.empty()) loc.source_path = std::make_shared<const std::string>(std::move(source_path));
    return loc;
}

template<typename Enum>
inline void write_enum(ByteWriter& out, Enum value) {
    write_u8(out, static_cast<std::uint8_t>(value));
}

template<typename T, typename U>
bool __enum_cast_store(U value [[maybe_unused]], T& out [[maybe_unused]]) {
    [[scpp::unsafe]] {
        auto ptr = static_cast<U*>(static_cast<void*>(&out));
        *ptr = value;
    }
    return true;
}

template<typename Enum>
[[nodiscard]] inline std::expected<Enum, DriverError> read_enum(ByteReader& in, const std::string& context) {
    auto value_result = read_u8(in, context);
    if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
    Enum converted{};
    if (!scpp::__enum_cast_store<Enum>(value_result.value(), converted)) {
        return std::unexpected(DriverError("invalid " + context + ": unknown enum tag " + std::to_string(static_cast<std::int64_t>(value_result.value()))));
    }
    std::expected<Enum, DriverError> result{converted};
    return std::move(result);
}

inline void write_type(ByteWriter& out, const Type& type);
[[nodiscard]] inline std::expected<ExprPtr, DriverError> read_expr(ByteReader& in, const std::string& context);
inline void write_expr(ByteWriter& out, const Expr& expr);
[[nodiscard]] inline std::expected<StmtPtr, DriverError> read_stmt(ByteReader& in, const std::string& context);
inline void write_stmt(ByteWriter& out, const Stmt& stmt);
inline void write_alignment_specifier(ByteWriter& out, const AlignmentSpecifier& spec);
[[nodiscard]] inline std::expected<AlignmentSpecifier, DriverError> read_alignment_specifier(ByteReader& in, const std::string& context);

inline void write_type(ByteWriter& out, const Type& type) {
    write_enum(out, type.kind);
    write_string(out, type.name);
    write_u8(out, type.pointee ? 1u : 0u);
    if (type.pointee) write_type(out, *type.pointee);
    write_u8(out, type.element ? 1u : 0u);
    if (type.element) write_type(out, *type.element);
    write_i64_le(out, type.array_size);
    // ch05 §9.4: a still-generic exported class/struct's field may have an
    // array bound that's deliberately left unresolved until an importing
    // module's own monomorphization substitutes a concrete type in for the
    // template parameter it depends on (e.g. `char storage[sizeof(T)]`) --
    // array_size stays 0 and array_size_expr must round-trip through
    // .scppm so the importer has something to substitute into, exactly
    // like non_type_args below.
    write_u8(out, type.array_size_expr ? 1u : 0u);
    if (type.array_size_expr) write_expr(out, *type.array_size_expr);
    write_u8(out, type.function_return ? 1u : 0u);
    if (type.function_return) write_type(out, *type.function_return);
    write_u32_le(out, static_cast<std::uint32_t>(type.function_params.size()));
    for (const Type& param : type.function_params) write_type(out, param);
    write_u8(out, type.is_unsafe_function_pointer ? 1u : 0u);
    write_u8(out, type.is_const_function ? 1u : 0u);
    write_enum(out, type.function_ref_qualifier);
    write_u8(out, type.is_mutable_ref ? 1u : 0u);
    write_u8(out, type.is_rvalue_ref ? 1u : 0u);
    write_u8(out, type.is_mutable_pointee ? 1u : 0u);
    write_u8(out, type.is_const_qualified ? 1u : 0u);
    write_u8(out, type.is_reference_wrapper_lifetime_source ? 1u : 0u);
    write_u32_le(out, static_cast<std::uint32_t>(type.template_args.size()));
    for (const Type& arg : type.template_args) write_type(out, arg);
    write_u32_le(out, static_cast<std::uint32_t>(type.non_type_args.size()));
    for (const auto& arg : type.non_type_args) {
        write_u8(out, arg ? 1u : 0u);
        if (arg) write_expr(out, *arg);
    }
    write_u8(out, type.is_pack_expansion ? 1u : 0u);
}

[[nodiscard]] inline std::expected<Type, DriverError> read_type(ByteReader& in, const std::string& context) {
    Type type{};
    {
        auto kind_result = scpp::read_enum<TypeKind>(in, context + " kind");
        if (!kind_result.has_value()) return std::unexpected(std::move(kind_result).error());
        type.kind = kind_result.value();
    }
    {
        auto name_result = read_string(in, context + " name");
        if (!name_result.has_value()) return std::unexpected(std::move(name_result).error());
        type.name = std::move(name_result).value();
    }
    {
        auto present_result = read_u8(in, context + " pointee present");
        if (!present_result.has_value()) return std::unexpected(std::move(present_result).error());
        if (present_result.value() != 0u) {
            auto pointee_result = read_type(in, context + " pointee");
            if (!pointee_result.has_value()) return std::unexpected(std::move(pointee_result).error());
            type.pointee = std::make_shared<Type>(std::move(pointee_result).value());
        }
    }
    {
        auto present_result = read_u8(in, context + " element present");
        if (!present_result.has_value()) return std::unexpected(std::move(present_result).error());
        if (present_result.value() != 0u) {
            auto element_result = read_type(in, context + " element");
            if (!element_result.has_value()) return std::unexpected(std::move(element_result).error());
            type.element = std::make_shared<Type>(std::move(element_result).value());
        }
    }
    {
        auto array_size_result = read_i64_le(in, context + " array size");
        if (!array_size_result.has_value()) return std::unexpected(std::move(array_size_result).error());
        type.array_size = array_size_result.value();
    }
    {
        auto present_result = read_u8(in, context + " array size expr present");
        if (!present_result.has_value()) return std::unexpected(std::move(present_result).error());
        if (present_result.value() != 0u) {
            auto expr_result = read_expr(in, context + " array size expr");
            if (!expr_result.has_value()) return std::unexpected(std::move(expr_result).error());
            type.array_size_expr = std::shared_ptr<Expr>(std::move(expr_result).value().release());
        }
    }
    {
        auto present_result = read_u8(in, context + " function return present");
        if (!present_result.has_value()) return std::unexpected(std::move(present_result).error());
        if (present_result.value() != 0u) {
            auto return_result = read_type(in, context + " function return");
            if (!return_result.has_value()) return std::unexpected(std::move(return_result).error());
            type.function_return = std::make_shared<Type>(std::move(return_result).value());
        }
    }
    std::uint32_t param_count = 0;
    {
        auto count_result = read_u32_le(in, context + " function param count");
        if (!count_result.has_value()) return std::unexpected(std::move(count_result).error());
        param_count = count_result.value();
    }
    type.function_params.reserve(static_cast<std::size_t>(param_count));
    for (std::uint32_t i = 0; i < param_count; i++) {
        auto param_result = read_type(in, context + " function param");
        if (!param_result.has_value()) return std::unexpected(std::move(param_result).error());
        type.function_params.push_back(std::move(param_result).value());
    }
    {
        auto flag_result = read_u8(in, context + " unsafe fn ptr");
        if (!flag_result.has_value()) return std::unexpected(std::move(flag_result).error());
        type.is_unsafe_function_pointer = flag_result.value() != 0u;
    }
    {
        auto flag_result = read_u8(in, context + " const fn");
        if (!flag_result.has_value()) return std::unexpected(std::move(flag_result).error());
        type.is_const_function = flag_result.value() != 0u;
    }
    {
        auto qualifier_result = scpp::read_enum<ReceiverRefQualifier>(in, context + " fn ref qualifier");
        if (!qualifier_result.has_value()) return std::unexpected(std::move(qualifier_result).error());
        type.function_ref_qualifier = qualifier_result.value();
    }
    {
        auto flag_result = read_u8(in, context + " mutable ref");
        if (!flag_result.has_value()) return std::unexpected(std::move(flag_result).error());
        type.is_mutable_ref = flag_result.value() != 0u;
    }
    {
        auto flag_result = read_u8(in, context + " rvalue ref");
        if (!flag_result.has_value()) return std::unexpected(std::move(flag_result).error());
        type.is_rvalue_ref = flag_result.value() != 0u;
    }
    {
        auto flag_result = read_u8(in, context + " mutable pointee");
        if (!flag_result.has_value()) return std::unexpected(std::move(flag_result).error());
        type.is_mutable_pointee = flag_result.value() != 0u;
    }
    {
        auto flag_result = read_u8(in, context + " const qualified");
        if (!flag_result.has_value()) return std::unexpected(std::move(flag_result).error());
        type.is_const_qualified = flag_result.value() != 0u;
    }
    {
        auto flag_result = read_u8(in, context + " ref_wrapper lifetime source");
        if (!flag_result.has_value()) return std::unexpected(std::move(flag_result).error());
        type.is_reference_wrapper_lifetime_source = flag_result.value() != 0u;
    }
    std::uint32_t template_arg_count = 0;
    {
        auto count_result = read_u32_le(in, context + " template arg count");
        if (!count_result.has_value()) return std::unexpected(std::move(count_result).error());
        template_arg_count = count_result.value();
    }
    type.template_args.reserve(static_cast<std::size_t>(template_arg_count));
    for (std::uint32_t i = 0; i < template_arg_count; i++) {
        auto arg_result = read_type(in, context + " template arg");
        if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
        type.template_args.push_back(std::move(arg_result).value());
    }
    std::uint32_t non_type_arg_count = 0;
    {
        auto count_result = read_u32_le(in, context + " non-type arg count");
        if (!count_result.has_value()) return std::unexpected(std::move(count_result).error());
        non_type_arg_count = count_result.value();
    }
    type.non_type_args.reserve(static_cast<std::size_t>(non_type_arg_count));
    for (std::uint32_t i = 0; i < non_type_arg_count; i++) {
        auto present_result = read_u8(in, context + " non-type arg present");
        if (!present_result.has_value()) return std::unexpected(std::move(present_result).error());
        if (present_result.value() != 0u) {
            auto arg_result = read_expr(in, context + " non-type arg");
            if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
            type.non_type_args.push_back(std::shared_ptr<Expr>(std::move(arg_result).value().release()));
        } else {
            type.non_type_args.push_back(nullptr);
        }
    }
    {
        auto flag_result = read_u8(in, context + " pack expansion");
        if (!flag_result.has_value()) return std::unexpected(std::move(flag_result).error());
        type.is_pack_expansion = flag_result.value() != 0u;
    }
    return type;
}


inline void write_generic_type_param(ByteWriter& out, const GenericTypeParam& param) {
    write_string(out, param.name);
    write_string(out, param.concept_name);
    write_u8(out, param.is_pack ? 1u : 0u);
    write_u8(out, param.is_non_type ? 1u : 0u);
    write_type(out, param.non_type_type);
}

[[nodiscard]] inline std::expected<GenericTypeParam, DriverError> read_generic_type_param(ByteReader& in, const std::string& context) {
    GenericTypeParam param{};
    auto name_r = read_string(in, context + " name");
    if (!name_r.has_value()) return std::unexpected(std::move(name_r).error());
    param.name = std::move(name_r).value();
    auto concept_r = read_string(in, context + " concept");
    if (!concept_r.has_value()) return std::unexpected(std::move(concept_r).error());
    param.concept_name = std::move(concept_r).value();
    auto is_pack_r = read_u8(in, context + " is_pack");
    if (!is_pack_r.has_value()) return std::unexpected(std::move(is_pack_r).error());
    param.is_pack = is_pack_r.value() != 0u;
    auto is_non_type_r = read_u8(in, context + " is_non_type");
    if (!is_non_type_r.has_value()) return std::unexpected(std::move(is_non_type_r).error());
    param.is_non_type = is_non_type_r.value() != 0u;
    auto non_type_type_r = read_type(in, context + " non-type type");
    if (!non_type_type_r.has_value()) return std::unexpected(std::move(non_type_type_r).error());
    param.non_type_type = std::move(non_type_type_r).value();
    return param;
}

// Structurally bound; see `write_expr`.
inline void write_param(ByteWriter& out, const Param& param) {
    write_type(out, param.type);
    write_string(out, param.name);
    write_u64_le(out, static_cast<std::uint64_t>(param.resolved_local));
    write_string(out, param.lifetime.name);
    write_u8(out, param.default_expr ? 1u : 0u);
    if (param.default_expr) write_expr(out, *param.default_expr);
    write_string(out, param.generic_concept);
    write_u8(out, param.require_thread_movable ? 1u : 0u);
    write_u8(out, param.require_thread_shareable ? 1u : 0u);
    write_u8(out, param.is_parameter_pack ? 1u : 0u);
    write_u8(out, param.is_const ? 1u : 0u);
    write_source_location(out, param.loc);
}

[[nodiscard]] inline std::expected<Param, DriverError> read_param(ByteReader& in, const std::string& context) {
    Param param{};
    auto type_r = read_type(in, context + " type");
    if (!type_r.has_value()) return std::unexpected(std::move(type_r).error());
    param.type = std::move(type_r).value();
    auto name_r = read_string(in, context + " name");
    if (!name_r.has_value()) return std::unexpected(std::move(name_r).error());
    param.name = std::move(name_r).value();
    auto resolved_local_r = read_u64_le(in, context + " resolved local");
    if (!resolved_local_r.has_value()) return std::unexpected(std::move(resolved_local_r).error());
    param.resolved_local = static_cast<std::size_t>(resolved_local_r.value());
    auto lifetime_r = read_string(in, context + " lifetime");
    if (!lifetime_r.has_value()) return std::unexpected(std::move(lifetime_r).error());
    param.lifetime.name = std::move(lifetime_r).value();
    auto default_present_r = read_u8(in, context + " default expr present");
    if (!default_present_r.has_value()) return std::unexpected(std::move(default_present_r).error());
    if (default_present_r.value() != 0u) {
        auto default_expr_r = read_expr(in, context + " default expr");
        if (!default_expr_r.has_value()) return std::unexpected(std::move(default_expr_r).error());
        param.default_expr = std::shared_ptr<Expr>(std::move(default_expr_r).value().release());
    }
    auto generic_concept_r = read_string(in, context + " generic concept");
    if (!generic_concept_r.has_value()) return std::unexpected(std::move(generic_concept_r).error());
    param.generic_concept = std::move(generic_concept_r).value();
    auto thread_movable_r = read_u8(in, context + " thread_movable");
    if (!thread_movable_r.has_value()) return std::unexpected(std::move(thread_movable_r).error());
    param.require_thread_movable = thread_movable_r.value() != 0u;
    auto thread_shareable_r = read_u8(in, context + " thread_shareable");
    if (!thread_shareable_r.has_value()) return std::unexpected(std::move(thread_shareable_r).error());
    param.require_thread_shareable = thread_shareable_r.value() != 0u;
    auto pack_r = read_u8(in, context + " parameter pack");
    if (!pack_r.has_value()) return std::unexpected(std::move(pack_r).error());
    param.is_parameter_pack = pack_r.value() != 0u;
    auto is_const_r = read_u8(in, context + " is_const");
    if (!is_const_r.has_value()) return std::unexpected(std::move(is_const_r).error());
    param.is_const = is_const_r.value() != 0u;
    auto loc_r = read_source_location(in, context + " loc");
    if (!loc_r.has_value()) return std::unexpected(std::move(loc_r).error());
    param.loc = loc_r.value();
    return param;
}

// Structurally bound; see `write_expr`.
inline void write_lambda_capture(ByteWriter& out, const LambdaCapture& capture) {
    write_string(out, capture.name);
    write_u8(out, capture.by_reference ? 1u : 0u);
    write_u64_le(out, static_cast<std::uint64_t>(capture.resolved_local));
    write_u8(out, capture.from_enclosing_closure ? 1u : 0u);
    write_u8(out, capture.init ? 1u : 0u);
    if (capture.init) write_expr(out, *capture.init);
}

[[nodiscard]] inline std::expected<LambdaCapture, DriverError> read_lambda_capture(ByteReader& in, const std::string& context) {
    LambdaCapture capture{};
    auto name_r = read_string(in, context + " name");
    if (!name_r.has_value()) return std::unexpected(std::move(name_r).error());
    capture.name = std::move(name_r).value();
    auto by_ref_r = read_u8(in, context + " by_reference");
    if (!by_ref_r.has_value()) return std::unexpected(std::move(by_ref_r).error());
    capture.by_reference = by_ref_r.value() != 0u;
    auto resolved_local_r = read_u64_le(in, context + " resolved local");
    if (!resolved_local_r.has_value()) return std::unexpected(std::move(resolved_local_r).error());
    capture.resolved_local = static_cast<std::size_t>(resolved_local_r.value());
    auto from_enclosing_r = read_u8(in, context + " from enclosing closure");
    if (!from_enclosing_r.has_value()) return std::unexpected(std::move(from_enclosing_r).error());
    capture.from_enclosing_closure = from_enclosing_r.value() != 0u;
    auto init_present_r = read_u8(in, context + " init present");
    if (!init_present_r.has_value()) return std::unexpected(std::move(init_present_r).error());
    if (init_present_r.value() != 0u) {
        auto init_r = read_expr(in, context + " init");
        if (!init_r.has_value()) return std::unexpected(std::move(init_r).error());
        capture.init = std::move(init_r).value();
    }
    return capture;
}

inline void write_explicit_template_arg(ByteWriter& out, const ExplicitTemplateArg& arg) {
    write_u8(out, arg.is_type ? 1u : 0u);
    write_type(out, arg.type);
    write_u8(out, arg.value ? 1u : 0u);
    if (arg.value) write_expr(out, *arg.value);
}

[[nodiscard]] inline std::expected<ExplicitTemplateArg, DriverError> read_explicit_template_arg(ByteReader& in, const std::string& context) {
    ExplicitTemplateArg arg{};
    auto is_type_r = read_u8(in, context + " is_type");
    if (!is_type_r.has_value()) return std::unexpected(std::move(is_type_r).error());
    arg.is_type = is_type_r.value() != 0u;
    auto type_r = read_type(in, context + " type");
    if (!type_r.has_value()) return std::unexpected(std::move(type_r).error());
    arg.type = std::move(type_r).value();
    auto value_present_r = read_u8(in, context + " value present");
    if (!value_present_r.has_value()) return std::unexpected(std::move(value_present_r).error());
    if (value_present_r.value() != 0u) {
        auto value_r = read_expr(in, context + " value");
        if (!value_r.has_value()) return std::unexpected(std::move(value_r).error());
        arg.value = std::shared_ptr<Expr>(std::move(value_r).value().release());
    }
    return arg;
}

// Serialization is the second place -- after ast.cppm's `assign_expr_fields` --
// that has to enumerate every field of an `Expr`, and an omission here is worse
// than an omission in a cloner: a lossy cache *persists*, so a module built from
// source and the same module loaded from its `.scppm` stop agreeing, and the
// difference outlives the run that created it.
//
// Binding the node structurally makes that omission a compile error here, at the
// exact function that must be updated:
//
//     error: type 'const scpp::Expr' binds to 30 elements, but only 29 names
//     were provided
//
// If you land here after adding a field to `Expr`: name it in the binding below,
// write it out, read it back in `read_expr` (in the same order), bump
// SCPPM_COMPILE_TIME_AST_VERSION so stale caches are rejected rather than
// misread, and extend the round-trip case in tests/driver_test.cpp. The reader
// has no equivalent compile-time guard -- the round-trip test is what covers it.
inline void write_expr(ByteWriter& out, const Expr& expr) {
    write_enum(out, expr.kind);
    write_u64_le(out, static_cast<std::uint64_t>(expr.resolved_local));
    write_source_location(out, expr.loc);
    write_i64_le(out, expr.int_value);
    write_double_le(out, expr.float_value);
    write_u8(out, expr.bool_value ? 1u : 0u);
    write_string(out, expr.name);
    write_u8(out, expr.explicit_global_qualification ? 1u : 0u);
    write_enum(out, expr.binary_op);
    write_u8(out, expr.lhs ? 1u : 0u);
    if (expr.lhs) write_expr(out, *expr.lhs);
    write_u8(out, expr.rhs ? 1u : 0u);
    if (expr.rhs) write_expr(out, *expr.rhs);
    write_u8(out, expr.third ? 1u : 0u);
    if (expr.third) write_expr(out, *expr.third);
    write_u8(out, expr.fold_ellipsis_on_left ? 1u : 0u);
    write_enum(out, expr.unary_op);
    write_u32_le(out, static_cast<std::uint32_t>(expr.args.size()));
    for (const auto& arg : expr.args) write_expr(out, *arg);
    write_u32_le(out, static_cast<std::uint32_t>(expr.explicit_template_args.size()));
    for (const ExplicitTemplateArg& arg : expr.explicit_template_args) write_explicit_template_arg(out, arg);
    write_type(out, expr.type);
    write_u8(out, expr.sizeof_operand_is_type ? 1u : 0u);
    write_u8(out, expr.has_paren_init ? 1u : 0u);
    write_u8(out, expr.destroy_through_pointer ? 1u : 0u);
    write_u8(out, expr.through_arrow ? 1u : 0u);
    write_u8(out, expr.implicit_arrow_deref ? 1u : 0u);
    write_u8(out, expr.implicit_arrow_chain_safe ? 1u : 0u);
    write_u32_le(out, static_cast<std::uint32_t>(expr.lambda_captures.size()));
    for (const LambdaCapture& capture : expr.lambda_captures) write_lambda_capture(out, capture);
    write_enum(out, expr.lambda_blanket_mode);
    write_u32_le(out, static_cast<std::uint32_t>(expr.lambda_params.size()));
    for (const Param& param : expr.lambda_params) write_param(out, param);
    write_u8(out, expr.has_lambda_explicit_return_type ? 1u : 0u);
    write_u8(out, expr.lambda_is_mutable ? 1u : 0u);
    write_u8(out, expr.lambda_body ? 1u : 0u);
    if (expr.lambda_body) write_stmt(out, *expr.lambda_body);
}

[[nodiscard]] inline std::expected<ExprPtr, DriverError> read_expr(ByteReader& in, const std::string& context) {
    auto expr = std::make_unique<Expr>();
    auto kind_r = scpp::read_enum<ExprKind>(in, context + " kind");
    if (!kind_r.has_value()) return std::unexpected(std::move(kind_r).error());
    expr->kind = kind_r.value();
    auto resolved_local_r = read_u64_le(in, context + " resolved local");
    if (!resolved_local_r.has_value()) return std::unexpected(std::move(resolved_local_r).error());
    expr->resolved_local = static_cast<std::size_t>(resolved_local_r.value());
    auto loc_r = read_source_location(in, context + " loc");
    if (!loc_r.has_value()) return std::unexpected(std::move(loc_r).error());
    expr->loc = std::move(loc_r).value();
    auto int_r = read_i64_le(in, context + " int");
    if (!int_r.has_value()) return std::unexpected(std::move(int_r).error());
    expr->int_value = int_r.value();
    auto float_r = read_double_le(in, context + " double");
    if (!float_r.has_value()) return std::unexpected(std::move(float_r).error());
    expr->float_value = float_r.value();
    auto bool_r = read_u8(in, context + " bool");
    if (!bool_r.has_value()) return std::unexpected(std::move(bool_r).error());
    expr->bool_value = bool_r.value() != 0u;
    auto name_r = read_string(in, context + " name");
    if (!name_r.has_value()) return std::unexpected(std::move(name_r).error());
    expr->name = std::move(name_r).value();
    auto global_qual_r = read_u8(in, context + " global qualification");
    if (!global_qual_r.has_value()) return std::unexpected(std::move(global_qual_r).error());
    expr->explicit_global_qualification = global_qual_r.value() != 0u;
    auto binary_op_r = scpp::read_enum<BinaryOp>(in, context + " binary op");
    if (!binary_op_r.has_value()) return std::unexpected(std::move(binary_op_r).error());
    expr->binary_op = binary_op_r.value();
    auto lhs_present_r = read_u8(in, context + " lhs present");
    if (!lhs_present_r.has_value()) return std::unexpected(std::move(lhs_present_r).error());
    if (lhs_present_r.value() != 0u) {
        auto lhs_r = read_expr(in, context + " lhs");
        if (!lhs_r.has_value()) return std::unexpected(std::move(lhs_r).error());
        expr->lhs = std::move(lhs_r.value());
    }
    auto rhs_present_r = read_u8(in, context + " rhs present");
    if (!rhs_present_r.has_value()) return std::unexpected(std::move(rhs_present_r).error());
    if (rhs_present_r.value() != 0u) {
        auto rhs_r = read_expr(in, context + " rhs");
        if (!rhs_r.has_value()) return std::unexpected(std::move(rhs_r).error());
        expr->rhs = std::move(rhs_r.value());
    }
    auto third_present_r = read_u8(in, context + " third present");
    if (!third_present_r.has_value()) return std::unexpected(std::move(third_present_r).error());
    if (third_present_r.value() != 0u) {
        auto third_r = read_expr(in, context + " third");
        if (!third_r.has_value()) return std::unexpected(std::move(third_r).error());
        expr->third = std::move(third_r.value());
    }
    auto fold_left_r = read_u8(in, context + " fold left");
    if (!fold_left_r.has_value()) return std::unexpected(std::move(fold_left_r).error());
    expr->fold_ellipsis_on_left = fold_left_r.value() != 0u;
    auto unary_op_r = scpp::read_enum<UnaryOp>(in, context + " unary op");
    if (!unary_op_r.has_value()) return std::unexpected(std::move(unary_op_r).error());
    expr->unary_op = unary_op_r.value();
    auto arg_count_r = read_u32_le(in, context + " arg count");
    if (!arg_count_r.has_value()) return std::unexpected(std::move(arg_count_r).error());
    std::uint32_t arg_count = arg_count_r.value();
    expr->args.reserve(static_cast<std::size_t>(arg_count));
    for (std::uint32_t i = 0; i < arg_count; i++) {
        auto arg_r = read_expr(in, context + " arg");
        if (!arg_r.has_value()) return std::unexpected(std::move(arg_r).error());
        expr->args.push_back(std::move(arg_r.value()));
    }
    auto explicit_arg_count_r = read_u32_le(in, context + " explicit arg count");
    if (!explicit_arg_count_r.has_value()) return std::unexpected(std::move(explicit_arg_count_r).error());
    std::uint32_t explicit_arg_count = explicit_arg_count_r.value();
    expr->explicit_template_args.reserve(static_cast<std::size_t>(explicit_arg_count));
    for (std::uint32_t i = 0; i < explicit_arg_count; i++) {
        auto explicit_arg_r = read_explicit_template_arg(in, context + " explicit arg");
        if (!explicit_arg_r.has_value()) return std::unexpected(std::move(explicit_arg_r).error());
        expr->explicit_template_args.push_back(std::move(explicit_arg_r).value());
    }
    auto type_r = read_type(in, context + " type");
    if (!type_r.has_value()) return std::unexpected(std::move(type_r).error());
    expr->type = std::move(type_r).value();
    auto sizeof_is_type_r = read_u8(in, context + " sizeof is type");
    if (!sizeof_is_type_r.has_value()) return std::unexpected(std::move(sizeof_is_type_r).error());
    expr->sizeof_operand_is_type = sizeof_is_type_r.value() != 0u;
    auto has_paren_init_r = read_u8(in, context + " has paren init");
    if (!has_paren_init_r.has_value()) return std::unexpected(std::move(has_paren_init_r).error());
    expr->has_paren_init = has_paren_init_r.value() != 0u;
    auto destroy_through_pointer_r = read_u8(in, context + " destroy through pointer");
    if (!destroy_through_pointer_r.has_value()) return std::unexpected(std::move(destroy_through_pointer_r).error());
    expr->destroy_through_pointer = destroy_through_pointer_r.value() != 0u;
    auto through_arrow_r = read_u8(in, context + " through arrow");
    if (!through_arrow_r.has_value()) return std::unexpected(std::move(through_arrow_r).error());
    expr->through_arrow = through_arrow_r.value() != 0u;
    auto implicit_arrow_deref_r = read_u8(in, context + " implicit arrow deref");
    if (!implicit_arrow_deref_r.has_value()) return std::unexpected(std::move(implicit_arrow_deref_r).error());
    expr->implicit_arrow_deref = implicit_arrow_deref_r.value() != 0u;
    auto implicit_arrow_chain_safe_r = read_u8(in, context + " implicit arrow chain safe");
    if (!implicit_arrow_chain_safe_r.has_value()) return std::unexpected(std::move(implicit_arrow_chain_safe_r).error());
    expr->implicit_arrow_chain_safe = implicit_arrow_chain_safe_r.value() != 0u;
    auto capture_count_r = read_u32_le(in, context + " capture count");
    if (!capture_count_r.has_value()) return std::unexpected(std::move(capture_count_r).error());
    std::uint32_t capture_count = capture_count_r.value();
    expr->lambda_captures.reserve(static_cast<std::size_t>(capture_count));
    for (std::uint32_t i = 0; i < capture_count; i++) {
        auto capture_r = read_lambda_capture(in, context + " capture");
        if (!capture_r.has_value()) return std::unexpected(std::move(capture_r).error());
        expr->lambda_captures.push_back(std::move(capture_r).value());
    }
    auto blanket_mode_r = scpp::read_enum<LambdaCaptureMode>(in, context + " blanket mode");
    if (!blanket_mode_r.has_value()) return std::unexpected(std::move(blanket_mode_r).error());
    expr->lambda_blanket_mode = blanket_mode_r.value();
    auto lambda_param_count_r = read_u32_le(in, context + " lambda param count");
    if (!lambda_param_count_r.has_value()) return std::unexpected(std::move(lambda_param_count_r).error());
    std::uint32_t lambda_param_count = lambda_param_count_r.value();
    expr->lambda_params.reserve(static_cast<std::size_t>(lambda_param_count));
    for (std::uint32_t i = 0; i < lambda_param_count; i++) {
        auto lambda_param_r = read_param(in, context + " lambda param");
        if (!lambda_param_r.has_value()) return std::unexpected(std::move(lambda_param_r).error());
        expr->lambda_params.push_back(std::move(lambda_param_r).value());
    }
    auto explicit_return_r = read_u8(in, context + " explicit return");
    if (!explicit_return_r.has_value()) return std::unexpected(std::move(explicit_return_r).error());
    expr->has_lambda_explicit_return_type = explicit_return_r.value() != 0u;
    auto lambda_mutable_r = read_u8(in, context + " lambda mutable");
    if (!lambda_mutable_r.has_value()) return std::unexpected(std::move(lambda_mutable_r).error());
    expr->lambda_is_mutable = lambda_mutable_r.value() != 0u;
    auto lambda_body_present_r = read_u8(in, context + " lambda body present");
    if (!lambda_body_present_r.has_value()) return std::unexpected(std::move(lambda_body_present_r).error());
    if (lambda_body_present_r.value() != 0u) {
        auto lambda_body_r = read_stmt(in, context + " lambda body");
        if (!lambda_body_r.has_value()) return std::unexpected(std::move(lambda_body_r).error());
        expr->lambda_body = std::move(lambda_body_r.value());
    }
    return std::move(expr);
}

// Structurally bound for the same reason as `write_expr` above; `is_static_local`
// and `declared_local` were both missing from this list until the round-trip test
// in tests/driver_test.cpp caught them.
inline void write_stmt(ByteWriter& out, const Stmt& stmt) {
    write_enum(out, stmt.kind);
    write_source_location(out, stmt.loc);
    write_type(out, stmt.type);
    write_string(out, stmt.var_name);
    write_u64_le(out, static_cast<std::uint64_t>(stmt.declared_local));
    write_u8(out, stmt.init ? 1u : 0u);
    if (stmt.init) write_expr(out, *stmt.init);
    write_u32_le(out, static_cast<std::uint32_t>(stmt.alignment_specs.size()));
    for (const AlignmentSpecifier& spec : stmt.alignment_specs) write_alignment_specifier(out, spec);
    write_u64_le(out, stmt.resolved_alignment);
    write_u8(out, stmt.is_const ? 1u : 0u);
    write_u8(out, stmt.is_constexpr ? 1u : 0u);
    write_u8(out, stmt.is_static_local ? 1u : 0u);
    write_u8(out, stmt.has_ctor_args ? 1u : 0u);
    write_u32_le(out, static_cast<std::uint32_t>(stmt.ctor_args.size()));
    for (const auto& arg : stmt.ctor_args) write_expr(out, *arg);
    write_u8(out, stmt.expr ? 1u : 0u);
    if (stmt.expr) write_expr(out, *stmt.expr);
    write_u8(out, stmt.condition ? 1u : 0u);
    if (stmt.condition) write_expr(out, *stmt.condition);
    write_enum(out, stmt.if_mode);
    write_u8(out, stmt.then_branch ? 1u : 0u);
    if (stmt.then_branch) write_stmt(out, *stmt.then_branch);
    write_u8(out, stmt.else_branch ? 1u : 0u);
    if (stmt.else_branch) write_stmt(out, *stmt.else_branch);
    write_u32_le(out, static_cast<std::uint32_t>(stmt.switch_cases.size()));
    for (const SwitchCase& switch_case : stmt.switch_cases) {
        write_source_location(out, switch_case.loc);
        write_u8(out, switch_case.value ? 1u : 0u);
        if (switch_case.value) write_expr(out, *switch_case.value);
        write_u32_le(out, static_cast<std::uint32_t>(switch_case.statements.size()));
        for (const auto& nested : switch_case.statements) write_stmt(out, *nested);
    }
    write_u32_le(out, static_cast<std::uint32_t>(stmt.statements.size()));
    for (const auto& nested : stmt.statements) write_stmt(out, *nested);
    write_u8(out, stmt.is_unsafe ? 1u : 0u);
}

[[nodiscard]] inline std::expected<StmtPtr, DriverError> read_stmt(ByteReader& in, const std::string& context) {
    auto stmt = std::make_unique<Stmt>();
    auto kind_r = scpp::read_enum<StmtKind>(in, context + " kind");
    if (!kind_r.has_value()) return std::unexpected(std::move(kind_r).error());
    stmt->kind = kind_r.value();
    auto loc_r = read_source_location(in, context + " loc");
    if (!loc_r.has_value()) return std::unexpected(std::move(loc_r).error());
    stmt->loc = std::move(loc_r).value();
    auto type_r = read_type(in, context + " type");
    if (!type_r.has_value()) return std::unexpected(std::move(type_r).error());
    stmt->type = std::move(type_r).value();
    auto var_name_r = read_string(in, context + " var name");
    if (!var_name_r.has_value()) return std::unexpected(std::move(var_name_r).error());
    stmt->var_name = std::move(var_name_r).value();
    auto declared_local_r = read_u64_le(in, context + " declared local");
    if (!declared_local_r.has_value()) return std::unexpected(std::move(declared_local_r).error());
    stmt->declared_local = static_cast<std::size_t>(declared_local_r.value());
    auto init_present_r = read_u8(in, context + " init present");
    if (!init_present_r.has_value()) return std::unexpected(std::move(init_present_r).error());
    if (init_present_r.value() != 0u) {
        auto init_r = read_expr(in, context + " init");
        if (!init_r.has_value()) return std::unexpected(std::move(init_r).error());
        stmt->init = std::move(init_r).value();
    }
    auto align_spec_count_r = read_u32_le(in, context + " align spec count");
    if (!align_spec_count_r.has_value()) return std::unexpected(std::move(align_spec_count_r).error());
    std::uint32_t align_spec_count = align_spec_count_r.value();
    stmt->alignment_specs.reserve(static_cast<std::size_t>(align_spec_count));
    for (std::uint32_t i = 0; i < align_spec_count; i++) {
        auto spec_r = read_alignment_specifier(in, context + " align spec");
        if (!spec_r.has_value()) return std::unexpected(std::move(spec_r).error());
        stmt->alignment_specs.push_back(std::move(spec_r).value());
    }
    auto resolved_alignment_r = read_u64_le(in, context + " resolved alignment");
    if (!resolved_alignment_r.has_value()) return std::unexpected(std::move(resolved_alignment_r).error());
    stmt->resolved_alignment = resolved_alignment_r.value();
    auto is_const_r = read_u8(in, context + " is_const");
    if (!is_const_r.has_value()) return std::unexpected(std::move(is_const_r).error());
    stmt->is_const = is_const_r.value() != 0u;
    auto is_constexpr_r = read_u8(in, context + " is_constexpr");
    if (!is_constexpr_r.has_value()) return std::unexpected(std::move(is_constexpr_r).error());
    stmt->is_constexpr = is_constexpr_r.value() != 0u;
    auto is_static_local_r = read_u8(in, context + " is_static_local");
    if (!is_static_local_r.has_value()) return std::unexpected(std::move(is_static_local_r).error());
    stmt->is_static_local = is_static_local_r.value() != 0u;
    auto has_ctor_args_r = read_u8(in, context + " has ctor args");
    if (!has_ctor_args_r.has_value()) return std::unexpected(std::move(has_ctor_args_r).error());
    stmt->has_ctor_args = has_ctor_args_r.value() != 0u;
    auto ctor_arg_count_r = read_u32_le(in, context + " ctor arg count");
    if (!ctor_arg_count_r.has_value()) return std::unexpected(std::move(ctor_arg_count_r).error());
    std::uint32_t ctor_arg_count = ctor_arg_count_r.value();
    stmt->ctor_args.reserve(static_cast<std::size_t>(ctor_arg_count));
    for (std::uint32_t i = 0; i < ctor_arg_count; i++) {
        auto ctor_arg_r = read_expr(in, context + " ctor arg");
        if (!ctor_arg_r.has_value()) return std::unexpected(std::move(ctor_arg_r).error());
        stmt->ctor_args.push_back(std::move(ctor_arg_r.value()));
    }
    auto expr_present_r = read_u8(in, context + " expr present");
    if (!expr_present_r.has_value()) return std::unexpected(std::move(expr_present_r).error());
    if (expr_present_r.value() != 0u) {
        auto expr_r = read_expr(in, context + " expr");
        if (!expr_r.has_value()) return std::unexpected(std::move(expr_r).error());
        stmt->expr = std::move(expr_r.value());
    }
    auto condition_present_r = read_u8(in, context + " condition present");
    if (!condition_present_r.has_value()) return std::unexpected(std::move(condition_present_r).error());
    if (condition_present_r.value() != 0u) {
        auto condition_r = read_expr(in, context + " condition");
        if (!condition_r.has_value()) return std::unexpected(std::move(condition_r).error());
        stmt->condition = std::move(condition_r.value());
    }
    auto if_mode_r = scpp::read_enum<IfMode>(in, context + " if mode");
    if (!if_mode_r.has_value()) return std::unexpected(std::move(if_mode_r).error());
    stmt->if_mode = if_mode_r.value();
    auto then_present_r = read_u8(in, context + " then present");
    if (!then_present_r.has_value()) return std::unexpected(std::move(then_present_r).error());
    if (then_present_r.value() != 0u) {
        auto then_r = read_stmt(in, context + " then");
        if (!then_r.has_value()) return std::unexpected(std::move(then_r).error());
        stmt->then_branch = std::move(then_r.value());
    }
    auto else_present_r = read_u8(in, context + " else present");
    if (!else_present_r.has_value()) return std::unexpected(std::move(else_present_r).error());
    if (else_present_r.value() != 0u) {
        auto else_r = read_stmt(in, context + " else");
        if (!else_r.has_value()) return std::unexpected(std::move(else_r).error());
        stmt->else_branch = std::move(else_r.value());
    }
    auto switch_case_count_r = read_u32_le(in, context + " switch case count");
    if (!switch_case_count_r.has_value()) return std::unexpected(std::move(switch_case_count_r).error());
    std::uint32_t switch_case_count = switch_case_count_r.value();
    stmt->switch_cases.reserve(static_cast<std::size_t>(switch_case_count));
    for (std::uint32_t i = 0; i < switch_case_count; i++) {
        SwitchCase switch_case{};
        auto switch_case_loc_r = read_source_location(in, context + " switch case loc");
        if (!switch_case_loc_r.has_value()) return std::unexpected(std::move(switch_case_loc_r).error());
        switch_case.loc = std::move(switch_case_loc_r).value();
        auto switch_case_value_present_r = read_u8(in, context + " switch case value present");
        if (!switch_case_value_present_r.has_value()) return std::unexpected(std::move(switch_case_value_present_r).error());
        if (switch_case_value_present_r.value() != 0u) {
            auto switch_case_value_r = read_expr(in, context + " switch case value");
            if (!switch_case_value_r.has_value()) return std::unexpected(std::move(switch_case_value_r).error());
            switch_case.value = std::move(switch_case_value_r.value());
        }
        auto switch_stmt_count_r = read_u32_le(in, context + " switch case stmt count");
        if (!switch_stmt_count_r.has_value()) return std::unexpected(std::move(switch_stmt_count_r).error());
        std::uint32_t switch_stmt_count = switch_stmt_count_r.value();
        switch_case.statements.reserve(static_cast<std::size_t>(switch_stmt_count));
        for (std::uint32_t j = 0; j < switch_stmt_count; j++) {
            auto switch_stmt_r = read_stmt(in, context + " switch case stmt");
            if (!switch_stmt_r.has_value()) return std::unexpected(std::move(switch_stmt_r).error());
            switch_case.statements.push_back(std::move(switch_stmt_r.value()));
        }
        stmt->switch_cases.push_back(std::move(switch_case));
    }
    auto nested_count_r = read_u32_le(in, context + " nested count");
    if (!nested_count_r.has_value()) return std::unexpected(std::move(nested_count_r).error());
    std::uint32_t nested_count = nested_count_r.value();
    stmt->statements.reserve(static_cast<std::size_t>(nested_count));
    for (std::uint32_t i = 0; i < nested_count; i++) {
        auto nested_r = read_stmt(in, context + " nested");
        if (!nested_r.has_value()) return std::unexpected(std::move(nested_r).error());
        stmt->statements.push_back(std::move(nested_r.value()));
    }
    auto unsafe_r = read_u8(in, context + " unsafe");
    if (!unsafe_r.has_value()) return std::unexpected(std::move(unsafe_r).error());
    stmt->is_unsafe = unsafe_r.value() != 0u;
    return std::move(stmt);
}

inline void write_initializer(ByteWriter& out, const Initializer& init) {
    write_u8(out, init.expr ? 1u : 0u);
    if (init.expr) write_expr(out, *init.expr);
    write_u8(out, init.has_brace_args ? 1u : 0u);
    write_u32_le(out, static_cast<std::uint32_t>(init.brace_args.size()));
    for (const ExprPtr& arg : init.brace_args) write_expr(out, *arg);
}

inline void write_alignment_specifier(ByteWriter& out, const AlignmentSpecifier& spec) {
    write_source_location(out, spec.loc);
    write_u8(out, spec.operand_is_type ? 1u : 0u);
    write_type(out, spec.type);
    write_u8(out, spec.expr ? 1u : 0u);
    if (spec.expr) write_expr(out, *spec.expr);
}

[[nodiscard]] inline std::expected<AlignmentSpecifier, DriverError> read_alignment_specifier(ByteReader& in, const std::string& context) {
    AlignmentSpecifier spec{};
    auto loc_r = read_source_location(in, context + " loc");
    if (!loc_r.has_value()) return std::unexpected(std::move(loc_r).error());
    spec.loc = std::move(loc_r).value();
    auto operand_is_type_r = read_u8(in, context + " operand is type");
    if (!operand_is_type_r.has_value()) return std::unexpected(std::move(operand_is_type_r).error());
    spec.operand_is_type = operand_is_type_r.value() != 0u;
    auto type_r = read_type(in, context + " type");
    if (!type_r.has_value()) return std::unexpected(std::move(type_r).error());
    spec.type = std::move(type_r).value();
    auto expr_present_r = read_u8(in, context + " expr present");
    if (!expr_present_r.has_value()) return std::unexpected(std::move(expr_present_r).error());
    if (expr_present_r.value() != 0u) {
        auto expr_r = read_expr(in, context + " expr");
        if (!expr_r.has_value()) return std::unexpected(std::move(expr_r).error());
        spec.expr = std::move(expr_r).value();
    }
    return spec;
}

[[nodiscard]] inline std::expected<Initializer, DriverError> read_initializer(ByteReader& in, const std::string& context) {
    Initializer init{};
    auto expr_present_r = read_u8(in, context + " expr present");
    if (!expr_present_r.has_value()) return std::unexpected(std::move(expr_present_r).error());
    if (expr_present_r.value() != 0u) {
        auto expr_r = read_expr(in, context + " expr");
        if (!expr_r.has_value()) return std::unexpected(std::move(expr_r).error());
        init.expr = std::move(expr_r.value());
    }
    auto brace_present_r = read_u8(in, context + " brace args present");
    if (!brace_present_r.has_value()) return std::unexpected(std::move(brace_present_r).error());
    init.has_brace_args = brace_present_r.value() != 0u;
    auto arg_count_r = read_u32_le(in, context + " brace arg count");
    if (!arg_count_r.has_value()) return std::unexpected(std::move(arg_count_r).error());
    std::uint32_t arg_count = arg_count_r.value();
    init.brace_args.reserve(static_cast<std::size_t>(arg_count));
    for (std::uint32_t i = 0; i < arg_count; i++) {
        auto arg_r = read_expr(in, context + " brace arg");
        if (!arg_r.has_value()) return std::unexpected(std::move(arg_r).error());
        init.brace_args.push_back(std::move(arg_r.value()));
    }
    return init;
}

inline void write_member_initializer(ByteWriter& out, const MemberInitializer& init) {
    write_string(out, init.member_name);
    write_initializer(out, init.initializer);
    write_source_location(out, init.loc);
}

[[nodiscard]] inline std::expected<MemberInitializer, DriverError> read_member_initializer(ByteReader& in, const std::string& context) {
    MemberInitializer init{};
    auto name_r = read_string(in, context + " member name");
    if (!name_r.has_value()) return std::unexpected(std::move(name_r).error());
    init.member_name = std::move(name_r).value();
    auto initializer_r = read_initializer(in, context + " initializer");
    if (!initializer_r.has_value()) return std::unexpected(std::move(initializer_r).error());
    init.initializer = std::move(initializer_r).value();
    auto loc_r = read_source_location(in, context + " loc");
    if (!loc_r.has_value()) return std::unexpected(std::move(loc_r).error());
    init.loc = std::move(loc_r).value();
    return init;
}

inline void write_struct_field(ByteWriter& out, const StructField& field) {
    write_source_location(out, field.loc);
    write_type(out, field.type);
    write_string(out, field.name);
    write_u8(out, field.default_initializer.has_value() ? 1u : 0u);
    if (field.default_initializer) write_initializer(out, *field.default_initializer);
    write_enum(out, field.access);
    write_u32_le(out, static_cast<std::uint32_t>(field.alignment_specs.size()));
    for (const AlignmentSpecifier& spec : field.alignment_specs) write_alignment_specifier(out, spec);
    write_u64_le(out, field.resolved_alignment);
}

[[nodiscard]] inline std::expected<StructField, DriverError> read_struct_field(ByteReader& in, const std::string& context) {
    StructField field{};
    auto loc_r = read_source_location(in, context + " loc");
    if (!loc_r.has_value()) return std::unexpected(std::move(loc_r).error());
    field.loc = std::move(loc_r).value();
    auto type_r = read_type(in, context + " type");
    if (!type_r.has_value()) return std::unexpected(std::move(type_r).error());
    field.type = std::move(type_r).value();
    auto name_r = read_string(in, context + " name");
    if (!name_r.has_value()) return std::unexpected(std::move(name_r).error());
    field.name = std::move(name_r).value();
    auto default_present_r = read_u8(in, context + " default initializer present");
    if (!default_present_r.has_value()) return std::unexpected(std::move(default_present_r).error());
    if (default_present_r.value() != 0u) {
        auto default_r = read_initializer(in, context + " default initializer");
        if (!default_r.has_value()) return std::unexpected(std::move(default_r).error());
        field.default_initializer = std::move(default_r).value();
    }
    auto access_r = scpp::read_enum<AccessSpecifier>(in, context + " access");
    if (!access_r.has_value()) return std::unexpected(std::move(access_r).error());
    field.access = access_r.value();
    auto align_spec_count_r = read_u32_le(in, context + " align spec count");
    if (!align_spec_count_r.has_value()) return std::unexpected(std::move(align_spec_count_r).error());
    std::uint32_t align_spec_count = align_spec_count_r.value();
    field.alignment_specs.reserve(static_cast<std::size_t>(align_spec_count));
    for (std::uint32_t i = 0; i < align_spec_count; i++) {
        auto spec_r = read_alignment_specifier(in, context + " align spec");
        if (!spec_r.has_value()) return std::unexpected(std::move(spec_r).error());
        field.alignment_specs.push_back(std::move(spec_r).value());
    }
    auto resolved_alignment_r = read_u64_le(in, context + " resolved alignment");
    if (!resolved_alignment_r.has_value()) return std::unexpected(std::move(resolved_alignment_r).error());
    field.resolved_alignment = resolved_alignment_r.value();
    return field;
}

inline void write_class_field(ByteWriter& out, const ClassField& field) {
    write_source_location(out, field.loc);
    write_type(out, field.type);
    write_string(out, field.name);
    write_u8(out, field.default_initializer.has_value() ? 1u : 0u);
    if (field.default_initializer) write_initializer(out, *field.default_initializer);
    write_enum(out, field.access);
    write_u32_le(out, static_cast<std::uint32_t>(field.alignment_specs.size()));
    for (const AlignmentSpecifier& spec : field.alignment_specs) write_alignment_specifier(out, spec);
    write_u64_le(out, field.resolved_alignment);
}

[[nodiscard]] inline std::expected<ClassField, DriverError> read_class_field(ByteReader& in, const std::string& context) {
    ClassField field{};
    auto loc_r = read_source_location(in, context + " loc");
    if (!loc_r.has_value()) return std::unexpected(std::move(loc_r).error());
    field.loc = std::move(loc_r).value();
    auto type_r = read_type(in, context + " type");
    if (!type_r.has_value()) return std::unexpected(std::move(type_r).error());
    field.type = std::move(type_r).value();
    auto name_r = read_string(in, context + " name");
    if (!name_r.has_value()) return std::unexpected(std::move(name_r).error());
    field.name = std::move(name_r).value();
    auto default_present_r = read_u8(in, context + " default initializer present");
    if (!default_present_r.has_value()) return std::unexpected(std::move(default_present_r).error());
    if (default_present_r.value() != 0u) {
        auto default_r = read_initializer(in, context + " default initializer");
        if (!default_r.has_value()) return std::unexpected(std::move(default_r).error());
        field.default_initializer = std::move(default_r).value();
    }
    auto access_r = scpp::read_enum<AccessSpecifier>(in, context + " access");
    if (!access_r.has_value()) return std::unexpected(std::move(access_r).error());
    field.access = access_r.value();
    auto align_spec_count_r = read_u32_le(in, context + " align spec count");
    if (!align_spec_count_r.has_value()) return std::unexpected(std::move(align_spec_count_r).error());
    std::uint32_t align_spec_count = align_spec_count_r.value();
    field.alignment_specs.reserve(static_cast<std::size_t>(align_spec_count));
    for (std::uint32_t i = 0; i < align_spec_count; i++) {
        auto spec_r = read_alignment_specifier(in, context + " align spec");
        if (!spec_r.has_value()) return std::unexpected(std::move(spec_r).error());
        field.alignment_specs.push_back(std::move(spec_r).value());
    }
    auto resolved_alignment_r = read_u64_le(in, context + " resolved alignment");
    if (!resolved_alignment_r.has_value()) return std::unexpected(std::move(resolved_alignment_r).error());
    field.resolved_alignment = resolved_alignment_r.value();
    return field;
}

inline void write_base_specifier(ByteWriter& out, const BaseSpecifier& base) {
    write_type(out, base.base_type);
    write_enum(out, base.access);
    write_u8(out, base.is_virtual ? 1u : 0u);
    write_enum(out, base.kind);
    write_string(out, base.pack_arg_name);
}

[[nodiscard]] inline std::expected<BaseSpecifier, DriverError> read_base_specifier(ByteReader& in, const std::string& context) {
    BaseSpecifier base{};
    auto type_r = read_type(in, context + " type");
    if (!type_r.has_value()) return std::unexpected(std::move(type_r).error());
    base.base_type = std::move(type_r).value();
    auto access_r = scpp::read_enum<AccessSpecifier>(in, context + " access");
    if (!access_r.has_value()) return std::unexpected(std::move(access_r).error());
    base.access = access_r.value();
    auto is_virtual_r = read_u8(in, context + " is_virtual");
    if (!is_virtual_r.has_value()) return std::unexpected(std::move(is_virtual_r).error());
    base.is_virtual = is_virtual_r.value() != 0u;
    auto kind_r = scpp::read_enum<BaseClassKind>(in, context + " kind");
    if (!kind_r.has_value()) return std::unexpected(std::move(kind_r).error());
    base.kind = kind_r.value();
    auto pack_arg_r = read_string(in, context + " pack arg");
    if (!pack_arg_r.has_value()) return std::unexpected(std::move(pack_arg_r).error());
    base.pack_arg_name = std::move(pack_arg_r).value();
    return base;
}

inline void write_class_using_declaration(ByteWriter& out, const ClassUsingDeclaration& decl) {
    write_string(out, decl.base_name);
    write_string(out, decl.member_name);
    write_enum(out, decl.access);
}

[[nodiscard]] inline std::expected<ClassUsingDeclaration, DriverError> read_class_using_declaration(ByteReader& in, const std::string& context) {
    ClassUsingDeclaration decl{};
    auto base_name_r = read_string(in, context + " base name");
    if (!base_name_r.has_value()) return std::unexpected(std::move(base_name_r).error());
    decl.base_name = std::move(base_name_r).value();
    auto member_name_r = read_string(in, context + " member name");
    if (!member_name_r.has_value()) return std::unexpected(std::move(member_name_r).error());
    decl.member_name = std::move(member_name_r).value();
    auto access_r = scpp::read_enum<AccessSpecifier>(in, context + " access");
    if (!access_r.has_value()) return std::unexpected(std::move(access_r).error());
    decl.access = access_r.value();
    return decl;
}

inline void write_enum_variant(ByteWriter& out, const EnumVariant& variant) {
    write_string(out, variant.name);
    write_i64_le(out, variant.value);
}

[[nodiscard]] inline std::expected<EnumVariant, DriverError> read_enum_variant(ByteReader& in, const std::string& context) {
    EnumVariant variant{};
    auto name_r = read_string(in, context + " name");
    if (!name_r.has_value()) return std::unexpected(std::move(name_r).error());
    variant.name = std::move(name_r).value();
    auto value_r = read_i64_le(in, context + " value");
    if (!value_r.has_value()) return std::unexpected(std::move(value_r).error());
    variant.value = value_r.value();
    return variant;
}

inline void write_enum_def(ByteWriter& out, const EnumDef& def) {
    write_string(out, def.name);
    write_type(out, def.underlying_type);
    write_u32_le(out, static_cast<std::uint32_t>(def.variants.size()));
    for (const EnumVariant& variant : def.variants) write_enum_variant(out, variant);
    write_u32_le(out, static_cast<std::uint32_t>(def.namespace_path.size()));
    for (const std::string& segment : def.namespace_path) write_string(out, segment);
    write_u8(out, def.is_exported ? 1u : 0u);
    write_u8(out, def.is_compile_time_dependency ? 1u : 0u);
    write_string(out, def.owning_module);
}

[[nodiscard]] inline std::expected<EnumDef, DriverError> read_enum_def(ByteReader& in, const std::string& context) {
    EnumDef def{};
    auto name_r = read_string(in, context + " name");
    if (!name_r.has_value()) return std::unexpected(std::move(name_r).error());
    def.name = std::move(name_r).value();
    auto underlying_r = read_type(in, context + " underlying");
    if (!underlying_r.has_value()) return std::unexpected(std::move(underlying_r).error());
    def.underlying_type = std::move(underlying_r).value();
    auto variant_count_r = read_u32_le(in, context + " variant count");
    if (!variant_count_r.has_value()) return std::unexpected(std::move(variant_count_r).error());
    std::uint32_t variant_count = variant_count_r.value();
    def.variants.reserve(static_cast<std::size_t>(variant_count));
    for (std::uint32_t i = 0; i < variant_count; i++) {
        auto variant_r = read_enum_variant(in, context + " variant");
        if (!variant_r.has_value()) return std::unexpected(std::move(variant_r).error());
        def.variants.push_back(std::move(variant_r).value());
    }
    auto ns_count_r = read_u32_le(in, context + " namespace count");
    if (!ns_count_r.has_value()) return std::unexpected(std::move(ns_count_r).error());
    std::uint32_t ns_count = ns_count_r.value();
    def.namespace_path.reserve(static_cast<std::size_t>(ns_count));
    for (std::uint32_t i = 0; i < ns_count; i++) {
        auto ns_r = read_string(in, context + " namespace");
        if (!ns_r.has_value()) return std::unexpected(std::move(ns_r).error());
        def.namespace_path.push_back(std::move(ns_r).value());
    }
    auto is_exported_r = read_u8(in, context + " is_exported");
    if (!is_exported_r.has_value()) return std::unexpected(std::move(is_exported_r).error());
    def.is_exported = is_exported_r.value() != 0u;
    auto is_ctd_r = read_u8(in, context + " is_compile_time_dependency");
    if (!is_ctd_r.has_value()) return std::unexpected(std::move(is_ctd_r).error());
    def.is_compile_time_dependency = is_ctd_r.value() != 0u;
    auto owning_module_r = read_string(in, context + " owning_module");
    if (!owning_module_r.has_value()) return std::unexpected(std::move(owning_module_r).error());
    def.owning_module = std::move(owning_module_r).value();
    return def;
}

inline void write_struct_def(ByteWriter& out, const StructDef& def) {
    write_source_location(out, def.loc);
    write_string(out, def.name);
    write_u32_le(out, static_cast<std::uint32_t>(def.fields.size()));
    for (const StructField& field : def.fields) write_struct_field(out, field);
    write_u8(out, def.is_union ? 1u : 0u);
    write_u8(out, def.is_concept_witness ? 1u : 0u);
    write_u8(out, def.is_packed ? 1u : 0u);
    write_u32_le(out, static_cast<std::uint32_t>(def.alignment_specs.size()));
    for (const AlignmentSpecifier& spec : def.alignment_specs) write_alignment_specifier(out, spec);
    write_u64_le(out, def.resolved_alignment);
    write_u32_le(out, static_cast<std::uint32_t>(def.namespace_path.size()));
    for (const std::string& segment : def.namespace_path) write_string(out, segment);
    write_u8(out, def.is_exported ? 1u : 0u);
    write_u8(out, def.is_compile_time_dependency ? 1u : 0u);
    write_string(out, def.owning_module);
    write_u32_le(out, static_cast<std::uint32_t>(def.template_params.size()));
    for (const GenericTypeParam& param : def.template_params) write_generic_type_param(out, param);
    write_u8(out, def.is_forward_declaration ? 1u : 0u);
    write_string(out, def.template_owner_id);
    write_u8(out, def.thread_movable_override ? 1u : 0u);
    write_u8(out, def.thread_shareable_override ? 1u : 0u);
    write_u8(out, def.is_nodiscard ? 1u : 0u);
    write_string(out, def.nodiscard_reason);
}

[[nodiscard]] inline std::expected<StructDef, DriverError> read_struct_def(ByteReader& in, const std::string& context) {
    StructDef def{};
    auto loc_r = read_source_location(in, context + " loc");
    if (!loc_r.has_value()) return std::unexpected(std::move(loc_r).error());
    def.loc = std::move(loc_r).value();
    auto name_r = read_string(in, context + " name");
    if (!name_r.has_value()) return std::unexpected(std::move(name_r).error());
    def.name = std::move(name_r).value();
    auto field_count_r = read_u32_le(in, context + " field count");
    if (!field_count_r.has_value()) return std::unexpected(std::move(field_count_r).error());
    std::uint32_t field_count = field_count_r.value();
    def.fields.reserve(static_cast<std::size_t>(field_count));
    for (std::uint32_t i = 0; i < field_count; i++) {
        auto field_r = read_struct_field(in, context + " field");
        if (!field_r.has_value()) return std::unexpected(std::move(field_r).error());
        def.fields.push_back(std::move(field_r).value());
    }
    auto is_union_r = read_u8(in, context + " is_union");
    if (!is_union_r.has_value()) return std::unexpected(std::move(is_union_r).error());
    def.is_union = is_union_r.value() != 0u;
    auto is_witness_r = read_u8(in, context + " is_concept_witness");
    if (!is_witness_r.has_value()) return std::unexpected(std::move(is_witness_r).error());
    def.is_concept_witness = is_witness_r.value() != 0u;
    auto is_packed_r = read_u8(in, context + " is_packed");
    if (!is_packed_r.has_value()) return std::unexpected(std::move(is_packed_r).error());
    def.is_packed = is_packed_r.value() != 0u;
    auto align_spec_count_r = read_u32_le(in, context + " align spec count");
    if (!align_spec_count_r.has_value()) return std::unexpected(std::move(align_spec_count_r).error());
    std::uint32_t align_spec_count = align_spec_count_r.value();
    def.alignment_specs.reserve(static_cast<std::size_t>(align_spec_count));
    for (std::uint32_t i = 0; i < align_spec_count; i++) {
        auto spec_r = read_alignment_specifier(in, context + " align spec");
        if (!spec_r.has_value()) return std::unexpected(std::move(spec_r).error());
        def.alignment_specs.push_back(std::move(spec_r).value());
    }
    auto resolved_alignment_r = read_u64_le(in, context + " resolved alignment");
    if (!resolved_alignment_r.has_value()) return std::unexpected(std::move(resolved_alignment_r).error());
    def.resolved_alignment = resolved_alignment_r.value();
    auto ns_count_r = read_u32_le(in, context + " namespace count");
    if (!ns_count_r.has_value()) return std::unexpected(std::move(ns_count_r).error());
    std::uint32_t ns_count = ns_count_r.value();
    def.namespace_path.reserve(static_cast<std::size_t>(ns_count));
    for (std::uint32_t i = 0; i < ns_count; i++) {
        auto ns_r = read_string(in, context + " namespace");
        if (!ns_r.has_value()) return std::unexpected(std::move(ns_r).error());
        def.namespace_path.push_back(std::move(ns_r).value());
    }
    auto is_exported_r = read_u8(in, context + " is_exported");
    if (!is_exported_r.has_value()) return std::unexpected(std::move(is_exported_r).error());
    def.is_exported = is_exported_r.value() != 0u;
    auto is_ctd_r = read_u8(in, context + " is_compile_time_dependency");
    if (!is_ctd_r.has_value()) return std::unexpected(std::move(is_ctd_r).error());
    def.is_compile_time_dependency = is_ctd_r.value() != 0u;
    auto owning_module_r = read_string(in, context + " owning_module");
    if (!owning_module_r.has_value()) return std::unexpected(std::move(owning_module_r).error());
    def.owning_module = std::move(owning_module_r).value();
    auto template_param_count_r = read_u32_le(in, context + " template param count");
    if (!template_param_count_r.has_value()) return std::unexpected(std::move(template_param_count_r).error());
    std::uint32_t template_param_count = template_param_count_r.value();
    def.template_params.reserve(static_cast<std::size_t>(template_param_count));
    for (std::uint32_t i = 0; i < template_param_count; i++) {
        auto template_param_r = read_generic_type_param(in, context + " template param");
        if (!template_param_r.has_value()) return std::unexpected(std::move(template_param_r).error());
        def.template_params.push_back(std::move(template_param_r).value());
    }
    auto is_fwd_decl_r = read_u8(in, context + " is_forward_declaration");
    if (!is_fwd_decl_r.has_value()) return std::unexpected(std::move(is_fwd_decl_r).error());
    def.is_forward_declaration = is_fwd_decl_r.value() != 0u;
    auto template_owner_id_r = read_string(in, context + " template owner id");
    if (!template_owner_id_r.has_value()) return std::unexpected(std::move(template_owner_id_r).error());
    def.template_owner_id = std::move(template_owner_id_r).value();
    auto thread_movable_override_r = read_u8(in, context + " thread movable override");
    if (!thread_movable_override_r.has_value()) return std::unexpected(std::move(thread_movable_override_r).error());
    def.thread_movable_override = thread_movable_override_r.value() != 0u;
    auto thread_shareable_override_r = read_u8(in, context + " thread shareable override");
    if (!thread_shareable_override_r.has_value()) return std::unexpected(std::move(thread_shareable_override_r).error());
    def.thread_shareable_override = thread_shareable_override_r.value() != 0u;
    auto is_nodiscard_r = read_u8(in, context + " nodiscard");
    if (!is_nodiscard_r.has_value()) return std::unexpected(std::move(is_nodiscard_r).error());
    def.is_nodiscard = is_nodiscard_r.value() != 0u;
    auto nodiscard_reason_r = read_string(in, context + " nodiscard reason");
    if (!nodiscard_reason_r.has_value()) return std::unexpected(std::move(nodiscard_reason_r).error());
    def.nodiscard_reason = std::move(nodiscard_reason_r).value();
    return def;
}

inline void write_class_def(ByteWriter& out, const ClassDef& def) {
    write_source_location(out, def.loc);
    write_string(out, def.name);
    write_u32_le(out, static_cast<std::uint32_t>(def.fields.size()));
    for (const ClassField& field : def.fields) write_class_field(out, field);
    write_u32_le(out, static_cast<std::uint32_t>(def.alignment_specs.size()));
    for (const AlignmentSpecifier& spec : def.alignment_specs) write_alignment_specifier(out, spec);
    write_u64_le(out, def.resolved_alignment);
    write_u32_le(out, static_cast<std::uint32_t>(def.namespace_path.size()));
    for (const std::string& segment : def.namespace_path) write_string(out, segment);
    write_u8(out, def.is_exported ? 1u : 0u);
    write_u8(out, def.is_compile_time_dependency ? 1u : 0u);
    write_string(out, def.owning_module);
    write_u8(out, def.is_concept_witness ? 1u : 0u);
    write_u8(out, def.is_interface ? 1u : 0u);
    write_u32_le(out, static_cast<std::uint32_t>(def.base_specifiers.size()));
    for (const BaseSpecifier& base : def.base_specifiers) write_base_specifier(out, base);
    write_u32_le(out, static_cast<std::uint32_t>(def.using_declarations.size()));
    for (const ClassUsingDeclaration& decl : def.using_declarations) write_class_using_declaration(out, decl);
    write_u32_le(out, static_cast<std::uint32_t>(def.template_params.size()));
    for (const GenericTypeParam& param : def.template_params) write_generic_type_param(out, param);
    write_string(out, def.template_owner_id);
    write_u8(out, def.is_forward_declaration ? 1u : 0u);
    write_u8(out, def.is_synthetic_check_only ? 1u : 0u);
    write_u8(out, def.is_variadic_primary_template ? 1u : 0u);
    write_u8(out, def.is_variadic_specialization ? 1u : 0u);
    write_u8(out, def.is_partial_specialization ? 1u : 0u);
    write_u32_le(out, static_cast<std::uint32_t>(def.specialization_template_args.size()));
    for (const Type& arg : def.specialization_template_args) write_type(out, arg);
    write_u8(out, def.thread_movable_override ? 1u : 0u);
    write_u8(out, def.thread_shareable_override ? 1u : 0u);
    write_u8(out, def.thread_movable_if_movable_expr ? 1u : 0u);
    if (def.thread_movable_if_movable_expr) write_expr(out, *def.thread_movable_if_movable_expr);
    write_u8(out, def.thread_movable_if_shareable_expr ? 1u : 0u);
    if (def.thread_movable_if_shareable_expr) write_expr(out, *def.thread_movable_if_shareable_expr);
    write_u8(out, def.is_nodiscard ? 1u : 0u);
    write_string(out, def.nodiscard_reason);
}

[[nodiscard]] inline std::expected<ClassDef, DriverError> read_class_def(ByteReader& in, const std::string& context) {
    ClassDef def{};
    auto loc_r = read_source_location(in, context + " loc");
    if (!loc_r.has_value()) return std::unexpected(std::move(loc_r).error());
    def.loc = std::move(loc_r).value();
    auto name_r = read_string(in, context + " name");
    if (!name_r.has_value()) return std::unexpected(std::move(name_r).error());
    def.name = std::move(name_r).value();
    auto field_count_r = read_u32_le(in, context + " field count");
    if (!field_count_r.has_value()) return std::unexpected(std::move(field_count_r).error());
    std::uint32_t field_count = field_count_r.value();
    def.fields.reserve(static_cast<std::size_t>(field_count));
    for (std::uint32_t i = 0; i < field_count; i++) {
        auto field_r = read_class_field(in, context + " field");
        if (!field_r.has_value()) return std::unexpected(std::move(field_r).error());
        def.fields.push_back(std::move(field_r).value());
    }
    auto align_spec_count_r = read_u32_le(in, context + " align spec count");
    if (!align_spec_count_r.has_value()) return std::unexpected(std::move(align_spec_count_r).error());
    std::uint32_t align_spec_count = align_spec_count_r.value();
    def.alignment_specs.reserve(static_cast<std::size_t>(align_spec_count));
    for (std::uint32_t i = 0; i < align_spec_count; i++) {
        auto spec_r = read_alignment_specifier(in, context + " align spec");
        if (!spec_r.has_value()) return std::unexpected(std::move(spec_r).error());
        def.alignment_specs.push_back(std::move(spec_r).value());
    }
    auto resolved_alignment_r = read_u64_le(in, context + " resolved alignment");
    if (!resolved_alignment_r.has_value()) return std::unexpected(std::move(resolved_alignment_r).error());
    def.resolved_alignment = resolved_alignment_r.value();
    auto ns_count_r = read_u32_le(in, context + " namespace count");
    if (!ns_count_r.has_value()) return std::unexpected(std::move(ns_count_r).error());
    std::uint32_t ns_count = ns_count_r.value();
    def.namespace_path.reserve(static_cast<std::size_t>(ns_count));
    for (std::uint32_t i = 0; i < ns_count; i++) {
        auto ns_r = read_string(in, context + " namespace");
        if (!ns_r.has_value()) return std::unexpected(std::move(ns_r).error());
        def.namespace_path.push_back(std::move(ns_r).value());
    }
    auto is_exported_r = read_u8(in, context + " is_exported");
    if (!is_exported_r.has_value()) return std::unexpected(std::move(is_exported_r).error());
    def.is_exported = is_exported_r.value() != 0u;
    auto is_ctd_r = read_u8(in, context + " is_compile_time_dependency");
    if (!is_ctd_r.has_value()) return std::unexpected(std::move(is_ctd_r).error());
    def.is_compile_time_dependency = is_ctd_r.value() != 0u;
    auto owning_module_r = read_string(in, context + " owning_module");
    if (!owning_module_r.has_value()) return std::unexpected(std::move(owning_module_r).error());
    def.owning_module = std::move(owning_module_r).value();
    auto is_concept_witness_r = read_u8(in, context + " is_concept_witness");
    if (!is_concept_witness_r.has_value()) return std::unexpected(std::move(is_concept_witness_r).error());
    def.is_concept_witness = is_concept_witness_r.value() != 0u;
    auto is_interface_r = read_u8(in, context + " is_interface");
    if (!is_interface_r.has_value()) return std::unexpected(std::move(is_interface_r).error());
    def.is_interface = is_interface_r.value() != 0u;
    auto base_count_r = read_u32_le(in, context + " base count");
    if (!base_count_r.has_value()) return std::unexpected(std::move(base_count_r).error());
    std::uint32_t base_count = base_count_r.value();
    def.base_specifiers.reserve(static_cast<std::size_t>(base_count));
    for (std::uint32_t i = 0; i < base_count; i++) {
        auto base_r = read_base_specifier(in, context + " base");
        if (!base_r.has_value()) return std::unexpected(std::move(base_r).error());
        def.base_specifiers.push_back(std::move(base_r).value());
    }
    auto using_count_r = read_u32_le(in, context + " using count");
    if (!using_count_r.has_value()) return std::unexpected(std::move(using_count_r).error());
    std::uint32_t using_count = using_count_r.value();
    def.using_declarations.reserve(static_cast<std::size_t>(using_count));
    for (std::uint32_t i = 0; i < using_count; i++) {
        auto using_r = read_class_using_declaration(in, context + " using");
        if (!using_r.has_value()) return std::unexpected(std::move(using_r).error());
        def.using_declarations.push_back(std::move(using_r).value());
    }
    auto template_param_count_r = read_u32_le(in, context + " template param count");
    if (!template_param_count_r.has_value()) return std::unexpected(std::move(template_param_count_r).error());
    std::uint32_t template_param_count = template_param_count_r.value();
    def.template_params.reserve(static_cast<std::size_t>(template_param_count));
    for (std::uint32_t i = 0; i < template_param_count; i++) {
        auto template_param_r = read_generic_type_param(in, context + " template param");
        if (!template_param_r.has_value()) return std::unexpected(std::move(template_param_r).error());
        def.template_params.push_back(std::move(template_param_r).value());
    }
    auto template_owner_id_r = read_string(in, context + " template owner id");
    if (!template_owner_id_r.has_value()) return std::unexpected(std::move(template_owner_id_r).error());
    def.template_owner_id = std::move(template_owner_id_r).value();
    auto is_fwd_decl_r = read_u8(in, context + " is_forward_declaration");
    if (!is_fwd_decl_r.has_value()) return std::unexpected(std::move(is_fwd_decl_r).error());
    def.is_forward_declaration = is_fwd_decl_r.value() != 0u;
    auto is_synth_check_only_r = read_u8(in, context + " is_synthetic_check_only");
    if (!is_synth_check_only_r.has_value()) return std::unexpected(std::move(is_synth_check_only_r).error());
    def.is_synthetic_check_only = is_synth_check_only_r.value() != 0u;
    auto is_variadic_primary_r = read_u8(in, context + " is_variadic_primary");
    if (!is_variadic_primary_r.has_value()) return std::unexpected(std::move(is_variadic_primary_r).error());
    def.is_variadic_primary_template = is_variadic_primary_r.value() != 0u;
    auto is_variadic_spec_r = read_u8(in, context + " is_variadic_specialization");
    if (!is_variadic_spec_r.has_value()) return std::unexpected(std::move(is_variadic_spec_r).error());
    def.is_variadic_specialization = is_variadic_spec_r.value() != 0u;
    auto is_partial_spec_r = read_u8(in, context + " is_partial_specialization");
    if (!is_partial_spec_r.has_value()) return std::unexpected(std::move(is_partial_spec_r).error());
    def.is_partial_specialization = is_partial_spec_r.value() != 0u;
    auto spec_arg_count_r = read_u32_le(in, context + " specialization arg count");
    if (!spec_arg_count_r.has_value()) return std::unexpected(std::move(spec_arg_count_r).error());
    std::uint32_t spec_arg_count = spec_arg_count_r.value();
    def.specialization_template_args.reserve(static_cast<std::size_t>(spec_arg_count));
    for (std::uint32_t i = 0; i < spec_arg_count; i++) {
        auto spec_arg_r = read_type(in, context + " specialization arg");
        if (!spec_arg_r.has_value()) return std::unexpected(std::move(spec_arg_r).error());
        def.specialization_template_args.push_back(std::move(spec_arg_r).value());
    }
    auto thread_movable_override_r = read_u8(in, context + " thread movable override");
    if (!thread_movable_override_r.has_value()) return std::unexpected(std::move(thread_movable_override_r).error());
    def.thread_movable_override = thread_movable_override_r.value() != 0u;
    auto thread_shareable_override_r = read_u8(in, context + " thread shareable override");
    if (!thread_shareable_override_r.has_value()) return std::unexpected(std::move(thread_shareable_override_r).error());
    def.thread_shareable_override = thread_shareable_override_r.value() != 0u;
    auto movable_if_present_r = read_u8(in, context + " movable_if expr present");
    if (!movable_if_present_r.has_value()) return std::unexpected(std::move(movable_if_present_r).error());
    if (movable_if_present_r.value() != 0u) {
        auto movable_if_r = read_expr(in, context + " movable_if expr");
        if (!movable_if_r.has_value()) return std::unexpected(std::move(movable_if_r).error());
        def.thread_movable_if_movable_expr = std::move(movable_if_r).value();
    }
    auto shareable_if_present_r = read_u8(in, context + " shareable_if expr present");
    if (!shareable_if_present_r.has_value()) return std::unexpected(std::move(shareable_if_present_r).error());
    if (shareable_if_present_r.value() != 0u) {
        auto shareable_if_r = read_expr(in, context + " shareable_if expr");
        if (!shareable_if_r.has_value()) return std::unexpected(std::move(shareable_if_r).error());
        def.thread_movable_if_shareable_expr = std::move(shareable_if_r).value();
    }
    auto is_nodiscard_r = read_u8(in, context + " nodiscard");
    if (!is_nodiscard_r.has_value()) return std::unexpected(std::move(is_nodiscard_r).error());
    def.is_nodiscard = is_nodiscard_r.value() != 0u;
    auto nodiscard_reason_r = read_string(in, context + " nodiscard reason");
    if (!nodiscard_reason_r.has_value()) return std::unexpected(std::move(nodiscard_reason_r).error());
    def.nodiscard_reason = std::move(nodiscard_reason_r).value();
    return def;
}

inline void write_function(ByteWriter& out, const Function& fn) {
    write_type(out, fn.return_type);
    write_string(out, fn.name);
    write_source_location(out, fn.loc);
    write_u32_le(out, static_cast<std::uint32_t>(fn.params.size()));
    for (const Param& param : fn.params) write_param(out, param);
    write_string(out, fn.return_lifetime.name);
    write_u8(out, fn.body ? 1u : 0u);
    if (fn.body) write_stmt(out, *fn.body);
    write_u8(out, fn.is_extern_c ? 1u : 0u);
    write_u8(out, fn.is_module_extern ? 1u : 0u);
    write_u8(out, fn.is_unsafe ? 1u : 0u);
    write_u8(out, fn.is_nodiscard ? 1u : 0u);
    write_string(out, fn.nodiscard_reason);
    write_u8(out, fn.is_compile_time_dependency ? 1u : 0u);
    write_enum(out, fn.eval_mode);
    write_u8(out, fn.has_varargs ? 1u : 0u);
    write_string(out, fn.method_requires_concept);
    write_string(out, fn.method_requires_param);
    write_u8(out, fn.is_generic_template ? 1u : 0u);
    write_u32_le(out, static_cast<std::uint32_t>(fn.template_params.size()));
    for (const GenericTypeParam& param : fn.template_params) write_generic_type_param(out, param);
    write_string(out, fn.generic_method_owner_id);
    write_string(out, fn.member_owner_class);
    write_u32_le(out, static_cast<std::uint32_t>(fn.member_initializers.size()));
    for (const MemberInitializer& init : fn.member_initializers) write_member_initializer(out, init);
    write_enum(out, fn.receiver_ref_qualifier);
    write_u8(out, fn.is_static ? 1u : 0u);
    write_enum(out, fn.access);
    write_u8(out, fn.is_virtual ? 1u : 0u);
    write_u8(out, fn.is_override ? 1u : 0u);
    write_u8(out, fn.is_pure ? 1u : 0u);
    write_u8(out, fn.is_defaulted ? 1u : 0u);
    write_u8(out, fn.is_explicit ? 1u : 0u);
    write_u8(out, fn.is_deleted ? 1u : 0u);
    write_string(out, fn.forwards_to);
    write_u32_le(out, static_cast<std::uint32_t>(fn.namespace_path.size()));
    for (const std::string& segment : fn.namespace_path) write_string(out, segment);
    write_u8(out, fn.is_exported ? 1u : 0u);
    write_string(out, fn.owning_module);
}

[[nodiscard]] inline std::expected<Function, DriverError> read_function(ByteReader& in, const std::string& context) {
    Function fn{};
    auto return_type_r = read_type(in, context + " return type");
    if (!return_type_r.has_value()) return std::unexpected(std::move(return_type_r).error());
    fn.return_type = std::move(return_type_r).value();
    auto name_r = read_string(in, context + " name");
    if (!name_r.has_value()) return std::unexpected(std::move(name_r).error());
    fn.name = std::move(name_r).value();
    auto loc_r = read_source_location(in, context + " loc");
    if (!loc_r.has_value()) return std::unexpected(std::move(loc_r).error());
    fn.loc = std::move(loc_r).value();
    auto param_count_r = read_u32_le(in, context + " param count");
    if (!param_count_r.has_value()) return std::unexpected(std::move(param_count_r).error());
    std::uint32_t param_count = param_count_r.value();
    fn.params.reserve(static_cast<std::size_t>(param_count));
    for (std::uint32_t i = 0; i < param_count; i++) {
        auto param_r = read_param(in, context + " param");
        if (!param_r.has_value()) return std::unexpected(std::move(param_r).error());
        fn.params.push_back(std::move(param_r).value());
    }
    auto return_lifetime_r = read_string(in, context + " return lifetime");
    if (!return_lifetime_r.has_value()) return std::unexpected(std::move(return_lifetime_r).error());
    fn.return_lifetime.name = std::move(return_lifetime_r).value();
    auto body_present_r = read_u8(in, context + " body present");
    if (!body_present_r.has_value()) return std::unexpected(std::move(body_present_r).error());
    if (body_present_r.value() != 0u) {
        auto body_r = read_stmt(in, context + " body");
        if (!body_r.has_value()) return std::unexpected(std::move(body_r).error());
        fn.body = std::move(body_r.value());
    }
    auto extern_c_r = read_u8(in, context + " extern_c");
    if (!extern_c_r.has_value()) return std::unexpected(std::move(extern_c_r).error());
    fn.is_extern_c = extern_c_r.value() != 0u;
    auto module_extern_r = read_u8(in, context + " module_extern");
    if (!module_extern_r.has_value()) return std::unexpected(std::move(module_extern_r).error());
    fn.is_module_extern = module_extern_r.value() != 0u;
    auto unsafe_r = read_u8(in, context + " unsafe");
    if (!unsafe_r.has_value()) return std::unexpected(std::move(unsafe_r).error());
    fn.is_unsafe = unsafe_r.value() != 0u;
    auto nodiscard_r = read_u8(in, context + " nodiscard");
    if (!nodiscard_r.has_value()) return std::unexpected(std::move(nodiscard_r).error());
    fn.is_nodiscard = nodiscard_r.value() != 0u;
    auto nodiscard_reason_r = read_string(in, context + " nodiscard reason");
    if (!nodiscard_reason_r.has_value()) return std::unexpected(std::move(nodiscard_reason_r).error());
    fn.nodiscard_reason = std::move(nodiscard_reason_r).value();
    auto ctd_r = read_u8(in, context + " compile_time_dependency");
    if (!ctd_r.has_value()) return std::unexpected(std::move(ctd_r).error());
    fn.is_compile_time_dependency = ctd_r.value() != 0u;
    auto eval_mode_r = scpp::read_enum<FunctionEvalMode>(in, context + " eval mode");
    if (!eval_mode_r.has_value()) return std::unexpected(std::move(eval_mode_r).error());
    fn.eval_mode = eval_mode_r.value();
    auto varargs_r = read_u8(in, context + " has_varargs");
    if (!varargs_r.has_value()) return std::unexpected(std::move(varargs_r).error());
    fn.has_varargs = varargs_r.value() != 0u;
    auto method_requires_concept_r = read_string(in, context + " method_requires_concept");
    if (!method_requires_concept_r.has_value()) return std::unexpected(std::move(method_requires_concept_r).error());
    fn.method_requires_concept = std::move(method_requires_concept_r).value();
    auto method_requires_param_r = read_string(in, context + " method_requires_param");
    if (!method_requires_param_r.has_value()) return std::unexpected(std::move(method_requires_param_r).error());
    fn.method_requires_param = std::move(method_requires_param_r).value();
    auto is_generic_template_r = read_u8(in, context + " is_generic_template");
    if (!is_generic_template_r.has_value()) return std::unexpected(std::move(is_generic_template_r).error());
    fn.is_generic_template = is_generic_template_r.value() != 0u;
    auto template_param_count_r = read_u32_le(in, context + " template param count");
    if (!template_param_count_r.has_value()) return std::unexpected(std::move(template_param_count_r).error());
    std::uint32_t template_param_count = template_param_count_r.value();
    fn.template_params.reserve(static_cast<std::size_t>(template_param_count));
    for (std::uint32_t i = 0; i < template_param_count; i++) {
        auto template_param_r = read_generic_type_param(in, context + " template param");
        if (!template_param_r.has_value()) return std::unexpected(std::move(template_param_r).error());
        fn.template_params.push_back(std::move(template_param_r).value());
    }
    auto generic_method_owner_r = read_string(in, context + " generic method owner");
    if (!generic_method_owner_r.has_value()) return std::unexpected(std::move(generic_method_owner_r).error());
    fn.generic_method_owner_id = std::move(generic_method_owner_r).value();
    auto member_owner_class_r = read_string(in, context + " member owner class");
    if (!member_owner_class_r.has_value()) return std::unexpected(std::move(member_owner_class_r).error());
    fn.member_owner_class = std::move(member_owner_class_r).value();
    auto member_init_count_r = read_u32_le(in, context + " member initializer count");
    if (!member_init_count_r.has_value()) return std::unexpected(std::move(member_init_count_r).error());
    std::uint32_t member_init_count = member_init_count_r.value();
    fn.member_initializers.reserve(static_cast<std::size_t>(member_init_count));
    for (std::uint32_t i = 0; i < member_init_count; i++) {
        auto member_init_r = read_member_initializer(in, context + " member initializer");
        if (!member_init_r.has_value()) return std::unexpected(std::move(member_init_r).error());
        fn.member_initializers.push_back(std::move(member_init_r).value());
    }
    auto receiver_ref_qualifier_r = scpp::read_enum<ReceiverRefQualifier>(in, context + " receiver ref qualifier");
    if (!receiver_ref_qualifier_r.has_value()) return std::unexpected(std::move(receiver_ref_qualifier_r).error());
    fn.receiver_ref_qualifier = receiver_ref_qualifier_r.value();
    auto is_static_r = read_u8(in, context + " is_static");
    if (!is_static_r.has_value()) return std::unexpected(std::move(is_static_r).error());
    fn.is_static = is_static_r.value() != 0u;
    auto access_r = scpp::read_enum<AccessSpecifier>(in, context + " access");
    if (!access_r.has_value()) return std::unexpected(std::move(access_r).error());
    fn.access = access_r.value();
    auto is_virtual_r = read_u8(in, context + " is_virtual");
    if (!is_virtual_r.has_value()) return std::unexpected(std::move(is_virtual_r).error());
    fn.is_virtual = is_virtual_r.value() != 0u;
    auto is_override_r = read_u8(in, context + " is_override");
    if (!is_override_r.has_value()) return std::unexpected(std::move(is_override_r).error());
    fn.is_override = is_override_r.value() != 0u;
    auto is_pure_r = read_u8(in, context + " is_pure");
    if (!is_pure_r.has_value()) return std::unexpected(std::move(is_pure_r).error());
    fn.is_pure = is_pure_r.value() != 0u;
    auto is_defaulted_r = read_u8(in, context + " is_defaulted");
    if (!is_defaulted_r.has_value()) return std::unexpected(std::move(is_defaulted_r).error());
    fn.is_defaulted = is_defaulted_r.value() != 0u;
    auto is_explicit_r = read_u8(in, context + " is_explicit");
    if (!is_explicit_r.has_value()) return std::unexpected(std::move(is_explicit_r).error());
    fn.is_explicit = is_explicit_r.value() != 0u;
    auto is_deleted_r = read_u8(in, context + " is_deleted");
    if (!is_deleted_r.has_value()) return std::unexpected(std::move(is_deleted_r).error());
    fn.is_deleted = is_deleted_r.value() != 0u;
    auto forwards_to_r = read_string(in, context + " forwards_to");
    if (!forwards_to_r.has_value()) return std::unexpected(std::move(forwards_to_r).error());
    fn.forwards_to = std::move(forwards_to_r).value();
    auto ns_count_r = read_u32_le(in, context + " namespace count");
    if (!ns_count_r.has_value()) return std::unexpected(std::move(ns_count_r).error());
    std::uint32_t ns_count = ns_count_r.value();
    fn.namespace_path.reserve(static_cast<std::size_t>(ns_count));
    for (std::uint32_t i = 0; i < ns_count; i++) {
        auto ns_r = read_string(in, context + " namespace");
        if (!ns_r.has_value()) return std::unexpected(std::move(ns_r).error());
        fn.namespace_path.push_back(std::move(ns_r).value());
    }
    auto is_exported_r = read_u8(in, context + " is_exported");
    if (!is_exported_r.has_value()) return std::unexpected(std::move(is_exported_r).error());
    fn.is_exported = is_exported_r.value() != 0u;
    auto owning_module_r = read_string(in, context + " owning_module");
    if (!owning_module_r.has_value()) return std::unexpected(std::move(owning_module_r).error());
    fn.owning_module = std::move(owning_module_r).value();
    return fn;
}


// The Type comparison these merge helpers need is scpp::types_equal
// (scpp.ast). This file used to define types_equal_for_payload_merge, a
// fifth independent copy; it and the parser's were the two that compared
// non_type_args by value and is_pack_expansion at all.
[[nodiscard]] bool params_equal_for_payload_merge(const std::vector<Param>& a, const std::vector<Param>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        if (a[i].name != b[i].name || a[i].generic_concept != b[i].generic_concept ||
            a[i].require_thread_movable != b[i].require_thread_movable ||
            a[i].require_thread_shareable != b[i].require_thread_shareable ||
            a[i].is_parameter_pack != b[i].is_parameter_pack ||
            !types_equal(a[i].type, b[i].type)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool same_function_identity_for_payload_merge(const Function& a, const Function& b) {
    return a.name == b.name && types_equal(a.return_type, b.return_type) &&
           params_equal_for_payload_merge(a.params, b.params) && a.receiver_ref_qualifier == b.receiver_ref_qualifier &&
           a.is_nodiscard == b.is_nodiscard && a.nodiscard_reason == b.nodiscard_reason &&
           a.member_owner_class == b.member_owner_class && a.is_static == b.is_static && a.access == b.access &&
           a.is_virtual == b.is_virtual && a.is_override == b.is_override && a.is_pure == b.is_pure &&
           a.is_defaulted == b.is_defaulted && a.is_explicit == b.is_explicit && a.is_deleted == b.is_deleted;
}

[[nodiscard]] bool same_template_param_shape(const std::vector<GenericTypeParam>& a,
                                                const std::vector<GenericTypeParam>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        if (a[i].name != b[i].name || a[i].concept_name != b[i].concept_name ||
            a[i].is_non_type != b[i].is_non_type || a[i].is_pack != b[i].is_pack) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool same_specialization_args(const std::vector<Type>& a, const std::vector<Type>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        if (!types_equal(a[i], b[i])) return false;
    }
    return true;
}

[[nodiscard]] bool same_struct_identity_for_payload_merge(const StructDef& a, const StructDef& b) {
    return a.name == b.name && a.is_union == b.is_union && a.is_nodiscard == b.is_nodiscard &&
           a.nodiscard_reason == b.nodiscard_reason &&
           same_template_param_shape(a.template_params, b.template_params);
}

[[nodiscard]] bool same_class_identity_for_payload_merge(const ClassDef& a, const ClassDef& b) {
    return a.name == b.name && a.is_variadic_primary_template == b.is_variadic_primary_template &&
           a.is_variadic_specialization == b.is_variadic_specialization &&
           a.is_partial_specialization == b.is_partial_specialization &&
           a.is_nodiscard == b.is_nodiscard && a.nodiscard_reason == b.nodiscard_reason &&
           same_template_param_shape(a.template_params, b.template_params) &&
           same_specialization_args(a.specialization_template_args, b.specialization_template_args);
}


class GenericMethodOwnerRemap {
public:
    virtual ~GenericMethodOwnerRemap() = default;
    std::string old_owner_id{};
    std::string new_owner_id{};
    std::string class_name{};

    GenericMethodOwnerRemap() = default;
    GenericMethodOwnerRemap(std::string old_id, std::string new_id, std::string cls)
        : old_owner_id{std::move(old_id)}, new_owner_id{std::move(new_id)}, class_name{std::move(cls)} {}
};

[[nodiscard]] std::string rewrite_generic_method_name_for_owner(const Function& fn,
                                                                const GenericMethodOwnerRemap& remap) {
    std::string old_prefix = remap.class_name + "__" + remap.old_owner_id;
    std::string new_prefix = remap.class_name + "__" + remap.new_owner_id;
    if (fn.name.starts_with(old_prefix)) return new_prefix + fn.name.substr(old_prefix.size());
    std::string name = fn.name;
    return name;
}

[[nodiscard]] bool is_local_module_enum(const EnumDef& def) { return def.owning_module.empty(); }
[[nodiscard]] bool is_local_module_struct(const StructDef& def) { return def.owning_module.empty(); }
[[nodiscard]] bool is_local_module_class(const ClassDef& def) { return def.owning_module.empty(); }
[[nodiscard]] bool is_local_module_function(const Function& fn) { return fn.owning_module.empty(); }

[[nodiscard]] std::string serialize_compile_time_payload(const Program& program) {
    CompileTimePayloadPlan plan = plan_compile_time_payload(program);
    if (plan.root_function_names.empty()) return {};

    std::unordered_set<std::size_t> reachable_function_indices{};
    for (std::size_t idx : plan.reachable_function_indices) reachable_function_indices.insert(idx);
    std::unordered_set<std::string> reachable_type_names{};
    for (const std::string& name : plan.reachable_type_names) reachable_type_names.insert(name);
    std::vector<std::size_t> structs{};
    std::vector<std::size_t> classes{};
    std::vector<std::size_t> enums{};
    std::vector<std::size_t> functions{};
    for (std::size_t i = 0; i < program.enums.size(); i++) {
        const EnumDef& def = program.enums[i];
        if (is_local_module_enum(def) && reachable_type_names.contains(def.name)) {
            enums.push_back(i);
        }
    }
    for (std::size_t i = 0; i < program.structs.size(); i++) {
        const StructDef& def = program.structs[i];
        if (is_local_module_struct(def) && reachable_type_names.contains(def.name)) structs.push_back(i);
    }
    for (std::size_t i = 0; i < program.classes.size(); i++) {
        const ClassDef& def = program.classes[i];
        if (is_local_module_class(def) && reachable_type_names.contains(def.name)) classes.push_back(i);
    }
    for (std::size_t i = 0; i < program.functions.size(); i++) {
        const Function& fn = program.functions[i];
        if (!is_local_module_function(fn)) continue;
        if (reachable_function_indices.contains(i)) functions.push_back(i);
    }
    ByteWriter payload{};
    payload.write(SCPPM_COMPILE_TIME_AST_MAGIC.data(), SCPPM_COMPILE_TIME_AST_MAGIC.size());
    write_u32_le(payload, SCPPM_COMPILE_TIME_AST_VERSION);
    write_u32_le(payload, static_cast<std::uint32_t>(plan.root_function_names.size()));
    for (const std::string& name : plan.root_function_names) write_string(payload, name);
    write_u32_le(payload, static_cast<std::uint32_t>(enums.size()));
    for (std::size_t idx : enums) write_enum_def(payload, program.enums[idx]);
    write_u32_le(payload, static_cast<std::uint32_t>(structs.size()));
    for (std::size_t idx : structs) write_struct_def(payload, program.structs[idx]);
    write_u32_le(payload, static_cast<std::uint32_t>(classes.size()));
    for (std::size_t idx : classes) write_class_def(payload, program.classes[idx]);
    write_u32_le(payload, static_cast<std::uint32_t>(functions.size()));
    for (std::size_t idx : functions) write_function(payload, program.functions[idx]);
    return payload.str();
}

[[nodiscard]] inline std::expected<StructuredCompileTimePayload, DriverError> deserialize_compile_time_payload(std::string_view bytes, const std::string& path) {
    ByteReader in{bytes};
    char magic[4] = {};
    if (!in.read(magic, 4) || std::string_view(magic, 4) != SCPPM_COMPILE_TIME_AST_MAGIC) {
        return std::unexpected(DriverError("invalid .scppm file '" + path + "': bad structured compile-time payload magic"));
    }
    auto version_r = read_u32_le(in, path + " payload version");
    if (!version_r.has_value()) return std::unexpected(std::move(version_r).error());
    std::uint32_t version = version_r.value();
    if (version != SCPPM_COMPILE_TIME_AST_VERSION) {
        return std::unexpected(DriverError("unsupported structured compile-time payload version " + std::to_string(static_cast<std::size_t>(version)) +
                          " in '" + path + "'"));
    }
    StructuredCompileTimePayload payload{};
    auto root_count_r = read_u32_le(in, path + " root count");
    if (!root_count_r.has_value()) return std::unexpected(std::move(root_count_r).error());
    std::uint32_t root_count = root_count_r.value();
    payload.root_function_names.reserve(static_cast<std::size_t>(root_count));
    for (std::uint32_t i = 0; i < root_count; i++) {
        auto root_r = read_string(in, path + " root");
        if (!root_r.has_value()) return std::unexpected(std::move(root_r).error());
        payload.root_function_names.push_back(std::move(root_r).value());
    }
    auto enum_count_r = read_u32_le(in, path + " enum count");
    if (!enum_count_r.has_value()) return std::unexpected(std::move(enum_count_r).error());
    std::uint32_t enum_count = enum_count_r.value();
    payload.enums.reserve(static_cast<std::size_t>(enum_count));
    for (std::uint32_t i = 0; i < enum_count; i++) {
        auto enum_r = read_enum_def(in, path + " enum");
        if (!enum_r.has_value()) return std::unexpected(std::move(enum_r).error());
        payload.enums.push_back(std::move(enum_r).value());
    }
    auto struct_count_r = read_u32_le(in, path + " struct count");
    if (!struct_count_r.has_value()) return std::unexpected(std::move(struct_count_r).error());
    std::uint32_t struct_count = struct_count_r.value();
    payload.structs.reserve(static_cast<std::size_t>(struct_count));
    for (std::uint32_t i = 0; i < struct_count; i++) {
        auto struct_r = read_struct_def(in, path + " struct");
        if (!struct_r.has_value()) return std::unexpected(std::move(struct_r).error());
        payload.structs.push_back(std::move(struct_r).value());
    }
    auto class_count_r = read_u32_le(in, path + " class count");
    if (!class_count_r.has_value()) return std::unexpected(std::move(class_count_r).error());
    std::uint32_t class_count = class_count_r.value();
    payload.classes.reserve(static_cast<std::size_t>(class_count));
    for (std::uint32_t i = 0; i < class_count; i++) {
        auto class_r = read_class_def(in, path + " class");
        if (!class_r.has_value()) return std::unexpected(std::move(class_r).error());
        payload.classes.push_back(std::move(class_r).value());
    }
    auto function_count_r = read_u32_le(in, path + " function count");
    if (!function_count_r.has_value()) return std::unexpected(std::move(function_count_r).error());
    std::uint32_t function_count = function_count_r.value();
    payload.functions.reserve(static_cast<std::size_t>(function_count));
    for (std::uint32_t i = 0; i < function_count; i++) {
        auto function_r = read_function(in, path + " function");
        if (!function_r.has_value()) return std::unexpected(std::move(function_r).error());
        payload.functions.push_back(std::move(function_r).value());
    }
    std::expected<StructuredCompileTimePayload, DriverError> ret{std::move(payload)};
    return ret;
}

[[nodiscard]] bool program_requires_structured_payload(const Program& program) {
    CompileTimePayloadPlan plan = plan_compile_time_payload(program);
    return !plan.root_function_names.empty();
}

void mark_reachable_hidden_compile_time_dependencies(Program& program) {
    CompileTimePayloadPlan plan = plan_compile_time_payload(program);
    std::unordered_set<std::size_t> reachable_function_indices{};
    for (std::size_t idx : plan.reachable_function_indices) reachable_function_indices.insert(idx);
    std::unordered_set<std::string> reachable_type_names{};
    for (const std::string& name : plan.reachable_type_names) reachable_type_names.insert(name);
    for (EnumDef& def : program.enums) {
        if (!def.is_exported && def.owning_module.empty() && reachable_type_names.contains(def.name)) {
            def.is_compile_time_dependency = true;
        }
    }
    for (StructDef& def : program.structs) {
        if (!def.is_exported && def.owning_module.empty() && reachable_type_names.contains(def.name)) {
            def.is_compile_time_dependency = true;
        }
    }
    for (ClassDef& def : program.classes) {
        if (!def.is_exported && def.owning_module.empty() && reachable_type_names.contains(def.name)) {
            def.is_compile_time_dependency = true;
        }
    }
    for (std::size_t i = 0; i < program.functions.size(); i++) {
        if (!program.functions[i].is_exported && program.functions[i].owning_module.empty() &&
            reachable_function_indices.contains(i)) {
            program.functions[i].is_compile_time_dependency = true;
        }
        if (program.functions[i].body && program.functions[i].is_compile_time_dependency &&
            program.functions[i].eval_mode == FunctionEvalMode::RuntimeOnly) {
            program.functions[i].skip_imported_body_verification = true;
        }
    }
}

void merge_compile_time_payload(Program& imported, StructuredCompileTimePayload&& payload) {
    std::vector<GenericMethodOwnerRemap> owner_remaps{};
    for (EnumDef& def : payload.enums) {
        if (!def.is_exported) def.is_compile_time_dependency = true;
        std::size_t found_idx = imported.enums.size();
        for (std::size_t j = 0; j < imported.enums.size(); j++) {
            if (imported.enums[j].name == def.name) {
                found_idx = j;
                break;
            }
        }
        if (found_idx < imported.enums.size()) {
            imported.enums[found_idx] = std::move(def);
        } else {
            imported.enums.push_back(std::move(def));
        }
    }
    for (StructDef& def : payload.structs) {
        if (!def.is_exported) def.is_compile_time_dependency = true;
        std::size_t found_idx = imported.structs.size();
        for (std::size_t j = 0; j < imported.structs.size(); j++) {
            if (same_struct_identity_for_payload_merge(imported.structs[j], def)) {
                found_idx = j;
                break;
            }
        }
        if (found_idx < imported.structs.size()) {
            imported.structs[found_idx] = std::move(def);
        } else {
            imported.structs.push_back(std::move(def));
        }
    }
    for (ClassDef& def : payload.classes) {
        if (!def.is_exported) def.is_compile_time_dependency = true;
        std::size_t found_idx = imported.classes.size();
        for (std::size_t j = 0; j < imported.classes.size(); j++) {
            if (same_class_identity_for_payload_merge(imported.classes[j], def)) {
                found_idx = j;
                break;
            }
        }
        if (found_idx < imported.classes.size()) {
            if (!imported.classes[found_idx].template_owner_id.empty() &&
                imported.classes[found_idx].template_owner_id != def.template_owner_id) {
                owner_remaps.push_back(GenericMethodOwnerRemap{
                    imported.classes[found_idx].template_owner_id, def.template_owner_id, def.name});
            }
            imported.classes[found_idx] = std::move(def);
        } else {
            imported.classes.push_back(std::move(def));
        }
    }
    for (const GenericMethodOwnerRemap& remap : owner_remaps) {
        for (Function& fn : imported.functions) {
            if (fn.generic_method_owner_id != remap.old_owner_id) continue;
            fn.name = rewrite_generic_method_name_for_owner(fn, remap);
            fn.generic_method_owner_id = remap.new_owner_id;
        }
    }
    for (Function& fn : payload.functions) {
        if (!fn.is_exported) fn.is_compile_time_dependency = true;
        if (fn.body && fn.is_compile_time_dependency && fn.eval_mode == FunctionEvalMode::RuntimeOnly) {
            fn.skip_imported_body_verification = true;
        }
        std::size_t found_idx = imported.functions.size();
        for (std::size_t j = 0; j < imported.functions.size(); j++) {
            if (same_function_identity_for_payload_merge(imported.functions[j], fn)) {
                found_idx = j;
                break;
            }
        }
        if (found_idx < imported.functions.size()) {
            imported.functions[found_idx] = std::move(fn);
        } else {
            imported.functions.push_back(std::move(fn));
        }
    }
}

export extern "C" {
    int open(const char* pathname, int flags, ...);
    int close(int fd);
    long read(int fd, void* buf, unsigned long count);
    long write(int fd, const void* buf, unsigned long count);
    int system(const char* command);
    char* getenv(const char* name);
}

[[nodiscard]] inline std::expected<void, DriverError> write_file_bytes(const std::string& path, std::string_view content) {
    int fd = -1;
    [[scpp::unsafe]] {
        fd = open(path.c_str(), 0x241, 0644);
    }
    if (fd < 0) {
        return std::unexpected(DriverError("cannot open file for writing: " + path));
    }
    std::size_t total_written = 0;
    while (total_written < content.size()) {
        long n = 0;
        [[scpp::unsafe]] {
            n = write(fd, content.data() + total_written, static_cast<unsigned long>(content.size() - total_written));
        }
        if (n <= 0) {
            [[scpp::unsafe]] { close(fd); }
            return std::unexpected(DriverError("failed writing to file: " + path));
        }
        total_written += static_cast<std::size_t>(n);
    }
    int close_rc = 0;
    [[scpp::unsafe]] {
        close_rc = close(fd);
    }
    if (close_rc != 0) {
        return std::unexpected(DriverError("failed closing file: " + path));
    }
    return {};
}

[[nodiscard]] inline std::expected<std::string, DriverError> read_file_bytes(const std::string& path) {
    int fd = -1;
    [[scpp::unsafe]] {
        fd = open(path.c_str(), 0);
    }
    if (fd < 0) {
        return std::unexpected(DriverError("cannot open file for reading: " + path));
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
            return std::unexpected(DriverError("failed reading file: " + path));
        }
        if (n == 0) break;
        result.append(std::string(buf, static_cast<std::size_t>(n)));
    }
    [[scpp::unsafe]] {
        close(fd);
    }
    return std::move(result);
}

[[nodiscard]] inline std::expected<void, DriverError> write_scppm_file(const Program& program, std::string_view interface_source, const std::string& path) {
    std::string payload = serialize_compile_time_payload(program);
    std::uint8_t flags = payload.empty() ? static_cast<std::uint8_t>(0) : static_cast<std::uint8_t>(0x01);
    ByteWriter out{};
    const char header[8] = {'S', 'C', 'P', 'P', 'M', static_cast<char>(1), '\0', static_cast<char>(flags)};
    out.write(&header[0], 8);
    write_u32_le(out, static_cast<std::uint32_t>(interface_source.size()));
    out.write(interface_source.data(), static_cast<std::size_t>(interface_source.size()));
    if ((flags & 0x01u) != 0u) {
        write_u32_le(out, static_cast<std::uint32_t>(payload.size()));
        out.write(payload.c_str(), static_cast<std::size_t>(payload.size()));
    }
    auto write_r = write_file_bytes(path, out.str());
    if (!write_r.has_value()) {
        return std::unexpected(DriverError("failed while writing module interface '" + path + "'"));
    }
    return {};
}

[[nodiscard]] inline std::expected<LoadedModuleFile, DriverError> read_module_file(const std::string& path) {
    LoadedModuleFile loaded{};
    auto bytes_r = read_file_bytes(path);
    if (!bytes_r.has_value()) {
        return std::unexpected(DriverError("cannot open imported module source '" + path + "'"));
    }
    const std::string& bytes = bytes_r.value();
    if (!path.ends_with(".scppm")) {
        loaded.interface_source = bytes;
        std::expected<LoadedModuleFile, DriverError> ret{std::move(loaded)};
        return ret;
    }

    loaded.is_scppm = true;
    ByteReader in{bytes};
    char header[8] = {};
    if (!in.read(header, 8)) {
        return std::unexpected(DriverError("invalid .scppm file '" + path + "': truncated header"));
    }
    if (header[0] != 'S' || header[1] != 'C' || header[2] != 'P' || header[3] != 'P' || header[4] != 'M') {
        return std::unexpected(DriverError("invalid .scppm file '" + path + "': bad magic"));
    }
    std::uint8_t major_version = static_cast<std::uint8_t>(header[5]);
    if (major_version != 1) {
        return std::unexpected(DriverError("unsupported .scppm major version " + std::to_string(static_cast<std::size_t>(major_version)) + " in '" + path + "'"));
    }
    std::uint8_t flags = static_cast<std::uint8_t>(header[7]);
    auto interface_length_r = read_u32_le(in, path + " interface length");
    if (!interface_length_r.has_value()) return std::unexpected(std::move(interface_length_r).error());
    std::uint32_t interface_length = interface_length_r.value();
    if (in.pos + static_cast<std::size_t>(interface_length) > in.data.size()) {
        return std::unexpected(DriverError("invalid .scppm file '" + path + "': truncated interface source"));
    }
    loaded.interface_source = string_from_view(in.data.substr(in.pos, static_cast<std::size_t>(interface_length)));
    in.pos += static_cast<std::size_t>(interface_length);
    if ((flags & 0x01u) != 0u) {
        loaded.has_compile_time_payload = true;
        auto payload_length_r = read_u32_le(in, path + " payload length");
        if (!payload_length_r.has_value()) return std::unexpected(std::move(payload_length_r).error());
        std::uint32_t payload_length = payload_length_r.value();
        if (in.pos + static_cast<std::size_t>(payload_length) > in.data.size()) {
            return std::unexpected(DriverError("invalid .scppm file '" + path + "': truncated structured payload"));
        }
        loaded.compile_time_payload_bytes = string_from_view(in.data.substr(in.pos, static_cast<std::size_t>(payload_length)));
        in.pos += static_cast<std::size_t>(payload_length);
    }
    std::expected<LoadedModuleFile, DriverError> ret{std::move(loaded)};
    return ret;
}

[[nodiscard]] std::expected<void, DriverError> create_archive(const std::vector<std::string>& object_paths, const std::string& archive_path) {
    if (object_paths.empty()) {
        return std::unexpected(DriverError("archive command requires at least one object file for '" + archive_path + "'"));
    }
    std::string command = "ar rcs \"" + archive_path + "\"";
    for (const std::string& object_path : object_paths) {
        command += " \"" + object_path + "\"";
    }
    int result = 0;
    [[scpp::unsafe]] {
        result = system(command.c_str());
    }
    if (result != 0) {
        return std::unexpected(DriverError("archive command failed: " + command));
    }
    return {};
}

export extern "C" {
    struct dirent {
        unsigned long d_ino;
        long d_off;
        std::uint16_t d_reclen;
        std::uint8_t d_type;
        char d_name[256];
    };
    struct posix_stat {
        unsigned long st_dev;
        unsigned long st_ino;
        unsigned long st_nlink;
        unsigned int st_mode;
        char __pad[116];
    };
    void* opendir(const char* name);
    dirent* readdir(void* dirp);
    int closedir(void* dirp);
    int stat(const char* pathname, posix_stat* statbuf);
    long readlink(const char* path, char* buf, unsigned long bufsiz);
    int access(const char* pathname, int mode);
    int unlink(const char* pathname);
    char* realpath(const char* path, char* resolved_path);
}

[[nodiscard]] inline bool path_ends_with(std::string_view path, std::string_view suffix) {
    if (path.size() < suffix.size()) return false;
    return path.substr(path.size() - suffix.size()) == suffix;
}

[[nodiscard]] inline std::string path_parent(std::string_view p) {
    if (p.empty()) return ".";
    std::size_t slash = p.rfind("/");
    if (slash == std::string_view::npos) return ".";
    if (slash == 0) return "/";
    return string_from_view(p.substr(0, slash));
}

[[nodiscard]] inline std::string path_filename(std::string_view p) {
    if (p.empty()) return "";
    std::size_t slash = p.rfind("/");
    if (slash == std::string_view::npos) return string_from_view(p);
    return string_from_view(p.substr(slash + 1));
}

[[nodiscard]] inline std::string path_join(std::string_view a, std::string_view b) {
    if (a.empty()) return string_from_view(b);
    if (b.empty()) return string_from_view(a);
    if (a.at(a.size() - 1) == '/') return string_from_view(a) + string_from_view(b);
    return string_from_view(a) + "/" + string_from_view(b);
}

[[nodiscard]] inline bool path_exists(const std::string& path) {
    int rc = -1;
    [[scpp::unsafe]] {
        rc = access(path.c_str(), 0);
    }
    return rc == 0;
}

inline void path_remove(const std::string& path) {
    [[scpp::unsafe]] {
        unlink(path.c_str());
    }
}

[[nodiscard]] inline std::string path_lexically_normal(std::string_view p) {
    std::vector<std::string> segments{};
    bool is_absolute = !p.empty() && p.at(0) == '/';
    std::size_t i = 0;
    while (i < p.size()) {
        while (i < p.size() && p.at(i) == '/') i++;
        if (i >= p.size()) break;
        std::size_t start = i;
        while (i < p.size() && p.at(i) != '/') i++;
        std::string_view seg = p.substr(start, i - start);
        if (seg == ".") {
            continue;
        } else if (seg == "..") {
            if (!segments.empty() && segments.back() != "..") {
                segments.pop_back();
            } else if (!is_absolute) {
                segments.push_back("..");
            }
        } else {
            segments.push_back(string_from_view(seg));
        }
    }
    if (segments.empty()) {
        return is_absolute ? "/" : ".";
    }
    std::string result{};
    for (std::size_t idx = 0; idx < segments.size(); idx++) {
        if (is_absolute || idx > 0) result += "/";
        result += segments[idx];
    }
    return result;
}

[[nodiscard]] inline std::optional<std::string> current_executable_path() {
    char buf[4096] = {};
    long len = 0;
    [[scpp::unsafe]] {
        len = readlink("/proc/self/exe", buf, 4095);
    }
    if (len <= 0) return std::nullopt;
    buf[len] = '\0';
    return std::string(buf, static_cast<std::size_t>(len));
}

[[nodiscard]] inline std::optional<std::string> runtime_default_prebuilt_stdlib_dir() {
    std::optional<std::string> exe = current_executable_path();
    if (!exe.has_value()) return std::nullopt;
    return path_lexically_normal(path_join(path_parent(*exe), "libs"));
}

[[nodiscard]] inline std::optional<std::string> runtime_installed_stdlib_dir() {
    std::optional<std::string> exe = current_executable_path();
    if (!exe.has_value()) return std::nullopt;
    return path_lexically_normal(path_join(path_join(path_join(path_parent(*exe), ".."), "share"), "scpp/libs"));
}

[[nodiscard]] inline std::optional<std::string> runtime_default_source_stdlib_dir() {
    std::optional<std::string> exe = current_executable_path();
    if (!exe.has_value()) return std::nullopt;
    return path_lexically_normal(path_join(path_join(path_parent(*exe), ".."), "libs"));
}

[[nodiscard]] inline std::vector<std::string> build_default_import_search_dirs(const std::vector<std::string>& explicit_dirs) {
    std::vector<std::string> dirs = explicit_dirs;
    auto append_if_missing = [&](std::string path) {
        if (path.empty()) return;
        bool found = false;
        for (const std::string& d : dirs) {
            if (d == path) {
                found = true;
                break;
            }
        }
        if (!found) dirs.push_back(std::move(path));
    };
    auto append_module_dirs = [&](const std::string& base) {
        append_if_missing(base);
        append_if_missing(path_join(base, "std"));
        append_if_missing(path_join(base, "scpp"));
    };
    const char* env = nullptr;
    [[scpp::unsafe]] {
        env = getenv("SCPP_STDLIB_PATH");
    }
    bool env_valid = false;
    if (env != nullptr) {
        [[scpp::unsafe]] {
            env_valid = (env[0] != '\0');
        }
    }
    if (env_valid) {
        append_module_dirs(env);
    } else {
        std::optional<std::string> prebuilt_dir = runtime_default_prebuilt_stdlib_dir();
        if (prebuilt_dir.has_value()) {
            append_module_dirs(*prebuilt_dir);
        }
        std::optional<std::string> installed_dir = runtime_installed_stdlib_dir();
        if (installed_dir.has_value()) {
            append_module_dirs(*installed_dir);
        }
        std::optional<std::string> source_dir = runtime_default_source_stdlib_dir();
        if (source_dir.has_value()) {
            append_module_dirs(*source_dir);
        }
    }
    return dirs;
}

[[nodiscard]] inline std::vector<std::string> default_stdlib_link_inputs() {
    std::vector<std::string> result{};
    auto append_if_exists = [&](const std::string& lib_path) {
        if (!path_exists(lib_path)) return;
        bool found = false;
        for (const std::string& r : result) {
            if (r == lib_path) {
                found = true;
                break;
            }
        }
        if (!found) {
            result.push_back(lib_path);
        }
    };
    std::vector<std::optional<std::string>> candidate_dirs{};
    candidate_dirs.push_back(runtime_default_prebuilt_stdlib_dir());
    candidate_dirs.push_back(runtime_installed_stdlib_dir());
    for (const std::optional<std::string>& lib_dir : candidate_dirs) {
        if (!lib_dir.has_value()) continue;
        append_if_exists(path_join(*lib_dir, "libstd.scppa"));
        append_if_exists(path_join(*lib_dir, "libscpp.scppa"));
    }
    return result;
}

[[nodiscard]] inline std::string absolute_source_path(const std::string& path) {
    char buf[4096] = {};
    char* res = nullptr;
    [[scpp::unsafe]] {
        res = realpath(path.c_str(), buf);
    }
    if (res != nullptr) return std::string(buf);
    std::string fallback = path;
    return fallback;
}

[[nodiscard]] inline std::vector<std::size_t> line_offsets(std::string_view source) {
    std::vector<std::size_t> offsets{};
    offsets.push_back(static_cast<std::size_t>(0));
    for (std::size_t i = 0; i < source.size(); i++) {
        if (source.at(i) == '\n') offsets.push_back(i + 1);
    }
    return offsets;
}

[[nodiscard]] std::size_t offset_for_loc(std::string_view source, const SourceLocation& loc) {
    std::vector<std::size_t> offsets = line_offsets(source);
    std::size_t line_index = static_cast<std::size_t>(std::max(loc.line, 1) - 1);
    if (line_index >= offsets.size()) return source.size();
    return std::min(offsets[line_index] + static_cast<std::size_t>(std::max(loc.column, 1) - 1), source.size());
}

[[nodiscard]] std::expected<std::size_t, DriverError> find_matching_brace(std::string_view source, std::size_t open_offset) {
    bool in_string = false;
    bool in_char = false;
    bool in_line_comment = false;
    bool in_block_comment = false;
    int depth = 0;
    for (std::size_t i = open_offset; i < source.size(); i++) {
        char c = source.at(i);
        char next = i + 1 < source.size() ? source.at(i + 1) : '\0';
        if (in_line_comment) {
            if (c == '\n') in_line_comment = false;
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && next == '/') {
                in_block_comment = false;
                i++;
            }
            continue;
        }
        if (in_string) {
            if (c == '\\' && next != '\0') {
                i++;
                continue;
            }
            if (c == '"') in_string = false;
            continue;
        }
        if (in_char) {
            if (c == '\\' && next != '\0') {
                i++;
                continue;
            }
            if (c == '\'') in_char = false;
            continue;
        }
        if (c == '/' && next == '/') {
            in_line_comment = true;
            i++;
            continue;
        }
        if (c == '/' && next == '*') {
            in_block_comment = true;
            i++;
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '\'') {
            in_char = true;
            continue;
        }
        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) return i;
        }
    }
    return std::unexpected(DriverError("failed to locate end of function body while writing module interface"));
}

// Scans forward from a bodyless declaration's own start (e.g. a
// standalone `Type f(...);` ordinary forward declaration -- see
// Function::superseded_forward_declaration_locs) for its terminating
// top-level `;`, the same string/char/comment-aware way find_matching_
// brace above locates a body's closing `}`. Tracks `()`/`{}` nesting
// depth too (not just parens) since a parameter's default argument can
// itself be a brace-init expression or a lambda with its own nested
// `;`-containing body.
[[nodiscard]] std::expected<std::size_t, DriverError> find_declaration_semicolon(std::string_view source, std::size_t decl_begin) {
    bool in_string = false;
    bool in_char = false;
    bool in_line_comment = false;
    bool in_block_comment = false;
    int depth = 0;
    for (std::size_t i = decl_begin; i < source.size(); i++) {
        char c = source.at(i);
        char next = i + 1 < source.size() ? source.at(i + 1) : '\0';
        if (in_line_comment) {
            if (c == '\n') in_line_comment = false;
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && next == '/') {
                in_block_comment = false;
                i++;
            }
            continue;
        }
        if (in_string) {
            if (c == '\\' && next != '\0') {
                i++;
                continue;
            }
            if (c == '"') in_string = false;
            continue;
        }
        if (in_char) {
            if (c == '\\' && next != '\0') {
                i++;
                continue;
            }
            if (c == '\'') in_char = false;
            continue;
        }
        if (c == '/' && next == '/') {
            in_line_comment = true;
            i++;
            continue;
        }
        if (c == '/' && next == '*') {
            in_block_comment = true;
            i++;
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '\'') {
            in_char = true;
            continue;
        }
        if (c == '(' || c == '{') depth++;
        else if (c == ')' || c == '}') {
            if (depth > 0) depth--;
        } else if (c == ';' && depth == 0) {
            return i;
        }
    }
    return std::unexpected(DriverError("failed to locate end of superseded forward declaration while writing module interface"));
}

// A standalone forward declaration's own recorded Function::loc starts
// right at its return type -- any leading `export` keyword (ch11 §11.3)
// was already consumed by the caller (parse_top_level_function_or_
// extern_group) before that location was captured, so it sits *before*
// decl_begin and would otherwise survive as dangling, orphaned text once
// the rest of the (now-redundant) declaration is stripped out below.
// Widens decl_begin backward over exactly one such keyword, if present
// immediately before it (skipping only whitespace in between, and
// checking it is a standalone word rather than e.g. a longer identifier
// ending in "export").
[[nodiscard]] std::size_t widen_declaration_begin_over_leading_export(std::string_view source, std::size_t decl_begin) {
    std::size_t i = decl_begin;
    while (i > 0 && is_ascii_space(source.at(i - 1))) i--;
    constexpr std::size_t kExportLen = 6;
    if (i < kExportLen) return decl_begin;
    std::size_t candidate = i - kExportLen;
    if (source.substr(candidate, kExportLen) != "export") return decl_begin;
    bool boundary_before = candidate == 0 ||
        !(is_ascii_alnum(source.at(candidate - 1)) || source.at(candidate - 1) == '_');
    if (!boundary_before) return decl_begin;
    return candidate;
}

// A standalone forward declaration's own recorded Function::loc also
// sits *after* any leading `[[...]]` attribute-specifier-seq (ch01
// §1.2/§1.3's `[[nodiscard]]`, `[[nodiscard("reason")]]`,
// `[[scpp::unsafe]]`, `[[packed]]`, ...) for the very same reason as
// widen_declaration_begin_over_leading_export above -- parse_top_level_
// item consumes it before dispatching into the function-parsing path
// that captures loc. Scans backward for the attribute-seq's own opening
// `[[`, bailing out (leaving decl_begin unwidened) if a statement/scope
// boundary character is hit first instead -- a defensive fallback for
// any attribute shape more exotic than this codebase's own current,
// argument-free forms.
[[nodiscard]] std::size_t widen_declaration_begin_over_leading_attribute(std::string_view source, std::size_t decl_begin) {
    std::size_t i = decl_begin;
    while (i > 0 && is_ascii_space(source.at(i - 1))) i--;
    if (i < 2 || source.at(i - 1) != ']' || source.at(i - 2) != ']') return decl_begin;
    std::size_t j = i - 2;
    while (j > 0) {
        j--;
        if (source.at(j) == ';' || source.at(j) == '{' || source.at(j) == '}') return decl_begin;
        if (source.at(j) == '[' && j > 0 && source.at(j - 1) == '[') return j - 1;
    }
    return decl_begin;
}

// Applies both widen_declaration_begin_over_leading_{export,attribute}
// repeatedly (in either order -- the grammar always writes `export`
// before an attribute-seq, but looping until neither helper can widen
// any further is simpler than hard-coding that assumption) until
// decl_begin no longer moves.
[[nodiscard]] std::size_t widen_declaration_begin_over_leading_modifiers(std::string_view source, std::size_t decl_begin) {
    for (;;) {
        std::size_t widened = widen_declaration_begin_over_leading_export(source, decl_begin);
        widened = widen_declaration_begin_over_leading_attribute(source, widened);
        if (widened == decl_begin) return decl_begin;
        decl_begin = widened;
    }
}

[[nodiscard]] std::optional<std::size_t> find_constructor_member_initializer_colon(std::string_view source,
                                                                               std::size_t signature_begin,
                                                                               std::size_t body_begin) {
    bool in_string = false;
    bool in_char = false;
    bool in_line_comment = false;
    bool in_block_comment = false;
    int paren_depth = 0;
    bool saw_param_list_end = false;
    for (std::size_t i = signature_begin; i < body_begin; i++) {
        char c = source.at(i);
        char next = i + 1 < source.size() ? source.at(i + 1) : '\0';
        if (in_line_comment) {
            if (c == '\n') in_line_comment = false;
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && next == '/') {
                in_block_comment = false;
                i++;
            }
            continue;
        }
        if (in_string) {
            if (c == '\\' && next != '\0') {
                i++;
                continue;
            }
            if (c == '"') in_string = false;
            continue;
        }
        if (in_char) {
            if (c == '\\' && next != '\0') {
                i++;
                continue;
            }
            if (c == '\'') in_char = false;
            continue;
        }
        if (c == '/' && next == '/') {
            in_line_comment = true;
            i++;
            continue;
        }
        if (c == '/' && next == '*') {
            in_block_comment = true;
            i++;
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '\'') {
            in_char = true;
            continue;
        }
        if (c == '(') {
            paren_depth++;
            continue;
        }
        if (c == ')' && paren_depth > 0) {
            paren_depth--;
            if (paren_depth == 0) saw_param_list_end = true;
            continue;
        }
        if (c == ':' && saw_param_list_end && paren_depth == 0) return i;
    }
    return std::nullopt;
}

class BodyRange {
public:
    virtual ~BodyRange() = default;
    std::size_t begin{};
    std::size_t end{};
    std::string replacement{};

    BodyRange() = default;
    BodyRange(std::size_t b, std::size_t e, std::string r)
        : begin{b}, end{e}, replacement{std::move(r)} {}
};

[[nodiscard]] std::expected<std::string, DriverError> strip_concrete_function_bodies(const Program& program, const std::string& file_path, std::string source) {
    std::vector<BodyRange> ranges{};
    for (const Function& fn : program.functions) {
        if (!fn.body || !fn.loc.has_source_path()) continue;
        if (absolute_source_path(fn.loc.source_path_text()) != file_path) continue;
        std::size_t begin = offset_for_loc(source, fn.body->loc);
        if (begin >= source.size() || source[begin] != '{') continue;
        if (!fn.member_initializers.empty()) {
            std::size_t signature_begin = offset_for_loc(source, fn.loc);
            if (auto colon = find_constructor_member_initializer_colon(source, signature_begin, begin); colon.has_value()) {
                ranges.push_back(BodyRange{*colon, begin, std::string("")});
            }
        }
        auto end_r = find_matching_brace(source, begin);
        if (!end_r.has_value()) return std::unexpected(std::move(end_r).error());
        std::size_t end = end_r.value();
        ranges.push_back(BodyRange{begin, end + 1, std::string(";")});
        // Once this definition's own body is stripped down to a bare
        // `;` above, any standalone forward declaration(s) that were
        // reconciled against it (see Function::superseded_forward_
        // declaration_locs's own comment) become an exact, redundant
        // second (or third, ...) copy of the very same now-bodyless
        // declaration -- drop each one's own source text entirely
        // rather than leave it for a later `import` to reject as a
        // repeated identical declaration.
        for (const SourceLocation& superseded_loc : fn.superseded_forward_declaration_locs) {
            if (!superseded_loc.has_source_path() ||
                absolute_source_path(superseded_loc.source_path_text()) != file_path) {
                continue;
            }
            std::size_t decl_begin = offset_for_loc(source, superseded_loc);
            if (decl_begin >= source.size()) continue;
            decl_begin = widen_declaration_begin_over_leading_modifiers(source, decl_begin);
            auto decl_end_r = find_declaration_semicolon(source, decl_begin);
            if (!decl_end_r.has_value()) return std::unexpected(std::move(decl_end_r).error());
            std::size_t decl_end = decl_end_r.value();
            ranges.push_back(BodyRange{decl_begin, decl_end + 1, std::string("")});
        }
    }
    // A local class/struct defined inside a function body (e.g.
    // layout_of_type's own LayoutComputer) registers its member functions
    // in program.functions exactly like any other function, each with its
    // own fn.body pointing *inside* the enclosing function's body -- so
    // the loop above pushes one range for the enclosing function's entire
    // body and additional, strictly nested ranges for each of the local
    // class's own members. Applying both independently corrupts the
    // output: whichever nested range is replaced first shrinks `source`,
    // which silently invalidates the enclosing range's own `end` offset
    // (computed against the original, unshrunk text), so replacing it
    // afterwards consumes extra, unrelated trailing source -- observed
    // firsthand as a truncated interface missing everything after the
    // outer function (up to and including the module's own closing
    // namespace brace). Discard any range fully contained within another
    // (keeping only maximal/outermost ranges) before sorting/applying --
    // stripping the outer function's body already removes its nested
    // classes' member bodies too, so the nested ranges are redundant, not
    // merely undesirable.
    std::vector<BodyRange> maximal_ranges{};
    for (const BodyRange& candidate : ranges) {
        bool contained = false;
        for (const BodyRange& other : ranges) {
            if (other.begin <= candidate.begin && candidate.end <= other.end &&
                (other.begin != candidate.begin || other.end != candidate.end)) {
                contained = true;
                break;
            }
        }
        if (!contained) maximal_ranges.push_back(BodyRange{candidate.begin, candidate.end, candidate.replacement});
    }
    ranges = std::move(maximal_ranges);
    for (std::size_t i = 0; i < ranges.size(); i++) {
        for (std::size_t j = i + 1; j < ranges.size(); j++) {
            bool swap_needed = false;
            if (ranges[i].begin > ranges[j].begin) {
                swap_needed = true;
            } else if (ranges[i].begin == ranges[j].begin) {
                if (ranges[i].end > ranges[j].end) {
                    swap_needed = true;
                }
            }
            if (swap_needed) {
                BodyRange tmp = std::move(ranges[i]);
                ranges[i] = std::move(ranges[j]);
                ranges[j] = std::move(tmp);
            }
        }
    }
    std::vector<BodyRange> unique_ranges{};
    for (std::size_t i = 0; i < ranges.size(); i++) {
        if (!unique_ranges.empty()) {
            std::size_t last_idx = unique_ranges.size() - 1;
            if (unique_ranges[last_idx].begin == ranges[i].begin &&
                unique_ranges[last_idx].end == ranges[i].end &&
                unique_ranges[last_idx].replacement == ranges[i].replacement) {
                continue;
            }
        }
        unique_ranges.push_back(BodyRange{ranges[i].begin, ranges[i].end, ranges[i].replacement});
    }
    ranges = std::move(unique_ranges);
    std::string new_source{};
    std::size_t last_pos = 0;
    for (std::size_t i = 0; i < ranges.size(); i++) {
        const BodyRange& range = ranges[i];
        if (range.begin > last_pos) {
            new_source += source.substr(last_pos, range.begin - last_pos);
        }
        new_source += range.replacement;
        if (range.end > last_pos) {
            last_pos = range.end;
        }
    }
    if (last_pos < source.size()) {
        new_source += source.substr(last_pos, source.size() - last_pos);
    }
    return new_source;
}

// ch11 §11.7/§11.8: resolves `import name;` declarations against a
// `--import name=path` mapping, recursively parsing (and caching) each
// imported module's source the first time it's needed -- multiple files
// importing the same module, or transitive imports (an imported module
// itself importing something else), only ever get parsed and merged
// once. Only the explicit, unambiguous `--import name=path` form is
// supported (mirrors Clang's `-fmodule-file=` and Rust's `--extern`,
// per ch11 §11.13); `-I` search now prefers prebuilt `.scppm`
// interfaces over raw `.scpp` source when both exist, so callers can
// ship/import compiled module artifacts transparently.
//
// Also resolves `import :part;`/`export import :part;` (ch11 §11.4,
// same-module partitions) against the *same* `--import` mapping, keyed
// as "<module>:<partition>" (e.g. "std:string") -- see resolve_partition
// below for why that path re-parses fresh every time instead of caching
// like resolve() does for ordinary cross-module imports.
constexpr int kMaxResolutionDepth = 256;

class ModuleCache {
public:
    virtual ~ModuleCache() = default;
    explicit ModuleCache(std::unordered_map<std::string, std::string> import_paths,
                         std::vector<std::string> import_search_dirs = {})
        : import_paths_{std::move(import_paths)},
          import_search_dirs_{build_default_import_search_dirs(import_search_dirs)} {}

    // resolve()/resolve_partition() recurse into each other (directly
    // and via the resolver lambdas parse() calls back into) one native
    // C++ stack frame per nested `import`, with no upper bound besides
    // the OS thread stack -- resolving_/partitions_resolving_ above only
    // catch a *cycle*, not a merely very long finite chain. Converting
    // this whole path from throw/catch to std::expected (batch 6/#412)
    // measurably raised the per-level stack cost (std::expected return
    // values, versus an exception's stack-less unwind), empirically
    // moving this process's own native crash point from a chain depth
    // of ~1046-1054 down to ~851-859 on this build/environment's default
    // 8 MiB thread stack -- a real, if narrow, regression matching the
    // exact class of risk that made batch 3 lower constexpression.cppm's own
    // max_recursion_depth 512->256. That lowering has since been shown to
    // have been made on a wrong premise and to a still-unreachable value:
    // the cost there is frame allocation on the way *down*, not
    // std::expected propagation on the way back up, and 256 levels of that
    // engine's walk still could not fit on an 8 MiB stack, so its budget
    // stayed unreachable until it was rederived from a measurement and
    // backed by a byte-measured guard. Real import graphs never come close
    // to this counter (the whole std library totals under 20 import
    // declarations), so this limit exists purely to turn an
    // unreachable-in-practice pathological chain into a clean, reported
    // ParseError instead of a raw SIGSEGV. 256 is retained here on its own
    // measurement rather than by borrowing that file's value: it leaves a
    // >3x margin below the ~851 empirical native crash point measured
    // above (and construction below is native C++, not self-hosted, so no
    // scpp-side stack-cost concern applies to this counter itself).

    // Returns std::expected rather than throwing on failure now that
    // parser.cppm's own ModuleResolver alias (batch 6/#412) has a
    // disengaged state of its own to report through -- see that alias's
    // comment for the loc.is_known() convention this function's
    // ParseError must follow: {0, 0} (unknown, the default -- see
    // ParseError's own constructor) for every resolver-native failure
    // below (this function's own reasons, or a wrapped DriverError from
    // read_module_file/deserialize_compile_time_payload, neither of
    // which ever carries a real position), and a real, non-zero position
    // only when forwarding imported_result's own ParseError verbatim (a
    // nested parse() call's failure, already positioned within *that*
    // file by its own frame -- see parse()'s own comment in
    // parser.cppm). A pointer rather than a reference return, matching
    // ModuleResolver's own required shape (see that alias's comment for
    // why): cache_'s std::unordered_map never invalidates a live
    // reference/pointer to an existing element on further insertion, so
    // this is exactly as safe as the reference this function returned
    // before.
    [[nodiscard]] std::expected<const Program*, ParseError> resolve(const std::string& module_name) {
        auto cached = cache_.find(module_name);
        if (cached != cache_.end()) return &cached->second;

        if (resolving_.contains(module_name)) {
            std::string msg = "circular import detected: module '";
            msg += module_name;
            msg += "' (directly or transitively) imports itself";
            return std::unexpected(ParseError{0, 0, std::move(msg)});
        }
        if (resolution_depth_ >= kMaxResolutionDepth) {
            std::string msg = "module import chain too deep (nested more than ";
            msg += std::to_string(static_cast<std::int64_t>(kMaxResolutionDepth));
            msg += " levels) while resolving '";
            msg += module_name;
            msg += "'; check for accidental complexity";
            return std::unexpected(ParseError{0, 0, std::move(msg)});
        }
        std::string target_path{};
        auto path_it = import_paths_.find(module_name);
        if (path_it == import_paths_.end()) {
            auto inferred_r = infer_module_path(module_name);
            if (!inferred_r.has_value()) return std::unexpected(std::move(inferred_r).error());
            if (inferred_r.value().has_value()) {
                target_path = *inferred_r.value();
                import_paths_.emplace(module_name, target_path);
            } else {
                std::string msg = "cannot find module '";
                msg += module_name;
                msg += "' (use --import ";
                msg += module_name;
                msg += "=path/to/file or -I <dir>)";
                return std::unexpected(ParseError{0, 0, std::move(msg)});
            }
        } else {
            target_path = path_it->second;
        }

        resolving_.insert(module_name);
        ++resolution_depth_;
        std::string resolved_path = absolute_source_path(target_path);
        auto loaded_r = read_module_file(resolved_path);
        if (!loaded_r.has_value()) return std::unexpected(ParseError{0, 0, loaded_r.error().what()});
        LoadedModuleFile loaded = std::move(loaded_r.value());
        // Stamps every SourceLocation this parse() produces (see
        // ParseError's and parse_primary()'s own comments) with
        // target_path -- the path exactly as given via `--import
        // name=path` (or as inferred by -I search) -- rather than
        // `resolved_path`, so a diagnostic rooted in this file prints
        // that same as-given spelling instead of silently rewriting it
        // to an absolute path (cli.cppm's print_diagnostic, and this
        // file's own entry-point parse() call below, agree on this
        // convention). `resolved_path` remains the absolute form used
        // for internal bookkeeping below (cache dedup, module-object
        // naming, archive lookup) where deduplicating equivalent paths
        // genuinely matters.
        auto imported_result = parse(
            std::string_view{loaded.interface_source},
            std::function<std::expected<const Program*, ParseError>(const std::string&)>(
                [this](const std::string& name) -> std::expected<const Program*, ParseError> { return resolve(name); }),
            std::function<std::expected<Program, ParseError>(const std::string&)>(
                [this](const std::string& key) -> std::expected<Program, ParseError> { return resolve_partition(key); }),
            target_path);
        if (!imported_result.has_value()) return std::unexpected(std::move(imported_result).error());
        Program imported = std::move(imported_result.value());
        imported.source_path = resolved_path;
        if (loaded.has_compile_time_payload) {
            auto payload_r = deserialize_compile_time_payload(loaded.compile_time_payload_bytes, resolved_path);
            if (!payload_r.has_value()) return std::unexpected(ParseError{0, 0, payload_r.error().what()});
            merge_compile_time_payload(imported, std::move(payload_r.value()));
        } else if (!loaded.is_scppm) {
            mark_reachable_hidden_compile_time_dependencies(imported);
        } else if (loaded.is_scppm && program_requires_structured_payload(imported)) {
            std::string msg = "module interface '";
            msg += resolved_path;
            msg += "' lacks the required structured compile-time payload; rebuild it with a newer scpp 'build-module' output";
            return std::unexpected(ParseError{0, 0, std::move(msg)});
        }
        resolving_.erase(module_name);
        --resolution_depth_;

        if (imported.module_name != module_name) {
            std::string msg = "'";
            msg += path_it->second;
            msg += "' does not declare module '";
            msg += module_name;
            msg += "' (its own module declaration names '";
            if (imported.module_name.empty()) {
                msg += "<none>";
            } else {
                msg += imported.module_name;
            }
            msg += "')";
            return std::unexpected(ParseError{0, 0, std::move(msg)});
        }

        resolution_order_.push_back(module_name);
        resolved_paths_[module_name] = resolved_path;
        auto emplace_res = cache_.emplace(module_name, std::move(imported));
        return &emplace_res.first->second;
    }

    // ch11 §11.4: resolves a same-module partition key
    // ("<module>:<partition>", e.g. "std:string") against the same
    // `--import name=path` mapping resolve() uses. Returns a *freshly
    // parsed* Program by value every call -- never cached -- since
    // scpp.parser's merge_partition genuinely moves each declaration
    // (bodies included) out of the returned Program; a cached, shared
    // instance would end up silently empty for a second importer of the
    // same partition (see PartitionResolver's own comment in
    // parser.cppm for why this v1 limitation -- no shared identity
    // across two importers of the same partition -- is acceptable).
    // Returns std::expected rather than throwing, for exactly the same
    // reason and following exactly the same loc.is_known() convention as
    // resolve() above.
    [[nodiscard]] std::expected<Program, ParseError> resolve_partition(const std::string& key) {
        if (partitions_resolving_.contains(key)) {
            std::string msg = "circular partition import detected: '";
            msg += key;
            msg += "' (directly or transitively) imports itself";
            return std::unexpected(ParseError{0, 0, std::move(msg)});
        }
        if (resolution_depth_ >= kMaxResolutionDepth) {
            std::string msg = "module import chain too deep (nested more than ";
            msg += std::to_string(static_cast<std::int64_t>(kMaxResolutionDepth));
            msg += " levels) while resolving partition '";
            msg += key;
            msg += "'; check for accidental complexity";
            return std::unexpected(ParseError{0, 0, std::move(msg)});
        }
        std::string target_path{};
        auto path_it = import_paths_.find(key);
        if (path_it == import_paths_.end()) {
            auto inferred_r = infer_partition_path(key);
            if (!inferred_r.has_value()) return std::unexpected(std::move(inferred_r).error());
            if (inferred_r.value().has_value()) {
                target_path = *inferred_r.value();
                import_paths_.emplace(key, target_path);
            } else {
                std::string msg = "cannot find partition '";
                msg += key;
                msg += "' (use --import ";
                msg += key;
                msg += "=path/to/file or import its parent module via -I <dir>)";
                return std::unexpected(ParseError{0, 0, std::move(msg)});
            }
        } else {
            target_path = path_it->second;
        }

        partitions_resolving_.insert(key);
        ++resolution_depth_;
        auto loaded_r = read_module_file(target_path);
        if (!loaded_r.has_value()) return std::unexpected(ParseError{0, 0, loaded_r.error().what()});
        LoadedModuleFile loaded = std::move(loaded_r.value());
        if (loaded.is_scppm) {
            std::string msg = "partition import path '";
            msg += target_path;
            msg += "' must use a source .scpp file, not a compiled .scppm artifact";
            return std::unexpected(ParseError{0, 0, std::move(msg)});
        }
        // target_path (not an absolute_source_path()-normalized
        // form) so this partition's own SourceLocations preserve
        // whatever spelling resolved it -- see resolve()'s matching
        // comment above; partition.source_path just below is still the
        // absolute form internal bookkeeping (e.g. resolved_paths_) can
        // rely on.
        auto partition_result = parse(
            std::string_view{loaded.interface_source},
            std::function<std::expected<const Program*, ParseError>(const std::string&)>(
                [this](const std::string& name) -> std::expected<const Program*, ParseError> { return resolve(name); }),
            std::function<std::expected<Program, ParseError>(const std::string&)>(
                [this](const std::string& nested_key) -> std::expected<Program, ParseError> { return resolve_partition(nested_key); }),
            target_path);
        if (!partition_result.has_value()) return std::unexpected(std::move(partition_result).error());
        Program partition = std::move(partition_result.value());
        partition.source_path = absolute_source_path(target_path);
        partitions_resolving_.erase(key);
        --resolution_depth_;

        std::string expected_key = partition.module_name + ":" + partition.partition_name;
        if (expected_key != key) {
            std::string msg = "'";
            msg += target_path;
            msg += "' does not declare partition '";
            msg += key;
            msg += "' (its own module declaration names '";
            msg += expected_key;
            msg += "')";
            return std::unexpected(ParseError{0, 0, std::move(msg)});
        }
        return partition;
    }

    // Returns std::expected<std::optional<...>, ParseError> -- an extra
    // std::expected layer around the pre-existing std::optional -- since
    // infer_partition_path (called below) can now itself fail (e.g. a
    // "multiple source files declare" conflict) instead of throwing; the
    // inner std::optional keeps meaning exactly what it always did
    // ("found a path" vs. "no known path, but not an error either" --
    // see infer_partition_path's own comment). inline_partition_imports
    // (this method's one external caller) checks both layers in turn.
    [[nodiscard]] std::expected<std::optional<std::string>, ParseError> source_path_for_partition(const std::string& key) {
        auto path_it = import_paths_.find(key);
        if (path_it != import_paths_.end()) return path_it->second;
        return infer_partition_path(key);
    }

    // Every module actually resolved so far, in first-resolved order (a
    // transitively-imported module is resolved -- and so appears here --
    // strictly before whatever imported it, since resolve() recurses
    // into a module's own imports before that module's entry is
    // recorded). Used by compile_to_executable to know which modules
    // need their own separately-compiled object file. Partitions are
    // deliberately never recorded here at all (see resolve_partition) --
    // a partition folds into whichever module imports it and never gets
    // an object file of its own.
    [[nodiscard]] const std::vector<std::string>& resolution_order() const { return resolution_order_; }
    // Non-const: emit_object_file_for_program (ch05 §5.11) needs to
    // mutate this module's own Program in place (monomorphize_generics
    // injects concrete clones before check_moves runs) -- safe since
    // each cached module's Program is only ever handed to that one
    // separate-compilation call, never read again afterward.
    [[nodiscard]] Program& program_for(const std::string& module_name) { return cache_.at(module_name); }
    [[nodiscard]] static std::string unescape_json_string(std::string_view text) {
        std::string out{};
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); i++) {
            char ch = text.at(i);
            if (ch == '\\' && i + 1 < text.size()) {
                i++;
                char next = text.at(i);
                switch (next) {
                    case '\\': out.push_back('\\'); break;
                    case '"': out.push_back('"'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    default: out.push_back(next); break;
                }
            } else {
                out.push_back(ch);
            }
        }
        return out;
    }
    [[nodiscard]] static std::optional<std::string> archive_from_metadata(const std::string& metadata_path,
                                                                           const std::string& module_name) {
        if (!path_exists(metadata_path)) return std::nullopt;
        auto bytes_r = read_file_bytes(metadata_path);
        if (!bytes_r.has_value()) return std::nullopt;
        const std::string& content = bytes_r.value();
        std::string line{};
        const std::string name_needle = "\"name\": \"" + module_name + "\"";
        const std::string archive_needle = "\"archive\": \"";
        std::size_t line_start = 0;
        while (line_start < content.size()) {
            std::size_t line_end = content.find('\n', line_start);
            if (line_end == std::string::npos) line_end = content.size();
            line = content.substr(line_start, line_end - line_start);
            line_start = line_end + 1;
            if (line.find(name_needle.c_str()) == std::string::npos) continue;
            std::size_t archive_pos = line.find(archive_needle.c_str());
            if (archive_pos == std::string::npos) continue;
            archive_pos += archive_needle.size();
            std::size_t archive_end = line.find('"', archive_pos);
            if (archive_end == std::string::npos) continue;
            return unescape_json_string(std::string_view(line).substr(archive_pos, archive_end - archive_pos));
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::string> archive_for(const std::string& module_name) const {
        auto path_it = resolved_paths_.find(module_name);
        if (path_it == resolved_paths_.end()) return std::nullopt;
        const std::string& interface_path = path_it->second;
        if (!path_ends_with(interface_path, ".scppm")) return std::nullopt;
        std::string parent = path_parent(interface_path);
        std::vector<std::string> candidates{};
        candidates.push_back(path_join(parent, "lib" + module_name + ".scppa"));
        if (path_filename(parent) == "modules") {
            std::string grand_parent = path_parent(parent);
            candidates.push_back(path_join(path_join(grand_parent, "archives"), "lib" + module_name + ".scppa"));
            std::optional<std::string> metadata_archive =
                archive_from_metadata(path_join(grand_parent, "package-metadata.json"), module_name);
            if (metadata_archive.has_value()) {
                candidates.push_back(*metadata_archive);
            }
        }
        for (const std::string& archive_path : candidates) {
            if (path_exists(archive_path)) return archive_path;
        }
        return std::nullopt;
    }

private:
    void register_discovered_source(const std::string& key, const std::string& path) {
        auto it = discovered_source_paths_.find(key);
        if (it == discovered_source_paths_.end()) {
            discovered_source_paths_.emplace(key, path);
            return;
        }
        if (absolute_source_path(it->second) == absolute_source_path(path)) return;
        std::vector<std::string>& conflicts = discovered_source_path_conflicts_[key];
        if (conflicts.empty()) conflicts.push_back(it->second);
        bool found = false;
        for (const std::string& existing : conflicts) {
            if (existing == path) { found = true; break; }
        }
        if (!found) conflicts.push_back(path);
    }

    // Returns std::expected rather than throwing "multiple source files
    // declare" now that every caller (scan_source_root's own callers,
    // transitively) is std::expected-based -- see resolve()'s own
    // comment for the loc.is_known() convention this ParseError follows
    // (unknown: this is a driver-native conflict with no source position
    // of its own).
    [[nodiscard]] std::expected<std::optional<std::string>, ParseError> lookup_discovered_source_path(const std::string& key) const {
        auto conflict_it = discovered_source_path_conflicts_.find(key);
        if (conflict_it != discovered_source_path_conflicts_.end() && !conflict_it->second.empty()) {
            std::string msg = "multiple source files declare '" + key + "': ";
            for (std::size_t i = 0; i < conflict_it->second.size(); ++i) {
                if (i != 0) msg += ", ";
                msg += conflict_it->second[i];
            }
            return std::unexpected(ParseError{0, 0, std::move(msg)});
        }
        auto it = discovered_source_paths_.find(key);
        if (it == discovered_source_paths_.end()) return std::nullopt;
        return it->second;
    }

    [[nodiscard]] std::expected<void, ParseError> scan_source_root_dir(const std::string& dir_path) {
        void* dir = nullptr;
        [[scpp::unsafe]] {
            dir = opendir(dir_path.c_str());
        }
        if (dir == nullptr) return {};
        while (true) {
            dirent* entry = nullptr;
            [[scpp::unsafe]] {
                entry = readdir(dir);
            }
            if (entry == nullptr) break;
            std::string name{};
            [[scpp::unsafe]] {
                name = std::string{entry->d_name};
            }
            if (name == "." || name == "..") continue;
            std::string full_path = path_join(dir_path, name);
            posix_stat st{};
            int rc = -1;
            [[scpp::unsafe]] {
                rc = stat(full_path.c_str(), &st);
            }
            if (rc != 0) continue;
            bool is_dir = (st.st_mode & 0170000) == 0040000;
            bool is_reg = (st.st_mode & 0170000) == 0100000;
            if (is_dir) {
                if (name == ".scpp") continue;
                if (auto r = scan_source_root_dir(full_path); !r.has_value()) {
                    [[scpp::unsafe]] {
                        closedir(dir);
                    }
                    return r;
                }
            } else if (is_reg) {
                if (path_ends_with(name, ".scpp") || path_ends_with(name, ".cppm") ||
                    path_ends_with(name, ".cpp") || path_ends_with(name, ".cc") ||
                    path_ends_with(name, ".cxx")) {
                    auto loaded_r = read_module_file(full_path);
                    if (!loaded_r.has_value()) {
                        [[scpp::unsafe]] {
                            closedir(dir);
                        }
                        return std::unexpected(ParseError{0, 0, loaded_r.error().what()});
                    }
                    LoadedModuleFile loaded = std::move(loaded_r.value());
                    if (std::optional<ScannedModuleDecl> decl = scan_declared_module_from_source(loaded.interface_source);
                        decl.has_value()) {
                        std::string key = decl->module_name;
                        if (!decl->partition_name.empty()) key += ":" + decl->partition_name;
                        register_discovered_source(key, path_lexically_normal(full_path));
                    }
                }
            }
        }
        [[scpp::unsafe]] {
            closedir(dir);
        }
        return {};
    }

    [[nodiscard]] std::expected<void, ParseError> scan_source_root(const std::string& root) {
        std::string normalized_root = path_lexically_normal(root);
        if (normalized_root.empty()) normalized_root = std::string{"."};
        if (scanned_source_roots_.contains(normalized_root)) return {};
        scanned_source_roots_.insert(normalized_root);
        if (!path_exists(normalized_root)) return {};
        return scan_source_root_dir(normalized_root);
    }

    [[nodiscard]] std::expected<void, ParseError> ensure_module_source_root_scanned(const std::string& module_name) {
        auto module_it = import_paths_.find(module_name);
        if (module_it == import_paths_.end()) return {};
        const std::string& module_path = module_it->second;
        if (!path_ends_with(module_path, ".scpp") && !path_ends_with(module_path, ".cppm") &&
            !path_ends_with(module_path, ".cpp") && !path_ends_with(module_path, ".cc") &&
            !path_ends_with(module_path, ".cxx")) return {};
        return scan_source_root(path_parent(module_path));
    }

    [[nodiscard]] std::expected<void, ParseError> ensure_search_dirs_scanned() {
        if (search_dirs_scanned_) return {};
        search_dirs_scanned_ = true;
        for (const std::string& dir : import_search_dirs_) {
            if (auto r = scan_source_root(dir); !r.has_value()) return std::unexpected(std::move(r).error());
        }
        return {};
    }

    [[nodiscard]] std::expected<std::optional<std::string>, ParseError> infer_module_path(const std::string& module_name) {
        for (const std::string& dir : import_search_dirs_) {
            std::string interface_candidate = path_join(dir, module_name + ".scppm");
            if (path_exists(interface_candidate)) return interface_candidate;
            std::string source_candidate = path_join(dir, module_name + ".scpp");
            if (path_exists(source_candidate)) return source_candidate;
            std::string cppm_candidate = path_join(dir, module_name + ".cppm");
            if (path_exists(cppm_candidate)) return cppm_candidate;
            std::string cpp_candidate = path_join(dir, module_name + ".cpp");
            if (path_exists(cpp_candidate)) return cpp_candidate;
            std::string cc_candidate = path_join(dir, module_name + ".cc");
            if (path_exists(cc_candidate)) return cc_candidate;
            std::string cxx_candidate = path_join(dir, module_name + ".cxx");
            if (path_exists(cxx_candidate)) return cxx_candidate;
        }
        if (auto scan_r = ensure_search_dirs_scanned(); !scan_r.has_value()) return std::unexpected(std::move(scan_r).error());
        return lookup_discovered_source_path(module_name);
    }

    [[nodiscard]] std::expected<std::optional<std::string>, ParseError> infer_partition_path(const std::string& key) {
        std::size_t colon = key.find(':');
        if (colon == std::string::npos) return std::nullopt;
        std::string module_name = key.substr(0, colon);
        if (auto scan_r = ensure_module_source_root_scanned(module_name); !scan_r.has_value()) {
            return std::unexpected(std::move(scan_r).error());
        }
        auto discovered_r = lookup_discovered_source_path(key);
        if (!discovered_r.has_value()) return std::unexpected(std::move(discovered_r).error());
        if (discovered_r.value().has_value()) return discovered_r;
        if (auto scan_r = ensure_search_dirs_scanned(); !scan_r.has_value()) {
            return std::unexpected(std::move(scan_r).error());
        }
        return lookup_discovered_source_path(key);
    }

    std::unordered_map<std::string, std::string> import_paths_{};
    std::vector<std::string> import_search_dirs_{};
    std::unordered_map<std::string, std::string> discovered_source_paths_{};
    std::unordered_map<std::string, std::vector<std::string>> discovered_source_path_conflicts_{};
    std::unordered_set<std::string> scanned_source_roots_{};
    bool search_dirs_scanned_ = false;
    std::unordered_map<std::string, Program> cache_{};
    std::unordered_map<std::string, std::string> resolved_paths_{};
    std::unordered_set<std::string> resolving_{};
    std::unordered_set<std::string> partitions_resolving_{};
    int resolution_depth_ = 0;
    std::vector<std::string> resolution_order_{};
};

[[nodiscard]] std::string trim_copy(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && is_ascii_space(text.at(begin))) begin++;
    std::size_t end = text.size();
    while (end > begin && is_ascii_space(text.at(end - 1))) end--;
    return string_from_view(text.substr(begin, end - begin));
}

[[nodiscard]] std::size_t string_view_find(std::string_view sv, char c, std::size_t start = 0) {
    for (std::size_t i = start; i < sv.size(); ++i) {
        if (sv.at(i) == c) return i;
    }
    return std::string_view::npos;
}

[[nodiscard]] bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] bool is_partition_import_line(std::string_view trimmed) {
    return starts_with(trimmed, "export import :") || starts_with(trimmed, "import :");
}

[[nodiscard]] bool is_non_partition_import_line(std::string_view trimmed) {
    if (is_partition_import_line(trimmed)) return false;
    return starts_with(trimmed, "export import ") || starts_with(trimmed, "import ");
}

[[nodiscard]] std::expected<std::string, DriverError> render_module_interface_file(const Program& program, ModuleCache& cache, const std::string& file_path,
                                         const std::string& module_source_path, bool keep_concrete_bodies,
                                         bool keep_module_declaration,
                                         std::unordered_set<std::string>& expanded_partition_paths);

[[nodiscard]] std::expected<std::string, DriverError> inline_partition_imports(const Program& program, ModuleCache& cache, const std::string& module_source_path,
                                     std::string_view source,
                                     bool keep_concrete_bodies, std::unordered_set<std::string>& expanded_partition_paths) {
    std::string out{};
    std::size_t line_start = 0;
    while (line_start <= source.size()) {
        std::size_t line_end = string_view_find(source, '\n', line_start);
        bool had_newline = line_end != std::string_view::npos;
        std::string_view line =
            had_newline ? source.substr(line_start, line_end - line_start) : source.substr(line_start);
        std::string trimmed = trim_copy(line);
        if (is_partition_import_line(trimmed)) {
            std::size_t colon = trimmed.find(':');
            std::size_t semi = trimmed.find(';', colon);
            std::string partition_name = trimmed.substr(colon + 1, semi == std::string::npos ? std::string::npos
                                                                                            : semi - (colon + 1));
            std::string key = program.module_name + ":" + partition_name;
            auto partition_path_r = cache.source_path_for_partition(key);
            if (!partition_path_r.has_value()) {
                const ParseError& error = partition_path_r.error();
                return std::unexpected(DriverError{error.what(), error.loc});
            }
            if (!partition_path_r.value().has_value()) {
                return std::unexpected(DriverError{std::string("cannot find partition '") + program.module_name + ":" + partition_name +
                                  "' while writing module interface artifacts"});
            }
            std::string absolute_partition_path = absolute_source_path(*partition_path_r.value());
            if (expanded_partition_paths.insert(absolute_partition_path).second) {
                auto rendered_r = render_module_interface_file(program, cache, absolute_partition_path, module_source_path, keep_concrete_bodies,
                                                    /*keep_module_declaration=*/false, expanded_partition_paths);
                if (!rendered_r.has_value()) return std::unexpected(std::move(rendered_r).error());
                out += rendered_r.value();
            }
        } else {
            out += string_from_view(line);
            if (had_newline) out += '\n';
        }
        if (!had_newline) break;
        line_start = line_end + 1;
    }
    return out;
}

[[nodiscard]] std::expected<std::string, DriverError> render_module_interface_file(const Program& program, ModuleCache& cache, const std::string& file_path,
                                         const std::string& module_source_path, bool keep_concrete_bodies,
                                         bool keep_module_declaration,
                                         std::unordered_set<std::string>& expanded_partition_paths) {
    auto loaded_r = read_module_file(file_path);
    if (!loaded_r.has_value()) return std::unexpected(std::move(loaded_r).error());
    LoadedModuleFile loaded = std::move(loaded_r).value();
    std::string source = loaded.interface_source;
    if (!keep_concrete_bodies) {
        auto stripped_r = strip_concrete_function_bodies(program, absolute_source_path(file_path), std::move(source));
        if (!stripped_r.has_value()) return std::unexpected(std::move(stripped_r).error());
        source = std::move(stripped_r).value();
    }

    std::string out{};
    std::size_t line_start = 0;
    while (line_start <= source.size()) {
        std::size_t line_end = source.find('\n', line_start);
        bool had_newline = line_end != std::string::npos;
        std::string_view line =
            had_newline ? std::string_view(source).substr(line_start, line_end - line_start)
                        : std::string_view(source).substr(line_start);
        std::string trimmed = trim_copy(line);
        bool is_module_decl = starts_with(trimmed, "export module ") || starts_with(trimmed, "module ") || trimmed == "module;";
        if (is_module_decl) {
            if (keep_module_declaration) {
                out += string_from_view(line);
                if (had_newline) out += '\n';
            }
        } else {
            auto inlined_r = inline_partition_imports(program, cache, module_source_path, line, keep_concrete_bodies,
                                            expanded_partition_paths);
            if (!inlined_r.has_value()) return std::unexpected(std::move(inlined_r).error());
            out += inlined_r.value();
            if (had_newline) out += '\n';
        }
        if (!had_newline) break;
        line_start = line_end + 1;
    }
    return out;
}

std::string hoist_non_partition_imports(std::string source) {
    std::vector<std::string> imports{};
    std::unordered_set<std::string> seen_imports{};
    std::vector<std::string> body_lines{};
    // ch11 §11.2: a global module fragment -- a leading bare `module;`
    // line, plus anything between it and the module declaration itself
    // (e.g. an ordinary comment, or, for a not-yet-self-hosted file, real
    // preprocessor directives) -- must stay ahead of `export module
    // name;`, never reordered alongside imports/body below it. Collected
    // separately from body_lines (rather than relying on is_module_decl,
    // which never matches a bare `module;`: it only recognizes
    // "module "/"export module " with a trailing space before a real
    // name) so it can be re-emitted first, before module_line itself.
    std::vector<std::string> prologue_lines{};
    std::string module_line{};
    bool module_line_set = false;

    std::size_t line_start = 0;
    while (line_start <= source.size()) {
        std::size_t line_end = source.find('\n', line_start);
        bool had_newline = line_end != std::string::npos;
        std::string_view line =
            had_newline ? std::string_view(source).substr(line_start, line_end - line_start)
                        : std::string_view(source).substr(line_start);
        std::string line_text = string_from_view(line);
        std::string trimmed = trim_copy(line);
        bool is_module_decl = starts_with(trimmed, "export module ") || starts_with(trimmed, "module ");
        if (is_module_decl && !module_line_set) {
            module_line = line_text;
            module_line_set = true;
        } else if (!module_line_set) {
            prologue_lines.push_back(line_text);
        } else if (is_non_partition_import_line(trimmed)) {
            if (seen_imports.insert(trimmed).second) imports.push_back(line_text);
        } else {
            body_lines.push_back(line_text);
        }
        if (!had_newline) break;
        line_start = line_end + 1;
    }

    std::string out{};
    for (const std::string& prologue_line : prologue_lines) {
        out += prologue_line;
        out += '\n';
    }
    if (module_line_set) {
        out += module_line;
        out += '\n';
    }
    if (!imports.empty()) {
        out += '\n';
        for (const std::string& import_line : imports) {
            out += import_line;
            out += '\n';
        }
    }
    for (std::size_t i = 0; i < body_lines.size(); i++) {
        if ((module_line_set || !imports.empty()) || i > 0) out += '\n';
        out += body_lines[i];
    }
    return out;
}

[[nodiscard]] std::expected<std::string, DriverError> build_merged_interface_source(const Program& program, ModuleCache& cache, const std::string& module_source_path,
                                          bool keep_concrete_bodies) {
    std::unordered_set<std::string> expanded_partition_paths{};
    auto rendered_r = render_module_interface_file(program, cache, module_source_path, module_source_path,
                                                                    keep_concrete_bodies,
                                                                    /*keep_module_declaration=*/true,
                                                                    expanded_partition_paths);
    if (!rendered_r.has_value()) return std::unexpected(std::move(rendered_r).error());
    return hoist_non_partition_imports(std::move(rendered_r).value());
}

llvm::LLVMCodeGenOptLevel codegen_opt_level_for(int opt_level) {
    if (opt_level <= 0) return llvm::LLVMCodeGenLevelNone;
    if (opt_level == 1) return llvm::LLVMCodeGenLevelLess;
    if (opt_level == 2) return llvm::LLVMCodeGenLevelDefault;
    return llvm::LLVMCodeGenLevelAggressive;
}

// The parser bounds its own recursion (see nesting_depth_ in
// scpp.parser), but that counts *parse* depth, and two shapes build a
// deep tree without a deep parse: left-associative operator chains are
// assembled by loops rather than by recursion, and a Program can also
// arrive here by being read back out of a .scppm module file by
// read_expr/read_stmt below rather than by being parsed at all. Since
// every pass after parsing -- codegen, movecheck's dataflow and interface
// walks, monomorphize, the constant evaluator, mir's lowering, the
// serializer in this file and cli's AST printer, thirteen separate
// recursive descents in total -- walks the finished tree on the host
// stack, the depth of the *tree* is checked here once, rather than asking
// each of the thirteen to guard itself.
//
// This check cannot itself overflow: it never recurses deeper than the
// budget it is given, because it stops as soon as that budget runs out.

[[nodiscard]] std::optional<SourceLocation> stmt_nesting_overflow(const Stmt& stmt, int budget);

// The child links enumerated here and in stmt_nesting_overflow are the
// same set scpp.ast's rewrite_expr_locs walks; that function is the
// closest thing this AST has to a canonical child enumeration, so the two
// are meant to be read (and changed) together.
[[nodiscard]] std::optional<SourceLocation> expr_nesting_overflow(const Expr& expr, int budget) {
    if (budget <= 0) return expr.loc;
    const int child_budget = budget - 1;
    if (expr.lhs != nullptr) {
        if (auto over = expr_nesting_overflow(*expr.lhs, child_budget); over.has_value()) return over;
    }
    if (expr.rhs != nullptr) {
        if (auto over = expr_nesting_overflow(*expr.rhs, child_budget); over.has_value()) return over;
    }
    if (expr.third != nullptr) {
        if (auto over = expr_nesting_overflow(*expr.third, child_budget); over.has_value()) return over;
    }
    for (const ExprPtr& arg : expr.args) {
        if (arg == nullptr) continue;
        if (auto over = expr_nesting_overflow(*arg, child_budget); over.has_value()) return over;
    }
    for (const ExplicitTemplateArg& template_arg : expr.explicit_template_args) {
        if (template_arg.is_type || template_arg.value == nullptr) continue;
        if (auto over = expr_nesting_overflow(*template_arg.value, child_budget); over.has_value()) return over;
    }
    for (const LambdaCapture& capture : expr.lambda_captures) {
        if (capture.init == nullptr) continue;
        if (auto over = expr_nesting_overflow(*capture.init, child_budget); over.has_value()) return over;
    }
    for (const Param& param : expr.lambda_params) {
        if (param.default_expr == nullptr) continue;
        if (auto over = expr_nesting_overflow(*param.default_expr, child_budget); over.has_value()) return over;
    }
    if (expr.lambda_body != nullptr) {
        if (auto over = stmt_nesting_overflow(*expr.lambda_body, child_budget); over.has_value()) return over;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<SourceLocation> stmt_nesting_overflow(const Stmt& stmt, int budget) {
    if (budget <= 0) return stmt.loc;
    const int child_budget = budget - 1;
    if (stmt.init != nullptr) {
        if (auto over = expr_nesting_overflow(*stmt.init, child_budget); over.has_value()) return over;
    }
    for (const ExprPtr& ctor_arg : stmt.ctor_args) {
        if (ctor_arg == nullptr) continue;
        if (auto over = expr_nesting_overflow(*ctor_arg, child_budget); over.has_value()) return over;
    }
    if (stmt.expr != nullptr) {
        if (auto over = expr_nesting_overflow(*stmt.expr, child_budget); over.has_value()) return over;
    }
    if (stmt.condition != nullptr) {
        if (auto over = expr_nesting_overflow(*stmt.condition, child_budget); over.has_value()) return over;
    }
    if (stmt.then_branch != nullptr) {
        if (auto over = stmt_nesting_overflow(*stmt.then_branch, child_budget); over.has_value()) return over;
    }
    if (stmt.else_branch != nullptr) {
        if (auto over = stmt_nesting_overflow(*stmt.else_branch, child_budget); over.has_value()) return over;
    }
    for (const SwitchCase& switch_case : stmt.switch_cases) {
        if (switch_case.value != nullptr) {
            if (auto over = expr_nesting_overflow(*switch_case.value, child_budget); over.has_value()) return over;
        }
        for (const StmtPtr& case_stmt : switch_case.statements) {
            if (case_stmt == nullptr) continue;
            if (auto over = stmt_nesting_overflow(*case_stmt, child_budget); over.has_value()) return over;
        }
    }
    for (const StmtPtr& nested : stmt.statements) {
        if (nested == nullptr) continue;
        if (auto over = stmt_nesting_overflow(*nested, child_budget); over.has_value()) return over;
    }
    return std::nullopt;
}

[[nodiscard]] std::expected<void, DriverError> reject_over_nested_ast(const Program& program) {
    std::optional<SourceLocation> over{};
    for (const Function& fn : program.functions) {
        for (const Param& param : fn.params) {
            if (param.default_expr == nullptr) continue;
            over = expr_nesting_overflow(*param.default_expr, kMaxNestingDepth);
            if (over.has_value()) break;
        }
        if (!over.has_value()) {
            for (const MemberInitializer& member_init : fn.member_initializers) {
                if (member_init.initializer.expr != nullptr) {
                    over = expr_nesting_overflow(*member_init.initializer.expr, kMaxNestingDepth);
                    if (over.has_value()) break;
                }
                for (const ExprPtr& brace_arg : member_init.initializer.brace_args) {
                    if (brace_arg == nullptr) continue;
                    over = expr_nesting_overflow(*brace_arg, kMaxNestingDepth);
                    if (over.has_value()) break;
                }
                if (over.has_value()) break;
            }
        }
        if (!over.has_value() && fn.body != nullptr) over = stmt_nesting_overflow(*fn.body, kMaxNestingDepth);
        if (over.has_value()) break;
    }
    if (!over.has_value()) {
        for (const GlobalVar& global : program.globals) {
            if (global.decl == nullptr) continue;
            over = stmt_nesting_overflow(*global.decl, kMaxNestingDepth);
            if (over.has_value()) break;
        }
    }
    if (!over.has_value()) return {};
    std::string message{};
    message += "nesting is too deep: this construct is more than ";
    message += std::to_string(static_cast<std::int64_t>(kMaxNestingDepth));
    message += " levels deep, which exceeds the maximum nesting depth the compiler supports";
    return std::unexpected(DriverError{message, *over});
}

// Move-checks an already-parsed (and, if it has imports of its own,
// already import-merged -- see scpp.parser's merge_imported_module)
// Program and lowers it to a native object file at `object_path`. Shared
// by emit_object_file (the main source) and, once per resolved module,
// by compile_to_executable below -- exactly the same backend either way,
// since by this point a Program is just a Program regardless of which
// file it came from.
[[nodiscard]] std::expected<void, DriverError> emit_object_file_for_program(Program& program, const std::string& object_path, bool emit_debug_info = false,
                                  int opt_level = 2) {
    reject_not_yet_lowerable_constexpr_surface(program);
    // ch05 §5.11: must run before check_moves -- see Monomorphizer's own
    // comment in movecheck.cppm for why call-site monomorphization has
    // to happen first (movecheck's ordinary exact-type-match call-
    // argument checking can only work once every call site targets an
    // already-concrete function).
    // monomorphize_generics/check_moves (movecheck) already return
    // std::expected<void, DataflowError> as of the batch-1 conversion, and
    // codegen.generate() below returns std::expected<LLVMModuleRef,
    // CodegenError> (batch 2, #408). Callers throughout this codebase --
    // cli.cppm, project.cppm, and tests/driver_test.cpp -- used to
    // deliberately `catch (const scpp::DataflowError&)`/`catch (const
    // scpp::CodegenError&)` this function's failures as their own
    // distinct C++ type (movecheck_test.cpp and codegen_test.cpp still do,
    // since they call monomorphize_generics/check_moves/codegen.generate()
    // directly rather than through this function). As of batch 5 (#411),
    // every external caller reached through *this* function consumes the
    // std::expected<void, DriverError> result directly instead, so a
    // disengaged result is wrapped into DriverError with
    // DriverErrorKind::Dataflow/::Codegen (rather than re-thrown) to
    // preserve exactly the same kind distinction without an exception.
    // (ConstexprError is different: both of its sources reachable from
    // here -- fold_immediate_calls just below, and
    // evaluate_immediate_expr's own call chain through codegen.generate()
    // -- are fully absorbed into DriverError already, since nothing
    // external needs to distinguish ConstexprError from DriverError by
    // type. As of batch 6 (#412), evaluate_immediate_expr's own call
    // sites in codegen/expressions.cppm convert its ConstexprError into a
    // CodegenError directly instead of throwing, so it now reaches here
    // -- like every other codegen failure -- through the ordinary
    // module_result.has_value() check below rather than a dedicated
    // catch; this function's own former `try`/`catch (const
    // ConstexprError&)` is gone, since nothing can throw through it
    // anymore.)
    // The nesting gate runs before any other pass touches the tree: every
    // pass below (and monomorphize itself) descends it recursively on the
    // host stack, so this has to reject an over-deep tree while the stack
    // is still empty.
    if (auto nesting_result = reject_over_nested_ast(program); !nesting_result.has_value()) {
        return std::unexpected(nesting_result.error());
    }
    auto monomorphize_result = monomorphize_generics(program);
    if (!monomorphize_result.has_value()) {
        const DataflowError& error = monomorphize_result.error();
        return std::unexpected(DriverError(error.what(), error.loc, DriverErrorKind::Dataflow));
    }
    ConstexprLimits limits{};
    auto fold_result = fold_immediate_calls(program, limits);
    if (!fold_result.has_value()) {
        return std::unexpected(DriverError{fold_result.error().what()});
    }
    auto check_moves_result = check_moves(program);
    if (!check_moves_result.has_value()) {
        const DataflowError& error = check_moves_result.error();
        return std::unexpected(DriverError{error.what(), error.loc, DriverErrorKind::Dataflow});
    }

    // Real llvm-c/Target.h's own llvm::LLVMInitializeNativeTarget/
    // llvm::LLVMInitializeNativeAsmPrinter are header-only `static inline`
    // functions with no real, exported ABI symbol to declare/link
    // against -- the `llvm` module's own `:target` partition's
    // scpp_llvm_target_initialize_* bridge calls through to them via
    // a small shim compiled with the real header available; see
    // libs/llvm/target.cpp's and libs/llvm/native_target_init.cpp's
    // own header comments.
    llvm::LLVMTargetMachineRef target_machine = nullptr;
    llvm::LLVMTargetDataRef target_data = nullptr;
    std::string triple{};
    std::string data_layout{};

    [[scpp::unsafe]] {
        llvm::scpp_llvm_target_initialize_native_target();
        llvm::scpp_llvm_target_initialize_native_asm_printer();

        char* default_triple_c = llvm::LLVMGetDefaultTargetTriple();
        triple = std::string{default_triple_c};
        llvm::LLVMDisposeMessage(default_triple_c);

        llvm::LLVMTargetRef target = nullptr;
        char* lookup_error_c = nullptr;
        if (llvm::LLVMGetTargetFromTriple(triple.c_str(), &target, &lookup_error_c) != 0) {
            std::string lookup_error{};
            if (lookup_error_c != nullptr) {
                lookup_error = std::string{lookup_error_c};
            }
            llvm::LLVMDisposeMessage(lookup_error_c);
            return std::unexpected(DriverError{std::string("failed to lookup target '") + triple + "': " + lookup_error});
        }

        target_machine = llvm::LLVMCreateTargetMachine(
            target, triple.c_str(), "generic", "", codegen_opt_level_for(opt_level),
            llvm::LLVMRelocPIC, llvm::LLVMCodeModelDefault);
        if (target_machine == nullptr) {
            return std::unexpected(DriverError{std::string("failed to create target machine for '") + triple + "'"});
        }

        target_data = llvm::LLVMCreateTargetDataLayout(target_machine);
        char* data_layout_c = llvm::LLVMCopyStringRepOfTargetData(target_data);
        data_layout = std::string{data_layout_c};
        llvm::LLVMDisposeMessage(data_layout_c);
        llvm::LLVMDisposeTargetData(target_data);
    }

    // The data layout must be set *before* codegen runs: std::make_unique
    // needs a target-accurate sizeof(T) to call malloc with, which comes
    // from the module's DataLayout.
    Codegen codegen{"scpp_module", program.source_path, emit_debug_info};
    codegen.set_target(triple, data_layout);

    llvm::LLVMModuleRef llvm_mod = nullptr;
    {
        auto module_result = codegen.generate(program);
        if (!module_result.has_value()) {
            [[scpp::unsafe]] {
                llvm::LLVMDisposeTargetMachine(target_machine);
            }
            const CodegenError& error = module_result.error();
            return std::unexpected(DriverError{error.what(), error.loc, DriverErrorKind::Codegen});
        }
        llvm_mod = std::move(module_result.value());
    }

    [[scpp::unsafe]] {
        char* emit_error_c = nullptr;
        if (llvm::LLVMTargetMachineEmitToFile(target_machine, llvm_mod, object_path.c_str(), llvm::LLVMObjectFile,
                                         &emit_error_c) != 0) {
            llvm::LLVMDisposeTargetMachine(target_machine);
            std::string emit_error = "unknown error";
            if (emit_error_c != nullptr) {
                emit_error = std::string{emit_error_c};
            }
            llvm::LLVMDisposeMessage(emit_error_c);
            return std::unexpected(DriverError{std::string("could not emit object file '") + object_path + "': " + emit_error});
        }
        llvm::LLVMDisposeTargetMachine(target_machine);
    }
    return {};
}

[[nodiscard]] std::expected<void, DriverError> emit_module_archive_for_program(Program& program, const std::string& archive_path, int opt_level = 2) {
    std::string object_path = archive_path;
    std::size_t dot = object_path.rfind('.');
    if (dot != std::string::npos) object_path = object_path.substr(0, dot);
    object_path += ".scppo";
    auto emit_r = emit_object_file_for_program(program, object_path, /*emit_debug_info=*/false, opt_level);
    if (!emit_r.has_value()) return std::unexpected(std::move(emit_r).error());
    std::vector<std::string> object_paths{};
    object_paths.push_back(object_path);
    auto archive_r = create_archive(object_paths, archive_path);
    if (!archive_r.has_value()) {
        path_remove(object_path);
        return std::unexpected(std::move(archive_r).error());
    }
    path_remove(object_path);
    return {};
}

// Shared boundary shim for the 3 public entry points below that call
// parse(): parser.cppm's own ParseError-producing failures surface
// normally as a disengaged std::expected (see parse()'s signature), and
// as of batch 6 (#412) the ModuleResolver/PartitionResolver callbacks
// passed into parse() -- here, cache.resolve()/cache.resolve_partition()
// -- report a resolution failure the very same way: both now return
// std::expected<..., ParseError> directly (see ModuleCache::resolve's
// own comment for the loc.is_known() convention distinguishing a
// resolver-native failure from a forwarded, already-positioned nested
// ParseError), so nothing reaching this function can throw anymore. This
// is now a plain .has_value() check converting parser.cppm's ParseError
// into this file's own DriverError, exactly like every other conversion
// in this file (no try/catch left at all -- src/ is exception-free).
[[nodiscard]] std::expected<Program, DriverError> parse_source_with_module_cache(std::string_view source, ModuleCache& cache,
                                                                                  const std::string& source_path) {
    auto program_result = parse(
        source,
        std::function<std::expected<const Program*, ParseError>(const std::string&)>(
            [&cache](const std::string& name) -> std::expected<const Program*, ParseError> { return cache.resolve(name); }),
        std::function<std::expected<Program, ParseError>(const std::string&)>(
            [&cache](const std::string& key) -> std::expected<Program, ParseError> { return cache.resolve_partition(key); }),
        source_path);
    if (!program_result.has_value()) {
        const ParseError& error = program_result.error();
        return std::unexpected(DriverError{error.what(), error.loc});
    }
    std::expected<Program, DriverError> ret{std::move(program_result).value()};
    return ret;
}

} // namespace scpp

export namespace scpp {

std::string host_target_triple() {
    std::string triple{};
    [[scpp::unsafe]] {
        char* triple_c = llvm::LLVMGetDefaultTargetTriple();
        triple = std::string{triple_c};
        llvm::LLVMDisposeMessage(triple_c);
    }
    return triple;
}

std::vector<std::string> project_default_stdlib_link_inputs() { return default_stdlib_link_inputs(); }

std::optional<std::string> driver_runtime_current_executable_path() { return current_executable_path(); }

std::optional<std::string> driver_runtime_default_prebuilt_stdlib_dir() {
    return scpp::runtime_default_prebuilt_stdlib_dir();
}

std::optional<std::string> driver_runtime_installed_stdlib_dir() { return scpp::runtime_installed_stdlib_dir(); }

std::optional<std::string> driver_runtime_default_source_stdlib_dir() {
    return scpp::runtime_default_source_stdlib_dir();
}

// Compiles scpp source text down to a native object file at `object_path`.
// This is the M1/M2/M3 backend: AST -> [move check] -> llvm::LLVM IR -> native
// object code. `import_paths` (ch11 §11.7, `--import name=path`) resolves
// any `import name;` declarations `source` itself has; empty by default
// (no imports -- the overwhelmingly common case, every file before this
// chapter). Only `source`'s own object file is produced here -- an
// imported module's *own* separate object file is compile_to_executable's
// job below, since deciding where to put it needs an executable-level
// path to derive from.
[[nodiscard]] std::expected<void, DriverError> emit_object_file(std::string_view source, const std::string& object_path,
                       const std::unordered_map<std::string, std::string>& import_paths = {},
                       const std::vector<std::string>& import_search_dirs = {},
                       bool emit_debug_info = false,
                       const std::string& source_path = {},
                       int opt_level = 2) {
    ModuleCache cache{import_paths, import_search_dirs};
    auto program_result = parse_source_with_module_cache(source, cache, source_path);
    if (!program_result.has_value()) return std::unexpected(std::move(program_result).error());
    Program program = std::move(program_result.value());
    program.source_path = source_path.empty() ? std::string() : absolute_source_path(source_path);
    return emit_object_file_for_program(program, object_path, emit_debug_info, opt_level);
}

// Compiles source text down to a native object file (-c mode) without invoking the linker.
[[nodiscard]] std::expected<void, DriverError> compile_to_object(std::string_view source, const std::string& object_path,
                       const std::unordered_map<std::string, std::string>& import_paths = {},
                       const std::vector<std::string>& import_search_dirs = {},
                       bool emit_debug_info = false,
                       const std::string& source_path = {},
                       int opt_level = 2) {
    return emit_object_file(source, object_path, import_paths, import_search_dirs, emit_debug_info, source_path, opt_level);
}

[[nodiscard]] inline std::string derive_object_path(std::string_view source_path) {
    std::string filename = path_filename(source_path);
    std::size_t dot = filename.rfind('.');
    if (dot != std::string::npos && dot > 0) {
        return filename.substr(0, dot) + ".o";
    }
    return filename + ".o";
}

[[nodiscard]] std::expected<void, DriverError> emit_module_artifacts(std::string_view source, const std::string& interface_path, const std::string& archive_path,
                           const std::unordered_map<std::string, std::string>& import_paths = {},
                           const std::vector<std::string>& import_search_dirs = {},
                           const std::string& source_path = {},
                           int opt_level = 2) {
    std::unordered_map<std::string, std::string> effective_import_paths = import_paths;
    if (!source_path.empty()) {
        if (std::optional<std::string> module_name = declared_module_name_from_source(source); module_name.has_value()) {
            effective_import_paths.emplace(*module_name, absolute_source_path(source_path));
        }
    }
    ModuleCache cache{std::move(effective_import_paths), import_search_dirs};
    auto program_result = parse_source_with_module_cache(source, cache, source_path);
    if (!program_result.has_value()) return std::unexpected(std::move(program_result).error());
    Program program = std::move(program_result.value());
    program.source_path = source_path.empty() ? std::string() : absolute_source_path(source_path);
    reject_not_yet_lowerable_constexpr_surface(program);
    if (!program.is_module_interface) {
        return std::unexpected(DriverError{std::string("module artifacts can only be emitted from an interface unit, not '") +
                          (program.module_name.empty() ? std::string("<non-module>") : module_key(program)) + "'"});
    }
    auto merged_interface_source_r =
        build_merged_interface_source(program, cache, absolute_source_path(source_path), /*keep_concrete_bodies=*/false);
    if (!merged_interface_source_r.has_value()) return std::unexpected(std::move(merged_interface_source_r).error());
    auto write_r = write_scppm_file(program, merged_interface_source_r.value(), interface_path);
    if (!write_r.has_value()) return std::unexpected(std::move(write_r).error());
    return emit_module_archive_for_program(program, archive_path, opt_level);
}

[[nodiscard]] std::expected<void, DriverError> archive_objects(const std::vector<std::string>& object_paths, const std::string& archive_path) {
    return create_archive(object_paths, archive_path);
}

// Links a native object file into an executable using the system compiler
// driver (clang/cc); this keeps us out of the business of re-implementing a
// platform linker for M1. `extra_link_inputs` is appended verbatim after the
// scpp object file -- additional .o/.a paths (e.g. manifest-built native
// helper objects/archives, see libs/README.md, or another module's own
// compiled object file, see compile_to_executable below) or
// `-lname`/`-Lpath` flags a caller wants forwarded straight to the linker;
// empty by default (an ordinary, no-C++-interop build needs none of this).
//
// Static linking is unconditional here now, mirroring Cargo's own
// static-by-default posture for its targets: scpp's manifest-driven
// `[profile.*]` system (the only mechanism that could ever have asked for
// a *dynamic* link from this function) was removed as underdesigned, so
// there is no longer any supported way to opt out. The `static_link`
// parameter is kept, but unnamed, purely so `compile_to_executable`'s own
// long-standing, unrelated single-file `--static` CLI flag (see
// cli.cppm) keeps a value to forward positionally without having to
// change that independent call site's signature; the value itself is now
// ignored.
[[nodiscard]] std::expected<void, DriverError> link_executable(const std::vector<std::string>& link_inputs, const std::string& executable_path,
                     bool static_link [[maybe_unused]] = false) {
    if (link_inputs.empty()) {
        return std::unexpected(DriverError{std::string("linker command requires at least one input for '") + executable_path + "'"});
    }
    std::string command = "cc -static";
    for (const std::string& input : link_inputs) {
        command += " \"" + input + "\"";
    }
    // A wrapper library that itself calls into real C++ (e.g. std::string)
    // needs libstdc++'s runtime linked in too; `cc` alone (plain C mode)
    // doesn't pull that in automatically the way `c++`/`clang++` would.
    // Only added when there's an actual C++ wrapper to support, so a plain
    // scpp-only build's link command is unaffected. libstdc++'s static
    // floating-point <charconv> support (std::from_chars/to_chars for
    // double/long double) calls fesetround/fegetround, which live in
    // libm -- `c++`/`clang++` pull that in implicitly, but plain `cc`
    // does not, so it must be requested explicitly for a static link.
    if (!link_inputs.empty()) command += " -lstdc++ -lm";
    command += " -o \"" + executable_path + "\"";
    int result = 0;
    [[scpp::unsafe]] {
        result = system(command.c_str());
    }
    if (result != 0) {
        return std::unexpected(DriverError{std::string("linker command failed: ") + command});
    }
    return {};
}

// `import_paths` (ch11 §11.7, `--import name=path`) resolves any
// `import name;` declarations `source` has. When an imported module came
// from a prebuilt `.scppm` and its companion `.scppa` archive exists,
// that archive is linked directly; otherwise the imported Program is
// separately compiled to its own object file. This matches ch11
// §11.13/§11.14's intended "prefer compiled artifacts, fall back to
// source/interface compilation" model while still letting generic
// instantiations materialize in the importing file's own object.
[[nodiscard]] std::expected<void, DriverError> compile_to_executable(std::string_view source, const std::string& executable_path,
                            const std::vector<std::string>& extra_link_inputs = {},
                            const std::unordered_map<std::string, std::string>& import_paths = {},
                            bool static_link = false,
                            const std::vector<std::string>& import_search_dirs = {},
                            bool emit_debug_info = false,
                            const std::string& source_path = {},
                            int opt_level = 2) {
    ModuleCache cache{import_paths, import_search_dirs};
    auto program_result = parse_source_with_module_cache(source, cache, source_path);
    if (!program_result.has_value()) return std::unexpected(std::move(program_result).error());
    Program program = std::move(program_result.value());
    program.source_path = source_path.empty() ? std::string() : absolute_source_path(source_path);

    std::string object_path = executable_path + ".o";
    auto emit_r = emit_object_file_for_program(program, object_path, emit_debug_info, opt_level);
    if (!emit_r.has_value()) return std::unexpected(std::move(emit_r).error());

    std::vector<std::string> module_object_paths{};
    std::vector<std::string> module_archive_paths{};
    std::vector<std::string> resolution = cache.resolution_order();
    for (const std::string& module_name : resolution) {
        if (std::optional<std::string> archive_path = cache.archive_for(module_name); archive_path.has_value()) {
            module_archive_paths.push_back(*archive_path);
            continue;
        }
        std::string module_object_path = executable_path + "." + module_name + ".o";
        auto module_emit_r = emit_object_file_for_program(cache.program_for(module_name), module_object_path, emit_debug_info, opt_level);
        if (!module_emit_r.has_value()) return std::unexpected(std::move(module_emit_r).error());
        module_object_paths.push_back(module_object_path);
    }

    // Each separately-emitted module object is placed before any archive
    // inputs. Prebuilt module archives are then added in reverse
    // dependency order (importer before imported dependency), which keeps
    // a conventional left-to-right static linker able to satisfy one
    // archive's references from a later one.
    std::vector<std::string> link_inputs = module_object_paths;
    auto contains_input = [](const std::vector<std::string>& vec, const std::string& item) -> bool {
        for (const std::string& v : vec) {
            if (v == item) return true;
        }
        return false;
    };
    for (std::size_t i = module_archive_paths.size(); i > 0; --i) {
        const std::string& archive_path = module_archive_paths[i - 1];
        if (!contains_input(link_inputs, archive_path)) {
            link_inputs.push_back(archive_path);
        }
    }
    for (const std::string& extra : extra_link_inputs) {
        link_inputs.push_back(extra);
    }
    bool uses_stdlib = false;
    for (const std::string& mod : resolution) {
        if (mod == "std") {
            uses_stdlib = true;
            break;
        }
    }
    if (uses_stdlib) {
        for (const std::string& input : default_stdlib_link_inputs()) {
            if (!contains_input(link_inputs, input)) {
                link_inputs.push_back(input);
            }
        }
    }
    std::vector<std::string> final_link_inputs{};
    final_link_inputs.push_back(object_path);
    for (const std::string& item : link_inputs) {
        final_link_inputs.push_back(item);
    }
    auto link_r = link_executable(final_link_inputs, executable_path, static_link);
    if (!link_r.has_value()) return std::unexpected(std::move(link_r).error());

    path_remove(object_path);
    for (const std::string& module_object_path : module_object_paths) {
        path_remove(module_object_path);
    }
    return {};
}

} // namespace scpp
