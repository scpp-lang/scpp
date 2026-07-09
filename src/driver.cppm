module;

#include <array>
#include <algorithm>
#include <cctype>
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
import scpp.lexer;
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

[[nodiscard]] std::string module_key(const Program& program) {
    if (program.partition_name.empty()) return program.module_name;
    return program.module_name + ":" + program.partition_name;
}

[[nodiscard]] std::optional<std::string> declared_module_name_from_source(std::string_view source) {
    std::vector<Token> tokens = tokenize(source);
    size_t i = 0;
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

[[nodiscard]] std::string serialize_generics_block(const Program& program, std::string_view source) {
    std::vector<std::string> exported_generic_names;
    for (const Function& fn : program.functions) {
        if (!fn.is_exported) continue;
        if (!(fn.is_generic_template || !fn.template_params.empty())) continue;
        exported_generic_names.push_back(fn.name);
    }
    if (exported_generic_names.empty()) return {};

    std::string block;
    block.append("SGEN", 4);
    auto append_u32 = [&](std::uint32_t value) {
        block.push_back(static_cast<char>(value & 0xffu));
        block.push_back(static_cast<char>((value >> 8) & 0xffu));
        block.push_back(static_cast<char>((value >> 16) & 0xffu));
        block.push_back(static_cast<char>((value >> 24) & 0xffu));
    };
    append_u32(1); // generics block format version
    append_u32(static_cast<std::uint32_t>(exported_generic_names.size()));
    for (const std::string& name : exported_generic_names) {
        append_u32(static_cast<std::uint32_t>(name.size()));
        block.append(name);
    }
    append_u32(static_cast<std::uint32_t>(source.size()));
    block.append(source.data(), source.size());
    return block;
}

void write_scppm_file(const Program& program, std::string_view interface_source, std::string_view generic_source_snapshot,
                      const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw DriverError("cannot write module interface '" + path + "'");
    }
    std::string generics_block = serialize_generics_block(program, generic_source_snapshot);
    unsigned char flags = generics_block.empty() ? 0u : 0x01u;
    const std::array<char, 8> header = {'S', 'C', 'P', 'P', 'M', 1, 0, static_cast<char>(flags)};
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    write_u32_le(out, static_cast<std::uint32_t>(interface_source.size()));
    out.write(interface_source.data(), static_cast<std::streamsize>(interface_source.size()));
    if ((flags & 0x01u) != 0u) {
        write_u32_le(out, static_cast<std::uint32_t>(generics_block.size()));
        out.write(generics_block.data(), static_cast<std::streamsize>(generics_block.size()));
    }
    if (!out) {
        throw DriverError("failed while writing module interface '" + path + "'");
    }
}

void create_archive(const std::string& object_path, const std::string& archive_path) {
    std::string command = "ar rcs \"" + archive_path + "\" \"" + object_path + "\"";
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
    return (exe->parent_path() / "stdlib").lexically_normal();
}

[[nodiscard]] std::optional<std::filesystem::path> runtime_default_source_stdlib_dir() {
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
    } else {
        if (std::optional<std::filesystem::path> runtime_dir = runtime_default_prebuilt_stdlib_dir(); runtime_dir.has_value()) {
            append_if_missing(runtime_dir->string());
        }
        if (std::optional<std::filesystem::path> runtime_dir = runtime_default_source_stdlib_dir(); runtime_dir.has_value()) {
            append_if_missing(runtime_dir->string());
        }
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

[[nodiscard]] std::string absolute_source_path(const std::string& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec) return path;
    return absolute.lexically_normal().string();
}

[[nodiscard]] bool should_keep_function_body_in_interface(const Function& fn) {
    return fn.is_generic_template || !fn.template_params.empty() || !fn.generic_method_owner_id.empty();
}

[[nodiscard]] std::vector<size_t> line_offsets(std::string_view source) {
    std::vector<size_t> offsets = {0};
    for (size_t i = 0; i < source.size(); i++) {
        if (source[i] == '\n') offsets.push_back(i + 1);
    }
    return offsets;
}

[[nodiscard]] size_t offset_for_loc(std::string_view source, const SourceLocation& loc) {
    std::vector<size_t> offsets = line_offsets(source);
    size_t line_index = static_cast<size_t>(std::max(loc.line, 1) - 1);
    if (line_index >= offsets.size()) return source.size();
    return std::min(offsets[line_index] + static_cast<size_t>(std::max(loc.column, 1) - 1), source.size());
}

