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
    std::optional<std::string> selected_package;
    bool release = false;
    bool build_workspace = false;
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

std::filesystem::path normalized_path(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) return std::filesystem::absolute(path).lexically_normal();
    return canonical;
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
    std::string value = trim(text);
    if (value.empty()) throw ManifestError(context + " must be an integer");
    size_t parsed = 0;
    try {
        int result = std::stoi(value, &parsed);
        if (parsed != value.size()) throw ManifestError(context + " must be an integer");
        return result;
    } catch (const std::invalid_argument&) {
        throw ManifestError(context + " must be an integer");
    } catch (const std::out_of_range&) {
        throw ManifestError(context + " must be an integer");
    }
}

std::vector<std::string> split_top_level(std::string_view text, char delimiter) {
    std::vector<std::string> parts;
    size_t start = 0;
    bool in_string = false;
    bool escape = false;
    int brace_depth = 0;
    int bracket_depth = 0;
    for (size_t i = 0; i < text.size(); i++) {
        char ch = text[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (ch == '\\' && in_string) {
            escape = true;
            continue;
        }
        if (ch == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) continue;
        if (ch == '{') brace_depth++;
        else if (ch == '}') brace_depth--;
        else if (ch == '[') bracket_depth++;
        else if (ch == ']') bracket_depth--;
        else if (ch == delimiter && brace_depth == 0 && bracket_depth == 0) {
            parts.push_back(trim(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    std::string tail = trim(text.substr(start));
    if (!tail.empty()) parts.push_back(std::move(tail));
    return parts;
}

std::vector<std::string> parse_string_array(std::string_view text, const std::string& context) {
    std::string value = trim(text);
    if (value.size() < 2 || value.front() != '[' || value.back() != ']') {
        throw ManifestError(context + " must be an array of strings");
    }
    std::vector<std::string> entries = split_top_level(std::string_view(value).substr(1, value.size() - 2), ',');
    std::vector<std::string> items;
    items.reserve(entries.size());
    for (const std::string& entry : entries) {
        items.push_back(parse_string_literal(entry, context));
    }
    return items;
}

std::unordered_map<std::string, std::string> parse_inline_table(std::string_view text, const std::string& context) {
    std::string value = trim(text);
    if (value.size() < 2 || value.front() != '{' || value.back() != '}') {
        throw ManifestError(context + " must be an inline table");
    }
    std::vector<std::string> entries = split_top_level(std::string_view(value).substr(1, value.size() - 2), ',');
    std::unordered_map<std::string, std::string> table;
    for (const std::string& entry : entries) {
        size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            throw ManifestError(context + " contains malformed inline table entry '" + entry + "'");
        }
        std::string key = trim(entry.substr(0, eq));
        std::string raw_value = trim(entry.substr(eq + 1));
        if (key.empty()) throw ManifestError(context + " contains an empty inline table key");
        table.emplace(std::move(key), std::move(raw_value));
    }
    return table;
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
        if (std::isalnum(ch)) out.push_back(static_cast<char>(ch));
        else out.push_back('_');
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
    std::string line_num = std::to_string(loc.line);
    std::string gutter(line_num.size(), ' ');
    std::cerr << " " << line_num << " | " << line_text << "\n";
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

struct DependencySpec {
    std::string alias;
    std::filesystem::path path;
};

struct WorkspaceConfig {
    std::vector<std::filesystem::path> members;
    std::vector<std::filesystem::path> default_members;
    bool has_default_members = false;
    bool has_workspace_dependencies = false;
};

struct ManifestTarget {
    std::string name;
    std::filesystem::path root;
    std::vector<std::string> source_patterns;
};

struct ManifestData {
    int manifest_version = -1;
    std::optional<std::string> package_name;
    std::optional<std::string> package_version;
    std::optional<ManifestTarget> lib_target;
    std::vector<ManifestTarget> bin_targets;
    std::map<std::string, ProfileSettings> profiles;
    std::unordered_set<std::string> explicit_profiles;
    std::filesystem::path manifest_path;
    std::optional<WorkspaceConfig> workspace;
    std::vector<DependencySpec> dependencies;
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

struct WorkspaceInfo {
    ManifestData manifest;
    std::vector<ManifestData> member_manifests;
    std::vector<std::filesystem::path> default_package_manifests;
};

struct PackageBuildResult {
    ManifestData manifest;
    std::filesystem::path package_output_root;
    std::vector<BuiltModule> library_modules;
    std::vector<std::filesystem::path> binaries;
    std::unordered_map<std::string, std::string> exported_modules;
    std::unordered_map<std::string, std::string> closure_import_paths;
    std::unordered_map<std::string, std::string> closure_module_owners;
    std::vector<std::filesystem::path> archive_closure;
};

struct ProjectDiscovery {
    std::vector<ManifestData> manifests_nearest_first;
    std::optional<ManifestData> current_manifest;
    std::optional<ManifestData> workspace_manifest;
};

ManifestData parse_manifest(const std::filesystem::path& manifest_path) {
    ManifestData manifest;
    manifest.manifest_path = normalized_path(manifest_path);
    manifest.profiles["dev"] = ProfileSettings{0, true, false};
    manifest.profiles["release"] = ProfileSettings{3, false, false};

    std::ifstream input(manifest.manifest_path);
    if (!input) throw ManifestError("cannot open manifest '" + manifest.manifest_path.string() + "'");

    enum class Section {
        Root,
        Package,
        Lib,
        Bin,
        Profile,
        Dependencies,
        Native,
        Workspace,
        WorkspaceDependencies,
        PackageMetadata,
        Ignored,
    };

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
                if (stripped.size() < 4 || stripped.substr(stripped.size() - 2) != "]]" ) {
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
                if (!manifest.workspace.has_value()) manifest.workspace = WorkspaceConfig{};
            } else if (section_name == "workspace.dependencies") {
                current_section = Section::WorkspaceDependencies;
                if (!manifest.workspace.has_value()) manifest.workspace = WorkspaceConfig{};
                manifest.workspace->has_workspace_dependencies = true;
            } else if (section_name == "package.metadata") {
                current_section = Section::PackageMetadata;
            } else if (section_name.rfind("profile.", 0) == 0) {
                current_section = Section::Profile;
                current_profile = section_name.substr(std::string("profile.").size());
                if (current_profile.empty()) {
                    throw ManifestError(manifest.manifest_path.string() + ":" + std::to_string(line_number) +
                                        ": profile section name cannot be empty");
                }
                manifest.explicit_profiles.insert(current_profile);
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
            case Section::Dependencies: {
                std::unordered_map<std::string, std::string> table = parse_inline_table(value, context);
                if (table.contains("path")) {
                    DependencySpec dep;
                    dep.alias = key;
                    dep.path = parse_string_literal(table.at("path"), context + " path");
                    if (table.size() != 1) {
                        throw ManifestError(context + " currently supports only { path = \"...\" }");
                    }
                    manifest.dependencies.push_back(std::move(dep));
                } else if (table.contains("scppkg") || table.contains("workspace") || table.contains("git") ||
                           table.contains("version")) {
                    throw ManifestError(context + " uses a dependency source that is designed but not implemented yet");
                } else {
                    throw ManifestError(context + " must specify { path = \"...\" }");
                }
                break;
            }
            case Section::Native:
                manifest.has_native = true;
                break;
            case Section::Workspace:
                if (key == "members") {
                    manifest.workspace->members.clear();
                    for (const std::string& member : parse_string_array(value, context)) {
                        manifest.workspace->members.emplace_back(member);
                    }
                } else if (key == "default-members") {
                    manifest.workspace->has_default_members = true;
                    manifest.workspace->default_members.clear();
                    for (const std::string& member : parse_string_array(value, context)) {
                        manifest.workspace->default_members.emplace_back(member);
                    }
                } else {
                    throw ManifestError(context + " is not supported in [workspace]");
                }
                break;
            case Section::WorkspaceDependencies:
                manifest.workspace->has_workspace_dependencies = true;
                break;
            case Section::PackageMetadata:
            case Section::Ignored:
                break;
        }
    }

    if (manifest.manifest_version != 1) {
        throw ManifestError("manifest-version = 1 is required in '" + manifest.manifest_path.string() + "'");
    }
    if (manifest.has_native) {
        throw ManifestError("[native] is designed but not implemented yet");
    }
    if (manifest.workspace.has_value() && manifest.workspace->has_workspace_dependencies) {
        throw ManifestError("[workspace.dependencies] is designed but not implemented yet");
    }
    if (!manifest.package_name.has_value() && !manifest.workspace.has_value()) {
        throw ManifestError("manifest must declare either [package] or [workspace]");
    }
    if (!manifest.package_name.has_value()) {
        if (manifest.lib_target.has_value() || !manifest.bin_targets.empty() || !manifest.dependencies.empty()) {
            throw ManifestError("a virtual workspace manifest cannot declare [lib], [[bin]], or [dependencies]");
        }
        return manifest;
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
    if (std::filesystem::exists(root)) paths.insert(normalized_path(root));
    std::filesystem::recursive_directory_iterator end;
    for (std::filesystem::recursive_directory_iterator it(base_dir, ec); it != end && !ec; it.increment(ec)) {
        if (ec || !it->is_regular_file()) continue;
        std::filesystem::path relative = std::filesystem::relative(it->path(), base_dir, ec);
        if (ec) continue;
        std::string candidate = relative.generic_string();
        for (const std::regex& matcher : matchers) {
            if (std::regex_match(candidate, matcher)) {
                paths.insert(normalized_path(it->path()));
                break;
            }
        }
    }
    return std::vector<std::filesystem::path>(paths.begin(), paths.end());
}

SourceInfo classify_source(const std::filesystem::path& path) {
    SourceInfo info;
    info.path = normalized_path(path);
    std::string source = read_file(info.path);
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
                    throw BuildError("invalid partition declaration in '" + info.path.string() + "'");
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
    std::filesystem::path root = normalized_path(manifest_dir / target.root);
    if (!std::filesystem::exists(root)) {
        throw BuildError("target root '" + (manifest_dir / target.root).string() + "' does not exist");
    }
    std::vector<std::filesystem::path> source_paths = expand_source_patterns(manifest_dir, target.source_patterns, root);
    bool found_root = false;
    std::vector<SourceInfo> sources;
    for (const std::filesystem::path& source_path : source_paths) {
        SourceInfo info = classify_source(source_path);
        if (normalized_path(source_path) == root) found_root = true;
        sources.push_back(std::move(info));
    }
    if (!found_root) {
        throw BuildError("target root '" + root.string() + "' is not covered by the declared sources globs");
    }
    return sources;
}

std::unordered_set<std::string> local_primary_module_names(const std::vector<SourceInfo>& sources) {
    std::unordered_set<std::string> names;
    for (const SourceInfo& source : sources) {
        if (source.kind == SourceInfo::Kind::PrimaryInterface) names.insert(source.module_name);
    }
    return names;
}

void validate_direct_visibility(const std::vector<SourceInfo>& sources,
                                const std::unordered_set<std::string>& local_modules,
                                const std::unordered_map<std::string, std::string>& direct_modules,
                                const std::unordered_map<std::string, std::string>& transitive_only_modules) {
    for (const SourceInfo& source : sources) {
        for (const std::string& imported : source.imported_modules) {
            if (imported == "std") continue;
            if (local_modules.contains(imported)) continue;
            if (direct_modules.contains(imported)) continue;
            auto transitive = transitive_only_modules.find(imported);
            if (transitive != transitive_only_modules.end()) {
                throw BuildError("module '" + imported + "' is exported only by transitive dependency package '" +
                                 transitive->second + "'; add it as a direct dependency to import it");
            }
        }
    }
}

std::vector<BuiltModule> build_modules_for_target(const std::vector<SourceInfo>& sources,
                                                  const std::filesystem::path& module_dir,
                                                  const std::filesystem::path& archive_dir,
                                                  const std::unordered_map<std::string, std::string>& base_import_paths,
                                                  int opt_level) {
    std::map<std::string, SourceInfo> primary_modules;
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
                        const std::unordered_map<std::string, std::string>& extra,
                        const std::unordered_map<std::string, std::string>* owner_lookup = nullptr) {
    for (const auto& [name, path] : extra) {
        auto [it, inserted] = into.emplace(name, path);
        if (!inserted && it->second != path) {
            std::string detail;
            if (owner_lookup != nullptr) {
                auto owner = owner_lookup->find(name);
                if (owner != owner_lookup->end()) detail = " from package '" + owner->second + "'";
            }
            throw BuildError("module '" + name + "' is produced by multiple direct dependencies" + detail);
        }
    }
}

void append_unique_paths(std::vector<std::filesystem::path>& into, const std::vector<std::filesystem::path>& extra) {
    for (const std::filesystem::path& path : extra) {
        if (std::find(into.begin(), into.end(), path) == into.end()) into.push_back(path);
    }
}

std::vector<std::filesystem::path> manifests_upward(const std::filesystem::path& start_dir) {
    std::vector<std::filesystem::path> manifests;
    std::filesystem::path current = normalized_path(start_dir);
    while (true) {
        std::filesystem::path candidate = current / "scpp.toml";
        if (std::filesystem::exists(candidate)) manifests.push_back(normalized_path(candidate));
        if (current == current.root_path()) break;
        std::filesystem::path parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return manifests;
}

ProjectDiscovery discover_project(const std::filesystem::path& start_dir) {
    ProjectDiscovery discovery;
    for (const std::filesystem::path& manifest_path : manifests_upward(start_dir)) {
        ManifestData manifest = parse_manifest(manifest_path);
        if (!discovery.current_manifest.has_value()) discovery.current_manifest = manifest;
        if (!discovery.workspace_manifest.has_value() && manifest.workspace.has_value()) {
            discovery.workspace_manifest = manifest;
        }
        discovery.manifests_nearest_first.push_back(std::move(manifest));
    }
    return discovery;
}

ManifestData load_package_manifest(const std::filesystem::path& manifest_path) {
    ManifestData manifest = parse_manifest(manifest_path);
    if (!manifest.package_name.has_value()) {
        throw ManifestError("dependency manifest '" + manifest.manifest_path.string() + "' does not declare [package]");
    }
    return manifest;
}

WorkspaceInfo load_workspace(const ManifestData& workspace_manifest) {
    WorkspaceInfo workspace;
    workspace.manifest = workspace_manifest;
    std::filesystem::path root_dir = workspace_manifest.manifest_path.parent_path();
    std::unordered_map<std::string, std::filesystem::path> seen_names;

    auto add_member = [&](const ManifestData& manifest) {
        if (!manifest.package_name.has_value()) {
            throw ManifestError("workspace member '" + manifest.manifest_path.string() + "' must declare [package]");
        }
        auto [it, inserted] = seen_names.emplace(*manifest.package_name, manifest.manifest_path);
        if (!inserted) {
            throw ManifestError("workspace contains duplicate package name '" + *manifest.package_name + "'");
        }
        workspace.member_manifests.push_back(manifest);
    };

    if (workspace_manifest.package_name.has_value()) add_member(workspace_manifest);

    for (const std::filesystem::path& member_path : workspace_manifest.workspace->members) {
        std::filesystem::path member_manifest_path = normalized_path(root_dir / member_path / "scpp.toml");
        if (!std::filesystem::exists(member_manifest_path)) {
            throw ManifestError("workspace member '" + (root_dir / member_path).string() + "' has no scpp.toml");
        }
        ManifestData member_manifest = load_package_manifest(member_manifest_path);
        if (member_manifest.manifest_path == workspace_manifest.manifest_path) continue;
        if (member_manifest.workspace.has_value()) {
            throw ManifestError("nested workspace member '" + member_manifest.manifest_path.string() +
                                "' is not supported");
        }
        add_member(member_manifest);
    }

    auto resolve_default_member = [&](const std::filesystem::path& relative_path) -> std::filesystem::path {
        std::filesystem::path candidate = normalized_path(root_dir / relative_path / "scpp.toml");
        if (relative_path == "." || relative_path.empty()) candidate = workspace_manifest.manifest_path;
        auto it = std::find_if(workspace.member_manifests.begin(), workspace.member_manifests.end(),
                               [&](const ManifestData& manifest) { return manifest.manifest_path == candidate; });
        if (it == workspace.member_manifests.end()) {
            throw ManifestError("default workspace member '" + relative_path.string() + "' is not a declared workspace package");
        }
        return candidate;
    };

    if (workspace_manifest.workspace->has_default_members) {
        for (const std::filesystem::path& member : workspace_manifest.workspace->default_members) {
            workspace.default_package_manifests.push_back(resolve_default_member(member));
        }
    } else if (workspace_manifest.package_name.has_value()) {
        workspace.default_package_manifests.push_back(workspace_manifest.manifest_path);
    } else {
        for (const ManifestData& manifest : workspace.member_manifests) {
            workspace.default_package_manifests.push_back(manifest.manifest_path);
        }
    }
    return workspace;
}

ProfileSettings resolve_profile(const ManifestData& package_manifest,
                                const std::optional<ManifestData>& workspace_manifest,
                                const std::string& profile_name) {
    auto package_it = package_manifest.profiles.find(profile_name);
    if (package_it == package_manifest.profiles.end()) {
        throw ManifestError("unknown profile '" + profile_name + "'");
    }
    ProfileSettings profile = package_it->second;
    if (workspace_manifest.has_value() && workspace_manifest->explicit_profiles.contains(profile_name)) {
        auto workspace_it = workspace_manifest->profiles.find(profile_name);
        if (workspace_it == workspace_manifest->profiles.end()) {
            throw ManifestError("unknown workspace profile '" + profile_name + "'");
        }
        profile = workspace_it->second;
    }
    return profile;
}

std::filesystem::path package_output_root(const std::filesystem::path& shared_root_dir,
                                          std::string_view profile_name,
                                          std::string_view package_name) {
    return shared_root_dir / ".scpp" / "build" / scpp::host_target_triple() / std::string(profile_name) /
           std::string(package_name);
}

void write_metadata_file(const PackageBuildResult& result, std::string_view profile_name,
                         const std::filesystem::path& metadata_path) {
    std::ostringstream json;
    json << "{\n";
    json << "  \"package\": \"" << escape_json(*result.manifest.package_name) << "\",\n";
    json << "  \"profile\": \"" << escape_json(profile_name) << "\",\n";
    json << "  \"triple\": \"" << escape_json(scpp::host_target_triple()) << "\",\n";
    json << "  \"modules\": [\n";
    for (size_t i = 0; i < result.library_modules.size(); i++) {
        const BuiltModule& module = result.library_modules[i];
        json << "    {\"name\": \"" << escape_json(module.name) << "\", \"interface\": \""
             << escape_json(module.interface_path.string()) << "\", \"archive\": \""
             << escape_json(module.archive_path.string()) << "\"}";
        if (i + 1 < result.library_modules.size()) json << ",";
        json << "\n";
    }
    json << "  ],\n";
    json << "  \"binaries\": [\n";
    for (size_t i = 0; i < result.binaries.size(); i++) {
        json << "    \"" << escape_json(result.binaries[i].string()) << "\"";
        if (i + 1 < result.binaries.size()) json << ",";
        json << "\n";
    }
    json << "  ]\n";
    json << "}\n";
    write_file(metadata_path, json.str());
}

class PackageBuilder {
public:
    PackageBuilder(std::filesystem::path shared_root_dir,
                   std::string profile_name,
                   std::optional<ManifestData> workspace_manifest,
                   std::optional<WorkspaceInfo> workspace_info)
        : shared_root_dir_(normalized_path(shared_root_dir)),
          profile_name_(std::move(profile_name)),
          workspace_manifest_(std::move(workspace_manifest)),
          workspace_info_(std::move(workspace_info)) {}

    PackageBuildResult& build_package(const std::filesystem::path& manifest_path, bool build_binaries,
                                      const scpp::ProjectBuildOptions& options) {
        std::filesystem::path normalized_manifest = normalized_path(manifest_path);
        std::string key = normalized_manifest.string() + "|" + profile_name_;
        if (cache_.contains(key)) {
            if (build_binaries && !cache_.at(key).binaries.empty()) return cache_.at(key);
            if (!build_binaries) return cache_.at(key);
        }
        if (!active_builds_.insert(key).second) {
            throw BuildError("cyclic package dependency involving '" + normalized_manifest.string() + "'");
        }

        ManifestData manifest = load_package_manifest(normalized_manifest);
        ProfileSettings profile = resolve_profile(manifest, workspace_manifest_, profile_name_);
        PackageBuildResult result;
        result.manifest = manifest;
        result.package_output_root = package_output_root(shared_root_dir_, profile_name_, *manifest.package_name);
        std::filesystem::create_directories(result.package_output_root / "modules");
        std::filesystem::create_directories(result.package_output_root / "archives");
        std::filesystem::create_directories(result.package_output_root / "objects");

        std::unordered_map<std::string, std::string> direct_import_paths;
        std::unordered_map<std::string, std::string> full_dependency_import_paths;
        std::unordered_map<std::string, std::string> direct_module_owners;
        std::unordered_map<std::string, std::string> transitive_only_modules;

        for (const DependencySpec& dep : manifest.dependencies) {
            std::filesystem::path dep_manifest_path = normalized_path(manifest.manifest_path.parent_path() / dep.path / "scpp.toml");
            if (!std::filesystem::exists(dep_manifest_path)) {
                throw BuildError("dependency '" + dep.alias + "' path '" +
                                 (manifest.manifest_path.parent_path() / dep.path).string() + "' has no scpp.toml");
            }
            PackageBuildResult& dep_result = build_package(dep_manifest_path, /*build_binaries=*/false,
                                                           scpp::ProjectBuildOptions{});
            if (dep_result.library_modules.empty()) {
                throw BuildError("dependency package '" + *dep_result.manifest.package_name + "' does not provide a [lib] target");
            }
            for (const auto& [module_name, interface_path] : dep_result.exported_modules) {
                auto [it, inserted] = direct_import_paths.emplace(module_name, interface_path);
                if (!inserted && it->second != interface_path) {
                    throw BuildError("module '" + module_name + "' is exported by multiple direct dependencies");
                }
                direct_module_owners.emplace(module_name, *dep_result.manifest.package_name);
            }
            append_import_maps(full_dependency_import_paths, dep_result.exported_modules);
            append_import_maps(full_dependency_import_paths, dep_result.closure_import_paths);
            for (const auto& [module_name, owner] : dep_result.closure_module_owners) {
                if (!direct_import_paths.contains(module_name) && !transitive_only_modules.contains(module_name)) {
                    transitive_only_modules.emplace(module_name, owner);
                }
            }
            for (const BuiltModule& module : dep_result.library_modules) {
                result.archive_closure.push_back(module.archive_path);
            }
            append_unique_paths(result.archive_closure, dep_result.archive_closure);
        }

        std::filesystem::path manifest_dir = manifest.manifest_path.parent_path();
        std::unordered_set<std::string> own_library_module_names;
        if (manifest.lib_target.has_value()) {
            std::vector<SourceInfo> lib_sources = classify_target_sources(manifest_dir, *manifest.lib_target);
            own_library_module_names = local_primary_module_names(lib_sources);
            validate_direct_visibility(lib_sources, own_library_module_names, direct_import_paths, transitive_only_modules);
            result.library_modules = build_modules_for_target(lib_sources, result.package_output_root / "modules",
                                                             result.package_output_root / "archives", full_dependency_import_paths,
                                                             profile.opt_level);
        } else if (options.build_lib_only) {
            throw ManifestError("manifest has no [lib] target");
        }

        result.exported_modules = to_import_map(result.library_modules);
        result.closure_import_paths = result.exported_modules;
        append_import_maps(result.closure_import_paths, full_dependency_import_paths);
        for (const BuiltModule& module : result.library_modules) {
            result.closure_module_owners.emplace(module.name, *manifest.package_name);
        }
        for (const auto& [module_name, owner] : direct_module_owners) {
            if (!result.closure_module_owners.contains(module_name)) result.closure_module_owners.emplace(module_name, owner);
        }
        for (const auto& [module_name, owner] : transitive_only_modules) {
            if (!result.closure_module_owners.contains(module_name)) result.closure_module_owners.emplace(module_name, owner);
        }

        if (build_binaries) {
            if (options.selected_bin.has_value()) {
                auto it = std::find_if(manifest.bin_targets.begin(), manifest.bin_targets.end(),
                                       [&](const ManifestTarget& target) { return target.name == *options.selected_bin; });
                if (it == manifest.bin_targets.end()) {
                    throw ManifestError("unknown [[bin]] target '" + *options.selected_bin + "'");
                }
                build_binary_target(manifest, *it, direct_import_paths, full_dependency_import_paths,
                                    transitive_only_modules, result, profile);
            } else {
                for (const ManifestTarget& bin_target : manifest.bin_targets) {
                    build_binary_target(manifest, bin_target, direct_import_paths, full_dependency_import_paths,
                                        transitive_only_modules, result, profile);
                }
            }
        }

        write_metadata_file(result, profile_name_, result.package_output_root / "package-metadata.json");
        active_builds_.erase(key);
        auto [it, _] = cache_.insert_or_assign(key, std::move(result));
        return it->second;
    }

private:
    void build_binary_target(const ManifestData& manifest,
                             const ManifestTarget& bin_target,
                             const std::unordered_map<std::string, std::string>& direct_dep_import_paths,
                             const std::unordered_map<std::string, std::string>& full_dependency_import_paths,
                             const std::unordered_map<std::string, std::string>& transitive_only_modules,
                             PackageBuildResult& result,
                             const ProfileSettings& profile) {
        std::filesystem::path manifest_dir = manifest.manifest_path.parent_path();
        std::vector<SourceInfo> sources = classify_target_sources(manifest_dir, bin_target);
        std::filesystem::path root = normalized_path(manifest_dir / bin_target.root);

        std::unordered_map<std::string, std::string> base_import_paths = full_dependency_import_paths;
        append_import_maps(base_import_paths, result.exported_modules);

        std::unordered_set<std::string> local_modules = own_module_names(result.library_modules);
        std::unordered_set<std::string> bin_local_names = local_primary_module_names(sources);
        local_modules.insert(bin_local_names.begin(), bin_local_names.end());
        validate_direct_visibility(sources, local_modules, direct_dep_import_paths, transitive_only_modules);

        std::unordered_set<std::string> library_source_paths;
        for (const BuiltModule& module : result.library_modules) library_source_paths.insert(module.source_path.string());
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

        std::filesystem::path module_dir = result.package_output_root / "modules";
        std::filesystem::path archive_dir = result.package_output_root / "archives";
        std::filesystem::path object_dir = result.package_output_root / "objects" / sanitize_filename(bin_target.name);
        std::filesystem::create_directories(object_dir);

        std::vector<BuiltModule> local_modules_built =
            build_modules_for_target(local_module_sources, module_dir, archive_dir, base_import_paths, profile.opt_level);
        std::unordered_map<std::string, std::string> compile_import_paths = base_import_paths;
        append_import_maps(compile_import_paths, to_import_map(local_modules_built));

        std::vector<std::filesystem::path> extra_objects;
        size_t plain_index = 0;
        for (const SourceInfo& source : sources) {
            if (source.path == root || source.kind != SourceInfo::Kind::Plain) continue;
            std::filesystem::path object_path = object_dir / (std::to_string(plain_index++) + "_" +
                                                              sanitize_filename(source.path.filename().string()) + ".o");
            std::string source_text = read_file(source.path);
            try {
                scpp::emit_object_file(source_text, object_path.string(), compile_import_paths, {}, profile.debug,
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
        for (const std::filesystem::path& object_path : extra_objects) extra_link_inputs.push_back(object_path.string());
        for (const std::filesystem::path& archive_path : result.archive_closure) extra_link_inputs.push_back(archive_path.string());

        std::filesystem::path executable_path = result.package_output_root / bin_target.name;
        std::string root_source = read_file(root);
        try {
            scpp::compile_to_executable(root_source, executable_path.string(), extra_link_inputs, compile_import_paths,
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
        result.binaries.push_back(executable_path);
    }

    std::unordered_set<std::string> own_module_names(const std::vector<BuiltModule>& modules) const {
        std::unordered_set<std::string> names;
        for (const BuiltModule& module : modules) names.insert(module.name);
        return names;
    }

    std::filesystem::path shared_root_dir_;
    std::string profile_name_;
    std::optional<ManifestData> workspace_manifest_;
    std::optional<WorkspaceInfo> workspace_info_;
    std::unordered_map<std::string, PackageBuildResult> cache_;
    std::unordered_set<std::string> active_builds_;
};

std::vector<ManifestData> select_workspace_packages(const WorkspaceInfo& workspace,
                                                    const ManifestData* current_manifest,
                                                    const scpp::ProjectBuildOptions& options,
                                                    bool invoked_from_workspace_root) {
    if (options.build_workspace && options.selected_package.has_value()) {
        throw ManifestError("--workspace and --package/-p cannot be used together");
    }
    if (options.selected_bin.has_value() && options.build_workspace) {
        throw ManifestError("--bin cannot be combined with --workspace");
    }

    auto find_by_name = [&](const std::string& name) -> std::optional<ManifestData> {
        for (const ManifestData& manifest : workspace.member_manifests) {
            if (manifest.package_name.has_value() && *manifest.package_name == name) return manifest;
        }
        return std::nullopt;
    };

    if (options.selected_package.has_value()) {
        std::optional<ManifestData> selected = find_by_name(*options.selected_package);
        if (!selected.has_value()) {
            throw ManifestError("workspace has no package named '" + *options.selected_package + "'");
        }
        return {*selected};
    }
    if (options.build_workspace) {
        return workspace.member_manifests;
    }
    if (!invoked_from_workspace_root && current_manifest != nullptr && current_manifest->package_name.has_value()) {
        return {*current_manifest};
    }

    std::vector<ManifestData> selected;
    for (const std::filesystem::path& manifest_path : workspace.default_package_manifests) {
        auto it = std::find_if(workspace.member_manifests.begin(), workspace.member_manifests.end(),
                               [&](const ManifestData& manifest) { return manifest.manifest_path == manifest_path; });
        if (it == workspace.member_manifests.end()) {
            throw ManifestError("workspace default member '" + manifest_path.string() + "' could not be resolved");
        }
        selected.push_back(*it);
    }
    return selected;
}

} // namespace

export namespace scpp {

std::optional<std::filesystem::path> find_project_manifest(const std::filesystem::path& start_dir) {
    std::vector<std::filesystem::path> manifests = manifests_upward(start_dir);
    if (manifests.empty()) return std::nullopt;
    return manifests.front();
}

int build_manifest_project(const std::filesystem::path& start_dir, const ProjectBuildOptions& options) {
    try {
        ProjectDiscovery discovery = discover_project(start_dir);
        if (!discovery.current_manifest.has_value()) {
            std::cerr << "error: no scpp.toml found in the current directory or any parent directory\n";
            return 1;
        }

        std::string profile_name = options.release ? "release" : options.selected_profile.value_or("dev");
        if (options.release && options.selected_profile.has_value()) {
            throw ManifestError("--release and --profile cannot be used together");
        }

        std::optional<WorkspaceInfo> workspace_info;
        std::vector<ManifestData> packages_to_build;
        std::filesystem::path shared_output_root;
        bool invoked_from_workspace_root = false;

        if (discovery.workspace_manifest.has_value()) {
            workspace_info = load_workspace(*discovery.workspace_manifest);
            shared_output_root = discovery.workspace_manifest->manifest_path.parent_path();
            invoked_from_workspace_root = discovery.current_manifest->manifest_path == discovery.workspace_manifest->manifest_path;
            if (!discovery.current_manifest->package_name.has_value() && !invoked_from_workspace_root) {
                throw ManifestError("current manifest is not a package manifest");
            }
            packages_to_build = select_workspace_packages(*workspace_info,
                                                          discovery.current_manifest->package_name.has_value()
                                                              ? &*discovery.current_manifest
                                                              : nullptr,
                                                          options, invoked_from_workspace_root);
        } else {
            if (options.build_workspace || options.selected_package.has_value()) {
                throw ManifestError("--workspace and --package/-p require a workspace root with [workspace]");
            }
            if (!discovery.current_manifest->package_name.has_value()) {
                throw ManifestError("a virtual workspace cannot be built without [workspace] package selection");
            }
            shared_output_root = discovery.current_manifest->manifest_path.parent_path();
            packages_to_build = {*discovery.current_manifest};
        }

        PackageBuilder builder(shared_output_root, profile_name, discovery.workspace_manifest, workspace_info);
        for (const ManifestData& manifest : packages_to_build) {
            builder.build_package(manifest.manifest_path, /*build_binaries=*/!options.build_lib_only, options);
        }
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
