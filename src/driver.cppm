module;

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>

export module scpp.driver;

import scpp.ast;
import scpp.codegen;
import scpp.movecheck;
import scpp.parser;

export namespace scpp {

struct DriverError : std::runtime_error {
    explicit DriverError(const std::string& message) : std::runtime_error(message) {}
};

} // namespace scpp

// Module-private helpers (ch11 §11.7/§11.8/§11.13): resolving `import
// name;` declarations against a `--import name=path` mapping (see
// cli.cppm) and lowering an already-parsed Program to a native object
// file -- shared by the main source and, once per resolved module, by
// compile_to_executable below.
namespace scpp {

[[nodiscard]] std::optional<std::filesystem::path> current_executable_path() {
    std::error_code ec;
    std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return std::nullopt;
    return path;
}

[[nodiscard]] std::optional<std::filesystem::path> runtime_default_stdlib_dir() {
    std::optional<std::filesystem::path> exe = current_executable_path();
    if (!exe.has_value()) return std::nullopt;
    return (exe->parent_path() / ".." / "stdlib").lexically_normal();
}

[[nodiscard]] std::vector<std::string> build_default_import_search_dirs(const std::vector<std::string>& explicit_dirs) {
    std::vector<std::string> dirs = explicit_dirs;
    auto append_if_missing = [&](std::string path) {
        if (path.empty()) return;
        if (std::find(dirs.begin(), dirs.end(), path) == dirs.end()) dirs.push_back(std::move(path));
    };
    if (const char* env = std::getenv("SCPP_STDLIB_PATH"); env != nullptr && env[0] != '\0') {
        append_if_missing(env);
    } else if (std::optional<std::filesystem::path> runtime_dir = runtime_default_stdlib_dir(); runtime_dir.has_value()) {
        append_if_missing(runtime_dir->string());
    }
    return dirs;
}

[[nodiscard]] std::vector<std::string> default_stdlib_link_inputs() {
    std::vector<std::string> result;
    std::optional<std::filesystem::path> exe = current_executable_path();
    if (!exe.has_value()) return result;
    std::filesystem::path lib_dir = (exe->parent_path() / "stdlib").lexically_normal();
    std::filesystem::path string_wrapper = lib_dir / "libscpp_string_wrapper.a";
    if (std::filesystem::exists(string_wrapper)) {
        result.push_back(string_wrapper.string());
    }
    std::filesystem::path thread_wrapper = lib_dir / "libscpp_thread_wrapper.a";
    if (std::filesystem::exists(thread_wrapper)) {
        result.push_back(thread_wrapper.string());
    }
    return result;
}

std::string read_scppm_interface_source(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw DriverError("cannot open imported module interface '" + path + "'");
    }
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
    std::uint32_t interface_length = 0;
    file.read(reinterpret_cast<char*>(&interface_length), sizeof(interface_length));
    if (file.gcount() != static_cast<std::streamsize>(sizeof(interface_length))) {
        throw DriverError("invalid .scppm file '" + path + "': missing interface length");
    }
    std::string source(interface_length, '\0');
    file.read(source.data(), static_cast<std::streamsize>(interface_length));
    if (file.gcount() != static_cast<std::streamsize>(interface_length)) {
        throw DriverError("invalid .scppm file '" + path + "': truncated interface source");
    }
    if ((flags & 0x01u) != 0u) {
        // v1 only consumes the embedded interface source. Generic bodies
        // serialized into the optional generics block are not read yet.
    }
    return source;
}