[[nodiscard]] size_t find_matching_brace(std::string_view source, size_t open_offset) {
    bool in_string = false;
    bool in_char = false;
    bool in_line_comment = false;
    bool in_block_comment = false;
    int depth = 0;
    for (size_t i = open_offset; i < source.size(); i++) {
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

std::string strip_concrete_function_bodies(const Program& program, const std::string& file_path, std::string source) {
    struct BodyRange {
        size_t begin;
        size_t end;
    };
    std::vector<BodyRange> ranges;
    for (const Function& fn : program.functions) {
        if (!fn.body || should_keep_function_body_in_interface(fn) || !fn.loc.has_source_path()) continue;
        if (absolute_source_path(fn.loc.source_path_text()) != file_path) continue;
        size_t begin = offset_for_loc(source, fn.body->loc);
        if (begin >= source.size() || source[begin] != '{') continue;
        size_t end = find_matching_brace(source, begin);
        ranges.push_back(BodyRange{begin, end + 1});
    }
    std::sort(ranges.begin(), ranges.end(), [](const BodyRange& a, const BodyRange& b) { return a.begin > b.begin; });
    for (const BodyRange& range : ranges) {
        source.replace(range.begin, range.end - range.begin, ";");
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
        std::string source = read_module_source(resolved_path);
        Program imported = parse(
            source, [this](const std::string& name) -> const Program& { return resolve(name); },
            [this](const std::string& key) -> Program { return resolve_partition(key); }, resolved_path);
        imported.source_path = resolved_path;
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
        std::string source = read_module_source(path_it->second);
        Program partition = parse(
            source, [this](const std::string& name) -> const Program& { return resolve(name); },
            [this](const std::string& nested_key) -> Program { return resolve_partition(nested_key); },
            absolute_source_path(path_it->second));
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
    std::unordered_map<std::string, std::string> resolved_paths_;
    std::unordered_set<std::string> resolving_;
    std::unordered_set<std::string> partitions_resolving_;
    std::vector<std::string> resolution_order_;
};

[[nodiscard]] std::string trim_copy(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) begin++;
    size_t end = text.size();
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

std::string render_module_interface_file(const Program& program, const std::string& file_path,
                                         const std::string& module_source_path, bool keep_concrete_bodies,
                                         bool keep_module_declaration,
                                         std::unordered_set<std::string>& expanded_partition_paths);

std::string inline_partition_imports(const Program& program, const std::string& module_source_path, std::string_view source,
                                     bool keep_concrete_bodies, std::unordered_set<std::string>& expanded_partition_paths) {
    std::ostringstream out;
    size_t line_start = 0;
    while (line_start <= source.size()) {
        size_t line_end = source.find('\n', line_start);
        bool had_newline = line_end != std::string_view::npos;
        std::string_view line =
            had_newline ? source.substr(line_start, line_end - line_start) : source.substr(line_start);
        std::string trimmed = trim_copy(line);
        if (starts_with(trimmed, "export import :") || starts_with(trimmed, "import :")) {
            size_t colon = trimmed.find(':');
            size_t semi = trimmed.find(';', colon);
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
    std::string source = read_module_source(file_path);
    if (!keep_concrete_bodies) {
        source = strip_concrete_function_bodies(program, absolute_source_path(file_path), std::move(source));
    }

    std::ostringstream out;
    size_t line_start = 0;
    while (line_start <= source.size()) {
        size_t line_end = source.find('\n', line_start);
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

std::string build_merged_interface_source(const Program& program, const std::string& module_source_path,
                                          bool keep_concrete_bodies) {
    std::unordered_set<std::string> expanded_partition_paths;
    return render_module_interface_file(program, module_source_path, module_source_path, keep_concrete_bodies,
                                        /*keep_module_declaration=*/true, expanded_partition_paths);
}

llvm::CodeGenOptLevel codegen_opt_level_for(int opt_level) {
    if (opt_level <= 0) return llvm::CodeGenOptLevel::None;
    if (opt_level == 1) return llvm::CodeGenOptLevel::Less;
    if (opt_level == 2) return llvm::CodeGenOptLevel::Default;
    return llvm::CodeGenOptLevel::Aggressive;
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
        target_triple, "generic", "", options, llvm::Reloc::PIC_, std::nullopt, codegen_opt_level_for(opt_level)));
    if (!target_machine) {
        throw DriverError("failed to create target machine for '" + triple + "'");
    }

    // The data layout must be set *before* codegen runs: std::make_unique
    // needs a target-accurate sizeof(T) to call malloc with, which comes
    // from the module's DataLayout.
    Codegen codegen("scpp_module", program.source_path, emit_debug_info);
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

void emit_module_archive_for_program(Program& program, const std::string& archive_path, int opt_level = 2) {
    std::filesystem::path object_path(archive_path);
    object_path.replace_extension(".scppo");
    emit_object_file_for_program(program, object_path.string(), /*emit_debug_info=*/false, opt_level);
    try {
        create_archive(object_path.string(), archive_path);
    } catch (...) {
        llvm::sys::fs::remove(object_path.string());
        throw;
    }
    llvm::sys::fs::remove(object_path.string());
}

} // namespace scpp

export namespace scpp {

std::string host_target_triple() { return llvm::sys::getDefaultTargetTriple(); }

std::vector<std::string> project_default_stdlib_link_inputs() { return default_stdlib_link_inputs(); }

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
    if (!program.is_module_interface) {
        throw DriverError("module artifacts can only be emitted from an interface unit, not '" +
                          (program.module_name.empty() ? std::string("<non-module>") : module_key(program)) + "'");
    }
    std::string merged_interface_source =
        build_merged_interface_source(program, absolute_source_path(source_path), /*keep_concrete_bodies=*/false);
    std::string merged_generic_source =
        build_merged_interface_source(program, absolute_source_path(source_path), /*keep_concrete_bodies=*/true);
    write_scppm_file(program, merged_interface_source, merged_generic_source, interface_path);
    emit_module_archive_for_program(program, archive_path, opt_level);
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
    link_executable(object_path, executable_path, link_inputs, static_link);

    llvm::sys::fs::remove(object_path);
    for (const std::string& module_object_path : module_object_paths) {
        llvm::sys::fs::remove(module_object_path);
    }
}

} // namespace scpp
