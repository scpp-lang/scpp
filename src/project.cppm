module;

#include <unistd.h>
#include <sqlite3.h>

export module scpp.project;

import std;
import scpp.ast;
import scpp.compiler.codegen;
import scpp.driver;
import scpp.lexer;
import scpp.compiler.movecheck;
import scpp.parser;

export namespace scpp {

struct ProjectBuildOptions {
    bool build_lib_only = false;
    std::optional<std::string> selected_bin;
    std::optional<std::string> selected_lib;
    std::optional<std::string> selected_package;
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

bool trace_enabled() {
    static bool enabled = []() {
        const char* env = std::getenv("SCPP_BUILD_TRACE");
        return env != nullptr && env[0] != '\0' && std::string_view(env) != "0";
    }();
    return enabled;
}

void trace_build(const std::string& message) {
    if (!trace_enabled()) return;
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&time);
    std::cerr << "[scpp-build " << std::put_time(&tm, "%H:%M:%S") << " tid "
              << std::this_thread::get_id() << "] " << message << "\n";
}

std::string trim(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) start++;
    std::size_t end = text.size();
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

[[nodiscard]] std::expected<std::string, ManifestError> parse_string_literal(std::string_view text, const std::string& context) {
    auto parse_multiline_basic_string = [&](std::string_view body) -> std::expected<std::string, ManifestError> {
        std::string inner(body);
        if (inner.starts_with("\r\n")) {
            inner.erase(0, 2);
        } else if (!inner.empty() && inner.front() == '\n') {
            inner.erase(0, 1);
        }

        std::string out;
        out.reserve(inner.size());
        for (std::size_t i = 0; i < inner.size(); i++) {
            char ch = inner[i];
            if (ch == '\\') {
                if (i + 1 < inner.size() && (inner[i + 1] == '\n' || inner[i + 1] == '\r')) {
                    i++;
                    if (inner[i] == '\r' && i + 1 < inner.size() && inner[i + 1] == '\n') i++;
                    while (i + 1 < inner.size() &&
                           (inner[i + 1] == ' ' || inner[i + 1] == '\t' || inner[i + 1] == '\n' ||
                            inner[i + 1] == '\r')) {
                        i++;
                    }
                    continue;
                }
                if (i + 1 >= inner.size()) {
                    return std::unexpected(ManifestError(context + " ends with an incomplete escape sequence"));
                }
                i++;
                switch (inner[i]) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    default: return std::unexpected(ManifestError(context + " contains an unsupported escape sequence"));
                }
                continue;
            }
            out.push_back(ch);
        }
        return out;
    };

    if (text.size() >= 6 && text.substr(0, 3) == "\"\"\"" && text.substr(text.size() - 3) == "\"\"\"") {
        return parse_multiline_basic_string(text.substr(3, text.size() - 6));
    }
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
        return std::unexpected(ManifestError(context + " must be a TOML string"));
    }
    std::string out;
    out.reserve(text.size() - 2);
    bool escape = false;
    for (std::size_t i = 1; i + 1 < text.size(); i++) {
        char ch = text[i];
        if (escape) {
            switch (ch) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                default: return std::unexpected(ManifestError(context + " contains an unsupported escape sequence"));
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
    if (escape) return std::unexpected(ManifestError(context + " ends with an incomplete escape sequence"));
    return out;
}

bool starts_multiline_basic_string(std::string_view text) {
    std::string value = trim(text);
    return value.size() >= 3 && value.substr(0, 3) == "\"\"\"";
}

bool closes_multiline_basic_string(std::string_view text) {
    if (!starts_multiline_basic_string(text)) return false;
    std::string value = trim(text);
    return value.find("\"\"\"", 3) != std::string::npos;
}

bool top_level_delimiters_balanced(std::string_view text) {
    int bracket_depth = 0;
    int brace_depth = 0;
    bool in_string = false;
    bool escape = false;
    for (char ch : text) {
        if (escape) {
            escape = false;
            continue;
        }
        if (in_string) {
            if (ch == '\\') {
                escape = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
            continue;
        }
        if (ch == '[') bracket_depth++;
        else if (ch == ']') bracket_depth--;
        else if (ch == '{') brace_depth++;
        else if (ch == '}') brace_depth--;
    }
    return !in_string && !escape && bracket_depth == 0 && brace_depth == 0;
}

[[nodiscard]] std::expected<std::string, ManifestError> read_manifest_value(std::string value, std::ifstream& input, int& line_number,
                                const std::filesystem::path& manifest_path) {
    value = trim(value);
    bool needs_multiline_string = starts_multiline_basic_string(value) && !closes_multiline_basic_string(value);
    bool needs_balanced_collection =
        (!value.empty() && (value.front() == '[' || value.front() == '{')) && !top_level_delimiters_balanced(value);
    if (!needs_multiline_string && !needs_balanced_collection) return value;

    std::string continued;
    while (std::getline(input, continued)) {
        line_number++;
        value += "\n";
        value += strip_toml_comment(continued);
        if (needs_multiline_string && value.find("\"\"\"", 3) != std::string::npos) return value;
        if (needs_balanced_collection && top_level_delimiters_balanced(value)) return value;
    }
    return std::unexpected(ManifestError(manifest_path.string() + ":" + std::to_string(line_number) +
                        (needs_multiline_string ? ": unterminated multiline TOML string"
                                                : ": unterminated multiline TOML collection")));
}

[[nodiscard]] std::expected<int, ManifestError> parse_int_literal(std::string_view text, const std::string& context) {
    std::string value = trim(text);
    if (value.empty()) return std::unexpected(ManifestError(context + " must be an integer"));
    // std::from_chars (unlike std::stoi) never throws, which matters here:
    // scpp has no exception support at all, so nothing in src/ may rely on
    // try/catch even to interface with a throwing standard-library
    // function. Unlike std::stoi, std::from_chars doesn't accept a leading
    // '+', so strip one manually first to keep accepting the same TOML
    // integer grammar (e.g. "+7") that std::stoi used to.
    std::string_view digits = value;
    if (!digits.empty() && digits.front() == '+') digits.remove_prefix(1);
    int result = 0;
    auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), result);
    if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
        return std::unexpected(ManifestError(context + " must be an integer"));
    }
    return result;
}

std::vector<std::string> split_top_level(std::string_view text, char delimiter) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    bool in_string = false;
    bool escape = false;
    int brace_depth = 0;
    int bracket_depth = 0;
    for (std::size_t i = 0; i < text.size(); i++) {
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

[[nodiscard]] std::expected<std::vector<std::string>, ManifestError> parse_string_array(std::string_view text, const std::string& context) {
    std::string value = trim(text);
    if (value.size() < 2 || value.front() != '[' || value.back() != ']') {
        return std::unexpected(ManifestError(context + " must be an array of strings"));
    }
    std::vector<std::string> entries = split_top_level(std::string_view(value).substr(1, value.size() - 2), ',');
    std::vector<std::string> items;
    items.reserve(entries.size());
    for (const std::string& entry : entries) {
        auto item_result = parse_string_literal(entry, context);
        if (!item_result.has_value()) return std::unexpected(std::move(item_result).error());
        items.push_back(*std::move(item_result));
    }
    return items;
}

[[nodiscard]] std::expected<std::vector<std::string>, ManifestError> parse_string_or_array(std::string_view text, const std::string& context) {
    std::string value = trim(text);
    if (!value.empty() && value.front() == '[') return parse_string_array(value, context);
    auto item_result = parse_string_literal(value, context);
    if (!item_result.has_value()) return std::unexpected(std::move(item_result).error());
    return std::vector<std::string>{*std::move(item_result)};
}

[[nodiscard]] std::expected<std::unordered_map<std::string, std::string>, ManifestError> parse_inline_table(std::string_view text, const std::string& context) {
    std::string value = trim(text);
    if (value.size() < 2 || value.front() != '{' || value.back() != '}') {
        return std::unexpected(ManifestError(context + " must be an inline table"));
    }
    std::vector<std::string> entries = split_top_level(std::string_view(value).substr(1, value.size() - 2), ',');
    std::unordered_map<std::string, std::string> table;
    for (const std::string& entry : entries) {
        std::size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            return std::unexpected(ManifestError(context + " contains malformed inline table entry '" + entry + "'"));
        }
        std::string key = trim(entry.substr(0, eq));
        std::string raw_value = trim(entry.substr(eq + 1));
        if (key.empty()) return std::unexpected(ManifestError(context + " contains an empty inline table key"));
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

[[nodiscard]] std::expected<std::string, BuildError> read_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) return std::unexpected(BuildError("cannot open file '" + path.string() + "'"));
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

[[nodiscard]] std::expected<void, BuildError> write_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream file(path);
    if (!file) return std::unexpected(BuildError("cannot write file '" + path.string() + "'"));
    file << content;
    return {};
}

