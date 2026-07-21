module;

// Official LLVM-C (llvm-c/*.h) is itself already a stable, extern "C"
// interface -- the TargetMachine construction and object-file emission
// this file performs go through its llvm-c/TargetMachine.h (module
// `llvm`'s `:target_machine` partition) and llvm-c/Target.h (`:target`
// partition) functions directly instead of any native LLVM C++ header
// (llvm::TargetRegistry, llvm::TargetMachine, llvm::legacy::PassManager,
// etc.); the few llvm-c/Core.h pieces this file also touches
// (LLVMModuleRef, LLVMDisposeMessage) come from the same module's
// `:core` partition instead -- all reached via the single `import llvm;`
// below (module `llvm` re-exports every partition, see
// libs/llvm/llvm.cpp). See libs/README.md for why this project binds
// straight to LLVM-C wherever it already covers what's needed --
// including LLVMTargetMachineEmitToFile, which alone replaces the
// raw_fd_ostream + legacy::PassManager + addPassesToEmitFile dance the
// native C++ API required: a rigorous, function-by-function empirical
// audit found LLVM-C fully covers every LLVM operation this project's
// driver needs.

export module scpp.driver;

import std;
import llvm;
import scpp.ast;
import scpp.compiler.codegen;
import scpp.constexpr_engine;
import scpp.lexer;
import scpp.compiler.movecheck;
import scpp.parser;

export namespace scpp {

struct DriverError : std::runtime_error {
    explicit DriverError(const std::string& message) : std::runtime_error(message) {}
};

inline constexpr std::uint32_t SCPPM_COMPILE_TIME_AST_VERSION = 7;
inline constexpr std::string_view SCPPM_COMPILE_TIME_AST_MAGIC = "SAST";

struct CompileTimePayloadPlan {
    std::uint32_t format_version = SCPPM_COMPILE_TIME_AST_VERSION;
    std::vector<std::string> root_function_names;
    std::vector<std::size_t> reachable_function_indices;
    std::vector<std::string> reachable_type_names;
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
    if (program.partition_name.empty()) return program.module_name;
    return program.module_name + ":" + program.partition_name;
}

[[nodiscard]] std::optional<std::string> declared_module_name_from_source(std::string_view source) {
    std::vector<Token> tokens = tokenize(source);
    std::size_t i = 0;
    if (i < tokens.size() && tokens[i].kind == TokenKind::KwExport) i++;
    if (i >= tokens.size() || tokens[i].kind != TokenKind::KwModule) return std::nullopt;
    i++;
    if (i >= tokens.size() || tokens[i].kind != TokenKind::Identifier) return std::nullopt;
    std::string name(tokens[i].text);
    i++;
    while (i + 1 < tokens.size() && tokens[i].kind == TokenKind::Dot && tokens[i + 1].kind == TokenKind::Identifier) {
        name += ".";
        name += std::string(tokens[i + 1].text);
        i += 2;
    }
    if (i < tokens.size() && tokens[i].kind == TokenKind::Colon) return name;
    return name;
}

void write_u32_le(std::ostream& out, std::uint32_t value) {
    std::array<char, 4> bytes = {
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8) & 0xffu),
        static_cast<char>((value >> 16) & 0xffu),
        static_cast<char>((value >> 24) & 0xffu),
    };
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}