// Reads an imported module's source file from disk -- the parser itself
// never touches the filesystem (see scpp.parser's ModuleResolver); this
// is the driver's own responsibility, mirroring cli.cppm's own read_file
// for the main input file (a small, separate duplicate rather than a
// shared cross-module helper -- consistent with this codebase's existing
// precedent, e.g. movecheck.cppm/codegen.cppm's independently-duplicated
// types_equal).
std::string read_module_source(const std::string& path) {
    if (std::filesystem::path(path).extension() == ".scppm") {
        return read_scppm_interface_source(path);
    }
    std::ifstream file(path);
    if (!file) {
        throw DriverError("cannot open imported module source '" + path + "'");
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// ch11 §11.7/§11.8: resolves `import name;` declarations against a
// `--import name=path` mapping, recursively parsing (and caching) each
// imported module's source the first time it's needed -- multiple files
// importing the same module, or transitive imports (an imported module
// itself importing something else), only ever get parsed and merged
// once. Only the explicit, unambiguous `--import name=path` form is
// supported (mirrors Clang's `-fmodule-file=` and Rust's `--extern`,
// per ch11 §11.13) -- a `.scppm` archive/package search path
// (§11.11/§11.13's `-I` convenience) is out of scope for this pass.
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
        std::string source = read_module_source(path_it->second);
        Program imported = parse(
            source, [this](const std::string& name) -> const Program& { return resolve(name); },
            [this](const std::string& key) -> Program { return resolve_partition(key); });
        resolving_.erase(module_name);

        if (imported.module_name != module_name) {
            throw DriverError("'" + path_it->second + "' does not declare module '" + module_name +
                               "' (its own module declaration names '" +
                               (imported.module_name.empty() ? std::string("<none>") : imported.module_name) +
                               "')");
        }

        resolution_order_.push_back(module_name);
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
        std::string source = read_module_source(path_it->second);
        Program partition = parse(
            source, [this](const std::string& name) -> const Program& { return resolve(name); },
            [this](const std::string& nested_key) -> Program { return resolve_partition(nested_key); });
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

private:
    [[nodiscard]] std::optional<std::string> infer_module_path(const std::string& module_name) const {
        for (const std::string& dir : import_search_dirs_) {
            std::filesystem::path base(dir);
            std::filesystem::path source_candidate = base / (module_name + ".scpp");
            if (std::filesystem::exists(source_candidate)) return source_candidate.string();
            std::filesystem::path interface_candidate = base / (module_name + ".scppm");
            if (std::filesystem::exists(interface_candidate)) return interface_candidate.string();
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> infer_partition_path(const std::string& key) const {
        size_t colon = key.find(':');
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
    std::unordered_set<std::string> resolving_;
    std::unordered_set<std::string> partitions_resolving_;
    std::vector<std::string> resolution_order_;
};

// Move-checks an already-parsed (and, if it has imports of its own,
// already import-merged -- see scpp.parser's merge_imported_module)
// Program and lowers it to a native object file at `object_path`. Shared
// by emit_object_file (the main source) and, once per resolved module,
// by compile_to_executable below -- exactly the same backend either way,
// since by this point a Program is just a Program regardless of which
// file it came from.
void emit_object_file_for_program(Program& program, const std::string& object_path) {
    // ch05 §5.11: must run before check_moves -- see Monomorphizer's own
    // comment in movecheck.cppm for why call-site monomorphization has
    // to happen first (movecheck's ordinary exact-type-match call-
    // argument checking can only work once every call site targets an
    // already-concrete function).
    monomorphize_generics(program);
    check_moves(program);

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    std::string triple = llvm::sys::getDefaultTargetTriple();
    llvm::Triple target_triple(triple);

    std::string lookup_error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(target_triple, lookup_error);
    if (target == nullptr) {
        throw DriverError("failed to lookup target '" + triple + "': " + lookup_error);
    }

    llvm::TargetOptions options;
    std::unique_ptr<llvm::TargetMachine> target_machine(target->createTargetMachine(
        target_triple, "generic", "", options, llvm::Reloc::PIC_));
    if (!target_machine) {
        throw DriverError("failed to create target machine for '" + triple + "'");
    }

    // The data layout must be set *before* codegen runs: std::make_unique
    // needs a target-accurate sizeof(T) to call malloc with, which comes
    // from the module's DataLayout.
    Codegen codegen("scpp_module");
    codegen.set_target(target_triple, target_machine->createDataLayout());
    llvm::Module& module = codegen.generate(program);

    std::error_code error_code;
    llvm::raw_fd_ostream dest(object_path, error_code, llvm::sys::fs::OF_None);
    if (error_code) {
        throw DriverError("could not open object file '" + object_path + "': " + error_code.message());
    }

    llvm::legacy::PassManager pass_manager;
    if (target_machine->addPassesToEmitFile(pass_manager, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        throw DriverError("target machine cannot emit an object file of this type");
    }
    pass_manager.run(module);
    dest.flush();
}

} // namespace scpp

export namespace scpp {

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
                       const std::vector<std::string>& import_search_dirs = {}) {
    ModuleCache cache(import_paths, import_search_dirs);
    Program program = parse(
        source, [&cache](const std::string& name) -> const Program& { return cache.resolve(name); },
        [&cache](const std::string& key) -> Program { return cache.resolve_partition(key); });
    emit_object_file_for_program(program, object_path);
}

// Links a native object file into an executable using the system compiler
// driver (clang/cc); this keeps us out of the business of re-implementing a
// platform linker for M1. `extra_link_inputs` is appended verbatim after the
// scpp object file -- additional .o/.a paths (e.g. a separately-built
// `extern "C"` wrapper library, see stdlib/README.md, or another module's
// own compiled object file, see compile_to_executable below) or
// `-lname`/`-Lpath` flags a caller wants forwarded straight to the linker;
// empty by default (an ordinary, no-C++-interop build needs none of this).
void link_executable(const std::string& object_path, const std::string& executable_path,
                      const std::vector<std::string>& extra_link_inputs = {}, bool static_link = false) {
    std::string command = "cc \"" + object_path + "\"";
    if (static_link) command += " -static";
    for (const std::string& input : extra_link_inputs) {
        command += " \"" + input + "\"";
    }
    // A wrapper library that itself calls into real C++ (e.g. std::string)
    // needs libstdc++'s runtime linked in too; `cc` alone (plain C mode)
    // doesn't pull that in automatically the way `c++`/`clang++` would.
    // Only added when there's an actual C++ wrapper to support, so a plain
    // scpp-only build's link command is unaffected.
    if (!extra_link_inputs.empty()) command += " -lstdc++";
    command += " -o \"" + executable_path + "\"";
    int result = std::system(command.c_str());
    if (result != 0) {
        throw DriverError("linker command failed: " + command);
    }
}

// `import_paths` (ch11 §11.7, `--import name=path`) resolves any
// `import name;` declarations `source` has. Every module actually
// resolved (directly or transitively) gets its own separately-compiled
// object file too -- an importer's codegen only ever *declares* (LLVM
// `extern`) an imported symbol; something else has to *define* it (see
// codegen.cppm's mangle_exported_symbol) -- then every object file
// (main + one per resolved module + `extra_link_inputs`) is linked
// together in one step. This is a real, if minimal, multi-file
// separate-compilation pipeline (ch11's own framing): each module is
// compiled fully independently, and only the system linker unifies them,
// exactly like ordinary multi-TU C/C++ builds always have.
void compile_to_executable(std::string_view source, const std::string& executable_path,
                            const std::vector<std::string>& extra_link_inputs = {},
                            const std::unordered_map<std::string, std::string>& import_paths = {},
                            bool static_link = false,
                            const std::vector<std::string>& import_search_dirs = {}) {
    ModuleCache cache(import_paths, import_search_dirs);
    Program program = parse(
        source, [&cache](const std::string& name) -> const Program& { return cache.resolve(name); },
        [&cache](const std::string& key) -> Program { return cache.resolve_partition(key); });

    std::string object_path = executable_path + ".o";
    emit_object_file_for_program(program, object_path);

    std::vector<std::string> module_object_paths;
    for (const std::string& module_name : cache.resolution_order()) {
        std::string module_object_path = executable_path + "." + module_name + ".o";
        emit_object_file_for_program(cache.program_for(module_name), module_object_path);
        module_object_paths.push_back(module_object_path);
    }

    // Each resolved module's own object file is placed *before*
    // extra_link_inputs (e.g. a native C++ wrapper library archive) so a
    // conventional archive-aware linker (which only pulls in an archive
    // member actually needed by a symbol reference seen *so far*) still
    // resolves a module's own extern "C" calls into that archive
    // correctly -- same reasoning `--link`'s existing placement already
    // relies on.
    std::vector<std::string> link_inputs = module_object_paths;
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
    link_executable(object_path, executable_path, link_inputs, static_link);

    llvm::sys::fs::remove(object_path);
    for (const std::string& module_object_path : module_object_paths) {
        llvm::sys::fs::remove(module_object_path);
    }
}

} // namespace scpp