std::string fnv1a64_hex(std::string_view text) {
    constexpr std::uint64_t offset_basis = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t value = offset_basis;
    for (unsigned char ch : text) {
        value ^= static_cast<std::uint64_t>(ch);
        value *= prime;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

std::string digest_file(const std::filesystem::path& path) {
    // Falls back to an empty digest if the file cannot be read, mirroring
    // path_digest_or_empty's own "doesn't exist" fallback just below: every
    // call site here only ever folds this into a cache *signature* to
    // compare against a previous build's recorded signature, never uses
    // the file's actual content, so a degraded digest merely risks an
    // extra cache-miss rebuild rather than ever serving a stale artifact.
    auto content = read_file(path);
    return content.has_value() ? fnv1a64_hex(*content) : std::string();
}

std::string join_for_digest(const std::vector<std::string>& values) {
    std::ostringstream out;
    for (const std::string& value : values) {
        out << value.size() << ":" << value << ";";
    }
    return out.str();
}

std::string path_digest_or_empty(const std::filesystem::path& path) {
    return std::filesystem::exists(path) ? digest_file(path) : std::string();
}

std::vector<std::string> path_digests(const std::vector<std::filesystem::path>& paths) {
    std::vector<std::string> digests;
    digests.reserve(paths.size());
    for (const std::filesystem::path& path : paths) {
        digests.push_back(path.string() + "#" + path_digest_or_empty(path));
    }
    return digests;
}

struct ManifestData;

struct BuildRecord {
    std::string key;
    std::string kind;
    std::string signature;
    std::string interface_digest;
    std::string archive_digest;
    std::string output_digest;
    std::string output_path;
    std::string manifest_digest;
    std::string compiler_version;
    std::string triple;
};

// BuildDatabase's constructor deliberately performs only infallible
// initialization (storing the normalized path). A C++ constructor cannot
// return std::expected, and this class can't switch to a
// factory-function-returning-by-value pattern either, because it holds a
// std::mutex directly (needed so build_modules_for_target's std::async
// worker threads can call get()/put() concurrently), and std::mutex is
// neither copyable nor movable. So the fallible part of construction
// (creating the on-disk cache directory, opening the sqlite3 handle, and
// creating the build_records table) is done by the separate open() method
// below, which callers must invoke exactly once right after constructing
// and before any other method.
class BuildDatabase {
public:
    explicit BuildDatabase(const std::filesystem::path& db_path) : db_path_(normalized_path(db_path)) {}

    BuildDatabase(const BuildDatabase&) = delete;
    BuildDatabase& operator=(const BuildDatabase&) = delete;

    ~BuildDatabase() {
        if (db_ != nullptr) sqlite3_close(db_);
    }

    [[nodiscard]] std::expected<void, BuildError> open() {
        std::error_code ec;
        std::filesystem::create_directories(db_path_.parent_path(), ec);
        if (ec) {
            return std::unexpected(BuildError("cannot create build database directory '" +
                                               db_path_.parent_path().string() + "': " + ec.message()));
        }
        if (sqlite3_open(db_path_.string().c_str(), &db_) != SQLITE_OK || db_ == nullptr) {
            std::string message = "cannot open build database '" + db_path_.string() + "'";
            if (db_ != nullptr) message += ": " + std::string(sqlite3_errmsg(db_));
            return std::unexpected(BuildError(message));
        }
        if (auto journal_result = exec("PRAGMA journal_mode=WAL;"); !journal_result.has_value()) {
            return journal_result;
        }
        return exec("CREATE TABLE IF NOT EXISTS build_records ("
                     "key TEXT PRIMARY KEY,"
                     "kind TEXT NOT NULL,"
                     "signature TEXT NOT NULL,"
                     "interface_digest TEXT,"
                     "archive_digest TEXT,"
                     "output_digest TEXT,"
                     "output_path TEXT,"
                     "manifest_digest TEXT,"
                     "compiler_version TEXT,"
                     "triple TEXT"
                     ");");
    }

    [[nodiscard]] std::expected<std::optional<BuildRecord>, BuildError> get(const std::string& key) {
        std::lock_guard lock(mutex_);
        auto prepare_result = prepare(
            "SELECT key, kind, signature, interface_digest, archive_digest, output_digest, output_path, "
            "manifest_digest, compiler_version, triple FROM build_records WHERE key = ?1");
        if (!prepare_result.has_value()) return std::unexpected(std::move(prepare_result).error());
        StatementHandle stmt(*prepare_result, &sqlite3_finalize);
        if (auto bind_result = bind_text(stmt.get(), 1, key); !bind_result.has_value()) {
            return std::unexpected(std::move(bind_result).error());
        }
        int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_ROW) {
            return std::optional<BuildRecord>(BuildRecord{
                column_text(stmt.get(), 0), column_text(stmt.get(), 1), column_text(stmt.get(), 2),
                column_text(stmt.get(), 3), column_text(stmt.get(), 4), column_text(stmt.get(), 5),
                column_text(stmt.get(), 6), column_text(stmt.get(), 7), column_text(stmt.get(), 8),
                column_text(stmt.get(), 9),
            });
        }
        if (rc != SQLITE_DONE) {
            return std::unexpected(BuildError("build database query failed: " + std::string(sqlite3_errmsg(db_))));
        }
        return std::optional<BuildRecord>(std::nullopt);
    }

    [[nodiscard]] std::expected<void, BuildError> put(const BuildRecord& record) {
        std::lock_guard lock(mutex_);
        auto prepare_result = prepare(
            "INSERT INTO build_records "
            "(key, kind, signature, interface_digest, archive_digest, output_digest, output_path, "
            "manifest_digest, compiler_version, triple) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10) "
            "ON CONFLICT(key) DO UPDATE SET "
            "kind=excluded.kind, signature=excluded.signature, interface_digest=excluded.interface_digest, "
            "archive_digest=excluded.archive_digest, output_digest=excluded.output_digest, "
            "output_path=excluded.output_path, manifest_digest=excluded.manifest_digest, "
            "compiler_version=excluded.compiler_version, triple=excluded.triple");
        if (!prepare_result.has_value()) return std::unexpected(std::move(prepare_result).error());
        StatementHandle stmt(*prepare_result, &sqlite3_finalize);
        const std::array<std::string, 10> columns = {
            record.key,          record.kind,           record.signature,     record.interface_digest,
            record.archive_digest, record.output_digest, record.output_path, record.manifest_digest,
            record.compiler_version, record.triple,
        };
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (auto bind_result = bind_text(stmt.get(), static_cast<int>(i) + 1, columns[i]);
                !bind_result.has_value()) {
                return std::unexpected(std::move(bind_result).error());
            }
        }
        int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) {
            return std::unexpected(BuildError("build database write failed: " + std::string(sqlite3_errmsg(db_))));
        }
        return {};
    }

private:
    using StatementHandle = std::unique_ptr<sqlite3_stmt, int (*)(sqlite3_stmt*)>;

    [[nodiscard]] std::expected<void, BuildError> exec(const char* sql) {
        std::lock_guard lock(mutex_);
        char* error = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            std::string message = "build database initialization failed";
            if (error != nullptr) {
                message += ": ";
                message += error;
                sqlite3_free(error);
            }
            return std::unexpected(BuildError(message));
        }
        return {};
    }

    [[nodiscard]] std::expected<sqlite3_stmt*, BuildError> prepare(const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK || stmt == nullptr) {
            return std::unexpected(
                BuildError("build database statement preparation failed: " + std::string(sqlite3_errmsg(db_))));
        }
        return stmt;
    }

    [[nodiscard]] std::expected<void, BuildError> bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
        if (sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
            return std::unexpected(BuildError("build database bind failed: " + std::string(sqlite3_errmsg(db_))));
        }
        return {};
    }

    std::string column_text(sqlite3_stmt* stmt, int index) const {
        const unsigned char* text = sqlite3_column_text(stmt, index);
        return text == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(text));
    }

    std::filesystem::path db_path_;
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};

std::string manifest_digest(const ManifestData& manifest);
std::string compiler_version_key();

void print_diagnostic(std::string_view path, const std::string& source, scpp::SourceLocation loc,
                      const std::string& message) {
    std::cerr << path << ":";
    if (loc.is_known()) std::cerr << loc.line << ":" << loc.column << ":";
    std::cerr << " error: " << message << "\n";
    if (!loc.is_known()) return;
    std::size_t line_start = 0;
    int current_line = 1;
    while (current_line < loc.line) {
        std::size_t next_nl = source.find('\n', line_start);
        if (next_nl == std::string::npos) return;
        line_start = next_nl + 1;
        current_line++;
    }
    std::size_t line_end = source.find('\n', line_start);
    if (line_end == std::string::npos) line_end = source.size();
    std::string_view line_text(source.data() + line_start, line_end - line_start);
    std::string line_num = std::to_string(loc.line);
    std::string gutter(line_num.size(), ' ');
    std::cerr << " " << line_num << " | " << line_text << "\n";
    std::cerr << " " << gutter << " | ";
    for (int i = 0; i < loc.column - 1 && static_cast<std::size_t>(i) < line_text.size(); i++) {
        std::cerr << (line_text[static_cast<std::size_t>(i)] == '\t' ? '\t' : ' ');
    }
    std::cerr << "^\n";
}

// scpp's manifest-driven builds used to expose a Cargo-style `[profile.*]`
// system (`dev`/`release`/custom profiles selecting opt-level, debug info,
// and opt-in static linking). That system was removed as underdesigned --
// v1 needed something much simpler. Manifest-driven builds now always use
// one fixed configuration: the values below match what the former `dev`
// profile used, since that is what this whole project-build feature has
// actually been built and tested against so far. Static linking is no
// longer profile-gated at all; see `link_executable` in driver.cppm, which
// now links statically unconditionally.
constexpr int kManifestBuildOptLevel = 0;
constexpr bool kManifestBuildEmitDebugInfo = true;

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

struct NativeRequirements {
    std::vector<std::string> links;
    std::vector<std::filesystem::path> search_paths;
};

struct CustomCommand {
    std::vector<std::filesystem::path> input_paths;
    std::vector<std::filesystem::path> output_paths;
    std::string command;
};

struct ManifestTarget {
    std::string name;
    std::vector<std::string> source_patterns;
    std::vector<std::string> additional_obj_steps;
};

