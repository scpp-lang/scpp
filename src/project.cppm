module;

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

export module scpp.project;

import scpp.ast;
import scpp.codegen;
import scpp.driver;
import scpp.lexer;
import scpp.movecheck;
import scpp.parser;

export namespace scpp {

struct ProjectBuildOptions {
    bool build_lib_only = false;
    std::optional<std::string> selected_bin;
    std::optional<std::string> selected_profile;
    bool release = false;
};

std::optional<std::filesystem::path> find_project_manifest(const std::filesystem::path& start_dir);
int build_manifest_project(const std::filesystem::path& start_dir, const ProjectBuildOptions& options);

} // namespace scpp

namespace {

struct ManifestError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct BuildError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string trim(std::string_view text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) start++;
    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) end--;
    return std::string(text.substr(start, end - start));
}

std::string strip_toml_comment(std::string_view line) {
    bool in_string = false;
    bool escape = false;
    std::string out;
    out.reserve(line.size());
    for (char ch : line) {
        if (escape) {
            out.push_back(ch);
            escape = false;
            continue;
        }
        if (ch == '\\' && in_string) {
            out.push_back(ch);
            escape = true;
            continue;
        }
        if (ch == '"') {
            in_string = !in_string;
            out.push_back(ch);
            continue;
        }
        if (ch == '#' && !in_string) break;
        out.push_back(ch);
    }
    return out;
}

std::string parse_string_literal(std::string_view text, const std::string& context) {
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
        throw ManifestError(context + " must be a TOML string");
    }
    std::string out;
    out.reserve(text.size() - 2);
    bool escape = false;
    for (size_t i = 1; i + 1 < text.size(); i++) {
        char ch = text[i];
        if (escape) {
            switch (ch) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                default: throw ManifestError(context + " contains an unsupported escape sequence");
            }
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        out.push_back(ch);
    }
    if (escape) throw ManifestError(context + " ends with an incomplete escape sequence");
    return out;
}

bool parse_bool_literal(std::string_view text, const std::string& context) {
    if (text == "true") return true;
    if (text == "false") return false;
    throw ManifestError(context + " must be true or false");
}

int parse_int_literal(std::string_view text, const std::string& context) {
    std::string trimmed = trim(text);
    if (trimmed.empty()) throw ManifestError(context + " must be an integer");
    size_t index = 0;
    int value = 0;
    try {
        value = std::stoi(trimmed, &index);
    } catch (...) {
        throw ManifestError(context + " must be an integer");
    }
    if (index != trimmed.size()) throw ManifestError(context + " must be an integer");
    return value;
}

std::vector<std::string> parse_string_array(std::string_view text, const std::string& context) {
    std::string trimmed = trim(text);
    if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']') {
        throw ManifestError(context + " must be an array of strings");
    }
    std::vector<std::string> items;
    size_t i = 1;
    while (i + 1 < trimmed.size()) {
        while (i + 1 < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[i]))) i++;
        if (i + 1 >= trimmed.size() || trimmed[i] == ']') break;
        size_t start = i;
        bool in_string = false;
        bool escape = false;
        while (i + 1 < trimmed.size()) {
            char ch = trimmed[i];
            if (escape) {
                escape = false;
                i++;
                continue;
            }
            if (ch == '\\' && in_string) {
                escape = true;
                i++;
                continue;
            }
            if (ch == '"') {
                in_string = !in_string;
                i++;
                continue;
            }
            if (ch == ',' && !in_string) break;
            if (ch == ']' && !in_string) break;
            i++;
        }
        items.push_back(parse_string_literal(trimmed.substr(start, i - start), context));
        while (i + 1 < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[i]))) i++;
        if (i + 1 < trimmed.size() && trimmed[i] == ',') {
            i++;
            continue;
        }
        if (i + 1 < trimmed.size() && trimmed[i] == ']') break;
    }
    return items;
}