namespace {

[[nodiscard]] bool is_exported_generic_type_name(const scpp::Program& program, std::string_view name) {
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

[[nodiscard]] bool is_compile_time_root(const scpp::Program& program, const scpp::Function& fn) {
    if (!fn.member_owner_class.empty() && is_exported_generic_type_name(program, fn.member_owner_class)) {
        return true;
    }
    if (!fn.is_exported) return false;
    if (fn.eval_mode != scpp::FunctionEvalMode::RuntimeOnly || fn.is_generic_template || !fn.template_params.empty()) {
        return true;
    }
    return false;
}

void collect_type_names(const scpp::Type& type, std::unordered_set<std::string>& out) {
    if (type.kind == scpp::TypeKind::Named && !type.name.empty()) out.insert(type.name);
    if (type.pointee) collect_type_names(*type.pointee, out);
    if (type.function_return) collect_type_names(*type.function_return, out);
    for (const scpp::Type& arg : type.template_args) collect_type_names(arg, out);
    for (const scpp::Type& param : type.function_params) collect_type_names(param, out);
}

void collect_stmt_edges(const scpp::Stmt& stmt, std::unordered_set<std::string>& function_names,
                        std::unordered_set<std::string>& type_names);

void collect_expr_edges(const scpp::Expr& expr, std::unordered_set<std::string>& function_names,
                        std::unordered_set<std::string>& type_names) {
    collect_type_names(expr.type, type_names);
    if (expr.kind == scpp::ExprKind::Call && !expr.name.empty()) function_names.insert(expr.name);
    if (expr.kind == scpp::ExprKind::New) collect_type_names(expr.type, type_names);
    if (expr.lhs) collect_expr_edges(*expr.lhs, function_names, type_names);
    if (expr.rhs) collect_expr_edges(*expr.rhs, function_names, type_names);
    if (expr.third) collect_expr_edges(*expr.third, function_names, type_names);
    for (const auto& arg : expr.args) collect_expr_edges(*arg, function_names, type_names);
    if (expr.lambda_body) collect_stmt_edges(*expr.lambda_body, function_names, type_names);
}

void collect_stmt_edges(const scpp::Stmt& stmt, std::unordered_set<std::string>& function_names,
                        std::unordered_set<std::string>& type_names) {
    collect_type_names(stmt.type, type_names);
    if (stmt.init) collect_expr_edges(*stmt.init, function_names, type_names);
    for (const auto& ctor_arg : stmt.ctor_args) collect_expr_edges(*ctor_arg, function_names, type_names);
    if (stmt.has_ctor_args && stmt.type.kind == scpp::TypeKind::Named && !stmt.type.name.empty()) {
        function_names.insert(stmt.type.name + "_new");
    }
    if (stmt.expr) collect_expr_edges(*stmt.expr, function_names, type_names);
    if (stmt.condition) collect_expr_edges(*stmt.condition, function_names, type_names);
    if (stmt.then_branch) collect_stmt_edges(*stmt.then_branch, function_names, type_names);
    if (stmt.else_branch) collect_stmt_edges(*stmt.else_branch, function_names, type_names);
    for (const auto& nested : stmt.statements) collect_stmt_edges(*nested, function_names, type_names);
}

void collect_function_signature_types(const scpp::Function& fn, std::unordered_set<std::string>& type_names) {
    collect_type_names(fn.return_type, type_names);
    for (const scpp::Param& param : fn.params) collect_type_names(param.type, type_names);
}

void collect_function_reachable_edges(const scpp::Function& fn, std::unordered_set<std::string>& function_names,
                                     std::unordered_set<std::string>& type_names) {
    collect_function_signature_types(fn, type_names);
    for (const scpp::MemberInitializer& init : fn.member_initializers) {
        if (init.initializer.expr) collect_expr_edges(*init.initializer.expr, function_names, type_names);
        for (const scpp::ExprPtr& arg : init.initializer.brace_args) collect_expr_edges(*arg, function_names, type_names);
    }
    if (fn.body) collect_stmt_edges(*fn.body, function_names, type_names);
}

void collect_class_reachable_edges(const scpp::ClassDef& def, std::unordered_set<std::string>& function_names,
                                   std::unordered_set<std::string>& type_names) {
    for (const scpp::ClassField& field : def.fields) {
        collect_type_names(field.type, type_names);
        if (!field.default_initializer) continue;
        if (field.default_initializer->expr) collect_expr_edges(*field.default_initializer->expr, function_names, type_names);
        for (const scpp::ExprPtr& arg : field.default_initializer->brace_args) collect_expr_edges(*arg, function_names, type_names);
        if (field.default_initializer->has_brace_args && field.type.kind == scpp::TypeKind::Named && !field.type.name.empty()) {
            function_names.insert(field.type.name + "_new");
        }
    }
    for (const scpp::BaseSpecifier& base : def.base_specifiers) {
        collect_type_names(base.base_type, type_names);
        for (const std::shared_ptr<scpp::Expr>& arg : base.base_type.non_type_args) {
            if (arg) collect_expr_edges(*arg, function_names, type_names);
        }
    }
    for (const scpp::Type& arg : def.specialization_template_args) collect_type_names(arg, type_names);
    if (def.thread_movable_if_movable_expr) {
        collect_expr_edges(*def.thread_movable_if_movable_expr, function_names, type_names);
    }
    if (def.thread_movable_if_shareable_expr) {
        collect_expr_edges(*def.thread_movable_if_shareable_expr, function_names, type_names);
    }
}

void reject_not_yet_lowerable_constexpr_surface(const Program& program) {
    std::function<void(const Stmt&)> walk_stmt = [&](const Stmt& stmt) {
        if (stmt.init) {
            // nothing to validate inside expressions yet
        }
        if (stmt.then_branch) walk_stmt(*stmt.then_branch);
        if (stmt.else_branch) walk_stmt(*stmt.else_branch);
        for (const StmtPtr& nested : stmt.statements) walk_stmt(*nested);
    };
    for (const Function& fn : program.functions) {
        if (fn.body) walk_stmt(*fn.body);
    }
}

} // namespace

CompileTimePayloadPlan plan_compile_time_payload(const Program& program) {
    CompileTimePayloadPlan plan;
    std::unordered_map<std::string, std::vector<std::size_t>> function_indices_by_name;
    std::unordered_map<std::string, const EnumDef*> enums_by_name;
    std::unordered_map<std::string, const StructDef*> structs_by_name;
    std::unordered_map<std::string, const ClassDef*> classes_by_name;
    std::unordered_multimap<std::string, std::size_t> methods_by_owner;
    for (std::size_t i = 0; i < program.functions.size(); i++) {
        function_indices_by_name[program.functions[i].name].push_back(i);
        if (!program.functions[i].member_owner_class.empty()) {
            methods_by_owner.emplace(program.functions[i].member_owner_class, i);
        }
    }
    for (const EnumDef& def : program.enums) enums_by_name.emplace(def.name, &def);
    for (const StructDef& def : program.structs) structs_by_name.emplace(def.name, &def);
    for (const ClassDef& def : program.classes) classes_by_name.emplace(def.name, &def);

    std::unordered_set<std::size_t> visited_function_indices;
    std::unordered_set<std::string> visited_types;
    std::unordered_set<std::string> pending_types;
    std::vector<std::size_t> worklist;

    auto enqueue_type = [&](const std::string& name) {
        if (name.empty()) return;
        if (visited_types.insert(name).second) {
            plan.reachable_type_names.push_back(name);
            pending_types.insert(name);
        }
    };

    auto enqueue_function_index = [&](std::size_t index, bool is_root = false) {
        if (index >= program.functions.size()) return;
        const Function& fn = program.functions[index];
        if (visited_function_indices.insert(index).second) {
            plan.reachable_function_indices.push_back(index);
            worklist.push_back(index);
        }
        if (is_root && std::find(plan.root_function_names.begin(), plan.root_function_names.end(), fn.name) ==
                           plan.root_function_names.end()) {
            plan.root_function_names.push_back(fn.name);
        }
    };
    auto enqueue_functions_by_name = [&](const std::string& name) {
        if (name.empty()) return;
        auto it = function_indices_by_name.find(name);
        if (it == function_indices_by_name.end()) return;
        for (std::size_t index : it->second) enqueue_function_index(index);
    };

    for (std::size_t i = 0; i < program.functions.size(); i++) {
        if (is_compile_time_root(program, program.functions[i])) enqueue_function_index(i, true);
    }

    std::size_t next_function_index = 0;
    while (next_function_index < worklist.size() || !pending_types.empty()) {
        while (next_function_index < worklist.size()) {
            const Function& fn = program.functions[worklist[next_function_index++]];
            std::unordered_set<std::string> local_function_names;
            std::unordered_set<std::string> local_type_names;
            collect_function_reachable_edges(fn, local_function_names, local_type_names);
            for (const std::string& callee : local_function_names) enqueue_functions_by_name(callee);
            for (const std::string& type_name : local_type_names) enqueue_type(type_name);
        }

        if (pending_types.empty()) continue;
        std::vector<std::string> batch(pending_types.begin(), pending_types.end());
        pending_types.clear();
        for (const std::string& type_name : batch) {
            if (auto it = enums_by_name.find(type_name); it != enums_by_name.end()) {
                std::unordered_set<std::string> nested;
                collect_type_names(it->second->underlying_type, nested);
                for (const std::string& nested_name : nested) enqueue_type(nested_name);
            }
            if (auto it = structs_by_name.find(type_name); it != structs_by_name.end()) {
                for (const StructField& field : it->second->fields) {
                    std::unordered_set<std::string> nested;
                    collect_type_names(field.type, nested);
                    for (const std::string& nested_name : nested) enqueue_type(nested_name);
                }
            }
            if (auto it = classes_by_name.find(type_name); it != classes_by_name.end()) {
                std::unordered_set<std::string> nested_function_names;
                std::unordered_set<std::string> nested_type_names;
                collect_class_reachable_edges(*it->second, nested_function_names, nested_type_names);
                for (const std::string& nested_name : nested_type_names) enqueue_type(nested_name);
                for (const std::string& function_name : nested_function_names) enqueue_functions_by_name(function_name);
                auto [begin, end] = methods_by_owner.equal_range(type_name);
                for (auto method = begin; method != end; ++method) {
                    enqueue_function_index(method->second);
                }
            }
        }
    }

    return plan;
}

struct StructuredCompileTimePayload {
    std::vector<std::string> root_function_names;
    std::vector<EnumDef> enums;
    std::vector<StructDef> structs;
    std::vector<ClassDef> classes;
    std::vector<Function> functions;
};

struct LoadedModuleFile {
    std::string interface_source;
    bool is_scppm = false;
    bool has_compile_time_payload = false;
    std::string compile_time_payload_bytes;
};

void write_u8(std::ostream& out, std::uint8_t value) { out.put(static_cast<char>(value)); }

[[nodiscard]] std::uint8_t read_u8(std::istream& in, const std::string& context) {
    char byte = '\0';
    in.read(&byte, 1);
    if (!in) throw DriverError("invalid " + context + ": truncated byte");
    return static_cast<std::uint8_t>(static_cast<unsigned char>(byte));
}

void write_i64_le(std::ostream& out, std::int64_t value) {
    std::array<char, 8> bytes = {};
    std::uint64_t raw = static_cast<std::uint64_t>(value);
    for (std::size_t i = 0; i < bytes.size(); i++) bytes[i] = static_cast<char>((raw >> (8 * i)) & 0xffu);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_u64_le(std::ostream& out, std::uint64_t value) {
    std::array<char, 8> bytes = {};
    for (std::size_t i = 0; i < bytes.size(); i++) bytes[i] = static_cast<char>((value >> (8 * i)) & 0xffu);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::uint32_t read_u32_le(std::istream& in, const std::string& context) {
    std::array<unsigned char, 4> bytes = {};
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in) throw DriverError("invalid " + context + ": truncated u32");
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

[[nodiscard]] std::int64_t read_i64_le(std::istream& in, const std::string& context) {
    std::array<unsigned char, 8> bytes = {};
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in) throw DriverError("invalid " + context + ": truncated i64");
    std::uint64_t raw = 0;
    for (std::size_t i = 0; i < bytes.size(); i++) raw |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    return static_cast<std::int64_t>(raw);
}

[[nodiscard]] std::uint64_t read_u64_le(std::istream& in, const std::string& context) {
    std::array<unsigned char, 8> bytes = {};
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in) throw DriverError("invalid " + context + ": truncated u64");
    std::uint64_t raw = 0;
    for (std::size_t i = 0; i < bytes.size(); i++) raw |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    return raw;
}

void write_double_le(std::ostream& out, double value) {
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    std::uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    write_i64_le(out, static_cast<std::int64_t>(raw));
}

[[nodiscard]] double read_double_le(std::istream& in, const std::string& context) {
    std::uint64_t raw = static_cast<std::uint64_t>(read_i64_le(in, context));
    double value = 0.0;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

void write_string(std::ostream& out, std::string_view text) {
    write_u32_le(out, static_cast<std::uint32_t>(text.size()));
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

[[nodiscard]] std::string read_string(std::istream& in, const std::string& context) {
    std::uint32_t size = read_u32_le(in, context + " string length");
    std::string text(size, '\0');
    in.read(text.data(), static_cast<std::streamsize>(size));
    if (!in) throw DriverError("invalid " + context + ": truncated string");
    return text;
}

void write_source_location(std::ostream& out, const SourceLocation& loc) {
    write_i64_le(out, loc.line);
    write_i64_le(out, loc.column);
    write_string(out, loc.source_path_text());
}

[[nodiscard]] SourceLocation read_source_location(std::istream& in, const std::string& context) {
    SourceLocation loc;
    loc.line = static_cast<int>(read_i64_le(in, context + " line"));
    loc.column = static_cast<int>(read_i64_le(in, context + " column"));
    std::string source_path = read_string(in, context + " source path");
    if (!source_path.empty()) loc.source_path = std::make_shared<const std::string>(std::move(source_path));
    return loc;
}

template<typename Enum>
void write_enum(std::ostream& out, Enum value) {
    write_u8(out, static_cast<std::uint8_t>(value));
}

template<typename Enum>
[[nodiscard]] Enum read_enum(std::istream& in, const std::string& context) {
    return static_cast<Enum>(read_u8(in, context));
}

void write_type(std::ostream& out, const Type& type);
ExprPtr read_expr(std::istream& in, const std::string& context);
void write_expr(std::ostream& out, const Expr& expr);
StmtPtr read_stmt(std::istream& in, const std::string& context);
void write_stmt(std::ostream& out, const Stmt& stmt);
void write_alignment_specifier(std::ostream& out, const AlignmentSpecifier& spec);
[[nodiscard]] AlignmentSpecifier read_alignment_specifier(std::istream& in, const std::string& context);

void write_type(std::ostream& out, const Type& type) {
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
    write_u32_le(out, static_cast<std::uint32_t>(type.template_args.size()));
    for (const Type& arg : type.template_args) write_type(out, arg);
    write_u32_le(out, static_cast<std::uint32_t>(type.non_type_args.size()));
    for (const auto& arg : type.non_type_args) {
        write_u8(out, arg ? 1u : 0u);
        if (arg) write_expr(out, *arg);
    }
    write_u8(out, type.is_pack_expansion ? 1u : 0u);
}

[[nodiscard]] Type read_type(std::istream& in, const std::string& context) {
    Type type;
    type.kind = read_enum<TypeKind>(in, context + " kind");
    type.name = read_string(in, context + " name");
    if (read_u8(in, context + " pointee present") != 0u) type.pointee = std::make_shared<Type>(read_type(in, context + " pointee"));
    if (read_u8(in, context + " element present") != 0u) type.element = std::make_shared<Type>(read_type(in, context + " element"));
    type.array_size = read_i64_le(in, context + " array size");
    if (read_u8(in, context + " array size expr present") != 0u) {
        type.array_size_expr = std::shared_ptr<Expr>(read_expr(in, context + " array size expr").release());
    }
    if (read_u8(in, context + " function return present") != 0u) {
        type.function_return = std::make_shared<Type>(read_type(in, context + " function return"));
    }
    std::uint32_t param_count = read_u32_le(in, context + " function param count");
    type.function_params.reserve(param_count);
    for (std::uint32_t i = 0; i < param_count; i++) type.function_params.push_back(read_type(in, context + " function param"));
    type.is_unsafe_function_pointer = read_u8(in, context + " unsafe fn ptr") != 0u;
    type.is_const_function = read_u8(in, context + " const fn") != 0u;
    type.function_ref_qualifier = read_enum<ReceiverRefQualifier>(in, context + " fn ref qualifier");
    type.is_mutable_ref = read_u8(in, context + " mutable ref") != 0u;
    type.is_rvalue_ref = read_u8(in, context + " rvalue ref") != 0u;
    type.is_mutable_pointee = read_u8(in, context + " mutable pointee") != 0u;
    std::uint32_t template_arg_count = read_u32_le(in, context + " template arg count");
    type.template_args.reserve(template_arg_count);
    for (std::uint32_t i = 0; i < template_arg_count; i++) type.template_args.push_back(read_type(in, context + " template arg"));
    std::uint32_t non_type_arg_count = read_u32_le(in, context + " non-type arg count");
    type.non_type_args.reserve(non_type_arg_count);
    for (std::uint32_t i = 0; i < non_type_arg_count; i++) {
        if (read_u8(in, context + " non-type arg present") != 0u) {
            type.non_type_args.push_back(std::shared_ptr<Expr>(read_expr(in, context + " non-type arg").release()));
        } else {
            type.non_type_args.push_back(nullptr);
        }
    }
    type.is_pack_expansion = read_u8(in, context + " pack expansion") != 0u;
    return type;
}

void write_generic_type_param(std::ostream& out, const GenericTypeParam& param) {
    write_string(out, param.name);
    write_string(out, param.concept_name);
    write_u8(out, param.is_pack ? 1u : 0u);
    write_u8(out, param.is_non_type ? 1u : 0u);
    write_type(out, param.non_type_type);
}

[[nodiscard]] GenericTypeParam read_generic_type_param(std::istream& in, const std::string& context) {
    GenericTypeParam param;
    param.name = read_string(in, context + " name");
    param.concept_name = read_string(in, context + " concept");
    param.is_pack = read_u8(in, context + " is_pack") != 0u;
    param.is_non_type = read_u8(in, context + " is_non_type") != 0u;
    param.non_type_type = read_type(in, context + " non-type type");
    return param;
}

void write_param(std::ostream& out, const Param& param) {
    write_type(out, param.type);
    write_string(out, param.name);
    write_string(out, param.lifetime.name);
    write_string(out, param.generic_concept);
    write_u8(out, param.require_thread_movable ? 1u : 0u);
    write_u8(out, param.require_thread_shareable ? 1u : 0u);
    write_u8(out, param.is_parameter_pack ? 1u : 0u);
}

[[nodiscard]] Param read_param(std::istream& in, const std::string& context) {
    Param param;
    param.type = read_type(in, context + " type");
    param.name = read_string(in, context + " name");
    param.lifetime.name = read_string(in, context + " lifetime");
    param.generic_concept = read_string(in, context + " generic concept");
    param.require_thread_movable = read_u8(in, context + " thread_movable") != 0u;
    param.require_thread_shareable = read_u8(in, context + " thread_shareable") != 0u;
    param.is_parameter_pack = read_u8(in, context + " parameter pack") != 0u;
    return param;
}

void write_lambda_capture(std::ostream& out, const LambdaCapture& capture) {
    write_string(out, capture.name);
    write_u8(out, capture.by_reference ? 1u : 0u);
    write_u8(out, capture.init ? 1u : 0u);
    if (capture.init) write_expr(out, *capture.init);
}

[[nodiscard]] LambdaCapture read_lambda_capture(std::istream& in, const std::string& context) {
    LambdaCapture capture;
    capture.name = read_string(in, context + " name");
    capture.by_reference = read_u8(in, context + " by_reference") != 0u;
    if (read_u8(in, context + " init present") != 0u) capture.init = read_expr(in, context + " init");
    return capture;
}

void write_explicit_template_arg(std::ostream& out, const ExplicitTemplateArg& arg) {
    write_u8(out, arg.is_type ? 1u : 0u);
    write_type(out, arg.type);
    write_u8(out, arg.value ? 1u : 0u);
    if (arg.value) write_expr(out, *arg.value);
}

[[nodiscard]] ExplicitTemplateArg read_explicit_template_arg(std::istream& in, const std::string& context) {
    ExplicitTemplateArg arg;
    arg.is_type = read_u8(in, context + " is_type") != 0u;
    arg.type = read_type(in, context + " type");
    if (read_u8(in, context + " value present") != 0u) arg.value = std::shared_ptr<Expr>(read_expr(in, context + " value").release());
    return arg;
}

void write_expr(std::ostream& out, const Expr& expr) {
    write_enum(out, expr.kind);
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

ExprPtr read_expr(std::istream& in, const std::string& context) {
    auto expr = std::make_unique<Expr>();
    expr->kind = read_enum<ExprKind>(in, context + " kind");
    expr->loc = read_source_location(in, context + " loc");
    expr->int_value = read_i64_le(in, context + " int");
    expr->float_value = read_double_le(in, context + " double");
    expr->bool_value = read_u8(in, context + " bool") != 0u;
    expr->name = read_string(in, context + " name");
    expr->explicit_global_qualification = read_u8(in, context + " global qualification") != 0u;
    expr->binary_op = read_enum<BinaryOp>(in, context + " binary op");
    if (read_u8(in, context + " lhs present") != 0u) expr->lhs = read_expr(in, context + " lhs");
    if (read_u8(in, context + " rhs present") != 0u) expr->rhs = read_expr(in, context + " rhs");
    if (read_u8(in, context + " third present") != 0u) expr->third = read_expr(in, context + " third");
    expr->fold_ellipsis_on_left = read_u8(in, context + " fold left") != 0u;
    expr->unary_op = read_enum<UnaryOp>(in, context + " unary op");
    std::uint32_t arg_count = read_u32_le(in, context + " arg count");
    expr->args.reserve(arg_count);
    for (std::uint32_t i = 0; i < arg_count; i++) expr->args.push_back(read_expr(in, context + " arg"));
    std::uint32_t explicit_arg_count = read_u32_le(in, context + " explicit arg count");
    expr->explicit_template_args.reserve(explicit_arg_count);
    for (std::uint32_t i = 0; i < explicit_arg_count; i++) expr->explicit_template_args.push_back(read_explicit_template_arg(in, context + " explicit arg"));
    expr->type = read_type(in, context + " type");
    expr->sizeof_operand_is_type = read_u8(in, context + " sizeof is type") != 0u;
    expr->has_paren_init = read_u8(in, context + " has paren init") != 0u;
    expr->destroy_through_pointer = read_u8(in, context + " destroy through pointer") != 0u;
    expr->through_arrow = read_u8(in, context + " through arrow") != 0u;
    expr->implicit_arrow_deref = read_u8(in, context + " implicit arrow deref") != 0u;
    expr->implicit_arrow_chain_safe = read_u8(in, context + " implicit arrow chain safe") != 0u;
    std::uint32_t capture_count = read_u32_le(in, context + " capture count");
    expr->lambda_captures.reserve(capture_count);
    for (std::uint32_t i = 0; i < capture_count; i++) expr->lambda_captures.push_back(read_lambda_capture(in, context + " capture"));
    expr->lambda_blanket_mode = read_enum<LambdaCaptureMode>(in, context + " blanket mode");
    std::uint32_t lambda_param_count = read_u32_le(in, context + " lambda param count");
    expr->lambda_params.reserve(lambda_param_count);
    for (std::uint32_t i = 0; i < lambda_param_count; i++) expr->lambda_params.push_back(read_param(in, context + " lambda param"));
    expr->has_lambda_explicit_return_type = read_u8(in, context + " explicit return") != 0u;
    expr->lambda_is_mutable = read_u8(in, context + " lambda mutable") != 0u;
    if (read_u8(in, context + " lambda body present") != 0u) expr->lambda_body = read_stmt(in, context + " lambda body");
    return expr;
}

void write_stmt(std::ostream& out, const Stmt& stmt) {
    write_enum(out, stmt.kind);
    write_source_location(out, stmt.loc);
    write_type(out, stmt.type);
    write_string(out, stmt.var_name);
    write_u8(out, stmt.init ? 1u : 0u);
    if (stmt.init) write_expr(out, *stmt.init);
    write_u32_le(out, static_cast<std::uint32_t>(stmt.alignment_specs.size()));
    for (const AlignmentSpecifier& spec : stmt.alignment_specs) write_alignment_specifier(out, spec);
    write_u64_le(out, stmt.resolved_alignment);
    write_u8(out, stmt.is_const ? 1u : 0u);
    write_u8(out, stmt.is_constexpr ? 1u : 0u);
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
    write_u32_le(out, static_cast<std::uint32_t>(stmt.statements.size()));
    for (const auto& nested : stmt.statements) write_stmt(out, *nested);
    write_u8(out, stmt.is_unsafe ? 1u : 0u);
}

StmtPtr read_stmt(std::istream& in, const std::string& context) {
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = read_enum<StmtKind>(in, context + " kind");
    stmt->loc = read_source_location(in, context + " loc");
    stmt->type = read_type(in, context + " type");
    stmt->var_name = read_string(in, context + " var name");
    if (read_u8(in, context + " init present") != 0u) stmt->init = read_expr(in, context + " init");
    std::uint32_t align_spec_count = read_u32_le(in, context + " align spec count");
    stmt->alignment_specs.reserve(align_spec_count);
    for (std::uint32_t i = 0; i < align_spec_count; i++) {
        stmt->alignment_specs.push_back(read_alignment_specifier(in, context + " align spec"));
    }
    stmt->resolved_alignment = read_u64_le(in, context + " resolved alignment");
    stmt->is_const = read_u8(in, context + " is_const") != 0u;
    stmt->is_constexpr = read_u8(in, context + " is_constexpr") != 0u;
    stmt->has_ctor_args = read_u8(in, context + " has ctor args") != 0u;
    std::uint32_t ctor_arg_count = read_u32_le(in, context + " ctor arg count");
    stmt->ctor_args.reserve(ctor_arg_count);
    for (std::uint32_t i = 0; i < ctor_arg_count; i++) stmt->ctor_args.push_back(read_expr(in, context + " ctor arg"));
    if (read_u8(in, context + " expr present") != 0u) stmt->expr = read_expr(in, context + " expr");
    if (read_u8(in, context + " condition present") != 0u) stmt->condition = read_expr(in, context + " condition");
    stmt->if_mode = read_enum<IfMode>(in, context + " if mode");
    if (read_u8(in, context + " then present") != 0u) stmt->then_branch = read_stmt(in, context + " then");
    if (read_u8(in, context + " else present") != 0u) stmt->else_branch = read_stmt(in, context + " else");
    std::uint32_t nested_count = read_u32_le(in, context + " nested count");
    stmt->statements.reserve(nested_count);
    for (std::uint32_t i = 0; i < nested_count; i++) stmt->statements.push_back(read_stmt(in, context + " nested"));
    stmt->is_unsafe = read_u8(in, context + " unsafe") != 0u;
    return stmt;
}

void write_initializer(std::ostream& out, const Initializer& init) {
    write_u8(out, init.expr ? 1u : 0u);
    if (init.expr) write_expr(out, *init.expr);
    write_u8(out, init.has_brace_args ? 1u : 0u);
    write_u32_le(out, static_cast<std::uint32_t>(init.brace_args.size()));
    for (const ExprPtr& arg : init.brace_args) write_expr(out, *arg);
}

void write_alignment_specifier(std::ostream& out, const AlignmentSpecifier& spec) {
    write_source_location(out, spec.loc);
    write_u8(out, spec.operand_is_type ? 1u : 0u);
    write_type(out, spec.type);
    write_u8(out, spec.expr ? 1u : 0u);
    if (spec.expr) write_expr(out, *spec.expr);
}

[[nodiscard]] AlignmentSpecifier read_alignment_specifier(std::istream& in, const std::string& context) {
    AlignmentSpecifier spec;
    spec.loc = read_source_location(in, context + " loc");
    spec.operand_is_type = read_u8(in, context + " operand is type") != 0u;
    spec.type = read_type(in, context + " type");
    if (read_u8(in, context + " expr present") != 0u) spec.expr = read_expr(in, context + " expr");
    return spec;
}

[[nodiscard]] Initializer read_initializer(std::istream& in, const std::string& context) {
    Initializer init;
    if (read_u8(in, context + " expr present") != 0u) init.expr = read_expr(in, context + " expr");
    init.has_brace_args = read_u8(in, context + " brace args present") != 0u;
    std::uint32_t arg_count = read_u32_le(in, context + " brace arg count");
    init.brace_args.reserve(arg_count);
    for (std::uint32_t i = 0; i < arg_count; i++) init.brace_args.push_back(read_expr(in, context + " brace arg"));
    return init;
}

void write_member_initializer(std::ostream& out, const MemberInitializer& init) {
    write_string(out, init.member_name);
    write_initializer(out, init.initializer);
    write_source_location(out, init.loc);
}

[[nodiscard]] MemberInitializer read_member_initializer(std::istream& in, const std::string& context) {
    MemberInitializer init;
    init.member_name = read_string(in, context + " member name");
    init.initializer = read_initializer(in, context + " initializer");
    init.loc = read_source_location(in, context + " loc");
    return init;
}

void write_struct_field(std::ostream& out, const StructField& field) {
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

[[nodiscard]] StructField read_struct_field(std::istream& in, const std::string& context) {
    StructField field;
    field.loc = read_source_location(in, context + " loc");
    field.type = read_type(in, context + " type");
    field.name = read_string(in, context + " name");
    if (read_u8(in, context + " default initializer present") != 0u) {
        field.default_initializer = read_initializer(in, context + " default initializer");
    }
    field.access = read_enum<AccessSpecifier>(in, context + " access");
    std::uint32_t align_spec_count = read_u32_le(in, context + " align spec count");
    field.alignment_specs.reserve(align_spec_count);
    for (std::uint32_t i = 0; i < align_spec_count; i++) {
        field.alignment_specs.push_back(read_alignment_specifier(in, context + " align spec"));
    }
    field.resolved_alignment = read_u64_le(in, context + " resolved alignment");
    return field;
}

void write_class_field(std::ostream& out, const ClassField& field) {
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

[[nodiscard]] ClassField read_class_field(std::istream& in, const std::string& context) {
    ClassField field;
    field.loc = read_source_location(in, context + " loc");
    field.type = read_type(in, context + " type");
    field.name = read_string(in, context + " name");
    if (read_u8(in, context + " default initializer present") != 0u) {
        field.default_initializer = read_initializer(in, context + " default initializer");
    }
    field.access = read_enum<AccessSpecifier>(in, context + " access");
    std::uint32_t align_spec_count = read_u32_le(in, context + " align spec count");
    field.alignment_specs.reserve(align_spec_count);
    for (std::uint32_t i = 0; i < align_spec_count; i++) {
        field.alignment_specs.push_back(read_alignment_specifier(in, context + " align spec"));
    }
    field.resolved_alignment = read_u64_le(in, context + " resolved alignment");
    return field;
}

void write_base_specifier(std::ostream& out, const BaseSpecifier& base) {
    write_type(out, base.base_type);
    write_enum(out, base.access);
    write_u8(out, base.is_virtual ? 1u : 0u);
    write_enum(out, base.kind);
    write_string(out, base.pack_arg_name);
}

[[nodiscard]] BaseSpecifier read_base_specifier(std::istream& in, const std::string& context) {
    BaseSpecifier base;
    base.base_type = read_type(in, context + " type");
    base.access = read_enum<AccessSpecifier>(in, context + " access");
    base.is_virtual = read_u8(in, context + " is_virtual") != 0u;
    base.kind = read_enum<BaseClassKind>(in, context + " kind");
    base.pack_arg_name = read_string(in, context + " pack arg");
    return base;
}

void write_class_using_declaration(std::ostream& out, const ClassUsingDeclaration& decl) {
    write_string(out, decl.base_name);
    write_string(out, decl.member_name);
    write_enum(out, decl.access);
}

[[nodiscard]] ClassUsingDeclaration read_class_using_declaration(std::istream& in, const std::string& context) {
    ClassUsingDeclaration decl;
    decl.base_name = read_string(in, context + " base name");
    decl.member_name = read_string(in, context + " member name");
    decl.access = read_enum<AccessSpecifier>(in, context + " access");
    return decl;
}

void write_enum_variant(std::ostream& out, const EnumVariant& variant) {
    write_string(out, variant.name);
    write_i64_le(out, variant.value);
}

[[nodiscard]] EnumVariant read_enum_variant(std::istream& in, const std::string& context) {
    EnumVariant variant;
    variant.name = read_string(in, context + " name");
    variant.value = read_i64_le(in, context + " value");
    return variant;
}

void write_enum_def(std::ostream& out, const EnumDef& def) {
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

[[nodiscard]] EnumDef read_enum_def(std::istream& in, const std::string& context) {
    EnumDef def;
    def.name = read_string(in, context + " name");
    def.underlying_type = read_type(in, context + " underlying");
    std::uint32_t variant_count = read_u32_le(in, context + " variant count");
    def.variants.reserve(variant_count);
    for (std::uint32_t i = 0; i < variant_count; i++) def.variants.push_back(read_enum_variant(in, context + " variant"));
    std::uint32_t ns_count = read_u32_le(in, context + " namespace count");
    def.namespace_path.reserve(ns_count);
    for (std::uint32_t i = 0; i < ns_count; i++) def.namespace_path.push_back(read_string(in, context + " namespace"));
    def.is_exported = read_u8(in, context + " is_exported") != 0u;
    def.is_compile_time_dependency = read_u8(in, context + " is_compile_time_dependency") != 0u;
    def.owning_module = read_string(in, context + " owning_module");
    return def;
}

void write_struct_def(std::ostream& out, const StructDef& def) {
    write_source_location(out, def.loc);
    write_string(out, def.name);
    write_u32_le(out, static_cast<std::uint32_t>(def.fields.size()));
    for (const StructField& field : def.fields) write_struct_field(out, field);
    write_u8(out, def.is_union ? 1u : 0u);
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

[[nodiscard]] StructDef read_struct_def(std::istream& in, const std::string& context) {
    StructDef def;
    def.loc = read_source_location(in, context + " loc");
    def.name = read_string(in, context + " name");
    std::uint32_t field_count = read_u32_le(in, context + " field count");
    def.fields.reserve(field_count);
    for (std::uint32_t i = 0; i < field_count; i++) def.fields.push_back(read_struct_field(in, context + " field"));
    def.is_union = read_u8(in, context + " is_union") != 0u;
    def.is_packed = read_u8(in, context + " is_packed") != 0u;
    std::uint32_t align_spec_count = read_u32_le(in, context + " align spec count");
    def.alignment_specs.reserve(align_spec_count);
    for (std::uint32_t i = 0; i < align_spec_count; i++) {
        def.alignment_specs.push_back(read_alignment_specifier(in, context + " align spec"));
    }
    def.resolved_alignment = read_u64_le(in, context + " resolved alignment");
    std::uint32_t ns_count = read_u32_le(in, context + " namespace count");
    def.namespace_path.reserve(ns_count);
    for (std::uint32_t i = 0; i < ns_count; i++) def.namespace_path.push_back(read_string(in, context + " namespace"));
    def.is_exported = read_u8(in, context + " is_exported") != 0u;
    def.is_compile_time_dependency = read_u8(in, context + " is_compile_time_dependency") != 0u;
    def.owning_module = read_string(in, context + " owning_module");
    std::uint32_t template_param_count = read_u32_le(in, context + " template param count");
    def.template_params.reserve(template_param_count);
    for (std::uint32_t i = 0; i < template_param_count; i++) def.template_params.push_back(read_generic_type_param(in, context + " template param"));
    def.is_forward_declaration = read_u8(in, context + " is_forward_declaration") != 0u;
    def.template_owner_id = read_string(in, context + " template owner id");
    def.thread_movable_override = read_u8(in, context + " thread movable override") != 0u;
    def.thread_shareable_override = read_u8(in, context + " thread shareable override") != 0u;
    def.is_nodiscard = read_u8(in, context + " nodiscard") != 0u;
    def.nodiscard_reason = read_string(in, context + " nodiscard reason");
    return def;
}

void write_class_def(std::ostream& out, const ClassDef& def) {
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

[[nodiscard]] ClassDef read_class_def(std::istream& in, const std::string& context) {
    ClassDef def;
    def.loc = read_source_location(in, context + " loc");
    def.name = read_string(in, context + " name");
    std::uint32_t field_count = read_u32_le(in, context + " field count");
    def.fields.reserve(field_count);
    for (std::uint32_t i = 0; i < field_count; i++) def.fields.push_back(read_class_field(in, context + " field"));
    std::uint32_t align_spec_count = read_u32_le(in, context + " align spec count");
    def.alignment_specs.reserve(align_spec_count);
    for (std::uint32_t i = 0; i < align_spec_count; i++) {
        def.alignment_specs.push_back(read_alignment_specifier(in, context + " align spec"));
    }
    def.resolved_alignment = read_u64_le(in, context + " resolved alignment");
    std::uint32_t ns_count = read_u32_le(in, context + " namespace count");
    def.namespace_path.reserve(ns_count);
    for (std::uint32_t i = 0; i < ns_count; i++) def.namespace_path.push_back(read_string(in, context + " namespace"));
    def.is_exported = read_u8(in, context + " is_exported") != 0u;
    def.is_compile_time_dependency = read_u8(in, context + " is_compile_time_dependency") != 0u;
    def.owning_module = read_string(in, context + " owning_module");
    def.is_concept_witness = read_u8(in, context + " is_concept_witness") != 0u;
    def.is_interface = read_u8(in, context + " is_interface") != 0u;
    std::uint32_t base_count = read_u32_le(in, context + " base count");
    def.base_specifiers.reserve(base_count);
    for (std::uint32_t i = 0; i < base_count; i++) def.base_specifiers.push_back(read_base_specifier(in, context + " base"));
    std::uint32_t using_count = read_u32_le(in, context + " using count");
    def.using_declarations.reserve(using_count);
    for (std::uint32_t i = 0; i < using_count; i++) {
        def.using_declarations.push_back(read_class_using_declaration(in, context + " using"));
    }
    std::uint32_t template_param_count = read_u32_le(in, context + " template param count");
    def.template_params.reserve(template_param_count);
    for (std::uint32_t i = 0; i < template_param_count; i++) def.template_params.push_back(read_generic_type_param(in, context + " template param"));
    def.template_owner_id = read_string(in, context + " template owner id");
    def.is_forward_declaration = read_u8(in, context + " is_forward_declaration") != 0u;
    def.is_synthetic_check_only = read_u8(in, context + " is_synthetic_check_only") != 0u;
    def.is_variadic_primary_template = read_u8(in, context + " is_variadic_primary") != 0u;
    def.is_variadic_specialization = read_u8(in, context + " is_variadic_specialization") != 0u;
    def.is_partial_specialization = read_u8(in, context + " is_partial_specialization") != 0u;
    std::uint32_t spec_arg_count = read_u32_le(in, context + " specialization arg count");
    def.specialization_template_args.reserve(spec_arg_count);
    for (std::uint32_t i = 0; i < spec_arg_count; i++) def.specialization_template_args.push_back(read_type(in, context + " specialization arg"));
    def.thread_movable_override = read_u8(in, context + " thread movable override") != 0u;
    def.thread_shareable_override = read_u8(in, context + " thread shareable override") != 0u;
    if (read_u8(in, context + " movable_if expr present") != 0u) {
        def.thread_movable_if_movable_expr = read_expr(in, context + " movable_if expr");
    }
    if (read_u8(in, context + " shareable_if expr present") != 0u) {
        def.thread_movable_if_shareable_expr = read_expr(in, context + " shareable_if expr");
    }
    def.is_nodiscard = read_u8(in, context + " nodiscard") != 0u;
    def.nodiscard_reason = read_string(in, context + " nodiscard reason");
    return def;
}

void write_function(std::ostream& out, const Function& fn) {
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
    write_string(out, fn.forwards_to);
    write_u32_le(out, static_cast<std::uint32_t>(fn.namespace_path.size()));
    for (const std::string& segment : fn.namespace_path) write_string(out, segment);
    write_u8(out, fn.is_exported ? 1u : 0u);
    write_string(out, fn.owning_module);
}

[[nodiscard]] Function read_function(std::istream& in, const std::string& context) {
    Function fn;
    fn.return_type = read_type(in, context + " return type");
    fn.name = read_string(in, context + " name");
    fn.loc = read_source_location(in, context + " loc");
    std::uint32_t param_count = read_u32_le(in, context + " param count");
    fn.params.reserve(param_count);
    for (std::uint32_t i = 0; i < param_count; i++) fn.params.push_back(read_param(in, context + " param"));
    fn.return_lifetime.name = read_string(in, context + " return lifetime");
    if (read_u8(in, context + " body present") != 0u) fn.body = read_stmt(in, context + " body");
    fn.is_extern_c = read_u8(in, context + " extern_c") != 0u;
    fn.is_module_extern = read_u8(in, context + " module_extern") != 0u;
    fn.is_unsafe = read_u8(in, context + " unsafe") != 0u;
    fn.is_nodiscard = read_u8(in, context + " nodiscard") != 0u;
    fn.nodiscard_reason = read_string(in, context + " nodiscard reason");
    fn.is_compile_time_dependency = read_u8(in, context + " compile_time_dependency") != 0u;
    fn.eval_mode = read_enum<FunctionEvalMode>(in, context + " eval mode");
    fn.has_varargs = read_u8(in, context + " has_varargs") != 0u;
    fn.method_requires_concept = read_string(in, context + " method_requires_concept");
    fn.is_generic_template = read_u8(in, context + " is_generic_template") != 0u;
    std::uint32_t template_param_count = read_u32_le(in, context + " template param count");
    fn.template_params.reserve(template_param_count);
    for (std::uint32_t i = 0; i < template_param_count; i++) fn.template_params.push_back(read_generic_type_param(in, context + " template param"));
    fn.generic_method_owner_id = read_string(in, context + " generic method owner");
    fn.member_owner_class = read_string(in, context + " member owner class");
    std::uint32_t member_init_count = read_u32_le(in, context + " member initializer count");
    fn.member_initializers.reserve(member_init_count);
    for (std::uint32_t i = 0; i < member_init_count; i++) {
        fn.member_initializers.push_back(read_member_initializer(in, context + " member initializer"));
    }
    fn.receiver_ref_qualifier = read_enum<ReceiverRefQualifier>(in, context + " receiver ref qualifier");
    fn.is_static = read_u8(in, context + " is_static") != 0u;
    fn.access = read_enum<AccessSpecifier>(in, context + " access");
    fn.is_virtual = read_u8(in, context + " is_virtual") != 0u;
    fn.is_override = read_u8(in, context + " is_override") != 0u;
    fn.is_pure = read_u8(in, context + " is_pure") != 0u;
    fn.is_defaulted = read_u8(in, context + " is_defaulted") != 0u;
    fn.forwards_to = read_string(in, context + " forwards_to");
    std::uint32_t ns_count = read_u32_le(in, context + " namespace count");
    fn.namespace_path.reserve(ns_count);
    for (std::uint32_t i = 0; i < ns_count; i++) fn.namespace_path.push_back(read_string(in, context + " namespace"));
    fn.is_exported = read_u8(in, context + " is_exported") != 0u;
    fn.owning_module = read_string(in, context + " owning_module");
    return fn;
}

[[nodiscard]] bool types_equal_for_payload_merge(const Type& a, const Type& b) {
    if (a.kind != b.kind || a.name != b.name || a.array_size != b.array_size ||
        a.is_unsafe_function_pointer != b.is_unsafe_function_pointer || a.is_const_function != b.is_const_function ||
        a.function_ref_qualifier != b.function_ref_qualifier || a.is_mutable_ref != b.is_mutable_ref ||
        a.is_rvalue_ref != b.is_rvalue_ref || a.is_mutable_pointee != b.is_mutable_pointee ||
        a.is_pack_expansion != b.is_pack_expansion || a.template_args.size() != b.template_args.size() ||
        a.non_type_args.size() != b.non_type_args.size() || a.function_params.size() != b.function_params.size()) {
        return false;
    }
    auto ptr_equal = [&](const std::shared_ptr<Type>& lhs, const std::shared_ptr<Type>& rhs) {
        if (static_cast<bool>(lhs) != static_cast<bool>(rhs)) return false;
        return !lhs || types_equal_for_payload_merge(*lhs, *rhs);
    };
    if (!ptr_equal(a.pointee, b.pointee) || !ptr_equal(a.element, b.element) || !ptr_equal(a.function_return, b.function_return)) {
        return false;
    }
    for (std::size_t i = 0; i < a.template_args.size(); i++) if (!types_equal_for_payload_merge(a.template_args[i], b.template_args[i])) return false;
    for (std::size_t i = 0; i < a.function_params.size(); i++) if (!types_equal_for_payload_merge(a.function_params[i], b.function_params[i])) return false;
    for (std::size_t i = 0; i < a.non_type_args.size(); i++) {
        const auto& lhs = a.non_type_args[i];
        const auto& rhs = b.non_type_args[i];
        if (static_cast<bool>(lhs) != static_cast<bool>(rhs)) return false;
        if (!lhs) continue;
        if (lhs->kind != rhs->kind || lhs->int_value != rhs->int_value || lhs->name != rhs->name) return false;
    }
    return true;
}

[[nodiscard]] bool params_equal_for_payload_merge(const std::vector<Param>& a, const std::vector<Param>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        if (a[i].name != b[i].name || a[i].generic_concept != b[i].generic_concept ||
            a[i].require_thread_movable != b[i].require_thread_movable ||
            a[i].require_thread_shareable != b[i].require_thread_shareable ||
            a[i].is_parameter_pack != b[i].is_parameter_pack ||
            !types_equal_for_payload_merge(a[i].type, b[i].type)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool same_function_identity_for_payload_merge(const Function& a, const Function& b) {
    return a.name == b.name && types_equal_for_payload_merge(a.return_type, b.return_type) &&
           params_equal_for_payload_merge(a.params, b.params) && a.receiver_ref_qualifier == b.receiver_ref_qualifier &&
           a.is_nodiscard == b.is_nodiscard && a.nodiscard_reason == b.nodiscard_reason &&
           a.member_owner_class == b.member_owner_class && a.is_static == b.is_static && a.access == b.access &&
           a.is_virtual == b.is_virtual && a.is_override == b.is_override && a.is_pure == b.is_pure &&
           a.is_defaulted == b.is_defaulted;
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
        if (!types_equal_for_payload_merge(a[i], b[i])) return false;
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


struct GenericMethodOwnerRemap {
    std::string old_owner_id;
    std::string new_owner_id;
    std::string class_name;
};

[[nodiscard]] std::string rewrite_generic_method_name_for_owner(const Function& fn,
                                                                const GenericMethodOwnerRemap& remap) {
    std::string old_prefix = remap.class_name + "__" + remap.old_owner_id;
    std::string new_prefix = remap.class_name + "__" + remap.new_owner_id;
    if (fn.name.rfind(old_prefix, 0) == 0) return new_prefix + fn.name.substr(old_prefix.size());
    return fn.name;
}

[[nodiscard]] bool is_local_module_enum(const EnumDef& def) { return def.owning_module.empty(); }
[[nodiscard]] bool is_local_module_struct(const StructDef& def) { return def.owning_module.empty(); }
[[nodiscard]] bool is_local_module_class(const ClassDef& def) { return def.owning_module.empty(); }
[[nodiscard]] bool is_local_module_function(const Function& fn) { return fn.owning_module.empty(); }

[[nodiscard]] std::string serialize_compile_time_payload(const Program& program) {
    CompileTimePayloadPlan plan = plan_compile_time_payload(program);
    if (plan.root_function_names.empty()) return {};

    std::unordered_set<std::size_t> reachable_function_indices(plan.reachable_function_indices.begin(),
                                                          plan.reachable_function_indices.end());
    std::unordered_set<std::string> reachable_type_names(plan.reachable_type_names.begin(), plan.reachable_type_names.end());
    std::vector<const StructDef*> structs;
    std::vector<const ClassDef*> classes;
    std::vector<const EnumDef*> enums;
    std::vector<const Function*> functions;
    for (const EnumDef& def : program.enums) {
        if (is_local_module_enum(def) && reachable_type_names.contains(def.name)) {
            enums.push_back(&def);
        }
    }
    for (const StructDef& def : program.structs) {
        if (is_local_module_struct(def) && reachable_type_names.contains(def.name)) structs.push_back(&def);
    }
    for (const ClassDef& def : program.classes) {
        if (is_local_module_class(def) && reachable_type_names.contains(def.name)) classes.push_back(&def);
    }
    for (std::size_t i = 0; i < program.functions.size(); i++) {
        const Function& fn = program.functions[i];
        if (!is_local_module_function(fn)) continue;
        if (reachable_function_indices.contains(i)) functions.push_back(&fn);
    }

    std::ostringstream payload(std::ios::binary);
    payload.write(SCPPM_COMPILE_TIME_AST_MAGIC.data(), static_cast<std::streamsize>(SCPPM_COMPILE_TIME_AST_MAGIC.size()));
    write_u32_le(payload, SCPPM_COMPILE_TIME_AST_VERSION);
    write_u32_le(payload, static_cast<std::uint32_t>(plan.root_function_names.size()));
    for (const std::string& name : plan.root_function_names) write_string(payload, name);
    write_u32_le(payload, static_cast<std::uint32_t>(enums.size()));
    for (const EnumDef* def : enums) write_enum_def(payload, *def);
    write_u32_le(payload, static_cast<std::uint32_t>(structs.size()));
    for (const StructDef* def : structs) write_struct_def(payload, *def);
    write_u32_le(payload, static_cast<std::uint32_t>(classes.size()));
    for (const ClassDef* def : classes) write_class_def(payload, *def);
    write_u32_le(payload, static_cast<std::uint32_t>(functions.size()));
    for (const Function* fn : functions) write_function(payload, *fn);
    return payload.str();
}

[[nodiscard]] StructuredCompileTimePayload deserialize_compile_time_payload(std::string_view bytes, const std::string& path) {
    std::istringstream in(std::string(bytes), std::ios::binary);
    char magic[4] = {};
    in.read(magic, sizeof(magic));
    if (!in || std::string_view(magic, 4) != SCPPM_COMPILE_TIME_AST_MAGIC) {
        throw DriverError("invalid .scppm file '" + path + "': bad structured compile-time payload magic");
    }
    std::uint32_t version = read_u32_le(in, path + " payload version");
    if (version != SCPPM_COMPILE_TIME_AST_VERSION) {
        throw DriverError("unsupported structured compile-time payload version " + std::to_string(version) +
                          " in '" + path + "'");
    }
    StructuredCompileTimePayload payload;
    std::uint32_t root_count = read_u32_le(in, path + " root count");
    payload.root_function_names.reserve(root_count);
    for (std::uint32_t i = 0; i < root_count; i++) payload.root_function_names.push_back(read_string(in, path + " root"));
    std::uint32_t enum_count = read_u32_le(in, path + " enum count");
    payload.enums.reserve(enum_count);
    for (std::uint32_t i = 0; i < enum_count; i++) payload.enums.push_back(read_enum_def(in, path + " enum"));
    std::uint32_t struct_count = read_u32_le(in, path + " struct count");
    payload.structs.reserve(struct_count);
    for (std::uint32_t i = 0; i < struct_count; i++) payload.structs.push_back(read_struct_def(in, path + " struct"));
    std::uint32_t class_count = read_u32_le(in, path + " class count");
    payload.classes.reserve(class_count);
    for (std::uint32_t i = 0; i < class_count; i++) payload.classes.push_back(read_class_def(in, path + " class"));
    std::uint32_t function_count = read_u32_le(in, path + " function count");
    payload.functions.reserve(function_count);
    for (std::uint32_t i = 0; i < function_count; i++) payload.functions.push_back(read_function(in, path + " function"));
    return payload;
}

[[nodiscard]] bool program_requires_structured_payload(const Program& program) {
    CompileTimePayloadPlan plan = plan_compile_time_payload(program);
    return !plan.root_function_names.empty();
}

void mark_reachable_hidden_compile_time_dependencies(Program& program) {
    CompileTimePayloadPlan plan = plan_compile_time_payload(program);
    std::unordered_set<std::size_t> reachable_function_indices(plan.reachable_function_indices.begin(),
                                                          plan.reachable_function_indices.end());
    std::unordered_set<std::string> reachable_type_names(plan.reachable_type_names.begin(), plan.reachable_type_names.end());
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
    }
}

void merge_compile_time_payload(Program& imported, StructuredCompileTimePayload&& payload) {
    std::vector<GenericMethodOwnerRemap> owner_remaps;
    for (EnumDef& def : payload.enums) {
        if (!def.is_exported) def.is_compile_time_dependency = true;
        auto existing =
            std::find_if(imported.enums.begin(), imported.enums.end(), [&](const EnumDef& current) { return current.name == def.name; });
        if (existing != imported.enums.end()) {
            *existing = std::move(def);
        } else {
            imported.enums.push_back(std::move(def));
        }
    }
    for (StructDef& def : payload.structs) {
        if (!def.is_exported) def.is_compile_time_dependency = true;
        auto existing = std::find_if(imported.structs.begin(), imported.structs.end(),
                                     [&](const StructDef& current) { return same_struct_identity_for_payload_merge(current, def); });
        if (existing != imported.structs.end()) {
            *existing = std::move(def);
        } else {
            imported.structs.push_back(std::move(def));
        }
    }
    for (ClassDef& def : payload.classes) {
        if (!def.is_exported) def.is_compile_time_dependency = true;
        auto existing = std::find_if(imported.classes.begin(), imported.classes.end(),
                                     [&](const ClassDef& current) { return same_class_identity_for_payload_merge(current, def); });
        if (existing != imported.classes.end()) {
            if (!existing->template_owner_id.empty() && existing->template_owner_id != def.template_owner_id) {
                owner_remaps.push_back(GenericMethodOwnerRemap{existing->template_owner_id, def.template_owner_id, def.name});
            }
            *existing = std::move(def);
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
        auto existing = std::find_if(imported.functions.begin(), imported.functions.end(),
                                     [&](const Function& current) { return same_function_identity_for_payload_merge(current, fn); });
        if (existing != imported.functions.end()) {
            *existing = std::move(fn);
        } else {
            imported.functions.push_back(std::move(fn));
        }
    }
}

void write_scppm_file(const Program& program, std::string_view interface_source, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw DriverError("cannot write module interface '" + path + "'");
    }
    std::string payload = serialize_compile_time_payload(program);
    unsigned char flags = payload.empty() ? 0u : 0x01u;
    const std::array<char, 8> header = {'S', 'C', 'P', 'P', 'M', 1, 0, static_cast<char>(flags)};
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    write_u32_le(out, static_cast<std::uint32_t>(interface_source.size()));
    out.write(interface_source.data(), static_cast<std::streamsize>(interface_source.size()));
    if ((flags & 0x01u) != 0u) {
        write_u32_le(out, static_cast<std::uint32_t>(payload.size()));
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }
    if (!out) {
        throw DriverError("failed while writing module interface '" + path + "'");
    }
}

LoadedModuleFile read_module_file(const std::string& path) {
    LoadedModuleFile loaded;
    std::filesystem::path file_path(path);
    if (file_path.extension() != ".scppm") {
        std::ifstream file(path);
        if (!file) throw DriverError("cannot open imported module source '" + path + "'");
        std::ostringstream buffer;
        buffer << file.rdbuf();
        loaded.interface_source = buffer.str();
        return loaded;
    }

    loaded.is_scppm = true;
    std::ifstream file(path, std::ios::binary);
    if (!file) throw DriverError("cannot open imported module interface '" + path + "'");
    char header[8];
    file.read(header, sizeof(header));
    if (file.gcount() != static_cast<std::streamsize>(sizeof(header))) {
        throw DriverError("invalid .scppm file '" + path + "': truncated header");
    }
    if (std::memcmp(header, "SCPPM", 5) != 0) {
        throw DriverError("invalid .scppm file '" + path + "': bad magic");
    }
    unsigned char major_version = static_cast<unsigned char>(header[5]);
    if (major_version != 1) {
        throw DriverError("unsupported .scppm major version " + std::to_string(major_version) + " in '" + path + "'");
    }
    unsigned char flags = static_cast<unsigned char>(header[7]);
    std::uint32_t interface_length = read_u32_le(file, path + " interface length");
    loaded.interface_source.resize(interface_length);
    file.read(loaded.interface_source.data(), static_cast<std::streamsize>(interface_length));
    if (!file) throw DriverError("invalid .scppm file '" + path + "': truncated interface source");
    if ((flags & 0x01u) != 0u) {
        loaded.has_compile_time_payload = true;
        std::uint32_t payload_length = read_u32_le(file, path + " payload length");
        loaded.compile_time_payload_bytes.resize(payload_length);
        file.read(loaded.compile_time_payload_bytes.data(), static_cast<std::streamsize>(payload_length));
        if (!file) throw DriverError("invalid .scppm file '" + path + "': truncated structured payload");
    }
    return loaded;
}

void create_archive(const std::vector<std::string>& object_paths, const std::string& archive_path) {
    if (object_paths.empty()) {
        throw DriverError("archive command requires at least one object file for '" + archive_path + "'");
    }
    std::string command = "ar rcs \"" + archive_path + "\"";
    for (const std::string& object_path : object_paths) {
        command += " \"" + object_path + "\"";
    }
    int result = std::system(command.c_str());
    if (result != 0) {
        throw DriverError("archive command failed: " + command);
    }
}

[[nodiscard]] std::optional<std::filesystem::path> current_executable_path() {
    std::error_code ec;
    std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return std::nullopt;
    return path;
}

[[nodiscard]] std::optional<std::filesystem::path> runtime_default_prebuilt_stdlib_dir() {
    std::optional<std::filesystem::path> exe = current_executable_path();
    if (!exe.has_value()) return std::nullopt;
    return (exe->parent_path() / "libs").lexically_normal();
}

[[nodiscard]] std::optional<std::filesystem::path> runtime_installed_stdlib_dir() {
    std::optional<std::filesystem::path> exe = current_executable_path();
    if (!exe.has_value()) return std::nullopt;
    return (exe->parent_path() / ".." / "share" / "scpp" / "libs").lexically_normal();
}

[[nodiscard]] std::optional<std::filesystem::path> runtime_default_source_stdlib_dir() {
    std::optional<std::filesystem::path> exe = current_executable_path();
    if (!exe.has_value()) return std::nullopt;
    return (exe->parent_path() / ".." / "libs").lexically_normal();
}

[[nodiscard]] std::vector<std::string> build_default_import_search_dirs(const std::vector<std::string>& explicit_dirs) {
    std::vector<std::string> dirs = explicit_dirs;
    auto append_if_missing = [&](std::string path) {
        if (path.empty()) return;
        if (std::find(dirs.begin(), dirs.end(), path) == dirs.end()) dirs.push_back(std::move(path));
    };
    auto append_module_dirs = [&](const std::filesystem::path& base) {
        append_if_missing(base.string());
        append_if_missing((base / "std").string());
        append_if_missing((base / "scpp").string());
    };
    if (const char* env = std::getenv("SCPP_STDLIB_PATH"); env != nullptr && env[0] != '\0') {
        append_module_dirs(env);
    } else {
        if (std::optional<std::filesystem::path> runtime_dir = runtime_default_prebuilt_stdlib_dir(); runtime_dir.has_value()) {
            append_module_dirs(*runtime_dir);
        }
        if (std::optional<std::filesystem::path> runtime_dir = runtime_installed_stdlib_dir(); runtime_dir.has_value()) {
            append_module_dirs(*runtime_dir);
        }
        if (std::optional<std::filesystem::path> runtime_dir = runtime_default_source_stdlib_dir(); runtime_dir.has_value()) {
            append_module_dirs(*runtime_dir);
        }
    }
    return dirs;
}

[[nodiscard]] std::vector<std::string> default_stdlib_link_inputs() {
    std::vector<std::string> result;
    auto append_if_exists = [&](const std::filesystem::path& lib_path) {
        if (!std::filesystem::exists(lib_path)) return;
        std::string path = lib_path.string();
        if (std::find(result.begin(), result.end(), path) == result.end()) {
            result.push_back(std::move(path));
        }
    };
    std::vector<std::optional<std::filesystem::path>> candidate_dirs = {
        runtime_default_prebuilt_stdlib_dir(),
        runtime_installed_stdlib_dir(),
    };
    for (const std::optional<std::filesystem::path>& lib_dir : candidate_dirs) {
        if (!lib_dir.has_value()) continue;
        append_if_exists(*lib_dir / "libstd.scppa");
        append_if_exists(*lib_dir / "libscpp.scppa");
    }
    return result;
}

[[nodiscard]] std::string absolute_source_path(const std::string& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec) return path;
    return absolute.lexically_normal().string();
}

[[nodiscard]] std::vector<std::size_t> line_offsets(std::string_view source) {
    std::vector<std::size_t> offsets = {0};
    for (std::size_t i = 0; i < source.size(); i++) {
        if (source[i] == '\n') offsets.push_back(i + 1);
    }
    return offsets;
}

[[nodiscard]] std::size_t offset_for_loc(std::string_view source, const SourceLocation& loc) {
    std::vector<std::size_t> offsets = line_offsets(source);
    std::size_t line_index = static_cast<std::size_t>(std::max(loc.line, 1) - 1);
    if (line_index >= offsets.size()) return source.size();
    return std::min(offsets[line_index] + static_cast<std::size_t>(std::max(loc.column, 1) - 1), source.size());
}

[[nodiscard]] std::size_t find_matching_brace(std::string_view source, std::size_t open_offset) {
    bool in_string = false;
    bool in_char = false;
    bool in_line_comment = false;
    bool in_block_comment = false;
    int depth = 0;
    for (std::size_t i = open_offset; i < source.size(); i++) {
        char c = source[i];
        char next = i + 1 < source.size() ? source[i + 1] : '\0';
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
    throw DriverError("failed to locate end of function body while writing module interface");
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
        char c = source[i];
        char next = i + 1 < source.size() ? source[i + 1] : '\0';
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

std::string strip_concrete_function_bodies(const Program& program, const std::string& file_path, std::string source) {
    struct BodyRange {
        std::size_t begin;
        std::size_t end;
        std::string replacement;
    };
    std::vector<BodyRange> ranges;
    for (const Function& fn : program.functions) {
        if (!fn.body || !fn.loc.has_source_path()) continue;
        if (absolute_source_path(fn.loc.source_path_text()) != file_path) continue;
        std::size_t begin = offset_for_loc(source, fn.body->loc);
        if (begin >= source.size() || source[begin] != '{') continue;
        if (!fn.member_initializers.empty()) {
            std::size_t signature_begin = offset_for_loc(source, fn.loc);
            if (auto colon = find_constructor_member_initializer_colon(source, signature_begin, begin)) {
                ranges.push_back(BodyRange{*colon, begin, ""});
            }
        }
        std::size_t end = find_matching_brace(source, begin);
        ranges.push_back(BodyRange{begin, end + 1, ";"});
    }
    std::sort(ranges.begin(), ranges.end(), [](const BodyRange& a, const BodyRange& b) { return a.begin > b.begin; });
    for (const BodyRange& range : ranges) {
        source.replace(range.begin, range.end - range.begin, range.replacement);
    }
    return source;
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
class ModuleCache {
public:
    explicit ModuleCache(std::unordered_map<std::string, std::string> import_paths,
                         std::vector<std::string> import_search_dirs = {})
        : import_paths_(std::move(import_paths)),
          import_search_dirs_(build_default_import_search_dirs(import_search_dirs)) {}

    const Program& resolve(const std::string& module_name) {
        auto cached = cache_.find(module_name);
        if (cached != cache_.end()) return cached->second;

        if (resolving_.contains(module_name)) {
            throw DriverError("circular import detected: module '" + module_name +
                               "' (directly or transitively) imports itself");
        }
        auto path_it = import_paths_.find(module_name);
        if (path_it == import_paths_.end()) {
            std::optional<std::string> inferred = infer_module_path(module_name);
            if (inferred.has_value()) {
                path_it = import_paths_.emplace(module_name, *inferred).first;
            } else {
                throw DriverError("cannot find module '" + module_name + "' (use --import " + module_name +
                                   "=path/to/file or -I <dir>)");
            }
        }

        resolving_.insert(module_name);
        std::string resolved_path = absolute_source_path(path_it->second);
        LoadedModuleFile loaded = read_module_file(resolved_path);
        // Stamps every SourceLocation this parse() produces (see
        // ParseError's and parse_primary()'s own comments) with
        // path_it->second -- the path exactly as given via `--import
        // name=path` (or as inferred by -I search) -- rather than
        // `resolved_path`, so a diagnostic rooted in this file prints
        // that same as-given spelling instead of silently rewriting it
        // to an absolute path (cli.cppm's print_diagnostic, and this
        // file's own entry-point parse() call below, agree on this
        // convention). `resolved_path` remains the absolute form used
        // for internal bookkeeping below (cache dedup, module-object
        // naming, archive lookup) where deduplicating equivalent paths
        // genuinely matters.
        Program imported = parse(
            loaded.interface_source, [this](const std::string& name) -> const Program& { return resolve(name); },
            [this](const std::string& key) -> Program { return resolve_partition(key); }, path_it->second);
        imported.source_path = resolved_path;
        if (loaded.has_compile_time_payload) {
            merge_compile_time_payload(imported,
                                       deserialize_compile_time_payload(loaded.compile_time_payload_bytes, resolved_path));
        } else if (!loaded.is_scppm) {
            mark_reachable_hidden_compile_time_dependencies(imported);
        } else if (loaded.is_scppm && program_requires_structured_payload(imported)) {
            throw DriverError("module interface '" + resolved_path +
                              "' lacks the required structured compile-time payload; rebuild it with a newer scpp "
                              "'build-module' output");
        }
        resolving_.erase(module_name);

        if (imported.module_name != module_name) {
            throw DriverError("'" + path_it->second + "' does not declare module '" + module_name +
                               "' (its own module declaration names '" +
                               (imported.module_name.empty() ? std::string("<none>") : imported.module_name) +
                               "')");
        }

        resolution_order_.push_back(module_name);
        resolved_paths_[module_name] = resolved_path;
        auto [it, inserted] = cache_.emplace(module_name, std::move(imported));
        return it->second;
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
    Program resolve_partition(const std::string& key) {
        if (partitions_resolving_.contains(key)) {
            throw DriverError("circular partition import detected: '" + key +
                               "' (directly or transitively) imports itself");
        }
        auto path_it = import_paths_.find(key);
        if (path_it == import_paths_.end()) {
            std::optional<std::string> inferred = infer_partition_path(key);
            if (inferred.has_value()) {
                path_it = import_paths_.emplace(key, *inferred).first;
            } else {
                throw DriverError("cannot find partition '" + key + "' (use --import " + key +
                                   "=path/to/file or import its parent module via -I <dir>)");
            }
        }

        partitions_resolving_.insert(key);
        LoadedModuleFile loaded = read_module_file(path_it->second);
        if (loaded.is_scppm) {
            throw DriverError("partition import path '" + path_it->second +
                              "' must use a source .scpp file, not a compiled .scppm artifact");
        }
        // path_it->second (not an absolute_source_path()-normalized
        // form) so this partition's own SourceLocations preserve
        // whatever spelling resolved it -- see resolve()'s matching
        // comment above; partition.source_path just below is still the
        // absolute form internal bookkeeping (e.g. resolved_paths_) can
        // rely on.
        Program partition = parse(
            loaded.interface_source, [this](const std::string& name) -> const Program& { return resolve(name); },
            [this](const std::string& nested_key) -> Program { return resolve_partition(nested_key); },
            path_it->second);
        partition.source_path = absolute_source_path(path_it->second);
        partitions_resolving_.erase(key);

        std::string expected_key = partition.module_name + ":" + partition.partition_name;
        if (expected_key != key) {
            throw DriverError("'" + path_it->second + "' does not declare partition '" + key +
                               "' (its own module declaration names '" + expected_key + "')");
        }
        return partition;
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
        std::string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); i++) {
            char ch = text[i];
            if (ch == '\\' && i + 1 < text.size()) {
                char next = text[++i];
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
    [[nodiscard]] static std::optional<std::string> archive_from_metadata(const std::filesystem::path& metadata_path,
                                                                           const std::string& module_name) {
        if (!std::filesystem::exists(metadata_path)) return std::nullopt;
        std::ifstream file(metadata_path);
        if (!file) return std::nullopt;
        std::string line;
        const std::string name_needle = "\"name\": \"" + module_name + "\"";
        const std::string archive_needle = "\"archive\": \"";
        while (std::getline(file, line)) {
            if (line.find(name_needle) == std::string::npos) continue;
            std::size_t archive_pos = line.find(archive_needle);
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
        std::filesystem::path interface_path(path_it->second);
        if (interface_path.extension() != ".scppm") return std::nullopt;
        std::vector<std::filesystem::path> candidates = {
            interface_path.parent_path() / ("lib" + module_name + ".scppa"),
        };
        if (interface_path.parent_path().filename() == "modules") {
            candidates.push_back(interface_path.parent_path().parent_path() / "archives" /
                                 ("lib" + module_name + ".scppa"));
            if (std::optional<std::string> metadata_archive =
                    archive_from_metadata(interface_path.parent_path().parent_path() / "package-metadata.json",
                                          module_name);
                metadata_archive.has_value()) {
                candidates.push_back(*metadata_archive);
            }
        }
        for (const std::filesystem::path& archive_path : candidates) {
            if (std::filesystem::exists(archive_path)) return archive_path.string();
        }
        return std::nullopt;
    }

private:
    [[nodiscard]] std::optional<std::string> infer_module_path(const std::string& module_name) const {
        for (const std::string& dir : import_search_dirs_) {
            std::filesystem::path base(dir);
            std::filesystem::path interface_candidate = base / (module_name + ".scppm");
            if (std::filesystem::exists(interface_candidate)) return interface_candidate.string();
            std::filesystem::path source_candidate = base / (module_name + ".scpp");
            if (std::filesystem::exists(source_candidate)) return source_candidate.string();
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> infer_partition_path(const std::string& key) const {
        std::size_t colon = key.find(':');
        if (colon == std::string::npos) return std::nullopt;
        std::string module_name = key.substr(0, colon);
        std::string partition_name = key.substr(colon + 1);
        auto module_it = import_paths_.find(module_name);
        if (module_it == import_paths_.end()) return std::nullopt;
        std::filesystem::path module_path(module_it->second);
        std::filesystem::path candidate =
            module_path.parent_path() / partition_name /
            (module_path.stem().string() + "_" + partition_name + module_path.extension().string());
        if (!std::filesystem::exists(candidate)) return std::nullopt;
        return candidate.string();
    }

    std::unordered_map<std::string, std::string> import_paths_;
    std::vector<std::string> import_search_dirs_;
    std::unordered_map<std::string, Program> cache_;
    std::unordered_map<std::string, std::string> resolved_paths_;
    std::unordered_set<std::string> resolving_;
    std::unordered_set<std::string> partitions_resolving_;
    std::vector<std::string> resolution_order_;
};

[[nodiscard]] std::string trim_copy(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) begin++;
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) end--;
    return std::string(text.substr(begin, end - begin));
}

[[nodiscard]] bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] std::string partition_path_from_primary(const std::string& module_source_path, const std::string& partition_name) {
    std::filesystem::path module_path(module_source_path);
    std::filesystem::path candidate =
        module_path.parent_path() / partition_name /
        (module_path.stem().string() + "_" + partition_name + module_path.extension().string());
    return candidate.string();
}

[[nodiscard]] bool is_partition_import_line(std::string_view trimmed) {
    return starts_with(trimmed, "export import :") || starts_with(trimmed, "import :");
}

[[nodiscard]] bool is_non_partition_import_line(std::string_view trimmed) {
    if (is_partition_import_line(trimmed)) return false;
    return starts_with(trimmed, "export import ") || starts_with(trimmed, "import ");
}

std::string render_module_interface_file(const Program& program, const std::string& file_path,
                                         const std::string& module_source_path, bool keep_concrete_bodies,
                                         bool keep_module_declaration,
                                         std::unordered_set<std::string>& expanded_partition_paths);

std::string inline_partition_imports(const Program& program, const std::string& module_source_path, std::string_view source,
                                     bool keep_concrete_bodies, std::unordered_set<std::string>& expanded_partition_paths) {
    std::ostringstream out;
    std::size_t line_start = 0;
    while (line_start <= source.size()) {
        std::size_t line_end = source.find('\n', line_start);
        bool had_newline = line_end != std::string_view::npos;
        std::string_view line =
            had_newline ? source.substr(line_start, line_end - line_start) : source.substr(line_start);
        std::string trimmed = trim_copy(line);
        if (is_partition_import_line(trimmed)) {
            std::size_t colon = trimmed.find(':');
            std::size_t semi = trimmed.find(';', colon);
            std::string partition_name = trimmed.substr(colon + 1, semi == std::string::npos ? std::string::npos
                                                                                            : semi - (colon + 1));
            std::string partition_path = absolute_source_path(partition_path_from_primary(module_source_path, partition_name));
            if (!std::filesystem::exists(partition_path)) {
                throw DriverError("cannot find partition '" + program.module_name + ":" + partition_name +
                                  "' while writing module interface artifacts");
            }
            if (expanded_partition_paths.insert(partition_path).second) {
                out << render_module_interface_file(program, partition_path, module_source_path, keep_concrete_bodies,
                                                    /*keep_module_declaration=*/false, expanded_partition_paths);
            }
        } else {
            out << std::string(line);
            if (had_newline) out << '\n';
        }
        if (!had_newline) break;
        line_start = line_end + 1;
    }
    return out.str();
}

std::string render_module_interface_file(const Program& program, const std::string& file_path,
                                         const std::string& module_source_path, bool keep_concrete_bodies,
                                         bool keep_module_declaration,
                                         std::unordered_set<std::string>& expanded_partition_paths) {
    LoadedModuleFile loaded = read_module_file(file_path);
    std::string source = std::move(loaded.interface_source);
    if (!keep_concrete_bodies) {
        source = strip_concrete_function_bodies(program, absolute_source_path(file_path), std::move(source));
    }

    std::ostringstream out;
    std::size_t line_start = 0;
    while (line_start <= source.size()) {
        std::size_t line_end = source.find('\n', line_start);
        bool had_newline = line_end != std::string::npos;
        std::string_view line =
            had_newline ? std::string_view(source).substr(line_start, line_end - line_start)
                        : std::string_view(source).substr(line_start);
        std::string trimmed = trim_copy(line);
        bool is_module_decl = starts_with(trimmed, "export module ") || starts_with(trimmed, "module ");
        if (is_module_decl) {
            if (keep_module_declaration) {
                out << std::string(line);
                if (had_newline) out << '\n';
            }
        } else {
            out << inline_partition_imports(program, module_source_path, line, keep_concrete_bodies, expanded_partition_paths);
            if (had_newline) out << '\n';
        }
        if (!had_newline) break;
        line_start = line_end + 1;
    }
    return out.str();
}

std::string hoist_non_partition_imports(std::string source) {
    std::vector<std::string> imports;
    std::unordered_set<std::string> seen_imports;
    std::vector<std::string> body_lines;
    std::string module_line;
    bool module_line_set = false;

    std::size_t line_start = 0;
    while (line_start <= source.size()) {
        std::size_t line_end = source.find('\n', line_start);
        bool had_newline = line_end != std::string::npos;
        std::string_view line =
            had_newline ? std::string_view(source).substr(line_start, line_end - line_start)
                        : std::string_view(source).substr(line_start);
        std::string line_text(line);
        std::string trimmed = trim_copy(line);
        bool is_module_decl = starts_with(trimmed, "export module ") || starts_with(trimmed, "module ");
        if (is_module_decl && !module_line_set) {
            module_line = line_text;
            module_line_set = true;
        } else if (is_non_partition_import_line(trimmed)) {
            if (seen_imports.insert(trimmed).second) imports.push_back(line_text);
        } else {
            body_lines.push_back(line_text);
        }
        if (!had_newline) break;
        line_start = line_end + 1;
    }

    std::ostringstream out;
    if (module_line_set) out << module_line << '\n';
    if (!imports.empty()) {
        out << '\n';
        for (const std::string& import_line : imports) out << import_line << '\n';
    }
    for (std::size_t i = 0; i < body_lines.size(); i++) {
        if ((module_line_set || !imports.empty()) || i > 0) out << '\n';
        out << body_lines[i];
    }
    return out.str();
}

std::string build_merged_interface_source(const Program& program, const std::string& module_source_path,
                                          bool keep_concrete_bodies) {
    std::unordered_set<std::string> expanded_partition_paths;
    return hoist_non_partition_imports(render_module_interface_file(program, module_source_path, module_source_path,
                                                                    keep_concrete_bodies,
                                                                    /*keep_module_declaration=*/true,
                                                                    expanded_partition_paths));
}

LLVMCodeGenOptLevel codegen_opt_level_for(int opt_level) {
    if (opt_level <= 0) return LLVMCodeGenLevelNone;
    if (opt_level == 1) return LLVMCodeGenLevelLess;
    if (opt_level == 2) return LLVMCodeGenLevelDefault;
    return LLVMCodeGenLevelAggressive;
}

// Move-checks an already-parsed (and, if it has imports of its own,
// already import-merged -- see scpp.parser's merge_imported_module)
// Program and lowers it to a native object file at `object_path`. Shared
// by emit_object_file (the main source) and, once per resolved module,
// by compile_to_executable below -- exactly the same backend either way,
// since by this point a Program is just a Program regardless of which
// file it came from.
void emit_object_file_for_program(Program& program, const std::string& object_path, bool emit_debug_info = false,
                                  int opt_level = 2) {
    reject_not_yet_lowerable_constexpr_surface(program);
    // ch05 §5.11: must run before check_moves -- see Monomorphizer's own
    // comment in movecheck.cppm for why call-site monomorphization has
    // to happen first (movecheck's ordinary exact-type-match call-
    // argument checking can only work once every call site targets an
    // already-concrete function).
    monomorphize_generics(program);
    try {
        fold_immediate_calls(program);
    } catch (const ConstexprError& error) {
        throw DriverError(error.what());
    }
    try {
        check_moves(program);

        // Real llvm-c/Target.h's own LLVMInitializeNativeTarget/
        // LLVMInitializeNativeAsmPrinter are header-only `static inline`
        // functions with no real, exported ABI symbol to declare/link
        // against -- the `llvm` module's own `:target` partition's
        // scpp_llvm_target_initialize_* bridge calls through to them via
        // a small shim compiled with the real header available; see
        // libs/llvm/target.cpp's and libs/llvm/native_target_init.cpp's
        // own header comments.
        scpp_llvm_target_initialize_native_target();
        scpp_llvm_target_initialize_native_asm_printer();

        char* default_triple_c = LLVMGetDefaultTargetTriple();
        std::string triple = default_triple_c;
        LLVMDisposeMessage(default_triple_c);

        LLVMTargetRef target = nullptr;
        char* lookup_error_c = nullptr;
        if (LLVMGetTargetFromTriple(triple.c_str(), &target, &lookup_error_c)) {
            std::string lookup_error = lookup_error_c != nullptr ? lookup_error_c : "";
            LLVMDisposeMessage(lookup_error_c);
            throw DriverError("failed to lookup target '" + triple + "': " + lookup_error);
        }

        // A std::unique_ptr with LLVMDisposeTargetMachine as its deleter
        // gives target_machine the exact same "always freed, even if an
        // exception unwinds through codegen.generate() below" exception
        // safety the original llvm::TargetMachine unique_ptr had, without
        // needing a bespoke RAII wrapper type. std::remove_pointer_t
        // recovers the pointee type from the exported LLVMTargetMachineRef
        // alias rather than naming the opaque LLVMOpaqueTargetMachine
        // struct tag directly: that tag is declared in the `llvm` module's
        // own `:target_machine` partition, in that partition's module
        // purview but never exported (see target_machine.cpp's own header
        // comment), so it is reachable through the alias but not nameable
        // by ordinary unqualified lookup here.
        std::unique_ptr<std::remove_pointer_t<LLVMTargetMachineRef>, void (*)(LLVMTargetMachineRef)> target_machine(
            LLVMCreateTargetMachine(target, triple.c_str(), "generic", "", codegen_opt_level_for(opt_level),
                                     LLVMRelocPIC, LLVMCodeModelDefault),
            &LLVMDisposeTargetMachine);
        if (!target_machine) {
            throw DriverError("failed to create target machine for '" + triple + "'");
        }

        // The data layout must be set *before* codegen runs: std::make_unique
        // needs a target-accurate sizeof(T) to call malloc with, which comes
        // from the module's DataLayout.
        Codegen codegen("scpp_module", program.source_path, emit_debug_info);
        LLVMTargetDataRef target_data = LLVMCreateTargetDataLayout(target_machine.get());
        char* data_layout_c = LLVMCopyStringRepOfTargetData(target_data);
        codegen.set_target(triple, data_layout_c);
        LLVMDisposeMessage(data_layout_c);
        LLVMDisposeTargetData(target_data);

        LLVMModuleRef module = codegen.generate(program);

        char* emit_error_c = nullptr;
        if (LLVMTargetMachineEmitToFile(target_machine.get(), module, object_path.c_str(), LLVMObjectFile,
                                         &emit_error_c)) {
            std::string emit_error = emit_error_c != nullptr ? emit_error_c : "unknown error";
            LLVMDisposeMessage(emit_error_c);
            throw DriverError("could not emit object file '" + object_path + "': " + emit_error);
        }
    } catch (const ConstexprError& error) {
        throw DriverError(error.what());
    }
}

void emit_module_archive_for_program(Program& program, const std::string& archive_path, int opt_level = 2) {
    std::filesystem::path object_path(archive_path);
    object_path.replace_extension(".scppo");
    emit_object_file_for_program(program, object_path.string(), /*emit_debug_info=*/false, opt_level);
    try {
        create_archive({object_path.string()}, archive_path);
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(object_path, ec);
        throw;
    }
    std::error_code ec;
    std::filesystem::remove(object_path, ec);
}

} // namespace scpp

export namespace scpp {

std::string host_target_triple() {
    char* triple_c = LLVMGetDefaultTargetTriple();
    std::string triple = triple_c;
    LLVMDisposeMessage(triple_c);
    return triple;
}

std::vector<std::string> project_default_stdlib_link_inputs() { return default_stdlib_link_inputs(); }

std::optional<std::filesystem::path> driver_runtime_current_executable_path() { return current_executable_path(); }

std::optional<std::filesystem::path> driver_runtime_default_prebuilt_stdlib_dir() {
    return scpp::runtime_default_prebuilt_stdlib_dir();
}

std::optional<std::filesystem::path> driver_runtime_installed_stdlib_dir() { return scpp::runtime_installed_stdlib_dir(); }

std::optional<std::filesystem::path> driver_runtime_default_source_stdlib_dir() {
    return scpp::runtime_default_source_stdlib_dir();
}

// Compiles scpp source text down to a native object file at `object_path`.
// This is the M1/M2/M3 backend: AST -> [move check] -> LLVM IR -> native
// object code. `import_paths` (ch11 §11.7, `--import name=path`) resolves
// any `import name;` declarations `source` itself has; empty by default
// (no imports -- the overwhelmingly common case, every file before this
// chapter). Only `source`'s own object file is produced here -- an
// imported module's *own* separate object file is compile_to_executable's
// job below, since deciding where to put it needs an executable-level
// path to derive from.
void emit_object_file(std::string_view source, const std::string& object_path,
                       const std::unordered_map<std::string, std::string>& import_paths = {},
                       const std::vector<std::string>& import_search_dirs = {},
                       bool emit_debug_info = false,
                       const std::string& source_path = {},
                       int opt_level = 2) {
    ModuleCache cache(import_paths, import_search_dirs);
    Program program = parse(
        source, [&cache](const std::string& name) -> const Program& { return cache.resolve(name); },
        [&cache](const std::string& key) -> Program { return cache.resolve_partition(key); }, source_path);
    program.source_path = source_path.empty() ? std::string() : absolute_source_path(source_path);
    emit_object_file_for_program(program, object_path, emit_debug_info, opt_level);
}

void emit_module_artifacts(std::string_view source, const std::string& interface_path, const std::string& archive_path,
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
    ModuleCache cache(std::move(effective_import_paths), import_search_dirs);
    Program program = parse(
        source, [&cache](const std::string& name) -> const Program& { return cache.resolve(name); },
        [&cache](const std::string& key) -> Program { return cache.resolve_partition(key); }, source_path);
    program.source_path = source_path.empty() ? std::string() : absolute_source_path(source_path);
    reject_not_yet_lowerable_constexpr_surface(program);
    if (!program.is_module_interface) {
        throw DriverError("module artifacts can only be emitted from an interface unit, not '" +
                          (program.module_name.empty() ? std::string("<non-module>") : module_key(program)) + "'");
    }
    std::string merged_interface_source =
        build_merged_interface_source(program, absolute_source_path(source_path), /*keep_concrete_bodies=*/false);
    write_scppm_file(program, merged_interface_source, interface_path);
    emit_module_archive_for_program(program, archive_path, opt_level);
}

void archive_objects(const std::vector<std::string>& object_paths, const std::string& archive_path) {
    create_archive(object_paths, archive_path);
}

// Links a native object file into an executable using the system compiler
// driver (clang/cc); this keeps us out of the business of re-implementing a
// platform linker for M1. `extra_link_inputs` is appended verbatim after the
// scpp object file -- additional .o/.a paths (e.g. manifest-built native
// helper objects/archives, see libs/README.md, or another module's own
// compiled object file, see compile_to_executable below) or
// `-lname`/`-Lpath` flags a caller wants forwarded straight to the linker;
// empty by default (an ordinary, no-C++-interop build needs none of this).
void link_executable(const std::vector<std::string>& link_inputs, const std::string& executable_path,
                     bool static_link = false) {
    if (link_inputs.empty()) {
        throw DriverError("linker command requires at least one input for '" + executable_path + "'");
    }
    std::string command = "cc";
    if (static_link) command += " -static";
    for (const std::string& input : link_inputs) {
        command += " \"" + input + "\"";
    }
    // A wrapper library that itself calls into real C++ (e.g. std::string)
    // needs libstdc++'s runtime linked in too; `cc` alone (plain C mode)
    // doesn't pull that in automatically the way `c++`/`clang++` would.
    // Only added when there's an actual C++ wrapper to support, so a plain
    // scpp-only build's link command is unaffected.
    if (!link_inputs.empty()) command += " -lstdc++";
    command += " -o \"" + executable_path + "\"";
    int result = std::system(command.c_str());
    if (result != 0) {
        throw DriverError("linker command failed: " + command);
    }
}

// `import_paths` (ch11 §11.7, `--import name=path`) resolves any
// `import name;` declarations `source` has. When an imported module came
// from a prebuilt `.scppm` and its companion `.scppa` archive exists,
// that archive is linked directly; otherwise the imported Program is
// separately compiled to its own object file. This matches ch11
// §11.13/§11.14's intended "prefer compiled artifacts, fall back to
// source/interface compilation" model while still letting generic
// instantiations materialize in the importing file's own object.
void compile_to_executable(std::string_view source, const std::string& executable_path,
                            const std::vector<std::string>& extra_link_inputs = {},
                            const std::unordered_map<std::string, std::string>& import_paths = {},
                            bool static_link = false,
                            const std::vector<std::string>& import_search_dirs = {},
                            bool emit_debug_info = false,
                            const std::string& source_path = {},
                            int opt_level = 2) {
    ModuleCache cache(import_paths, import_search_dirs);
    Program program = parse(
        source, [&cache](const std::string& name) -> const Program& { return cache.resolve(name); },
        [&cache](const std::string& key) -> Program { return cache.resolve_partition(key); }, source_path);
    program.source_path = source_path.empty() ? std::string() : absolute_source_path(source_path);

    std::string object_path = executable_path + ".o";
    emit_object_file_for_program(program, object_path, emit_debug_info, opt_level);

    std::vector<std::string> module_object_paths;
    std::vector<std::string> module_archive_paths;
    for (const std::string& module_name : cache.resolution_order()) {
        if (std::optional<std::string> archive_path = cache.archive_for(module_name); archive_path.has_value()) {
            module_archive_paths.push_back(*archive_path);
            continue;
        }
        std::string module_object_path = executable_path + "." + module_name + ".o";
        emit_object_file_for_program(cache.program_for(module_name), module_object_path, emit_debug_info, opt_level);
        module_object_paths.push_back(module_object_path);
    }

    // Each separately-emitted module object is placed before any archive
    // inputs. Prebuilt module archives are then added in reverse
    // dependency order (importer before imported dependency), which keeps
    // a conventional left-to-right static linker able to satisfy one
    // archive's references from a later one.
    std::vector<std::string> link_inputs = module_object_paths;
    for (auto it = module_archive_paths.rbegin(); it != module_archive_paths.rend(); ++it) {
        if (std::find(link_inputs.begin(), link_inputs.end(), *it) == link_inputs.end()) {
            link_inputs.push_back(*it);
        }
    }
    link_inputs.insert(link_inputs.end(), extra_link_inputs.begin(), extra_link_inputs.end());
    bool uses_stdlib = std::find(cache.resolution_order().begin(), cache.resolution_order().end(), "std") !=
                       cache.resolution_order().end();
    if (uses_stdlib) {
        for (const std::string& input : default_stdlib_link_inputs()) {
            if (std::find(link_inputs.begin(), link_inputs.end(), input) == link_inputs.end()) {
                link_inputs.push_back(input);
            }
        }
    }
    std::vector<std::string> final_link_inputs = {object_path};
    final_link_inputs.insert(final_link_inputs.end(), link_inputs.begin(), link_inputs.end());
    link_executable(final_link_inputs, executable_path, static_link);

    std::error_code final_cleanup_ec;
    std::filesystem::remove(object_path, final_cleanup_ec);
    for (const std::string& module_object_path : module_object_paths) {
        std::filesystem::remove(module_object_path, final_cleanup_ec);
    }
}

} // namespace scpp