struct ManifestData {
    int manifest_version = -1;
    std::optional<std::string> package_name;
    std::optional<std::string> package_version;
    std::vector<ManifestTarget> lib_targets;
    std::vector<ManifestTarget> bin_targets;
    std::map<std::string, CustomCommand> custom_commands;
    std::filesystem::path manifest_path;
    std::optional<WorkspaceConfig> workspace;
    std::vector<DependencySpec> dependencies;
    NativeRequirements native;
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

struct ScannedModuleDecl {
    std::string module_name;
    std::string partition_name;
    bool is_interface = false;
};

struct BuiltModule {
    std::string name;
    std::filesystem::path source_path;
    std::filesystem::path interface_path;
    std::filesystem::path archive_path;
    std::string interface_digest;
    std::string archive_digest;
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
    std::unordered_map<std::string, std::vector<std::filesystem::path>> custom_outputs;
    std::unordered_map<std::string, std::string> exported_modules;
    std::unordered_map<std::string, std::string> closure_import_paths;
    std::unordered_map<std::string, std::string> closure_module_owners;
    std::vector<std::filesystem::path> archive_closure;
    std::vector<std::string> native_link_inputs;
    bool uses_stdlib = false;
};

struct ProjectDiscovery {
    std::vector<ManifestData> manifests_nearest_first;
    std::optional<ManifestData> current_manifest;
    std::optional<ManifestData> workspace_manifest;
};

[[nodiscard]] std::expected<ManifestData, ManifestError> parse_manifest(const std::filesystem::path& manifest_path) {
    ManifestData manifest;
    manifest.manifest_path = normalized_path(manifest_path);

    std::ifstream input(manifest.manifest_path);
    if (!input) return std::unexpected(ManifestError("cannot open manifest '" + manifest.manifest_path.string() + "'"));

    enum class Section {
        Root,
        Package,
        Lib,
        Bin,
        Custom,
        Dependencies,
        Native,
        Workspace,
        WorkspaceDependencies,
        PackageMetadata,
        Ignored,
    };

    Section current_section = Section::Root;
    std::string current_custom;
    ManifestTarget* current_bin = nullptr;
    ManifestTarget* current_lib = nullptr;

    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        line_number++;
        std::string stripped = trim(strip_toml_comment(line));
        if (stripped.empty()) continue;
        if (stripped.front() == '[') {
            if (stripped.size() < 2 || stripped.back() != ']') {
                return std::unexpected(ManifestError(manifest.manifest_path.string() + ":" + std::to_string(line_number) +
                                    ": malformed section header"));
            }
            current_bin = nullptr;
            current_lib = nullptr;
            if (stripped.rfind("[[", 0) == 0) {
                if (stripped.size() < 4 || stripped.substr(stripped.size() - 2) != "]]" ) {
                    return std::unexpected(ManifestError(manifest.manifest_path.string() + ":" + std::to_string(line_number) +
                                        ": malformed array-of-table header"));
                }
                std::string section_name = trim(stripped.substr(2, stripped.size() - 4));
                if (section_name == "bin") {
                    manifest.bin_targets.emplace_back();
                    current_bin = &manifest.bin_targets.back();
                    current_section = Section::Bin;
                    continue;
                }
                if (section_name == "lib") {
                    manifest.lib_targets.emplace_back();
                    current_lib = &manifest.lib_targets.back();
                    current_section = Section::Lib;
                    continue;
                }
                {
                    return std::unexpected(ManifestError(manifest.manifest_path.string() + ":" + std::to_string(line_number) +
                                        ": unsupported array-of-table [[" + section_name + "]]"));
                }
            }
            std::string section_name = trim(stripped.substr(1, stripped.size() - 2));
            if (section_name == "package") {
                current_section = Section::Package;
            } else if (section_name == "lib") {
                return std::unexpected(ManifestError(manifest.manifest_path.string() + ":" + std::to_string(line_number) +
                                    ": [lib] has been replaced by [[lib]]"));
            } else if (section_name == "dependencies") {
                current_section = Section::Dependencies;
            } else if (section_name == "native") {
                current_section = Section::Native;
            } else if (section_name.rfind("additional_objs.", 0) == 0) {
                current_section = Section::Custom;
                current_custom = section_name.substr(std::string("additional_objs.").size());
                if (current_custom.empty()) {
                    return std::unexpected(ManifestError(manifest.manifest_path.string() + ":" + std::to_string(line_number) +
                                        ": additional_objs section name cannot be empty"));
                }
                if (!manifest.custom_commands.contains(current_custom)) {
                    manifest.custom_commands.emplace(current_custom, CustomCommand{});
                }
            } else if (section_name == "workspace") {
                current_section = Section::Workspace;
                if (!manifest.workspace.has_value()) manifest.workspace = WorkspaceConfig{};
            } else if (section_name == "workspace.dependencies") {
                current_section = Section::WorkspaceDependencies;
                if (!manifest.workspace.has_value()) manifest.workspace = WorkspaceConfig{};
                manifest.workspace->has_workspace_dependencies = true;
            } else if (section_name == "package.metadata") {
                current_section = Section::PackageMetadata;
            } else {
                current_section = Section::Ignored;
            }
            continue;
        }

        std::size_t eq = stripped.find('=');
        if (eq == std::string::npos) {
            return std::unexpected(ManifestError(manifest.manifest_path.string() + ":" + std::to_string(line_number) +
                                ": expected key = value"));
        }
        std::string key = trim(stripped.substr(0, eq));
        auto value_result = read_manifest_value(stripped.substr(eq + 1), input, line_number, manifest.manifest_path);
        if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
        std::string value = std::move(value_result).value();
        std::string context = manifest.manifest_path.string() + ":" + std::to_string(line_number) + ": " + key;

        switch (current_section) {
            case Section::Root:
                if (key == "manifest-version") {
                    auto r = parse_int_literal(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.manifest_version = *r;
                } else {
                    return std::unexpected(ManifestError(context + " is not supported in the manifest root"));
                }
                break;
            case Section::Package:
                if (key == "name") {
                    auto r = parse_string_literal(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.package_name = std::move(r).value();
                } else if (key == "version") {
                    auto r = parse_string_literal(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.package_version = std::move(r).value();
                } else {
                    return std::unexpected(ManifestError(context + " is not supported in [package]"));
                }
                break;
            case Section::Lib:
                if (current_lib == nullptr) return std::unexpected(ManifestError(context + " is outside a [[lib]] table"));
                if (key == "name") {
                    auto r = parse_string_literal(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    current_lib->name = std::move(r).value();
                } else if (key == "sources") {
                    auto r = parse_string_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    current_lib->source_patterns = std::move(r).value();
                } else if (key == "additional_objs") {
                    auto r = parse_string_or_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    current_lib->additional_obj_steps = std::move(r).value();
                } else {
                    return std::unexpected(ManifestError(context + " is not supported in [[lib]]"));
                }
                break;
            case Section::Bin:
                if (current_bin == nullptr) return std::unexpected(ManifestError(context + " is outside a [[bin]] table"));
                if (key == "name") {
                    auto r = parse_string_literal(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    current_bin->name = std::move(r).value();
                } else if (key == "sources") {
                    auto r = parse_string_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    current_bin->source_patterns = std::move(r).value();
                } else if (key == "additional_objs") {
                    auto r = parse_string_or_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    current_bin->additional_obj_steps = std::move(r).value();
                } else {
                    return std::unexpected(ManifestError(context + " is not supported in [[bin]]"));
                }
                break;
            case Section::Custom:
                if (!manifest.custom_commands.contains(current_custom)) {
                    return std::unexpected(ManifestError(context + " is outside an [additional_objs.<name>] table"));
                }
                if (key == "input") {
                    auto r = parse_string_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.custom_commands[current_custom].input_paths.clear();
                    for (const std::string& path : *r) {
                        manifest.custom_commands[current_custom].input_paths.emplace_back(path);
                    }
                } else if (key == "output") {
                    auto r = parse_string_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.custom_commands[current_custom].output_paths.clear();
                    for (const std::string& path : *r) {
                        manifest.custom_commands[current_custom].output_paths.emplace_back(path);
                    }
                } else if (key == "command") {
                    auto r = parse_string_literal(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.custom_commands[current_custom].command = std::move(r).value();
                } else {
                    return std::unexpected(ManifestError(context + " is not supported in [additional_objs." + current_custom + "]"));
                }
                break;
            case Section::Dependencies: {
                auto table_result = parse_inline_table(value, context);
                if (!table_result.has_value()) return std::unexpected(std::move(table_result).error());
                std::unordered_map<std::string, std::string> table = std::move(table_result).value();
                if (table.contains("path")) {
                    DependencySpec dep;
                    dep.alias = key;
                    auto path_result = parse_string_literal(table.at("path"), context + " path");
                    if (!path_result.has_value()) return std::unexpected(std::move(path_result).error());
                    dep.path = std::move(path_result).value();
                    if (table.size() != 1) {
                        return std::unexpected(ManifestError(context + " currently supports only { path = \"...\" }"));
                    }
                    manifest.dependencies.push_back(std::move(dep));
                } else if (table.contains("scppkg") || table.contains("workspace") || table.contains("git") ||
                           table.contains("version")) {
                    return std::unexpected(ManifestError(context + " uses a dependency source that is designed but not implemented yet"));
                } else {
                    return std::unexpected(ManifestError(context + " must specify { path = \"...\" }"));
                }
                break;
            }
            case Section::Native:
                if (key == "links") {
                    auto r = parse_string_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.native.links = std::move(r).value();
                } else if (key == "search") {
                    auto r = parse_string_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.native.search_paths.clear();
                    for (const std::string& path : *r) {
                        manifest.native.search_paths.emplace_back(path);
                    }
                } else {
                    return std::unexpected(ManifestError(context + " is not supported in [native]"));
                }
                break;
            case Section::Workspace:
                if (key == "members") {
                    auto r = parse_string_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.workspace->members.clear();
                    for (const std::string& member : *r) {
                        manifest.workspace->members.emplace_back(member);
                    }
                } else if (key == "default-members") {
                    auto r = parse_string_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.workspace->has_default_members = true;
                    manifest.workspace->default_members.clear();
                    for (const std::string& member : *r) {
                        manifest.workspace->default_members.emplace_back(member);
                    }
                } else {
                    return std::unexpected(ManifestError(context + " is not supported in [workspace]"));
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
        return std::unexpected(ManifestError("manifest-version = 1 is required in '" + manifest.manifest_path.string() + "'"));
    }
    if (manifest.workspace.has_value() && manifest.workspace->has_workspace_dependencies) {
        return std::unexpected(ManifestError("[workspace.dependencies] is designed but not implemented yet"));
    }
    if (!manifest.package_name.has_value() && !manifest.workspace.has_value()) {
        return std::unexpected(ManifestError("manifest must declare either [package] or [workspace]"));
    }
    if (!manifest.package_name.has_value()) {
        if (!manifest.lib_targets.empty() || !manifest.bin_targets.empty() || !manifest.dependencies.empty()) {
            return std::unexpected(ManifestError("a virtual workspace manifest cannot declare [[lib]], [[bin]], or [dependencies]"));
        }
        return manifest;
    }
    if (manifest.lib_targets.empty() && manifest.bin_targets.empty()) {
        return std::unexpected(ManifestError("manifest must declare at least one [[lib]] or [[bin]] target"));
    }
    std::unordered_set<std::string> lib_names;
    for (const ManifestTarget& lib : manifest.lib_targets) {
        if (lib.name.empty()) return std::unexpected(ManifestError("[[lib]].name is required"));
        if (lib.source_patterns.empty()) return std::unexpected(ManifestError("[[lib]].sources is required"));
        if (!lib_names.insert(lib.name).second) {
            return std::unexpected(ManifestError("duplicate [[lib]] target name '" + lib.name + "'"));
        }
    }
    std::unordered_set<std::string> bin_names;
    for (const ManifestTarget& bin : manifest.bin_targets) {
        if (bin.name.empty()) return std::unexpected(ManifestError("[[bin]].name is required"));
        if (bin.source_patterns.empty()) return std::unexpected(ManifestError("[[bin]].sources is required"));
        if (!bin_names.insert(bin.name).second) {
            return std::unexpected(ManifestError("duplicate [[bin]] target name '" + bin.name + "'"));
        }
    }
    for (const auto& [name, custom] : manifest.custom_commands) {
        if (custom.input_paths.empty()) return std::unexpected(ManifestError("[additional_objs." + name + "].input is required"));
        if (custom.output_paths.empty()) return std::unexpected(ManifestError("[additional_objs." + name + "].output is required"));
        if (custom.command.empty()) return std::unexpected(ManifestError("[additional_objs." + name + "].command is required"));
    }
    auto validate_custom_refs = [&](const ManifestTarget& target,
                                    std::string_view label) -> std::expected<void, ManifestError> {
        std::unordered_set<std::string> seen;
        for (const std::string& step_name : target.additional_obj_steps) {
            if (!manifest.custom_commands.contains(step_name)) {
                return std::unexpected(ManifestError(std::string(label) + " target '" + target.name +
                                    "' references unknown [additional_objs." + step_name + "]"));
            }
            if (!seen.insert(step_name).second) {
                return std::unexpected(ManifestError(std::string(label) + " target '" + target.name +
                                    "' references duplicate custom step '" + step_name + "'"));
            }
        }
        return {};
    };
    for (const ManifestTarget& lib : manifest.lib_targets) {
        if (auto r = validate_custom_refs(lib, "[[lib]]"); !r.has_value()) return std::unexpected(std::move(r).error());
    }
    for (const ManifestTarget& bin : manifest.bin_targets) {
        if (auto r = validate_custom_refs(bin, "[[bin]]"); !r.has_value()) return std::unexpected(std::move(r).error());
    }
    return manifest;
}

std::regex glob_to_regex(std::string_view pattern) {
    std::string regex = "^";
    for (std::size_t i = 0; i < pattern.size(); i++) {
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
                                                          const std::vector<std::string>& patterns) {
    std::vector<std::regex> matchers;
    for (const std::string& pattern : patterns) {
        matchers.push_back(glob_to_regex(std::filesystem::path(pattern).generic_string()));
    }
    std::set<std::filesystem::path> paths;
    std::error_code ec;
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

[[nodiscard]] std::expected<std::optional<ScannedModuleDecl>, BuildError> scan_declared_module(const std::vector<scpp::Token>& tokens,
                                                      const std::filesystem::path& path_for_errors) {
    std::size_t i = 0;
    if (i + 1 < tokens.size() && tokens[i].kind == scpp::TokenKind::KwModule &&
        tokens[i + 1].kind == scpp::TokenKind::Semicolon) {
        i += 2;
    }
    bool exported_module = false;
    if (i < tokens.size() && tokens[i].kind == scpp::TokenKind::KwExport) {
        if (i + 1 < tokens.size() && tokens[i + 1].kind == scpp::TokenKind::KwModule) {
            exported_module = true;
            i++;
        }
    }
    if (i >= tokens.size() || tokens[i].kind != scpp::TokenKind::KwModule) return std::nullopt;
    i++;
    if (i >= tokens.size() || tokens[i].kind != scpp::TokenKind::Identifier) return std::nullopt;

    ScannedModuleDecl decl;
    decl.module_name = std::string(tokens[i].text);
    decl.is_interface = exported_module;
    i++;
    while (i + 1 < tokens.size() && tokens[i].kind == scpp::TokenKind::Dot &&
           tokens[i + 1].kind == scpp::TokenKind::Identifier) {
        decl.module_name += ".";
        decl.module_name += std::string(tokens[i + 1].text);
        i += 2;
    }
    if (i < tokens.size() && tokens[i].kind == scpp::TokenKind::Colon) {
        i++;
        if (i >= tokens.size() || tokens[i].kind != scpp::TokenKind::Identifier) {
            return std::unexpected(BuildError("invalid partition declaration in '" + path_for_errors.string() + "'"));
        }
        decl.partition_name = std::string(tokens[i].text);
    }
    return decl;
}

[[nodiscard]] std::expected<SourceInfo, BuildError> classify_source(const std::filesystem::path& path) {
    SourceInfo info;
    info.path = normalized_path(path);
    auto source_result = read_file(info.path);
    if (!source_result.has_value()) return std::unexpected(std::move(source_result).error());
    std::vector<scpp::Token> tokens = scpp::tokenize(*source_result);
    auto decl_result = scan_declared_module(tokens, info.path);
    if (!decl_result.has_value()) return std::unexpected(std::move(decl_result).error());
    if (decl_result->has_value()) {
        const ScannedModuleDecl& decl = **decl_result;
        info.module_name = decl.module_name;
        info.partition_name = decl.partition_name;
        if (!decl.partition_name.empty()) {
            info.kind = decl.is_interface ? SourceInfo::Kind::InterfacePartition
                                           : SourceInfo::Kind::ImplementationPartition;
        } else {
            info.kind = decl.is_interface ? SourceInfo::Kind::PrimaryInterface
                                           : SourceInfo::Kind::ImplementationUnit;
        }
    }

    for (std::size_t token_index = 0; token_index < tokens.size(); token_index++) {
        std::size_t import_index = token_index;
        if (tokens[token_index].kind == scpp::TokenKind::KwExport) {
            if (token_index + 1 >= tokens.size() || tokens[token_index + 1].kind != scpp::TokenKind::KwImport) continue;
            import_index = token_index + 1;
        } else if (tokens[token_index].kind != scpp::TokenKind::KwImport) {
            continue;
        }
        std::size_t j = import_index + 1;
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

[[nodiscard]] std::expected<std::vector<std::string>, BuildError> topo_sort_modules(const std::map<std::string, SourceInfo>& primary_modules) {
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
        return std::unexpected(BuildError("cyclic local module dependency detected in manifest target source set"));
    }
    return order;
}

[[nodiscard]] std::expected<std::vector<SourceInfo>, BuildError> classify_target_sources(const std::filesystem::path& manifest_dir, const ManifestTarget& target) {
    std::vector<std::filesystem::path> source_paths = expand_source_patterns(manifest_dir, target.source_patterns);
    if (source_paths.empty()) {
        return std::unexpected(BuildError("target sources globs matched no files"));
    }
    std::vector<SourceInfo> sources;
    for (const std::filesystem::path& source_path : source_paths) {
        auto info_result = classify_source(source_path);
        if (!info_result.has_value()) return std::unexpected(std::move(info_result).error());
        sources.push_back(*std::move(info_result));
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

[[nodiscard]] std::expected<std::unordered_map<std::string, std::string>, BuildError> local_source_import_paths(const std::vector<SourceInfo>& sources) {
    std::unordered_map<std::string, std::string> import_paths;
    for (const SourceInfo& source : sources) {
        std::optional<std::string> key;
        switch (source.kind) {
            case SourceInfo::Kind::PrimaryInterface:
                key = source.module_name;
                break;
            case SourceInfo::Kind::InterfacePartition:
            case SourceInfo::Kind::ImplementationPartition:
                key = source.module_name + ":" + source.partition_name;
                break;
            case SourceInfo::Kind::ImplementationUnit:
            case SourceInfo::Kind::Plain:
                break;
        }
        if (!key.has_value()) continue;
        auto [it, inserted] = import_paths.emplace(*key, source.path.string());
        if (!inserted && it->second != source.path.string()) {
            return std::unexpected(BuildError("multiple source files declare '" + *key + "' within one manifest target"));
        }
    }
    return import_paths;
}

bool source_uses_stdlib(const SourceInfo& source) {
    return std::find(source.imported_modules.begin(), source.imported_modules.end(), "std") != source.imported_modules.end();
}

bool sources_use_stdlib(const std::vector<SourceInfo>& sources) {
    return std::any_of(sources.begin(), sources.end(), [](const SourceInfo& source) { return source_uses_stdlib(source); });
}

[[nodiscard]] std::expected<void, BuildError> validate_direct_visibility(const std::vector<SourceInfo>& sources,
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
                return std::unexpected(BuildError("module '" + imported + "' is exported only by transitive dependency package '" +
                                 transitive->second + "'; add it as a direct dependency to import it"));
            }
        }
    }
    return {};
}

[[nodiscard]] std::expected<std::vector<BuiltModule>, BuildError> build_modules_for_target(const std::vector<SourceInfo>& sources,
                                                  const std::filesystem::path& module_dir,
                                                  const std::filesystem::path& archive_dir,
                                                  const std::unordered_map<std::string, std::string>& base_import_paths,
                                                  int opt_level,
                                                  const ManifestData& manifest,
                                                  const ManifestTarget* target,
                                                  BuildDatabase& database) {
    std::map<std::string, SourceInfo> primary_modules;
    for (const SourceInfo& source : sources) {
        switch (source.kind) {
            case SourceInfo::Kind::PrimaryInterface:
                if (primary_modules.contains(source.module_name)) {
                    return std::unexpected(BuildError("duplicate primary interface for module '" + source.module_name + "'"));
                }
                primary_modules.emplace(source.module_name, source);
                break;
            case SourceInfo::Kind::InterfacePartition:
            case SourceInfo::Kind::ImplementationPartition:
                break;
            case SourceInfo::Kind::ImplementationUnit:
                return std::unexpected(BuildError("module implementation units are not implemented in project builds yet ('" +
                                 source.path.string() + "')"));
            case SourceInfo::Kind::Plain:
                break;
        }
    }

    for (const SourceInfo& source : sources) {
        if ((source.kind == SourceInfo::Kind::InterfacePartition ||
             source.kind == SourceInfo::Kind::ImplementationPartition) &&
            !primary_modules.contains(source.module_name)) {
            return std::unexpected(BuildError("partition '" + source.module_name + ":" + source.partition_name +
                             "' has no primary interface in this target"));
        }
    }

    auto build_order_result = topo_sort_modules(primary_modules);
    if (!build_order_result.has_value()) return std::unexpected(std::move(build_order_result).error());
    std::vector<std::string> build_order = *std::move(build_order_result);
    std::unordered_map<std::string, std::string> import_paths = base_import_paths;
    auto local_import_paths_result = local_source_import_paths(sources);
    if (!local_import_paths_result.has_value()) return std::unexpected(std::move(local_import_paths_result).error());
    for (const auto& [name, path] : *local_import_paths_result) {
        import_paths[name] = path;
    }
    std::unordered_map<std::string, int> indegree;
    std::unordered_map<std::string, std::vector<std::string>> dependents;
    for (const auto& [name, _] : primary_modules) indegree[name] = 0;
    for (const auto& [name, source] : primary_modules) {
        std::unordered_set<std::string> local_deps;
        for (const std::string& imported : source.imported_modules) {
            if (primary_modules.contains(imported) && imported != name) local_deps.insert(imported);
        }
        for (const std::string& dep : local_deps) {
            dependents[dep].push_back(name);
            indegree[name]++;
        }
    }
    std::vector<BuiltModule> outputs;
    std::vector<std::string> ready;
    for (const auto& [name, degree] : indegree) {
        if (degree == 0) ready.push_back(name);
    }
    std::sort(ready.begin(), ready.end());
    if (target != nullptr) {
        if (primary_modules.empty()) {
            return std::unexpected(BuildError("[[lib]] target must contain at least one primary interface module"));
        }
        // A [[lib]] target may bundle multiple independent primary modules into
        // one build (each still gets its own interface/archive artifact -- see
        // archive_base_name below, and topo_sort_modules/the ready-batch loop
        // above already build an arbitrary number of them in dependency order).
        // The one thing that still requires exactly one primary module is
        // additional_objs: build_library_target merges its native object
        // outputs into built_lib_modules.front().archive_path, which can't
        // unambiguously pick a merge target when there's more than one module.
        if (primary_modules.size() != 1 && !target->additional_obj_steps.empty()) {
            return std::unexpected(BuildError("[[lib]] target '" + target->name +
                             "' must contain exactly one primary interface module when using additional_objs "
                             "(archive merging can't tell which module's archive to merge native objects into)"));
        }
    }
    const std::string manifest_key = manifest_digest(manifest);
    const std::string compiler_key = compiler_version_key();
    // A primary interface module's compiled output also depends on the
    // content of every interface/implementation partition it re-exports
    // (e.g. `std.scpp`'s `export import :string_view;` pulls in
    // `string_view/std_string_view.scpp`), not just the primary file
    // itself -- so the cache signature below must cover all of them, or
    // editing a partition's own file alone would never invalidate the
    // cached module artifact.
    std::unordered_map<std::string, std::vector<std::filesystem::path>> partition_paths_by_module;
    for (const SourceInfo& source : sources) {
        if (source.kind == SourceInfo::Kind::InterfacePartition || source.kind == SourceInfo::Kind::ImplementationPartition) {
            partition_paths_by_module[source.module_name].push_back(source.path);
        }
    }
    while (!ready.empty()) {
        std::vector<std::string> batch = ready;
        ready.clear();
        std::vector<std::future<std::expected<BuiltModule, BuildError>>> futures;
        for (const std::string& module_name : batch) {
            futures.push_back(std::async(std::launch::async, [&, module_name]() -> std::expected<BuiltModule, BuildError> {
                const SourceInfo& source = primary_modules.at(module_name);
                std::filesystem::path interface_path = module_dir / (module_name + ".scppm");
                // When a [[lib]] target has exactly one primary module (the
                // common case today), keep naming its archive after the
                // target for backward compatibility. With multiple primary
                // modules sharing one target, fall back to naming each
                // archive after its own module instead, so they don't all
                // collide on the same lib<target-name>.scppa path.
                std::string archive_base_name =
                    (target != nullptr && !target->name.empty() && primary_modules.size() == 1) ? target->name
                                                                                                  : module_name;
                std::filesystem::path archive_path = archive_dir / ("lib" + archive_base_name + ".scppa");
                auto module_source_result = read_file(source.path);
                if (!module_source_result.has_value()) return std::unexpected(std::move(module_source_result).error());
                const std::string& module_source = *module_source_result;
                std::vector<std::string> dep_keys;
                for (const std::string& imported : source.imported_modules) {
                    auto it = import_paths.find(imported);
                    if (it == import_paths.end()) continue;
                    dep_keys.push_back(imported + "=" + it->second + "#" + path_digest_or_empty(it->second));
                }
                std::sort(dep_keys.begin(), dep_keys.end());
                std::vector<std::filesystem::path> module_source_paths = {source.path};
                if (auto it = partition_paths_by_module.find(module_name); it != partition_paths_by_module.end()) {
                    module_source_paths.insert(module_source_paths.end(), it->second.begin(), it->second.end());
                }
                std::sort(module_source_paths.begin(), module_source_paths.end());
                std::string signature = fnv1a64_hex(join_for_digest({
                    "kind=module",
                    "source=" + join_for_digest(path_digests(module_source_paths)),
                    "triple=" + scpp::host_target_triple(),
                    "compiler=" + compiler_key,
                    "manifest=" + manifest_key,
                    "opt=" + std::to_string(opt_level),
                    "deps=" + join_for_digest(dep_keys),
                }));
                std::string record_key = "module|" + manifest.manifest_path.string() + "|" + module_name;
                auto cached_result = database.get(record_key);
                if (!cached_result.has_value()) return std::unexpected(std::move(cached_result).error());
                if (cached_result->has_value() && (*cached_result)->signature == signature &&
                    std::filesystem::exists(interface_path) && std::filesystem::exists(archive_path)) {
                    trace_build("cache hit module " + module_name);
                    return BuiltModule{module_name, source.path, interface_path, archive_path,
                                       path_digest_or_empty(interface_path), path_digest_or_empty(archive_path)};
                }
                trace_build("build module " + module_name);
                auto emit_r = scpp::emit_module_artifacts(module_source, interface_path.string(), archive_path.string(), import_paths, {},
                                            source.path.string(), opt_level);
                if (!emit_r.has_value()) {
                    print_diagnostic(source.path.string(), module_source, emit_r.error().loc, emit_r.error().what());
                    return std::unexpected(BuildError(emit_r.error().what()));
                }
                BuiltModule built{module_name, source.path, interface_path, archive_path,
                                  path_digest_or_empty(interface_path), path_digest_or_empty(archive_path)};
                if (auto put_result = database.put(BuildRecord{
                    record_key,
                    "module",
                    signature,
                    built.interface_digest,
                    built.archive_digest,
                    built.archive_digest,
                    archive_path.string(),
                    manifest_key,
                    compiler_key,
                    scpp::host_target_triple(),
                }); !put_result.has_value()) {
                    return std::unexpected(std::move(put_result).error());
                }
                return built;
            }));
        }
        for (auto& future : futures) {
            auto built_result = future.get();
            if (!built_result.has_value()) return std::unexpected(std::move(built_result).error());
            BuiltModule built = *std::move(built_result);
            import_paths[built.name] = built.interface_path.string();
            outputs.push_back(std::move(built));
        }
        std::sort(outputs.begin(), outputs.end(), [&](const BuiltModule& lhs, const BuiltModule& rhs) {
            auto lhs_it = std::find(build_order.begin(), build_order.end(), lhs.name);
            auto rhs_it = std::find(build_order.begin(), build_order.end(), rhs.name);
            return lhs_it < rhs_it;
        });
        std::vector<std::string> newly_ready;
        for (const std::string& module_name : batch) {
            for (const std::string& dependent : dependents[module_name]) {
                indegree[dependent]--;
                if (indegree[dependent] == 0) newly_ready.push_back(dependent);
            }
        }
        std::sort(newly_ready.begin(), newly_ready.end());
        ready = std::move(newly_ready);
    }
    return outputs;
}

std::unordered_map<std::string, std::string> to_import_map(const std::vector<BuiltModule>& modules) {
    std::unordered_map<std::string, std::string> import_paths;
    for (const BuiltModule& module : modules) import_paths.emplace(module.name, module.interface_path.string());
    return import_paths;
}

[[nodiscard]] std::expected<void, BuildError> append_import_maps(std::unordered_map<std::string, std::string>& into,
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
            return std::unexpected(BuildError("module '" + name + "' is produced by multiple direct dependencies" + detail));
        }
    }
    return {};
}

void append_unique_paths(std::vector<std::filesystem::path>& into, const std::vector<std::filesystem::path>& extra) {
    for (const std::filesystem::path& path : extra) {
        if (std::find(into.begin(), into.end(), path) == into.end()) into.push_back(path);
    }
}

void append_unique_strings(std::vector<std::string>& into, const std::vector<std::string>& extra) {
    for (const std::string& value : extra) {
        if (std::find(into.begin(), into.end(), value) == into.end()) into.push_back(value);
    }
}

std::string quote_for_shell(const std::string& text) {
    std::string quoted = "\"";
    for (char ch : text) {
        if (ch == '"' || ch == '\\' || ch == '$' || ch == '`') quoted.push_back('\\');
        quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
}

std::optional<std::string> find_program_on_path(const std::string& program) {
    if (program.empty()) return std::nullopt;
    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr || path_env[0] == '\0') return std::nullopt;
    std::stringstream stream(path_env);
    std::string dir;
    while (std::getline(stream, dir, ':')) {
        if (dir.empty()) continue;
        std::filesystem::path candidate = std::filesystem::path(dir) / program;
        if (access(candidate.c_str(), X_OK) == 0) return candidate.string();
    }
    return std::nullopt;
}

std::optional<std::string> default_custom_step_cxx() {
    const char* env = std::getenv("CXX");
    if (env != nullptr && env[0] != '\0') return std::string(env);
    for (const std::string& candidate : {"clang++", "clang++-22", "g++", "c++"}) {
        if (std::optional<std::string> resolved = find_program_on_path(candidate); resolved.has_value()) {
            return resolved;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::expected<void, BuildError> prepare_custom_workdir(const std::filesystem::path& manifest_dir,
                                                                      const std::filesystem::path& work_dir) {
    std::error_code ec;
    std::filesystem::create_directories(work_dir, ec);
    if (ec) {
        return std::unexpected(BuildError("failed to create custom step workdir '" + work_dir.string() + "': " + ec.message()));
    }
    for (const auto& entry : std::filesystem::directory_iterator(manifest_dir)) {
        std::filesystem::path link_path = work_dir / entry.path().filename();
        if (entry.path().filename() == ".scpp") continue;
        std::filesystem::remove_all(link_path, ec);
        if (entry.is_directory()) {
            std::filesystem::create_directory_symlink(entry.path(), link_path, ec);
        } else {
            std::filesystem::create_symlink(entry.path(), link_path, ec);
        }
        if (ec) {
            return std::unexpected(BuildError("failed to prepare custom step workdir entry '" + link_path.string() +
                             "': " + ec.message()));
        }
    }
    return {};
}

std::vector<std::string> expand_native_link_inputs(const ManifestData& manifest) {
    std::vector<std::string> inputs;
    std::filesystem::path manifest_dir = manifest.manifest_path.parent_path();
    for (const std::filesystem::path& path : manifest.native.search_paths) {
        std::filesystem::path resolved = path.is_absolute() ? path : normalized_path(manifest_dir / path);
        inputs.push_back("-L" + resolved.string());
    }
    for (const std::string& link : manifest.native.links) {
        inputs.push_back("-l" + link);
    }
    return inputs;
}

std::string manifest_digest(const ManifestData& manifest) {
    return digest_file(manifest.manifest_path);
}

std::optional<std::filesystem::path> current_executable_path_local() {
    std::error_code ec;
    std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return std::nullopt;
    return normalized_path(exe);
}

std::string compiler_version_key() {
    if (std::optional<std::filesystem::path> exe = current_executable_path_local(); exe.has_value()) {
        return digest_file(*exe);
    }
    return "unknown-compiler";
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

[[nodiscard]] std::expected<ProjectDiscovery, ManifestError> discover_project(const std::filesystem::path& start_dir) {
    ProjectDiscovery discovery;
    for (const std::filesystem::path& manifest_path : manifests_upward(start_dir)) {
        auto manifest_result = parse_manifest(manifest_path);
        if (!manifest_result.has_value()) return std::unexpected(std::move(manifest_result).error());
        ManifestData manifest = std::move(manifest_result).value();
        if (!discovery.current_manifest.has_value()) discovery.current_manifest = manifest;
        if (!discovery.workspace_manifest.has_value() && manifest.workspace.has_value()) {
            discovery.workspace_manifest = manifest;
        }
        discovery.manifests_nearest_first.push_back(std::move(manifest));
    }
    return discovery;
}

[[nodiscard]] std::expected<ManifestData, ManifestError> load_package_manifest(const std::filesystem::path& manifest_path) {
    auto manifest_result = parse_manifest(manifest_path);
    if (!manifest_result.has_value()) return std::unexpected(std::move(manifest_result).error());
    ManifestData manifest = std::move(manifest_result).value();
    if (!manifest.package_name.has_value()) {
        return std::unexpected(ManifestError("dependency manifest '" + manifest.manifest_path.string() + "' does not declare [package]"));
    }
    return manifest;
}

[[nodiscard]] std::expected<WorkspaceInfo, ManifestError> load_workspace(const ManifestData& workspace_manifest) {
    WorkspaceInfo workspace;
    workspace.manifest = workspace_manifest;
    std::filesystem::path root_dir = workspace_manifest.manifest_path.parent_path();
    std::unordered_map<std::string, std::filesystem::path> seen_names;

    auto add_member = [&](const ManifestData& manifest) -> std::expected<void, ManifestError> {
        if (!manifest.package_name.has_value()) {
            return std::unexpected(ManifestError("workspace member '" + manifest.manifest_path.string() + "' must declare [package]"));
        }
        auto [it, inserted] = seen_names.emplace(*manifest.package_name, manifest.manifest_path);
        if (!inserted) {
            return std::unexpected(ManifestError("workspace contains duplicate package name '" + *manifest.package_name + "'"));
        }
        workspace.member_manifests.push_back(manifest);
        return {};
    };

    if (workspace_manifest.package_name.has_value()) {
        if (auto r = add_member(workspace_manifest); !r.has_value()) return std::unexpected(std::move(r).error());
    }

    for (const std::filesystem::path& member_path : workspace_manifest.workspace->members) {
        std::filesystem::path member_manifest_path = normalized_path(root_dir / member_path / "scpp.toml");
        if (!std::filesystem::exists(member_manifest_path)) {
            return std::unexpected(ManifestError("workspace member '" + (root_dir / member_path).string() + "' has no scpp.toml"));
        }
        auto member_manifest_result = load_package_manifest(member_manifest_path);
        if (!member_manifest_result.has_value()) return std::unexpected(std::move(member_manifest_result).error());
        ManifestData member_manifest = std::move(member_manifest_result).value();
        if (member_manifest.manifest_path == workspace_manifest.manifest_path) continue;
        if (member_manifest.workspace.has_value()) {
            return std::unexpected(ManifestError("nested workspace member '" + member_manifest.manifest_path.string() +
                                "' is not supported"));
        }
        if (auto r = add_member(member_manifest); !r.has_value()) return std::unexpected(std::move(r).error());
    }

    auto resolve_default_member =
        [&](const std::filesystem::path& relative_path) -> std::expected<std::filesystem::path, ManifestError> {
        std::filesystem::path candidate = normalized_path(root_dir / relative_path / "scpp.toml");
        if (relative_path == "." || relative_path.empty()) candidate = workspace_manifest.manifest_path;
        auto it = std::find_if(workspace.member_manifests.begin(), workspace.member_manifests.end(),
                               [&](const ManifestData& manifest) { return manifest.manifest_path == candidate; });
        if (it == workspace.member_manifests.end()) {
            return std::unexpected(ManifestError("default workspace member '" + relative_path.string() + "' is not a declared workspace package"));
        }
        return candidate;
    };

    if (workspace_manifest.workspace->has_default_members) {
        for (const std::filesystem::path& member : workspace_manifest.workspace->default_members) {
            auto candidate_result = resolve_default_member(member);
            if (!candidate_result.has_value()) return std::unexpected(std::move(candidate_result).error());
            workspace.default_package_manifests.push_back(std::move(candidate_result).value());
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

std::filesystem::path package_output_root(const std::filesystem::path& shared_root_dir,
                                          std::string_view package_name) {
    return shared_root_dir / ".scpp" / "build" / scpp::host_target_triple() / std::string(package_name);
}

[[nodiscard]] std::expected<void, BuildError> write_metadata_file(const PackageBuildResult& result,
                                                                   const std::filesystem::path& metadata_path) {
    std::ostringstream json;
    json << "{\n";
    json << "  \"package\": \"" << escape_json(*result.manifest.package_name) << "\",\n";
    json << "  \"triple\": \"" << escape_json(scpp::host_target_triple()) << "\",\n";
    json << "  \"modules\": [\n";
    for (std::size_t i = 0; i < result.library_modules.size(); i++) {
        const BuiltModule& module = result.library_modules[i];
        json << "    {\"name\": \"" << escape_json(module.name) << "\", \"interface\": \""
             << escape_json(module.interface_path.string()) << "\", \"archive\": \""
             << escape_json(module.archive_path.string()) << "\"}";
        if (i + 1 < result.library_modules.size()) json << ",";
        json << "\n";
    }
    json << "  ],\n";
    json << "  \"binaries\": [\n";
    for (std::size_t i = 0; i < result.binaries.size(); i++) {
        json << "    \"" << escape_json(result.binaries[i].string()) << "\"";
        if (i + 1 < result.binaries.size()) json << ",";
        json << "\n";
    }
    json << "  ]\n";
    json << "}\n";
    return write_file(metadata_path, json.str());
}

class PackageBuilder {
public:
    PackageBuilder(std::filesystem::path shared_root_dir,
                   std::optional<ManifestData> workspace_manifest,
                   std::optional<WorkspaceInfo> workspace_info)
        : shared_root_dir_(normalized_path(shared_root_dir)),
          workspace_manifest_(std::move(workspace_manifest)),
          workspace_info_(std::move(workspace_info)),
          database_(shared_root_dir_ / ".scpp" / "cache" / "build.db") {}

    // Must be called exactly once, immediately after construction and before
    // build_package(), because BuildDatabase's own construction is
    // deliberately non-fallible (see the comment on BuildDatabase) and this
    // is where its fallible sqlite3 initialization actually happens.
    [[nodiscard]] std::expected<void, BuildError> open_database() { return database_.open(); }

    [[nodiscard]] std::expected<std::reference_wrapper<PackageBuildResult>, BuildError> build_package(
        const std::filesystem::path& manifest_path, bool build_binaries, const scpp::ProjectBuildOptions& options) {
        std::filesystem::path normalized_manifest = normalized_path(manifest_path);
        std::string key = normalized_manifest.string();
        thread_local std::unordered_set<std::string> recursion_stack;
        if (!recursion_stack.insert(key).second) {
            return std::unexpected(BuildError("cyclic package dependency involving '" + normalized_manifest.string() + "'"));
        }
        auto erase_from_stack = [&]() { recursion_stack.erase(key); };

        while (true) {
            std::unique_lock lock(mutex_);
            auto cached = cache_.find(key);
            if (cached != cache_.end()) {
                bool has_all_libraries =
                    cached->second.library_modules.size() >= cached->second.manifest.lib_targets.size();
                if (has_all_libraries) {
                    if (!build_binaries) {
                        erase_from_stack();
                        return std::ref(cached->second);
                    }
                    std::filesystem::path selected_binary_path;
                    if (options.selected_bin.has_value()) {
                        selected_binary_path = cached->second.package_output_root / *options.selected_bin;
                    }
                    bool has_requested_binary = options.selected_bin.has_value()
                        ? std::find(cached->second.binaries.begin(), cached->second.binaries.end(), selected_binary_path) !=
                              cached->second.binaries.end()
                        : (!cached->second.binaries.empty() || cached->second.manifest.bin_targets.empty());
                    if (has_requested_binary) {
                        erase_from_stack();
                        return std::ref(cached->second);
                    }
                }
            }
            auto state = states_.find(key);
            if (state == states_.end()) {
                states_[key] = PackageState::Building;
                break;
            }
            if (state->second == PackageState::Built) {
                states_[key] = PackageState::Building;
                break;
            }
            if (state->second == PackageState::Failed) {
                std::string failure = failures_.contains(key) ? failures_.at(key) : "package build failed";
                erase_from_stack();
                return std::unexpected(BuildError(failure));
            }
            cv_.wait(lock, [&, this] {
                return !states_.contains(key) || states_.at(key) != PackageState::Building;
            });
        }

        // fail() is the single funnel every failure path below goes through
        // (mirroring the old catch (...) block this replaced): it records the
        // failure so that other threads/recursive callers currently waiting
        // on this same package key observe PackageState::Failed with the
        // same message, then unwinds the recursion-guard entry before
        // propagating the error onward.
        auto fail = [&, this](BuildError error) -> std::expected<std::reference_wrapper<PackageBuildResult>, BuildError> {
            {
                std::lock_guard fail_lock(mutex_);
                states_[key] = PackageState::Failed;
                failures_[key] = error.what();
                cv_.notify_all();
            }
            erase_from_stack();
            return std::unexpected(std::move(error));
        };

        trace_build("build package " + normalized_manifest.string());
        auto manifest_result = load_package_manifest(normalized_manifest);
        if (!manifest_result.has_value()) return fail(BuildError(manifest_result.error().what()));
        ManifestData manifest = std::move(manifest_result).value();
        PackageBuildResult result;
        result.manifest = manifest;
        result.package_output_root = package_output_root(shared_root_dir_, *manifest.package_name);
        result.native_link_inputs = expand_native_link_inputs(manifest);
        std::error_code ec;
        std::filesystem::create_directories(result.package_output_root / "modules", ec);
        if (!ec) std::filesystem::create_directories(result.package_output_root / "archives", ec);
        if (!ec) std::filesystem::create_directories(result.package_output_root / "objects", ec);
        if (ec) {
            return fail(BuildError("failed to create package output directories under '" +
                                    result.package_output_root.string() + "': " + ec.message()));
        }

        std::unordered_map<std::string, std::string> direct_import_paths;
        std::unordered_map<std::string, std::string> full_dependency_import_paths;
        std::unordered_map<std::string, std::string> direct_module_owners;
        std::unordered_map<std::string, std::string> transitive_only_modules;

        for (const DependencySpec& dep : manifest.dependencies) {
            std::filesystem::path dep_manifest_path =
                normalized_path(manifest.manifest_path.parent_path() / dep.path / "scpp.toml");
            if (!std::filesystem::exists(dep_manifest_path)) {
                return fail(BuildError("dependency '" + dep.alias + "' path '" +
                                 (manifest.manifest_path.parent_path() / dep.path).string() + "' has no scpp.toml"));
            }
            auto dep_result_or_error = build_package(dep_manifest_path, /*build_binaries=*/false, scpp::ProjectBuildOptions{});
            if (!dep_result_or_error.has_value()) return fail(std::move(dep_result_or_error).error());
            PackageBuildResult& dep_result = dep_result_or_error->get();
            if (dep_result.library_modules.empty()) {
                return fail(BuildError("dependency package '" + *dep_result.manifest.package_name +
                                 "' does not provide a [[lib]] target"));
            }
            for (const auto& [module_name, interface_path] : dep_result.exported_modules) {
                auto [it, inserted] = direct_import_paths.emplace(module_name, interface_path);
                if (!inserted && it->second != interface_path) {
                    return fail(BuildError("module '" + module_name + "' is exported by multiple direct dependencies"));
                }
                direct_module_owners.emplace(module_name, *dep_result.manifest.package_name);
            }
            if (auto r = append_import_maps(full_dependency_import_paths, dep_result.exported_modules); !r.has_value()) {
                return fail(std::move(r).error());
            }
            if (auto r = append_import_maps(full_dependency_import_paths, dep_result.closure_import_paths); !r.has_value()) {
                return fail(std::move(r).error());
            }
            for (const auto& [module_name, owner] : dep_result.closure_module_owners) {
                if (!direct_import_paths.contains(module_name) && !transitive_only_modules.contains(module_name)) {
                    transitive_only_modules.emplace(module_name, owner);
                }
            }
            for (const BuiltModule& module : dep_result.library_modules) {
                result.archive_closure.push_back(module.archive_path);
            }
            append_unique_paths(result.archive_closure, dep_result.archive_closure);
            append_unique_strings(result.native_link_inputs, dep_result.native_link_inputs);
            result.uses_stdlib = result.uses_stdlib || dep_result.uses_stdlib;
        }

        std::unordered_set<std::string> referenced_custom_steps;
        if (options.selected_lib.has_value()) {
            auto it = std::find_if(manifest.lib_targets.begin(), manifest.lib_targets.end(),
                                   [&](const ManifestTarget& target) { return target.name == *options.selected_lib; });
            if (it == manifest.lib_targets.end()) {
                return fail(BuildError("unknown [[lib]] target '" + *options.selected_lib + "'"));
            }
            referenced_custom_steps.insert(it->additional_obj_steps.begin(), it->additional_obj_steps.end());
        } else {
            for (const ManifestTarget& target : manifest.lib_targets) {
                referenced_custom_steps.insert(target.additional_obj_steps.begin(), target.additional_obj_steps.end());
            }
        }
        if (build_binaries) {
            if (options.selected_bin.has_value()) {
                auto it = std::find_if(manifest.bin_targets.begin(), manifest.bin_targets.end(),
                                       [&](const ManifestTarget& target) { return target.name == *options.selected_bin; });
                if (it == manifest.bin_targets.end()) {
                    return fail(BuildError("unknown [[bin]] target '" + *options.selected_bin + "'"));
                }
                referenced_custom_steps.insert(it->additional_obj_steps.begin(), it->additional_obj_steps.end());
            } else {
                for (const ManifestTarget& target : manifest.bin_targets) {
                    referenced_custom_steps.insert(target.additional_obj_steps.begin(), target.additional_obj_steps.end());
                }
            }
        }
        for (const std::string& step_name : referenced_custom_steps) {
            auto outputs_result = build_custom_step_with_cache(manifest, result.package_output_root, step_name);
            if (!outputs_result.has_value()) return fail(std::move(outputs_result).error());
            result.custom_outputs.emplace(step_name, std::move(outputs_result).value());
        }

        if (options.selected_lib.has_value()) {
            auto it = std::find_if(manifest.lib_targets.begin(), manifest.lib_targets.end(),
                                   [&](const ManifestTarget& target) { return target.name == *options.selected_lib; });
            if (auto r = build_library_target(manifest, *it, direct_import_paths, full_dependency_import_paths,
                                 transitive_only_modules, result); !r.has_value()) {
                return fail(std::move(r).error());
            }
        } else {
            for (const ManifestTarget& lib_target : manifest.lib_targets) {
                if (auto r = build_library_target(manifest, lib_target, direct_import_paths, full_dependency_import_paths,
                                     transitive_only_modules, result); !r.has_value()) {
                    return fail(std::move(r).error());
                }
            }
        }
        if (manifest.lib_targets.empty() && options.build_lib_only) {
            return fail(BuildError("manifest has no [[lib]] target"));
        }

        result.exported_modules = to_import_map(result.library_modules);
        result.closure_import_paths = result.exported_modules;
        if (auto r = append_import_maps(result.closure_import_paths, full_dependency_import_paths); !r.has_value()) {
            return fail(std::move(r).error());
        }
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
                if (auto r = build_binary_target(manifest, *it, direct_import_paths, full_dependency_import_paths,
                                    transitive_only_modules, result); !r.has_value()) {
                    return fail(std::move(r).error());
                }
            } else {
                std::vector<std::future<std::expected<void, BuildError>>> futures;
                for (const ManifestTarget& bin_target : manifest.bin_targets) {
                    futures.push_back(std::async(std::launch::async, [&, bin_target] {
                        return build_binary_target(manifest, bin_target, direct_import_paths, full_dependency_import_paths,
                                            transitive_only_modules, result);
                    }));
                }
                for (auto& future : futures) {
                    if (auto r = future.get(); !r.has_value()) return fail(std::move(r).error());
                }
            }
        }

        std::filesystem::path metadata_path = result.package_output_root / "package-metadata.json";
        if (auto r = write_metadata_file(result, metadata_path); !r.has_value()) return fail(std::move(r).error());
        if (auto r = database_.put(BuildRecord{
                "package|" + manifest.manifest_path.string(),
                "package",
                fnv1a64_hex(join_for_digest({
                    "manifest=" + manifest_digest(manifest),
                    "triple=" + scpp::host_target_triple(),
                    "metadata=" + digest_file(metadata_path),
                })),
                {},
                {},
                digest_file(metadata_path),
                metadata_path.string(),
                manifest_digest(manifest),
                compiler_version_key(),
                scpp::host_target_triple(),
            });
            !r.has_value()) {
            return fail(std::move(r).error());
        }

        {
            std::lock_guard lock(mutex_);
            states_[key] = PackageState::Built;
            failures_.erase(key);
            auto [it, _] = cache_.insert_or_assign(key, std::move(result));
            cv_.notify_all();
            erase_from_stack();
            return std::ref(it->second);
        }
    }

private:
    enum class PackageState {
        Building,
        Built,
        Failed,
    };

    [[nodiscard]] std::expected<std::vector<std::filesystem::path>, BuildError> build_custom_step_with_cache(
        const ManifestData& manifest, const std::filesystem::path& package_output_root, const std::string& step_name) {
        const auto step_it = manifest.custom_commands.find(step_name);
        if (step_it == manifest.custom_commands.end()) {
            return std::unexpected(BuildError("unknown additional_objs step '" + step_name + "' in '" + manifest.manifest_path.string() + "'"));
        }
        const CustomCommand& step = step_it->second;
        std::filesystem::path manifest_dir = manifest.manifest_path.parent_path();
        std::filesystem::path work_dir = package_output_root / "custom" / sanitize_filename(step_name);

        std::vector<std::filesystem::path> inputs;
        inputs.reserve(step.input_paths.size());
        for (const std::filesystem::path& input : step.input_paths) {
            std::filesystem::path resolved = input.is_absolute() ? normalized_path(input) : normalized_path(manifest_dir / input);
            if (!std::filesystem::exists(resolved)) {
                return std::unexpected(BuildError("[additional_objs." + step_name + "] input '" + resolved.string() + "' does not exist"));
            }
            inputs.push_back(std::move(resolved));
        }

        std::vector<std::filesystem::path> outputs;
        outputs.reserve(step.output_paths.size());
        for (const std::filesystem::path& output : step.output_paths) {
            std::filesystem::path resolved = output.is_absolute() ? output.lexically_normal() : (work_dir / output).lexically_normal();
            outputs.push_back(std::move(resolved));
        }

        std::vector<std::string> input_keys = path_digests(inputs);
        std::sort(input_keys.begin(), input_keys.end());
        std::vector<std::string> output_keys;
        output_keys.reserve(outputs.size());
        for (const std::filesystem::path& output : outputs) output_keys.push_back(output.string());
        std::sort(output_keys.begin(), output_keys.end());

        std::string signature = fnv1a64_hex(join_for_digest({
            "kind=custom",
            "manifest=" + manifest_digest(manifest),
            "compiler=" + compiler_version_key(),
            "command=" + step.command,
            "inputs=" + join_for_digest(input_keys),
            "outputs=" + join_for_digest(output_keys),
        }));
        std::string record_key = "custom|" + manifest.manifest_path.string() + "|" + step_name;

        bool outputs_exist = std::all_of(outputs.begin(), outputs.end(),
                                         [](const std::filesystem::path& output) { return std::filesystem::exists(output); });
        auto cached_result = database_.get(record_key);
        if (!cached_result.has_value()) return std::unexpected(std::move(cached_result).error());
        if (cached_result->has_value() && (*cached_result)->signature == signature && outputs_exist) {
            trace_build("cache hit custom " + step_name);
            return outputs;
        }

        if (auto r = prepare_custom_workdir(manifest_dir, work_dir); !r.has_value()) return std::unexpected(std::move(r).error());
        for (const std::filesystem::path& output : outputs) {
            if (output.has_parent_path()) {
                std::error_code ec;
                std::filesystem::create_directories(output.parent_path(), ec);
                if (ec) {
                    return std::unexpected(BuildError("failed to create directory '" + output.parent_path().string() +
                                                       "': " + ec.message()));
                }
            }
        }

        trace_build("run custom " + step_name);
        std::string command = "cd " + quote_for_shell(work_dir.string()) + " && ";
        if (std::getenv("CXX") == nullptr || std::getenv("CXX")[0] == '\0') {
            if (std::optional<std::string> cxx = default_custom_step_cxx(); cxx.has_value()) {
                command += "export CXX=" + quote_for_shell(*cxx) + " && ";
            }
        }
        command += step.command;
        int result = std::system(command.c_str());
        if (result != 0) {
            return std::unexpected(BuildError("[additional_objs." + step_name + "] command failed: " + command));
        }
        for (const std::filesystem::path& output : outputs) {
            if (!std::filesystem::exists(output)) {
                return std::unexpected(BuildError("[additional_objs." + step_name + "] did not produce expected output '" + output.string() + "'"));
            }
        }

        std::vector<std::string> output_digests = path_digests(outputs);
        std::sort(output_digests.begin(), output_digests.end());
        if (auto r = database_.put(BuildRecord{
            record_key,
            "custom",
            signature,
            {},
            {},
            join_for_digest(output_digests),
            join_for_digest(output_keys),
            manifest_digest(manifest),
            compiler_version_key(),
            scpp::host_target_triple(),
        }); !r.has_value()) {
            return std::unexpected(std::move(r).error());
        }
        return outputs;
    }

    [[nodiscard]] std::expected<std::vector<std::filesystem::path>, BuildError> collect_custom_outputs(
        const PackageBuildResult& result, const ManifestTarget& target) const {
        std::vector<std::filesystem::path> outputs;
        for (const std::string& step_name : target.additional_obj_steps) {
            auto it = result.custom_outputs.find(step_name);
            if (it == result.custom_outputs.end()) {
                return std::unexpected(BuildError("custom step '" + step_name + "' was not built for target '" + target.name + "'"));
            }
            append_unique_paths(outputs, it->second);
        }
        return outputs;
    }

    [[nodiscard]] std::expected<void, BuildError> build_library_target(
        const ManifestData& manifest, const ManifestTarget& lib_target,
        const std::unordered_map<std::string, std::string>& direct_dep_import_paths,
        const std::unordered_map<std::string, std::string>& full_dependency_import_paths,
        const std::unordered_map<std::string, std::string>& transitive_only_modules, PackageBuildResult& result) {
        std::filesystem::path manifest_dir = manifest.manifest_path.parent_path();
        auto lib_sources_result = classify_target_sources(manifest_dir, lib_target);
        if (!lib_sources_result.has_value()) return std::unexpected(std::move(lib_sources_result).error());
        std::vector<SourceInfo> lib_sources = std::move(lib_sources_result).value();
        std::unordered_set<std::string> own_library_module_names = local_primary_module_names(lib_sources);
        if (auto r = validate_direct_visibility(lib_sources, own_library_module_names, direct_dep_import_paths,
                                                 transitive_only_modules);
            !r.has_value()) {
            return std::unexpected(std::move(r).error());
        }
        result.uses_stdlib = result.uses_stdlib || sources_use_stdlib(lib_sources);
        auto built_lib_modules_result = build_modules_for_target(
            lib_sources, result.package_output_root / "modules", result.package_output_root / "archives",
            full_dependency_import_paths, kManifestBuildOptLevel, manifest, &lib_target, database_);
        if (!built_lib_modules_result.has_value()) return std::unexpected(std::move(built_lib_modules_result).error());
        std::vector<BuiltModule> built_lib_modules = std::move(built_lib_modules_result).value();
        auto lib_custom_outputs_result = collect_custom_outputs(result, lib_target);
        if (!lib_custom_outputs_result.has_value()) return std::unexpected(std::move(lib_custom_outputs_result).error());
        std::vector<std::filesystem::path> lib_custom_outputs = std::move(lib_custom_outputs_result).value();
        if (!lib_custom_outputs.empty()) {
            std::vector<std::string> archive_inputs;
            for (const std::filesystem::path& path : lib_custom_outputs) archive_inputs.push_back(path.string());
            auto archive_r = scpp::archive_objects(archive_inputs, built_lib_modules.front().archive_path.string());
            if (!archive_r.has_value()) {
                return std::unexpected(BuildError(archive_r.error().what()));
            }
            built_lib_modules.front().archive_digest = path_digest_or_empty(built_lib_modules.front().archive_path);
        }
        result.library_modules.insert(result.library_modules.end(), built_lib_modules.begin(), built_lib_modules.end());
        return {};
    }

    [[nodiscard]] std::expected<void, BuildError> build_binary_target(
        const ManifestData& manifest, const ManifestTarget& bin_target,
        const std::unordered_map<std::string, std::string>& direct_dep_import_paths,
        const std::unordered_map<std::string, std::string>& full_dependency_import_paths,
        const std::unordered_map<std::string, std::string>& transitive_only_modules, PackageBuildResult& result) {
        std::filesystem::path manifest_dir = manifest.manifest_path.parent_path();
        auto sources_result = classify_target_sources(manifest_dir, bin_target);
        if (!sources_result.has_value()) return std::unexpected(std::move(sources_result).error());
        std::vector<SourceInfo> sources = std::move(sources_result).value();

        std::unordered_map<std::string, std::string> base_import_paths = full_dependency_import_paths;
        if (auto r = append_import_maps(base_import_paths, result.exported_modules); !r.has_value()) {
            return std::unexpected(std::move(r).error());
        }

        std::unordered_set<std::string> local_modules = own_module_names(result.library_modules);
        std::unordered_set<std::string> bin_local_names = local_primary_module_names(sources);
        local_modules.insert(bin_local_names.begin(), bin_local_names.end());
        if (auto r = validate_direct_visibility(sources, local_modules, direct_dep_import_paths, transitive_only_modules);
            !r.has_value()) {
            return std::unexpected(std::move(r).error());
        }

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
        std::error_code ec;
        std::filesystem::create_directories(object_dir, ec);
        if (ec) {
            return std::unexpected(BuildError("failed to create directory '" + object_dir.string() + "': " + ec.message()));
        }

        auto local_modules_built_result =
            build_modules_for_target(local_module_sources, module_dir, archive_dir, base_import_paths,
                                     kManifestBuildOptLevel, manifest, nullptr, database_);
        if (!local_modules_built_result.has_value()) return std::unexpected(std::move(local_modules_built_result).error());
        std::vector<BuiltModule> local_modules_built = std::move(local_modules_built_result).value();
        std::unordered_map<std::string, std::string> compile_import_paths = base_import_paths;
        if (auto r = append_import_maps(compile_import_paths, to_import_map(local_modules_built)); !r.has_value()) {
            return std::unexpected(std::move(r).error());
        }

        bool binary_uses_stdlib = result.uses_stdlib || sources_use_stdlib(sources);

        std::vector<std::filesystem::path> plain_objects;
        std::size_t plain_index = 0;
        for (const SourceInfo& source : sources) {
            if (source.kind != SourceInfo::Kind::Plain) continue;
            std::filesystem::path object_path = object_dir / (std::to_string(plain_index++) + "_" +
                                                              sanitize_filename(source.path.filename().string()) + ".o");
            if (auto r = build_object_with_cache("plain:" + bin_target.name + ":" + std::to_string(plain_index - 1), manifest,
                                                  source, object_path, compile_import_paths);
                !r.has_value()) {
                return std::unexpected(std::move(r).error());
            }
            plain_objects.push_back(object_path);
        }

        std::vector<std::string> extra_link_inputs;
        auto bin_custom_outputs_result = collect_custom_outputs(result, bin_target);
        if (!bin_custom_outputs_result.has_value()) return std::unexpected(std::move(bin_custom_outputs_result).error());
        for (const std::filesystem::path& custom_output : *bin_custom_outputs_result) {
            extra_link_inputs.push_back(custom_output.string());
        }
        for (const std::filesystem::path& object_path : plain_objects) extra_link_inputs.push_back(object_path.string());
        for (auto it = local_modules_built.rbegin(); it != local_modules_built.rend(); ++it) {
            extra_link_inputs.push_back(it->archive_path.string());
        }
        for (auto it = result.library_modules.rbegin(); it != result.library_modules.rend(); ++it) {
            extra_link_inputs.push_back(it->archive_path.string());
        }
        for (const std::filesystem::path& archive_path : result.archive_closure) extra_link_inputs.push_back(archive_path.string());
        if (binary_uses_stdlib) append_unique_strings(extra_link_inputs, scpp::project_default_stdlib_link_inputs());
        append_unique_strings(extra_link_inputs, result.native_link_inputs);

        std::filesystem::path executable_path = result.package_output_root / bin_target.name;
        std::string binary_key = "binary|" + manifest.manifest_path.string() + "|" + bin_target.name;
        std::vector<std::string> binary_inputs{
            "manifest=" + manifest_digest(manifest),
            "triple=" + scpp::host_target_triple(),
            "compiler=" + compiler_version_key(),
        };
        for (const std::filesystem::path& object_path : plain_objects) {
            binary_inputs.push_back("obj=" + object_path.string() + "#" + path_digest_or_empty(object_path));
        }
        for (const std::string& input : extra_link_inputs) {
            if (!input.empty() && input[0] == '-') {
                binary_inputs.push_back("flag=" + input);
            } else {
                binary_inputs.push_back("link=" + input + "#" + path_digest_or_empty(input));
            }
        }
        std::sort(binary_inputs.begin(), binary_inputs.end());
        std::string link_signature = fnv1a64_hex(join_for_digest(binary_inputs));
        auto cached_result = database_.get(binary_key);
        if (!cached_result.has_value()) return std::unexpected(std::move(cached_result).error());
        if (cached_result->has_value() && (*cached_result)->signature == link_signature &&
            std::filesystem::exists(executable_path)) {
            trace_build("cache hit link " + executable_path.string());
        } else {
            trace_build("link binary " + executable_path.string());
            // Static linking is unconditional now that the profile system is
            // gone (see link_executable in driver.cppm, which links
            // statically regardless of this argument) -- pass `true`
            // explicitly rather than a no-longer-existing profile setting.
            auto link_r = scpp::link_executable(extra_link_inputs, executable_path.string(), /*static_link=*/true);
            if (!link_r.has_value()) {
                return std::unexpected(BuildError(link_r.error().what()));
            }
            if (auto r = database_.put(BuildRecord{
                binary_key,
                "binary",
                link_signature,
                {},
                {},
                path_digest_or_empty(executable_path),
                executable_path.string(),
                manifest_digest(manifest),
                compiler_version_key(),
                scpp::host_target_triple(),
            }); !r.has_value()) {
                return std::unexpected(std::move(r).error());
            }
        }
        std::lock_guard lock(mutex_);
        if (std::find(result.binaries.begin(), result.binaries.end(), executable_path) == result.binaries.end()) {
            result.binaries.push_back(executable_path);
        }
        return {};
    }

    [[nodiscard]] std::expected<void, BuildError> build_object_with_cache(
        const std::string& key_suffix, const ManifestData& manifest, const SourceInfo& source,
        const std::filesystem::path& object_path, const std::unordered_map<std::string, std::string>& import_paths) {
        std::vector<std::string> dep_keys;
        for (const std::string& imported : source.imported_modules) {
            auto it = import_paths.find(imported);
            if (it == import_paths.end()) continue;
            dep_keys.push_back(imported + "=" + it->second + "#" + path_digest_or_empty(it->second));
        }
        std::sort(dep_keys.begin(), dep_keys.end());
        std::string signature = fnv1a64_hex(join_for_digest({
            "kind=object",
            "source=" + digest_file(source.path),
            "triple=" + scpp::host_target_triple(),
            "compiler=" + compiler_version_key(),
            "manifest=" + manifest_digest(manifest),
            "opt=" + std::to_string(kManifestBuildOptLevel),
            "debug=" + std::string(kManifestBuildEmitDebugInfo ? "1" : "0"),
            "deps=" + join_for_digest(dep_keys),
        }));
        std::string record_key = "object|" + manifest.manifest_path.string() + "|" + key_suffix;
        auto cached_result = database_.get(record_key);
        if (!cached_result.has_value()) return std::unexpected(std::move(cached_result).error());
        if (cached_result->has_value() && (*cached_result)->signature == signature && std::filesystem::exists(object_path)) {
            trace_build("cache hit object " + object_path.string());
            return {};
        }
        trace_build("build object " + object_path.string());
        auto source_text_result = read_file(source.path);
        if (!source_text_result.has_value()) return std::unexpected(std::move(source_text_result).error());
        std::string source_text = std::move(source_text_result).value();
        // emit_object_file (driver.cppm) fully absorbs DataflowError/CodegenError
        // into its returned DriverError (see the comment on
        // emit_object_file_for_program), so no scpp::DataflowError/CodegenError
        // exception can reach here anymore -- only the std::expected check below
        // is needed.
        auto emit_r = scpp::emit_object_file(source_text, object_path.string(), import_paths, {}, kManifestBuildEmitDebugInfo,
                               source.path.string(), kManifestBuildOptLevel);
        if (!emit_r.has_value()) {
            print_diagnostic(source.path.string(), source_text, emit_r.error().loc, emit_r.error().what());
            return std::unexpected(BuildError(emit_r.error().what()));
        }
        return database_.put(BuildRecord{
            record_key,
            "object",
            signature,
            {},
            {},
            path_digest_or_empty(object_path),
            object_path.string(),
            manifest_digest(manifest),
            compiler_version_key(),
            scpp::host_target_triple(),
        });
    }

    std::unordered_set<std::string> own_module_names(const std::vector<BuiltModule>& modules) const {
        std::unordered_set<std::string> names;
        for (const BuiltModule& module : modules) names.insert(module.name);
        return names;
    }

    std::filesystem::path shared_root_dir_;
    std::optional<ManifestData> workspace_manifest_;
    std::optional<WorkspaceInfo> workspace_info_;
    std::unordered_map<std::string, PackageBuildResult> cache_;
    std::unordered_map<std::string, PackageState> states_;
    std::unordered_map<std::string, std::string> failures_;
    std::mutex mutex_;
    std::condition_variable cv_;
    BuildDatabase database_;
};

[[nodiscard]] std::expected<std::vector<ManifestData>, ManifestError> select_workspace_packages(
    const WorkspaceInfo& workspace, const ManifestData* current_manifest, const scpp::ProjectBuildOptions& options,
    bool invoked_from_workspace_root) {
    if (options.build_workspace && options.selected_package.has_value()) {
        return std::unexpected(ManifestError("--workspace and --package/-p cannot be used together"));
    }
    if (options.selected_bin.has_value() && options.build_workspace) {
        return std::unexpected(ManifestError("--bin cannot be combined with --workspace"));
    }
    if (options.selected_lib.has_value() && options.build_workspace) {
        return std::unexpected(ManifestError("--lib <name> cannot be combined with --workspace"));
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
            return std::unexpected(ManifestError("workspace has no package named '" + *options.selected_package + "'"));
        }
        return std::vector<ManifestData>{*selected};
    }
    if (options.build_workspace) {
        return workspace.member_manifests;
    }
    if (!invoked_from_workspace_root && current_manifest != nullptr && current_manifest->package_name.has_value()) {
        return std::vector<ManifestData>{*current_manifest};
    }

    std::vector<ManifestData> selected;
    for (const std::filesystem::path& manifest_path : workspace.default_package_manifests) {
        auto it = std::find_if(workspace.member_manifests.begin(), workspace.member_manifests.end(),
                               [&](const ManifestData& manifest) { return manifest.manifest_path == manifest_path; });
        if (it == workspace.member_manifests.end()) {
            return std::unexpected(ManifestError("workspace default member '" + manifest_path.string() + "' could not be resolved"));
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
    auto discovery_result = discover_project(start_dir);
    if (!discovery_result.has_value()) {
        std::cerr << "error: " << discovery_result.error().what() << "\n";
        return 1;
    }
    ProjectDiscovery discovery = std::move(discovery_result).value();
    if (!discovery.current_manifest.has_value()) {
        std::cerr << "error: no scpp.toml found in the current directory or any parent directory\n";
        return 1;
    }

    std::optional<WorkspaceInfo> workspace_info;
    std::vector<ManifestData> packages_to_build;
    std::filesystem::path shared_output_root;
    bool invoked_from_workspace_root = false;

    // An ancestor `[workspace]` manifest only governs manifests it actually
    // declares as `members` (or the workspace root manifest itself) -- a
    // standalone `[package]` manifest that simply happens to be nested
    // somewhere below an unrelated ancestor workspace (for example a
    // scratch/test project created under this repo's own gitignored
    // `build/` directory, now that the repo root itself carries a
    // `[workspace]` manifest covering `libs/std`/`libs/scpp`/`src`) must
    // not be silently absorbed into that workspace's shared output root.
    // Verify membership before committing to workspace mode, mirroring
    // Cargo's requirement that workspace membership be explicit rather
    // than inferred from directory nesting.
    if (discovery.workspace_manifest.has_value()) {
        auto candidate_workspace_result = load_workspace(*discovery.workspace_manifest);
        if (!candidate_workspace_result.has_value()) {
            std::cerr << "error: " << candidate_workspace_result.error().what() << "\n";
            return 1;
        }
        WorkspaceInfo candidate_workspace = std::move(candidate_workspace_result).value();
        bool current_is_workspace_root =
            discovery.current_manifest->manifest_path == discovery.workspace_manifest->manifest_path;
        bool current_is_declared_member = current_is_workspace_root ||
            std::any_of(candidate_workspace.member_manifests.begin(), candidate_workspace.member_manifests.end(),
                       [&](const ManifestData& member) {
                           return member.manifest_path == discovery.current_manifest->manifest_path;
                       });
        if (current_is_declared_member) {
            workspace_info = std::move(candidate_workspace);
            shared_output_root = discovery.workspace_manifest->manifest_path.parent_path();
            invoked_from_workspace_root = current_is_workspace_root;
        }
    }

    if (workspace_info.has_value()) {
        if (!discovery.current_manifest->package_name.has_value() && !invoked_from_workspace_root) {
            std::cerr << "error: current manifest is not a package manifest\n";
            return 1;
        }
        auto packages_result = select_workspace_packages(*workspace_info,
                                                      discovery.current_manifest->package_name.has_value()
                                                          ? &*discovery.current_manifest
                                                          : nullptr,
                                                      options, invoked_from_workspace_root);
        if (!packages_result.has_value()) {
            std::cerr << "error: " << packages_result.error().what() << "\n";
            return 1;
        }
        packages_to_build = std::move(packages_result).value();
    } else {
        if (options.build_workspace || options.selected_package.has_value()) {
            std::cerr << "error: --workspace and --package/-p require a workspace root with [workspace]\n";
            return 1;
        }
        if (!discovery.current_manifest->package_name.has_value()) {
            std::cerr << "error: a virtual workspace cannot be built without [workspace] package selection\n";
            return 1;
        }
        shared_output_root = discovery.current_manifest->manifest_path.parent_path();
        packages_to_build = {*discovery.current_manifest};
    }

    PackageBuilder builder(shared_output_root, discovery.workspace_manifest, workspace_info);
    if (auto r = builder.open_database(); !r.has_value()) {
        std::cerr << "error: " << r.error().what() << "\n";
        return 1;
    }
    std::vector<std::future<std::expected<void, BuildError>>> futures;
    for (const ManifestData& manifest : packages_to_build) {
        std::filesystem::path manifest_path = manifest.manifest_path;
        futures.push_back(std::async(std::launch::async, [&builder, manifest_path, &options]() -> std::expected<void, BuildError> {
            auto r = builder.build_package(manifest_path, /*build_binaries=*/!options.build_lib_only, options);
            if (!r.has_value()) return std::unexpected(std::move(r).error());
            return {};
        }));
    }
    for (auto& future : futures) {
        if (auto r = future.get(); !r.has_value()) {
            std::cerr << "error: " << r.error().what() << "\n";
            return 1;
        }
    }
    return 0;
}

} // namespace scpp