std::string escape_json(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string sanitize_filename(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char ch : raw) {
        if (std::isalnum(ch)) {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "target";
    return out;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot open file '" + path.string() + "'");
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void write_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream file(path);
    if (!file) throw std::runtime_error("cannot write file '" + path.string() + "'");
    file << content;
}

void print_diagnostic(std::string_view path, const std::string& source, scpp::SourceLocation loc,
                      const std::string& message) {
    std::cerr << path << ":";
    if (loc.is_known()) std::cerr << loc.line << ":" << loc.column << ":";
    std::cerr << " error: " << message << "\n";
    if (!loc.is_known()) return;
    size_t line_start = 0;
    int current_line = 1;
    while (current_line < loc.line) {
        size_t next_nl = source.find('\n', line_start);
        if (next_nl == std::string::npos) return;
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
    for (int i = 0; i < loc.column - 1 && static_cast<size_t>(i) < line_text.size(); i++) {
        std::cerr << (line_text[static_cast<size_t>(i)] == '\t' ? '\t' : ' ');
    }
    std::cerr << "^\n";
}

struct ProfileSettings {
    int opt_level = 0;
    bool debug = true;
    bool static_link = false;
};

struct ManifestTarget {
    std::string name;
    std::filesystem::path root;
    std::vector<std::string> source_patterns;
};

struct ManifestData {
    int manifest_version = -1;
    std::string package_name;
    std::optional<std::string> package_version;
    std::optional<ManifestTarget> lib_target;
    std::vector<ManifestTarget> bin_targets;
    std::map<std::string, ProfileSettings> profiles;
    std::filesystem::path manifest_path;
    bool has_workspace = false;
    bool has_dependencies = false;
    bool has_native = false;
};

struct SourceInfo {
    enum class Kind {
        Plain,
        PrimaryInterface,
        InterfacePartition,
        ImplementationUnit,
        ImplementationPartition,
    };

    std::filesystem::path path;
    Kind kind = Kind::Plain;
    std::string module_name;
    std::string partition_name;
    std::vector<std::string> imported_modules;
};

struct BuiltModule {
    std::string name;
    std::filesystem::path source_path;
    std::filesystem::path interface_path;
    std::filesystem::path archive_path;
};

struct BuildOutputs {
    std::vector<BuiltModule> library_modules;
    std::vector<std::filesystem::path> binaries;
};

std::optional<std::filesystem::path> find_project_manifest_impl(std::filesystem::path start_dir) {
    std::error_code ec;
    std::filesystem::path current = std::filesystem::absolute(start_dir, ec);
    if (ec) current = start_dir;
    while (true) {
        std::filesystem::path candidate = current / "scpp.toml";
        if (std::filesystem::exists(candidate)) return candidate;
        if (current == current.root_path()) break;
        std::filesystem::path parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return std::nullopt;
}

ManifestData parse_manifest(const std::filesystem::path& manifest_path) {
    ManifestData manifest;
    manifest.manifest_path = std::filesystem::absolute(manifest_path);
    manifest.profiles["dev"] = ProfileSettings{0, true, false};
    manifest.profiles["release"] = ProfileSettings{3, false, false};

    std::ifstream input(manifest.manifest_path);
    if (!input) throw ManifestError("cannot open manifest '" + manifest.manifest_path.string() + "'");

    enum class Section { Root, Package, Lib, Bin, Profile, Dependencies, Native, Workspace, WorkspaceDependencies, PackageMetadata, Ignored };
    Section current_section = Section::Root;
    std::string current_profile;
    ManifestTarget* current_bin = nullptr;

    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        line_number++;
        std::string stripped = trim(strip_toml_comment(line));
        if (stripped.empty()) continue;
        if (stripped.front() == '[') {
            if (stripped.size() < 2 || stripped.back() != ']') {
                throw ManifestError(manifest.manifest_path.string() + ":" + std::to_string(line_number) +
                                    ": malformed section header");
            }
            current_bin = nullptr;
            if (stripped.rfind("[[", 0) == 0) {
                if (stripped.size() < 4 || stripped.substr(stripped.size() - 2) != "]]") {
                    throw ManifestError(manifest.manifest_path.string() + ":" + std::to_string(line_number) +
                                        ": malformed array-of-table header");
                }
                std::string section_name = trim(stripped.substr(2, stripped.size() - 4));
                if (section_name != "bin") {
                    throw ManifestError(manifest.manifest_path.string() + ":" + std::to_string(line_number) +
                                        ": unsupported array-of-table [[" + section_name + "]]");
                }
                manifest.bin_targets.emplace_back();
                current_bin = &manifest.bin_targets.back();
                current_section = Section::Bin;
                continue;
            }
            std::string section_name = trim(stripped.substr(1, stripped.size() - 2));
            if (section_name == "package") {
                current_section = Section::Package;
            } else if (section_name == "lib") {
                current_section = Section::Lib;
                if (!manifest.lib_target.has_value()) manifest.lib_target = ManifestTarget{};
            } else if (section_name == "dependencies") {
                current_section = Section::Dependencies;
            } else if (section_name == "native") {
                current_section = Section::Native;
            } else if (section_name == "workspace") {
                current_section = Section::Workspace;
                manifest.has_workspace = true;
            } else if (section_name == "workspace.dependencies") {
                current_section = Section::WorkspaceDependencies;
                manifest.has_workspace = true;
            } else if (section_name == "package.metadata") {
                current_section = Section::PackageMetadata;
            } else if (section_name.rfind("profile.", 0) == 0) {
                current_section = Section::Profile;
                current_profile = section_name.substr(std::string("profile.").size());
                if (current_profile.empty()) {
                    throw ManifestError(manifest.manifest_path.string() + ":" + std::to_string(line_number) +
                                        ": profile section name cannot be empty");
                }
                if (!manifest.profiles.contains(current_profile)) {
                    manifest.profiles[current_profile] = (current_profile == "release")
                        ? ProfileSettings{3, false, false}
                        : ProfileSettings{0, true, false};
                }
            } else {
                current_section = Section::Ignored;
            }
            continue;
        }

        size_t eq = stripped.find('=');
        if (eq == std::string::npos) {
            throw ManifestError(manifest.manifest_path.string() + ":" + std::to_string(line_number) +
                                ": expected key = value");
        }
        std::string key = trim(stripped.substr(0, eq));
        std::string value = trim(stripped.substr(eq + 1));
        std::string context = manifest.manifest_path.string() + ":" + std::to_string(line_number) + ": " + key;

        switch (current_section) {
            case Section::Root:
                if (key == "manifest-version") {
                    manifest.manifest_version = parse_int_literal(value, context);
                } else {
                    throw ManifestError(context + " is not supported in the manifest root");
                }
                break;
            case Section::Package:
                if (key == "name") {
                    manifest.package_name = parse_string_literal(value, context);
                } else if (key == "version") {
                    manifest.package_version = parse_string_literal(value, context);
                } else {
                    throw ManifestError(context + " is not supported in [package]");
                }
                break;
            case Section::Lib:
                if (key == "root") {
                    manifest.lib_target->root = parse_string_literal(value, context);
                } else if (key == "sources") {
                    manifest.lib_target->source_patterns = parse_string_array(value, context);
                } else {
                    throw ManifestError(context + " is not supported in [lib]");
                }
                break;
            case Section::Bin:
                if (current_bin == nullptr) throw ManifestError(context + " is outside a [[bin]] table");
                if (key == "name") {
                    current_bin->name = parse_string_literal(value, context);
                } else if (key == "root") {
                    current_bin->root = parse_string_literal(value, context);
                } else if (key == "sources") {
                    current_bin->source_patterns = parse_string_array(value, context);
                } else {
                    throw ManifestError(context + " is not supported in [[bin]]");
                }
                break;
            case Section::Profile:
                if (key == "opt-level") {
                    manifest.profiles[current_profile].opt_level = parse_int_literal(value, context);
                } else if (key == "debug") {
                    manifest.profiles[current_profile].debug = parse_bool_literal(value, context);
                } else if (key == "static") {
                    manifest.profiles[current_profile].static_link = parse_bool_literal(value, context);
                } else {
                    throw ManifestError(context + " is not supported in [profile." + current_profile + "]");
                }
                break;
            case Section::Dependencies:
                manifest.has_dependencies = true;
                break;
            case Section::Native:
                manifest.has_native = true;
                break;
            case Section::Workspace:
            case Section::WorkspaceDependencies:
                manifest.has_workspace = true;
                break;
            case Section::PackageMetadata:
            case Section::Ignored:
                break;
        }
    }

    if (manifest.manifest_version != 1) {
        throw ManifestError("manifest-version = 1 is required in '" + manifest.manifest_path.string() + "'");
    }
    if (manifest.has_workspace) {
        throw ManifestError("workspace manifests are designed but not implemented yet");
    }
    if (manifest.has_dependencies) {
        throw ManifestError("[dependencies] is designed but not implemented yet");
    }
    if (manifest.has_native) {
        throw ManifestError("[native] is designed but not implemented yet");
    }
    if (manifest.package_name.empty()) {
        throw ManifestError("[package].name is required in '" + manifest.manifest_path.string() + "'");
    }
    if (!manifest.lib_target.has_value() && manifest.bin_targets.empty()) {
        throw ManifestError("manifest must declare at least one [lib] or [[bin]] target");
    }
    if (manifest.lib_target.has_value()) {
        if (manifest.lib_target->root.empty()) throw ManifestError("[lib].root is required");
        if (manifest.lib_target->source_patterns.empty()) throw ManifestError("[lib].sources is required");
    }
    std::unordered_set<std::string> bin_names;
    for (const ManifestTarget& bin : manifest.bin_targets) {
        if (bin.name.empty()) throw ManifestError("[[bin]].name is required");
        if (bin.root.empty()) throw ManifestError("[[bin]].root is required");
        if (bin.source_patterns.empty()) throw ManifestError("[[bin]].sources is required");
        if (!bin_names.insert(bin.name).second) {
            throw ManifestError("duplicate [[bin]] target name '" + bin.name + "'");
        }
    }
    return manifest;
}

std::regex glob_to_regex(std::string_view pattern) {
    std::string regex = "^";
    for (size_t i = 0; i < pattern.size(); i++) {
        char ch = pattern[i];
        if (ch == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                if (i + 2 < pattern.size() && pattern[i + 2] == '/') {
                    regex += "(?:.*/)?";
                    i += 2;
                } else {
                    regex += ".*";
                    i++;
                }
            } else {
                regex += "[^/]*";
            }
        } else if (ch == '?') {
            regex += "[^/]";
        } else if (std::string_view(".\\+^$()[]{}|").find(ch) != std::string_view::npos) {
            regex.push_back('\\');
            regex.push_back(ch);
        } else {
            regex.push_back(ch);
        }
    }
    regex += "$";
    return std::regex(regex);
}

std::vector<std::filesystem::path> expand_source_patterns(const std::filesystem::path& base_dir,
                                                          const std::vector<std::string>& patterns,
                                                          const std::filesystem::path& root) {
    std::vector<std::regex> matchers;
    for (const std::string& pattern : patterns) {
        matchers.push_back(glob_to_regex(std::filesystem::path(pattern).generic_string()));
    }
    std::set<std::filesystem::path> paths;
    std::error_code ec;
    if (std::filesystem::exists(root)) paths.insert(std::filesystem::weakly_canonical(root, ec));
    std::filesystem::recursive_directory_iterator end;
    for (std::filesystem::recursive_directory_iterator it(base_dir, ec); it != end && !ec; it.increment(ec)) {
        if (ec || !it->is_regular_file()) continue;
        std::filesystem::path relative = std::filesystem::relative(it->path(), base_dir, ec);
        if (ec) continue;
        std::string candidate = relative.generic_string();
        for (const std::regex& matcher : matchers) {
            if (std::regex_match(candidate, matcher)) {
                paths.insert(std::filesystem::weakly_canonical(it->path(), ec));
                break;
            }
        }
    }
    return std::vector<std::filesystem::path>(paths.begin(), paths.end());
}

SourceInfo classify_source(const std::filesystem::path& path) {
    SourceInfo info;
    info.path = path;
    std::string source = read_file(path);
    std::vector<scpp::Token> tokens = scpp::tokenize(source);
    size_t i = 0;
    bool exported_module = false;
    if (i < tokens.size() && tokens[i].kind == scpp::TokenKind::KwExport) {
        if (i + 1 < tokens.size() && tokens[i + 1].kind == scpp::TokenKind::KwModule) {
            exported_module = true;
            i++;
        }
    }
    if (i < tokens.size() && tokens[i].kind == scpp::TokenKind::KwModule) {
        i++;
        if (i < tokens.size() && tokens[i].kind == scpp::TokenKind::Identifier) {
            info.module_name = std::string(tokens[i].text);
            i++;
            while (i + 1 < tokens.size() && tokens[i].kind == scpp::TokenKind::Dot &&
                   tokens[i + 1].kind == scpp::TokenKind::Identifier) {
                info.module_name += ".";
                info.module_name += std::string(tokens[i + 1].text);
                i += 2;
            }
            if (i < tokens.size() && tokens[i].kind == scpp::TokenKind::Colon) {
                i++;
                if (i >= tokens.size() || tokens[i].kind != scpp::TokenKind::Identifier) {
                    throw BuildError("invalid partition declaration in '" + path.string() + "'");
                }
                info.partition_name = std::string(tokens[i].text);
                info.kind = exported_module ? SourceInfo::Kind::InterfacePartition
                                            : SourceInfo::Kind::ImplementationPartition;
            } else {
                info.kind = exported_module ? SourceInfo::Kind::PrimaryInterface
                                            : SourceInfo::Kind::ImplementationUnit;
            }
        }
    }

    for (size_t token_index = 0; token_index < tokens.size(); token_index++) {
        size_t import_index = token_index;
        if (tokens[token_index].kind == scpp::TokenKind::KwExport) {
            if (token_index + 1 >= tokens.size() || tokens[token_index + 1].kind != scpp::TokenKind::KwImport) continue;
            import_index = token_index + 1;
        } else if (tokens[token_index].kind != scpp::TokenKind::KwImport) {
            continue;
        }
        size_t j = import_index + 1;
        if (j >= tokens.size()) continue;
        if (tokens[j].kind == scpp::TokenKind::Colon) continue;
        if (tokens[j].kind != scpp::TokenKind::Identifier) continue;
        std::string module_name(tokens[j].text);
        j++;
        while (j + 1 < tokens.size() && tokens[j].kind == scpp::TokenKind::Dot &&
               tokens[j + 1].kind == scpp::TokenKind::Identifier) {
            module_name += ".";
            module_name += std::string(tokens[j + 1].text);
            j += 2;
        }
        info.imported_modules.push_back(module_name);
    }
    return info;
}

std::vector<std::string> topo_sort_modules(const std::map<std::string, SourceInfo>& primary_modules) {
    std::unordered_map<std::string, std::vector<std::string>> edges;
    std::unordered_map<std::string, int> indegree;
    for (const auto& [name, _] : primary_modules) indegree[name] = 0;
    for (const auto& [name, source] : primary_modules) {
        std::unordered_set<std::string> local_deps;
        for (const std::string& imported : source.imported_modules) {
            if (primary_modules.contains(imported) && imported != name) local_deps.insert(imported);
        }
        for (const std::string& dep : local_deps) {
            edges[dep].push_back(name);
            indegree[name]++;
        }
    }
    std::vector<std::string> ready;
    for (const auto& [name, degree] : indegree) {
        if (degree == 0) ready.push_back(name);
    }
    std::sort(ready.begin(), ready.end());
    std::vector<std::string> order;
    while (!ready.empty()) {
        std::string current = ready.front();
        ready.erase(ready.begin());
        order.push_back(current);
        for (const std::string& dependent : edges[current]) {
            indegree[dependent]--;
            if (indegree[dependent] == 0) {
                ready.push_back(dependent);
                std::sort(ready.begin(), ready.end());
            }
        }
    }
    if (order.size() != primary_modules.size()) {
        throw BuildError("cyclic local module dependency detected in manifest target source set");
    }
    return order;
}

std::vector<SourceInfo> classify_target_sources(const std::filesystem::path& manifest_dir, const ManifestTarget& target) {
    std::filesystem::path root = std::filesystem::weakly_canonical(manifest_dir / target.root);
    if (!std::filesystem::exists(root)) {
        throw BuildError("target root '" + (manifest_dir / target.root).string() + "' does not exist");
    }
    std::vector<std::filesystem::path> source_paths = expand_source_patterns(manifest_dir, target.source_patterns, root);
    bool found_root = false;
    std::vector<SourceInfo> sources;
    for (const std::filesystem::path& source_path : source_paths) {
        SourceInfo info = classify_source(source_path);
        if (source_path == root) found_root = true;
        sources.push_back(std::move(info));
    }
    if (!found_root) {
        throw BuildError("target root '" + root.string() + "' is not covered by the declared sources globs");
    }
    return sources;
}

std::vector<BuiltModule> build_modules_for_target(const std::vector<SourceInfo>& sources,
                                                  const std::filesystem::path& module_dir,
                                                  const std::filesystem::path& archive_dir,
                                                  const std::unordered_map<std::string, std::string>& base_import_paths,
                                                  int opt_level) {
    std::map<std::string, SourceInfo> primary_modules;
    std::unordered_set<std::string> declared_partitions;
    for (const SourceInfo& source : sources) {
        switch (source.kind) {
            case SourceInfo::Kind::PrimaryInterface:
                if (primary_modules.contains(source.module_name)) {
                    throw BuildError("duplicate primary interface for module '" + source.module_name + "'");
                }
                primary_modules.emplace(source.module_name, source);
                break;
            case SourceInfo::Kind::InterfacePartition:
            case SourceInfo::Kind::ImplementationPartition:
                declared_partitions.insert(source.module_name + ":" + source.partition_name);
                break;
            case SourceInfo::Kind::ImplementationUnit:
                throw BuildError("module implementation units are not implemented in project builds yet ('" +
                                 source.path.string() + "')");
            case SourceInfo::Kind::Plain:
                break;
        }
    }

    for (const SourceInfo& source : sources) {
        if ((source.kind == SourceInfo::Kind::InterfacePartition ||
             source.kind == SourceInfo::Kind::ImplementationPartition) &&
            !primary_modules.contains(source.module_name)) {
            throw BuildError("partition '" + source.module_name + ":" + source.partition_name +
                             "' has no primary interface in this target");
        }
    }

    std::vector<std::string> build_order = topo_sort_modules(primary_modules);
    std::unordered_map<std::string, std::string> import_paths = base_import_paths;
    std::vector<BuiltModule> outputs;
    for (const std::string& module_name : build_order) {
        const SourceInfo& source = primary_modules.at(module_name);
        std::filesystem::path interface_path = module_dir / (module_name + ".scppm");
        std::filesystem::path archive_path = archive_dir / ("lib" + module_name + ".scppa");
        std::string module_source = read_file(source.path);
        try {
            scpp::emit_module_artifacts(module_source, interface_path.string(), archive_path.string(), import_paths, {},
                                        source.path.string(), opt_level);
        } catch (const scpp::ParseError& e) {
            print_diagnostic(source.path.string(), module_source, e.loc, e.what());
            throw;
        } catch (const scpp::DataflowError& e) {
            print_diagnostic(source.path.string(), module_source, e.loc, e.what());
            throw;
        } catch (const scpp::CodegenError& e) {
            print_diagnostic(source.path.string(), module_source, e.loc, e.what());
            throw;
        } catch (const scpp::DriverError& e) {
            print_diagnostic(source.path.string(), module_source, scpp::SourceLocation{}, e.what());
            throw;
        }
        import_paths[module_name] = interface_path.string();
        outputs.push_back(BuiltModule{module_name, source.path, interface_path, archive_path});
    }
    return outputs;
}

std::unordered_map<std::string, std::string> to_import_map(const std::vector<BuiltModule>& modules) {
    std::unordered_map<std::string, std::string> import_paths;
    for (const BuiltModule& module : modules) import_paths.emplace(module.name, module.interface_path.string());
    return import_paths;
}

void append_import_maps(std::unordered_map<std::string, std::string>& into,
                        const std::unordered_map<std::string, std::string>& extra) {
    for (const auto& [name, path] : extra) {
        auto [it, inserted] = into.emplace(name, path);
        if (!inserted && it->second != path) {
            throw BuildError("module '" + name + "' is produced by multiple local targets");
        }
    }
}

std::filesystem::path output_root_for(const ManifestData& manifest, std::string_view profile_name) {
    std::filesystem::path manifest_dir = manifest.manifest_path.parent_path();
    return manifest_dir / ".scpp" / "build" / scpp::host_target_triple() / std::string(profile_name) /
           manifest.package_name;
}

void write_metadata_file(const ManifestData& manifest, std::string_view profile_name,
                         const BuildOutputs& outputs, const std::filesystem::path& metadata_path) {
    std::ostringstream json;
    json << "{\n";
    json << "  \"package\": \"" << escape_json(manifest.package_name) << "\",\n";
    json << "  \"profile\": \"" << escape_json(profile_name) << "\",\n";
    json << "  \"triple\": \"" << escape_json(scpp::host_target_triple()) << "\",\n";
    json << "  \"modules\": [\n";
    for (size_t i = 0; i < outputs.library_modules.size(); i++) {
        const BuiltModule& module = outputs.library_modules[i];
        json << "    {\"name\": \"" << escape_json(module.name) << "\", \"interface\": \""
             << escape_json(module.interface_path.string()) << "\", \"archive\": \""
             << escape_json(module.archive_path.string()) << "\"}";
        if (i + 1 < outputs.library_modules.size()) json << ",";
        json << "\n";
    }
    json << "  ],\n";
    json << "  \"binaries\": [\n";
    for (size_t i = 0; i < outputs.binaries.size(); i++) {
        json << "    \"" << escape_json(outputs.binaries[i].string()) << "\"";
        if (i + 1 < outputs.binaries.size()) json << ",";
        json << "\n";
    }
    json << "  ]\n";
    json << "}\n";
    write_file(metadata_path, json.str());
}

void build_binary_target(const ManifestData& manifest, const ManifestTarget& bin_target,
                         const std::vector<BuiltModule>& library_modules,
                         const scpp::ProjectBuildOptions& options,
                         const ProfileSettings& profile,
                         const std::filesystem::path& package_root,
                         std::vector<std::filesystem::path>& binary_outputs) {
    std::filesystem::path manifest_dir = manifest.manifest_path.parent_path();
    std::vector<SourceInfo> sources = classify_target_sources(manifest_dir, bin_target);
    std::filesystem::path root = std::filesystem::weakly_canonical(manifest_dir / bin_target.root);

    std::filesystem::path module_dir = package_root / "modules";
    std::filesystem::path archive_dir = package_root / "archives";
    std::filesystem::path object_dir = package_root / "objects" / sanitize_filename(bin_target.name);
    std::filesystem::create_directories(object_dir);

    std::unordered_map<std::string, std::string> import_paths = to_import_map(library_modules);
    std::unordered_set<std::string> library_source_paths;
    for (const BuiltModule& module : library_modules) library_source_paths.insert(module.source_path.string());
    std::vector<SourceInfo> local_module_sources;
    local_module_sources.reserve(sources.size());
    for (const SourceInfo& source : sources) {
        if ((source.kind == SourceInfo::Kind::PrimaryInterface || source.kind == SourceInfo::Kind::InterfacePartition ||
             source.kind == SourceInfo::Kind::ImplementationPartition) &&
            library_source_paths.contains(source.path.string())) {
            continue;
        }
        local_module_sources.push_back(source);
    }
    std::vector<BuiltModule> local_modules = build_modules_for_target(local_module_sources, module_dir, archive_dir,
                                                                      import_paths, profile.opt_level);
    append_import_maps(import_paths, to_import_map(local_modules));

    std::vector<std::filesystem::path> extra_objects;
    size_t plain_index = 0;
    for (const SourceInfo& source : sources) {
        if (source.path == root || source.kind != SourceInfo::Kind::Plain) continue;
        std::filesystem::path object_path = object_dir / (std::to_string(plain_index++) + "_" +
                                                          sanitize_filename(source.path.filename().string()) + ".o");
        std::string source_text = read_file(source.path);
        try {
            scpp::emit_object_file(source_text, object_path.string(), import_paths, {}, profile.debug,
                                   source.path.string(), profile.opt_level);
        } catch (const scpp::ParseError& e) {
            print_diagnostic(source.path.string(), source_text, e.loc, e.what());
            throw;
        } catch (const scpp::DataflowError& e) {
            print_diagnostic(source.path.string(), source_text, e.loc, e.what());
            throw;
        } catch (const scpp::CodegenError& e) {
            print_diagnostic(source.path.string(), source_text, e.loc, e.what());
            throw;
        } catch (const scpp::DriverError& e) {
            print_diagnostic(source.path.string(), source_text, scpp::SourceLocation{}, e.what());
            throw;
        }
        extra_objects.push_back(object_path);
    }

    std::vector<std::string> extra_link_inputs;
    for (const std::filesystem::path& path : extra_objects) extra_link_inputs.push_back(path.string());
    std::filesystem::path executable_path = package_root / bin_target.name;
    std::string root_source = read_file(root);
    try {
        scpp::compile_to_executable(root_source, executable_path.string(), extra_link_inputs, import_paths,
                                    profile.static_link, {}, profile.debug, root.string(), profile.opt_level);
    } catch (const scpp::ParseError& e) {
        print_diagnostic(root.string(), root_source, e.loc, e.what());
        throw;
    } catch (const scpp::DataflowError& e) {
        print_diagnostic(root.string(), root_source, e.loc, e.what());
        throw;
    } catch (const scpp::CodegenError& e) {
        print_diagnostic(root.string(), root_source, e.loc, e.what());
        throw;
    } catch (const scpp::DriverError& e) {
        print_diagnostic(root.string(), root_source, scpp::SourceLocation{}, e.what());
        throw;
    }
    binary_outputs.push_back(executable_path);
}

} // namespace

export namespace scpp {
std::optional<std::filesystem::path> find_project_manifest(const std::filesystem::path& start_dir) {
    return find_project_manifest_impl(start_dir);
}

int build_manifest_project(const std::filesystem::path& start_dir, const ProjectBuildOptions& options) {
    std::optional<std::filesystem::path> manifest_path = find_project_manifest_impl(start_dir);
    if (!manifest_path.has_value()) {
        std::cerr << "error: no scpp.toml found in the current directory or any parent directory\n";
        return 1;
    }

    try {
        ManifestData manifest = parse_manifest(*manifest_path);
        std::string profile_name = options.release ? "release" : options.selected_profile.value_or("dev");
        auto profile_it = manifest.profiles.find(profile_name);
        if (profile_it == manifest.profiles.end()) {
            throw ManifestError("unknown profile '" + profile_name + "'");
        }
        ProfileSettings profile = profile_it->second;
        std::filesystem::path package_root = output_root_for(manifest, profile_name);
        std::filesystem::create_directories(package_root / "modules");
        std::filesystem::create_directories(package_root / "archives");
        std::filesystem::create_directories(package_root / "objects");

        BuildOutputs outputs;
        if (manifest.lib_target.has_value()) {
            std::vector<SourceInfo> lib_sources = classify_target_sources(manifest.manifest_path.parent_path(), *manifest.lib_target);
            outputs.library_modules = build_modules_for_target(lib_sources, package_root / "modules",
                                                               package_root / "archives", {}, profile.opt_level);
        } else if (options.build_lib_only) {
            throw ManifestError("manifest has no [lib] target");
        }

        if (!options.build_lib_only) {
            if (options.selected_bin.has_value()) {
                auto it = std::find_if(manifest.bin_targets.begin(), manifest.bin_targets.end(),
                                       [&](const ManifestTarget& target) { return target.name == *options.selected_bin; });
                if (it == manifest.bin_targets.end()) {
                    throw ManifestError("unknown [[bin]] target '" + *options.selected_bin + "'");
                }
                build_binary_target(manifest, *it, outputs.library_modules, options, profile, package_root,
                                    outputs.binaries);
            } else {
                for (const ManifestTarget& bin_target : manifest.bin_targets) {
                    build_binary_target(manifest, bin_target, outputs.library_modules, options, profile,
                                        package_root, outputs.binaries);
                }
            }
        }

        write_metadata_file(manifest, profile_name, outputs, package_root / "package-metadata.json");
        return 0;
    } catch (const ManifestError& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    } catch (const BuildError& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

} // namespace scpp
